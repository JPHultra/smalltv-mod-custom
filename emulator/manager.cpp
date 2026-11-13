#include "manager.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#include "httplib.h"
#include "md5.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <iptypes.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

std::string local_ipv4() {
#if defined(_WIN32)
  SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock == INVALID_SOCKET) return {};
#else
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) return {};
#endif
  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(80);
  inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
  sockaddr_in local{};
  socklen_t len = sizeof(local);
  std::string result;
  if (connect(sock, (sockaddr*)&remote, sizeof(remote)) == 0 &&
      getsockname(sock, (sockaddr*)&local, &len) == 0) {
    char buf[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf))) result = buf;
  }
#if defined(_WIN32)
  closesocket(sock);
#else
  close(sock);
#endif
  return result;
}

struct LocalNetworks {
  std::vector<std::string> ips;
  std::vector<std::string> subnets;
};

void add_local_network(LocalNetworks& networks, const std::string& ip) {
  int a, b, c, d;
  if (sscanf(ip.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return;
  if (a <= 0 || a >= 224 || a == 127 || (a == 169 && b == 254)) return;
  char base[32];
  snprintf(base, sizeof(base), "%d.%d.%d", a, b, c);
  if (std::find(networks.ips.begin(), networks.ips.end(), ip) == networks.ips.end())
    networks.ips.push_back(ip);
  if (std::find(networks.subnets.begin(), networks.subnets.end(), base) == networks.subnets.end())
    networks.subnets.push_back(base);
}

LocalNetworks local_networks() {
  LocalNetworks networks;
#if defined(_WIN32)
  ULONG size = 0;
  DWORD result = GetAdaptersAddresses(AF_INET,
                                      GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                          GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, nullptr, &size);
  if (result == ERROR_BUFFER_OVERFLOW) {
    std::vector<BYTE> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET,
                             GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                 GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, adapters, &size) == NO_ERROR) {
      for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast;
             unicast = unicast->Next) {
          const sockaddr* address = unicast->Address.lpSockaddr;
          if (!address || address->sa_family != AF_INET) continue;
          char text[INET_ADDRSTRLEN] = {};
          const auto* inet = reinterpret_cast<const sockaddr_in*>(address);
          if (inet_ntop(AF_INET, &inet->sin_addr, text, sizeof(text)))
            add_local_network(networks, text);
        }
      }
    }
  }
#else
  struct ifaddrs* addresses = nullptr;
  if (getifaddrs(&addresses) == 0) {
    for (auto* ifa = addresses; ifa; ifa = ifa->ifa_next) {
      if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
      if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
      char text[INET_ADDRSTRLEN] = {};
      const auto* inet = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
      if (inet_ntop(AF_INET, &inet->sin_addr, text, sizeof(text)))
        add_local_network(networks, text);
    }
    freeifaddrs(addresses);
  }
#endif
  if (networks.subnets.empty()) add_local_network(networks, local_ipv4());
  return networks;
}

std::string json_string_field(const std::string& body, const char* key) {
  std::string needle = std::string("\"") + key + "\":\"";
  size_t at = body.find(needle);
  if (at == std::string::npos) return {};
  at += needle.size();
  size_t end = body.find('"', at);
  return end == std::string::npos ? std::string() : body.substr(at, end - at);
}

std::string auth_param(const std::string& header, const char* key) {
  std::string needle = std::string(key) + "=";
  size_t at = header.find(needle);
  if (at == std::string::npos) return {};
  at += needle.size();
  if (at < header.size() && header[at] == '"') {
    size_t end = header.find('"', at + 1);
    return end == std::string::npos ? std::string() : header.substr(at + 1, end - at - 1);
  }
  size_t end = header.find_first_of(", ", at);
  return header.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

std::string make_digest(const std::string& challenge, const std::string& user,
                        const std::string& pass, const std::string& method,
                        const std::string& uri) {
  std::string realm = auth_param(challenge, "realm");
  std::string nonce = auth_param(challenge, "nonce");
  std::string opaque = auth_param(challenge, "opaque");
  std::string qop = auth_param(challenge, "qop");
  if (realm.empty() || nonce.empty() || user.empty()) return {};
  const std::string h1 = emu::md5_hex(user + ":" + realm + ":" + pass);
  const std::string h2 = emu::md5_hex(method + ":" + uri);
  std::string response;
  std::string extra;
  if (!qop.empty()) {
    const std::string cnonce = emu::md5_hex(std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count())).substr(0, 16);
    const std::string nc = "00000001";
    response = emu::md5_hex(h1 + ":" + nonce + ":" + nc + ":" + cnonce + ":auth:" + h2);
    extra = ", qop=auth, nc=" + nc + ", cnonce=\"" + cnonce + "\"";
  } else {
    response = emu::md5_hex(h1 + ":" + nonce + ":" + h2);
  }
  std::string header = "Digest username=\"" + user + "\", realm=\"" + realm +
                       "\", nonce=\"" + nonce + "\", uri=\"" + uri +
                       "\", response=\"" + response + "\"";
  if (!opaque.empty()) header += ", opaque=\"" + opaque + "\"";
  return header + extra;
}

}  // namespace

