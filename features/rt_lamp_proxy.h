#pragma once
// Real-time outdoor lamps (Lamp lighting > Instant, "lampLightingMode"; docs/features/night-lighting/rt-outdoor-lamps.md): the occluder
// proxy a lamp's shadow is rendered from, and the CPU reference of what it lets through. Pure functions, no game reads:
// the in-game code (rt_outdoor_lamps.cpp, fed by LevelLightShare's story grids and placed-floor copies) and the offline
// harness (tools/rt_lamps_test, fed by a Full capture's World.txt or synthetic stories) build the same mesh from the same
// numbers.
//
// What a lot story gives (lot space, metres, the 1 m tiles of its grid; level_light_share.cpp StoryGrid / FloorSnap):
//   - each wall edge (a tile side or a tile diagonal) with its foot, the floor its openings stand on (base) and its top at its
//     low end, middle and high end (taller walls, gables); an opening on it: its span u0..u1 along the edge, its sill and its
//     head profile (m over the base). Doors that block light are not openings (the grid leaves them out);
//   - each tile's floor height and, per quadrant (the tile split along its diagonals: 0 toward -z, 1 +x, 2 +z, 3 -x;
//     lamp_visibility.h Quadrant), whether a roof or a ceiling covers it (at floor + 3 m, as the grid walk's roof test);
//   - for stories 1 and up, the placed floor quadrants (slabs: balconies, decks, upper floors).
// The mesh, every triangle double-sided:
//   - a wall is its edge's vertical quad from foot - kFootExt to its top (piecewise linear through L, M, R), with each
//     opening a hole from base + sill to base + head(u) (the head profile at kProfile points);
//   - a roofed quadrant is a horizontal triangle at its ceiling;
//   - a placed quadrant is a slab kSlabThick thick under its floor: its top and bottom triangles, and a side wherever the
//     quadrant beside it is not placed at the same height.
// The reference (Visibility): the share of a lamp ball of radius R (Shadow softness) a point sees past the mesh, by rays to
// kRefSamples points of the disc across the ray (a sphere seen from afar is that disc), each ray tested against every
// triangle it can meet (a uniform 1 m grid over the lot).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>
#include "open_rooms_above.h"
#include "wall_openings.h"

