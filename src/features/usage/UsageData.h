// UsageData.h — one normalized usage snapshot per provider.
//
// The companion daemon is the only writer. It pushes Claude, ChatGPT and
// DeepSeek data in one JSON document; the firmware stores the latest snapshot
// per provider and the dashboard's usage components read it.
#pragma once
#include <Arduino.h>
#include "config.h"

struct UsageMetric {
  char  label[USAGE_MAX_LABEL];
  float pct;        // 0..100
  int   resetMin;   // minutes until reset; <= 0 means "now"/unknown
};

struct ProviderUsage {
  char     id[USAGE_MAX_ID];
  char     name[USAGE_MAX_NAME];
  char     status[USAGE_MAX_STATUS];
  UsageMetric metrics[USAGE_MAX_METRICS];
  uint8_t  metricCount;
  bool     valid;     // populated at least once
  bool     error;     // most recent push failed
  uint32_t lastOkMs;  // millis() of the last successful push
};

struct UsageStore {
  ProviderUsage providers[USAGE_MAX_PROVIDERS];
  uint8_t count;
};