DeviceManager::~DeviceManager() { stop(); }

std::string DeviceManager::target() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return target_;
}

void DeviceManager::setTarget(const std::string& url) {
  std::lock_guard<std::mutex> lock(mutex_);
  target_ = url;
}

void DeviceManager::setAuth(const std::string& user, const std::string& pass) {
  std::lock_guard<std::mutex> lock(mutex_);
  user_ = user;
  pass_ = pass;
}

void DeviceManager::stop() {
  if (scanThread_.joinable()) scanThread_.join();
}

std::vector<EmuDevice> DeviceManager::discover() {
  if (!scanning_.exchange(true)) {
    scanCancel_ = false;
    if (scanThread_.joinable()) scanThread_.join();
    scanDone_ = 0;
    scanTotal_ = 0;
    scanFound_ = 0;
    {
      std::lock_guard<std::mutex> lock(scanMutex_);
      scanSubnet_.clear();
      scanError_.clear();
    }
    scanThread_ = std::thread([this] {
      auto result = scan();
      {
        std::lock_guard<std::mutex> lock(scanMutex_);
        found_ = std::move(result);
      }
      scanning_ = false;
    });
  }
  std::lock_guard<std::mutex> lock(scanMutex_);
  return found_;
}

void DeviceManager::cancelDiscovery() {
  if (scanning_.load()) scanCancel_ = true;
}

std::vector<EmuDevice> DeviceManager::knownDevices() const {
  std::lock_guard<std::mutex> lock(scanMutex_);
  return found_;
}

ScanState DeviceManager::scanState() const {
  ScanState state;
  state.running = scanning_.load();
  state.done = scanDone_.load();
  state.total = scanTotal_.load();
  state.found = scanFound_.load();
  std::lock_guard<std::mutex> lock(scanMutex_);
  state.subnet = scanSubnet_;
  state.error = scanError_;
  return state;
}

std::vector<EmuDevice> DeviceManager::scan() {
  LocalNetworks local = local_networks();
  if (local.subnets.empty()) {
    std::lock_guard<std::mutex> lock(scanMutex_);
    scanError_ = "No local IPv4 address found";
    return {};
  }

  std::vector<std::string> targets;
  std::string subnetText;
  for (const auto& base : local.subnets) {
    if (!subnetText.empty()) subnetText += ", ";
    subnetText += base + ".0/24";
    for (int host = 1; host <= 254; host++) {
      std::string ip = base + "." + std::to_string(host);
      if (std::find(local.ips.begin(), local.ips.end(), ip) != local.ips.end()) continue;
      targets.push_back(ip);
    }
  }
  {
    std::lock_guard<std::mutex> lock(scanMutex_);
    scanSubnet_ = subnetText;
  }
  scanDone_ = 0;
  scanFound_ = 0;
  scanTotal_ = (int)targets.size();

  std::vector<EmuDevice> found;
  std::mutex foundMu;
  std::atomic<int> next{0};
  auto worker = [&] {
    while (true) {
      if (scanCancel_.load()) break;
      int index = next.fetch_add(1);
      if (index >= (int)targets.size()) break;
      const std::string& ip = targets[index];
      httplib::Client cli(ip, 80);
      cli.set_connection_timeout(0, 250000);
      cli.set_read_timeout(0, 350000);
      auto res = cli.Get("/api/status");
      if (res && res->status == 200 &&
          res->body.find("smalltv-mod") != std::string::npos) {
        EmuDevice dev;
        dev.ip = ip;
        dev.name = json_string_field(res->body, "hostname");
        if (dev.name.empty()) dev.name = json_string_field(res->body, "ssid");
        if (dev.name.empty()) dev.name = ip;
        dev.version = json_string_field(res->body, "version");
        dev.variant = json_string_field(res->body, "variant");
        std::lock_guard<std::mutex> lock(foundMu);
        found.push_back(dev);
        scanFound_.fetch_add(1);
      } else if (res && res->status == 401) {
        std::string challenge = res->get_header_value("WWW-Authenticate");
        if (challenge.find("Digest") != std::string::npos ||
            challenge.find("SmallTV") != std::string::npos) {
          EmuDevice dev;
          dev.ip = ip;
          dev.name = ip;
          dev.auth = true;
          std::lock_guard<std::mutex> lock(foundMu);
          found.push_back(dev);
          scanFound_.fetch_add(1);
        }
      }
      scanDone_.fetch_add(1);
    }
  };
  std::vector<std::thread> threads;
  for (int i = 0; i < 32; i++) threads.emplace_back(worker);
  for (auto& t : threads) t.join();
  std::sort(found.begin(), found.end(), [](const EmuDevice& x, const EmuDevice& y) { return x.ip < y.ip; });
  return found;
}

