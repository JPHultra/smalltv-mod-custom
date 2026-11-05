#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace imagecodec {

struct GifFrame {
  int delayMs = 100;
  std::vector<uint8_t> rgba;   // full-canvas RGBA
};

struct GifImage {
  int width = 0;
  int height = 0;
  std::vector<GifFrame> frames;
};

bool decodeGif(const std::string& path, GifImage& out, std::string& error);

}  // namespace imagecodec
