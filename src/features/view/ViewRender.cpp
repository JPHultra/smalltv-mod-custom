#include "ViewRender.h"
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "Clock.h"
#include "Gfx.h"
#include "UsageClient.h"
#include "ViewIcons.h"
#include "font/glcdfont.h"
#include "fonts/NotoSansMonoAA.h"

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

// Legacy 5x7 fallback, used when the anti-aliased font is unavailable (for
// example if the strip canvas could not be allocated, or for sizes above 6).
static void drawLegacyGlyphString(Arduino_GFX* gfx, const char* text, int x, int y,
                                  uint8_t size, uint16_t color) {
  if (!text || !*text) return;
  const uint8_t s = size ? size : 1;
  int cx = x, cy = y;
  for (const char* p = text; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    if (c == '\r') continue;
    if (c == '\n') { cx = x; cy += 8 * s; continue; }
    for (uint8_t i = 0; i < 5; i++) {
      uint8_t line = pgm_read_byte(&font[c * 5 + i]);
      for (uint8_t j = 0; j < 8; j++, line >>= 1) {
        if (!(line & 1)) continue;
        if (s == 1) gfx->drawPixel(cx + i, cy + j, color);
        else        gfx->fillRect(cx + i * s, cy + j * s, s, s, color);
      }
    }
    cx += 6 * s;
  }
}

class ViewStripCanvas;
static ViewStripCanvas* g_textCanvas = nullptr;
static void drawGlyphString(Arduino_GFX* gfx, const char* text, int x, int y,
                            uint8_t size, uint16_t color);

