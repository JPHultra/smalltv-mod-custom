// ViewMode.h — the persistent dashboard: one declarative draw list stored in
// LittleFS at /view.json and painted as the device's main screen.
#pragma once
#include "ViewModel.h"
#include "ImageAsset.h"

class ViewMode {
 public:
  void begin();
  void service();
  void invalidate();
  void wake() { repaint(); }
  void freeForRequest() {}

  // Validate, persist and activate a new dashboard JSON. On failure the current
  // dashboard and the stored file are left untouched.
  bool replace(const String& json, String& error);

  // Re-read /view.json (settings changed, factory reset, boot).
  bool reload();

  const ViewScreen& screen() const { return screen_; }
  bool hasScreen() const { return loaded_; }
  const String& error() const { return error_; }

  void repaint();

 private:
  bool prepareImages(const ViewScreen& screen, String& error);
  void resetAnimation();

  ViewScreen screen_{};
  FsImageSource imageSource_;
  bool       loaded_ = false;
  bool       painted_ = false;
  bool       messageDrawn_ = false;
  bool       animated_ = false;
  uint16_t   animDelayMs_ = 0;
  uint32_t   animStartMs_ = 0;
  uint32_t   lastAnimMs_ = 0;
  uint32_t   lastUsageRev_ = 0;
  uint32_t   lastClockKey_ = 0;
  bool       stale_ = false;
  String     error_;
};

extern ViewMode g_viewMode;
