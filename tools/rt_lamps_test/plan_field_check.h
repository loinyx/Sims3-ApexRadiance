#pragma once
// Real-time GI, the wall-plan visibility field (features/plan_field.h) on synthetic stories, CPU only. Included by check.cpp
// after its Check(): a closed wall, a doorway's cone, a diagonal wall, a window and a half wall under the sill-height rule
// under several cuts of the height bands, and a stairwell between two stories; the field against its twin, the twin against the
// proxy mesh's rays (RtLampProxy::Clear), and the steps a trace takes.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>
#include "features/plan_field.h"

namespace PlanFieldTest {
using RtLampProxy::Edge;
using RtLampProxy::Lot;
using RtLampProxy::Story;
using P3 = std::array<float, 3>;

inline Story MakeStory(int index, int w, int h, float base) {
    Story s;
    s.story = index, s.w = w, s.h = h, s.plane = base;
    s.floorY.assign(static_cast<size_t>(w) * h, base);
    s.roofed.assign(static_cast<size_t>(w) * h, 0);
    s.ceil.assign(static_cast<size_t>(w) * h, base + 3.0f);
    return s;
}
// Unit wall edges from (x0, z0) to (x1, z1): along a tile side or a diagonal
inline void Line(Story& s, int x0, int z0, int x1, int z1) {
    const int n = std::max(std::abs(x1 - x0), std::abs(z1 - z0)), sx = (x1 > x0) - (x1 < x0), sz = (z1 > z0) - (z1 < z0);
    for (int i = 0; i < n; i++) {
        Edge e;
        e.x0 = static_cast<float>(x0 + sx * i), e.z0 = static_cast<float>(z0 + sz * i);
        e.x1 = e.x0 + static_cast<float>(sx), e.z1 = e.z0 + static_cast<float>(sz);
        e.foot = e.base = s.plane, e.topL = e.topM = e.topR = s.plane + 3.0f;
        s.edges.push_back(e);
    }
}
inline void Box(Story& s, int x0, int z0, int x1, int z1) { Line(s, x0, z0, x1, z0), Line(s, x1, z0, x1, z1), Line(s, x1, z1, x0, z1), Line(s, x0, z1, x0, z0); }
inline Edge* EdgeAt(Story& s, float x0, float z0, float x1, float z1) {
    for (Edge& e : s.edges)
        if (e.x0 == x0 && e.z0 == z0 && e.x1 == x1 && e.z1 == z1) return &e;
    return nullptr;
}
inline std::vector<P3> GridPts(float x0, float x1, float z0, float z1, float step, std::initializer_list<float> ys) {
    std::vector<P3> v;
    uint32_t seed = 12345u; // (each point up to 0.02 m off the lattice: a ray through a tile corner or along a triangle's edge tests nothing)
    const auto off = [&] { return (static_cast<float>((seed = seed * 1664525u + 1013904223u) >> 8) / 16777216.0f - 0.5f) * 0.04f; };
    for (const float y : ys)
        for (float z = z0; z <= z1 + 1e-4f; z += step)
            for (float x = x0; x <= x1 + 1e-4f; x += step) v.push_back({x + off(), y, z + off()});
    return v;
}
struct Tally {
    long n = 0, clear = 0, leak = 0, dark = 0; // leak: clear where the other says blocked; dark: the reverse
    void Add(bool test, bool ref) { n++, clear += ref, leak += test && !ref, dark += !test && ref; }
    double Leak() const { return n > clear ? 100.0 * static_cast<double>(leak) / static_cast<double>(n - clear) : 0.0; } // % of the pairs the other blocks
    double Dark() const { return clear ? 100.0 * static_cast<double>(dark) / static_cast<double>(clear) : 0.0; }         // % of the pairs the other sees
};
// The distance (m, in the plan) from point (px, pz) to the segment a -> b
inline float PlanDist(const P3& a, const P3& b, float px, float pz) {
    const float dx = b[0] - a[0], dz = b[2] - a[2];
    const float t = std::clamp(((px - a[0]) * dx + (pz - a[2]) * dz) / std::max(dx * dx + dz * dz, 1e-12f), 0.0f, 1.0f);
    return std::hypot(a[0] + dx * t - px, a[2] + dz * t - pz);
}
constexpr int kLong = 4096; // a budget no trace here runs out of

// Rooms A (x 1..6) and B (x 6..11), z 1..7, on a 12 x 8 story; their shared wall is edges (6, z) -> (6, z + 1), z = 1..6
inline Lot TwoRooms() {
    Lot lot;
    Story s = MakeStory(0, 12, 8, 0.0f);
    Box(s, 1, 1, 11, 7);
    Line(s, 6, 1, 6, 7);
    lot.stories.push_back(std::move(s));
    return lot;
}
} // namespace PlanFieldTest