namespace RtLampProxy {

constexpr int kProfile = 5;          // the head heights across an opening (WallOpenings::kProfile)
constexpr float kFootExt = 0.10f;    // m: walls reach this far under their foot (no light leaks under a wall onto a floor)
constexpr float kSlabThick = 0.20f;  // m: a placed floor's thickness
constexpr float kNoFloor = 1e9f;

struct Opening {
    float u0 = 0.0f, u1 = 1.0f; // along the edge from its low end
    float sill = 0.0f;          // m over the edge's base
    float head[kProfile] = {2.2f, 2.2f, 2.2f, 2.2f, 2.2f};
};
struct Edge {
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0; // lot space: its low end, its high end
    float foot = 0, base = 0;
    float topL = 3, topM = 3, topR = 3;
    bool open = false;
    uint8_t glazed = 0; // (the padding after open: the layout scenes hold) its opening passes light through glass (PaneCount)
    Opening o;
};
struct Story {
    int story = 0;
    int w = 0, h = 0;
    std::vector<Edge> edges;
    std::vector<float> floorY;   // per tile (iz * w + ix), kNoFloor = none
    std::vector<uint8_t> roofed; // per tile: bit q = quadrant q under a roof or a ceiling
    std::vector<float> ceil;     // per tile: the roof's height (floor + 3)
    std::vector<uint8_t> placed; // per tile: bit q = quadrant q holds a placed floor (stories 1 and up; empty = not read)
    std::vector<uint8_t> removed; // per tile: bit q = quadrant q's floor was removed, an opening (RemovedFloorKey; as placed)
    std::vector<int32_t> room;   // per tile, 4: the quadrants' room ids (empty = not known; Build's open tops)
    float plane = kNoFloor;      // the story's lowest floor (a placed quadrant with no tile floor stands at it)
};
struct Lot {
    float worldToLot[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}; // rows (mgr+0xE0): lot = world x rows
    std::vector<Story> stories;
};

inline void ToLot(const Lot& l, const float w[3], float out[3]) {
    for (int k = 0; k < 3; k++) out[k] = w[0] * l.worldToLot[0][k] + w[1] * l.worldToLot[1][k] + w[2] * l.worldToLot[2][k] + l.worldToLot[3][k];
}
// A direction (normal) into lot space (the rotation part; the lot matrix is a rotation and a translation)
inline void DirToLot(const Lot& l, const float w[3], float out[3]) {
    for (int k = 0; k < 3; k++) out[k] = w[0] * l.worldToLot[0][k] + w[1] * l.worldToLot[1][k] + w[2] * l.worldToLot[2][k];
}
// A glazed opening as the GPU's table holds it (BlockerTable::AppendPanes): its span in lot space, its sill and head
struct Pane {
    float x0, z0, x1, z1, y0, y1;
};
inline void PanesOf(const Lot& lot, std::vector<Pane>& out) {
    out.clear();
    for (const Story& s : lot.stories)
        for (const Edge& e : s.edges) {
            if (!e.open || !e.glazed) continue;
            const float dx = e.x1 - e.x0, dz = e.z1 - e.z0;
            out.push_back({e.x0 + e.o.u0 * dx, e.z0 + e.o.u0 * dz, e.x0 + e.o.u1 * dx, e.z0 + e.o.u1 * dz, e.base + e.o.sill, e.base + *std::max_element(e.o.head, e.o.head + kProfile)});
        }
}
// The panes the segment a -> b crosses (T3dDensePS PaneCount, its operations). The game charges a pane's pass for each: 50 wall texels
// behind two panes read 0.585 of the GPU's light, 05/11. Half open along a span: two windows side by side on one line count once
inline int PaneCount(const std::vector<Pane>& panes, const float a[3], const float b[3]) {
    int n = 0;
    const float dx = b[0] - a[0], dz = b[2] - a[2];
    for (const Pane& p : panes) {
        const float ex = p.x1 - p.x0, ez = p.z1 - p.z0, ox = p.x0 - a[0], oz = p.z0 - a[2];
        const float den = dx * ez - dz * ex;
        const float t = (ox * ez - oz * ex) / den, u = (ox * dz - oz * dx) / den;
        const float y = a[1] + t * (b[1] - a[1]);
        n += t > 0.0f && t < 1.0f && u >= 0.0f && u < 1.0f && y >= p.y0 && y <= p.y1;
    }
    return n;
}
// That share of the light, taken after the soft cut (cut after it, the faint light past two panes went black: 771 lit kitchen floor texels
// fell to 595, 05/11)
inline float PaneShare(const std::vector<Pane>& panes, const float a[3], const float b[3]) { return std::pow(WallOpenings::kWindowPass, static_cast<float>(PaneCount(panes, a, b))); }

// A room's texel the game samples just past its own diagonal wall (a half tile's texel, its centre over the wall line; the game's
// tests light it, the shadow mesh's wall shaded it: a dark band along the room's diagonal walls, 1:8 of the scene of 03/11) is
// taken for the shadow test kInset inside the nearest quadrant of its room in the same tile (lot space, P's height kept). False:
// it lies in its room, its tile holds no quadrant of the room, the ids are not known, or it is over kNear from that quadrant
inline bool IntoRoom(const Lot& lot, int story, int id, float P[3]) {
    constexpr float kInset = 0.03f, kNear = 0.5f;
    const Story* s = nullptr;
    for (const Story& st : lot.stories)
        if (st.story == story) s = &st;
    if (!s || id <= 0) return false;
    const float fx0 = std::floor(P[0]), fz0 = std::floor(P[2]);
    const int ix = static_cast<int>(fx0), iz = static_cast<int>(fz0);
    if (ix < 0 || iz < 0 || ix >= s->w || iz >= s->h || s->room.size() != 4 * static_cast<size_t>(s->w) * s->h) return false;
    const int32_t* q = &s->room[4 * (static_cast<size_t>(iz) * s->w + ix)];
    const float fx = P[0] - fx0, fz = P[2] - fz0;
    const int at = std::fabs(fx - 0.5f) <= std::fabs(fz - 0.5f) ? (fz > 0.5f ? 2 : 0) : (fx > 0.5f ? 1 : 3); // (LampVis::Quadrant)
    if (q[at] == id) return false;
    const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}}; // quadrant k: corners k, k + 1 and the centre (QuadrantCorners)
    float best = kNear * kNear, bx = 0, bz = 0;
    for (int k = 0; k < 4; k++) {
        if (q[k] != id) continue;
        const float t[3][2] = {{corners[k][0], corners[k][1]}, {corners[(k + 1) & 3][0], corners[(k + 1) & 3][1]}, {0.5f, 0.5f}};
        const float cx = (t[0][0] + t[1][0] + t[2][0]) / 3.0f, cz = (t[0][1] + t[1][1] + t[2][1]) / 3.0f;
        for (int e = 0; e < 3; e++) { // the nearest point of each side (P is outside the triangle)
            const float* a = t[e];
            const float* b = t[(e + 1) % 3];
            const float dx = b[0] - a[0], dz = b[1] - a[1];
            const float u = std::clamp(((fx - a[0]) * dx + (fz - a[1]) * dz) / (dx * dx + dz * dz), 0.0f, 1.0f);
            const float nx = a[0] + u * dx, nz = a[1] + u * dz;
            const float d2 = (fx - nx) * (fx - nx) + (fz - nz) * (fz - nz);
            if (d2 < best) {
                const float ix2 = cx - nx, iz2 = cz - nz, il = std::sqrt(ix2 * ix2 + iz2 * iz2);
                best = d2, bx = nx + ix2 / il * kInset, bz = nz + iz2 / il * kInset;
            }
        }
    }
    if (!(best < kNear * kNear)) return false;
    P[0] = fx0 + bx, P[2] = fz0 + bz;
    return true;
}

struct Tri {
    float v[3][3];
};
struct Mesh {
    std::vector<Tri> tris;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    int walls = 0, wallTris = 0, openings = 0, roofTris = 0, slabTris = 0;
    int openTops = 0; // roofed quadrants left without their ceiling (Build ceilingsOpen: OpenTops)
    std::vector<std::pair<int, int>> openRooms; // (story, room) rooms open to the story above by OpenTops' room rule, sorted
    // the uniform grid over x / z (1 m cells) the reference walks
    int gx0 = 0, gz0 = 0, gw = 0, gh = 0;
    std::vector<uint32_t> cellStart, cellTris; // CSR: cell c holds cellTris[cellStart[c] .. cellStart[c + 1])
    // what each triangle is (the self-check names the blocker): kind 0 wall, 1 roof or ceiling, 2 slab, 3 object shape; its story
    enum : uint8_t { kWall = 0, kRoof = 1, kSlab = 2, kShape = 3 };
    uint8_t curKind = kWall;
    int8_t curStory = 0;
    std::vector<uint8_t> kind;
    std::vector<int8_t> story;
};

