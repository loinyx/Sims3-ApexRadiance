#pragma once
// Doors, windows, arches and half walls in the render thread's copies of the outside walls (level_light_share.cpp
// CaptureStoryWalls). A copied wall is a segment (world xz, base y0, top y1 = y0 + 3 m) that stopped every ray crossing it
// between y0 and y1, so a lamp inside an arcade lit nothing outside it through the arches, and a garden post never lit the
// ground or an object past a door. Each segment now carries patches: one per 1 m cell of its story grid (StoryGrid) whose
// wall is lower than the copy (a half wall, a foundation side) or holds an opening, in the segment's own u (0 at x0 z0, 1
// at x1 z1). A ray crossing the segment at (u, y) passes under y0 and over the top as before; within a patch it passes over
// the cell's own top, and through its opening with the lamp's penumbra (penumbra.h, as the light tree thread's grid walk
// has it: the spot of the lamp's disc where the ray crosses, spread over the cells beside, so two cut cells side by side
// leave no seam). Where no patch lies the wall is whole, so a wall whose openings are not known (no grid for its story, a
// diagonal wall) stays closed; past the segment's ends the spot is not spread (its neighbour there is not known).
// POD and inline functions only, no game reads: the offline harness (tools/roofed_openings_test) tests it directly.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>
#include "penumbra.h"

