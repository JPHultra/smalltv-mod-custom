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
  VIEW_P_USAGE,   // x y w h c provider metric
};

enum ViewAlign : uint8_t { VIEW_A_LEFT, VIEW_A_CENTER, VIEW_A_RIGHT };

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
  uint8_t  primCount;
  bool     needsClock;  // some text has a {token}
  bool     hasUsage;    // at least one usage component
  ViewPrim prims[VIEW_MAX_PRIMS];
  char     text[VIEW_TEXT_POOL];
  uint8_t  bitmap[VIEW_BITMAP_POOL];
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

// Clock key for one-second repaint decisions; 0 until NTP has set the clock.
uint32_t viewClockKey();
