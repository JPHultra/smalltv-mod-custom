// config.h — compile-time constants for the personal SmallTV dashboard build.
//
// One declarative 240x240 view, a transient webhook overlay, and usage data
// pushed by a local daemon. Hardware variants are the same as upstream; the
// board pin map and panel quirks live in the board headers.
#pragma once
#include "Target.h"

// ---------------------------------------------------------------------------
// Firmware identity
// ---------------------------------------------------------------------------
#define FW_NAME     "smalltv-mod"
#define FW_VERSION  "4.0.5-custom"

#define REPO_URL      "https://github.com/JPHultra/smalltv-mod-custom"
#define REPO_OWNER    "JPHultra"
#define REPO_NAME     "smalltv-mod-custom"
// Release asset the GitHub self-updater pulls, and the short variant name shown
// in the web UI. A device keeps its own variant across a self-update.
//
// Order matters: this is an #if defined() chain, and SMALLTV_ESP32_PRO is
// defined alongside SMALLTV_ESP32 rather than instead of it, so it has to be
// tested first.
#if defined(SMALLTV_ESP32C2)
  #define UPDATE_ASSET "smalltv-mod-firmware-c2.bin"
  #define FW_VARIANT   "c2"
#elif defined(SMALLTV_ESP32_PRO)
  #define UPDATE_ASSET "smalltv-mod-firmware-esp32-pro.bin"
  #define FW_VARIANT   "esp32-pro"
#elif defined(SMALLTV_ESP32)
  #define UPDATE_ASSET "smalltv-mod-firmware-esp32.bin"
  #define FW_VARIANT   "esp32"
#else
  #define UPDATE_ASSET "smalltv-mod-firmware.bin"
  #define FW_VARIANT   "esp8266"
#endif
#define GH_API_HOST   "api.github.com"
#define DAEMON_URL    "https://github.com/JPHultra/smalltv-mod-custom/tree/main/daemon"

// ---------------------------------------------------------------------------
// Display wiring + panel quirks — board-specific.
// ---------------------------------------------------------------------------
#if defined(SMALLTV_ESP32C2)
  #include "board_esp32c2.h"
#elif defined(SMALLTV_ESP32_PRO)
  #include "board_esp32_pro.h"
#elif defined(SMALLTV_ESP32)
  #include "board_esp32.h"
#else
  #include "board_esp8266.h"
#endif

#define TFT_WIDTH  240
#define TFT_HEIGHT 240

#ifndef TFT_COL_OFFSET1
#define TFT_COL_OFFSET1 0
#endif
#ifndef TFT_ROW_OFFSET1
#define TFT_ROW_OFFSET1 0
#endif
#ifndef TFT_COL_OFFSET2
#define TFT_COL_OFFSET2 0
#endif
#ifndef TFT_ROW_OFFSET2
#define TFT_ROW_OFFSET2 80
#endif

// ---------------------------------------------------------------------------
// Limits (bounded RAM on the ESP8266)
// ---------------------------------------------------------------------------
#define MAX_WIFI_NETS     4
#define MAX_AUTH_USER_LEN 32
#define MAX_AUTH_PASS_LEN 64
#define DEFAULT_AUTH_USER "admin"
#define AUTH_REALM        "SmallTV"

// ---------------------------------------------------------------------------
// The dashboard draw list and the transient overlay.
//
// ESP32 boards get more primitives and larger text/bitmap pools. The ESP8266
// keeps the same shape but with the smaller limits that fit beside WiFi and
// the web server.
// ---------------------------------------------------------------------------
#ifndef WITH_VIEW
#define WITH_VIEW 1
#endif
#ifndef WITH_USAGE
#define WITH_USAGE 1
#endif

#define VIEW_MAX_JSON        4096
#define VIEW_MAX_TEXT          96
#define VIEW_MAX_ICON_LEN      24
#define VIEW_MAX_PROVIDER_LEN  16
#define VIEW_TTL_MAX_SEC   604800UL   // payload clamp; the overlay uses less
#define VIEW_OVERLAY_TTL_MIN_SEC   2
#define VIEW_OVERLAY_TTL_MAX_SEC 120
#define VIEW_DEFAULT_TTL_SEC      15
#define VIEW_POLL_MIN_SEC          5
#define VIEW_POLL_MAX_SEC       3600
#define VIEW_DEFAULT_POLL_SEC      0

