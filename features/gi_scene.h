#pragma once
// Real-time GI scene blob builder (work package S2-01b): the lot proxy (rt_lamp_proxy.h) as the bridge server's texel
// scene (R-04, patch 0018 util_gi.h: GiSceneHeader, GiSceneTriangle, GiScenePatch, map, GiSceneTexel), in world space as
// the lamps. Pure functions, no game reads: realtime_gi.cpp feeds it the active lot's proxy and floor colours,
// tools/gi_server_test the synthetic lot of R-02 (its offline solve is the check of what this builds).
//
// What it builds:
//   - triangles: RtLampProxy::Build (walls with their openings, ceilings, slabs; open tops as the GPU room lamps have them)
//     plus a floor under every tile that has one where the proxy holds none (the ground story, and the stories whose
//     placed floors were not read), plus the object shapes given;
//   - texels at `density` a metre on the room side of every surface: floors and ceilings of roofed quadrants, the walls'
//     faces toward a roofed quadrant. Outdoor faces, slab sides and object shapes hold none (they block and return no
//     light);
//   - patches (the per-triangle texel index of R-04): one a story for floors, one a story for ceilings (planar grids over the
//     lot), one a wall line and side over all stories (R-04 finding: the proxy's walls start under their story's floor in
//     the plane of the wall below, so a patch a story read the wrong story's texel there).
// Pixels: each texel keeps its pixel of the layer atlas (kAtlasWidth wide) across rebuilds of the same lot (keyed by its
// lot-space place and facing), so layers of lamps an edit does not reach stay valid; new texels take freed pixels first.
// The atlas grows (and every layer must be solved again) only when the texels outgrow it.
// The edit box: what differs from the previous build (texels added, removed or with another reflectance; triangles added
// or removed), as scheduler tiles (GiSched::Box) to re-bake only the lamps whose layer touches it.
#include "gi_sched.h"
#include "rt_lamp_proxy.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GiScene {

// ---- the wire form (util_gi.h of native/remix-bridge patch 0018) ----
struct Header {
    uint32_t magic, version, triangles, patches, mapEntries, texels, rays, shadowRays, bounces, reserved[3];
};
struct Tri {
    float v[3][3];
    int32_t front, back; // the patch of the side cross(v1 - v0, v2 - v0) points to, and of the other; -1 = no texel
};
struct Patch {
    float origin[3], u[3], v[3]; // cell = floor(dot(P - origin, u)), floor(dot(P - origin, v)), clamped
    uint32_t nu, nv, mapOffset;
};
struct Texel {
    float pos[3], nrm[3], alb[3];
    uint32_t pixel;
};
static_assert(sizeof(Header) == 48 && sizeof(Tri) == 44 && sizeof(Patch) == 48 && sizeof(Texel) == 40, "util_gi.h layouts");
constexpr uint32_t kMagic = 0x43534947u; // "GISC"
constexpr uint32_t kAtlasWidth = 256;    // layer atlas width; height from the texel count
constexpr float kOff = 0.02f;            // floor and ceiling texels sit this far off their plane
constexpr float kWallOff = 0.05f;        // wall texels this far off their wall

enum class Surface : uint8_t { Floor, Ceiling, Wall };

struct Options {
    int density = 2; // texels a metre
    uint32_t rays = 256, shadowRays = 16, bounces = 16;
    bool ceilingsOpen = true; // RtLampProxy::Build's open tops (rooms open to the story above have no ceiling)
};

// The reflectance of a surface point (lot space; tile ix, iz, quadrant q of story `story`). Unset: kDefault*
constexpr float kDefaultFloor[3] = {0.50f, 0.40f, 0.30f}, kDefaultCeiling[3] = {0.80f, 0.80f, 0.80f}, kDefaultWall[3] = {0.70f, 0.70f, 0.70f};
using ReflectFn = std::function<void(Surface s, int story, int ix, int iz, int q, const float lotP[3], const float lotN[3], float rgb[3])>;

