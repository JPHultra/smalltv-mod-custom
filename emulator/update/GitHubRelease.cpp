#include "GitHubRelease.h"
#include "../usage/UsageHttp.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>

namespace appupdate {
namespace {
constexpr const char* kApi = "https://api.github.com/repos/JPHultra/smalltv-mod-custom";

bool parseVersion(const std::string& value, int& a, int& b, int& c) {
  const char* text = value.c_str();
  if (*text == 'v') ++text;
  return sscanf(text, "%d.%d.%d", &a, &b, &c) == 3;
}

bool parseRelease(const std::string& body, Release& release, std::string& error) {
  JsonDocument document;
  if (deserializeJson(document, body) || !document.is<JsonObject>()) {
    error = "invalid GitHub release response";
    return false;
  }
  release = Release();
  release.tag = (const char*)(document["tag_name"] | "");
  release.body = (const char*)(document["body"] | "");
  for (JsonObject item : document["assets"].as<JsonArray>()) {
    Asset asset;
    asset.name = (const char*)(item["name"] | "");
    asset.url = (const char*)(item["browser_download_url"] | "");
    asset.digest = (const char*)(item["digest"] | "");
    asset.size = item["size"] | 0;
    if (!asset.name.empty() && !asset.url.empty()) release.assets.push_back(asset);
  }
  if (release.tag.empty()) {
    error = "release has no tag";
    return false;
  }
  return true;
}

bool apiGet(const std::string& url, std::string& body, std::string& error) {
  usage::HttpResponse response;
  if (!usage::httpRequest("GET", url,
                          {{"Accept", "application/vnd.github+json"},
                           {"User-Agent", "smalltv-emulator"}},
                          "", 20000, response)) {
    error = response.error.empty() ? "GitHub request failed" : response.error;
    return false;
  }
  if (response.status < 200 || response.status >= 300) {
    error = "GitHub HTTP " + std::to_string(response.status);
    return false;
  }
  body = std::move(response.body);
  return true;
}
}  // namespace

bool fetchLatest(Release& release, std::string& error) {
  std::string body;
  if (!apiGet(std::string(kApi) + "/releases/latest", body, error)) return false;
  return parseRelease(body, release, error);
}

bool fetchReleases(std::vector<Release>& releases, std::string& error) {
  std::string body;
  if (!apiGet(std::string(kApi) + "/releases?per_page=20", body, error)) return false;
  JsonDocument document;
  if (deserializeJson(document, body) || !document.is<JsonArray>()) {
    error = "invalid GitHub releases response";
    return false;
  }
  releases.clear();
  for (JsonObject item : document.as<JsonArray>()) {
    if ((item["draft"] | false) || (item["prerelease"] | false)) continue;
    Release release;
    std::string ignored;
    std::string encoded;
    serializeJson(item, encoded);
    if (parseRelease(encoded, release, ignored)) releases.push_back(release);
  }
  if (releases.empty()) {
    error = "no GitHub releases found";
    return false;
  }
  return true;
}

bool downloadAsset(const Asset& asset, const std::string& path, std::string& error) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    error = "cannot create download file";
    return false;
  }
  usage::HttpResponse response;
  auto sink = [&file](const char* data, size_t length) {
    file.write(data, (std::streamsize)length);
    return (bool)file;
  };
  if (!usage::httpRequest("GET", asset.url,
                          {{"User-Agent", "smalltv-emulator"}},
                          "", 120000, response, sink)) {
    file.close();
    std::remove(path.c_str());
    error = response.error.empty() ? "download failed" : response.error;
    return false;
  }
  file.close();
  if (response.status < 200 || response.status >= 300) {
    std::remove(path.c_str());
    error = "download HTTP " + std::to_string(response.status);
    return false;
  }
  return true;
}

bool newer(const std::string& tag, const std::string& current) {
  int a = 0, b = 0, c = 0;
  int x = 0, y = 0, z = 0;
  if (!parseVersion(tag, a, b, c) || !parseVersion(current, x, y, z)) return false;
  return a * 10000 + b * 100 + c > x * 10000 + y * 100 + z;
}

const char* platformAssetName() {
#if defined(_WIN32)
  return "smalltv-emulator-windows-x86_64.zip";
#else
  return "smalltv-emulator-linux-x86_64.tar.gz";
#endif
}

}  // namespace appupdate
