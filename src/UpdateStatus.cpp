#include "UpdateStatus.h"

namespace update_status {
namespace {

Snapshot g_status;

const char* phase_name(Phase phase) {
  switch (phase) {
    case CHECKING:    return "checking";
    case DOWNLOADING: return "downloading";
    case INSTALLING:  return "installing";
    case REBOOTING:   return "rebooting";
    case DONE:        return "done";
    case ERROR:       return "error";
    default:          return "idle";
  }
}

bool is_active(Phase phase) {
  return phase == CHECKING || phase == DOWNLOADING || phase == INSTALLING ||
         phase == REBOOTING;
}

void clear_details() {
  g_status.progress = -1;
  g_status.downloaded = 0;
  g_status.total = 0;
}

}  // namespace

void setChecking(const String& target) {
  g_status = Snapshot();
  g_status.active = true;
  g_status.phase = CHECKING;
  g_status.phaseName = phase_name(CHECKING);
  g_status.target = target;
  g_status.message = F("Checking GitHub releases");
}

void setPhase(Phase phase, const char* message) {
  g_status.phase = phase;
  g_status.phaseName = phase_name(phase);
  g_status.active = is_active(phase);
  if (message) g_status.message = message;
  if (phase != DOWNLOADING && phase != INSTALLING) clear_details();
  if (!is_active(phase)) g_status.error = "";
}

void setProgress(uint32_t downloaded, uint32_t total) {
  g_status.downloaded = downloaded;
  g_status.total = total;
  if (total > 0) {
    uint32_t percent = downloaded * 100 / total;
    g_status.progress = percent > 100 ? 100 : (int)percent;
  } else {
    g_status.progress = -1;
  }
}

void setTarget(const String& target) {
  g_status.target = target;
}

void fail(const String& message) {
  g_status.phase = ERROR;
  g_status.phaseName = phase_name(ERROR);
  g_status.active = false;
  g_status.error = message;
  g_status.message = message;
  clear_details();
}

void complete(const String& message) {
  g_status.phase = DONE;
  g_status.phaseName = phase_name(DONE);
  g_status.active = false;
  g_status.error = "";
  if (message.length()) g_status.message = message;
  clear_details();
}

void bootResult(const String& message) {
  if (!message.length()) return;
  if (message.indexOf("already up to date") >= 0 ||
      message.indexOf("server reported no update") >= 0) {
    complete(message);
  } else {
    fail(message);
  }
}

Snapshot snapshot() { return g_status; }

}  // namespace update_status
