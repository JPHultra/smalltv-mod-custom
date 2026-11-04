// UpdateStatus.h — small, readable state for the firmware update UI.
#pragma once
#include <Arduino.h>

namespace update_status {

enum Phase {
  IDLE,
  CHECKING,
  DOWNLOADING,
  INSTALLING,
  REBOOTING,
  DONE,
  ERROR,
};

struct Snapshot {
  bool active = false;
  Phase phase = IDLE;
  const char* phaseName = "idle";
  int progress = -1;          // -1 when total is unknown
  uint32_t downloaded = 0;
  uint32_t total = 0;
  String target;              // release tag, when known
  String message;
  String error;
};

void setChecking(const String& target = String());
void setPhase(Phase phase, const char* message = nullptr);
void setProgress(uint32_t downloaded, uint32_t total);
void setTarget(const String& target);
void fail(const String& message);
void complete(const String& message = String());
void bootResult(const String& message);
Snapshot snapshot();

}  // namespace update_status
