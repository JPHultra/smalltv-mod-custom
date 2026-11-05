#pragma once
#include <string>
#include <vector>

namespace usage {

struct Metric {
  std::string label;
  double pct = 0;
  int resetMin = 0;
  std::string value;
};

struct Result {
  std::string id;
  std::string name;
  std::string status;
  std::string error;
  bool ok = false;
  std::vector<Metric> metrics;
};

struct FetchResult {
  std::vector<Result> providers;
  std::string credentialsJson;
  std::string error;
};

std::string credentialsTemplate();
FetchResult fetchAll(const std::string& credentialsJson,
                     const std::string& deepseekUsageUrl, int timeoutMs = 15000);
std::string normalizedJson(const std::vector<Result>& providers);

}  // namespace usage
