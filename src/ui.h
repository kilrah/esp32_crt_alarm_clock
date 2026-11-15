#pragma once
// Screen rendering, modeled after the layout of the Lovelace clock-weather-card.
// Free of Arduino dependencies so it can be rendered on a PC (tools/preview).
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <cstdio>
#include "weather.h"

class WeatherUi {
 public:
  // `display` must already be initialised. Allocates an off-screen band buffer
  // (falls back to drawing directly if there isn't enough RAM).
  void begin(lgfx::LovyanGFX* display);

  // Redraw everything.
  void render(const WeatherData& w, time_t nowUtc, bool timeValid);
  // Redraw only the header (current conditions + clock), e.g. once a minute.
  void renderHeader(const WeatherData& w, time_t nowUtc, bool timeValid);
  // Redraw only the forecast rows.
  void renderForecast(const WeatherData& w, time_t nowUtc);

  // Radio line at the bottom of the screen ("" station = hidden). Changing whether
  // it is shown needs renderForecast() (rows move); a new title only renderNowPlaying().
  void setNowPlaying(const char* station, const char* title);
  bool nowPlayingShown() const { return _nowPlaying[0] != 0; }
  void renderNowPlaying();

  // Second line shown under the error when there is no weather data (e.g. where
  // the settings page is). Empty string = none.
  void setHint(const char* hint) { snprintf(_hint, sizeof(_hint), "%s", hint ? hint : ""); }

  // Simple full-screen status message (boot, setup portal, errors).
  void showMessage(const char* title, const char* line1 = nullptr, const char* line2 = nullptr,
                   const char* line3 = nullptr);

 private:
  struct Row {
    const ForecastEntry* e;
    char label[16];
    char lo[8], hi[8];
    float tLo, tHi;
    bool isNow;
  };

  void drawHeader(lgfx::LovyanGFX* g, int ox, int oy, const WeatherData& w, time_t nowUtc, bool timeValid);
  void drawRow(lgfx::LovyanGFX* g, int ox, int oy, int rowH, const Row& r, int labelW, int tempW,
               float gMin, float gMax, const WeatherData& w);
  void drawTempBar(lgfx::LovyanGFX* g, int x, int y, int w, int h, float gMin, float gMax, float lo, float hi,
                   float marker);
  void drawNowPlaying(lgfx::LovyanGFX* g, int ox, int oy);
  void pushBand(int x, int y, int w, int h);

  static constexpr int BAND_H = 30;         // off-screen strip height (= max row height)
  static constexpr int NOW_PLAYING_H = 22;  // radio line height
  bool isNight(const WeatherData& w, time_t nowUtc) const;
  bool fahrenheit(const WeatherData& w) const;

  lgfx::LovyanGFX* _d = nullptr;
  lgfx::LGFX_Sprite* _band = nullptr;  // off-screen buffer, content width x header height
  int _x = 0, _y = 0, _w = 0, _h = 0;  // content rectangle
  int _headerH = 0;
  bool _dither = false;
  bool _fahr = false;
  char _hint[64] = "";
  char _nowPlaying[32] = "";
  char _nowTitle[96] = "";
};
