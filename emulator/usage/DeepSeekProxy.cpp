#include "DeepSeekProxy.h"
#include "UsageHttp.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <deque>
#include <condition_variable>
#include <mutex>

#include "httplib.h"

namespace usage {
namespace {
std::string lower(std::string value) {
  for (char& c : value) c = (char)std::tolower((unsigned char)c);
  return value;
}

std::string text(JsonVariantConst value) {
  const char* out = value | "";
  return out ? out : "";
}

unsigned long long numberValue(JsonVariantConst value) {
  if (value.is<unsigned long long>()) return value.as<unsigned long long>();
  if (value.is<double>()) return (unsigned long long)std::max(0.0, value.as<double>());
  if (value.is<const char*>()) {
    char* end = nullptr;
    const char* first = value.as<const char*>();
    if (!first || !*first) return 0;
    double parsed = std::strtod(first, &end);
    return end != first && parsed > 0 ? (unsigned long long)parsed : 0;
  }
  return 0;
}

}  // namespace

struct DeepSeekProxy::Server {
  httplib::Server server;
};

DeepSeekProxy::~DeepSeekProxy() { stop(); }

std::string DeepSeekProxy::today() {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1,
           tm.tm_mday);
  return buffer;
}

void DeepSeekProxy::load() {
  std::lock_guard<std::mutex> lock(mutex_);
  days_.clear();
  total_ = Bucket();
  std::ifstream file(statePath_);
  if (!file) return;
  JsonDocument document;
  if (deserializeJson(document, file)) return;
  JsonObject root = document.as<JsonObject>();
  if (root.isNull()) return;
  total_.prompt = numberValue(root["prompt_tokens"]);
  total_.completion = numberValue(root["completion_tokens"]);
  total_.total = numberValue(root["total_tokens"]);
  total_.requests = numberValue(root["requests"]);
  JsonObject days = root["days"].as<JsonObject>();
  for (JsonPair item : days) {
    JsonObject value = item.value().as<JsonObject>();
    if (value.isNull()) continue;
    Bucket bucket;
    bucket.prompt = numberValue(value["prompt_tokens"]);
    bucket.completion = numberValue(value["completion_tokens"]);
    bucket.total = numberValue(value["total_tokens"]);
    bucket.requests = numberValue(value["requests"]);
    days_[item.key().c_str()] = bucket;
  }
}

bool DeepSeekProxy::save(std::string& error) const {
  JsonDocument document;
  JsonObject root = document.to<JsonObject>();
  root["prompt_tokens"] = total_.prompt;
  root["completion_tokens"] = total_.completion;
  root["total_tokens"] = total_.total;
  root["requests"] = total_.requests;
  JsonObject days = root["days"].to<JsonObject>();
  for (const auto& entry : days_) {
    JsonObject bucket = days[entry.first].to<JsonObject>();
    bucket["prompt_tokens"] = entry.second.prompt;
    bucket["completion_tokens"] = entry.second.completion;
    bucket["total_tokens"] = entry.second.total;
    bucket["requests"] = entry.second.requests;
  }
  std::error_code ec;
  const auto path = std::filesystem::path(statePath_);
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    error = "cannot create proxy state directory";
    return false;
  }
  std::string temporary = statePath_ + ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
      error = "cannot write proxy state";
      return false;
    }
    serializeJson(document, file);
  }
  std::filesystem::rename(temporary, statePath_, ec);
  if (ec) {
    error = "cannot replace proxy state";
    return false;
  }
  return true;
}

void DeepSeekProxy::record(unsigned long long prompt, unsigned long long completion,
                           unsigned long long total) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string day = today();
  Bucket& bucket = days_[day];
  bucket.prompt += prompt;
  bucket.completion += completion;
  bucket.total += total;
  bucket.requests++;
  total_.prompt += prompt;
  total_.completion += completion;
  total_.total += total;
  total_.requests++;
  while (days_.size() > 32) days_.erase(days_.begin());
  std::string error;
  save(error);
}

std::string DeepSeekProxy::snapshotJson() const {
  std::lock_guard<std::mutex> lock(mutex_);
  JsonDocument document;
  JsonObject root = document.to<JsonObject>();
  root["prompt_tokens"] = total_.prompt;
  root["completion_tokens"] = total_.completion;
  root["total_tokens"] = total_.total;
  root["requests"] = total_.requests;
  const std::string day = today();
  JsonObject todayBucket = root["today"].to<JsonObject>();
  const auto it = days_.find(day);
  if (it != days_.end()) {
    todayBucket["prompt_tokens"] = it->second.prompt;
    todayBucket["completion_tokens"] = it->second.completion;
    todayBucket["total_tokens"] = it->second.total;
    todayBucket["requests"] = it->second.requests;
  } else {
    todayBucket["prompt_tokens"] = 0;
    todayBucket["completion_tokens"] = 0;
    todayBucket["total_tokens"] = 0;
    todayBucket["requests"] = 0;
  }
  std::string output;
  serializeJson(document, output);
  return output;
}

