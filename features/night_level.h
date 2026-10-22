#pragma once
// The night level Apex works with (0 day, 1 night): the light manager's +0xF0, except while the game uses its seasonless sun
// times. The sky asks the weather manager for sunrise and sunset (FUN_00D68FE0, called at 0x00C15D9A with ECX =
// *(*(sky+0x21C)+0x74)): its byte +0xB04 picks the set at +0xA48 instead of the season's at +0x9EC. Build mode sets it
// (21/10 Daylight log: sunrise / sunset 2.0 / -2.5 in winter live, 0 / 0 in Build mode at 18:57), so +0xF0 went from 1 to 0
// in half a second while the sky stayed dark and the clock stood still: every Apex lamp term took its daytime 3 % (the lawn
// around a lamp dark in Build mode, F7 21-01-35 c3.x 0.0225) and leaving Build mode armed a dusk rebuild of the terrain.
// While the byte is set, the level last read with it clear (same light manager) is kept; with no such reading, +0xF0.
// Render thread and solve threads: the kept level is an atomic.
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace NightLevel {

inline std::atomic<uintptr_t> g_keptMgr{0};
inline std::atomic<float> g_kept{0.0f};
inline std::atomic<bool> g_holding{false};

// lightMgr+0xF0 and whether the seasonless sun times are in use; false when the light manager is unreadable
inline bool ReadRaw(uintptr_t lightMgr, float& f0, bool& seasonless) {
    seasonless = false;
    __try {
        f0 = *reinterpret_cast<const float*>(lightMgr + 0xF0);
        const uintptr_t back = *reinterpret_cast<const uintptr_t*>(lightMgr + 0xD0);
        if (back < 0x10000 + 0x210) return true;
        const uintptr_t sky = back - 0x210;
        if (*reinterpret_cast<const uintptr_t*>(sky + 0x1F4) != lightMgr) return true;
        const uintptr_t holder = *reinterpret_cast<const uintptr_t*>(sky + 0x21C);
        const uintptr_t weather = holder >= 0x10000 ? *reinterpret_cast<const uintptr_t*>(holder + 0x74) : 0;
        if (weather >= 0x10000) seasonless = *reinterpret_cast<const BYTE*>(weather + 0xB04) != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The level Apex uses; false when the light manager is unreadable
inline bool Read(uintptr_t lightMgr, float& level) {
    float f0 = 0.0f;
    bool seasonless = false;
    if (!lightMgr || !ReadRaw(lightMgr, f0, seasonless)) return false;
    if (!std::isfinite(f0)) return false;
    if (seasonless && g_keptMgr.load(std::memory_order_relaxed) == lightMgr) {
        level = g_kept.load(std::memory_order_relaxed);
        g_holding.store(true, std::memory_order_relaxed);
        return true;
    }
    if (!seasonless) {
        g_kept.store(f0, std::memory_order_relaxed);
        g_keptMgr.store(lightMgr, std::memory_order_relaxed);
    }
    g_holding.store(false, std::memory_order_relaxed);
    level = f0;
    return true;
}

// The last Read kept the level of before the seasonless sun times (developer status)
inline bool Holding() { return g_holding.load(std::memory_order_relaxed); }

} // namespace NightLevel
