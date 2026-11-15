#pragma once

#if !defined(WIFI_SSID) || !defined(WIFI_PASSWORD) || !defined(OTA_PASSWORD) || !defined(HA_TOKEN)
#error "Credentials missing: copy secrets.example.ini to secrets.ini and fill it in"
#endif
// ---------------------------------------------------------------------------
// Compile-time defaults.
//
// WiFi, Home Assistant URL/token/entity and timezone are runtime settings on the
// web page (http://weatherclock.local/); values saved there take precedence over
// the defaults below and survive reflashing.
// WIFI_SSID, WIFI_PASSWORD, OTA_PASSWORD and HA_TOKEN come from secrets.ini
// (git-ignored) via build flags in platformio.ini.
// Every option can also be overridden with a build flag, e.g.
//   build_flags = -D CLOCK_24H=0
// ---------------------------------------------------------------------------

// --- Home Assistant -------------------------------------------------------
// Base URL without trailing slash. Plain http on the LAN is recommended; https
// works too but certificates are not validated and it costs ~40 KB of RAM.
#ifndef DEFAULT_HA_URL
#define DEFAULT_HA_URL        "http://homeassistant.local:8123"
#endif
// Weather entity, e.g. weather.forecast_home
#ifndef DEFAULT_WEATHER_ENTITY
#define DEFAULT_WEATHER_ENTITY "weather.forecast_home"
#endif
// Sun entity used to pick day/night icons. Leave empty to disable.
#ifndef SUN_ENTITY
#define SUN_ENTITY            "sun.sun"
#endif

// --- Time -------------------------------------------------------------------
// POSIX TZ string, see https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
#ifndef DEFAULT_TIMEZONE
#define DEFAULT_TIMEZONE      "CET-1CEST,M3.5.0,M10.5.0/3"
#endif
#ifndef NTP_SERVER_1
#define NTP_SERVER_1          "pool.ntp.org"
#endif
#ifndef NTP_SERVER_2
#define NTP_SERVER_2          "time.google.com"
#endif
#ifndef CLOCK_24H
#define CLOCK_24H             1     // 0 = 12h clock with AM/PM
#endif

// --- Display content ---------------------------------------------------------
#ifndef FORECAST_HOURLY
#define FORECAST_HOURLY       0     // 0 = daily forecast, 1 = hourly forecast
#endif
#ifndef FORECAST_ROWS
#define FORECAST_ROWS         5     // Number of forecast rows (max 8, fewer if they don't fit)
#endif
#ifndef SHOW_HUMIDITY
#define SHOW_HUMIDITY         1     // Show humidity next to the current temperature
#endif
#ifndef SHOW_DECIMAL
#define SHOW_DECIMAL          0     // Show current temperature with one decimal
#endif
#ifndef WEATHER_REFRESH_S
#define WEATHER_REFRESH_S     300   // How often to poll Home Assistant
#endif
#ifndef STALE_AFTER_S
#define STALE_AFTER_S         1800  // Mark data as outdated after this long without update
#endif

// --- Composite video output -------------------------------------------------
#ifndef CVBS_PIN
#define CVBS_PIN              25    // DAC pin, 25 or 26
#endif
#ifndef CVBS_PAL
#define CVBS_PAL              1     // 1 = PAL, 0 = NTSC
#endif
// Frame buffer size. Rendering scales to whatever area is visible.
#ifndef CVBS_MEMORY_WIDTH
#define CVBS_MEMORY_WIDTH     340
#endif
#ifndef CVBS_MEMORY_HEIGHT
#define CVBS_MEMORY_HEIGHT    260
#endif
// Visible area inside the frame buffer and its offset (shrink/move to fight overscan)
#ifndef CVBS_PANEL_WIDTH
#define CVBS_PANEL_WIDTH      330
#endif
#ifndef CVBS_PANEL_HEIGHT
#define CVBS_PANEL_HEIGHT     250
#endif
#ifndef CVBS_OFFSET_X
#define CVBS_OFFSET_X         5
#endif
#ifndef CVBS_OFFSET_Y
#define CVBS_OFFSET_Y         5
#endif
// Additional safe margin in pixels around the drawn content
#ifndef SCREEN_MARGIN
#define SCREEN_MARGIN         8
#endif
#ifndef CVBS_OUTPUT_LEVEL
#define CVBS_OUTPUT_LEVEL     128   // Raise if the picture is dark (e.g. series resistor)
#endif
#ifndef CVBS_CHROMA_LEVEL
#define CVBS_CHROMA_LEVEL     128   // 0 = black & white, higher = more saturated
#endif
// 8 = RGB332 (88 KB frame buffer, known working), 16 = RGB565 (176 KB, nicer
// gradients but very tight on RAM next to WiFi; not recommended without PSRAM)
#ifndef CVBS_COLOR_DEPTH
#define CVBS_COLOR_DEPTH      8
#endif
// Vertical dithering of the temperature bars in 8-bit mode (smooths gradients;
// the PAL decoder averages adjacent lines)
#ifndef GRADIENT_DITHER
#define GRADIENT_DITHER       1
#endif

