#pragma once
// Pause lot streaming in map view (docs/features/performance/lot-streaming.md).

#include <string>

namespace LotLodStreaming {

// While map view is open (plus a 1 s exit grace), Apex sets the live WorldManager+0x258 "skip lot streaming" gate
// without detouring WorldManager::Update. The previous byte is restored safely when the grace ends or the feature is
// switched off. If S3SS owns LotStreamingOptimizations.mapViewBlocker, Apex makes no writes.
bool StartMapViewBlocker(std::string* error = nullptr);
void StopMapViewBlocker();
void TickMapViewBlocker();
bool MapViewBlockerRunning();
bool MapViewBlockerHandledByS3SS();
std::string MapViewBlockerStatusText();

} // namespace LotLodStreaming
