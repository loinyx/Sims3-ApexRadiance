#pragma once
// Game allocation categories (numbered development builds only: APEX_DEVELOPMENT_BUILD_FLAVOR).
//
// Every native allocation of TS3W.exe goes through three cdecl entry points of the general allocator ([0x011CB864]):
//   0x004E3F90 Alloc(size, name, flags)                        -> allocator 0x004E69D0(size, flags)
//   0x004E3FB0 AllocAligned(size, align, offset, name, flags)  -> allocator 0x004E6A20(size, align, offset, flags)
//   0x004E3F70 Free(ptr)                                       -> allocator 0x004E5920(ptr)
// The callers pass an EA memory-category name ("Lot/FloorManager", "CAS/TextureCompositorResource", ...; about 4000 call
// sites) that the retail build ignores. Hooking the three entries attributes the game's heap by category: blocks of 1 KB
// and more are remembered (8-way buckets, 1M entries) so frees subtract from the right category; smaller blocks only count
// allocations. Generic EASTL containers pass a header path instead of a name and are grouped by caller address.
// Steam 1.67.2 only (fixed addresses, prologue bytes checked); hooks installed with every other thread suspended.
// Observation only: sizes, flags and results are passed through unchanged.
#include <string>

namespace GameAllocCategories {

bool Install();        // init thread; false when not compiled in, not the Steam executable or the bytes differ
bool Active();
void Report(unsigned long long uptimeMs); // address-space thread, about once a minute: CSV + log line
std::string ReportText();

} // namespace GameAllocCategories
