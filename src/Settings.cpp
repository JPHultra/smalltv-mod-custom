#include "Settings.h"
#include "AtomicJson.h"
#include "Platform.h"
#include <LittleFS.h>

static const char* CONFIG_PATH = "/config.json";
static size_t g_storedSize = 0;

// ===========================================================================
// Clock / night mode slice
// ===========================================================================
static uint16_t hhmmToMin(const char* s, uint16_t fallback) {
  if (!s || !s[0]) return fallback;
  int h = 0, m = 0;
  if (sscanf(s, "%d:%d", &h, &m) != 2) return fallback;
  if (h < 0 || h > 23 || m < 0 || m > 59) return fallback;
  return (uint16_t)(h * 60 + m);
}
static String minToHhmm(uint16_t v) {
  if (v > 1439) v = 0;
  char b[6];
  snprintf(b, sizeof(b), "%02u:%02u", (unsigned)(v / 60), (unsigned)(v % 60));
  return String(b);
}

void ClockSettings::setDefaults() {
  tz            = DEFAULT_TZ_NAME;
  tzPosix       = DEFAULT_TZ_POSIX;
  nightEnabled  = DEFAULT_NIGHT_ENABLED;
  nightStartMin = DEFAULT_NIGHT_START_MIN;
  nightEndMin   = DEFAULT_NIGHT_END_MIN;
  nightLevel    = DEFAULT_NIGHT_LEVEL;
}

void ClockSettings::toJson(JsonObject o) const {
  o["tz"]           = tz;
  o["tzPosix"]      = tzPosix;
  o["nightEnabled"] = nightEnabled;
  o["nightStart"]   = minToHhmm(nightStartMin);
  o["nightEnd"]     = minToHhmm(nightEndMin);
  o["nightLevel"]   = nightLevel;
}

void ClockSettings::fromJson(JsonObjectConst o) {
  if (o["tz"].is<const char*>())          tz = o["tz"].as<String>();
  if (o["tzPosix"].is<const char*>())     tzPosix = o["tzPosix"].as<String>();
  if (o["nightEnabled"].is<bool>())       nightEnabled = o["nightEnabled"];
  if (o["nightStart"].is<const char*>())  nightStartMin = hhmmToMin(o["nightStart"], nightStartMin);
  if (o["nightEnd"].is<const char*>())    nightEndMin   = hhmmToMin(o["nightEnd"], nightEndMin);
  if (o["nightLevel"].is<int>())          nightLevel = constrain((int)o["nightLevel"], 0, 100);
}

// ===========================================================================
// Physical button slice
// ===========================================================================
void ButtonSettings::setDefaults() {
  sensitivity = DEFAULT_BTN_SENS;
  shortAction = DEFAULT_BTN_SHORT;
  longAction  = DEFAULT_BTN_LONG;
}

void ButtonSettings::toJson(JsonObject o) const {
  o["sensitivity"] = sensitivity;
  o["shortAction"] = shortAction;
  o["longAction"]  = longAction;
}

void ButtonSettings::fromJson(JsonObjectConst o) {
  if (o["sensitivity"].is<int>()) sensitivity = constrain((int)o["sensitivity"], 50, 99);
  if (o["shortAction"].is<int>()) shortAction = constrain((int)o["shortAction"], 0, BTN_ACT_MAX);
  if (o["longAction"].is<int>())  longAction  = constrain((int)o["longAction"],  0, BTN_ACT_MAX);
}

// ===========================================================================
// Web UI password slice
// ===========================================================================
void AuthSettings::setDefaults() {
  enabled = false;
  user = DEFAULT_AUTH_USER;
  pass = "";
}

void AuthSettings::toJson(JsonObject o, bool includeSecrets) const {
  o["enabled"] = enabled && pass.length() > 0;
  o["user"]    = user;
  o["passSet"] = pass.length() > 0;
  if (includeSecrets) o["pass"] = pass;
}

