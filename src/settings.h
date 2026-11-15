#pragma once
#include <Arduino.h>

// Runtime-changeable settings, persisted in NVS (survive reboot and reflash).
// Defaults come from config.h / secrets.ini. Shared between the web task and
// loop(): always go through settingsGet() (copy under a lock).

struct Settings {
  char wifiSsid[33];
  char wifiPass[64];   // never sent to the web page or printed
  char haUrl[128];     // e.g. http://homeassistant.local:8123 (no trailing slash)
  char haToken[256];   // never sent to the web page or printed
  char entity[64];     // e.g. weather.forecast_home
  char timezone[64];   // POSIX TZ string
};

void     settingsBegin();                    // load from NVS, fall back to defaults
Settings settingsGet();                      // thread-safe copy
void     settingsSave(const Settings& s);    // apply + persist
void     settingsResetDefaults();            // erase NVS, revert to config.h / secrets.ini
uint32_t settingsVersion();                  // increments on every change (cheap, lock-free)

// Validation helpers: nullptr if OK, else an error message
const char* validateWifiSsid(const String& ssid);
const char* validateWifiPass(const String& pass);
const char* validateHaUrl(const String& url);
const char* validateHaToken(const String& token);
const char* validateEntity(const String& entity);
const char* validateTimezone(const String& tz);
