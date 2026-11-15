#include "webui.h"
#include "settings.h"
#include "radio.h"
#include "config.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Update.h>

volatile bool otaInProgress = false;

static WebServer server(80);
static DNSServer dns;  // captive portal on the own AP: every name -> the AP's IP

// ---- Shared status (web task <-> loop) ---------------------------------------------
static portMUX_TYPE statusLock = portMUX_INITIALIZER_UNLOCKED;
static NetStatus netStatus{NetState::Joining, IPAddress(), false};
static time_t weatherLastOk = 0;
static char weatherError[64] = "";
static volatile bool refreshRequested = false;

NetStatus webUiNetStatus()
{
  portENTER_CRITICAL(&statusLock);
  NetStatus s = netStatus;
  portEXIT_CRITICAL(&statusLock);
  return s;
}

static void setNetStatus(NetState state, IPAddress ip)
{
  portENTER_CRITICAL(&statusLock);
  netStatus.state = state;
  netStatus.ip = ip;
  if (state == NetState::Joined) netStatus.everJoined = true;
  portEXIT_CRITICAL(&statusLock);
}

void webUiSetWeatherStatus(time_t lastOk, const char* error)
{
  portENTER_CRITICAL(&statusLock);
  weatherLastOk = lastOk;
  strlcpy(weatherError, error ? error : "", sizeof(weatherError));
  portEXIT_CRITICAL(&statusLock);
}

bool webUiTakeRefreshRequest()
{
  if (!refreshRequested) return false;
  refreshRequested = false;
  return true;
}

// ---- WiFi state machine (web task only) ---------------------------------------------
static NetState wifiState = NetState::Joining;
static uint32_t wifiStateSince = 0;
static bool apActive = false;
static bool netStarted = false;  // ArduinoOTA started
// Set by the page handlers: millis() | 1, 0 = none. The join starts a moment later so
// the redirected page still loads before the radio starts scanning.
static uint32_t wifiRejoinAt = 0;
#define WIFI_REJOIN_DELAY_MS 1500

static void startArduinoOta();

