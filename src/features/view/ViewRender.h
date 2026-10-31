#pragma once
#include "ViewModel.h"

// Paint a whole view once. The panel has no framebuffer; every primitive is
// drawn in array order, back to front.
void viewRender(const ViewScreen& screen);

// Repaint only token text (clock/date) when nothing underneath it changed.
// Returns false when the caller must do a full viewRender instead.
bool viewRenderClock(const ViewScreen& screen);
