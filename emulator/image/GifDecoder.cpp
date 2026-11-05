#include "GifDecoder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace imagecodec {
namespace {

struct Palette {
  uint8_t rgb[256 * 3] = {};
  int size = 0;
};

struct Gce {
  int delayCs = 10;
  int transparent = -1;
  int disposal = 0;
};

class Reader {
 public:
  explicit Reader(std::vector<uint8_t> data) : data_(std::move(data)) {}
  size_t left() const { return pos_ < data_.size() ? data_.size() - pos_ : 0; }
  bool u8(uint8_t& v) {
    if (pos_ >= data_.size()) return false;
    v = data_[pos_++];
    return true;
  }
  bool u16(uint16_t& v) {
    uint8_t a, b;
    if (!u8(a) || !u8(b)) return false;
    v = (uint16_t)(a | (b << 8));
    return true;
  }
  bool bytes(void* out, size_t n) {
    if (n > left()) return false;
    memcpy(out, data_.data() + pos_, n);
    pos_ += n;
    return true;
  }
  bool skip(size_t n) {
    if (n > left()) return false;
    pos_ += n;
    return true;
  }
  bool subBlocks(std::vector<uint8_t>& out) {
    for (;;) {
      uint8_t n;
      if (!u8(n)) return false;
      if (n == 0) return true;
      if (n > left()) return false;
      out.insert(out.end(), data_.begin() + pos_, data_.begin() + pos_ + n);
      pos_ += n;
    }
  }

 private:
  std::vector<uint8_t> data_;
  size_t pos_ = 0;
};

class BitReader {
 public:
  explicit BitReader(const std::vector<uint8_t>& data) : data_(data) {}
  bool read(int bits, int& value) {
    while (have_ < bits) {
      if (pos_ >= data_.size()) return false;
      buffer_ |= (uint32_t)data_[pos_++] << have_;
      have_ += 8;
    }
    value = (int)(buffer_ & ((1u << bits) - 1u));
    buffer_ >>= bits;
    have_ -= bits;
    return true;
  }
 private:
  const std::vector<uint8_t>& data_;
  size_t pos_ = 0;
  uint32_t buffer_ = 0;
  int have_ = 0;
};

bool lzwDecode(const std::vector<uint8_t>& data, int minCode, int pixels,
               std::vector<uint8_t>& out) {
  if (minCode < 2 || minCode > 8 || pixels <= 0) return false;
  const int clear = 1 << minCode;
  const int end = clear + 1;
  std::vector<uint16_t> prefix(4096);
  std::vector<uint8_t> suffix(4096);
  std::vector<uint8_t> stack(4096);
  auto reset = [&]() {
    for (int i = 0; i < clear; i++) {
      prefix[(size_t)i] = 0;
      suffix[(size_t)i] = (uint8_t)i;
    }
  };
  auto emit = [&](int code, uint8_t& first) -> bool {
    int top = 0;
    while (code >= clear) {
      if (code >= 4096 || top >= (int)stack.size()) return false;
      stack[(size_t)top++] = suffix[(size_t)code];
      code = prefix[(size_t)code];
    }
    if (code < 0 || code >= clear) return false;
    first = suffix[(size_t)code];
    if ((int)out.size() < pixels) out.push_back((uint8_t)code);
    while (top > 0 && (int)out.size() < pixels) out.push_back(stack[(size_t)--top]);
    return true;
  };

  reset();
  BitReader bits(data);
  int next = end + 1;
  int codeSize = minCode + 1;
  int old = -1;
  uint8_t first = 0;
  while ((int)out.size() < pixels) {
    int code;
    if (!bits.read(codeSize, code)) break;
    if (code == clear) {
      reset();
      next = end + 1;
      codeSize = minCode + 1;
      old = -1;
      continue;
    }
    if (code == end) break;
    if (old < 0) {
      if (code >= clear || !emit(code, first)) return false;
      old = code;
      continue;
    }
    if (code < next) {
      if (!emit(code, first)) return false;
    } else if (code == next) {
      if (!emit(old, first)) return false;
      if ((int)out.size() < pixels) out.push_back(first);
    } else {
      return false;
    }
    if (next < 4096) {
      prefix[(size_t)next] = (uint16_t)old;
      suffix[(size_t)next] = first;
      next++;
      if (next == (1 << codeSize) && codeSize < 12) codeSize++;
    }
    old = code;
  }
  return (int)out.size() >= pixels;
}

int interlacedRow(int row, int height) {
  static const int start[4] = {0, 4, 2, 1};
  static const int step[4] = {8, 8, 4, 2};
  for (int pass = 0; pass < 4; pass++) {
    if (row < 0) break;
    const int rows = (height - start[pass] + step[pass] - 1) / step[pass];
    if (row < rows) return start[pass] + row * step[pass];
    row -= rows;
  }
  return -1;
}

}  // namespace

