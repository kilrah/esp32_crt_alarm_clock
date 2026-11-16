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
#ifndef CLOCK_SECONDS
#define CLOCK_SECONDS         1     // Seconds after the time, in the temperature's font size
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
#ifndef ICON_ANIMATION
#define ICON_ANIMATION        1     // Animate the large weather icon (turning sun, falling rain...)
#endif
#ifndef ICON_ANIM_FPS
#define ICON_ANIM_FPS         12    // Frames per second of that animation
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
// 1 = grayscale for black & white TVs: 256 gray levels and no color information in
// the picture (no dot patterns from the color carrier). Same 82 KB frame buffer as
// 8-bit color; the library has no 4-bit mode for composite output.
#ifndef CVBS_GRAYSCALE
#define CVBS_GRAYSCALE        0
#endif
#if CVBS_GRAYSCALE && CVBS_COLOR_DEPTH != 8
#error "CVBS_GRAYSCALE needs CVBS_COLOR_DEPTH 8"
#endif
// Vertical dithering of the temperature bars in 8-bit mode (smooths gradients;
// the PAL decoder averages adjacent lines)
#ifndef GRADIENT_DITHER
#define GRADIENT_DITHER       1
#endif

// --- Web radio audio output -----------------------------------------------------
// Where the decoded audio goes (mono, left + right mixed for the internal options):
#define AUDIO_OUT_I2S         0    // PCM5102 or other I2S DAC on I2S1 (best quality)
#define AUDIO_OUT_DAC         1    // ESP32's other internal DAC pin (GPIO26 when the video is on
                                   // GPIO25). 8-bit, no extra parts except an RC filter
#define AUDIO_OUT_PWM         2    // LEDC PWM on AUDIO_PWM_PIN, needs an RC low-pass filter
#ifndef AUDIO_OUTPUT
#define AUDIO_OUTPUT          AUDIO_OUT_I2S
#endif
#if AUDIO_OUTPUT != AUDIO_OUT_I2S && AUDIO_OUTPUT != AUDIO_OUT_DAC && AUDIO_OUTPUT != AUDIO_OUT_PWM
#error "AUDIO_OUTPUT must be 0 (I2S), 1 (DAC) or 2 (PWM)"
#endif
// AUDIO_OUT_DAC: the video's I2S0 stream feeds both internal DACs. The video's channel stays
// as it is and the other channel is switched to I2S0's "constant value" register, which
// a timer interrupt updates at the stream's sample rate. 3 = left channel constant (video on
// GPIO25), 4 = right channel constant (video on GPIO26). 3 with the video on GPIO25 is
// verified on hardware; try the other value if there is no sound or the picture is lost.
#ifndef AUDIO_DAC_CHAN_MOD
#define AUDIO_DAC_CHAN_MOD    (CVBS_PIN == 25 ? 3 : 4)
#endif
// AUDIO_OUT_PWM: output pin, LEDC channel (high-speed group) and resolution. The PWM
// frequency is 80 MHz / 2^bits: 8 = 312.5 kHz (easy to filter), 9 = 156 kHz, 10 = 78 kHz.
// The 4 fractional duty bits of the LEDC are used too (hardware dithering).
#ifndef AUDIO_PWM_PIN
#define AUDIO_PWM_PIN         26
#endif
#ifndef AUDIO_PWM_CHANNEL
#define AUDIO_PWM_CHANNEL     0    // 0-7
#endif
#ifndef AUDIO_PWM_BITS
#define AUDIO_PWM_BITS        8
#endif
#if AUDIO_PWM_BITS < 6 || AUDIO_PWM_BITS > 10
#error "AUDIO_PWM_BITS must be 6-10"
#endif
#if AUDIO_OUTPUT == AUDIO_OUT_PWM && AUDIO_PWM_PIN == CVBS_PIN
#error "AUDIO_PWM_PIN must not be the video pin"
#endif
// DAC/PWM: samples buffered between the decoder and the sample-rate timer interrupt
// (power of 2; 1024 = 23 ms at 44.1 kHz like the I2S DMA buffers; 1 KB of RAM with the
// DAC, 2 KB with PWM). Network hiccups are bridged by RADIO_BUFFER_BYTES, not by this.
#ifndef AUDIO_RING_SAMPLES
#define AUDIO_RING_SAMPLES    1024
#endif

// --- Web radio (PCM5102 or other I2S DAC on the ESP32's second I2S port) ------
// Only used with AUDIO_OUTPUT == AUDIO_OUT_I2S.
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

// Backup alarm: when the radio should play but there has been no stream for this many
// seconds (no WiFi/internet, station down), an MP3 file from the flash file system is
// played in a loop until the stream works or the radio is stopped. 0 = off.
// The file is not part of the project: put your own data/alarm.mp3 (max ~900 KB, no
// big ID3 tag / cover art) and upload it with `pio run -t uploadfs`.
#ifndef RADIO_BACKUP_AFTER_S
#define RADIO_BACKUP_AFTER_S  30
#endif
#ifndef RADIO_BACKUP_FILE
#define RADIO_BACKUP_FILE     "/alarm.mp3"
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
