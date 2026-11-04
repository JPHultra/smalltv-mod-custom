#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"

struct EmuDevice {
  std::string ip;
  std::string name;
  std::string version;
  std::string variant;
  bool auth = false;
};

class DeviceManager {
 public:
  DeviceManager(std::string assetsRoot, int port);
  ~DeviceManager();

  bool start(std::string& error);
  void stop();
  std::string url() const;
  std::string target() const;
  void setTarget(const std::string& url);
  void setAuth(const std::string& user, const std::string& pass);
  std::vector<EmuDevice> discover();
  bool scanning() const { return scanning_.load(); }

 private:
  void configureRoutes();
  bool forward(const httplib::Request& req, httplib::Response& res,
               const std::string& method);
  std::vector<EmuDevice> scan();
  std::string asset(const std::string& name) const;
  std::string editorDir() const;
  std::string deviceUiPath() const;
  std::string managerPath() const;

  std::string assetsRoot_;
  int port_;
  httplib::Server server_;
  std::thread thread_;
  std::thread scanThread_;
  mutable std::mutex mutex_;
  mutable std::mutex scanMutex_;
  std::vector<EmuDevice> found_;
  std::atomic<bool> scanning_{false};
  std::string target_;
  std::string user_;
  std::string pass_;
  std::atomic<bool> running_{false};
};
