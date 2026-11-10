#pragma once
// Game memory meter (the Overview's Game memory tile). Credits: @loinyx
// TS3W.exe is 32-bit: what runs out is its address space (4 GB with large-address-aware), and a world save needs one
// large free block ("Error 12"; Room to save, memory_guard.h). The meter keeps the last 120 samples of used space and
// of the largest free block and draws them as a chart with the current values and a health state. Samples come from
// Room to save's worker (every 4 s) while it runs, else from the menu itself (every 2 s while the row is drawn).
#include <cstdint>

namespace MemoryMeter {

// free = free address space (bytes), largest = largest free block, reserved = what Room to save holds for world saves
void Push(uint64_t free, uint64_t largest, uint64_t reserved = 0); // any thread
// Render thread, inside the Overview's status card; true when the player clicked it (the caller opens Performance)
bool RenderTile();
// For the low-memory note: takes a sample when the last one is older than maxAgeMs (Room to save off), then tells
// whether the largest free block is low (under 320 MB) or critical (under 160 MB). Render thread.
struct Status {
    bool known = false, low = false, critical = false;
};
Status Check(unsigned maxAgeMs);

} // namespace MemoryMeter
