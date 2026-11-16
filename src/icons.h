#pragma once
// Procedurally drawn weather icons (scale to any size, no bitmaps needed).
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// Draws the icon for a Home Assistant weather condition into the square box
// (x, y, size). `bg` is the background color (used to cut outlines between
// overlapping shapes). Colors are RGB565.
// animMs >= 0 animates the icon (turning sun, drifting clouds, falling rain...):
// pass a running millisecond clock and redraw several times a second. -1 = static.
void drawWeatherIcon(lgfx::LovyanGFX* g, int x, int y, int size, const char* condition, bool night, uint16_t bg,
                     int32_t animMs = -1);

// Small speaker symbol (radio playing), box (x, y, size).
void drawSpeakerIcon(lgfx::LovyanGFX* g, int x, int y, int size, uint16_t color);

// Small water drop symbol (humidity), box (x, y, size).
void drawDropIcon(lgfx::LovyanGFX* g, int x, int y, int size, uint16_t color);
