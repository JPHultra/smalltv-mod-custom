#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct DeviceResponse {
  int status = 0;
  std::string body;
  std::string contentType;
};

struct EmuDevice {
  std::string ip;
  std::string name;
  std::string version;
  std::string variant;
  bool auth = false;
};

struct ScanState {
  bool running = false;
  int done = 0;
  int total = 0;
  int found = 0;
  std::string subnet;
  std::string error;
};

class DeviceManager {
 public:
  DeviceManager() = default;
  ~DeviceManager();

  void stop();
  std::string target() const;
  void setTarget(const std::string& url);
  void setAuth(const std::string& user, const std::string& pass);
  std::vector<EmuDevice> discover();
  std::vector<EmuDevice> knownDevices() const;
  ScanState scanState() const;
  bool request(const std::string& method, const std::string& path,
               const std::string& body, const std::string& contentType,
               DeviceResponse& response);
  bool uploadFirmware(const std::string& path, std::string& error);
  bool scanning() const { return scanning_.load(); }

 private:
  std::vector<EmuDevice> scan();

  std::thread scanThread_;
  mutable std::mutex mutex_;
  mutable std::mutex scanMutex_;
  std::vector<EmuDevice> found_;
  std::atomic<bool> scanning_{false};
  std::atomic<int> scanDone_{0};
  std::atomic<int> scanTotal_{0};
  std::atomic<int> scanFound_{0};
  std::string scanSubnet_;
  std::string scanError_;
  std::string target_;
  std::string user_;
  std::string pass_;
};
