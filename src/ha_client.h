#pragma once
#include <Arduino.h>
#include "weather.h"

struct HaConfig {
  String url;     // e.g. http://homeassistant.local:8123
  String token;   // long-lived access token
  String entity;  // e.g. weather.forecast_home
  // Optional: entity whose state is the current temperature (e.g. sensor.balcony_temperature),
  // shown instead of the weather entity's. Empty = not used. If it can't be read, the
  // weather entity's temperature is shown.
  String tempEntity;
  String humEntity;  // same for the humidity
};

// Fetches current conditions, forecast and sun state from Home Assistant.
// On success `w` is updated and true is returned. On failure `w.error` is set
// and the previous data in `w` is kept.
bool haUpdateWeather(const HaConfig& cfg, bool hourly, WeatherData& w);
