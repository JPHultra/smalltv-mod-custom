// ViewModel.h — the declarative 240x240 draw list used by the dashboard and the
// transient webhook overlay.
//
// A view is data, never code. The device parses it into fixed-size structs so a
// running dashboard costs one bounded block of RAM and no heap at frame time.
#pragma once
#include <Arduino.h>
#include "config.h"

enum ViewPrimType : uint8_t {
  VIEW_P_FILL,    // c
  VIEW_P_RECT,    // x y w h c
  VIEW_P_RRECT,   // x y w h r c
  VIEW_P_CIRCLE,  // x y r c
  VIEW_P_LINE,    // x y x2 y2 c
  VIEW_P_TEXT,    // x y s c a v
  VIEW_P_ICON,    // x y s c a v
  VIEW_P_BITMAP,  // x y w h s c a d
  VIEW_P_IMAGE,   // x y s a src (asset-backed)
  VIEW_P_USAGE,   // x y w h c provider metric
};

enum ViewAlign : uint8_t { VIEW_A_LEFT, VIEW_A_CENTER, VIEW_A_RIGHT };

// Configurable layout for a usage card. Values are kept compact; text and
// labels live in the screen text pool by offset.
enum UsageStyleFlags : uint8_t {
  USAGE_SHOW_NAME  = 0x01,
  USAGE_SHOW_DOT   = 0x02,
  USAGE_SHOW_LABEL = 0x04,
  USAGE_SHOW_VALUE = 0x08,
  USAGE_SHOW_BAR   = 0x10,
  USAGE_SHOW_RESET = 0x20,
  USAGE_SHOW_PANEL = 0x40,
  USAGE_BAR_AUTO   = 0x80,
};

struct UsageStyle {
  uint8_t nameSize = 2;       // 0 = auto
  uint8_t labelSize = 0;      // 0 = auto
  uint8_t valueSize = 0;      // 0 = auto
  uint8_t resetSize = 1;
  uint8_t nameAlign = VIEW_A_LEFT;
  uint8_t labelAlign = 0xFF;  // layout default: single right, rows left
  uint8_t valueAlign = 0xFF;  // layout default: single left, rows right
  uint8_t resetAlign = VIEW_A_LEFT;
  uint8_t flags = 0xFF;
  uint8_t barHeight = 8;
  uint8_t radius = 8;
  uint8_t padding = 12;
  uint16_t panelColor = 0x18E3;
  uint16_t trackColor = 0x2945;
  uint16_t valueColor = 0xFFFF;
  uint16_t labelColor = 0xB574;
  uint16_t resetColor = 0xB574;
  uint16_t nameColor = 0;     // 0 = use the component accent colour
  uint16_t barColor = 0;
  uint16_t titleOffset = 0xFFFF;
  uint16_t labelsOffset = 0xFFFF;
};

struct TextStyle {
  int8_t marginX = 0;
  int8_t marginY = 0;
  uint8_t padX = 0;
  uint8_t padY = 0;
  uint8_t lineGap = 0;
  uint8_t valign = 1;  // 0 top, 1 middle, 2 bottom
};

// Absolute geometry for configurable usage-card elements. x/y/w/h are relative
// to the card origin; margins and padding are applied inside that box.
struct UsageBox {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
  int8_t marginX = 0;
  int8_t marginY = 0;
  uint8_t padX = 0;
  uint8_t padY = 0;
  uint8_t align = 0;
  uint8_t valign = 1;
  uint8_t flags = 0;   // 1 = position set, 2 = size set
  uint8_t reserved = 0;
};

struct ViewBitmapStyle {
  uint16_t displayW = 0;
  uint16_t displayH = 0;
};

struct UsageRowLayout {
  UsageBox label;
  UsageBox value;
  UsageBox bar;
};

