#include "ViewRender.h"
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "Clock.h"
#include "Gfx.h"
#include "UsageClient.h"
#include "ViewIcons.h"

// ---------------------------------------------------------------------------
// Text tokens. A text primitive may contain {HH}, {MM}, ...; the same tokens
// the old theme engine used. Unknown braces are left alone so literal braces
// still work. Until NTP sets the clock, known tokens render as "--".
// ---------------------------------------------------------------------------
static const char* const kMonths[] = {
  "January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December"
};
static const char* const kDays[] = {
  "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

static void append(char* out, size_t cap, size_t& used, const char* text) {
  while (text && *text && used + 1 < cap) out[used++] = *text++;
}

static void expandText(const char* in, char* out, size_t cap) {
  struct tm t;
  const bool have = clockNow(t);
  size_t used = 0;
  for (size_t i = 0; in[i] && used + 1 < cap; ) {
    if (in[i] != '{') { out[used++] = in[i++]; continue; }
    const char* end = strchr(in + i, '}');
    if (!end) { out[used++] = in[i++]; continue; }
    const size_t len = (size_t)(end - (in + i + 1));
    char token[20];
    if (len >= sizeof(token)) { out[used++] = in[i++]; continue; }
    memcpy(token, in + i + 1, len);
    token[len] = 0;

    char value[24];
    const char* text = nullptr;
    int n = -1;
    if (!strcmp(token, "HH")) n = have ? t.tm_hour : -1;
    else if (!strcmp(token, "hh")) n = have ? (t.tm_hour % 12 ? t.tm_hour % 12 : 12) : -1;
    else if (!strcmp(token, "MM")) n = have ? t.tm_min : -1;
    else if (!strcmp(token, "SS")) n = have ? t.tm_sec : -1;
    else if (!strcmp(token, "DD")) n = have ? t.tm_mday : -1;
    else if (!strcmp(token, "YYYY")) n = have ? t.tm_year + 1900 : -1;
    if (n >= 0) {
      snprintf(value, sizeof(value), strcmp(token, "YYYY") ? "%02d" : "%04d", n);
      text = value;
    } else if (!strcmp(token, "MON") || !strcmp(token, "MONTH")) {
      if (have && t.tm_mon >= 0 && t.tm_mon < 12) {
        if (token[3] == 0) { strlcpy(value, kMonths[t.tm_mon], 4); text = value; }
        else text = kMonths[t.tm_mon];
      } else text = "--";
    } else if (!strcmp(token, "WD") || !strcmp(token, "WEEKDAY")) {
      if (have && t.tm_wday >= 0 && t.tm_wday < 7) {
        if (token[2] == 0) { strlcpy(value, kDays[t.tm_wday], 4); text = value; }
        else text = kDays[t.tm_wday];
      } else text = "--";
    } else if (!strcmp(token, "HH") || !strcmp(token, "hh") || !strcmp(token, "MM") ||
               !strcmp(token, "SS") || !strcmp(token, "DD") || !strcmp(token, "YYYY")) {
      text = "--";
    }

    if (!text) { out[used++] = in[i++]; continue; }   // unknown token: literal
    append(out, cap, used, text);
    i = (size_t)(end - in) + 1;
  }
  out[used] = 0;
}

static int textWidth(const char* text, uint8_t size) {
  return (int)strlen(text) * 6 * (size ? size : 1);
}

static void textAnchor(const char* text, uint8_t size, uint8_t align, int x, int& outX) {
  const int w = textWidth(text, size);
  outX = (align == VIEW_A_CENTER) ? x - w / 2
       : (align == VIEW_A_RIGHT)  ? x - w
                                  : x;
}

static void drawTextPrim(Arduino_GFX* gfx, const ViewScreen& screen,
                         const ViewPrim& p, uint16_t color) {
  char out[VIEW_MAX_TEXT * 2];
  expandText(viewPrimText(screen, p), out, sizeof(out));
  const uint8_t size = p.aux ? p.aux : 1;
  int x;
  textAnchor(out, size, p.align, p.x, x);
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, p.y);
  gfx->print(out);
}

