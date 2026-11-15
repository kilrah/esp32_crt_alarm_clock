// ESP32 weather clock on a composite (PAL/NTSC) TV screen.
// Shows time, current conditions and the forecast from Home Assistant in the
// style of the clock-weather-card. Settings: http://weatherclock.local/
#include <Arduino.h>
#include "config.h"
#include "crt.h"
#include "ha_client.h"
#include "radio.h"
#include "settings.h"
#include "ui.h"
#include "webui.h"

// RAM is tight without PSRAM: the loop task needs ~4 KB (measured), default is 8 KB
SET_LOOP_TASK_STACK_SIZE(6 * 1024);

static LGFX_CVBS display;
static WeatherUi ui;
static WeatherData weather;

static constexpr time_t MIN_VALID_TIME = 1700000000;  // 2023-11
static bool timeValid() { return time(nullptr) > MIN_VALID_TIME; }

static void applyTimezone(const char* tz)
{
  setenv("TZ", tz, 1);
  tzset();
}

// What the TV currently shows
enum class Screen : uint8_t { Boot, Setup, Weather, Ota };
static Screen screen = Screen::Boot;

static void showSetupScreen(const NetStatus& net)
{
  const Settings s = settingsGet();
  char l1[64], l3[48];
  if (s.wifiSsid[0]) snprintf(l1, sizeof(l1), "WiFi \"%s\" not reachable.", s.wifiSsid);
  else snprintf(l1, sizeof(l1), "No WiFi configured.");
  snprintf(l3, sizeof(l3), "http://%s/", net.ip.toString().c_str());
  ui.showMessage("Setup", l1, "Join WiFi \"" DEVICE_HOSTNAME "\" and open", l3);
}

static bool fetchWeather()
{
  const Settings s = settingsGet();
  HaConfig cfg{s.haUrl, s.haToken, s.entity};
  bool ok = haUpdateWeather(cfg, FORECAST_HOURLY, weather);
  webUiSetWeatherStatus(weather.lastUpdate, weather.error);
  return ok;
}

void setup()
{
  Serial.begin(115200);

  display.init();
#if CVBS_COLOR_DEPTH == 16
  display.setColorDepth(16);
#endif
  ui.begin(&display);
  Serial.printf("Free heap after display init: %u (largest block %u)\n", ESP.getFreeHeap(),
                heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  settingsBegin();
  const Settings s = settingsGet();
  configTzTime(s.timezone, NTP_SERVER_1, NTP_SERVER_2);  // SNTP starts once WiFi is up

  ui.showMessage(DEVICE_TITLE, "Connecting to WiFi", s.wifiSsid[0] ? s.wifiSsid : nullptr);
  radioBegin();  // audio task on core 0, idle until a station is played
  webUiBegin();  // WiFi, settings page and OTA run in their own task
}

// Updates the radio line on the TV. Returns true if the forecast rows must be
// redrawn (the line appeared or disappeared).
static bool updateNowPlaying()
{
  const RadioStatus r = radioStatus();
  const bool show = r.state != RadioState::Stopped;
  const char* title = r.state == RadioState::Connecting ? "Connecting..."
                    : r.state == RadioState::Retrying   ? "Reconnecting..."
                                                        : r.title;
  const bool wasShown = ui.nowPlayingShown();
  ui.setNowPlaying(show ? r.name : "", title);
  return show != wasShown;
}

// Diagnostics (DEBUG_HEAP_LOG_S in config.h): byte-addressable heap and the unused
// stack of each task. RAM is tight without PSRAM; use this after changes.
static void heapReport()
{
#if DEBUG_HEAP_LOG_S > 0
  static uint32_t last = 0;
  if (millis() - last < DEBUG_HEAP_LOG_S * 1000UL) return;
  last = millis();
  Serial.printf("[Radio] %s, underruns %u\n", radioStateName(radioStatus().state), (unsigned)radioUnderruns());
  Serial.printf("[Heap] 8bit free %u largest %u min-ever %u | internal %u | stack free: loop %u",
                heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT), heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                uxTaskGetStackHighWaterMark(nullptr));
  for (const char* n : {"web", "audio", "mdns", "tiT", "wifi", "async_tcp", "sys_evt", "arduino_events"}) {
    TaskHandle_t h = xTaskGetHandle(n);
    if (h) Serial.printf(" %s %u", n, uxTaskGetStackHighWaterMark(h));
  }
  Serial.println();
#endif
}

