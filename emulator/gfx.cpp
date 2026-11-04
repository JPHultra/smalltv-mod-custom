#include "Gfx.h"
#include "Arduino_GFX_Library.h"

static Arduino_GFX g_display(TFT_WIDTH, TFT_HEIGHT);

uint16_t gfxTint(uint16_t rgb565) { return rgb565; }
Arduino_GFX* gfxDev() { return &g_display; }