void AuthSettings::fromJson(JsonObjectConst o) {
  if (o["user"].is<const char*>()) user = o["user"].as<String>();
  if (o["pass"].is<const char*>()) {
    String p = o["pass"].as<String>();
    if (p.length()) pass = p;
  }
  if (o["enabled"].is<bool>()) enabled = o["enabled"];
  if (user.length() >= MAX_AUTH_USER_LEN) user.remove(MAX_AUTH_USER_LEN - 1);
  if (pass.length() >= MAX_AUTH_PASS_LEN) pass.remove(MAX_AUTH_PASS_LEN - 1);
  if (!user.length()) user = DEFAULT_AUTH_USER;
  if (!pass.length()) enabled = false;
}

// ===========================================================================
// WireGuard slice
// ===========================================================================
void WgSettings::setDefaults() {
  enabled = false;
  privateKey = "";
  peerPublicKey = "";
  endpointHost = "";
  endpointPort = DEFAULT_WG_PORT;
  address = "";
  allowedIps = "";
  keepalive = DEFAULT_WG_KEEPALIVE;
}

void WgSettings::toJson(JsonObject o, bool includeSecrets) const {
  o["enabled"]        = enabled;
  o["peerPublicKey"]  = peerPublicKey;
  o["endpointHost"]   = endpointHost;
  o["endpointPort"]   = endpointPort;
  o["address"]        = address;
  o["allowedIps"]     = allowedIps;
  o["keepalive"]      = keepalive;
  o["privateKeySet"]  = privateKey.length() > 0;
  if (includeSecrets) o["privateKey"] = privateKey;
}

void WgSettings::fromJson(JsonObjectConst o) {
  if (o["enabled"].is<bool>())              enabled = o["enabled"];
  if (o["peerPublicKey"].is<const char*>()) peerPublicKey = o["peerPublicKey"].as<String>();
  if (o["endpointHost"].is<const char*>())  endpointHost = o["endpointHost"].as<String>();
  if (o["endpointPort"].is<int>())          endpointPort = constrain((int)o["endpointPort"], 1, 65535);
  if (o["address"].is<const char*>())       address = o["address"].as<String>();
  if (o["allowedIps"].is<const char*>())    allowedIps = o["allowedIps"].as<String>();
  if (o["keepalive"].is<int>())             keepalive = constrain((int)o["keepalive"], 0, 3600);
  if (o["privateKey"].is<const char*>()) {
    String p = o["privateKey"].as<String>();
    if (p.length()) privateKey = p;
  }
  if (peerPublicKey.length() >= MAX_WG_KEY_LEN)   peerPublicKey.remove(MAX_WG_KEY_LEN - 1);
  if (privateKey.length()    >= MAX_WG_KEY_LEN)   privateKey.remove(MAX_WG_KEY_LEN - 1);
  if (endpointHost.length()  >= MAX_WG_HOST_LEN)  endpointHost.remove(MAX_WG_HOST_LEN - 1);
  if (allowedIps.length()    >= MAX_WG_ALLOWED_LEN) allowedIps.remove(MAX_WG_ALLOWED_LEN - 1);
}

// ===========================================================================
// Panel colour slice
// ===========================================================================
void DisplaySettings::setDefaults() {
  colorOrder = DEFAULT_COLOR_ORDER;
  invert     = DEFAULT_COLOR_INVERT;
  rGain = gGain = bGain = DEFAULT_COLOR_GAIN;
}

void DisplaySettings::toJson(JsonObject o) const {
  o["colorOrder"] = (colorOrder == COLOR_ORDER_RGB) ? "rgb"
                  : (colorOrder == COLOR_ORDER_BGR) ? "bgr" : "auto";
  o["invert"] = invert;
  o["rGain"]  = rGain;
  o["gGain"]  = gGain;
  o["bGain"]  = bGain;
}

void DisplaySettings::fromJson(JsonObjectConst o) {
  if (o["colorOrder"].is<const char*>()) {
    String c = o["colorOrder"].as<String>();
    colorOrder = c.equalsIgnoreCase("rgb") ? COLOR_ORDER_RGB
               : c.equalsIgnoreCase("bgr") ? COLOR_ORDER_BGR : COLOR_ORDER_AUTO;
  }
  if (o["invert"].is<bool>()) invert = o["invert"];
  if (o["rGain"].is<int>()) rGain = constrain((int)o["rGain"], MIN_COLOR_GAIN, MAX_COLOR_GAIN);
  if (o["gGain"].is<int>()) gGain = constrain((int)o["gGain"], MIN_COLOR_GAIN, MAX_COLOR_GAIN);
  if (o["bGain"].is<int>()) bGain = constrain((int)o["bGain"], MIN_COLOR_GAIN, MAX_COLOR_GAIN);
}

