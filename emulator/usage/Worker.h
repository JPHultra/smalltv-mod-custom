#pragma once
#include <string>

namespace usage {
int runWorker(const std::string& deviceUrl, int intervalSeconds, bool enableProxy,
              bool once);
}
