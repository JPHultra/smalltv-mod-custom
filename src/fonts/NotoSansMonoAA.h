// Anti-aliased Noto Sans Mono cells for text scales 1..6.
//
// Cell dimensions match the firmware's existing text contract: 6*scale wide
// and 8*scale high, with one 4-bit alpha value per pixel. The glyph data is
// derived from Noto Sans Mono Regular; see FONT-LICENSE.txt at the repository
// root for the font license.
#pragma once
#include <Arduino.h>

struct ViewAaFont {
  const uint8_t* data;
  uint8_t width;
  uint8_t height;
  uint16_t bytes;
};

const ViewAaFont* viewAaFont(uint8_t scale);
