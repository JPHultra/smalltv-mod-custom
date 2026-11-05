#include "ImageCodec.h"
#include "GifDecoder.h"
#include "../../src/features/view/ImageAssetFormat.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"

namespace imagecodec {
namespace {

struct RgbaFrame {
  std::vector<uint8_t> pixels;
  int delayMs = 0;
};

struct Decoded {
  int width = 0;
  int height = 0;
  std::vector<RgbaFrame> frames;
};

uint32_t fnv1a(const std::vector<uint8_t>& data) {
  uint32_t h = 2166136261u;
  for (uint8_t v : data) {
    h ^= v;
    h *= 16777619u;
  }
  return h;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return !out.empty();
}

std::string extensionLower(const std::string& path) {
  std::string ext = std::filesystem::path(path).extension().string();
  for (char& c : ext) c = (char)tolower((unsigned char)c);
  return ext;
}

std::string sanitizeName(const std::string& path, uint32_t hash) {
  std::string stem = std::filesystem::path(path).stem().string();
  std::string clean;
  for (char c : stem) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_')
      clean.push_back(c);
    else if (c == ' ' || c == '.')
      clean.push_back('-');
    if (clean.size() >= 20) break;
  }
  if (clean.empty()) clean = "image";
  char suffix[16];
  snprintf(suffix, sizeof(suffix), "-%08x", hash);
  return clean + suffix;
}

bool decodeFile(const std::string& path, Decoded& out, std::string& error) {
  const std::string ext = extensionLower(path);
  if (ext == ".gif") {
    GifImage gif;
    if (!decodeGif(path, gif, error)) return false;
    out.width = gif.width;
    out.height = gif.height;
    for (auto& f : gif.frames) {
      RgbaFrame frame;
      frame.pixels = std::move(f.rgba);
      frame.delayMs = f.delayMs;
      out.frames.push_back(std::move(frame));
    }
    return !out.frames.empty();
  }

  int w = 0, h = 0, channels = 0;
  unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 4);
  if (!data) {
    error = "cannot decode image";
    return false;
  }
  if (w <= 0 || h <= 0 || w > 2048 || h > 2048) {
    stbi_image_free(data);
    error = "unsupported image dimensions";
    return false;
  }
  RgbaFrame frame;
  frame.pixels.assign(data, data + (size_t)w * h * 4);
  frame.delayMs = 0;
  stbi_image_free(data);
  out.width = w;
  out.height = h;
  out.frames.push_back(std::move(frame));
  return true;
}

void resizeFrame(const RgbaFrame& src, int sw, int sh, int dw, int dh, RgbaFrame& dst) {
  dst.delayMs = src.delayMs;
  dst.pixels.resize((size_t)dw * dh * 4);
  for (int y = 0; y < dh; y++) {
    const float sy = ((y + 0.5f) * sh / dh) - 0.5f;
    int y0 = (int)floorf(sy);
    int y1 = y0 + 1;
    float fy = sy - y0;
    if (y0 < 0) { y0 = 0; y1 = 0; fy = 0; }
    if (y1 >= sh) { y0 = sh - 1; y1 = sh - 1; fy = 0; }
    for (int x = 0; x < dw; x++) {
      const float sx = ((x + 0.5f) * sw / dw) - 0.5f;
      int x0 = (int)floorf(sx);
      int x1 = x0 + 1;
      float fx = sx - x0;
      if (x0 < 0) { x0 = 0; x1 = 0; fx = 0; }
      if (x1 >= sw) { x0 = sw - 1; x1 = sw - 1; fx = 0; }
      for (int c = 0; c < 4; c++) {
        const float a = src.pixels[((size_t)y0 * sw + x0) * 4 + c];
        const float b = src.pixels[((size_t)y0 * sw + x1) * 4 + c];
        const float d = src.pixels[((size_t)y1 * sw + x0) * 4 + c];
        const float e = src.pixels[((size_t)y1 * sw + x1) * 4 + c];
        const float top = a + (b - a) * fx;
        const float bottom = d + (e - d) * fx;
        dst.pixels[((size_t)y * dw + x) * 4 + c] =
            (uint8_t)std::clamp((int)lroundf(top + (bottom - top) * fy), 0, 255);
      }
    }
  }
}

