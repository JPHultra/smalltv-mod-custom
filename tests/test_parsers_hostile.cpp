// Hostile-input regression test for the view and usage parsers.
// Compile with ASan+UBSan; the point is that arbitrary/truncated JSON must
// never read out of bounds, loop forever, or abort the firmware-side parser.
#include "UsageClient.h"
#include "ViewModel.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

bool clockNow(struct tm&) { return false; }

static uint32_t rngState = 0x9e3779b9u;
static uint32_t rnd() {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 17;
  rngState ^= rngState << 5;
  return rngState;
}

static const char* kViewSeed =
    R"({"bg":"#0b1420","ttl":15,"priority":2,"draw":[)"
    R"({"t":"text","x":120,"y":6,"s":5,"c":"#e9edf4","a":"c","v":"{HH}:{MM}"},)"
    R"({"t":"usage","x":8,"y":68,"w":224,"h":54,"c":"#d97757","provider":"claude","metric":-1},)"
    R"({"t":"usage","x":8,"y":126,"w":224,"h":62,"c":"#10a37f","provider":"chatgpt","metric":2},)"
    R"({"t":"icon","x":10,"y":10,"s":2,"c":"#ffff00","a":"l","v":"sun"},)"
    R"({"t":"bitmap","x":0,"y":0,"w":3,"h":3,"s":1,"c":"#00ff00","a":"l","d":"80C0E0"},)"
    R"({"t":"rect","x":0,"y":0,"w":10,"h":10,"c":"#ffffff"},)"
    R"({"t":"rrect","x":0,"y":0,"w":10,"h":10,"r":3,"c":"#ffffff"},)"
    R"({"t":"circle","x":10,"y":10,"r":5,"c":"#ffffff"},)"
    R"({"t":"line","x":0,"y":0,"x2":10,"y2":10,"c":"#ffffff"},)"
    R"({"t":"future-type","x":0,"y":0,"c":"#ffffff"})"
    R"(]})";

static const char* kUsageSeed =
    R"({"providers":[)"
    R"({"id":"claude","name":"Claude","ok":true,"status":"normal",)"
    R"("metrics":[{"label":"5h","pct":29,"resetMin":142},{"label":"7d","pct":4,"resetMin":9876}]},)"
    R"({"id":"chatgpt","name":"Codex","ok":true,"metrics":[)"
    R"({"label":"5h","pct":62,"resetMin":88,"value":"1.2M tokens"},)"
    R"({"label":"plan","value":"plus"},{"label":"credits","value":"12.50"}]},)"
    R"({"id":"deepseek","name":"DeepSeek","ok":false,"error":"no key"})"
    R"(]})";

static void mutate(std::vector<char>& buf) {
  if (buf.empty()) return;
  switch (rnd() % 5) {
    case 0:  // single byte
      buf[rnd() % buf.size()] = (char)(rnd() & 0xff);
      break;
    case 1:  // truncate
      buf.resize(rnd() % buf.size());
      break;
    case 2: {  // splice a random chunk
      static const char kChars[] = "{}[]\",:0123456789abcXYZ\\ \t\n";
      for (int i = 0, n = 1 + (int)(rnd() % 24); i < n && !buf.empty(); i++)
        buf[rnd() % buf.size()] = kChars[rnd() % (sizeof(kChars) - 1)];
      break;
    }
    case 3: {  // erase a run
      size_t at = rnd() % buf.size(), n = 1 + rnd() % 16;
      if (at + n > buf.size()) n = buf.size() - at;
      buf.erase(buf.begin() + at, buf.begin() + at + n);
      break;
    }
    default: {  // duplicate a run (copy first: self-insert is unsafe)
      size_t at = rnd() % buf.size(), n = 1 + rnd() % 16;
      if (at + n > buf.size()) n = buf.size() - at;
      std::vector<char> chunk(buf.begin() + at, buf.begin() + at + n);
      buf.insert(buf.end(), chunk.begin(), chunk.end());
      break;
    }
  }
  if (buf.size() > 4096) buf.resize(4096);
}

int main() {
  usageInit();
  ViewScreen screen;
  String parseError;

  // Every truncation of both seeds, including zero length.
  for (size_t n = 0; n <= strlen(kViewSeed); n++) {
    viewScreenParse(kViewSeed, n, screen, parseError);
  }
  for (size_t n = 0; n <= strlen(kUsageSeed); n++) {
    usageApply(String(kUsageSeed, n));
  }

  // Randomized mutation fuzzing: parser output is irrelevant; surviving is not.
  for (int iter = 0; iter < 6000; iter++) {
    std::vector<char> view(kViewSeed, kViewSeed + strlen(kViewSeed));
    std::vector<char> usage(kUsageSeed, kUsageSeed + strlen(kUsageSeed));
    for (int m = 0, n = 1 + (int)(rnd() % 4); m < n; m++) mutate(view);
    for (int m = 0, n = 1 + (int)(rnd() % 4); m < n; m++) mutate(usage);
    parseError = "";
    viewScreenParse(view.data(), view.size(), screen, parseError);
    usageApply(String(usage.data(), usage.size()));
  }

  // Extreme but syntactically valid shapes: many primitives and providers.
  std::string manyViews = "{\"draw\":[";
  for (int i = 0; i < 400; i++) {
    if (i) manyViews += ',';
    manyViews += "{\"t\":\"text\",\"x\":1,\"y\":2,\"s\":1,\"c\":\"#ffffff\",\"v\":\"x\"}";
  }
  manyViews += "]}";
  viewScreenParse(manyViews.c_str(), manyViews.size(), screen, parseError);

  std::string manyProviders = "{\"providers\":[";
  for (int i = 0; i < 100; i++) {
    if (i) manyProviders += ',';
    manyProviders += "{\"id\":\"p\",\"metrics\":[{\"label\":\"m\",\"pct\":50}]}";
  }
  manyProviders += "]}";
  usageApply(String(manyProviders.c_str(), manyProviders.size()));

  std::puts("hostile parser test passed");
  return 0;
}
