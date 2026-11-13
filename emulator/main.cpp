// SmallTV native desktop app: live dashboard preview, device manager and
// visual dashboard editor.  SDL2 + Dear ImGui, no local web server.
#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif

#include "Gfx.h"
#include "Arduino_GFX_Library.h"
#include "manager.h"
#include "UsageClient.h"
#include "ViewModel.h"
#include "ViewRender.h"
#include "theme.h"
#include "usage/SecureStore.h"
#include "device/DeviceStore.h"
#include "usage/UsageCore.h"
#include "usage/DeepSeekProxy.h"
#include "usage/Worker.h"
#include "update/GitHubRelease.h"
#include "image/ImageCodec.h"
#include "image/HostImageSource.h"

namespace fs = std::filesystem;

void gfxSetPreviewDisplay(int order, bool invert, int rGain, int gGain, int bGain);

static void fatal_error(const std::string& message) {
  std::cerr << message << "\n";
#if defined(_WIN32)
  MessageBoxA(nullptr, message.c_str(), "SmallTV", MB_OK | MB_ICONERROR);
#endif
}

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

static bool write_file(const std::string& path, const std::string& data) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return false;
  file.write(data.data(), (std::streamsize)data.size());
  return (bool)file;
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

static void print_usage() {
  std::cerr
      << "usage: smalltv-emulator [options]\n"
      << "  --view PATH       draw-list JSON (default examples/view.json)\n"
      << "  --usage PATH      normalized usage JSON (default daemon/usage.example.json)\n"
      << "  --time HH:MM      fixed clock time instead of the local clock\n"
      << "  --tab NAME        start on device, design, usage or settings\n"
      << "  --settings-tab NAME start settings on wifi, display, view, system or json\n"
      << "  --usage-worker    run the headless usage collector\n"
      << "  --usage-device URL device to push usage to\n"
      << "  --url URL         connect to a device at startup\n"
      << "  --offline         start in Design and Usage without a device\n"
      << "  --user USER       device web UI user\n"
      << "  --pass PASS       device web UI password\n"
      << "  --screenshot PATH write a 240x240 P6 PPM and exit\n";
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static std::string json_text(const std::string& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return body;
  std::string out;
  serializeJsonPretty(doc, out);
  return out;
}

static std::string hex_color(JsonVariant v, const char* fallback = "#ffffff") {
  if (v.is<const char*>()) return v.as<const char*>();
  return fallback;
}

static uint32_t hex_to_rgb(const std::string& s) {
  if (s.size() != 7 || s[0] != '#') return 0xffffff;
  return (uint32_t)strtoul(s.c_str() + 1, nullptr, 16);
}

static std::string rgb_to_hex(uint32_t rgb) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%06x", rgb & 0xffffff);
  return buf;
}

static ImVec4 rgb_vec(uint32_t rgb) {
  return ImVec4(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f,
                (rgb & 255) / 255.0f, 1.0f);
}

static bool g_formGrid = false;
static void formNext() { if (g_formGrid) ImGui::TableNextColumn(); }

static bool input_text_json(JsonObject o, const char* key, const char* label,
                            bool secret = false, const char* hint = nullptr) {
  formNext();
  const char* cur = o[key] | "";
  char buf[1024];
  snprintf(buf, sizeof(buf), "%s", cur ? cur : "");
  if (label && *label) ImGui::TextDisabled("%s", label);
  const std::string id = std::string("##") + key + label;
  ImGuiInputTextFlags flags = secret ? ImGuiInputTextFlags_Password : 0;
  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputText(id.c_str(), buf, sizeof(buf), flags)) {
    if (!secret || buf[0]) o[key] = buf;
    return true;
  }
  if (hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", hint);
  return false;
}

static bool input_int_json(JsonObject o, const char* key, const char* label,
                           int lo, int hi, int fallback = 0) {
  formNext();
  int value = o[key] | fallback;
  if (label && *label) ImGui::TextDisabled("%s", label);
  const std::string id = std::string("##") + key + label;
  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputInt(id.c_str(), &value)) {
    value = std::max(lo, std::min(hi, value));
    o[key] = value;
    return true;
  }
  return false;
}

static bool input_px_size(JsonObject o, const char* px_key, const char* scale_key,
                          const char* label, int step, int min_px, int max_px,
                          int max_scale, int fallback_scale = 1) {
  formNext();
  const int scale = o[scale_key] | fallback_scale;
  int value = o[px_key].isNull() ? scale * step : (int)(o[px_key] | 0);
  if (value > 0) value = ((value + step / 2) / step) * step;
  value = std::max(min_px, std::min(max_px, value));
  if (label && *label) ImGui::TextDisabled("%s", label);
  const std::string id = std::string("##") + px_key + label;
  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputInt(id.c_str(), &value)) {
    if (value > 0) value = ((value + step / 2) / step) * step;
    value = std::max(min_px, std::min(max_px, value));
    o[px_key] = value;
    o[scale_key] = value > 0 ? std::max(1, std::min(max_scale, value / step)) : 0;
    return true;
  }
  return false;
}

static bool input_bool_json(JsonObject o, const char* key, const char* label) {
  formNext();
  bool value = o[key] | false;
  if (label && *label) ImGui::TextDisabled("%s", label);
  const std::string id = std::string("##") + key + label;
  if (ImGui::Checkbox(id.c_str(), &value)) {
    o[key] = value;
    return true;
  }
  return false;
}

