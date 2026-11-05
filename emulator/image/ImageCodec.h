#pragma once
#include <cstdint>
#include <string>

namespace imagecodec {

struct AssetInfo {
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t frameCount = 0;
  uint16_t frameDelayMs = 0;
  uint8_t format = 0;
  uint8_t flags = 0;
  uint64_t bytes = 0;
};

struct ImportedAsset {
  std::string name;
  std::string path;
  AssetInfo info;
};

// Decode PNG/JPEG/BMP/GIF on the desktop, convert to the smallest device asset
// that fits maxBytes, and write <cacheDir>/<name>.simg.
bool importImage(const std::string& source, const std::string& cacheDir,
                 uint32_t maxBytes, ImportedAsset& out, std::string& error);

bool readAssetInfo(const std::string& path, AssetInfo& info, std::string& error);

}  // namespace imagecodec
