#include "UsageCore.h"
#include "UsageHttp.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

namespace usage {
namespace {

constexpr const char* kClaudeUsageUrl = "https://api.anthropic.com/api/oauth/usage";
constexpr const char* kClaudeTokenUrl = "https://platform.claude.com/v1/oauth/token";
constexpr const char* kClaudeClientId = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
constexpr const char* kClaudeUserAgent = "claude-code/2.1.146";
constexpr const char* kCodexUsageUrl = "https://chatgpt.com/backend-api/wham/usage";
constexpr const char* kCodexTokenUrl = "https://auth.openai.com/oauth/token";
constexpr const char* kCodexClientId = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr const char* kCodexUserAgent = "codex-cli";
constexpr const char* kDeepSeekBalanceUrl = "https://api.deepseek.com/user/balance";

JsonObject providerObject(JsonDocument& doc, const char* id) {
  JsonObject root = doc.as<JsonObject>();
  if (root.isNull()) root = doc.to<JsonObject>();
  JsonObject value = root[id].as<JsonObject>();
  if (value.isNull()) value = root[id].to<JsonObject>();
  return value;
}

std::string text(JsonVariantConst value) {
  const char* out = value | "";
  return out ? out : "";
}

double number(JsonVariantConst value) {
  if (value.is<double>() || value.is<int>() || value.is<long>())
    return value.as<double>();
  if (value.is<const char*>()) {
    char* end = nullptr;
    const char* first = value.as<const char*>();
    if (!first || !*first) return 0;
    double parsed = std::strtod(first, &end);
    return end != first && std::isfinite(parsed) ? parsed : 0;
  }
  return 0;
}

long long nowMs() {
  return (long long)std::time(nullptr) * 1000LL;
}

time_t utc_mktime(std::tm* tm) {
#if defined(_WIN32)
  return _mkgmtime(tm);
#else
  return timegm(tm);
#endif
}

long long isoMinutes(const std::string& value) {
  if (value.size() < 19) return 0;
  std::tm tm{};
  char timezone[8] = {};
  int parts = sscanf(value.c_str(), "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon,
                     &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
  (void)timezone;
  if (parts != 6) return 0;
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  time_t when = utc_mktime(&tm);
  long long delta = (long long)when - (long long)std::time(nullptr);
  return delta > 0 ? (long long)(delta / 60) : 0;
}

std::string windowLabel(int seconds) {
  if (seconds >= 86400) return std::to_string(seconds / 86400) + "d";
  if (seconds >= 3600) return std::to_string(seconds / 3600) + "h";
  return std::to_string(std::max(1, seconds / 60)) + "m";
}

bool addMetric(std::vector<Metric>& metrics, const std::string& label,
               double pct, int resetMin, const std::string& value = {}) {
  if (pct <= 0 && value.empty()) return false;
  Metric metric;
  metric.label = label.substr(0, 11);
  metric.pct = std::max(0.0, std::min(100.0, pct));
  metric.resetMin = std::max(0, resetMin);
  metric.value = value.substr(0, 19);
  metrics.push_back(metric);
  return true;
}

Result errorResult(const char* id, const char* name, const std::string& error) {
  Result result;
  result.id = id;
  result.name = name;
  result.ok = false;
  result.status = "unavailable";
  result.error = error;
  return result;
}

std::string requestError(const HttpResponse& response) {
  if (!response.error.empty()) return response.error;
  if (response.status != 0)
    return "HTTP " + std::to_string(response.status);
  return "request failed";
}

bool parseJson(const std::string& body, JsonDocument& document) {
  return !deserializeJson(document, body) && document.is<JsonObject>();
}

bool exchangeToken(JsonDocument& credentials, JsonObject provider,
                   const char* tokenKey, const char* refreshKey,
                   const char* accountKey, const char* defaultClientId,
                   const char* defaultTokenUrl, const char* defaultUserAgent,
                   int timeoutMs, std::string& access, std::string& refresh,
                   std::string& account, std::string& error) {
  (void)credentials;
  access = text(provider[tokenKey]);
  refresh = text(provider[refreshKey]);
  account = accountKey ? text(provider[accountKey]) : "";
  if (access.empty() && refresh.empty()) {
    error = "credentials are not configured";
    return false;
  }
  if (refresh.empty()) {
    error = "credentials are incomplete or cannot be refreshed";
    return false;
  }

  JsonDocument body;
  body["grant_type"] = "refresh_token";
  body["client_id"] = text(provider["clientId"]).empty()
                          ? defaultClientId
                          : text(provider["clientId"]);
  body["refresh_token"] = refresh;
  if (accountKey) body["refresh_reason"] = "scheduled";
  std::string requestBody;
  serializeJson(body, requestBody);
  std::vector<Header> headers = {
      {"Content-Type", "application/json"},
      {"Accept", "application/json"},
      {"User-Agent", text(provider["userAgent"]).empty() ? defaultUserAgent
                                                          : text(provider["userAgent"])}};
  HttpResponse response;
  const std::string url = text(provider["tokenUrl"]).empty()
                              ? defaultTokenUrl : text(provider["tokenUrl"]);
  if (!httpRequest("POST", url, headers, requestBody, timeoutMs, response)) {
    error = requestError(response);
    return false;
  }
  JsonDocument result;
  if (response.status < 200 || response.status >= 300 || !parseJson(response.body, result)) {
    error = "token refresh returned " + (response.status ? std::to_string(response.status) : "an error");
    return false;
  }
  access = text(result["access_token"]);
  if (access.empty()) {
    error = "token refresh returned no access token";
    return false;
  }
  const std::string newRefresh = text(result["refresh_token"]);
  if (!newRefresh.empty()) {
    refresh = newRefresh;
    provider[refreshKey] = refresh;
  }
  if (accountKey) {
    const std::string newAccount = text(result["account_id"]);
    if (!newAccount.empty()) account = newAccount;
    provider[accountKey] = account;
  }
  provider[tokenKey] = access;
  if (result["expires_in"].is<int>() || result["expires_in"].is<double>())
    provider["expiresAt"] = nowMs() + (long long)(number(result["expires_in"]) * 1000);
  return true;
}

Result collectClaude(JsonDocument& credentials, int timeoutMs) {
  JsonObject provider = providerObject(credentials, "claude");
  std::string access = text(provider["accessToken"]);
  std::string refresh = text(provider["refreshToken"]);
  const long long expiresAt = (long long)number(provider["expiresAt"]);
  std::string error;
  if (access.empty() || (expiresAt > 0 && nowMs() >= expiresAt - 300000)) {
    std::string unusedAccount;
    if (!exchangeToken(credentials, provider, "accessToken", "refreshToken", nullptr,
                       kClaudeClientId, kClaudeTokenUrl, kClaudeUserAgent, timeoutMs,
                       access, refresh, unusedAccount, error)) {
      if (error.empty()) error = "Claude credentials are not configured";
      return errorResult("claude", "Claude", error);
    }
  }
  std::vector<Header> headers = {
      {"Authorization", "Bearer " + access},
      {"anthropic-version", "2023-06-01"},
      {"anthropic-beta", "oauth-2025-04-20"},
      {"Accept", "application/json"},
      {"User-Agent", text(provider["userAgent"]).empty() ? kClaudeUserAgent
                                                          : text(provider["userAgent"])}};
  HttpResponse response;
  if (!httpRequest("GET", text(provider["url"]).empty() ? kClaudeUsageUrl
                                                        : text(provider["url"]),
                   headers, "", timeoutMs, response)) {
    return errorResult("claude", "Claude", requestError(response));
  }
  if (response.status < 200 || response.status >= 300) {
    if ((response.status == 401 || response.status == 403) && !refresh.empty()) {
      std::string unusedAccount;
      if (!exchangeToken(credentials, provider, "accessToken", "refreshToken", nullptr,
                         kClaudeClientId, kClaudeTokenUrl, kClaudeUserAgent, timeoutMs,
                         access, refresh, unusedAccount, error))
        return errorResult("claude", "Claude", error);
      headers[0].second = "Bearer " + access;
      response = HttpResponse();
      if (!httpRequest("GET", text(provider["url"]).empty() ? kClaudeUsageUrl
                                                            : text(provider["url"]),
                       headers, "", timeoutMs, response))
        return errorResult("claude", "Claude", requestError(response));
    }
    if (response.status < 200 || response.status >= 300)
      return errorResult("claude", "Claude", "HTTP " + std::to_string(response.status));
  }
  JsonDocument payload;
  if (!parseJson(response.body, payload))
    return errorResult("claude", "Claude", "invalid usage response");
  JsonObject five = payload["five_hour"].as<JsonObject>();
  JsonObject seven = payload["seven_day"].as<JsonObject>();
  Result result;
  result.id = "claude";
  result.name = "Claude";
  result.ok = true;
  result.status = "allowed";
  addMetric(result.metrics, "5h", number(five["utilization"]),
            (int)isoMinutes(text(five["resets_at"])));
  addMetric(result.metrics, "7d", number(seven["utilization"]),
            (int)isoMinutes(text(seven["resets_at"])));
  JsonArray limits = payload["limits"].as<JsonArray>();
  for (JsonObject limit : limits) {
    if (text(limit["kind"]) == "session" && !text(limit["severity"]).empty()) {
      result.status = text(limit["severity"]);
      break;
    }
  }
  if (result.metrics.empty())
    return errorResult("claude", "Claude", "no usage windows returned");
  return result;
}

Result collectCodex(JsonDocument& credentials, int timeoutMs) {
  JsonObject provider = providerObject(credentials, "codex");
  std::string access = text(provider["accessToken"]);
  std::string refresh = text(provider["refreshToken"]);
  std::string account = text(provider["accountId"]);
  std::string error;
  if (access.empty() || account.empty()) {
    if (!exchangeToken(credentials, provider, "accessToken", "refreshToken", "accountId",
                       kCodexClientId, kCodexTokenUrl, kCodexUserAgent, timeoutMs,
                       access, refresh, account, error))
      return errorResult("chatgpt", "Codex", error.empty() ? "Codex credentials are not configured" : error);
  }
  std::vector<Header> headers = {
      {"Authorization", "Bearer " + access},
      {"chatgpt-account-id", account},
      {"Accept", "application/json"},
      {"User-Agent", text(provider["userAgent"]).empty() ? kCodexUserAgent
                                                          : text(provider["userAgent"])}};
  HttpResponse response;
  const std::string url = text(provider["url"]).empty() ? kCodexUsageUrl : text(provider["url"]);
  if (!httpRequest("GET", url, headers, "", timeoutMs, response))
    return errorResult("chatgpt", "Codex", requestError(response));
  if ((response.status == 401 || response.status == 403) && !refresh.empty()) {
    if (!exchangeToken(credentials, provider, "accessToken", "refreshToken", "accountId",
                       kCodexClientId, kCodexTokenUrl, kCodexUserAgent, timeoutMs,
                       access, refresh, account, error))
      return errorResult("chatgpt", "Codex", error);
    headers[0].second = "Bearer " + access;
    headers[1].second = account;
    response = HttpResponse();
    if (!httpRequest("GET", url, headers, "", timeoutMs, response))
      return errorResult("chatgpt", "Codex", requestError(response));
  }
  if (response.status < 200 || response.status >= 300)
    return errorResult("chatgpt", "Codex", "HTTP " + std::to_string(response.status));
  JsonDocument payload;
  if (!parseJson(response.body, payload))
    return errorResult("chatgpt", "Codex", "invalid usage response");
  JsonObject rate = payload["rate_limit"].as<JsonObject>();
  if (rate.isNull())
    return errorResult("chatgpt", "Codex", "missing rate_limit");
  Result result;
  result.id = "chatgpt";
  result.name = "Codex";
  result.ok = true;
  result.status = text(payload["plan_type"]);
  for (const char* key : {"primary_window", "secondary_window"}) {
    JsonObject window = rate[key].as<JsonObject>();
    if (window.isNull()) continue;
    const int seconds = (int)number(window["limit_window_seconds"]);
    addMetric(result.metrics, windowLabel(seconds), number(window["used_percent"]),
              (int)(number(window["reset_after_seconds"]) / 60));
  }
  if (result.metrics.empty())
    return errorResult("chatgpt", "Codex", "no rate-limit windows");
  if (result.status.empty()) result.status = "unknown";
  addMetric(result.metrics, "plan", 0, 0, result.status);
  return result;
}

Result collectDeepSeek(JsonDocument& credentials, const std::string& usageUrl,
                       int timeoutMs) {
  JsonObject provider = providerObject(credentials, "deepseek");
  const std::string key = text(provider["apiKey"]);
  if (key.empty())
    return errorResult("deepseek", "DeepSeek", "DeepSeek API key is not configured");
  std::vector<Header> headers = {
      {"Authorization", "Bearer " + key},
      {"Accept", "application/json"},
      {"User-Agent", "smalltv-usage-native"}};
  HttpResponse response;
  if (!httpRequest("GET", text(provider["url"]).empty() ? kDeepSeekBalanceUrl
                                                        : text(provider["url"]),
                   headers, "", timeoutMs, response))
    return errorResult("deepseek", "DeepSeek", requestError(response));
  if (response.status < 200 || response.status >= 300)
    return errorResult("deepseek", "DeepSeek", "HTTP " + std::to_string(response.status));
  JsonDocument payload;
  if (!parseJson(response.body, payload))
    return errorResult("deepseek", "DeepSeek", "invalid balance response");
  JsonArray infos = payload["balance_infos"].as<JsonArray>();
  if (infos.isNull() || infos.size() == 0)
    return errorResult("deepseek", "DeepSeek", "no balance data");
  JsonObject info = infos[0];
  Result result;
  result.id = "deepseek";
  result.name = "DeepSeek";
  result.ok = true;
  result.status = (payload["is_available"] | true) ? "available" : "low balance";
  const std::string total = text(info["total_balance"]);
  if (total.empty())
    return errorResult("deepseek", "DeepSeek", "missing total_balance");
  addMetric(result.metrics, "balance", 0, 0,
            total + " " + text(info["currency"]));
  if (!usageUrl.empty()) {
    HttpResponse usageResponse;
    if (httpRequest("GET", usageUrl, {}, "", timeoutMs, usageResponse) &&
        usageResponse.status >= 200 && usageResponse.status < 300) {
      JsonDocument usage;
      if (!deserializeJson(usage, usageResponse.body)) {
        JsonObject today = usage["today"].as<JsonObject>();
        const double tokens = number(today["total_tokens"]);
        if (tokens > 0) {
          const double budget = number(provider["usageBudget"]);
          char value[32];
          if (tokens >= 1000000) snprintf(value, sizeof(value), "%.1fM", tokens / 1000000);
          else if (tokens >= 1000) snprintf(value, sizeof(value), "%.1fk", tokens / 1000);
          else snprintf(value, sizeof(value), "%.0f", tokens);
          addMetric(result.metrics, "tokens", budget > 0 ? tokens * 100 / budget : 0, 0, value);
        }
      }
    }
  }
  return result;
}

void addResultJson(JsonArray& array, const Result& result) {
  JsonObject object = array.add<JsonObject>();
  object["id"] = result.id;
  object["name"] = result.name;
  object["ok"] = result.ok;
  object["status"] = result.status;
  if (!result.error.empty()) object["error"] = result.error;
  JsonArray metrics = object["metrics"].to<JsonArray>();
  for (const Metric& metric : result.metrics) {
    JsonObject item = metrics.add<JsonObject>();
    item["label"] = metric.label;
    item["pct"] = metric.pct;
    item["resetMin"] = metric.resetMin;
    if (!metric.value.empty()) item["value"] = metric.value;
  }
}

}  // namespace

std::string credentialsTemplate() {
  return R"({"claude":{},"codex":{},"deepseek":{}})";
}

FetchResult fetchAll(const std::string& credentialsJson,
                     const std::string& deepseekUsageUrl, int timeoutMs) {
  FetchResult output;
  output.credentialsJson = credentialsJson;
  JsonDocument credentials;
  if (deserializeJson(credentials, credentialsJson) || !credentials.is<JsonObject>()) {
    output.error = "Credential store is not valid JSON";
    return output;
  }
  output.providers.push_back(collectClaude(credentials, timeoutMs));
  output.providers.push_back(collectCodex(credentials, timeoutMs));
  output.providers.push_back(collectDeepSeek(credentials, deepseekUsageUrl, timeoutMs));
  serializeJson(credentials, output.credentialsJson);
  return output;
}

std::string normalizedJson(const std::vector<Result>& providers) {
  JsonDocument document;
  JsonObject root = document.to<JsonObject>();
  JsonArray array = root["providers"].to<JsonArray>();
  for (const Result& provider : providers) addResultJson(array, provider);
  std::string output;
  serializeJson(document, output);
  return output;
}

}  // namespace usage
