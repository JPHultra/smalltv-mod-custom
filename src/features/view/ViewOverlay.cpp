#include "ViewOverlay.h"
#include "UsageClient.h"
#include "ViewMode.h"
#include "ViewRender.h"

ViewOverlay g_viewOverlay;

static uint16_t overlayTtl(const ViewScreen& screen, uint16_t defaultTtlSec) {
  uint32_t ttl = screen.ttl ? screen.ttl : defaultTtlSec;
  if (ttl < VIEW_OVERLAY_TTL_MIN_SEC) ttl = VIEW_OVERLAY_TTL_MIN_SEC;
  if (ttl > VIEW_OVERLAY_TTL_MAX_SEC) ttl = VIEW_OVERLAY_TTL_MAX_SEC;
  return (uint16_t)ttl;
}

void ViewOverlay::start(const ViewScreen& screen, uint16_t ttlSec) {
  screen_ = screen;
  active_ = true;
  untilMs_ = millis() + (uint32_t)ttlSec * 1000UL;
  error_ = "";
  viewRender(screen_);
  lastUsageRev_ = usageRevision();
  lastClockKey_ = screen_.needsClock ? viewClockKey(screen_.needsSeconds) : 0;
}

bool ViewOverlay::show(const String& json, uint16_t defaultTtlSec, String& error) {
  ViewScreen& staging = viewStaging();
  if (!viewScreenParse(json.c_str(), json.length(), staging, error)) return false;
  const uint16_t ttl = overlayTtl(staging, defaultTtlSec);

  if (!active_) {
    pendingValid_ = false;   // a stale queued item cannot survive an idle gap
    start(staging, ttl);
    return true;
  }
  if (staging.priority > screen_.priority) {
    start(staging, ttl);     // strictly more important: take the panel now
    return true;
  }
  // Queue one item. Equal priority refreshes the waiting item (latest wins).
  if (!pendingValid_ || staging.priority >= pending_.priority) {
    pending_ = staging;
    pendingTtl_ = ttl;
    pendingValid_ = true;
  }
  return true;
}

void ViewOverlay::advance() {
  if (!pendingValid_) {
    active_ = false;
    g_viewMode.repaint();
    return;
  }
  const uint16_t ttl = pendingTtl_;
  pendingValid_ = false;
  start(pending_, ttl);
}

void ViewOverlay::service() {
  if (!active_) return;
  if ((int32_t)(millis() - untilMs_) >= 0) {
    advance();
    return;
  }
  const uint32_t rev = usageRevision();
  const uint32_t clock = screen_.needsClock ? viewClockKey(screen_.needsSeconds) : 0;
  const bool usageChanged = screen_.hasUsage && rev != lastUsageRev_;
  const bool clockChanged = screen_.needsClock && clock != lastClockKey_;
  if (usageChanged) {
    viewRender(screen_);
    lastUsageRev_ = rev;
    lastClockKey_ = clock;
  } else if (clockChanged) {
    if (!viewRenderClock(screen_)) {
      viewRender(screen_);
      lastUsageRev_ = rev;
    }
    lastClockKey_ = clock;
  }
}

void ViewOverlay::dismiss() {
  if (!active_ && !pendingValid_) return;
  active_ = false;
  pendingValid_ = false;
  g_viewMode.repaint();
}
