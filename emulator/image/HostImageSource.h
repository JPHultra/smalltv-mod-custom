#pragma once
#include <string>
#include <vector>
#include "features/view/ImageAssetFormat.h"

namespace imagecodec {

// Host implementation of ViewImageSource used by the desktop dashboard preview.
// It reads .simg files from a local cache directory.
class HostImageSource : public ViewImageSource {
 public:
  explicit HostImageSource(std::string directory) : directory_(std::move(directory)) {}
  bool info(const char* name, ViewImageInfo& out) override;
  bool readRow(const char* name, uint16_t frame, uint16_t y,
               uint16_t* colors, uint8_t* alpha, uint16_t width) override;
  void clear() { entries_.clear(); }

 private:
  struct Entry {
    bool valid = false;
    std::string name;
    std::vector<uint8_t> bytes;
    ImageAssetHeader header{};
    uint16_t palette[256] = {};
    uint16_t cachedFrame = 0xFFFF;
    uint32_t frameOffset = 0;
  };
  Entry* get(const char* name);
  std::string directory_;
  std::vector<Entry> entries_;
};

}  // namespace imagecodec
