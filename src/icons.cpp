#include "icons.h"
#include <cstring>
#include <cmath>

using lgfx::LovyanGFX;

namespace {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

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

void sun(const Tf& t, float cx, float cy, float r)
{
  const float rayIn = r * 1.4f, rayOut = r * 1.95f, w = r * 0.17f;
  for (int i = 0; i < 8; ++i) {
    float a = i * (float)M_PI / 4.0f;
    line(t, cx + cosf(a) * rayIn, cy + sinf(a) * rayIn, cx + cosf(a) * rayOut, cy + sinf(a) * rayOut, w, C_SUN);
  }
  t.g->fillCircle(t.X(cx), t.Y(cy), t.R(r), C_SUN);
}

void moon(const Tf& t, float cx, float cy, float r, uint16_t bg)
{
  t.g->fillCircle(t.X(cx), t.Y(cy), t.R(r), C_MOON);
  t.g->fillCircle(t.X(cx + r * 0.45f), t.Y(cy - r * 0.35f), t.R(r * 0.8f), bg);
}

void drops(const Tf& t, int count, float len)
{
  // Evenly spaced slanted drops below the cloud
  const float x0 = 16, x1 = 48;
  for (int i = 0; i < count; ++i) {
    float x = (count == 1) ? 32 : x0 + (x1 - x0) * i / (count - 1);
    float y = 48 + ((i & 1) ? 4 : 0);
    line(t, x + 2, y, x + 2 - len * 0.35f, y + len, 1.6f, C_RAIN);
  }
}

void flake(const Tf& t, float cx, float cy, float r)
{
  for (int i = 0; i < 3; ++i) {
    float a = i * (float)M_PI / 3.0f + (float)M_PI / 2.0f;
    line(t, cx - cosf(a) * r, cy - sinf(a) * r, cx + cosf(a) * r, cy + sinf(a) * r, 1.1f, C_SNOW);
  }
}

void bolt(const Tf& t, float dx, float dy, float s)
{
  // Lightning bolt as two triangles
  auto P = [&](float x, float y, int& ox, int& oy) { ox = t.X(dx + x * s); oy = t.Y(dy + y * s); };
  int ax, ay, bx, by, cx, cy;
  P(14, 0, ax, ay); P(2, 18, bx, by); P(12, 18, cx, cy);
  t.g->fillTriangle(ax, ay, bx, by, cx, cy, C_BOLT);
  P(7, 13, ax, ay); P(18, 13, bx, by); P(4, 32, cx, cy);
  t.g->fillTriangle(ax, ay, bx, by, cx, cy, C_BOLT);
  P(14, 0, ax, ay); P(12, 18, bx, by); P(22, 0, cx, cy);
  t.g->fillTriangle(ax, ay, bx, by, cx, cy, C_BOLT);
}

void windLines(const Tf& t, uint16_t c)
{
  // Three gusts with curled ends
  line(t, 6, 24, 40, 24, 1.8f, c);
  t.g->fillArc(t.X(40), t.Y(17), t.R(5.2f), t.R(8.8f), 180, 450, c);
  line(t, 6, 34, 50, 34, 1.8f, c);
  line(t, 6, 44, 36, 44, 1.8f, c);
  t.g->fillArc(t.X(36), t.Y(51), t.R(5.2f), t.R(8.8f), -90, 180, c);
}

}  // namespace

void drawWeatherIcon(LovyanGFX* g, int x, int y, int size, const char* cond, bool night, uint16_t bg)
{
  Tf t{g, (float)x, (float)y, size / 64.0f};
  if (!cond) cond = "";
  auto is = [&](const char* s) { return strcmp(cond, s) == 0; };

  // Cloud lifted up to leave room for precipitation below
  Tf upper = sub(t, 0, -8, 1.0f);

  if (is("sunny")) {
    if (night) moon(t, 30, 32, 19, bg);
    else sun(t, 32, 32, 13);
  } else if (is("clear-night")) {
    moon(t, 30, 32, 19, bg);
  } else if (is("partlycloudy")) {
    if (night) moon(t, 24, 22, 13, bg);
    else sun(t, 20, 18, 9.5f);
    cloud(sub(t, 6, 10, 0.88f), C_CLOUD, bg, 3.5f);
  } else if (is("cloudy")) {
    cloudShape(sub(t, 14, -6, 0.75f), C_CLOUD_BACK, 0);
    cloud(sub(t, 0, 6, 0.95f), C_CLOUD, bg);
  } else if (is("fog")) {
    cloud(upper, C_CLOUD_BACK, bg);
    line(t, 10, 48, 54, 48, 1.7f, C_FOG);
    line(t, 6, 55, 46, 55, 1.7f, C_FOG);
    line(t, 16, 62, 58, 62, 1.7f, C_FOG);
  } else if (is("rainy")) {
    cloud(upper, C_CLOUD, bg);
    drops(t, 3, 9);
  } else if (is("pouring")) {
    cloud(upper, C_CLOUD_DARK, bg);
    drops(t, 5, 12);
  } else if (is("snowy")) {
    cloud(upper, C_CLOUD, bg);
    flake(t, 18, 51, 4.5f);
    flake(t, 32, 57, 4.5f);
    flake(t, 46, 51, 4.5f);
  } else if (is("snowy-rainy")) {
    cloud(upper, C_CLOUD, bg);
    line(t, 20, 48, 16, 58, 1.6f, C_RAIN);
    flake(t, 32, 55, 4.5f);
    line(t, 48, 48, 44, 58, 1.6f, C_RAIN);
  } else if (is("hail")) {
    cloud(upper, C_CLOUD, bg);
    const float pts[][2] = {{18, 50}, {32, 54}, {46, 50}, {25, 60}, {39, 60}};
    for (auto& p : pts) g->fillCircle(t.X(p[0]), t.Y(p[1]), t.R(2.6f), C_SNOW);
  } else if (is("lightning")) {
    cloud(upper, C_CLOUD_DARK, bg);
    bolt(t, 22, 32, 0.95f);
  } else if (is("lightning-rainy")) {
    cloud(upper, C_CLOUD_DARK, bg);
    line(t, 16, 48, 12, 58, 1.6f, C_RAIN);
    line(t, 50, 48, 46, 58, 1.6f, C_RAIN);
    bolt(t, 22, 32, 0.95f);
  } else if (is("windy")) {
    windLines(sub(t, 0, 4, 1.0f), C_WIND);
  } else if (is("windy-variant")) {
    cloud(sub(t, 14, -6, 0.75f), C_CLOUD, bg);
    windLines(sub(t, 0, 16, 0.85f), C_WIND);
  } else if (is("exceptional")) {
    g->fillTriangle(t.X(32), t.Y(6), t.X(4), t.Y(58), t.X(60), t.Y(58), C_WARN);
    g->fillRoundRect(t.X(29.5f), t.Y(22), t.R(5), t.R(22), t.R(2), bg);
    g->fillCircle(t.X(32), t.Y(50), t.R(3), bg);
  } else {
    // Unknown condition: plain cloud
    cloud(sub(t, 0, 4, 1.0f), C_CLOUD_BACK, bg);
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
