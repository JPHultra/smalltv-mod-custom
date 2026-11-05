#include "ImageAsset.h"
#include <LittleFS.h>
#include <cstring>

namespace {
const char* kImageDir = "/img/";
const char* kImageSuffix = ".simg";

bool readHeader(File& file, ImageAssetHeader& header, uint32_t fileSize) {
  if (!file.seek(0) || file.read((uint8_t*)&header, sizeof(header)) != sizeof(header)) return false;
  if (memcmp(header.magic, IMAGE_ASSET_MAGIC, 4) || header.version != IMAGE_ASSET_VERSION) return false;
  if (header.format != IMAGE_FORMAT_RGB565 && header.format != IMAGE_FORMAT_INDEX8) return false;
  if (!header.width || !header.height || header.width > IMAGE_ASSET_MAX_DIM ||
      header.height > IMAGE_ASSET_MAX_DIM) return false;
  if (!header.frameCount || header.frameCount > IMAGE_ASSET_MAX_FRAMES) return false;
  if (header.frameTableOffset < IMAGE_ASSET_HEADER_SIZE) return false;
  if (header.frameTableOffset + (uint32_t)header.frameCount * IMAGE_ASSET_FRAME_SIZE > fileSize)
    return false;
  if (header.dataOffset > fileSize || header.totalSize != fileSize) return false;
  if (header.format == IMAGE_FORMAT_INDEX8) {
    if (header.paletteOffset < header.frameTableOffset + (uint32_t)header.frameCount * IMAGE_ASSET_FRAME_SIZE)
      return false;
    if (header.paletteOffset + IMAGE_ASSET_PALETTE_SIZE > fileSize) return false;
    if (!header.frameDelayMs) return false;
  }
  return true;
}

bool frameFits(const ImageAssetHeader& header, const ImageAssetFrame& frame, uint32_t fileSize) {
  if (frame.offset < header.dataOffset || frame.offset > fileSize) return false;
  const uint32_t expected = (uint32_t)header.width * header.height *
                            (header.format == IMAGE_FORMAT_RGB565 ? 2 : 1);
  return frame.size == expected && frame.offset + frame.size <= fileSize;
}

String imagePath(const char* name) {
  String path(kImageDir);
  path += name;
  path += kImageSuffix;
  return path;
}
}  // namespace

FsImageSource::~FsImageSource() { clear(); }

void FsImageSource::clear() {
  for (auto& entry : entries_) {
    if (entry.used && entry.file) entry.file.close();
    entry = Entry();
  }
}

FsImageSource::Entry* FsImageSource::find(const char* name) {
  for (auto& entry : entries_)
    if (entry.used && !strcmp(entry.name, name)) return &entry;
  return nullptr;
}

FsImageSource::Entry* FsImageSource::open(const char* name) {
  if (!imageAssetNameValid(name)) return nullptr;
  if (Entry* entry = find(name)) return entry;
  Entry* freeEntry = nullptr;
  for (auto& entry : entries_) {
    if (!entry.used) { freeEntry = &entry; break; }
  }
  if (!freeEntry) return nullptr;

  File file = LittleFS.open(imagePath(name), "r");
  if (!file) return nullptr;
  ImageAssetHeader header{};
  const uint32_t size = file.size();
  if (!readHeader(file, header, size)) { file.close(); return nullptr; }

  freeEntry->used = true;
  strlcpy(freeEntry->name, name, sizeof(freeEntry->name));
  freeEntry->file = file;
  freeEntry->header = header;
  freeEntry->cachedFrame = 0xFFFF;
  freeEntry->frameOffset = 0;
  freeEntry->frameSize = 0;
  if (header.format == IMAGE_FORMAT_INDEX8) {
    if (!file.seek(header.paletteOffset) ||
        file.read((uint8_t*)freeEntry->palette, IMAGE_ASSET_PALETTE_SIZE) !=
            IMAGE_ASSET_PALETTE_SIZE) {
      freeEntry->file.close();
      *freeEntry = Entry();
      return nullptr;
    }
  }
  return freeEntry;
}

bool FsImageSource::info(const char* name, ViewImageInfo& out) {
  Entry* entry = open(name);
  if (!entry) return false;
  out.width = entry->header.width;
  out.height = entry->header.height;
  out.frameCount = entry->header.frameCount;
  out.frameDelayMs = entry->header.frameDelayMs;
  out.format = entry->header.format;
  out.flags = entry->header.flags;
  return true;
}

bool FsImageSource::readRow(const char* name, uint16_t frame, uint16_t y,
                            uint16_t* colors, uint8_t* alpha, uint16_t width) {
  Entry* entry = open(name);
  if (!entry || width != entry->header.width || y >= entry->header.height ||
      frame >= entry->header.frameCount || !colors || !alpha)
    return false;
  if (entry->cachedFrame != frame) {
    ImageAssetFrame info{};
    const uint32_t at = entry->header.frameTableOffset + (uint32_t)frame * IMAGE_ASSET_FRAME_SIZE;
    if (!entry->file.seek(at) ||
        entry->file.read((uint8_t*)&info, sizeof(info)) != sizeof(info) ||
        !frameFits(entry->header, info, entry->header.totalSize))
      return false;
    entry->cachedFrame = frame;
    entry->frameOffset = info.offset;
    entry->frameSize = info.size;
  }
  if (entry->header.format == IMAGE_FORMAT_RGB565) {
    uint8_t bytes[IMAGE_ASSET_MAX_DIM * 2];
    const uint32_t at = entry->frameOffset + (uint32_t)y * width * 2;
    if (!entry->file.seek(at) || entry->file.read(bytes, width * 2) != width * 2) return false;
    for (uint16_t x = 0; x < width; x++) {
      colors[x] = (uint16_t)(bytes[x * 2] | ((uint16_t)bytes[x * 2 + 1] << 8));
      alpha[x] = 255;
    }
    return true;
  }
  uint8_t indices[IMAGE_ASSET_MAX_DIM];
  const uint32_t at = entry->frameOffset + (uint32_t)y * width;
  if (!entry->file.seek(at) || entry->file.read(indices, width) != width) return false;
  const bool transparent = entry->header.flags & IMAGE_FLAG_TRANSPARENT;
  for (uint16_t x = 0; x < width; x++) {
    const uint8_t index = indices[x];
    colors[x] = entry->palette[index];
    alpha[x] = (transparent && index == 0) ? 0 : 255;
  }
  return true;
}

bool imageAssetValidateFile(const char* path, ViewImageInfo* info) {
  File file = LittleFS.open(path, "r");
  if (!file) return false;
  ImageAssetHeader header{};
  const uint32_t size = file.size();
  if (!readHeader(file, header, size)) { file.close(); return false; }
  for (uint16_t i = 0; i < header.frameCount; i++) {
    ImageAssetFrame frame{};
    const uint32_t at = header.frameTableOffset + (uint32_t)i * IMAGE_ASSET_FRAME_SIZE;
    if (!file.seek(at) || file.read((uint8_t*)&frame, sizeof(frame)) != sizeof(frame) ||
        !frameFits(header, frame, size)) {
      file.close();
      return false;
    }
  }
  file.close();
  if (info) {
    info->width = header.width;
    info->height = header.height;
    info->frameCount = header.frameCount;
    info->frameDelayMs = header.frameDelayMs;
    info->format = header.format;
    info->flags = header.flags;
  }
  return true;
}
