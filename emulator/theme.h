#pragma once
#include <SDL.h>
#include <imgui.h>
#include <string>

namespace theme {

struct Fonts {
  ImFont* body = nullptr;
  ImFont* medium = nullptr;
  ImFont* heading = nullptr;
  ImFont* display = nullptr;
};

bool init(SDL_Renderer* renderer, const std::string& assetsDir);
void shutdown();
const Fonts& fonts();

void applyStyle();

ImU32 bg();
ImU32 surface();
ImU32 surfaceAlt();
ImU32 border();
ImU32 text();
ImU32 muted();
ImU32 accent();
ImU32 success();
ImU32 warning();
ImU32 danger();

void panel(ImDrawList* dl, const ImVec2& a, const ImVec2& b,
           ImU32 fill, ImU32 borderColor, float rounding = 6.0f);
void sectionLabel(const char* label);

bool primaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
bool secondaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
bool dangerButton(const char* label, const ImVec2& size = ImVec2(0, 0));

}  // namespace theme
