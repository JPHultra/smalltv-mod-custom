// ViewOverlay.h — a transient full-screen draw list, pushed by HTTP or fetched
// by the poller. It owns the panel for a TTL, then hands it back to the
// dashboard. Never persisted; a reboot clears it.
#pragma once
#include "ViewModel.h"

class ViewOverlay {
 public:
  // defaultTtlSec is used when the payload has no "ttl". Returns false on
  // invalid JSON; the current overlay is then left untouched.
  bool show(const String& json, uint16_t defaultTtlSec, String& error);
  void service();
  bool active() const { return active_; }
  void dismiss();
  const String& error() const { return error_; }

 private:
  ViewScreen screen_{};
  bool       active_ = false;
  uint32_t   untilMs_ = 0;
  uint32_t   lastUsageRev_ = 0;
  uint32_t   lastClockKey_ = 0;
  String     error_;
};

extern ViewOverlay g_viewOverlay;
