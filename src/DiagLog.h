// DiagLog.h — small, bounded diagnostic log stored in LittleFS.
//
// Startup and error events only, never periodic status: the device keeps one
// 8 KiB log for the current boot and one for the previous boot. At the start of
// a boot the current file rotates to /diag.prev, so a crash or failed update
// leaves the previous session available for inspection.
#pragma once
#include <Arduino.h>

namespace diag {

constexpr const char* kCurrentPath = "/diag.log";
constexpr const char* kPreviousPath = "/diag.prev";
constexpr size_t kMaxFileBytes = 8 * 1024;

// Mount is already done by settingsBegin(). If the filesystem is unavailable
// this disables file logging and keeps mirroring to Serial.
void begin(const char* version, const char* variant, const char* resetReason,
           bool wasCrash, const char* epc, const char* addr, bool safeMode);

void info(const char* tag, const char* fmt, ...);
void warn(const char* tag, const char* fmt, ...);
void error(const char* tag, const char* fmt, ...);

// Remove both current and previous logs (used by factory reset).
void clear();

}  // namespace diag
