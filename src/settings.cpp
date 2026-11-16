#include "settings.h"
#include "config.h"
#include <Preferences.h>

static const char* NVS_NAMESPACE = "wclock";

static Settings current;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;  // web task vs. loop task
static volatile uint32_t version = 0;

static Settings defaults()
{
  Settings s{};
  strlcpy(s.wifiSsid, WIFI_SSID, sizeof(s.wifiSsid));
  strlcpy(s.wifiPass, WIFI_PASSWORD, sizeof(s.wifiPass));
  strlcpy(s.haUrl, DEFAULT_HA_URL, sizeof(s.haUrl));
  strlcpy(s.haToken, HA_TOKEN, sizeof(s.haToken));
  strlcpy(s.entity, DEFAULT_WEATHER_ENTITY, sizeof(s.entity));
  strlcpy(s.timezone, DEFAULT_TIMEZONE, sizeof(s.timezone));
  return s;
}

static void apply(const Settings& s)
{
  portENTER_CRITICAL(&lock);
  current = s;
  portEXIT_CRITICAL(&lock);
  version = version + 1;
}

static void print(const char* what, const Settings& s)
{
  Serial.printf("[Settings] %s: WiFi \"%s\"%s, HA %s, entity %s, temperature %s, humidity %s, token %s, TZ %s\n",
                what, s.wifiSsid, s.wifiPass[0] ? "" : " (open)", s.haUrl, s.entity,
                s.tempEntity[0] ? s.tempEntity : "(weather entity)", s.humEntity[0] ? s.humEntity : "(weather entity)",
                s.haToken[0] ? "set" : "(none)", s.timezone);
}

// Copies a stored string into `dst` if it passes `validate`
static void loadString(Preferences& p, const char* key, char* dst, size_t size,
                       const char* (*validate)(const String&))
{
  if (!p.isKey(key)) return;
  String v = p.getString(key, "");
  if (!validate(v)) strlcpy(dst, v.c_str(), size);
}

void settingsBegin()
{
  Settings s = defaults();
  Preferences p;
  if (p.begin(NVS_NAMESPACE, true)) {
    loadString(p, "ssid", s.wifiSsid, sizeof(s.wifiSsid), validateWifiSsid);
    loadString(p, "pass", s.wifiPass, sizeof(s.wifiPass), validateWifiPass);
    loadString(p, "url", s.haUrl, sizeof(s.haUrl), validateHaUrl);
    loadString(p, "token", s.haToken, sizeof(s.haToken), validateHaToken);
    loadString(p, "entity", s.entity, sizeof(s.entity), validateEntity);
    loadString(p, "tempent", s.tempEntity, sizeof(s.tempEntity), validateSensorEntity);
    loadString(p, "hument", s.humEntity, sizeof(s.humEntity), validateSensorEntity);
    loadString(p, "tz", s.timezone, sizeof(s.timezone), validateTimezone);
    p.end();
  }
  apply(s);
  print("Loaded", s);
}

Settings settingsGet()
{
  Settings s;
  portENTER_CRITICAL(&lock);
  s = current;
  portEXIT_CRITICAL(&lock);
  return s;
}

uint32_t settingsVersion() { return version; }

void settingsSave(const Settings& s)
{
  apply(s);
  Preferences p;
  if (p.begin(NVS_NAMESPACE, false)) {
    p.putString("ssid", s.wifiSsid);
    p.putString("pass", s.wifiPass);
    p.putString("url", s.haUrl);
    p.putString("token", s.haToken);
    p.putString("entity", s.entity);
    p.putString("tempent", s.tempEntity);
    p.putString("hument", s.humEntity);
    p.putString("tz", s.timezone);
    p.end();
  }
  print("Saved", s);
}

void settingsResetDefaults()
{
  Preferences p;
  if (p.begin(NVS_NAMESPACE, false)) {
    p.clear();
    p.end();
  }
  apply(defaults());
  print("Reset to defaults", defaults());
}

