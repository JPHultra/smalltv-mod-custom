#include "theme.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace theme {
namespace {

constexpr ImU32 kBg = IM_COL32(14, 16, 19, 255);
constexpr ImU32 kSurface = IM_COL32(22, 25, 29, 255);
constexpr ImU32 kSurfaceAlt = IM_COL32(28, 32, 38, 255);
constexpr ImU32 kBorder = IM_COL32(42, 47, 54, 255);
constexpr ImU32 kText = IM_COL32(231, 233, 236, 255);
constexpr ImU32 kMuted = IM_COL32(138, 146, 156, 255);
constexpr ImU32 kAccent = IM_COL32(242, 169, 59, 255);
constexpr ImU32 kSuccess = IM_COL32(70, 201, 139, 255);
constexpr ImU32 kWarning = IM_COL32(242, 169, 59, 255);
constexpr ImU32 kDanger = IM_COL32(240, 93, 111, 255);

Fonts g_fonts;

std::string path_join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (a.back() == '/' || a.back() == '\\') return a + b;
  return a + "/" + b;
}

ImFont* load_font(const std::string& path, float size) {
  if (path.empty()) return nullptr;
  ImFontConfig cfg;
  cfg.OversampleH = 2;
  cfg.OversampleV = 2;
  cfg.PixelSnapH = true;
  return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), size, &cfg);
}

}  // namespace

bool init(SDL_Renderer* renderer, const std::string& assetsDir) {
  (void)renderer;
  applyStyle();

  const std::string regular = path_join(assetsDir, "fonts/Inter-Regular.ttf");
  const std::string medium = path_join(assetsDir, "fonts/Inter-Medium.ttf");
  const std::string semibold = path_join(assetsDir, "fonts/Inter-SemiBold.ttf");
  g_fonts.body = load_font(regular, 14.0f);
  g_fonts.medium = load_font(medium, 14.0f);
  g_fonts.heading = load_font(semibold, 18.0f);
  g_fonts.display = load_font(semibold, 26.0f);
  if (!g_fonts.body) g_fonts.body = ImGui::GetIO().Fonts->AddFontDefault();
  if (!g_fonts.medium) g_fonts.medium = g_fonts.body;
  if (!g_fonts.heading) g_fonts.heading = g_fonts.body;
  if (!g_fonts.display) g_fonts.display = g_fonts.heading;
  ImGui::GetIO().FontDefault = g_fonts.body;
  return true;
}

void shutdown() {}

const Fonts& fonts() { return g_fonts; }

ImU32 bg() { return kBg; }
ImU32 surface() { return kSurface; }
ImU32 surfaceAlt() { return kSurfaceAlt; }
ImU32 border() { return kBorder; }
ImU32 text() { return kText; }
ImU32 muted() { return kMuted; }
ImU32 accent() { return kAccent; }
ImU32 success() { return kSuccess; }
ImU32 warning() { return kWarning; }
ImU32 danger() { return kDanger; }

