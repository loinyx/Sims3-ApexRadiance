#pragma once
// The night level Apex works with (0 day, 1 night): the light manager's +0xF0, except when it falls while the game's
// clock stands still. Build mode switches the sky to sun times without the season: in a winter world at dusk sunrise /
// sunset went from 2.0 / -2.5 to 0 / 0 (sky+0x974 / +0x978) and +0xF0 from 1 to 0 within half a second, while the sky
// stayed dark and the clock (lightMgr+0xB4) stood still (Daylight logs). Every Apex lamp term took its
// daytime 3 % (the lawn around a lamp dark in Build mode, F7 captures: lot pass c3.x 0.0225) and leaving
// Build mode armed a dusk rebuild of the terrain. Night only turns to day with the clock moving (dawn), so a fall with the
// clock unchanged keeps the level of before; a rise is always taken (the 0 to 1 ramp of a world loaded at night runs
// with the clock still). The weather manager's byte +0xB04, which picks the sun time set (FUN_00D68FE0), read 0 in Build
// mode with both sets at 0 / 0: it is not used.
// Render thread and solve threads: the kept values are atomics (a torn pair only delays a change by one read).
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace NightLevel {

inline std::atomic<uintptr_t> g_keptMgr{0};
inline std::atomic<float> g_kept{0.0f}, g_keptHour{-1.0f};
inline std::atomic<bool> g_holding{false};

// lightMgr+0xF0 and the clock (+0xB4, hours); false when unreadable
inline bool ReadRaw(uintptr_t lightMgr, float& f0, float& hour) {
    __try {
        f0 = *reinterpret_cast<const float*>(lightMgr + 0xF0);
        hour = *reinterpret_cast<const float*>(lightMgr + 0xB4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The level Apex uses; false when the light manager is unreadable
inline bool Read(uintptr_t lightMgr, float& level) {
    float f0 = 0.0f, hour = 0.0f;
    if (!lightMgr || !ReadRaw(lightMgr, f0, hour) || !std::isfinite(f0)) return false;
    const float kept = g_kept.load(std::memory_order_relaxed);
    const bool sameClock = std::isfinite(hour) && std::fabs(hour - g_keptHour.load(std::memory_order_relaxed)) < 1e-4f;
    if (g_keptMgr.load(std::memory_order_relaxed) == lightMgr && sameClock && f0 < kept) {
        level = kept;
        g_holding.store(true, std::memory_order_relaxed);
        return true;
    }
    g_kept.store(f0, std::memory_order_relaxed);
    g_keptHour.store(hour, std::memory_order_relaxed);
    g_keptMgr.store(lightMgr, std::memory_order_relaxed);
    g_holding.store(false, std::memory_order_relaxed);
    level = f0;
    return true;
}

// The last Read kept the level of before (developer status and log)
inline bool Holding() { return g_holding.load(std::memory_order_relaxed); }

} // namespace NightLevel
