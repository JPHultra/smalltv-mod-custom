// ViewOverlay.h — a transient full-screen draw list, pushed by HTTP or fetched
// by the poller. One overlay is on screen and one may wait behind it; a higher
// priority pre-empts, and a queued item starts when the current one expires.
// Never persisted; a reboot clears it.
#pragma once
#include "ViewModel.h"

class ViewOverlay {
 public:
  ~ViewOverlay();
  // defaultTtlSec is used when the payload has no "ttl". Returns false on
  // invalid JSON; the current overlay/queue is then left untouched.
  bool show(const String& json, uint16_t defaultTtlSec, String& error);
  void service();
  bool active() const { return active_; }
  bool queued() const { return pending_ != nullptr; }
  // Clear the visible overlay and anything waiting; restore the dashboard.
  void dismiss();
  const String& error() const { return error_; }

 private:
  void start(const ViewScreen& screen, uint16_t ttlSec);
  void advance();

  ViewScreen screen_{};
  ViewScreen* pending_ = nullptr;
  bool       active_ = false;
  uint16_t   pendingTtl_ = 0;
  uint32_t   untilMs_ = 0;
  uint32_t   lastUsageRev_ = 0;
  uint32_t   lastClockKey_ = 0;
  String     error_;
};

extern ViewOverlay g_viewOverlay;