struct Input {
    const RtLampProxy::Lot* lot = nullptr;
    std::vector<RtLampProxy::ObjShape> shapes; // lot space; block light, hold no texel
    ReflectFn reflect;
};

struct Slot {
    uint32_t pixel = 0;
    int story = 0;
    float world[3] = {}; // its point (world)
    float alb[3] = {};
};
struct TriWhere {
    int s0 = 0, s1 = 0;
    float lo[2] = {}, hi[2] = {}; // world x, z
};

struct Built {
    std::vector<uint8_t> blob;
    uint32_t texels = 0, height = 0;                          // atlas kAtlasWidth x height
    std::unordered_map<uint64_t, Slot> slots;                 // texel key -> its pixel and place (the allocator)
    std::vector<uint32_t> freePixels;                         // pixels below the atlas's end no texel holds
    uint32_t nextPixel = 0;                                   // first pixel never handed out
    std::unordered_map<uint64_t, std::vector<uint32_t>> tiles; // GiSched::TileKey -> the pixels of its texels
    std::unordered_map<uint64_t, TriWhere> tris;              // triangle hash -> where (the edit diff)
    uint32_t triangles = 0, patches = 0;
};

// ---- helpers ----
inline int QuadrantOf(float fx, float fz) { // (LampVis::Quadrant: 0 toward -z, 1 +x, 2 +z, 3 -x)
    return std::fabs(fx - 0.5f) <= std::fabs(fz - 0.5f) ? (fz > 0.5f ? 2 : 0) : (fx > 0.5f ? 1 : 3);
}
inline const RtLampProxy::Story* StoryOf(const RtLampProxy::Lot& lot, int story) {
    for (const auto& s : lot.stories)
        if (s.story == story) return &s;
    return nullptr;
}
inline bool Roofed(const RtLampProxy::Story* s, int ix, int iz, int q) {
    if (!s || ix < 0 || iz < 0 || ix >= s->w || iz >= s->h) return false;
    const size_t t = static_cast<size_t>(iz) * s->w + ix;
    return t < s->roofed.size() && ((s->roofed[t] >> q) & 1u);
}
// lot = world x rows + translation (a rotation and a translation): world = (lot - t) x R^T
inline void ToWorld(const RtLampProxy::Lot& l, const float p[3], float out[3]) {
    float d[3];
    for (int j = 0; j < 3; j++) d[j] = p[j] - l.worldToLot[3][j];
    for (int k = 0; k < 3; k++) out[k] = d[0] * l.worldToLot[k][0] + d[1] * l.worldToLot[k][1] + d[2] * l.worldToLot[k][2];
}
inline void DirToWorld(const RtLampProxy::Lot& l, const float d[3], float out[3]) {
    for (int k = 0; k < 3; k++) out[k] = d[0] * l.worldToLot[k][0] + d[1] * l.worldToLot[k][1] + d[2] * l.worldToLot[k][2];
}
inline int32_t Q64(float v) { return static_cast<int32_t>(std::lround(v * 64.0f)); }
// A texel's identity across rebuilds: its lot-space place (1/64 m) and facing (8 wall directions, up, down)
inline uint64_t TexelKey(const float p[3], const float n[3]) {
    int code = n[1] > 0.5f ? 8 : n[1] < -0.5f ? 9 : (static_cast<int>(std::lround(std::atan2(n[2], n[0]) / 0.78539816f)) + 8) & 7;
    return static_cast<uint64_t>(static_cast<uint16_t>(Q64(p[0]))) << 48 | static_cast<uint64_t>(static_cast<uint16_t>(Q64(p[1]))) << 32 |
           static_cast<uint64_t>(static_cast<uint16_t>(Q64(p[2]))) << 16 | static_cast<uint64_t>(code);
}
inline uint64_t TriHash(const RtLampProxy::Tri& t, uint8_t kind) {
    uint64_t h = 1469598103934665603ull ^ kind;
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) h = (h ^ static_cast<uint32_t>(Q64(t.v[i][k]))) * 1099511628211ull;
    return h;
}

