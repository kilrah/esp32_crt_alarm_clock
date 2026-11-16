#include "radio.h"
#include "config.h"

#include <WiFi.h>
#include <Preferences.h>
#include <AudioFileSourceICYStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioFileSourceFS.h>
#include <AudioGeneratorMP3.h>
#include <LittleFS.h>
#if AUDIO_OUTPUT == AUDIO_OUT_I2S
#include <AudioOutputI2S.h>
#include <driver/i2s.h>
#else
#include <driver/timer.h>
#if AUDIO_OUTPUT == AUDIO_OUT_DAC
#include <driver/dac.h>
#include <soc/i2s_struct.h>
#include <soc/sens_struct.h>
#else
#include <soc/ledc_struct.h>
#endif
#endif

namespace {

const char* NVS_NAMESPACE = "radio";
constexpr int AUDIO_TASK_CORE = 0;   // video interrupt and UI run on core 1
constexpr int AUDIO_TASK_PRIO = 3;   // above the web task (1), below WiFi/lwIP
constexpr uint32_t VOLUME_SAVE_DELAY_MS = 2000;
// Free RAM needed to start a stream on top of the reserved decoder + buffer memory:
// I2S DMA buffers (~5 KB), HTTP client and lwIP receive buffers.
constexpr size_t MIN_FREE_HEAP = 24 * 1024;

// Decoder state and network buffer are reserved once at boot, while the heap is
// still in one piece. Allocating ~40 KB at play time fails once the heap is
// fragmented (WiFi, web server, HA requests), and running out of memory mid-stream
// can hang the decoder.
void* mp3Space = nullptr;
uint8_t* streamBuffer = nullptr;

// ---- Commands (any task -> audio task) ----------------------------------------------
enum class CmdType : uint8_t { Play, Stop, Volume };
struct Cmd {
  CmdType type;
  int16_t station;
  uint8_t volume;
  char name[32];
  char url[128];
  bool fromBoot;  // Play: the backup alarm delay counts from power-up, not from now
  uint8_t fadeS;  // Play: fade in over this many seconds, 0 = full volume at once
};
QueueHandle_t queue;

// ---- Shared status ----------------------------------------------------------------
portMUX_TYPE statusLock = portMUX_INITIALIZER_UNLOCKED;
RadioStatus status{RadioState::Stopped, -1, "", "", "", RADIO_DEFAULT_VOLUME};
volatile uint32_t statusVersion = 0;

template <typename F>
void updateStatus(F f)
{
  portENTER_CRITICAL(&statusLock);
  f(status);
  portEXIT_CRITICAL(&statusLock);
  statusVersion = statusVersion + 1;
}

// ---- Stations (web task and loop) -------------------------------------------------
SemaphoreHandle_t stationsMutex;
Station stations[RADIO_MAX_STATIONS];
RadioAutoplay autoplay{false, 0, 0};  // guarded by stationsMutex too

// ---- Fade-in (audio task only) -----------------------------------------------------
// Samples are scaled by fadeQ / FADE_FULL before the volume is applied. Done on the
// samples because the library's volume has only 64 steps: far too coarse for a slow
// fade at a low volume setting.
constexpr int32_t FADE_FULL = 1 << 15;
int32_t fadeQ = FADE_FULL;
uint32_t fadeMs = 0;        // duration
uint32_t fadeStart = 0;     // 0 = not running
bool fadeArmed = false;     // starts with the first sound (stream or backup alarm)

inline int16_t faded(int16_t s) { return (int16_t)((s * fadeQ) >> 15); }

// ---- Diagnostics: gaps in the sample flow to the output -------------------------------
// A gap longer than what the output buffers (OUTPUT_BUFFER_MS) is audible.
#if AUDIO_OUTPUT == AUDIO_OUT_I2S
constexpr uint32_t OUTPUT_BUFFER_MS = AUDIO_I2S_DMA_BUFFERS * 128 * 1000 / 44100;
#else
constexpr uint32_t OUTPUT_BUFFER_MS = AUDIO_RING_SAMPLES * 1000 / 44100;
#endif
uint32_t lastSampleTick = 0;         // 0 = stream just started
volatile uint32_t maxGapMs = 0;      // longest since radioMaxGapMs() was last called
volatile uint32_t stalls = 0;        // gaps longer than the buffer, since boot

inline void sampleWritten()
{
  const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS | 1;
  if (lastSampleTick) {
    const uint32_t gap = now - lastSampleTick;
    if (gap > maxGapMs) maxGapMs = gap;
    if (gap > OUTPUT_BUFFER_MS) stalls = stalls + 1;
  }
  lastSampleTick = now;
}

void fadeCancel()
{
  fadeArmed = false;
  fadeStart = 0;
  fadeQ = FADE_FULL;
}

void fadeArm(uint32_t seconds)
{
  fadeCancel();
  if (!seconds) return;
  fadeMs = seconds * 1000;
  fadeArmed = true;
  fadeQ = 0;
}

// Sound is starting: an armed fade begins now (a running one just continues, e.g. when
// the stream takes over from the backup alarm)
void fadeOnSound()
{
  if (!fadeArmed) return;
  fadeArmed = false;
  fadeStart = millis() | 1;
}

void fadeUpdate()
{
  if (!fadeStart) return;
  const uint32_t e = millis() - fadeStart;
  if (e >= fadeMs) {
    fadeCancel();
    return;
  }
  // Squared like the volume setting: loudness then rises about evenly
  const float f = (float)e / fadeMs;
  fadeQ = (int32_t)(f * f * FADE_FULL);
}

#if AUDIO_OUTPUT == AUDIO_OUT_I2S
// The legacy ESP-IDF I2S driver powers down the ESP32's built-in DAC whenever an I2S
// port is uninstalled - even I2S1, which doesn't use it. The composite video comes out
// of that DAC (I2S0 -> GPIO25), so stopping the radio blanked the TV. The audio driver
// is therefore installed once and never uninstalled; "stop" just outputs silence.
class PersistentI2S : public AudioOutputI2S {
 public:
  using AudioOutputI2S::AudioOutputI2S;
  bool stop() override
  {
    if (i2sOn) i2s_zero_dma_buffer((i2s_port_t)portNo);
    lastSampleTick = 0;
    count = sent = 0;
    return true;
  }

