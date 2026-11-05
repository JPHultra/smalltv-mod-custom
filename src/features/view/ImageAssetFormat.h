// Shared on-device image asset container. Host tools write this format and the
// firmware only reads rows from LittleFS; no image decoder runs on the device.
#pragma once
#include <stdint.h>
#include <string.h>

#define IMAGE_ASSET_MAGIC      "SIMG"
#define IMAGE_ASSET_VERSION    1
#define IMAGE_ASSET_HEADER_SIZE 32
#define IMAGE_ASSET_FRAME_SIZE  8
#define IMAGE_ASSET_PALETTE_SIZE 512
#define IMAGE_ASSET_MAX_DIM    240
#define IMAGE_ASSET_MAX_FRAMES 256
#define IMAGE_ASSET_NAME_MAX   31

#define IMAGE_FORMAT_RGB565  0
#define IMAGE_FORMAT_INDEX8  1

#define IMAGE_FLAG_TRANSPARENT 0x01
#define IMAGE_FLAG_ANIMATED    0x02

#pragma pack(push, 1)
struct ImageAssetHeader {
  char     magic[4];
  uint8_t  version;
  uint8_t  format;
  uint8_t  flags;
  uint8_t  reserved;
  uint16_t width;
  uint16_t height;
  uint16_t frameCount;
  uint16_t frameDelayMs;
  uint32_t paletteOffset;
  uint32_t frameTableOffset;
  uint32_t dataOffset;
  uint32_t totalSize;
};
struct ImageAssetFrame {
  uint32_t offset;
  uint32_t size;
};
#pragma pack(pop)

static_assert(sizeof(ImageAssetHeader) == IMAGE_ASSET_HEADER_SIZE, "image header size");
static_assert(sizeof(ImageAssetFrame) == IMAGE_ASSET_FRAME_SIZE, "image frame size");

inline bool imageAssetNameValid(const char* name) {
  if (!name || !name[0]) return false;
  const size_t length = strlen(name);
  if (length > IMAGE_ASSET_NAME_MAX) return false;
  for (size_t i = 0; i < length; i++) {
    const char c = name[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_') continue;
    return false;
  }
  return true;
}

struct ViewImageInfo {
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t frameCount = 0;
  uint16_t frameDelayMs = 0;
  uint8_t format = 0;
  uint8_t flags = 0;
};

// Implemented by the firmware LittleFS source and the emulator host source.
class ViewImageSource {
 public:
  virtual ~ViewImageSource() = default;
  virtual bool info(const char* name, ViewImageInfo& out) = 0;
  virtual bool readRow(const char* name, uint16_t frame, uint16_t y,
                       uint16_t* colors, uint8_t* alpha, uint16_t width) = 0;
};

inline uint16_t viewImageFrameAt(const ViewImageInfo& info, uint32_t elapsedMs) {
  if (!(info.flags & IMAGE_FLAG_ANIMATED) || info.frameCount <= 1 || !info.frameDelayMs)
    return 0;
  return (uint16_t)((elapsedMs / info.frameDelayMs) % info.frameCount);
}
