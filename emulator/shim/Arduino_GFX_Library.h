// Host-side stand-in for the Arduino_GFX display and canvas used by the
// firmware view renderer. It is an RGB565 framebuffer, not an ST7789 emulator.
#pragma once

#include <Arduino.h>
#include <pgmspace.h>
#include <algorithm>
#include <cstdint>
#include <vector>

#define GFX_SKIP_OUTPUT_BEGIN -2

class Arduino_G {
 public:
  virtual ~Arduino_G() = default;
  virtual bool begin(int32_t speed = -1) = 0;
};

class Arduino_GFX : public Arduino_G {
 public:
  Arduino_GFX(int16_t w, int16_t h)
      : WIDTH(w), HEIGHT(h), pixels((size_t)w * h, 0) {}

  bool begin(int32_t = -1) override { return true; }
  int16_t width() const { return WIDTH; }
  int16_t height() const { return HEIGHT; }
  uint16_t* getFramebuffer() { return pixels.data(); }
  const uint16_t* getFramebuffer() const { return pixels.data(); }

  void fillScreen(uint16_t color) {
    std::fill(pixels.begin(), pixels.end(), color);
  }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    if (w < 0) { x += w + 1; w = -w; }
    if (h < 0) { y += h + 1; h = -h; }
    int x0 = std::max<int>(x, 0), y0 = std::max<int>(y, 0);
    int x1 = std::min<int>(x + w, WIDTH), y1 = std::min<int>(y + h, HEIGHT);
    for (int yy = y0; yy < y1; yy++)
      for (int xx = x0; xx < x1; xx++) pixels[(size_t)yy * WIDTH + xx] = color;
  }

  void fillCircle(int16_t cx, int16_t cy, int16_t r, uint16_t color) {
    if (r < 0) return;
    int x0 = std::max<int>(cx - r, 0), x1 = std::min<int>(cx + r, WIDTH - 1);
    int y0 = std::max<int>(cy - r, 0), y1 = std::min<int>(cy + r, HEIGHT - 1);
    for (int y = y0; y <= y1; y++) {
      for (int x = x0; x <= x1; x++) {
        int dx = x - cx, dy = y - cy;
        if (dx * dx + dy * dy <= r * r) pixels[(size_t)y * WIDTH + x] = color;
      }
    }
  }

  void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                     uint16_t color) {
    if (w <= 0 || h <= 0) return;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int yy = 0; yy < h; yy++) {
      int dy = 0;
      if (yy < r) dy = r - yy;
      else if (yy >= h - r) dy = yy - (h - r - 1);
      for (int xx = 0; xx < w; xx++) {
        int dx = 0;
        if (xx < r) dx = r - xx;
        else if (xx >= w - r) dx = xx - (w - r - 1);
        if (dx * dx + dy * dy <= r * r) {
          int px = x + xx, py = y + yy;
          if (px >= 0 && px < WIDTH && py >= 0 && py < HEIGHT)
            pixels[(size_t)py * WIDTH + px] = color;
        }
      }
    }
  }

  void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                    int16_t x2, int16_t y2, uint16_t color) {
    int minx = std::min(std::min(x0, x1), x2), maxx = std::max(std::max(x0, x1), x2);
    int miny = std::min(std::min(y0, y1), y2), maxy = std::max(std::max(y0, y1), y2);
    minx = std::max(minx, 0); maxx = std::min(maxx, WIDTH - 1);
    miny = std::max(miny, 0); maxy = std::min(maxy, HEIGHT - 1);
    const int area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (!area) return;
    for (int y = miny; y <= maxy; y++) {
      for (int x = minx; x <= maxx; x++) {
        const int w0 = (x1 - x) * (y2 - y) - (y1 - y) * (x2 - x);
        const int w1 = (x2 - x) * (y0 - y) - (y2 - y) * (x0 - x);
        const int w2 = (x0 - x) * (y1 - y) - (y0 - y) * (x1 - x);
        if ((w0 >= 0 && w1 >= 0 && w2 >= 0) ||
            (w0 <= 0 && w1 <= 0 && w2 <= 0))
          pixels[(size_t)y * WIDTH + x] = color;
      }
    }
  }

  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
      drawPixel(x0, y0, color);
      if (x0 == x1 && y0 == y1) break;
      int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }

  void drawPixel(int16_t x, int16_t y, uint16_t color) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    pixels[(size_t)y * WIDTH + x] = color;
  }

  void blit(int16_t x, int16_t y, const uint16_t* src, int16_t w, int16_t h) {
    for (int yy = 0; yy < h; yy++) {
      int dy = y + yy;
      if (dy < 0 || dy >= HEIGHT) continue;
      for (int xx = 0; xx < w; xx++) {
        int dx = x + xx;
        if (dx < 0 || dx >= WIDTH) continue;
        pixels[(size_t)dy * WIDTH + dx] = src[(size_t)yy * w + xx];
      }
    }
  }

  virtual void flush(bool = false) {}

 protected:
  int16_t WIDTH, HEIGHT;
  std::vector<uint16_t> pixels;
};

class Arduino_Canvas : public Arduino_GFX {
 public:
  Arduino_Canvas(int16_t w, int16_t h, Arduino_G* output, int16_t outputX = 0,
                 int16_t outputY = 0)
      : Arduino_GFX(w, h), _output(output), _output_x(outputX), _output_y(outputY) {}

  bool begin(int32_t = -1) override { return true; }

  void flush(bool = false) override {
    if (!_output) return;
    static_cast<Arduino_GFX*>(_output)->blit(_output_x, _output_y, pixels.data(),
                                             WIDTH, HEIGHT);
  }

 protected:
  Arduino_G* _output = nullptr;
  int16_t _output_x = 0;
  int16_t _output_y = 0;
};
