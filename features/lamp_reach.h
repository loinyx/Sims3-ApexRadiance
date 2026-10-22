#pragma once
#include "fast_math.h"
// Realistic lamp reach (12/10, option "alcanceRealistaDasLampadas"): the distance law of a lamp on an outdoor wall texel,
// shared by the room solve (level_light_share.cpp ReachShareImpl, wall texels) and the per-pixel lamps of outdoor objects,
// fences and windows (lot_light_bridge.cpp, ShaderPatches AppendLampLaw), so a column, a balcony band or a window frame takes
// a lamp's light by the same rule as the wall behind it. Pure functions, no game reads (offline harnesses include it).
#include <algorithm>
#include <cmath>

namespace LampReach {

// w = range (+0x130) x intensity (+0x10). r0 (the near part the game's law keeps) and R (where the light ends) grow with
// sqrt(w / 85): 3.5 m and 15 m for the garden post of the 12/10 captures (range 42.5 x intensity 2), within 2..5 m and
// 6..20 m. An unreadable w counts as 85.
inline void Radii(float w, float& r0, float& R) {
    if (!(w > 0.0f) || !ApexMath::IsFinite(w)) w = 85.0f;
    const float k = ApexMath::Sqrt(w / 85.0f);
    r0 = std::clamp(3.5f * k, 2.0f, 5.0f);
    R = std::clamp(15.0f * k, 6.0f, 20.0f);
}

// The share of the game's light an outdoor wall texel keeps at distance d from the lamp head: all of it up to r0, then
// (r0 / d)^2 x (1 - (d / R)^4)^2, none from R on.
inline float Share(float d, float r0, float R) {
    if (!(d < R)) return 0.0f;
    if (d <= r0) return 1.0f;
    const float q = d / R, win = 1.0f - q * q * q * q;
    return (r0 * r0) / (d * d) * win * win;
}

// The game's weight of the lamp on an outdoor wall texel per unit of the colour the per-pixel lamps keep (F0 x intensity x
// fade, lot_light_bridge ReadLamp): the texel is min(1, sum over the room's list of colour x Gain x N.l / d^2 x Share).
// Per entry of an outdoor room's light list 0.25 w = k2 0.075 x w x the street factor 3.333 (docs/engine/room-light-maps.md);
// Entries() = how many entries the lamp has. Fitted on 12/10 F7 wall atlas columns of one garden post (type 11, range 42.5,
// a story-0 lot lamp: 2 entries, 0.5 w): 85 at intensity 2 (the captures behind ReachShareImpl), 245.7 at intensity 3.40
// (20-31-19 / 20-31-21, within 2/255). UNVERIFIED: that the 3.333 applies to every wall sample, and to lamp types other than
// 11 (only the street-lamp class 0x006BE020 was read in full).
inline float EntryGain(float w) {
    if (!(w > 0.0f) || !ApexMath::IsFinite(w)) w = 85.0f;
    return 0.25f * w;
}
// Entries of a lamp in the outdoor rooms' (room 0) light lists, on every story (12/10, review: the walls weigh only the
// story-0 lot lamps twice):
//  - a lot lamp of story 0 (+0xD0 == 0): 2. The game gathers level 0's room 0 twice (0x006C6AB0) and the story share gives the
//    other stories' room 0 story 0's lamps twice (level_light_share.cpp OutdoorGather, "times"); F7 20-31-19 lists the
//    story-0 post (d0 0 in F8 20-32-07) twice for an upper story's wall, F8 20-32-07 evaluates it twice per story-3 sample;
//  - a lot lamp of another story: 1 (gathered once on its own story, shared once to the others);
//  - a world lamp (lot id 0, the street lamps): 1, from the world lights around the lot (0x006B66B0); F7 03-03-20 lists the
//    street lamps once and the story-0 type-3 lamps twice in story 0's room 0. Apex keeps the real lot id at that gather
//    (split_level_ground_light_patch.cpp), so a lot street lamp is not listed a third time (the 3 of F8 01:39 in
//    docs/engine/room-light-maps.md matches the zeroed GetLotID that patch's comment describes; UNVERIFIED for that capture).
// UNVERIFIED: a type-11 lot lamp of story 0 registered in a roofless room (not room 0) counts 2 here; the story share gives
// such lamps once (AddOutdoorLamps). Not seen in a capture.
inline float Entries(bool lotLamp, int story) { return lotLamp && story == 0 ? 2.0f : 1.0f; }
inline float Gain(float w, float entries) { return EntryGain(w) * entries; }

// The per-pixel lamps' laws (ShaderPatches AppendLampLaw) at d^2 from the lamp's head, before N.l and the cones, per unit of
// the colour the per-pixel lamps keep (F0 x intensity x fade), so a law evaluated on the CPU (a rig's record) matches them:
//  - reach: Gain(w, entries) / d^2 x min(1, r0^2 / d^2 x sat(1 - (d^2 / R^2)^2)^2), d^2 at least minD2 (the shader's cS.w);
//  - the first law: sat(1 - d^2 / radius^2)^2 (radius = 1.2 sqrt(range), 2..25 m, lot_light_bridge ReadLamp).
inline float PixelLaw(float d2, float w, float entries, float minD2 = 1e-4f) {
    float r0, R;
    Radii(w, r0, R);
    d2 = std::max(d2, minD2);
    const float q = d2 / (R * R), win = std::clamp(1.0f - q * q, 0.0f, 1.0f);
    return Gain(w, entries) / d2 * std::min(1.0f, r0 * r0 / d2 * win * win);
}
inline float FirstLaw(float d2, float radius) {
    const float s = std::clamp(1.0f - d2 / std::max(radius * radius, 1e-6f), 0.0f, 1.0f);
    return s * s;
}

} // namespace LampReach
