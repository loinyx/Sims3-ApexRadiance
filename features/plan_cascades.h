#pragma once
// Real-time GI (docs/features/realtime-gi, work package X-05): 2D radiance cascades over a story's wall-plan field
// (features/plan_field.h), the CPU form. Pure functions; the shader forms (shaders/plan_cascades_hlsl.h, ps_3_0, and
// tools/gi_cascades_test/cascades.comp, a compute shader) follow it function by function.
//
// The cascades (Sannikov 2023). Level i holds probes every spacing x 2^i metres, each with d0 x 4^i directions, and covers
// the distances t_i .. t_i+1 from its probes, t_i = l0 (4^i - 1) / 3. A level's texel (probe, direction j) is the mean of
// its 4 child directions 4 j + c of the next level's angular grid: each is traced over the level's interval and, where the
// interval is clear, continued by the next level's texel (probe, 4 j + c), blended over the 4 next-level probes around.
// fix (the "bilinear fix"): one trace a next-level probe, from this probe's interval start to that probe's interval start,
// so a ray is continued only by a probe it reached. Without it one trace is continued by all 4 probes, of which some may
// stand behind a wall: light leaks. Texels: every level is one tw x th image, probe (px, pz) owning a side x side block,
// direction j at (j % side, j / side).
//
// The slab. Flatland has no floor and no ceiling, which carry most of a room's bounce light. Here a ray of the plan stands
// for every elevation over its azimuth in a story of height kStoryH: walls are taken as uniform over their height, floor and
// ceiling as diffuse sheets whose radiosity is read along the ray. For a receiver at height h the elevations that reach a
// plan distance t before they meet the sheet at height a over (or under) it are a fixed share SlabT(a, t) of the cosine-
// weighted elevations, so a sheet's radiosity B between t0 and t1 adds B (SlabT(t0) - SlabT(t1)) and a wall at t adds
// B SlabT(t); these shares depend on the distance from the receiver only, and the cascades' intervals are distances from
// the receiver, so they are applied at the nominal distance and need no transmittance to be carried from level to level.
// Three receivers share a ray (the texel's x, y, z): a vertical face at kEye over the floor (the directional layers), the
// floor (sees ceiling and walls) and the ceiling (sees floor and walls). The result is exact for a prism: full-height
// walls, radiosity uniform over a wall's height. Furniture, door heads and sills are not in it (one band of the field).
//
// Emit: per field pixel the radiosity of the wall this pixel is nearest to, on this pixel's side (read kBack pixels back
// along the ray at a hit, so a thin wall keeps two sides), of the floor and of the ceiling (0 where there is none).
// Gather: per level-0 probe the irradiance of faces +x, -x, +z, -z at kEye, of the floor and of the ceiling.
#include <functional>
#include <thread>
#include "plan_field.h"