// ---------------------------------------------------------------------------
// Usage card. One provider, one metric index. The layout scales down to short
// cards and shows "waiting" until the daemon has pushed that provider.
// ---------------------------------------------------------------------------
static void fmtReset(int mins, char* out, size_t n) {
  if (mins <= 0) { strlcpy(out, "now", n); return; }
  int d = mins / 1440, h = (mins % 1440) / 60, m = mins % 60;
  if (d > 0)      snprintf(out, n, "%dd %dh", d, h);
  else if (h > 0) snprintf(out, n, "%dh %02dm", h, m);
  else            snprintf(out, n, "%dm", m);
}

static uint16_t barColor(float pct) {
  if (pct >= 90) return gfxTint(0xF800);
  if (pct >= 75) return gfxTint(0xDBAA);
  return gfxTint(0x7C6B);
}

static void drawUsageBar(Arduino_GFX* gfx, int x, int y, int w, int h,
                         float pct, uint16_t track) {
  if (w <= 0 || h <= 0) return;
  gfx->fillRoundRect(x, y, w, h, h / 2, track);
  int filled = (int)(w * constrain(pct, 0.0f, 100.0f) / 100.0f);
  if (filled >= h)    gfx->fillRoundRect(x, y, filled, h, h / 2, barColor(pct));
  else if (filled > 0) gfx->fillRect(x, y, filled, h, barColor(pct));
}

static void drawUsageSingle(Arduino_GFX* gfx, int x, int y, int w, int h,
                            const UsageMetric& m, uint16_t dim, uint16_t track) {
  gfx->setTextSize(2);
  gfx->setTextColor(dim);
  gfx->setCursor(x + w - textWidth(m.label, 2) - 16, y + 8);
  gfx->print(m.label);

  char pct[10];
  snprintf(pct, sizeof(pct), "%d%%", (int)lroundf(constrain(m.pct, 0.0f, 100.0f)));
  const bool hasValue = m.value[0] != 0;
  char big[USAGE_VALUE_MAX + 1];
  strlcpy(big, hasValue ? m.value : pct, sizeof(big));
  uint8_t size = (h >= 74) ? 4 : (h >= 56 ? 3 : 2);
  while (size > 1 && textWidth(big, size) > w - 24) size--;
  if (textWidth(big, 1) > w - 24 && w > 24) {
    int maxChars = (w - 24) / 6;
    if (maxChars < 1) maxChars = 1;
    if ((size_t)maxChars < strlen(big)) big[maxChars] = 0;
  }
  gfx->setTextSize(size);
  gfx->setTextColor(gfxTint(0xFFFF));
  gfx->setCursor(x + 12, y + 20);
  gfx->print(big);

  const int bx = x + 12, bw = w - 24;
  const int by = y + h - 14;
  if (bw > 0 && by > y + 18 && (m.pct > 0 || !hasValue))
    drawUsageBar(gfx, bx, by, bw, 8, m.pct, track);

  if (h >= 80 && (!hasValue || m.resetMin > 0)) {
    char reset[20], line[36];
    fmtReset(m.resetMin, reset, sizeof(reset));
    snprintf(line, sizeof(line), "Resets in %s", reset);
    gfx->setTextSize(1);
    gfx->setTextColor(dim);
    gfx->setCursor(x + 12, y + h - 28);
    gfx->print(line);
  }
}

// -1/omitted metric: stack every metric the provider sent, compactly.
static void drawUsageRows(Arduino_GFX* gfx, int x, int y, int w, int h,
                          const ProviderUsage& u, uint16_t dim, uint16_t track) {
  const int count = u.metricCount;
  if (count <= 0) return;
  const int top = y + 20;
  const int rowH = (h - 20) / count;
  for (int i = 0; i < count; i++) {
    const UsageMetric& m = u.metrics[i];
    const int ry = top + i * rowH;
    const bool large = rowH >= 24;
    gfx->setTextSize(1);
    gfx->setTextColor(dim);
    gfx->setCursor(x + 12, ry);
    gfx->print(m.label);

    char pct[10];
    snprintf(pct, sizeof(pct), "%d%%", (int)lroundf(constrain(m.pct, 0.0f, 100.0f)));
    char right[USAGE_VALUE_MAX + 1];
    strlcpy(right, m.value[0] ? m.value : pct, sizeof(right));
    int maxRight = w - 12 - textWidth(m.label, 1) - 8;
    if (maxRight < 12) maxRight = 12;
    uint8_t ps = large ? 2 : 1;
    while (ps > 1 && textWidth(right, ps) > maxRight) ps--;
    if (textWidth(right, 1) > maxRight) {
      int maxChars = maxRight / 6;
      if (maxChars < 1) maxChars = 1;
      if ((size_t)maxChars < strlen(right)) right[maxChars] = 0;
    }
    gfx->setTextSize(ps);
    gfx->setTextColor(gfxTint(0xFFFF));
    gfx->setCursor(x + w - textWidth(right, ps) - 12, ry - (large ? 2 : 0));
    gfx->print(right);

    const int by = ry + (large ? 13 : 9);
    const int bh = large ? 6 : 4;
    const int bw = w - 24;
    if (m.pct > 0 && bw > 0 && by + bh <= y + h)
      drawUsageBar(gfx, x + 12, by, bw, bh, m.pct, track);
  }
}

