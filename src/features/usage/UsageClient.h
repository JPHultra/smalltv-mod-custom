// UsageClient.h — the device side of the usage daemon contract.
//
// The daemon POSTs one normalized document to /api/usage. The legacy Claude
// payload ({s,sr,w,wr,st,ok}) is still accepted and mapped to a "claude"
// provider, so an existing clawdmeter-daemon keeps working.
#pragma once
#include "Settings.h"
#include "UsageData.h"

void usageInit();
bool usageApply(const String& body);

const ProviderUsage* usageFind(const char* id);
bool usageStale(const ProviderUsage& provider, uint32_t withinMs = USAGE_STALE_MS);

// Bumped on every accepted push; the dashboard/overlay use it to repaint only
// when something actually arrived.
uint32_t usageRevision();

// /api/status and /api/usage reply shape.
void usageStatusJson(JsonObject& root);