static void drawTextPrim(Arduino_GFX* gfx, const ViewScreen& screen,
                         const ViewPrim& p, uint16_t color) {
  char out[VIEW_MAX_TEXT * 2];
  expandText(viewPrimText(screen, p), out, sizeof(out));
  const uint8_t size = p.aux ? p.aux : 1;
  const TextStyle* ts = viewPrimTextStyle(screen, p);
  const int marginX = ts ? ts->marginX : 0;
  const int marginY = ts ? ts->marginY : 0;
  const int padX = ts ? ts->padX : 0;
  const int padY = ts ? ts->padY : 0;
  const int lineGap = ts ? ts->lineGap : 0;
  const bool boxed = p.x2 > 0 || p.y2 > 0 || ts;
  if (!boxed) {
    int x;
    textAnchor(out, size, p.align, p.x, x);
    drawGlyphString(gfx, out, x, p.y, size, color);
    return;
  }

  int lines = 1;
  int maxWidth = 0;
  int current = 0;
  for (const char* q = out; ; q++) {
    if (*q == '\n' || *q == 0) {
      if (current > maxWidth) maxWidth = current;
      current = 0;
      if (*q == 0) break;
      lines++;
    } else {
      current += 6 * size;
    }
  }
  const int lineHeight = 8 * size;
  const int textHeight = lines * lineHeight + (lines - 1) * lineGap;
  const int boxW = p.x2 > 0 ? p.x2 : maxWidth;
  const int boxH = p.y2 > 0 ? p.y2 : textHeight;
  const int left = p.x + marginX;
  const int top = p.y + marginY;
  const int contentLeft = left + padX;
  const int contentWidth = std::max(0, boxW - 2 * padX);
  const int contentHeight = std::max(0, boxH - 2 * padY);
  const int startY = top + padY +
      (ts && ts->valign == 1 ? std::max(0, (contentHeight - textHeight) / 2)
       : ts && ts->valign == 2 ? std::max(0, contentHeight - textHeight) : 0);

  char line[VIEW_MAX_TEXT * 2];
  int used = 0, y = startY, lineIndex = 0;
  for (const char* q = out; ; q++) {
    if (*q == '\n' || *q == 0) {
      line[used] = 0;
      int width = textWidth(line, size);
      int tx = contentLeft;
      if (p.align == VIEW_A_CENTER) tx = contentLeft + (contentWidth - width) / 2;
      else if (p.align == VIEW_A_RIGHT) tx = contentLeft + contentWidth - width;
      if (line[0]) drawGlyphString(gfx, line, tx, y, size, color);
      used = 0;
      if (*q == 0) break;
      y += lineHeight + lineGap;
      lineIndex++;
    } else {
      if (used + 1 < (int)sizeof(line)) line[used++] = *q;
    }
  }
  (void)lineIndex;
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

static uint16_t barColor(float remaining) {
  if (remaining <= 10) return gfxTint(0xF800);
  if (remaining <= 25) return gfxTint(0xDBAA);
  return gfxTint(0x7C6B);
}

static uint8_t usageAutoSize(int height) {
  return (uint8_t)(height >= 74 ? 4 : height >= 56 ? 3 : 2);
}

static uint8_t usageFitSize(const char* text, uint8_t size, int maxWidth) {
  if (size < 1) size = 1;
  while (size > 1 && textWidth(text, size) > maxWidth) size--;
  return size;
}

static uint8_t usageAlign(uint8_t value, bool rows, bool label) {
  if (value != 0xFF) return value;
  return label ? (rows ? VIEW_A_LEFT : VIEW_A_RIGHT)
               : (rows ? VIEW_A_RIGHT : VIEW_A_LEFT);
}

static int usageTextX(int left, int right, uint8_t align, int width) {
  if (align == VIEW_A_CENTER) return left + (right - left - width) / 2;
  if (align == VIEW_A_RIGHT) return right - width;
  return left;
}

static uint16_t usageColor(uint16_t color, uint16_t fallback) {
  return color ? gfxTint(color) : fallback;
}

static void drawUsageBar(Arduino_GFX* gfx, int x, int y, int w, int h,
                         float pct, uint16_t track, uint16_t fill) {
  if (w <= 0 || h <= 0) return;
  gfx->fillRoundRect(x, y, w, h, h / 2, track);
  int filled = (int)(w * constrain(pct, 0.0f, 100.0f) / 100.0f);
  if (filled >= h)    gfx->fillRoundRect(x, y, filled, h, h / 2, fill);
  else if (filled > 0) gfx->fillRect(x, y, filled, h, fill);
}

static bool usageBoxReady(const UsageBox& box) {
  return (box.flags & 3) == 3 && box.w > 0 && box.h > 0;
}

static void drawUsageBoxText(Arduino_GFX* gfx, const ViewScreen& screen,
                             const UsageStyle& style, const UsageBox& box,
                             int cardX, int cardY, const char* text, uint8_t size,
                             uint16_t color, uint8_t defaultAlign) {
  (void)screen;
  (void)style;
  if (!text || !text[0] || !usageBoxReady(box)) return;
  const int avail = std::max(1, box.w - 2 * box.padX);
  size = usageFitSize(text, size ? size : 1, avail);
  const uint8_t align = (box.flags & 4) ? box.align : defaultAlign;
  const int width = textWidth(text, size);
  const int left = cardX + box.x + box.marginX + box.padX;
  const int right = cardX + box.x + box.marginX + box.w - box.padX;
  int x = usageTextX(left, right, align, width);
  int y = cardY + box.y + box.marginY + box.padY;
  const int contentH = std::max(0, box.h - 2 * box.padY);
  if (box.valign == 1) y += std::max(0, (contentH - 8 * size) / 2);
  else if (box.valign == 2) y += std::max(0, contentH - 8 * size);
  drawGlyphString(gfx, text, x, y, size, color);
}

static void drawUsageBoxBar(Arduino_GFX* gfx, const ViewScreen& style, const UsageBox& box,
                            int cardX, int cardY, float pct, uint16_t track,
                            uint16_t fill) {
  (void)style;
  if (!usageBoxReady(box)) return;
  const int x = cardX + box.x + box.marginX + box.padX;
  const int y = cardY + box.y + box.marginY + box.padY;
  const int w = std::max(0, box.w - 2 * box.padX);
  const int h = std::max(0, box.h - 2 * box.padY);
  drawUsageBar(gfx, x, y, w, h, pct, track, fill);
}

static void drawUsageCustomSingle(Arduino_GFX* gfx, const ViewScreen& screen,
                                  const ViewPrim& p, const UsageStyle& style,
                                  const UsageLayout& layout, const UsageMetric& m,
                                  int metric, uint16_t accent, uint16_t dim,
                                  uint16_t track) {
  const UsageRowLayout& row = layout.rows[metric < USAGE_MAX_METRICS ? metric : 0];
  const uint16_t valueColor = usageColor(style.valueColor, gfxTint(0xFFFF));
  const uint16_t labelColor = usageColor(style.labelColor, dim);
  const uint16_t resetColor = usageColor(style.resetColor, dim);
  if ((style.flags & USAGE_SHOW_LABEL) && usageBoxReady(row.label)) {
    const char* custom = viewUsageMetricLabel(screen, style, metric);
    drawUsageBoxText(gfx, screen, style, row.label, p.x, p.y,
                     custom ? custom : m.label,
                     style.labelSize ? style.labelSize : 2, labelColor,
                     usageAlign(style.labelAlign, false, true));
  }
  char pct[10];
  snprintf(pct, sizeof(pct), "%d%%", (int)lroundf(constrain(m.pct, 0.0f, 100.0f)));
  const bool hasValue = m.value[0] != 0;
  char big[USAGE_VALUE_MAX + 1];
  strlcpy(big, hasValue ? m.value : pct, sizeof(big));
  if ((style.flags & USAGE_SHOW_VALUE) && usageBoxReady(row.value)) {
    drawUsageBoxText(gfx, screen, style, row.value, p.x, p.y, big,
                     style.valueSize ? style.valueSize : usageAutoSize(p.y2),
                     valueColor, usageAlign(style.valueAlign, false, false));
  }
  if ((style.flags & USAGE_SHOW_BAR) && usageBoxReady(row.bar) &&
      (m.pct > 0 || !hasValue)) {
    const uint16_t fill = (style.flags & USAGE_BAR_AUTO)
                              ? barColor(m.pct) : usageColor(style.barColor, accent);
    drawUsageBoxBar(gfx, screen, row.bar, p.x, p.y, m.pct, track, fill);
  }
  if ((style.flags & USAGE_SHOW_RESET) && usageBoxReady(layout.reset) &&
      (!hasValue || m.resetMin > 0)) {
    char reset[20], line[36];
    fmtReset(m.resetMin, reset, sizeof(reset));
    snprintf(line, sizeof(line), "Resets in %s", reset);
    drawUsageBoxText(gfx, screen, style, layout.reset, p.x, p.y, line,
                     style.resetSize ? style.resetSize : 1, resetColor,
                     style.resetAlign);
  }
}

static void drawUsageCustomRows(Arduino_GFX* gfx, const ViewScreen& screen,
                                const ViewPrim& p, const UsageStyle& style,
                                const UsageLayout& layout, const ProviderUsage& u,
                                uint16_t accent, uint16_t dim, uint16_t track) {
  const uint16_t valueColor = usageColor(style.valueColor, gfxTint(0xFFFF));
  const uint16_t labelColor = usageColor(style.labelColor, dim);
  for (int i = 0; i < u.metricCount && i < USAGE_MAX_METRICS; i++) {
    const UsageMetric& m = u.metrics[i];
    const UsageRowLayout& row = layout.rows[i];
    const char* custom = viewUsageMetricLabel(screen, style, i);
    if ((style.flags & USAGE_SHOW_LABEL) && usageBoxReady(row.label)) {
      drawUsageBoxText(gfx, screen, style, row.label, p.x, p.y,
                       custom ? custom : m.label,
                       style.labelSize ? style.labelSize : 1, labelColor,
                       usageAlign(style.labelAlign, true, true));
    }
    char pct[10];
    snprintf(pct, sizeof(pct), "%d%%", (int)lroundf(constrain(m.pct, 0.0f, 100.0f)));
    char right[USAGE_VALUE_MAX + 1];
    strlcpy(right, m.value[0] ? m.value : pct, sizeof(right));
    if ((style.flags & USAGE_SHOW_VALUE) && usageBoxReady(row.value)) {
      drawUsageBoxText(gfx, screen, style, row.value, p.x, p.y, right,
                       style.valueSize ? style.valueSize : 2, valueColor,
                       usageAlign(style.valueAlign, true, false));
    }
    if ((style.flags & USAGE_SHOW_BAR) && usageBoxReady(row.bar) &&
        m.pct > 0) {
      const uint16_t fill = (style.flags & USAGE_BAR_AUTO)
                                ? barColor(m.pct) : usageColor(style.barColor, accent);
      drawUsageBoxBar(gfx, screen, row.bar, p.x, p.y, m.pct, track, fill);
    }
  }
  if ((style.flags & USAGE_SHOW_RESET) && usageBoxReady(layout.reset)) {
    // Rows share a reset position; only show it when the last metric has one.
    const UsageMetric& last = u.metrics[u.metricCount - 1];
    if (last.resetMin > 0) {
      char reset[20], line[36];
      fmtReset(last.resetMin, reset, sizeof(reset));
      snprintf(line, sizeof(line), "Resets in %s", reset);
      drawUsageBoxText(gfx, screen, style, layout.reset, p.x, p.y, line,
                       style.resetSize ? style.resetSize : 1,
                       usageColor(style.resetColor, dim), style.resetAlign);
    }
  }
}

static void drawUsageSingle(Arduino_GFX* gfx, const ViewScreen& screen,
                            const ViewPrim& p, const UsageStyle& style,
                            const UsageMetric& m, int metric,
                            uint16_t accent, uint16_t dim, uint16_t track) {
  const int x = p.x, y = p.y, w = p.x2, h = p.y2;
  const int padding = style.padding;
  const uint16_t valueColor = usageColor(style.valueColor, gfxTint(0xFFFF));
  const uint16_t labelColor = usageColor(style.labelColor, dim);
  const uint16_t resetColor = usageColor(style.resetColor, dim);

  if (style.flags & USAGE_SHOW_LABEL) {
    const char* custom = viewUsageMetricLabel(screen, style, metric);
    const char* label = custom ? custom : m.label;
    if (label && label[0]) {
      uint8_t size = style.labelSize ? style.labelSize : 2;
      size = usageFitSize(label, size, w - 2 * padding);
      const uint8_t align = usageAlign(style.labelAlign, false, true);
      const int tx = usageTextX(x + padding, x + w - padding, align,
                                textWidth(label, size));
      drawGlyphString(gfx, label, tx, y + 8, size, labelColor);
    }
  }

  char pct[10];
  snprintf(pct, sizeof(pct), "%d%%", (int)lroundf(constrain(m.pct, 0.0f, 100.0f)));
  const bool hasValue = m.value[0] != 0;
  char big[USAGE_VALUE_MAX + 1];
  strlcpy(big, hasValue ? m.value : pct, sizeof(big));
  if (style.flags & USAGE_SHOW_VALUE) {
    uint8_t size = style.valueSize ? style.valueSize : usageAutoSize(h);
    size = usageFitSize(big, size, w - 2 * padding);
    const uint8_t align = usageAlign(style.valueAlign, false, false);
    const int tx = usageTextX(x + padding, x + w - padding, align,
                              textWidth(big, size));
    drawGlyphString(gfx, big, tx, y + 20, size, valueColor);
  }

  const int barH = style.barHeight;
  const int barY = y + h - 6 - barH;
  const int barW = w - 2 * padding;
  if ((style.flags & USAGE_SHOW_BAR) && barW > 0 && barY > y + 18 &&
      (m.pct > 0 || !hasValue)) {
    const uint16_t fill = (style.flags & USAGE_BAR_AUTO)
                              ? barColor(m.pct) : usageColor(style.barColor, accent);
    drawUsageBar(gfx, x + padding, barY, barW, barH, m.pct, track, fill);
  }

  if ((style.flags & USAGE_SHOW_RESET) && h >= 72 && (!hasValue || m.resetMin > 0)) {
    char reset[20], line[36];
    fmtReset(m.resetMin, reset, sizeof(reset));
    snprintf(line, sizeof(line), "Resets in %s", reset);
    uint8_t size = style.resetSize ? style.resetSize : 1;
    size = usageFitSize(line, size, w - 2 * padding);
    const uint8_t align = style.resetAlign;
    const int tx = usageTextX(x + padding, x + w - padding, align,
                              textWidth(line, size));
    drawGlyphString(gfx, line, tx, y + h - 28, size, resetColor);
  }
}

static void drawUsageRows(Arduino_GFX* gfx, const ViewScreen& screen,
                          const ViewPrim& p, const UsageStyle& style,
                          const ProviderUsage& u, uint16_t accent,
                          uint16_t dim, uint16_t track) {
  const int x = p.x, y = p.y, w = p.x2, h = p.y2;
  const int count = u.metricCount;
  if (count <= 0) return;
  const int padding = style.padding;
  const int top = y + 20;
  const int rowH = (h - 20) / count;
  if (rowH <= 0) return;
  const uint16_t valueColor = usageColor(style.valueColor, gfxTint(0xFFFF));
  const uint16_t labelColor = usageColor(style.labelColor, dim);
  for (int i = 0; i < count; i++) {
    const UsageMetric& m = u.metrics[i];
    const int ry = top + i * rowH;
    const bool large = rowH >= 24;
    const char* custom = viewUsageMetricLabel(screen, style, i);
    const char* label = custom ? custom : m.label;
    int labelWidth = 0;
    if ((style.flags & USAGE_SHOW_LABEL) && label && label[0]) {
      uint8_t size = style.labelSize ? style.labelSize : 1;
      size = usageFitSize(label, size, w / 2);
      labelWidth = textWidth(label, size);
      const uint8_t align = usageAlign(style.labelAlign, true, true);
      const int tx = usageTextX(x + padding, x + w - padding, align, labelWidth);
      drawGlyphString(gfx, label, tx, ry, size, labelColor);
    }

    char pct[10];
    snprintf(pct, sizeof(pct), "%d%%", (int)lroundf(constrain(m.pct, 0.0f, 100.0f)));
    char right[USAGE_VALUE_MAX + 1];
    strlcpy(right, m.value[0] ? m.value : pct, sizeof(right));
    if (style.flags & USAGE_SHOW_VALUE) {
      const int labelRoom = labelWidth ? labelWidth + 8 : 0;
      int maxRight = w - 2 * padding - labelRoom;
      if (maxRight < 12) maxRight = 12;
      uint8_t size = style.valueSize ? style.valueSize : (large ? 2 : 1);
      size = usageFitSize(right, size, maxRight);
      const uint8_t align = usageAlign(style.valueAlign, true, false);
      const int tx = usageTextX(x + padding, x + w - padding, align,
                                textWidth(right, size));
      drawGlyphString(gfx, right, tx, ry - (large ? 2 : 0), size, valueColor);
    }

    if (style.flags & USAGE_SHOW_BAR) {
      const int bh = std::min<int>(style.barHeight, large ? 6 : 4);
      const int by = ry + (large ? 13 : 9);
      const int bw = w - 2 * padding;
      if (m.pct > 0 && bw > 0 && by + bh <= y + h) {
        const uint16_t fill = (style.flags & USAGE_BAR_AUTO)
                                  ? barColor(m.pct) : usageColor(style.barColor, accent);
        drawUsageBar(gfx, x + padding, by, bw, bh, m.pct, track, fill);
      }
    }
  }
}

static void drawUsage(Arduino_GFX* gfx, const ViewScreen& screen, const ViewPrim& p,
                      uint16_t accent) {
  const char* id = viewPrimText(screen, p);
  const ProviderUsage* u = usageFind(id);
  const int x = p.x, y = p.y, w = p.x2, h = p.y2;
  if (w <= 0 || h <= 0) return;

  UsageStyle fallback;
  const UsageStyle* found = viewPrimUsageStyle(screen, p);
  const UsageStyle& style = found ? *found : fallback;
  const UsageLayout* layout = viewPrimUsageLayout(screen, p);
  const uint16_t panel = gfxTint(style.panelColor);
  const uint16_t dim = gfxTint(style.labelColor ? style.labelColor : 0xB574);
  const uint16_t track = gfxTint(style.trackColor);

  if (style.flags & USAGE_SHOW_PANEL)
    gfx->fillRoundRect(x, y, w, h, style.radius, panel);

  const char* title = viewUsageTitle(screen, style);
  if (!title) title = (u && u->name[0]) ? u->name : id;
  if ((style.flags & USAGE_SHOW_NAME) && title && title[0]) {
    if (layout && usageBoxReady(layout->name)) {
      drawUsageBoxText(gfx, screen, style, layout->name, x, y, title,
                       style.nameSize ? style.nameSize : 2,
                       usageColor(style.nameColor, accent), style.nameAlign);
    } else {
      const int dotRoom = (style.flags & USAGE_SHOW_DOT) ? 24 : 0;
      uint8_t size = style.nameSize ? style.nameSize : 2;
      size = usageFitSize(title, size, w - style.padding - 8 - dotRoom);
      const int tx = usageTextX(x + 8, x + w - 8 - dotRoom, style.nameAlign,
                                textWidth(title, size));
      drawGlyphString(gfx, title, tx, y + 6, size,
                      usageColor(style.nameColor, accent));
    }
  }

  if (u && (style.flags & USAGE_SHOW_DOT)) {
    const uint16_t dot = u->error ? gfxTint(0xF800)
                       : (u->valid && !usageStale(*u) ? gfxTint(0x07E0) : dim);
    gfx->fillCircle(x + w - 8, y + 10, 3, dot);
  }

  if (!u || u->metricCount == 0) {
    const char* waiting = "waiting";
    uint8_t size = style.valueSize ? style.valueSize : 2;
    size = usageFitSize(waiting, size, w - 2 * style.padding);
    const int tx = usageTextX(x + style.padding, x + w - style.padding,
                              VIEW_A_CENTER, textWidth(waiting, size));
    drawGlyphString(gfx, waiting, tx, y + h / 2 - 8, size, dim);
    return;
  }

  if (p.aux == 0xFF && u->metricCount > 1) {
    if (layout && layout->configured && usageBoxReady(layout->rows[0].label) &&
        usageBoxReady(layout->rows[0].value)) {
      drawUsageCustomRows(gfx, screen, p, style, *layout, *u, accent, dim, track);
    } else {
      drawUsageRows(gfx, screen, p, style, *u, accent, dim, track);
    }
    return;
  }
  const int idx = (p.aux == 0xFF) ? 0 : (p.aux < u->metricCount ? p.aux : 0);
  if (layout && layout->configured && usageBoxReady(layout->rows[idx].label) &&
      usageBoxReady(layout->rows[idx].value)) {
    drawUsageCustomSingle(gfx, screen, p, style, *layout, u->metrics[idx], idx,
                          accent, dim, track);
  } else {
    drawUsageSingle(gfx, screen, p, style, u->metrics[idx], idx, accent, dim, track);
  }
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

  // Blend an anti-aliased glyph pixel with the strip's current pixel.
  void blendPixel(int x, int y, uint16_t fg, uint8_t alpha) {
    if (alpha == 0 || x < 0 || x >= TFT_WIDTH || y < 0 || y >= VIEW_STRIP_H) return;
    uint16_t* fb = getFramebuffer();
    if (!fb) return;
    uint16_t* dst = &fb[y * TFT_WIDTH + x];
    if (alpha >= 255) { *dst = fg; return; }
    const uint32_t inv = 255 - alpha;
    uint32_t r = (((fg >> 11) & 0x1F) * alpha + ((*dst >> 11) & 0x1F) * inv + 127) / 255;
    uint32_t g = (((fg >> 5) & 0x3F) * alpha + ((*dst >> 5) & 0x3F) * inv + 127) / 255;
    uint32_t b = ((fg & 0x1F) * alpha + (*dst & 0x1F) * inv + 127) / 255;
    *dst = (uint16_t)((r << 11) | (g << 5) | b);
  }
};

static void drawGlyphString(Arduino_GFX* gfx, const char* text, int x, int y,
                            uint8_t size, uint16_t color) {
  if (!text || !*text) return;
  const uint8_t s = size ? size : 1;
  const ViewAaFont* font = (g_textCanvas && gfx == g_textCanvas) ? viewAaFont(s) : nullptr;
  if (!font) { drawLegacyGlyphString(gfx, text, x, y, s, color); return; }

  const int cw = font->width;
  const int ch = font->height;
  int cx = x, cy = y;
  for (const char* p = text; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    if (c == '\r') continue;
    if (c == '\n') { cx = x; cy += ch; continue; }
    const int code = (c >= 32 && c <= 126) ? c : 63;
    const uint8_t* glyph = font->data + (uint16_t)(code - 32) * font->bytes;
    for (int py = 0; py < ch; py++) {
      for (int px = 0; px < cw; px++) {
        const int bit = py * cw + px;
        const uint8_t packed = pgm_read_byte(&glyph[bit >> 1]);
        const uint8_t a = (px & 1) ? (packed & 0x0F) : (packed >> 4);
        if (a) g_textCanvas->blendPixel(cx + px, cy + py, color, (uint8_t)(a * 17));
      }
    }
    cx += cw;
  }
}

static void drawImageAsset(Arduino_GFX* gfx, ViewImageSource* images, const char* name,
                            uint32_t elapsedMs, int x, int y, int w, int h,
                            bool cover, int yOffset) {
  if (!images || !name || w <= 0 || h <= 0) return;
  ViewImageInfo info;
  if (!images->info(name, info) || !info.width || !info.height) return;
  const uint16_t frame = viewImageFrameAt(info, elapsedMs);
  uint16_t rowColors[IMAGE_ASSET_MAX_DIM];
  uint8_t rowAlpha[IMAGE_ASSET_MAX_DIM];
  int lastSourceY = -1;

  float scaleX = 1.0f, scaleY = 1.0f, left = 0.0f, top = 0.0f;
  if (cover) {
    const float scale = fmaxf((float)w / info.width, (float)h / info.height);
    const float drawW = info.width * scale;
    const float drawH = info.height * scale;
    left = (w - drawW) * 0.5f;
    top = (h - drawH) * 0.5f;
    scaleX = scaleY = scale;
  }
  for (int dy = 0; dy < h; dy++) {
    const int screenY = y + dy;
    if (screenY < yOffset || screenY >= yOffset + VIEW_STRIP_H) continue;
    int sourceY;
    if (cover) {
      const float fy = (dy - top) / scaleY;
      if (fy < 0 || fy >= info.height) continue;
      sourceY = (int)fy;
    } else {
      sourceY = (int)((int64_t)dy * info.height / h);
    }
    if (sourceY < 0 || sourceY >= info.height) continue;
    if (sourceY != lastSourceY) {
      if (!images->readRow(name, frame, (uint16_t)sourceY, rowColors, rowAlpha, info.width))
        return;
      lastSourceY = sourceY;
    }
    for (int dx = 0; dx < w; dx++) {
      int sourceX;
      if (cover) {
        const float fx = (dx - left) / scaleX;
        if (fx < 0 || fx >= info.width) continue;
        sourceX = (int)fx;
      } else {
        sourceX = (int)((int64_t)dx * info.width / w);
      }
      if (sourceX < 0 || sourceX >= info.width) continue;
      if (!rowAlpha[sourceX]) continue;
      gfx->drawPixel(x + dx, screenY - yOffset, gfxTint(rowColors[sourceX]));
    }
  }
}

static void drawPrims(const ViewScreen& screen, Arduino_GFX* gfx, int yOffset,
                      uint32_t elapsedMs, ViewImageSource* images) {
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
        const ViewBitmapStyle* bs = viewPrimBitmapStyle(screen, p);
        const int rw = bs ? bs->displayW : w * s;
        const int rh = bs ? bs->displayH : h * s;
        const int x = (p.align == VIEW_A_CENTER) ? p.x - rw / 2
                    : (p.align == VIEW_A_RIGHT)  ? p.x - rw : p.x;
        const uint8_t* bits = screen.bitmap + p.voff;
        for (int py = 0; py < rh; py++) {
          const int sy = py * h / rh;
          for (int px = 0; px < rw; px++) {
            const int sx = px * w / rw;
            const int idx = sy * w + sx;
            if (bits[idx >> 3] & (0x80 >> (idx & 7))) {
              if (bs || s != 1) gfx->fillRect(x + px, p.y + py, 1, 1, c);
              else gfx->drawPixel(x + px, p.y + py, c);
            }
          }
        }
        break;
      }
      case VIEW_P_IMAGE: {
        const int s = p.aux ? p.aux : 1;
        ViewImageInfo info;
        const char* name = viewPrimImage(screen, p);
        if (name && images && images->info(name, info)) {
          const int w = p.x2 > 0 ? p.x2 : info.width * s;
          const int h = p.y2 > 0 ? p.y2 : info.height * s;
          const int x = (p.align == VIEW_A_CENTER) ? p.x - w / 2
                      : (p.align == VIEW_A_RIGHT)  ? p.x - w : p.x;
          drawImageAsset(gfx, images, name, elapsedMs, x, p.y + yOffset, w, h,
                         false, yOffset);
        }
        break;
      }
      case VIEW_P_USAGE:
        drawUsage(gfx, screen, p, c);
        break;
    }
  }
}

