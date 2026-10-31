#pragma once
#include "ViewModel.h"

// Paint a whole view once. The panel has no framebuffer; every primitive is
// drawn in array order, back to front.
void viewRender(const ViewScreen& screen);

// Repaint only the rows occupied by token text or usage cards. The strip
// canvas clips every primitive, so this is safe with overlapping layouts.
// Returns false when the caller must do a full viewRender instead.
bool viewRenderText(const ViewScreen& screen);
bool viewRenderUsage(const ViewScreen& screen);