void fitFrames(const Decoded& src, int maxDim, std::vector<RgbaFrame>& frames,
               int& width, int& height) {
  if (src.width <= maxDim && src.height <= maxDim) {
    width = src.width;
    height = src.height;
    frames = src.frames;
    return;
  }
  const float scale = std::min((float)maxDim / src.width, (float)maxDim / src.height);
  width = std::max(1, (int)lroundf(src.width * scale));
  height = std::max(1, (int)lroundf(src.height * scale));
  frames.clear();
  for (const auto& f : src.frames) {
    RgbaFrame out;
    resizeFrame(f, src.width, src.height, width, height, out);
    frames.push_back(std::move(out));
  }
}

bool paletteExact(const std::vector<RgbaFrame>& frames, bool transparent,
                  std::vector<uint16_t>& palette,
                  std::unordered_map<uint32_t, uint8_t>& mapping) {
  std::set<uint32_t> unique;
  for (const auto& f : frames) {
    for (size_t i = 0; i < f.pixels.size(); i += 4) {
      if (f.pixels[i + 3] < 128) continue;
      const uint32_t rgb = (uint32_t)f.pixels[i] |
                           ((uint32_t)f.pixels[i + 1] << 8) |
                           ((uint32_t)f.pixels[i + 2] << 16);
      unique.insert(rgb);
      if (unique.size() > 255) return false;
    }
  }
  palette.clear();
  mapping.clear();
  palette.push_back(0);
  uint8_t next = 1;
  if (!transparent) {
    // Index 0 is usable when the source has no transparency.
    next = 0;
    palette.clear();
  }
  for (uint32_t rgb : unique) {
    const uint8_t r = (uint8_t)(rgb & 0xFF);
    const uint8_t g = (uint8_t)((rgb >> 8) & 0xFF);
    const uint8_t b = (uint8_t)((rgb >> 16) & 0xFF);
    const uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    mapping[rgb] = next;
    palette.push_back(c);
    next++;
  }
  palette.resize(256, 0);
  return true;
}

void paletteCube(bool transparent, std::vector<uint16_t>& palette,
                 std::unordered_map<uint32_t, uint8_t>&) {
  palette.clear();
  const uint8_t base = transparent ? 1 : 0;
  palette.push_back(0);
  for (int r = 0; r < 6; r++)
    for (int g = 0; g < 6; g++)
      for (int b = 0; b < 6; b++) {
        const uint8_t rr = (uint8_t)(r * 51);
        const uint8_t gg = (uint8_t)(g * 51);
        const uint8_t bb = (uint8_t)(b * 51);
        palette.push_back((uint16_t)(((rr & 0xF8) << 8) | ((gg & 0xFC) << 3) | (bb >> 3)));
      }
  for (int i = 0; i < 39 && (int)palette.size() < 256; i++) {
    const uint8_t v = (uint8_t)(i * 255 / 38);
    palette.push_back((uint16_t)(((v & 0xF8) << 8) | ((v & 0xFC) << 3) | (v >> 3)));
  }
  (void)base;
  palette.resize(256, 0);
}

uint8_t mapPixel(uint8_t r, uint8_t g, uint8_t b, bool transparent,
                 const std::unordered_map<uint32_t, uint8_t>& exact) {
  const uint32_t rgb = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
  if (!exact.empty()) {
    auto it = exact.find(rgb);
    return it == exact.end() ? 0 : it->second;
  }
  const int base = transparent ? 1 : 0;
  if (std::max({r, g, b}) - std::min({r, g, b}) < 18) {
    const int gray = (r + g + b) / 3;
    const int index = base + 216 + std::min(38, gray * 39 / 256);
    return (uint8_t)std::min(255, index);
  }
  const int ri = std::min(5, r * 6 / 256);
  const int gi = std::min(5, g * 6 / 256);
  const int bi = std::min(5, b * 6 / 256);
  return (uint8_t)(base + ri * 36 + gi * 6 + bi);
}

