#include "WebPortal.h"
#include "Platform.h"
#include "WebGuard.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "webui.h"
#include "Net.h"
#include "Gfx.h"
#include "OtaUpdate.h"
#include "Clock.h"
#include "WgClient.h"
#include "SettingsTransaction.h"
#include "features/view/ViewMode.h"
#include "features/view/ViewOverlay.h"
#include "features/view/ViewPoll.h"
#include "features/usage/UsageClient.h"

extern void appInvalidate();
extern void appRelease();
extern const char* appResetReason();
extern void appApplyBrightness();

static WebServerClass server(80);
static Settings*        S = nullptr;
static bool             g_reboot = false;
static uint32_t         g_rebootAt = 0;
static bool             g_selfUpdate = false;
static String           g_updateMsg;

static const char* EDITOR_URL = "https://jphultra.github.io/smalltv-mod-custom/";

static void scheduleReboot(uint32_t inMs) {
  g_reboot = true;
  g_rebootAt = millis() + inMs;
}

// ---- CSRF / DNS-rebinding guard, with one editor exception -----------------
// The static editor is served from GitHub Pages, so its Origin is not the
// device. A request to /api/dashboard or /api/view carrying the configured
// editor token is allowed; everything else keeps the same-origin rule. The
// token is in the query string because that avoids a CORS preflight.
static bool isEditorPath() {
  String p = server.uri();
  return p == "/api/dashboard" || p == "/api/view" || p == "/api/view/clear" || p == "/api/usage";
}

static bool editorTokenOk() {
  if (!S || S->view.editorToken.length() == 0) return false;
  if (!server.hasArg("token")) return false;
  return server.arg("token") == S->view.editorToken;
}

static const char* guardReject() {
  String host = server.hostHeader();
  if (netMode() != NET_AP) {
    const char* name = S->hostname.length() ? S->hostname.c_str() : DEFAULT_HOSTNAME;
    if (!webguard::hostAllowed(host.c_str(), name)) return "Forbidden: unexpected Host header";
  }
  HTTPMethod m = server.method();
  if (m != HTTP_GET && m != HTTP_HEAD && m != HTTP_OPTIONS) {
    if (!(isEditorPath() && editorTokenOk())) {
      String origin = server.header("Origin");
      String site   = server.header("Sec-Fetch-Site");
      if (!webguard::originAllowed(origin.c_str(), site.c_str(), host.c_str()))
        return "Forbidden: cross-site request";
    }
  }
  return nullptr;
}

static bool guardAllowed() { return guardReject() == nullptr; }

static bool guardRequest() {
  const char* why = guardReject();
  if (!why) return true;
  server.send(403, "text/plain", why);
  return false;
}

static bool requireAuth() {
  if (!guardRequest()) return false;
  if (!S->auth.enabled || !S->auth.pass.length()) return true;
  if (server.authenticate(S->auth.user.c_str(), S->auth.pass.c_str())) return true;
  server.requestAuthentication(HTTPAuthMethod::DIGEST_AUTH, AUTH_REALM,
                               "Authentication required");
  return false;
}

// Editor token first, then the normal web-UI password. A same-origin browser
// form has no token, so it still goes through digest auth.
static bool requireEditor() {
  if (editorTokenOk()) return guardRequest();
  return requireAuth();
}

static void sendCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
}

static void sendJson(JsonDocument& doc, int code = 200) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

static void sendError(int code, const char* message) {
  JsonDocument doc;
  doc["error"] = message;
  sendJson(doc, code);
}

static void handleOptions() {
  sendCors();
  server.send(204, "text/plain", "");
}

static void handleRoot() {
  if (!requireAuth()) return;
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html", (PGM_P)WEBUI_HTML_GZ, WEBUI_HTML_GZ_LEN);
}