bool decodeGif(const std::string& path, GifImage& out, std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) { error = "cannot open GIF"; return false; }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
  if (bytes.size() < 13) { error = "truncated GIF"; return false; }
  Reader r(std::move(bytes));
  char sig[6] = {};
  if (!r.bytes(sig, 6) || (memcmp(sig, "GIF87a", 6) && memcmp(sig, "GIF89a", 6))) {
    error = "not a GIF"; return false;
  }
  uint16_t width, height;
  uint8_t packed, bg, aspect;
  if (!r.u16(width) || !r.u16(height) || !r.u8(packed) || !r.u8(bg) || !r.u8(aspect)) {
    error = "truncated GIF header"; return false;
  }
  (void)bg;
  (void)aspect;
  if (!width || !height || width > 1024 || height > 1024) {
    error = "unsupported GIF dimensions"; return false;
  }
  Palette global;
  if (packed & 0x80) {
    global.size = 1 << ((packed & 7) + 1);
    if (!r.bytes(global.rgb, (size_t)global.size * 3)) {
      error = "truncated global palette"; return false;
    }
  }

  const int pixels = (int)width * (int)height;
  std::vector<uint32_t> canvas((size_t)pixels, 0);
  std::vector<uint32_t> saved;
  Gce gce;
  out = GifImage();
  out.width = width;
  out.height = height;

  for (;;) {
    uint8_t kind;
    if (!r.u8(kind)) { error = "truncated GIF"; return false; }
    if (kind == 0x3B) break;
    if (kind == 0x21) {
      uint8_t label;
      if (!r.u8(label)) { error = "truncated GIF extension"; return false; }
      if (label == 0xF9) {
        uint8_t size, flags, term;
        uint16_t delay;
        uint8_t transparent;
        if (!r.u8(size) || size < 4 || !r.u8(flags) || !r.u16(delay) ||
            !r.u8(transparent) || !r.u8(term)) {
          error = "invalid graphic control extension"; return false;
        }
        gce.delayCs = delay;
        gce.disposal = (flags >> 2) & 7;
        gce.transparent = (flags & 1) ? transparent : -1;
      } else {
        std::vector<uint8_t> ignored;
        if (!r.subBlocks(ignored)) { error = "truncated GIF extension"; return false; }
      }
      continue;
    }
    if (kind != 0x2C) { error = "invalid GIF block"; return false; }

    uint16_t fx, fy, fw, fh;
    uint8_t ipacked;
    if (!r.u16(fx) || !r.u16(fy) || !r.u16(fw) || !r.u16(fh) || !r.u8(ipacked)) {
      error = "truncated GIF image"; return false;
    }
    if (!fw || !fh || fx >= width || fy >= height ||
        (int)fx + fw > width || (int)fy + fh > height) {
      error = "invalid GIF frame bounds"; return false;
    }
    Palette local;
    const Palette* palette = &global;
    if (ipacked & 0x80) {
      local.size = 1 << ((ipacked & 7) + 1);
      if (!r.bytes(local.rgb, (size_t)local.size * 3)) {
        error = "truncated local palette"; return false;
      }
      palette = &local;
    }
    if (!palette->size) { error = "GIF has no palette"; return false; }

    uint8_t minCode;
    if (!r.u8(minCode)) { error = "truncated GIF image data"; return false; }
    std::vector<uint8_t> data;
    if (!r.subBlocks(data)) { error = "truncated GIF image data"; return false; }
    std::vector<uint8_t> indices;
    if (!lzwDecode(data, minCode, (int)fw * (int)fh, indices)) {
      error = "invalid GIF LZW data"; return false;
    }

    if (gce.disposal == 3) saved = canvas;
    const bool interlaced = (ipacked & 0x40) != 0;
    for (int row = 0; row < (int)fh; row++) {
      const int dstY = (int)fy + (interlaced ? interlacedRow(row, fh) : row);
      if (dstY < 0 || dstY >= height) continue;
      for (int x = 0; x < (int)fw; x++) {
        const uint8_t index = indices[(size_t)row * fw + (size_t)x];
        if (gce.transparent >= 0 && index == gce.transparent) continue;
        if (index >= palette->size) continue;
        const uint8_t* c = &palette->rgb[(size_t)index * 3];
        canvas[(size_t)dstY * width + (size_t)fx + (size_t)x] =
            (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16) | 0xFF000000u;
      }
    }

    GifFrame frame;
    frame.delayMs = std::max(20, gce.delayCs * 10);
    frame.rgba.resize((size_t)pixels * 4);
    for (int i = 0; i < pixels; i++) {
      const uint32_t v = canvas[(size_t)i];
      frame.rgba[(size_t)i * 4 + 0] = (uint8_t)(v & 0xFF);
      frame.rgba[(size_t)i * 4 + 1] = (uint8_t)((v >> 8) & 0xFF);
      frame.rgba[(size_t)i * 4 + 2] = (uint8_t)((v >> 16) & 0xFF);
      frame.rgba[(size_t)i * 4 + 3] = (uint8_t)((v >> 24) & 0xFF);
    }
    out.frames.push_back(std::move(frame));
    if (out.frames.size() > 500) { error = "GIF has too many frames"; return false; }

    if (gce.disposal == 2) {
      for (int y = (int)fy; y < (int)fy + (int)fh; y++)
        for (int x = (int)fx; x < (int)fx + (int)fw; x++)
          canvas[(size_t)y * width + (size_t)x] = 0;
    } else if (gce.disposal == 3) {
      canvas = saved;
    }
    gce = Gce();
  }
  if (out.frames.empty()) { error = "GIF has no frames"; return false; }
  error.clear();
  return true;
}

}  // namespace imagecodec
