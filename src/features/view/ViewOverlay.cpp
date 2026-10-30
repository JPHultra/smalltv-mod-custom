#include "ViewOverlay.h"
#include "UsageClient.h"
#include "ViewMode.h"
#include "ViewRender.h"

ViewOverlay g_viewOverlay;

bool ViewOverlay::show(const String& json, uint16_t defaultTtlSec, String& error) {
  ViewScreen& staging = viewStaging();
  if (!viewScreenParse(json.c_str(), json.length(), staging, error)) return false;

  screen_ = staging;
  uint32_t ttl = screen_.ttl ? screen_.ttl : defaultTtlSec;
  if (ttl < VIEW_OVERLAY_TTL_MIN_SEC) ttl = VIEW_OVERLAY_TTL_MIN_SEC;
  if (ttl > VIEW_OVERLAY_TTL_MAX_SEC) ttl = VIEW_OVERLAY_TTL_MAX_SEC;

  untilMs_ = millis() + ttl * 1000UL;
  active_ = true;
  error_ = "";
  viewRender(screen_);
  lastUsageRev_ = usageRevision();
  lastClockKey_ = screen_.needsClock ? viewClockKey() : 0;
  return true;
}

void ViewOverlay::service() {
  if (!active_) return;
  if ((int32_t)(millis() - untilMs_) >= 0) {
    dismiss();
    return;
  }
  const uint32_t rev = usageRevision();
  const uint32_t clock = screen_.needsClock ? viewClockKey() : 0;
  if ((screen_.hasUsage && rev != lastUsageRev_) ||
      (screen_.needsClock && clock != lastClockKey_)) {
    viewRender(screen_);
    lastUsageRev_ = rev;
    lastClockKey_ = clock;
  }
}

void ViewOverlay::dismiss() {
  if (!active_) return;
  active_ = false;
  g_viewMode.repaint();
}