static void handleGetConfig() {
  if (!requireAuth()) return;
  appRelease();
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  settingsToJson(*S, root, /*includeSecrets=*/false);

  JsonObject feat = root["features"].to<JsonObject>();
  feat["view"]   = (bool)WITH_VIEW;
  feat["usage"]  = (bool)WITH_USAGE;
#if defined(SMALLTV_WIREGUARD)
  feat["wireguard"] = true;
#else
  feat["wireguard"] = false;
#endif
#ifdef BTN_PIN
  feat["button"] = true;
#else
  feat["button"] = false;
#endif

#if defined(SMALLTV_ESP32C2)
  root["chip"] = "esp32c2";
#elif defined(SMALLTV_ESP32)
  root["chip"] = "esp32";
#else
  root["chip"] = "esp8266";
#endif
  root["editorUrl"] = EDITOR_URL;
  root["daemonUrl"] = DAEMON_URL;
  sendJson(doc);
}

static void handleStatus() {
  if (!requireAuth()) return;
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  o["fw"] = FW_NAME;
  o["version"] = FW_VERSION;
  o["variant"] = FW_VARIANT;
  o["repo"] = REPO_URL;
  if (g_updateMsg.length()) o["updateMsg"] = g_updateMsg;
  o["mode"] = (netMode() == NET_AP) ? "ap" : "sta";
  o["connected"] = netConnected();
  o["ssid"] = netSSID();
  o["ip"] = netIP();
  o["rssi"] = netRSSI();
  if (netFailReason()[0]) o["wifiFail"] = netFailReason();
  o["heap"] = ESP.getFreeHeap();
  o["maxblk"] = platformMaxFreeBlock();
  o["contstk"] = platformFreeContStack();
  o["uptime"] = millis() / 1000;
  o["reset"] = appResetReason();
  o["synced"] = clockSynced();
  { String ts = clockTimeStr(); if (ts.length()) o["time"] = ts; }
  o["tz"] = S->clock.tz;
  o["night"] = clockNightActive();
  o["nightHeld"] = clockNightHeld();
  o["clockFresh"] = clockTrusted();
  wgStatusJson(o["wg"].to<JsonObject>());

  JsonObject u = o["usage"].to<JsonObject>();
  usageStatusJson(u);

  JsonObject v = o["view"].to<JsonObject>();
  v["loaded"] = g_viewMode.hasScreen();
  v["error"] = g_viewMode.error();
  v["overlay"] = g_viewOverlay.active();
  v["poll"] = S->view.pollSec > 0 && S->view.pollUrl.length() >= 8;
  if (viewPollError().length()) v["pollError"] = viewPollError();
  v["editorUrl"] = EDITOR_URL;

  sendJson(doc);
}

static String netFingerprint(const Settings& s) {
  String f((int)s.wifiCount);
  for (uint8_t i = 0; i < s.wifiCount; i++) {
    f += '\n';
    f += s.wifi[i].ssid;
    f += '\x01';
    f += s.wifi[i].pass;
  }
  f += '\n';
  f += s.hostname;
  return f;
}

static String wgFingerprint(const Settings& s) {
  String f(s.wg.enabled ? '1' : '0');
  f += s.wg.privateKey;    f += '\x01';
  f += s.wg.peerPublicKey; f += '\x01';
  f += s.wg.endpointHost;  f += '\x01';
  f += String(s.wg.endpointPort); f += '\x01';
  f += s.wg.address;       f += '\x01';
  f += s.wg.allowedIps;    f += '\x01';
  f += String(s.wg.keepalive);
  return f;
}

static void sendParseError(DeserializationError err) {
  if (err == DeserializationError::NoMemory)
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"not enough memory\"}");
  else
    server.send(400, "application/json", String("{\"ok\":false,\"error\":\"bad json (") + err.c_str() + ")\"}");
}

static void handlePostConfig() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }

  appRelease();
  JsonDocument doc;
  if (DeserializationError err = deserializeJson(doc, server.arg("plain"))) {
    sendParseError(err);
    return;
  }

  String oldNet = netFingerprint(*S);
  String oldWg = wgFingerprint(*S);

  if (!applyAndSaveSettings(*S,
        [&](Settings& settings) { settingsApplyJson(settings, doc.as<JsonObjectConst>()); doc.clear(); },
        [](const Settings& settings) { return saveSettings(settings); })) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"could not save settings\"}");
    return;
  }

  clockReapply(*S);
  appApplyBrightness();
  gfxApplyColors(*S);
  appInvalidate();

  bool wgChanged = (wgFingerprint(*S) != oldWg) || wgHeld();
  bool wifiChanged = netFingerprint(*S) != oldNet;

  JsonDocument res;
  res["ok"] = true;
  res["reboot"] = wifiChanged;
  sendJson(res);

  if (wgChanged) wgReapply(*S);
  if (wifiChanged) scheduleReboot(800);
}

