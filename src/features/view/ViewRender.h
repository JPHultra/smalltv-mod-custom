#pragma once
#include "ViewModel.h"

// Paint a whole view once. The panel has no framebuffer; every primitive is
// drawn in array order, back to front.
void viewRender(const ViewScreen& screen);
