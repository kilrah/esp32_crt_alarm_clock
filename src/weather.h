#pragma once
// Platform independent weather data model (no Arduino dependencies so the UI
// can also be rendered on a PC, see tools/preview).
#include <cstdint>
#include <ctime>
#include <cmath>

static constexpr int MAX_FORECAST = 12;

struct ForecastEntry {
  time_t time = 0;          // UTC epoch
  char condition[20] = "";  // HA condition string, e.g. "partlycloudy"
  float tempHigh = NAN;     // "temperature" attribute
  float tempLow = NAN;      // "templow" attribute, NAN if not provided (hourly)
  int8_t isDaytime = -1;    // -1 = unknown, 0 = night, 1 = day (hourly forecasts)
};

struct WeatherData {
  bool valid = false;           // At least one successful update happened
  char condition[20] = "";
  float temperature = NAN;
  float humidity = NAN;
  char unit[6] = "\xC2\xB0" "C"; // UTF-8 "°C"
  int8_t sunUp = -1;            // -1 = unknown
  ForecastEntry forecast[MAX_FORECAST];
  int forecastCount = 0;
  bool hourly = false;
  time_t lastUpdate = 0;        // UTC epoch of last successful update
  char error[64] = "";          // Last error, empty if last update succeeded
};

// Parses an ISO 8601 timestamp ("2026-11-15T11:00:00+00:00", "...Z") to UTC epoch.
// Returns 0 on failure.
time_t parseIsoTime(const char* s);