bool writeAsset(const std::string& path, const std::vector<RgbaFrame>& frames,
                int width, int height, bool animated, bool transparent,
                std::string& error) {
  if (frames.empty() || width <= 0 || height <= 0 ||
      width > IMAGE_ASSET_MAX_DIM || height > IMAGE_ASSET_MAX_DIM) {
    error = "invalid image dimensions";
    return false;
  }
  std::vector<std::vector<uint8_t>> payload;
  std::vector<uint16_t> palette;
  std::unordered_map<uint32_t, uint8_t> exact;
  uint8_t format = IMAGE_FORMAT_RGB565;
  uint8_t flags = 0;

  if (animated) {
    format = IMAGE_FORMAT_INDEX8;
    flags |= IMAGE_FLAG_ANIMATED;
    if (transparent) flags |= IMAGE_FLAG_TRANSPARENT;
    if (!paletteExact(frames, transparent, palette, exact))
      paletteCube(transparent, palette, exact);
    for (const auto& f : frames) {
      std::vector<uint8_t> indices((size_t)width * height);
      for (int i = 0; i < width * height; i++) {
        const uint8_t* p = &f.pixels[(size_t)i * 4];
        if (p[3] < 128) {
          indices[(size_t)i] = 0;
        } else {
          indices[(size_t)i] = mapPixel(p[0], p[1], p[2], transparent, exact);
        }
      }
      payload.push_back(std::move(indices));
    }
  } else {
    const RgbaFrame& f = frames.front();
    std::vector<uint8_t> rgb((size_t)width * height * 2);
    for (int i = 0; i < width * height; i++) {
      const uint8_t* p = &f.pixels[(size_t)i * 4];
      const uint8_t a = p[3];
      uint8_t r = p[0], g = p[1], b = p[2];
      if (a < 255) {
        r = (uint8_t)(r * a / 255);
        g = (uint8_t)(g * a / 255);
        b = (uint8_t)(b * a / 255);
      }
      const uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
      rgb[(size_t)i * 2] = (uint8_t)(c & 0xFF);
      rgb[(size_t)i * 2 + 1] = (uint8_t)(c >> 8);
    }
    payload.push_back(std::move(rgb));
  }

  const uint32_t frameCount = (uint32_t)payload.size();
  const uint32_t tableOffset = IMAGE_ASSET_HEADER_SIZE;
  const uint32_t paletteOffset = animated ? tableOffset + frameCount * IMAGE_ASSET_FRAME_SIZE : 0;
  const uint32_t dataOffset = animated ? paletteOffset + IMAGE_ASSET_PALETTE_SIZE
                                       : tableOffset + frameCount * IMAGE_ASSET_FRAME_SIZE;
  uint64_t total = dataOffset;
  for (auto& p : payload) total += p.size();
  if (total > 0xFFFFFFFFull) { error = "image asset is too large"; return false; }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) { error = "cannot create image asset"; return false; }
  ImageAssetHeader header{};
  memcpy(header.magic, IMAGE_ASSET_MAGIC, 4);
  header.version = IMAGE_ASSET_VERSION;
  header.format = format;
  header.flags = flags;
  header.width = (uint16_t)width;
  header.height = (uint16_t)height;
  header.frameCount = (uint16_t)frameCount;
  header.frameDelayMs = (uint16_t)std::clamp(frames.front().delayMs > 0 ? frames.front().delayMs : 100, 20, 1000);
  header.paletteOffset = paletteOffset;
  header.frameTableOffset = tableOffset;
  header.dataOffset = dataOffset;
  header.totalSize = (uint32_t)total;
  out.write((const char*)&header, sizeof(header));
  uint32_t offset = dataOffset;
  for (const auto& p : payload) {
    ImageAssetFrame entry{};
    entry.offset = offset;
    entry.size = (uint32_t)p.size();
    out.write((const char*)&entry, sizeof(entry));
    offset += (uint32_t)p.size();
  }
  if (animated) {
    out.write((const char*)palette.data(), IMAGE_ASSET_PALETTE_SIZE);
  }
  for (const auto& p : payload) out.write((const char*)p.data(), (std::streamsize)p.size());
  out.close();
  if (!out) { error = "could not write image asset"; return false; }
  return true;
}

