#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace usage {

using Header = std::pair<std::string, std::string>;
using BodySink = std::function<bool(const char*, size_t)>;
using HeadCallback = std::function<bool(const struct HttpResponse&)>;

struct HttpResponse {
  long status = 0;
  std::string body;
  std::string contentType;
  std::string error;
  std::vector<Header> headers;
};

bool httpRequest(const std::string& method, const std::string& url,
                 const std::vector<Header>& headers, const std::string& body,
                 int timeoutMs, HttpResponse& response,
                 const BodySink& sink = nullptr,
                 const HeadCallback& onHead = nullptr);

}  // namespace usage
