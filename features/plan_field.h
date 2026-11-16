#pragma once
// Real-time GI (docs/features/realtime-gi, work package S0-06): visibility between two points of a lot from a distance
// field of its wall plan, and the CPU twin of that test. Pure functions over RtLampProxy's story data, no game reads.
//
// The plan. TS3 walls are vertical and stand on tile sides or tile diagonals, so on one story "does A see B" is a 2D
// question, except for what a wall leaves open at some heights (a door under its head, a window between sill and head,
// the air over a half wall). The story's height (kStoryH) is cut into height bands at the `cuts`; each band has its own
// plan: a wall span is drawn into a band's plan when the wall is solid at the band's middle height (Solid: the sill-height
// rule). Each plan becomes a distance field by jump flooding (Rong and Tan 2006): every pixel a wall crosses holds the
// wall's point nearest its centre as a seed (sub-pixel, so a pixel centre's distance to a straight wall is exact), then
// one pass at step 1, passes at steps N/2 .. 1 and one more at step 1 leave in every pixel its nearest seed. The passes
// here read and write whole buffers the way pixel shader passes would (9 reads a pixel, two buffers swapped).
//
// The trace. The segment A -> B is walked in the plan (Trace): at each step the field of the band the segment is in at that
// point is read with bilinear filtering; under kHit pixels the segment is blocked; else it advances by the distance less
// kHit (at least kMinStep), and never past the end of the band it is in, since the next band's walls differ. Between two
// stories the segment must cross where no ceiling or placed floor lies (SlabOpen: a stairwell, a removed floor, outdoors).
// A step budget that runs out counts as blocked, so a short budget darkens and never leaks.
//
// The twin (TwinClear): the same bands, rule and story crossings with exact 2D segment tests against the wall edges in
// place of the field; with no cuts each wall is tested at the height the segment crosses it (the walls of the proxy
// mesh, less the slabs' thickness).
//
// Points must stand off the walls: an end point nearer than kHit + kMinStep pixels to a wall of its band reads as blocked
// (a wall texel is pushed along its normal first).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
#include "rt_lamp_proxy.h"