// --- Web radio (PCM5102 or other I2S DAC on the ESP32's second I2S port) ------
// I2S0 is used by the composite video, the DAC runs on I2S1.
#ifndef I2S_BCK_PIN
#define I2S_BCK_PIN           4    // PCM5102 BCK
#endif
#ifndef I2S_LRCK_PIN
#define I2S_LRCK_PIN          15   // PCM5102 LCK / LRCK / WS
#endif
#ifndef I2S_DOUT_PIN
#define I2S_DOUT_PIN          2    // PCM5102 DIN
#endif
// GPIO wired to the PCM5102's SCK (system clock) input, driven LOW so the DAC uses its
// internal PLL (clock derived from BCK). A floating SCK makes the DAC drop out
// intermittently. -1 = not connected (SCK grounded or the module's SCK bridge closed).
// Don't use GPIO16/17 on boards with PSRAM (WROVER): they belong to the PSRAM.
#ifndef I2S_SCK_LOW_PIN
#define I2S_SCK_LOW_PIN       16
#endif
// Network buffer in front of the MP3 decoder (bridges WiFi hiccups; ~0.5 s at 128 kbit/s).
// Reserved permanently at boot together with the ~30 KB decoder state.
#ifndef RADIO_BUFFER_BYTES
#define RADIO_BUFFER_BYTES    8192
#endif
#ifndef RADIO_MAX_STATIONS
#define RADIO_MAX_STATIONS    8
#endif
#ifndef RADIO_DEFAULT_VOLUME
#define RADIO_DEFAULT_VOLUME  30    // 0-100
#endif
// Default station (editable on the web page). Must be an http:// MP3 stream.
#ifndef RADIO_DEFAULT_NAME
#define RADIO_DEFAULT_NAME    "FIP"
#endif
#ifndef RADIO_DEFAULT_URL
#define RADIO_DEFAULT_URL     "http://icecast.radiofrance.fr/fip-midfi.mp3"
#endif

// --- Network / web page -------------------------------------------------------
// Settings page at http://<DEVICE_HOSTNAME>.local/ ; also the name of the fallback
// access point (password = OTA_PASSWORD if it has 8+ characters, else open)
#ifndef DEVICE_HOSTNAME
#define DEVICE_HOSTNAME       "weatherclock"
#endif
#ifndef DEVICE_TITLE
#define DEVICE_TITLE          "Weather Clock"
#endif
// Open the own access point if the WiFi can't be joined within this time
#ifndef WIFI_JOIN_TIMEOUT_MS
#define WIFI_JOIN_TIMEOUT_MS  20000
#endif
// While on the own access point with nobody connected, retry the WiFi this often
#ifndef WIFI_RETRY_MS
#define WIFI_RETRY_MS         120000
#endif
// Login for the web "Firmware update" page (password = OTA_PASSWORD)
#ifndef OTA_USER
#define OTA_USER              "admin"
#endif

// --- Debug ---------------------------------------------------------------------
// Print free RAM and task stack headroom on the serial port every N seconds (0 = off)
#ifndef DEBUG_HEAP_LOG_S
#define DEBUG_HEAP_LOG_S      0
#endif
