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
// Outdoors a room-0 texel's sum is divided by its largest channel past 1 (FUN_006a31d0 for a room solved as outdoor, its hue
// kept: OutdoorDeltaModel::KeepHue), so a lamp listed `times` in the list shows at most 1 there; an indoor sum has no such
// cut before the room's curve, and a saturated colour (one channel, a third of the curve's mean) kept growing up to twice
// that. Each entry of the lamp is held under 1 / times at its largest channel before the opening's share, as outdoors; cut
// channel by channel, a strong orange (1.7, 0.85, 0.26) entered as (0.5, 0.5, 0.26), nearly white.
inline void CapEntry(float* colour, int times) {
    const float cap = 1.0f / static_cast<float>(std::max(times, 1));
    const float m = std::max({colour[0], colour[1], colour[2]});
    if (m > cap && std::isfinite(m))
        for (int i = 0; i < 3; i++) colour[i] *= cap / m;
}

// The two curves an indoor texel goes through both scale its colour by a function of the MEAN of its channels, so a
// saturated colour (one channel lit, a mean a third of it) is compressed as if it were three times dimmer than a white
// light of the same channel; outdoors the map is linear up to its hue-kept cut at the largest channel. The room solve's
// curve (FUN_006a31d0 at 0x0069EC40, GpuLampLaw::Tone): c = x tanh(m / 2) / m for a mean m >= 0.01, else x. The room-lit
// floor shader's (PS 44ECECA0 and 981C53E8, captures of a kitchen floor): y x (2 / (1 + 2^(-4.2 m')) - 1) / m', m' = 0.333
// (r + g + b) of y = map x albedo, a D3D `exp` being 2^x: y tanh(K m') / m' with K = 4.2 ln 2 / 2. The walls' shader is taken
// to match the floor's (unverified).
constexpr float kFloorK = 1.4556343f;  // 4.2 ln 2 / 2
inline float SolveGain(float m) { return m >= 0.01f ? std::tanh(0.5f * m) / m : 1.0f; }
// The scale on an outdoor lamp's share of a roofed room's texel (x: the room's normalisation x its entries, as the solve's
// curve takes it) so that both curves give it the response of its largest channel, the response a white light of that
// channel gets, as outdoors: a white share keeps 1, a saturated one less (at most 1). Taken as if the share lit the texel
// alone (a lit room around it compresses it more, so the share is then dimmed a little more than needed); the floor's
// albedo is taken as 1 (a darker floor needs less, so the same).
inline float MaxResponse(const float c[3], float x) {
    const float X[3] = {c[0] * x, c[1] * x, c[2] * x};
    const float M = std::max({X[0], X[1], X[2]});
    const float m = (X[0] + X[1] + X[2]) / 3.0f;
    if (!(M >= 0.01f) || !std::isfinite(M) || !(m > 0.0f) || m >= M) return 1.0f; // (both curves leave it alone, or a white light)
    const float r = m / M;
    // the floor: q2 on the solve's colour so that tanh(K q2 r Mc) = r tanh(K Mc), Mc the solve's largest channel for the white
    const float gM = SolveGain(M);
    const float Mc = std::min(M * gM, 1.0f); // (the pack's cut at 1)
    const float q2 = std::atanh(std::min(r * std::tanh(kFloorK * Mc), 0.999999f)) / (kFloorK * r * Mc);
    // the solve: x' = q x with T(x') = q2 x gM (the game's curve, its step under a mean of 0.01 included)
    const float t = q2 * gM;
    float q = t; // (the identity under a mean of 0.01)
    if (t * m >= 0.01f) q = 2.0f * std::atanh(std::min(m * t, 0.999999f)) / m;
    return std::isfinite(q) ? std::clamp(q, 0.0f, 1.0f) : 1.0f;
}
// The scales moved enough to solve the rooms again (the maps are baked)
inline bool Moved(const Scales& a, const Scales& b) { return std::fabs(a.ground - b.ground) > 0.005f || std::fabs(a.wall - b.wall) > 0.005f; }

} // namespace OpeningLampBalance
