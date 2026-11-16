#include "icons.h"
#include "config.h"
#include <cstring>
#include <cmath>

using lgfx::LovyanGFX;

namespace {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

#if CVBS_GRAYSCALE
// Black & white TV: pick brightness, not hue (yellow sun would be darker than the clouds)
constexpr uint16_t gray(uint8_t v) { return rgb(v, v, v); }
const uint16_t C_SUN        = gray(255);
const uint16_t C_MOON       = gray(235);
const uint16_t C_CLOUD      = gray(170);
const uint16_t C_CLOUD_BACK = gray(115);
const uint16_t C_CLOUD_DARK = gray(100);
const uint16_t C_RAIN       = gray(235);
const uint16_t C_SNOW       = gray(255);
const uint16_t C_BOLT       = gray(255);
const uint16_t C_FOG        = gray(150);
const uint16_t C_WIND       = gray(210);
const uint16_t C_WARN       = gray(220);
#else
const uint16_t C_SUN        = rgb(255, 196, 30);
const uint16_t C_MOON       = rgb(235, 230, 190);
const uint16_t C_CLOUD      = rgb(215, 220, 230);
const uint16_t C_CLOUD_BACK = rgb(150, 156, 170);
const uint16_t C_CLOUD_DARK = rgb(125, 130, 145);
const uint16_t C_RAIN       = rgb(70, 160, 255);
const uint16_t C_SNOW       = rgb(245, 250, 255);
const uint16_t C_BOLT       = rgb(255, 215, 0);
const uint16_t C_FOG        = rgb(170, 175, 185);
const uint16_t C_WIND       = rgb(190, 200, 215);
const uint16_t C_WARN       = rgb(255, 120, 40);
#endif

// Maps a 64x64 design grid onto the target box.
struct Tf {
  LovyanGFX* g;
  float x0, y0, k;  // origin and pixels per design unit
  int X(float v) const { return (int)lroundf(x0 + v * k); }
  int Y(float v) const { return (int)lroundf(y0 + v * k); }
  int R(float v) const { int r = (int)lroundf(v * k); return r < 1 ? 1 : r; }
  float W(float v) const { float r = v * k; return r < 0.6f ? 0.6f : r; }
};

// Sub-transform: place a full 64 grid icon at offset (dx,dy) and scale s inside the parent.
Tf sub(const Tf& t, float dx, float dy, float s)
{
  return Tf{t.g, t.x0 + dx * t.k, t.y0 + dy * t.k, t.k * s};
}

void line(const Tf& t, float x0, float y0, float x1, float y1, float w, uint16_t c)
{
  t.g->drawWideLine(t.X(x0), t.Y(y0), t.X(x1), t.Y(y1), t.W(w), c);
}

// Cloud shape in the 64 grid: spans roughly x 6..58, y 14..50.
void cloudShape(const Tf& t, uint16_t c, float grow)
{
  t.g->fillCircle(t.X(21), t.Y(36), t.R(11 + grow), c);
  t.g->fillCircle(t.X(36), t.Y(29), t.R(15 + grow), c);
  t.g->fillCircle(t.X(49), t.Y(38), t.R(10 + grow), c);
  int x = t.X(8 - grow), y = t.Y(34 - grow);
  int w = t.X(57 + grow) - x, h = t.Y(49 + grow) - y;
  t.g->fillRoundRect(x, y, w, h, t.R(7.5f + grow), c);
}

void cloud(const Tf& t, uint16_t c, uint16_t bg, float cutout = 2.5f)
{
  cloudShape(t, bg, cutout);  // cut-out so the cloud stands out from what's behind
  cloudShape(t, c, 0);
}

// Animation clock. ms < 0 = static icon (the classic pose).
struct Anim {
  int32_t ms;
  bool on() const { return ms >= 0; }
  // Position 0..1 within a cycle of `period` ms, shifted by `offset` cycles. Integer
  // modulo, so it stays smooth however long the clock has been running.
  float ph(uint32_t period, float offset = 0) const
  {
    if (!on()) return 0;
    float p = ((uint32_t)ms % period) / (float)period + offset;
    return p - floorf(p);
  }
  // Sine swing -1..1 (0 when static)
  float wave(uint32_t period, float offset = 0) const
  {
    return on() ? sinf(ph(period, offset) * 2.0f * (float)M_PI) : 0;
  }
};

// Moves a whole shape sideways by whole pixels (parts rounded one by one would wobble).
Tf drift(const Tf& t, const Anim& a, float amp, uint32_t period, float offset = 0)
{
  Tf r = t;
  r.x0 += lroundf(a.wave(period, offset) * amp * t.k);
  return r;
}

// Rays turn slowly (one ray spacing per 6 s, then the picture repeats)
void sun(const Tf& t, const Anim& a, float cx, float cy, float r)
{
  const float rayIn = r * 1.4f, rayOut = r * 1.95f, w = r * 0.17f;
  const float a0 = a.ph(6000) * (float)M_PI / 4.0f;
  for (int i = 0; i < 8; ++i) {
    float an = a0 + i * (float)M_PI / 4.0f;
    line(t, cx + cosf(an) * rayIn, cy + sinf(an) * rayIn, cx + cosf(an) * rayOut, cy + sinf(an) * rayOut, w, C_SUN);
  }
  t.g->fillCircle(t.X(cx), t.Y(cy), t.R(r), C_SUN);
}

// The crescent rocks a little
void moon(const Tf& t, const Anim& a, float cx, float cy, float r, uint16_t bg)
{
  const float d = a.wave(8000) * 0.16f;  // +-9 degrees
  const float ox = r * 0.45f, oy = -r * 0.35f;
  t.g->fillCircle(t.X(cx), t.Y(cy), t.R(r), C_MOON);
  t.g->fillCircle(t.X(cx + ox * cosf(d) - oy * sinf(d)), t.Y(cy + ox * sinf(d) + oy * cosf(d)), t.R(r * 0.8f), bg);
}

// Precipitation falls from behind the cloud (draw it before the cloud) to FALL_END.
constexpr float FALL_START = 40, FALL_END = 63;

// One slanted rain drop. Static: from (x, y) down by len. Animated: x is where it
// passes y = 48; it shortens as it reaches the ground.
void drop(const Tf& t, const Anim& a, float x, float y, float len, float slope, uint32_t period, float offset)
{
  float y0 = y, y1 = y + len, xr = x + y * slope;  // x at height yy = xr - yy * slope
  if (a.on()) {
    xr = x + 48 * slope;
    y0 = FALL_START + a.ph(period, offset) * (FALL_END - FALL_START);
    y1 = y0 + len < FALL_END ? y0 + len : FALL_END;
    if (y1 - y0 < 1.5f) return;
  }
  line(t, xr - y0 * slope, y0, xr - y1 * slope, y1, 1.6f, C_RAIN);
}

void drops(const Tf& t, const Anim& a, int count, float len, uint32_t period)
{
  // Evenly spaced slanted drops below the cloud
  const float x0 = 16, x1 = 48;
  for (int i = 0; i < count; ++i) {
    float x = (count == 1) ? 32 : x0 + (x1 - x0) * i / (count - 1);
    float y = 48 + ((i & 1) ? 4 : 0);
    drop(t, a, x + 2, y, len, 0.35f, period, i * 0.37f);
  }
}

// Snow flake; animated it sinks slowly, swaying, and melts away at the bottom
void flake(const Tf& t, const Anim& a, float cx, float cy, float r, float offset)
{
  if (a.on()) {
    const float p = a.ph(3200, offset);
    cy = FALL_START + 2 + p * (FALL_END - 1 - FALL_START - 2);
    cx += a.wave(2100, offset) * 1.5f;
    if (p > 0.8f) r *= (1 - p) / 0.2f;
    if (r < 1.2f) return;
  }
  for (int i = 0; i < 3; ++i) {
    float an = i * (float)M_PI / 3.0f + (float)M_PI / 2.0f;
    line(t, cx - cosf(an) * r, cy - sinf(an) * r, cx + cosf(an) * r, cy + sinf(an) * r, 1.1f, C_SNOW);
  }
}

void hailstone(const Tf& t, const Anim& a, float x, float y, float offset)
{
  if (a.on()) y = FALL_START + a.ph(800, offset) * (FALL_END - 2 - FALL_START);
  t.g->fillCircle(t.X(x), t.Y(y), t.R(2.6f), C_SNOW);
}

// Lightning bolt as triangles; animated it flickers and goes dark between strikes
void bolt(const Tf& t, const Anim& a, float dx, float dy, float s)
{
  if (a.on()) {
    const float p = a.ph(3000);
    if ((p >= 0.07f && p < 0.13f) || p >= 0.7f) return;
  }
  auto P = [&](float x, float y, int& ox, int& oy) { ox = t.X(dx + x * s); oy = t.Y(dy + y * s); };
  int ax, ay, bx, by, cx, cy;
  P(14, 0, ax, ay); P(2, 18, bx, by); P(12, 18, cx, cy);
  t.g->fillTriangle(ax, ay, bx, by, cx, cy, C_BOLT);
  P(7, 13, ax, ay); P(18, 13, bx, by); P(4, 32, cx, cy);
  t.g->fillTriangle(ax, ay, bx, by, cx, cy, C_BOLT);
  P(14, 0, ax, ay); P(12, 18, bx, by); P(22, 0, cx, cy);
  t.g->fillTriangle(ax, ay, bx, by, cx, cy, C_BOLT);
}

// Three gusts with curled ends, each sliding back and forth at its own pace
void windLines(const Tf& t, const Anim& a, uint16_t c)
{
  const Tf g1 = drift(t, a, 3, 2600), g2 = drift(t, a, 3, 3400, 0.3f), g3 = drift(t, a, 3, 2900, 0.6f);
  line(g1, 6, 24, 40, 24, 1.8f, c);
  g1.g->fillArc(g1.X(40), g1.Y(17), g1.R(5.2f), g1.R(8.8f), 180, 450, c);
  line(g2, 6, 34, 50, 34, 1.8f, c);
  line(g3, 6, 44, 36, 44, 1.8f, c);
  g3.g->fillArc(g3.X(36), g3.Y(51), g3.R(5.2f), g3.R(8.8f), -90, 180, c);
}

}  // namespace

