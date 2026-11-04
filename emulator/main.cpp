// SmallTV dashboard emulator: the firmware's real view parser, renderer,
// icons, usage store and anti-aliased font on a host RGB565 framebuffer.
#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>

#include "Gfx.h"
#include "Arduino_GFX_Library.h"
#include "UsageClient.h"
#include "ViewModel.h"
#include "ViewRender.h"

static bool local_time(std::time_t now, struct tm& out) {
  struct tm* local = std::localtime(&now);
  if (!local) return false;
  out = *local;
  return true;
}

static bool g_fixedTime = false;
static struct tm g_fixedTm{};

bool clockNow(struct tm& out) {
  if (g_fixedTime) {
    out = g_fixedTm;
    return true;
  }
  return local_time(std::time(nullptr), out);
}

static bool read_file(const char* path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return true;
}

static bool write_ppm(const char* path, const uint16_t* frame, int w, int h) {
  FILE* file = fopen(path, "wb");
  if (!file) return false;
  fprintf(file, "P6\n%d %d\n255\n", w, h);
  for (int i = 0; i < w * h; i++) {
    uint16_t p = frame[i];
    uint8_t rgb[3] = {
      (uint8_t)(((p >> 11) & 0x1F) * 255 / 31),
      (uint8_t)(((p >> 5) & 0x3F) * 255 / 63),
      (uint8_t)((p & 0x1F) * 255 / 31),
    };
    fwrite(rgb, 1, 3, file);
  }
  fclose(file);
  return true;
}

static void usage() {
  std::cerr
      << "usage: smalltv-emulator [options]\n"
      << "  --view PATH       draw-list JSON (default examples/view.json)\n"
      << "  --usage PATH      normalized usage JSON (default daemon/usage.example.json)\n"
      << "  --time HH:MM      fixed clock time instead of the local clock\n"
      << "  --scale N         window scale 1-8 (default 2)\n"
      << "  --screenshot PATH write a 240x240 P6 PPM and exit\n";
}

int main(int argc, char** argv) {
  const char* viewPath = "examples/view.json";
  const char* usagePath = "daemon/usage.example.json";
  const char* screenshot = nullptr;
  const char* timeText = nullptr;
  int scale = 2;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--view") && i + 1 < argc) viewPath = argv[++i];
    else if (!strcmp(argv[i], "--usage") && i + 1 < argc) usagePath = argv[++i];
    else if (!strcmp(argv[i], "--time") && i + 1 < argc) timeText = argv[++i];
    else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
    else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
    else { usage(); return 2; }
  }
  if (scale < 1) scale = 1;
  if (scale > 8) scale = 8;

  if (timeText) {
    int hh = 0, mm = 0;
    if (sscanf(timeText, "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 ||
        mm < 0 || mm > 59) {
      std::cerr << "invalid --time, expected HH:MM\n";
      return 2;
    }
    if (!local_time(std::time(nullptr), g_fixedTm)) {
      std::cerr << "cannot read the local clock\n";
      return 1;
    }
    g_fixedTm.tm_hour = hh;
    g_fixedTm.tm_min = mm;
    g_fixedTm.tm_sec = 0;
    g_fixedTime = true;
  }

  std::string viewJson;
  if (!read_file(viewPath, viewJson)) {
    std::cerr << "cannot read view: " << viewPath << "\n";
    return 1;
  }
  ViewScreen screen;
  String viewError;
  if (!viewScreenParse(viewJson.c_str(), viewJson.size(), screen, viewError)) {
    std::cerr << "invalid view: " << viewError.c_str() << "\n";
    return 1;
  }

  usageInit();
  std::string usageJson;
  if (read_file(usagePath, usageJson)) {
    if (!usageApply(String(usageJson.c_str(), usageJson.size())))
      std::cerr << "warning: usage JSON was not accepted: " << usagePath << "\n";
  } else {
    std::cerr << "usage file not found, showing waiting state: " << usagePath << "\n";
  }

  viewRender(screen);

  if (screenshot) {
    if (!write_ppm(screenshot, gfxDev()->getFramebuffer(), gfxDev()->width(),
                   gfxDev()->height())) {
      std::cerr << "cannot write screenshot: " << screenshot << "\n";
      return 1;
    }
    std::cout << "wrote " << screenshot << " (" << gfxDev()->width() << "x"
              << gfxDev()->height() << ")\n";
    return 0;
  }

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::cerr << "SDL init failed: " << SDL_GetError() << "\n";
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow(
      "SmallTV Emulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      gfxDev()->width() * scale, gfxDev()->height() * scale, SDL_WINDOW_SHOWN);
  SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, 0) : nullptr;
  SDL_Texture* texture = renderer
      ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                          SDL_TEXTUREACCESS_STREAMING, gfxDev()->width(),
                          gfxDev()->height())
      : nullptr;
  if (!window || !renderer || !texture) {
    std::cerr << "SDL window failed: " << SDL_GetError() << "\n";
    return 1;
  }

  auto present = [&]() {
    SDL_UpdateTexture(texture, nullptr, gfxDev()->getFramebuffer(),
                      gfxDev()->width() * sizeof(uint16_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
  };
  present();

  bool running = true;
  uint32_t lastPaint = SDL_GetTicks();
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE))
        running = false;
    }
    uint32_t now = SDL_GetTicks();
    if (now - lastPaint >= 1000) {
      lastPaint = now;
      viewRender(screen);
      present();
    }
    SDL_Delay(30);
  }

  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