  // The library hands every sample to the I2S driver in a call of its own (44100 calls
  // per second, each taking the driver's lock): that alone kept the audio task busy most
  // of the time. Samples are collected here and passed on in blocks instead.
  bool ConsumeSample(int16_t sample[2]) override
  {
    if (!i2sOn) return false;
    if (count == BLOCK && !sendBlock()) return false;  // driver full: the decoder retries later
    int16_t s[2] = {sample[0], sample[1]};
    MakeSampleStereo16(s);
    if (fadeQ != FADE_FULL) {
      s[0] = faded(s[0]);
      s[1] = faded(s[1]);
    }
    block[count++] = ((uint32_t)(uint16_t)Amplify(s[RIGHTCHANNEL]) << 16) | (uint16_t)Amplify(s[LEFTCHANNEL]);
    return true;
  }

  // Called after every decoder pass: don't sit on a partly filled block
  bool loop() override
  {
    if (count) sendBlock();
    return true;
  }

 private:
  // Passes the collected block on; false if the driver didn't take all of it yet
  bool sendBlock()
  {
    size_t written = 0;
    i2s_write((i2s_port_t)portNo, (const char*)block + sent, count * sizeof(uint32_t) - sent, &written, 0);
    if (written) sampleWritten();
    sent += written;
    if (sent < count * sizeof(uint32_t)) return false;
    count = sent = 0;
    return true;
  }