void drawWeatherIcon(LovyanGFX* g, int x, int y, int size, const char* cond, bool night, uint16_t bg, int32_t animMs)
{
  Tf t{g, (float)x, (float)y, size / 64.0f};
  const Anim a{animMs};
  if (!cond) cond = "";
  auto is = [&](const char* s) { return strcmp(cond, s) == 0; };

  // Cloud lifted up to leave room for precipitation below. Precipitation is drawn
  // first: the cloud's cut-out hides where it starts.
  Tf upper = sub(t, 0, -8, 1.0f);

  if (is("sunny")) {
    if (night) moon(t, a, 30, 32, 19, bg);
    else sun(t, a, 32, 32, 13);
  } else if (is("clear-night")) {
    moon(t, a, 30, 32, 19, bg);
  } else if (is("partlycloudy")) {
    if (night) moon(t, a, 24, 22, 13, bg);
    else sun(t, a, 20, 18, 9.5f);
    cloud(drift(sub(t, 6, 10, 0.88f), a, 2.5f, 7000), C_CLOUD, bg, 3.5f);
  } else if (is("cloudy")) {
    cloudShape(drift(sub(t, 14, -6, 0.75f), a, 3, 9000), C_CLOUD_BACK, 0);
    cloud(drift(sub(t, 0, 6, 0.95f), a, -2, 7000), C_CLOUD, bg);
  } else if (is("fog")) {
    cloud(upper, C_CLOUD_BACK, bg);
    line(drift(t, a, 3, 5000), 10, 48, 54, 48, 1.7f, C_FOG);
    line(drift(t, a, 3, 5000, 0.33f), 6, 55, 46, 55, 1.7f, C_FOG);
    line(drift(t, a, 3, 5000, 0.66f), 16, 62, 58, 62, 1.7f, C_FOG);
  } else if (is("rainy")) {
    drops(t, a, 3, 9, 900);
    cloud(upper, C_CLOUD, bg);
  } else if (is("pouring")) {
    drops(t, a, 5, 12, 600);
    cloud(upper, C_CLOUD_DARK, bg);
  } else if (is("snowy")) {
    flake(t, a, 18, 51, 4.5f, 0);
    flake(t, a, 32, 57, 4.5f, 0.4f);
    flake(t, a, 46, 51, 4.5f, 0.75f);
    cloud(upper, C_CLOUD, bg);
  } else if (is("snowy-rainy")) {
    drop(t, a, 20, 48, 10, 0.4f, 900, 0);
    flake(t, a, 32, 55, 4.5f, 0.3f);
    drop(t, a, 48, 48, 10, 0.4f, 900, 0.5f);
    cloud(upper, C_CLOUD, bg);
  } else if (is("hail")) {
    const float pts[][2] = {{18, 50}, {32, 54}, {46, 50}, {25, 60}, {39, 60}};
    int i = 0;
    for (auto& p : pts) hailstone(t, a, p[0], p[1], i++ * 0.37f);
    cloud(upper, C_CLOUD, bg);
  } else if (is("lightning")) {
    cloud(upper, C_CLOUD_DARK, bg);
    bolt(t, a, 22, 32, 0.95f);
  } else if (is("lightning-rainy")) {
    drop(t, a, 16, 48, 10, 0.4f, 700, 0);
    drop(t, a, 50, 48, 10, 0.4f, 700, 0.5f);
    cloud(upper, C_CLOUD_DARK, bg);
    bolt(t, a, 22, 32, 0.95f);
  } else if (is("windy")) {
    windLines(sub(t, 0, 4, 1.0f), a, C_WIND);
  } else if (is("windy-variant")) {
    cloud(drift(sub(t, 14, -6, 0.75f), a, 2, 7000), C_CLOUD, bg);
    windLines(sub(t, 0, 16, 0.85f), a, C_WIND);
  } else if (is("exceptional")) {
    g->fillTriangle(t.X(32), t.Y(6), t.X(4), t.Y(58), t.X(60), t.Y(58), C_WARN);
    g->fillRoundRect(t.X(29.5f), t.Y(22), t.R(5), t.R(22), t.R(2), bg);
    g->fillCircle(t.X(32), t.Y(50), t.R(3), bg);
  } else {
    // Unknown condition: plain cloud
    cloud(drift(sub(t, 0, 4, 1.0f), a, 2, 7000), C_CLOUD_BACK, bg);
  }
}

