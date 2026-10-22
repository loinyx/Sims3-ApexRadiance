#pragma once
// The night level (lightMgr+0xF0, 0 day, 1 night) in Build mode. Build mode switches the sky to sun times without the
// season: in a winter world at dusk sunrise / sunset (sky+0x974 / +0x978) went from 2.0 / -2.5 to 0 / 0 and the level the
// sky update hands the light manager fell from 1 to 0 within half a second, while the sky stayed dark and the clock
// stood still (Daylight logs). The game then faded its street lamps and every lamp factor it gives the terrain, road and
// lot shaders, and Apex's lamp terms took their daytime 3 % (F7 captures: lot pass c3.x 0.0225): the lawn and the
// sidewalks around lamps went dark and leaving Build mode armed a dusk rebuild of the terrain.
// SunGate keeps the level of before when it falls together with a change of the sun times (a season shift while the clock
// runs is let through after kSunGateHours of game time). The sky update's call of the level setter (FUN_00C11AD0 at
// 0x00C11B40 -> 0x006ADD60, night_terrain_relight_patch.cpp NightSetHook) goes through it, so the game keeps its night;
// Read applies the same gate to +0xF0 when that call could not be redirected.
// The weather manager's byte +0xB04 (FUN_00D68FE0 picks the sun time set by it) read 0 in Build mode with both sets at
// 0 / 0: it does not tell Build mode apart and is not used.
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace NightLevel {

constexpr float kSunGateHours = 0.25f; // game hours a held level outlasts a running clock

struct SunGate {
    uintptr_t mgr = 0;
    bool have = false, holding = false;
    float level = 0.0f, sunrise = 0.0f, sunset = 0.0f, holdHour = 0.0f;
    // The level to use for `wanted` with the sky's sun times and the clock now
    float Decide(uintptr_t lightMgr, float wanted, float rise, float set, float hour) {
        if (!have || lightMgr != mgr || !std::isfinite(wanted)) {
            mgr = lightMgr, have = true, holding = false, level = wanted, sunrise = rise, sunset = set;
            return wanted;
        }
        const bool sunChanged = rise != sunrise || set != sunset;
        if (wanted < level && sunChanged) {
            if (!holding) holding = true, holdHour = hour;
            if (!std::isfinite(hour) || std::fabs(hour - holdHour) < kSunGateHours) return level;
        }
        holding = false, level = wanted, sunrise = rise, sunset = set;
        return wanted;
    }
};

inline SRWLOCK g_lock = SRWLOCK_INIT;
inline SunGate g_gate;
inline std::atomic<bool> g_setterHooked{false}; // the game's setter goes through the gate (then +0xF0 is already right)
inline std::atomic<bool> g_holding{false};

// The sky's sun times and the clock (lightMgr+0xB4, hours); false when unreadable
inline bool ReadSun(uintptr_t lightMgr, float& rise, float& set, float& hour) {
    __try {
        hour = *reinterpret_cast<const float*>(lightMgr + 0xB4);
        const uintptr_t back = *reinterpret_cast<const uintptr_t*>(lightMgr + 0xD0);
        if (back < 0x10000 + 0x210) return false;
        const uintptr_t sky = back - 0x210;
        if (*reinterpret_cast<const uintptr_t*>(sky + 0x1F4) != lightMgr) return false;
        rise = *reinterpret_cast<const float*>(sky + 0x974);
        set = *reinterpret_cast<const float*>(sky + 0x978);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The gate for a level about to be used or set; `wanted` as it is when the sky cannot be read
inline float Gate(uintptr_t lightMgr, float wanted) {
    float rise = 0.0f, set = 0.0f, hour = 0.0f;
    if (!lightMgr || !ReadSun(lightMgr, rise, set, hour)) return wanted;
    AcquireSRWLockExclusive(&g_lock);
    const float out = g_gate.Decide(lightMgr, wanted, rise, set, hour);
    g_holding.store(g_gate.holding, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_lock);
    return out;
}

inline bool ReadF0(uintptr_t lightMgr, float& f0) {
    __try {
        f0 = *reinterpret_cast<const float*>(lightMgr + 0xF0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The level Apex uses: +0xF0 (through the gate when the game's setter is not redirected); false when unreadable
inline bool Read(uintptr_t lightMgr, float& level) {
    float f0 = 0.0f;
    if (!lightMgr || !ReadF0(lightMgr, f0) || !std::isfinite(f0)) return false;
    level = g_setterHooked.load(std::memory_order_relaxed) ? f0 : Gate(lightMgr, f0);
    return true;
}

// The gate is keeping the level of before (log and developer status)
inline bool Holding() { return g_holding.load(std::memory_order_relaxed); }

} // namespace NightLevel