void applyStyle() {
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 0.0f;
  s.ChildRounding = 0.0f;
  s.FrameRounding = 4.0f;
  s.PopupRounding = 6.0f;
  s.ScrollbarRounding = 4.0f;
  s.GrabRounding = 4.0f;
  s.TabRounding = 4.0f;
  s.WindowBorderSize = 1.0f;
  s.ChildBorderSize = 1.0f;
  s.PopupBorderSize = 1.0f;
  s.FrameBorderSize = 1.0f;
  s.WindowPadding = ImVec2(12, 10);
  s.FramePadding = ImVec2(9, 6);
  s.ItemSpacing = ImVec2(8, 7);
  s.ItemInnerSpacing = ImVec2(6, 5);
  s.CellPadding = ImVec2(8, 6);
  s.ScrollbarSize = 9.0f;
  s.GrabMinSize = 9.0f;

  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = ImVec4(0.906f, 0.914f, 0.925f, 1.0f);
  c[ImGuiCol_TextDisabled] = ImVec4(0.541f, 0.573f, 0.612f, 1.0f);
  c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.063f, 0.075f, 1.0f);
  c[ImGuiCol_ChildBg] = ImVec4(0.086f, 0.098f, 0.114f, 1.0f);
  c[ImGuiCol_PopupBg] = ImVec4(0.086f, 0.098f, 0.114f, 1.0f);
  c[ImGuiCol_Border] = ImVec4(0.165f, 0.184f, 0.212f, 1.0f);
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = ImVec4(0.110f, 0.125f, 0.149f, 1.0f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.133f, 0.153f, 0.180f, 1.0f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.157f, 0.180f, 0.212f, 1.0f);
  c[ImGuiCol_TitleBg] = ImVec4(0.086f, 0.098f, 0.114f, 1.0f);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.086f, 0.098f, 0.114f, 1.0f);
  c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.086f, 0.098f, 0.114f, 1.0f);
  c[ImGuiCol_MenuBarBg] = ImVec4(0.086f, 0.098f, 0.114f, 1.0f);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0.055f, 0.063f, 0.075f, 1.0f);
  c[ImGuiCol_ScrollbarGrab] = ImVec4(0.224f, 0.247f, 0.278f, 1.0f);
  c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.290f, 0.318f, 0.353f, 1.0f);
  c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.949f, 0.663f, 0.231f, 1.0f);
  c[ImGuiCol_CheckMark] = ImVec4(0.949f, 0.663f, 0.231f, 1.0f);
  c[ImGuiCol_SliderGrab] = ImVec4(0.949f, 0.663f, 0.231f, 1.0f);
  c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.718f, 0.322f, 1.0f);
  c[ImGuiCol_Button] = ImVec4(0.110f, 0.125f, 0.149f, 1.0f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.157f, 0.180f, 0.212f, 1.0f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.196f, 0.224f, 0.259f, 1.0f);
  c[ImGuiCol_Header] = ImVec4(0.949f, 0.663f, 0.231f, 0.14f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.949f, 0.663f, 0.231f, 0.20f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.949f, 0.663f, 0.231f, 0.28f);
  c[ImGuiCol_Separator] = ImVec4(0.165f, 0.184f, 0.212f, 1.0f);
  c[ImGuiCol_SeparatorHovered] = ImVec4(0.949f, 0.663f, 0.231f, 0.5f);
  c[ImGuiCol_SeparatorActive] = ImVec4(0.949f, 0.663f, 0.231f, 0.8f);
  c[ImGuiCol_ResizeGrip] = ImVec4(0.949f, 0.663f, 0.231f, 0.18f);
  c[ImGuiCol_ResizeGripHovered] = ImVec4(0.949f, 0.663f, 0.231f, 0.32f);
  c[ImGuiCol_ResizeGripActive] = ImVec4(0.949f, 0.663f, 0.231f, 0.5f);
  c[ImGuiCol_Tab] = ImVec4(0.110f, 0.125f, 0.149f, 1.0f);
  c[ImGuiCol_TabHovered] = ImVec4(0.157f, 0.180f, 0.212f, 1.0f);
  c[ImGuiCol_TabSelected] = ImVec4(0.196f, 0.224f, 0.259f, 1.0f);
  c[ImGuiCol_TabDimmed] = ImVec4(0.086f, 0.098f, 0.114f, 1.0f);
  c[ImGuiCol_TabDimmedSelected] = ImVec4(0.133f, 0.153f, 0.180f, 1.0f);
  c[ImGuiCol_PlotLines] = ImVec4(0.949f, 0.663f, 0.231f, 1.0f);
  c[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 0.718f, 0.322f, 1.0f);
  c[ImGuiCol_PlotHistogram] = ImVec4(0.949f, 0.663f, 0.231f, 1.0f);
  c[ImGuiCol_PlotHistogramHovered] = ImVec4(1.0f, 0.718f, 0.322f, 1.0f);
  c[ImGuiCol_TableHeaderBg] = ImVec4(0.110f, 0.125f, 0.149f, 1.0f);
  c[ImGuiCol_TableBorderStrong] = ImVec4(0.165f, 0.184f, 0.212f, 1.0f);
  c[ImGuiCol_TableBorderLight] = ImVec4(0.165f, 0.184f, 0.212f, 0.6f);
  c[ImGuiCol_TextSelectedBg] = ImVec4(0.949f, 0.663f, 0.231f, 0.24f);
  c[ImGuiCol_DragDropTarget] = ImVec4(0.949f, 0.663f, 0.231f, 0.8f);
  c[ImGuiCol_NavCursor] = ImVec4(0.949f, 0.663f, 0.231f, 0.9f);
  c[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.7f);
  c[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.4f);
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.72f);
}

void panel(ImDrawList* dl, const ImVec2& a, const ImVec2& b,
           ImU32 fill, ImU32 borderColor, float rounding) {
  if (b.x <= a.x || b.y <= a.y) return;
  dl->AddRectFilled(a, b, fill, rounding);
  dl->AddRect(a, b, borderColor, rounding, 0, 1.0f);
}

void sectionLabel(const char* label) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kMuted));
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
}

bool primaryButton(const char* label, const ImVec2& size) {
  ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kAccent));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.718f, 0.322f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.85f, 0.56f, 0.16f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.075f, 0.082f, 0.094f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(kAccent));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 6));
  const bool clicked = ImGui::Button(label, size);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(5);
  return clicked;
}

bool secondaryButton(const char* label, const ImVec2& size) {
  ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kSurfaceAlt));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.157f, 0.180f, 0.212f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.196f, 0.224f, 0.259f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(kBorder));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 6));
  const bool clicked = ImGui::Button(label, size);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(4);
  return clicked;
}

bool dangerButton(const char* label, const ImVec2& size) {
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.11f, 0.13f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.13f, 0.16f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.40f, 0.15f, 0.19f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kDanger));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.40f, 0.16f, 0.20f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 6));
  const bool clicked = ImGui::Button(label, size);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(5);
  return clicked;
}

}  // namespace theme
