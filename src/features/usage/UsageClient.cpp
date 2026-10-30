#include "UsageClient.h"
#include <ArduinoJson.h>
#include <string.h>

static UsageStore g_store;
static uint32_t   g_revision = 0;

void usageInit() {
  memset(&g_store, 0, sizeof(g_store));
  g_revision = 0;
}

static ProviderUsage* findOrAdd(const char* id) {
  if (!id || !id[0]) return nullptr;
  for (uint8_t i = 0; i < g_store.count; i++)
    if (!strcmp(g_store.providers[i].id, id)) return &g_store.providers[i];
  if (g_store.count >= USAGE_MAX_PROVIDERS) return nullptr;
  ProviderUsage& p = g_store.providers[g_store.count++];
  memset(&p, 0, sizeof(p));
  strlcpy(p.id, id, sizeof(p.id));
  strlcpy(p.name, id, sizeof(p.name));
  return &p;
}

static void readMetrics(JsonObjectConst src, ProviderUsage& dst) {
  JsonArrayConst metrics = src["metrics"].as<JsonArrayConst>();
  if (metrics.isNull()) return;   // no metrics in this push: keep the previous set
  dst.metricCount = 0;
  for (JsonObjectConst m : metrics) {
    if (dst.metricCount >= USAGE_MAX_METRICS) break;
    UsageMetric& out = dst.metrics[dst.metricCount];
    memset(&out, 0, sizeof(out));
    const char* label = m["label"] | "";
    strlcpy(out.label, label[0] ? label : "--", sizeof(out.label));
    out.pct = constrain(m["pct"] | 0.0f, 0.0f, 100.0f);
    out.resetMin = m["resetMin"] | 0;
    if (out.resetMin < 0) out.resetMin = 0;
    dst.metricCount++;
  }
}

bool usageApply(const String& body) {
  if (body.length() == 0 || body.length() > 4096) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  bool touched = false;
  JsonArrayConst providers = doc["providers"].as<JsonArrayConst>();
  if (!providers.isNull()) {
    for (JsonObjectConst p : providers) {
      const char* id = p["id"] | "";
      ProviderUsage* dst = findOrAdd(id);
      if (!dst) continue;
      const char* name = p["name"] | "";
      if (name[0]) strlcpy(dst->name, name, sizeof(dst->name));
      const char* status = p["status"] | "";
      strlcpy(dst->status, status, sizeof(dst->status));
      readMetrics(p, *dst);
      const bool ok = p["ok"].is<bool>() ? p["ok"].as<bool>() : true;
      if (ok) {
        dst->valid = true;
        dst->error = false;
        dst->lastOkMs = millis();
      } else {
        dst->error = true;
      }
      touched = true;
    }
  } else if (doc["s"].is<float>() || doc["s"].is<int>()) {
    // Legacy Claude daemon: {s,sr,w,wr,st,ok}
    ProviderUsage* dst = findOrAdd("claude");
    if (dst) {
      strlcpy(dst->name, "Claude", sizeof(dst->name));
      const char* status = doc["st"] | "";
      strlcpy(dst->status, status, sizeof(dst->status));
      dst->metricCount = 2;
      memset(&dst->metrics[0], 0, sizeof(dst->metrics[0]));
      memset(&dst->metrics[1], 0, sizeof(dst->metrics[1]));
      strlcpy(dst->metrics[0].label, "5h", sizeof(dst->metrics[0].label));
      dst->metrics[0].pct = constrain(doc["s"] | 0.0f, 0.0f, 100.0f);
      dst->metrics[0].resetMin = doc["sr"] | 0;
      strlcpy(dst->metrics[1].label, "7d", sizeof(dst->metrics[1].label));
      dst->metrics[1].pct = constrain(doc["w"] | 0.0f, 0.0f, 100.0f);
      dst->metrics[1].resetMin = doc["wr"] | 0;
      const bool ok = doc["ok"].is<bool>() ? doc["ok"].as<bool>() : true;
      if (ok) {
        dst->valid = true;
        dst->error = false;
        dst->lastOkMs = millis();
      } else {
        dst->error = true;
      }
      touched = true;
    }
  }

  if (!touched) return false;
  g_revision++;
  return true;
}

const ProviderUsage* usageFind(const char* id) {
  if (!id || !id[0]) return nullptr;
  for (uint8_t i = 0; i < g_store.count; i++)
    if (!strcmp(g_store.providers[i].id, id)) return &g_store.providers[i];
  return nullptr;
}

bool usageStale(const ProviderUsage& provider, uint32_t withinMs) {
  if (!provider.valid) return true;
  return (int32_t)(millis() - provider.lastOkMs) > (int32_t)withinMs;
}

uint32_t usageRevision() { return g_revision; }

void usageStatusJson(JsonObject& root) {
  root["count"] = g_store.count;
  JsonArray arr = root["providers"].to<JsonArray>();
  for (uint8_t i = 0; i < g_store.count; i++) {
    const ProviderUsage& p = g_store.providers[i];
    JsonObject o = arr.add<JsonObject>();
    o["id"] = p.id;
    o["name"] = p.name;
    o["valid"] = p.valid;
    o["error"] = p.error;
    o["stale"] = usageStale(p);
    o["status"] = p.status;
    o["age"] = p.valid ? (millis() - p.lastOkMs) / 1000UL : 0;
    JsonArray ms = o["metrics"].to<JsonArray>();
    for (uint8_t k = 0; k < p.metricCount; k++) {
      JsonObject m = ms.add<JsonObject>();
      m["label"] = p.metrics[k].label;
      m["pct"] = p.metrics[k].pct;
      m["resetMin"] = p.metrics[k].resetMin;
    }
  }
}