// ===========================================================================
// Dashboard / overlay slice
// ===========================================================================
void ViewSettings::setDefaults() {
  pollUrl = "";
  pollSec = VIEW_DEFAULT_POLL_SEC;
  ttlSec  = VIEW_DEFAULT_TTL_SEC;
  editorToken = DEFAULT_EDITOR_TOKEN;
}

void ViewSettings::toJson(JsonObject o, bool includeSecrets) const {
  o["pollUrl"]  = pollUrl;
  o["pollSec"]  = pollSec;
  o["ttlSec"]   = ttlSec;
  o["tokenSet"] = editorToken.length() > 0;
  if (includeSecrets) o["editorToken"] = editorToken;
}

void ViewSettings::fromJson(JsonObjectConst o) {
  if (o["pollUrl"].is<const char*>()) pollUrl = o["pollUrl"].as<String>();
  if (o["pollSec"].is<int>()) {
    int v = (int)o["pollSec"];
    if (v > 0 && v < VIEW_POLL_MIN_SEC) v = VIEW_POLL_MIN_SEC;
    pollSec = constrain(v, 0, VIEW_POLL_MAX_SEC);
  }
  if (o["ttlSec"].is<int>())  ttlSec  = constrain((int)o["ttlSec"], VIEW_OVERLAY_TTL_MIN_SEC, VIEW_OVERLAY_TTL_MAX_SEC);
  if (o["editorToken"].is<const char*>()) {
    String t = o["editorToken"].as<String>();
    if (t.length()) editorToken = t;
  }
  if (editorToken.length() >= MAX_EDITOR_TOKEN_LEN) editorToken.remove(MAX_EDITOR_TOKEN_LEN - 1);
}

// ===========================================================================
// Top-level settings
// ===========================================================================
void Settings::setDefaults() {
  wifiCount = 0;
  for (uint8_t i = 0; i < MAX_WIFI_NETS; i++) {
    wifi[i].ssid = "";
    wifi[i].pass = "";
  }
  apSsid  = DEFAULT_AP_SSID;
  apPass  = DEFAULT_AP_PASS;
  hostname = String(DEFAULT_HOSTNAME) + "-" + String(platformChipId() & 0xFFFF, HEX);

  httpTimeout = DEFAULT_HTTP_TIMEOUT;
  brightness = DEFAULT_BRIGHTNESS;
  autoBrightness = false;
  backlightInverted = TFT_BL_DEFAULT_INVERTED;
  rotation = 0;

  clock.setDefaults();
  display.setDefaults();
  wg.setDefaults();
  auth.setDefaults();
  button.setDefaults();
  view.setDefaults();
}

// ---------------------------------------------------------------------------
bool settingsBegin() {
  if (LittleFS.begin()) return true;
  if (LittleFS.format() && LittleFS.begin()) return true;
  return false;
}

bool loadSettings(Settings& s) {
  s.setDefaults();
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return false;
  g_storedSize = f.size();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  settingsApplyJson(s, doc.as<JsonObjectConst>());
  return true;
}

bool saveSettings(const Settings& s) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  settingsToJson(s, root, /*includeSecrets=*/true);
  if (!saveJsonAtomically(LittleFS, doc, CONFIG_PATH, "/config.json.tmp")) return false;
  g_storedSize = measureJson(doc);
  return true;
}

void factoryReset(Settings& s) {
  LittleFS.remove(CONFIG_PATH);
  g_storedSize = 0;
  s.setDefaults();
}

size_t settingsStoredSize() { return g_storedSize; }