static void PlanFieldChecks() {
    using namespace PlanFieldTest;
    using namespace PlanField;
    std::printf("\n[plan field] wall-plan visibility: %d px a tile, %zu bands a story, hit %.2f px\n", kPx, kCuts.size() + 1, kHit);
    const float px = 1.0f / static_cast<float>(kPx);
    std::vector<int> stepsUsed;
    const std::initializer_list<float> ys = {0.05f, 0.8f, 1.5f, 2.5f, 2.9f};
    // (the columns 0.08 to 0.12 m from the shared wall: from 1.28 px, the nearest a pushed texel stands)
    std::vector<P3> ptsA = GridPts(1.2f, 5.6f, 1.2f, 6.8f, 0.4f, ys), ptsB = GridPts(6.4f, 10.8f, 1.2f, 6.8f, 0.4f, ys);
    for (const P3& p : GridPts(5.9f, 5.9f, 1.2f, 6.8f, 0.4f, ys)) ptsA.push_back(p);
    for (const P3& p : GridPts(6.1f, 6.1f, 1.2f, 6.8f, 0.4f, ys)) ptsB.push_back(p);

    { // 1. a closed wall
        const Lot lot = TwoRooms();
        const Plan plan = BuildPlan(lot);
        long field = 0, twin = 0, same = 0, sameClear = 0;
        for (const P3& a : ptsA)
            for (const P3& b : ptsB) field += Trace(plan, lot, a.data(), b.data(), kLong), twin += TwinClear(lot, a.data(), b.data());
        for (size_t i = 0; i < ptsA.size(); i += 7)
            for (size_t j = i + 1; j < ptsA.size(); j += 3) same++, sameClear += Trace(plan, lot, ptsA[i].data(), ptsA[j].data(), kLong);
        std::printf("  closed wall: %zu x %zu pairs across, field sees %ld, twin %ld; inside one room %ld of %ld clear; field %d x %d px, %d passes a band\n",
                    ptsA.size(), ptsB.size(), field, twin, sameClear, same, plan.f[0].w, plan.f[0].h, plan.f[0].passes);
        Check(field == 0 && twin == 0, "plan field: light crosses a closed wall");
        Check(sameClear == same, "plan field: two points of one room do not see each other");
    }
    { // 2. a doorway: z 3.1..3.9 of the shared wall, head 2.2 m
        Lot lot = TwoRooms();
        Edge* door = EdgeAt(lot.stories[0], 6, 3, 6, 4);
        door->open = true, door->o.u0 = 0.1f, door->o.u1 = 0.9f, door->o.sill = 0.0f;
        const Plan plan = BuildPlan(lot);
        long seen = 0, outside = 0, fineOutside = 0, inCone = 0, inConeSeen = 0, at16 = 0, at32 = 0;
        const Plan fine = BuildPlan(lot, kCuts, 2 * kPx);
        Tally vsTwin, fineVsTwin;
        for (const P3& a : ptsA)
            for (const P3& b : ptsB) {
                int used = 0;
                const bool f = Trace(plan, lot, a.data(), b.data(), kLong, &used), twin = TwinClear(lot, a.data(), b.data());
                const bool ff = Trace(fine, lot, a.data(), b.data(), kLong);
                vsTwin.Add(f, twin), fineVsTwin.Add(ff, twin);
                const float r = (6.0f - a[0]) / (b[0] - a[0]), zc = a[2] + (b[2] - a[2]) * r, yc = a[1] + (b[1] - a[1]) * r;
                // (the cone: through the door's span under its head, with half a pixel of the 16 px plan (3 cm) at each jamb; the field's
                // bands end the head at 2.15 m. A real wall is thicker than that: the proxy's walls have no thickness)
                const bool cone = zc > 3.1f - 0.5f * px && zc < 3.9f + 0.5f * px && yc < 2.2f + 1e-3f;
                fineOutside += ff && !cone;
                // (well inside: 2 px off the jambs, and under the head from 2 px before the wall to 2 px after it)
                const float yr = std::fabs(b[1] - a[1]) * 2.0f * px / std::max(std::hypot(b[0] - a[0], b[2] - a[2]), 1e-6f);
                const bool well = yc + yr < 2.15f && zc > 3.1f && zc < 3.9f && PlanDist(a, b, 6.0f, 3.1f) > 2.0f * px && PlanDist(a, b, 6.0f, 3.9f) > 2.0f * px;
                seen += f, outside += f && !cone, inCone += well, inConeSeen += well && f;
                if (f) stepsUsed.push_back(used), at16 += used <= 16, at32 += used <= 32;
            }
        std::printf("  doorway: %ld pairs seen, %ld outside the door's cone; %ld of %ld pairs well inside it seen; against the twin leak %.3f %% dark %.3f %% (at %d px a tile: %.3f %%, %.3f %%); within 16 steps %.1f %%, 32 steps %.1f %%\n",
                    seen, outside, inConeSeen, inCone, vsTwin.Leak(), vsTwin.Dark(), 2 * kPx, fineVsTwin.Leak(), fineVsTwin.Dark(), 100.0 * at16 / std::max(seen, 1L), 100.0 * at32 / std::max(seen, 1L));
        Check(seen > 0 && outside == 0 && fineOutside == 0, "plan field: a doorway spills light outside the door's cone");
        Check(inConeSeen == inCone, "plan field: a pair well inside the door's cone is blocked");
        Check(vsTwin.Leak() < 0.5 && fineVsTwin.Leak() < 0.5 && vsTwin.Dark() < 10.0 && fineVsTwin.Dark() < vsTwin.Dark(), "plan field: the doorway's field and twin differ");
    }
    { // 3. a diagonal wall across a room
        Lot lot;
        Story s = MakeStory(0, 7, 7, 0.0f);
        Box(s, 1, 1, 6, 6);
        Line(s, 1, 1, 6, 6);
        lot.stories.push_back(std::move(s));
        const Plan plan = BuildPlan(lot);
        std::vector<P3> pts;
        for (const P3& p : GridPts(1.1f, 5.9f, 1.1f, 5.9f, 0.2f, {1.0f}))
            if (std::fabs(p[0] - p[2]) * 0.70710678f >= 0.08f) pts.push_back(p); // (0.08 m off the diagonal: 1.28 px)
        long across = 0, acrossSeen = 0, same = 0, sameSeen = 0, at32 = 0, differ = 0;
        for (size_t i = 0; i < pts.size(); i++)
            for (size_t j = i + 1; j < pts.size(); j++) {
                const bool cross = (pts[i][0] > pts[i][2]) != (pts[j][0] > pts[j][2]);
                int used = 0;
                const bool f = Trace(plan, lot, pts[i].data(), pts[j].data(), kLong, &used);
                (cross ? across : same)++, (cross ? acrossSeen : sameSeen) += f;
                if (f) stepsUsed.push_back(used), at32 += used <= 32;
                differ += f != TwinClear(lot, pts[i].data(), pts[j].data());
            }
        std::printf("  diagonal wall: %ld of %ld pairs seen across it; %ld of %ld pairs seen on one side (points from 0.08 m off it), %.1f %% within 32 steps\n",
                    acrossSeen, across, sameSeen, same, 100.0 * at32 / std::max(sameSeen, 1L));
        Check(acrossSeen == 0 && differ == 0, "plan field: light crosses a diagonal wall, or its field and twin differ");
        Check(sameSeen == same, "plan field: a diagonal wall shades points 0.08 m off it on its own side");
    }
    { // 4. a window (sill 0.9, head 2.1) and a half wall (1 m): the sill-height rule
        Lot lot = TwoRooms();
        Edge* win = EdgeAt(lot.stories[0], 6, 3, 6, 4);
        win->open = true, win->glazed = 1, win->o.u0 = 0.1f, win->o.u1 = 0.9f, win->o.sill = 0.9f;
        for (float& hd : win->o.head) hd = 2.1f;
        for (const float z : {5.0f, 6.0f}) {
            Edge* half = EdgeAt(lot.stories[0], 6, z, 6, z + 1);
            half->topL = half->topM = half->topR = 1.0f;
        }
        const Plan plan = BuildPlan(lot);
        const auto see = [&](float ya, float yb, float z) {
            const float a[3] = {4.0f, ya, z}, b[3] = {8.0f, yb, z};
            return Trace(plan, lot, a, b, kLong);
        };
        Check(see(1.5f, 1.5f, 3.5f) && !see(0.3f, 0.3f, 3.5f) && !see(2.6f, 2.6f, 3.5f), "plan field: a window passes light between its sill and head only");
        Check(see(1.5f, 1.5f, 6.0f) && !see(0.3f, 0.3f, 6.0f) && see(2.5f, 0.05f, 6.0f), "plan field: a half wall blocks under its top only");
        RtLampProxy::Mesh mesh;
        RtLampProxy::Build(lot, mesh);
        Tally exactVsMesh;
        bool first = true;
        const std::vector<std::pair<const char*, std::vector<float>>> rules = {
            {"1 band", {}}, {"2 bands cut at 1.0 m", {1.0f}}, {"the default 3 bands", kCuts}, {"4 equal bands", UniformCuts(4)}, {"8 equal bands", UniformCuts(8)}};
        for (const auto& [name, cuts] : rules) {
            const Plan pb = BuildPlan(lot, cuts);
            Tally rule, field;
            for (const P3& a : ptsA)
                for (const P3& b : ptsB) {
                    const bool exact = TwinClear(lot, a.data(), b.data(), nullptr), twin = TwinClear(lot, a.data(), b.data(), &cuts);
                    rule.Add(twin, exact);
                    field.Add(Trace(pb, lot, a.data(), b.data(), kLong), twin);
                    if (first) exactVsMesh.Add(exact, RtLampProxy::Clear(mesh, a.data(), b.data()));
                }
            first = false;
            std::printf("  window + half wall, %s: the rule against exact heights leak %.2f %% dark %.2f %%; the field against its twin leak %.3f %% dark %.3f %%\n",
                        name, rule.Leak(), rule.Dark(), field.Leak(), field.Dark());
            if (cuts == kCuts) Check(rule.Leak() < 5.0 && rule.Dark() < 5.0, "plan field: the sill-height rule at the default bands is over 5 % off the exact heights");
            Check(field.Leak() < 0.5 && field.Dark() < 10.0, "plan field: the window's field and twin differ");
        }
        std::printf("  the twin at exact heights against the proxy mesh's rays: %ld of %ld pairs differ\n", exactVsMesh.leak + exactVsMesh.dark, exactVsMesh.n);
        Check((exactVsMesh.leak + exactVsMesh.dark) * 10000 < exactVsMesh.n, // (under 0.01 %: crossings within 0.1 mm of two edges' joint)
              "plan field: the twin at exact heights and the proxy mesh's rays differ");
    }
    { // 5. a stairwell: two stories, a 2 x 2 tile hole (x, z 3..5) in the upper floor, a wall across story 0 at z = 6
        Lot lot;
        Story s0 = MakeStory(0, 8, 8, 0.0f), s1 = MakeStory(1, 8, 8, 3.0f);
        Box(s0, 1, 1, 7, 7), Box(s1, 1, 1, 7, 7);
        Line(s0, 1, 6, 7, 6);
        s1.placed.assign(64, 0);
        for (int iz = 1; iz < 7; iz++)
            for (int ix = 1; ix < 7; ix++) {
                const bool hole = ix >= 3 && ix < 5 && iz >= 3 && iz < 5;
                s0.roofed[iz * 8 + ix] = hole ? 0 : 0xF, s1.placed[iz * 8 + ix] = hole ? 0 : 0xF, s1.roofed[iz * 8 + ix] = 0xF;
            }
        lot.stories.push_back(std::move(s0)), lot.stories.push_back(std::move(s1));
        const Plan plan = BuildPlan(lot);
        RtLampProxy::Mesh mesh;
        RtLampProxy::Build(lot, mesh);
        const std::vector<P3> up = GridPts(1.3f, 6.5f, 1.3f, 6.5f, 0.4f, {3.5f, 5.5f}), down = GridPts(1.3f, 6.5f, 1.3f, 6.5f, 0.4f, {0.05f, 1.5f});
        long underSlab = 0, underSlabSeen = 0, hole = 0, holeSeen = 0;
        Tally fieldVsTwin, twinVsMesh, fieldVsMesh;
        for (const P3& a : up)
            for (const P3& b : down) {
                int used = 0;
                const bool f = Trace(plan, lot, a.data(), b.data(), kLong, &used), twin = TwinClear(lot, a.data(), b.data());
                const bool ref = RtLampProxy::Clear(mesh, a.data(), b.data());
                fieldVsTwin.Add(f, twin), twinVsMesh.Add(TwinClear(lot, a.data(), b.data(), nullptr), ref), fieldVsMesh.Add(f, ref);
                const float r = (3.0f - a[1]) / (b[1] - a[1]), xc = a[0] + (b[0] - a[0]) * r, zc = a[2] + (b[2] - a[2]) * r;
                const bool in = xc > 3.0f && xc < 5.0f && zc > 3.0f && zc < 5.0f;
                underSlab += !in, underSlabSeen += !in && f;
                hole += in && twin, holeSeen += in && twin && f;
                if (f) stepsUsed.push_back(used);
            }
        std::printf("  stairwell: %ld of %ld pairs seen through the floor; %ld of %ld pairs through the hole seen\n", underSlabSeen, underSlab, holeSeen, hole);
        std::printf("    field against twin leak %.3f %% dark %.3f %%; twin at exact heights against the mesh leak %.3f %% dark %.3f %%; field against the mesh leak %.3f %% dark %.3f %%\n",
                    fieldVsTwin.Leak(), fieldVsTwin.Dark(), twinVsMesh.Leak(), twinVsMesh.Dark(), fieldVsMesh.Leak(), fieldVsMesh.Dark());
        Check(underSlabSeen == 0, "plan field: light crosses a floor between two stories");
        Check(hole > 0 && holeSeen == hole && fieldVsTwin.leak == 0 && fieldVsTwin.Dark() < 2.0, "plan field: the stairwell's field and twin differ");
        Check(twinVsMesh.Dark() == 0.0 && twinVsMesh.Leak() < 3.0, "plan field: the story walk and the proxy mesh's rays differ at the stairwell");
    }
    if (!stepsUsed.empty()) {
        std::sort(stepsUsed.begin(), stepsUsed.end());
        const size_t n = stepsUsed.size();
        const auto within = [&](int k) { return 100.0 * static_cast<double>(std::upper_bound(stepsUsed.begin(), stepsUsed.end(), k) - stepsUsed.begin()) / static_cast<double>(n); };
        std::printf("  steps of the %zu clear traces: median %d, 99 %% %d, most %d; within 8: %.1f %%, 16: %.1f %%, 24: %.1f %%, 32: %.1f %%\n", n, stepsUsed[n / 2],
                    stepsUsed[n * 99 / 100], stepsUsed[n - 1], within(8), within(16), within(24), within(32));
    }
}
