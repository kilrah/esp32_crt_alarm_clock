#include "ui.h"
#include "icons.h"
#include "config.h"
#include "fonts.h"
#include <cstdio>
#include <cstring>
#include <cmath>

using lgfx::LovyanGFX;
using lgfx::textdatum_t;

namespace {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

const uint16_t C_BG      = rgb(0, 0, 0);
const uint16_t C_TEXT    = rgb(255, 255, 255);
const uint16_t C_DIM     = rgb(165, 170, 180);
#if CVBS_GRAYSCALE
const uint16_t C_TRACK   = rgb(36, 36, 36);
const uint16_t C_WARN    = rgb(230, 230, 230);
const uint16_t C_HUMID   = rgb(200, 200, 200);
#else
const uint16_t C_TRACK   = rgb(40, 40, 85);  // maps to a neutral dark blue-grey in RGB332
const uint16_t C_WARN    = rgb(255, 120, 40);
const uint16_t C_HUMID   = rgb(70, 160, 255);
#endif

const lgfx::U8g2font fontSmall(u8g2_font_helvR14_tf);
const lgfx::U8g2font fontMedium(u8g2_font_helvR18_tf);

// Approximate cap heights (pixels above the baseline) used for vertical layout
constexpr int CAP_SMALL = 13;
constexpr int CAP_MEDIUM = 18;
struct ClockFont { const lgfx::IFont* font; int cap; };
const ClockFont CLOCK_FONTS[] = {
  {&lgfx::fonts::DejaVu72, 53},
  {&lgfx::fonts::DejaVu56, 41},
  {&lgfx::fonts::DejaVu40, 29},
  {&lgfx::fonts::DejaVu24, 18},
};

// ---- Texts -----------------------------------------------------------------
struct CondName { const char* key; const char* text; };
const CondName CONDITIONS[] = {
  {"clear-night", "Clear night"},
  {"cloudy", "Cloudy"},
  {"exceptional", "Exceptional"},
  {"fog", "Fog"},
  {"hail", "Hail"},
  {"lightning", "Lightning"},
  {"lightning-rainy", "Thunderstorm"},
  {"partlycloudy", "Partly cloudy"},
  {"pouring", "Pouring"},
  {"rainy", "Rainy"},
  {"snowy", "Snowy"},
  {"snowy-rainy", "Sleet"},
  {"sunny", "Sunny"},
  {"windy", "Windy"},
  {"windy-variant", "Windy, cloudy"},
};

const char* const DAY_SHORT[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const DAY_LONG[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* const MON_SHORT[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char* const MON_LONG[] = {"January", "February", "March", "April", "May", "June",
                                "July", "August", "September", "October", "November", "December"};
const char* const STR_TODAY = "Today";
const char* const STR_TOMORROW = "Tomorrow";
const char* const STR_NOW = "Now";
const char* const STR_NO_DATA = "No data";
const char* const STR_NO_FORECAST = "No forecast";

const char* conditionText(const char* key)
{
  for (auto& c : CONDITIONS)
    if (strcmp(c.key, key) == 0) return c.text;
  return key;
}

void formatDate(char* out, size_t n, const tm& t, bool longForm)
{
  if (longForm) snprintf(out, n, "%s, %s %d", DAY_LONG[t.tm_wday], MON_LONG[t.tm_mon], t.tm_mday);
  else snprintf(out, n, "%s, %s %d", DAY_SHORT[t.tm_wday], MON_SHORT[t.tm_mon], t.tm_mday);
}

// Shortens `s` (UTF-8) in place with ".." until it fits into maxW pixels.
void fitText(LovyanGFX* g, char* s, int maxW)
{
  if (g->textWidth(s) <= maxW) return;
  size_t len = strlen(s);
  while (len > 1) {
    // drop one UTF-8 code point
    do { --len; } while (len > 0 && (s[len] & 0xC0) == 0x80);
    while (len > 0 && s[len - 1] == ' ') --len;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%.*s..", (int)len, s);
    if (g->textWidth(tmp) <= maxW || len <= 1) {
      strcpy(s, tmp);
      return;
    }
  }
}

void formatTemp(char* out, size_t n, float t, bool decimal, const char* suffix)
{
  if (std::isnan(t)) { snprintf(out, n, "--%s", suffix); return; }
  if (decimal) snprintf(out, n, "%.1f%s", t, suffix);
  else {
    int v = (int)lroundf(t);
    if (v == 0) v = 0;  // avoid "-0"
    snprintf(out, n, "%d%s", v, suffix);
  }
}

// Temperature -> color gradient, same idea as clock-weather-card (stops in °C)
struct Stop { float t; uint8_t r, g, b; };
const Stop GRADIENT[] = {
  {-20, 0, 60, 98},
  {-10, 120, 162, 204},
  {0, 164, 195, 210},
  {10, 121, 210, 179},
  {20, 252, 245, 112},
  {30, 255, 150, 79},
  {40, 245, 85, 70},
};

void tempColor(float c, uint8_t& r, uint8_t& g, uint8_t& b)
{
#if CVBS_GRAYSCALE
  // Brightness ramp instead of hues: -20 °C dark grey ... +40 °C white
  float f = (c + 20.0f) / 60.0f;
  f = f < 0 ? 0 : f > 1 ? 1 : f;
  r = g = b = (uint8_t)lroundf(80 + f * 175);
  return;
#endif
  const int n = sizeof(GRADIENT) / sizeof(GRADIENT[0]);
  if (c <= GRADIENT[0].t) { r = GRADIENT[0].r; g = GRADIENT[0].g; b = GRADIENT[0].b; return; }
  for (int i = 1; i < n; ++i) {
    if (c <= GRADIENT[i].t) {
      const Stop& a = GRADIENT[i - 1];
      const Stop& z = GRADIENT[i];
      float f = (c - a.t) / (z.t - a.t);
      r = (uint8_t)lroundf(a.r + (z.r - a.r) * f);
      g = (uint8_t)lroundf(a.g + (z.g - a.g) * f);
      b = (uint8_t)lroundf(a.b + (z.b - a.b) * f);
      return;
    }
  }
  r = GRADIENT[n - 1].r; g = GRADIENT[n - 1].g; b = GRADIENT[n - 1].b;
}

uint8_t quantize(uint8_t v, int levels, float threshold)
{
  int q = (int)(v * (levels - 1) / 255.0f + threshold);
  return (uint8_t)(q < 0 ? 0 : q > levels - 1 ? levels - 1 : q);
}

}  // namespace

// ---------------------------------------------------------------------------

void WeatherUi::begin(LovyanGFX* display)
{
  _d = display;
  _x = SCREEN_MARGIN;
  _y = SCREEN_MARGIN;
  _w = _d->width() - 2 * SCREEN_MARGIN;
  _h = _d->height() - 2 * SCREEN_MARGIN;
  _headerH = _h * 40 / 100;
  if (_headerH > 110) _headerH = 110;
  _dither = GRADIENT_DITHER && _d->getColorDepth() == lgfx::color_depth_t::rgb332_1Byte;

  // Off-screen buffer for one strip: a forecast row, or a slice of the header
  // (the header is drawn strip by strip; saves ~17 KB RAM for the web radio)
  _band = new lgfx::LGFX_Sprite(_d);
  _band->setColorDepth(_d->getColorDepth());
  if (!_band->createSprite(_w, _headerH < BAND_H ? _headerH : BAND_H)) {
    delete _band;
    _band = nullptr;  // draw directly to the screen instead
  }
}

bool WeatherUi::fahrenheit(const WeatherData& w) const
{
  return strstr(w.unit, "F") != nullptr;
}

bool WeatherUi::isNight(const WeatherData& w, time_t nowUtc) const
{
  if (w.sunUp >= 0) return w.sunUp == 0;
  tm lt;
  localtime_r(&nowUtc, &lt);
  return lt.tm_hour < 7 || lt.tm_hour >= 19;
}

void WeatherUi::pushBand(int x, int y, int w, int h)
{
  _d->setClipRect(x, y, w, h);
  _band->pushSprite(x, y);
  _d->clearClipRect();
}

void WeatherUi::render(const WeatherData& w, time_t nowUtc, bool timeValid)
{
  renderHeader(w, nowUtc, timeValid);
  renderForecast(w, nowUtc);
}

void WeatherUi::renderHeader(const WeatherData& w, time_t nowUtc, bool timeValid)
{
  if (_band) {
    const int bh = _band->height();
    for (int y0 = 0; y0 < _headerH; y0 += bh) {
      drawHeader(_band, 0, -y0, w, nowUtc, timeValid);  // sprite clips to this strip
      pushBand(_x, _y + y0, _w, _headerH - y0 < bh ? _headerH - y0 : bh);
    }
  } else {
    drawHeader(_d, _x, _y, w, nowUtc, timeValid);
  }
}

void WeatherUi::drawHeader(LovyanGFX* g, int ox, int oy, const WeatherData& w, time_t nowUtc, bool timeValid)
{
  const int W = _w, H = _headerH;
  g->fillRect(ox, oy, W, H, C_BG);
  g->setTextDatum(textdatum_t::baseline_left);

  // ---- Right block: clock + date -------------------------------------------
  tm lt;
  localtime_r(&nowUtc, &lt);
  char timeStr[8], ampm[4] = "";
  if (!timeValid) {
    strcpy(timeStr, "--:--");
  } else if (CLOCK_24H) {
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", lt.tm_hour, lt.tm_min);
  } else {
    int h12 = lt.tm_hour % 12;
    snprintf(timeStr, sizeof(timeStr), "%d:%02d", h12 == 0 ? 12 : h12, lt.tm_min);
    strcpy(ampm, lt.tm_hour < 12 ? "AM" : "PM");
  }
  char secStr[4] = "";
#if CLOCK_SECONDS
  if (timeValid) snprintf(secStr, sizeof(secStr), "%02d", lt.tm_sec);
  else strcpy(secStr, "--");
#endif
  char date[40] = "";
  if (timeValid) formatDate(date, sizeof(date), lt, true);

  char dateShort[40] = "";
  if (timeValid) formatDate(dateShort, sizeof(dateShort), lt, false);

  // Left block texts
  char cond[64], cond2[64] = "";
  snprintf(cond, sizeof(cond), "%s", w.valid ? conditionText(w.condition) : STR_NO_DATA);
  char temp[16];
  formatTemp(temp, sizeof(temp), w.valid ? w.temperature : NAN, SHOW_DECIMAL, w.unit);
  char hum[8] = "";
  const int dropS = 14;
#if SHOW_HUMIDITY
  if (w.valid && !std::isnan(w.humidity)) snprintf(hum, sizeof(hum), "%d%%", (int)lroundf(w.humidity));
#endif

  const int iconS = H < 84 ? H : 84;
  const int gapV = 6;
  const int tx = ox + iconS + 8;

  g->setFont(&fontMedium);
  const int tempW = g->textWidth(temp);
  const int secW = secStr[0] ? g->textWidth("00") + 4 : 0;  // fixed width: no jitter
  g->setFont(&fontSmall);
  const int ampmW = ampm[0] ? g->textWidth(ampm) + 3 : 0;
  const int condW = g->textWidth(cond);
  const int humW = hum[0] ? 8 + dropS + 1 + g->textWidth(hum) : 0;

  // Column right of HH:MM: AM/PM at the top, seconds at the bottom. Stacked when the
  // clock is tall enough, otherwise side by side.
  auto stacked = [&](const ClockFont& f) { return f.cap >= CAP_SMALL + 4 + CAP_MEDIUM; };
  auto sideWidth = [&](const ClockFont& f) {
    if (ampmW && secW && !stacked(f)) return ampmW + secW + 6;  // "PM 00"
    return ampmW > secW ? ampmW : secW;
  };

  // Two layouts, picked from fixed reference sizes (so nothing jumps from minute to minute
  // or when the weather changes):
  //  A: left column (condition, temperature, humidity) next to the clock + date, both
  //     centered vertically. Needs minTextW next to the clock.
  //  B: condition on its own line across the full width, the clock moved down below it;
  //     temperature on the clock's baseline, humidity next to it or on the date's line.
  //     Only the temperature has to fit next to the clock, so a bigger clock fits.
  // Biggest clock wins; per size A is preferred (the classic look).
  const int fullTextW = W - (tx - ox);
  const int condGap = 8;  // condition baseline -> top of the clock digits (layout B)
  g->setFont(&fontMedium);
  const int tempRefW = g->textWidth(SHOW_DECIMAL ? "-88.8\xC2\xB0" "C" : "-88\xC2\xB0" "C");
  const int minTextW = ampm[0] ? 56 : 90;
  const int nFonts = sizeof(CLOCK_FONTS) / sizeof(CLOCK_FONTS[0]);
  const ClockFont* cf = &CLOCK_FONTS[nFonts - 1];
  bool layoutB = false;
  for (auto& f : CLOCK_FONTS) {
    g->setFont(f.font);
    const int cw = g->textWidth("00:00") + sideWidth(f);
    const int besideW = fullTextW - cw - 10;
    if (f.cap + gapV + CAP_SMALL + 4 <= H && besideW >= minTextW) { cf = &f; break; }
    if (CAP_SMALL + condGap + f.cap + gapV + CAP_SMALL + 2 <= H && besideW >= tempRefW) {
      cf = &f;
      layoutB = true;
      break;
    }
  }
  const int sideW = sideWidth(*cf);
  g->setFont(cf->font);
  const int clockW = g->textWidth(timeStr) + sideW;
  int textW = layoutB ? fullTextW : W - clockW - 10 - (tx - ox);

  // Condition on one line, or wrapped onto two (split at the last fitting space)
  bool wrap = false;
  if (condW > textW && !layoutB) {
    g->setFont(&fontSmall);
    const char* best = nullptr;
    for (const char* p = strchr(cond, ' '); p; p = strchr(p + 1, ' ')) {
      char first[64];
      snprintf(first, sizeof(first), "%.*s", (int)(p - cond), cond);
      if (g->textWidth(first) <= textW && g->textWidth(p + 1) <= textW) best = p;
    }
    if (best) {
      snprintf(cond2, sizeof(cond2), "%s", best + 1);
      cond[best - cond] = 0;
      wrap = true;
    }
  }

  // Long date if it is not wider than the clock (or, in layout A, the available room)
  g->setFont(&fontSmall);
  int dateW = g->textWidth(date);
  if (dateW > clockW && (layoutB || dateW > W - (tx - ox) - textW - 10)) {
    strcpy(date, dateShort);
    dateW = g->textWidth(date);
  }
  const int rightW = clockW > dateW ? clockW : dateW;
  if (!layoutB && rightW > clockW) textW = W - rightW - 10 - (tx - ox);

  int clockBase, condBase;
  if (layoutB) {
    const int blockH = CAP_SMALL + condGap + cf->cap + gapV + CAP_SMALL + 2;
    condBase = oy + (H - blockH) / 2 + CAP_SMALL;
    clockBase = condBase + condGap + cf->cap;
  } else {
    const int blockH = cf->cap + gapV + CAP_SMALL;
    clockBase = oy + (H - blockH) / 2 + cf->cap;
    condBase = 0;  // set below
  }
  const int dateBase = clockBase + gapV + CAP_SMALL + 2;
  const int right = ox + W;

  g->setFont(cf->font);
  g->setTextColor(C_TEXT);
  g->setTextDatum(textdatum_t::baseline_right);
  g->drawString(timeStr, right - sideW, clockBase);
  g->setTextDatum(textdatum_t::baseline_left);
  if (ampm[0]) {
    g->setFont(&fontSmall);
    g->drawString(ampm, right - sideW + 3, clockBase - cf->cap + CAP_SMALL);
  }
  if (secStr[0]) {
    g->setFont(&fontMedium);
    const int sx = (ampmW && !stacked(*cf)) ? right - secW + 4 : right - sideW + 4;
    g->drawString(secStr, sx, clockBase);
  }
  g->setFont(&fontSmall);
  g->setTextColor(C_DIM);
  g->setTextDatum(textdatum_t::baseline_right);
  g->drawString(date, right, dateBase);
  g->setTextDatum(textdatum_t::baseline_left);

  // ---- Left block: icon, condition, temperature ------------------------------
  const bool stale = !w.valid || (w.lastUpdate > 0 && nowUtc - w.lastUpdate > STALE_AFTER_S);
  const int iconY = oy + (H - iconS) / 2;
  if (w.valid) drawWeatherIcon(g, ox, iconY, iconS, w.condition, isNight(w, nowUtc), C_BG);
  else drawWeatherIcon(g, ox, iconY, iconS, "exceptional", false, C_BG);

  const int lineGap = 5;
  int tempBase;
  bool humInline, humLine;
  int humLineBase;
  if (layoutB) {
    // Temperature on the clock's baseline; humidity next to it or on the date's line
    tempBase = clockBase;
    humInline = hum[0] && tempW + humW <= fullTextW - clockW - 10;
    humLine = hum[0] && !humInline && humW - 8 <= fullTextW - dateW - 10;
    humLineBase = dateBase;
  } else {
    int leftBlockH = CAP_SMALL + 12 + CAP_MEDIUM + (wrap ? CAP_SMALL + lineGap : 0);
    // Humidity next to the temperature, or on its own line below if there's room
    humInline = hum[0] && tempW + humW <= textW;
    humLine = hum[0] && !humInline && leftBlockH + 8 + CAP_SMALL <= H - 6;
    if (humLine) leftBlockH += 8 + CAP_SMALL;
    condBase = oy + (H - leftBlockH) / 2 + CAP_SMALL;
    tempBase = condBase + (wrap ? CAP_SMALL + lineGap : 0) + 12 + CAP_MEDIUM;
    humLineBase = tempBase + 8 + CAP_SMALL;
  }

  g->setFont(&fontSmall);
  g->setTextColor(C_DIM);
  fitText(g, cond, textW);
  g->drawString(cond, tx, condBase);
  if (wrap) {
    fitText(g, cond2, textW);
    g->drawString(cond2, tx, condBase + CAP_SMALL + lineGap);
  }

  g->setFont(&fontMedium);
  g->setTextColor(stale ? C_DIM : C_TEXT);
  int x = tx + g->drawString(temp, tx, tempBase);

  if (humInline || humLine) {
    const int hx = humInline ? x + 8 : tx;
    const int hBase = humInline ? tempBase : humLineBase;
    g->setFont(&fontSmall);
    drawDropIcon(g, hx, hBase - dropS + 1, dropS, C_HUMID);
    g->setTextColor(C_DIM);
    g->drawString(hum, hx + dropS + 1, hBase);
  }

  // Outdated data / error indicator
  if (stale && w.valid) g->fillCircle(ox + 4, oy + 4, 4, C_WARN);
}

void WeatherUi::renderForecast(const WeatherData& w, time_t nowUtc)
{
  const int top = _y + _headerH + 4;
  const int nowH = _nowPlaying[0] ? NOW_PLAYING_H : 0;  // radio line at the bottom
  const int areaH = _y + _h - top - nowH;
  LovyanGFX* tg = _band ? (LovyanGFX*)_band : _d;
  _fahr = fahrenheit(w);
  if (nowH) renderNowPlaying();

  // Collect rows to show, skipping entries in the past
  tm now;
  localtime_r(&nowUtc, &now);
  Row rows[MAX_FORECAST];
  int n = 0;
  int maxRows = FORECAST_ROWS < MAX_FORECAST ? FORECAST_ROWS : MAX_FORECAST;
  const int minRowH = 24;
  if (maxRows > areaH / minRowH) maxRows = areaH / minRowH;

  for (int i = 0; w.valid && i < w.forecastCount && n < maxRows; ++i) {
    const ForecastEntry& e = w.forecast[i];
    tm et;
    localtime_r(&e.time, &et);
    Row& r = rows[n];
    r.e = &e;
    r.isNow = false;
    if (w.hourly) {
      if (e.time + 3600 <= nowUtc) continue;
      r.isNow = e.time <= nowUtc;
      if (r.isNow) snprintf(r.label, sizeof(r.label), "%s", STR_NOW);
      else if (CLOCK_24H) snprintf(r.label, sizeof(r.label), "%02d:00", et.tm_hour);
      else snprintf(r.label, sizeof(r.label), "%d %s", et.tm_hour % 12 == 0 ? 12 : et.tm_hour % 12,
                    et.tm_hour < 12 ? "AM" : "PM");
    } else {
      // Compare calendar days in local time
      tm a = et, b = now;
      a.tm_hour = b.tm_hour = 12; a.tm_min = b.tm_min = 0; a.tm_sec = b.tm_sec = 0;
      a.tm_isdst = b.tm_isdst = -1;
      int dayDiff = (int)lround(difftime(mktime(&a), mktime(&b)) / 86400.0);
      if (dayDiff < 0) continue;
      r.isNow = dayDiff == 0;
      if (dayDiff == 0) snprintf(r.label, sizeof(r.label), "%s", STR_TODAY);
      else if (dayDiff == 1) snprintf(r.label, sizeof(r.label), "%s", STR_TOMORROW);
      else snprintf(r.label, sizeof(r.label), "%s", DAY_SHORT[et.tm_wday]);
    }
    r.tHi = e.tempHigh;
    r.tLo = std::isnan(e.tempLow) ? e.tempHigh : e.tempLow;
    if (r.tLo > r.tHi) { float t = r.tLo; r.tLo = r.tHi; r.tHi = t; }
    formatTemp(r.lo, sizeof(r.lo), r.tLo, false, "\xC2\xB0");
    formatTemp(r.hi, sizeof(r.hi), r.tHi, false, "\xC2\xB0");
    ++n;
  }

  // Clear the area outside the drawn rows (direct, it's background only)
  if (n == 0) {
    _d->fillRect(_x, top, _w, areaH, C_BG);
    _d->setFont(&fontSmall);
    _d->setTextColor(C_DIM);
    _d->setTextDatum(textdatum_t::middle_center);
    const char* msg = w.error[0] ? w.error : (w.valid ? STR_NO_FORECAST : STR_NO_DATA);
    if (_hint[0]) {
      _d->drawString(msg, _x + _w / 2, top + areaH / 2 - 12);
      _d->drawString(_hint, _x + _w / 2, top + areaH / 2 + 12);
    } else {
      _d->drawString(msg, _x + _w / 2, top + areaH / 2);
    }
    _d->setTextDatum(textdatum_t::baseline_left);
    return;
  }

  int rowH = areaH / n;
  if (rowH > 40) rowH = 40;
  if (_band && rowH > _band->height()) rowH = _band->height();

  // Global temperature range for bar scaling (includes current temperature)
  float gMin = 1e9f, gMax = -1e9f;
  for (int i = 0; i < n; ++i) {
    if (std::isnan(rows[i].tLo) || std::isnan(rows[i].tHi)) continue;
    if (rows[i].tLo < gMin) gMin = rows[i].tLo;
    if (rows[i].tHi > gMax) gMax = rows[i].tHi;
    if (rows[i].isNow && !std::isnan(w.temperature)) {
      if (w.temperature < gMin) gMin = w.temperature;
      if (w.temperature > gMax) gMax = w.temperature;
    }
  }
  if (gMin > gMax) { gMin = 0; gMax = 1; }
  if (gMax - gMin < 1) gMax = gMin + 1;

  // Column widths
  tg->setFont(&fontSmall);
  int labelW = 0, tempW = 0;
  for (int i = 0; i < n; ++i) {
    int lw = tg->textWidth(rows[i].label);
    if (lw > labelW) labelW = lw;
    int t1 = tg->textWidth(rows[i].lo), t2 = tg->textWidth(rows[i].hi);
    if (t1 > tempW) tempW = t1;
    if (t2 > tempW) tempW = t2;
  }

  const int used = rowH * n;
  const int y0 = top + (areaH - used) / 2;
  if (y0 > top) _d->fillRect(_x, top, _w, y0 - top, C_BG);
  if (y0 + used < top + areaH) _d->fillRect(_x, y0 + used, _w, top + areaH - (y0 + used), C_BG);

  for (int i = 0; i < n; ++i) {
    int ry = y0 + i * rowH;
    if (_band) {
      drawRow(_band, 0, 0, rowH, rows[i], labelW, tempW, gMin, gMax, w);
      pushBand(_x, ry, _w, rowH);
    } else {
      drawRow(_d, _x, ry, rowH, rows[i], labelW, tempW, gMin, gMax, w);
    }
  }
}

void WeatherUi::drawRow(LovyanGFX* g, int ox, int oy, int rowH, const Row& r, int labelW, int tempW,
                        float gMin, float gMax, const WeatherData& w)
{
  const int W = _w;
  g->fillRect(ox, oy, W, rowH, C_BG);
  g->setFont(&fontSmall);
  g->setTextDatum(textdatum_t::baseline_left);
  const int base = oy + (rowH + CAP_SMALL) / 2;

  g->setTextColor(C_TEXT);
  g->drawString(r.label, ox, base);

  int iconS = rowH - 2;
  if (iconS > 34) iconS = 34;
  const int iconX = ox + labelW + 8;
  bool night = false;
  if (w.hourly) night = r.e->isDaytime == 0 || (r.e->isDaytime < 0 && r.isNow && isNight(w, r.e->time));
  drawWeatherIcon(g, iconX, oy + (rowH - iconS) / 2, iconS, r.e->condition, night, C_BG);

  const int loRight = iconX + iconS + 8 + tempW;
  const int barX = loRight + 8;
  const int barW = ox + W - tempW - 8 - barX;
  int barH = rowH * 3 / 10;
  if (barH < 8) barH = 8;
  if (barH > 10) barH = 10;

  g->setTextColor(C_DIM);
  g->setTextDatum(textdatum_t::baseline_right);
  g->drawString(r.lo, loRight, base);
  g->setTextColor(C_TEXT);
  g->drawString(r.hi, ox + W, base);
  g->setTextDatum(textdatum_t::baseline_left);

  if (barW > 10 && !std::isnan(r.tLo)) {
    float marker = (r.isNow && !std::isnan(w.temperature)) ? w.temperature : NAN;
    drawTempBar(g, barX, oy + (rowH - barH) / 2, barW, barH, gMin, gMax, r.tLo, r.tHi, marker);
  }
}

void WeatherUi::drawTempBar(LovyanGFX* g, int x, int y, int w, int h, float gMin, float gMax, float lo, float hi,
                            float marker)
{
  const float rad = h / 2.0f;
  g->fillRoundRect(x, y, w, h, h / 2, C_TRACK);

  const float span = gMax - gMin;
  float sx0 = x + (lo - gMin) / span * w;
  float sx1 = x + (hi - gMin) / span * w;
  if (sx1 - sx0 < h) {  // keep at least a round dot visible
    float c = (sx0 + sx1) / 2;
    sx0 = c - h / 2.0f;
    sx1 = c + h / 2.0f;
    if (sx0 < x) { sx1 += x - sx0; sx0 = x; }
    if (sx1 > x + w) { sx0 -= sx1 - (x + w); sx1 = x + w; }
  }
  const float cy = y + rad;
  static const float ROW_THRESH[4] = {0.125f, 0.625f, 0.375f, 0.875f};

  for (int px = (int)floorf(sx0); px < (int)ceilf(sx1); ++px) {
    float fx = px + 0.5f;
    // Vertical extent of the capsule at this column
    float ext = rad;
    if (fx < sx0 + rad) {
      float d = sx0 + rad - fx;
      ext = d >= rad ? 0 : sqrtf(rad * rad - d * d);
    } else if (fx > sx1 - rad) {
      float d = fx - (sx1 - rad);
      ext = d >= rad ? 0 : sqrtf(rad * rad - d * d);
    }
    if (ext <= 0.3f) continue;
    float t = gMin + (fx - x) / w * span;
    float tc = _fahr ? (t - 32.0f) * 5.0f / 9.0f : t;  // gradient stops are in °C
    uint8_t cr, cg, cb;
    tempColor(tc, cr, cg, cb);
    int py0 = (int)lroundf(cy - ext), py1 = (int)lroundf(cy + ext);
    for (int py = py0; py < py1; ++py) {
      if (_dither) {
        float th = ROW_THRESH[py & 3];
        uint8_t c332 = (uint8_t)((quantize(cr, 8, th) << 5) | (quantize(cg, 8, th) << 2) | quantize(cb, 4, th));
        g->drawPixel(px, py, c332);
      } else {
        g->drawPixel(px, py, rgb(cr, cg, cb));
      }
    }
  }

  if (!std::isnan(marker)) {
    float mx = x + (marker - gMin) / span * w;
    int r = h / 2 + 2;
    if (mx < x + r) mx = x + r;
    if (mx > x + w - r) mx = x + w - r;
    g->fillCircle((int)lroundf(mx), (int)lroundf(cy - 0.5f), r + 1, C_BG);
    g->fillCircle((int)lroundf(mx), (int)lroundf(cy - 0.5f), r, C_TEXT);
  }
}

void WeatherUi::renderNowPlaying()
{
  if (!_nowPlaying[0]) return;
  const int y = _y + _h - NOW_PLAYING_H;
  if (_band) {
    drawNowPlaying(_band, 0, 0);
    pushBand(_x, y, _w, NOW_PLAYING_H);
  } else {
    drawNowPlaying(_d, _x, y);
  }
}

void WeatherUi::drawNowPlaying(LovyanGFX* g, int ox, int oy)
{
  const int H = NOW_PLAYING_H;
  g->fillRect(ox, oy, _w, H, C_BG);
  g->drawFastHLine(ox, oy + 1, _w, C_TRACK);
  const int iconS = 14;
  const int base = oy + (H + CAP_SMALL) / 2 + 1;
  drawSpeakerIcon(g, ox, base - iconS + 1, iconS, C_HUMID);

  // "Station · Title": station in white (shortened to fit), title dimmed; a title that
  // doesn't fit scrolls (animateNowPlaying)
  g->setFont(&fontSmall);
  g->setTextDatum(textdatum_t::baseline_left);
  int x = ox + iconS + 6;
  const int right = ox + _w;
  char name[64];  // fitText() needs 64-byte buffers
  snprintf(name, sizeof(name), "%s", _nowPlaying);
  fitText(g, name, right - x);
  g->setTextColor(C_TEXT);
  x += g->drawString(name, x, base);
  _titleW = 0;
  if (_nowTitle[0] && right - x > 40) {
    g->setTextColor(C_DIM);
    x += g->drawString(" \xC2\xB7 ", x, base);  // " · "
    const int areaW = right - x;
    const int tw = g->textWidth(_nowTitle);
    if (tw <= areaW) {
      g->drawString(_nowTitle, x, base);
    } else {
      _titleW = tw;
      int cx, cy, cw, ch;
      g->getClipRect(&cx, &cy, &cw, &ch);
      g->setClipRect(x, oy, areaW, H);
      const int tx = x - _scrollX;
      g->drawString(_nowTitle, tx, base);
      if (tx + tw < right) g->drawString(_nowTitle, tx + tw + SCROLL_GAP, base);  // next repeat
      g->setClipRect(cx, cy, cw, ch);
    }
  }
}

bool WeatherUi::animateNowPlaying(uint32_t ms)
{
  if (!_nowPlaying[0] || !_titleW) {
    _scrollRestart = true;
    return false;
  }
  if (_scrollRestart) {
    _scrollRestart = false;
    _scrollT0 = ms;
  }
  int pos = 0;
  const uint32_t t = ms - _scrollT0;
  if (t > SCROLL_PAUSE_MS) {
    pos = (int)((t - SCROLL_PAUSE_MS) * SCROLL_PX_PER_S / 1000);
    if (pos >= _titleW + SCROLL_GAP) {  // the repeat is now where the title started: pause
      pos = 0;
      _scrollT0 = ms;
    }
  }
  if (pos != _scrollX) {
    _scrollX = pos;
    renderNowPlaying();
  }
  return true;
}

void WeatherUi::setNowPlaying(const char* station, const char* title)
{
  if (strcmp(title ? title : "", _nowTitle) != 0 || strcmp(station ? station : "", _nowPlaying) != 0) {
    _scrollX = 0;  // new title: start from the beginning
    _scrollRestart = true;
  }
  snprintf(_nowPlaying, sizeof(_nowPlaying), "%s", station ? station : "");
  snprintf(_nowTitle, sizeof(_nowTitle), "%s", title ? title : "");
}

void WeatherUi::showMessage(const char* title, const char* l1, const char* l2, const char* l3)
{
  _d->fillScreen(C_BG);
  const int cx = _d->width() / 2;
  int y = _d->height() / 2 - 40;
  _d->setTextDatum(textdatum_t::baseline_center);
  _d->setFont(&fontMedium);
  _d->setTextColor(C_TEXT);
  if (title) _d->drawString(title, cx, y);
  y += 34;
  _d->setFont(&fontSmall);
  _d->setTextColor(C_DIM);
  for (const char* l : {l1, l2, l3}) {
    if (!l) continue;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", l);
    fitText(_d, buf, _d->width() - 2 * SCREEN_MARGIN);
    _d->drawString(buf, cx, y);
    y += 24;
  }
  _d->setTextDatum(textdatum_t::baseline_left);
}