  static constexpr size_t BLOCK = 128;  // stereo samples (2.9 ms)
  uint32_t block[BLOCK];
  size_t count = 0;  // samples in the block
  size_t sent = 0;   // bytes of it the driver already has
};
using RadioOutput = PersistentI2S;

#else
// ---- Internal DAC / PWM output ------------------------------------------------------
// The decoder (audio task) fills a ring buffer with ready-to-write output levels; a
// hardware timer interrupt at the stream's sample rate writes one level per tick to
// the DAC or the LEDC duty register. Both run on core 0 (the video interrupt is on core 1).
static_assert((AUDIO_RING_SAMPLES & (AUDIO_RING_SAMPLES - 1)) == 0, "AUDIO_RING_SAMPLES must be a power of 2");
constexpr uint32_t RING_MASK = AUDIO_RING_SAMPLES - 1;
constexpr timer_group_t SAMPLE_TIMER_GROUP = TIMER_GROUP_0;
constexpr timer_idx_t SAMPLE_TIMER = TIMER_0;
constexpr uint32_t SAMPLE_TIMER_HZ = APB_CLK_FREQ / 2;  // divider 2 -> 40 MHz

#if AUDIO_OUTPUT == AUDIO_OUT_DAC
using Level = uint8_t;   // 8-bit DAC
#else
using Level = uint16_t;  // PWM duty incl. fractional bits
#endif
Level ring[AUDIO_RING_SAMPLES];     // .bss = internal RAM, safe for the IRAM interrupt
volatile uint32_t ringHead = 0;     // written by the audio task
volatile uint32_t ringTail = 0;     // written by the interrupt
volatile uint16_t lastLevel;        // last level written to the hardware
volatile uint32_t underruns = 0;    // timer ticks with an empty buffer (diagnostics)
volatile bool primed = false;       // the current stream has delivered samples

#if AUDIO_OUTPUT == AUDIO_OUT_DAC
constexpr int32_t OUT_LEVELS = 256;  // 8-bit DAC
inline void IRAM_ATTR writeLevel(uint16_t v)
{
  // Same 8-bit value in every byte: which half/byte of the 32-bit register reaches the
  // DAC doesn't matter then
  I2S0.conf_single_data = v * 0x01010101u;
}
#else
constexpr int32_t OUT_LEVELS = (1 << AUDIO_PWM_BITS) << 4;  // duty incl. 4 fractional bits
inline void IRAM_ATTR writeLevel(uint16_t v)
{
  LEDC.channel_group[0].channel[AUDIO_PWM_CHANNEL].duty.duty = v;
  LEDC.channel_group[0].channel[AUDIO_PWM_CHANNEL].conf1.duty_start = 1;  // latched at next period
}
#endif
constexpr uint16_t MID_LEVEL = OUT_LEVELS / 2;

bool IRAM_ATTR sampleTimerIsr(void*)
{
  const uint32_t t = ringTail;
  if (t != ringHead) {  // on underrun keep the last level (no click)
    const uint16_t v = ring[t];
    writeLevel(v);
    lastLevel = v;
    ringTail = (t + 1) & RING_MASK;
  } else if (primed) {  // not the silence while a new stream is buffering
    underruns = underruns + 1;
  }
  return false;  // no task woken
}

class SampleOutput : public AudioOutput {
 public:
  // Hardware setup that doesn't need an interrupt (any core)
  void setupHardware()
  {
#if AUDIO_OUTPUT == AUDIO_OUT_DAC
    // I2S0 (video) already drives the internal DACs from DMA. Power up the second DAC pad
    // and give the I2S channel that isn't the video a constant value we control.
    // Starts at 0 V; rampUp() then moves it to the idle level. The channel is switched to
    // the constant before the DAC is powered, otherwise the pin briefly carries the video.
    const dac_channel_t ch = CVBS_PIN == 25 ? DAC_CHANNEL_2 : DAC_CHANNEL_1;
    writeLevel(0);
    I2S0.conf_chan.tx_chan_mod = AUDIO_DAC_CHAN_MOD;
    if (ch == DAC_CHANNEL_2) SENS.sar_dac_ctrl2.dac_cw_en2 = 0;  // no cosine generator
    else SENS.sar_dac_ctrl2.dac_cw_en1 = 0;
    dac_output_enable(ch);
#else
    ledcSetup(AUDIO_PWM_CHANNEL, APB_CLK_FREQ >> AUDIO_PWM_BITS, AUDIO_PWM_BITS);
    ledcWrite(AUDIO_PWM_CHANNEL, 0);  // 0 V; also sets up the fixed-duty mode
    ledcAttachPin(AUDIO_PWM_PIN, AUDIO_PWM_CHANNEL);
#endif
    lastLevel = 0;
    hertz = 0;  // not initialized by AudioOutput
  }

