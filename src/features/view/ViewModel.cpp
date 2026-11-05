#include "ViewModel.h"
#include "Clock.h"
#include "ImageAssetFormat.h"
#include <ArduinoJson.h>
#include <pgmspace.h>
#include <string.h>

// ---------------------------------------------------------------------------
// UTF-8 -> font byte. The built-in 6x8 font is ASCII below 0x80 and CP437
// above it, so Latin-1 lands on single bytes. 0 means "no glyph" and the
// character is dropped; 3/4-byte UTF-8 characters never have a glyph.
// ---------------------------------------------------------------------------
static const uint8_t latin1ToFont[96] PROGMEM = {
  0x20, 0xAD, 0x9B, 0x9C, 0x00, 0x9D, 0x00, 0x00,
  0x00, 0x00, 0xA6, 0xAE, 0xAA, 0x00, 0x00, 0x00,
  0xF8, 0xF1, 0xFD, 0x00, 0x00, 0xE6, 0x00, 0xFA,
  0x00, 0x00, 0xA7, 0xAF, 0xAC, 0xAB, 0x00, 0xA8,
  0x00, 0x00, 0x00, 0x00, 0x8E, 0x8F, 0x92, 0x80,
  0x00, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0xA5, 0x00, 0x00, 0x00, 0x00, 0x99, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x9A, 0x00, 0x00, 0xE1,
  0x85, 0xA0, 0x83, 0x00, 0x84, 0x86, 0x91, 0x87,
  0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B,
  0x00, 0xA4, 0x95, 0xA2, 0x93, 0x00, 0x94, 0xF6,
  0x00, 0x97, 0xA3, 0x96, 0x81, 0x00, 0x00, 0x98,
};

static uint8_t utf8FontByte(uint16_t cp) {
  if (cp < 0xA0 || cp > 0xFF) return 0;
  return pgm_read_byte(&latin1ToFont[cp - 0xA0]);
}