static bool renderRegion(const ViewScreen& screen, int y0, int y1,
                         uint32_t elapsedMs, ViewImageSource* images) {
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
    if (const char* bg = viewBackgroundImage(screen))
      drawImageAsset(&canvas, images, bg, elapsedMs, 0, 0, TFT_WIDTH, TFT_HEIGHT,
                     true, y);
    g_textCanvas = &canvas;
    drawPrims(screen, &canvas, y, elapsedMs, images);
    g_textCanvas = nullptr;
    canvas.flush();
  }
  return true;
}

bool viewRenderText(const ViewScreen& screen, uint32_t elapsedMs,
                    ViewImageSource* images) {
  int y0 = TFT_HEIGHT, y1 = 0;
  for (uint8_t i = 0; i < screen.primCount; i++) {
    const ViewPrim& p = screen.prims[i];
    if (p.type != VIEW_P_TEXT || !strchr(viewPrimText(screen, p), '{')) continue;
    const int size = p.aux ? p.aux : 1;
    const int height = p.y2 > 0 ? p.y2 : 8 * size;
    if (p.y - 1 < y0) y0 = p.y - 1;
    if (p.y + height + 1 > y1) y1 = p.y + height + 1;
  }
  return y1 > y0 && renderRegion(screen, y0, y1, elapsedMs, images);
}

bool viewRenderUsage(const ViewScreen& screen, uint32_t elapsedMs,
                     ViewImageSource* images) {
  bool rendered = false;
  for (uint8_t i = 0; i < screen.primCount; i++) {
    const ViewPrim& p = screen.prims[i];
    if (p.type != VIEW_P_USAGE || p.x2 <= 0 || p.y2 <= 0) continue;
    if (!renderRegion(screen, p.y, p.y + p.y2, elapsedMs, images)) return false;
    rendered = true;
  }
  return rendered;
}

void viewRenderFrame(const ViewScreen& screen, uint32_t elapsedMs,
                     ViewImageSource* images) {
  if (renderRegion(screen, 0, TFT_HEIGHT, elapsedMs, images)) return;

  // Last-resort path if the strip canvas cannot be allocated.
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  gfx->fillScreen(gfxTint(screen.bg));
  if (const char* bg = viewBackgroundImage(screen))
    drawImageAsset(gfx, images, bg, elapsedMs, 0, 0, TFT_WIDTH, TFT_HEIGHT,
                   true, 0);
  drawPrims(screen, gfx, 0, elapsedMs, images);
}

void viewRender(const ViewScreen& screen) {
  viewRenderFrame(screen, 0, nullptr);
}
