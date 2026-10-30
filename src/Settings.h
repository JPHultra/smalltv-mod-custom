// Settings.h — persisted configuration (LittleFS /config.json)
//
// The dashboard keeps the shared device/network fields at the top level and a
// small `view` slice for the webhook overlay and editor token. Feature slices
// from upstream (ticker, radar, HA, themes, carousel) are gone; an imported old
// config still restores the WiFi list and device settings, and the fields it
// does not recognize are ignored.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"

// One saved WiFi station network. The device keeps up to MAX_WIFI_NETS and
// joins the strongest visible one at boot (hidden SSIDs are tried last).
struct WifiCred {
  String ssid;
  String pass;
};

// ---- Clock / night mode slice (device-wide) --------------------------------
struct ClockSettings {
  String   tz;
  String   tzPosix;
  bool     nightEnabled;
  uint16_t nightStartMin;
  uint16_t nightEndMin;
  uint8_t  nightLevel;

  void setDefaults();
  void toJson(JsonObject o) const;
  void fromJson(JsonObjectConst o);
};

// ---- Web UI password slice (device-wide) -----------------------------------
struct AuthSettings {
  bool   enabled;
  String user;
  String pass;

  void setDefaults();
  void toJson(JsonObject o, bool includeSecrets) const;
  void fromJson(JsonObjectConst o);
};

// ---- WireGuard slice (device-wide) -----------------------------------------
struct WgSettings {
  bool     enabled;
  String   privateKey;
  String   peerPublicKey;
  String   endpointHost;
  uint16_t endpointPort;
  String   address;
  String   allowedIps;
  uint16_t keepalive;

  void setDefaults();
  void toJson(JsonObject o, bool includeSecrets) const;
  void fromJson(JsonObjectConst o);
};

// ---- Panel colour slice (device-wide) --------------------------------------
struct DisplaySettings {
  uint8_t colorOrder;
  bool    invert;
  uint8_t rGain;
  uint8_t gGain;
  uint8_t bGain;

  void setDefaults();
  void toJson(JsonObject o) const;
  void fromJson(JsonObjectConst o);
};

// ---- Physical button slice (device-wide) -----------------------------------
struct ButtonSettings {
  uint8_t sensitivity;
  uint8_t shortAction;
  uint8_t longAction;

  void setDefaults();
  void toJson(JsonObject o) const;
  void fromJson(JsonObjectConst o);
};

// ---- Dashboard / overlay slice ---------------------------------------------
struct ViewSettings {
  String   pollUrl;       // optional GET endpoint for the transient overlay
  uint16_t pollSec;       // 0 = polling off
  uint16_t ttlSec;        // default overlay hold when the payload has no ttl
  String   editorToken;   // cross-origin editor token; never returned by the API

  void setDefaults();
  void toJson(JsonObject o, bool includeSecrets) const;
  void fromJson(JsonObjectConst o);
};

// ---- Top-level settings ----------------------------------------------------
struct Settings {
  WifiCred wifi[MAX_WIFI_NETS];
  uint8_t  wifiCount;

  String apSsid;
  String apPass;
  String hostname;

  uint16_t httpTimeout;
  uint8_t  brightness;
  bool     autoBrightness;
  bool     backlightInverted;
  uint8_t  rotation;

  ClockSettings   clock;
  DisplaySettings display;
  WgSettings      wg;
  AuthSettings    auth;
  ButtonSettings  button;
  ViewSettings    view;

  void setDefaults();
};

// Persistence
bool   settingsBegin();
bool   loadSettings(Settings& s);
bool   saveSettings(const Settings& s);
void   factoryReset(Settings& s);
size_t settingsStoredSize();

void settingsToJson(const Settings& s, JsonObject root, bool includeSecrets);
void settingsApplyJson(Settings& s, JsonObjectConst root);
