#include "ViewOverlay.h"
#include "UsageClient.h"
#include "ViewMode.h"
#include "ViewRender.h"

ViewOverlay g_viewOverlay;

ViewOverlay::~ViewOverlay() { delete pending_; }

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
    delete pending_;         // a stale queued item cannot survive an idle gap
    pending_ = nullptr;
    start(staging, ttl);
    return true;
  }
  if (staging.priority > screen_.priority) {
    start(staging, ttl);     // strictly more important: take the panel now
    return true;
  }
  // Queue one item. Equal priority refreshes the waiting item (latest wins).
  if (!pending_ || staging.priority >= pending_->priority) {
    if (!pending_) pending_ = new ViewScreen;
    if (pending_) *pending_ = staging;
    pendingTtl_ = ttl;
  }
  return true;
}

void ViewOverlay::advance() {
  if (!pending_) {
    active_ = false;
    g_viewMode.repaint();
    return;
  }
  ViewScreen* next = pending_;
  pending_ = nullptr;
  const uint16_t ttl = pendingTtl_;
  start(*next, ttl);
  delete next;
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
    if (!viewRenderUsage(screen_)) { viewRender(screen_); return; }
    lastUsageRev_ = rev;
  }
  if (clockChanged) {
    if (!viewRenderText(screen_)) { viewRender(screen_); return; }
    lastClockKey_ = clock;
  }
}

void ViewOverlay::dismiss() {
  if (!active_ && !pending_) return;
  active_ = false;
  delete pending_;
  pending_ = nullptr;
  g_viewMode.repaint();
}