static bool color_edit_json(JsonObject o, const char* key, const char* label,
                            const char* fallback = "#ffffff") {
  uint32_t rgb = hex_to_rgb(hex_color(o[key], fallback));
  ImVec4 c = rgb_vec(rgb);
  if (ImGui::ColorEdit3(label, (float*)&c, ImGuiColorEditFlags_NoInputs)) {
    uint32_t r = (uint32_t)(c.x * 255.0f + .5f);
    uint32_t g = (uint32_t)(c.y * 255.0f + .5f);
    uint32_t b = (uint32_t)(c.z * 255.0f + .5f);
    o[key] = rgb_to_hex((r << 16) | (g << 8) | b).c_str();
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// In-app file browser.  No zenity/kdialog or WebView dependency.
// ---------------------------------------------------------------------------
struct FileDialog {
  bool active = false;
  bool save = false;
  bool requestOpen = false;
  std::string title;
  std::string path;
  std::string filter;
  std::string filename;
  std::function<void(const std::string&)> onPick;
  std::vector<std::string> dirs;
  std::vector<std::string> files;
  char pathBuf[1024] = {};
  char nameBuf[256] = {};

  void refresh() {
    dirs.clear();
    files.clear();
    std::error_code ec;
    fs::path p(path);
    if (p.empty() || !fs::is_directory(p, ec)) {
      p = fs::current_path(ec);
      if (ec) p = "/";
      path = p.string();
    }
    std::vector<std::string> ds, ffs;
    for (fs::directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
      const auto name = it->path().filename().string();
      if (name.empty() || name[0] == '.') continue;
      std::error_code e2;
      if (it->is_directory(e2)) ds.push_back(name);
      else if (it->is_regular_file(e2)) {
        if (filter.empty()) { ffs.push_back(name); continue; }
        std::string ext = it->path().extension().string();
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        size_t start = 0;
        while (start <= filter.size()) {
          const size_t comma = filter.find(',', start);
          std::string wanted = filter.substr(start, comma == std::string::npos
                                                        ? std::string::npos : comma - start);
          for (char& c : wanted) c = (char)tolower((unsigned char)c);
          if (wanted == ext) { ffs.push_back(name); break; }
          if (comma == std::string::npos) break;
          start = comma + 1;
        }
      }
    }
    std::sort(ds.begin(), ds.end());
    std::sort(ffs.begin(), ffs.end());
    dirs = std::move(ds);
    files = std::move(ffs);
    snprintf(pathBuf, sizeof(pathBuf), "%s", path.c_str());
  }

  void openOpen(const std::string& t, const std::string& f,
                std::function<void(const std::string&)> cb) {
    active = true;
    save = false;
    requestOpen = true;
    title = t;
    filter = f;
    onPick = std::move(cb);
    path = fs::current_path().string();
    filename.clear();
    refresh();
  }

  void openSave(const std::string& t, const std::string& f, const std::string& name,
                std::function<void(const std::string&)> cb) {
    active = true;
    save = true;
    requestOpen = true;
    title = t;
    filter = f;
    filename = name;
    onPick = std::move(cb);
    path = fs::current_path().string();
    refresh();
    snprintf(nameBuf, sizeof(nameBuf), "%s", filename.c_str());
  }

  void pick(const std::string& full) {
    auto cb = onPick;
    active = false;
    ImGui::CloseCurrentPopup();
    if (cb) cb(full);
  }

  void draw() {
    if (!active) return;
    if (requestOpen) {
      ImGui::OpenPopup(title.c_str());
      requestOpen = false;
    }
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(title.c_str(), &active, ImGuiWindowFlags_NoResize)) return;
    ImGui::SetNextItemWidth(-120);
    if (ImGui::InputText("##path", pathBuf, sizeof(pathBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
      path = pathBuf;
      refresh();
    }
    ImGui::SameLine();
    if (theme::secondaryButton("Up")) {
      fs::path p(path);
      if (p.has_parent_path() && p.parent_path() != p) {
        path = p.parent_path().string();
        refresh();
      }
    }
    ImGui::SameLine();
    if (theme::secondaryButton("Home")) {
      path = fs::current_path().string();
      refresh();
    }
    ImGui::Separator();
    ImGui::BeginChild("##files", ImVec2(0, save ? -70 : -40), true);
    for (const auto& d : dirs) {
      std::string label = "[dir] " + d;
      if (ImGui::Selectable(label.c_str(), false,
                            ImGuiSelectableFlags_AllowDoubleClick)) {
        if (ImGui::IsMouseDoubleClicked(0)) {
          path = (fs::path(path) / d).string();
          refresh();
        }
      }
    }
    for (const auto& f : files) {
      bool selected = (f == filename);
      if (ImGui::Selectable(f.c_str(), selected,
                            ImGuiSelectableFlags_AllowDoubleClick)) {
        filename = f;
        if (ImGui::IsMouseDoubleClicked(0)) pick((fs::path(path) / f).string());
      }
    }
    ImGui::EndChild();
    if (save) {
      ImGui::SetNextItemWidth(-120);
      ImGui::InputText("File name", nameBuf, sizeof(nameBuf));
      filename = nameBuf;
    }
    if (theme::primaryButton(save ? "Save" : "Open")) {
      if (save) {
        if (!filename.empty()) pick((fs::path(path) / filename).string());
      } else if (!filename.empty()) {
        pick((fs::path(path) / filename).string());
      }
    }
    ImGui::SameLine();
    if (theme::secondaryButton("Cancel")) {
      active = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
};

// ---------------------------------------------------------------------------
// App
// ---------------------------------------------------------------------------
static SDL_Texture* g_texture = nullptr;
static SDL_Renderer* g_renderer = nullptr;

static void upload_preview_texture() {
  if (!g_texture || !gfxDev()) return;
  SDL_UpdateTexture(g_texture, nullptr, gfxDev()->getFramebuffer(),
                    gfxDev()->width() * sizeof(uint16_t));
}

static std::string find_assets_dir() {
  std::error_code ec;
  std::vector<fs::path> candidates;
  candidates.push_back(fs::current_path(ec) / "assets");
  candidates.push_back(fs::current_path(ec) / "emulator" / "assets");
#if SDL_VERSION_ATLEAST(2, 0, 1)
  char* base = SDL_GetBasePath();
  if (base) {
    fs::path exe(base);
    SDL_free(base);
    candidates.push_back(exe / "assets");
    candidates.push_back(exe / ".." / "assets");
    candidates.push_back(exe / ".." / ".." / "emulator" / "assets");
  }
#endif
  for (const auto& path : candidates) {
    if (fs::exists(path / "fonts" / "Inter-Regular.ttf", ec)) return path.string();
  }
  return (fs::current_path(ec) / "emulator" / "assets").string();
}

struct App {
  enum class ConnState { Disconnected, Connecting, Online, Degraded, Offline, AuthRequired, Updating };
  struct HeartbeatResult {
    uint32_t seq = 0;
    bool ok = false;
    int status = 0;
    std::string body;
    uint32_t latencyMs = 0;
  };

  DeviceManager manager;
  std::vector<EmuDevice> devices;
  int selectedDevice = -1;
  char deviceUrl[192] = "http://192.168.4.1";
  char authUser[96] = "";
  char authPass[96] = "";
  bool connected = false;
  ConnState connState = ConnState::Disconnected;
  std::string connError;
  std::string lastError;
  std::string toast;
  uint32_t toastUntil = 0;
  uint32_t lastSuccessAt = 0;
  uint32_t lastAttemptAt = 0;
  uint32_t lastLatencyMs = 0;
  int consecutiveFailures = 0;
  bool heartbeatRunning = false;
  uint32_t heartbeatSeq = 0;
  std::future<HeartbeatResult> heartbeatFuture;
  enum class AppMode { Connection, Online, Offline };
  AppMode appMode = AppMode::Connection;
  bool connectionsLoaded = false;
  bool connectionActive = false;
  bool profileDirty = false;
  std::string savedUrl;
  std::string savedUser;
  std::string deviceSecretError;
  bool rememberPassword = true;
  bool autoConnect = true;
  bool disconnectPrompt = false;
  int tab = 0;
  int forceTab = -1;
  int settingsTab = 0;
  bool scanRequested = false;

  JsonDocument cfg;
  bool cfgLoaded = false;
  char cfgRaw[65536] = {};
  std::string status;
  std::string deviceVersion;
  std::string usage;
  std::string liveUsage;
  int usagePreviewMode = 0;  // 0 live, 1 waiting, 2 error, 3 long values
  bool usagePreviewActive = false;
  std::string styleClipboard;
  bool imageManagerOpen = false;
  bool imageManagerLoaded = false;
  std::string imageListJson;
  std::string imageManagerError;
  uint32_t lastAutosave = 0;
  std::string lastAutosaveContent;
  std::string scanJson;
  JsonDocument scanDoc;
  JsonDocument usageCreds;
  bool usageCredsLoaded = false;
  std::string usageCredsError;
  std::vector<usage::Result> usageResults;
  bool usageFetchRunning = false;
  std::future<usage::FetchResult> usageFuture;
  std::string usageError;
  char usageProxyUrl[192] = "http://127.0.0.1:8789/usage";
  usage::DeepSeekProxy deepSeekProxy;
  bool deepSeekProxyRunning = false;
  std::string updateJson;
  std::string latestVersion;
  bool updateAvailable = false;
  bool updateWatch = false;
  std::string uploadStartVersion;
  std::string updateStartVersion;
  std::string updateTarget;
  std::string updatePhase;
  std::string updateMessage;
  std::string updateError;
  int updateProgress = -1;
  uint32_t updateStartedAt = 0;
  bool pendingInitialLoad = false;
  uint32_t lastPoll = 0;

  JsonDocument view;
  bool viewLoaded = false;
  bool viewDirty = false;
  std::string viewError;
  bool previewDirty = true;
  uint32_t previewAnimStart = 0;
  uint32_t lastPreviewTick = 0;
  int selected = -1;
  std::set<int> selection;
  std::string clipboard;
  bool snapEnabled = true;
  bool showGuides = true;
  bool showRulers = true;
  int snapGrid = 8;
  bool componentsDrawerOpen = true;
  bool documentDrawerOpen = false;
  bool inspectorDismissed = false;
  float componentsDrawerWidth = 230.0f;
  float documentDrawerWidth = 270.0f;
  float inspectorDrawerWidth = 300.0f;
  std::string lastSelectionSignature;
  float snapGuideX = -1.0f;
  float snapGuideY = -1.0f;
  int usageLayoutRow = 0;
  int usageDragKind = 0;  // 1 name, 2 reset, 3 label, 4 value, 5 bar
  int usageDragRow = 0;
  int usageSelectedKind = 0;
  int usageSelectedRow = 0;
  char viewJsonBuf[65536] = {};
  std::vector<std::string> undoStack;
  std::vector<std::string> redoStack;
  bool showJsonEditor = false;

  std::string firmwarePath;
  std::future<bool> uploadFuture;
  bool uploadRunning = false;
  std::string uploadError;
  std::string diagText;
  std::string diagTitle;
  bool diagOpen = false;
  std::string initialViewPath = "examples/view.json";
  std::string executablePath;
  bool workerServiceActive = false;
  bool workerServiceChecked = false;
  appupdate::Release appRelease;
  std::future<appupdate::Release> appUpdateFuture;
  std::future<bool> appUpdateDownloadFuture;
  bool appUpdateChecking = false;
  bool appUpdatePrompt = false;
  bool appUpdateModalOpen = false;
  bool appUpdateDownloading = false;
  bool appUpdateManualCheck = false;
  bool appUpdateWorkerWasActive = false;
  std::string appUpdateSkipped;
  std::string appUpdateError;
  std::string appUpdateArchive;
  std::string appUpdateRoot;
  std::vector<appupdate::Release> firmwareReleases;
  std::future<std::vector<appupdate::Release>> firmwareReleasesFuture;
  bool firmwareReleasesLoading = false;
  std::string firmwareReleaseError;
  int firmwareReleaseIndex = 0;
  FileDialog dialog;

  bool beginPane(const char* id, ImVec2 size, bool scroll = true) {
    if (size.x <= 0) size.x = ImGui::GetContentRegionAvail().x;
    if (size.y <= 0) size.y = ImGui::GetContentRegionAvail().y;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 b(p.x + size.x, p.y + size.y);
    theme::panel(ImGui::GetWindowDrawList(), p, b, theme::surface(), theme::border());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoBackground |
                          (scroll ? 0 : ImGuiWindowFlags_NoScrollbar));
    return true;
  }

  void endPane() {
    ImGui::EndChild();
    ImGui::PopStyleVar();
  }

  bool beginDrawer(const char* id, float width, const char* title, bool* open) {
    if (width <= 0) width = 240;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (width > avail.x - 120) width = std::max(160.0f, avail.x - 120);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 b(p.x + width, p.y + avail.y);
    theme::panel(ImGui::GetWindowDrawList(), p, b, theme::surface(), theme::border(), 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 10));
    ImGui::BeginChild(id, ImVec2(width, avail.y),
                      ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PushFont(theme::fonts().heading);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::SameLine(std::max(20.0f, width - 34));
    if (ImGui::SmallButton("x")) *open = false;
    ImGui::Separator();
    return *open;
  }

  void endDrawer() {
    ImGui::EndChild();
    ImGui::PopStyleVar();
  }

  void paneTitle(const char* title, const char* trailing = nullptr) {
    ImGui::PushFont(theme::fonts().heading);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (trailing) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", trailing);
    }
  }

  void drawGrid(ImU32 color) {
    const ImVec2 a = ImGui::GetWindowPos();
    const ImVec2 b(a.x + ImGui::GetWindowSize().x, a.y + ImGui::GetWindowSize().y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (float x = a.x + 20; x < b.x; x += 24) dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), color);
    for (float y = a.y + 20; y < b.y; y += 24) dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), color);
  }

  void statusPill() {
    const ImU32 col = connColor(connState);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(p.x + 4, p.y + 9), 4.0f, col);
    ImGui::Dummy(ImVec2(10, 0));
    ImGui::SameLine();
    const char* label = appMode == AppMode::Offline ? "Offline" : connLabel(connState);
    const ImU32 labelColor = appMode == AppMode::Offline ? theme::warning() : col;
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(labelColor), "%s", label);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Target: %s\nLast seen: %s\nLatency: %u ms%s%s",
                        deviceUrl, lastSeenText().c_str(), lastLatencyMs,
                        connError.empty() ? "" : "\nLast error: ",
                        connError.c_str());
    }
  }

  void topbar() {
    const char* names[] = {"Device", "Design", "Usage", "Settings"};
    const int ids[] = {0, 1, 2, 3};
    const int tabCount = appMode == AppMode::Offline ? 2 : 4;
    const int first = appMode == AppMode::Offline ? 1 : 0;
    ImGui::PushFont(theme::fonts().heading);
    ImGui::TextUnformatted("SmallTV");
    ImGui::PopFont();
    ImGui::SameLine(112);
    for (int k = 0; k < tabCount; k++) {
      const int id = ids[first + k];
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 9));
      ImGui::PushStyleColor(ImGuiCol_Button, id == tab ? theme::surfaceAlt() : theme::surface());
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::surfaceAlt());
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::surfaceAlt());
      ImGui::PushStyleColor(ImGuiCol_Text, id == tab ? theme::accent() : theme::muted());
      if (ImGui::Button(names[id], ImVec2(0, 30))) tab = id;
      ImGui::PopStyleColor(4);
      ImGui::PopStyleVar();
      if (id == tab) {
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + 10, b.y - 2),
                                                  ImVec2(b.x - 10, b.y),
                                                  theme::accent(), 1.0f);
      }
      ImGui::SameLine(0, 2);
    }
    ImGui::SameLine(0, 22);
    statusPill();
    ImGui::SameLine(0, 18);
    ImGui::TextDisabled("%s", deviceUrl[0] ? deviceUrl : "No device");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 28.0f);
    if (theme::secondaryButton("...")) ImGui::OpenPopup("##connection");
    if (ImGui::BeginPopup("##connection")) {
      ImGui::TextDisabled("CONNECTION");
      ImGui::SetNextItemWidth(300);
      ImGui::InputTextWithHint("##url", "device URL", deviceUrl, sizeof(deviceUrl));
      ImGui::SetNextItemWidth(140);
      ImGui::InputTextWithHint("##user", "web user", authUser, sizeof(authUser));
      ImGui::SameLine();
      ImGui::SetNextItemWidth(160);
      ImGui::InputTextWithHint("##pass", "web password", authPass, sizeof(authPass), ImGuiInputTextFlags_Password);
      if (theme::primaryButton("Connect")) {
        connect();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      scanButton("Scan");
      if (deviceUrl[0]) {
        ImGui::Separator();
        ImGui::TextDisabled("STATE");
        ImGui::Text("%s", connLabel(connState));
        ImGui::TextDisabled("Last seen: %s", lastSeenText().c_str());
        if (lastLatencyMs) ImGui::TextDisabled("Latency: %u ms", lastLatencyMs);
        if (!connError.empty()) ImGui::TextWrapped("%s", connError.c_str());
        if (theme::secondaryButton("Switch device")) {
          requestConnectionScreen();
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (theme::secondaryButton("Disconnect")) {
          requestConnectionScreen();
          ImGui::CloseCurrentPopup();
        }
      }
      ImGui::EndPopup();
    }
  }

  void toastWindow() {
    if (toast.empty() || SDL_GetTicks() >= toastUntil) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 380, vp->Pos.y + vp->Size.y - 52));
    ImGui::SetNextWindowSize(ImVec2(360, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const ImVec2 p = ImGui::GetWindowPos();
    const ImVec2 sz = ImGui::GetWindowSize();
    theme::panel(ImGui::GetWindowDrawList(), p, ImVec2(p.x + sz.x, p.y + sz.y),
                 theme::surfaceAlt(), theme::border());
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 3, p.y + sz.y), theme::accent());
    ImGui::SetCursorPos(ImVec2(14, 10));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 330);
    ImGui::TextUnformatted(toast.c_str());
    ImGui::PopTextWrapPos();
    ImGui::End();
    ImGui::PopStyleVar();
  }

  void notify(const std::string& msg) {
    toast = msg;
    toastUntil = SDL_GetTicks() + 5000;
  }

  static const char* connLabel(ConnState state) {
    switch (state) {
      case ConnState::Connecting: return "Connecting";
      case ConnState::Online: return "Online";
      case ConnState::Degraded: return "Degraded";
      case ConnState::Offline: return "Offline";
      case ConnState::AuthRequired: return "Auth required";
      case ConnState::Updating: return "Updating";
      default: return "Disconnected";
    }
  }

  static ImU32 connColor(ConnState state) {
    switch (state) {
      case ConnState::Online: return theme::success();
      case ConnState::Connecting:
      case ConnState::Updating: return theme::accent();
      case ConnState::Degraded: return theme::warning();
      case ConnState::AuthRequired:
      case ConnState::Offline: return theme::danger();
      default: return theme::muted();
    }
  }

  bool online() const { return connState == ConnState::Online; }

  std::string lastSeenText() const {
    if (!lastSuccessAt) return "never";
    const uint32_t age = (SDL_GetTicks() - lastSuccessAt) / 1000;
    if (age < 2) return "just now";
    if (age < 60) return std::to_string(age) + "s ago";
    return std::to_string(age / 60) + "m ago";
  }

  void finishUpdate(bool ok, const std::string& message) {
    updateWatch = false;
    updatePhase = ok ? "done" : "error";
    updateMessage = message;
    updateError = ok ? "" : message;
    updateProgress = ok ? 100 : -1;
    if (ok) updateAvailable = false;
    const bool recentlyOnline = lastSuccessAt &&
        (SDL_GetTicks() - lastSuccessAt) < 15000;
    connected = ok || recentlyOnline;
    connState = connected ? ConnState::Online : ConnState::Offline;
    if (connected) appMode = AppMode::Online;
    notify(message);
  }

  void startUpdateWatch(const std::string& startVersion = "") {
    updateWatch = true;
    updateStartVersion = startVersion.empty() ? deviceVersion : startVersion;
    if (updateStartVersion.empty() && !status.empty()) {
      JsonDocument doc;
      if (!deserializeJson(doc, status) && doc.is<JsonObject>()) {
        const char* version = doc["version"] | "";
        if (version[0]) updateStartVersion = version;
      }
    }
    updateStartedAt = SDL_GetTicks();
    updatePhase = "starting";
    updateMessage = "Starting update";
    updateError.clear();
    updateProgress = -1;
    connState = ConnState::Updating;
    connected = false;
  }

  void applyStatus(const std::string& body) {
    status = json_text(body);
    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc.is<JsonObject>()) return;
    const char* version = doc["version"] | "";
    if (version[0]) deviceVersion = version;
    JsonObject update = doc["update"].as<JsonObject>();
    if (!update.isNull()) {
      const char* phase = update["phase"] | "";
      if (phase[0]) updatePhase = phase;
      updateProgress = update["progress"] | -1;
      const char* message = update["message"] | "";
      if (message[0]) updateMessage = message;
      const char* error = update["error"] | "";
      updateError = error;
      const char* target = update["target"] | "";
      if (target[0]) updateTarget = target;
    }
    if (updateWatch) {
      String target = updateTarget;
      if (target.startsWith("v")) target.remove(0, 1);
      if (!target.isEmpty() && deviceVersion == target.c_str()) {
        finishUpdate(true, "Updated to " + deviceVersion);
      } else if (!updateStartVersion.empty() && !deviceVersion.empty() &&
                 deviceVersion != updateStartVersion) {
        finishUpdate(true, "Updated to " + deviceVersion);
      } else if (!updateError.empty() || updatePhase == "error") {
        finishUpdate(false, updateError.empty() ? updateMessage : updateError);
      }
    }
  }

  void markOnline(uint32_t latencyMs) {
    const bool wasConnection = appMode == AppMode::Connection;
    lastSuccessAt = SDL_GetTicks();
    lastLatencyMs = latencyMs;
    consecutiveFailures = 0;
    connError.clear();
    connected = true;
    if (!updateWatch) connState = ConnState::Online;
    appMode = AppMode::Online;
    if (wasConnection) tab = 0;
    if (profileDirty) saveConnectionProfile();
    if (pendingInitialLoad) {
      pendingInitialLoad = false;
      loadConfig();
      loadUsage();
      if (!viewDirty) loadViewFromDevice();
    }
  }

  void markFailure(const std::string& error, int httpStatus = 0) {
    connError = error;
    connected = false;
    lastAttemptAt = SDL_GetTicks();
    if (uploadRunning) {
      connState = ConnState::Updating;
      return;
    }
    if (updateWatch) {
      connState = ConnState::Updating;
      updatePhase = "rebooting";
      updateMessage = "Waiting for the device to come back";
      if (SDL_GetTicks() - updateStartedAt > 180000)
        finishUpdate(false, "Device did not return after 3 minutes");
      return;
    }
    consecutiveFailures++;
    if (httpStatus == 401 || httpStatus == 403) connState = ConnState::AuthRequired;
    else if (httpStatus >= 500) connState = ConnState::Degraded;
    else if (consecutiveFailures >= 2) connState = ConnState::Offline;
    else connState = ConnState::Connecting;
  }

  void startHeartbeat() {
    if (!connectionActive || heartbeatRunning || !deviceUrl[0]) return;
    heartbeatRunning = true;
    const uint32_t seq = ++heartbeatSeq;
    lastAttemptAt = SDL_GetTicks();
    heartbeatFuture = std::async(std::launch::async, [this, seq] {
      const uint32_t started = SDL_GetTicks();
      DeviceResponse r;
      HeartbeatResult result;
      result.seq = seq;
      result.ok = manager.request("GET", "/api/status", "", "", r);
      result.status = r.status;
      result.body = std::move(r.body);
      result.latencyMs = SDL_GetTicks() - started;
      return result;
    });
  }

  void pollHeartbeat() {
    if (uploadRunning || !connectionActive) return;
    const uint32_t now = SDL_GetTicks();
    if (!heartbeatRunning) {
      const uint32_t interval = updateWatch ? 1000 : (online() ? 5000 : 3000);
      if (deviceUrl[0] && now - lastAttemptAt >= interval) startHeartbeat();
      return;
    }
    if (heartbeatFuture.wait_for(std::chrono::milliseconds(0)) !=
        std::future_status::ready) return;
    HeartbeatResult result = heartbeatFuture.get();
    heartbeatRunning = false;
    if (result.seq != heartbeatSeq) return;
    lastAttemptAt = SDL_GetTicks();
    if (!result.ok) {
      markFailure("Device is unreachable");
    } else if (result.status == 200) {
      markOnline(result.latencyMs);
      applyStatus(result.body);
    } else if (result.status == 401 || result.status == 403) {
      markFailure("Authentication required", result.status);
    } else if (result.status >= 500) {
      markFailure("Device error HTTP " + std::to_string(result.status), result.status);
    } else {
      markOnline(result.latencyMs);
      connError = "HTTP " + std::to_string(result.status);
    }
  }

  void disconnect() {
    manager.setTarget("");
    ++heartbeatSeq;
    appMode = AppMode::Connection;
    connectionActive = false;
    disconnectPrompt = false;
    connState = ConnState::Disconnected;
    connected = false;
    connError.clear();
    lastSuccessAt = 0;
    lastLatencyMs = 0;
    consecutiveFailures = 0;
    pendingInitialLoad = false;
    updateWatch = false;
    updateAvailable = false;
    latestVersion.clear();
    uploadStartVersion.clear();
    updateStartVersion.clear();
    updateTarget.clear();
    updatePhase.clear();
    updateMessage.clear();
    updateError.clear();
    updateProgress = -1;
    status.clear();
    deviceVersion.clear();
    cfgLoaded = false;
    profileDirty = false;
    notify("Disconnected");
  }

  void connect(bool keepShell = false) {
    std::string url = deviceUrl;
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
      url = "http://" + url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    snprintf(deviceUrl, sizeof(deviceUrl), "%s", url.c_str());
    manager.setTarget(url);
    manager.setAuth(authUser, authPass);
    ++heartbeatSeq;
    if (!keepShell) appMode = AppMode::Connection;
    connectionActive = true;
    profileDirty = true;
    connState = ConnState::Connecting;
    connected = false;
    connError.clear();
    consecutiveFailures = 0;
    lastSuccessAt = 0;
    lastAttemptAt = 0;
    lastLatencyMs = 0;
    pendingInitialLoad = true;
    updateWatch = false;
    updateAvailable = false;
    latestVersion.clear();
    uploadStartVersion.clear();
    updateStartVersion.clear();
    updateTarget.clear();
    updatePhase.clear();
    updateMessage.clear();
    updateError.clear();
    updateProgress = -1;
    status.clear();
    deviceVersion.clear();
    cfgLoaded = false;
    notify("Connecting to " + url);
  }

  void checkGithubUpdates() {
    DeviceResponse r;
    if (!api("GET", "/api/checkupdate", "", "", r)) return;
    JsonDocument doc;
    if (deserializeJson(doc, r.body) || !doc.is<JsonObject>()) {
      updateError = "Invalid update-check response";
      return;
    }
    const char* latest = doc["latest"] | "";
    const char* current = doc["current"] | "";
    updateAvailable = doc["newer"] | false;
    updateError = (const char*)(doc["error"] | "");
    if (latest[0]) latestVersion = latest;
    if (latest[0]) updateTarget = latest;
    if (current[0]) deviceVersion = current;
    if (!updateAvailable && updateError.empty())
      updateMessage = "Up to date (" + deviceVersion + ")";
    else if (updateAvailable)
      updateMessage = "Version " + latestVersion + " is available";
  }

  void startGithubUpdate() {
    DeviceResponse r;
    if (!api("POST", "/api/selfupdate", "", "application/json", r)) return;
    if (!latestVersion.empty()) updateTarget = latestVersion;
    startUpdateWatch();
    notify("Update started; waiting for the device");
  }

  bool api(const std::string& method, const std::string& path,
           const std::string& body, const std::string& ct, DeviceResponse& r) {
    const uint32_t started = SDL_GetTicks();
    if (!manager.request(method, path, body, ct, r)) {
      lastError = "network error";
      markFailure(lastError);
      if (!updateWatch) notify(lastError);
      return false;
    }
    const uint32_t latency = SDL_GetTicks() - started;
    lastAttemptAt = SDL_GetTicks();
    if (r.status >= 200 && r.status < 300) {
      markOnline(latency);
      if (path == "/api/status") applyStatus(r.body);
      return true;
    }
    lastError = "HTTP " + std::to_string(r.status) +
                (r.body.empty() ? "" : ": " + r.body.substr(0, 180));
    if (r.status == 401 || r.status == 403 || r.status >= 500) {
      markFailure(lastError, r.status);
    } else {
      markOnline(latency);
      connError = lastError;
    }
    if (!updateWatch) notify(lastError);
    return false;
  }

  void loadStatus() {
    DeviceResponse r;
    if (api("GET", "/api/status", "", "", r)) status = json_text(r.body);
  }

  void readDiagnostics(bool previous) {
    DeviceResponse r;
    const char* path = previous ? "/api/logs/previous" : "/api/logs";
    if (!api("GET", path, "", "", r)) return;
    diagText = r.body.empty() ? "(empty log)" : r.body;
    diagTitle = previous ? "Previous boot log" : "Current boot log";
    diagOpen = true;
  }

  void loadUsage() {
    DeviceResponse r;
    if (api("GET", "/api/usage", "", "", r)) {
      usage = json_text(r.body);
      if (usagePreviewActive) {
        liveUsage = usage;
      } else if (!usage.empty()) {
        std::string raw = r.body;
        if (usageApply(String(raw.c_str(), raw.size()))) previewDirty = true;
      }
    }
  }

  std::string appUpdateStatePath() const {
#if defined(_WIN32)
    const char* base = std::getenv("APPDATA");
    return std::string(base ? base : ".") + "/smalltv/emulator-app.json";
#else
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/smalltv/emulator-app.json";
#endif
  }

  void loadAppUpdateState() {
    std::ifstream file(appUpdateStatePath());
    if (!file) return;
    std::string line;
    while (std::getline(file, line)) {
      if (line.rfind("skipped=", 0) == 0) appUpdateSkipped = line.substr(8);
    }
  }

  std::string connectionStatePath() const {
#if defined(_WIN32)
    const char* base = std::getenv("APPDATA");
    return std::string(base ? base : ".") + "/smalltv/connections.json";
#else
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/smalltv/connections.json";
#endif
  }

  void loadConnectionProfile() {
    if (connectionsLoaded) return;
    connectionsLoaded = true;
    std::string text;
    if (read_file(connectionStatePath().c_str(), text) && !text.empty()) {
      JsonDocument doc;
      if (!deserializeJson(doc, text) && doc.is<JsonObject>()) {
        savedUrl = (const char*)(doc["url"] | "");
        savedUser = (const char*)(doc["user"] | "");
        rememberPassword = doc["rememberPassword"] | true;
        autoConnect = doc["autoConnect"] | true;
      }
    }
    if (!savedUrl.empty()) {
      snprintf(deviceUrl, sizeof(deviceUrl), "%s", savedUrl.c_str());
      snprintf(authUser, sizeof(authUser), "%s", savedUser.c_str());
      if (rememberPassword) {
        std::string secret;
        if (device::secureLoad(secret, deviceSecretError) && !secret.empty()) {
          JsonDocument secretDoc;
          if (!deserializeJson(secretDoc, secret) && secretDoc.is<JsonObject>()) {
            const char* url = secretDoc["url"] | "";
            const char* password = secretDoc["password"] | "";
            if (savedUrl == url && password[0])
              snprintf(authPass, sizeof(authPass), "%s", password);
          }
        }
      }
    }
  }

  void saveConnectionProfile() {
    if (!deviceUrl[0]) return;
    savedUrl = deviceUrl;
    savedUser = authUser;
    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::path(connectionStatePath()).parent_path(), error);
    JsonDocument doc;
    doc["url"] = savedUrl;
    doc["user"] = savedUser;
    doc["rememberPassword"] = rememberPassword;
    doc["autoConnect"] = autoConnect;
    std::string text;
    serializeJson(doc, text);
    write_file(connectionStatePath(), text);
    if (rememberPassword && authPass[0]) {
      JsonDocument secret;
      secret["url"] = savedUrl;
      secret["password"] = authPass;
      std::string secretText;
      serializeJson(secret, secretText);
      std::string secretError;
      if (!device::secureSave(secretText, secretError))
        deviceSecretError = secretError;
      else
        deviceSecretError.clear();
    } else {
      std::string ignored;
      device::secureClear(ignored);
      deviceSecretError.clear();
    }
    profileDirty = false;
  }

  void forgetConnectionProfile() {
    savedUrl.clear();
    savedUser.clear();
    profileDirty = false;
    std::error_code error;
    std::filesystem::remove(connectionStatePath(), error);
    std::string ignored;
    device::secureClear(ignored);
    deviceSecretError.clear();
    snprintf(deviceUrl, sizeof(deviceUrl), "http://192.168.4.1");
    authUser[0] = 0;
    authPass[0] = 0;
  }

  bool shouldAutoConnect() const {
    return autoConnect && !savedUrl.empty();
  }

  void requestConnectionScreen() {
    if (viewDirty) {
      disconnectPrompt = true;
      return;
    }
    disconnect();
  }

  void saveSkippedAppVersion(const std::string& tag) {
    appUpdateSkipped = tag;
    const std::string path = appUpdateStatePath();
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    std::ofstream file(path, std::ios::trunc);
    if (file) file << "skipped=" << tag << "\n";
  }

  void checkAppUpdate(bool manual) {
    if (appUpdateChecking) return;
    appUpdateChecking = true;
    appUpdateManualCheck = manual;
    appUpdateError.clear();
    appUpdateFuture = std::async(std::launch::async, [] {
      appupdate::Release release;
      std::string error;
      if (!appupdate::fetchLatest(release, error)) {
        release.tag = "!" + error;
        return release;
      }
      return release;
    });
  }

  void startAppUpdateDownload() {
    if (appUpdateDownloading) return;
    const char* wanted = appupdate::platformAssetName();
    const appupdate::Asset* asset = nullptr;
    for (const auto& item : appRelease.assets)
      if (item.name == wanted) asset = &item;
    if (!asset) {
      notify(std::string("This release has no ") + wanted);
      return;
    }
    std::filesystem::path root = std::filesystem::absolute(executablePath).parent_path();
    if (!std::filesystem::exists(root / "VERSION.txt")) {
      notify("App self-update is only available from a packaged install");
      return;
    }
    const std::string archive =
        (std::filesystem::temp_directory_path() / asset->name).string();
    appUpdateRoot = root.string();
    appUpdateArchive = archive;
    appUpdateDownloading = true;
    appUpdateError.clear();
    appUpdateWorkerWasActive = workerServiceRunning();
    const appupdate::Asset copy = *asset;
    appUpdateDownloadFuture = std::async(std::launch::async, [copy, archive] {
      std::string error;
      return appupdate::downloadAsset(copy, archive, error);
    });
  }

  static std::string psQuote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
      if (c == '\'') out += "''";
      else out += c;
    }
    out += "'";
    return out;
  }

  void launchAppUpdater(bool workerActive) {
    const std::string root = appUpdateRoot;
#if defined(_WIN32)
    const std::filesystem::path scriptPath = std::filesystem::temp_directory_path() /
                                             "smalltv-update-app.ps1";
    const std::string archiveRoot = "smalltv-emulator-windows-x86_64";
    std::string expected = appRelease.tag;
    if (!expected.empty() && expected[0] == 'v') expected.erase(0, 1);
    std::ofstream file(scriptPath, std::ios::trunc);
    if (!file) { notify("Cannot create the app updater"); return; }
    file << "\xEF\xBB\xBF";
    file << "$ErrorActionPreference='Stop'\n"
            "$log=Join-Path $env:TEMP 'smalltv-update.log'\n"
            "function Log([string]$m){ Add-Content -LiteralPath $log -Value ((Get-Date).ToString('o')+' '+$m) }\n"
            "Set-Location -LiteralPath $env:TEMP\n"
            "$archive=" << psQuote(appUpdateArchive) << "\n"
            "$root=" << psQuote(root) << "\n"
            "$back=" << psQuote(root + ".smalltv-old") << "\n"
            "$expected=" << psQuote(expected) << "\n"
            "$oldMoved=$false\n"
            "$newMoved=$false\n"
            "try {\n"
            "  Log 'starting app update'\n"
            "  Wait-Process -Id " << _getpid() << " -ErrorAction SilentlyContinue\n"
            "  $tmp=Join-Path $env:TEMP 'smalltv-emulator-update'\n"
            "  Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue\n"
            "  Expand-Archive -LiteralPath $archive -DestinationPath $tmp -Force\n"
            "  $new=Join-Path $tmp " << psQuote(archiveRoot) << "\n"
            "  if (-not (Test-Path -LiteralPath (Join-Path $new 'VERSION.txt'))) { throw 'VERSION.txt missing' }\n"
            "  $version=(Get-Content -LiteralPath (Join-Path $new 'VERSION.txt') -Raw).Trim()\n"
            "  if ($version -ne $expected) { throw ('unexpected version '+$version) }\n"
            "  Remove-Item -LiteralPath $back -Recurse -Force -ErrorAction SilentlyContinue\n"
            "  Move-Item -LiteralPath $root -Destination $back\n"
            "  $oldMoved=$true\n"
            "  try { Move-Item -LiteralPath $new -Destination $root; $newMoved=$true }\n"
            "  catch { Move-Item -LiteralPath $back -Destination $root; $oldMoved=$false; throw }\n"
            "  $exe=Join-Path $root 'smalltv-emulator.exe'\n"
            "  if (-not (Test-Path -LiteralPath $exe)) { throw 'updated executable missing' }\n"
            "  if (" << (workerActive ? "$true" : "$false") << ") { schtasks /Run /TN SmallTVUsageWorker | Out-Null }\n"
            "  Start-Process -FilePath $exe -WorkingDirectory $root\n"
            "  Log 'update complete'\n"
            "  Remove-Item -LiteralPath $back -Recurse -Force -ErrorAction SilentlyContinue\n"
            "} catch {\n"
            "  Log ('update failed: '+($_ | Out-String))\n"
            "  if ($newMoved -and (Test-Path -LiteralPath $root)) {\n"
            "    $failed=$root+'.smalltv-newfailed'\n"
            "    Remove-Item -LiteralPath $failed -Recurse -Force -ErrorAction SilentlyContinue\n"
            "    Move-Item -LiteralPath $root -Destination $failed -ErrorAction SilentlyContinue\n"
            "  }\n"
            "  if ($oldMoved -and (Test-Path -LiteralPath $back) -and -not (Test-Path -LiteralPath $root)) {\n"
            "    Move-Item -LiteralPath $back -Destination $root\n"
            "  }\n"
            "  $oldExe=Join-Path $root 'smalltv-emulator.exe'\n"
            "  if (Test-Path -LiteralPath $oldExe) { Start-Process -FilePath $oldExe -WorkingDirectory $root }\n"
            "  exit 1\n"
            "}\n";
    file.close();
    if (!file) { notify("Cannot write the app updater"); return; }

    const std::wstring script = scriptPath.wstring();
    std::wstring command = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\"";
    std::wstring cwd = std::filesystem::temp_directory_path().wstring();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> commandLine(command.begin(), command.end());
    commandLine.push_back(L'\0');
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, cwd.c_str(), &startup, &process)) {
      notify("Cannot start the Windows updater");
      return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    const std::string script = (std::filesystem::temp_directory_path() /
                                "smalltv-update-app.sh").string();
    const std::string archiveRoot = "smalltv-emulator-linux-x86_64";
    std::ofstream file(script, std::ios::trunc);
    if (!file) { notify("Cannot create the app updater"); return; }
    file << "#!/bin/sh\nset -eu\n"
            "while kill -0 " << getpid() << " 2>/dev/null; do sleep 0.2; done\n"
            "tmp=$(mktemp -d)\n"
            "tar -xzf \"" << appUpdateArchive << "\" -C \"$tmp\"\n"
            "new=\"$tmp/" << archiveRoot << "\"\n"
            "test -f \"$new/VERSION.txt\"\n"
            "back=\"" << root << ".smalltv-old\"\n"
            "rm -rf \"$back\"\n"
            "mv \"" << root << "\" \"$back\"\n"
            "if ! mv \"$new\" \"" << root << "\"; then mv \"$back\" \"" << root << "\"; exit 1; fi\n"
            "rm -rf \"$back\" \"$tmp\"\n"
            "if [ \"" << (workerActive ? "1" : "0") << "\" = 1 ]; then systemctl --user start smalltv-usage-worker.service || true; fi\n"
            "exec \"" << root << "/smalltv-emulator\"\n";
    file.close();
    std::system(("setsid sh \"" + script + "\" >/dev/null 2>&1 &").c_str());
#endif
    std::exit(0);
  }

  void pollAppUpdate() {
    if (appUpdateChecking && appUpdateFuture.valid() &&
        appUpdateFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
      appRelease = appUpdateFuture.get();
      appUpdateChecking = false;
      if (!appRelease.tag.empty() && appRelease.tag[0] == '!') {
        appUpdateError = appRelease.tag.substr(1);
        if (appUpdateManualCheck) notify("App update check failed: " + appUpdateError);
      } else if (appupdate::newer(appRelease.tag, FW_VERSION)) {
        if (appUpdateManualCheck || appRelease.tag != appUpdateSkipped) {
          appUpdatePrompt = true;
        } else if (appUpdateManualCheck) {
          notify("App is up to date (" FW_VERSION ")");
        }
      } else if (appUpdateManualCheck) {
        notify("App is up to date (" FW_VERSION ")");
      }
    }
    if (appUpdateDownloading && appUpdateDownloadFuture.valid() &&
        appUpdateDownloadFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
      const bool ok = appUpdateDownloadFuture.get();
      appUpdateDownloading = false;
      if (!ok) {
        notify("App update download failed");
        if (appUpdateWorkerWasActive) controlWorkerService(true);
        return;
      }
      if (appUpdateWorkerWasActive) {
        controlWorkerService(false);
        for (int i = 0; i < 100 && workerServiceRunning(); i++)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (workerServiceRunning()) {
          notify("Could not stop the usage worker for the app update");
          controlWorkerService(true);
          return;
        }
      }
      if (manualWorkerRunning()) {
        notify("A manually started usage worker is running; stop it before updating the app");
        if (appUpdateWorkerWasActive) controlWorkerService(true);
        return;
      }
      launchAppUpdater(appUpdateWorkerWasActive);
    }
  }

  void appUpdateModal() {
    if (appUpdatePrompt) {
      ImGui::OpenPopup("App update");
      appUpdatePrompt = false;
      appUpdateModalOpen = true;
    }
    if (!appUpdateModalOpen) return;
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("App update", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("SmallTV %s is available.", appRelease.tag.c_str());
      ImGui::TextDisabled("Installed: " FW_VERSION);
      ImGui::Separator();
      ImGui::TextWrapped("%s", appRelease.body.c_str());
      if (viewDirty) ImGui::TextWrapped("Unsaved dashboard changes will be lost.");
      ImGui::TextWrapped("The background usage worker will be stopped and restarted if installed.");
      if (ImGui::Button("Install update")) {
        appUpdateModalOpen = false;
        ImGui::CloseCurrentPopup();
        startAppUpdateDownload();
      }
      ImGui::SameLine();
      if (ImGui::Button("Later")) {
        appUpdateModalOpen = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Skip this version")) {
        saveSkippedAppVersion(appRelease.tag);
        appUpdateModalOpen = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    } else {
      appUpdateModalOpen = false;
    }
  }

  JsonObject usageProvider(const char* id) {
    JsonObject root = usageCreds.as<JsonObject>();
    if (root.isNull()) root = usageCreds.to<JsonObject>();
    JsonObject provider = root[id].as<JsonObject>();
    if (provider.isNull()) provider = root[id].to<JsonObject>();
    return provider;
  }

  bool loadUsageCredentials() {
    if (usageCredsLoaded) return true;
    std::string json;
    std::string error;
    if (!usage::secureLoad(json, error)) {
      usageCredsError = error;
      usageCredsLoaded = true;
      usageCreds.clear();
      deserializeJson(usageCreds, usage::credentialsTemplate());
      return false;
    }
    if (json.empty()) json = usage::credentialsTemplate();
    usageCreds.clear();
    if (deserializeJson(usageCreds, json) || !usageCreds.is<JsonObject>()) {
      usageCredsError = "Stored usage credentials are invalid";
      deserializeJson(usageCreds, usage::credentialsTemplate());
      usageCredsLoaded = true;
      return false;
    }
    usageCredsError.clear();
    usageCredsLoaded = true;
    return true;
  }

  bool saveUsageCredentials() {
    std::string json;
    serializeJson(usageCreds, json);
    std::string error;
    if (!usage::secureSave(json, error)) {
      usageCredsError = error;
      notify("Usage credentials: " + error);
      return false;
    }
    usageCredsError.clear();
    notify("Usage credentials saved to the OS store");
    return true;
  }

  void clearUsageCredentials() {
    std::string error;
    if (!usage::secureClear(error)) {
      usageCredsError = error;
      notify("Usage credentials: " + error);
      return;
    }
    usageCreds.clear();
    deserializeJson(usageCreds, usage::credentialsTemplate());
    usageResults.clear();
    usageError.clear();
    notify("Usage credentials cleared");
  }

  void startUsageFetch() {
    if (usageFetchRunning) return;
    std::string body;
    serializeJson(usageCreds, body);
    usageFetchRunning = true;
    usageError.clear();
    usageResults.clear();
    const std::string proxy = usageProxyUrl;
    notify("Fetching usage providers...");
    usageFuture = std::async(std::launch::async, [body, proxy] {
      return usage::fetchAll(body, proxy, 15000);
    });
  }

  void pushUsage() {
    if (usageResults.empty()) {
      notify("Fetch usage before pushing");
      return;
    }
    if (!deviceUrl[0]) {
      notify("Connect to a device first");
      return;
    }
    const std::string normalized = usage::normalizedJson(usageResults);
    DeviceResponse response;
    if (api("POST", "/api/usage", normalized, "application/json", response)) {
      usage = normalized;
      usageApply(String(normalized.c_str(), normalized.size()));
      previewDirty = true;
      notify("Usage pushed to device");
    }
  }

  std::string firmwareAssetName() {
    JsonDocument document;
    if (status.empty() || deserializeJson(document, status) || !document.is<JsonObject>())
      return {};
    const std::string variant = (const char*)(document["variant"] | "");
    if (variant == "esp8266") return "smalltv-mod-firmware.bin";
    if (variant == "c2") return "smalltv-mod-firmware-c2.bin";
    if (variant == "esp32") return "smalltv-mod-firmware-esp32.bin";
    if (variant == "esp32-pro") return "smalltv-mod-firmware-esp32-pro.bin";
    return {};
  }

  const appupdate::Asset* selectedFirmwareAsset() {
    if (firmwareReleaseIndex < 0 || firmwareReleaseIndex >= (int)firmwareReleases.size())
      return nullptr;
    const std::string wanted = firmwareAssetName();
    if (wanted.empty()) return nullptr;
    for (const auto& item : firmwareReleases[firmwareReleaseIndex].assets)
      if (item.name == wanted) return &item;
    return nullptr;
  }

  void refreshFirmwareReleases() {
    if (firmwareReleasesLoading) return;
    firmwareReleasesLoading = true;
    firmwareReleaseError.clear();
    firmwareReleasesFuture = std::async(std::launch::async, [] {
      std::vector<appupdate::Release> releases;
      std::string error;
      if (!appupdate::fetchReleases(releases, error)) {
        appupdate::Release marker;
        marker.tag = "!" + error;
        releases.push_back(marker);
      }
      return releases;
    });
  }

  void installSelectedFirmware() {
    const appupdate::Asset* asset = selectedFirmwareAsset();
    if (!asset) {
      notify("Select a release and connect a supported device first");
      return;
    }
    if (uploadRunning) return;
    const std::string path =
        (std::filesystem::temp_directory_path() / asset->name).string();
    uploadStartVersion = deviceVersion;
    uploadRunning = true;
    uploadError.clear();
    updateTarget = firmwareReleases[firmwareReleaseIndex].tag;
    updatePhase = "downloading";
    updateMessage = "Downloading release firmware";
    updateError.clear();
    updateProgress = -1;
    connState = ConnState::Updating;
    connected = false;
    notify("Downloading firmware " + firmwareReleases[firmwareReleaseIndex].tag);
    const appupdate::Asset copy = *asset;
    uploadFuture = std::async(std::launch::async, [this, copy, path] {
      std::string error;
      if (!appupdate::downloadAsset(copy, path, error)) {
        uploadError = error;
        return false;
      }
      const bool ok = manager.uploadFirmware(path, error);
      std::filesystem::remove(path);
      uploadError = error;
      return ok;
    });
  }

  std::string proxyStatePath() const {
#if defined(_WIN32)
    const char* base = std::getenv("APPDATA");
    return std::string(base ? base : ".") + "/smalltv/deepseek-usage.json";
#else
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/smalltv/deepseek-usage.json";
#endif
  }

#if defined(_WIN32)
  std::string workerTaskCommand(const char* action) const {
    return "schtasks /" + std::string(action) +
           " /TN SmallTVUsageWorker" + (strcmp(action, "Query") == 0 ? "" : " /F");
  }
#else
  std::string workerUnitPath() const {
    const char* base = std::getenv("XDG_CONFIG_HOME");
    return std::string(base ? base : "") +
           (base ? "/systemd/user/smalltv-usage-worker.service"
                 : "/.config/systemd/user/smalltv-usage-worker.service");
  }
#endif

  void installWorkerService() {
    if (executablePath.empty()) {
      notify("Cannot locate the application executable");
      return;
    }
    const std::string device = deviceUrl[0] ? deviceUrl : "http://smalltv.local";
#if defined(_WIN32)
    const std::string command =
        "schtasks /Create /SC ONLOGON /TN SmallTVUsageWorker /TR \"" +
        executablePath + " --usage-worker --usage-device " + device +
        "\" /F >NUL 2>&1";
    const int result = std::system(command.c_str());
#else
    const std::string unit = workerUnitPath();
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(unit).parent_path(), error);
    std::ofstream file(unit, std::ios::trunc);
    if (!file) {
      notify("Cannot write the usage worker service file");
      return;
    }
    file << "[Unit]\nDescription=SmallTV native usage collector\n"
            "After=graphical-session.target\n\n"
            "[Service]\nType=simple\nExecStart=\"" << executablePath
         << "\" --usage-worker --usage-device \"" << device << "\"\n"
            "Restart=on-failure\nRestartSec=10\n\n"
            "[Install]\nWantedBy=default.target\n";
    file.close();
    const int result = std::system(
        "systemctl --user daemon-reload && systemctl --user enable --now smalltv-usage-worker.service");
#endif
    workerServiceChecked = false;
    notify(result == 0 ? "Usage worker autostart installed" : "Could not install usage worker autostart");
  }

  void uninstallWorkerService() {
#if defined(_WIN32)
    const int result = std::system("schtasks /Delete /TN SmallTVUsageWorker /F >NUL 2>&1");
#else
    std::system("systemctl --user disable --now smalltv-usage-worker.service >/dev/null 2>&1 || true");
    std::error_code error;
    std::filesystem::remove(workerUnitPath(), error);
    const int result = std::system("systemctl --user daemon-reload");
#endif
    workerServiceChecked = false;
    notify(result == 0 ? "Usage worker autostart removed" : "Could not remove usage worker autostart");
  }

  void controlWorkerService(bool start) {
#if defined(_WIN32)
    const int result = std::system(start
        ? "schtasks /Run /TN SmallTVUsageWorker >NUL 2>&1"
        : "schtasks /End /TN SmallTVUsageWorker >NUL 2>&1");
#else
    const int result = std::system(start
        ? "systemctl --user start smalltv-usage-worker.service"
        : "systemctl --user stop smalltv-usage-worker.service");
#endif
    workerServiceChecked = false;
    notify(result == 0 ? (start ? "Usage worker started" : "Usage worker stopped")
                       : "Usage worker command failed");
  }

  bool manualWorkerRunning() {
#if defined(_WIN32)
    const std::string script = (std::filesystem::temp_directory_path() /
                                "smalltv-worker-check.ps1").string();
    std::ofstream file(script, std::ios::trunc);
    if (!file) return false;
    file << "try {\n"
            "  $p=Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'smalltv-emulator.exe' -and $_.CommandLine -like '*--usage-worker*' }\n"
            "  if ($p) { exit 1 }\n"
            "  exit 0\n"
            "} catch { exit 2 }\n";
    file.close();
    const int result = std::system(
        ("powershell -NoProfile -ExecutionPolicy Bypass -File \"" + script +
         "\" >NUL 2>&1").c_str());
    return result == 1;
#else
    return std::system("pgrep -f 'smalltv-emulator.*--usage-worker' >/dev/null 2>&1") == 0;
#endif
  }

  bool workerServiceRunning() {
#if defined(_WIN32)
    return std::system(
        "powershell -NoProfile -Command \"$t=Get-ScheduledTask -TaskName 'SmallTVUsageWorker' -ErrorAction SilentlyContinue; if ($t -and $t.State -eq 'Running') { exit 0 } else { exit 1 }\" >NUL 2>&1") == 0;
#else
    return std::system("systemctl --user is-active --quiet smalltv-usage-worker.service") == 0;
#endif
  }

  void startDeepSeekProxy() {
    if (deepSeekProxyRunning) return;
    std::string error;
    if (!deepSeekProxy.start("127.0.0.1", 8789, "https://api.deepseek.com",
                             proxyStatePath(), error)) {
      notify("DeepSeek proxy: " + error);
      return;
    }
    deepSeekProxyRunning = true;
    notify("DeepSeek proxy started on 127.0.0.1:8789");
  }

  void stopDeepSeekProxy() {
    deepSeekProxy.stop();
    deepSeekProxyRunning = false;
    notify("DeepSeek proxy stopped");
  }

  void usageScreen() {
    loadUsageCredentials();
    beginPane("##usagepage", ImVec2(0, 0), true);
    paneTitle("USAGE", "credentials stay in the OS secure store");
    ImGui::Spacing();
    if (!usageCredsError.empty()) ImGui::TextWrapped("Storage: %s", usageCredsError.c_str());
    if (ImGui::Button("Save credentials")) saveUsageCredentials();
    ImGui::SameLine();
    ImGui::BeginDisabled(usageFetchRunning);
    if (ImGui::Button("Fetch now")) startUsageFetch();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(usageResults.empty() || !online());
    if (ImGui::Button("Push to device")) pushUsage();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear credentials")) clearUsageCredentials();
    if (usageFetchRunning) {
      ImGui::Spacing();
      ImGui::TextDisabled("Fetching Claude, Codex and DeepSeek...");
      ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(240, 0), "working");
    } else if (!usageError.empty()) {
      ImGui::TextWrapped("Fetch error: %s", usageError.c_str());
    }

    ImGui::SeparatorText("Claude");
    JsonObject claude = usageProvider("claude");
    formStart("##usage-claude");
    input_text_json(claude, "accessToken", "Access token", true);
    input_text_json(claude, "refreshToken", "Refresh token", true);
    input_text_json(claude, "expiresAt", "Expires at (epoch ms)");
    input_text_json(claude, "clientId", "OAuth client ID");
    input_text_json(claude, "tokenUrl", "Token URL");
    input_text_json(claude, "userAgent", "User agent");
    formEnd();

    ImGui::SeparatorText("Codex / ChatGPT");
    JsonObject codex = usageProvider("codex");
    formStart("##usage-codex");
    input_text_json(codex, "accessToken", "Access token", true);
    input_text_json(codex, "refreshToken", "Refresh token", true);
    input_text_json(codex, "accountId", "Account ID", true);
    input_text_json(codex, "clientId", "OAuth client ID");
    input_text_json(codex, "tokenUrl", "Token URL");
    input_text_json(codex, "userAgent", "User agent");
    formEnd();

    ImGui::SeparatorText("DeepSeek");
    JsonObject deepseek = usageProvider("deepseek");
    formStart("##usage-deepseek");
    input_text_json(deepseek, "apiKey", "API key", true);
    formEnd();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##usage-proxy", "http://127.0.0.1:8789/usage",
                             usageProxyUrl, sizeof(usageProxyUrl));
    ImGui::TextDisabled("Token usage requires the DeepSeek client to use the local proxy.");
    ImGui::SameLine();
    if (deepSeekProxyRunning) {
      if (ImGui::Button("Stop proxy")) stopDeepSeekProxy();
    } else {
      if (ImGui::Button("Start proxy")) startDeepSeekProxy();
    }
    ImGui::TextDisabled("Proxy: %s", deepSeekProxyRunning ? "running on 127.0.0.1:8789" : "stopped");
    ImGui::SeparatorText("Background worker");
    if (!workerServiceChecked) {
      workerServiceActive = workerServiceRunning();
      workerServiceChecked = true;
    }
    ImGui::TextDisabled("Autostart: %s", workerServiceActive ? "running" : "not running");
    if (ImGui::Button("Install autostart")) installWorkerService();
    ImGui::SameLine();
    if (ImGui::Button("Uninstall autostart")) uninstallWorkerService();
    ImGui::SameLine();
    if (ImGui::Button("Start worker")) controlWorkerService(true);
    ImGui::SameLine();
    if (ImGui::Button("Stop worker")) controlWorkerService(false);
    ImGui::SameLine();
    if (ImGui::Button("Refresh status")) workerServiceChecked = false;

    ImGui::SeparatorText("Last fetch");
    if (usageResults.empty()) {
      ImGui::TextDisabled("No provider results yet.");
    } else {
      for (const usage::Result& result : usageResults) {
        ImGui::Text("%s", result.name.c_str());
        ImGui::SameLine();
        if (result.ok) ImGui::TextDisabled("%s", result.status.c_str());
        else ImGui::TextDisabled("error");
        if (!result.error.empty()) ImGui::TextWrapped("%s", result.error.c_str());
        for (const usage::Metric& metric : result.metrics) {
          ImGui::TextDisabled("%s", metric.label.c_str());
          ImGui::SameLine(ImGui::GetContentRegionAvail().x - 64);
          if (!metric.value.empty()) ImGui::Text("%s", metric.value.c_str());
          else ImGui::Text("%.0f%%", metric.pct);
          if (metric.pct > 0) ImGui::ProgressBar((float)metric.pct / 100.0f, ImVec2(-1, 5), "");
        }
        ImGui::Spacing();
      }
    }
    endPane();
  }

  void loadConfig() {
    DeviceResponse r;
    if (!api("GET", "/api/config", "", "", r)) return;
    cfg.clear();
    if (deserializeJson(cfg, r.body)) {
      notify("Invalid config JSON from device");
      return;
    }
    cfgLoaded = true;
    JsonObject display = cfg["display"].as<JsonObject>();
    const char* order = display["colorOrder"] | "auto";
    gfxSetPreviewDisplay(!strcmp(order, "rgb") ? 1 : !strcmp(order, "bgr") ? 2 : 0,
                         display["invert"] | false,
                         display["rGain"] | 100, display["gGain"] | 100,
                         display["bGain"] | 100);
    previewDirty = true;
    std::string pretty;
    serializeJsonPretty(cfg, pretty);
    snprintf(cfgRaw, sizeof(cfgRaw), "%s", pretty.c_str());
    notify("Configuration loaded");
  }

  bool saveConfig() {
    std::string body;
    serializeJson(cfg, body);
    DeviceResponse r;
    if (!api("POST", "/api/config", body, "application/json", r)) return false;
    JsonObject display = cfg["display"].as<JsonObject>();
    const char* order = display["colorOrder"] | "auto";
    gfxSetPreviewDisplay(!strcmp(order, "rgb") ? 1 : !strcmp(order, "bgr") ? 2 : 0,
                         display["invert"] | false,
                         display["rGain"] | 100, display["gGain"] | 100,
                         display["bGain"] | 100);
    previewDirty = true;
    std::string pretty;
    serializeJsonPretty(cfg, pretty);
    snprintf(cfgRaw, sizeof(cfgRaw), "%s", pretty.c_str());
    notify("Settings saved" + std::string(r.body.find("reboot") != std::string::npos ? " (device rebooting)" : ""));
    return true;
  }

  std::string serializeView() {
    std::string s;
    serializeJson(view, s);
    return s;
  }

  void pushUndo() {
    undoStack.push_back(serializeView());
    if (undoStack.size() > 100) undoStack.erase(undoStack.begin());
    redoStack.clear();
  }

  void undo() {
    if (undoStack.empty()) return;
    redoStack.push_back(serializeView());
    std::string s = undoStack.back();
    undoStack.pop_back();
    JsonDocument d;
    if (!deserializeJson(d, s)) return;
    view = d;
    normalizePixelUnits();
    ensureComponentIds();
    selected = -1;
    selection.clear();
    syncSelection();
    previewDirty = true;
    syncJson();
  }

  void redo() {
    if (redoStack.empty()) return;
    undoStack.push_back(serializeView());
    std::string s = redoStack.back();
    redoStack.pop_back();
    JsonDocument d;
    if (!deserializeJson(d, s)) return;
    view = d;
    ensureComponentIds();
    selected = -1;
    selection.clear();
    syncSelection();
    previewDirty = true;
    syncJson();
  }

  void syncJson() {
    std::string s;
    serializeJsonPretty(view, s);
    snprintf(viewJsonBuf, sizeof(viewJsonBuf), "%s", s.c_str());
  }

  void markViewChanged() {
    viewDirty = true;
    previewDirty = true;
    lastAutosave = 0;
    syncJson();
  }

  static int clampScale(int value, int minValue, int maxValue) {
    return std::max(minValue, std::min(maxValue, value));
  }

  void normalizePixelUnits() {
    const int count = (int)view["draw"].as<JsonArray>().size();
    for (int i = 0; i < count; i++) {
      JsonArray draw = view["draw"].as<JsonArray>();
      JsonObject l = draw[i];
      const char* t = l["t"] | "";
      if (!strcmp(t, "text")) {
        if (l["sizePx"].isNull()) {
          const int scale = clampScale(l["s"] | 1, 1, 6);
          l["sizePx"] = scale * 8;
        }
      } else if (!strcmp(t, "icon")) {
        if (l["sizePx"].isNull()) {
          const int scale = clampScale(l["s"] | 1, 1, 8);
          l["sizePx"] = scale * 24;
        }
      } else if (!strcmp(t, "bitmap")) {
        if (l["displayW"].isNull() || l["displayH"].isNull()) {
          const int scale = clampScale(l["s"] | 1, 1, 4);
          l["displayW"] = std::max(1, (int)(l["w"] | 1) * scale);
          l["displayH"] = std::max(1, (int)(l["h"] | 1) * scale);
        }
      } else if (!strcmp(t, "image")) {
        if ((l["displayW"].isNull() || l["displayH"].isNull()) &&
            (int)(l["w"] | 0) > 0 && (int)(l["h"] | 0) > 0) {
          const int scale = clampScale(l["s"] | 1, 1, 8);
          l["displayW"] = std::max(1, (int)(l["w"] | 1) * scale);
          l["displayH"] = std::max(1, (int)(l["h"] | 1) * scale);
        }
      } else if (!strcmp(t, "usage")) {
        JsonObject style = l["style"].as<JsonObject>();
        if (!style.isNull()) {
          auto ensure = [&](const char* pxKey, const char* sizeKey, int fallback) {
            if (!style[pxKey].isNull()) return;
            style[pxKey] = clampScale(style[sizeKey] | fallback, 0, 6) * 8;
          };
          ensure("namePx", "nameSize", 2);
          ensure("labelPx", "labelSize", 0);
          ensure("valuePx", "valueSize", 0);
          ensure("resetPx", "resetSize", 1);
        }
      }
    }
  }

  void syncDisplayScale(JsonObject l) {
    const int w = l["w"] | 0, h = l["h"] | 0;
    const int dw = l["displayW"] | 0, dh = l["displayH"] | 0;
    if (w <= 0 || h <= 0 || dw <= 0 || dh <= 0) return;
    const int sx = (dw + w / 2) / w;
    const int sy = (dh + h / 2) / h;
    const int maxScale = !strcmp(l["t"] | "", "bitmap") ? 4 : 8;
    l["s"] = clampScale((sx + sy + 1) / 2, 1, maxScale);
  }

  void clearSelection() {
    selected = -1;
    selection.clear();
    usageSelectedKind = 0;
    usageDragKind = 0;
    snapGuideX = snapGuideY = -1.0f;
  }

  std::string selectionSignature() {
    std::string sig;
    for (int i : selection) {
      sig += std::to_string(i);
      sig += ',';
    }
    return sig;
  }

  bool loadViewText(const std::string& text, std::string& err) {
    JsonDocument d;
    if (deserializeJson(d, text)) { err = "invalid JSON"; return false; }
    if (!d.is<JsonObject>()) { err = "expected an object"; return false; }
    ViewScreen test;
    String parseError;
    if (!viewScreenParse(text.c_str(), text.size(), test, parseError)) {
      err = parseError.c_str();
      return false;
    }
    view = d;
    ensureComponentIds();
    selected = -1;
    selection.clear();
    undoStack.clear();
    redoStack.clear();
    viewDirty = false;
    previewAnimStart = SDL_GetTicks();
    syncJson();
    previewDirty = true;
    return true;
  }

  bool loadViewFile(const std::string& path) {
    std::string text;
    if (!read_file(path.c_str(), text)) { notify("Cannot read " + path); return false; }
    std::string err;
    if (!loadViewText(text, err)) { notify("Invalid view: " + err); return false; }
    viewLoaded = true;
    notify("Loaded " + path);
    return true;
  }

  void loadViewFromDevice() {
    DeviceResponse r;
    if (!api("GET", "/api/dashboard", "", "", r)) return;
    std::string err;
    if (!loadViewText(r.body, err)) { notify("Device view: " + err); return; }
    viewLoaded = true;
    downloadViewAssets();
    notify("Loaded dashboard from device");
  }

  void saveViewToDevice() {
    if (!uploadViewAssets()) return;
    std::string body = serializeView();
    DeviceResponse r;
    if (!api("POST", "/api/dashboard", body, "application/json", r)) return;
    notify("Dashboard saved to device");
  }

  void sendOverlay(bool clear) {
    DeviceResponse r;
    if (clear) {
      if (api("POST", "/api/view/clear", "", "application/json", r)) notify("Overlay cleared");
    } else {
      if (!uploadViewAssets()) return;
      if (api("POST", "/api/view", serializeView(), "application/json", r)) notify("Overlay sent");
    }
  }

  std::string imageCacheDir() const {
#if defined(_WIN32)
    const char* base = std::getenv("APPDATA");
    return std::string(base ? base : ".") + "/smalltv/images";
#else
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/smalltv/images";
#endif
  }

  uint32_t imageBudget() const {
    uint32_t budget = 700000;
    JsonDocument d;
    if (!status.empty() && !deserializeJson(d, status) && d.is<JsonObject>()) {
      const uint64_t freeBytes = d["fsFree"] | 0;
      if (freeBytes > 65536)
        budget = (uint32_t)std::min<uint64_t>(3000000, freeBytes - 65536);
    }
    return budget;
  }

  void collectImageNames(std::vector<std::string>& names) {
    auto add = [&](const char* name) {
      if (!name || !imageAssetNameValid(name)) return;
      if (std::find(names.begin(), names.end(), name) == names.end()) names.emplace_back(name);
    };
    add(view["bgImage"] | "");
    JsonArray draw = view["draw"].as<JsonArray>();
    for (JsonObject layer : draw) {
      if (!strcmp(layer["t"] | "", "image")) add(layer["src"] | "");
    }
  }

  bool importAsset(const std::string& source, imagecodec::ImportedAsset& asset) {
    std::string error;
    std::error_code ec;
    std::filesystem::create_directories(imageCacheDir(), ec);
    if (!imagecodec::importImage(source, imageCacheDir(), imageBudget(), asset, error)) {
      notify("Image import failed: " + error);
      return false;
    }
    return true;
  }

  void addImportedLayer(const std::string& source) {
    imagecodec::ImportedAsset asset;
    if (!importAsset(source, asset)) return;
    std::vector<std::string> names;
    collectImageNames(names);
    if (std::find(names.begin(), names.end(), asset.name) == names.end() &&
        names.size() >= VIEW_MAX_IMAGES) {
      notify("A dashboard can reference at most four image assets");
      return;
    }
    pushUndo();
    JsonArray draw = view["draw"].as<JsonArray>();
    if (draw.isNull()) draw = view["draw"].to<JsonArray>();
    JsonObject layer = draw.add<JsonObject>();
    layer["t"] = "image";
    layer["src"] = asset.name;
    layer["x"] = 0;
    layer["y"] = 0;
    layer["w"] = asset.info.width;
    layer["h"] = asset.info.height;
    layer["s"] = 1;
    layer["displayW"] = asset.info.width;
    layer["displayH"] = asset.info.height;
    layer["a"] = "l";
    layer["z"] = maxZ() + 1;
    ensureComponentIds();
    draw = view["draw"].as<JsonArray>();
    selectOnly((int)draw.size() - 1);
    viewDirty = true;
    previewDirty = true;
    previewAnimStart = SDL_GetTicks();
    syncJson();
    notify("Imported " + asset.name);
  }

  void importBackground(const std::string& source) {
    imagecodec::ImportedAsset asset;
    if (!importAsset(source, asset)) return;
    std::vector<std::string> names;
    collectImageNames(names);
    if (std::find(names.begin(), names.end(), asset.name) == names.end() &&
        names.size() >= VIEW_MAX_IMAGES) {
      notify("A dashboard can reference at most four image assets");
      return;
    }
    pushUndo();
    view["bgImage"] = asset.name;
    viewDirty = true;
    previewDirty = true;
    previewAnimStart = SDL_GetTicks();
    syncJson();
    notify("Background image set to " + asset.name);
  }

  bool hasImageRefs() {
    std::vector<std::string> names;
    collectImageNames(names);
    return !names.empty();
  }

  bool uploadViewAssets() {
    std::vector<std::string> names;
    collectImageNames(names);
    std::error_code ec;
    std::filesystem::create_directories(imageCacheDir(), ec);
    for (const auto& name : names) {
      const std::string path = (std::filesystem::path(imageCacheDir()) /
                                (name + ".simg")).string();
      if (!std::filesystem::exists(path)) {
        notify("Missing local image asset: " + name);
        return false;
      }
      std::string error;
      if (!manager.uploadImage(path, name, error)) {
        notify("Image upload failed: " + error);
        return false;
      }
    }
    return true;
  }

  void downloadViewAssets() {
    std::vector<std::string> names;
    collectImageNames(names);
    std::error_code ec;
    std::filesystem::create_directories(imageCacheDir(), ec);
    for (const auto& name : names) {
      const std::string path = (std::filesystem::path(imageCacheDir()) /
                                (name + ".simg")).string();
      if (std::filesystem::exists(path)) continue;
      std::string error;
      if (!manager.downloadImage(name, path, error))
        notify("Could not cache image " + name + ": " + error);
    }
  }

  void setUsagePreviewMode(int mode) {
    if (mode < 0) mode = 0;
    if (mode > 3) mode = 3;
    if (mode == 0) {
      usagePreviewMode = 0;
      usagePreviewActive = false;
      if (!liveUsage.empty()) usageApply(String(liveUsage.c_str(), liveUsage.size()));
      previewDirty = true;
      return;
    }
    if (!usagePreviewActive) liveUsage = usage;
    usagePreviewActive = true;
    usagePreviewMode = mode;
    const char* mock = nullptr;
    if (mode == 1) {
      mock = "{\"providers\":["
             "{\"id\":\"claude\",\"name\":\"Claude\",\"ok\":true,\"status\":\"waiting\",\"metrics\":[]},"
             "{\"id\":\"chatgpt\",\"name\":\"ChatGPT\",\"ok\":true,\"status\":\"waiting\",\"metrics\":[]},"
             "{\"id\":\"deepseek\",\"name\":\"DeepSeek\",\"ok\":true,\"status\":\"waiting\",\"metrics\":[]}]}";
    } else if (mode == 2) {
      mock = "{\"providers\":["
             "{\"id\":\"claude\",\"name\":\"Claude\",\"ok\":false,\"status\":\"error\",\"metrics\":["
             "{\"label\":\"5h\",\"pct\":71,\"resetMin\":142},{\"label\":\"7d\",\"pct\":96,\"resetMin\":9876}]},"
             "{\"id\":\"chatgpt\",\"name\":\"ChatGPT\",\"ok\":false,\"status\":\"error\",\"metrics\":["
             "{\"label\":\"5h\",\"pct\":38,\"resetMin\":88}]},"
             "{\"id\":\"deepseek\",\"name\":\"DeepSeek\",\"ok\":false,\"status\":\"error\",\"metrics\":["
             "{\"label\":\"balance\",\"pct\":0,\"resetMin\":0,\"value\":\"110 CNY\"}]}]}";
    } else {
      mock = "{\"providers\":["
             "{\"id\":\"claude\",\"name\":\"Claude\",\"ok\":true,\"status\":\"normal\",\"metrics\":["
             "{\"label\":\"Very long five-hour metric label\",\"pct\":28,\"resetMin\":142,\"value\":\"1,234,567,890 tokens\"},"
             "{\"label\":\"Weekly usage window\",\"pct\":96,\"resetMin\":9876}]},"
             "{\"id\":\"chatgpt\",\"name\":\"ChatGPT\",\"ok\":true,\"status\":\"normal\",\"metrics\":["
             "{\"label\":\"Five-hour window\",\"pct\":38,\"resetMin\":88,\"value\":\"38 percent remaining\"}]},"
             "{\"id\":\"deepseek\",\"name\":\"DeepSeek\",\"ok\":true,\"status\":\"normal\",\"metrics\":["
             "{\"label\":\"Account balance remaining\",\"pct\":0,\"resetMin\":0,\"value\":\"12,345,678,901 CNY\"}]}]}";
    }
    usageApply(String(mock));
    previewDirty = true;
  }

  std::string autosavePath() const {
#if defined(_WIN32)
    const char* base = std::getenv("APPDATA");
    return std::string(base ? base : ".") + "/smalltv/editor-autosave.json";
#else
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/smalltv/editor-autosave.json";
#endif
  }

  void autosaveTick() {
    if (!viewLoaded || !viewDirty) return;
    const uint32_t now = SDL_GetTicks();
    if (lastAutosave && now - lastAutosave < 1500) return;
    lastAutosave = now;
    const std::string content = serializeView();
    if (content == lastAutosaveContent) return;
    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::path(autosavePath()).parent_path(), error);
    if (write_file(autosavePath(), content)) lastAutosaveContent = content;
  }

  static std::string bundleBase64Encode(const std::string& input) {
    static const char* alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);
    for (size_t i = 0; i < input.size(); i += 3) {
      const uint32_t a = (uint8_t)input[i];
      const uint32_t b = i + 1 < input.size() ? (uint8_t)input[i + 1] : 0;
      const uint32_t c = i + 2 < input.size() ? (uint8_t)input[i + 2] : 0;
      const uint32_t value = (a << 16) | (b << 8) | c;
      out += alphabet[(value >> 18) & 63];
      out += alphabet[(value >> 12) & 63];
      out += i + 1 < input.size() ? alphabet[(value >> 6) & 63] : '=';
      out += i + 2 < input.size() ? alphabet[value & 63] : '=';
    }
    return out;
  }

  static bool bundleBase64Decode(const std::string& input, std::string& out) {
    auto value = [](char c) -> int {
      if (c >= 'A' && c <= 'Z') return c - 'A';
      if (c >= 'a' && c <= 'z') return c - 'a' + 26;
      if (c >= '0' && c <= '9') return c - '0' + 52;
      if (c == '+') return 62;
      if (c == '/') return 63;
      return -1;
    };
    out.clear();
    uint32_t buffer = 0;
    int bits = 0;
    for (char c : input) {
      if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
      const int v = value(c);
      if (v < 0) return false;
      buffer = (buffer << 6) | (uint32_t)v;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out += (char)((buffer >> bits) & 0xFF);
      }
    }
    return true;
  }

  void exportDashboardBundle(const std::string& path) {
    JsonDocument bundle;
    bundle["format"] = "smalltv-dashboard";
    bundle["version"] = 1;
    bundle["view"].set(view.as<JsonObject>());
    JsonObject assets = bundle["assets"].to<JsonObject>();
    std::vector<std::string> names;
    collectImageNames(names);
    for (const auto& name : names) {
      const std::string assetPath = (std::filesystem::path(imageCacheDir()) /
                                     (name + ".simg")).string();
      std::string data;
      if (!read_file(assetPath.c_str(), data)) {
        notify("Bundle export failed: missing " + name);
        return;
      }
      assets[name] = bundleBase64Encode(data);
    }
    std::string text;
    serializeJson(bundle, text);
    if (write_file(path, text)) notify("Exported dashboard bundle");
    else notify("Cannot write bundle");
  }

  void importDashboardBundle(const std::string& path) {
    std::string text;
    if (!read_file(path.c_str(), text)) { notify("Cannot read bundle"); return; }
    JsonDocument bundle;
    if (deserializeJson(bundle, text) || !bundle.is<JsonObject>()) {
      notify("Invalid dashboard bundle"); return;
    }
    JsonObject savedView = bundle["view"].as<JsonObject>();
    if (savedView.isNull()) { notify("Bundle has no view"); return; }
    std::error_code error;
    std::filesystem::create_directories(imageCacheDir(), error);
    JsonObject assets = bundle["assets"].as<JsonObject>();
    for (JsonPair asset : assets) {
      if (!imageAssetNameValid(asset.key().c_str())) {
        notify("Bundle has an invalid asset name");
        return;
      }
      std::string data;
      if (!bundleBase64Decode(asset.value().as<const char*>() ? asset.value().as<const char*>() : "", data)) {
        notify("Bundle has invalid asset data");
        return;
      }
      const std::string pathOut = (std::filesystem::path(imageCacheDir()) /
                                   (std::string(asset.key().c_str()) + ".simg")).string();
      if (!write_file(pathOut, data)) { notify("Cannot write bundled asset"); return; }
      imagecodec::AssetInfo info;
      std::string err;
      if (!imagecodec::readAssetInfo(pathOut, info, err)) {
        notify("Bundle has invalid image asset");
        return;
      }
    }
    std::string viewText;
    serializeJson(savedView, viewText);
    std::string err;
    if (!loadViewText(viewText, err)) { notify("Bundle view: " + err); return; }
    viewLoaded = true;
    notify("Imported dashboard bundle");
  }

  void refreshImageList() {
    imageManagerLoaded = true;
    imageManagerError.clear();
    DeviceResponse r;
    if (!api("GET", "/api/images", "", "", r)) {
      imageListJson.clear();
      imageManagerError = "Could not load device images";
      return;
    }
    imageListJson = r.body;
  }

  void openImageManager() {
    imageManagerOpen = true;
    imageManagerLoaded = false;
    refreshImageList();
  }

  void imageManagerModal() {
    if (!imageManagerOpen) return;
    ImGui::SetNextWindowSize(ImVec2(560, 430), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Device images", &imageManagerOpen)) return;
    if (ImGui::Button("Refresh")) refreshImageList();
    ImGui::SameLine();
    if (ImGui::Button("Close")) { imageManagerOpen = false; ImGui::CloseCurrentPopup(); }
    if (!imageManagerError.empty()) ImGui::TextWrapped("%s", imageManagerError.c_str());
    ImGui::Separator();
    JsonDocument doc;
    if (!imageListJson.empty() && !deserializeJson(doc, imageListJson) && doc.is<JsonArray>()) {
      ImGui::BeginChild("##image-list", ImVec2(0, -36), true);
      for (JsonObject image : doc.as<JsonArray>()) {
        const std::string name = image["name"] | "";
        char label[160];
        snprintf(label, sizeof(label), "%s  %ux%u  %u frame(s)  %u B",
                 name.c_str(), (unsigned)(image["width"] | 0),
                 (unsigned)(image["height"] | 0), (unsigned)(image["frames"] | 0),
                 (unsigned)(image["size"] | 0));
        ImGui::PushID(name.c_str());
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) {
          DeviceResponse r;
          if (api("DELETE", "/api/image?name=" + name, "", "", r)) refreshImageList();
          else imageManagerError = "Delete failed for " + name;
        }
        ImGui::PopID();
      }
      ImGui::EndChild();
    } else if (imageListJson.empty() && imageManagerError.empty()) {
      ImGui::TextDisabled("No images.");
    }
    ImGui::TextDisabled("Images are stored on the device; referenced assets are uploaded when saving.");
    ImGui::EndPopup();
  }

  void refreshPreview() {
    std::string s = serializeView();
    ViewScreen screen;
    String err;
    if (!viewScreenParse(s.c_str(), s.size(), screen, err)) {
      viewError = err.c_str();
      return;
    }
    if (!previewAnimStart) previewAnimStart = SDL_GetTicks();
    imagecodec::HostImageSource images(imageCacheDir());
    viewError.clear();
    viewRenderFrame(screen, SDL_GetTicks() - previewAnimStart, &images);
    upload_preview_texture();
    previewDirty = false;
  }

  void initView() {
    std::string text;
    if (!read_file(initialViewPath.c_str(), text)) return;
    std::string err;
    if (loadViewText(text, err)) viewLoaded = true;
  }

  // ---- discovery ---------------------------------------------------------
  void scan() {
    if (manager.scanning()) return;
    scanRequested = true;
    manager.discover();
  }

  bool scanButton(const char* label, ImVec2 size = ImVec2(0, 0)) {
    if (manager.scanning()) {
      ImGui::BeginDisabled();
      theme::secondaryButton("Scanning...", size);
      ImGui::EndDisabled();
      return false;
    }
    if (theme::secondaryButton(label, size)) {
      scan();
      return true;
    }
    return false;
  }

  void scanProgress() {
    const ScanState state = manager.scanState();
    if (!state.running) return;
    char overlay[64];
    if (state.total > 0) snprintf(overlay, sizeof(overlay), "%d / %d", state.done, state.total);
    else snprintf(overlay, sizeof(overlay), "starting...");
    const float fraction = state.total > 0 ? (float)state.done / (float)state.total : 0.0f;
    ImGui::ProgressBar(fraction, ImVec2(-1, 0), overlay);
    if (!state.subnet.empty()) ImGui::TextWrapped("Scanning %s", state.subnet.c_str());
    else ImGui::TextDisabled("Finding local subnet...");
    if (state.found > 0) ImGui::TextDisabled("%d device(s) found", state.found);
  }

  void pollDevices() {
    devices = manager.knownDevices();
    if (!scanRequested || manager.scanning()) return;
    scanRequested = false;
    const ScanState state = manager.scanState();
    if (!state.error.empty()) {
      notify("Scan failed: " + state.error);
    } else if (devices.empty()) {
      notify("Scan complete: no devices found");
    } else if (devices.size() == 1) {
      notify("Scan complete: 1 device found");
    } else {
      notify("Scan complete: " + std::to_string(devices.size()) + " devices found");
    }
  }

  // ---- settings forms ----------------------------------------------------
  void formStart(const char* id) {
    ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchSame);
    g_formGrid = true;
  }
  void formEnd() {
    ImGui::EndTable();
    g_formGrid = false;
  }

  void settingsWifi() {
    JsonObject o = cfg.as<JsonObject>();
    formStart("##wifihead");
    input_text_json(o, "hostname", "Hostname");
    input_text_json(o, "apSsid", "AP SSID");
    input_text_json(o, "apPass", "AP password", true, "Leave blank to keep the current password");
    input_int_json(o, "httpTimeout", "HTTP timeout (ms)", 1000, 20000, 8000);
    formEnd();
    ImGui::SeparatorText("WiFi networks");
    JsonArray wifi = o["wifi"].as<JsonArray>();
    if (wifi.isNull()) wifi = o["wifi"].to<JsonArray>();
    for (int i = 0; i < (int)wifi.size();) {
      JsonObject e = wifi[i];
      ImGui::PushID(i);
      formStart("##network");
      input_text_json(e, "ssid", "SSID");
      input_text_json(e, "pass", "Password", true, "Blank keeps the current password");
      formEnd();
      bool set = e["passSet"] | false;
      if (set) ImGui::TextDisabled("password is set");
      if (ImGui::Button("Remove")) { wifi.remove(i); ImGui::PopID(); continue; }
      ImGui::PopID();
      ImGui::Separator();
      i++;
    }
    static char ssid[96] = "";
    static char pass[96] = "";
    formStart("##newwifi");
    formNext();
    ImGui::TextDisabled("New SSID");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##newssid", ssid, sizeof(ssid));
    formNext();
    ImGui::TextDisabled("New password");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##newpass", pass, sizeof(pass), ImGuiInputTextFlags_Password);
    formEnd();
    if (ImGui::Button("Add network") && ssid[0]) {
      JsonObject e = wifi.add<JsonObject>();
      e["ssid"] = ssid;
      if (pass[0]) e["pass"] = pass;
      ssid[0] = pass[0] = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Scan networks")) {
      DeviceResponse r;
      if (api("GET", "/api/scan", "", "", r)) {
        scanDoc.clear();
        deserializeJson(scanDoc, r.body);
        notify("WiFi scan complete");
      }
    }
    if (scanDoc.is<JsonArray>()) {
      ImGui::BeginChild("##scan", ImVec2(0, 130), true);
      for (JsonObject ap : scanDoc.as<JsonArray>()) {
        const char* name = ap["ssid"] | "";
        if (!name[0]) continue;
        ImGui::Text("%s  (%d dBm)", name, (int)(ap["rssi"] | 0));
        ImGui::SameLine();
        if (ImGui::SmallButton((std::string("Add##") + name).c_str())) {
          JsonArray w = o["wifi"].as<JsonArray>();
          if (w.isNull()) w = o["wifi"].to<JsonArray>();
          JsonObject e = w.add<JsonObject>();
          e["ssid"] = name;
        }
      }
      ImGui::EndChild();
    }
    if (ImGui::Button("Save settings")) saveConfig();
  }

  void settingsDisplay() {
    JsonObject o = cfg.as<JsonObject>();
    formStart("##displaybasic");
    input_int_json(o, "brightness", "Brightness", 0, 100, 80);
    input_bool_json(o, "autoBrightness", "Auto brightness (LDR)");
    input_bool_json(o, "backlightInverted", "Backlight inverted");
    input_int_json(o, "rotation", "Rotation (0-3)", 0, 3, 0);
    formEnd();
    ImGui::SeparatorText("Panel colour");
    JsonObject d = o["display"].as<JsonObject>();
    if (d.isNull()) d = o["display"].to<JsonObject>();
    static const char* orders[] = {"auto", "rgb", "bgr"};
    const char* cur = d["colorOrder"] | "auto";
    int idx = !strcmp(cur, "rgb") ? 1 : !strcmp(cur, "bgr") ? 2 : 0;
    formStart("##panel");
    formNext();
    ImGui::TextDisabled("Colour order");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##colorOrder", &idx, orders, 3)) d["colorOrder"] = orders[idx];
    input_bool_json(d, "invert", "Invert");
    input_int_json(d, "rGain", "Red gain", 0, 255, 255);
    input_int_json(d, "gGain", "Green gain", 0, 255, 255);
    input_int_json(d, "bGain", "Blue gain", 0, 255, 255);
    formEnd();
    ImGui::SeparatorText("Clock / night mode");
    JsonObject c = o["clock"].as<JsonObject>();
    if (c.isNull()) c = o["clock"].to<JsonObject>();
    formStart("##clock");
    input_text_json(c, "tz", "Timezone");
    input_text_json(c, "tzPosix", "POSIX TZ");
    input_bool_json(c, "nightEnabled", "Night mode");
    input_text_json(c, "nightStart", "Night start (HH:MM)");
    input_text_json(c, "nightEnd", "Night end (HH:MM)");
    input_int_json(c, "nightLevel", "Night brightness", 0, 100, 15);
    formEnd();
    ImGui::SeparatorText("Button");
    JsonObject b = o["button"].as<JsonObject>();
    if (b.isNull()) b = o["button"].to<JsonObject>();
    formStart("##button");
    input_int_json(b, "sensitivity", "Sensitivity", 50, 99, 80);
    input_int_json(b, "shortAction", "Short action", 0, 8, 0);
    input_int_json(b, "longAction", "Long action", 0, 8, 0);
    formEnd();
    if (ImGui::Button("Save settings")) saveConfig();
  }

  void settingsView() {
    JsonObject o = cfg.as<JsonObject>();
    JsonObject v = o["view"].as<JsonObject>();
    if (v.isNull()) v = o["view"].to<JsonObject>();
    formStart("##viewform");
    input_text_json(v, "pollUrl", "Overlay poll URL");
    input_int_json(v, "pollSec", "Poll seconds (0 off)", 0, 86400, 0);
    input_int_json(v, "ttlSec", "Default overlay TTL", 1, 86400, 15);
    input_text_json(v, "pollAuthHeader", "Poll auth header");
    input_text_json(v, "pollAuthValue", "Poll auth value", true, "Blank keeps the current value");
    input_text_json(v, "editorToken", "Editor token", true, "Blank keeps the current token");
    formEnd();
    if (ImGui::Button("Save settings")) saveConfig();
    ImGui::SeparatorText("Dashboard");
    if (ImGui::Button("Load from device")) loadViewFromDevice();
    ImGui::SameLine();
    if (ImGui::Button("Save to device")) saveViewToDevice();
    ImGui::SameLine();
    if (ImGui::Button("Send as overlay")) sendOverlay(false);
    ImGui::SameLine();
    if (ImGui::Button("Clear overlay")) sendOverlay(true);
  }

  void diagnosticsSection() {
    ImGui::SeparatorText("App update");
    ImGui::TextDisabled("Installed: " FW_VERSION);
    if (appUpdateChecking) ImGui::TextDisabled("Checking GitHub...");
    if (!appUpdateError.empty()) ImGui::TextWrapped("Check failed: %s", appUpdateError.c_str());
    if (ImGui::Button("Check for app updates")) checkAppUpdate(true);
    if (appUpdateDownloading) {
      ImGui::TextDisabled("Downloading the app update...");
      ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(240, 0), "downloading");
    }

    ImGui::SeparatorText("Diagnostics");
    if (ImGui::Button("View current boot log")) readDiagnostics(false);
    ImGui::SameLine();
    if (ImGui::Button("View previous boot log")) readDiagnostics(true);
    if (diagOpen) {
      ImGui::Spacing();
      ImGui::TextDisabled("%s", diagTitle.c_str());
      ImGui::BeginChild("##diaglog", ImVec2(0, 180), true,
                        ImGuiWindowFlags_HorizontalScrollbar);
      ImGui::TextUnformatted(diagText.c_str());
      ImGui::EndChild();
      if (ImGui::Button("Copy log")) ImGui::SetClipboardText(diagText.c_str());
      ImGui::SameLine();
      if (ImGui::Button("Close log")) diagOpen = false;
    }
  }

  void settingsSystem() {
    JsonObject o = cfg.as<JsonObject>();
    JsonObject a = o["auth"].as<JsonObject>();
    if (a.isNull()) a = o["auth"].to<JsonObject>();
    ImGui::SeparatorText("Web password");
    formStart("##authform");
    input_bool_json(a, "enabled", "Require a password");
    input_text_json(a, "user", "User");
    input_text_json(a, "pass", "Password", true, "Blank keeps the current password");
    formEnd();
    if (ImGui::Button("Save security settings")) saveConfig();
    ImGui::SeparatorText("Firmware");
    if (ImGui::Button("Choose .bin file...")) {
      dialog.openOpen("Firmware image", ".bin", [this](const std::string& p) {
        firmwarePath = p;
        notify("Firmware selected: " + p);
      });
    }
    if (!firmwarePath.empty()) {
      ImGui::TextWrapped("%s", firmwarePath.c_str());
      if (uploadRunning) {
        const size_t done = manager.uploadDone();
        const size_t total = manager.uploadTotal();
        const float fraction = total ? (float)done / (float)total : 0.0f;
        char label[64];
        if (total) snprintf(label, sizeof(label), "%llu / %llu bytes",
                            (unsigned long long)done, (unsigned long long)total);
        else snprintf(label, sizeof(label), "uploading");
        ImGui::ProgressBar(total ? fraction : -1.0f * (float)ImGui::GetTime(),
                           ImVec2(240, 0), label);
        ImGui::TextDisabled("Sending firmware to the device...");
      } else {
        ImGui::BeginDisabled(updateWatch);
        if (ImGui::Button("Upload firmware")) {
          const std::string path = firmwarePath;
          uploadStartVersion = deviceVersion;
          if (uploadStartVersion.empty() && !status.empty()) {
            JsonDocument doc;
            if (!deserializeJson(doc, status) && doc.is<JsonObject>()) {
              const char* version = doc["version"] | "";
              if (version[0]) uploadStartVersion = version;
            }
          }
          uploadRunning = true;
          uploadError.clear();
          updateTarget.clear();
          connState = ConnState::Updating;
          connected = false;
          updatePhase = "uploading";
          updateMessage = "Sending firmware to the device";
          updateError.clear();
          updateProgress = -1;
          notify("Uploading firmware...");
          uploadFuture = std::async(std::launch::async, [this, path] {
            std::string error;
            bool ok = manager.uploadFirmware(path, error);
            uploadError = error;
            return ok;
          });
        }
        ImGui::EndDisabled();
      }
    }
    ImGui::Spacing();
    if (ImGui::Button("Refresh firmware releases")) refreshFirmwareReleases();
    if (firmwareReleasesLoading) ImGui::TextDisabled("Loading releases from GitHub...");
    if (!firmwareReleaseError.empty()) ImGui::TextWrapped("Release list: %s", firmwareReleaseError.c_str());
    if (!firmwareReleases.empty() && firmwareReleases[0].tag.empty()) {}
    if (!firmwareReleases.empty() && firmwareReleases[0].tag[0] != '!') {
      const char* currentTag = firmwareReleases[firmwareReleaseIndex].tag.c_str();
      ImGui::SetNextItemWidth(-1);
      if (ImGui::BeginCombo("##firmware-release", currentTag)) {
        for (int i = 0; i < (int)firmwareReleases.size(); i++) {
          const bool selected = i == firmwareReleaseIndex;
          if (ImGui::Selectable(firmwareReleases[i].tag.c_str(), selected))
            firmwareReleaseIndex = i;
          if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      const appupdate::Asset* asset = selectedFirmwareAsset();
      if (asset) ImGui::TextDisabled("Asset: %s", asset->name.c_str());
      else ImGui::TextDisabled("Connect a device to choose the matching firmware asset.");
      ImGui::BeginDisabled(!asset || uploadRunning);
      if (ImGui::Button("Download and install")) installSelectedFirmware();
      ImGui::EndDisabled();
    }
    if (updateWatch) {
      ImGui::Spacing();
      ImGui::TextDisabled("Update: %s", updatePhase.empty() ? "starting" : updatePhase.c_str());
      const float fraction = updateProgress >= 0 ? (float)updateProgress / 100.0f
                                                 : -1.0f * (float)ImGui::GetTime();
      char label[64];
      if (updateProgress >= 0) snprintf(label, sizeof(label), "%d%%", updateProgress);
      else snprintf(label, sizeof(label), "working");
      ImGui::ProgressBar(fraction, ImVec2(240, 0), label);
      if (!updateTarget.empty()) ImGui::TextDisabled("Target: %s", updateTarget.c_str());
      if (!updateMessage.empty()) ImGui::TextWrapped("%s", updateMessage.c_str());
      if (!updateError.empty()) ImGui::TextWrapped("Error: %s", updateError.c_str());
    } else if (!updateMessage.empty()) {
      ImGui::Spacing();
      ImGui::TextWrapped("%s", updateMessage.c_str());
      if (!updateError.empty()) ImGui::TextWrapped("Error: %s", updateError.c_str());
    }
    ImGui::SeparatorText("Backup");
    if (ImGui::Button("Export settings...")) {
      dialog.openSave("Export settings", ".json", "smalltv-settings.json",
                      [this](const std::string& p) {
        DeviceResponse r;
        if (api("GET", "/api/export", "", "", r)) {
          if (write_file(p, r.body)) notify("Exported " + p);
          else notify("Cannot write " + p);
        }
      });
    }
    ImGui::SameLine();
    if (ImGui::Button("Import settings...")) {
      dialog.openOpen("Import settings", ".json", [this](const std::string& p) {
        std::string body;
        if (!read_file(p.c_str(), body)) { notify("Cannot read " + p); return; }
        DeviceResponse r;
        if (api("POST", "/api/import", body, "application/json", r)) notify("Settings imported");
      });
    }
    diagnosticsSection();
    ImGui::SeparatorText("Restart");
    if (ImGui::Button("Reboot device")) {
      DeviceResponse r;
      if (api("POST", "/api/reboot", "", "application/json", r)) notify("Reboot requested");
    }
    ImGui::SameLine();
    if (ImGui::Button("Factory reset")) ImGui::OpenPopup("Factory reset?");
    if (ImGui::BeginPopupModal("Factory reset?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextWrapped("Erase the device configuration and reboot into setup mode?");
      if (ImGui::Button("Factory reset")) {
        DeviceResponse r;
        if (api("POST", "/api/factory", "", "application/json", r)) notify("Factory reset requested");
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  }

  void settingsJson() {
    if (ImGui::Button("Reload from device")) loadConfig();
    ImGui::SameLine();
    if (ImGui::Button("Save JSON to device")) {
      cfg.clear();
      if (deserializeJson(cfg, cfgRaw)) { notify("Invalid config JSON"); return; }
      saveConfig();
    }
    ImGui::InputTextMultiline("##configjson", cfgRaw, sizeof(cfgRaw),
                              ImVec2(-1, -1));
  }

  // ---- editor ------------------------------------------------------------
  std::string componentId(JsonObject l) {
    const char* id = l["id"] | "";
    return id;
  }

  void ensureComponentIds() {
    JsonArray draw = view["draw"].as<JsonArray>();
    std::set<std::string> used;
    for (JsonObject l : draw) {
      const std::string id = componentId(l);
      if (!id.empty()) used.insert(id);
    }
    int next = 1;
    const int count = (int)draw.size();
    for (int i = 0; i < count; i++) {
      draw = view["draw"].as<JsonArray>();
      JsonObject l = draw[i];
      if (!componentId(l).empty()) continue;
      char id[24];
      do snprintf(id, sizeof(id), "c%d", next++); while (used.count(id));
      l["id"] = id;
      used.insert(id);
    }
  }

  int componentZ(JsonObject l, int fallback) const {
    return l["z"] | fallback;
  }

  std::vector<int> componentsByZ(bool frontFirst = true) {
    JsonArray draw = view["draw"].as<JsonArray>();
    std::vector<int> order;
    for (int i = 0; i < (int)draw.size(); i++) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
      const int za = componentZ(draw[a], a), zb = componentZ(draw[b], b);
      return frontFirst ? za > zb : za < zb;
    });
    return order;
  }

  int maxZ() {
    JsonArray draw = view["draw"].as<JsonArray>();
    int z = 0;
    for (int i = 0; i < (int)draw.size(); i++) z = std::max(z, componentZ(draw[i], i));
    return z;
  }

  void selectComponent(int index, bool additive) {
    usageSelectedKind = 0;
    JsonArray draw = view["draw"].as<JsonArray>();
    if (index < 0 || index >= (int)draw.size()) {
      selected = -1;
      selection.clear();
      return;
    }
    if (!additive) selection.clear();
    if (selection.count(index)) selection.erase(index);
    else selection.insert(index);
    if (selection.empty()) {
      selection.insert(index);
    }
    selected = index;
  }

  void selectOnly(int index) {
    usageSelectedKind = 0;
    selection.clear();
    if (index >= 0) selection.insert(index);
    selected = index;
  }

  void syncSelection() {
    JsonArray draw = view["draw"].as<JsonArray>();
    std::set<int> valid;
    for (int i : selection) if (i >= 0 && i < (int)draw.size()) valid.insert(i);
    selection.swap(valid);
    if (selection.empty()) selected = -1;
    else if (!selection.count(selected)) selected = *selection.begin();
  }

  void reindexZFromOrder(const std::vector<int>& order) {
    JsonArray draw = view["draw"].as<JsonArray>();
    for (int pos = 0; pos < (int)order.size(); pos++) {
      if (order[pos] >= 0 && order[pos] < (int)draw.size())
        draw[order[pos]]["z"] = pos;
    }
  }

  void moveSelectedZ(int direction, bool toEnd = false) {
    if (selection.empty()) return;
    pushUndo();
    std::vector<int> order = componentsByZ(false);  // back to front
    if (toEnd) {
      std::vector<int> moving, rest;
      for (int i : order) (selection.count(i) ? moving : rest).push_back(i);
      if (direction > 0) rest.insert(rest.end(), moving.begin(), moving.end());
      else rest.insert(rest.begin(), moving.begin(), moving.end());
      order = std::move(rest);
    } else if (direction != 0) {
      if (direction > 0) {
        for (int i = (int)order.size() - 2; i >= 0; i--) {
          if (selection.count(order[i]) && !selection.count(order[i + 1]))
            std::swap(order[i], order[i + 1]);
        }
      } else {
        for (int i = 1; i < (int)order.size(); i++) {
          if (selection.count(order[i]) && !selection.count(order[i - 1]))
            std::swap(order[i], order[i - 1]);
        }
      }
    }
    reindexZFromOrder(order);
    viewDirty = true;
    previewDirty = true;
    syncJson();
  }

  static void layer_bounds(JsonObject l, float& x, float& y, float& w, float& h) {
    const char* t = l["t"] | "";
    x = l["x"] | 0;
    y = l["y"] | 0;
    int s = l["s"] | 1;
    if (!l["sizePx"].isNull() && (int)(l["sizePx"] | 0) > 0)
      s = std::max(1, (int)(l["sizePx"] | 0) / 8);
    const char* a = l["a"] | "l";
    w = h = 0;
    if (!strcmp(t, "text")) {
      const int boxW = l["w"] | 0, boxH = l["h"] | 0;
      if (boxW > 0 || boxH > 0) {
        w = boxW > 0 ? (float)boxW : (float)strlen(l["v"] | "") * 6 * s;
        h = boxH > 0 ? (float)boxH : 8.0f * s;
        x = (float)(l["x"] | 0) + (l["mx"] | 0);
        y = (float)(l["y"] | 0) + (l["my"] | 0);
      } else {
        w = (float)strlen(l["v"] | "") * 6 * s;
        h = 8.0f * s;
        if (a[0] == 'c') x -= w / 2;
        else if (a[0] == 'r') x -= w;
      }
    } else if (!strcmp(t, "rect") || !strcmp(t, "rrect") || !strcmp(t, "usage")) {
      w = (float)(l["w"] | 0);
      h = (float)(l["h"] | 0);
    } else if (!strcmp(t, "bitmap") || !strcmp(t, "image")) {
      w = (float)(l["displayW"] | 0);
      h = (float)(l["displayH"] | 0);
      if (w <= 0 || h <= 0) {
        w = (float)(l["w"] | 0) * s;
        h = (float)(l["h"] | 0) * s;
      }
      if (a[0] == 'c') x -= w / 2;
      else if (a[0] == 'r') x -= w;
    } else if (!strcmp(t, "circle")) {
      int r = l["r"] | 0;
      w = h = (float)r * 2;
      x -= r;
      y -= r;
    } else if (!strcmp(t, "line")) {
      float x2 = l["x2"] | 0, y2 = l["y2"] | 0;
      w = std::abs(x2 - x);
      h = std::abs(y2 - y);
      x = std::min(x, x2);
      y = std::min(y, y2);
    } else if (!strcmp(t, "icon")) {
      w = h = (float)((l["sizePx"] | 0) > 0 ? (int)(l["sizePx"] | 0) : 24 * s);
      if (a[0] == 'c') x -= w / 2;
      else if (a[0] == 'r') x -= w;
    } else if (!strcmp(t, "fill")) {
      w = 240; h = 240;
    }
  }

  void addLayer(const char* type) {
    pushUndo();
    JsonArray draw = view["draw"].as<JsonArray>();
    if (draw.isNull()) draw = view["draw"].to<JsonArray>();
    JsonObject l = draw.add<JsonObject>();
    l["t"] = type;
    l["x"] = 120;
    l["y"] = 120;
    if (!strcmp(type, "text")) {
      l["s"] = 3; l["sizePx"] = 24; l["c"] = "#e9edf4"; l["a"] = "c"; l["v"] = "text";
    } else if (!strcmp(type, "rect") || !strcmp(type, "rrect")) {
      l["w"] = 100; l["h"] = 40; l["c"] = "#3fb950";
      if (!strcmp(type, "rrect")) l["r"] = 8;
    } else if (!strcmp(type, "circle")) {
      l["r"] = 30; l["c"] = "#d97757";
    } else if (!strcmp(type, "line")) {
      l["x2"] = 200; l["y2"] = 200; l["c"] = "#e9edf4";
    } else if (!strcmp(type, "icon")) {
      l["s"] = 2; l["sizePx"] = 48; l["c"] = "#86e2bf"; l["a"] = "c"; l["v"] = "sun";
    } else if (!strcmp(type, "bitmap")) {
      l["x"] = 100; l["y"] = 100; l["w"] = 8; l["h"] = 8; l["s"] = 2;
      l["displayW"] = 16; l["displayH"] = 16;
      l["a"] = "l"; l["c"] = "#3fb950"; l["d"] = "AA55AA55AA55AA55";
    } else if (!strcmp(type, "usage")) {
      l["w"] = 224; l["h"] = 54; l["c"] = "#d97757";
      l["provider"] = "claude"; l["metric"] = -1;
    } else {
      l["c"] = "#ffffff";
    }
    l["z"] = maxZ() + 1;
    ensureComponentIds();
    draw = view["draw"].as<JsonArray>();
    selectOnly((int)draw.size() - 1);
    viewDirty = true;
    previewDirty = true;
    syncJson();
  }

  void addTemplate(const char* kind) {
    pushUndo();
    JsonArray draw = view["draw"].as<JsonArray>();
    if (draw.isNull()) draw = view["draw"].to<JsonArray>();
    if (!strcmp(kind, "clock")) {
      draw = view["draw"].as<JsonArray>();
      JsonObject clock = draw.add<JsonObject>();
      clock["t"] = "text"; clock["x"] = 120; clock["y"] = 22; clock["s"] = 5; clock["sizePx"] = 40;
      clock["c"] = "#e9edf4"; clock["a"] = "c"; clock["v"] = "{HH}:{MM}";
      clock["z"] = maxZ() + 1;
      draw = view["draw"].as<JsonArray>();
      JsonObject date = draw.add<JsonObject>();
      date["t"] = "text"; date["x"] = 120; date["y"] = 64; date["s"] = 2; date["sizePx"] = 16;
      date["c"] = "#86e2bf"; date["a"] = "c"; date["v"] = "{WD} {YYYY}-{MON}-{DD}";
      date["z"] = maxZ() + 1;
    } else if (!strcmp(kind, "usage")) {
      static const char* providers[] = {"claude", "chatgpt", "deepseek"};
      static const char* colors[] = {"#d97757", "#10a37f", "#4d6bfe"};
      for (int i = 0; i < 3; i++) {
        draw = view["draw"].as<JsonArray>();
        JsonObject card = draw.add<JsonObject>();
        card["t"] = "usage"; card["x"] = 8; card["y"] = 8 + i * 58;
        card["w"] = 224; card["h"] = 52; card["c"] = colors[i];
        card["provider"] = providers[i]; card["metric"] = -1;
        card["z"] = maxZ() + 1;
      }
    }
    ensureComponentIds();
    draw = view["draw"].as<JsonArray>();
    if (draw.size() > 0) selectOnly((int)draw.size() - 1);
    viewDirty = true;
    previewDirty = true;
    syncJson();
  }

  void addImageLayer() {
    dialog.openOpen("Import image or GIF", ".png,.jpg,.jpeg,.gif,.bmp",
                    [this](const std::string& p) { addImportedLayer(p); });
  }

  void convertSelectedBitmapToImage(const std::string& source) {
    JsonArray draw = view["draw"].as<JsonArray>();
    if (selected < 0 || selected >= (int)draw.size()) return;
    JsonObject layer = draw[selected];
    if (strcmp(layer["t"] | "", "bitmap")) return;
    imagecodec::ImportedAsset asset;
    if (!importAsset(source, asset)) return;
    pushUndo();
    layer["t"] = "image";
    layer["src"] = asset.name;
    layer["w"] = asset.info.width;
    layer["h"] = asset.info.height;
    layer["s"] = 1;
    layer["displayW"] = asset.info.width;
    layer["displayH"] = asset.info.height;
    layer.remove("d");
    viewDirty = true;
    previewDirty = true;
    previewAnimStart = SDL_GetTicks();
    syncJson();
  }

  void replaceSelectedImage(const std::string& source) {
    JsonArray draw = view["draw"].as<JsonArray>();
    if (selected < 0 || selected >= (int)draw.size()) return;
    JsonObject layer = draw[selected];
    if (strcmp(layer["t"] | "", "image")) return;
    imagecodec::ImportedAsset asset;
    if (!importAsset(source, asset)) return;
    pushUndo();
    layer["src"] = asset.name;
    layer["w"] = asset.info.width;
    layer["h"] = asset.info.height;
    layer["displayW"] = asset.info.width;
    layer["displayH"] = asset.info.height;
    viewDirty = true;
    previewDirty = true;
    previewAnimStart = SDL_GetTicks();
    syncJson();
  }

  void duplicateSelected() {
    JsonArray draw = view["draw"].as<JsonArray>();
    if (selection.empty()) return;
    const std::string sourceGroup = selectedGroupId();
    std::string newGid;
    std::string sourceGroupName = "Group";
    bool duplicateGroup = false;
    if (!sourceGroup.empty()) {
      JsonObject sourceGroupObject = groupById(sourceGroup);
      JsonArray members = sourceGroupObject["members"].as<JsonArray>();
      sourceGroupName = (const char*)(sourceGroupObject["name"] | "Group");
      duplicateGroup = members.size() == selection.size();
      if (duplicateGroup) newGid = newGroupId();
    }
    pushUndo();
    std::vector<int> indices(selection.begin(), selection.end());
    std::sort(indices.begin(), indices.end());
    std::vector<int> added;
    const int baseZ = maxZ() + 1;
    int offset = 0;
    for (int index : indices) {
      draw = view["draw"].as<JsonArray>();
      if (index < 0 || index >= (int)draw.size()) continue;
      JsonDocument sourceDoc;
      sourceDoc.set(draw[index]);
      JsonObject src = sourceDoc.as<JsonObject>();
      draw = view["draw"].as<JsonArray>();
      JsonObject dst = draw.add<JsonObject>();
      for (JsonPair p : src) dst[p.key()] = p.value();
      dst["x"] = (src["x"] | 0) + 8;
      dst["y"] = (src["y"] | 0) + 8;
      dst["z"] = baseZ + offset++;
      dst.remove("id");
      if (duplicateGroup) dst["gid"] = newGid.c_str();
      else dst.remove("gid");
      added.push_back((int)draw.size() - 1);
    }
    if (added.empty()) return;
    ensureComponentIds();
    if (duplicateGroup) {
      JsonArray groups = view["groups"].as<JsonArray>();
      JsonObject group = groups.add<JsonObject>();
      group["id"] = newGid.c_str();
      group["name"] = sourceGroupName + " copy";
      JsonArray members = group["members"].to<JsonArray>();
      draw = view["draw"].as<JsonArray>();
      for (int i : added) members.add(componentId(draw[i]).c_str());
    }
    selection.clear();
    for (int i : added) selection.insert(i);
    selected = added.back();
    viewDirty = true;
    previewDirty = true;
    syncJson();
  }

  void copySelected() {
    JsonArray draw = view["draw"].as<JsonArray>();
    JsonDocument doc;
    JsonArray copy = doc.to<JsonArray>();
    for (int index : selection) {
      if (index < 0 || index >= (int)draw.size()) continue;
      JsonObject item = copy.add<JsonObject>();
      JsonObject source = draw[index];
      for (JsonPair p : source) item[p.key()] = p.value();
    }
    serializeJson(doc, clipboard);
    notify(selection.empty() ? "Nothing selected" : "Components copied");
  }

  void pasteComponents() {
    JsonDocument doc;
    if (deserializeJson(doc, clipboard) || !doc.is<JsonArray>()) {
      notify("Clipboard is empty");
      return;
    }
    JsonArray source = doc.as<JsonArray>();
    if (source.size() == 0) return;
    pushUndo();
    JsonArray draw = view["draw"].as<JsonArray>();
    const int baseZ = maxZ() + 1;
    std::vector<int> added;
    int offset = 0;
    for (JsonObject src : source) {
      JsonDocument sourceDoc;
      sourceDoc.set(src);
      JsonObject copy = sourceDoc.as<JsonObject>();
      draw = view["draw"].as<JsonArray>();
      JsonObject dst = draw.add<JsonObject>();
      for (JsonPair p : copy) dst[p.key()] = p.value();
      dst["x"] = (copy["x"] | 0) + 12;
      dst["y"] = (copy["y"] | 0) + 12;
      dst["z"] = baseZ + offset++;
      dst.remove("id");
      dst.remove("gid");
      added.push_back((int)draw.size() - 1);
    }
    ensureComponentIds();
    selection.clear();
    for (int i : added) selection.insert(i);
    selected = added.back();
    viewDirty = true;
    previewDirty = true;
    syncJson();
  }

  void deleteSelected() {
    JsonArray draw = view["draw"].as<JsonArray>();
    if (selection.empty()) return;
    pushUndo();
    std::vector<int> indices(selection.begin(), selection.end());
    std::sort(indices.begin(), indices.end(), std::greater<int>());
    std::set<std::string> removedIds;
    for (int index : indices) {
      if (index < 0 || index >= (int)draw.size()) continue;
      removedIds.insert(componentId(draw[index]));
      draw.remove(index);
    }
    JsonArray groups = view["groups"].as<JsonArray>();
    for (int i = (int)groups.size() - 1; i >= 0; i--) {
      JsonObject group = groups[i];
      JsonArray members = group["members"].as<JsonArray>();
      for (int j = (int)members.size() - 1; j >= 0; j--) {
        const std::string id = members[j] | "";
        if (removedIds.count(id)) members.remove(j);
      }
      if (members.size() == 0) groups.remove(i);
    }
    selected = -1;
    selection.clear();
    syncSelection();
    viewDirty = true;
    previewDirty = true;
    syncJson();
  }

  JsonObject groupById(const std::string& id) {
    JsonArray groups = view["groups"].as<JsonArray>();
    for (JsonObject group : groups) {
      if (id == (const char*)(group["id"] | "")) return group;
    }
    return JsonObject();
  }

  std::string selectedGroupId() {
    JsonArray draw = view["draw"].as<JsonArray>();
    std::string gid;
    for (int index : selection) {
      if (index < 0 || index >= (int)draw.size()) continue;
      const std::string current = draw[index]["gid"] | "";
      if (current.empty()) return "";
      if (gid.empty()) gid = current;
      else if (gid != current) return "";
    }
    return gid;
  }

  void selectGroup(const std::string& gid) {
    JsonArray draw = view["draw"].as<JsonArray>();
    selection.clear();
    for (int i = 0; i < (int)draw.size(); i++) {
      if (gid == (const char*)(draw[i]["gid"] | "")) selection.insert(i);
    }
    selected = selection.empty() ? -1 : *selection.begin();
    syncSelection();
  }

  std::string newGroupId() {
    JsonArray groups = view["groups"].as<JsonArray>();
    int serial = (int)groups.size() + 1;
    std::string id;
    do {
      id = "g" + std::to_string(serial++);
    } while (!groupById(id).isNull());
    return id;
  }

  void groupSelected() {
    if (selection.size() < 2) {
      notify("Select at least two components to group");
      return;
    }
    pushUndo();
    JsonArray draw = view["draw"].as<JsonArray>();
    ensureComponentIds();
    JsonArray groups = view["groups"].as<JsonArray>();
    if (groups.isNull()) groups = view["groups"].to<JsonArray>();
    const std::string gid = newGroupId();
    JsonObject group = groups.add<JsonObject>();
    group["id"] = gid;
    group["name"] = "Group";
    JsonArray members = group["members"].to<JsonArray>();
    draw = view["draw"].as<JsonArray>();
    for (int index : selection) {
      if (index < 0 || index >= (int)draw.size()) continue;
      const std::string id = componentId(draw[index]);
      if (id.empty()) continue;
      members.add(id.c_str());
      draw[index]["gid"] = gid.c_str();
    }
    viewDirty = true;
    previewDirty = true;
    syncJson();
  }

  void ungroupSelected() {
    const std::string gid = selectedGroupId();
    if (gid.empty()) return;
    pushUndo();
    std::set<std::string> removed;
    JsonArray draw = view["draw"].as<JsonArray>();
    for (int index : selection) {
      if (index >= 0 && index < (int)draw.size()) {
        removed.insert(componentId(draw[index]));
        draw[index].remove("gid");
      }
    }
    JsonArray groups = view["groups"].as<JsonArray>();
    for (int i = 0; i < (int)groups.size(); i++) {
      JsonObject group = groups[i];
      if (gid != (const char*)(group["id"] | "")) continue;
      JsonArray members = group["members"].as<JsonArray>();
      for (int j = (int)members.size() - 1; j >= 0; j--) {
        const std::string id = members[j] | "";
        if (removed.count(id)) members.remove(j);
      }
      if (members.size() == 0) groups.remove(i);
      break;
    }
    viewDirty = true;
    previewDirty = true;
    syncJson();
  }

  void setGroupHidden(const std::string& gid, bool hidden) {
    JsonArray draw = view["draw"].as<JsonArray>();
    for (JsonObject l : draw) {
      if (gid == (const char*)(l["gid"] | "")) l["hidden"] = hidden;
    }
    JsonObject group = groupById(gid);
    if (!group.isNull()) group["hidden"] = hidden;
  }

  void setGroupLocked(const std::string& gid, bool locked) {
    JsonArray draw = view["draw"].as<JsonArray>();
    for (JsonObject l : draw) {
      if (gid == (const char*)(l["gid"] | "")) l["locked"] = locked;
    }
    JsonObject group = groupById(gid);
    if (!group.isNull()) group["locked"] = locked;
  }

  void moveLayer(int delta) { moveSelectedZ(delta); }

  void distributeSelected(bool horizontal) {
    if (selection.size() < 3) {
      notify("Select at least three components to distribute");
      return;
    }
    pushUndo();
    JsonArray draw = view["draw"].as<JsonArray>();
    std::vector<int> indices;
    for (int i : selection) if (i >= 0 && i < (int)draw.size()) indices.push_back(i);
    std::sort(indices.begin(), indices.end(), [&](int a, int b) {
      float ax, ay, aw, ah, bx, by, bw, bh;
      layer_bounds(draw[a], ax, ay, aw, ah);
      layer_bounds(draw[b], bx, by, bw, bh);
      return horizontal ? ax < bx : ay < by;
    });
    float first = 0, last = 0, total = 0;
    for (int i = 0; i < (int)indices.size(); i++) {
      float x, y, w, h;
      layer_bounds(draw[indices[i]], x, y, w, h);
      if (i == 0) first = horizontal ? x : y;
      last = horizontal ? x + w : y + h;
      total += horizontal ? w : h;
    }
    const float gap = (last - first - total) / (float)(indices.size() - 1);
    float cursor = first;
    for (int index : indices) {
      float x, y, w, h;
      layer_bounds(draw[index], x, y, w, h);
      if (horizontal) {
        draw[index]["x"] = (int)lroundf(cursor);
        cursor += w + gap;
      } else {
        draw[index]["y"] = (int)lroundf(cursor);
        cursor += h + gap;
      }
    }
    viewDirty = true;
    previewDirty = true;
    syncJson();
  }

  void alignSelected(JsonObject l, const char* mode) {
    const bool horizontal = !strcmp(mode, "left") || !strcmp(mode, "hcenter") || !strcmp(mode, "right");
    float x = 0, y = 0, w = 0, h = 0;
    if (usageSelectedKind && selected >= 0) {
      JsonObject box = usageLayoutBox(usageSelectedKind, usageSelectedRow);
      JsonArray draw = view["draw"].as<JsonArray>();
      if (!box.isNull() && selected < (int)draw.size()) {
        int cardW = draw[selected]["w"] | 240;
        int cardH = draw[selected]["h"] | 240;
        const int mx = box["mx"] | 0, my = box["my"] | 0;
        const int bw = box["w"] | 0, bh = box["h"] | 0;
        int targetX = 4, targetY = 4;
        if (!strcmp(mode, "hcenter")) targetX = (cardW - bw) / 2;
        else if (!strcmp(mode, "right")) targetX = cardW - bw - 4;
        else if (!strcmp(mode, "vcenter")) targetY = (cardH - bh) / 2;
        else if (!strcmp(mode, "bottom")) targetY = cardH - bh - 4;
        if (horizontal) box["x"] = targetX - mx;
        else box["y"] = targetY - my;
        markViewChanged();
        return;
      }
    }

    if (selection.size() > 1) {
      bool first = true;
      float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      JsonArray draw = view["draw"].as<JsonArray>();
      for (int i : selection) {
        if (i < 0 || i >= (int)draw.size()) continue;
        float bx, by, bw, bh;
        layer_bounds(draw[i], bx, by, bw, bh);
        if (first) { x0 = bx; y0 = by; x1 = bx + bw; y1 = by + bh; first = false; }
        else { x0 = std::min(x0, bx); y0 = std::min(y0, by); x1 = std::max(x1, bx + bw); y1 = std::max(y1, by + bh); }
      }
      if (first) return;
      x = x0; y = y0; w = x1 - x0; h = y1 - y0;
    } else {
      layer_bounds(l, x, y, w, h);
    }

    float dx = 0, dy = 0;
    if (!strcmp(mode, "left")) dx = 4 - x;
    else if (!strcmp(mode, "hcenter")) dx = 120 - (x + w / 2);
    else if (!strcmp(mode, "right")) dx = 236 - (x + w);
    else if (!strcmp(mode, "top")) dy = 4 - y;
    else if (!strcmp(mode, "vcenter")) dy = 120 - (y + h / 2);
    else if (!strcmp(mode, "bottom")) dy = 236 - (y + h);

    if (selection.size() > 1) {
      JsonArray draw = view["draw"].as<JsonArray>();
      for (int i : selection) {
        if (i < 0 || i >= (int)draw.size() || (draw[i]["locked"] | false)) continue;
        draw[i]["x"] = (int)((draw[i]["x"] | 0) + dx);
        draw[i]["y"] = (int)((draw[i]["y"] | 0) + dy);
      }
    } else {
      l["x"] = (int)((l["x"] | 0) + dx);
      l["y"] = (int)((l["y"] | 0) + dy);
    }
    markViewChanged();
  }

  void bitmapGrid(JsonObject l) {
    int w = std::max(1, std::min(48, (int)(l["w"] | 8)));
    int h = std::max(1, std::min(48, (int)(l["h"] | 8)));
    std::string d = l["d"] | "";
    size_t need = ((size_t)w * h + 7) / 8;
    if (d.size() != need * 2) d.assign(need * 2, '0');
    std::vector<uint8_t> bits((size_t)w * h);
    for (int i = 0; i < w * h; i++) {
      int byte = i >> 3, bit = 7 - (i & 7);
      int hi = d.size() > (size_t)byte * 2 ? (int)strtol(d.substr(byte * 2, 2).c_str(), nullptr, 16) : 0;
      bits[i] = (hi >> bit) & 1;
    }
    ImGui::Text("Pixel grid (%dx%d)", w, h);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float cell = std::max(2.0f, std::min(240.0f, avail.x) / std::max(w, h));
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##bitmapgrid", ImVec2(w * cell, h * cell));
    bool changed = false;
    if (ImGui::IsItemClicked(0)) {
      ImVec2 m = ImGui::GetIO().MousePos;
      int px = (int)((m.x - origin.x) / cell);
      int py = (int)((m.y - origin.y) / cell);
      if (px >= 0 && px < w && py >= 0 && py < h) {
        bits[py * w + px] ^= 1;
        changed = true;
      }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        ImVec2 a(origin.x + x * cell, origin.y + y * cell);
        ImVec2 b(a.x + cell, a.y + cell);
        dl->AddRectFilled(a, b, bits[y * w + x] ? IM_COL32(63, 185, 80, 255) : IM_COL32(20, 25, 33, 255));
        dl->AddRect(a, b, IM_COL32(70, 80, 95, 255));
      }
    }
    if (changed) {
      std::vector<uint8_t> out(need, 0);
      for (int i = 0; i < w * h; i++)
        if (bits[i]) out[i >> 3] |= (uint8_t)(1 << (7 - (i & 7)));
      std::string hex;
      char b[3];
      for (uint8_t v : out) { snprintf(b, sizeof(b), "%02X", v); hex += b; }
      l["d"] = hex.c_str();
      viewDirty = true;
      previewDirty = true;
      syncJson();
    }
  }

  bool style_checkbox(JsonObject style, const char* key, const char* label, bool fallback) {
    bool value = style[key] | fallback;
    if (ImGui::Checkbox(label, &value)) {
      style[key] = value;
      return true;
    }
    return false;
  }

  bool style_align(JsonObject style, const char* key, const char* label, const char* fallback) {
    static const char* names[] = {"auto", "l", "c", "r"};
    static const char* shown[] = {"Auto", "Left", "Centre", "Right"};
    const char* cur = style[key] | fallback;
    int index = !strcmp(cur, "l") ? 1 : !strcmp(cur, "c") ? 2 : !strcmp(cur, "r") ? 3 : 0;
    ImGui::TextDisabled("%s", label);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo((std::string("##align") + key).c_str(), &index, shown, 4)) {
      style[key] = names[index];
      return true;
    }
    return false;
  }

  void syncUsagePx(JsonObject style) {
    style["namePx"] = (int)(style["nameSize"] | 2) * 8;
    style["labelPx"] = (int)(style["labelSize"] | 0) * 8;
    style["valuePx"] = (int)(style["valueSize"] | 0) * 8;
    style["resetPx"] = (int)(style["resetSize"] | 1) * 8;
  }

  void applyUsagePreset(JsonObject style, int preset) {
    if (preset <= 0) return;
    style["showName"] = true;
    style["showDot"] = true;
    style["showLabel"] = true;
    style["showValue"] = true;
    style["showBar"] = true;
    style["showReset"] = true;
    style["showPanel"] = true;
    style["barAuto"] = true;
    style["nameAlign"] = "l";
    style["labelAlign"] = "auto";
    style["valueAlign"] = "auto";
    style["resetAlign"] = "l";
    style["resetSize"] = 1;
    if (preset == 1) {          // Standard
      style["nameSize"] = 2; style["labelSize"] = 0; style["valueSize"] = 0;
      style["barHeight"] = 8; style["radius"] = 8; style["padding"] = 12;
    } else if (preset == 2) {   // Compact
      style["nameSize"] = 1; style["labelSize"] = 1; style["valueSize"] = 2;
      style["barHeight"] = 4; style["radius"] = 6; style["padding"] = 8;
      style["showReset"] = false;
    } else if (preset == 3) {   // Large value
      style["nameSize"] = 2; style["labelSize"] = 0; style["valueSize"] = 5;
      style["barHeight"] = 8; style["radius"] = 8; style["padding"] = 10;
    } else if (preset == 4) {   // Minimal
      style["nameSize"] = 1; style["labelSize"] = 1; style["valueSize"] = 3;
      style["barHeight"] = 4; style["radius"] = 4; style["padding"] = 6;
      style["showPanel"] = false; style["showReset"] = false;
      style["showLabel"] = false;
    }
    syncUsagePx(style);
  }

  static void setUsageBox(JsonObject box, int x, int y, int w, int h,
                         int padX = 0, int padY = 0) {
    box["x"] = x; box["y"] = y; box["w"] = w; box["h"] = h;
    box["padX"] = padX; box["padY"] = padY;
    box["mx"] = 0; box["my"] = 0;
    box["a"] = "l"; box["va"] = "m";
  }

  void ensureUsageLayout(JsonObject style, JsonObject l) {
    JsonObject layout = style["layout"].as<JsonObject>();
    if (!layout.isNull() && !(layout["auto"] | false)) return;
    layout = style["layout"].to<JsonObject>();
    layout["auto"] = false;
    const int w = l["w"] | 224;
    const int h = l["h"] | 82;
    const int padding = style["padding"] | 12;
    const int count = 3;
    const int rowH = std::max(14, (h - 20) / count);
    setUsageBox(layout["name"].to<JsonObject>(), 8, 6, std::max(1, w - 16), 16);
    setUsageBox(layout["reset"].to<JsonObject>(), padding, h - 28,
                std::max(1, w - 2 * padding), 10);
    JsonArray rows = layout["rows"].to<JsonArray>();
    for (int i = 0; i < 5; i++) {
      JsonObject row = rows.add<JsonObject>();
      const int ry = 20 + i * rowH;
      setUsageBox(row["label"].to<JsonObject>(), padding, ry,
                  std::max(1, (w - 2 * padding) * 55 / 100), 12);
      setUsageBox(row["value"].to<JsonObject>(), padding + (w - 2 * padding) * 55 / 100,
                  std::max(0, ry - 2), std::max(1, (w - 2 * padding) * 45 / 100), 18);
      setUsageBox(row["bar"].to<JsonObject>(), padding, ry + rowH - 7,
                  std::max(1, w - 2 * padding), 4);
    }
  }

  bool usageBoxEditor(JsonObject box, const char* label) {
    if (box.isNull()) return false;
    ImGui::PushID(label);
    bool changed = false;
    ImGui::SeparatorText(label);
    changed |= input_int_json(box, "x", "X (px)", -512, 512, 0);
    changed |= input_int_json(box, "y", "Y (px)", -512, 512, 0);
    changed |= input_int_json(box, "w", "Width (px)", 0, 240, 0);
    changed |= input_int_json(box, "h", "Height (px)", 0, 240, 0);
    changed |= input_int_json(box, "mx", "Margin X (px)", -128, 127, 0);
    changed |= input_int_json(box, "my", "Margin Y (px)", -128, 127, 0);
    changed |= input_int_json(box, "padX", "Padding X (px)", 0, 60, 0);
    changed |= input_int_json(box, "padY", "Padding Y (px)", 0, 60, 0);
    static const char* aligns[] = {"l", "c", "r"};
    const char* cur = box["a"] | "l";
    int ai = !strcmp(cur, "c") ? 1 : !strcmp(cur, "r") ? 2 : 0;
    if (ImGui::Combo("Align", &ai, aligns, 3)) { box["a"] = aligns[ai]; changed = true; }
    static const char* valigns[] = {"t", "m", "b"};
    const char* cv = box["va"] | "m";
    int vi = !strcmp(cv, "t") ? 0 : !strcmp(cv, "b") ? 2 : 1;
    if (ImGui::Combo("Vertical align", &vi, valigns, 3)) { box["va"] = valigns[vi]; changed = true; }
    ImGui::PopID();
    return changed;
  }

  void usageStyleEditor(JsonObject l, bool& changed) {
    std::string key = componentId(l);
    if (key.empty()) key = "selected";
    JsonObject style = l["style"].as<JsonObject>();
    if (style.isNull()) style = l["style"].to<JsonObject>();
    if (ImGui::CollapsingHeader("Usage style", ImGuiTreeNodeFlags_DefaultOpen)) {
      static const char* presetNames[] = {"Custom", "Standard", "Compact", "Large value", "Minimal"};
      int preset = 0;
      ImGui::TextDisabled("Style preset");
      ImGui::SetNextItemWidth(-1);
      if (ImGui::Combo("##usage-preset", &preset, presetNames, 5) && preset) {
        applyUsagePreset(style, preset);
        changed = true;
      }
      const float styleHalf = std::max(70.0f, ImGui::GetContentRegionAvail().x / 2 - 3);
      if (ImGui::Button("Copy style", ImVec2(styleHalf, 0))) {
        JsonDocument clip;
        clip.set(style);
        serializeJson(clip, styleClipboard);
        notify("Usage style copied");
      }
      ImGui::SameLine();
      if (ImGui::Button("Paste style", ImVec2(styleHalf, 0))) {
        JsonDocument clip;
        if (deserializeJson(clip, styleClipboard) || !clip.is<JsonObject>()) {
          notify("Usage style clipboard is empty");
        } else {
          l["style"].set(clip.as<JsonObject>());
          style = l["style"].as<JsonObject>();
          syncUsagePx(style);
          changed = true;
          notify("Usage style pasted");
        }
      }
      changed |= input_px_size(style, "namePx", "nameSize", "Name height (px, 0 auto)", 8, 0, 48, 6, 2);
      changed |= input_px_size(style, "labelPx", "labelSize", "Label height (px, 0 auto)", 8, 0, 48, 6, 0);
      changed |= input_px_size(style, "valuePx", "valueSize", "Value height (px, 0 auto)", 8, 0, 48, 6, 0);
      changed |= input_px_size(style, "resetPx", "resetSize", "Reset height (px)", 8, 0, 48, 6, 1);
      changed |= style_align(style, "nameAlign", "Name align", "l");
      changed |= style_align(style, "labelAlign", "Label align", "auto");
      changed |= style_align(style, "valueAlign", "Value align", "auto");
      changed |= style_align(style, "resetAlign", "Reset align", "l");
      changed |= input_int_json(style, "barHeight", "Bar height (px)", 1, 32, 8);
      changed |= input_int_json(style, "radius", "Panel radius (px)", 0, 32, 8);
      changed |= input_int_json(style, "padding", "Padding (px)", 0, 60, 12);
      changed |= style_checkbox(style, "showName", "Show name", true);
      changed |= style_checkbox(style, "showDot", "Show status dot", true);
      changed |= style_checkbox(style, "showLabel", "Show metric label", true);
      changed |= style_checkbox(style, "showValue", "Show value", true);
      changed |= style_checkbox(style, "showBar", "Show bar", true);
      changed |= style_checkbox(style, "showReset", "Show reset", true);
      changed |= style_checkbox(style, "showPanel", "Show panel", true);
      bool barAuto = style["barAuto"] | true;
      if (ImGui::Checkbox("Automatic bar colour", &barAuto)) {
        style["barAuto"] = barAuto;
        changed = true;
      }
      changed |= color_edit_json(style, "panel", "Panel colour", "#181818");
      changed |= color_edit_json(style, "track", "Track colour", "#2a2a28");
      changed |= color_edit_json(style, "valueColor", "Value colour", "#ffffff");
      changed |= color_edit_json(style, "labelColor", "Label colour", "#8b96a5");
      changed |= color_edit_json(style, "resetColor", "Reset colour", "#8b96a5");
      changed |= color_edit_json(style, "nameColor", "Name colour", "#d97757");
      if (!barAuto) changed |= color_edit_json(style, "barColor", "Bar colour", "#7c6b52");
      changed |= input_text_json(style, "title", "Card title", false,
                                 "Blank uses the provider name");
      static char labelBuf[5][VIEW_USAGE_TEXT_MAX] = {};
      static std::string labelKey;
      if (labelKey != key) {
        labelKey = key;
        JsonArray labels = style["labels"].as<JsonArray>();
        for (int i = 0; i < 5; i++) {
          const char* value = (i < (int)labels.size()) ? (const char*)(labels[i] | "") : "";
          snprintf(labelBuf[i], sizeof(labelBuf[i]), "%s", value ? value : "");
        }
      }
      ImGui::TextDisabled("Metric labels (one per metric)");
      bool labelsChanged = false;
      for (int i = 0; i < 5; i++) {
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(-1);
        char id[24];
        snprintf(id, sizeof(id), "##usage-label-%d", i);
        if (ImGui::InputText(id, labelBuf[i], sizeof(labelBuf[i]))) labelsChanged = true;
        ImGui::PopID();
      }
      if (labelsChanged) {
        JsonArray labels = style["labels"].to<JsonArray>();
        for (int i = 0; i < 5; i++) labels.add(labelBuf[i]);
        changed = true;
      }
      bool custom = !style["layout"].isNull() && !(style["layout"]["auto"] | false);
      if (ImGui::Checkbox("Custom geometry", &custom)) {
        if (custom) ensureUsageLayout(style, l);
        else style.remove("layout");
        changed = true;
        style = l["style"].as<JsonObject>();
      }
      if (custom) {
        if (style["layout"].isNull()) ensureUsageLayout(style, l);
        JsonObject layout = style["layout"].as<JsonObject>();
        if (!layout.isNull()) {
          changed |= usageBoxEditor(layout["name"].as<JsonObject>(), "Name box");
          changed |= usageBoxEditor(layout["reset"].as<JsonObject>(), "Reset box");
          if (usageLayoutRow < 0) usageLayoutRow = 0;
          if (usageLayoutRow > 4) usageLayoutRow = 4;
          ImGui::TextDisabled("Metric row");
          ImGui::SetNextItemWidth(-1);
          ImGui::Combo("##layout-row", &usageLayoutRow, [](void*, int i, const char** out) {
            static char label[24];
            snprintf(label, sizeof(label), "Row %d", i + 1);
            *out = label;
            return true;
          }, nullptr, 5);
          if (usageSelectedKind) usageSelectedRow = usageLayoutRow;
          JsonArray rows = layout["rows"].as<JsonArray>();
          if (rows.isNull()) rows = layout["rows"].to<JsonArray>();
          while ((int)rows.size() <= usageLayoutRow) {
            JsonObject row = rows.add<JsonObject>();
            setUsageBox(row["label"].to<JsonObject>(), 12, 20, 100, 12);
            setUsageBox(row["value"].to<JsonObject>(), 140, 18, 72, 18);
            setUsageBox(row["bar"].to<JsonObject>(), 12, 30, 200, 4);
          }
          JsonObject row = rows[usageLayoutRow];
          changed |= usageBoxEditor(row["label"].as<JsonObject>(), "Label box");
          changed |= usageBoxEditor(row["value"].as<JsonObject>(), "Value box");
          changed |= usageBoxEditor(row["bar"].as<JsonObject>(), "Bar box");
        }
      }
      if (ImGui::Button("Reset usage style", ImVec2(-1, 0))) {
        l.remove("style");
        changed = true;
        labelKey.clear();
      }
    }
  }

  void layerProperties() {
    JsonArray draw = view["draw"].as<JsonArray>();
    if (selection.size() > 1) {
      ImGui::Text("%d components selected", (int)selection.size());
      ImGui::TextDisabled("Drag to move them together.");
      if (ImGui::Button("Group selected", ImVec2(-1, 0))) groupSelected();
      if (!selectedGroupId().empty() && ImGui::Button("Ungroup", ImVec2(-1, 0))) ungroupSelected();
      if (ImGui::Button("Duplicate", ImVec2(-1, 0))) duplicateSelected();
      if (ImGui::Button("Delete", ImVec2(-1, 0))) deleteSelected();
      if (theme::secondaryButton("Distribute horizontally", ImVec2(-1, 0))) distributeSelected(true);
      if (theme::secondaryButton("Distribute vertically", ImVec2(-1, 0))) distributeSelected(false);
      if (theme::secondaryButton("Bring forward", ImVec2(-1, 0))) moveSelectedZ(1);
      if (theme::secondaryButton("Send backward", ImVec2(-1, 0))) moveSelectedZ(-1);
      return;
    }
    if (selected < 0 || selected >= (int)draw.size()) {
      ImGui::TextDisabled("Select a component on the canvas or in the list.");
      ImGui::Spacing();
      ImGui::SeparatorText("View defaults");
      JsonObject vo = view.as<JsonObject>();
      color_edit_json(vo, "bg", "Background", "#0b1420");
      input_int_json(vo, "pr", "Priority", 0, 255, 100);
      input_int_json(vo, "ttl", "TTL (seconds)", 0, 86400, 15);
      ImGui::TextDisabled("Components: %d", (int)draw.size());
      ImGui::Spacing();
      ImGui::TextWrapped("Drag a component to move it. Drag the bottom-right handle to resize.");
      return;
    }
    JsonObject l = draw[selected];
    const char* t = l["t"] | "";
    bool changed = false;
    ImGui::Text("Type: %s", t);
    changed |= input_text_json(l, "name", "Component name");
    changed |= input_int_json(l, "z", "Z order", -32768, 32767, componentZ(l, selected));
    const std::string cid = componentId(l);
    if (!cid.empty()) ImGui::TextDisabled("ID: %s", cid.c_str());
    bool hidden = l["hidden"] | false;
    if (ImGui::Checkbox("Hidden", &hidden)) { l["hidden"] = hidden; changed = true; }
    ImGui::SameLine();
    bool locked = l["locked"] | false;
    if (ImGui::Checkbox("Locked", &locked)) { l["locked"] = locked; changed = true; }
    changed |= input_int_json(l, "x", "X (px)", -512, 512, 0);
    changed |= input_int_json(l, "y", "Y (px)", -512, 512, 0);
    changed |= color_edit_json(l, "c", "Colour", "#ffffff");
    if (ImGui::Button("Delete")) { deleteSelected(); return; }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate")) { duplicateSelected(); return; }
    ImGui::Separator();
    if (!strcmp(t, "text")) {
      changed |= input_text_json(l, "v", "Text", false, "Tokens: {HH} {MM} {SS} {DD} {YYYY} {MON} {WD}");
      changed |= input_px_size(l, "sizePx", "s", "Text height (px)", 8, 8, 48, 6, 1);
      static const char* aligns[] = {"l", "c", "r"};
      const char* cur = l["a"] | "l";
      int ai = !strcmp(cur, "c") ? 1 : !strcmp(cur, "r") ? 2 : 0;
      if (ImGui::Combo("Align", &ai, aligns, 3)) { l["a"] = aligns[ai]; changed = true; }
      changed |= input_int_json(l, "w", "Box width (px, 0 auto)", 0, 240, 0);
      changed |= input_int_json(l, "h", "Box height (px, 0 auto)", 0, 240, 0);
      changed |= input_int_json(l, "padX", "Padding X (px)", 0, 60, 0);
      changed |= input_int_json(l, "padY", "Padding Y (px)", 0, 60, 0);
      changed |= input_int_json(l, "mx", "Margin X (px)", -128, 127, 0);
      changed |= input_int_json(l, "my", "Margin Y (px)", -128, 127, 0);
      changed |= input_int_json(l, "lineGap", "Line gap (px)", 0, 40, 0);
      static const char* valigns[] = {"t", "m", "b"};
      const char* cv = l["va"] | "m";
      int vi = !strcmp(cv, "t") ? 0 : !strcmp(cv, "b") ? 2 : 1;
      if (ImGui::Combo("Vertical align", &vi, valigns, 3)) { l["va"] = valigns[vi]; changed = true; }
    } else if (!strcmp(t, "rect") || !strcmp(t, "rrect") || !strcmp(t, "usage")) {
      changed |= input_int_json(l, "w", "Width (px)", 1, 480, 10);
      changed |= input_int_json(l, "h", "Height (px)", 1, 480, 10);
      if (!strcmp(t, "rrect")) changed |= input_int_json(l, "r", "Radius (px)", 0, 255, 0);
      if (!strcmp(t, "usage")) {
        static const char* providers[] = {"claude", "chatgpt", "deepseek"};
        const char* cur = l["provider"] | "claude";
        int pi = !strcmp(cur, "chatgpt") ? 1 : !strcmp(cur, "deepseek") ? 2 : 0;
        if (ImGui::Combo("Provider", &pi, providers, 3)) { l["provider"] = providers[pi]; changed = true; }
        changed |= input_int_json(l, "metric", "Metric (-1 all)", -1, 32, -1);
        usageStyleEditor(l, changed);
        draw = view["draw"].as<JsonArray>();
        if (selected >= 0 && selected < (int)draw.size()) l = draw[selected];
      }
    } else if (!strcmp(t, "circle")) {
      changed |= input_int_json(l, "r", "Radius (px)", 1, 255, 20);
    } else if (!strcmp(t, "line")) {
      changed |= input_int_json(l, "x2", "X2 (px)", -512, 512, 0);
      changed |= input_int_json(l, "y2", "Y2 (px)", -512, 512, 0);
    } else if (!strcmp(t, "icon")) {
      static const char* icons[] = {
        "alert", "arrow-down", "arrow-up", "battery", "check", "cloud",
        "door-closed", "door-open", "home", "humidity", "motion", "plug",
        "rain", "snow", "sun", "thermometer", "wind", "window-closed",
        "window-open", "x"};
      const char* cur = l["v"] | "sun";
      int ii = 0;
      for (int i = 0; i < (int)(sizeof(icons) / sizeof(icons[0])); i++)
        if (!strcmp(cur, icons[i])) { ii = i; break; }
      if (ImGui::Combo("Icon", &ii, icons, (int)(sizeof(icons) / sizeof(icons[0])))) {
        l["v"] = icons[ii]; changed = true;
      }
      changed |= input_px_size(l, "sizePx", "s", "Icon size (px)", 24, 24, 192, 8, 1);
      static const char* aligns[] = {"l", "c", "r"};
      const char* a = l["a"] | "l";
      int ai = !strcmp(a, "c") ? 1 : !strcmp(a, "r") ? 2 : 0;
      if (ImGui::Combo("Align", &ai, aligns, 3)) { l["a"] = aligns[ai]; changed = true; }
    } else if (!strcmp(t, "image")) {
      ImGui::TextDisabled("Asset: %s", (const char*)(l["src"] | "-"));
      changed |= input_int_json(l, "displayW", "Display width (px)", 1, 480,
                                (int)(l["w"] | 0) * std::max(1, (int)(l["s"] | 1)));
      changed |= input_int_json(l, "displayH", "Display height (px)", 1, 480,
                                (int)(l["h"] | 0) * std::max(1, (int)(l["s"] | 1)));
      if (changed) syncDisplayScale(l);
      if (theme::secondaryButton("Replace image...", ImVec2(-1, 0)))
        dialog.openOpen("Replace image or GIF", ".png,.jpg,.jpeg,.gif,.bmp",
                        [this](const std::string& p) { replaceSelectedImage(p); });
      if (theme::secondaryButton("Use as background", ImVec2(-1, 0))) {
        pushUndo();
        view["bgImage"] = l["src"];
        changed = true;
      }
      const char* bg = view["bgImage"] | "";
      if (!strcmp(bg, l["src"] | "")) ImGui::TextDisabled("This asset is the background.");
    } else if (!strcmp(t, "bitmap")) {
      changed |= input_int_json(l, "w", "Source width (px)", 1, 48, 8);
      changed |= input_int_json(l, "h", "Source height (px)", 1, 48, 8);
      changed |= input_int_json(l, "displayW", "Display width (px)", 1, 480,
                                (int)(l["w"] | 8) * std::max(1, (int)(l["s"] | 1)));
      changed |= input_int_json(l, "displayH", "Display height (px)", 1, 480,
                                (int)(l["h"] | 8) * std::max(1, (int)(l["s"] | 1)));
      if (changed) syncDisplayScale(l);
      changed |= input_text_json(l, "d", "Hex data");
      if (theme::secondaryButton("Import image/GIF...", ImVec2(-1, 0)))
        dialog.openOpen("Import image or GIF", ".png,.jpg,.jpeg,.gif,.bmp",
                        [this](const std::string& p) { convertSelectedBitmapToImage(p); });
      bitmapGrid(l);
    } else if (!strcmp(t, "fill")) {
      ImGui::TextDisabled("Background fill");
    }
    ImGui::SeparatorText(usageSelectedKind ? "Align in component" : "Align on canvas");
    if (ImGui::Button("Left")) alignSelected(l, "left");
    ImGui::SameLine();
    if (ImGui::Button("Centre")) alignSelected(l, "hcenter");
    ImGui::SameLine();
    if (ImGui::Button("Right")) alignSelected(l, "right");
    if (ImGui::Button("Top")) alignSelected(l, "top");
    ImGui::SameLine();
    if (ImGui::Button("Middle")) alignSelected(l, "vcenter");
    ImGui::SameLine();
    if (ImGui::Button("Bottom")) alignSelected(l, "bottom");
    if (changed) {
      viewDirty = true;
      previewDirty = true;
      syncJson();
    }
  }

  void editorToolbar() {
    if (theme::secondaryButton("Components")) {
      componentsDrawerOpen = !componentsDrawerOpen;
      if (componentsDrawerOpen) documentDrawerOpen = false;
    }
    ImGui::SameLine();
    if (theme::secondaryButton("Document")) {
      documentDrawerOpen = !documentDrawerOpen;
      if (documentDrawerOpen) componentsDrawerOpen = false;
    }
    ImGui::SameLine();
    if (!selection.empty()) {
      if (theme::secondaryButton(inspectorDismissed ? "Show Inspector" : "Inspector"))
        inspectorDismissed = !inspectorDismissed;
    }
    ImGui::NewLine();
    if (theme::secondaryButton("Undo")) undo();
    ImGui::SameLine();
    if (theme::secondaryButton("Redo")) redo();
    ImGui::SameLine();
    if (theme::secondaryButton("Copy")) copySelected();
    ImGui::SameLine();
    if (theme::secondaryButton("Paste")) pasteComponents();
    ImGui::SameLine();
    if (theme::secondaryButton("Duplicate")) duplicateSelected();
    ImGui::SameLine();
    if (theme::secondaryButton("Delete")) deleteSelected();
    ImGui::SameLine();
    if (theme::secondaryButton("Group")) groupSelected();
    ImGui::SameLine();
    if (theme::secondaryButton("Ungroup")) ungroupSelected();
    ImGui::NewLine();
    ImGui::Checkbox("Snap", &snapEnabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::InputInt("##snapgrid", &snapGrid);
    if (snapGrid < 1) snapGrid = 1;
    if (snapGrid > 40) snapGrid = 40;
    ImGui::SameLine();
    ImGui::Checkbox("Guides", &showGuides);
    ImGui::SameLine();
    ImGui::Checkbox("Rulers", &showRulers);
    ImGui::SameLine();
    ImGui::TextDisabled("Usage preview");
    ImGui::SameLine();
    static const char* previewModes[] = {"Live", "Waiting", "Error", "Long values"};
    int previewMode = usagePreviewMode;
    ImGui::SetNextItemWidth(116);
    if (ImGui::Combo("##usage-preview", &previewMode, previewModes, 4))
      setUsagePreviewMode(previewMode);
  }

  void editorLayerPane() {
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##addcomponent", "Add component...")) {
      const char* types[] = {"text", "rect", "rrect", "circle", "line", "icon",
                             "bitmap", "image", "usage", "fill"};
      for (const char* t : types) {
        if (ImGui::Selectable(t)) {
          if (!strcmp(t, "image")) addImageLayer();
          else addLayer(t);
        }
      }
      ImGui::Separator();
      if (ImGui::Selectable("Template: clock")) addTemplate("clock");
      if (ImGui::Selectable("Template: usage stack")) addTemplate("usage");
      ImGui::EndCombo();
    }
    JsonArray groups = view["groups"].as<JsonArray>();
    if (groups.size() > 0) {
      ImGui::SeparatorText("Groups");
      for (JsonObject group : groups) {
        const std::string gid = group["id"] | "";
        if (gid.empty()) continue;
        const bool selectedGroup = selectedGroupId() == gid;
        char label[96];
        snprintf(label, sizeof(label), "group  %s  (%d)", (const char*)(group["name"] | gid.c_str()),
                 (int)(group["members"].as<JsonArray>().size()));
        ImGui::PushID(gid.c_str());
        if (ImGui::Selectable(label, selectedGroup, 0,
                              ImVec2(std::max(55.0f, ImGui::GetContentRegionAvail().x - 62), 0)))
          selectGroup(gid);
        ImGui::SameLine();
        if (ImGui::SmallButton((group["hidden"] | false) ? "S" : "H")) {
          pushUndo();
          setGroupHidden(gid, !(group["hidden"] | false));
          viewDirty = true;
          previewDirty = true;
          syncJson();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hide or show group");
        ImGui::SameLine();
        if (ImGui::SmallButton((group["locked"] | false) ? "U" : "L")) {
          pushUndo();
          setGroupLocked(gid, !(group["locked"] | false));
          viewDirty = true;
          previewDirty = true;
          syncJson();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lock or unlock group");
        ImGui::PopID();
      }
    }
    ImGui::SeparatorText("Component stack");
    ImGui::BeginChild("##layers", ImVec2(0, -112), true);
    JsonArray draw = view["draw"].as<JsonArray>();
    for (int i : componentsByZ(true)) {
      JsonObject l = draw[i];
      char label[220];
      const char* t = l["t"] | "?";
      const char* name = l["name"] | "";
      std::string detail = name;
      if (detail.empty()) detail = (const char*)(l["v"] | "");
      if (detail.empty()) detail = (const char*)(l["provider"] | "");
      if (detail.empty()) detail = (const char*)(l["src"] | "");
      const int z = componentZ(l, i);
      snprintf(label, sizeof(label), "%03d  %s  %s%s%s", z, t, detail.c_str(),
               (l["hidden"] | false) ? "  [hidden]" : "",
               (l["locked"] | false) ? "  [locked]" : "");
      if (ImGui::Selectable(label, selection.count(i), 0, ImVec2(0, 26))) {
        selectComponent(i, ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl);
      }
    }
    ImGui::EndChild();
    const float half = std::max(54.0f, ImGui::GetContentRegionAvail().x / 2 - 3);
    if (theme::secondaryButton("Up", ImVec2(half, 0))) moveLayer(1);
    ImGui::SameLine();
    if (theme::secondaryButton("Down", ImVec2(half, 0))) moveLayer(-1);
    ImGui::NewLine();
    if (theme::secondaryButton("Front", ImVec2(half, 0))) moveSelectedZ(1, true);
    ImGui::SameLine();
    if (theme::secondaryButton("Back", ImVec2(half, 0))) moveSelectedZ(-1, true);
    ImGui::NewLine();
    if (theme::secondaryButton("Group", ImVec2(half, 0))) groupSelected();
    ImGui::SameLine();
    if (theme::secondaryButton("Ungroup", ImVec2(half, 0))) ungroupSelected();
  }

  void documentDrawerContent() {
    JsonObject vo = view.as<JsonObject>();
    bool changed = false;
    changed |= color_edit_json(vo, "bg", "Background colour", "#0b1420");
    changed |= input_int_json(vo, "priority", "Priority", 0, 255, 0);
    changed |= input_int_json(vo, "ttl", "TTL (seconds)", 0, 86400, 15);
    if (theme::secondaryButton("Background image...", ImVec2(-1, 0)))
      dialog.openOpen("Background image or GIF", ".png,.jpg,.jpeg,.gif,.bmp",
                      [this](const std::string& p) { importBackground(p); });
    const char* bg = view["bgImage"] | "";
    if (bg[0]) {
      ImGui::TextDisabled("Background: %s", bg);
      if (theme::secondaryButton("Clear background", ImVec2(-1, 0))) {
        view.remove("bgImage");
        changed = true;
      }
    }
    if (changed) markViewChanged();
    ImGui::Separator();
    editorActions();
  }

  void editorActions() {
    if (theme::secondaryButton("Load file", ImVec2(-1, 0))) {
      dialog.openOpen("Load dashboard", ".json", [this](const std::string& p) { loadViewFile(p); });
    }
    if (theme::secondaryButton("Save file", ImVec2(-1, 0))) {
      dialog.openSave("Save dashboard", ".json", "view.json", [this](const std::string& p) {
        if (write_file(p, serializeView())) notify("Saved " + p);
        else notify("Cannot write " + p);
      });
    }
    if (theme::secondaryButton("Export bundle...", ImVec2(-1, 0))) {
      dialog.openSave("Export dashboard bundle", ".smalltv", "dashboard.smalltv",
                      [this](const std::string& p) { exportDashboardBundle(p); });
    }
    if (theme::secondaryButton("Import bundle...", ImVec2(-1, 0))) {
      dialog.openOpen("Import dashboard bundle", ".smalltv",
                      [this](const std::string& p) { importDashboardBundle(p); });
    }
    if (theme::secondaryButton("Load device", ImVec2(-1, 0))) loadViewFromDevice();
    if (theme::secondaryButton("Save device", ImVec2(-1, 0))) saveViewToDevice();
    if (theme::secondaryButton("Send overlay", ImVec2(-1, 0))) sendOverlay(false);
    if (theme::secondaryButton("Clear overlay", ImVec2(-1, 0))) sendOverlay(true);
    if (theme::secondaryButton("Fetch usage", ImVec2(-1, 0))) loadUsage();
    if (theme::secondaryButton("Manage device images...", ImVec2(-1, 0))) openImageManager();
    if (std::filesystem::exists(autosavePath()) &&
        theme::secondaryButton("Recover autosave", ImVec2(-1, 0)))
      loadViewFile(autosavePath());
    if (theme::secondaryButton("Copy JSON", ImVec2(-1, 0))) {
      std::string s = serializeView();
      ImGui::SetClipboardText(s.c_str());
      notify("Dashboard JSON copied");
    }
    if (theme::secondaryButton(showJsonEditor ? "Hide JSON" : "Show JSON", ImVec2(-1, 0)))
      showJsonEditor = !showJsonEditor;
    if (showJsonEditor) {
      ImGui::InputTextMultiline("##viewjson", viewJsonBuf, sizeof(viewJsonBuf), ImVec2(-1, 220));
      if (theme::primaryButton("Apply JSON")) {
        std::string err;
        if (loadViewText(viewJsonBuf, err)) notify("JSON applied");
        else notify("JSON error: " + err);
      }
    }
  }

  JsonObject usageLayoutFor(JsonObject l) {
    if (!l || strcmp(l["t"] | "", "usage")) return JsonObject();
    JsonObject style = l["style"].as<JsonObject>();
    if (style.isNull()) return JsonObject();
    JsonObject layout = style["layout"].as<JsonObject>();
    if (layout.isNull() || (layout["auto"] | false)) return JsonObject();
    return layout;
  }

  JsonObject usageLayoutBox(int kind, int row) {
    JsonArray draw = view["draw"].as<JsonArray>();
    if (selected < 0 || selected >= (int)draw.size()) return JsonObject();
    JsonObject layout = usageLayoutFor(draw[selected]);
    if (layout.isNull()) return JsonObject();
    if (kind == 1) return layout["name"].as<JsonObject>();
    if (kind == 2) return layout["reset"].as<JsonObject>();
    JsonArray rows = layout["rows"].as<JsonArray>();
    if (row < 0 || row >= (int)rows.size()) return JsonObject();
    JsonObject item = rows[row];
    if (kind == 3) return item["label"].as<JsonObject>();
    if (kind == 4) return item["value"].as<JsonObject>();
    if (kind == 5) return item["bar"].as<JsonObject>();
    return JsonObject();
  }

  bool usageBoxBounds(JsonObject box, float cardX, float cardY, float& x, float& y,
                      float& w, float& h) {
    if (box.isNull()) return false;
    w = box["w"] | 0;
    h = box["h"] | 0;
    if (w <= 0 || h <= 0) return false;
    x = cardX + (box["x"] | 0) + (box["mx"] | 0);
    y = cardY + (box["y"] | 0) + (box["my"] | 0);
    return true;
  }

  std::set<int> effectiveSelection() {
    std::set<int> result = selection;
    if (result.size() == 1 && selected >= 0) {
      JsonArray draw = view["draw"].as<JsonArray>();
      if (selected < (int)draw.size()) {
        const std::string gid = draw[selected]["gid"] | "";
        if (!gid.empty()) {
          for (int i = 0; i < (int)draw.size(); i++)
            if (gid == (const char*)(draw[i]["gid"] | "")) result.insert(i);
        }
      }
    }
    return result;
  }

  void snapPosition(float& x, float& y, float w, float h, const std::set<int>& moving) {
    snapGuideX = snapGuideY = -1.0f;
    if (!snapEnabled) return;
    JsonArray draw = view["draw"].as<JsonArray>();
    std::vector<float> xs = {0, 120, 240};
    std::vector<float> ys = {0, 120, 240};
    for (int i = 0; i < (int)draw.size(); i++) {
      if (moving.count(i) || (draw[i]["hidden"] | false)) continue;
      float bx, by, bw, bh;
      layer_bounds(draw[i], bx, by, bw, bh);
      xs.push_back(bx); xs.push_back(bx + bw / 2); xs.push_back(bx + bw);
      ys.push_back(by); ys.push_back(by + bh / 2); ys.push_back(by + bh);
    }
    auto snapAxis = [&](float value, float size, const std::vector<float>& lines,
                        float& guide) {
      float best = 4.0f;
      float adjusted = value;
      for (float edge : {value, value + size / 2, value + size}) {
        for (float line : lines) {
          const float delta = line - edge;
          if (std::abs(delta) < best) {
            best = std::abs(delta);
            adjusted = value + delta;
            guide = line;
          }
        }
        const float grid = std::round(edge / snapGrid) * snapGrid;
        if (std::abs(grid - edge) < best) {
          best = std::abs(grid - edge);
          adjusted = value + grid - edge;
          guide = grid;
        }
      }
      return adjusted;
    };
    x = snapAxis(x, w, xs, snapGuideX);
    y = snapAxis(y, h, ys, snapGuideY);
  }

  void editorCanvas(float side) {
    const float S = side / 240.0f;
    if (previewDirty) refreshPreview();
    ImVec2 imgPos = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(intptr_t)g_texture, ImVec2(side, side));
    ImGui::SetCursorScreenPos(imgPos);
    ImGui::InvisibleButton("##canvas", ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImVec2 mouse = ImGui::GetIO().MousePos;
    const float mx = (mouse.x - imgPos.x) / S;
    const float my = (mouse.y - imgPos.y) / S;

    JsonArray draw = view["draw"].as<JsonArray>();
    static int dragMode = 0;
    static bool undoPushed = false;
    static bool dragThresholdMet = false;
    static bool boxSelecting = false;
    static ImVec2 boxStart{};
    static ImVec2 dragOrigin{};
    static int dragSubKind = 0;
    static int dragSubRow = 0;
    static int dragSubX = 0, dragSubY = 0;
    static int dragPrimaryX = 0, dragPrimaryY = 0;
    static int dragPrimaryW = 0, dragPrimaryH = 0;
    static std::vector<int> dragIndices;
    static std::vector<int> dragStartX;
    static std::vector<int> dragStartY;
    if (ImGui::IsItemClicked(0)) {
      const bool additive = ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl;
      const bool alt = ImGui::GetIO().KeyAlt;
      bool usageHit = false;
      if (!alt && selected >= 0 && selected < (int)draw.size()) {
        JsonObject layout = usageLayoutFor(draw[selected]);
        if (!layout.isNull()) {
          float cardX = draw[selected]["x"] | 0, cardY = draw[selected]["y"] | 0;
          auto tryBox = [&](int kind, int row) {
            JsonObject box = usageLayoutBox(kind, row);
            float bx, by, bw, bh;
            if (!usageBoxBounds(box, cardX, cardY, bx, by, bw, bh)) return false;
            if (mx < bx || mx > bx + bw || my < by || my > by + bh) return false;
            usageSelectedKind = kind;
            usageSelectedRow = usageLayoutRow = row;
            usageDragKind = dragSubKind = kind;
            usageDragRow = dragSubRow = row;
            dragSubX = box["x"] | 0;
            dragSubY = box["y"] | 0;
            dragMode = 3;
            dragOrigin = ImVec2(mx, my);
            undoPushed = false;
            dragThresholdMet = false;
            usageHit = true;
            return true;
          };
          if (!tryBox(1, 0) && !tryBox(2, 0)) {
            for (int row = 0; row < (int)layout["rows"].as<JsonArray>().size() && !usageHit; row++) {
              tryBox(3, row) || tryBox(4, row) || tryBox(5, row);
            }
          }
        }
      }
      bool onHandle = false;
      if (selected >= 0 && selected < (int)draw.size()) {
        float x, y, w, h;
        layer_bounds(draw[selected], x, y, w, h);
        onHandle = mx >= x + w - 10 && mx <= x + w + 10 &&
                   my >= y + h - 10 && my <= y + h + 10;
      }
      if (!usageHit && !onHandle) {
        int hit = -1;
        for (int i : componentsByZ(true)) {
          if (i < 0 || i >= (int)draw.size() || (draw[i]["hidden"] | false)) continue;
          float x, y, w, h;
          layer_bounds(draw[i], x, y, w, h);
          if (mx >= x && mx <= x + w && my >= y && my <= y + h) { hit = i; break; }
        }
        if (hit >= 0) {
          usageSelectedKind = 0;
          if (additive) selectComponent(hit, true);
          else {
            selection.clear();
            selection.insert(hit);
            selected = hit;
          }
        } else {
          if (!additive) clearSelection();
          boxSelecting = true;
          boxStart = ImVec2(mx, my);
        }
      }
      if (!usageHit && selected >= 0 && selected < (int)draw.size()) {
        usageSelectedKind = 0;
        const bool locked = draw[selected]["locked"] | false;
        dragMode = locked ? 0 : (onHandle && selection.size() == 1 ? 2 : 1);
        dragOrigin = ImVec2(mx, my);
        undoPushed = false;
        dragThresholdMet = false;
        dragPrimaryX = draw[selected]["x"] | 0;
        dragPrimaryY = draw[selected]["y"] | 0;
        dragPrimaryW = draw[selected]["w"] | 0;
        dragPrimaryH = draw[selected]["h"] | 0;
        if (!strcmp(draw[selected]["t"] | "", "circle")) {
          dragPrimaryW = dragPrimaryH = 2 * (draw[selected]["r"] | 0);
        }
        dragIndices.clear();
        dragStartX.clear();
        dragStartY.clear();
        if (dragMode == 1) {
          for (int i : effectiveSelection()) {
            if (i < 0 || i >= (int)draw.size()) continue;
            dragIndices.push_back(i);
            dragStartX.push_back(draw[i]["x"] | 0);
            dragStartY.push_back(draw[i]["y"] | 0);
          }
        }
        snapGuideX = snapGuideY = -1;
      }
    }
    if (active && ImGui::IsMouseDragging(0) && selected >= 0 &&
        selected < (int)draw.size() && dragMode) {
      float dx = mx - dragOrigin.x, dy = my - dragOrigin.y;
      if (!dragThresholdMet && std::abs(dx) < 3.0f && std::abs(dy) < 3.0f) {
        // Do not move on a tiny accidental drag.
      } else {
        dragThresholdMet = true;
        if (!undoPushed) { pushUndo(); undoPushed = true; }
        if (dragMode == 3) {
          JsonObject box = usageLayoutBox(dragSubKind, dragSubRow);
          if (!box.isNull()) {
            box["x"] = dragSubX + (int)std::round(dx);
            box["y"] = dragSubY + (int)std::round(dy);
          }
        } else if (dragMode == 2) {
          JsonObject primary = draw[selected];
          const char* t = primary["t"] | "";
          if (!strcmp(t, "circle")) {
            int r = std::max(1, dragPrimaryW / 2 + (int)std::max(dx, dy));
            primary["r"] = std::min(255, r);
          } else {
            primary["w"] = std::max(1, dragPrimaryW + (int)std::round(dx));
            primary["h"] = std::max(1, dragPrimaryH + (int)std::round(dy));
          }
        } else {
          std::set<int> moving(dragIndices.begin(), dragIndices.end());
          float sx = (float)dragPrimaryX + dx;
          float sy = (float)dragPrimaryY + dy;
          snapPosition(sx, sy, (float)dragPrimaryW, (float)dragPrimaryH, moving);
          const int finalDx = (int)std::round(sx - dragPrimaryX);
          const int finalDy = (int)std::round(sy - dragPrimaryY);
          for (size_t k = 0; k < dragIndices.size(); k++) {
            const int i = dragIndices[k];
            if (i < 0 || i >= (int)draw.size() || (draw[i]["locked"] | false)) continue;
            draw[i]["x"] = dragStartX[k] + finalDx;
            draw[i]["y"] = dragStartY[k] + finalDy;
          }
        }
        viewDirty = true;
        previewDirty = true;
        syncJson();
      }
    }
    if (boxSelecting && active && ImGui::IsMouseDragging(0)) {
      ImDrawList* boxDraw = ImGui::GetWindowDrawList();
      const ImVec2 a(imgPos.x + std::min(boxStart.x, mx) * S,
                     imgPos.y + std::min(boxStart.y, my) * S);
      const ImVec2 b(imgPos.x + std::max(boxStart.x, mx) * S,
                     imgPos.y + std::max(boxStart.y, my) * S);
      boxDraw->AddRectFilled(a, b, IM_COL32(70, 140, 230, 36));
      boxDraw->AddRect(a, b, IM_COL32(90, 170, 255, 180));
    }
    if (ImGui::IsItemDeactivated()) {
      if (boxSelecting) {
        const float x0 = std::min(boxStart.x, mx), x1 = std::max(boxStart.x, mx);
        const float y0 = std::min(boxStart.y, my), y1 = std::max(boxStart.y, my);
        if (x1 - x0 > 2 || y1 - y0 > 2) {
          for (int i : componentsByZ(true)) {
            if (i < 0 || i >= (int)draw.size() || (draw[i]["hidden"] | false)) continue;
            float x, y, w, h;
            layer_bounds(draw[i], x, y, w, h);
            if (x + w >= x0 && x <= x1 && y + h >= y0 && y <= y1) selection.insert(i);
          }
          if (!selection.empty()) selected = *selection.begin();
        }
        boxSelecting = false;
      }
      dragMode = 0;
      usageDragKind = 0;
      snapGuideX = snapGuideY = -1;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (showRulers) {
      const ImU32 tick = IM_COL32(120, 135, 155, 110);
      for (int v = 0; v <= 240; v += 20) {
        const float px = imgPos.x + v * S;
        const float py = imgPos.y + v * S;
        dl->AddLine(ImVec2(px, imgPos.y - 5), ImVec2(px, imgPos.y), tick);
        dl->AddLine(ImVec2(imgPos.x - 5, py), ImVec2(imgPos.x, py), tick);
        if (v % 60 == 0) {
          char label[8];
          snprintf(label, sizeof(label), "%d", v);
          dl->AddText(ImVec2(px + 2, imgPos.y - 18), tick, label);
          dl->AddText(ImVec2(imgPos.x - 28, py + 2), tick, label);
        }
      }
    }
    if (showGuides) {
      const ImU32 guide = IM_COL32(90, 135, 210, 70);
      dl->AddLine(ImVec2(imgPos.x + 120 * S, imgPos.y), ImVec2(imgPos.x + 120 * S, imgPos.y + side), guide);
      dl->AddLine(ImVec2(imgPos.x, imgPos.y + 120 * S), ImVec2(imgPos.x + side, imgPos.y + 120 * S), guide);
      if (snapGuideX >= 0)
        dl->AddLine(ImVec2(imgPos.x + snapGuideX * S, imgPos.y),
                    ImVec2(imgPos.x + snapGuideX * S, imgPos.y + side),
                    IM_COL32(90, 210, 140, 180));
      if (snapGuideY >= 0)
        dl->AddLine(ImVec2(imgPos.x, imgPos.y + snapGuideY * S),
                    ImVec2(imgPos.x + side, imgPos.y + snapGuideY * S),
                    IM_COL32(90, 210, 140, 180));
    }

    const std::set<int> highlighted = effectiveSelection();
    for (int i : highlighted) {
      if (i < 0 || i >= (int)draw.size()) continue;
      float x, y, w, h;
      layer_bounds(draw[i], x, y, w, h);
      ImVec2 a(imgPos.x + x * S, imgPos.y + y * S);
      ImVec2 b(imgPos.x + (x + w) * S, imgPos.y + (y + h) * S);
      dl->AddRect(a, b, IM_COL32(255, 200, 60, 255), 0, 0, 2.0f);
      if (i == selected)
        dl->AddRectFilled(ImVec2(b.x - 5, b.y - 5), ImVec2(b.x + 5, b.y + 5),
                          IM_COL32(255, 200, 60, 255));
    }
    if (selected >= 0 && selected < (int)draw.size()) {
      JsonObject layout = usageLayoutFor(draw[selected]);
      if (!layout.isNull()) {
        float cardX = draw[selected]["x"] | 0, cardY = draw[selected]["y"] | 0;
        static const int nameKinds[] = {1, 2, 3, 4, 5};
        static const int rowKinds[] = {3, 4, 5};
        for (int row = 0; row < 5; row++) {
          const int* kinds = row == 0 ? nameKinds : rowKinds;
          const int count = row == 0 ? 5 : 3;
          for (int k = 0; k < count; k++) {
            const int kind = kinds[k];
            JsonObject box = usageLayoutBox(kind, row);
            float bx, by, bw, bh;
            if (!usageBoxBounds(box, cardX, cardY, bx, by, bw, bh)) continue;
            ImVec2 a(imgPos.x + bx * S, imgPos.y + by * S);
            ImVec2 b(imgPos.x + (bx + bw) * S, imgPos.y + (by + bh) * S);
            const bool active = kind == usageSelectedKind && row == usageSelectedRow;
            dl->AddRect(a, b, active ? IM_COL32(255, 200, 60, 235) : IM_COL32(80, 190, 230, 110),
                        0, 0, active ? 2.0f : 1.0f);
          }
        }
      }
    }
    if (hovered && !active) {
      for (int i : componentsByZ(true)) {
        if (i < 0 || i >= (int)draw.size() || (draw[i]["hidden"] | false)) continue;
        float x, y, w, h;
        layer_bounds(draw[i], x, y, w, h);
        if (mx >= x && mx <= x + w && my >= y && my <= y + h) {
          ImVec2 a(imgPos.x + x * S, imgPos.y + y * S);
          ImVec2 b(imgPos.x + (x + w) * S, imgPos.y + (y + h) * S);
          dl->AddRect(a, b, IM_COL32(120, 170, 255, 160));
          break;
        }
      }
    }
    if (!viewError.empty()) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", viewError.c_str());
  }

  void editor() {
    if (!viewLoaded) initView();
    const std::string signature = selectionSignature();
    if (signature != lastSelectionSignature) {
      lastSelectionSignature = signature;
      if (!signature.empty()) inspectorDismissed = false;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (io.KeyShift) redo(); else undo();
      }
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) copySelected();
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) pasteComponents();
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicateSelected();
      if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) deleteSelected();
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (usageSelectedKind) usageSelectedKind = 0;
        else clearSelection();
      }
      if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) moveSelectedZ(-1);
      if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) moveSelectedZ(1);
      int nx = 0, ny = 0;
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) nx = -1;
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) nx = 1;
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) ny = -1;
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) ny = 1;
      if ((nx || ny) && !selection.empty()) {
        const int step = io.KeyShift ? 10 : 1;
        pushUndo();
        if (usageSelectedKind && selected >= 0) {
          JsonObject box = usageLayoutBox(usageSelectedKind, usageSelectedRow);
          if (!box.isNull()) {
            box["x"] = (int)((box["x"] | 0) + nx * step);
            box["y"] = (int)((box["y"] | 0) + ny * step);
          }
        } else {
          JsonArray draw = view["draw"].as<JsonArray>();
          for (int i : effectiveSelection()) {
            if (i < 0 || i >= (int)draw.size() || (draw[i]["locked"] | false)) continue;
            draw[i]["x"] = (draw[i]["x"] | 0) + nx * step;
            draw[i]["y"] = (draw[i]["y"] | 0) + ny * step;
          }
        }
        markViewChanged();
      }
      if (usageSelectedKind && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        usageSelectedKind = usageSelectedKind >= 5 ? 1 : usageSelectedKind + 1;
      }
    }
    ImGui::BeginChild("##designbody", ImVec2(0, 0), false);
    const ImVec2 bodyPos = ImGui::GetWindowPos();
    const ImVec2 bodySize = ImGui::GetWindowSize();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const bool hasSelection = !selection.empty() && selected >= 0;
    const bool showInspector = hasSelection && !inspectorDismissed;
    const float leftWidth = documentDrawerOpen ? documentDrawerWidth
                          : componentsDrawerOpen ? componentsDrawerWidth : 0.0f;
    const float rightWidth = showInspector ? inspectorDrawerWidth : 0.0f;
    const float centerW = std::max(240.0f, avail.x - leftWidth - rightWidth);

    if (leftWidth > 0) {
      bool open = documentDrawerOpen ? documentDrawerOpen : componentsDrawerOpen;
      if (beginDrawer("##leftdrawer", leftWidth,
                      documentDrawerOpen ? "DOCUMENT" : "COMPONENTS", &open)) {
        if (documentDrawerOpen) documentDrawerContent();
        else editorLayerPane();
      }
      endDrawer();
      if (!open) {
        documentDrawerOpen = false;
        componentsDrawerOpen = false;
      }
      ImGui::SameLine(0, 0);
    }

    beginPane("##canvaspane", ImVec2(centerW, avail.y), false);
    drawGrid(IM_COL32(42, 47, 54, 65));
    editorToolbar();
    ImGui::Separator();
    ImGui::Spacing();
    const ImVec2 cav = ImGui::GetContentRegionAvail();
    const float side = std::max(120.0f, std::min(560.0f, std::min(cav.x - 4.0f, cav.y - 46.0f)));
    const float cx = ImGui::GetCursorPosX() + (cav.x - side) * 0.5f;
    const float cy = ImGui::GetCursorPosY() + std::max(0.0f, (cav.y - 40.0f - side) * 0.5f);
    ImGui::SetCursorPos(ImVec2(cx, cy));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    theme::panel(ImGui::GetWindowDrawList(), ImVec2(p.x - 8, p.y - 8),
                 ImVec2(p.x + side + 8, p.y + side + 8), theme::surfaceAlt(), theme::border());
    editorCanvas(side);
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 26);
    const char* element = usageSelectedKind == 1 ? "name" :
                          usageSelectedKind == 2 ? "reset" :
                          usageSelectedKind == 3 ? "label" :
                          usageSelectedKind == 4 ? "value" :
                          usageSelectedKind == 5 ? "bar" : "";
    if (element[0])
      ImGui::TextDisabled("240 × 240  ·  %d components  ·  row %d %s  ·  %s",
                          (int)view["draw"].as<JsonArray>().size(),
                          usageSelectedRow + 1, element,
                          viewError.empty() ? "ready" : viewError.c_str());
    else
      ImGui::TextDisabled("240 × 240  ·  %d components  ·  %s",
                          (int)view["draw"].as<JsonArray>().size(),
                          viewError.empty() ? "ready" : viewError.c_str());
    endPane();

    if (showInspector) {
      ImGui::SameLine(0, 0);
      bool open = true;
      beginDrawer("##inspectordrawer", rightWidth, "INSPECTOR", &open);
      layerProperties();
      endDrawer();
      if (!open) inspectorDismissed = true;
    }

    // A click on empty design background clears the selection. Clicks inside
    // the drawers, canvas or toolbar are consumed by their child windows.
    if (ImGui::IsMouseClicked(0) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        !ImGui::IsAnyItemHovered()) {
      clearSelection();
    }
    (void)bodyPos;
    (void)bodySize;
    ImGui::EndChild();
  }

  // ---- connection shell --------------------------------------------------
  void connectionScreen() {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    beginPane("##connhead", ImVec2(avail.x, 64), false);
    ImGui::PushFont(theme::fonts().heading);
    ImGui::TextUnformatted("SmallTV");
    ImGui::PopFont();
    ImGui::SameLine(112);
    ImGui::TextDisabled("Connect to a device");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 150.0f);
    ImGui::TextDisabled("%s", connLabel(connState));
    endPane();
    ImGui::Spacing();

    const float gap = 12.0f;
    const float rightW = std::min(370.0f, std::max(290.0f, avail.x * 0.34f));
    const float leftW = std::max(280.0f, avail.x - rightW - gap);

    beginPane("##conndevices", ImVec2(leftW, avail.y - 76.0f), true);
    paneTitle("DEVICES", "local network");
    ImGui::Spacing();
    scanProgress();
    if (manager.scanning()) {
      if (theme::secondaryButton("Cancel scan", ImVec2(150, 0))) {
        manager.cancelDiscovery();
        scanRequested = false;
        notify("Scan cancelled");
      }
    }
    if (!savedUrl.empty()) {
      ImGui::Spacing();
      theme::sectionLabel("LAST DEVICE");
      ImGui::TextWrapped("%s", savedUrl.c_str());
      if (savedUser[0]) ImGui::TextDisabled("User: %s", savedUser.c_str());
      if (theme::secondaryButton("Use last device", ImVec2(150, 0))) {
        snprintf(deviceUrl, sizeof(deviceUrl), "%s", savedUrl.c_str());
        snprintf(authUser, sizeof(authUser), "%s", savedUser.c_str());
      }
      ImGui::SameLine();
      if (theme::secondaryButton("Forget", ImVec2(100, 0))) forgetConnectionProfile();
      ImGui::Separator();
    }
    ImGui::BeginChild("##conndevicelist", ImVec2(0, -44), true);
    if (devices.empty()) {
      if (manager.scanning()) ImGui::TextDisabled("Scanning the local network...");
      else {
        const ScanState state = manager.scanState();
        if (!state.error.empty()) ImGui::TextWrapped("Scan failed: %s", state.error.c_str());
        else ImGui::TextDisabled("No devices found yet.");
      }
    } else {
      for (int i = 0; i < (int)devices.size(); i++) deviceRow(i);
    }
    ImGui::EndChild();
    scanButton("Scan network", ImVec2(-1, 0));
    endPane();

    ImGui::SameLine(0, gap);
    beginPane("##connform", ImVec2(rightW, avail.y - 76.0f), true);
    paneTitle("CONNECTION", connLabel(connState));
    ImGui::Spacing();
    if (connState == ConnState::Connecting) {
      ImGui::TextDisabled("Trying to reach the device...");
      ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 4), "");
      if (theme::secondaryButton("Cancel", ImVec2(-1, 0))) disconnect();
    } else {
      if (!connError.empty()) ImGui::TextWrapped("%s", connError.c_str());
      if (connState == ConnState::AuthRequired) {
        ImGui::TextWrapped("The device requires web-UI credentials.");
      } else if (connState == ConnState::Offline) {
        ImGui::TextWrapped("The device is unreachable. Check the address and network.");
      }
    }
    if (!deviceSecretError.empty())
      ImGui::TextWrapped("Credential store: %s", deviceSecretError.c_str());
    ImGui::Separator();
    ImGui::TextDisabled("ADDRESS");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##connurl", "http://smalltv.local", deviceUrl, sizeof(deviceUrl));
    ImGui::TextDisabled("WEB UI CREDENTIALS");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##connuser", "user", authUser, sizeof(authUser));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##connpass", "password", authPass, sizeof(authPass),
                             ImGuiInputTextFlags_Password);
    ImGui::Checkbox("Remember password", &rememberPassword);
    ImGui::SameLine();
    ImGui::Checkbox("Connect on launch", &autoConnect);
    ImGui::Spacing();
    if (connState == ConnState::Connecting) ImGui::BeginDisabled();
    if (theme::primaryButton("Connect", ImVec2(-1, 0))) {
      profileDirty = true;
      connect();
    }
    if (connState == ConnState::Connecting) ImGui::EndDisabled();
    if (!savedUrl.empty() && connState != ConnState::Connecting) {
      if (theme::secondaryButton("Forget saved device", ImVec2(-1, 0))) forgetConnectionProfile();
    }
    if (strstr(deviceUrl, "192.168.4.1")) {
      ImGui::Spacing();
      ImGui::TextWrapped("192.168.4.1 is the device setup AP. Join SmallTV-Setup first, then configure home WiFi.");
    }
    ImGui::Spacing();
    ImGui::Separator();
    if (theme::secondaryButton("Continue offline", ImVec2(-1, 0))) {
      disconnect();
      appMode = AppMode::Offline;
      tab = 1;
        disconnectPrompt = false;
    }
    ImGui::TextDisabled("Design and Usage work offline; Device and Settings require a connection.");
    endPane();
  }

  void disconnectModal() {
    if (disconnectPrompt) {
      ImGui::OpenPopup("Leave device");
      disconnectPrompt = false;
    }
    if (ImGui::BeginPopupModal("Leave device", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextWrapped("Leave this device? Unsaved dashboard changes will be kept in the editor.");
      if (ImGui::Button("Leave")) {
        ImGui::CloseCurrentPopup();
        disconnect();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
  }

  // ---- root --------------------------------------------------------------
  void deviceRow(int i) {
    const EmuDevice& d = devices[i];
    ImGui::PushID(i);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("##row", ImVec2(w, 48));
    const bool hovered = ImGui::IsItemHovered();
    const bool selected = selectedDevice == i;
    if (ImGui::IsItemClicked()) {
      selectedDevice = i;
      snprintf(deviceUrl, sizeof(deviceUrl), "http://%s", d.ip.c_str());
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected || hovered) {
      dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 44),
                        selected ? theme::surfaceAlt() : IM_COL32(28, 32, 38, 140), 5.0f);
      if (selected) dl->AddRectFilled(p, ImVec2(p.x + 3, p.y + 44), theme::accent(), 2.0f);
    }
    dl->AddCircleFilled(ImVec2(p.x + 12, p.y + 14), 3.5f,
                        online() && selected ? theme::success() : theme::muted());
    dl->AddText(ImVec2(p.x + 22, p.y + 7), theme::text(), d.name.c_str());
    char sub[160];
    snprintf(sub, sizeof(sub), "%s  ·  %s", d.ip.c_str(), d.version.c_str());
    dl->AddText(ImVec2(p.x + 22, p.y + 25), theme::muted(), sub);
    ImGui::PopID();
  }

  void deviceList() {
    paneTitle("DEVICES");
    static char search[96] = "";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##devsearch", "Search devices", search, sizeof(search));
    ImGui::Spacing();
    scanProgress();
    if (manager.scanning()) ImGui::Spacing();
    ImGui::BeginChild("##devicelist", ImVec2(0, -196), true);
    if (online() && selectedDevice < 0) {
      ImGui::TextDisabled("CURRENT SESSION");
      ImGui::TextWrapped("%s", deviceUrl);
      ImGui::Separator();
    }
    int shown = 0;
    for (int i = 0; i < (int)devices.size(); i++) {
      if (search[0]) {
        std::string hay = devices[i].name + " " + devices[i].ip + " " + devices[i].version;
        std::string needle = search;
        for (auto& ch : hay) ch = (char)tolower((unsigned char)ch);
        for (auto& ch : needle) ch = (char)tolower((unsigned char)ch);
        if (hay.find(needle) == std::string::npos) continue;
      }
      deviceRow(i);
      shown++;
    }
    if (!shown && !manager.scanning()) {
      ImGui::Spacing();
      const ScanState state = manager.scanState();
      if (!state.error.empty()) {
        ImGui::TextWrapped("Scan failed: %s", state.error.c_str());
      } else {
        ImGui::TextDisabled("No devices found.");
      }
      scanButton("Scan subnet");
    }
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::TextDisabled("MANUAL CONNECTION");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##listurl", "http://device.local", deviceUrl, sizeof(deviceUrl));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##listuser", "web user", authUser, sizeof(authUser));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##listpass", "web password", authPass, sizeof(authPass), ImGuiInputTextFlags_Password);
    if (theme::primaryButton("Connect", ImVec2(-1, 0))) connect();
    scanButton("Scan for devices", ImVec2(-1, 0));
  }

  void deviceStage() {
    if (previewDirty) refreshPreview();
    drawGrid(IM_COL32(42, 47, 54, 70));
    paneTitle("PREVIEW", online() ? "live firmware render" : "last render");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 94.0f);
    if (theme::secondaryButton("Open Design")) tab = 1;
    ImGui::SetCursorPosX(14);
    ImGui::Spacing();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float side = std::max(120.0f, std::min(480.0f, std::min(avail.x - 4.0f, avail.y - 76.0f)));
    const float x = ImGui::GetCursorPosX() + (avail.x - side) * 0.5f;
    const float yoff = std::max(0.0f, (avail.y - 74.0f - side) * 0.5f);
    ImGui::SetCursorPos(ImVec2(x, ImGui::GetCursorPosY() + yoff));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    theme::panel(ImGui::GetWindowDrawList(), ImVec2(p.x - 6, p.y - 6),
                 ImVec2(p.x + side + 6, p.y + side + 6), theme::surfaceAlt(), theme::border());
    ImGui::Image((ImTextureID)(intptr_t)g_texture, ImVec2(side, side));
    ImGui::Spacing();
    JsonDocument sd;
    if (!status.empty() && !deserializeJson(sd, status) && sd.is<JsonObject>()) {
      char line[220];
      snprintf(line, sizeof(line), "%s  ·  %s  ·  %s",
               (const char*)(sd["hostname"] | "device"),
               (const char*)(sd["ip"] | "-"),
               (const char*)(sd["mode"] | "unknown"));
      ImGui::SetCursorPosX(x);
      ImGui::TextDisabled("%s", line);
    } else {
      ImGui::SetCursorPosX(x);
      ImGui::TextDisabled("No device connected");
    }
    ImGui::Spacing();
    ImGui::SetCursorPosX(x);
    if (theme::secondaryButton("Load device view")) loadViewFromDevice();
  }

  void deviceInspector() {
    beginPane("##inspector", ImVec2(0, 0), true);
    paneTitle("INSPECTOR");
    ImGui::Spacing();
    theme::sectionLabel("STATUS");
    ImGui::Spacing();
    JsonDocument sd;
    const bool hasStatus = !status.empty() && !deserializeJson(sd, status) && sd.is<JsonObject>();
    if (!hasStatus) {
      ImGui::TextDisabled("No live status.");
      ImGui::TextWrapped("Connect a device to load telemetry.");
    } else {
      if (!online()) {
        ImGui::TextDisabled("Last known data · %s · seen %s", connLabel(connState),
                             lastSeenText().c_str());
        if (!connError.empty()) ImGui::TextWrapped("%s", connError.c_str());
        ImGui::Spacing();
      }
      if (ImGui::BeginTable("##statusrows", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 78.0f);
        ImGui::TableSetupColumn("v");
        auto row = [](const char* k, const char* v) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("%s", k);
          ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(v && *v ? v : "-");
        };
        char sig[32], heap[32], up[48];
        snprintf(sig, sizeof(sig), "%d dBm", (int)(sd["rssi"] | 0));
        snprintf(heap, sizeof(heap), "%u B", (unsigned)(sd["heap"] | 0));
        uint32_t sec = sd["uptime"] | 0;
        snprintf(up, sizeof(up), "%ud %02u:%02u", sec / 86400, (sec / 3600) % 24, (sec / 60) % 60);
        row("Hostname", sd["hostname"] | "-");
        row("Firmware", sd["version"] | "-");
        row("Variant", sd["variant"] | "-");
        row("Mode", sd["mode"] | "-");
        row("SSID", sd["ssid"] | "-");
        row("IP", sd["ip"] | "-");
        row("Signal", sig);
        row("Heap", heap);
        row("Uptime", up);
        ImGui::EndTable();
      }
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    theme::sectionLabel("USAGE");
    ImGui::Spacing();
    JsonDocument ud;
    JsonArray providers = (!usage.empty() && !deserializeJson(ud, usage) && ud.is<JsonObject>())
                              ? ud["providers"].as<JsonArray>() : JsonArray();
    if (providers.isNull() || providers.size() == 0) {
      ImGui::TextDisabled("No usage data.");
    } else {
      for (JsonObject p : providers) {
        const char* id = p["id"] | "provider";
        ImGui::PushID(id);
        ImGui::TextUnformatted((const char*)(p["name"] | id));
        const bool valid = p["valid"] | p["ok"] | false;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", valid ? "ok" : "stale");
        JsonArray ms = p["metrics"].as<JsonArray>();
        for (JsonObject m : ms) {
          int pct = std::max(0, std::min(100, (int)(m["pct"] | 0)));
          ImGui::TextDisabled("%s", (const char*)(m["label"] | "usage"));
          ImGui::SameLine(ImGui::GetContentRegionAvail().x - 34);
          ImGui::Text("%d%%", pct);
          ImGui::ProgressBar(pct / 100.0f, ImVec2(-1, 5), "");
        }
        ImGui::PopID();
      }
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    theme::sectionLabel("MAINTENANCE");
    ImGui::Spacing();
    if (theme::secondaryButton("Refresh telemetry", ImVec2(-1, 0))) {
      loadStatus();
      loadUsage();
    }
    if (theme::secondaryButton("Reload configuration", ImVec2(-1, 0))) loadConfig();
    scanButton("Scan devices", ImVec2(-1, 0));
    endPane();
  }

  void deviceScreen() {
    ImGui::BeginChild("##devicebody", ImVec2(0, 0), false);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = 12.0f;
    const float leftW = 250.0f;
    const float rightW = 300.0f;
    const float centerW = std::max(220.0f, avail.x - leftW - rightW - 2.0f * gap);
    beginPane("##devices", ImVec2(leftW, avail.y), true);
    deviceList();
    endPane();
    ImGui::SameLine(0, gap);
    beginPane("##stage", ImVec2(centerW, avail.y), true);
    deviceStage();
    endPane();
    ImGui::SameLine(0, gap);
    deviceInspector();
    ImGui::EndChild();
  }

  void settings() {
    ImGui::BeginChild("##settingsbody", ImVec2(0, 0), false);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = 12.0f;
    const float navW = 190.0f;
    beginPane("##settingsnav", ImVec2(navW, avail.y), true);
    paneTitle("SETTINGS");
    ImGui::Spacing();
    const char* items[] = {"WiFi", "Display", "View", "System", "JSON"};
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 4));
    for (int i = 0; i < 5; i++) {
      ImGui::PushID(i);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x;
      const float h = 32.0f;
      ImGui::InvisibleButton("##nav", ImVec2(w, h));
      const bool hovered = ImGui::IsItemHovered();
      if (ImGui::IsItemClicked()) settingsTab = i;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      if (settingsTab == i) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), theme::surfaceAlt());
        dl->AddRectFilled(p, ImVec2(p.x + 2, p.y + h), theme::accent());
      } else if (hovered) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(26, 30, 36, 255));
      }
      const ImVec2 ts = ImGui::CalcTextSize(items[i]);
      dl->AddText(ImVec2(p.x + 14, p.y + (h - ts.y) * 0.5f),
                  settingsTab == i ? theme::accent() : theme::muted(), items[i]);
      ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Changes are written to the device.");
    ImGui::PopTextWrapPos();
    endPane();
    ImGui::SameLine(0, gap);
    beginPane("##settingsform", ImVec2(0, avail.y), true);
    if (!cfgLoaded) {
      paneTitle("CONFIGURATION");
      ImGui::TextDisabled("Connect to load the editable configuration.");
      if (theme::primaryButton("Load configuration")) loadConfig();
      diagnosticsSection();
    } else {
      const char* titles[] = {"WiFi", "Display", "View", "System", "JSON"};
      paneTitle("SETTINGS", titles[settingsTab]);
      ImGui::Spacing();
      if (settingsTab == 0) settingsWifi();
      else if (settingsTab == 1) settingsDisplay();
      else if (settingsTab == 2) settingsView();
      else if (settingsTab == 3) settingsSystem();
      else settingsJson();
    }
    endPane();
    ImGui::EndChild();
  }

  void draw() {
    autosaveTick();
    pollAppUpdate();
    if (firmwareReleasesLoading && firmwareReleasesFuture.valid() &&
        firmwareReleasesFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
      firmwareReleases = firmwareReleasesFuture.get();
      firmwareReleasesLoading = false;
      firmwareReleaseIndex = 0;
      if (!firmwareReleases.empty() && !firmwareReleases[0].tag.empty() &&
          firmwareReleases[0].tag[0] == '!') {
        firmwareReleaseError = firmwareReleases[0].tag.substr(1);
      }
    }
    if (usageFetchRunning && usageFuture.valid() &&
        usageFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
      usage::FetchResult batch = usageFuture.get();
      usageFetchRunning = false;
      usageError = batch.error;
      if (!batch.error.empty()) {
        notify("Usage fetch failed: " + batch.error);
      } else {
        usageResults = batch.providers;
        usageCreds.clear();
        if (!deserializeJson(usageCreds, batch.credentialsJson) && usageCreds.is<JsonObject>()) {
          std::string error;
          if (!usage::secureSave(batch.credentialsJson, error)) usageCredsError = error;
          else usageCredsError.clear();
        }
        notify("Usage fetch complete");
      }
    }
    if (uploadRunning && uploadFuture.valid() &&
        uploadFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
      bool ok = uploadFuture.get();
      uploadRunning = false;
      if (ok) {
        startUpdateWatch(uploadStartVersion);
        notify("Firmware uploaded; waiting for reboot");
      } else {
        updatePhase = "error";
        updateMessage = "Firmware upload failed";
        updateError = uploadError;
        updateProgress = -1;
        connState = SDL_GetTicks() - lastSuccessAt < 15000 ? ConnState::Online : ConnState::Offline;
        notify("Firmware upload failed: " + uploadError);
      }
    }
    if (forceTab >= 0) { tab = forceTab; forceTab = -1; }
    if (appMode == AppMode::Offline && (tab == 0 || tab == 3)) tab = 1;
    if (hasImageRefs() && SDL_GetTicks() - lastPreviewTick >= 100) {
      lastPreviewTick = SDL_GetTicks();
      previewDirty = true;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(vp->WorkPos,
        ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y),
        theme::bg());
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("SmallTV", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground);
    const float margin = 12.0f;
    const float topbarH = 56.0f;
    ImGui::SetCursorPos(ImVec2(margin, margin));
    if (appMode == AppMode::Connection) {
      connectionScreen();
      ImGui::End();
      ImGui::PopStyleVar();
      toastWindow();
      appUpdateModal();
      disconnectModal();
      return;
    }

    beginPane("##topbar", ImVec2(ImGui::GetContentRegionAvail().x - margin, topbarH), false);
    topbar();
    endPane();

    float pageY = margin + topbarH + 12.0f;
    if (appMode == AppMode::Online && !online() && !updateWatch) {
      ImGui::SetCursorPos(ImVec2(margin, pageY));
      beginPane("##offlinebanner", ImVec2(ImGui::GetContentRegionAvail().x - margin, 38), false);
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::warning()),
                         "Device offline%s%s", connError.empty() ? "" : " · ",
                         connError.empty() ? "" : connError.c_str());
      ImGui::SameLine(ImGui::GetContentRegionMax().x - 260.0f);
      if (theme::secondaryButton("Reconnect", ImVec2(110, 0))) connect(true);
      ImGui::SameLine();
      if (theme::secondaryButton("Switch device", ImVec2(130, 0))) requestConnectionScreen();
      endPane();
      pageY += 48.0f;
    }

    ImGui::SetCursorPos(ImVec2(margin, pageY));
    const ImVec2 pageAvail = ImGui::GetContentRegionAvail();
    const float pageH = std::max(120.0f, pageAvail.y - margin);
    ImGui::BeginChild("##page", ImVec2(pageAvail.x - margin, pageH), false);
    if (tab == 0) deviceScreen();
    else if (tab == 1) editor();
    else if (tab == 2) usageScreen();
    else settings();
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar();
    toastWindow();
    appUpdateModal();
    disconnectModal();
    dialog.draw();
    if (imageManagerOpen) {
      if (!ImGui::IsPopupOpen("Device images")) ImGui::OpenPopup("Device images");
      imageManagerModal();
    }
  }
};

