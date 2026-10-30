// Host test for the normalized usage parser. Compile with tests/shim, src,
// src/features/usage and the ArduinoJson include path.
#include "UsageClient.h"
#include <cassert>
#include <cstring>

int main() {
  usageInit();

  const char* normalized = R"({"providers":[
    {"id":"claude","name":"Claude","ok":true,"status":"normal",
     "metrics":[{"label":"5h","pct":29,"resetMin":142},
                {"label":"7d","pct":4,"resetMin":9876}]},
    {"id":"chatgpt","name":"ChatGPT","ok":true,
     "metrics":[{"label":"5h","pct":62,"resetMin":88}]}
  ]})";
  assert(usageApply(String(normalized)));
  const ProviderUsage* claude = usageFind("claude");
  assert(claude && claude->valid && !claude->error);
  assert(claude->metricCount == 2);
  assert(!strcmp(claude->metrics[0].label, "5h"));
  assert(claude->metrics[0].pct == 29.0f);
  assert(claude->metrics[0].resetMin == 142);
  const ProviderUsage* chatgpt = usageFind("chatgpt");
  assert(chatgpt && chatgpt->metricCount == 1 && chatgpt->metrics[0].pct == 62.0f);
  assert(usageFind("deepseek") == nullptr);

  // The legacy Claude daemon keeps working and updates the same provider.
  assert(usageApply(String(R"({"s":50,"sr":10,"w":20,"wr":100,"st":"normal","ok":true})")));
  claude = usageFind("claude");
  assert(claude && claude->metrics[0].pct == 50.0f && claude->metrics[1].pct == 20.0f);

  // ok:false without metrics keeps the previous metrics and flags the error.
  assert(usageApply(String(R"({"providers":[{"id":"claude","ok":false}]})")));
  claude = usageFind("claude");
  assert(claude && claude->error && claude->metricCount == 2);
  assert(claude->metrics[0].pct == 50.0f);

  // A provider absent from a later document keeps its snapshot.
  assert(usageApply(String(R"({"providers":[{"id":"chatgpt","ok":true,"metrics":[{"label":"5h","pct":70}]}]})")));
  chatgpt = usageFind("chatgpt");
  assert(chatgpt && chatgpt->metrics[0].pct == 70.0f);
  assert(usageFind("claude") && usageFind("claude")->metrics[1].pct == 20.0f);

  assert(!usageApply(String("not json")));
  assert(!usageApply(String(R"({"providers":[]})")));

  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  usageStatusJson(root);
  JsonArrayConst providers = root["providers"].as<JsonArrayConst>();
  assert(providers.size() == 2);
  return 0;
}
