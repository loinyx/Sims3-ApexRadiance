#pragma once
// Tree and bush canopies as porous occluders of lamp light on the ground (features/ground_lamp_shadow.h). POD and pure
// functions: no game reads, tested offline (tools/canopy_test). Credits: @loinyx
//
// Beer-Lambert in a turbid medium of leaves (Monsi & Saeki 1953; Ross 1981; Nilson 1971):
//   T(s) = exp(-k u s),  k = G x Omega x sqrt(1 - w)
//   G     = 0.5   spherical leaf-angle distribution (the same for every ray direction)
//   Omega = 0.8   clumping index of broadleaf crowns (Chen et al. 2005: 0.6 to 0.9)
//   w     = 0.15  leaf single-scattering albedo in the visible (reflectance about 0.08 plus transmittance about 0.07,
//                 Jacquemoud & Baret 1990); sqrt(1 - w) is Goudriaan's (1977) two-stream term for light scattered on
//   u     = leaf area density (m2/m3): tree crowns, crown leaf area index 3 to 6 over 3 to 5 m of depth -> about 1;
//           bushes and hedges 1.5 to 4 (Lalic & Mihailovic 2004; Kucharik et al. 1998) -> 2.5
// Check: a vertical ray through the centre of a 4 m crown (u = 1) keeps exp(-0.37 x 4) = 0.23 of the light, inside the gap
// fraction measured under isolated crowns (0.1 to 0.35). The crown edge is continuous (the chord goes to 0 at the shell),
// so the shadow is soft and partial with no added blur.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Canopy {

constexpr float kG = 0.5f, kClump = 0.8f, kLeafAlbedo = 0.15f;
inline float Extinction() {
    static const float k = kG * kClump * std::sqrt(1.0f - kLeafAlbedo);
    return k; // about 0.369
}
constexpr float kLadTree = 1.0f;          // m2/m3, foliage taller than 2.5 m
constexpr float kLadBush = 2.5f;          // m2/m3, foliage up to 2.5 m
constexpr float kMinOpticalDepth = 0.01f; // tau below 0.01 (T above 0.99): treated as 1

// One crown: an ellipsoid, upright in y, turned in xz by the instance's yaw. World coordinates.
struct Ellipsoid {
    float cx, cy, cz; // centre
    float rx, ry, rz; // semi-axes (m)
    float c, s;       // cos / sin of the yaw (local x -> world)
    float lad;        // u, m2/m3
};

// Length (m) of the segment P0 -> P1 inside the ellipsoid. Analytic: one square root, no loop.
inline float ChordLength(const Ellipsoid& e, float x0, float y0, float z0, float x1, float y1, float z1) {
    const auto local = [&](float x, float y, float z, float o[3]) { // inverse yaw, scaled to the unit sphere
        const float dx = x - e.cx, dz = z - e.cz;
        o[0] = (dx * e.c + dz * e.s) / e.rx;
        o[1] = (y - e.cy) / e.ry;
        o[2] = (-dx * e.s + dz * e.c) / e.rz;
    };
    float a[3], b[3];
    local(x0, y0, z0, a);
    local(x1, y1, z1, b);
    const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float A = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (!(A > 1e-12f)) return 0.0f;
    const float B = a[0] * d[0] + a[1] * d[1] + a[2] * d[2];
    const float C = a[0] * a[0] + a[1] * a[1] + a[2] * a[2] - 1.0f;
    const float disc = B * B - A * C;
    if (!(disc > 0.0f)) return 0.0f;
    const float r = std::sqrt(disc);
    const float t0 = std::max((-B - r) / A, 0.0f), t1 = std::min((-B + r) / A, 1.0f); // only between the lamp and the receiver
    if (!(t1 > t0)) return 0.0f;
    const float wx = x1 - x0, wy = y1 - y0, wz = z1 - z0;
    return (t1 - t0) * std::sqrt(wx * wx + wy * wy + wz * wz);
}

inline float Transmission(const Ellipsoid& e, float lx, float ly, float lz, float px, float py, float pz) {
    const float tau = Extinction() * e.lad * ChordLength(e, lx, ly, lz, px, py, pz);
    return tau < kMinOpticalDepth ? 1.0f : std::exp(-tau);
}

// A conservative xz box of a crown's shadow on the ground plane gy for a lamp of reach R. false: no shadow (the head
// below the crown's base lights the ground without crossing it).
// Nor does a crown the head stands inside: a lamp is never in a plant's leaves, the ellipsoid only wraps them (a post beside a
// bush under a wide crown shape lost its light on the ground all round)
inline bool ShadowBox(const Ellipsoid& e, float lx, float ly, float lz, float gy, float R, float& x0, float& z0, float& x1, float& z1) {
    const float rxz = std::max(e.rx, e.rz), bottom = e.cy - e.ry;
    if (!(ly > bottom)) return false;
    {
        const float dx = lx - e.cx, dz = lz - e.cz;
        const float u = (dx * e.c + dz * e.s) / e.rx, v = (ly - e.cy) / e.ry, w = (-dx * e.s + dz * e.c) / e.rz;
        if (u * u + v * v + w * w < 1.0f) return false;
    }
    const float dx = e.cx - lx, dz = e.cz - lz, d = std::sqrt(dx * dx + dz * dz);
    if (d - rxz >= R) return false;
    if (d <= rxz || ly <= e.cy + e.ry) { // the head beside or inside the crown: the shadow can fall anywhere within R
        x0 = lx - R, x1 = lx + R, z0 = lz - R, z1 = lz + R;
        return true;
    }
    // the head above the crown: the crown projected from the head onto the ground (scale h / (h - base))
    const float h = ly - gy, k = h / std::max(ly - bottom, 0.05f);
    const float px = lx + dx * k, pz = lz + dz * k, pr = rxz * k;
    x0 = std::min(px - pr, e.cx - rxz), x1 = std::max(px + pr, e.cx + rxz);
    z0 = std::min(pz - pr, e.cz - rxz), z1 = std::max(pz + pr, e.cz + rxz);
    return true;
}

// Leaf area density by the foliage's height (the one discrete choice, from the ranges above).
inline float LadFor(float foliageHeight) { return foliageHeight > 2.5f ? kLadTree : kLadBush; }

} // namespace Canopy
