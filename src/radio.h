#pragma once
#include <Arduino.h>

// Web radio: http:// MP3 streams decoded on the ESP32 and played through an I2S DAC
// (PCM5102) on I2S1, the second internal DAC or a PWM pin (AUDIO_OUTPUT). Decoding runs in its own task on core 0. All functions below
// are thread-safe and non-blocking (commands are queued to the audio task).

constexpr int RADIO_FADE_MAX_S = 60;  // longest volume fade-in

struct Station {
  char name[32];
  char url[128];  // "" = empty slot
};

// Backup = no stream for RADIO_BACKUP_AFTER_S: the alarm sound from flash is playing
// (the stream is still retried, each time the sound has played through)
enum class RadioState : uint8_t { Stopped, Connecting, Playing, Retrying, Backup };

struct RadioStatus {
  RadioState state;
  int station;       // index into the station list, -1 = custom URL / none
  char name[32];     // station name
  char title[96];    // ICY StreamTitle (UTF-8), "" if none
  char error[64];    // last error, "" if none
  uint8_t volume;    // 0-100
};

void radioBegin();  // load stations + volume from NVS, start the audio task

// Play from the station list. If there is no stream for RADIO_BACKUP_AFTER_S, the alarm
// sound from flash plays instead (a wake-up must not fail silently). fromBoot = count
// that time from power-up instead of from this call (autoplay: the power-up is the alarm).
// fadeS = fade the volume in from silence over this many seconds (0-RADIO_FADE_MAX_S),
// starting with the first sound; it ends at the set volume.
void radioPlay(int station, bool fromBoot = false, int fadeS = 0);
void radioPlayUrl(const char* url, const char* name, int fadeS = 0);
void radioStop();
void radioSetVolume(int volume);                  // 0-100, persisted

RadioStatus radioStatus();
uint32_t radioStatusVersion();  // increments whenever the status changes (lock-free)
// Samples that could not be played because the decoder fell behind, since boot
// (internal DAC / PWM output only, 0 with I2S)
uint32_t radioUnderruns();
// Diagnostics: longest time (ms) the output went without new samples since the last
// call (I2S output only). More than the output buffer holds (AUDIO_I2S_DMA_BUFFERS) is
// an audible gap.
uint32_t radioMaxGapMs();
// Diagnostics: percent of the time since the last call that the audio task spent in the
// decoder (decoding, writing samples, waiting for stream data). Near 100 = can't keep up.
int radioAudioLoad();
// Diagnostics: times the network buffer (RADIO_BUFFER_BYTES) ran empty since boot: the
// stream did not arrive fast enough. Each one is a pause of up to 0.5 s.
uint32_t radioStreamDry();

// Autoplay: start a preset automatically after boot (persisted). Works without WiFi too:
// the backup alarm sound plays then.
struct RadioAutoplay {
  bool enabled;
  int station;  // 0-based preset index
  int fadeS;    // volume fade-in in seconds, 0 = none
};
RadioAutoplay radioAutoplay();
void radioSetAutoplay(const RadioAutoplay& a);

Station radioStation(int index);                  // copy; empty if out of range
void radioSetStation(int index, const Station& s);  // persist one slot
const char* validateStation(const Station& s);      // nullptr if OK

const char* radioOutputName();  // compiled-in audio output (AUDIO_OUTPUT), for display
const char* radioStateName(RadioState s);  // "stopped", "connecting", "playing", "retrying", "backup"
// Size in bytes of the backup alarm sound found in flash at boot, 0 = none (or disabled)
uint32_t radioBackupSize();
