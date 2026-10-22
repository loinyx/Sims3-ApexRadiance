#pragma once
// The night level (lightMgr+0xF0, 0 day, 1 night) in Build mode. Build mode switches the sky to sun times without the
// season: in a winter world at dusk sunrise / sunset (sky+0x974 / +0x978) went from 2.0 / -2.5 to 0 / 0 and the level the
// sky update hands the light manager fell from 1 to 0 within half a second, while the sky stayed dark and the clock
// stood still (Daylight logs). The game then faded its street lamps and every lamp factor it gives the terrain, road and
// lot shaders, and Apex's lamp terms took their daytime 3 % (F7 captures: lot pass c3.x 0.0225): the lawn and the
// sidewalks around lamps went dark and leaving Build mode armed a dusk rebuild of the terrain.
// Every day/night output of the sky comes from those times (static analysis of 1.67.2): FUN_00C130B0 measures the clock
// against them to move its smooth night sky+0x780 (.x; .y the manmade light), the "ShaderDayNightParameters" global the
// shaders read by pointer (registered at 0x00C10F1C; the lot pass c3.x is taken to be it: 0 with +0xF0 held at 1 in an F7
// capture), then hands .x / .y to the light manager (+0xF0 / +0xF4, FUN_00C11AD0) and so to every light's fade (0x006BC430:
// world lights F0, lot lights F4). The sun and moon directions (0x00C11970), the dominant light and shadow direction
// (0x00C15062), and the sky, ambient and fog colours (0x00C152F4) read them too. The sky copies them from the weather
// manager every update (0x00C15D9A -> FUN_00D68FE0); the active set +0x9EC is filled by game script
// (WeatherControl_SetSet, 0x007846E0 -> FUN_00D69900), which pushes 0 / 0 in Build mode (the script side is unverified:
// its assembly is encrypted). So SunTimesGate keeps the season's sunrise / sunset in that copy when it turns to 0 / 0
// while the clock stands still (night_terrain_relight_patch.cpp SunTimesHook): Build mode then shows sun, moon, shadows,
// sky colour and lamps exactly as live mode at that hour. A 0 / 0 lasting kSunGateHours of game time with the clock
// running (a season without offsets) is taken, as is any other pair, a new sky or weather manager (world load, travel,
// Edit Town) and the game's debug weather set (weather +0xB04). Nothing of it is saved: the copy is rewritten every
// frame and the weather set itself is not touched. When the copy could not be redirected, SunGate keeps the level Apex
// reads (+0xF0) when it falls together with a change of the sun times.
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

// The sky's copy of the sun times: keeps the season's while Build mode's 0 / 0 stands with the clock still
struct SunTimesGate {
    uintptr_t sky = 0, weather = 0;
    bool have = false, pending = false;
    float rise = 0.0f, set = 0.0f, pendingHour = 0.0f;
    // r / s: the times just copied (replaced with the kept ones when held); true when held. debug: the game's debug
    // weather set is in use (never held)
    bool Apply(uintptr_t skyAddr, uintptr_t weatherAddr, bool debug, float& r, float& s, float hour) {
        if (!have || skyAddr != sky || weatherAddr != weather || debug) {
            sky = skyAddr, weather = weatherAddr, have = true, pending = false, rise = r, set = s;
            return false;
        }
        if (r == rise && s == set) {
            pending = false;
            return false;
        }
        if (r == 0.0f && s == 0.0f) { // the seasonless times (Build mode)
            if (!pending) pending = true, pendingHour = hour;
            if (!std::isfinite(hour) || std::fabs(hour - pendingHour) < kSunGateHours) {
                r = rise, s = set;
                return true;
            }
        }
        pending = false, rise = r, set = s;
        return false;
    }
};

inline SRWLOCK g_lock = SRWLOCK_INIT;
inline SunGate g_gate;
inline SunTimesGate g_timesGate;
inline std::atomic<bool> g_timesHooked{false}; // the sky's copy of the sun times goes through g_timesGate (+0xF0 is then right)
inline std::atomic<bool> g_holding{false};

// The sky's copy of the sun times (render thread): r / s replaced by the season's while held
inline void GateSunTimes(uintptr_t sky, uintptr_t weather, bool debug, float& r, float& s, float hour) {
    AcquireSRWLockExclusive(&g_lock);
    g_holding.store(g_timesGate.Apply(sky, weather, debug, r, s, hour), std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_lock);
}

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
    level = g_timesHooked.load(std::memory_order_relaxed) ? f0 : Gate(lightMgr, f0);
    return true;
}

// A gate is keeping the season's sun times or the level of before (log and developer status)
inline bool Holding() { return g_holding.load(std::memory_order_relaxed); }

} // namespace NightLevel
