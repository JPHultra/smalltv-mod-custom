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

#include "md5.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

std::string read_text(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

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

DeviceManager::DeviceManager(std::string assetsRoot, int port)
    : assetsRoot_(std::move(assetsRoot)), port_(port) {}

DeviceManager::~DeviceManager() { stop(); }

std::string DeviceManager::url() const {
  return "http://127.0.0.1:" + std::to_string(port_) + "/";
}

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

std::string DeviceManager::asset(const std::string& name) const {
  std::vector<std::string> candidates = {
      assetsRoot_ + "/" + name,
      assetsRoot_ + "/emulator/" + name,
  };
  for (const auto& path : candidates) {
    if (std::ifstream(path).good()) return path;
  }
  return {};
}

std::string DeviceManager::managerPath() const {
  return asset("manager.html");
}

std::string DeviceManager::deviceUiPath() const {
  std::string path = asset("device-ui.html");
  if (!path.empty()) return path;
  return asset("src/webui.html");
}

std::string DeviceManager::editorDir() const {
  std::vector<std::string> candidates = {
      assetsRoot_ + "/editor",
      assetsRoot_ + "/../editor",
  };
  for (const auto& path : candidates) {
    if (std::ifstream(path + "/index.html").good()) return path;
  }
  return {};
}

void DeviceManager::configureRoutes() {
  server_.set_payload_max_length(8 * 1024 * 1024);
  server_.set_read_timeout(10, 0);
  server_.set_write_timeout(10, 0);

  server_.Get("/", [this](const httplib::Request&, httplib::Response& res) {
    std::string path = managerPath();
    std::string body = read_text(path);
    if (body.empty()) { res.status = 500; res.set_content("manager.html missing", "text/plain"); return; }
    res.set_content(body, "text/html; charset=utf-8");
  });
  server_.Get("/device-ui", [this](const httplib::Request&, httplib::Response& res) {
    std::string body = read_text(deviceUiPath());
    if (body.empty()) { res.status = 500; res.set_content("device UI missing", "text/plain"); return; }
    res.set_content(body, "text/html; charset=utf-8");
  });
  server_.Get("/api/local/status", [this](const httplib::Request&, httplib::Response& res) {
    JsonDocument doc;
    doc["target"] = target();
    std::string out;
    serializeJson(doc, out);
    res.set_content(out, "application/json");
  });
  server_.Get("/api/local/discover", [this](const httplib::Request&, httplib::Response& res) {
    auto devices = discover();
    JsonDocument doc;
    doc["scanning"] = scanning();
    JsonArray arr = doc["devices"].to<JsonArray>();
    for (const auto& d : devices) {
      JsonObject o = arr.add<JsonObject>();
      o["ip"] = d.ip;
      o["name"] = d.name;
      o["version"] = d.version;
      o["variant"] = d.variant;
      o["auth"] = d.auth;
    }
    std::string out;
    serializeJson(doc, out);
    res.set_content(out, "application/json");
  });
  server_.Post("/api/local/target", [this](const httplib::Request& req, httplib::Response& res) {
    JsonDocument doc;
    if (deserializeJson(doc, req.body)) { res.status = 400; res.set_content("{\"error\":\"bad json\"}", "application/json"); return; }
    std::string url = doc["url"] | "";
    if (url.rfind("http://", 0) != 0) { res.status = 400; res.set_content("{\"error\":\"device URL must start with http://\"}", "application/json"); return; }
    setTarget(url);
    res.set_content("{\"ok\":true}", "application/json");
  });
  server_.Post("/api/local/auth", [this](const httplib::Request& req, httplib::Response& res) {
    JsonDocument doc;
    if (deserializeJson(doc, req.body)) { res.status = 400; res.set_content("{\"error\":\"bad json\"}", "application/json"); return; }
    setAuth(doc["user"] | "", doc["pass"] | "");
    res.set_content("{\"ok\":true}", "application/json");
  });

  auto proxy = [this](const httplib::Request& req, httplib::Response& res) {
    forward(req, res, req.method);
  };
  server_.Get(R"(/api/.*)", proxy);
  server_.Post(R"(/api/.*)", proxy);
  server_.Put(R"(/api/.*)", proxy);
  server_.Delete(R"(/api/.*)", proxy);
  server_.Post("/update", proxy);

  std::string dir = editorDir();
  if (!dir.empty()) {
    server_.set_mount_point("/editor", dir);
    server_.Get("/editor", [dir](const httplib::Request&, httplib::Response& res) {
      std::string body = read_text(dir + "/index.html");
      res.set_content(body, "text/html; charset=utf-8");
    });
  }
}

bool DeviceManager::start(std::string& error) {
  configureRoutes();
  if (!server_.bind_to_port("127.0.0.1", port_)) {
    error = "cannot bind 127.0.0.1:" + std::to_string(port_);
    return false;
  }
  running_ = true;
  thread_ = std::thread([this] { server_.listen_after_bind(); });
  return true;
}

void DeviceManager::stop() {
  if (!running_.exchange(false)) return;
  server_.stop();
  if (thread_.joinable()) thread_.join();
  if (scanThread_.joinable()) scanThread_.join();
}

std::vector<EmuDevice> DeviceManager::discover() {
  if (!scanning_.exchange(true)) {
    if (scanThread_.joinable()) scanThread_.join();
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

std::vector<EmuDevice> DeviceManager::scan() {
  std::string local = local_ipv4();
  int a, b, c, d;
  if (sscanf(local.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return {};

  std::vector<EmuDevice> found;
  std::mutex foundMu;
  std::atomic<int> next{1};
  auto worker = [&] {
    while (true) {
      int host = next.fetch_add(1);
      if (host > 254) break;
      std::string ip = std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) + "." + std::to_string(host);
      if (ip == local) continue;
      httplib::Client cli("http://" + ip, 80);
      cli.set_connection_timeout(0, 250000);
      cli.set_read_timeout(0, 350000);
      auto res = cli.Get("/api/status");
      if (!res) continue;
      if (res->status == 200 && res->body.find("smalltv-mod") != std::string::npos) {
        EmuDevice dev;
        dev.ip = ip;
        dev.name = json_string_field(res->body, "hostname");
        if (dev.name.empty()) dev.name = json_string_field(res->body, "ssid");
        if (dev.name.empty()) dev.name = ip;
        dev.version = json_string_field(res->body, "version");
        dev.variant = json_string_field(res->body, "variant");
        std::lock_guard<std::mutex> lock(foundMu);
        found.push_back(dev);
      } else if (res->status == 401) {
        std::string challenge = res->get_header_value("WWW-Authenticate");
        if (challenge.find("Digest") != std::string::npos ||
            challenge.find("SmallTV") != std::string::npos) {
          EmuDevice dev;
          dev.ip = ip;
          dev.name = ip;
          dev.auth = true;
          std::lock_guard<std::mutex> lock(foundMu);
          found.push_back(dev);
        }
      }
    }
  };
  std::vector<std::thread> threads;
  for (int i = 0; i < 32; i++) threads.emplace_back(worker);
  for (auto& t : threads) t.join();
  std::sort(found.begin(), found.end(), [](const EmuDevice& x, const EmuDevice& y) { return x.ip < y.ip; });
  return found;
}

bool DeviceManager::forward(const httplib::Request& req, httplib::Response& res,
                            const std::string& method) {
  std::string base, user, pass;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    base = target_;
    user = user_;
    pass = pass_;
  }
  if (base.empty()) {
    res.status = 503;
    res.set_content("{\"error\":\"no device selected\"}", "application/json");
    return true;
  }
  const std::string path = req.target.empty() ? req.path : req.target;
  httplib::Headers headers;
  for (const auto& kv : req.headers) {
    if (kv.first == "Content-Type" || kv.first == "Accept" ||
        kv.first == "Content-Disposition")
      headers.emplace(kv.first, kv.second);
  }
  const std::string contentType = req.get_header_value("Content-Type");

  auto send = [&](const httplib::Headers& h) -> httplib::Result {
    httplib::Client cli(base.c_str());
    cli.set_connection_timeout(0, 700000);
    cli.set_read_timeout(20, 0);
    cli.set_write_timeout(20, 0);
    if (method == "GET") return cli.Get(path, h);
    if (method == "POST") return cli.Post(path, h, req.body, contentType.empty() ? "application/octet-stream" : contentType);
    if (method == "PUT") return cli.Put(path, h, req.body, contentType.empty() ? "application/octet-stream" : contentType);
    if (method == "DELETE") return cli.Delete(path, h, req.body, contentType.empty() ? "application/octet-stream" : contentType);
    return cli.Get(path, h);
  };

  auto result = send(headers);
  if (result && result->status == 401 && !user.empty()) {
    std::string challenge = result->get_header_value("WWW-Authenticate");
    if (challenge.find("Digest") != std::string::npos) {
      std::string auth = make_digest(challenge, user, pass, method == "GET" ? "GET" : method, path);
      if (!auth.empty()) {
        headers.emplace("Authorization", auth);
        result = send(headers);
      }
    }
  }
  if (!result) {
    res.status = 502;
    res.set_content("{\"error\":\"device request failed\"}", "application/json");
    return true;
  }
  res.status = result->status;
  res.body = result->body;
  for (const auto& key : {"Content-Type", "Content-Disposition", "Cache-Control", "Location", "WWW-Authenticate"}) {
    std::string value = result->get_header_value(key);
    if (!value.empty()) res.set_header(key, value);
  }
  if (res.get_header_value("Content-Type").empty())
    res.set_header("Content-Type", "application/octet-stream");
  return true;
}
