#pragma once
#include <Arduino.h>

// Web radio: http:// MP3 streams decoded on the ESP32 and played through an I2S DAC
// (PCM5102) on I2S1. Decoding runs in its own task on core 0. All functions below
// are thread-safe and non-blocking (commands are queued to the audio task).

struct Station {
  char name[32];
  char url[128];  // "" = empty slot
};

enum class RadioState : uint8_t { Stopped, Connecting, Playing, Retrying };

struct RadioStatus {
  RadioState state;
  int station;       // index into the station list, -1 = custom URL / none
  char name[32];     // station name
  char title[96];    // ICY StreamTitle (UTF-8), "" if none
  char error[64];    // last error, "" if none
  uint8_t volume;    // 0-100
};

void radioBegin();  // load stations + volume from NVS, start the audio task

void radioPlay(int station);                      // play from the station list
void radioPlayUrl(const char* url, const char* name);
void radioStop();
void radioSetVolume(int volume);                  // 0-100, persisted

RadioStatus radioStatus();
uint32_t radioStatusVersion();  // increments whenever the status changes (lock-free)

// Autoplay: start a preset automatically once WiFi is up after boot (persisted)
struct RadioAutoplay {
  bool enabled;
  int station;  // 0-based preset index
};
RadioAutoplay radioAutoplay();
void radioSetAutoplay(const RadioAutoplay& a);

Station radioStation(int index);                  // copy; empty if out of range
void radioSetStation(int index, const Station& s);  // persist one slot
const char* validateStation(const Station& s);      // nullptr if OK

const char* radioStateName(RadioState s);  // "stopped", "connecting", "playing", "retrying"
