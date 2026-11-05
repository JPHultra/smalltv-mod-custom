#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <map>

namespace usage {

class DeepSeekProxy {
 public:
  DeepSeekProxy() = default;
  ~DeepSeekProxy();
  bool start(const std::string& host, int port, const std::string& upstream,
             const std::string& statePath, std::string& error);
  void stop();
  bool running() const { return running_; }
  std::string snapshotJson() const;
  bool reset(std::string& error);

 private:
  struct Bucket {
    unsigned long long prompt = 0;
    unsigned long long completion = 0;
    unsigned long long total = 0;
    unsigned long long requests = 0;
  };
  void record(unsigned long long prompt, unsigned long long completion,
              unsigned long long total);
  void load();
  bool save(std::string& error) const;
  static std::string today();

  mutable std::mutex mutex_;
  std::map<std::string, Bucket> days_;
  Bucket total_;
  std::string statePath_;
  std::string upstream_;
  std::string host_ = "127.0.0.1";
  int port_ = 8789;
  struct Server;
  Server* server_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace usage
