#include "Worker.h"
#include "DeepSeekProxy.h"
#include "SecureStore.h"
#include "UsageCore.h"
#include "UsageHttp.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace usage {
namespace {
void logMessage(const std::string& message) {
  std::fprintf(stderr, "[usage-worker] %s\n", message.c_str());
}
}  // namespace

int runWorker(const std::string& deviceUrl, int intervalSeconds, bool enableProxy,
              bool once) {
  std::string error;
#if defined(_WIN32)
  const char* base = std::getenv("APPDATA");
  const std::string statePath = std::string(base ? base : ".") + "/smalltv/deepseek-usage.json";
#else
  const char* home = std::getenv("HOME");
  const std::string statePath = std::string(home ? home : ".") + "/.config/smalltv/deepseek-usage.json";
#endif
  DeepSeekProxy proxy;
  if (enableProxy && !proxy.start("127.0.0.1", 8789, "https://api.deepseek.com",
                                  statePath, error)) {
    logMessage("proxy: " + error);
  }
  const int interval = intervalSeconds > 5 ? intervalSeconds : 60;
  do {
    std::string credentials;
    if (!secureLoad(credentials, error)) {
      logMessage("credentials: " + error);
      credentials = credentialsTemplate();
    }
    if (credentials.empty()) credentials = credentialsTemplate();
    FetchResult fetched = fetchAll(credentials, "http://127.0.0.1:8789/usage", 15000);
    if (!fetched.error.empty()) {
      logMessage("fetch: " + fetched.error);
    } else {
      std::string saveError;
      if (!secureSave(fetched.credentialsJson, saveError))
        logMessage("credentials save: " + saveError);
      if (!deviceUrl.empty()) {
        const std::string body = normalizedJson(fetched.providers);
        HttpResponse response;
        if (!httpRequest("POST", deviceUrl + "/api/usage",
                         {{"Content-Type", "application/json"}, {"Accept", "application/json"}},
                         body, 10000, response) ||
            response.status < 200 || response.status >= 300) {
          logMessage("push failed: " + (response.error.empty()
                                            ? "HTTP " + std::to_string(response.status)
                                            : response.error));
        }
      }
    }
    if (once) break;
    std::this_thread::sleep_for(std::chrono::seconds(interval));
  } while (true);
  proxy.stop();
  return 0;
}

}  // namespace usage