static bool parseColor(const char* s, uint16_t& out) {
  if (!s || s[0] != '#' || strlen(s) != 7) return false;
  char* end = nullptr;
  long v = strtol(s + 1, &end, 16);
  if (!end || *end) return false;
  uint8_t r = (uint8_t)((v >> 16) & 0xFF);
  uint8_t g = (uint8_t)((v >> 8) & 0xFF);
  uint8_t b = (uint8_t)(v & 0xFF);
  out = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  return true;
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool addImageName(ViewScreen& sc, const char* name, uint16_t& offset) {
  if (!imageAssetNameValid(name)) return false;
  const size_t length = strlen(name);
  uint8_t count = 0;
  for (uint16_t at = 0; at < sc.imageUsed;) {
    const char* existing = sc.images + at;
    if (!strcmp(existing, name)) { offset = at; return true; }
    at += (uint16_t)strlen(existing) + 1;
    count++;
  }
  if (count >= VIEW_MAX_IMAGES || sc.imageUsed + length + 1 > sizeof(sc.images)) return false;
  offset = sc.imageUsed;
  memcpy(sc.images + sc.imageUsed, name, length);
  sc.imageUsed += (uint16_t)length;
  sc.images[sc.imageUsed++] = 0;
  return true;
}

static uint8_t parseAlign(const char* value, uint8_t fallback) {
  if (!value) return fallback;
  if (value[0] == 'c') return VIEW_A_CENTER;
  if (value[0] == 'r') return VIEW_A_RIGHT;
  if (value[0] == 'l') return VIEW_A_LEFT;
  return fallback;
}

static bool appendText(ViewScreen& sc, const char* text, uint16_t& offset) {
  if (!text || !text[0]) return true;
  const size_t length = strlen(text);
  if (length >= VIEW_USAGE_TEXT_MAX) return false;
  if (sc.textUsed + length + 1 > sizeof(sc.text)) return false;
  offset = sc.textUsed;
  memcpy(sc.text + sc.textUsed, text, length);
  sc.textUsed += (uint16_t)length;
  sc.text[sc.textUsed++] = 0;
  return true;
}

static bool parseUsageStyle(JsonObjectConst s, ViewScreen& sc, UsageStyle& style) {
  style.nameSize = (uint8_t)constrain((int)(s["nameSize"] | 2), 0, 6);
  style.labelSize = (uint8_t)constrain((int)(s["labelSize"] | 0), 0, 6);
  style.valueSize = (uint8_t)constrain((int)(s["valueSize"] | 0), 0, 6);
  style.resetSize = (uint8_t)constrain((int)(s["resetSize"] | 1), 0, 6);
  style.nameAlign = parseAlign(s["nameAlign"] | "l", VIEW_A_LEFT);
  style.labelAlign = parseAlign(s["labelAlign"] | "r", VIEW_A_RIGHT);
  style.valueAlign = parseAlign(s["valueAlign"] | "l", VIEW_A_LEFT);
  style.resetAlign = parseAlign(s["resetAlign"] | "l", VIEW_A_LEFT);
  style.barHeight = (uint8_t)constrain((int)(s["barHeight"] | 8), 1, 32);
  style.radius = (uint8_t)constrain((int)(s["radius"] | 8), 0, 32);
  style.padding = (uint8_t)constrain((int)(s["padding"] | 12), 0, 60);
  style.flags = 0;
  if (s["showName"] | true) style.flags |= USAGE_SHOW_NAME;
  if (s["showDot"] | true) style.flags |= USAGE_SHOW_DOT;
  if (s["showLabel"] | true) style.flags |= USAGE_SHOW_LABEL;
  if (s["showValue"] | true) style.flags |= USAGE_SHOW_VALUE;
  if (s["showBar"] | true) style.flags |= USAGE_SHOW_BAR;
  if (s["showReset"] | true) style.flags |= USAGE_SHOW_RESET;
  if (s["showPanel"] | true) style.flags |= USAGE_SHOW_PANEL;
  if (s["barAuto"] | true) style.flags |= USAGE_BAR_AUTO;
  parseColor(s["panel"] | "", style.panelColor);
  parseColor(s["track"] | "", style.trackColor);
  parseColor(s["valueColor"] | "", style.valueColor);
  parseColor(s["labelColor"] | "", style.labelColor);
  parseColor(s["resetColor"] | "", style.resetColor);
  parseColor(s["nameColor"] | "", style.nameColor);
  parseColor(s["barColor"] | "", style.barColor);
  if (!appendText(sc, s["title"] | "", style.titleOffset)) return false;
  if (s["labels"].is<JsonArrayConst>()) {
    JsonArrayConst labels = s["labels"].as<JsonArrayConst>();
    const uint16_t start = sc.textUsed;
    bool any = false;
    for (int i = 0; i < USAGE_MAX_METRICS; i++) {
      const char* label = labels[i] | "";
      const size_t length = strlen(label);
      if (length >= VIEW_USAGE_TEXT_MAX) return false;
      if (label[0]) any = true;
      if (sc.textUsed + length + 1 > sizeof(sc.text)) return false;
      memcpy(sc.text + sc.textUsed, label, length);
      sc.textUsed += (uint16_t)length;
      sc.text[sc.textUsed++] = 0;
    }
    if (any) style.labelsOffset = start;
    else sc.textUsed = start;
  }
  return true;
}

static bool parsePrim(JsonObjectConst p, ViewScreen& sc, ViewPrim& out) {
  const char* t = p["t"] | "";
  if (!t[0]) return false;

  memset(&out, 0, sizeof(out));
  if (strcmp(t, "image") && !parseColor(p["c"] | "", out.color)) return false;
  out.x  = p["x"] | 0;
  out.y  = p["y"] | 0;
  out.x2 = p["x2"] | 0;
  out.y2 = p["y2"] | 0;

  if (!strcmp(t, "fill")) {
    out.type = VIEW_P_FILL;
  } else if (!strcmp(t, "rect") || !strcmp(t, "rrect")) {
    int w = p["w"] | 0, h = p["h"] | 0;
    if (w <= 0 || h <= 0) return false;
    out.type = (t[1] == 'e') ? VIEW_P_RECT : VIEW_P_RRECT;
    out.x2 = (int16_t)constrain(w, 1, TFT_WIDTH * 2);
    out.y2 = (int16_t)constrain(h, 1, TFT_HEIGHT * 2);
    if (out.type == VIEW_P_RRECT) out.aux = (uint8_t)constrain((int)(p["r"] | 0), 0, 255);
  } else if (!strcmp(t, "circle")) {
    int r = p["r"] | 0;
    if (r <= 0) return false;
    out.type = VIEW_P_CIRCLE;
    out.aux = (uint8_t)constrain(r, 1, 255);
  } else if (!strcmp(t, "line")) {
    out.type = VIEW_P_LINE;
  } else if (!strcmp(t, "text")) {
    const char* v = p["v"];
    if (!v) return false;
    int s = p["s"] | 1;
    if (s < 1) return false;
    char buf[VIEW_MAX_TEXT];
    size_t n = 0;
    for (size_t i = 0; v[i] && n < VIEW_MAX_TEXT - 1; ) {
      uint8_t c = (uint8_t)v[i++];
      if (c < 0x80) {
        if (c >= 32 && c <= 126) buf[n++] = (char)c;
      } else if ((c & 0xE0) == 0xC0 && (v[i] & 0xC0) == 0x80) {
        uint16_t cp = ((uint16_t)(c & 0x1F) << 6) | ((uint8_t)v[i++] & 0x3F);
        uint8_t b = utf8FontByte(cp);
        if (b) buf[n++] = (char)b;
      } else if ((c & 0xF0) == 0xE0 && (v[i] & 0xC0) == 0x80 && (v[i + 1] & 0xC0) == 0x80) {
        i += 2;
      } else if ((c & 0xF8) == 0xF0 && (v[i] & 0xC0) == 0x80 &&
                 (v[i + 1] & 0xC0) == 0x80 && (v[i + 2] & 0xC0) == 0x80) {
        i += 3;
      }
    }
    if (sc.textUsed + n + 1 > sizeof(sc.text)) return false;
    out.type  = VIEW_P_TEXT;
    out.aux   = (uint8_t)constrain(s, 1, 10);
    const char* a = p["a"] | "l";
    out.align = (a[0] == 'c') ? VIEW_A_CENTER : (a[0] == 'r') ? VIEW_A_RIGHT : VIEW_A_LEFT;
    out.voff  = sc.textUsed;
    memcpy(sc.text + sc.textUsed, buf, n);
    sc.textUsed += n;
    sc.text[sc.textUsed++] = 0;
    if (memchr(buf, '{', n)) sc.needsClock = true;
    for (size_t i = 0; i + 3 < n; i++) {
      if (buf[i] == '{' && buf[i + 1] == 'S' && buf[i + 2] == 'S' && buf[i + 3] == '}') {
        sc.needsSeconds = true;
        break;
      }
    }
  } else if (!strcmp(t, "icon")) {
    const char* v = p["v"];
    if (!v) return false;
    int s = p["s"] | 1;
    if (s < 1) return false;
    size_t L = strlen(v);
    if (!L || L > VIEW_MAX_ICON_LEN) return false;
    if (sc.textUsed + L + 1 > sizeof(sc.text)) return false;
    out.type  = VIEW_P_ICON;
    out.aux   = (uint8_t)constrain(s, 1, 8);
    const char* a = p["a"] | "l";
    out.align = (a[0] == 'c') ? VIEW_A_CENTER : (a[0] == 'r') ? VIEW_A_RIGHT : VIEW_A_LEFT;
    out.voff  = sc.textUsed;
    for (size_t i = 0; i < L; i++) {
      char c = v[i];
      if (c >= 32 && c <= 126) sc.text[sc.textUsed++] = c;
    }
    sc.text[sc.textUsed++] = 0;
  } else if (!strcmp(t, "bitmap")) {
    const char* d = p["d"];
    if (!d) return false;
    int w = p["w"] | 0, h = p["h"] | 0;
    if (w < 1 || h < 1 || w > VIEW_BITMAP_MAX_DIM || h > VIEW_BITMAP_MAX_DIM) return false;
    int s = constrain(p["s"] | 1, 1, 4);
    if (w * s > TFT_WIDTH || h * s > TFT_HEIGHT) return false;
    size_t need = ((size_t)w * h + 7) / 8;
    if (strlen(d) != need * 2) return false;
    if (sc.bitmapUsed + need > sizeof(sc.bitmap)) return false;
    for (size_t i = 0; i < need * 2; i++) if (hexVal(d[i]) < 0) return false;
    out.type  = VIEW_P_BITMAP;
    out.x2    = (int16_t)w;
    out.y2    = (int16_t)h;
    out.aux   = (uint8_t)s;
    const char* a = p["a"] | "l";
    out.align = (a[0] == 'c') ? VIEW_A_CENTER : (a[0] == 'r') ? VIEW_A_RIGHT : VIEW_A_LEFT;
    out.voff  = sc.bitmapUsed;
    for (size_t i = 0; i < need; i++)
      sc.bitmap[sc.bitmapUsed + i] = (uint8_t)((hexVal(d[2 * i]) << 4) | hexVal(d[2 * i + 1]));
    sc.bitmapUsed += need;
  } else if (!strcmp(t, "image")) {
    const char* src = p["src"];
    uint16_t offset = 0;
    if (!src || !addImageName(sc, src, offset)) return false;
    int s = constrain(p["s"] | 1, 1, 8);
    out.type  = VIEW_P_IMAGE;
    out.aux   = (uint8_t)s;
    const char* a = p["a"] | "l";
    out.align = (a[0] == 'c') ? VIEW_A_CENTER : (a[0] == 'r') ? VIEW_A_RIGHT : VIEW_A_LEFT;
    out.voff  = offset;
    out.x2    = (int16_t)constrain((int)(p["w"] | 0), 0, TFT_WIDTH);
    out.y2    = (int16_t)constrain((int)(p["h"] | 0), 0, TFT_HEIGHT);
  } else if (!strcmp(t, "usage")) {
    const char* provider = p["provider"];
    if (!provider || !provider[0]) return false;
    size_t L = strlen(provider);
    if (L >= VIEW_MAX_PROVIDER_LEN) return false;
    int w = p["w"] | 0, h = p["h"] | 0;
    if (w <= 0 || h <= 0 || w > TFT_WIDTH || h > TFT_HEIGHT) return false;
    if (sc.usageStyleCount >= VIEW_MAX_USAGE_STYLES) return false;
    UsageStyle style;
    JsonObjectConst styleJson = p["style"].as<JsonObjectConst>();
    if (!styleJson.isNull() && !parseUsageStyle(styleJson, sc, style)) return false;
    const uint8_t styleIndex = sc.usageStyleCount++;
    sc.usageStyles[styleIndex] = style;
    if (sc.textUsed + L + 1 > sizeof(sc.text)) return false;
    out.type  = VIEW_P_USAGE;
    out.x2    = (int16_t)w;
    out.y2    = (int16_t)h;
    // -1 (or an omitted metric) means "all metrics"; the renderer stacks them.
    const int metric = p["metric"] | -1;
    out.aux   = metric < 0 ? 255 : (uint8_t)constrain(metric, 0, USAGE_MAX_METRICS - 1);
    out._pad  = (uint8_t)(styleIndex + 1);
    sc.hasUsage = true;
    out.voff  = sc.textUsed;
    memcpy(sc.text + sc.textUsed, provider, L);
    sc.textUsed += L;
    sc.text[sc.textUsed++] = 0;
  } else {
    return false;
  }
  return true;
}

bool viewScreenParse(const char* json, size_t length, ViewScreen& screen, String& error) {
  screen = ViewScreen();
  if (!json || length == 0 || length > VIEW_MAX_JSON) {
    error = F("empty or oversized view");
    return false;
  }
  JsonDocument doc;
  if (DeserializationError err = deserializeJson(doc, json, length)) {
    error = err.c_str();
    return false;
  }
  if (!doc.is<JsonObject>()) {
    error = F("expected a JSON object");
    return false;
  }
  JsonObjectConst o = doc.as<JsonObjectConst>();

  screen.bg = 0;
  screen.bgImage = 0xFFFF;
  if (o["bg"].is<const char*>()) parseColor(o["bg"].as<const char*>(), screen.bg);
  if (o["bgImage"].is<const char*>()) {
    uint16_t offset = 0;
    if (addImageName(screen, o["bgImage"].as<const char*>(), offset)) screen.bgImage = offset;
  }
  uint32_t ttl = o["ttl"] | 0;
  if (ttl > VIEW_TTL_MAX_SEC) ttl = VIEW_TTL_MAX_SEC;
  screen.ttl = (uint16_t)ttl;
  screen.priority = (uint8_t)constrain((int)(o["priority"] | 0), 0, 3);

  int16_t zOrder[VIEW_MAX_PRIMS] = {};
  if (o["draw"].is<JsonArrayConst>()) {
    for (JsonObjectConst p : o["draw"].as<JsonArrayConst>()) {
      if (screen.primCount >= VIEW_MAX_PRIMS) break;
      if (p["hidden"] | false) continue;
      ViewPrim prim;
      if (parsePrim(p, screen, prim)) {
        const int index = screen.primCount;
        screen.prims[index] = prim;
        zOrder[index] = (int16_t)constrain((int)(p["z"] | index), -32768, 32767);
        screen.primCount++;
      }
    }
  }
  // Stable insertion sort by z; array order breaks ties so old views behave
  // exactly as before when z is omitted.
  for (int i = 1; i < screen.primCount; i++) {
    const ViewPrim key = screen.prims[i];
    const int16_t keyZ = zOrder[i];
    int j = i - 1;
    while (j >= 0 && zOrder[j] > keyZ) {
      screen.prims[j + 1] = screen.prims[j];
      zOrder[j + 1] = zOrder[j];
      j--;
    }
    screen.prims[j + 1] = key;
    zOrder[j + 1] = keyZ;
  }
  error = "";
  return true;
}

ViewScreen& viewStaging() {
  static ViewScreen staging;
  return staging;
}

const char* viewPrimText(const ViewScreen& screen, const ViewPrim& prim) {
  return screen.text + (prim.voff < sizeof(screen.text) ? prim.voff : 0);
}

const char* viewPrimImage(const ViewScreen& screen, const ViewPrim& prim) {
  if (prim.type != VIEW_P_IMAGE || prim.voff >= sizeof(screen.images)) return nullptr;
  return screen.images + prim.voff;
}

const char* viewBackgroundImage(const ViewScreen& screen) {
  if (screen.bgImage == 0xFFFF || screen.bgImage >= sizeof(screen.images)) return nullptr;
  return screen.images + screen.bgImage;
}

const UsageStyle* viewPrimUsageStyle(const ViewScreen& screen, const ViewPrim& prim) {
  if (prim.type != VIEW_P_USAGE || !prim._pad) return nullptr;
  const uint8_t index = (uint8_t)(prim._pad - 1);
  if (index >= screen.usageStyleCount) return nullptr;
  return &screen.usageStyles[index];
}

const char* viewUsageTitle(const ViewScreen& screen, const UsageStyle& style) {
  if (style.titleOffset == 0xFFFF || style.titleOffset >= sizeof(screen.text)) return nullptr;
  return screen.text + style.titleOffset;
}

const char* viewUsageMetricLabel(const ViewScreen& screen, const UsageStyle& style, int metric) {
  if (style.labelsOffset == 0xFFFF || style.labelsOffset >= sizeof(screen.text) ||
      metric < 0 || metric >= USAGE_MAX_METRICS)
    return nullptr;
  const char* p = screen.text + style.labelsOffset;
  for (int i = 0; i < metric; i++) {
    p += strlen(p) + 1;
    if ((size_t)(p - screen.text) >= sizeof(screen.text)) return nullptr;
  }
  return p[0] ? p : nullptr;
}

uint32_t viewClockKey(bool includeSeconds) {
  struct tm t;
  if (!clockNow(t)) return 0;
  const uint32_t now = (uint32_t)time(nullptr);
  return includeSeconds ? now : now / 60;
}