void drawSpeakerIcon(LovyanGFX* g, int x, int y, int size, uint16_t color)
{
  Tf t{g, (float)x, (float)y, size / 64.0f};
  g->fillRect(t.X(4), t.Y(22), t.X(18) - t.X(4), t.Y(42) - t.Y(22), color);
  g->fillTriangle(t.X(14), t.Y(22), t.X(34), t.Y(4), t.X(34), t.Y(32), color);
  g->fillTriangle(t.X(14), t.Y(42), t.X(34), t.Y(60), t.X(34), t.Y(32), color);
  g->fillRect(t.X(14), t.Y(22), t.X(34) - t.X(14), t.Y(42) - t.Y(22), color);
  g->fillArc(t.X(34), t.Y(32), t.R(14), t.R(19), -45, 45, color);
  g->fillArc(t.X(34), t.Y(32), t.R(25), t.R(30), -50, 50, color);
}

void drawDropIcon(LovyanGFX* g, int x, int y, int size, uint16_t color)
{
  Tf t{g, (float)x, (float)y, size / 64.0f};
  g->fillCircle(t.X(32), t.Y(42), t.R(17), color);
  g->fillTriangle(t.X(32), t.Y(4), t.X(16.5f), t.Y(36), t.X(47.5f), t.Y(36), color);
}