void loop()
{
  heapReport();
  static uint32_t lastFetch = 0;
  static bool fetchDue = true;
  static bool lastFetchOk = false;
  static uint32_t settingsVer = settingsVersion();
  static int lastMinute = -1;
  static int lastKey = -1;
  static IPAddress shownIp;
  static uint32_t radioVer = 0;
  static bool radioLayoutChanged = false;

  // Firmware update in progress: show it and leave the CPU (and RAM) to the upload
  if (otaInProgress) {
    if (screen != Screen::Ota) {
      radioStop();
      ui.showMessage("Firmware update", "Do not switch off");
      screen = Screen::Ota;
    }
    delay(100);
    return;
  }

  // Settings changed on the web page: new timezone, reload the weather
  if (settingsVersion() != settingsVer) {
    settingsVer = settingsVersion();
    applyTimezone(settingsGet().timezone);
    fetchDue = true;
    lastKey = -1;  // day labels may change with the timezone
  }
  if (webUiTakeRefreshRequest()) fetchDue = true;

  // Radio status changed: update the line on the TV (drawn below if on the weather screen)
  bool radioTitleChanged = false;
  if (radioStatusVersion() != radioVer) {
    radioVer = radioStatusVersion();
    if (updateNowPlaying()) radioLayoutChanged = true;
    else radioTitleChanged = true;
  }

  const NetStatus net = webUiNetStatus();

  // Own access point: show how to reach the setup page
  if (net.state == NetState::OwnAp) {
    if (screen != Screen::Setup || net.ip != shownIp) {
      showSetupScreen(net);
      screen = Screen::Setup;
      shownIp = net.ip;
    }
    delay(50);
    return;
  }
  // Still connecting after boot (or after the setup screen): keep the message up
  if (net.state == NetState::Joining && screen != Screen::Weather) {
    delay(50);
    return;
  }

  bool full = false;
  if (screen != Screen::Weather) {
    screen = Screen::Weather;
    full = true;
    fetchDue = fetchDue || !weather.valid;
  }

  char hint[64];
  snprintf(hint, sizeof(hint), "Settings: http://%s/", net.ip.toString().c_str());
  ui.setHint(hint);

  // Weather update (only with a network; a lost connection just lets the data age)
  const uint32_t now = millis();
  const uint32_t interval = lastFetchOk ? WEATHER_REFRESH_S * 1000UL : 30000UL;  // retry faster on error
  if (net.state == NetState::Joined && (fetchDue || now - lastFetch >= interval)) {
    if (!weather.valid) {
      ui.showMessage(DEVICE_TITLE, "Loading weather from", settingsGet().haUrl, hint);
      // Give SNTP a moment on the first fetch so "Today" is right
      for (int i = 0; i < 50 && !timeValid(); ++i) delay(100);
    }
    fetchDue = false;
    lastFetch = millis();
    lastFetchOk = fetchWeather();
    full = true;
  }

  // Autoplay once per boot: after WiFi is up and the first weather fetch is done
  // (not in parallel with it - RAM is tight)
  static bool autoplayDone = false;
  if (!autoplayDone && net.state == NetState::Joined && !fetchDue) {
    autoplayDone = true;
    const RadioAutoplay ap = radioAutoplay();
    if (ap.enabled) {
      Serial.printf("[Radio] Autoplay station %d\n", ap.station + 1);
      radioPlay(ap.station);
    }
  }

  time_t t = time(nullptr);
  tm lt;
  localtime_r(&t, &lt);
  if (full) {
    ui.render(weather, t, timeValid());
    lastMinute = lt.tm_min;
    lastKey = FORECAST_HOURLY ? lt.tm_hour : lt.tm_yday;
    radioLayoutChanged = false;
  } else if (radioLayoutChanged) {
    radioLayoutChanged = false;
    ui.renderForecast(weather, t);  // rows make room for / reclaim the radio line
  } else if (radioTitleChanged) {
    ui.renderNowPlaying();
  }
  if (!full && lt.tm_min != lastMinute) {
    lastMinute = lt.tm_min;
    ui.renderHeader(weather, t, timeValid());
    // New day or new hour: labels ("Today", "Now") change
    const int key = FORECAST_HOURLY ? lt.tm_hour : lt.tm_yday;
    if (key != lastKey) {
      ui.renderForecast(weather, t);
      lastKey = key;
    }
  }

  // Long song titles scroll: faster loop for smooth movement (~30 px/s)
  const bool scrolling = ui.animateNowPlaying(millis());
  delay(scrolling ? 20 : 50);
}