namespace WallOpenings {

constexpr int kProfile = 5;      // head heights across an opening, u0..u1 evenly (an arch's curve; flat by default)
constexpr float kSoft = 0.04f;   // m: the narrowest soft rim (2 x Penumbra::kMinHalf)
constexpr float kEdge = 0.02f;   // m: the hard margin the copies always had at a wall's base and top

struct Patch {
    float u0 = 0, u1 = 0;        // the cell's span along the segment (u0 < u1)
    float top0 = 0, top1 = 0;    // world y of the cell's top at u0 and u1 (at most the segment's y1)
    float ou0 = 0, ou1 = 0;      // the opening's span along the segment (when open)
    float sill = 0;              // world y
    float head[kProfile] = {};   // world y at ou0 .. ou1
    uint8_t open = 0;            // 1 = the cell holds an opening
    uint8_t jamb0 = 0, jamb1 = 0; // a jamb (soft rim) at ou0 / ou1
    uint8_t shaped = 0;          // 1 = sill and head from the opening's cutout mask, 2 = from its window's panes, 0 = its kind's defaults
    // (2.15.3) a sill on the floor (an arch, a door): no rim under it, as the grid walk's kFloorSill (the ground just past an
    // arch lost up to 10 % of an outdoor lamp to the sill's rim)
    uint8_t floorSill = 0;
    float glass = 1.0f;          // (2.15.3) the share its opening passes: kWindowPass for a window's glass and frame, 1 for doors and arches
};

// (2.15.3) Wall thickness: the game lights a wall from two faces, each 0.05 m inside its cell from the edge line (the wall
// setup FUN_006AB280 adds [0x00FF37E0] = 0.05 to the start of a -z / -x face and [0x01158B90] = 0.95 to a +z / +x one,
// [0x00FF37E4] = 0.0353 across a diagonal), and a window's two daylight panes (type 7) stand 0.05 m in front of each side
// (Lighting snapshot: pairs 0.1 m apart). So a wall is 0.1 m thick for light. A ray through an opening is tested again at
// both faces and keeps the smaller share: the beam narrows at grazing angles (at 60 degrees off the wall's normal each jamb
// takes 9 cm more of it). The drawn mesh's own thickness is not read.
constexpr float kHalfThick = 0.05f;
constexpr float kFaceShiftMax = 0.5f; // m: the faces' shift along the wall and up is capped (a ray almost along the wall)
// (2.15.3) Light through a window: clear glass passes about 0.85 at the angles that matter (a single pane at normal incidence
// 0.90, double glazing about 0.8) and the frame's bars and the mullions between panes cover about a tenth of the hole: 0.85 x
// 0.9. Doors and arches pass 1. A chosen constant, not read from the game (it keeps no glass value for lamp light).
constexpr float kGlassPass = 0.85f, kFrameOpen = 0.9f, kWindowPass = kGlassPass * kFrameOpen;
// A ray's run per metre across a wall's line (sx, sz): metres along the wall (from its start toward its end) and up. on =
// false for a ray parallel to it.
struct Across {
    float along = 0.0f, up = 0.0f;
    bool on = false;
};
inline Across AcrossOf(float rx, float ry, float rz, float sx, float sz) {
    const float m = std::sqrt(sx * sx + sz * sz);
    if (!(m > 1e-6f)) return {};
    const float n = std::fabs(rx * sz - rz * sx) / m;
    if (!(n > 1e-6f)) return {};
    return {(rx * sx + rz * sz) / m / n, ry / n, true};
}

inline float Ramp(float x, float soft = kSoft) {
    const float t = std::clamp(x / soft + 0.5f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline float HeadAt(const Patch& p, float u) {
    if (!(p.ou1 > p.ou0)) return p.head[0];
    const float f = std::clamp((u - p.ou0) / (p.ou1 - p.ou0), 0.0f, 1.0f) * static_cast<float>(kProfile - 1);
    const int i = std::min(static_cast<int>(f), kProfile - 2);
    return p.head[i] + (p.head[i + 1] - p.head[i]) * (f - static_cast<float>(i));
}

// The spot of the lamp's disc on a wall whose line runs (sx, sz), crossed at t (0 lamp, 1 point) by the ray (dx, dy, dz)
inline Penumbra::Rim RimAt(float t, float dx, float dy, float dz, float sx, float sz, float radius) {
    if (!Penumbra::g_on.load(std::memory_order_relaxed)) return Penumbra::Rim{}; // (penumbra off: the 4 cm rim, no slant)
    return Penumbra::SlantOf(dx, dy, dz, sz, -sx).At(Penumbra::Half(radius, t));
}

// The share of a ray that crosses the segment's line at u (0..1 along it, the segment `segLen` metres long) at world height
// y, the lamp's spot there `rim` wide (half widths; the default is the narrowest rim). The spot is spread along the segment
// over the patches it covers: a patch's opening passes Cover(sill) x Cover(head) or over its top, its solid part over its
// top only, the wall between patches nothing. why: 0 under its base or over its top (or no wall there), 1 over a patch's
// lower top, 2 through an opening, 3 stopped. opened: the spot covered an opening.
inline float CrossPassAt(const Patch* ps, int n, float segLen, float y0, float y1, float u, float y, int* why, const Penumbra::Rim& rim, bool& opened) {
    if (why) *why = 0;
    if (!(y > y0 + kEdge && y < y1 - kEdge)) return 1.0f; // (the copies' rule before patches)
    const float m = std::max(segLen, 1e-3f);
    const float c = u * m, h = rim.along, a = std::max(0.0f, c - h), b = std::min(m, c + h);
    const float total = Penumbra::Weight(c, h, a, b);
    float over = 0.0f, through = 0.0f;
    for (int i = 0; i < n; i++) {
        const Patch& p = ps[i];
        const float p0 = std::max(p.u0 * m, a), p1 = std::min(p.u1 * m, b);
        if (!(p1 > p0)) continue;
        const auto topAt = [&](float v) { // v: metres along
            const float f = p.u1 > p.u0 ? std::clamp((v / m - p.u0) / (p.u1 - p.u0), 0.0f, 1.0f) : 0.0f;
            return p.top0 + (p.top1 - p.top0) * f;
        };
        const auto piece = [&](float s0, float s1, bool open) { // [s0, s1] within the patch: its weight x its pass
            s0 = std::max(s0, p0), s1 = std::min(s1, p1);
            if (!(s1 > s0)) return;
            const float w = Penumbra::Weight(c, h, s0, s1), v = std::clamp(c, s0, s1);
            const float ov = Penumbra::Cover(y - topAt(v), rim.up);
            float in = 0.0f;
            if (open) {
                opened = true;
                in = (p.floorSill ? 1.0f : Penumbra::Cover(y - p.sill, rim.up)) * Penumbra::Cover(HeadAt(p, v / m) - y, rim.up) * p.glass;
            }
            if (in > ov) through += w * in;
            else over += w * ov;
        };
        if (!p.open) {
            piece(p0, p1, false);
            continue;
        }
        const float o0 = p.ou0 * m, o1 = p.ou1 * m;
        piece(p0, o0, false);
        piece(o0, o1, true);
        piece(o1, p1, false);
    }
    const float pass = total > 0.0f ? std::clamp((over + through) / total, 0.0f, 1.0f) : 0.0f;
    if (why) *why = pass <= 0.0f ? 3 : through >= over ? 2 : 1;
    return pass;
}

// CrossPassAt; (2.15.3) a ray through an opening (across: its run across the wall, AcrossOf) is tested at the wall's two
// faces instead (kHalfThick) and keeps the smaller share. Rays that cover no opening cost what they did.
inline float CrossPass(const Patch* ps, int n, float segLen, float y0, float y1, float u, float y, int* why = nullptr, const Penumbra::Rim& rim = {}, const Across& across = {}) {
    bool opened = false;
    const float pass = CrossPassAt(ps, n, segLen, y0, y1, u, y, why, rim, opened);
    if (!opened || !across.on) return pass;
    const float m = std::max(segLen, 1e-3f);
    const float du = std::clamp(kHalfThick * across.along, -kFaceShiftMax, kFaceShiftMax) / m, dy = std::clamp(kHalfThick * across.up, -kFaceShiftMax, kFaceShiftMax);
    int w0 = 0, w1 = 0;
    bool unused = false;
    // (the faces stay on the segment: past its ends its neighbour is not known, as for the spot)
    const float p0 = CrossPassAt(ps, n, segLen, y0, y1, std::clamp(u - du, 0.0f, 1.0f), y - dy, &w0, rim, unused);
    const float p1 = CrossPassAt(ps, n, segLen, y0, y1, std::clamp(u + du, 0.0f, 1.0f), y + dy, &w1, rim, unused);
    if (why) *why = p0 <= p1 ? w0 : w1;
    return std::min(p0, p1);
}

// ---- Opening shapes from the cutout masks ----
// The game keeps each opening's exact shape in a mask texture, one per wall cell (the cut post's TGI, type 0x00B2D882), that
// it draws the wall through. tools/opening_mask_table reads them from the game's packages into the table below, so nothing is
// read at run time. shortcut: custom content masks are not in the table and keep today's shape; a run time reader would add them.
struct MaskShape {
    float u0 = 0, u1 = 1, sill = 0;
    float head[kProfile] = {};
};
struct MaskRow { // in 1/255: u0, u1 of the cell, sill and head of the mask's 3 m
    uint8_t u0, u1, sill, head[kProfile];
};
struct MaskEntry { // sorted by instance
    uint64_t instance;
    uint16_t shape; // index in kMaskShapes
};
#include "opening_masks_table.inc"
constexpr uint32_t kMaskType = 0x00B2D882;
constexpr float kMaskHeightM = 3.0f;
// A post's orientation 2, 3, 4 and 5 run from the high corner to the low one: the mask's u runs against EdgeKey's low-first edge
// (read in 928 logged posts: orientation gives the direction of each cell's first post)
inline bool MaskRunsBack(uint8_t orient) { return orient >= 2 && orient <= 5; }
// The hole of a mask (instance = tgi[1]:tgi[0]) along a low-first edge; false when the table does not hold it
inline bool BakedMask(uint64_t instance, bool runsBack, MaskShape& out) {
    const MaskEntry* const end = std::end(kMasks);
    const MaskEntry* const it = std::lower_bound(std::begin(kMasks), end, instance, [](const MaskEntry& e, uint64_t v) { return e.instance < v; });
    if (it == end || it->instance != instance) return false;
    const MaskRow& r = kMaskShapes[it->shape];
    const float u0 = static_cast<float>(r.u0) / 255.0f, u1 = static_cast<float>(r.u1) / 255.0f;
    out.u0 = runsBack ? 1.0f - u1 : u0;
    out.u1 = runsBack ? 1.0f - u0 : u1;
    out.sill = static_cast<float>(r.sill) * kMaskHeightM / 255.0f;
    for (int k = 0; k < kProfile; k++) out.head[k] = static_cast<float>(r.head[runsBack ? kProfile - 1 - k : k]) * kMaskHeightM / 255.0f;
    return true;
}

} // namespace WallOpenings
