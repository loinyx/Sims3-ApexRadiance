#pragma once
// The soft edge a lamp's size gives a wall's opening, end or top (2.12.4). A lamp is a ball of radius R (Shadow softness in
// the menu; the game keeps no bulb or shade size). A point Q sees it through the cone from Q to the ball: where the ray
// from the lamp (t = 0) to Q (t = 1) crosses an occluder's edge, that cone is a disc of radius R (1 - t) across the ray
// (similar triangles), so a point right behind a jamb sees a sharp edge and one far behind it a wide one. The share of the
// disc on the open side of an edge at signed distance x is taken as smoothstep(x / 2h + 1/2) (h the half width): the
// cumulative of a parabolic kernel, so the weight of any stretch [a, b] of a wall is Cover(c - a) - Cover(c - b), and
// stretches side by side add up with no seam. Measured on the wall, the disc stretches by the ray's slant (Slant).
// No allocation, no game reads: the offline harnesses test it directly (tools/roofed_openings_test).
#include <algorithm>
#include <atomic>
#include <cmath>

namespace Penumbra {

constexpr float kMinHalf = 0.02f;       // m: the narrowest half rim (the 4 cm rim openings had before 2.12.4)
constexpr float kMaxHalf = 1.0f;        // m: the widest half rim on a wall (grazing rays); keeps a grid walk within 3 cells
constexpr float kDefaultRadius = 0.15f; // m: a small shade (the 0.3 m lantern the slab edges assumed)
constexpr float kMinRadius = 0.02f, kMaxRadius = 0.5f;

inline std::atomic<float> g_radius{kDefaultRadius};
// The developer Tester tab's switch (TesterToggles item "penumbra"): off, every edge keeps the narrowest rim (the 4 cm rim of
// 2.12.3, no texel spread) whatever the radius
inline std::atomic<bool> g_on{true};
inline float Radius() { return g_on.load(std::memory_order_relaxed) ? g_radius.load(std::memory_order_relaxed) : kMinRadius; }
inline void SetRadius(float r) { g_radius.store(std::clamp(std::isfinite(r) ? r : kDefaultRadius, kMinRadius, kMaxRadius), std::memory_order_relaxed); }

// The share (0..1) of a spot of half width h on the open side of an edge at signed distance x (x > 0: open side)
inline float Cover(float x, float h) {
    const float t = std::clamp(x / (2.0f * std::max(h, 1e-6f)) + 0.5f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
// The weight of the stretch [a, b] in a spot of half width h centred at c
inline float Weight(float c, float h, float a, float b) { return Cover(c - a, h) - Cover(c - b, h); }

// The penumbra of a slab's edge (soft_slab_edges), unlike Cover() not a smooth ramp across one straight edge at a known
// distance but a count over an opening known only as open or closed per point: the lamp's disk of `radius` seen from a point
// crosses the slab's plane within (1 - t) x radius of the ray's crossing (t: the crossing's place from the lamp, 0..1); the
// share of the lamp passing is the share of 5 points there (the crossing and 4 around it) that open(x, z) finds open (lot
// space). Tested in tools/stairwell_bounce_test.
template <class Open> float PenumbraShare(float x, float z, float t, float radius, Open open) {
    const float r = radius * (1.0f - t);
    const int n = (open(x, z) ? 1 : 0) + (open(x + r, z) ? 1 : 0) + (open(x - r, z) ? 1 : 0) + (open(x, z + r) ? 1 : 0) + (open(x, z - r) ? 1 : 0);
    return static_cast<float>(n) / 5.0f;
}

// The half width of the lamp's disc across the ray at t (0 lamp, 1 point). aa: the receiver's own half texel when it is a
// light map (0 otherwise): a map stores the light at its texel centres, so an edge sharper than a texel or two draws as
// texel stairs; aa t is the width at the occluder that spreads the edge over aa at the point.
inline float Half(float radius, float t, float aa = 0.0f) {
    if (!g_on.load(std::memory_order_relaxed)) return kMinHalf;
    const float s = std::clamp(t, 0.0f, 1.0f);
    return std::max({kMinHalf, radius * (1.0f - s), aa * s});
}

// The half widths of the spot measured on a vertical wall: along the wall (across a jamb or a wall's end) and up it (across
// a sill, a head or a top), from the half width across the ray
struct Rim {
    float along = kMinHalf, up = kMinHalf;
};
// A ray's slant against a vertical wall: how much a width across the ray stretches on it
struct Slant {
    float along = 1.0f, up = 1.0f;
    Rim At(float half) const {
        return Rim{std::clamp(half * along, kMinHalf, kMaxHalf), std::clamp(half * up, kMinHalf, kMaxHalf)};
    }
};
// d: the ray (any length); (nx, nz): the wall's horizontal normal (any length). Across a vertical edge the ray and the edge
// are skew lines a distance |x| |cos a| apart (a: the ray's horizontal angle off the normal); across a horizontal edge along
// the wall (tangent T, normal N), |x| |d.N| / sqrt(1 - (d.T)^2).
inline Slant SlantOf(float dx, float dy, float dz, float nx, float nz) {
    Slant s;
    const float nl = std::sqrt(nx * nx + nz * nz), dl = std::sqrt(dx * dx + dy * dy + dz * dz), hl = std::sqrt(dx * dx + dz * dz);
    if (!(nl > 1e-6f) || !(dl > 1e-6f)) return s;
    nx /= nl, nz /= nl;
    const float dn = std::fabs(dx * nx + dz * nz);  // |d.N| (unnormalised)
    const float dt = std::fabs(-dx * nz + dz * nx); // |d.T|
    s.along = hl > 1e-6f ? hl / std::max(dn, 1e-3f * hl) : 1.0f;
    s.up = std::sqrt(std::max(dl * dl - dt * dt, 0.0f)) / std::max(dn, 1e-3f * dl);
    return s;
}

} // namespace Penumbra
