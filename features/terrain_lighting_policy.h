#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Only the baked lamp RGB is adjusted. Native solar/shadow terms keep their scale.
namespace TerrainLightingPolicy {
// One daytime curve for outdoor lamp light: the share of its night strength a lamp keeps at night level n (0 day, 1 night),
// 3% in full daylight rising as smoothstep(0, 1, n) to exactly 1 at night. Every outdoor lamp term (walls, ground, lot
// pass, objects, fences, windows, roofs, water, outdoor lamps in roofed rooms' maps) takes this factor, so a lamp left on
// by day fades the same way on every surface. Visual tuning, not a measured ratio.
inline constexpr float kLampDayFloor = 0.03f;
inline float LampDay(float night) {
    if (!std::isfinite(night) || night <= 0.0f) return kLampDayFloor;
    if (night >= 1.0f) return 1.0f;
    const float s = night * night * (3.0f - 2.0f * night);
    return kLampDayFloor + (1.0f - kLampDayFloor) * s;
}
// The sun's part of the game's own lamp curve: sky+0x784 = M + (1 - M) x this (FUN_00C130B0, sunSine = sky+0x6B4): 1 with the sun 13.9
// degrees under the horizon, 0 at 2.3. The manmade light itself is no measure: M, about 0.25, stays all day
inline float SunLampTerm(float sunSine) { return std::isfinite(sunSine) ? std::clamp(-5.0f * sunSine - 0.2f, 0.0f, 1.0f) : 0.0f; }
// The level Apex's lamp terms take (switch sun_lamp_level). The sky's smooth night moves by frame time, not by clock: at dawn it fell
// from 1 to 0 in under 0.6 s with the sun 7 degrees down, and the lamps on the deck went to their 3% at once (live log of 05/11).
// Level alone in daylight, so by day the 3% stands
inline float LampLevel(float level, float sunSine) { return std::max(level, SunLampTerm(sunSine)); }
// The level then takes this long for its full range: the sky can still turn with the sun term at 0.4, and 1 to 0.4 in half a second is a
// snap of its own. In real time, so a clock stopped by Build mode cannot leave it halfway. Visual tuning, not a measured time
inline constexpr float kLampFadeSeconds = 8.0f;
inline float LampLevelStep(float was, float want, float seconds) {
    const float d = seconds / kLampFadeSeconds;
    return std::clamp(want, was - d, was + d);
}
// The lamp term of the ground (terrain, lot pass) before full night: gain x LampDay(n), 0 for invalid input
inline float GroundLampTotal(float night, float gain) {
    if (!std::isfinite(night) || !std::isfinite(gain) || gain < 0) return 0;
    return gain * LampDay(night);
}
// Surface atlases/point lamps (objects, fences, windows, snow on objects): the night strength x LampDay, bit exact at night.
inline float SurfaceLampGain(float night, float gain) {
    if (!std::isfinite(night) || !std::isfinite(gain) || night < 0 || gain < 0) return gain;
    if (night >= 1.0f) return gain;
    return gain * LampDay(night);
}
// The lamp light of furniture indoors, at its draw (DrawIndoorObject's strength, the Counters' CLC.y): the directional maps hold the
// rig's colour, about 0.35 of the room law the floor and walls show (0x0069F280 reads light vfunc+0x10), so a cabinet face beside a
// lit wall read a fifth of it. One gain, not in the maps: they clamp at 1 (22 of 40 kitchen tiles would clip) and Sims read them.
// Weighted by the night level: by day these maps also hold the sun through the windows. UNVERIFIED in game: g = 2 (estimate from the
// rebuilt kitchen maps; cabinet albedo unknown, A/B 2.0 against 2.9)
inline constexpr float kFurnitureLampGain = 2.0f;
inline float FurnitureLampGain(float night) {
    if (!std::isfinite(night) || night <= 0.0f) return 1.0f;
    return 1.0f + (kFurnitureLampGain - 1.0f) * std::min(night, 1.0f);
}
// ExteriorWall cK.x scales only baked lamp RGB. At full night the previous native x gain, bit exact; before it the wall
// takes gain x LampDay(n) in place of the game's own daytime factor (0 by day on the captured variant, F7 10:28:03).
inline float WallLampScale(float native, float night, float gain) {
    if (!std::isfinite(native) || native < 0 || !std::isfinite(night) || night < 0 || !std::isfinite(gain) || gain < 0) return native;
    if (night >= 1.0f) return native * gain;
    return gain * LampDay(night);
}
// Realistic lamp reach (12/10): the per-pixel lamps of objects, fences and windows then hold an outdoor wall texel's light
// (lamp_reach.h), so they take the outdoor walls' scale: the native wall lamp scale with the walls' gain as DrawWallGain
// applies it (walls on), or the native scale alone (walls off), times trim (an object slider over its default: 1 at the
// defaults, the same light as the wall behind).
inline float ReachLampScale(float native, float night, bool wallsOn, float wallGain, float trim) {
    if (!std::isfinite(trim) || trim < 0) trim = 1.0f;
    return (wallsOn ? WallLampScale(native, night, wallGain) : native) * trim;
}
inline float LampScale(float native, float night, float gain, bool squared) {
    if (!std::isfinite(native) || native < 0 || !std::isfinite(gain) || gain < 0 || !std::isfinite(night)) return native;
    const float n = std::clamp(night, 0.0f, 1.0f);
    const float weighted = 1.0f + (gain - 1.0f) * n;
    // Preserve the previous night formula, including its floating point rounding.
    if (n == 1) return native * (squared ? std::sqrt(weighted) : weighted);
    // Before full night the lamp term is gain x LampDay(n) whatever the game's own factor (0 by day on the captured
    // variants); the squared (multi-pass) variants square the constant.
    const float total = GroundLampTotal(n, gain);
    return squared ? std::sqrt(total) : total;
}
// The lot light pass (kReplacementHlsl): c3.x scales the terrain term and the lot's own map. Before full night it is set to
// gain x LampDay(n) and c31.x to the lot map's night gain, so the lot ground follows the same curve as the terrain around
// it; at full night the previous constants stay (night = false).
struct LotPassConstants {
    bool day = false;   // replace c3.x and c31.x (else the previous night constants)
    float c3x = 0;      // the lamp scale for the terrain and the lot map
    float lotGain = 1;  // c31.x
};
inline LotPassConstants LotPass(float night, float groundGain, float lotMapGain) {
    LotPassConstants c;
    if (!std::isfinite(night) || night >= 1.0f || !std::isfinite(groundGain) || groundGain < 0 || !std::isfinite(lotMapGain) || lotMapGain < 0) return c;
    c.day = true;
    c.c3x = GroundLampTotal(night, groundGain);
    c.lotGain = lotMapGain;
    return c;
}
inline bool DeferDayEdit(bool night, bool automaticDusk, bool user, bool force, bool worldLamp) {
    return !night && automaticDusk && !user && !force && !worldLamp;
}
inline int64_t PhaseDelay(int64_t configuredMs, bool editing) {
    return editing ? 0 : std::max<int64_t>(0, configuredMs);
}
inline std::size_t PreviewPriorityChunks(std::size_t total, bool editing, bool haveEye) {
    return editing && haveEye ? std::min<std::size_t>(total, 4) : 0;
}

enum class Phase { Day, Twilight, Night };
inline Phase LevelPhase(float level) {
    return level > .99f ? Phase::Night : level < .01f ? Phase::Day : Phase::Twilight;
}
// The factor a room solve gives outdoor lamps in roofed rooms (baked maps): exactly LampDay(0) or 1 at the settled
// endpoints, LampDay(level) at twilight. Rooms solved at another factor are sent again once an endpoint settles; nothing is
// solved again per frame.
inline float SolveLampDay(float level) {
    const Phase p = LevelPhase(level);
    return p == Phase::Night ? 1.0f : p == Phase::Day ? LampDay(0.0f) : LampDay(level);
}
// Settled endpoints only. Reversing direction cancels a stale delayed rebuild.
struct Cycle {
    Phase last = Phase::Day, target = Phase::Day;
    bool pending = false;
    int64_t due = 0;
    void Reset(float level) { last = target = LevelPhase(level); pending = false; due = 0; }
    void Observe(float level, int64_t now, int64_t delay, bool enabled, bool loading) {
        const Phase phase = LevelPhase(level);
        if (!enabled || loading || (pending && phase != target)) pending = false;
        if (enabled && !loading && phase != last && phase != Phase::Twilight) {
            target = phase;
            due = now + std::max<int64_t>(0, delay);
            pending = true;
        }
        last = phase;
    }
    bool Consume(int64_t now, Phase& phase) {
        if (!pending || now < due) return false;
        phase = target;
        pending = false;
        return true;
    }
};

// Outdoor rigs take their outdoor lamps by the per-pixel lamps' reach law (ObjectLightBridge ReachLawRec), whose scale
// carries the night level: the rigs gather again when the law is turned on or off (a negative scale = off) or its scale
// moved by more than 5% (about 70 gathers from day to night; between them a rig's lamps are at most 5% off).
inline constexpr float kRigLawStep = 0.05f;
inline bool RigLawChanged(float gathered, float now) {
    if (!std::isfinite(now)) return false;
    if ((gathered >= 0.0f) != (now >= 0.0f)) return true;
    if (now < 0.0f) return false;
    return std::fabs(now - gathered) > kRigLawStep * std::max(gathered, 1e-4f);
}

// The foliage vertex shader's lamp weight (PatchFoliageVs, VS c255): weight = max(N.L x c255.x + c255.y, 0).
//  - Rig lamps by the reach law (reachLaw): the plain Lambert term N.L, the rule the per-pixel lamps of objects, fences and
//    walls follow; the per-instance colours already carry the daytime curve (the law's scale), so no second factor;
//  - otherwise the wrap of 1.0.0 (N.L / 1.5 + 1/3) on the boosted colours, x LampDay(night) when dayFix.
inline void FoliageLampWeights(float night, bool reachLaw, bool dayFix, float out[2]) {
    if (reachLaw) {
        out[0] = 1.0f;
        out[1] = 0.0f;
        return;
    }
    const float day = dayFix ? LampDay(night) : 1.0f;
    out[0] = day * (1.0f / 1.5f);
    out[1] = day * (0.5f / 1.5f);
}
}
