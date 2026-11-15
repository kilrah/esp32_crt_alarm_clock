#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include <ctime>

// WiFi connection handling + settings web page + firmware update (web upload and
// ArduinoOTA), all running in their own FreeRTOS task so that slow Home Assistant
// requests in loop() never block the page and vice versa.
//
// WiFi: joins the configured network. If that fails for WIFI_JOIN_TIMEOUT_MS, an own
// access point (SSID = DEVICE_HOSTNAME) with a captive portal is opened; while nobody
// is connected to it, joining is retried every WIFI_RETRY_MS.

void webUiBegin();

enum class NetState : uint8_t { Joining, Joined, OwnAp };

struct NetStatus {
  NetState state;
  IPAddress ip;      // station IP when joined, AP IP on the own access point
  bool everJoined;   // joined at least once since boot
};
NetStatus webUiNetStatus();

// Weather status shown on the page (called from loop() after each update attempt)
void webUiSetWeatherStatus(time_t lastOk, const char* error);

// True once after "Refresh weather now" was clicked on the page
bool webUiTakeRefreshRequest();

// True while a firmware update (web upload or ArduinoOTA) is being received
extern volatile bool otaInProgress;
