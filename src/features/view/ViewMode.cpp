#include "ViewMode.h"
#include <LittleFS.h>
#include "Gfx.h"
#include "UsageClient.h"
#include "ViewRender.h"

ViewMode g_viewMode;

static const char* VIEW_PATH = "/view.json";
static const char* VIEW_TMP  = "/view.json.tmp";

bool ViewMode::reload() {
  File f = LittleFS.open(VIEW_PATH, "r");
  if (!f) {
    loaded_ = false;
    error_ = F("No view installed");
    return false;
  }
  if (f.size() == 0 || f.size() > VIEW_MAX_JSON) {
    f.close();
    loaded_ = false;
    error_ = F("View file is empty or too large");
    return false;
  }
  String json = f.readString();
  f.close();
  ViewScreen& staging = viewStaging();
  String error;
  if (!viewScreenParse(json.c_str(), json.length(), staging, error)) {
    loaded_ = false;
    error_ = error;
    return false;
  }
  screen_ = staging;
  loaded_ = true;
  error_ = "";
  return true;
}

bool ViewMode::replace(const String& json, String& error) {
  ViewScreen& staging = viewStaging();
  if (!viewScreenParse(json.c_str(), json.length(), staging, error)) return false;

  File f = LittleFS.open(VIEW_TMP, "w");
  if (!f) { error = F("Cannot create the view file"); return false; }
  const size_t expected = json.length();
  const size_t written = f.write((const uint8_t*)json.c_str(), expected);
  f.flush();
  const bool ok = written == expected && !f.getWriteError() && f.size() == expected;
  f.close();
  if (!ok) {
    LittleFS.remove(VIEW_TMP);
    error = F("Could not save the view");
    return false;
  }
  // LittleFS rename replaces an existing file atomically, so a failed publish
  // leaves the previous dashboard in place.
  if (!LittleFS.rename(VIEW_TMP, VIEW_PATH)) {
    LittleFS.remove(VIEW_TMP);
    error = F("Could not publish the view");
    return false;
  }

  screen_ = staging;
  loaded_ = true;
  error_ = "";
  messageDrawn_ = false;
  repaint();
  return true;
}

void ViewMode::begin() {
  reload();
  messageDrawn_ = false;
}

void ViewMode::repaint() {
  if (loaded_) {
    viewRender(screen_);
    lastUsageRev_ = usageRevision();
    lastClockKey_ = screen_.needsClock ? viewClockKey() : 0;
    return;
  }
  gfxMessage("SmallTV View", "Push a view from the editor", C_YELLOW);
  messageDrawn_ = true;
}

void ViewMode::invalidate() {
  reload();
  messageDrawn_ = false;
  repaint();
}

void ViewMode::service() {
  if (!loaded_) {
    if (!messageDrawn_) repaint();
    return;
  }
  const uint32_t rev = usageRevision();
  const uint32_t clock = screen_.needsClock ? viewClockKey() : 0;
  if ((screen_.hasUsage && rev != lastUsageRev_) ||
      (screen_.needsClock && clock != lastClockKey_)) repaint();
}