bool DeepSeekProxy::reset(std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  days_.clear();
  total_ = Bucket();
  return save(error);
}

bool DeepSeekProxy::start(const std::string& host, int port, const std::string& upstream,
                          const std::string& statePath, std::string& error) {
  if (running_) return true;
  host_ = host;
  port_ = port;
  upstream_ = upstream;
  statePath_ = statePath;
  load();
  server_ = new Server();
  auto& server = server_->server;
  server.set_pre_routing_handler([this](const httplib::Request& request,
                                        httplib::Response& response) {
    if (request.method == "GET" && request.path == "/usage") {
      response.set_content(snapshotJson(), "application/json");
      return httplib::Server::HandlerResponse::Handled;
    }
    if (request.method == "POST" && request.path == "/usage/reset") {
      std::string resetError;
      reset(resetError);
      response.status = resetError.empty() ? 200 : 500;
      response.set_content(resetError.empty() ? "{\"ok\":true}" : "{\"ok\":false}",
                           "application/json");
      return httplib::Server::HandlerResponse::Handled;
    }
    if (request.method == "OPTIONS") {
      response.status = 204;
      response.set_header("Access-Control-Allow-Origin", "*");
      response.set_header("Access-Control-Allow-Headers", "Authorization, Content-Type");
      response.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
      return httplib::Server::HandlerResponse::Handled;
    }

    std::vector<Header> headers;
    for (const auto& item : request.headers) {
      const std::string key = lower(item.first);
      if (key == "host" || key == "content-length" || key == "connection" ||
          key == "transfer-encoding" || key == "accept-encoding")
        continue;
      headers.emplace_back(item.first, item.second);
    }
    headers.emplace_back("Accept-Encoding", "identity");
    std::string body = request.body;
    if (!request.path.rfind("/anthropic", 0) && !body.empty()) {
      JsonDocument document;
      if (!deserializeJson(document, body) && document.is<JsonObject>() &&
          document["stream"] == true) {
        JsonObject options = document["stream_options"].as<JsonObject>();
        if (options.isNull()) options = document["stream_options"].to<JsonObject>();
        options["include_usage"] = true;
        body.clear();
        serializeJson(document, body);
      }
    }

    struct StreamState {
      std::mutex mutex;
      std::condition_variable cv;
      std::deque<std::string> chunks;
      std::string body;
      std::string pending;
      std::string error;
      std::string contentType;
      long status = 0;
      bool headReady = false;
      bool isSse = false;
      bool done = false;
      bool ok = false;
      bool canceled = false;
      bool anthropic = false;
      unsigned long long prompt = 0;
      unsigned long long completion = 0;
      unsigned long long total = 0;
      unsigned long long input = 0;
      unsigned long long output = 0;
      bool counted = false;
    };

    auto state = std::make_shared<StreamState>();
    const std::string method = request.method;
    const std::string target = upstream_ + (request.target.empty() ? request.path : request.target);
    std::thread([state, method, target, headers, body] {
      HttpResponse upstream;
      auto onHead = [state](const HttpResponse& response) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->status = response.status;
        state->contentType = response.contentType;
        state->isSse = lower(response.contentType).find("text/event-stream") != std::string::npos;
        state->headReady = true;
        state->cv.notify_all();
        return !state->canceled;
      };
      auto onData = [state](const char* data, size_t length) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->canceled) return false;
        if (state->isSse) {
          state->pending.append(data, length);
          size_t newline;
          while ((newline = state->pending.find('\n')) != std::string::npos) {
            std::string line = state->pending.substr(0, newline);
            state->pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("data:", 0) != 0) continue;
            JsonDocument event;
            if (deserializeJson(event, line.substr(5))) continue;
            JsonObject usage = event["usage"].as<JsonObject>();
            if (!usage.isNull()) {
              state->prompt += numberValue(usage["prompt_tokens"].isNull() ? usage["input_tokens"] : usage["prompt_tokens"]);
              state->completion += numberValue(usage["completion_tokens"].isNull() ? usage["output_tokens"] : usage["completion_tokens"]);
              state->total += numberValue(usage["total_tokens"]);
            }
            const std::string type = text(event["type"]);
            if (type == "message_start") {
              JsonObject message = event["message"].as<JsonObject>();
              JsonObject usage = message["usage"].as<JsonObject>();
              state->anthropic = true;
              state->input = numberValue(usage["input_tokens"]);
              state->output = numberValue(usage["output_tokens"]);
              state->counted = false;
            } else if (type == "message_delta") {
              JsonObject usage = event["usage"].as<JsonObject>();
              state->anthropic = true;
              state->output = numberValue(usage["output_tokens"]);
            } else if (type == "message_stop") {
              state->anthropic = true;
            }
          }
        } else {
          state->body.append(data, length);
        }
        state->chunks.emplace_back(data, length);
        state->cv.notify_all();
        return true;
      };
      bool ok = httpRequest(method, target, headers, body, 300000, upstream, onData, onHead);
      std::lock_guard<std::mutex> lock(state->mutex);
      state->done = true;
      state->ok = ok;
      state->error = upstream.error;
      if (state->status == 0) state->status = upstream.status;
      if (state->contentType.empty()) state->contentType = upstream.contentType;
      if (state->isSse && !state->pending.empty()) {
        std::string line = state->pending;
        state->pending.clear();
        if (line.rfind("data:", 0) == 0) {
          JsonDocument event;
          if (!deserializeJson(event, line.substr(5))) {
            JsonObject usage = event["usage"].as<JsonObject>();
            if (!usage.isNull()) {
              state->prompt += numberValue(usage["prompt_tokens"].isNull() ? usage["input_tokens"] : usage["prompt_tokens"]);
              state->completion += numberValue(usage["completion_tokens"].isNull() ? usage["output_tokens"] : usage["completion_tokens"]);
              state->total += numberValue(usage["total_tokens"]);
            }
          }
        }
      }
      state->cv.notify_all();
    }).detach();

    {
      std::unique_lock<std::mutex> lock(state->mutex);
      if (!state->cv.wait_for(lock, std::chrono::seconds(30),
                              [state] { return state->headReady || state->done; }) ||
          !state->headReady) {
        state->canceled = true;
        response.status = 502;
        response.set_content("{\"error\":\"upstream unavailable\"}", "application/json");
        return httplib::Server::HandlerResponse::Handled;
      }
      response.status = (int)state->status;
      const std::string contentType = state->contentType.empty() ? "application/json"
                                                                 : state->contentType;
      response.set_chunked_content_provider(
          contentType,
          [state](size_t, httplib::DataSink& sink) -> bool {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock, [state] {
              return state->canceled || state->done || !state->chunks.empty();
            });
            if (state->canceled) return false;
            if (!state->chunks.empty()) {
              std::string chunk = std::move(state->chunks.front());
              state->chunks.pop_front();
              lock.unlock();
              return sink.write(chunk.data(), chunk.size());
            }
            if (state->done) {
              sink.done();
              return true;
            }
            return true;
          },
          [state](bool) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->canceled = true;
            state->cv.notify_all();
          });
    }

    std::thread([this, state] {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->cv.wait(lock, [state] { return state->done || state->canceled; });
      if (state->canceled) return;
      if (state->isSse) {
        if (state->anthropic && !state->counted && (state->input || state->output)) {
          state->counted = true;
          record(state->input, state->output, state->input + state->output);
        } else if (!state->anthropic &&
                   (state->prompt || state->completion || state->total)) {
          record(state->prompt, state->completion,
                 state->total ? state->total : state->prompt + state->completion);
        }
      } else {
        JsonDocument document;
        if (!deserializeJson(document, state->body) && document.is<JsonObject>()) {
          JsonObject usage = document["usage"].as<JsonObject>();
          if (!usage.isNull()) {
            const auto prompt = numberValue(usage["prompt_tokens"].isNull() ? usage["input_tokens"] : usage["prompt_tokens"]);
            const auto completion = numberValue(usage["completion_tokens"].isNull() ? usage["output_tokens"] : usage["completion_tokens"]);
            const auto total = numberValue(usage["total_tokens"]);
            record(prompt, completion, total ? total : prompt + completion);
          }
        }
      }
    }).detach();
    return httplib::Server::HandlerResponse::Handled;
  });

  if (!server.bind_to_port(host_, port_)) {
    delete server_;
    server_ = nullptr;
    error = "cannot bind DeepSeek proxy port";
    return false;
  }
  running_ = true;
  thread_ = std::thread([this] {
    if (server_) server_->server.listen_after_bind();
  });
  return true;
}

void DeepSeekProxy::stop() {
  if (!running_) return;
  running_ = false;
  if (server_) server_->server.stop();
  if (thread_.joinable()) thread_.join();
  delete server_;
  server_ = nullptr;
}

}  // namespace usage