// The mesh the scene is made of (lot space): the proxy, the floors it lacks, the shapes; [floorFirst, floorEnd) are the
// added floors (story in m.story). Shared with the offline check, so its own solve sees the same geometry.
inline void BuildMesh(const Input& in, const Options& o, RtLampProxy::Mesh& m, size_t& floorFirst, size_t& floorEnd) {
    using namespace RtLampProxy;
    Build(*in.lot, m, o.ceilingsOpen);
    floorFirst = m.tris.size();
    m.curKind = Mesh::kSlab;
    for (const Story& s : in.lot->stories) {
        if (s.story >= 1 && !s.placed.empty()) continue; // (placed floors read: the slabs are its floors)
        m.curStory = static_cast<int8_t>(std::clamp(s.story, -8, 8));
        for (int iz = 0; iz < s.h; iz++)
            for (int ix = 0; ix < s.w; ix++) {
                const size_t t = static_cast<size_t>(iz) * s.w + ix;
                if (t >= s.floorY.size() || !(s.floorY[t] < kNoFloor * 0.5f)) continue;
                const float y = s.floorY[t];
                const float a[3] = {float(ix), y, float(iz)}, b[3] = {float(ix + 1), y, float(iz)}, c[3] = {float(ix + 1), y, float(iz + 1)}, d[3] = {float(ix), y, float(iz + 1)};
                AddTri(m, a, c, b), AddTri(m, a, d, c); // (normal up)
            }
    }
    floorEnd = m.tris.size();
    for (const ObjShape& sh : in.shapes) AddShape(m, sh);
    BuildGrid(m);
}