static void handleScan() {
  if (!requireAuth()) return;
  int n = WiFi.scanNetworks();
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n && i < 25; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["enc"] = !platformScanIsOpen(i);
  }
  WiFi.scanDelete();
  sendJson(doc);
}

static void handleReboot() {
  if (!requireAuth()) return;
  server.send(200, "application/json", "{\"ok\":true}");
  scheduleReboot(400);
}

static void handleFactory() {
  if (!requireAuth()) return;
  factoryReset(*S);
  saveSettings(*S);
  server.send(200, "application/json", "{\"ok\":true}");
  scheduleReboot(400);
}

static void handleExport() {
  if (!requireAuth()) return;
  File f = LittleFS.open("/config.json", "r");
  if (!f) { server.send(404, "text/plain", "no config saved yet"); return; }
  server.sendHeader("Content-Disposition", "attachment; filename=smalltv-config.json");
  server.streamFile(f, "application/json");
  f.close();
}

static void handleImport() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
  appRelease();
  JsonDocument doc;
  if (DeserializationError err = deserializeJson(doc, server.arg("plain"))) {
    sendParseError(err);
    return;
  }
  if (!applyAndSaveSettings(*S,
        [&](Settings& settings) { settingsApplyJson(settings, doc.as<JsonObjectConst>()); doc.clear(); },
        [](const Settings& settings) { return saveSettings(settings); })) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"could not save settings\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  scheduleReboot(800);
}

static void handleCheckUpdate() {
  if (!requireAuth()) return;
  OtaLatest r = otaCheckLatest(*S);
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  o["current"] = FW_VERSION;
  o["ok"] = r.ok;
  o["latest"] = r.tag;
  o["newer"] = r.newer;
  if (!r.ok) o["error"] = r.error;
  sendJson(doc);
}

static void handleSelfUpdate() {
  if (!requireAuth()) return;
  g_selfUpdate = true;
  g_updateMsg = "starting...";
  server.send(200, "application/json", "{\"ok\":true}");
}

// ---- usage push (daemon, no web-UI password) -------------------------------
static void handleUsagePush() {
  if (!guardRequest()) return;
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
  const String body = server.arg("plain");
  const bool ok = body.length() <= 4096 && usageApply(body);
  server.send(ok ? 200 : 400, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handleUsageGet() {
  if (!requireEditor()) return;
  sendCors();
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  usageStatusJson(root);
  sendJson(doc);
}

// ---- dashboard -------------------------------------------------------------
static void handleDashboardGet() {
  if (!requireEditor()) return;
  sendCors();
  File f = LittleFS.open("/view.json", "r");
  if (!f) { sendError(404, "No dashboard installed"); return; }
  server.streamFile(f, "application/json");
  f.close();
}

static void handleDashboardPost() {
  if (!requireEditor()) return;
  sendCors();
  if (!server.hasArg("plain")) { sendError(400, "no body"); return; }
  const String body = server.arg("plain");
  if (body.length() == 0 || body.length() > VIEW_MAX_JSON) {
    sendError(400, "view is empty or too large");
    return;
  }
  String error;
  if (!g_viewMode.replace(body, error)) {
    sendError(400, error.length() ? error.c_str() : "invalid view");
    return;
  }
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(doc);
}

// ---- transient overlay -----------------------------------------------------
static void handleViewPost() {
  if (!requireEditor()) return;
  sendCors();
  if (!server.hasArg("plain")) { sendError(400, "no body"); return; }
  const String body = server.arg("plain");
  if (body.length() == 0 || body.length() > VIEW_MAX_JSON) {
    sendError(400, "view is empty or too large");
    return;
  }
  String error;
  if (!g_viewOverlay.show(body, S->view.ttlSec, error)) {
    sendError(400, error.length() ? error.c_str() : "invalid view");
    return;
  }
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(doc);
}

static void handleViewClear() {
  if (!requireEditor()) return;
  sendCors();
  g_viewOverlay.dismiss();
  server.send(200, "application/json", "{\"ok\":true}");
}

// ---- OTA ------------------------------------------------------------------
static void handleUpdateDone() {
  if (!requireAuth()) return;
  bool ok = !Update.hasError();
  server.sendHeader("Connection", "close");
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : platformUpdateError().c_str());
  if (ok) scheduleReboot(1200);
}

static void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (!guardAllowed()) {
    if (up.status != UPLOAD_FILE_START) Update.end();
    return;
  }
  if (S->auth.enabled && S->auth.pass.length() &&
      !server.authenticate(S->auth.user.c_str(), S->auth.pass.c_str())) {
    if (up.status != UPLOAD_FILE_START) Update.end();
    return;
  }
  if (up.status == UPLOAD_FILE_START) {
#if defined(SMALLTV_FAMILY_ESP8266)
    WiFiUDP::stopAll();
#endif
    uint32_t maxSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(maxSpace)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.end();
  }
  yield();
}