bool assetSize(const std::vector<RgbaFrame>& frames, int width, int height,
               bool animated, bool transparent, uint64_t& bytes) {
  const uint64_t frameCount = frames.size();
  uint64_t total = IMAGE_ASSET_HEADER_SIZE + frameCount * IMAGE_ASSET_FRAME_SIZE;
  if (animated) total += IMAGE_ASSET_PALETTE_SIZE + frameCount * (uint64_t)width * height;
  else total += (uint64_t)width * height * 2;
  bytes = total;
  (void)transparent;
  return true;
}

}  // namespace

bool importImage(const std::string& source, const std::string& cacheDir,
                 uint32_t maxBytes, ImportedAsset& out, std::string& error) {
  Decoded decoded;
  if (!decodeFile(source, decoded, error)) return false;
  if (decoded.frames.empty()) { error = "image has no frames"; return false; }

  std::vector<uint8_t> raw;
  if (!readFile(source, raw)) { error = "cannot read image"; return false; }
  const std::string name = sanitizeName(source, fnv1a(raw));
  const bool animated = decoded.frames.size() > 1;
  bool transparent = false;
  for (const auto& f : decoded.frames)
    for (size_t i = 3; i < f.pixels.size(); i += 4)
      if (f.pixels[i] < 128) { transparent = true; break; }

  // Preserve all frames when possible, then progressively reduce the image.
  std::vector<RgbaFrame> frames;
  int width = 0, height = 0;
  for (int maxDim = 240; maxDim >= 96; maxDim = maxDim > 160 ? maxDim - 24 : maxDim - 16) {
    fitFrames(decoded, maxDim, frames, width, height);
    for (size_t stride = 1; stride <= 8; stride *= 2) {
      std::vector<RgbaFrame> selected;
      for (size_t i = 0; i < frames.size(); i += stride) {
        RgbaFrame f = frames[i];
        if (stride > 1) f.delayMs = std::min(1000, f.delayMs * (int)stride);
        selected.push_back(std::move(f));
      }
      if (selected.size() > IMAGE_ASSET_MAX_FRAMES) selected.resize(IMAGE_ASSET_MAX_FRAMES);
      uint64_t bytes = 0;
      assetSize(selected, width, height, animated, transparent, bytes);
      if (bytes <= maxBytes) {
        std::error_code ec;
        std::filesystem::create_directories(cacheDir, ec);
        const std::string path = (std::filesystem::path(cacheDir) / (name + ".simg")).string();
        if (!writeAsset(path, selected, width, height, animated, transparent, error))
          return false;
        out.name = name;
        out.path = path;
        out.info.width = (uint16_t)width;
        out.info.height = (uint16_t)height;
        out.info.frameCount = (uint16_t)selected.size();
        out.info.format = animated ? IMAGE_FORMAT_INDEX8 : IMAGE_FORMAT_RGB565;
        out.info.flags = (uint8_t)((animated ? IMAGE_FLAG_ANIMATED : 0) |
                                   (transparent ? IMAGE_FLAG_TRANSPARENT : 0));
        out.info.frameDelayMs = (uint16_t)std::clamp(selected.front().delayMs > 0 ? selected.front().delayMs : 100, 20, 1000);
        out.info.bytes = bytes;
        return true;
      }
    }
    if (!animated) break;
  }
  error = "image is too large for the device";
  return false;
}

bool readAssetInfo(const std::string& path, AssetInfo& info, std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) { error = "cannot open asset"; return false; }
  ImageAssetHeader h{};
  file.read((char*)&h, sizeof(h));
  if (!file || memcmp(h.magic, IMAGE_ASSET_MAGIC, 4) || h.version != IMAGE_ASSET_VERSION) {
    error = "invalid image asset";
    return false;
  }
  if (!h.width || !h.height || !h.frameCount || h.width > IMAGE_ASSET_MAX_DIM ||
      h.height > IMAGE_ASSET_MAX_DIM || h.frameCount > IMAGE_ASSET_MAX_FRAMES) {
    error = "invalid image asset fields";
    return false;
  }
  info.width = h.width;
  info.height = h.height;
  info.frameCount = h.frameCount;
  info.frameDelayMs = h.frameDelayMs;
  info.format = h.format;
  info.flags = h.flags;
  file.seekg(0, std::ios::end);
  info.bytes = (uint64_t)file.tellg();
  return true;
}

}  // namespace imagecodec