// ---------------------------------------------------------------------------
void settingsToJson(const Settings& s, JsonObject root, bool includeSecrets) {
  root["hostname"] = s.hostname;

  JsonArray wf = root["wifi"].to<JsonArray>();
  for (uint8_t i = 0; i < s.wifiCount; i++) {
    JsonObject e = wf.add<JsonObject>();
    e["ssid"]    = s.wifi[i].ssid;
    e["passSet"] = s.wifi[i].pass.length() > 0;
    if (includeSecrets) e["pass"] = s.wifi[i].pass;
  }
  // Legacy mirror of the primary network, so an older cached page or a
  // downgrade still finds its WiFi in config.json.
  root["staSsid"]    = s.wifiCount ? s.wifi[0].ssid : "";
  root["staPassSet"] = s.wifiCount && s.wifi[0].pass.length() > 0;
  root["apSsid"]     = s.apSsid;
  root["apPassSet"]  = s.apPass.length() > 0;
  if (includeSecrets) {
    root["staPass"] = s.wifiCount ? s.wifi[0].pass : "";
    root["apPass"]  = s.apPass;
  }

  root["httpTimeout"]       = s.httpTimeout;
  root["brightness"]        = s.brightness;
  root["autoBrightness"]    = s.autoBrightness;
  root["backlightInverted"] = s.backlightInverted;
  root["rotation"]          = s.rotation;

  s.clock.toJson(root["clock"].to<JsonObject>());
  s.display.toJson(root["display"].to<JsonObject>());
  s.wg.toJson(root["wg"].to<JsonObject>(), includeSecrets);
  s.auth.toJson(root["auth"].to<JsonObject>(), includeSecrets);
  s.button.toJson(root["button"].to<JsonObject>());
  s.view.toJson(root["view"].to<JsonObject>(), includeSecrets);
}

void settingsApplyJson(Settings& s, JsonObjectConst root) {
  if (root["hostname"].is<const char*>()) s.hostname = root["hostname"].as<String>();

  if (root["wifi"].is<JsonArrayConst>()) {
    WifiCred old[MAX_WIFI_NETS];
    uint8_t oldCount = s.wifiCount;
    for (uint8_t i = 0; i < oldCount; i++) old[i] = s.wifi[i];

    s.wifiCount = 0;
    for (JsonObjectConst e : root["wifi"].as<JsonArrayConst>()) {
      if (s.wifiCount >= MAX_WIFI_NETS) break;
      const char* ssid = e["ssid"] | "";
      if (!ssid[0]) continue;
      WifiCred& dst = s.wifi[s.wifiCount];
      dst.ssid = ssid;
      const char* pass = e["pass"] | "";
      dst.pass = pass;
      if (!pass[0])
        for (uint8_t i = 0; i < oldCount; i++)
          if (old[i].ssid == dst.ssid) { dst.pass = old[i].pass; break; }
      s.wifiCount++;
    }
  } else if (root["staSsid"].is<const char*>()) {
    String ssid = root["staSsid"].as<String>();
    if (ssid.length()) {
      s.wifi[0].ssid = ssid;
      if (root["staPass"].is<const char*>()) {
        String p = root["staPass"].as<String>();
        if (p.length() > 0) s.wifi[0].pass = p;
      }
      if (s.wifiCount < 1) s.wifiCount = 1;
    }
  }
  if (root["apSsid"].is<const char*>()) s.apSsid = root["apSsid"].as<String>();
  if (root["apPass"].is<const char*>()) s.apPass = root["apPass"].as<String>();

  if (root["httpTimeout"].is<int>())        s.httpTimeout = constrain((int)root["httpTimeout"], 1000, 20000);
  if (root["brightness"].is<int>())         s.brightness = constrain((int)root["brightness"], 0, 100);
  if (root["autoBrightness"].is<bool>())    s.autoBrightness = root["autoBrightness"];
  if (root["backlightInverted"].is<bool>()) s.backlightInverted = root["backlightInverted"];
  if (root["rotation"].is<int>())           s.rotation = (uint8_t)(((int)root["rotation"]) & 3);

  if (root["clock"].is<JsonObjectConst>())   s.clock.fromJson(root["clock"].as<JsonObjectConst>());
  if (root["display"].is<JsonObjectConst>()) s.display.fromJson(root["display"].as<JsonObjectConst>());
  if (root["wg"].is<JsonObjectConst>())      s.wg.fromJson(root["wg"].as<JsonObjectConst>());
  if (root["auth"].is<JsonObjectConst>())    s.auth.fromJson(root["auth"].as<JsonObjectConst>());
  if (root["button"].is<JsonObjectConst>())  s.button.fromJson(root["button"].as<JsonObjectConst>());
  if (root["view"].is<JsonObjectConst>())    s.view.fromJson(root["view"].as<JsonObjectConst>());
}
