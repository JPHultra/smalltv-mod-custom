#pragma once
#include "ViewModel.h"
#include "ImageAssetFormat.h"

// Paint a whole view once. The panel has no framebuffer; every primitive is
// drawn in array order, back to front.
void viewRender(const ViewScreen& screen);
void viewRenderFrame(const ViewScreen& screen, uint32_t elapsedMs,
                     ViewImageSource* images);

// Repaint only the rows occupied by token text or usage cards. The strip
// canvas clips every primitive, so this is safe with overlapping layouts.
// Returns false when the caller must do a full viewRender instead.
bool viewRenderText(const ViewScreen& screen, uint32_t elapsedMs,
                    ViewImageSource* images);
bool viewRenderUsage(const ViewScreen& screen, uint32_t elapsedMs,
                     ViewImageSource* images);
