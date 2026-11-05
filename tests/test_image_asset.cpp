#include "image/HostImageSource.h"
#include "image/ImageCodec.h"
#include "features/view/ImageAssetFormat.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static std::string writeAsset(const std::string& dir, const std::string& name,
                              uint8_t format, uint8_t flags, uint16_t width,
                              uint16_t height, uint16_t frames, uint16_t delay,
                              const std::vector<uint16_t>& palette,
                              const std::vector<std::vector<uint8_t>>& payload) {
  const uint32_t table = IMAGE_ASSET_HEADER_SIZE;
  const uint32_t paletteAt = format == IMAGE_FORMAT_INDEX8
                                 ? table + frames * IMAGE_ASSET_FRAME_SIZE : 0;
  const uint32_t data = format == IMAGE_FORMAT_INDEX8
                            ? paletteAt + IMAGE_ASSET_PALETTE_SIZE
                            : table + frames * IMAGE_ASSET_FRAME_SIZE;
  uint32_t total = data;
  for (auto& p : payload) total += (uint32_t)p.size();
  ImageAssetHeader h{};
  memcpy(h.magic, IMAGE_ASSET_MAGIC, 4);
  h.version = IMAGE_ASSET_VERSION;
  h.format = format;
  h.flags = flags;
  h.width = width;
  h.height = height;
  h.frameCount = frames;
  h.frameDelayMs = delay;
  h.paletteOffset = paletteAt;
  h.frameTableOffset = table;
  h.dataOffset = data;
  h.totalSize = total;

  const std::filesystem::path path = std::filesystem::path(dir) / (name + ".simg");
  std::ofstream out(path, std::ios::binary);
  out.write((const char*)&h, sizeof(h));
  uint32_t off = data;
  for (auto& p : payload) {
    ImageAssetFrame f{off, (uint32_t)p.size()};
    out.write((const char*)&f, sizeof(f));
    off += (uint32_t)p.size();
  }
  if (format == IMAGE_FORMAT_INDEX8) {
    uint16_t tablePalette[256] = {};
    for (size_t i = 0; i < palette.size() && i < 256; i++) tablePalette[i] = palette[i];
    out.write((const char*)tablePalette, sizeof(tablePalette));
  }
  for (auto& p : payload) out.write((const char*)p.data(), (std::streamsize)p.size());
  return path.string();
}

int main() {
  const std::string dir = (std::filesystem::temp_directory_path() / "smalltv-image-test").string();
  std::filesystem::create_directories(dir);

  // Static 2x2 RGB565: red, green / blue, white.
  std::vector<uint8_t> rgb = {0x00, 0xF8, 0xE0, 0x07, 0x1F, 0x00, 0xFF, 0xFF};
  const std::string still = writeAsset(dir, "still", IMAGE_FORMAT_RGB565, 0, 2, 2, 1, 0,
                                       {}, {rgb});
  imagecodec::AssetInfo info;
  std::string error;
  assert(imagecodec::readAssetInfo(still, info, error));
  assert(info.width == 2 && info.height == 2 && info.frameCount == 1);
  imagecodec::HostImageSource source(dir);
  ViewImageInfo vi;
  assert(source.info("still", vi));
  assert(vi.width == 2 && vi.height == 2 && vi.frameCount == 1);
  uint16_t row[2] = {};
  uint8_t alpha[2] = {};
  assert(source.readRow("still", 0, 0, row, alpha, 2));
  assert(row[0] == 0xF800 && row[1] == 0x07E0);
  assert(alpha[0] == 255 && alpha[1] == 255);

  // Indexed, transparent, animated 2x2. Index 0 is transparent.
  std::vector<uint16_t> palette(256, 0);
  palette[0] = 0;
  palette[1] = 0xF800;
  palette[2] = 0x07E0;
  std::vector<uint8_t> frameA = {0, 1, 2, 1};
  std::vector<uint8_t> frameB = {1, 0, 1, 2};
  const std::string animated = writeAsset(dir, "anim", IMAGE_FORMAT_INDEX8,
                                          IMAGE_FLAG_TRANSPARENT | IMAGE_FLAG_ANIMATED,
                                          2, 2, 2, 100, palette, {frameA, frameB});
  assert(imagecodec::readAssetInfo(animated, info, error));
  assert(info.frameCount == 2 && (info.flags & IMAGE_FLAG_ANIMATED));
  imagecodec::HostImageSource animatedSource(dir);
  assert(animatedSource.info("anim", vi));
  assert(vi.frameDelayMs == 100);
  assert(animatedSource.readRow("anim", 0, 0, row, alpha, 2));
  assert(alpha[0] == 0 && alpha[1] == 255 && row[1] == 0xF800);
  assert(viewImageFrameAt(vi, 250) == 0);
  assert(viewImageFrameAt(vi, 350) == 1);

  std::filesystem::remove_all(dir);
  return 0;
}
