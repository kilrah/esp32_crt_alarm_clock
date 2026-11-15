#include "ha_client.h"
#include "config.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <memory>

namespace {

const uint32_t HTTP_TIMEOUT_MS = 10000;

// Opens a request to `path` on the HA instance. Returns the HTTP status code (or
// a negative HTTPClient error).
class HaRequest {
 public:
  explicit HaRequest(const HaConfig& cfg) : _cfg(cfg)
  {
    if (cfg.url.startsWith("https")) {
      auto* c = new WiFiClientSecure();
      c->setInsecure();  // no certificate validation (LAN use)
      _client.reset(c);
    } else {
      _client.reset(new WiFiClient());
    }
  }

  int get(const String& path) { return send(path, nullptr); }
  int post(const String& path, const String& body) { return send(path, &body); }
  Stream& stream() { return _http.getStream(); }
  ~HaRequest() { _http.end(); }

 private:
  int send(const String& path, const String* body)
  {
    String url = _cfg.url;
    while (url.endsWith("/")) url.remove(url.length() - 1);
    url += path;
    _http.useHTTP10(true);  // no chunked encoding, so the JSON can be streamed
    _http.setTimeout(HTTP_TIMEOUT_MS);
    _http.setConnectTimeout(HTTP_TIMEOUT_MS);
    if (!_http.begin(*_client, url)) return -100;
    _http.addHeader("Authorization", "Bearer " + _cfg.token);
    _http.addHeader("Content-Type", "application/json");
    return body ? _http.POST(*body) : _http.GET();
  }

  const HaConfig& _cfg;
  std::unique_ptr<WiFiClient> _client;
  HTTPClient _http;
};

void setHttpError(WeatherData& w, const char* what, int code)
{
  if (code == 401) snprintf(w.error, sizeof(w.error), "HA: unauthorized, check token");
  else if (code == 404) snprintf(w.error, sizeof(w.error), "HA: %s not found", what);
  else if (code < 0) snprintf(w.error, sizeof(w.error), "HA %s: %s", what, HTTPClient::errorToString(code).c_str());
  else snprintf(w.error, sizeof(w.error), "HA %s: HTTP %d", what, code);
  Serial.printf("[HA] %s failed: %d\n", what, code);
}

float jsonFloat(JsonVariantConst v)
{
  if (v.is<float>()) return v.as<float>();
  if (v.is<const char*>()) {  // some integrations report numbers as strings
    const char* s = v.as<const char*>();
    char* end;
    float f = strtof(s, &end);
    if (end != s) return f;
  }
  return NAN;
}

}  // namespace

bool haUpdateWeather(const HaConfig& cfg, bool hourly, WeatherData& w)
{
  if (cfg.token.isEmpty()) {
    snprintf(w.error, sizeof(w.error), "No Home Assistant token configured");
    return false;
  }

  // ---- Current state ------------------------------------------------------
  WeatherData nw = w;  // work on a copy, commit on success
  {
    HaRequest req(cfg);
    int code = req.get("/api/states/" + cfg.entity);
    if (code != 200) { setHttpError(w, "state", code); return false; }

    JsonDocument filter;
    filter["state"] = true;
    filter["attributes"]["temperature"] = true;
    filter["attributes"]["humidity"] = true;
    filter["attributes"]["temperature_unit"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, req.stream(), DeserializationOption::Filter(filter));
    if (err) {
      snprintf(w.error, sizeof(w.error), "HA state JSON: %s", err.c_str());
      return false;
    }
    const char* state = doc["state"] | "";
    if (!strcmp(state, "unavailable") || !strcmp(state, "unknown")) {
      snprintf(w.error, sizeof(w.error), "Weather entity is %s", state);
      return false;
    }
    strlcpy(nw.condition, state, sizeof(nw.condition));
    nw.temperature = jsonFloat(doc["attributes"]["temperature"]);
    nw.humidity = jsonFloat(doc["attributes"]["humidity"]);
    const char* unit = doc["attributes"]["temperature_unit"] | "\xC2\xB0" "C";
    strlcpy(nw.unit, unit, sizeof(nw.unit));
  }

  // ---- Forecast (weather.get_forecasts service, HA 2024.3+) -----------------
  {
    HaRequest req(cfg);
    String body = String("{\"entity_id\":\"") + cfg.entity + "\",\"type\":\"" + (hourly ? "hourly" : "daily") + "\"}";
    int code = req.post("/api/services/weather/get_forecasts?return_response", body);
    if (code != 200) { setHttpError(w, "forecast", code); return false; }

    JsonDocument filter;
    JsonObject item = filter["service_response"][cfg.entity]["forecast"].add<JsonObject>();
    item["datetime"] = true;
    item["condition"] = true;
    item["temperature"] = true;
    item["templow"] = true;
    item["is_daytime"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, req.stream(), DeserializationOption::Filter(filter));
    if (err) {
      snprintf(w.error, sizeof(w.error), "HA forecast JSON: %s", err.c_str());
      return false;
    }
    JsonArrayConst arr = doc["service_response"][cfg.entity]["forecast"];
    nw.forecastCount = 0;
    nw.hourly = hourly;
    for (JsonObjectConst f : arr) {
      if (nw.forecastCount >= MAX_FORECAST) break;
      ForecastEntry& e = nw.forecast[nw.forecastCount];
      e = ForecastEntry();
      e.time = parseIsoTime(f["datetime"] | "");
      if (e.time == 0) continue;
      strlcpy(e.condition, f["condition"] | "", sizeof(e.condition));
      e.tempHigh = jsonFloat(f["temperature"]);
      e.tempLow = jsonFloat(f["templow"]);
      if (!f["is_daytime"].isNull()) e.isDaytime = f["is_daytime"].as<bool>() ? 1 : 0;
      nw.forecastCount++;
    }
  }

  // ---- Sun (optional, for day/night icons) ----------------------------------
  nw.sunUp = -1;
  if (strlen(SUN_ENTITY) > 0) {
    HaRequest req(cfg);
    int code = req.get("/api/states/" SUN_ENTITY);
    if (code == 200) {
      JsonDocument filter;
      filter["state"] = true;
      JsonDocument doc;
      if (!deserializeJson(doc, req.stream(), DeserializationOption::Filter(filter))) {
        const char* s = doc["state"] | "";
        if (!strcmp(s, "above_horizon")) nw.sunUp = 1;
        else if (!strcmp(s, "below_horizon")) nw.sunUp = 0;
      }
    }
  }

  nw.valid = true;
  nw.lastUpdate = time(nullptr);
  nw.error[0] = 0;
  w = nw;
  Serial.printf("[HA] %s %.1f%s, %d forecast entries\n", w.condition, w.temperature, w.unit, w.forecastCount);
  return true;
}