// ---- Validation ----------------------------------------------------------------

static bool printable(const String& s)
{
  for (size_t i = 0; i < s.length(); ++i)
    if ((uint8_t)s[i] < 0x20 || s[i] == 0x7F) return false;
  return true;
}

const char* validateWifiSsid(const String& ssid)
{
  if (ssid.length() < 1 || ssid.length() > 32) return "WiFi name must be 1-32 characters";
  if (!printable(ssid)) return "WiFi name contains invalid characters";
  return nullptr;
}

const char* validateWifiPass(const String& pass)
{
  if (pass.length() && (pass.length() < 8 || pass.length() > 63))
    return "WiFi password must be 8-63 characters (or tick \"Open network\")";
  if (!printable(pass)) return "WiFi password contains invalid characters";
  return nullptr;
}

const char* validateHaUrl(const String& url)
{
  if (!url.startsWith("http://") && !url.startsWith("https://"))
    return "Home Assistant URL must start with http:// or https://";
  if (url.length() >= sizeof(Settings::haUrl)) return "Home Assistant URL is too long";
  if (url.indexOf(' ') >= 0 || !printable(url)) return "Home Assistant URL must not contain spaces";
  const int hostStart = url.indexOf("//") + 2;
  if ((int)url.length() <= hostStart || url[hostStart] == '/' || url[hostStart] == ':')
    return "Home Assistant URL has no host name";
  if (url.endsWith("/")) return "Home Assistant URL must not end with /";
  return nullptr;
}

const char* validateHaToken(const String& token)
{
  // Empty is allowed (not configured yet); HA tokens are JWTs: base64url and dots
  if (token.length() >= sizeof(Settings::haToken)) return "Access token is too long";
  for (size_t i = 0; i < token.length(); ++i) {
    char c = token[i];
    if (!isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_')
      return "Access token contains invalid characters (copy it again from Home Assistant)";
  }
  return nullptr;
}

const char* validateEntity(const String& entity)
{
  if (!entity.startsWith("weather.") || entity.length() <= 8) return "Weather entity must look like weather.forecast_home";
  if (entity.length() >= sizeof(Settings::entity)) return "Weather entity is too long";
  for (size_t i = 8; i < entity.length(); ++i) {
    char c = entity[i];
    if (!islower((unsigned char)c) && !isdigit((unsigned char)c) && c != '_')
      return "Weather entity may only contain a-z, 0-9 and _ after \"weather.\"";
  }
  return nullptr;
}

const char* validateSensorEntity(const String& entity)
{
  if (!entity.length()) return nullptr;  // not used
  if (entity.length() >= sizeof(Settings::tempEntity)) return "Temperature / humidity entity is too long";
  // domain.object_id, e.g. sensor.balcony_temperature
  const int dot = entity.indexOf('.');
  if (dot < 1 || dot == (int)entity.length() - 1 || entity.indexOf('.', dot + 1) >= 0)
    return "Temperature / humidity entity must look like sensor.balcony_temperature (or be empty)";
  for (size_t i = 0; i < entity.length(); ++i) {
    char c = entity[i];
    if (!islower((unsigned char)c) && !isdigit((unsigned char)c) && c != '_' && c != '.')
      return "Temperature / humidity entity may only contain a-z, 0-9, _ and one dot";
  }
  return nullptr;
}

const char* validateTimezone(const String& tz)
{
  if (tz.length() < 3 || tz.length() >= sizeof(Settings::timezone)) return "Timezone must be a POSIX TZ string like CET-1CEST,M3.5.0,M10.5.0/3";
  if (tz.indexOf('/') >= 0 && tz.indexOf(',') < 0)
    return "Use a POSIX TZ string (e.g. CET-1CEST,M3.5.0,M10.5.0/3), not a zone name like Europe/Zurich";
  if (tz.indexOf(' ') >= 0 || !printable(tz)) return "Timezone must not contain spaces";
  return nullptr;
}