namespace PlanCascades {

using PlanField::Field;
using PlanField::kStoryH;

constexpr float kEye = 1.0f;   // m over the floor: the directional receivers (design 3.3's layers)
constexpr float kBack = 1.5f;  // px back along the ray from a hit: where the wall's radiosity is read
constexpr float kCapPx = 8.0f; // px: the longest step near the probe; further out a quarter of the distance
constexpr float kPi = 3.14159265f;
constexpr int kOut = 8;        // Gather's floats a probe: +x, -x, +z, -z, floor, ceiling, 0, 0

struct Params {
    int d0 = 16;           // directions of a level-0 probe (4 or 16)
    int levels = 3;
    float spacing = 0.25f; // m between level-0 probes
    float l0 = 1.0f;       // m: level 0's interval
    bool fix = true;
    int steps = 32;        // a trace's budget; one that runs out is dark
    int band = 1;          // the field's band (kCuts: 1 = window height, doors and windows open)
    float capFar = 0.1f;   // a step's longest share of the distance from the probe (TraceSeg)
};
struct Emit {
    int w = 0, h = 0;
    std::vector<float> v; // 4 a pixel: wall, floor, ceiling, 0
};
struct Level {
    int nx = 0, nz = 0, side = 0, dirs = 0;
    float spacing = 0, t0 = 0, t1 = 0;
};
struct Layout {
    int nx = 0, nz = 0, tw = 0, th = 0; // level-0 probes; every level's image
    std::vector<Level> lv;
};
inline Layout LayoutOf(const Field& f, const Params& p) {
    Layout l;
    const int mult = 1 << (p.levels - 1), side0 = p.d0 == 4 ? 2 : 4;
    const auto count = [&](int px) { return (static_cast<int>(std::ceil(static_cast<float>(px - 2 * PlanField::kPad) / static_cast<float>(f.px) / p.spacing)) + mult - 1) / mult * mult; };
    l.nx = count(f.w), l.nz = count(f.h), l.tw = l.nx * side0, l.th = l.nz * side0;
    for (int i = 0; i < p.levels; i++) {
        const float pow4 = static_cast<float>(1 << (2 * i));
        l.lv.push_back({l.nx >> i, l.nz >> i, side0 << i, p.d0 << (2 * i), p.spacing * static_cast<float>(1 << i), p.l0 * (pow4 - 1.0f) / 3.0f, p.l0 * (4.0f * pow4 - 1.0f) / 3.0f});
    }
    return l;
}

// The share of a receiver's cosine-weighted elevations (one side of its height) still under a sheet `a` metres away at plan distance t
inline float SlabT(float a, float t) { return 0.63661977f * (std::atan2(a, t) + a * t / (a * a + t * t)); }
// The same for a receiver on the other sheet (cosine to the vertical)
inline float SlabU(float t) { return kStoryH * kStoryH / (t * t + kStoryH * kStoryH); }

inline const float* EmitAt(const Field& f, const Emit& e, float x, float z) {
    const int ix = std::clamp(static_cast<int>(std::floor(x * static_cast<float>(f.px) + static_cast<float>(PlanField::kPad))), 0, e.w - 1);
    const int iz = std::clamp(static_cast<int>(std::floor(z * static_cast<float>(f.px) + static_cast<float>(PlanField::kPad))), 0, e.h - 1);
    return &e.v[(static_cast<size_t>(iz) * e.w + ix) * 4];
}

// The plan segment a -> b (lot metres), standing for the distances tA .. tB from the receiver: adds what it meets to rad
// (vertical face, floor, ceiling); 1 = it reached b. A step is at most capPx, or capFar of the distance when that is longer:
// the sheets are read once a step, so a source smaller than a step is met by some rays and missed by others
inline float TraceSeg(const Field& f, const Emit& e, int band, float ax, float az, float bx, float bz, float tA, float tB, int steps, float rad[3], float capPx = kCapPx,
                      float capFar = 0.1f) {
    const float px = static_cast<float>(f.px), dx = bx - ax, dz = bz - az, len = std::hypot(dx, dz) * px;
    if (!(len > 1e-3f)) return 1.0f;
    const float i2p = 0.5f / kPi;
    float s = 0.0f, t0 = tA, f0 = SlabT(kEye, tA), c0 = SlabT(kStoryH - kEye, tA), u0 = SlabU(tA);
    for (int n = 0; n < steps; n++) {
        const float d = f.At(band, ax + dx * (s / len), az + dz * (s / len));
        if (d < PlanField::kHit) {
            const float r = (s - kBack) / len, w = EmitAt(f, e, ax + dx * r, az + dz * r)[0];
            rad[0] += w * (f0 + c0) * 0.25f, rad[1] += w * u0 * i2p, rad[2] += w * u0 * i2p;
            return 0.0f;
        }
        const float st = std::min(std::min(std::max(d - PlanField::kHit, PlanField::kMinStep), std::max(capPx, capFar * t0 * px)), len - s);
        const float s1 = s + st, t1 = tA + (tB - tA) * s1 / len, r = (s + 0.5f * st) / len;
        const float f1 = SlabT(kEye, t1), c1 = SlabT(kStoryH - kEye, t1), u1 = SlabU(t1);
        const float* m = EmitAt(f, e, ax + dx * r, az + dz * r);
        rad[0] += (m[1] * (f0 - f1) + m[2] * (c0 - c1)) * 0.25f, rad[1] += m[2] * (u0 - u1) * i2p, rad[2] += m[1] * (u0 - u1) * i2p;
        s = s1, t0 = t1, f0 = f1, c0 = c1, u0 = u1;
        if (s >= len - 1e-4f) return 1.0f;
    }
    return 0.0f;
}

inline void Rows(int n, const std::function<void(int)>& row) {
    const int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; t++)
        pool.emplace_back([&, t] {
            for (int i = t; i < n; i += threads) row(i);
        });
    for (std::thread& t : pool) t.join();
}

