#include "UsageHttp.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

namespace usage {
namespace {

std::string lower(std::string value) {
  for (char& c : value) c = (char)std::tolower((unsigned char)c);
  return value;
}

#if !defined(_WIN32)
struct CurlContext {
  HttpResponse* response = nullptr;
  const BodySink* sink = nullptr;
  const HeadCallback* onHead = nullptr;
  bool headSent = false;
  CURL* curl = nullptr;
};

size_t curlWrite(char* data, size_t size, size_t count, void* user) {
  auto* context = static_cast<CurlContext*>(user);
  const size_t length = size * count;
  if (context->sink && *context->sink && !(*context->sink)(data, length)) return 0;
  if (!context->sink || !*context->sink) context->response->body.append(data, length);
  return length;
}

size_t curlHeader(char* data, size_t size, size_t count, void* user) {
  auto* context = static_cast<CurlContext*>(user);
  const size_t length = size * count;
  std::string line(data, length);
  if (line == "\r\n" && !context->headSent) {
    context->headSent = true;
    if (context->curl) curl_easy_getinfo(context->curl, CURLINFO_RESPONSE_CODE, &context->response->status);
    if (context->onHead && *context->onHead && !(*context->onHead)(*context->response)) return 0;
    return length;
  }
  const size_t colon = line.find(':');
  if (colon != std::string::npos) {
    std::string name = lower(line.substr(0, colon));
    std::string value = line.substr(colon + 1);
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' '))
      value.pop_back();
    size_t first = value.find_first_not_of(' ');
    if (first != std::string::npos) value.erase(0, first);
    context->response->headers.emplace_back(name, value);
    if (name == "content-type") context->response->contentType = value;
  }
  return length;
}
#else
std::wstring widen(const std::string& text) {
  if (text.empty()) return {};
  int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
  std::wstring out((size_t)length, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), length);
  return out;
}

std::string narrow(const std::wstring& text) {
  if (text.empty()) return {};
  int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
                                   nullptr, 0, nullptr, nullptr);
  std::string out((size_t)length, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), length,
                      nullptr, nullptr);
  return out;
}

struct UrlParts {
  bool secure = false;
  std::wstring host;
  INTERNET_PORT port = 80;
  std::wstring path = L"/";
};

bool splitUrl(const std::string& url, UrlParts& parts) {
  size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return false;
  const std::string scheme = lower(url.substr(0, schemeEnd));
  parts.secure = scheme == "https";
  if (!parts.secure && scheme != "http") return false;
  size_t at = schemeEnd + 3;
  size_t slash = url.find('/', at);
  std::string hostPort = url.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
  parts.path = widen(slash == std::string::npos ? "/" : url.substr(slash));
  size_t colon = hostPort.rfind(':');
  if (colon != std::string::npos) {
    parts.host = widen(hostPort.substr(0, colon));
    parts.port = (INTERNET_PORT)std::stoi(hostPort.substr(colon + 1));
  } else {
    parts.host = widen(hostPort);
    parts.port = parts.secure ? 443 : 80;
  }
  return !parts.host.empty();
}

std::string winError(const char* prefix) {
  return std::string(prefix) + " (Windows error " + std::to_string(GetLastError()) + ")";
}
#endif

}  // namespace

bool httpRequest(const std::string& method, const std::string& url,
                 const std::vector<Header>& headers, const std::string& body,
                 int timeoutMs, HttpResponse& response, const BodySink& sink,
                 const HeadCallback& onHead) {
  response = HttpResponse();

#if !defined(_WIN32)
  static bool initialized = false;
  if (!initialized) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    initialized = true;
  }
  CURL* curl = curl_easy_init();
  if (!curl) {
    response.error = "cannot initialize HTTP client";
    return false;
  }
  CurlContext context{&response, &sink, &onHead, false, curl};
  curl_slist* list = nullptr;
  for (const auto& header : headers) {
    std::string line = header.first + ": " + header.second;
    list = curl_slist_append(list, line.c_str());
  }
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)std::min(timeoutMs, 15000));
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeoutMs);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curlHeader);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &context);
  if (method == "POST" || method == "PUT" || method == "PATCH") {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
  } else if (method == "GET") {
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  }
  const CURLcode result = curl_easy_perform(curl);
  if (result != CURLE_OK) response.error = curl_easy_strerror(result);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
  curl_slist_free_all(list);
  curl_easy_cleanup(curl);
  return result == CURLE_OK;
#else
  UrlParts parts;
  if (!splitUrl(url, parts)) {
    response.error = "invalid URL";
    return false;
  }
  HINTERNET session = WinHttpOpen(L"smalltv-usage/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    response.error = winError("cannot initialize WinHTTP");
    return false;
  }
  WinHttpSetTimeouts(session, std::min(timeoutMs, 15000), std::min(timeoutMs, 15000),
                     timeoutMs, timeoutMs);

  HINTERNET connection = WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
  if (!connection) {
    response.error = winError("cannot connect");
    WinHttpCloseHandle(session);
    return false;
  }
  const std::wstring wideMethod = widen(method);
  HINTERNET request = WinHttpOpenRequest(connection, wideMethod.c_str(), parts.path.c_str(),
                                         nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         parts.secure ? WINHTTP_FLAG_SECURE : 0);
  if (!request) {
    response.error = winError("cannot create request");
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  std::wstring headerBlock;
  for (const auto& header : headers) {
    if (!headerBlock.empty()) headerBlock += L"\r\n";
    headerBlock += widen(header.first + ": " + header.second);
  }
  const void* requestBody = body.empty() ? WINHTTP_NO_REQUEST_DATA : body.data();
  const DWORD requestSize = (DWORD)body.size();
  BOOL sent = WinHttpSendRequest(request,
                                headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
                                headerBlock.empty() ? 0 : (DWORD)-1L,
                                (LPVOID)requestBody, requestSize, requestSize, 0);
  if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
    response.error = winError("HTTP request failed");
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                      WINHTTP_NO_HEADER_INDEX);
  response.status = (long)status;

  wchar_t contentType[512] = {};
  DWORD contentTypeSize = sizeof(contentType);
  if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE,
                          WINHTTP_HEADER_NAME_BY_INDEX, contentType,
                          &contentTypeSize, WINHTTP_NO_HEADER_INDEX))
    response.contentType = narrow(contentType);
  if (onHead && !onHead(response)) {
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    response.error = "canceled";
    return false;
  }

  std::string collected;
  while (true) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available)) break;
    if (available == 0) break;
    std::string chunk(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
    chunk.resize(read);
    if (sink && !sink(chunk.data(), chunk.size())) break;
    if (!sink) collected.append(chunk);
  }
  if (!sink) response.body = std::move(collected);

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return true;
#endif
}

}  // namespace usage
