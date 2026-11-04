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
#include <sstream>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <winsock2.h>
#endif

#include "Gfx.h"
#include "Arduino_GFX_Library.h"
#include "manager.h"
#include "UsageClient.h"
#include "ViewModel.h"
#include "ViewRender.h"
#include "theme.h"

namespace fs = std::filesystem;

void gfxSetPreviewDisplay(int order, bool invert, int rGain, int gGain, int bGain);

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

static void usage() {
  std::cerr
      << "usage: smalltv-emulator [options]\n"
      << "  --view PATH       draw-list JSON (default examples/view.json)\n"
      << "  --usage PATH      normalized usage JSON (default daemon/usage.example.json)\n"
      << "  --time HH:MM      fixed clock time instead of the local clock\n"
      << "  --tab NAME        start on device, design or settings\n"
      << "  --settings-tab NAME start settings on wifi, display, view, system or json\n"
      << "  --url URL         connect to a device at startup\n"
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
        if (filter.empty() || it->path().extension() == filter) ffs.push_back(name);
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
  DeviceManager manager;
  std::vector<EmuDevice> devices;
  int selectedDevice = -1;
  char deviceUrl[192] = "http://192.168.4.1";
  char authUser[96] = "";
  char authPass[96] = "";
  bool connected = false;
  std::string lastError;
  std::string toast;
  uint32_t toastUntil = 0;
  int tab = 0;
  int forceTab = -1;
  int settingsTab = 0;
  bool scanRequested = false;

  JsonDocument cfg;
  bool cfgLoaded = false;
  char cfgRaw[65536] = {};
  std::string status;
  std::string usage;
  std::string scanJson;
  JsonDocument scanDoc;
  std::string updateJson;
  uint32_t lastPoll = 0;

  JsonDocument view;
  bool viewLoaded = false;
  bool viewDirty = false;
  std::string viewError;
  bool previewDirty = true;
  int selected = -1;
  char viewJsonBuf[65536] = {};
  std::vector<std::string> undoStack;
  std::vector<std::string> redoStack;
  bool showJsonEditor = false;

  std::string firmwarePath;
  std::future<bool> uploadFuture;
  bool uploadRunning = false;
  std::string uploadError;
  std::string initialViewPath = "examples/view.json";
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
    const bool online = connected;
    const ImU32 col = online ? theme::success() : theme::muted();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(p.x + 4, p.y + 9), 4.0f, col);
    ImGui::Dummy(ImVec2(10, 0));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", online ? "Online" : "Offline");
  }

  void topbar() {
    const char* names[] = {"Device", "Design", "Settings"};
    ImGui::PushFont(theme::fonts().heading);
    ImGui::TextUnformatted("SmallTV");
    ImGui::PopFont();
    ImGui::SameLine(112);
    for (int i = 0; i < 3; i++) {
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 9));
      ImGui::PushStyleColor(ImGuiCol_Button, i == tab ? theme::surfaceAlt() : theme::surface());
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::surfaceAlt());
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::surfaceAlt());
      ImGui::PushStyleColor(ImGuiCol_Text, i == tab ? theme::accent() : theme::muted());
      if (ImGui::Button(names[i], ImVec2(0, 30))) tab = i;
      ImGui::PopStyleColor(4);
      ImGui::PopStyleVar();
      if (i == tab) {
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
    ImGui::TextDisabled("%s", deviceUrl);
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
      if (theme::primaryButton("Connect")) { connect(); ImGui::CloseCurrentPopup(); }
      ImGui::SameLine();
      scanButton("Scan");
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

  void connect() {
    std::string url = deviceUrl;
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
      url = "http://" + url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    snprintf(deviceUrl, sizeof(deviceUrl), "%s", url.c_str());
    manager.setTarget(url);
    manager.setAuth(authUser, authPass);
    connected = true;
    lastError.clear();
    notify("Connected to " + url);
    loadConfig();
    loadStatus();
    loadUsage();
    if (!viewDirty) loadViewFromDevice();
  }

  bool api(const std::string& method, const std::string& path,
           const std::string& body, const std::string& ct, DeviceResponse& r) {
    if (!manager.request(method, path, body, ct, r)) {
      lastError = "network error";
      notify(lastError);
      return false;
    }
    if (r.status < 200 || r.status >= 300) {
      lastError = "HTTP " + std::to_string(r.status) +
                  (r.body.empty() ? "" : ": " + r.body.substr(0, 180));
      notify(lastError);
      return false;
    }
    return true;
  }

  void loadStatus() {
    DeviceResponse r;
    if (api("GET", "/api/status", "", "", r)) status = json_text(r.body);
  }

  void loadUsage() {
    DeviceResponse r;
    if (api("GET", "/api/usage", "", "", r)) {
      usage = json_text(r.body);
      if (!usage.empty()) {
        std::string raw = r.body;
        if (usageApply(String(raw.c_str(), raw.size()))) previewDirty = true;
      }
    }
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
    selected = std::min(selected, (int)(view["draw"].as<JsonArray>().size()) - 1);
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
    selected = std::min(selected, (int)(view["draw"].as<JsonArray>().size()) - 1);
    previewDirty = true;
    syncJson();
  }

  void syncJson() {
    std::string s;
    serializeJsonPretty(view, s);
    snprintf(viewJsonBuf, sizeof(viewJsonBuf), "%s", s.c_str());
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
    selected = -1;
    undoStack.clear();
    redoStack.clear();
    viewDirty = false;
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
    notify("Loaded dashboard from device");
  }

  void saveViewToDevice() {
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
      if (api("POST", "/api/view", serializeView(), "application/json", r)) notify("Overlay sent");
    }
  }

  void refreshPreview() {
    std::string s = serializeView();
    ViewScreen screen;
    String err;
    if (!viewScreenParse(s.c_str(), s.size(), screen, err)) {
      viewError = err.c_str();
      return;
    }
    viewError.clear();
    viewRender(screen);
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
        ImGui::TextDisabled("Uploading firmware...");
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(240, 0), "uploading");
      } else if (ImGui::Button("Upload firmware")) {
        const std::string path = firmwarePath;
        uploadRunning = true;
        uploadError.clear();
        notify("Uploading firmware...");
        uploadFuture = std::async(std::launch::async, [this, path] {
          std::string error;
          bool ok = manager.uploadFirmware(path, error);
          uploadError = error;
          return ok;
        });
      }
    }
    if (ImGui::Button("Check GitHub updates")) {
      DeviceResponse r;
      if (api("GET", "/api/checkupdate", "", "", r)) {
        updateJson = json_text(r.body);
        notify("Update check complete");
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Install latest update")) {
      DeviceResponse r;
      if (api("POST", "/api/selfupdate", "", "application/json", r)) {
        updateJson = "Update started. The device will restart.\n" + json_text(r.body);
        notify("Self-update started");
      }
    }
    if (!updateJson.empty()) {
      if (ImGui::BeginChild("##update", ImVec2(0, 130), true)) ImGui::TextUnformatted(updateJson.c_str());
      ImGui::EndChild();
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
  static void layer_bounds(JsonObject l, float& x, float& y, float& w, float& h) {
    const char* t = l["t"] | "";
    x = l["x"] | 0;
    y = l["y"] | 0;
    int s = l["s"] | 1;
    const char* a = l["a"] | "l";
    w = h = 0;
    if (!strcmp(t, "text")) {
      w = (float)strlen(l["v"] | "") * 6 * s;
      h = 8.0f * s;
      if (a[0] == 'c') x -= w / 2;
      else if (a[0] == 'r') x -= w;
    } else if (!strcmp(t, "rect") || !strcmp(t, "rrect") || !strcmp(t, "usage") ||
               !strcmp(t, "bitmap")) {
      w = (float)(l["w"] | 0) * (!strcmp(t, "bitmap") ? s : 1);
      h = (float)(l["h"] | 0) * (!strcmp(t, "bitmap") ? s : 1);
      if (!strcmp(t, "bitmap")) {
        if (a[0] == 'c') x -= w / 2;
        else if (a[0] == 'r') x -= w;
      }
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
      w = h = 24.0f * s;
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
      l["s"] = 3; l["c"] = "#e9edf4"; l["a"] = "c"; l["v"] = "text";
    } else if (!strcmp(type, "rect") || !strcmp(type, "rrect")) {
      l["w"] = 100; l["h"] = 40; l["c"] = "#3fb950";
      if (!strcmp(type, "rrect")) l["r"] = 8;
    } else if (!strcmp(type, "circle")) {
      l["r"] = 30; l["c"] = "#d97757";
    } else if (!strcmp(type, "line")) {
      l["x2"] = 200; l["y2"] = 200; l["c"] = "#e9edf4";
    } else if (!strcmp(type, "icon")) {
      l["s"] = 2; l["c"] = "#86e2bf"; l["a"] = "c"; l["v"] = "sun";
    } else if (!strcmp(type, "bitmap")) {
      l["x"] = 100; l["y"] = 100; l["w"] = 8; l["h"] = 8; l["s"] = 2;
      l["a"] = "l"; l["c"] = "#3fb950"; l["d"] = "AA55AA55AA55AA55";
    } else if (!strcmp(type, "usage")) {
      l["w"] = 224; l["h"] = 54; l["c"] = "#d97757";
      l["provider"] = "claude"; l["metric"] = -1;
    } else {
      l["c"] = "#ffffff";
    }
    selected = (int)draw.size() - 1;
    viewDirty = true;
    previewDirty = true;
    syncJson();
  }

  void duplicateSelected() {
    JsonArray draw = view["draw"].as<JsonArray>();
    if (selected < 0 || selected >= (int)draw.size()) return;
    pushUndo();
    JsonObject src = draw[selected];
    JsonObject dst = draw.add<JsonObject>();
    for (JsonPair p : src) dst[p.key()] = p.value();
    dst["x"] = (src["x"] | 0) + 8;
    dst["y"] = (src["y"] | 0) + 8;
    selected = (int)draw.size() - 1;
    viewDirty = true;
    previewDirty = true;
    syncJson();
  }

  void deleteSelected() {
    JsonArray draw = view["draw"].as<JsonArray>();
    if (selected < 0 || selected >= (int)draw.size()) return;
    pushUndo();
    draw.remove(selected);
    selected = std::min(selected, (int)draw.size() - 1);
    viewDirty = true;
    previewDirty = true;
    syncJson();
  }

  void moveLayer(int delta) {
    JsonArray draw = view["draw"].as<JsonArray>();
    if (selected < 0 || selected >= (int)draw.size()) return;
    int to = selected + delta;
    if (to < 0 || to >= (int)draw.size()) return;
    pushUndo();
    std::string a, b;
    serializeJson(draw[selected], a);
    serializeJson(draw[to], b);
    JsonDocument da, db;
    deserializeJson(da, a);
    deserializeJson(db, b);
    draw[selected].set(db.as<JsonVariant>());
    draw[to].set(da.as<JsonVariant>());
    selected = to;
    viewDirty = true;
    previewDirty = true;
    syncJson();
  }

  void alignSelected(JsonObject l, const char* mode) {
    if (!strcmp(mode, "left")) l["x"] = 4;
    else if (!strcmp(mode, "hcenter")) l["x"] = 120;
    else if (!strcmp(mode, "right")) l["x"] = 236;
    else if (!strcmp(mode, "top")) l["y"] = 4;
    else if (!strcmp(mode, "vcenter")) l["y"] = 120;
    else if (!strcmp(mode, "bottom")) l["y"] = 236;
    viewDirty = true;
    previewDirty = true;
    syncJson();
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

  void layerProperties() {
    JsonArray draw = view["draw"].as<JsonArray>();
    if (selected < 0 || selected >= (int)draw.size()) {
      ImGui::TextDisabled("Select a layer on the canvas or in the list.");
      ImGui::Spacing();
      ImGui::SeparatorText("View defaults");
      JsonObject vo = view.as<JsonObject>();
      color_edit_json(vo, "bg", "Background", "#0b1420");
      input_int_json(vo, "pr", "Priority", 0, 255, 100);
      input_int_json(vo, "ttl", "TTL (seconds)", 0, 86400, 15);
      ImGui::TextDisabled("Layers: %d", (int)draw.size());
      ImGui::Spacing();
      ImGui::TextWrapped("Drag a layer to move it. Drag the bottom-right handle to resize.");
      return;
    }
    JsonObject l = draw[selected];
    const char* t = l["t"] | "";
    bool changed = false;
    ImGui::Text("Type: %s", t);
    changed |= input_int_json(l, "x", "X", -512, 512, 0);
    changed |= input_int_json(l, "y", "Y", -512, 512, 0);
    changed |= color_edit_json(l, "c", "Colour", "#ffffff");
    if (ImGui::Button("Delete")) { deleteSelected(); return; }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate")) { duplicateSelected(); return; }
    ImGui::Separator();
    if (!strcmp(t, "text")) {
      changed |= input_text_json(l, "v", "Text", false, "Tokens: {HH} {MM} {SS} {DD} {YYYY} {MON} {WD}");
      changed |= input_int_json(l, "s", "Scale", 1, 6, 1);
      static const char* aligns[] = {"l", "c", "r"};
      const char* cur = l["a"] | "l";
      int ai = !strcmp(cur, "c") ? 1 : !strcmp(cur, "r") ? 2 : 0;
      if (ImGui::Combo("Align", &ai, aligns, 3)) { l["a"] = aligns[ai]; changed = true; }
    } else if (!strcmp(t, "rect") || !strcmp(t, "rrect") || !strcmp(t, "usage")) {
      changed |= input_int_json(l, "w", "Width", 1, 480, 10);
      changed |= input_int_json(l, "h", "Height", 1, 480, 10);
      if (!strcmp(t, "rrect")) changed |= input_int_json(l, "r", "Radius", 0, 255, 0);
      if (!strcmp(t, "usage")) {
        static const char* providers[] = {"claude", "chatgpt", "deepseek"};
        const char* cur = l["provider"] | "claude";
        int pi = !strcmp(cur, "chatgpt") ? 1 : !strcmp(cur, "deepseek") ? 2 : 0;
        if (ImGui::Combo("Provider", &pi, providers, 3)) { l["provider"] = providers[pi]; changed = true; }
        changed |= input_int_json(l, "metric", "Metric (-1 all)", -1, 32, -1);
      }
    } else if (!strcmp(t, "circle")) {
      changed |= input_int_json(l, "r", "Radius", 1, 255, 20);
    } else if (!strcmp(t, "line")) {
      changed |= input_int_json(l, "x2", "X2", -512, 512, 0);
      changed |= input_int_json(l, "y2", "Y2", -512, 512, 0);
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
      changed |= input_int_json(l, "s", "Scale", 1, 8, 1);
      static const char* aligns[] = {"l", "c", "r"};
      const char* a = l["a"] | "l";
      int ai = !strcmp(a, "c") ? 1 : !strcmp(a, "r") ? 2 : 0;
      if (ImGui::Combo("Align", &ai, aligns, 3)) { l["a"] = aligns[ai]; changed = true; }
    } else if (!strcmp(t, "bitmap")) {
      changed |= input_int_json(l, "w", "Width", 1, 48, 8);
      changed |= input_int_json(l, "h", "Height", 1, 48, 8);
      changed |= input_int_json(l, "s", "Scale", 1, 4, 1);
      changed |= input_text_json(l, "d", "Hex data");
      bitmapGrid(l);
    } else if (!strcmp(t, "fill")) {
      ImGui::TextDisabled("Background fill");
    }
    ImGui::SeparatorText("Align on canvas");
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
    if (theme::secondaryButton("Undo")) undo();
    ImGui::SameLine();
    if (theme::secondaryButton("Redo")) redo();
    ImGui::SameLine();
    if (theme::secondaryButton("Duplicate")) duplicateSelected();
    ImGui::SameLine();
    if (theme::secondaryButton("Delete")) deleteSelected();
  }

  void editorLayerPane() {
    paneTitle("LAYERS");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##addlayer", "Add layer...")) {
      const char* types[] = {"text", "rect", "rrect", "circle", "line", "icon", "bitmap", "usage", "fill"};
      for (const char* t : types) if (ImGui::Selectable(t)) addLayer(t);
      ImGui::EndCombo();
    }
    ImGui::Spacing();
    JsonArray draw = view["draw"].as<JsonArray>();
    ImGui::BeginChild("##layers", ImVec2(0, -42), true);
    for (int i = (int)draw.size() - 1; i >= 0; i--) {
      JsonObject l = draw[i];
      char label[180];
      const char* t = l["t"] | "?";
      if (!strcmp(t, "text")) snprintf(label, sizeof(label), "%02d   TEXT   %s", i, (const char*)(l["v"] | ""));
      else if (!strcmp(t, "usage")) snprintf(label, sizeof(label), "%02d   USAGE  %s", i, (const char*)(l["provider"] | ""));
      else if (!strcmp(t, "icon")) snprintf(label, sizeof(label), "%02d   ICON   %s", i, (const char*)(l["v"] | ""));
      else snprintf(label, sizeof(label), "%02d   %s", i, t);
      if (ImGui::Selectable(label, selected == i, 0, ImVec2(0, 26))) selected = i;
    }
    ImGui::EndChild();
    if (theme::secondaryButton("Up")) moveLayer(-1);
    ImGui::SameLine();
    if (theme::secondaryButton("Down")) moveLayer(1);
    ImGui::SameLine();
    if (theme::secondaryButton("Delete")) deleteSelected();
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
    if (theme::secondaryButton("Load device", ImVec2(-1, 0))) loadViewFromDevice();
    if (theme::secondaryButton("Save device", ImVec2(-1, 0))) saveViewToDevice();
    if (theme::secondaryButton("Send overlay", ImVec2(-1, 0))) sendOverlay(false);
    if (theme::secondaryButton("Clear overlay", ImVec2(-1, 0))) sendOverlay(true);
    if (theme::secondaryButton("Fetch usage", ImVec2(-1, 0))) loadUsage();
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

  void editorCanvas(float side) {
    const float S = side / 240.0f;
    if (previewDirty) refreshPreview();
    ImVec2 imgPos = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(intptr_t)g_texture, ImVec2(side, side));
    ImGui::SetCursorScreenPos(imgPos);
    ImGui::InvisibleButton("##canvas", ImVec2(side, side));
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    ImVec2 mouse = ImGui::GetIO().MousePos;
    float mx = (mouse.x - imgPos.x) / S;
    float my = (mouse.y - imgPos.y) / S;

    JsonArray draw = view["draw"].as<JsonArray>();
    static int dragMode = 0;
    static bool undoPushed = false;
    static ImVec2 press{};
    if (ImGui::IsItemClicked(0)) {
      bool onHandle = false;
      if (selected >= 0 && selected < (int)draw.size()) {
        float x, y, w, h;
        layer_bounds(draw[selected], x, y, w, h);
        onHandle = mx >= x + w - 10 && mx <= x + w + 10 && my >= y + h - 10 && my <= y + h + 10;
      }
      if (!onHandle) {
        selected = -1;
        for (int i = (int)draw.size() - 1; i >= 0; i--) {
          float x, y, w, h;
          layer_bounds(draw[i], x, y, w, h);
          if (mx >= x && mx <= x + w && my >= y && my <= y + h) { selected = i; break; }
        }
      }
      if (selected >= 0 && selected < (int)draw.size()) {
        float x, y, w, h;
        layer_bounds(draw[selected], x, y, w, h);
        dragMode = (mx >= x + w - 10 && mx <= x + w + 10 &&
                    my >= y + h - 10 && my <= y + h + 10) ? 2 : 1;
        press = ImVec2(mx, my);
        undoPushed = false;
      }
    }
    if (active && ImGui::IsMouseDragging(0) && selected >= 0 && selected < (int)draw.size()) {
      if (!undoPushed) { pushUndo(); undoPushed = true; }
      JsonObject l = draw[selected];
      float dx = mx - press.x, dy = my - press.y;
      if (dragMode == 2) {
        const char* t = l["t"] | "";
        if (!strcmp(t, "circle")) {
          int r = std::max(1, (int)((l["r"] | 1) + std::max(dx, dy)));
          l["r"] = std::min(255, r);
        } else {
          l["w"] = std::max(1, (int)((l["w"] | 1) + dx));
          l["h"] = std::max(1, (int)((l["h"] | 1) + dy));
        }
      } else {
        l["x"] = (int)((l["x"] | 0) + dx);
        l["y"] = (int)((l["y"] | 0) + dy);
      }
      press = ImVec2(mx, my);
      viewDirty = true;
      previewDirty = true;
      syncJson();
    }
    if (ImGui::IsItemDeactivated()) dragMode = 0;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < (int)draw.size(); i++) {
      if (i != selected) continue;
      float x, y, w, h;
      layer_bounds(draw[i], x, y, w, h);
      ImVec2 a(imgPos.x + x * S, imgPos.y + y * S);
      ImVec2 b(imgPos.x + (x + w) * S, imgPos.y + (y + h) * S);
      dl->AddRect(a, b, IM_COL32(255, 200, 60, 255), 0, 0, 2.0f);
      dl->AddRectFilled(ImVec2(b.x - 5, b.y - 5), ImVec2(b.x + 5, b.y + 5), IM_COL32(255, 200, 60, 255));
    }
    if (hovered && !active) {
      for (int i = (int)draw.size() - 1; i >= 0; i--) {
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
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (io.KeyShift) redo(); else undo();
      }
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
      if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) deleteSelected();
    }
    ImGui::BeginChild("##designbody", ImVec2(0, 0), false);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = 12.0f;
    const float leftW = avail.x > 920 ? 220.0f : 190.0f;
    const float rightW = avail.x > 920 ? 300.0f : 260.0f;
    const float centerW = std::max(240.0f, avail.x - leftW - rightW - 2.0f * gap);
    beginPane("##layerspane", ImVec2(leftW, avail.y), true);
    editorLayerPane();
    endPane();
    ImGui::SameLine(0, gap);
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
    ImGui::TextDisabled("240 × 240  ·  %d layers  ·  %s",
                        (int)view["draw"].as<JsonArray>().size(),
                        viewError.empty() ? "ready" : viewError.c_str());
    endPane();
    ImGui::SameLine(0, gap);
    beginPane("##inspectorpane", ImVec2(rightW, avail.y), true);
    paneTitle("INSPECTOR");
    ImGui::Spacing();
    layerProperties();
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::CollapsingHeader("File & device")) {
      ImGui::Spacing();
      editorActions();
    }
    endPane();
    ImGui::EndChild();
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
                        connected && selected ? theme::success() : theme::muted());
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
    if (connected && selectedDevice < 0) {
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
    paneTitle("PREVIEW", connected ? "live firmware render" : "local render");
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
    if (uploadRunning && uploadFuture.valid() &&
        uploadFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
      bool ok = uploadFuture.get();
      uploadRunning = false;
      if (ok) notify("Firmware uploaded; device is restarting");
      else notify("Firmware upload failed: " + uploadError);
    }
    if (forceTab >= 0) { tab = forceTab; forceTab = -1; }
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
    beginPane("##topbar", ImVec2(ImGui::GetContentRegionAvail().x - margin, topbarH), false);
    topbar();
    endPane();

    ImGui::SetCursorPos(ImVec2(margin, margin + topbarH + 12.0f));
    const ImVec2 pageAvail = ImGui::GetContentRegionAvail();
    const float pageH = std::max(120.0f, pageAvail.y - margin);
    ImGui::BeginChild("##page", ImVec2(pageAvail.x - margin, pageH), false);
    if (tab == 0) deviceScreen();
    else if (tab == 1) editor();
    else settings();
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar();
    toastWindow();
    dialog.draw();
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
    else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
    else { usage(); return 2; }
  }

  if (timeText) {
    int hh = 0, mm = 0;
    if (sscanf(timeText, "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 ||
        mm < 0 || mm > 59) {
      std::cerr << "invalid --time, expected HH:MM\n";
      return 2;
    }
    if (!local_time(std::time(nullptr), g_fixedTm)) { std::cerr << "cannot read local clock\n"; return 1; }
    g_fixedTm.tm_hour = hh; g_fixedTm.tm_min = mm; g_fixedTm.tm_sec = 0;
    g_fixedTime = true;
  }

  std::string viewJson;
  if (!read_file(viewPath, viewJson)) { std::cerr << "cannot read view: " << viewPath << "\n"; return 1; }
  ViewScreen screen;
  String viewError;
  if (!viewScreenParse(viewJson.c_str(), viewJson.size(), screen, viewError)) {
    std::cerr << "invalid view: " << viewError.c_str() << "\n"; return 1;
  }
  usageInit();
  std::string usageJson;
  if (read_file(usagePath, usageJson)) usageApply(String(usageJson.c_str(), usageJson.size()));
  viewRender(screen);
  if (screenshot) {
    if (!write_ppm(screenshot, gfxDev()->getFramebuffer(), gfxDev()->width(), gfxDev()->height())) {
      std::cerr << "cannot write screenshot\n"; return 1;
    }
    std::cout << "wrote " << screenshot << "\n";
    return 0;
  }

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
#if defined(_WIN32)
  WSADATA wsa{};
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::cerr << "SDL init failed: " << SDL_GetError() << "\n"; return 1; }
  SDL_Window* window = SDL_CreateWindow("SmallTV", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        1280, 820, SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
  SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
  if (!renderer) renderer = window ? SDL_CreateRenderer(window, -1, 0) : nullptr;
  if (!window || !renderer) { std::cerr << "SDL window failed: " << SDL_GetError() << "\n"; return 1; }
  SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                            SDL_TEXTUREACCESS_STREAMING, gfxDev()->width(), gfxDev()->height());
  if (!texture) { std::cerr << "texture failed: " << SDL_GetError() << "\n"; return 1; }
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
  if (tabText) {
    if (!strcmp(tabText, "design") || !strcmp(tabText, "editor")) app.forceTab = 1;
    else if (!strcmp(tabText, "settings")) app.forceTab = 2;
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
  app.initView();
  if (connectUrl) {
    snprintf(app.deviceUrl, sizeof(app.deviceUrl), "%s", connectUrl);
    if (connectUser) snprintf(app.authUser, sizeof(app.authUser), "%s", connectUser);
    if (connectPass) snprintf(app.authPass, sizeof(app.authPass), "%s", connectPass);
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
    if (app.connected && SDL_GetTicks() - app.lastPoll > 5000) {
      app.lastPoll = SDL_GetTicks();
      app.loadStatus();
    }
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
