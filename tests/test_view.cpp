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

  // Asset-backed images are references; the pixels live in LittleFS, never in
  // the JSON or the legacy bitmap pool.
  const char* image = R"({"bgImage":"aurora","draw":[{"t":"image","src":"aurora","x":0,"y":0,"w":240,"h":240,"s":1,"a":"l"}]})";
  assert(viewScreenParse(image, strlen(image), s, error));
  assert(s.primCount == 1);
  assert(s.prims[0].type == VIEW_P_IMAGE);
  assert(!strcmp(viewBackgroundImage(s), "aurora"));
  assert(!strcmp(viewPrimImage(s, s.prims[0]), "aurora"));
  assert(s.imageUsed > 0);

  const char* bad_image = R"({"draw":[{"t":"image","src":"../secret","x":0,"y":0}]})";
  assert(viewScreenParse(bad_image, strlen(bad_image), s, error));
  assert(s.primCount == 0);

  // Explicit z order is sorted back-to-front; hidden components do not render.
  const char* zorder = R"({"draw":[
    {"t":"rect","x":0,"y":0,"w":4,"h":4,"c":"#ff0000","z":9},
    {"t":"rect","x":0,"y":0,"w":4,"h":4,"c":"#00ff00","z":-2},
    {"t":"rect","x":0,"y":0,"w":4,"h":4,"c":"#0000ff","hidden":true,"z":20}
  ]})";
  assert(viewScreenParse(zorder, strlen(zorder), s, error));
  assert(s.primCount == 2);
  assert(s.prims[0].color == 0x07E0);  // green, z=-2
  assert(s.prims[1].color == 0xF800);  // red, z=9

  // Usage style values and custom title/labels live in the text pool.
  const char* usage_style = R"({"draw":[{"t":"usage","x":0,"y":0,"w":224,"h":80,"c":"#d97757","provider":"claude","metric":-1,"style":{"nameSize":3,"labelAlign":"l","valueAlign":"c","barHeight":5,"showBar":false,"title":"Claude custom","labels":["Five hour","Weekly"]}}]})";
  assert(viewScreenParse(usage_style, strlen(usage_style), s, error));
  assert(s.primCount == 1);
  const UsageStyle* st = viewPrimUsageStyle(s, s.prims[0]);
  assert(st && st->nameSize == 3 && st->labelAlign == VIEW_A_LEFT);
  assert(st->valueAlign == VIEW_A_CENTER && st->barHeight == 5);
  assert(!(st->flags & USAGE_SHOW_BAR));
  assert(!strcmp(viewUsageTitle(s, *st), "Claude custom"));
  assert(!strcmp(viewUsageMetricLabel(s, *st, 0), "Five hour"));
  assert(!strcmp(viewUsageMetricLabel(s, *st, 1), "Weekly"));

  const char* text_box = R"({"draw":[{"t":"text","x":10,"y":12,"w":120,"h":40,"padX":4,"padY":3,"mx":1,"my":-1,"lineGap":2,"va":"b","s":2,"c":"#ffffff","v":"box"}]})";
  assert(viewScreenParse(text_box, strlen(text_box), s, error));
  assert(s.primCount == 1 && s.prims[0].x2 == 120 && s.prims[0].y2 == 40);
  const TextStyle* text_style = viewPrimTextStyle(s, s.prims[0]);
  assert(text_style && text_style->padX == 4 && text_style->padY == 3);
  assert(text_style->marginX == 1 && text_style->marginY == -1 && text_style->valign == 2);

  const char* custom_layout = R"({"draw":[{"t":"usage","x":0,"y":0,"w":224,"h":100,"c":"#ffffff","provider":"claude","metric":-1,"style":{"layout":{"auto":false,"name":{"x":8,"y":6,"w":100,"h":16,"a":"c"},"rows":[{"label":{"x":10,"y":30,"w":80,"h":12,"a":"l"},"value":{"x":100,"y":28,"w":100,"h":20,"a":"r"},"bar":{"x":10,"y":52,"w":180,"h":5}},{"label":{"x":20,"y":70,"w":80,"h":12,"a":"l"},"value":{"x":110,"y":68,"w":100,"h":20,"a":"r"},"bar":{"x":20,"y":92,"w":180,"h":5}}]}}}]})";
  assert(viewScreenParse(custom_layout, strlen(custom_layout), s, error));
  const UsageLayout* layout = viewPrimUsageLayout(s, s.prims[0]);
  assert(layout && layout->configured);
  assert(layout->name.x == 8 && layout->name.align == VIEW_A_CENTER);
  assert(layout->rows[0].label.x == 10 && layout->rows[1].label.y == 70);
  assert(layout->rows[1].bar.w == 180);

  // Pixel fields take precedence while legacy values remain available.
  const char* px_fields = R"({"draw":[
    {"t":"text","x":0,"y":0,"s":2,"sizePx":24,"c":"#ffffff","v":"px"},
    {"t":"icon","x":0,"y":0,"s":1,"sizePx":48,"c":"#ffffff","v":"sun"},
    {"t":"bitmap","x":0,"y":0,"w":2,"h":2,"s":1,"displayW":20,"displayH":30,"c":"#ffffff","d":"00"},
    {"t":"image","x":0,"y":0,"s":1,"displayW":40,"displayH":50,"src":"aa"},
    {"t":"usage","x":0,"y":0,"w":100,"h":50,"c":"#ffffff","provider":"claude","style":{"namePx":24,"labelPx":16,"valuePx":32}}
  ]})";
  assert(viewScreenParse(px_fields, strlen(px_fields), s, error));
  assert(s.prims[0].aux == 3);
  assert(s.prims[1].aux == 2);
  const ViewBitmapStyle* bitmap_style = viewPrimBitmapStyle(s, s.prims[2]);
  assert(bitmap_style && bitmap_style->displayW == 20 && bitmap_style->displayH == 30);
  assert(s.prims[3].x2 == 40 && s.prims[3].y2 == 50);
  const UsageStyle* pixel_style = viewPrimUsageStyle(s, s.prims[4]);
  assert(pixel_style && pixel_style->nameSize == 3 &&
         pixel_style->labelSize == 2 && pixel_style->valueSize == 4);

  // An omitted metric means "all metrics" (the renderer stacks them).
  const char* all_metrics = R"({"draw":[{"t":"usage","x":0,"y":0,"w":100,"h":50,"c":"#ffffff","provider":"claude"}]})";
  assert(viewScreenParse(all_metrics, strlen(all_metrics), s, error));
  assert(s.prims[0].type == VIEW_P_USAGE && s.prims[0].aux == 255);
  assert(s.priority == 0);
  assert(!s.needsClock);

  // Usage-bound text and bars use a compact side table.
  const char* bindings = R"({"draw":[
    {"t":"text","x":120,"y":20,"s":3,"c":"#ffffff","a":"c","v":"Codex --% remaining",
     "data":{"source":"usage","provider":"chatgpt","metric":0,"field":"pct"},
     "format":"Codex {value}% remaining"},
    {"t":"bar","x":8,"y":60,"w":224,"h":8,"c":"#10a37f","track":"#303030","r":4,
     "data":{"source":"usage","provider":"chatgpt","metric":1,"field":"usedPct"}}
  ]})";
  assert(viewScreenParse(bindings, strlen(bindings), s, error));
  assert(s.primCount == 2);
  assert(s.hasUsage && s.bindingCount == 2);
  const ViewBinding* text_binding = viewBindingAt(s, 0);
  const ViewBinding* bar_binding = viewBindingAt(s, 1);
  assert(text_binding && text_binding->source == VIEW_BIND_USAGE);
  assert(text_binding->field == VIEW_BIND_PCT && text_binding->metric == 0);
  assert(!strcmp(viewBindingProvider(s, *text_binding), "chatgpt"));
  assert(!strcmp(viewBindingFormat(s, *text_binding), "Codex {value}% remaining"));
  assert(bar_binding && bar_binding->field == VIEW_BIND_USED_PCT && bar_binding->metric == 1);
  assert(s.prims[1].type == VIEW_P_BAR);
  const ViewBarStyle* bar_style = viewPrimBarStyle(s, s.prims[1]);
  assert(bar_style && bar_style->trackColor == 0x3186 && bar_style->radius == 4);

  // Stale behavior is per primitive and ignored by older firmware.
  const char* stale = R"({"draw":[
    {"t":"text","x":120,"y":10,"s":2,"c":"#ffffff","v":"{HH}:{MM}","staleY":86},
    {"t":"usage","x":0,"y":90,"w":100,"h":40,"c":"#ffffff","provider":"chatgpt","hideWhenStale":true}
  ]})";
  assert(viewScreenParse(stale, strlen(stale), s, error));
  assert(s.hasStaleBehavior);
  const ViewStaleStyle* clock_stale = viewPrimStaleStyle(s, 0);
  assert(clock_stale && (clock_stale->flags & VIEW_STALE_Y) && clock_stale->y == 86);
  const ViewStaleStyle* usage_stale = viewPrimStaleStyle(s, 1);
  assert(usage_stale && (usage_stale->flags & VIEW_STALE_HIDE));

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