// Builds the scene of `in` into `out`. prev: the last build of the same lot (pixels kept, the edit found), or null.
// edit / whole: the tiles that differ from prev, and whether every layer must be solved again (no prev, or the atlas grew).
inline void Build(const Input& in, const Options& o, const Built* prev, Built& out, GiSched::Box* edit = nullptr, bool* whole = nullptr) {
    using namespace RtLampProxy;
    const Lot& lot = *in.lot;
    const int D = std::max(1, o.density);
    const float step = 1.0f / static_cast<float>(D);
    Mesh m;
    size_t floorFirst = 0, floorEnd = 0;
    BuildMesh(in, o, m, floorFirst, floorEnd);

    struct T {
        float lot[3], n[3], alb[3];
        int story;
        uint64_t key;
    };
    std::vector<T> tx;
    std::vector<Patch> patches;
    std::vector<int32_t> map;
    std::vector<std::pair<int32_t, size_t>> cellOf; // (map entry, texel) filled once the texel order is known
    const auto reflect = [&](Surface s, int story, int ix, int iz, int q, const float p[3], const float n[3], float rgb[3]) {
        const float* d = s == Surface::Floor ? kDefaultFloor : s == Surface::Ceiling ? kDefaultCeiling : kDefaultWall;
        rgb[0] = d[0], rgb[1] = d[1], rgb[2] = d[2];
        if (in.reflect) in.reflect(s, story, ix, iz, q, p, n, rgb);
    };
    const auto addTexel = [&](size_t mapEntry, Surface s, int story, int ix, int iz, int q, const float p[3], const float n[3]) {
        if (map[mapEntry] != -2) return; // (taken: the first surface there wins)
        T t{{p[0], p[1], p[2]}, {n[0], n[1], n[2]}, {}, story, TexelKey(p, n)};
        reflect(s, story, ix, iz, q, p, n, t.alb);
        map[mapEntry] = -3; // (a texel; its index once ordered)
        cellOf.push_back({static_cast<int32_t>(mapEntry), tx.size()});
        tx.push_back(t);
    };

    // floors and ceilings: a planar grid a story and class over the story's tiles
    std::map<int, int32_t> floorPatch, ceilPatch;
    for (const Story& s : lot.stories) {
        for (int cls = 0; cls < 2; cls++) {
            Patch p{{0, 0, 0}, {static_cast<float>(D), 0, 0}, {0, 0, static_cast<float>(D)}, static_cast<uint32_t>(s.w * D), static_cast<uint32_t>(s.h * D),
                    static_cast<uint32_t>(map.size())};
            (cls ? ceilPatch : floorPatch)[s.story] = static_cast<int32_t>(patches.size());
            patches.push_back(p);
            map.resize(map.size() + static_cast<size_t>(p.nu) * p.nv, -2);
        }
    }
    // the texels of one horizontal quadrant triangle (tile ix, iz, quadrant q) at height y, facing up or down
    const auto quadTexels = [&](int story, int ix, int iz, int q, float y, bool up) {
        const auto& pm = up ? floorPatch : ceilPatch;
        const auto it = pm.find(story);
        const Story* s = StoryOf(lot, story);
        if (it == pm.end() || !s || !Roofed(s, ix, iz, q)) return; // (an outdoor floor holds no texel)
        const Patch& p = patches[static_cast<size_t>(it->second)];
        for (int cz = 0; cz < D; cz++)
            for (int cx = 0; cx < D; cx++) {
                const float fx = (cx + 0.5f) * step, fz = (cz + 0.5f) * step;
                if (QuadrantOf(fx, fz) != q) continue;
                const int gx = ix * D + cx, gz = iz * D + cz;
                if (gx < 0 || gz < 0 || gx >= static_cast<int>(p.nu) || gz >= static_cast<int>(p.nv)) continue;
                const float P[3] = {ix + fx, y + (up ? kOff : -kOff), iz + fz}, N[3] = {0, up ? 1.0f : -1.0f, 0};
                addTexel(p.mapOffset + static_cast<size_t>(gz) * p.nu + gx, up ? Surface::Floor : Surface::Ceiling, story, ix, iz, q, P, N);
            }
    };
    // horizontal triangles: their tile and quadrant from the centroid (a quadrant triangle or half a tile's floor)
    std::vector<uint8_t> horiz(m.tris.size(), 0); // 1 up, 2 down
    for (size_t i = 0; i < m.tris.size(); i++) {
        const RtLampProxy::Tri& t = m.tris[i];
        const float e1[3] = {t.v[1][0] - t.v[0][0], t.v[1][1] - t.v[0][1], t.v[1][2] - t.v[0][2]}, e2[3] = {t.v[2][0] - t.v[0][0], t.v[2][1] - t.v[0][1], t.v[2][2] - t.v[0][2]};
        const float ny = e1[2] * e2[0] - e1[0] * e2[2], len = std::sqrt(std::pow(e1[1] * e2[2] - e1[2] * e2[1], 2.0f) + ny * ny + std::pow(e1[0] * e2[1] - e1[1] * e2[0], 2.0f));
        if (len > 0 && std::fabs(ny) > 0.99f * len) horiz[i] = ny > 0 ? 1 : 2;
    }
    for (size_t i = 0; i < m.tris.size(); i++) {
        if (!horiz[i] || m.kind[i] == Mesh::kShape || m.kind[i] == Mesh::kWall) continue;
        const RtLampProxy::Tri& t = m.tris[i];
        const float y = t.v[0][1];
        const bool added = i >= floorFirst && i < floorEnd;
        const int story = m.story[i];
        if (added) { // a whole tile's half: the tile's four quadrants once (from its first half)
            if ((i - floorFirst) & 1) continue;
            const int ix = static_cast<int>(std::floor(std::min({t.v[0][0], t.v[1][0], t.v[2][0]}) + 0.5f)),
                      iz = static_cast<int>(std::floor(std::min({t.v[0][2], t.v[1][2], t.v[2][2]}) + 0.5f));
            for (int q = 0; q < 4; q++) quadTexels(story, ix, iz, q, y, true);
            continue;
        }
        const float cx = (t.v[0][0] + t.v[1][0] + t.v[2][0]) / 3, cz = (t.v[0][2] + t.v[1][2] + t.v[2][2]) / 3;
        const int ix = static_cast<int>(std::floor(cx)), iz = static_cast<int>(std::floor(cz)), q = QuadrantOf(cx - ix, cz - iz);
        if (m.kind[i] == Mesh::kRoof) {
            quadTexels(story, ix, iz, q, y, false);
            // an upper floor laid on this ceiling (its slab left out, RtLampProxy::OverCeiling): its floor texels here
            const Story* up = StoryOf(lot, story + 1);
            if (up && PlacedQ(*up, ix, iz, q) && std::fabs(FloorOf(*up, ix, iz) - y) < 0.05f) quadTexels(story + 1, ix, iz, q, y, true);
        } else if (m.kind[i] == Mesh::kSlab && horiz[i] == 1)
            quadTexels(story, ix, iz, q, y, true);
    }

    // walls: one patch a wall line and side over all stories
    struct Line {
        float d[2], n[2], c, a0 = 1e30f, a1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
        int32_t patch[2] = {-1, -1};
        std::vector<std::pair<const Edge*, int>> edges; // (edge, story)
    };
    std::map<std::pair<int, int>, Line> lines;
    const auto lineKey = [](float dx, float dz, float& ox, float& oz) {
        if (dx < -1e-6f || (std::fabs(dx) <= 1e-6f && dz < 0)) dx = -dx, dz = -dz;
        ox = dx, oz = dz;
        return static_cast<int>(std::lround(std::atan2(dz, dx) * 1000.0f));
    };
    for (const Story& s : lot.stories)
        for (const Edge& e : s.edges) {
            const float dx = e.x1 - e.x0, dz = e.z1 - e.z0, len = std::sqrt(dx * dx + dz * dz);
            if (!(len > 1e-4f)) continue;
            float d[2];
            const int ang = lineKey(dx / len, dz / len, d[0], d[1]);
            const float n[2] = {-d[1], d[0]}, c = n[0] * e.x0 + n[1] * e.z0;
            Line& L = lines[{ang, Q64(c)}];
            L.d[0] = d[0], L.d[1] = d[1], L.n[0] = n[0], L.n[1] = n[1], L.c = c;
            const float a = d[0] * e.x0 + d[1] * e.z0, b = d[0] * e.x1 + d[1] * e.z1;
            L.a0 = std::min({L.a0, a, b}), L.a1 = std::max({L.a1, a, b});
            L.y0 = std::min(L.y0, e.base), L.y1 = std::max({L.y1, e.topL, e.topM, e.topR});
            L.edges.push_back({&e, s.story});
        }
    for (auto& [k, L] : lines) {
        const uint32_t nu = static_cast<uint32_t>(std::max(1.0f, std::ceil((L.a1 - L.a0) * D))), nv = static_cast<uint32_t>(std::max(1.0f, std::ceil((L.y1 - L.y0) * D)));
        if (static_cast<uint64_t>(nu) * nv > (1u << 22)) continue; // (a nonsense line: left without texels)
        for (int side = 0; side < 2; side++) {
            const float sg = side ? 1.0f : -1.0f;
            const size_t first = tx.size();
            const uint32_t off = static_cast<uint32_t>(map.size());
            map.resize(map.size() + static_cast<size_t>(nu) * nv, -2);
            for (const auto& [ep, story] : L.edges) {
                const Edge& e = *ep;
                const Story* s = StoryOf(lot, story);
                const float dx = e.x1 - e.x0, dz = e.z1 - e.z0, len = std::sqrt(dx * dx + dz * dz);
                // the side's quadrant: a quarter metre off the edge's middle
                const float mx = 0.5f * (e.x0 + e.x1) + 0.25f * sg * L.n[0], mz = 0.5f * (e.z0 + e.z1) + 0.25f * sg * L.n[1];
                const int ix = static_cast<int>(std::floor(mx)), iz = static_cast<int>(std::floor(mz)), q = QuadrantOf(mx - ix, mz - iz);
                if (!Roofed(s, ix, iz, q)) continue; // (an outdoor face: no texel)
                const float ea = L.d[0] * e.x0 + L.d[1] * e.z0, eb = L.d[0] * e.x1 + L.d[1] * e.z1, flip = eb < ea ? -1.0f : 1.0f;
                const int c0 = static_cast<int>(std::floor((std::min(ea, eb) - L.a0) * D + 0.5f)), c1 = static_cast<int>(std::floor((std::max(ea, eb) - L.a0) * D + 0.5f));
                const int r0 = static_cast<int>(std::floor((e.base - L.y0) * D + 0.5f));
                for (int cu = std::max(c0, 0); cu < c1 && cu < static_cast<int>(nu); cu++) {
                    const float along = L.a0 + (cu + 0.5f) * step;
                    const float u = flip > 0 ? (along - ea) / len : (ea - along) / len; // (from the edge's low end)
                    const float top = EdgeTop(e, u);
                    for (int row = std::max(r0, 0); row < static_cast<int>(nv); row++) {
                        const float y = L.y0 + (row + 0.5f) * step;
                        if (!(y < top)) break;
                        const float rel = y - e.base;
                        if (e.open && u > e.o.u0 && u < e.o.u1 && rel > e.o.sill && rel < OpeningHead(e.o, u)) continue;
                        const float P[3] = {e.x0 + u * dx + kWallOff * sg * L.n[0], y, e.z0 + u * dz + kWallOff * sg * L.n[1]}, N[3] = {sg * L.n[0], 0, sg * L.n[1]};
                        addTexel(off + static_cast<size_t>(row) * nu + cu, Surface::Wall, story, ix, iz, q, P, N);
                    }
                }
            }
            if (tx.size() == first) { // no texel on this side: no patch (its hits block and return nothing)
                map.resize(off);
                continue;
            }
            L.patch[side] = static_cast<int32_t>(patches.size());
            patches.push_back({{L.a0 * L.d[0] + L.c * L.n[0], L.y0, L.a0 * L.d[1] + L.c * L.n[1]},
                               {L.d[0] * D, 0, L.d[1] * D},
                               {0, static_cast<float>(D), 0},
                               nu,
                               nv,
                               off});
        }
    }

    // triangles and their patches
    std::vector<Tri> tris(m.tris.size());
    out.tris.clear();
    for (size_t i = 0; i < m.tris.size(); i++) {
        const RtLampProxy::Tri& t = m.tris[i];
        Tri& w = tris[i];
        w.front = w.back = -1;
        const int story = m.story[i];
        const uint8_t kind = m.kind[i];
        const bool added = i >= floorFirst && i < floorEnd;
        const auto patchOf = [](const std::map<int, int32_t>& pm, int s) {
            const auto it = pm.find(s);
            return it == pm.end() ? -1 : it->second;
        };
        if (horiz[i] && (added || kind == Mesh::kRoof || kind == Mesh::kSlab)) {
            int32_t up = -1, down = -1;
            if (added) up = patchOf(floorPatch, story), down = patchOf(ceilPatch, story - 1);
            else if (kind == Mesh::kRoof) up = patchOf(floorPatch, story + 1), down = patchOf(ceilPatch, story);
            else if (horiz[i] == 1) up = patchOf(floorPatch, story); // (a slab's top; its bottom and sides hold none)
            w.front = horiz[i] == 1 ? up : down, w.back = horiz[i] == 1 ? down : up;
        } else if (kind == Mesh::kWall) {
            const float e1[3] = {t.v[1][0] - t.v[0][0], t.v[1][1] - t.v[0][1], t.v[1][2] - t.v[0][2]}, e2[3] = {t.v[2][0] - t.v[0][0], t.v[2][1] - t.v[0][1], t.v[2][2] - t.v[0][2]};
            const float nx = e1[1] * e2[2] - e1[2] * e2[1], nz = e1[0] * e2[1] - e1[1] * e2[0], nl = std::sqrt(nx * nx + nz * nz);
            if (nl > 0) {
                float d[2];
                const int ang = lineKey(nz / nl, -nx / nl, d[0], d[1]);
                const float n[2] = {-d[1], d[0]};
                const float cx = (t.v[0][0] + t.v[1][0] + t.v[2][0]) / 3, cz = (t.v[0][2] + t.v[1][2] + t.v[2][2]) / 3;
                const auto it = lines.find({ang, Q64(n[0] * cx + n[1] * cz)});
                if (it != lines.end()) {
                    const bool plus = nx * n[0] + nz * n[1] > 0; // the winding's normal toward +n: side 1
                    w.front = it->second.patch[plus ? 1 : 0], w.back = it->second.patch[plus ? 0 : 1];
                }
            }
        }
        for (int v = 0; v < 3; v++) ToWorld(lot, t.v[v], w.v[v]);
        // the edit diff: where it is (world x, z) and its stories
        TriWhere where;
        where.s0 = horiz[i] && kind != Mesh::kRoof ? story - 1 : story, where.s1 = kind == Mesh::kRoof ? story + 1 : story;
        where.lo[0] = std::min({w.v[0][0], w.v[1][0], w.v[2][0]}), where.hi[0] = std::max({w.v[0][0], w.v[1][0], w.v[2][0]});
        where.lo[1] = std::min({w.v[0][2], w.v[1][2], w.v[2][2]}), where.hi[1] = std::max({w.v[0][2], w.v[1][2], w.v[2][2]});
        out.tris[TriHash(t, kind)] = where;
    }
    for (Patch& p : patches) {
        float wo[3], wu[3], wv[3];
        ToWorld(lot, p.origin, wo), DirToWorld(lot, p.u, wu), DirToWorld(lot, p.v, wv);
        std::memcpy(p.origin, wo, 12), std::memcpy(p.u, wu, 12), std::memcpy(p.v, wv, 12);
    }

    // pixels: kept by key from prev; the new ones in tile order take freed pixels first, then the end
    out.slots.clear(), out.tiles.clear(), out.freePixels.clear();
    std::vector<size_t> order(tx.size());
    std::vector<float> world(tx.size() * 3);
    std::vector<uint64_t> tileOf(tx.size());
    for (size_t i = 0; i < tx.size(); i++) {
        ToWorld(lot, tx[i].lot, &world[i * 3]);
        tileOf[i] = GiSched::TileKey(tx[i].story, static_cast<int>(std::floor(world[i * 3])), static_cast<int>(std::floor(world[i * 3 + 2])));
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return tileOf[a] < tileOf[b]; });
    std::vector<uint32_t> pixel(tx.size(), UINT32_MAX);
    std::vector<uint32_t> freed;
    uint32_t next = 0;
    if (prev) {
        next = prev->nextPixel;
        freed = prev->freePixels;
        std::unordered_set<uint64_t> here;
        for (const T& t : tx) here.insert(t.key);
        for (const auto& [k, s] : prev->slots)
            if (!here.count(k)) freed.push_back(s.pixel);
        std::unordered_set<uint32_t> used;
        for (size_t i = 0; i < tx.size(); i++)
            if (const auto it = prev->slots.find(tx[i].key); it != prev->slots.end() && used.insert(it->second.pixel).second) pixel[i] = it->second.pixel;
        std::sort(freed.begin(), freed.end(), std::greater<uint32_t>()); // (popped from the back: lowest first)
    }
    for (size_t i : order)
        if (pixel[i] == UINT32_MAX) {
            if (!freed.empty()) pixel[i] = freed.back(), freed.pop_back();
            else pixel[i] = next++;
        }
    out.freePixels = freed;
    out.nextPixel = next;
    const uint32_t need = (next + kAtlasWidth - 1) / kAtlasWidth;
    const bool grew = !prev || need > prev->height;
    out.height = grew ? std::max(16u, (static_cast<uint32_t>(std::ceil(need * 1.25)) + 15) / 16 * 16) : prev->height;
    out.texels = static_cast<uint32_t>(tx.size());

    // the blob lists texels by falling pixel (a first build: texel i at pixel texels - 1 - i, as R-04's test reads them)
    std::vector<size_t> byPixel(tx.size());
    for (size_t i = 0; i < tx.size(); i++) byPixel[i] = i;
    std::sort(byPixel.begin(), byPixel.end(), [&](size_t a, size_t b) { return pixel[a] > pixel[b]; });
    std::vector<int32_t> indexOf(tx.size());
    std::vector<Texel> wire(tx.size());
    for (size_t j = 0; j < byPixel.size(); j++) {
        const size_t i = byPixel[j];
        indexOf[i] = static_cast<int32_t>(j);
        Texel& w = wire[j];
        std::memcpy(w.pos, &world[i * 3], 12);
        DirToWorld(lot, tx[i].n, w.nrm);
        std::memcpy(w.alb, tx[i].alb, 12);
        w.pixel = pixel[i];
        Slot s{pixel[i], tx[i].story, {world[i * 3], world[i * 3 + 1], world[i * 3 + 2]}, {tx[i].alb[0], tx[i].alb[1], tx[i].alb[2]}};
        out.slots[tx[i].key] = s;
        out.tiles[tileOf[i]].push_back(pixel[i]);
    }
    for (int32_t& v : map)
        if (v < 0) v = -1;
    for (const auto& [entry, i] : cellOf) map[static_cast<size_t>(entry)] = indexOf[i];

    const Header h{kMagic, 1, static_cast<uint32_t>(tris.size()), static_cast<uint32_t>(patches.size()), static_cast<uint32_t>(map.size()),
                   static_cast<uint32_t>(wire.size()), o.rays, o.shadowRays, o.bounces, {}};
    const auto put = [&](const void* p, size_t n) { out.blob.insert(out.blob.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
    out.blob.clear();
    put(&h, sizeof h), put(tris.data(), tris.size() * sizeof(Tri)), put(patches.data(), patches.size() * sizeof(Patch)), put(map.data(), map.size() * 4),
        put(wire.data(), wire.size() * sizeof(Texel));
    out.triangles = h.triangles, out.patches = h.patches;

    // the edit against prev
    if (whole) *whole = grew;
    if (!edit) return;
    *edit = {};
    if (!prev) return;
    const auto tileBox = [](const float w[3], int story) { return GiSched::Box{static_cast<int>(std::floor(w[0])), static_cast<int>(std::floor(w[2])), static_cast<int>(std::floor(w[0])),
                                                                             static_cast<int>(std::floor(w[2])), story, story}; };
    for (const auto& [k, s] : out.slots) {
        const auto it = prev->slots.find(k);
        if (it == prev->slots.end() || std::memcmp(it->second.alb, s.alb, sizeof s.alb)) *edit = edit->Union(tileBox(s.world, s.story));
    }
    for (const auto& [k, s] : prev->slots)
        if (!out.slots.count(k)) *edit = edit->Union(tileBox(s.world, s.story));
    const auto triBox = [](const TriWhere& w) {
        return GiSched::Box{static_cast<int>(std::floor(w.lo[0])), static_cast<int>(std::floor(w.lo[1])), static_cast<int>(std::floor(w.hi[0])),
                            static_cast<int>(std::floor(w.hi[1])), w.s0, w.s1};
    };
    for (const auto& [k, w] : out.tris)
        if (!prev->tris.count(k)) *edit = edit->Union(triBox(w));
    for (const auto& [k, w] : prev->tris)
        if (!out.tris.count(k)) *edit = edit->Union(triBox(w));
}

} // namespace GiScene
