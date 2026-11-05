#pragma once
#include "ImageAssetFormat.h"
#include "config.h"
#include <FS.h>

// Firmware-side reader. It keeps up to VIEW_MAX_IMAGES files open so animated
// rows do not reopen LittleFS for every scanline.
class FsImageSource : public ViewImageSource {
 public:
  ~FsImageSource() override;
  void clear();
  bool info(const char* name, ViewImageInfo& out) override;
  bool readRow(const char* name, uint16_t frame, uint16_t y,
               uint16_t* colors, uint8_t* alpha, uint16_t width) override;

 private:
  struct Entry {
    bool used = false;
    char name[VIEW_IMAGE_NAME_LEN] = {};
    File file;
    ImageAssetHeader header{};
    uint16_t palette[256] = {};
    uint16_t cachedFrame = 0xFFFF;
    uint32_t frameOffset = 0;
    uint32_t frameSize = 0;
  };
  Entry* find(const char* name);
  Entry* open(const char* name);

  Entry entries_[VIEW_MAX_IMAGES];
};

// Validate a complete .simg file on LittleFS before accepting an upload.
bool imageAssetValidateFile(const char* path, ViewImageInfo* info = nullptr);
