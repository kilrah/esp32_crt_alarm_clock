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
  // Scrolls a title that is too long for the line (the station name stays put). Call
  // often (every ~20 ms) with a millisecond clock while the radio line is on screen;
  // redraws the line when the scroll position moved. Returns true while scrolling.
  bool animateNowPlaying(uint32_t ms);

  // Animates the large weather icon (ICON_ANIMATION in config.h). Call often with a
  // millisecond clock while the weather screen is shown; redraws only the icon, at
  // ICON_ANIM_FPS. Returns true while animating.
  bool animateIcon(const WeatherData& w, time_t nowUtc, uint32_t ms);

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
  // Left column of the header: large icon + "outdated" dot (background already cleared)
  void drawIconColumn(lgfx::LovyanGFX* g, int ox, int oy, const WeatherData& w, time_t nowUtc);
  int iconSize() const { return _headerH < 84 ? _headerH : 84; }
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
  // Title scrolling (marquee)
  static constexpr int SCROLL_GAP = 40;          // pixels between the end and the next repeat
  static constexpr int SCROLL_PX_PER_S = 30;
  static constexpr uint32_t SCROLL_PAUSE_MS = 2000;  // still at the start of each pass
  int _titleW = 0;           // pixel width of the title if it doesn't fit, 0 = fits / none
  int _scrollX = 0;          // current offset in pixels
  uint32_t _scrollT0 = 0;    // start of the current pass (incl. pause)
  bool _scrollRestart = true;
  // Large icon animation
  int32_t _iconMs = -1;      // animation clock of the frame on screen, -1 = static
  uint32_t _iconLast = 0;    // when that frame was drawn
};
