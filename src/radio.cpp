#include "radio.h"
#include "config.h"

#include <WiFi.h>
#include <Preferences.h>
#include <AudioFileSourceICYStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <driver/i2s.h>

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
RadioAutoplay autoplay{false, 0};  // guarded by stationsMutex too

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
    return true;
  }
};

// ---- Audio task state (audio task only) ---------------------------------------------
PersistentI2S* out = nullptr;
AudioFileSourceICYStream* file = nullptr;
AudioFileSourceBuffer* buff = nullptr;
AudioGeneratorMP3* mp3 = nullptr;
bool wantPlay = false;       // user wants the stream: reconnect after failures
char wantUrl[128] = "";
uint32_t retryAt = 0;
int retryCount = 0;
uint32_t volumeSaveAt = 0;   // 0 = nothing pending

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

void startStream()
{
  stopStream();
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
  mp3 = new AudioGeneratorMP3(mp3Space, AudioGeneratorMP3::preAllocSize());
  if (!mp3->begin(buff, out)) {
    stopStream();
    scheduleRetry("Stream could not be decoded (MP3 only)");
    return;
  }
  retryCount = 0;
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
      updateStatus([&](RadioStatus& s) {
        s.station = c.station;
        strlcpy(s.name, c.name, sizeof(s.name));
        s.error[0] = 0;
      });
      startStream();
      break;
    case CmdType::Stop:
      wantPlay = false;
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
  for (;;) {
    const bool running = mp3 && mp3->isRunning();
    TickType_t wait = running ? 0 : pdMS_TO_TICKS(wantPlay ? 200 : 1000);
    Cmd c;
    while (xQueueReceive(queue, &c, wait) == pdTRUE) {
      handleCmd(c);
      wait = 0;
    }

    if (mp3 && mp3->isRunning()) {
      if (!mp3->loop()) {
        stopStream();
        if (wantPlay) scheduleRetry("Stream interrupted");
      }
    } else if (wantPlay && !mp3 && (int32_t)(millis() - retryAt) >= 0) {
      startStream();
    }

    if (volumeSaveAt && millis() - volumeSaveAt >= VOLUME_SAVE_DELAY_MS) {
      volumeSaveAt = 0;
      Preferences p;
      if (p.begin(NVS_NAMESPACE, false)) {
        p.putUChar("vol", radioStatus().volume);
        p.end();
      }
    }
    // The decoder returns as soon as the I2S DMA buffer (~23 ms) is full: yield
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

  Preferences p;
  uint8_t volume = RADIO_DEFAULT_VOLUME;
  bool initialized = false;
  if (p.begin(NVS_NAMESPACE, false)) {  // read-write: creates the namespace on first boot
    initialized = p.isKey("init");
    autoplay.enabled = p.getBool("auto", false);
    autoplay.station = p.getUChar("autost", 0);
    if (autoplay.station >= RADIO_MAX_STATIONS) autoplay.station = 0;
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

#if I2S_SCK_LOW_PIN >= 0
  // PCM5102 SCK held low = no external master clock, use the internal PLL
  pinMode(I2S_SCK_LOW_PIN, OUTPUT);
  digitalWrite(I2S_SCK_LOW_PIN, LOW);
#endif
  out = new PersistentI2S(1, AudioOutputI2S::EXTERNAL_I2S);  // I2S0 belongs to the video
  out->SetPinout(I2S_BCK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN);
  out->begin();  // install the driver now (DMA buffers allocated before WiFi fragments the heap)
  applyVolume(volume);

  xTaskCreatePinnedToCore(audioTask, "audio", 5120, nullptr, AUDIO_TASK_PRIO, nullptr, AUDIO_TASK_CORE);
  Serial.printf("[Radio] Ready, I2S1 BCK %d LRCK %d DOUT %d, volume %u\n", I2S_BCK_PIN, I2S_LRCK_PIN,
                I2S_DOUT_PIN, volume);
}

void radioPlay(int index)
{
  const Station s = radioStation(index);
  if (!s.url[0]) {
    char e[32];
    snprintf(e, sizeof(e), "Station %d is empty", index + 1);
    updateStatus([&](RadioStatus& st) { strlcpy(st.error, e, sizeof(st.error)); });
    return;
  }
  Cmd c{CmdType::Play, (int16_t)index, 0, "", ""};
  strlcpy(c.name, s.name[0] ? s.name : "Radio", sizeof(c.name));
  strlcpy(c.url, s.url, sizeof(c.url));
  queueCmd(c);
}

void radioPlayUrl(const char* url, const char* name)
{
  Cmd c{CmdType::Play, -1, 0, "", ""};
  strlcpy(c.name, name && *name ? name : "Radio", sizeof(c.name));
  strlcpy(c.url, url, sizeof(c.url));
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
  xSemaphoreTake(stationsMutex, portMAX_DELAY);
  autoplay = v;
  xSemaphoreGive(stationsMutex);
  Preferences p;
  if (p.begin(NVS_NAMESPACE, false)) {
    p.putBool("auto", v.enabled);
    p.putUChar("autost", (uint8_t)v.station);
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

const char* radioStateName(RadioState s)
{
  switch (s) {
    case RadioState::Connecting: return "connecting";
    case RadioState::Playing: return "playing";
    case RadioState::Retrying: return "retrying";
    default: return "stopped";
  }
}
