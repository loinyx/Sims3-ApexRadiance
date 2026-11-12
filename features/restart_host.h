#pragma once
// Restart host ([ui] restart_host, off by default; no menu option yet). Apex starts ts3wrap.exe (native/ts3wrap, next to
// ApexRadiance.asi) with the game's process id and keeps a small shared block for it. After each world save it records
// whether the largest free block was under 320 MB. When the game then quits normally, the host asks the player whether
// to start it again with that save (EA's -worldFile switch). Nothing changes while it is off or the exe is missing.
// Adapted from Dalexlol's sims3-modernization wrapper (PRs #360 and #363). Credits: @loinyx
#include <cstdint>

namespace RestartHost {

void Start();                                    // after LoadSettings
void AfterSave(bool ok, uint64_t largestBytes);  // Room to save's world-save hook (largest free block with its reserve held)

} // namespace RestartHost
