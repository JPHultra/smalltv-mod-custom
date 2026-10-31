// Host test for the draw-list parser. Compile from the repository root with
// tests/shim, src, src/features/view and the ArduinoJson include path.
#include "ViewModel.h"
#include <cassert>
#include <cstring>
#include <ctime>

// ViewModel::viewClockKey() needs the clock; the parser test never calls it.
bool clockNow(struct tm&) { return false; }

int main() {
  const char* json = R"({
    "bg":"#102030","ttl":15,"priority":2,
    "draw":[
      {"t":"text","x":120,"y":20,"s":3,"c":"#ffffff","a":"c","v":"{HH}:{MM}"},
      {"t":"usage","x":8,"y":50,"w":224,"h":82,"c":"#d97757","provider":"claude","metric":1},
      {"t":"icon","x":10,"y":10,"s":2,"c":"#ffff00","a":"l","v":"sun"},
      {"t":"bitmap","x":0,"y":0,"w":2,"h":2,"s":1,"c":"#00ff00","a":"l","d":"80"},
      {"t":"line","x":0,"y":0,"x2":10,"y2":10,"c":"#ffffff"},
      {"t":"bogus","x":0,"y":0,"c":"#ffffff"}
    ]
  })";
  ViewScreen s;
  String error;
  assert(viewScreenParse(json, strlen(json), s, error));
  assert(s.primCount == 5);
  assert(s.needsClock);
  assert(s.hasUsage);
  assert(s.ttl == 15);
  assert(s.priority == 2);
  assert(!s.needsSeconds);
  assert(s.bg != 0);
  assert(s.prims[0].type == VIEW_P_TEXT);
  assert(!strcmp(viewPrimText(s, s.prims[0]), "{HH}:{MM}"));
  assert(s.prims[1].type == VIEW_P_USAGE);
  assert(s.prims[1].aux == 1);
  assert(!strcmp(viewPrimText(s, s.prims[1]), "claude"));
  assert(s.prims[2].type == VIEW_P_ICON);
  assert(!strcmp(viewPrimText(s, s.prims[2]), "sun"));
  assert(s.prims[3].type == VIEW_P_BITMAP);
  assert(s.bitmap[s.prims[3].voff] == 0x80);
  assert(s.prims[4].type == VIEW_P_LINE);

  // An omitted metric means "all metrics" (the renderer stacks them).
  const char* all_metrics = R"({"draw":[{"t":"usage","x":0,"y":0,"w":100,"h":50,"c":"#ffffff","provider":"claude"}]})";
  assert(viewScreenParse(all_metrics, strlen(all_metrics), s, error));
  assert(s.prims[0].type == VIEW_P_USAGE && s.prims[0].aux == 255);
  assert(s.priority == 0);
  assert(!s.needsClock);

  const char* seconds = R"({"draw":[{"t":"text","x":0,"y":0,"s":1,"c":"#ffffff","v":"{SS}"}]})";
  assert(viewScreenParse(seconds, strlen(seconds), s, error));
  assert(s.needsClock && s.needsSeconds);

  // Priority is clamped to 0..3 so a bad value cannot escape the queue contract.
  const char* high_priority = R"({"priority":9,"draw":[]})";
  assert(viewScreenParse(high_priority, strlen(high_priority), s, error));
  assert(s.priority == 3);

  assert(!viewScreenParse("not json", 8, s, error));
  assert(error.length() > 0);
  assert(!viewScreenParse("[]", 2, s, error));
  assert(error.length() > 0);

  // A malformed primitive is skipped, the rest still parse.
  const char* partial = R"({"draw":[{"t":"rect","c":"#ffffff"},{"t":"text","x":1,"y":2,"s":1,"c":"#ffffff","v":"ok"}]})";
  assert(viewScreenParse(partial, strlen(partial), s, error));
  assert(s.primCount == 1);
  assert(s.prims[0].type == VIEW_P_TEXT);
  return 0;
}