  // Power-up: raise the output from 0 V to the idle level (half the supply) slowly and
  // along a raised-cosine curve. A step would pass the coupling capacitor and the amplifier
  // as a thump; the low-pass filter doesn't help against it. ~0.5 s keeps the energy below
  // the coupling capacitor's high-pass corner. Blocks the calling task.
  void rampUp()
  {
    constexpr int STEPS = 250;
    for (int i = 1; i <= STEPS; ++i) {
      const float f = 0.5f - 0.5f * cosf((float)M_PI * i / STEPS);
      writeLevel((uint16_t)(MID_LEVEL * f + 0.5f));
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    lastLevel = MID_LEVEL;
  }

  // Must be called on core 0: the interrupt is allocated on the calling core
  void setupTimer()
  {
    timer_config_t cfg = {};
    cfg.alarm_en = TIMER_ALARM_EN;
    cfg.counter_en = TIMER_PAUSE;
    cfg.intr_type = TIMER_INTR_LEVEL;
    cfg.counter_dir = TIMER_COUNT_UP;
    cfg.auto_reload = TIMER_AUTORELOAD_EN;
    cfg.divider = APB_CLK_FREQ / SAMPLE_TIMER_HZ;
    timer_init(SAMPLE_TIMER_GROUP, SAMPLE_TIMER, &cfg);
    timer_set_counter_value(SAMPLE_TIMER_GROUP, SAMPLE_TIMER, 0);
    timer_set_alarm_value(SAMPLE_TIMER_GROUP, SAMPLE_TIMER, SAMPLE_TIMER_HZ / 44100);
    timer_enable_intr(SAMPLE_TIMER_GROUP, SAMPLE_TIMER);
    timer_isr_callback_add(SAMPLE_TIMER_GROUP, SAMPLE_TIMER, sampleTimerIsr, nullptr, ESP_INTR_FLAG_IRAM);
  }

  bool SetRate(int hz) override
  {
    if (hz < 8000 || hz > 48000) return false;
    if (hz == hertz) return true;
    hertz = hz;
    // The alarm only fires when the counter reaches it: lowering it below the current
    // count would stop the interrupt until the 54-bit counter wraps (audio frozen). Restart
    // the count from 0.
    timer_pause(SAMPLE_TIMER_GROUP, SAMPLE_TIMER);
    timer_set_counter_value(SAMPLE_TIMER_GROUP, SAMPLE_TIMER, 0);
    timer_set_alarm_value(SAMPLE_TIMER_GROUP, SAMPLE_TIMER, (SAMPLE_TIMER_HZ + hz / 2) / hz);
    if (running) timer_start(SAMPLE_TIMER_GROUP, SAMPLE_TIMER);
    return true;
  }

  bool begin() override
  {
    err = 0;
    primed = false;
    if (!running) {
      timer_set_counter_value(SAMPLE_TIMER_GROUP, SAMPLE_TIMER, 0);
      timer_start(SAMPLE_TIMER_GROUP, SAMPLE_TIMER);
      running = true;
    }
    return true;
  }

  bool ConsumeSample(int16_t sample[2]) override
  {
    const uint32_t h = ringHead;
    const uint32_t next = (h + 1) & RING_MASK;
    if (next == ringTail) return false;  // full: the decoder retries later

    int16_t s[2] = {sample[0], sample[1]};
    MakeSampleStereo16(s);
    if (fadeQ != FADE_FULL) {
      s[0] = faded(s[0]);
      s[1] = faded(s[1]);
    }
    const int32_t mono = ((int32_t)Amplify(s[0]) + Amplify(s[1])) / 2;
    // Scale to output levels (16.16 fixed point) with first-order noise shaping: the
    // quantization error is fed into the next sample, which moves the noise of the
    // coarse (8-bit) output towards high frequencies where the RC filter removes it.
    const int32_t v = mono * OUT_LEVELS + ((int32_t)MID_LEVEL << 16) + err;
    int32_t q = v >> 16;
    if (q < 0) {
      q = 0;
      err = 0;
    } else if (q > OUT_LEVELS - 1) {
      q = OUT_LEVELS - 1;
      err = 0;
    } else {
      err = v - (q << 16);
    }
    ring[h] = (Level)q;
    __asm__ __volatile__("" ::: "memory");  // sample stored before the index moves
    ringHead = next;
    // Count underruns once the buffer has filled up for the first time
    if (!primed && ((next - ringTail) & RING_MASK) >= RING_MASK / 2) primed = true;
    return true;
  }

  bool stop() override
  {
    if (running) {
      timer_pause(SAMPLE_TIMER_GROUP, SAMPLE_TIMER);
      running = false;
    }
    primed = false;
    ringHead = ringTail = 0;
    // Ramp to the idle level over ~3 ms instead of jumping (avoids a click)
    const int32_t from = lastLevel;
    for (int i = 1; i <= 64; ++i) {
      writeLevel((uint16_t)(from + (MID_LEVEL - from) * i / 64));
      delayMicroseconds(50);
    }
    lastLevel = MID_LEVEL;
    return true;
  }

 private:
  bool running = false;
  int32_t err = 0;
};
using RadioOutput = SampleOutput;
#endif

// ---- Audio task state (audio task only) ---------------------------------------------
RadioOutput* out = nullptr;
AudioFileSourceICYStream* file = nullptr;
AudioFileSourceBuffer* buff = nullptr;
AudioGeneratorMP3* mp3 = nullptr;
bool wantPlay = false;       // user wants the stream: reconnect after failures
char wantUrl[128] = "";
uint32_t retryAt = 0;
int retryCount = 0;
uint32_t volumeSaveAt = 0;   // 0 = nothing pending
// Diagnostics: share of the time the audio task spends decoding / waiting for the network
volatile uint32_t busyUs = 0;        // inside the decoder since radioAudioLoad() was last called
uint32_t busySinceUs = 0;
volatile uint32_t streamDry = 0;     // times the network buffer ran empty, since boot

void onBufferStatus(void*, int code, const char*)
{
  if (code == AudioFileSourceBuffer::STATUS_UNDERFLOW) streamDry = streamDry + 1;
}
// Backup alarm sound (RADIO_BACKUP_AFTER_S)
AudioFileSourceFS* backupFile = nullptr;  // != nullptr while the sound is playing
uint32_t backupSize = 0;     // size of the file found at boot, 0 = none
uint32_t silentSince = 0;    // when the wanted stream went missing
bool backupActive = false;   // the stream has been missing for too long: fill the gaps with the sound

// ICY titles are usually UTF-8 but some stations send Latin-1: convert those
void toUtf8(const char* in, char* out, size_t size)
{
  bool valid = true;
  for (const uint8_t* p = (const uint8_t*)in; *p && valid;) {
    int n = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
    if (n < 0) { valid = false; break; }
    ++p;
    for (int i = 0; i < n; ++i, ++p)
      if ((*p & 0xC0) != 0x80) { valid = false; break; }
  }
  size_t o = 0;
  for (const uint8_t* p = (const uint8_t*)in; *p && o + 3 < size; ++p) {
    if (valid || *p < 0x80) {
      out[o++] = (char)*p;
    } else {
      out[o++] = (char)(0xC0 | (*p >> 6));
      out[o++] = (char)(0x80 | (*p & 0x3F));
    }
  }
  // Don't leave a cut UTF-8 sequence at the end
  while (o > 0 && ((uint8_t)out[o - 1] & 0xC0) == 0x80) {
    size_t start = o - 1;
    while (start > 0 && ((uint8_t)out[start] & 0xC0) == 0x80) --start;
    uint8_t lead = (uint8_t)out[start];
    size_t need = (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : 4;
    if (o - start >= need) break;
    o = start;
  }
  out[o] = 0;
}

void onMetadata(void*, const char* type, bool, const char* str)
{
  if (strcmp(type, "StreamTitle") != 0) return;
  char title[sizeof(RadioStatus::title)];
  toUtf8(str, title, sizeof(title));
  updateStatus([&](RadioStatus& s) { strlcpy(s.title, title, sizeof(s.title)); });
  Serial.printf("[Radio] Now playing: %s\n", title);
}

void applyVolume(uint8_t volume)
{
  // Perceived loudness is roughly logarithmic: square the setting. The library gain
  // has 1/64 steps, so keep the lowest settings audible.
  const float f = volume / 100.0f;
  float gain = f * f;
  if (volume > 0 && gain < 1.0f / 64) gain = 1.0f / 64;
  if (out) out->SetGain(gain);
}

void stopStream()
{
  if (mp3) {
    mp3->stop();  // also stops the I2S output (driver uninstalled, DMA memory freed)
    delete mp3;
    mp3 = nullptr;
  }
  delete buff;
  buff = nullptr;
  delete file;
  file = nullptr;
  delete backupFile;
  backupFile = nullptr;
}

void scheduleRetry(const char* error)
{
  static const uint32_t delays[] = {3000, 10000, 30000, 60000};
  const uint32_t d = delays[retryCount < 3 ? retryCount : 3];
  ++retryCount;
  retryAt = millis() + d;
  updateStatus([&](RadioStatus& s) {
    s.state = RadioState::Retrying;
    strlcpy(s.error, error, sizeof(s.error));
    s.title[0] = 0;
  });
  Serial.printf("[Radio] %s, retrying in %u s\n", error, (unsigned)(d / 1000));
}

// True once the wanted stream has been missing for RADIO_BACKUP_AFTER_S (stays true
// until the stream plays or the radio is stopped)
bool backupDue()
{
#if RADIO_BACKUP_AFTER_S > 0
  if (!backupActive && backupSize && millis() - silentSince >= RADIO_BACKUP_AFTER_S * 1000UL) {
    backupActive = true;
    Serial.println("[Radio] No stream, switching to the backup alarm sound");
  }
#endif
  return backupActive;
}

// Plays the alarm sound from flash once (the audio task restarts it or retries the
// stream when it ends). The status keeps the stream's error.
void startBackup()
{
  stopStream();
  backupFile = new AudioFileSourceFS(LittleFS, RADIO_BACKUP_FILE);
  mp3 = new AudioGeneratorMP3(mp3Space, AudioGeneratorMP3::preAllocSize());
  if (!backupFile->isOpen() || !mp3->begin(backupFile, out)) {
    stopStream();
    backupActive = false;
    backupSize = 0;  // don't try again
    updateStatus([](RadioStatus& s) { s.state = RadioState::Retrying; });
    Serial.println("[Radio] Backup alarm sound could not be played");
    return;
  }
  fadeOnSound();
  updateStatus([](RadioStatus& s) {
    s.state = RadioState::Backup;
    s.title[0] = 0;
  });
}

void startStream()
{
  stopStream();
  if (!wantUrl[0]) {
    scheduleRetry("Station is empty");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    scheduleRetry("No WiFi connection");
    return;
  }
  if (!mp3Space || !streamBuffer) {
    scheduleRetry("No memory reserved for the radio");
    return;
  }
  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    char e[64];
    snprintf(e, sizeof(e), "Not enough memory (%u KB free)", (unsigned)(ESP.getFreeHeap() / 1024));
    scheduleRetry(e);
    return;
  }

  updateStatus([](RadioStatus& s) {
    s.state = RadioState::Connecting;
    s.title[0] = 0;
  });
  Serial.printf("[Radio] Connecting to %s\n", wantUrl);

  file = new AudioFileSourceICYStream();
  file->RegisterMetadataCB(onMetadata, nullptr);
  file->SetReconnect(2, 300);
  file->useHTTP10();  // no chunked transfer encoding: the decoder reads the raw socket
  if (!file->open(wantUrl)) {
    stopStream();
    scheduleRetry("Cannot open stream (http:// MP3 only)");
    return;
  }
  buff = new AudioFileSourceBuffer(file, streamBuffer, RADIO_BUFFER_BYTES);
  buff->RegisterStatusCB(onBufferStatus, nullptr);
  mp3 = new AudioGeneratorMP3(mp3Space, AudioGeneratorMP3::preAllocSize());
  if (!mp3->begin(buff, out)) {
    stopStream();
    scheduleRetry("Stream could not be decoded (MP3 only)");
    return;
  }
  retryCount = 0;
  backupActive = false;
  fadeOnSound();
  updateStatus([](RadioStatus& s) {
    s.state = RadioState::Playing;
    s.error[0] = 0;
  });
  Serial.printf("[Radio] Playing, %u KB free\n", (unsigned)(ESP.getFreeHeap() / 1024));
}

void handleCmd(const Cmd& c)
{
  switch (c.type) {
    case CmdType::Play:
      wantPlay = true;
      strlcpy(wantUrl, c.url, sizeof(wantUrl));
      retryCount = 0;
      silentSince = c.fromBoot ? 0 : millis();
      backupActive = false;
      fadeArm(c.fadeS);
      updateStatus([&](RadioStatus& s) {
        s.station = c.station;
        strlcpy(s.name, c.name, sizeof(s.name));
        s.error[0] = 0;
      });
      startStream();
      break;
    case CmdType::Stop:
      wantPlay = false;
      backupActive = false;
      fadeCancel();
      stopStream();
      updateStatus([](RadioStatus& s) {
        s.state = RadioState::Stopped;
        s.title[0] = 0;
        s.error[0] = 0;
      });
      Serial.println("[Radio] Stopped");
      break;
    case CmdType::Volume:
      applyVolume(c.volume);
      updateStatus([&](RadioStatus& s) { s.volume = c.volume; });
      volumeSaveAt = millis() | 1;  // persist once the slider has settled
      break;
  }
}

void audioTask(void*)
{
#if AUDIO_OUTPUT != AUDIO_OUT_I2S
  out->setupTimer();  // allocates the sample interrupt on this core
  out->rampUp();      // 0 V -> idle level without a click (play commands wait in the queue)
#endif
  for (;;) {
    const bool running = mp3 && mp3->isRunning();
    TickType_t wait = running ? 0 : pdMS_TO_TICKS(wantPlay ? 200 : 1000);
    Cmd c;
    while (xQueueReceive(queue, &c, wait) == pdTRUE) {
      handleCmd(c);
      wait = 0;
    }
    fadeUpdate();

    if (mp3 && mp3->isRunning()) {
      const uint32_t t = micros();
      const bool ok = mp3->loop();
      busyUs = busyUs + (micros() - t);
      if (!ok) {
        const bool wasBackup = backupFile != nullptr;
        stopStream();
        if (wasBackup) {
          // The sound has played through: restart it below, or try the stream first
          // if a retry is due (it comes back right away if that fails)
        } else if (wantPlay) {
          silentSince = millis();
          scheduleRetry("Stream interrupted");
        }
      }
    }
    if (wantPlay && !mp3) {
      if ((int32_t)(millis() - retryAt) >= 0) startStream();
      if (!mp3 && backupDue()) startBackup();
    }

    if (volumeSaveAt && millis() - volumeSaveAt >= VOLUME_SAVE_DELAY_MS) {
      volumeSaveAt = 0;
      Preferences p;
      if (p.begin(NVS_NAMESPACE, false)) {
        p.putUChar("vol", radioStatus().volume);
        p.end();
      }
    }
    // The decoder returns as soon as the output buffer is full: yield
    vTaskDelay(1);
  }
}

bool queueCmd(const Cmd& c)
{
  if (xQueueSend(queue, &c, pdMS_TO_TICKS(100)) != pdTRUE) {
    Serial.println("[Radio] Command queue full");
    return false;
  }
  return true;
}

}  // namespace

// ---- Public API ------------------------------------------------------------------------

void radioBegin()
{
  stationsMutex = xSemaphoreCreateMutex();
  queue = xQueueCreate(4, sizeof(Cmd));

  audioLogger = &Serial;  // ESP8266Audio's diagnostics (silent by default)

  // Reserve the decoder and stream buffer now (see mp3Space above)
  const size_t heapBefore = ESP.getFreeHeap();
  mp3Space = malloc(AudioGeneratorMP3::preAllocSize());
  streamBuffer = (uint8_t*)malloc(RADIO_BUFFER_BYTES);
  if (!mp3Space || !streamBuffer) {
    free(mp3Space);
    free(streamBuffer);
    mp3Space = nullptr;
    streamBuffer = nullptr;
    Serial.println("[Radio] Could not reserve memory, radio disabled");
  }
  Serial.printf("[Radio] Reserved %u KB (MP3 decoder %u + buffer %u), %u KB free\n",
                (unsigned)((heapBefore - ESP.getFreeHeap()) / 1024), (unsigned)AudioGeneratorMP3::preAllocSize(),
                (unsigned)RADIO_BUFFER_BYTES, (unsigned)(ESP.getFreeHeap() / 1024));

#if RADIO_BACKUP_AFTER_S > 0
  // Backup alarm sound in the LittleFS partition (not formatted if it isn't there)
  if (mp3Space && LittleFS.begin(false)) {
    File f = LittleFS.open(RADIO_BACKUP_FILE, "r");
    if (f && !f.isDirectory()) backupSize = f.size();
    f.close();
  }
  if (backupSize) Serial.printf("[Radio] Backup alarm sound: %u KB\n", (unsigned)(backupSize / 1024));
  else Serial.println("[Radio] No backup alarm sound (" RADIO_BACKUP_FILE " not in flash, see README)");
#endif

  Preferences p;
  uint8_t volume = RADIO_DEFAULT_VOLUME;
  bool initialized = false;
  if (p.begin(NVS_NAMESPACE, false)) {  // read-write: creates the namespace on first boot
    initialized = p.isKey("init");
    autoplay.enabled = p.getBool("auto", false);
    autoplay.station = p.getUChar("autost", 0);
    if (autoplay.station >= RADIO_MAX_STATIONS) autoplay.station = 0;
    autoplay.fadeS = p.getUChar("autofd", 0);
    if (autoplay.fadeS > RADIO_FADE_MAX_S) autoplay.fadeS = RADIO_FADE_MAX_S;
    volume = p.getUChar("vol", RADIO_DEFAULT_VOLUME);
    for (int i = 0; i < RADIO_MAX_STATIONS; ++i) {
      char key[4];
      snprintf(key, sizeof(key), "n%d", i);
      if (p.isKey(key)) strlcpy(stations[i].name, p.getString(key, "").c_str(), sizeof(stations[i].name));
      snprintf(key, sizeof(key), "u%d", i);
      if (p.isKey(key)) strlcpy(stations[i].url, p.getString(key, "").c_str(), sizeof(stations[i].url));
    }
    p.end();
  }
  if (!initialized) {  // first boot: one example station
    strlcpy(stations[0].name, RADIO_DEFAULT_NAME, sizeof(stations[0].name));
    strlcpy(stations[0].url, RADIO_DEFAULT_URL, sizeof(stations[0].url));
  }
  if (volume > 100) volume = RADIO_DEFAULT_VOLUME;
  status.volume = volume;

#if AUDIO_OUTPUT == AUDIO_OUT_I2S
#if I2S_SCK_LOW_PIN >= 0
  // PCM5102 SCK held low = no external master clock, use the internal PLL
  pinMode(I2S_SCK_LOW_PIN, OUTPUT);
  digitalWrite(I2S_SCK_LOW_PIN, LOW);
#endif
  out = new PersistentI2S(1, AudioOutputI2S::EXTERNAL_I2S, AUDIO_I2S_DMA_BUFFERS);  // I2S0 belongs to the video
  out->SetPinout(I2S_BCK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN);
  out->begin();  // install the driver now (DMA buffers allocated before WiFi fragments the heap)
#else
  out = new SampleOutput();
  out->setupHardware();  // the timer interrupt is set up by the audio task (core 0)
#endif
  applyVolume(volume);

  xTaskCreatePinnedToCore(audioTask, "audio", 5120, nullptr, AUDIO_TASK_PRIO, nullptr, AUDIO_TASK_CORE);
#if AUDIO_OUTPUT == AUDIO_OUT_I2S
  Serial.printf("[Radio] Ready, I2S1 BCK %d LRCK %d DOUT %d, volume %u\n", I2S_BCK_PIN, I2S_LRCK_PIN,
                I2S_DOUT_PIN, volume);
#elif AUDIO_OUTPUT == AUDIO_OUT_DAC
  Serial.printf("[Radio] Ready, internal DAC on GPIO%d (I2S0 chan_mod %d), volume %u\n", CVBS_PIN == 25 ? 26 : 25,
                AUDIO_DAC_CHAN_MOD, volume);
#else
  Serial.printf("[Radio] Ready, PWM on GPIO%d (%u kHz, %d+4 bits), volume %u\n", AUDIO_PWM_PIN,
                (unsigned)((APB_CLK_FREQ >> AUDIO_PWM_BITS) / 1000), AUDIO_PWM_BITS, volume);
#endif
}

void radioPlay(int index, bool fromBoot, int fadeS)
{
  const Station s = radioStation(index);
  if (!s.url[0] && !fromBoot) {  // at power-up an empty slot still leads to the backup alarm
    char e[32];
    snprintf(e, sizeof(e), "Station %d is empty", index + 1);
    updateStatus([&](RadioStatus& st) { strlcpy(st.error, e, sizeof(st.error)); });
    return;
  }
  Cmd c{CmdType::Play, (int16_t)index, 0, "", ""};
  strlcpy(c.name, s.name[0] ? s.name : "Radio", sizeof(c.name));
  strlcpy(c.url, s.url, sizeof(c.url));
  c.fromBoot = fromBoot;
  c.fadeS = (uint8_t)constrain(fadeS, 0, RADIO_FADE_MAX_S);
  queueCmd(c);
}

void radioPlayUrl(const char* url, const char* name, int fadeS)
{
  Cmd c{CmdType::Play, -1, 0, "", ""};
  strlcpy(c.name, name && *name ? name : "Radio", sizeof(c.name));
  strlcpy(c.url, url, sizeof(c.url));
  c.fadeS = (uint8_t)constrain(fadeS, 0, RADIO_FADE_MAX_S);
  queueCmd(c);
}

void radioStop()
{
  Cmd c{CmdType::Stop, -1, 0, "", ""};
  queueCmd(c);
}

void radioSetVolume(int volume)
{
  Cmd c{CmdType::Volume, -1, (uint8_t)constrain(volume, 0, 100), "", ""};
  queueCmd(c);
}

RadioStatus radioStatus()
{
  portENTER_CRITICAL(&statusLock);
  RadioStatus s = status;
  portEXIT_CRITICAL(&statusLock);
  return s;
}

uint32_t radioStatusVersion() { return statusVersion; }

uint32_t radioUnderruns()
{
#if AUDIO_OUTPUT == AUDIO_OUT_I2S
  return stalls;  // estimate: times no samples arrived for longer than the DMA buffers last
#else
  return underruns;
#endif
}

RadioAutoplay radioAutoplay()
{
  xSemaphoreTake(stationsMutex, portMAX_DELAY);
  RadioAutoplay a = autoplay;
  xSemaphoreGive(stationsMutex);
  return a;
}

void radioSetAutoplay(const RadioAutoplay& a)
{
  RadioAutoplay v = a;
  if (v.station < 0 || v.station >= RADIO_MAX_STATIONS) v.station = 0;
  v.fadeS = constrain(v.fadeS, 0, RADIO_FADE_MAX_S);
  xSemaphoreTake(stationsMutex, portMAX_DELAY);
  autoplay = v;
  xSemaphoreGive(stationsMutex);
  Preferences p;
  if (p.begin(NVS_NAMESPACE, false)) {
    p.putBool("auto", v.enabled);
    p.putUChar("autost", (uint8_t)v.station);
    p.putUChar("autofd", (uint8_t)v.fadeS);
    p.end();
  }
}

Station radioStation(int index)
{
  Station s{"", ""};
  if (index < 0 || index >= RADIO_MAX_STATIONS) return s;
  xSemaphoreTake(stationsMutex, portMAX_DELAY);
  s = stations[index];
  xSemaphoreGive(stationsMutex);
  return s;
}

void radioSetStation(int index, const Station& s)
{
  if (index < 0 || index >= RADIO_MAX_STATIONS) return;
  xSemaphoreTake(stationsMutex, portMAX_DELAY);
  stations[index] = s;
  xSemaphoreGive(stationsMutex);
  Preferences p;
  if (p.begin(NVS_NAMESPACE, false)) {
    char key[4];
    snprintf(key, sizeof(key), "n%d", index);
    p.putString(key, s.name);
    snprintf(key, sizeof(key), "u%d", index);
    p.putString(key, s.url);
    p.putBool("init", true);
    p.end();
  }
}

const char* validateStation(const Station& s)
{
  if (!s.url[0]) return nullptr;  // empty slot
  if (strncmp(s.url, "http://", 7) != 0)
    return "Station URLs must start with http:// (https streams are not supported, not enough RAM)";
  if (strchr(s.url, ' ')) return "Station URL must not contain spaces";
  return nullptr;
}

#define STR_(x) #x
#define STR(x) STR_(x)
const char* radioOutputName()
{
#if AUDIO_OUTPUT == AUDIO_OUT_I2S
  return "I2S DAC (PCM5102) on GPIO" STR(I2S_BCK_PIN) "/" STR(I2S_LRCK_PIN) "/" STR(I2S_DOUT_PIN);
#elif AUDIO_OUTPUT == AUDIO_OUT_DAC
  return CVBS_PIN == 25 ? "Internal DAC on GPIO26 (8-bit)" : "Internal DAC on GPIO25 (8-bit)";
#else
  return "PWM on GPIO" STR(AUDIO_PWM_PIN) " (" STR(AUDIO_PWM_BITS) "+4 bit)";
#endif
}

const char* radioStateName(RadioState s)
{
  switch (s) {
    case RadioState::Connecting: return "connecting";
    case RadioState::Playing: return "playing";
    case RadioState::Retrying: return "retrying";
    case RadioState::Backup: return "backup";
    default: return "stopped";
  }
}

uint32_t radioBackupSize() { return backupSize; }

uint32_t radioStreamDry() { return streamDry; }

uint32_t radioMaxGapMs()
{
  const uint32_t g = maxGapMs;
  maxGapMs = 0;
  return g;
}

int radioAudioLoad()
{
  const uint32_t now = micros(), span = now - busySinceUs, busy = busyUs;
  busySinceUs = now;
  busyUs = 0;
  return span ? (int)((uint64_t)busy * 100 / span) : 0;
}