static void drawUsage(Arduino_GFX* gfx, const ViewScreen& screen, const ViewPrim& p,
                      uint16_t accent) {
  const char* id = viewPrimText(screen, p);
  const ProviderUsage* u = usageFind(id);
  const int x = p.x, y = p.y, w = p.x2, h = p.y2;
  if (w <= 0 || h <= 0) return;

  const uint16_t panel = gfxTint(0x18E3);
  const uint16_t dim   = gfxTint(0xB574);
  const uint16_t track = gfxTint(0x2945);
  gfx->fillRoundRect(x, y, w, h, 8, panel);

  char label[USAGE_MAX_NAME];
  strlcpy(label, (u && u->name[0]) ? u->name : id, sizeof(label));
  uint8_t nameSize = 2;
  while (nameSize > 1 && textWidth(label, nameSize) > w - 60) nameSize--;
  if (textWidth(label, nameSize) > w - 60) {
    int maxChars = (w - 60) / (6 * (int)nameSize);
    if (maxChars < 1) maxChars = 1;
    if ((size_t)maxChars < strlen(label)) label[maxChars] = 0;
  }
  gfx->setTextSize(nameSize);
  gfx->setTextColor(accent);
  gfx->setCursor(x + 8, y + 6);
  gfx->print(label);

  // State dot: green when data is fresh, red when the last push failed, dim
  // when the provider has no data yet or the snapshot is stale.
  if (u) {
    uint16_t dot = u->error ? gfxTint(0xF800)
                 : (u->valid && !usageStale(*u) ? gfxTint(0x07E0) : dim);
    gfx->fillCircle(x + w - 8, y + 10, 3, dot);
  }

  if (!u || u->metricCount == 0) {
    const char* waiting = "waiting";
    const int ww = textWidth(waiting, 2);
    gfx->setTextSize(2);
    gfx->setTextColor(dim);
    gfx->setCursor(x + (w - ww) / 2, y + h / 2 - 8);
    gfx->print(waiting);
    return;
  }

  if (p.aux == 0xFF && u->metricCount > 1) {
    drawUsageRows(gfx, x, y, w, h, *u, dim, track);
    return;
  }
  const int idx = (p.aux == 0xFF) ? 0 : (p.aux < u->metricCount ? p.aux : 0);
  drawUsageSingle(gfx, x, y, w, h, u->metrics[idx], dim, track);
}

// ---------------------------------------------------------------------------
// The panel has no framebuffer. Render into a small strip canvas and flush each
// strip, so updates never blank the whole screen. A dirty row range is clipped
// by the canvas: every primitive is drawn in order, just like a full render.
// ---------------------------------------------------------------------------
static const int VIEW_STRIP_H = 8;

class ViewStripCanvas : public Arduino_Canvas {
 public:
  explicit ViewStripCanvas(Arduino_G* output)
      : Arduino_Canvas(TFT_WIDTH, VIEW_STRIP_H, output, 0, 0) {}
  void setOutputY(int16_t y) { _output_y = y; }
};