// ---- building ----
inline void AddTri(Mesh& m, const float a[3], const float b[3], const float c[3]) {
    Tri t;
    for (int k = 0; k < 3; k++) {
        t.v[0][k] = a[k];
        t.v[1][k] = b[k];
        t.v[2][k] = c[k];
    }
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) {
            if (t.v[i][k] < m.lo[k]) m.lo[k] = t.v[i][k];
            if (t.v[i][k] > m.hi[k]) m.hi[k] = t.v[i][k];
        }
    m.tris.push_back(t);
    m.kind.push_back(m.curKind);
    m.story.push_back(m.curStory);
}
// The quad (u0, y00) .. (u1, y11) of an edge: bottom y0a at u0, y0b at u1; top y1a at u0, y1b at u1
inline int AddWallQuad(Mesh& m, const Edge& e, float u0, float u1, float y0a, float y0b, float y1a, float y1b) {
    if (!(u1 > u0 + 1e-5f)) return 0;
    if (!(y1a > y0a + 1e-4f) && !(y1b > y0b + 1e-4f)) return 0;
    if (y1a < y0a) y1a = y0a;
    if (y1b < y0b) y1b = y0b;
    const float dx = e.x1 - e.x0, dz = e.z1 - e.z0;
    const float a[3] = {e.x0 + dx * u0, y0a, e.z0 + dz * u0}, b[3] = {e.x0 + dx * u1, y0b, e.z0 + dz * u1};
    const float c[3] = {e.x0 + dx * u1, y1b, e.z0 + dz * u1}, d[3] = {e.x0 + dx * u0, y1a, e.z0 + dz * u0};
    AddTri(m, a, b, c);
    AddTri(m, a, c, d);
    return 2;
}
inline float EdgeTop(const Edge& e, float u) {
    u = std::clamp(u, 0.0f, 1.0f);
    return u < 0.5f ? e.topL + (e.topM - e.topL) * u * 2.0f : e.topM + (e.topR - e.topM) * (u - 0.5f) * 2.0f;
}
inline float OpeningHead(const Opening& o, float u) {
    if (!(o.u1 > o.u0)) return o.head[0];
    const float f = std::clamp((u - o.u0) / (o.u1 - o.u0), 0.0f, 1.0f) * static_cast<float>(kProfile - 1);
    const int i = std::min(static_cast<int>(f), kProfile - 2);
    return o.head[i] + (o.head[i + 1] - o.head[i]) * (f - static_cast<float>(i));
}
// One wall with its opening as a hole. The top is piecewise linear through u = 0, 0.5, 1 and the head through its profile:
// every span is cut at those knots so each quad is exact.
inline void AddWall(Mesh& m, const Edge& e, float footExt = kFootExt) {
    const float foot = e.foot - footExt;
    std::vector<float> knots = {0.0f, 0.5f, 1.0f};
    if (e.open) {
        const float u0 = std::clamp(e.o.u0, 0.0f, 1.0f), u1 = std::clamp(e.o.u1, 0.0f, 1.0f);
        if (u1 > u0)
            for (int i = 0; i < kProfile; i++) knots.push_back(u0 + (u1 - u0) * static_cast<float>(i) / static_cast<float>(kProfile - 1));
    }
    std::sort(knots.begin(), knots.end());
    knots.erase(std::unique(knots.begin(), knots.end(), [](float a, float b) { return std::fabs(a - b) < 1e-5f; }), knots.end());
    int tris = 0;
    for (size_t i = 0; i + 1 < knots.size(); i++) {
        const float a = knots[i], b = knots[i + 1], mid = 0.5f * (a + b);
        const float ta = EdgeTop(e, a), tb = EdgeTop(e, b);
        const bool inOpening = e.open && mid > e.o.u0 && mid < e.o.u1;
        if (!inOpening) {
            tris += AddWallQuad(m, e, a, b, foot, foot, ta, tb);
            continue;
        }
        const float sill = e.base + e.o.sill;
        const float ha = std::min(e.base + OpeningHead(e.o, a), ta), hb = std::min(e.base + OpeningHead(e.o, b), tb);
        if (sill > foot + 1e-4f) tris += AddWallQuad(m, e, a, b, foot, foot, std::min(sill, ta), std::min(sill, tb));
        tris += AddWallQuad(m, e, a, b, std::max(ha, foot), std::max(hb, foot), ta, tb);
    }
    m.walls++;
    m.wallTris += tris;
    if (e.open) m.openings++;
}
// The quadrant triangle of tile (ix, iz) at height y (corners in the quadrant's order, the tile's centre last)
inline void QuadrantCorners(int ix, int iz, int q, float y, float a[3], float b[3], float c[3]) {
    const float x0 = static_cast<float>(ix), z0 = static_cast<float>(iz), x1 = x0 + 1.0f, z1 = z0 + 1.0f;
    const float cx = x0 + 0.5f, cz = z0 + 0.5f;
    const float corners[4][2] = {{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}}; // q 0: -z side (corners 0, 1), 1: +x (1, 2), 2: +z (2, 3), 3: -x (3, 0)
    const int i0 = q, i1 = (q + 1) & 3;
    a[0] = corners[i0][0], a[1] = y, a[2] = corners[i0][1];
    b[0] = corners[i1][0], b[1] = y, b[2] = corners[i1][1];
    c[0] = cx, c[1] = y, c[2] = cz;
}
inline float FloorOf(const Story& s, int ix, int iz) {
    if (ix < 0 || iz < 0 || ix >= s.w || iz >= s.h) return kNoFloor;
    const size_t t = static_cast<size_t>(iz) * s.w + ix;
    const float f = t < s.floorY.size() ? s.floorY[t] : kNoFloor;
    return f < kNoFloor * 0.5f ? f : s.plane;
}
inline bool PlacedQ(const Story& s, int ix, int iz, int q) {
    if (ix < 0 || iz < 0 || ix >= s.w || iz >= s.h) return false;
    const size_t t = static_cast<size_t>(iz) * s.w + ix;
    return t < s.placed.size() && ((s.placed[t] >> q) & 1u);
}
// A room-0 texel of `story` (lot space P, normal N) facing into a room of the story below that the open-top build opened to this
// story (Mesh::openRooms, sorted): one space two stories tall, so the texel is that room's wall, not an outdoor one (03/11, lot C49C:
// the upper half of a double-height room's walls took the street lamps through the windows as room 0, the lower half not). Its
// quadrant is taken kStep along the normal; no floor may be placed there on this story
inline bool OverOpenBelow(const Lot& lot, const std::vector<std::pair<int, int>>& openRooms, int story, const float P[3], const float N[3]) {
    constexpr float kStep = 0.25f;
    const Story *below = nullptr, *here = nullptr;
    for (const Story& s : lot.stories) {
        if (s.story == story - 1) below = &s;
        if (s.story == story) here = &s;
    }
    if (!below || !here || openRooms.empty() || below->room.size() != 4 * static_cast<size_t>(below->w) * below->h) return false;
    const float x = P[0] + N[0] * kStep, z = P[2] + N[2] * kStep, fx0 = std::floor(x), fz0 = std::floor(z);
    const int ix = static_cast<int>(fx0), iz = static_cast<int>(fz0);
    if (!(ix >= 0 && iz >= 0 && ix < below->w && iz < below->h)) return false;
    const float fx = x - fx0, fz = z - fz0;
    const int q = std::fabs(fx - 0.5f) <= std::fabs(fz - 0.5f) ? (fz > 0.5f ? 2 : 0) : (fx > 0.5f ? 1 : 3); // (LampVis::Quadrant)
    const int id = below->room[4 * (static_cast<size_t>(iz) * below->w + ix) + q];
    return id > 0 && std::binary_search(openRooms.begin(), openRooms.end(), std::make_pair(story - 1, id)) && !PlacedQ(*here, ix, iz, q);
}
inline void AddSlabSide(Mesh& m, const float a[3], const float b[3], float top, float bottom) {
    const float p0[3] = {a[0], bottom, a[2]}, p1[3] = {b[0], bottom, b[2]}, p2[3] = {b[0], top, b[2]}, p3[3] = {a[0], top, a[2]};
    AddTri(m, p0, p1, p2);
    AddTri(m, p0, p2, p3);
    m.slabTris += 2;
}
// below: the story under s (or null). With it, a placed quadrant lying on a roofed quadrant of that story at its ceiling (an
// upper floor over a room) gets no slab: the roofed quadrant's plane already parts the two stories, and a 0.2 m slab under
// the floor would cover that room's ceiling and a lamp hung under it (GPU lamps for every room lights ceilings)
inline bool OverCeiling(const Story* below, const Story& s, int ix, int iz, int q, float y) {
    if (!below || below->w != s.w || below->h != s.h || ix < 0 || iz < 0 || ix >= s.w || iz >= s.h) return false;
    const size_t t = static_cast<size_t>(iz) * s.w + ix;
    return t < below->roofed.size() && ((below->roofed[t] >> q) & 1u) && t < below->ceil.size() && std::fabs(below->ceil[t] - y) < 0.05f;
}
inline void AddSlabs(Mesh& m, const Story& s, const Story* below = nullptr) {
    for (int iz = 0; iz < s.h; iz++)
        for (int ix = 0; ix < s.w; ix++)
            for (int q = 0; q < 4; q++) {
                if (!PlacedQ(s, ix, iz, q)) continue;
                const float y = FloorOf(s, ix, iz);
                if (!(y < kNoFloor * 0.5f)) continue;
                if (OverCeiling(below, s, ix, iz, q, y)) continue;
                float a[3], b[3], c[3];
                QuadrantCorners(ix, iz, q, y, a, b, c);
                AddTri(m, a, b, c);
                float a2[3] = {a[0], y - kSlabThick, a[2]}, b2[3] = {b[0], y - kSlabThick, b[2]}, c2[3] = {c[0], y - kSlabThick, c[2]};
                AddTri(m, a2, c2, b2);
                m.slabTris += 2;
                // the outer side (toward the next tile) and the two diagonal sides (toward the quadrants beside it)
                static constexpr int kDx[4] = {0, 1, 0, -1}, kDz[4] = {-1, 0, 1, 0};
                const int nx = ix + kDx[q], nz = iz + kDz[q], nq = (q + 2) & 3;
                if (!PlacedQ(s, nx, nz, nq) || std::fabs(FloorOf(s, nx, nz) - y) > 1e-3f) AddSlabSide(m, a, b, y, y - kSlabThick);
                if (!PlacedQ(s, ix, iz, (q + 1) & 3)) AddSlabSide(m, b, c, y, y - kSlabThick);
                if (!PlacedQ(s, ix, iz, (q + 3) & 3)) AddSlabSide(m, c, a, y, y - kSlabThick);
            }
}
inline void AddRoofs(Mesh& m, const Story& s) {
    for (int iz = 0; iz < s.h; iz++)
        for (int ix = 0; ix < s.w; ix++) {
            const size_t t = static_cast<size_t>(iz) * s.w + ix;
            const uint8_t r = t < s.roofed.size() ? s.roofed[t] : 0;
            if (!r || t >= s.ceil.size() || !(s.ceil[t] < kNoFloor * 0.5f)) continue;
            for (int q = 0; q < 4; q++) {
                if (!((r >> q) & 1u)) continue;
                float a[3], b[3], c[3];
                QuadrantCorners(ix, iz, q, s.ceil[t], a, b, c);
                AddTri(m, a, b, c);
                m.roofTris++;
            }
        }
}
// An object shape (lot space): a box as its 12 triangles, a cylinder as a 12-sided prism (24 triangles); returns the
// triangles added (GPU lamps for every room: the object shadows; the shadow pass leaves out the shapes holding a lamp)
struct ObjShape {
    float c[3];
    float u[3][3]; // unit axes (rows)
    float h[3];    // half extents
    bool cylinder = false;
    int axis = 1;
    float rad = 0.0f;
};
inline int AddShape(Mesh& m, const ObjShape& s) {
    m.curKind = Mesh::kShape;
    const auto P = [&](float a, float b, float c, float out[3]) {
        for (int k = 0; k < 3; k++) out[k] = s.c[k] + a * s.u[0][k] + b * s.u[1][k] + c * s.u[2][k];
    };
    int n = 0;
    if (!s.cylinder) {
        float v[8][3];
        for (int i = 0; i < 8; i++) P((i & 1) ? s.h[0] : -s.h[0], (i & 2) ? s.h[1] : -s.h[1], (i & 4) ? s.h[2] : -s.h[2], v[i]);
        static const int kQuads[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
        for (const auto& q : kQuads) {
            AddTri(m, v[q[0]], v[q[1]], v[q[2]]);
            AddTri(m, v[q[0]], v[q[2]], v[q[3]]);
            n += 2;
        }
        return n;
    }
    const int ax = std::clamp(s.axis, 0, 2), a1 = (ax + 1) % 3, a2 = (ax + 2) % 3;
    constexpr int kSides = 12;
    for (int i = 0; i < kSides; i++) {
        const float t0 = 6.2831853f * i / kSides, t1 = 6.2831853f * (i + 1) / kSides;
        float c0[3] = {}, c1[3] = {};
        c0[a1] = std::cos(t0) * s.rad, c0[a2] = std::sin(t0) * s.rad;
        c1[a1] = std::cos(t1) * s.rad, c1[a2] = std::sin(t1) * s.rad;
        float lo0[3], hi0[3], lo1[3], hi1[3], cl[3], ch[3];
        float l0[3] = {c0[0], c0[1], c0[2]}, l1[3] = {c1[0], c1[1], c1[2]};
        l0[ax] = l1[ax] = -s.h[ax];
        P(l0[0], l0[1], l0[2], lo0), P(l1[0], l1[1], l1[2], lo1);
        l0[ax] = l1[ax] = s.h[ax];
        P(l0[0], l0[1], l0[2], hi0), P(l1[0], l1[1], l1[2], hi1);
        float z[3] = {};
        z[ax] = -s.h[ax];
        P(z[0], z[1], z[2], cl);
        z[ax] = s.h[ax];
        P(z[0], z[1], z[2], ch);
        AddTri(m, lo0, lo1, hi1), AddTri(m, lo0, hi1, hi0), AddTri(m, cl, lo1, lo0), AddTri(m, ch, hi0, hi1);
        n += 4;
    }
    return n;
}
inline void BuildGrid(Mesh& m) {
    m.cellStart.clear(), m.cellTris.clear();
    if (m.tris.empty()) {
        m.gw = m.gh = 0;
        return;
    }
    m.gx0 = static_cast<int>(std::floor(m.lo[0])) - 1, m.gz0 = static_cast<int>(std::floor(m.lo[2])) - 1;
    m.gw = static_cast<int>(std::floor(m.hi[0])) - m.gx0 + 2, m.gh = static_cast<int>(std::floor(m.hi[2])) - m.gz0 + 2;
    m.gw = std::clamp(m.gw, 1, 4096), m.gh = std::clamp(m.gh, 1, 4096);
    const size_t cells = static_cast<size_t>(m.gw) * m.gh;
    std::vector<uint32_t> count(cells + 1, 0);
    const auto range = [&](const Tri& t, int& x0, int& x1, int& z0, int& z1) {
        float lx = 1e30f, hx = -1e30f, lz = 1e30f, hz = -1e30f;
        for (int i = 0; i < 3; i++) {
            if (t.v[i][0] < lx) lx = t.v[i][0];
            if (t.v[i][0] > hx) hx = t.v[i][0];
            if (t.v[i][2] < lz) lz = t.v[i][2];
            if (t.v[i][2] > hz) hz = t.v[i][2];
        }
        x0 = std::clamp(static_cast<int>(std::floor(lx - 1e-3f)) - m.gx0, 0, m.gw - 1), x1 = std::clamp(static_cast<int>(std::floor(hx + 1e-3f)) - m.gx0, 0, m.gw - 1);
        z0 = std::clamp(static_cast<int>(std::floor(lz - 1e-3f)) - m.gz0, 0, m.gh - 1), z1 = std::clamp(static_cast<int>(std::floor(hz + 1e-3f)) - m.gz0, 0, m.gh - 1);
    };
    for (const Tri& t : m.tris) {
        int x0, x1, z0, z1;
        range(t, x0, x1, z0, z1);
        for (int z = z0; z <= z1; z++)
            for (int x = x0; x <= x1; x++) count[static_cast<size_t>(z) * m.gw + x + 1]++;
    }
    for (size_t c = 0; c < cells; c++) count[c + 1] += count[c];
    m.cellStart = count;
    m.cellTris.assign(count[cells], 0);
    std::vector<uint32_t> fill(count.begin(), count.end() - 1);
    for (uint32_t i = 0; i < m.tris.size(); i++) {
        int x0, x1, z0, z1;
        range(m.tris[i], x0, x1, z0, z1);
        for (int z = z0; z <= z1; z++)
            for (int x = x0; x <= x1; x++) m.cellTris[fill[static_cast<size_t>(z) * m.gw + x]++] = i;
    }
}
// A roofed quadrant's ceiling at floor + 3 m (the grid walk's roof test) passes through the story above where that story's
// floor lies lower (14/10, Full captures: a raised house's ground story stands on its foundation, story 1, with its floor at
// the foundation's top, 0.75-1.02 m; the space under it is story 0's roofed rooms, whose ceiling at their floor + 3 m cut
// story 1's rooms 2.25 m over their floor, so every lamp hung above it shaded those rooms' walls and floors). The ceiling is
// taken down to the floor of the story above where that floor is lower (still over the story's own floor).
inline void CeilingsUnderAbove(const Story& s, const Story* above, std::vector<float>& ceil, long* lowered = nullptr) {
    ceil = s.ceil;
    if (!above || above->w != s.w || above->h != s.h) return;
    for (size_t t = 0; t < ceil.size() && t < above->floorY.size(); t++) {
        if (!(ceil[t] < kNoFloor * 0.5f)) continue;
        const float up = above->floorY[t];
        const float own = t < s.floorY.size() ? s.floorY[t] : kNoFloor;
        if (!(up < kNoFloor * 0.5f) || !(up < ceil[t] - 0.05f)) continue;
        if (own < kNoFloor * 0.5f && !(up > own + 0.3f)) continue; // (not a floor over this story's own: left as it was)
        ceil[t] = up;
        if (lowered) ++*lowered;
    }
}
// A roofed room of story s open to the story above (OpenAbove::Classify, the rule room 0 takes the lamps below by: no floor
// placed over half of it, its walls going up through that story) is one space two stories tall: its roofed quadrants with no
// floor placed over them lose their ceiling at floor + 3 m (02/11, lot C49C: story 2's rooms 5, 6 and 7 under story 3 cut
// their lamps' light at the story line), here with ownWalls: also a room under a sloped roof with a wall of its own on B. Room
// ids or placed floors not read: nothing opens. A roofed quadrant under a removed floor of the story above (the game's opening
// between stories, ReadOpenings) loses its ceiling too (02/11, lot C49C: story 1's ceiling at 3.99 m closed story 2's
// 40-quadrant floor hole, so a lamp above it lit story 1 only through a small spot).
// Returns the quadrants opened.
inline int OpenUnderRemoved(Story& s, const Story& above) {
    int opened = 0;
    for (size_t t = 0; t < s.roofed.size() && t < above.removed.size(); t++)
        for (int q = 0; q < 4; q++)
            if (((s.roofed[t] & above.removed[t]) >> q) & 1u) s.roofed[t] &= static_cast<uint8_t>(~(1u << q)), opened++;
    return opened;
}
inline int OpenTops(Story& s, const Story& above, std::vector<int>* rooms = nullptr) { // (rooms: the room rule's open rooms)
    const int w = s.w, h = s.h;
    const size_t nt = static_cast<size_t>(w) * h;
    if (w <= 0 || h <= 0 || above.w != w || above.h != h || s.roofed.size() != nt) return 0;
    if (s.room.size() != 4 * nt || above.placed.size() != nt) return OpenUnderRemoved(s, above);
    std::vector<uint8_t> wz(static_cast<size_t>(h + 1) * w, 0), wx(static_cast<size_t>(w + 1) * h, 0); // the story above's walls on tile sides
    for (const Edge& e : above.edges) {
        const int x0 = static_cast<int>(std::lround(e.x0)), z0 = static_cast<int>(std::lround(e.z0));
        const int x1 = static_cast<int>(std::lround(e.x1)), z1 = static_cast<int>(std::lround(e.z1));
        const int xl = std::min(x0, x1), zl = std::min(z0, z1);
        if (z0 == z1 && std::abs(x1 - x0) == 1 && z0 >= 0 && z0 <= h && xl >= 0 && xl < w) wz[static_cast<size_t>(z0) * w + xl] = 1;
        if (x0 == x1 && std::abs(z1 - z0) == 1 && x0 >= 0 && x0 <= w && zl >= 0 && zl < h) wx[static_cast<size_t>(zl) * (w + 1) + x0] = 1;
    }
    const auto at = [&](int x, int z) { return static_cast<size_t>(z) * w + x; };
    // (a roofless room's lamps are outdoor lamps already: room 0 for the rule, as ShareRoomZeroFromBelow)
    const auto roomAt = [&](int x, int z, int q) { return ((s.roofed[at(x, z)] >> q) & 1u) ? s.room[4 * at(x, z) + q] : 0; };
    std::vector<int> open;
    OpenAbove::Classify(
        w, h, roomAt, [&](int x, int z, int q) { return PlacedQ(above, x, z, q); }, [&](int x, int Z) { return wz[static_cast<size_t>(Z) * w + x] != 0; },
        [&](int X, int z) { return wx[static_cast<size_t>(z) * (w + 1) + X] != 0; }, open, true);
    if (rooms) *rooms = open;
    int opened = 0;
    for (int z = 0; z < h && !open.empty(); z++)
        for (int x = 0; x < w; x++)
            for (int q = 0; q < 4; q++)
                if (!PlacedQ(above, x, z, q) && std::binary_search(open.begin(), open.end(), roomAt(x, z, q))) {
                    s.roofed[at(x, z)] &= static_cast<uint8_t>(~(1u << q));
                    opened++;
                }
    return opened + OpenUnderRemoved(s, above); // (after the rule: a hole's quadrants still count as their room's)
}
// ceilingsOpen (GPU lamps for every room, which light ceilings): no slab over a roofed quadrant of the story below, and no
// ceiling over a room open to the story above (OpenTops)
inline void Build(const Lot& lot, Mesh& m, bool ceilingsOpen = false) {
    m = Mesh{};
    // (each story's ceilings taken down to the floor of the story above where it is lower: see CeilingsUnderAbove)
    std::vector<Story> fixed;
    fixed.reserve(lot.stories.size());
    for (const Story& s : lot.stories) {
        const Story* above = nullptr;
        for (const Story& a : lot.stories)
            if (a.story == s.story + 1) above = &a;
        fixed.push_back(s);
        CeilingsUnderAbove(s, above, fixed.back().ceil);
        if (ceilingsOpen && above) {
            std::vector<int> rooms;
            m.openTops += OpenTops(fixed.back(), *above, &rooms);
            for (int r : rooms) m.openRooms.emplace_back(s.story, r);
        }
    }
    std::sort(m.openRooms.begin(), m.openRooms.end());
    for (const Story& s : fixed) {
        m.curStory = static_cast<int8_t>(std::clamp(s.story, -8, 8));
        m.curKind = Mesh::kWall;
        // (open tops: an upper story's wall stops at its foot; reaching under it, it shaded the ceiling of the story below past
        // the wall's line, which the game lights: the scene of 03/11, ceiling rows at a story's floor line)
        for (const Edge& e : s.edges) AddWall(m, e, ceilingsOpen && s.story > 0 ? 0.0f : kFootExt);
        m.curKind = Mesh::kRoof;
        AddRoofs(m, s);
        m.curKind = Mesh::kSlab;
        const Story* below = nullptr;
        if (ceilingsOpen)
            for (const Story& b : fixed)
                if (b.story == s.story - 1) below = &b;
        if (s.story >= 1) AddSlabs(m, s, below);
    }
    BuildGrid(m);
}

// ---- the reference ----
// Segment a -> b (t in (tMin, tMax)) against one triangle (Moller-Trumbore, both sides)
inline bool SegTri(const float o[3], const float d[3], const Tri& t, float tMin, float tMax) {
    const float e1[3] = {t.v[1][0] - t.v[0][0], t.v[1][1] - t.v[0][1], t.v[1][2] - t.v[0][2]};
    const float e2[3] = {t.v[2][0] - t.v[0][0], t.v[2][1] - t.v[0][1], t.v[2][2] - t.v[0][2]};
    const float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (std::fabs(det) < 1e-12f) return false;
    const float inv = 1.0f / det;
    const float s[3] = {o[0] - t.v[0][0], o[1] - t.v[0][1], o[2] - t.v[0][2]};
    const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
    const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
    return tt > tMin && tt < tMax;
}
// True when the segment from a to b (excluding `skip` metres at each end) meets no triangle
inline bool Clear(const Mesh& m, const float a[3], const float b[3], float skipA = 0.01f, float skipB = 0.01f, std::vector<uint32_t>* stamp = nullptr,
                  uint32_t mark = 0) {
    const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-5f) || m.gw <= 0) return true;
    const float tMin = skipA / len, tMax = 1.0f - skipB / len;
    if (!(tMax > tMin)) return true;
    // 2D DDA over the 1 m cells the segment's xz crosses
    float px = a[0] - static_cast<float>(m.gx0), pz = a[2] - static_cast<float>(m.gz0);
    int cx = static_cast<int>(std::floor(px)), cz = static_cast<int>(std::floor(pz));
    const int ex = static_cast<int>(std::floor(b[0] - static_cast<float>(m.gx0))), ez = static_cast<int>(std::floor(b[2] - static_cast<float>(m.gz0)));
    const int sx = d[0] > 0 ? 1 : -1, sz = d[2] > 0 ? 1 : -1;
    const float tdx = std::fabs(d[0]) > 1e-9f ? 1.0f / std::fabs(d[0]) : 1e30f, tdz = std::fabs(d[2]) > 1e-9f ? 1.0f / std::fabs(d[2]) : 1e30f;
    float tx = tdx < 1e29f ? (sx > 0 ? (static_cast<float>(cx + 1) - px) : (px - static_cast<float>(cx))) * tdx : 1e30f;
    float tz = tdz < 1e29f ? (sz > 0 ? (static_cast<float>(cz + 1) - pz) : (pz - static_cast<float>(cz))) * tdz : 1e30f;
    std::vector<uint32_t> local;
    std::vector<uint32_t>& seen = stamp ? *stamp : local;
    if (!stamp) {
        local.assign(m.tris.size(), 0);
        mark = 1;
    } else if (seen.size() < m.tris.size())
        seen.assign(m.tris.size(), 0);
    for (int guard = 0; guard < 100000; guard++) {
        if (cx >= 0 && cz >= 0 && cx < m.gw && cz < m.gh) {
            const size_t c = static_cast<size_t>(cz) * m.gw + cx;
            for (uint32_t k = m.cellStart[c]; k < m.cellStart[c + 1]; k++) {
                const uint32_t i = m.cellTris[k];
                if (seen[i] == mark) continue;
                seen[i] = mark;
                if (SegTri(a, d, m.tris[i], tMin, tMax)) return false;
            }
        }
        if (cx == ex && cz == ez) break;
        if (tx < tz) {
            if (tx > 1.0f) break;
            cx += sx, tx += tdx;
        } else {
            if (tz > 1.0f) break;
            cz += sz, tz += tdz;
        }
    }
    return true;
}
// The nearest triangle the segment a -> b meets (excluding `skip` metres at each end): its index and the segment parameter
// (0 at a, 1 at b); -1 when none (the self-check's diagnosis)
inline int FirstHit(const Mesh& m, const float a[3], const float b[3], float skipA, float skipB, float& tHit) {
    tHit = 2.0f;
    const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-5f)) return -1;
    const float tMin = skipA / len, tMax = 1.0f - skipB / len;
    int best = -1;
    for (uint32_t i = 0; i < m.tris.size(); i++) { // (all of them: a diagnosis, not a hot path)
        const Tri& t = m.tris[i];
        const float e1[3] = {t.v[1][0] - t.v[0][0], t.v[1][1] - t.v[0][1], t.v[1][2] - t.v[0][2]};
        const float e2[3] = {t.v[2][0] - t.v[0][0], t.v[2][1] - t.v[0][1], t.v[2][2] - t.v[0][2]};
        const float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
        const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (std::fabs(det) < 1e-12f) continue;
        const float inv = 1.0f / det;
        const float sv[3] = {a[0] - t.v[0][0], a[1] - t.v[0][1], a[2] - t.v[0][2]};
        const float u = (sv[0] * p[0] + sv[1] * p[1] + sv[2] * p[2]) * inv;
        if (u < 0.0f || u > 1.0f) continue;
        const float q[3] = {sv[1] * e1[2] - sv[2] * e1[1], sv[2] * e1[0] - sv[0] * e1[2], sv[0] * e1[1] - sv[1] * e1[0]};
        const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
        if (v < 0.0f || u + v > 1.0f) continue;
        const float tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
        if (tt > tMin && tt < tMax && tt < tHit) tHit = tt, best = static_cast<int>(i);
    }
    return best;
}
constexpr int kRefSamples = 64;
// The share of a lamp ball (centre L, radius R) the point P sees (stratified points on the disc across the ray: rings of
// equal area, golden-angle turns). The emitter is the part of the ball its centre sees: a lamp hung 5 cm under a ceiling or
// mounted on a wall shines from its own side only (a point of the disc behind a surface, seen from the centre, is not
// light). R <= 0: the centre alone.
inline float Visibility(const Mesh& m, const float L[3], float R, const float P[3], int samples = kRefSamples, std::vector<uint32_t>* stamp = nullptr,
                        uint32_t* mark = nullptr) {
    float d[3] = {P[0] - L[0], P[1] - L[1], P[2] - L[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-4f)) return 1.0f;
    for (float& v : d) v /= len;
    const auto next = [&]() -> uint32_t {
        if (!mark) return 0;
        if (++*mark == 0) {
            std::fill(stamp->begin(), stamp->end(), 0u);
            *mark = 1;
        }
        return *mark;
    };
    if (!(R > 0.0f)) return Clear(m, L, P, 0.01f, 0.01f, stamp, next()) ? 1.0f : 0.0f;
    float t[3] = {std::fabs(d[1]) < 0.9f ? 0.0f : 1.0f, std::fabs(d[1]) < 0.9f ? 1.0f : 0.0f, 0.0f};
    float u[3] = {d[1] * t[2] - d[2] * t[1], d[2] * t[0] - d[0] * t[2], d[0] * t[1] - d[1] * t[0]};
    float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (float& v : u) v /= ul;
    const float w[3] = {d[1] * u[2] - d[2] * u[1], d[2] * u[0] - d[0] * u[2], d[0] * u[1] - d[1] * u[0]};
    int clear = 0, emit = 0;
    for (int i = 0; i < samples; i++) {
        const float r = R * std::sqrt((static_cast<float>(i) + 0.5f) / static_cast<float>(samples));
        const float a = static_cast<float>(i) * 2.39996323f;
        const float ca = std::cos(a) * r, sa = std::sin(a) * r;
        const float q[3] = {L[0] + u[0] * ca + w[0] * sa, L[1] + u[1] * ca + w[1] * sa, L[2] + u[2] * ca + w[2] * sa};
        if (!Clear(m, L, q, 0.0f, 0.0f, stamp, next())) continue; // behind a surface from the lamp's centre: no light there
        emit++;
        if (Clear(m, q, P, 0.01f, 0.01f, stamp, next())) clear++;
    }
    if (!emit) return Clear(m, L, P, 0.01f, 0.01f, stamp, next()) ? 1.0f : 0.0f;
    return static_cast<float>(clear) / static_cast<float>(emit);
}

} // namespace RtLampProxy