// Level i's image (tw x th x 4) from the next level's (null: the last level, nothing beyond its interval)
inline void Pass(const Field& f, const Emit& e, const Params& p, const Layout& l, int i, const std::vector<float>* up, std::vector<float>& out) {
    const Level& g = l.lv[i];
    const Level* g1 = up ? &l.lv[i + 1] : nullptr;
    const int d1 = g.dirs * 4;
    out.assign(static_cast<size_t>(l.tw) * l.th * 4, 0.0f);
    Rows(g.nz * g.side, [&](int tz) {
        for (int tx = 0; tx < g.nx * g.side; tx++) {
            const int Px = tx / g.side, Pz = tz / g.side, j = (tz - Pz * g.side) * g.side + (tx - Px * g.side);
            const float x = (static_cast<float>(Px) + 0.5f) * g.spacing, z = (static_cast<float>(Pz) + 0.5f) * g.spacing;
            float ux = 0, uz = 0, bx = 0, bz = 0;
            if (g1) ux = x / g1->spacing - 0.5f, uz = z / g1->spacing - 0.5f, bx = std::floor(ux), bz = std::floor(uz);
            const float fx = ux - bx, fz = uz - bz;
            float acc[3] = {0, 0, 0};
            for (int c = 0; c < 4; c++) {
                const int jj = 4 * j + c;
                const float th = (static_cast<float>(jj) + 0.5f) * 2.0f * kPi / static_cast<float>(d1), cx = std::cos(th), cz = std::sin(th);
                const float sx = x + g.t0 * cx, sz = z + g.t0 * cz;
                float rad[3] = {0, 0, 0};
                const float clear = !g1 || !p.fix ? TraceSeg(f, e, p.band, sx, sz, x + g.t1 * cx, z + g.t1 * cz, g.t0, g.t1, p.steps, rad, kCapPx, p.capFar) : 0.0f;
                for (int k = 0; g1 && k < 4; k++) {
                    const int kx = k & 1, kz = k >> 1;
                    const int qx = std::clamp(static_cast<int>(bx) + kx, 0, g1->nx - 1), qz = std::clamp(static_cast<int>(bz) + kz, 0, g1->nz - 1);
                    const float w = (kx ? fx : 1.0f - fx) * (kz ? fz : 1.0f - fz);
                    const float* u = &(*up)[((static_cast<size_t>(qz) * g1->side + jj / g1->side) * l.tw + static_cast<size_t>(qx) * g1->side + jj % g1->side) * 4];
                    if (!p.fix) {
                        for (int v = 0; v < 3; v++) rad[v] += w * clear * u[v];
                        continue;
                    }
                    float seg[3] = {0, 0, 0};
                    const float cl = TraceSeg(f, e, p.band, sx, sz, (static_cast<float>(qx) + 0.5f) * g1->spacing + g.t1 * cx, (static_cast<float>(qz) + 0.5f) * g1->spacing + g.t1 * cz, g.t0, g.t1,
                                              p.steps, seg, kCapPx, p.capFar);
                    for (int v = 0; v < 3; v++) rad[v] += w * (seg[v] + cl * u[v]);
                }
                for (int v = 0; v < 3; v++) acc[v] += 0.25f * rad[v];
            }
            float* o = &out[(static_cast<size_t>(tz) * l.tw + tx) * 4];
            o[0] = acc[0], o[1] = acc[1], o[2] = acc[2];
        }
    });
}

