#pragma once
// Physical indoor lamps, stage 4 (docs/features/night-lighting/gpu-room-lamps.md "Physical model"): which lot lamps a kept
// map draws, with which room row, and whether a lamp edit skips the room solves. No game access (tools/outdoor_delta_test).
//   - A kept map draws every lamp of the active lot with a GPU law whose reach meets its texels' box, not only the lamps its
//     solve listed: a lamp moved into the room, switched on, or new lights it at once.
//   - Never twice: a light in the solve's base (`baked`: no law when kept) is never drawn; a listed lamp's measured part left the
//     base, so it is drawn; a lamp the solve did not list has nothing in the base, so it is drawn too.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "gpu_lamp_law.h"
#include "inner_retry.h"
#include "outdoor_delta_model.h"

namespace PhysLampPick {

// A map's texels' box (world space)
struct Box {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    void Grow(const float p[3]) {
        for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
    }
    bool Empty() const { return lo[0] > hi[0]; }
};

// The squared distance from the lamp's position beyond which it adds nothing to a texel: ReachBound2 of its law with W times
// `scale` (its room row's mult x norm scale, at least 1), a TubeLight's segment and an area light's size added to the distance.
// -1: no bound (always drawn); 0: it adds nothing (off)
inline float Reach2(const GpuLampLaw::Lamp& L, float scale, float thr) {
    const float esum = (L.colour[0] + L.colour[1]) + L.colour[2];
    const float spill = std::max({L.spill[0], L.spill[1], L.spill[2]});
    const float d2 = OutdoorDeltaModel::ReachBound2(L.W * std::max(scale, 1.0f), esum, spill, thr);
    if (d2 <= 0.0f) return d2;
    float ext = 0.0f;
    if (L.sc2 == GpuLampLaw::kTube) ext = std::sqrt(L.axis[0] * L.axis[0] + L.axis[1] * L.axis[1] + L.axis[2] * L.axis[2]);
    else if (L.sc2 < -1.5f) ext = std::sqrt(L.r0sq * L.r0sq + L.invR2 * L.invR2) + std::sqrt(std::fabs(L.r0sq)); // (area: its sizes, either form)
    if (!std::isfinite(ext)) return -1.0f;
    const float r = std::sqrt(d2) + ext;
    return r * r;
}
// The lamp's reach (r2 as Reach2 gives it) meets the box
inline bool Meets(const Box& b, const float c[3], float r2) {
    if (r2 < 0.0f) return true;
    if (r2 == 0.0f || b.Empty()) return false;
    float d2 = 0.0f;
    for (int k = 0; k < 3; k++) {
        const float d = c[k] < b.lo[k] ? b.lo[k] - c[k] : c[k] > b.hi[k] ? c[k] - b.hi[k] : 0.0f;
        d2 += d * d;
    }
    return d2 <= r2;
}

// A lot lamp in a kept solve: in its base (never drawn), listed (its part left the base: drawn with the solve's row), or fresh
// (not in the solve: nothing of it in the base, drawn with a row of its own)
enum class Use { Base, Listed, Fresh };
inline Use UseOf(uintptr_t light, const std::vector<uintptr_t>& listed, const std::vector<uintptr_t>& baked) {
    if (std::find(baked.begin(), baked.end(), light) != baked.end()) return Use::Base;
    return std::find(listed.begin(), listed.end(), light) != listed.end() ? Use::Listed : Use::Fresh;
}
// A fresh lamp's norm scale in a room (outdoor_delta.cpp NormKOf's lookup without the record's innerSrc, which holds only
// lamps it gathered): a lamp registered in another room of the same story shines at that room's normalisation against this
// one's; else 1
inline float NormKFresh(int lampStory, int lampHome, float homeNorm, int roomStory, int roomId, float hereNorm, bool hereOutdoor) {
    if (lampStory != roomStory || lampHome <= 0 || lampHome == roomId || !std::isfinite(homeNorm) || !std::isfinite(hereNorm)) return 1.0f;
    return InnerLight::NormScale(homeNorm, hereNorm, hereOutdoor);
}
// A fresh lamp's realistic reach share (LevelLightShare::ReachAppliesIn without the record's sets): an outdoor room takes it
// from every lamp, a roofed room from an outdoor lamp (one through its openings)
// shortcut: the openings and open-top tests of ReachAppliesIn are not read for a lamp the solve did not gather; its next solve
// (the settle of a switch, or any later one) gives the exact row
inline bool ReachFresh(bool lampOutdoor, bool roomOutdoor) { return roomOutdoor || lampOutdoor; }

// ---- a lamp edit without the room solves ----
// One room of the lot as the edit finds it (story -4 .. 7)
struct RoomFact {
    int story = 0, id = 0;
    bool lists = false;  // its light list holds the lamp
    bool kept = false;   // (id > 0) a kept physical solve of the class it shows
    bool drawn = false;  // that solve drawn on every map it wrote (room 0: drawn physically, every story covered)
    bool baked = false;  // that solve has the lamp's light in its base
};
// Null when every story 0 .. 7 draws the lamp from its law now (the edit marks no room); else why not, `at` the room
inline const char* EditRefusal(const std::vector<RoomFact>& rooms, int& at) {
    bool any = false;
    for (at = 0; at < static_cast<int>(rooms.size()); at++) {
        const RoomFact& f = rooms[at];
        if (f.story < 0 || f.story > 7) {
            if (f.lists) return "a basement room lists it (basements keep the game's solves)";
            continue;
        }
        any = true;
        if (f.id == 0) {
            if (!f.drawn) return "its room 0 is not drawn physically on every story";
            continue;
        }
        if (!f.kept) return "no kept physical solve of the class it shows";
        if (!f.drawn) return "a map of its kept solve not drawn on the GPU";
        if (f.baked) return "its kept solve has the lamp in its base";
    }
    at = -1;
    return any ? nullptr : "no story of the lot found";
}

} // namespace PhysLampPick
