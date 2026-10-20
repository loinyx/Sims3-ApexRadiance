#pragma once
// Address-space monitor (development build; docs/features/frame-profiler.md, "Address space").
//
// TS3W is a 32-bit, large-address-aware game: 4 GB of addresses. When no large enough contiguous piece is left, big
// allocations fail (the save's "Error 12", DXVK's texture mappings, the script heap's growth). A helper thread walks the
// address space with VirtualQuery every 10 s (about 1-3 ms, off the game's threads) and keeps: free total and largest free
// block (below / above 2 GB), the five largest free blocks, reserved and committed memory by kind (images, mapped views,
// private; executable pages have unknown ownership), allocations by size class, the loaded images by size, the game
// allocator's own big blocks (count and bytes at [[0x011CB864]+0x488 / +0x48C]) and the session minimum of the largest
// free block with its time. One log line a minute ([AddressSpace]); the full table goes into the frame profiler report.
// Builds with development tools also write process/system memory samples to a bounded CSV every 10 seconds, even
// when the menu's developer mode is off. These are observations, not automatic memory optimizations.
#include <string>

namespace AddressSpace {

void Start(); // init thread, builds with development tools, independent of the menu's developer-mode setting
void Stop();  // FreeLibrary only
std::string ReportText(); // the latest snapshot and the session minimum, for the profiler report ("" before the first one)

} // namespace AddressSpace