namespace PlanField {

using RtLampProxy::Edge;
using RtLampProxy::Lot;
using RtLampProxy::Story;

constexpr int kPx = 16;           // plan pixels a tile (1 m)
// The heights (m over the story's base) the default bands are cut at: a half wall's top (1 m, the game's own constant) and
// just under a door's head, so the bands are "under the sills", "window height" and "over the heads".
inline const std::vector<float> kCuts = {1.0f, 2.15f};
constexpr float kStoryH = 3.0f;   // m: a story's height (the proxy's ceilings stand at floor + 3)
constexpr float kHit = 0.75f;     // px: nearer to a wall than this is blocked (a wall crosses a pixel within 0.71 px of a corner)
constexpr float kMinStep = 0.25f; // px
// px: a wall's end at an opening is drawn this much short. A jamb is exact to half a pixel either way (the bilinear read of the
// last seeds); a continuous wall reads under kHit everywhere along it, so only a jamb can pass what the twin blocks.
constexpr float kErode = 0.25f;
constexpr int kPad = 2;           // px around the story's grid
constexpr int kSteps = 32;        // the default step budget

inline float BaseOf(const Story& s) {
    if (s.plane < RtLampProxy::kNoFloor * 0.5f) return s.plane;
    float b = RtLampProxy::kNoFloor;
    for (const Edge& e : s.edges) b = std::min(b, e.base);
    return b < RtLampProxy::kNoFloor * 0.5f ? b : 0.0f;
}
inline std::vector<float> UniformCuts(int bands) {
    std::vector<float> c;
    for (int i = 1; i < bands; i++) c.push_back(kStoryH * static_cast<float>(i) / static_cast<float>(bands));
    return c;
}
inline int BandOf(float y, float base, const std::vector<float>& cuts) {
    int band = 0;
    while (band < static_cast<int>(cuts.size()) && y - base >= cuts[band]) band++;
    return band;
}
inline float BandMid(int band, float base, const std::vector<float>& cuts) {
    return base + 0.5f * ((band > 0 ? cuts[band - 1] : 0.0f) + (band < static_cast<int>(cuts.size()) ? cuts[band] : kStoryH));
}

// The sill-height rule's test: the wall of edge e is solid at u along it and height y (lot space)
inline bool Solid(const Edge& e, float u, float y) {
    if (y < e.foot - RtLampProxy::kFootExt || y > RtLampProxy::EdgeTop(e, u)) return false;
    return !(e.open && u > e.o.u0 && u < e.o.u1 && y > e.base + e.o.sill && y < e.base + RtLampProxy::OpeningHead(e.o, u));
}

struct Field {
    int w = 0, h = 0, px = kPx; // w, h in pixels
    std::vector<float> cuts;    // the bands' cut heights (cuts.size() + 1 bands)
    float base = 0.0f;
    int passes = 0;
    std::vector<float> d; // per band, pixel: the distance to the nearest wall of the band, in pixels
    // (bilinear, clamped at the border; x, z in lot metres)
    float At(int band, float x, float z) const {
        const float fx = std::clamp(x * static_cast<float>(px) + static_cast<float>(kPad) - 0.5f, 0.0f, static_cast<float>(w - 1));
        const float fz = std::clamp(z * static_cast<float>(px) + static_cast<float>(kPad) - 0.5f, 0.0f, static_cast<float>(h - 1));
        const int ix = std::min(static_cast<int>(fx), w - 2), iz = std::min(static_cast<int>(fz), h - 2);
        const float tx = fx - static_cast<float>(ix), tz = fz - static_cast<float>(iz);
        const float* p = d.data() + (static_cast<size_t>(band) * h + iz) * w + ix;
        return (p[0] * (1.0f - tx) + p[1] * tx) * (1.0f - tz) + (p[w] * (1.0f - tx) + p[w + 1] * tx) * tz;
    }
};

inline Field BuildField(const Story& s, const std::vector<float>& cuts = kCuts, int px = kPx) {
    Field f;
    const int bands = static_cast<int>(cuts.size()) + 1;
    f.px = px, f.cuts = cuts, f.base = BaseOf(s);
    f.w = s.w * px + 2 * kPad, f.h = s.h * px + 2 * kPad;
    const size_t n = static_cast<size_t>(f.w) * f.h;
    f.d.assign(n * bands, 0.0f);
    constexpr float kNone = std::numeric_limits<float>::infinity();
    struct Seed {
        float x, z; // pixel space; x = kNone: no seed
    };
    std::vector<Seed> a(n), b(n);
    const auto dist2 = [](const Seed& sd, int ix, int iz) {
        const float dx = sd.x - (static_cast<float>(ix) + 0.5f), dz = sd.z - (static_cast<float>(iz) + 0.5f);
        return sd.x == kNone ? kNone : dx * dx + dz * dz;
    };
    for (int band = 0; band < bands; band++) {
        const float y = BandMid(band, f.base, cuts);
        std::fill(a.begin(), a.end(), Seed{kNone, kNone});
        for (const Edge& e : s.edges) { // the plan: four samples a pixel along each wall, the nearest to its pixel's centre kept
            const float x0 = e.x0 * px + kPad, z0 = e.z0 * px + kPad, dx = (e.x1 - e.x0) * px, dz = (e.z1 - e.z0) * px;
            const float lenPx = std::max(std::hypot(dx, dz), 1e-6f);
            const int m = std::max(1, static_cast<int>(std::ceil(lenPx * 4.0f)));
            // (a wall's end at an opening is drawn kErode short: the trace blocks within kHit of a seed, which narrows the opening)
            const float du = kErode / lenPx;
            for (int i = 0; i <= m; i++) {
                const float u = static_cast<float>(i) / static_cast<float>(m);
                if (!Solid(e, u, y) || !Solid(e, std::max(u - du, 0.0f), y) || !Solid(e, std::min(u + du, 1.0f), y)) continue;
                const Seed sd{x0 + dx * u, z0 + dz * u};
                const int ix = std::clamp(static_cast<int>(sd.x), 0, f.w - 1), iz = std::clamp(static_cast<int>(sd.z), 0, f.h - 1);
                Seed& at = a[static_cast<size_t>(iz) * f.w + ix];
                if (dist2(sd, ix, iz) < dist2(at, ix, iz)) at = sd;
            }
        }
        int top = 1;
        while (top * 2 < std::max(f.w, f.h)) top *= 2;
        std::vector<int> steps = {1};
        for (int st = top; st >= 1; st /= 2) steps.push_back(st);
        steps.push_back(1);
        for (const int st : steps) { // one jump flooding pass: the nearest of the 9 seeds at +- st
            for (int iz = 0; iz < f.h; iz++)
                for (int ix = 0; ix < f.w; ix++) {
                    Seed best{kNone, kNone};
                    float bd = kNone;
                    for (int oz = -st; oz <= st; oz += st)
                        for (int ox = -st; ox <= st; ox += st) {
                            const int jx = ix + ox, jz = iz + oz;
                            if (jx < 0 || jz < 0 || jx >= f.w || jz >= f.h) continue;
                            const Seed& c = a[static_cast<size_t>(jz) * f.w + jx];
                            if (const float cd = dist2(c, ix, iz); cd < bd) bd = cd, best = c;
                        }
                    b[static_cast<size_t>(iz) * f.w + ix] = best;
                }
            a.swap(b);
        }
        f.passes = static_cast<int>(steps.size());
        float* out = f.d.data() + static_cast<size_t>(band) * n;
        for (int iz = 0; iz < f.h; iz++)
            for (int ix = 0; ix < f.w; ix++) {
                const float d2 = dist2(a[static_cast<size_t>(iz) * f.w + ix], ix, iz);
                out[static_cast<size_t>(iz) * f.w + ix] = d2 == kNone ? 1e6f : std::sqrt(d2); // (no wall in this band)
            }
    }
    return f;
}

// a -> b inside one story's field. True: clear. steps: the budget left, counted down.
inline bool TraceStory(const Field& f, const float a[3], const float b[3], int& steps) {
    const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    const float len = std::hypot(dx, dz) * static_cast<float>(f.px); // px in the plan
    if (!(len > 1e-4f)) return true;
    const int last = static_cast<int>(f.cuts.size());
    for (float t = 0.0f; steps > 0; steps--) {
        const float r = t / len, y = a[1] + dy * r;
        const int band = BandOf(y, f.base, f.cuts);
        const float d = f.At(band, a[0] + dx * r, a[2] + dz * r);
        if (d < kHit) return false;
        float step = std::max(d - kHit, kMinStep);
        if ((dy > 1e-6f && band < last) || (dy < -1e-6f && band > 0)) { // not past the band's end: the next band's walls differ
            const float tEnd = (f.base + f.cuts[dy > 0.0f ? band : band - 1] - a[1]) / dy * len;
            if (tEnd > t) step = std::min(step, tEnd - t + 1e-3f);
        }
        t += step;
        if (t >= len) return true;
    }
    return false; // (the budget ran out: blocked, never a leak)
}

inline int QuadrantAt(float x, float z, int ix, int iz) {
    const float fx = x - static_cast<float>(ix) - 0.5f, fz = z - static_cast<float>(iz) - 0.5f;
    return std::fabs(fx) > std::fabs(fz) ? (fx > 0.0f ? 1 : 3) : (fz > 0.0f ? 2 : 0); // (RtLampProxy's quadrants: 0 -z, 1 +x, 2 +z, 3 -x)
}
// The plane between story s and the one above it (null: none) is open at (x, z): no ceiling or roof of s, no placed floor above
inline bool SlabOpen(const Story& s, const Story* above, float x, float z) {
    const int ix = static_cast<int>(std::floor(x)), iz = static_cast<int>(std::floor(z));
    if (ix < 0 || iz < 0 || ix >= s.w || iz >= s.h) return true;
    const size_t t = static_cast<size_t>(iz) * s.w + ix;
    const int q = QuadrantAt(x, z, ix, iz);
    if (t < s.roofed.size() && ((s.roofed[t] >> q) & 1u)) return false;
    return !(above && above->w == s.w && above->h == s.h && RtLampProxy::PlacedQ(*above, ix, iz, q));
}

// The walk both tests share: the segment cut at the story planes (lot.stories in rising order, each kStoryH or up to the next
// one's base), each piece given to `story(index, p0, p1)`, each plane crossed tested with SlabOpen.
template <class StoryClear> bool Walk(const Lot& lot, const float a[3], const float b[3], StoryClear&& story) {
    const float* lo = a[1] <= b[1] ? a : b;
    const float* hi = a[1] <= b[1] ? b : a;
    const int n = static_cast<int>(lot.stories.size());
    const float dy = hi[1] - lo[1];
    const auto at = [&](float y, float out[3], float level) { // (level: where a level segment is taken, its start or its end)
        const float r = dy > 1e-6f ? std::clamp((y - lo[1]) / dy, 0.0f, 1.0f) : level;
        out[0] = lo[0] + (hi[0] - lo[0]) * r, out[1] = lo[1] + dy * r, out[2] = lo[2] + (hi[2] - lo[2]) * r;
    };
    for (int i = 0; i < n; i++) {
        const float base = BaseOf(lot.stories[i]);
        const float top = i + 1 < n ? BaseOf(lot.stories[i + 1]) : base + kStoryH;
        if (i > 0 && hi[1] < base) break;
        if (lo[1] >= top) continue; // (the piece over the top story's ceiling crosses no wall)
        float p0[3], p1[3];
        at(i > 0 ? std::max(lo[1], base) : lo[1], p0, 0.0f);
        at(std::min(hi[1], top), p1, 1.0f);
        if (!story(i, p0, p1)) return false;
        if (hi[1] > top && !SlabOpen(lot.stories[i], i + 1 < n ? &lot.stories[i + 1] : nullptr, p1[0], p1[2])) return false;
    }
    return true;
}

struct Plan {
    std::vector<Field> f; // one a story of the lot
};
inline Plan BuildPlan(const Lot& lot, const std::vector<float>& cuts = kCuts, int px = kPx) {
    Plan p;
    for (const Story& s : lot.stories) p.f.push_back(BuildField(s, cuts, px));
    return p;
}
// True: b is seen from a. used: the steps taken (maxSteps when the budget ran out).
inline bool Trace(const Plan& p, const Lot& lot, const float a[3], const float b[3], int maxSteps = kSteps, int* used = nullptr) {
    int steps = maxSteps;
    const bool clear = Walk(lot, a, b, [&](int i, const float p0[3], const float p1[3]) { return TraceStory(p.f[i], p0, p1, steps); });
    if (used) *used = maxSteps - steps;
    return clear;
}

// The twin's story test: exact crossings of the wall edges. cuts null: each wall at the height it is crossed.
inline bool TwinStory(const Story& s, const std::vector<float>* cuts, const float a[3], const float b[3]) {
    const float dx = b[0] - a[0], dz = b[2] - a[2], base = BaseOf(s);
    for (const Edge& e : s.edges) {
        const float ex = e.x1 - e.x0, ez = e.z1 - e.z0, den = dx * ez - dz * ex;
        if (std::fabs(den) < 1e-9f) continue; // (along the wall)
        const float t = ((e.x0 - a[0]) * ez - (e.z0 - a[2]) * ex) / den, u = ((e.x0 - a[0]) * dz - (e.z0 - a[2]) * dx) / den;
        if (t < 0.0f || t > 1.0f || u < -1e-4f || u > 1.0f + 1e-4f) continue; // (a crossing at the joint of two edges meets both)
        const float y = a[1] + (b[1] - a[1]) * t;
        if (Solid(e, u, cuts ? BandMid(BandOf(y, base, *cuts), base, *cuts) : y)) return false;
    }
    return true;
}
inline bool TwinClear(const Lot& lot, const float a[3], const float b[3], const std::vector<float>* cuts = &kCuts) {
    return Walk(lot, a, b, [&](int i, const float p0[3], const float p1[3]) { return TwinStory(lot.stories[i], cuts, p0, p1); });
}

} // namespace PlanField
