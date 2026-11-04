#include "Gfx.h"
#include "Arduino_GFX_Library.h"

static Arduino_GFX g_display(TFT_WIDTH, TFT_HEIGHT);

static int g_previewOrder = 0;  // 0 auto, 1 rgb, 2 bgr
static bool g_previewInvert = false;
static int g_previewRGain = 100;
static int g_previewGGain = 100;
static int g_previewBGain = 100;

void gfxSetPreviewDisplay(int order, bool invert, int rGain, int gGain, int bGain) {
  g_previewOrder = order;
  g_previewInvert = invert;
  g_previewRGain = rGain;
  g_previewGGain = gGain;
  g_previewBGain = bGain;
}

uint16_t gfxTint(uint16_t rgb565) {
  uint32_t r = (rgb565 >> 11) & 0x1F;
  uint32_t g = (rgb565 >> 5) & 0x3F;
  uint32_t b = rgb565 & 0x1F;
  if (g_previewRGain != 100) { r = r * g_previewRGain / 100; if (r > 31) r = 31; }
  if (g_previewGGain != 100) { g = g * g_previewGGain / 100; if (g > 63) g = 63; }
  if (g_previewBGain != 100) { b = b * g_previewBGain / 100; if (b > 31) b = 31; }
  if (g_previewOrder == 2) std::swap(r, b);
  uint16_t out = (uint16_t)((r << 11) | (g << 5) | b);
  if (g_previewInvert) out = (uint16_t)~out;
  return out;
}
Arduino_GFX* gfxDev() { return &g_display; }
