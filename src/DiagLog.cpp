#include "DiagLog.h"

#include <LittleFS.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace diag {
namespace {

constexpr size_t kMaxLineBytes = 192;
constexpr size_t kTruncationReserve = 96;

bool g_enabled = false;
bool g_truncated = false;
size_t g_size = 0;

void appendLine(const char* line) {
  if (!g_enabled || g_truncated) return;
  const size_t length = strlen(line);
  if (g_size + length > kMaxFileBytes - kTruncationReserve) {
    if (!g_truncated) {
      char marker[64];
      snprintf(marker, sizeof(marker), "[%lu] W fs: log full, truncated\n",
               (unsigned long)(millis() / 1000));
      File mf = LittleFS.open(kCurrentPath, "a");
      if (mf) {
        mf.write((const uint8_t*)marker, strlen(marker));
        mf.close();
      }
      g_truncated = true;
    }
    return;
  }
  File f = LittleFS.open(kCurrentPath, "a");
  if (!f) {
    g_enabled = false;
    return;
  }
  const size_t written = f.write((const uint8_t*)line, length);
  f.close();
  if (written != length) {
    g_enabled = false;
    return;
  }
  g_size += length;
}

void log(char level, const char* tag, const char* fmt, va_list args) {
  char line[kMaxLineBytes];
  int at = snprintf(line, sizeof(line), "[%lu] %c %s: ",
                    (unsigned long)(millis() / 1000), level, tag ? tag : "-");
  if (at < 0 || (size_t)at >= sizeof(line)) return;
  vsnprintf(line + at, sizeof(line) - (size_t)at, fmt, args);

  for (char* c = line; *c; c++) {
    if (*c == '\r' || *c == '\n') *c = ' ';
  }
  size_t length = strlen(line);
  if (length + 2 < sizeof(line)) {
    line[length++] = '\n';
    line[length] = 0;
  } else {
    line[sizeof(line) - 2] = '\n';
    line[sizeof(line) - 1] = 0;
  }
  Serial.print(line);
  appendLine(line);
}

}  // namespace

void begin(const char* version, const char* variant, const char* resetReason,
           bool wasCrash, const char* epc, const char* addr, bool safeMode) {
  g_enabled = false;
  g_truncated = false;
  g_size = 0;

  LittleFS.remove(kPreviousPath);
  if (LittleFS.exists(kCurrentPath) &&
      !LittleFS.rename(kCurrentPath, kPreviousPath)) {
    LittleFS.remove(kCurrentPath);
  }

  File f = LittleFS.open(kCurrentPath, "w");
  if (!f) {
    Serial.println("[diag] file logging disabled");
    return;
  }
  f.close();
  g_enabled = true;

  info("boot", "%s %s", version ? version : "-", variant ? variant : "-");
  if (wasCrash) {
    info("boot", "reset: %s epc=%s addr=%s safe=%d",
         resetReason ? resetReason : "-",
         epc && epc[0] ? epc : "-", addr && addr[0] ? addr : "-",
         safeMode ? 1 : 0);
  } else {
    info("boot", "reset: %s safe=%d", resetReason ? resetReason : "-",
         safeMode ? 1 : 0);
  }
  info("boot", "heap=%lu", (unsigned long)ESP.getFreeHeap());
}

void info(const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  log('I', tag, fmt, args);
  va_end(args);
}

void warn(const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  log('W', tag, fmt, args);
  va_end(args);
}

void error(const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  log('E', tag, fmt, args);
  va_end(args);
}

void clear() {
  g_enabled = false;
  g_truncated = false;
  g_size = 0;
  LittleFS.remove(kCurrentPath);
  LittleFS.remove(kPreviousPath);
  Serial.println("[diag] logs cleared");
}

}  // namespace diag