// The weight of direction j of d in the irradiance of faces +x, -x, +z, -z (the cosine over the direction's arc)
inline void FaceWeights(int j, int d, float w[4]) {
    const float th = (static_cast<float>(j) + 0.5f) * 2.0f * kPi / static_cast<float>(d), k = 2.0f * std::sin(kPi / static_cast<float>(d));
    w[0] = k * std::max(std::cos(th), 0.0f), w[1] = k * std::max(-std::cos(th), 0.0f), w[2] = k * std::max(std::sin(th), 0.0f), w[3] = k * std::max(-std::sin(th), 0.0f);
}
// Level 0's image to nx x nz x kOut
inline std::vector<float> Gather(const Layout& l, const std::vector<float>& lv0) {
    const Level& g = l.lv[0];
    std::vector<float> out(static_cast<size_t>(g.nx) * g.nz * kOut, 0.0f);
    for (int pz = 0; pz < g.nz; pz++)
        for (int px = 0; px < g.nx; px++) {
            float* o = &out[(static_cast<size_t>(pz) * g.nx + px) * kOut];
            for (int j = 0; j < g.dirs; j++) {
                const float* r = &lv0[((static_cast<size_t>(pz) * g.side + j / g.side) * l.tw + static_cast<size_t>(px) * g.side + j % g.side) * 4];
                float w[4];
                FaceWeights(j, g.dirs, w);
                for (int k = 0; k < 4; k++) o[k] += r[0] * w[k];
                o[4] += r[1] * 2.0f * kPi / static_cast<float>(g.dirs), o[5] += r[2] * 2.0f * kPi / static_cast<float>(g.dirs);
            }
        }
    return out;
}
// All levels, last to first, and the gather. levels (optional): every level's image
inline std::vector<float> Solve(const Field& f, const Emit& e, const Params& p, Layout* layout = nullptr, std::vector<std::vector<float>>* levels = nullptr) {
    const Layout l = LayoutOf(f, p);
    std::vector<std::vector<float>> lv(p.levels);
    for (int i = p.levels - 1; i >= 0; i--) Pass(f, e, p, l, i, i + 1 < p.levels ? &lv[i + 1] : nullptr, lv[i]);
    std::vector<float> out = Gather(l, lv[0]);
    if (layout) *layout = l;
    if (levels) *levels = std::move(lv);
    return out;
}

// The same integral at one point with `dirs` whole rays of `range` metres and no cascades: what the cascades approximate
inline void Brute(const Field& f, const Emit& e, int band, float x, float z, int dirs, float range, float out[kOut]) {
    std::fill(out, out + kOut, 0.0f);
    for (int j = 0; j < dirs; j++) {
        const float th = (static_cast<float>(j) + 0.5f) * 2.0f * kPi / static_cast<float>(dirs);
        float rad[3] = {0, 0, 0}, w[4];
        TraceSeg(f, e, band, x, z, x + range * std::cos(th), z + range * std::sin(th), 0.0f, range, 1 << 20, rad, 2.0f, 0.0f); // (2 px steps: the sheets read finely)
        FaceWeights(j, dirs, w);
        for (int k = 0; k < 4; k++) out[k] += rad[0] * w[k];
        out[4] += rad[1] * 2.0f * kPi / static_cast<float>(dirs), out[5] += rad[2] * 2.0f * kPi / static_cast<float>(dirs);
    }
}

} // namespace PlanCascades
