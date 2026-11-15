#pragma once
#include <Arduino.h>
#include "weather.h"

struct HaConfig {
  String url;     // e.g. http://homeassistant.local:8123
  String token;   // long-lived access token
  String entity;  // e.g. weather.forecast_home
};

// Fetches current conditions, forecast and sun state from Home Assistant.
// On success `w` is updated and true is returned. On failure `w.error` is set
// and the previous data in `w` is kept.
bool haUpdateWeather(const HaConfig& cfg, bool hourly, WeatherData& w);