int main(int argc, char** argv) {
  const char* viewPath = "examples/view.json";
  const char* usagePath = "daemon/usage.example.json";
  const char* screenshot = nullptr;
  const char* timeText = nullptr;
  const char* tabText = nullptr;
  const char* settingsTabText = nullptr;
  const char* connectUrl = nullptr;
  const char* connectUser = nullptr;
  const char* connectPass = nullptr;
  bool startOffline = false;
  bool usageWorker = false;
  bool usageOnce = false;
  bool usageProxy = true;
  int usageInterval = 60;
  const char* usageDevice = "http://smalltv.local";

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--view") && i + 1 < argc) viewPath = argv[++i];
    else if (!strcmp(argv[i], "--usage") && i + 1 < argc) usagePath = argv[++i];
    else if (!strcmp(argv[i], "--time") && i + 1 < argc) timeText = argv[++i];
    else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
    else if (!strcmp(argv[i], "--tab") && i + 1 < argc) tabText = argv[++i];
    else if (!strcmp(argv[i], "--settings-tab") && i + 1 < argc) settingsTabText = argv[++i];
    else if (!strcmp(argv[i], "--url") && i + 1 < argc) connectUrl = argv[++i];
    else if (!strcmp(argv[i], "--user") && i + 1 < argc) connectUser = argv[++i];
    else if (!strcmp(argv[i], "--pass") && i + 1 < argc) connectPass = argv[++i];
    else if (!strcmp(argv[i], "--offline")) startOffline = true;
    else if (!strcmp(argv[i], "--usage-worker")) usageWorker = true;
    else if (!strcmp(argv[i], "--usage-once")) usageOnce = true;
    else if (!strcmp(argv[i], "--no-usage-proxy")) usageProxy = false;
    else if (!strcmp(argv[i], "--usage-interval") && i + 1 < argc) usageInterval = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--usage-device") && i + 1 < argc) usageDevice = argv[++i];
    else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { print_usage(); return 0; }
    else { print_usage(); return 2; }
  }

  if (usageWorker) {
    return usage::runWorker(usageDevice, usageInterval, usageProxy, usageOnce);
  }

  if (timeText) {
    int hh = 0, mm = 0;
    if (sscanf(timeText, "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 ||
        mm < 0 || mm > 59) {
      fatal_error("Invalid --time, expected HH:MM");
      return 2;
    }
    if (!local_time(std::time(nullptr), g_fixedTm)) {
      fatal_error("Cannot read the local clock");
      return 1;
    }
    g_fixedTm.tm_hour = hh; g_fixedTm.tm_min = mm; g_fixedTm.tm_sec = 0;
    g_fixedTime = true;
  }

  std::string viewJson;
  if (!read_file(viewPath, viewJson)) {
    fatal_error(std::string("Cannot read view: ") + viewPath);
    return 1;
  }
  ViewScreen screen;
  String viewError;
  if (!viewScreenParse(viewJson.c_str(), viewJson.size(), screen, viewError)) {
    fatal_error(std::string("Invalid view: ") + viewError.c_str());
    return 1;
  }
  usageInit();
  std::string usageJson;
  if (read_file(usagePath, usageJson)) usageApply(String(usageJson.c_str(), usageJson.size()));
  viewRender(screen);
  if (screenshot) {
    if (!write_ppm(screenshot, gfxDev()->getFramebuffer(), gfxDev()->width(), gfxDev()->height())) {
      fatal_error("Cannot write screenshot");
      return 1;
    }
    std::cout << "wrote " << screenshot << "\n";
    return 0;
  }

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
#if defined(_WIN32)
  WSADATA wsa{};
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fatal_error(std::string("SDL init failed: ") + SDL_GetError());
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow("SmallTV", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        1280, 820, SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
  SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
  if (!renderer) renderer = window ? SDL_CreateRenderer(window, -1, 0) : nullptr;
  if (!window || !renderer) {
    fatal_error(std::string("SDL window failed: ") + SDL_GetError());
    return 1;
  }
  SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                            SDL_TEXTUREACCESS_STREAMING, gfxDev()->width(), gfxDev()->height());
  if (!texture) {
    fatal_error(std::string("SDL texture failed: ") + SDL_GetError());
    return 1;
  }
  g_texture = texture;
  g_renderer = renderer;
  upload_preview_texture();

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.FontGlobalScale = 1.0f;
  ImGui::StyleColorsDark();
  const std::string assetsDir = find_assets_dir();
  theme::init(renderer, assetsDir);
  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);

  App app;
  app.initialViewPath = viewPath;
  app.executablePath = fs::absolute(argv[0]).string();
  app.liveUsage = usageJson;
  app.loadAppUpdateState();
  app.checkAppUpdate(false);
  if (tabText) {
    if (!strcmp(tabText, "design") || !strcmp(tabText, "editor")) app.forceTab = 1;
    else if (!strcmp(tabText, "usage")) app.forceTab = 2;
    else if (!strcmp(tabText, "settings")) app.forceTab = 3;
    else app.forceTab = 0;
  }
  if (settingsTabText) {
    if (!strcmp(settingsTabText, "display")) app.settingsTab = 1;
    else if (!strcmp(settingsTabText, "view")) app.settingsTab = 2;
    else if (!strcmp(settingsTabText, "system")) app.settingsTab = 3;
    else if (!strcmp(settingsTabText, "json")) app.settingsTab = 4;
    else app.settingsTab = 0;
  }
  app.manager.discover();
  app.loadConnectionProfile();
  app.initView();
  if (startOffline) {
    app.appMode = App::AppMode::Offline;
    app.tab = 1;
  } else if (connectUrl) {
    snprintf(app.deviceUrl, sizeof(app.deviceUrl), "%s", connectUrl);
    if (connectUser) snprintf(app.authUser, sizeof(app.authUser), "%s", connectUser);
    if (connectPass) snprintf(app.authPass, sizeof(app.authPass), "%s", connectPass);
    app.connect();
  } else if (!startOffline && app.shouldAutoConnect()) {
    app.connect();
  }

  bool running = true;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL2_ProcessEvent(&event);
      if (event.type == SDL_QUIT) running = false;
      if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
          event.window.windowID == SDL_GetWindowID(window)) running = false;
    }
    app.pollDevices();
    app.pollHeartbeat();
    if (app.previewDirty) app.refreshPreview();

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    app.draw();
    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 12, 16, 22, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
  }

  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  theme::shutdown();
  ImGui::DestroyContext();
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