bool DeviceManager::request(const std::string& method, const std::string& path,
                            const std::string& body, const std::string& contentType,
                            DeviceResponse& response, ProgressCallback progress) {
  std::string base, user, pass;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    base = target_;
    user = user_;
    pass = pass_;
  }
  if (base.empty()) {
    response.status = 503;
    response.body = "{\"error\":\"no device selected\"}";
    response.contentType = "application/json";
    return true;
  }
  httplib::Headers headers;
  headers.emplace("Accept", "application/json");
  auto send = [&](const httplib::Headers& h) -> httplib::Result {
    httplib::Client cli(base.c_str());
    cli.set_connection_timeout(0, 700000);
    const int timeoutSec = (path == "/update" || path == "/api/images" ||
                            path.rfind("/api/image", 0) == 0) ? 60 : 5;
    cli.set_read_timeout(timeoutSec, 0);
    cli.set_write_timeout(timeoutSec, 0);
    if (method == "GET") return cli.Get(path, h);
    if (method == "POST") return cli.Post(path, h, body, contentType.empty() ? "application/octet-stream" : contentType, progress);
    if (method == "PUT") return cli.Put(path, h, body, contentType.empty() ? "application/octet-stream" : contentType, progress);
    if (method == "DELETE") return cli.Delete(path, h, body, contentType.empty() ? "application/octet-stream" : contentType);
    return cli.Get(path, h);
  };
  auto result = send(headers);
  if (result && result->status == 401 && !user.empty()) {
    std::string challenge = result->get_header_value("WWW-Authenticate");
    if (challenge.find("Digest") != std::string::npos) {
      std::string auth = make_digest(challenge, user, pass, method, path);
      if (!auth.empty()) {
        headers.emplace("Authorization", auth);
        result = send(headers);
      }
    }
  }
  if (!result) return false;
  response.status = result->status;
  response.body = result->body;
  response.contentType = result->get_header_value("Content-Type");
  return true;
}

bool DeviceManager::uploadImage(const std::string& path, const std::string& name,
                                std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) { error = "cannot open image asset"; return false; }
  std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (data.empty()) { error = "image asset is empty"; return false; }
  const std::string boundary = "----SmallTVImageBoundary";
  std::string body;
  body.reserve(data.size() + 256);
  body += "--" + boundary + "\r\n";
  body += "Content-Disposition: form-data; name=\"image\"; filename=\"" + name + ".simg\"\r\n";
  body += "Content-Type: application/octet-stream\r\n\r\n";
  body += data;
  body += "\r\n--" + boundary + "--\r\n";
  DeviceResponse response;
  if (!request("POST", "/api/images", body, "multipart/form-data; boundary=" + boundary,
               response)) {
    error = "device request failed";
    return false;
  }
  if (response.status != 200) {
    error = "device returned HTTP " + std::to_string(response.status) +
            (response.body.empty() ? "" : ": " + response.body.substr(0, 160));
    return false;
  }
  return true;
}

bool DeviceManager::downloadImage(const std::string& name, const std::string& path,
                                  std::string& error) {
  DeviceResponse response;
  if (!request("GET", "/api/image?name=" + name, "", "", response)) {
    error = "device request failed";
    return false;
  }
  if (response.status != 200) {
    error = "device returned HTTP " + std::to_string(response.status);
    return false;
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) { error = "cannot write image cache"; return false; }
  file.write(response.body.data(), (std::streamsize)response.body.size());
  if (!file) { error = "cannot write image cache"; return false; }
  return true;
}

bool DeviceManager::uploadFirmware(const std::string& path, std::string& error) {
  uploadDone_ = 0;
  uploadTotal_ = 0;
  std::ifstream file(path, std::ios::binary);
  if (!file) { error = "cannot open firmware file"; return false; }
  std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (data.empty()) { error = "firmware file is empty"; return false; }
  const std::string boundary = "----SmallTVEmulatorBoundary";
  std::string body;
  body.reserve(data.size() + 256);
  body += "--" + boundary + "\r\n";
  body += "Content-Disposition: form-data; name=\"firmware\"; filename=\"firmware.bin\"\r\n";
  body += "Content-Type: application/octet-stream\r\n\r\n";
  body += data;
  body += "\r\n--" + boundary + "--\r\n";
  DeviceResponse response;
  auto progress = [this](size_t current, size_t total) {
    uploadDone_ = current;
    uploadTotal_ = total;
    return true;
  };
  if (!request("POST", "/update", body, "multipart/form-data; boundary=" + boundary,
               response, progress)) {
    error = "device request failed";
    return false;
  }
  uploadDone_ = uploadTotal_.load();
  if (response.status != 200) {
    error = "device returned HTTP " + std::to_string(response.status) + (response.body.empty() ? "" : ": " + response.body.substr(0, 120));
    return false;
  }
  return true;
}