static void startMdns()
{
  MDNS.end();
  if (MDNS.begin(DEVICE_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("arduino", "tcp", 3232);  // lets OTA tools discover the device
  } else {
    Serial.println("[Web] mDNS start failed");
  }
}

static void netServicesUp()
{
  startMdns();
  if (!netStarted) {
    netStarted = true;
    startArduinoOta();
  }
  const IPAddress ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP();
  Serial.printf("[Web] Settings page: http://%s/  or  http://" DEVICE_HOSTNAME ".local/\n",
                ip.toString().c_str());
}

static void apStart();

static void wifiJoin()
{
  const Settings s = settingsGet();
  wifiState = NetState::Joining;
  wifiStateSince = millis();
  if (!s.wifiSsid[0]) {
    Serial.println("[WiFi] No network configured");
    if (!apActive) apStart();
    return;
  }
  WiFi.mode(apActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(s.wifiSsid, s.wifiPass[0] ? s.wifiPass : nullptr);
  if (!apActive) setNetStatus(NetState::Joining, IPAddress());
  Serial.printf("[WiFi] Joining \"%s\"%s...\n", s.wifiSsid, apActive ? " (access point stays up meanwhile)" : "");
}

// Stops the station from scanning: a scan hops channels and would take the AP with it
static void wifiStopJoining()
{
  WiFi.setAutoReconnect(false);
  WiFi.disconnect();
}

static void apStart()
{
  wifiStopJoining();
  WiFi.mode(WIFI_AP_STA);
  const bool secured = strlen(OTA_PASSWORD) >= 8;  // WPA2 needs 8+ characters
  if (!WiFi.softAP(DEVICE_HOSTNAME, secured ? OTA_PASSWORD : nullptr)) {
    Serial.println("[WiFi] Access point start failed");
    return;
  }
  dns.start(53, "*", WiFi.softAPIP());
  apActive = true;
  wifiState = NetState::OwnAp;
  wifiStateSince = millis();
  setNetStatus(NetState::OwnAp, WiFi.softAPIP());
  Serial.printf("[WiFi] Access point \"" DEVICE_HOSTNAME "\" (%s), page at http://%s/\n",
                secured ? "password = OTA password" : "open: OTA password under 8 characters",
                WiFi.softAPIP().toString().c_str());
  netServicesUp();
}

static void apStop()
{
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
  Serial.println("[WiFi] Access point closed");
}

static void handleWifi()
{
  const uint32_t now = millis();
  const bool joined = WiFi.status() == WL_CONNECTED;

  if (wifiRejoinAt && now - wifiRejoinAt >= WIFI_REJOIN_DELAY_MS) {  // network saved on the page
    wifiRejoinAt = 0;
    if (joined) WiFi.disconnect();
    wifiJoin();
    return;
  }

  switch (wifiState) {
    case NetState::Joining:
      if (joined) {
        wifiState = NetState::Joined;
        wifiStateSince = now;
        if (apActive) apStop();
        setNetStatus(NetState::Joined, WiFi.localIP());
        Serial.printf("[WiFi] Joined \"%s\", IP %s, %d dBm\n", WiFi.SSID().c_str(),
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        netServicesUp();
      } else if (now - wifiStateSince >= WIFI_JOIN_TIMEOUT_MS) {
        if (apActive) {
          // Retry from the AP failed: keep the AP, stop scanning
          wifiStopJoining();
          wifiState = NetState::OwnAp;
          wifiStateSince = now;
          setNetStatus(NetState::OwnAp, WiFi.softAPIP());
          Serial.printf("[WiFi] Could not join \"%s\", access point stays up\n", settingsGet().wifiSsid);
        } else {
          Serial.printf("[WiFi] \"%s\" not reachable\n", settingsGet().wifiSsid);
          apStart();
        }
      }
      break;
    case NetState::Joined:
      if (joined) {
        wifiStateSince = now;
        setNetStatus(NetState::Joined, WiFi.localIP());  // DHCP may renew to another address
      } else if (now - wifiStateSince >= 3 * WIFI_JOIN_TIMEOUT_MS) {
        // Network gone for a while (autoReconnect kept trying): fall back to the own AP
        Serial.println("[WiFi] Connection lost");
        apStart();
      }
      break;
    case NetState::OwnAp:
      // Retry the network now and then, but never while someone uses the setup page
      if (now - wifiStateSince >= WIFI_RETRY_MS && WiFi.softAPgetStationNum() == 0) wifiJoin();
      break;
  }
  if (apActive) dns.processNextRequest();
}

// ---- Page ------------------------------------------------------------------------
static String htmlEscape(const char* in)
{
  String out;
  for (; *in; in++) {
    switch (*in) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      default: out += *in;
    }
  }
  return out;
}

static const char PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>)HTML" DEVICE_TITLE R"HTML(</title>
<style>
body{font-family:system-ui,sans-serif;max-width:480px;margin:24px auto;padding:0 16px;background:#111;color:#eee}
h1{font-size:1.3em}h2{font-size:1.1em;margin-top:24px}label{display:block;margin-top:14px;font-size:.9em;color:#aaa}
input{width:100%;box-sizing:border-box;padding:9px;font-size:1em;border:1px solid #444;border-radius:6px;background:#222;color:#eee}
button{margin-top:16px;padding:10px 16px;font-size:1em;border:0;border-radius:6px;cursor:pointer}
.save{background:#2a7;color:#fff}.test{background:#357;color:#fff}.reset{background:#444;color:#ccc}
.msg{padding:10px;border-radius:6px;margin:12px 0}.ok{background:#1d4d2e}.err{background:#5a1d1d}
.info{font-size:.85em;color:#888;margin-top:24px;line-height:1.6}.warn{color:#fc8}
form.inline{display:inline}a{color:#8af}
</style></head><body><h1>)HTML" DEVICE_TITLE R"HTML(</h1>
)HTML";

// ---- Radio ------------------------------------------------------------------------
static String jsonEscape(const char* in)
{
  String out;
  for (; *in; in++) {
    const char c = *in;
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
    else out += c;
  }
  return out;
}

// Inner HTML of the radio status line; the page re-fetches it from /radio/status
static String radioStatusHtml()
{
  const RadioStatus r = radioStatus();
  String html;
  switch (r.state) {
    case RadioState::Playing:
      html += F("&#9654; <b>");
      html += htmlEscape(r.name);
      html += F("</b>");
      if (r.title[0]) {
        html += F(" &middot; ");
        html += htmlEscape(r.title);
      }
      break;
    case RadioState::Connecting:
      html += F("Connecting to <b>");
      html += htmlEscape(r.name);
      html += F("</b>&hellip;");
      break;
    case RadioState::Retrying:
      html += F("<b>");
      html += htmlEscape(r.name);
      html += F("</b>: reconnecting&hellip;");
      break;
    default:
      html += F("Stopped");
  }
  if (r.error[0]) {
    html += F("<br><span class=\"warn\">");
    html += htmlEscape(r.error);
    html += F("</span>");
  }
  return html;
}

static void appendRadioSection(String& html)
{
  const RadioStatus r = radioStatus();
  html += F("<h2>Radio</h2><div id=\"radio\" class=\"info\" style=\"margin-top:0\">");
  html += radioStatusHtml();
  html += F("</div>");
  bool any = false;
  for (int i = 0; i < RADIO_MAX_STATIONS; ++i) {
    const Station st = radioStation(i);
    if (!st.url[0]) continue;
    any = true;
    html += F("<form class=\"inline\" method=\"post\" action=\"/radio/play\"><input type=\"hidden\" name=\"station\" value=\"");
    html += i;
    html += F("\"><button class=\"test\" style=\"margin-right:6px\">");
    html += htmlEscape(st.name[0] ? st.name : "Station");
    html += F("</button></form>");
  }
  if (!any) html += F("<p class=\"info\">No stations yet.</p>");
  html += F("<form class=\"inline\" method=\"post\" action=\"/radio/stop\"><button class=\"reset\">&#9632; Stop</button></form>"
            "<label>Volume <span id=\"vv\">");
  html += r.volume;
  html += F("</span></label><input type=\"range\" min=\"0\" max=\"100\" style=\"padding:0\" value=\"");
  html += r.volume;
  html += F("\" oninput=\"vv.textContent=this.value\" "
            "onchange=\"fetch('/api/radio/volume?value='+this.value,{method:'POST'})\">"
            "<p><a href=\"/stations\">Edit stations</a></p>"
            "<script>setInterval(()=>{if(!document.hidden)fetch('/radio/status').then(r=>r.ok?r.text():Promise.reject())"
            ".then(t=>{document.getElementById('radio').innerHTML=t}).catch(()=>{})},3000)</script>");
}

static void sendPage(const char* message = nullptr, bool isError = false)
{
  const Settings s = settingsGet();
  // Sent in chunks: building the whole page (~4 KB) at once can exhaust the heap
  // while the radio is playing
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  String html;
  html.reserve(1600);
  auto flush = [&] {
    server.sendContent(html);
    html = "";
  };
  html += FPSTR(PAGE_HEAD);

  if (message) {
    html += F("<div class=\"msg ");
    html += isError ? F("err") : F("ok");
    html += F("\">");
    html += htmlEscape(message);
    html += F("</div>");
  }

  appendRadioSection(html);
  flush();

  html += F("<form method=\"post\" action=\"/save\">"
            "<h2>Home Assistant</h2>"
            "<label>URL (e.g. http://homeassistant.local:8123; plain http on the LAN is recommended)</label>"
            "<input name=\"url\" required maxlength=\"127\" value=\"");
  html += htmlEscape(s.haUrl);
  html += F("\"><label>Long-lived access token (Profile &rarr; Security; leave empty to keep the current one)</label>"
            "<input name=\"token\" type=\"password\" maxlength=\"255\" autocomplete=\"off\" placeholder=\"");
  html += s.haToken[0] ? F("unchanged") : F("not set");
  html += F("\"><label>Weather entity</label>"
            "<input name=\"entity\" required maxlength=\"63\" placeholder=\"weather.forecast_home\" value=\"");
  html += htmlEscape(s.entity);
  html += F("\"><label>Timezone (POSIX TZ string, "
            "<a href=\"https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv\" target=\"_blank\">list</a>)</label>"
            "<input name=\"tz\" required maxlength=\"63\" value=\"");
  html += htmlEscape(s.timezone);
  html += F("\"><h2>WiFi</h2>"
            "<label>Network name (if it can't be joined, the clock opens its own access point \"" DEVICE_HOSTNAME "\")</label>"
            "<input name=\"ssid\" required maxlength=\"32\" value=\"");
  html += htmlEscape(s.wifiSsid);
  html += F("\"><label>Password (leave empty to keep the current one)</label>"
            "<input name=\"pass\" type=\"password\" maxlength=\"63\" autocomplete=\"new-password\" placeholder=\"unchanged\">"
            "<label><input name=\"open\" type=\"checkbox\" style=\"width:auto\"> Open network (no password)</label>"
            "<button class=\"save\" type=\"submit\">Save</button></form>"
            "<form class=\"inline\" method=\"post\" action=\"/refresh\"><button class=\"test\">Refresh weather now</button></form> "
            "<form class=\"inline\" method=\"post\" action=\"/reset\" "
            "onsubmit=\"return confirm('Revert all settings to the firmware defaults?')\">"
            "<button class=\"reset\">Reset to defaults</button></form>"
            "<p><a href=\"/update\">Firmware update</a></p>");
  flush();

  // Status
  time_t lastOk;
  char err[64];
  portENTER_CRITICAL(&statusLock);
  lastOk = weatherLastOk;
  strlcpy(err, weatherError, sizeof(err));
  portEXIT_CRITICAL(&statusLock);

  const bool joined = WiFi.status() == WL_CONNECTED;
  html += F("<div class=\"info\">");
  if (joined) {
    html += F("WiFi: ");
    html += htmlEscape(WiFi.SSID().c_str());
    html += F(" (");
    html += WiFi.RSSI();
    html += F(" dBm), IP ");
    html += WiFi.localIP().toString();
  } else {
    html += F("<b class=\"warn\">Own access point \"" DEVICE_HOSTNAME "\" (");
    html += WiFi.softAPIP().toString();
    html += F("): &quot;");
    html += htmlEscape(s.wifiSsid);
    html += F("&quot; could not be joined. Saving the WiFi settings tries again.</b>");
  }
  html += F("<br>Hostname: " DEVICE_HOSTNAME ".local<br>Audio output: ");
  html += radioOutputName();
  html += F("<br>Weather: ");
  if (lastOk > 0) {
    char buf[32];
    tm lt;
    localtime_r(&lastOk, &lt);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &lt);
    html += F("last update ");
    html += buf;
  } else {
    html += F("no data yet");
  }
  if (err[0]) {
    html += F("<br><b class=\"warn\">");
    html += htmlEscape(err);
    html += F("</b>");
  }
  // Byte-addressable RAM only (ESP.getFreeHeap() also counts 32-bit-only IRAM)
  html += F("<br>Free memory: ");
  html += heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024;
  html += F(" KB (largest block ");
  html += heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024;
  html += F(" KB, lowest so far ");
  html += heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT) / 1024;
  html += F(" KB)<br>Firmware built: " __DATE__ " " __TIME__ "</div></body></html>");
  flush();
  server.sendContent("");  // end of chunked response
}

// WebServer drops query args without '=', so status flags are sent as "flag=1"
static void redirectHome(const char* flag)
{
  server.sendHeader("Location", *flag ? String("/?") + flag + "=1" : String("/"));
  server.send(303);
}

static void handleRoot()
{
  if (server.hasArg("saved")) sendPage("Saved. The weather is reloaded now.");
  else if (server.hasArg("join"))
    sendPage("Saved. Joining the WiFi network now; if it works, the access point closes and this page "
             "moves to http://" DEVICE_HOSTNAME ".local/ on that network.");
  else if (server.hasArg("refresh")) sendPage("Weather refresh requested.");
  else if (server.hasArg("reset")) sendPage("Reverted to firmware defaults.");
  else sendPage();
}

static void handleSave()
{
  const Settings old = settingsGet();
  String url = server.arg("url");
  url.trim();
  while (url.endsWith("/")) url.remove(url.length() - 1);
  String token = server.arg("token");
  token.trim();
  if (!token.length()) token = old.haToken;  // empty = keep (it is never sent to the page)
  String entity = server.arg("entity");
  entity.trim();
  entity.toLowerCase();
  String tz = server.arg("tz");
  tz.trim();
  String ssid = server.arg("ssid");  // not trimmed: spaces are valid in an SSID
  String pass = server.hasArg("open") ? String("")
              : server.arg("pass").length() ? server.arg("pass")
              : String(old.wifiPass);

  const char* err = validateHaUrl(url);
  if (!err) err = validateHaToken(token);
  if (!err) err = validateEntity(entity);
  if (!err) err = validateTimezone(tz);
  if (!err) err = validateWifiSsid(ssid);
  if (!err) err = validateWifiPass(pass);
  if (err) {
    sendPage(err, true);  // keep stored values, show error
    return;
  }

  Settings s{};
  strlcpy(s.haUrl, url.c_str(), sizeof(s.haUrl));
  strlcpy(s.haToken, token.c_str(), sizeof(s.haToken));
  strlcpy(s.entity, entity.c_str(), sizeof(s.entity));
  strlcpy(s.timezone, tz.c_str(), sizeof(s.timezone));
  strlcpy(s.wifiSsid, ssid.c_str(), sizeof(s.wifiSsid));
  strlcpy(s.wifiPass, pass.c_str(), sizeof(s.wifiPass));
  settingsSave(s);  // loop() sees the new version, applies the timezone and refetches

  const bool wifiChanged = strcmp(s.wifiSsid, old.wifiSsid) || strcmp(s.wifiPass, old.wifiPass);
  const bool retry = wifiChanged || WiFi.status() != WL_CONNECTED;
  if (retry) wifiRejoinAt = millis() | 1;
  redirectHome(retry ? "join" : "saved");
}

static void handleRefresh()
{
  refreshRequested = true;
  redirectHome("refresh");
}

static void handleReset()
{
  const Settings old = settingsGet();
  settingsResetDefaults();
  const Settings s = settingsGet();
  if (strcmp(old.wifiSsid, s.wifiSsid) || strcmp(old.wifiPass, s.wifiPass) || WiFi.status() != WL_CONNECTED)
    wifiRejoinAt = millis() | 1;
  redirectHome("reset");
}

// ---- Radio handlers ---------------------------------------------------------------
static void handleRadioPlayForm()
{
  radioPlay(server.arg("station").toInt());
  redirectHome("");
}

static void handleRadioStopForm()
{
  radioStop();
  redirectHome("");
}

static void handleRadioStatusHtml()
{
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", radioStatusHtml());
}

// JSON API for Home Assistant (rest_command / RESTful sensor). Stations are 1-based.
static void sendRadioJson(int code = 200, const char* error = nullptr)
{
  const RadioStatus r = radioStatus();
  String json = F("{\"state\":\"");
  json += radioStateName(r.state);
  json += F("\",\"station\":");
  json += r.station >= 0 ? r.station + 1 : 0;
  json += F(",\"name\":\"");
  json += jsonEscape(r.state == RadioState::Stopped ? "" : r.name);
  json += F("\",\"title\":\"");
  json += jsonEscape(r.title);
  json += F("\",\"volume\":");
  json += r.volume;
  json += F(",\"error\":\"");
  json += jsonEscape(error ? error : r.error);
  json += F("\",\"output\":\"");
  json += radioOutputName();
  json += F("\",\"underruns\":");
  json += radioUnderruns();
  json += F("}");
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", json);
}

static void handleApiRadio() { sendRadioJson(); }

static void handleApiPlay()
{
  if (server.hasArg("url")) {
    Station st{};
    strlcpy(st.url, server.arg("url").c_str(), sizeof(st.url));
    strlcpy(st.name, server.arg("name").c_str(), sizeof(st.name));
    if (const char* err = validateStation(st)) return sendRadioJson(400, err);
    if (!st.url[0]) return sendRadioJson(400, "url is empty");
    radioPlayUrl(st.url, st.name);
  } else {
    // station=N (1-based); without it the last played station (or the first one)
    int idx = server.hasArg("station") ? server.arg("station").toInt() - 1 : radioStatus().station;
    if (idx < 0) idx = 0;
    if (idx >= RADIO_MAX_STATIONS || !radioStation(idx).url[0]) return sendRadioJson(400, "No such station");
    radioPlay(idx);
  }
  sendRadioJson();
}

static void handleApiStop()
{
  radioStop();
  sendRadioJson();
}

static void handleApiVolume()
{
  int v = radioStatus().volume;
  if (server.hasArg("value")) v = server.arg("value").toInt();
  else if (server.hasArg("change")) v += server.arg("change").toInt();  // e.g. change=-5
  else return sendRadioJson(400, "Use value=0-100 or change=+N/-N");
  radioSetVolume(constrain(v, 0, 100));
  sendRadioJson();
}

// ---- Station editor ----------------------------------------------------------------
static void sendStationsPage(const char* message = nullptr, bool isError = false)
{
  String html;
  html.reserve(4000);
  html += FPSTR(PAGE_HEAD);
  html += F("<h2>Radio stations</h2>");
  if (message) {
    html += F("<div class=\"msg ");
    html += isError ? F("err") : F("ok");
    html += F("\">");
    html += htmlEscape(message);
    html += F("</div>");
  }
  html += F("<p class=\"info\" style=\"margin-top:0\">http:// MP3 streams only (find URLs e.g. on "
            "<a href=\"https://www.radio-browser.info\" target=\"_blank\">radio-browser.info</a>, codec MP3). "
            "Empty URL = unused slot. Home Assistant uses the numbers below.</p>"
            "<form method=\"post\" action=\"/stations\">");
  for (int i = 0; i < RADIO_MAX_STATIONS; ++i) {
    const Station st = radioStation(i);
    html += F("<label>");
    html += i + 1;
    html += F("</label><input name=\"n");
    html += i;
    html += F("\" maxlength=\"31\" placeholder=\"Name\" value=\"");
    html += htmlEscape(st.name);
    html += F("\"><input name=\"u");
    html += i;
    html += F("\" maxlength=\"127\" placeholder=\"http://...\" style=\"margin-top:4px\" value=\"");
    html += htmlEscape(st.url);
    html += F("\">");
  }
  // Autoplay at boot
  const RadioAutoplay ap = radioAutoplay();
  html += F("<h2>At startup</h2>"
            "<label><input name=\"auto\" type=\"checkbox\" style=\"width:auto\"");
  if (ap.enabled) html += F(" checked");
  html += F("> Start playing automatically when the clock boots</label>"
            "<label>Station</label><select name=\"autost\" style=\"width:100%;padding:9px;font-size:1em;"
            "border:1px solid #444;border-radius:6px;background:#222;color:#eee\">");
  for (int i = 0; i < RADIO_MAX_STATIONS; ++i) {
    const Station st = radioStation(i);
    html += F("<option value=\"");
    html += i;
    html += '"';
    if (i == ap.station) html += F(" selected");
    html += '>';
    html += i + 1;
    html += F(": ");
    html += htmlEscape(st.url[0] ? (st.name[0] ? st.name : "Station") : "(empty)");
    html += F("</option>");
  }
  html += F("</select>"
            "<button class=\"save\" type=\"submit\">Save</button></form>"
            "<p><a href=\"/\">Back</a></p></body></html>");
  server.send(200, "text/html", html);
}

static void handleStationsSave()
{
  Station list[RADIO_MAX_STATIONS];
  for (int i = 0; i < RADIO_MAX_STATIONS; ++i) {
    String name = server.arg(String("n") + i), url = server.arg(String("u") + i);
    name.trim();
    url.trim();
    if (url.length() >= sizeof(Station::url)) return sendStationsPage("A station URL is too long (max 127)", true);
    strlcpy(list[i].name, name.c_str(), sizeof(list[i].name));
    strlcpy(list[i].url, url.c_str(), sizeof(list[i].url));
    if (const char* err = validateStation(list[i])) {
      String msg = String("Station ") + (i + 1) + ": " + err;
      return sendStationsPage(msg.c_str(), true);
    }
  }
  RadioAutoplay ap{server.hasArg("auto"), (int)server.arg("autost").toInt()};  // checkbox sent only when checked
  if (ap.station < 0 || ap.station >= RADIO_MAX_STATIONS) ap.station = 0;
  if (ap.enabled && !list[ap.station].url[0]) {
    String msg = String("Autoplay: station ") + (ap.station + 1) + " has no URL";
    return sendStationsPage(msg.c_str(), true);
  }
  for (int i = 0; i < RADIO_MAX_STATIONS; ++i) {
    const Station old = radioStation(i);
    if (strcmp(old.name, list[i].name) || strcmp(old.url, list[i].url)) radioSetStation(i, list[i]);
  }
  const RadioAutoplay oldAp = radioAutoplay();
  if (oldAp.enabled != ap.enabled || oldAp.station != ap.station) radioSetAutoplay(ap);
  sendStationsPage("Saved.");
}

// ---- Web firmware upload ----------------------------------------------------------
static bool uploadRejected = false;
static String updateError;

static bool updateAuthorized()
{
  if (strlen(OTA_PASSWORD) == 0) return true;
  return server.authenticate(OTA_USER, OTA_PASSWORD);
}

static void handleUpdatePage()
{
  if (!updateAuthorized()) return server.requestAuthentication();
  String html = FPSTR(PAGE_HEAD);
  html += F("<h2>Firmware update</h2>"
            "<p class=\"info\">Upload <code>.pio/build/esp32dev_pal/firmware.bin</code>. "
            "Settings are kept. The clock reboots when done.</p>"
            "<form method=\"post\" action=\"/update\" enctype=\"multipart/form-data\" "
            "onsubmit=\"b.disabled=true;b.textContent='Uploading... do not close this page'\">"
            "<input type=\"file\" name=\"firmware\" accept=\".bin\" required>"
            "<button id=\"b\" class=\"save\">Upload &amp; install</button></form>"
            "<p><a href=\"/\">Back</a></p>"
            "<div class=\"info\">Current firmware built: " __DATE__ " " __TIME__ "</div></body></html>");
  server.send(200, "text/html", html);
}

static void handleUpdateUpload()
{
  HTTPUpload& up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      uploadRejected = !updateAuthorized();
      updateError = "";
      if (uploadRejected) return;
      otaInProgress = true;
      Serial.printf("[OTA] Web upload started: %s\n", up.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) updateError = Update.errorString();
      break;
    case UPLOAD_FILE_WRITE:
      if (uploadRejected || updateError.length()) return;
      if (Update.write(up.buf, up.currentSize) != up.currentSize) updateError = Update.errorString();
      break;
    case UPLOAD_FILE_END:
      if (uploadRejected) return;
      if (!updateError.length() && !Update.end(true)) updateError = Update.errorString();
      if (!updateError.length()) Serial.printf("[OTA] Web upload done, %u bytes\n", (unsigned)up.totalSize);
      break;
    case UPLOAD_FILE_ABORTED:
      updateError = "Upload aborted";
      break;
  }
}

static void handleUpdateDone()
{
  if (uploadRejected) return server.requestAuthentication();
  if (updateError.length()) {
    if (Update.isRunning()) Update.abort();
    otaInProgress = false;
    Serial.printf("[OTA] Web update failed: %s\n", updateError.c_str());
    String msg = "Update failed: " + updateError;
    sendPage(msg.c_str(), true);
    return;
  }
  String html = FPSTR(PAGE_HEAD);
  html += F("<meta http-equiv=\"refresh\" content=\"15;url=/\">"
            "<div class=\"msg ok\">Update installed. Rebooting&hellip; this page reloads in 15 s.</div></body></html>");
  server.send(200, "text/html", html);
  delay(500);
  ESP.restart();
}

// ---- ArduinoOTA (PlatformIO network upload) ---------------------------------------
static void startArduinoOta()
{
  ArduinoOTA.setHostname(DEVICE_HOSTNAME);
  ArduinoOTA.setMdnsEnabled(false);  // mDNS is started separately
  if (strlen(OTA_PASSWORD)) ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA
      .onStart([] {
        otaInProgress = true;
        Serial.println("[OTA] Network upload started");
      })
      .onEnd([] { Serial.println("[OTA] Network upload done, rebooting"); })
      .onError([](ota_error_t e) {
        otaInProgress = false;
        Serial.printf("[OTA] Network upload error %d\n", (int)e);
      });
  ArduinoOTA.begin();
}

static void webTask(void*)
{
  wifiJoin();
  for (;;) {
    handleWifi();
    if (netStarted) ArduinoOTA.handle();  // blocks for the duration of an upload (this task only)
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void webUiBegin()
{
  WiFi.persistent(false);  // credentials live in our own settings, not the SDK's flash copy
  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.mode(WIFI_STA);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/refresh", HTTP_POST, handleRefresh);
  server.on("/reset", HTTP_POST, handleReset);
  server.on("/radio/play", HTTP_POST, handleRadioPlayForm);
  server.on("/radio/stop", HTTP_POST, handleRadioStopForm);
  server.on("/radio/status", HTTP_GET, handleRadioStatusHtml);
  server.on("/stations", HTTP_GET, [] { sendStationsPage(); });
  server.on("/stations", HTTP_POST, handleStationsSave);
  server.on("/api/radio", HTTP_GET, handleApiRadio);
  server.on("/api/radio/play", HTTP_ANY, handleApiPlay);
  server.on("/api/radio/stop", HTTP_ANY, handleApiStop);
  server.on("/api/radio/volume", HTTP_ANY, handleApiVolume);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound([] { redirectHome(""); });  // also catches captive-portal checks
  server.begin();

  xTaskCreate(webTask, "web", 5120, nullptr, 1, nullptr);  // ~2 KB used, measured
}
