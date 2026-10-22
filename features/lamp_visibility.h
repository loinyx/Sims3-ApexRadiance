#pragma once
// Slabs between a lamp and a point (12/10, F7 captures of the wall behind a balcony with a ground post under it: the wall
// kept 25-45% of the post's light through the slab). A placed floor of a story blocks the light of a lamp below it. The
// room solve (level_light_share.cpp OutdoorEntryImpl) reads the game's floor grids through its view; the render thread
// (LevelLightShare::LampVisibility) reads a copy of them. POD and templates only, no game reads: a read-only offline harness
// can test it with views of its own.
//
// Lot space (ToLocal in level_light_share.cpp): y up, 1 m tiles, each split along its diagonals into four quadrants
// (FUN_006aa390): 0 toward -z, 1 +x, 2 +z, 3 -x. A view is one story's plane: `float h` (its lowest floor, lot space) and
// `bool Placed(int ix, int iz, int q) const` (a floor a player placed in that quadrant).
#include "fast_math.h"
#include <algorithm>
#include <cmath>
#include "penumbra.h"

namespace LampVis {

inline int Quadrant(float fx, float fz) {
    if (ApexMath::Fabs(fx - 0.5f) <= ApexMath::Fabs(fz - 0.5f)) return fz > 0.5f ? 2 : 0;
    return fx > 0.5f ? 1 : 3;
}

constexpr float kOnPlane = 0.02f;     // m: a point this close to a plane lies on it (the rule OutdoorEntryImpl had for its story)
constexpr float kOnPlaneStep = 0.05f; // m: such a point is tested this far toward the lamp, so a wall's foot row on a flush
                                      // facade looks past its own floor's edge and the seam between two stories stays open
constexpr float kMinRim = 0.04f;      // m: the narrowest half rim of a slab's edge (its floor quadrants' stairs)

template <class View> bool PlacedAt(const View& v, float x, float z) {
    const float fx = std::floor(x), fz = std::floor(z);
    return v.Placed(static_cast<int>(fx), static_cast<int>(fz), Quadrant(x - fx, z - fz));
}

// The share (0..1) of the segment a..b (lot xz) that lies over no placed floor of the plane, exact over the quadrant cells:
// every quadrant edge lies on a line x = k, z = k, x - z = k or x + z = k (k whole), so the segment is cut where it crosses
// one and each piece is tested at its middle. No allocation; a segment up to a few metres long.
template <class View> float OpenFraction(const View& v, float ax, float az, float bx, float bz) {
    constexpr int kMaxCuts = 34;
    float cut[kMaxCuts];
    int n = 0;
    cut[n++] = 0.0f;
    cut[n++] = 1.0f;
    const float dx = bx - ax, dz = bz - az;
    const auto family = [&](float a0, float d) { // the lines a0 + s d = k for s in (0, 1)
        if (!(ApexMath::Fabs(d) > 1e-6f)) return;
        const float lo = std::min(a0, a0 + d), hi = std::max(a0, a0 + d);
        for (float k = std::floor(lo) + 1.0f; k < hi && n < kMaxCuts; k += 1.0f) cut[n++] = (k - a0) / d;
    };
    family(ax, dx);
    family(az, dz);
    family(ax - az, dx - dz);
    family(ax + az, dx + dz);
    std::sort(cut, cut + n);
    // Both shares are summed and the open one is returned over their total: a segment with no piece over a floor gives
    // exactly 1 and one with no piece over open air exactly 0 (12/10 review: the pieces of an open segment summed in float
    // to 0.99999994 for 224 of 200 000 random segments, so the edge counters counted open rays as edge hits)
    float open = 0.0f, shut = 0.0f;
    for (int i = 0; i + 1 < n; i++) {
        const float s0 = std::clamp(cut[i], 0.0f, 1.0f), s1 = std::clamp(cut[i + 1], 0.0f, 1.0f);
        if (!(s1 > s0)) continue;
        const float s = 0.5f * (s0 + s1);
        (PlacedAt(v, ax + s * dx, az + s * dz) ? shut : open) += s1 - s0;
    }
    const float total = open + shut;
    return total > 0.0f ? std::clamp(open / total, 0.0f, 1.0f) : 1.0f;
}

// A ray going up from the lamp P to the point Q (lot space): the share of the lamp's light that no placed floor of the planes
// st[0..n) stops (1 = clear, 0 = blocked); blocked = the index of the plane that stopped it, -1 when none did. A plane
// counts when it lies above the lamp and below the point (or within kOnPlane over it). The soft edge is that of the lamp's
// ball, `radius` (Shadow softness, penumbra.h): at the crossing (t = 0 at the lamp, 1 at the point) the point sees it
// through a band 2 radius (1 - t) wide (similar triangles), so the light kept is the share of that band over open air,
// centred on the crossing along the ray's horizontal direction (half width at least kMinRim, and aa t for a light map's
// texel: Penumbra::Half). Only along that direction: a slab edge the ray runs along stays hard. Rays going down (lamp
// above the point) are not tested here: 1.
template <class View>
float SlabGate(const View* st, int n, const float* P, const float* Q, int* blocked = nullptr, float radius = Penumbra::Radius(), float aa = 0.0f) {
    if (blocked) *blocked = -1;
    const float dy = Q[1] - P[1];
    if (!(dy > 1e-4f)) return 1.0f;
    float ux = P[0] - Q[0], uz = P[2] - Q[2]; // from the point toward the lamp, along the ground
    const float ul = ApexMath::Sqrt(ux * ux + uz * uz);
    const bool across = ul > 1e-4f;
    if (across) ux /= ul, uz /= ul;
    else ux = uz = 0.0f;
    float v = 1.0f;
    for (int i = 0; i < n; i++) {
        const float h = st[i].h;
        if (!(h > P[1]) || !(h < Q[1] + kOnPlane)) continue;
        float t, x, z;
        if (ApexMath::Fabs(Q[1] - h) < kOnPlane) {
            t = 1.0f;
            x = Q[0] + kOnPlaneStep * ux;
            z = Q[2] + kOnPlaneStep * uz;
        } else {
            t = (h - P[1]) / dy;
            x = P[0] + t * (Q[0] - P[0]);
            z = P[2] + t * (Q[2] - P[2]);
        }
        // (penumbra off, the Tester switch: the 0.3 m lantern of 2.12.3, max(0.04, 0.15 (1 - t)))
        const float rho = Penumbra::g_on.load(std::memory_order_relaxed) ? std::max(kMinRim, Penumbra::Half(radius, t, aa)) : std::max(kMinRim, 0.15f * (1.0f - t));
        const float f = across ? OpenFraction(st[i], x - rho * ux, z - rho * uz, x + rho * ux, z + rho * uz) : (PlacedAt(st[i], x, z) ? 0.0f : 1.0f);
        v *= f;
        if (v <= 0.0f) {
            if (blocked) *blocked = i;
            return 0.0f;
        }
    }
    return v;
}

} // namespace LampVis