struct UsageLayout {
  uint8_t configured = 0;
  uint8_t reserved = 0;
  UsageBox name;
  UsageBox reset;
  UsageRowLayout rows[USAGE_MAX_METRICS];
};

// One draw primitive, 16 bytes. Text, icon names, provider ids and decoded
// bitmap data live in the owning screen's pools; voff indexes the matching one.
struct ViewPrim {
  uint8_t  type;        // ViewPrimType
  uint8_t  aux;         // text/icon scale, radius, bitmap scale, usage metric
  uint8_t  align;       // text/icon/bitmap anchor
  uint8_t  _pad;
  int16_t  x, y;        // rect: top-left; circle: centre; text: anchor
  int16_t  x2, y2;      // line: end point; rect/rrect/bitmap/usage: w/h
  uint16_t color;       // RGB565
  uint16_t voff;        // text/icon/provider: offset into text[]; bitmap: into bitmap[]
};
static_assert(sizeof(ViewPrim) == 16, "ViewPrim must stay compact");

struct ViewScreen {
  uint16_t bg;          // background RGB565
  uint16_t ttl;         // overlay hold, seconds (dashboard ignores it)
  uint16_t textUsed;
  uint16_t bitmapUsed;
  uint16_t imageUsed;
  uint16_t bgImage;     // image pool offset, 0xFFFF when unset
  uint8_t  primCount;
  bool     needsClock;  // some text has a {token}
  bool     needsSeconds; // some text has {SS}, so the clock key is per-second
  bool     hasUsage;    // at least one usage component
  uint8_t  priority;    // overlay priority, 0..3
  uint8_t  usageStyleCount;
  UsageStyle usageStyles[VIEW_MAX_USAGE_STYLES];
  UsageLayout usageLayouts[VIEW_MAX_USAGE_STYLES];
  uint8_t  textStyleCount;
  TextStyle textStyles[VIEW_MAX_TEXT_STYLES];
  uint8_t  bitmapStyleCount;
  ViewBitmapStyle bitmapStyles[VIEW_MAX_BITMAP_STYLES];
  ViewPrim prims[VIEW_MAX_PRIMS];
  char     text[VIEW_TEXT_POOL];
  uint8_t  bitmap[VIEW_BITMAP_POOL];
  char     images[VIEW_IMAGE_POOL];
};

// Parses a JSON view into `screen`. Returns false only for invalid JSON or a
// non-object root; individual malformed primitives are skipped. `error` names
// the reason on failure. The screen is fully cleared first.
bool viewScreenParse(const char* json, size_t length, ViewScreen& screen, String& error);

// Shared parse target. The dashboard and overlay never parse at the same time,
// so one bounded staging screen keeps the ESP8266's static RAM down.
ViewScreen& viewStaging();

// NUL-terminated text or provider id referenced by `prim`.
const char* viewPrimText(const ViewScreen& screen, const ViewPrim& prim);

// Asset name referenced by an image primitive/background, or nullptr.
const char* viewPrimImage(const ViewScreen& screen, const ViewPrim& prim);
const char* viewBackgroundImage(const ViewScreen& screen);

// Usage style/text accessors. Return defaults when a component has no style.
const UsageStyle* viewPrimUsageStyle(const ViewScreen& screen, const ViewPrim& prim);
const UsageLayout* viewPrimUsageLayout(const ViewScreen& screen, const ViewPrim& prim);
const TextStyle* viewPrimTextStyle(const ViewScreen& screen, const ViewPrim& prim);
const ViewBitmapStyle* viewPrimBitmapStyle(const ViewScreen& screen, const ViewPrim& prim);
const char* viewUsageTitle(const ViewScreen& screen, const UsageStyle& style);
const char* viewUsageMetricLabel(const ViewScreen& screen, const UsageStyle& style, int metric);

// Clock key for repaint decisions; per-minute unless includeSeconds is true.
// Returns 0 until NTP has set the clock.
uint32_t viewClockKey(bool includeSeconds);