static void drawPrims(const ViewScreen& screen, Arduino_GFX* gfx, int yOffset) {
  for (uint8_t i = 0; i < screen.primCount; i++) {
    ViewPrim p = screen.prims[i];
    p.y -= yOffset;
    if (p.type == VIEW_P_LINE) p.y2 -= yOffset;
    const uint16_t c = gfxTint(p.color);
    switch (p.type) {
      case VIEW_P_FILL:
        gfx->fillScreen(c);
        break;
      case VIEW_P_RECT:
        gfx->fillRect(p.x, p.y, p.x2, p.y2, c);
        break;
      case VIEW_P_RRECT:
        gfx->fillRoundRect(p.x, p.y, p.x2, p.y2, p.aux, c);
        break;
      case VIEW_P_CIRCLE:
        gfx->fillCircle(p.x, p.y, p.aux, c);
        break;
      case VIEW_P_LINE:
        gfx->drawLine(p.x, p.y, p.x2, p.y2, c);
        break;
      case VIEW_P_TEXT:
        drawTextPrim(gfx, screen, p, c);
        break;
      case VIEW_P_ICON: {
        const uint8_t size = p.aux ? p.aux : 1;
        const int iw = 24 * size;
        const int x = (p.align == VIEW_A_CENTER) ? p.x - iw / 2
                    : (p.align == VIEW_A_RIGHT)  ? p.x - iw : p.x;
        viewDrawIcon(gfx, viewPrimText(screen, p), x, p.y, size, c);
        break;
      }
      case VIEW_P_BITMAP: {
        const int w = p.x2, h = p.y2, s = p.aux ? p.aux : 1;
        const int rw = w * s;
        const int x = (p.align == VIEW_A_CENTER) ? p.x - rw / 2
                    : (p.align == VIEW_A_RIGHT)  ? p.x - rw : p.x;
        const uint8_t* bits = screen.bitmap + p.voff;
        for (int py = 0; py < h; py++)
          for (int px = 0; px < w; px++) {
            const int idx = py * w + px;
            if (bits[idx >> 3] & (0x80 >> (idx & 7))) {
              if (s == 1) gfx->drawPixel(x + px, p.y + py, c);
              else        gfx->fillRect(x + px * s, p.y + py * s, s, s, c);
            }
          }
        break;
      }
      case VIEW_P_USAGE:
        drawUsage(gfx, screen, p, c);
        break;
    }
  }
}

static bool renderRegion(const ViewScreen& screen, int y0, int y1) {
  Arduino_GFX* output = gfxDev();
  if (!output) return false;
  if (y0 < 0) y0 = 0;
  if (y1 > TFT_HEIGHT) y1 = TFT_HEIGHT;
  if (y1 <= y0) return false;

  ViewStripCanvas canvas(output);
  if (!canvas.begin(GFX_SKIP_OUTPUT_BEGIN)) return false;

  const int start = (y0 / VIEW_STRIP_H) * VIEW_STRIP_H;
  int end = ((y1 + VIEW_STRIP_H - 1) / VIEW_STRIP_H) * VIEW_STRIP_H;
  if (end > TFT_HEIGHT) end = TFT_HEIGHT;
  for (int y = start; y < end; y += VIEW_STRIP_H) {
    canvas.setOutputY(y);
    canvas.fillScreen(gfxTint(screen.bg));
    drawPrims(screen, &canvas, y);
    canvas.flush();
  }
  return true;
}

bool viewRenderText(const ViewScreen& screen) {
  int y0 = TFT_HEIGHT, y1 = 0;
  for (uint8_t i = 0; i < screen.primCount; i++) {
    const ViewPrim& p = screen.prims[i];
    if (p.type != VIEW_P_TEXT || !strchr(viewPrimText(screen, p), '{')) continue;
    const int size = p.aux ? p.aux : 1;
    if (p.y - 1 < y0) y0 = p.y - 1;
    if (p.y + 8 * size + 1 > y1) y1 = p.y + 8 * size + 1;
  }
  return y1 > y0 && renderRegion(screen, y0, y1);
}

bool viewRenderUsage(const ViewScreen& screen) {
  bool rendered = false;
  for (uint8_t i = 0; i < screen.primCount; i++) {
    const ViewPrim& p = screen.prims[i];
    if (p.type != VIEW_P_USAGE || p.x2 <= 0 || p.y2 <= 0) continue;
    if (!renderRegion(screen, p.y, p.y + p.y2)) return false;
    rendered = true;
  }
  return rendered;
}

void viewRender(const ViewScreen& screen) {
  if (renderRegion(screen, 0, TFT_HEIGHT)) return;

  // Last-resort path if the strip canvas cannot be allocated.
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  gfx->fillScreen(gfxTint(screen.bg));
  drawPrims(screen, gfx, 0);
}
