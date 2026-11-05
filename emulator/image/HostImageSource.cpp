#include "HostImageSource.h"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace imagecodec {
namespace {
bool validHeader(const ImageAssetHeader& h, uint32_t size) {
  if (memcmp(h.magic, IMAGE_ASSET_MAGIC, 4) || h.version != IMAGE_ASSET_VERSION) return false;
  if (h.format != IMAGE_FORMAT_RGB565 && h.format != IMAGE_FORMAT_INDEX8) return false;
  if (!h.width || !h.height || h.width > IMAGE_ASSET_MAX_DIM || h.height > IMAGE_ASSET_MAX_DIM)
    return false;
  if (!h.frameCount || h.frameCount > IMAGE_ASSET_MAX_FRAMES) return false;
  if (h.frameTableOffset < IMAGE_ASSET_HEADER_SIZE ||
      h.frameTableOffset + (uint32_t)h.frameCount * IMAGE_ASSET_FRAME_SIZE > size)
    return false;
  if (h.dataOffset > size || h.totalSize != size) return false;
  if (h.format == IMAGE_FORMAT_INDEX8 &&
      (h.paletteOffset + IMAGE_ASSET_PALETTE_SIZE > size || !h.frameDelayMs))
    return false;
  return true;
}
}  // namespace

HostImageSource::Entry* HostImageSource::get(const char* name) {
  if (!imageAssetNameValid(name)) return nullptr;
  for (auto& e : entries_)
    if (e.valid && e.name == name) return &e;
  if (entries_.size() >= 8) return nullptr;

  Entry entry;
  entry.name = name;
  const std::filesystem::path path =
      std::filesystem::path(directory_) / (std::string(name) + ".simg");
  std::ifstream file(path, std::ios::binary);
  if (!file) return nullptr;
  entry.bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  if (entry.bytes.size() < sizeof(ImageAssetHeader)) return nullptr;
  memcpy(&entry.header, entry.bytes.data(), sizeof(entry.header));
  if (!validHeader(entry.header, (uint32_t)entry.bytes.size())) return nullptr;
  if (entry.header.format == IMAGE_FORMAT_INDEX8) {
    memcpy(entry.palette, entry.bytes.data() + entry.header.paletteOffset,
           IMAGE_ASSET_PALETTE_SIZE);
  }
  entry.valid = true;
  entries_.push_back(std::move(entry));
  return &entries_.back();
}

bool HostImageSource::info(const char* name, ViewImageInfo& out) {
  Entry* e = get(name);
  if (!e) return false;
  out.width = e->header.width;
  out.height = e->header.height;
  out.frameCount = e->header.frameCount;
  out.frameDelayMs = e->header.frameDelayMs;
  out.format = e->header.format;
  out.flags = e->header.flags;
  return true;
}

bool HostImageSource::readRow(const char* name, uint16_t frame, uint16_t y,
                              uint16_t* colors, uint8_t* alpha, uint16_t width) {
  Entry* e = get(name);
  if (!e || width != e->header.width || y >= e->header.height ||
      frame >= e->header.frameCount || !colors || !alpha)
    return false;
  ImageAssetFrame fi{};
  const uint32_t at = e->header.frameTableOffset + (uint32_t)frame * IMAGE_ASSET_FRAME_SIZE;
  if (at + sizeof(fi) > e->bytes.size()) return false;
  memcpy(&fi, e->bytes.data() + at, sizeof(fi));
  const uint32_t expected = (uint32_t)e->header.width * e->header.height *
                            (e->header.format == IMAGE_FORMAT_RGB565 ? 2 : 1);
  if (fi.size != expected || fi.offset < e->header.dataOffset ||
      (uint64_t)fi.offset + fi.size > e->bytes.size())
    return false;

  if (e->header.format == IMAGE_FORMAT_RGB565) {
    const uint32_t row = fi.offset + (uint32_t)y * width * 2;
    for (uint16_t x = 0; x < width; x++) {
      colors[x] = (uint16_t)(e->bytes[row + x * 2] |
                             ((uint16_t)e->bytes[row + x * 2 + 1] << 8));
      alpha[x] = 255;
    }
  } else {
    const uint32_t row = fi.offset + (uint32_t)y * width;
    const bool transparent = e->header.flags & IMAGE_FLAG_TRANSPARENT;
    for (uint16_t x = 0; x < width; x++) {
      const uint8_t index = e->bytes[row + x];
      colors[x] = e->palette[index];
      alpha[x] = (transparent && index == 0) ? 0 : 255;
    }
  }
  return true;
}

}  // namespace imagecodec
