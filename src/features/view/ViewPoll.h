// ViewPoll.h — optional HTTP poll for the transient overlay.
//
// When view.pollSec > 0 and view.pollUrl is set, the device GETs that URL on
// the configured cadence. A 200 with a draw list shows the overlay; 204 or an
// empty body clears it. Invalid JSON leaves the current overlay alone.
#pragma once
#include "Settings.h"

void viewPollBegin(const Settings& s);
void viewPollService(const Settings& s);
void viewPollForce();
const String& viewPollError();
