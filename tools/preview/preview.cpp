// Off-screen PC preview of the weather clock UI.
// Renders sample screens into an 8-bit sprite (same format as the CVBS frame
// buffer) and writes them as PPM images. See README in this folder.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <thread>
#include "ui.h"
#include "icons.h"
#include "config.h"

// ---- Platform shims for LovyanGFX (normally provided by the SDL backend) ----
namespace lgfx { inline namespace v1 {
static auto t0 = std::chrono::steady_clock::now();
unsigned long millis(void) { return (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(); }
unsigned long micros(void) { return (unsigned long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(); }
void delay(unsigned long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void delayMicroseconds(unsigned int us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
void gpio_hi(uint32_t) {}
void gpio_lo(uint32_t) {}
bool gpio_in(uint32_t) { return false; }
void pinMode(int_fast16_t, pin_mode_t) {}
void lgfxPinMode(int_fast16_t, pin_mode_t) {}
}}

static void writePpm(lgfx::LGFX_Sprite& s, const char* path, int scale)
{
  FILE* f = fopen(path, "wb");
  if (!f) { perror(path); return; }
  int w = s.width(), h = s.height();
  fprintf(f, "P6\n%d %d\n255\n", w * scale, h * scale);
  for (int y = 0; y < h; ++y)
    for (int sy = 0; sy < scale; ++sy)
      for (int x = 0; x < w; ++x) {
        auto c = s.readPixelRGB(x, y);
        for (int sx = 0; sx < scale; ++sx) { fputc(c.r, f); fputc(c.g, f); fputc(c.b, f); }
      }
  fclose(f);
  printf("wrote %s\n", path);
}

static void addDaily(WeatherData& w, time_t now, const char* const* conds, const float (*temps)[2], int n)
{
  w.hourly = false;
  w.forecastCount = n;
  for (int i = 0; i < n; ++i) {
    ForecastEntry& e = w.forecast[i];
    e.time = now + i * 86400;
    strcpy(e.condition, conds[i]);
    e.tempLow = temps[i][0];
    e.tempHigh = temps[i][1];
  }
}

int main(int argc, char** argv)
{
  const char* outDir = argc > 1 ? argv[1] : ".";
  _putenv("TZ=CET-1CEST");
  _tzset();

  lgfx::LGFX_Sprite screen;
#if CVBS_GRAYSCALE
  screen.setColorDepth(lgfx::color_depth_t::grayscale_8bit);
#else
  screen.setColorDepth(CVBS_COLOR_DEPTH);
#endif
  screen.createSprite(CVBS_PANEL_WIDTH, CVBS_PANEL_HEIGHT);
  WeatherUi ui;
  ui.begin(&screen);

  // Fixed "now": 2026-11-15 14:35 local
  tm t{};
  t.tm_year = 2026 - 1900; t.tm_mon = 10; t.tm_mday = 15; t.tm_hour = 14; t.tm_min = 35; t.tm_isdst = -1;
  time_t now = mktime(&t);
  char path[256];

  // 1) Typical autumn day
  {
    WeatherData w;
    w.valid = true;
    strcpy(w.condition, "partlycloudy");
    w.temperature = 8.4f;
    w.humidity = 71;
    w.sunUp = 1;
    w.lastUpdate = now;
    const char* conds[] = {"partlycloudy", "rainy", "cloudy", "sunny", "snowy-rainy", "fog"};
    const float temps[][2] = {{3, 10}, {5, 9}, {2, 7}, {-2, 6}, {-4, 1}, {0, 4}};
    addDaily(w, now, conds, temps, 6);
    ui.render(w, now, true);
    snprintf(path, sizeof(path), "%s/screen_day.ppm", outDir);
    writePpm(screen, path, 3);

    // 1b) Same with the web radio playing (UTF-8 title with umlauts, too long to fit)
    ui.setNowPlaying("Radio SRF 3", "Z\xC3\xBCri West - Ich wett nur dass du weisch was du mir bedeutisch");
    ui.render(w, now, true);
    snprintf(path, sizeof(path), "%s/screen_radio.ppm", outDir);
    writePpm(screen, path, 3);
    ui.setNowPlaying("", "");
  }
  // 2) Summer, night, stale data
  {
    WeatherData w;
    w.valid = true;
    strcpy(w.condition, "clear-night");
    w.temperature = 23.6f;
    w.humidity = 48;
    w.sunUp = 0;
    w.lastUpdate = now - 3600;
    const char* conds[] = {"lightning-rainy", "pouring", "windy", "hail", "lightning"};
    const float temps[][2] = {{17, 31}, {18, 27}, {15, 22}, {14, 25}, {19, 34}};
    addDaily(w, now, conds, temps, 5);
    ui.render(w, now, true);
    snprintf(path, sizeof(path), "%s/screen_night.ppm", outDir);
    writePpm(screen, path, 3);
  }
  // 3) No data yet / error
  {
    WeatherData w;
    strcpy(w.error, "HA: unauthorized, check token");
    ui.setHint("Settings: http://192.168.1.42/");
    ui.render(w, now, false);
    ui.setHint("");
    snprintf(path, sizeof(path), "%s/screen_error.ppm", outDir);
    writePpm(screen, path, 3);
  }
  // 3b) Hourly forecast, evening (forecast entries without templow)
  {
    WeatherData w;
    w.valid = true;
    w.hourly = true;
    strcpy(w.condition, "rainy");
    w.temperature = 12.7f;
    w.humidity = 93;
    w.sunUp = 1;
    w.lastUpdate = now;
    const char* conds[] = {"rainy", "rainy", "cloudy", "partlycloudy", "clear-night", "clear-night"};
    const float temps[] = {13, 12, 11, 10, 8, 7};
    w.forecastCount = 6;
    for (int i = 0; i < 6; ++i) {
      ForecastEntry& e = w.forecast[i];
      e.time = now - 35 * 60 + i * 3600;  // first entry is the current hour
      strcpy(e.condition, conds[i]);
      e.tempHigh = temps[i];
      e.isDaytime = i < 3;
    }
    ui.render(w, now, true);
    snprintf(path, sizeof(path), "%s/screen_hourly.ppm", outDir);
    writePpm(screen, path, 3);
  }
  // 4) Icon sheet
  {
    const char* all[] = {"sunny", "clear-night", "partlycloudy", "cloudy", "fog", "rainy", "pouring", "snowy",
                         "snowy-rainy", "hail", "lightning", "lightning-rainy", "windy", "windy-variant",
                         "exceptional", "partlycloudy"};
    lgfx::LGFX_Sprite sheet;
    sheet.setColorDepth(screen.getColorDepth());
    sheet.createSprite(4 * 72 + 4 * 36, 4 * 72);
    sheet.fillScreen(0);
    for (int i = 0; i < 16; ++i) {
      bool night = i == 15;
      drawWeatherIcon(&sheet, (i % 4) * 72 + 4, (i / 4) * 72 + 4, 64, all[i], night, 0);
      drawWeatherIcon(&sheet, 4 * 72 + (i % 4) * 36 + 4, (i / 4) * 72 + 4, 28, all[i], night, 0);
    }
    snprintf(path, sizeof(path), "%s/icons.ppm", outDir);
    writePpm(sheet, path, 3);
  }
  // 5) Boot message
  {
    // Same text as showSetupScreen() in main.cpp
    ui.showMessage("Setup", "WiFi \"MyHomeNetwork\" not reachable.",
                   "Join WiFi \"" DEVICE_HOSTNAME "\" and open", "http://192.168.4.1/");
    snprintf(path, sizeof(path), "%s/screen_setup.ppm", outDir);
    writePpm(screen, path, 3);
  }
  return 0;
}