#if defined(SMALLTV_FAMILY_ESP32)
  #define VIEW_MAX_PRIMS      48
  #define VIEW_TEXT_POOL    2048
  #define VIEW_BITMAP_POOL  2048
  #define VIEW_BITMAP_MAX_DIM 48
#else
  #define VIEW_MAX_PRIMS      24
  #define VIEW_TEXT_POOL     768
  #define VIEW_BITMAP_POOL   512
  #define VIEW_BITMAP_MAX_DIM 32
#endif

// Normalized usage store. The daemon pushes at most three providers; a provider
// may have up to five metrics (Codex windows, plan, credits, extras).
#define USAGE_MAX_PROVIDERS 3
#define USAGE_MAX_METRICS   5
#define USAGE_MAX_ID       16
#define USAGE_MAX_NAME     20
#define USAGE_MAX_LABEL    12
#define USAGE_VALUE_MAX    20   // a display value such as "110.00 CNY" or "1.2M tokens"
#define USAGE_MAX_STATUS   16
#define USAGE_STALE_MS 120000UL

// The editor token lets the static GitHub Pages editor push a view to a device
// on the LAN. It is a write token, not a read-everything credential.
#define MAX_EDITOR_TOKEN_LEN 48
#define DEFAULT_EDITOR_TOKEN ""
#define MAX_POLL_AUTH_HEADER_LEN 32
#define MAX_POLL_AUTH_VALUE_LEN  96

// ---------------------------------------------------------------------------
// Defaults (used on first boot / factory reset)
// ---------------------------------------------------------------------------
#define DEFAULT_AP_SSID      "SmallTV-Setup"
#define DEFAULT_AP_PASS      ""
#define DEFAULT_HOSTNAME     "smalltv"
#define DEFAULT_BRIGHTNESS    90
#define DEFAULT_HTTP_TIMEOUT  8000

// --- Panel colour correction (device-wide) ---
#define COLOR_ORDER_AUTO   0
#define COLOR_ORDER_RGB    1
#define COLOR_ORDER_BGR    2
#define DEFAULT_COLOR_ORDER  COLOR_ORDER_AUTO
#define DEFAULT_COLOR_INVERT false
#define DEFAULT_COLOR_GAIN   100
#define MIN_COLOR_GAIN        50
#define MAX_COLOR_GAIN       150

// --- Clock / night mode (device-wide) ---
#define NTP_SERVER1             "pool.ntp.org"
#define NTP_SERVER2             "time.nist.gov"
#define DEFAULT_TZ_NAME         ""
#define DEFAULT_TZ_POSIX        "UTC0"
#define DEFAULT_NIGHT_ENABLED   false
#define DEFAULT_NIGHT_START_MIN 1320
#define DEFAULT_NIGHT_END_MIN   420
#define DEFAULT_NIGHT_LEVEL     0

#define NIGHT_NTP_TRUST_MS      300000UL
#define NIGHT_NTP_RESYNC_MS      30000UL

// ---------------------------------------------------------------------------
// Physical button (boards that define BTN_PIN). Short and long press each run
// a configurable action; sensitivity is adjustable in the web UI.
// ---------------------------------------------------------------------------
#define BTN_ACT_NONE       0
#define BTN_ACT_SCREEN_OFF 1
#define BTN_ACT_HOME       2   // repaint the dashboard
#define BTN_ACT_NEXT       3   // dismiss the overlay
#define BTN_ACT_NETINFO    4
#define BTN_ACT_WAKE       5
#define BTN_ACT_ACK        6   // dismiss the overlay
#define BTN_ACT_MAX        BTN_ACT_ACK

#define BTN_LONG_MS        600
#define BTN_INFO_MS       4000
#define BTN_WAKE_MS       5000
#define BTN_CONFIRM          3

#ifdef BTN_TOUCH_RATIO
#define DEFAULT_BTN_SENS   BTN_TOUCH_RATIO
#else
#define DEFAULT_BTN_SENS   85
#endif
#define DEFAULT_BTN_SHORT  BTN_ACT_NEXT
#define DEFAULT_BTN_LONG   BTN_ACT_NETINFO