// ---- captive portal -------------------------------------------------------
static void handleNotFound() {
  if (netMode() == NET_AP) {
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

// ---------------------------------------------------------------------------
void webPortalBegin(Settings& settings) {
  S = &settings;
  g_updateMsg = otaTakeBootResult();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handlePostConfig);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/factory", HTTP_POST, handleFactory);
  server.on("/api/export", HTTP_GET, handleExport);
  server.on("/api/import", HTTP_POST, handleImport);
  server.on("/api/checkupdate", HTTP_GET, handleCheckUpdate);
  server.on("/api/selfupdate", HTTP_POST, handleSelfUpdate);

  server.on("/api/usage", HTTP_GET, handleUsageGet);
  server.on("/api/usage", HTTP_POST, handleUsagePush);
  server.on("/api/usage", HTTP_OPTIONS, handleOptions);

  server.on("/api/dashboard", HTTP_GET, handleDashboardGet);
  server.on("/api/dashboard", HTTP_POST, handleDashboardPost);
  server.on("/api/dashboard", HTTP_OPTIONS, handleOptions);

  server.on("/api/view", HTTP_POST, handleViewPost);
  server.on("/api/view/clear", HTTP_POST, handleViewClear);
  server.on("/api/view", HTTP_OPTIONS, handleOptions);
  server.on("/api/view/clear", HTTP_OPTIONS, handleOptions);

  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);

  server.on("/generate_204", handleNotFound);
  server.on("/gen_204", handleNotFound);
  server.on("/hotspot-detect.html", handleNotFound);
  server.on("/connecttest.txt", handleNotFound);
  server.onNotFound(handleNotFound);

#if defined(SMALLTV_FAMILY_ESP8266)
  server.collectHeaders("Origin", "Sec-Fetch-Site");
#else
  static const char* kGuardHeaders[] = {"Origin", "Sec-Fetch-Site"};
  server.collectHeaders(kGuardHeaders, 2);
#endif

  server.begin();
}

void webPortalLoop() {
  server.handleClient();

  if (g_selfUpdate) {
    g_selfUpdate = false;
#if defined(SMALLTV_FAMILY_ESP8266)
    OtaLatest r = otaCheckLatest(*S);
    if (!r.ok)         g_updateMsg = "check failed: " + r.error;
    else if (!r.newer) g_updateMsg = "already up to date (" FW_VERSION ")";
    else if (otaRequestBootUpdate(r.tag.c_str())) {
      g_updateMsg = "updating...";
      scheduleReboot(400);
    } else {
      g_updateMsg = F("could not queue update (storage error)");
    }
#else
    String err = otaUpdateFromGitHub(*S);
    g_updateMsg = err.length() ? err : "updating...";
#endif
  }
}

bool webPortalRebootDue() {
  return g_reboot && (int32_t)(millis() - g_rebootAt) >= 0;
}
