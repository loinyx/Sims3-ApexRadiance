#pragma once
// Outdoor light through openings (level_light_share.cpp ShareRoofedOpenings): the scale an outdoor lamp of room 0 keeps in a
// roofed room's solve, so it lights the room behind a door, window or arch no more than it lights the same surface outside.
// The same lamp outdoors is drawn x the outdoor surface's brightness (the lot pass: the lot map x Ground brightness x Lot lamp
// brightness; the outside walls: the baked map x Walls brightness) and from an outdoor room, which the game does not
// normalise. Indoors the room solve summed it at full strength and the room's normalisation (room+0x160) multiplied it; the
// indoor draws then show the map at about the outdoor map's scale (the room solve's curve halves it, the room-lit floor
// shader's curve doubles it). A capture of a green sconce (intensity 1.7, story-0 room-0 lamp, listed twice) lit a kitchen
// behind a glass door at 0.808 on screen while the courtyard beside the lamp kept 0.75 x 0.756 = 0.567 of its map.
// Pure functions, no game reads (the offline harness tools/roofed_openings_test includes this header).
#include <algorithm>
#include <cmath>
#include "inner_retry.h"

namespace OpeningLampBalance {

// The outdoor scales at night (the sliders' values: the daytime curve is RoofedDay's). groundOn: the lot pass applies
// Ground brightness and Lot lamp brightness (Street lamps light lots on); wallsOn: Lamps light walls on.
struct Scales {
    float ground = 1.0f; // floors, ceilings and the directional maps
    float wall = 1.0f;   // wall texels
};
inline float Clamp(float v, float lo, float hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : 1.0f; }
inline Scales From(float groundBrightness, float lotLampBrightness, bool groundOn, float wallBrightness, bool wallsOn) {
    Scales s;
    if (groundOn) s.ground = Clamp(groundBrightness, 0.25f, 3.0f) * Clamp(lotLampBrightness, 0.25f, 3.0f);
    if (wallsOn) s.wall = Clamp(wallBrightness, 0.25f, 8.0f); // (LotLightBridge::SetWallGain's range)
    return s;
}
// A wall texel as the realistic reach tells it (|n.y| < 0.5); no normal (the directional maps): not a wall
inline bool WallTexel(const float* normal) { return normal && std::fabs(normal[1]) < 0.5f; }
// The factor on the lamp's colour at a texel of a roofed room of normalisation `norm`: the outdoor surface's scale over the
// room's normalisation (the lamp comes from an outdoor room, factor 1: InnerLight::NormScale(1, norm, false))
inline float Factor(const Scales& s, const float* normal, float norm, bool roomOutdoor) {
    return (WallTexel(normal) ? s.wall : s.ground) * InnerLight::NormScale(1.0f, norm, roomOutdoor);
}
// Outdoors a room-0 texel holds min(sum, 1) per channel (FUN_006a31d0 for a room solved as outdoor), so a lamp listed
// `times` in the list shows at most 1 there; an indoor sum has no such cut before the room's curve, and a saturated colour
// (one channel, a third of the curve's mean) kept growing up to twice that. Each entry of the lamp is held under 1 / times per
// channel before the opening's share, as outdoors.
inline void CapEntry(float* colour, int times) {
    const float cap = 1.0f / static_cast<float>(std::max(times, 1));
    for (int i = 0; i < 3; i++) colour[i] = std::min(colour[i], cap);
}
// The scales moved enough to solve the rooms again (the maps are baked)
inline bool Moved(const Scales& a, const Scales& b) { return std::fabs(a.ground - b.ground) > 0.005f || std::fabs(a.wall - b.wall) > 0.005f; }

} // namespace OpeningLampBalance
