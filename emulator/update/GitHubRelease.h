#pragma once
#include <string>
#include <vector>

namespace appupdate {

struct Asset {
  std::string name;
  std::string url;
  std::string digest;
  unsigned long long size = 0;
};

struct Release {
  std::string tag;
  std::string body;
  std::vector<Asset> assets;
};

bool fetchLatest(Release& release, std::string& error);
bool fetchReleases(std::vector<Release>& releases, std::string& error);
bool downloadAsset(const Asset& asset, const std::string& path, std::string& error);
bool newer(const std::string& tag, const std::string& current);
const char* platformAssetName();

}  // namespace appupdate
