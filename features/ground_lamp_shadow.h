#pragma once
// Walls shadow lamp light on lot ground (lot_light_bridge.cpp, the lot light pass). The terrain stamp (the world atlas) is
// baked with no occluder, and the lot pass takes max(lot map, atlas), so a free-standing wall next to a garden post left the
// grass behind it fully lit. Per lot, on the room mask's grid (4 texels a metre, lot-local), the share of the nearby
// outdoor lamps' stamp that reaches each texel past the outside walls:
//   M = 1 - sum_i w_i (1 - v_i) / max(sum_i w_i, kOtherLight)  (= sum w v / sum w where the lamps light the ground),
//   w_i = sat(0.4 x lum(colour x intensity x range) x cos / d^2) (the stamp's law, engine/terrain-and-light-bake.md),
//   v_i = 0 when an outside wall stands between the lamp's head and the texel. 1 where no lamp reaches.
// (2.15.3) The sum runs over every bake lamp whose stamp covers the texel: the lamps tested against the walls and, with
// v = 1, the others (ShadeJob::Init's `others`: past kMaxLamps or not over the lot), so a wall's shadow takes away only the
// light of the lamps it blocks (F8: a fence's shadow on snow was darker than the snow no lamp of the lot reaches, as it
// took the light of the lamps not listed too). A lamp weighs only inside its stamp's rectangle (the bake writes nothing
// past it): its shadow ends where its light on the ground ends. POD and templates only, no game reads: built from copies (the walls copied from the room solves,
// the bake lamp snapshot), so a read-only offline harness can test it, and it is built a slice at a time on the render
// thread (ShadeJob::Step).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>
#include "wall_openings.h"
#include "foliage_canopy.h"
#include "terrain_bake_rule.h"

namespace GroundShadow {

struct Wall {
    float x0, z0, x1, z1; // the face's line, world xz
    float y0, y1;         // its base and top, world y
    uint32_t p0 = 0;      // its patches (lower tops, openings; wall_openings.h): [p0, p0 + pn) of the job's patch list
    uint32_t pn = 0;
    // its ends: free (no wall of the list goes on in line from it, MarkFreeEnds), so a soft shadow spreads past it; a joint
    // in line keeps the shadow whole across it
    bool free0 = false, free1 = false;
    // the share of the light its body lets through: 0 a wall; a mesh fence the open share of its cutout texture
    float pass = 0.0f;
};
struct Lamp {
    float x, y, z; // the head, world
    float k;       // 0.4 x lum(colour x intensity x range): the stamp's weight numerator
    bool liftable = false; // a lot lamp the bake stamps from kLowLampLift when it lies on the ground (TerrainBakeRule::Liftable)
    float rx0 = -1e30f, rz0 = -1e30f, rx1 = 1e30f, rz1 = 1e30f; // its stamp's rectangle, world xz (none: unbounded)
};
// Lot-local metres -> world: wx = m0 lx + m8 lz + tx, wz = m2 lx + m10 lz + tz (room 0's +0xF8 matrix); ground at ty
struct Grid {
    int w = 0, h = 0; // texels (4 a metre)
    float m0 = 1, m2 = 0, m8 = 0, m10 = 1, tx = 0, tz = 0, ty = 0;
    // soft edges (14/10: the shade's edges on grass drew in steps of the mask's texels): every wall's free end, top and opening
    // takes the lamp's penumbra (penumbra.h, Shadow softness), at least kSoftAA wide at the ground, so a texel holds the share
    // of the lamp it sees instead of all or nothing. false: the hard test
    bool soft = false;
};

constexpr float kTexelsPerMetre = 4.0f;
constexpr float kLampSkip = 0.15f;   // m: a wall crossed this close to the lamp's head is the wall a sconce hangs on (its own face)
constexpr float kMinWeight = 1.0f / 256.0f;
constexpr float kMaxReach = 40.0f;   // m
constexpr float kMinHead = 0.1f;     // m: the head's height over the ground used for cos (a lamp at ground level)
constexpr int kMaxLamps = 32;        // strongest lamps per lot, by weight at the lot's nearest point
constexpr float kOtherLight = 0.05f; // the stamp weight under which a blocked lamp takes only its own share of the light
constexpr float kSoftAA = 0.75f / kTexelsPerMetre; // m: the half width an edge spans at least on the mask (1.5 texels)
constexpr float kJoinEps = 0.02f;    // m: two walls' ends this close meet
constexpr float kJoinCos = 0.996f;   // and they join (one run) when in line within about 5 degrees

// (2.15.3) == static_cast<uint8_t>(std::lround(v)) for v in [0, 255], without the CRT call: v + 0.5 is exact in double and
// the cast truncates it, as lround rounds a half away from zero (checked for every float of the range, tools/ground_shade_test)
inline uint8_t ToByte(float v) { return static_cast<uint8_t>(static_cast<int>(static_cast<double>(v) + 0.5)); }

inline void TexelWorld(const Grid& g, int i, int j, float& wx, float& wz) {
    const float lx = (static_cast<float>(i) + 0.5f) / kTexelsPerMetre, lz = (static_cast<float>(j) + 0.5f) / kTexelsPerMetre;
    wx = g.m0 * lx + g.m8 * lz + g.tx;
    wz = g.m2 * lx + g.m10 * lz + g.tz;
}

// World xz -> continuous texel coordinates (texel centres at i + 0.5); false when the matrix is singular
inline bool WorldTexel(const Grid& g, float wx, float wz, float& fi, float& fj) {
    const float det = g.m0 * g.m10 - g.m8 * g.m2;
    if (!(std::fabs(det) > 1e-6f)) return false;
    const float dx = wx - g.tx, dz = wz - g.tz;
    const float lx = (g.m10 * dx - g.m8 * dz) / det, lz = (-g.m2 * dx + g.m0 * dz) / det;
    fi = lx * kTexelsPerMetre;
    fj = lz * kTexelsPerMetre;
    return std::isfinite(fi) && std::isfinite(fj);
}

// Outdoor floors of story 0 take the same shade (13/10, F7 2026-10-13 08-16-00: a paved path behind a free-standing wall was
// lit by the garden post on the other side while the grass beside it was shaded): their lamp term max(floor map, atlas) has
// no wall test either (the floor map is room 0's solve, which tests no wall for floor texels). The floor shaders
// (ShaderPatches FloorPatch::shadeConst) multiply it by 1 + gate (M - 1), M = the mask's alpha at
//   uv = x rows[0..1] + z rows[4..5] + rows[2..3], (x, z) = the world xz the shader reads x xzScale (2: it reads world xz / 2),
//   gate = sat(|y + rows[6]| rows[9] + rows[8]), y = world y, rows[6] = -ground, rows[7] = 1:
// 1 up to kFloorGateFull from the lot's ground, 0 from kFloorGateNone (the stories above keep their own wall test, the floor
// map's alpha). on = false: gate 0 everywhere (the floor as before). False (the floor as before) for a degenerate grid.
constexpr float kFloorGateFull = 1.0f;  // m
// Fences: a face reads the mask this far in front of it, along its normal (rows[10]; ShaderPatches PatchInstancedLamps)
constexpr float kFenceShadeReach = 0.6f; // m
constexpr float kFloorGateNone = 1.25f; // m
inline float GroundPoolHeightGate(float y, float groundY) {
    if (!std::isfinite(y) || !std::isfinite(groundY)) return 1.0f;
    return std::clamp((kFloorGateNone - std::fabs(y - groundY)) / (kFloorGateNone - kFloorGateFull), 0.0f, 1.0f);
}
// Elevated structure pieces share architectural objects' direct-lamp gain.
// Ground-level runs retain their fence gain; the existing height band blends both.
inline float StructureLampGain(float groundPool, float wallGain) {
    if (!std::isfinite(wallGain) || wallGain < 0.0f) return 1.0f;
    const float pool = std::clamp(groundPool, 0.0f, 1.0f);
    return wallGain + pool * (1.0f - wallGain);
}
inline bool FloorShadeRows(const Grid& g, float xzScale, bool on, float rows[12]) {
    for (int i = 0; i < 12; i++) rows[i] = 0.0f;
    rows[2] = rows[3] = 0.5f;
    rows[7] = 1.0f;
    const double det = static_cast<double>(g.m0) * g.m10 - static_cast<double>(g.m8) * g.m2;
    if (!(std::fabs(det) > 1e-6) || g.w <= 0 || g.h <= 0 || !std::isfinite(det)) return false;
    // lot-local metres -> texels (WorldTexel) -> uv: texel / size
    const double su = kTexelsPerMetre / (det * g.w), sv = kTexelsPerMetre / (det * g.h), s = xzScale;
    rows[0] = static_cast<float>(g.m10 * s * su);
    rows[4] = static_cast<float>(-g.m8 * s * su);
    rows[2] = static_cast<float>((-static_cast<double>(g.m10) * g.tx + static_cast<double>(g.m8) * g.tz) * su);
    rows[1] = static_cast<float>(-g.m2 * s * sv);
    rows[5] = static_cast<float>(g.m0 * s * sv);
    rows[3] = static_cast<float>((static_cast<double>(g.m2) * g.tx - static_cast<double>(g.m0) * g.tz) * sv);
    rows[6] = -g.ty;
    if (on) {
        rows[8] = kFloorGateNone / (kFloorGateNone - kFloorGateFull);
        rows[9] = -1.0f / (kFloorGateNone - kFloorGateFull);
    }
    return true;
}

// The stamp's weight of a lamp at a ground point (x, groundY, z)
inline float Weight(const Lamp& L, float x, float groundY, float z) {
    if (x < L.rx0 || x > L.rx1 || z < L.rz0 || z > L.rz1) return 0.0f; // past its stamp
    const float h = std::max(TerrainBakeRule::StampHeight(L.y - groundY, L.liftable), kMinHead); // a lamp on the lawn: as the bake stamps it
    const float dx = L.x - x, dz = L.z - z;
    const float d2 = dx * dx + dz * dz + h * h;
    const float cosv = h / ApexMath::Sqrt(d2); // (2.15.3: the same bits as std::sqrt, fast_math.h, without the CRT call)
    return std::clamp(L.k * cosv / d2, 0.0f, 1.0f);
}

// How far (horizontal metres) the lamp's stamp weighs at least kMinWeight: k h / d^3 >= kMinWeight
inline float Reach(const Lamp& L, float groundY) {
    const float h = std::max(TerrainBakeRule::StampHeight(L.y - groundY, L.liftable), kMinHead);
    if (!(L.k > 0.0f) || !std::isfinite(L.k)) return 0.0f;
    // no further than its stamp's farthest corner
    const float fx = std::max(std::fabs(L.rx0 - L.x), std::fabs(L.rx1 - L.x)), fz = std::max(std::fabs(L.rz0 - L.z), std::fabs(L.rz1 - L.z));
    return std::min({std::cbrt(L.k * h / kMinWeight), kMaxReach, std::sqrt(fx * fx + fz * fz)});
}

// The share of the head's light reaching the ground point (x, groundY, z) past an outside wall: 0 where the wall stands
// between them, through its openings and over its lower cells as WallOpenings::CrossPass has them (patches: the job's
// list; none = a whole wall). The ground at the wall's foot: a wall standing within 1 m of the lot's ground level is tested
// with the point on its own base (terrain under a lot is not perfectly flat), others (raised foundations, upper stories)
// with the point at groundY. Crossings within kLampSkip of the head are the lamp's own wall face and do not block (the face
// behind it, past the wall's thickness, still does).
// soft (Grid::soft): the lamp is a ball of Shadow softness (penumbra.h). Where the ray crosses the wall's line its spot spreads
// along the wall over the wall's run (past a free end it leaves the wall: light comes round it; across a joint or a corner the
// run goes on) and up it over the wall's top, each at least kSoftAA wide at the ground point; the base stays hard (no light
// under a wall). t, u: the crossing along the ray and along the wall.
inline float SoftPass(const Lamp& L, float groundY, const Wall& w, const WallOpenings::Patch* patches, float t, float u, float rx, float rz, float sx, float sz) {
    const float m = ApexMath::Sqrt(sx * sx + sz * sz);
    if (!(m > 1e-4f)) return 1.0f;
    const float py = ApexMath::Fabs(w.y0 - groundY) < 1.0f ? std::max(groundY, w.y0) : groundY;
    const float y = L.y + t * (py - L.y);
    if (!(y > w.y0 - 0.05f)) return 1.0f; // under its base: hard, as before
    const float c = u * m;
    // (2.15.3) past a joined end, or past a free end by the widest penumbra (the rim's half width is at most kMaxHalf): lit,
    // as the tests below find, before the rim is worked out (most of a wall's shadow box lies beside its wedge)
    if ((!w.free0 && c < 0.0f) || (!w.free1 && c > m) || (w.free0 && c <= -Penumbra::kMaxHalf) || (w.free1 && c >= m + Penumbra::kMaxHalf)) return 1.0f;
    const Penumbra::Rim rim = Penumbra::SlantOf(rx, py - L.y, rz, sz, -sx).At(Penumbra::Half(Penumbra::Radius(), t, kSoftAA));
    const float h = rim.along;
    // (across a joint the run goes on: by the spot's width, which the wall beyond covers in full)
    const float a = w.free0 ? 0.0f : -2.0f * h, b = w.free1 ? m : m + 2.0f * h;
    if (c <= a - h || c >= b + h) return 1.0f; // past a free end's penumbra
    // past a joined end the wall beyond decides (at a convex corner it turns away: this run must not shade there)
    if ((!w.free0 && c < 0.0f) || (!w.free1 && c > m)) return 1.0f;
    const float along = Penumbra::Weight(c, h, a, b); // the spot's share in front of the wall's run
    if (!(along > 0.0f)) return 1.0f;
    float inside;
    if (w.pn && patches && u >= 0.0f && u <= 1.0f) // its patches decide within the run (their tops, openings: wall_openings.h)
        inside = WallOpenings::CrossPass(patches + w.p0, static_cast<int>(w.pn), m, std::min(w.y0, y) - 1.0f, w.y1, u, y, nullptr, rim,
                                         WallOpenings::AcrossOf(rx, py - L.y, rz, sx, sz));
    else
        inside = 1.0f - Penumbra::Cover((w.y1 - 0.02f) - y, rim.up); // the wall's top, softened
    return std::clamp(1.0f - along * (1.0f - inside) * (1.0f - w.pass), 0.0f, 1.0f);
}
inline float Pass(const Lamp& L, float x, float groundY, float z, const Wall& w, const WallOpenings::Patch* patches = nullptr, bool soft = false) {
    const float ax = L.x, az = L.z;
    const float rx = x - ax, rz = z - az;
    const float sx = w.x1 - w.x0, sz = w.z1 - w.z0;
    const float den = rx * sz - rz * sx;
    if (ApexMath::Fabs(den) < 1e-9f) return 1.0f; // parallel
    const float qx = w.x0 - ax, qz = w.z0 - az;
    const float t = (qx * sz - qz * sx) / den; // along head -> point
    const float u = (qx * rz - qz * rx) / den; // along the wall
    if (!(t > 0.0f && t < 1.0f)) return 1.0f;
    const float len = ApexMath::Sqrt(rx * rx + rz * rz);
    if (t * len < kLampSkip) return 1.0f;
    if (soft) return SoftPass(L, groundY, w, patches, t, u, rx, rz, sx, sz);
    if (!(u >= 0.0f && u <= 1.0f)) return 1.0f;
    const float py = std::fabs(w.y0 - groundY) < 1.0f ? std::max(groundY, w.y0) : groundY;
    const float y = L.y + t * (py - L.y); // the ray's height where it crosses the wall line
    if (!(y > w.y0 - 0.05f && y < w.y1 - 0.02f)) return 1.0f;
    if (!w.pn || !patches) return w.pass;
    // within the wall's band: its patches decide (the base margin above stays the wall's: a ray to the wall's own foot), with
    // the lamp's penumbra at the crossing (penumbra.h)
    const float p = WallOpenings::CrossPass(patches + w.p0, static_cast<int>(w.pn), ApexMath::Sqrt(sx * sx + sz * sz), std::min(w.y0, y) - 1.0f, w.y1, u, y, nullptr,
                                            WallOpenings::RimAt(t, rx, py - L.y, rz, sx, sz, Penumbra::Radius()), WallOpenings::AcrossOf(rx, py - L.y, rz, sx, sz));
    return p + (1.0f - p) * w.pass;
}
inline bool Blocks(const Lamp& L, float x, float groundY, float z, const Wall& w, const WallOpenings::Patch* patches = nullptr) {
    return Pass(L, x, groundY, z, w, patches) <= 0.0f;
}

// Each wall's ends that no other wall of the list meets (within kJoinEps): free. Sorted by x, so O(n log n).
inline void MarkFreeEnds(std::vector<Wall>& walls) {
    struct End {
        float x, z;
        uint32_t wall;
        uint8_t which;
    };
    std::vector<End> ends;
    ends.reserve(walls.size() * 2);
    for (uint32_t i = 0; i < walls.size(); i++) {
        walls[i].free0 = walls[i].free1 = true;
        ends.push_back({walls[i].x0, walls[i].z0, i, 0});
        ends.push_back({walls[i].x1, walls[i].z1, i, 1});
    }
    std::sort(ends.begin(), ends.end(), [](const End& a, const End& b) { return a.x < b.x; });
    for (size_t i = 0; i < ends.size(); i++)
        for (size_t j = i + 1; j < ends.size() && ends[j].x - ends[i].x <= kJoinEps; j++) {
            if (ends[j].wall == ends[i].wall || std::fabs(ends[j].z - ends[i].z) > kJoinEps) continue;
            const Wall &wi = walls[ends[i].wall], &wj = walls[ends[j].wall];
            if (!(std::min(wi.y1, wj.y1) > std::max(wi.y0, wj.y0))) continue; // (walls of other heights do not meet: a story above, a fence below)
            // only a wall going on in line joins: at an angled corner each run ends at its end (no shade carried past it)
            const float ax = wi.x1 - wi.x0, az = wi.z1 - wi.z0, bx = wj.x1 - wj.x0, bz = wj.z1 - wj.z0;
            const float la = std::sqrt(ax * ax + az * az), lb = std::sqrt(bx * bx + bz * bz);
            if (!(la > 1e-4f && lb > 1e-4f) || std::fabs(ax * bx + az * bz) < kJoinCos * la * lb) continue;
            (ends[i].which ? walls[ends[i].wall].free1 : walls[ends[i].wall].free0) = false;
            (ends[j].which ? walls[ends[j].wall].free1 : walls[ends[j].wall].free0) = false;
        }
}

// The incremental build of one lot's shade mask. Init copies the inputs; Step runs until more() is false or the build is
// done (true). Result(): w x h bytes, 255 = every nearby lamp reaches the texel.
class ShadeJob {
public:
    void Init(const Grid& g, std::vector<Lamp> lamps, std::vector<Wall> walls, std::vector<WallOpenings::Patch> patches = {},
              std::vector<Canopy::Ellipsoid> canopies = {}, std::vector<Lamp> others = {}) {
        grid_ = g;
        lamps_ = std::move(lamps);
        others_ = std::move(others);
        walls_ = std::move(walls);
        patches_ = std::move(patches);
        canopies_ = std::move(canopies);
        canopy_ = 0;
        canopyTests_ = 0;
        for (Wall& w : walls_) // (a wall whose patches are not all in the list is whole)
            if (static_cast<uint64_t>(w.p0) + w.pn > patches_.size()) w.pn = 0;
        if (g.soft) MarkFreeEnds(walls_);
        const size_t n = static_cast<size_t>(std::max(g.w, 0)) * static_cast<size_t>(std::max(g.h, 0));
        num_.assign(n, 0.0f);
        den_.assign(n, 0.0f);
        vis_.assign(n, 1.0f);
        wgt_.assign(n, 0.0f);
        out_.clear();
        lamp_ = 0;
        wall_ = 0;
        phase_ = 0;
        tests_ = 0;
        through_ = 0;
        done_ = n == 0;
        if (done_) out_.clear();
    }
    template <class More> bool Step(More more) {
        while (!done_) {
            if (!more()) return false;
            if (lamp_ >= lamps_.size()) {
                for (const Lamp& L : others_) { // lit everywhere they reach: in the total only
                    reach_ = Reach(L, grid_.ty);
                    Weights(L);
                    Accumulate(false);
                }
                Finish();
                done_ = true;
                break;
            }
            const Lamp& L = lamps_[lamp_];
            if (phase_ == 0) { // the lamp's walls: the texels behind each one, within its reach
                if (wall_ == 0) {
                    reach_ = Reach(L, grid_.ty);
                    Weights(L);
                    for (int j = box_[2]; j <= box_[3]; j++) // (2.15.3: only where it is read, box_)
                        std::fill_n(vis_.begin() + static_cast<ptrdiff_t>(j) * grid_.w + box_[0], box_[1] - box_[0] + 1, 1.0f);
                }
                if (wall_ < walls_.size()) {
                    ShadowOf(L, walls_[wall_]);
                    wall_++;
                    continue;
                }
                phase_ = 2;
                continue;
            }
            if (phase_ == 2) { // the crowns in the lamp's reach: porous occluders in series with the walls (foliage_canopy.h)
                if (canopy_ < canopies_.size()) {
                    CanopyOf(L, canopies_[canopy_]);
                    canopy_++;
                    continue;
                }
                canopy_ = 0;
                phase_ = 1;
                continue;
            }
            Accumulate();
            lamp_++;
            wall_ = 0;
            phase_ = 0;
        }
        return true;
    }
    bool Done() const { return done_; }
    const std::vector<uint8_t>& Result() const { return out_; }
    // the lamps' total stamp weight per texel (sum w_i, after the build): how much bake light the shade is a share of
    const std::vector<float>& StampWeight() const { return den_; }
    long long Tests() const { return tests_; }
    long long Through() const { return through_; } // texel tests that crossed a wall with patches and kept some light
    long long CanopyTests() const { return canopyTests_; } // texel tests against a crown
    const Grid& GetGrid() const { return grid_; }

private:
    // The share of the lamp's light that passes the crown, per texel of its shadow box (vis_ multiplies: walls and crowns
    // are transmissions in series on the same ray, in any order)
    void CanopyOf(const Lamp& L, const Canopy::Ellipsoid& e) {
        if (!(reach_ > 0.0f)) return;
        float x0, z0, x1, z1;
        if (!Canopy::ShadowBox(e, L.x, L.y, L.z, grid_.ty, reach_, x0, z0, x1, z1)) return;
        float i0 = 1e30f, i1 = -1e30f, j0 = 1e30f, j1 = -1e30f; // the four corners: the lot grid can be turned
        for (const float px : {x0, x1})
            for (const float pz : {z0, z1}) {
                float fi, fj;
                if (!WorldTexel(grid_, px, pz, fi, fj)) return;
                i0 = std::min(i0, fi), i1 = std::max(i1, fi), j0 = std::min(j0, fj), j1 = std::max(j1, fj);
            }
        int a0 = std::max(0, static_cast<int>(std::floor(i0))), a1 = std::min(grid_.w - 1, static_cast<int>(std::ceil(i1)));
        int b0 = std::max(0, static_cast<int>(std::floor(j0))), b1 = std::min(grid_.h - 1, static_cast<int>(std::ceil(j1)));
        if (!Clip(a0, a1, b0, b1)) return;
        const float r2 = reach_ * reach_;
        for (int j = b0; j <= b1; j++)
            for (int i = a0; i <= a1; i++) {
                const size_t at = static_cast<size_t>(j) * grid_.w + i;
                if (!Weighs(at) || !(vis_[at] > 0.0f)) continue; // already behind a wall, or out of the lamp's sum
                float x, z;
                TexelWorld(grid_, i, j, x, z);
                const float dx = x - L.x, dz = z - L.z;
                if (dx * dx + dz * dz >= r2) continue;
                canopyTests_++;
                vis_[at] *= Canopy::Transmission(e, L.x, L.y, L.z, x, grid_.ty, z);
            }
    }
    void ShadowOf(const Lamp& L, const Wall& w) {
        if (!(reach_ > 0.0f) || !(w.y0 < L.y) || !(w.y1 > grid_.ty - 2.0f)) return; // a wall over the head or under the ground
        // the wall's distance from the head (xz): past the reach it shades nothing that weighs
        const float sx = w.x1 - w.x0, sz = w.z1 - w.z0, s2 = sx * sx + sz * sz;
        float u = s2 > 1e-9f ? ((L.x - w.x0) * sx + (L.z - w.z0) * sz) / s2 : 0.0f;
        u = std::clamp(u, 0.0f, 1.0f);
        const float cx = w.x0 + u * sx - L.x, cz = w.z0 + u * sz - L.z;
        if (cx * cx + cz * cz >= reach_ * reach_) return;
        // the shadow lies in the wedge from the head through both ends, out to the reach: its box (ends, the ends pushed
        // to the reach, and the reach circle's extreme points inside the wedge)
        float px[8], pz[8];
        int n = 0;
        float ends[2][2] = {{w.x0, w.z0}, {w.x1, w.z1}};
        if (grid_.soft && s2 > 1e-9f) { // a free end's penumbra reaches past it (at most kMaxHalf along the wall)
            const float sl = std::sqrt(s2), ex = sx / sl * Penumbra::kMaxHalf, ez = sz / sl * Penumbra::kMaxHalf;
            if (w.free0) ends[0][0] -= ex, ends[0][1] -= ez;
            if (w.free1) ends[1][0] += ex, ends[1][1] += ez;
        }
        float dir[2][2];
        for (int e = 0; e < 2; e++) {
            const float dx = ends[e][0] - L.x, dz = ends[e][1] - L.z, d = std::sqrt(dx * dx + dz * dz);
            px[n] = ends[e][0];
            pz[n++] = ends[e][1];
            if (d < 1e-4f) return; // the head on the wall's end: no shadow to speak of
            dir[e][0] = dx / d;
            dir[e][1] = dz / d;
            px[n] = L.x + dir[e][0] * reach_;
            pz[n++] = L.z + dir[e][1] * reach_;
        }
        const float c01 = dir[0][0] * dir[1][1] - dir[0][1] * dir[1][0];
        const float axes[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& a : axes) { // inside the (under 180 degree) wedge between dir0 and dir1
            const float c0a = dir[0][0] * a[1] - dir[0][1] * a[0], ca1 = a[0] * dir[1][1] - a[1] * dir[1][0];
            const bool inside = c01 >= 0.0f ? (c0a >= 0.0f && ca1 >= 0.0f) : (c0a <= 0.0f && ca1 <= 0.0f);
            const bool forward = a[0] * (dir[0][0] + dir[1][0]) + a[1] * (dir[0][1] + dir[1][1]) > 0.0f;
            if (inside && forward) {
                px[n] = L.x + a[0] * reach_;
                pz[n++] = L.z + a[1] * reach_;
            }
        }
        float i0 = 1e30f, i1 = -1e30f, j0 = 1e30f, j1 = -1e30f;
        for (int k = 0; k < n; k++) {
            float fi, fj;
            if (!WorldTexel(grid_, px[k], pz[k], fi, fj)) return;
            i0 = std::min(i0, fi), i1 = std::max(i1, fi), j0 = std::min(j0, fj), j1 = std::max(j1, fj);
        }
        int a0 = std::max(0, static_cast<int>(std::floor(i0)) - 1), a1 = std::min(grid_.w - 1, static_cast<int>(std::ceil(i1)) + 1);
        int b0 = std::max(0, static_cast<int>(std::floor(j0)) - 1), b1 = std::min(grid_.h - 1, static_cast<int>(std::ceil(j1)) + 1);
        if (!Clip(a0, a1, b0, b1)) return; // (2.15.3) only the texels the lamp's light is summed over
        const float r2 = reach_ * reach_;
        for (int j = b0; j <= b1; j++)
            for (int i = a0; i <= a1; i++) {
                const size_t at = static_cast<size_t>(j) * grid_.w + i;
                if (!Weighs(at) || !(vis_[at] > 0.0f)) continue;
                float x, z;
                TexelWorld(grid_, i, j, x, z);
                const float dx = x - L.x, dz = z - L.z;
                if (dx * dx + dz * dz >= r2) continue;
                tests_++;
                const float p = Pass(L, x, grid_.ty, z, w, patches_.data(), grid_.soft);
                if (p < 1.0f) vis_[at] *= std::max(p, 0.0f);
                if (w.pn && p > 0.0f && CrossesLine(L, x, z, w)) through_++;
            }
    }
    // (2.15.3) The lamp's stamp weight per texel of the box its light is summed over (wgt_, box_; box_ empty when it adds
    // nothing), made once per lamp: Accumulate reads it, and the wall and crown tests skip the texels it leaves out (their
    // share past the walls is never read), so a build does fewer tests and gives the same bits
    void Weights(const Lamp& L) {
        box_[0] = box_[2] = 0, box_[1] = box_[3] = -1;
        if (!(reach_ > 0.0f)) return;
        float ci, cj;
        if (!WorldTexel(grid_, L.x, L.z, ci, cj)) return;
        // the reach in texels along each lot axis (rotation keeps lengths: the lot matrix's rows are unit for a lot)
        const float sc = std::sqrt(std::max(grid_.m0 * grid_.m0 + grid_.m2 * grid_.m2, grid_.m8 * grid_.m8 + grid_.m10 * grid_.m10));
        const float rt = reach_ * kTexelsPerMetre / (sc > 1e-3f ? sc : 1.0f) + 1.0f;
        const int a0 = std::max(0, static_cast<int>(std::floor(ci - rt))), a1 = std::min(grid_.w - 1, static_cast<int>(std::ceil(ci + rt)));
        const int b0 = std::max(0, static_cast<int>(std::floor(cj - rt))), b1 = std::min(grid_.h - 1, static_cast<int>(std::ceil(cj + rt)));
        for (int j = b0; j <= b1; j++)
            for (int i = a0; i <= a1; i++) {
                float x, z;
                TexelWorld(grid_, i, j, x, z);
                wgt_[static_cast<size_t>(j) * grid_.w + i] = Weight(L, x, grid_.ty, z);
            }
        box_[0] = a0, box_[1] = a1, box_[2] = b0, box_[3] = b1;
    }
    // The texels of [a0, a1] x [b0, b1] the lamp's light is summed over, clipped to box_; false when none
    bool Clip(int& a0, int& a1, int& b0, int& b1) const {
        a0 = std::max(a0, box_[0]), a1 = std::min(a1, box_[1]), b0 = std::max(b0, box_[2]), b1 = std::min(b1, box_[3]);
        return a0 <= a1 && b0 <= b1;
    }
    bool Weighs(size_t at) const { return wgt_[at] >= kMinWeight; }
    void Accumulate(bool tested = true) {
        for (int j = box_[2]; j <= box_[3]; j++)
            for (int i = box_[0]; i <= box_[1]; i++) {
                const size_t at = static_cast<size_t>(j) * grid_.w + i;
                const float wgt = wgt_[at];
                if (!(wgt >= kMinWeight)) continue;
                den_[at] += wgt;
                num_[at] += tested ? wgt * vis_[at] : wgt;
            }
    }
    void Finish() {
        const int W = grid_.w, H = grid_.h;
        std::vector<float> m(static_cast<size_t>(W) * H), t(m.size());
        // 1 - blocked / max(total, kOtherLight) (blocked: the light each lamp lost to the walls, partly through an opening's rim): the lamps' own share where they light the ground; where their light is faint
        // the pass's other light (the lot map also holds light that is not a bake lamp's) is not taken with it
        for (size_t i = 0; i < m.size(); i++) m[i] = std::clamp(1.0f - (den_[i] - num_[i]) / std::max(den_[i], kOtherLight), 0.0f, 1.0f);
        // soft edges: [1 2 1] along each axis, twice (a penumbra of about half a metre; the pass samples it bilinearly)
        // (2.15.3: by rows, the edges apart, so the loops vectorise; each texel takes the same sum as before)
        const auto blur = [](float a, float b, float c) { return (a + 2.0f * b + c) * 0.25f; };
        for (int pass = 0; pass < 2; pass++) {
            for (int j = 0; j < H; j++) {
                const float* r = &m[static_cast<size_t>(j) * W];
                float* o = &t[static_cast<size_t>(j) * W];
                o[0] = blur(r[0], r[0], r[std::min(1, W - 1)]);
                for (int i = 1; i < W - 1; i++) o[i] = blur(r[i - 1], r[i], r[i + 1]);
                if (W > 1) o[W - 1] = blur(r[W - 2], r[W - 1], r[W - 1]);
            }
            for (int j = 0; j < H; j++) {
                const float *a = &t[static_cast<size_t>(std::max(j - 1, 0)) * W], *b = &t[static_cast<size_t>(j) * W], *c = &t[static_cast<size_t>(std::min(j + 1, H - 1)) * W];
                float* o = &m[static_cast<size_t>(j) * W];
                for (int i = 0; i < W; i++) o[i] = blur(a[i], b[i], c[i]);
            }
        }
        out_.resize(m.size());
        for (size_t i = 0; i < m.size(); i++) out_[i] = ToByte(std::clamp(m[i], 0.0f, 1.0f) * 255.0f);
    }

    Grid grid_;
    std::vector<Lamp> lamps_, others_; // others_: not tested against the walls (Init)
    std::vector<Wall> walls_;
    std::vector<WallOpenings::Patch> patches_;
    std::vector<Canopy::Ellipsoid> canopies_;
    size_t canopy_ = 0;
    long long canopyTests_ = 0;
    std::vector<float> num_, den_, vis_; // vis_: the current lamp's share past the walls tested so far, per texel
    std::vector<float> wgt_; // the current lamp's stamp weight over box_ (Weights)
    int box_[4] = {0, -1, 0, -1}; // i0, i1, j0, j1
    std::vector<uint8_t> out_;
    size_t lamp_ = 0, wall_ = 0;
    int phase_ = 0;
    float reach_ = 0.0f;
    long long tests_ = 0, through_ = 0;
    bool done_ = true;
    // (counters only) the ray from the head to (x, z) crosses the wall's line
    static bool CrossesLine(const Lamp& L, float x, float z, const Wall& w) {
        const float rx = x - L.x, rz = z - L.z, sx = w.x1 - w.x0, sz = w.z1 - w.z0, den = rx * sz - rz * sx;
        if (std::fabs(den) < 1e-9f) return false;
        const float qx = w.x0 - L.x, qz = w.z0 - L.z, t = (qx * sz - qz * sx) / den, u = (qx * rz - qz * rx) / den;
        return t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f;
    }
};


// ---- (2.15.3) The shade on the world grass around lots: one terrain chunk's texture (lot_light_bridge.cpp ChunkShade; L8,
// kChunkTexels a side over the chunk's kChunkSize metres, full mips). A texel is 1 - the most light a lot's walls take
// there / the most lamp light a lot sees there (W = max(stamp weight, kOtherLight): (1 - M) W is the light that lot's walls
// take; 255 where no lot is over it), so over one lot it is that lot's shade, the lot pass's on its side of the edge. Each
// lot tests only its own walls: weighing the shades (sum W M / sum W) let a lot next door that sees the same lamp but not the
// wall lighten the shadow past the edge, a straight step at the lot's edge. A mip texel is the rounded mean of the four
// under it. ChunkFull
// makes the whole chunk; ChunkUpdate makes again only the texels over a changed box and the mip texels over them, from the
// texture's own texels around that box. Both give the same bytes (tools/ground_shade_test): a texel sums its lots in the
// order of the list (sorted by lot), and a texel past the box has the same lots as when the texture was made.
struct ExtData { // one lot's shade over its grid (ShadeJob::Result) and the lamps' stamp weight it is a share of
    std::vector<uint8_t> v;
    std::vector<float> w;
};
struct ExtSrc { // one lot's shade, for a chunk (shared, never changed once made)
    Grid grid;
    std::shared_ptr<const ExtData> v;
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0; // its world box
    uint64_t lot = 0;
};
constexpr int kChunkTexels = 1024;
constexpr float kChunkSize = 256.0f;
constexpr int kChunkLevels = 11; // 1024 x 1024 .. 1 x 1
struct TexRect { // texels [i0, i1] x [j0, j1] of one level
    int i0 = 0, j0 = 0, i1 = -1, j1 = -1;
    bool Empty() const { return i1 < i0 || j1 < j0; }
    int W() const { return i1 - i0 + 1; }
    int H() const { return j1 - j0 + 1; }
    size_t Size() const { return Empty() ? 0 : static_cast<size_t>(W()) * H(); }
};
// The level-l texels whose level-0 texels meet r0
inline TexRect LevelRect(const TexRect& r0, int l) { return {r0.i0 >> l, r0.j0 >> l, r0.i1 >> l, r0.j1 >> l}; }
// The level-l texels the level-(l + 1) texels over r0 are the means of (l < kChunkLevels - 1): LevelRect(r0, l) and the
// texels beside it in the same 2 x 2 blocks
inline TexRect BlockRect(const TexRect& r0, int l) {
    const TexRect n = LevelRect(r0, l + 1);
    return {2 * n.i0, 2 * n.j0, 2 * n.i1 + 1, 2 * n.j1 + 1};
}
// The level-0 texels a world box covers, clipped to the chunk whose corner is (wx0, wz0)
inline TexRect ChunkRectOf(float wx0, float wz0, float x0, float z0, float x1, float z1) {
    constexpr int N = kChunkTexels;
    constexpr float tpm = N / kChunkSize; // texels a metre
    TexRect r;
    r.i0 = std::max(0, static_cast<int>(ApexMath::Floor((x0 - wx0) * tpm))), r.i1 = std::min(N - 1, static_cast<int>(ApexMath::Ceil((x1 - wx0) * tpm)));
    r.j0 = std::max(0, static_cast<int>(ApexMath::Floor((z0 - wz0) * tpm))), r.j1 = std::min(N - 1, static_cast<int>(ApexMath::Ceil((z1 - wz0) * tpm)));
    return r;
}
// The shade of one lot at the world point (bilinear on its grid); false outside its grid
inline bool ExtShadeAt(const ExtSrc& m, float x, float z, float& v, float& wt) {
    float fi, fj;
    if (!WorldTexel(m.grid, x, z, fi, fj)) return false;
    const int W = m.grid.w, H = m.grid.h;
    if (!(fi >= 0.0f) || !(fj >= 0.0f) || !(fi <= W) || !(fj <= H)) return false;
    const float a = std::clamp(fi - 0.5f, 0.0f, W - 1.0f), b = std::clamp(fj - 0.5f, 0.0f, H - 1.0f);
    const int i0 = static_cast<int>(a), j0 = static_cast<int>(b), i1 = std::min(i0 + 1, W - 1), j1 = std::min(j0 + 1, H - 1);
    const float u = a - i0, t = b - j0;
    const std::vector<uint8_t>& e = m.v->v;
    const std::vector<float>& ww = m.v->w;
    const auto at = [&](int i, int j) { return static_cast<float>(e[static_cast<size_t>(j) * W + i]); };
    const auto wa = [&](int i, int j) { return ww[static_cast<size_t>(j) * W + i]; };
    v = (at(i0, j0) * (1 - u) + at(i1, j0) * u) * (1 - t) + (at(i0, j1) * (1 - u) + at(i1, j1) * u) * t;
    wt = (wa(i0, j0) * (1 - u) + wa(i1, j0) * u) * (1 - t) + (wa(i0, j1) * (1 - u) + wa(i1, j1) * u) * t;
    return true;
}
// The level-0 texels over r (px: r's size, by rows) from the lots over the chunk whose corner is (wx0, wz0); returns the
// count of them under 255
inline int ChunkTexels(const std::vector<ExtSrc>& srcs, float wx0, float wz0, const TexRect& r, std::vector<uint8_t>& px) {
    constexpr float tpm = kChunkTexels / kChunkSize;
    px.assign(r.Size(), 255);
    if (r.Empty()) return 0;
    std::vector<float> lost, seen; // per texel: the most light a lot's walls take there, the most lamp light a lot sees there
    for (const ExtSrc& m : srcs) {
        const size_t n = static_cast<size_t>(m.grid.w) * m.grid.h;
        if (!m.v || m.v->v.size() != n || m.v->w.size() != n) continue;
        if (seen.empty()) lost.assign(r.Size(), 0.0f), seen.assign(r.Size(), 0.0f);
        TexRect s = ChunkRectOf(wx0, wz0, m.x0, m.z0, m.x1, m.z1);
        s.i0 = std::max(s.i0, r.i0), s.i1 = std::min(s.i1, r.i1), s.j0 = std::max(s.j0, r.j0), s.j1 = std::min(s.j1, r.j1);
        for (int j = s.j0; j <= s.j1; j++)
            for (int i = s.i0; i <= s.i1; i++) {
                float v, wt;
                if (!ExtShadeAt(m, wx0 + (i + 0.5f) / tpm, wz0 + (j + 0.5f) / tpm, v, wt)) continue;
                const float w = std::max(wt, kOtherLight); // (as the lot's own shade: 1 - lost / max(total, kOtherLight))
                const size_t at = static_cast<size_t>(j - r.j0) * r.W() + (i - r.i0);
                lost[at] = std::max(lost[at], (1.0f - v / 255.0f) * w);
                seen[at] = std::max(seen[at], w);
            }
    }
    int shaded = 0;
    for (size_t k = 0; k < seen.size(); k++)
        if (seen[k] > 0.0f) {
            const uint8_t q = ToByte(std::clamp(255.0f * (1.0f - lost[k] / seen[k]), 0.0f, 255.0f));
            if (q < 255) px[k] = q, shaded++;
        }
    return shaded;
}
// The level-l texels over `out` from the level-(l - 1) texels `in` over inRect (which holds out's 2 x 2 blocks)
inline void ChunkMip(const std::vector<uint8_t>& in, const TexRect& inRect, const TexRect& out, std::vector<uint8_t>& px) {
    px.resize(out.Size());
    const int iw = inRect.W();
    for (int j = out.j0; j <= out.j1; j++)
        for (int i = out.i0; i <= out.i1; i++) {
            const size_t p = static_cast<size_t>(2 * j - inRect.j0) * iw + (2 * i - inRect.i0), q = p + iw;
            px[static_cast<size_t>(j - out.j0) * out.W() + (i - out.i0)] = static_cast<uint8_t>((in[p] + in[p + 1] + in[q] + in[q + 1] + 2) / 4);
        }
}
// a's texels over ra into b over rb (which holds ra)
inline void PutRect(const std::vector<uint8_t>& a, const TexRect& ra, std::vector<uint8_t>& b, const TexRect& rb) {
    for (int j = ra.j0; j <= ra.j1; j++)
        std::copy_n(&a[static_cast<size_t>(j - ra.j0) * ra.W()], ra.W(), &b[static_cast<size_t>(j - rb.j0) * rb.W() + (ra.i0 - rb.i0)]);
}
// A chunk's texels: levels[l] over rects[l] (the whole level, or what changed); no levels: no lot shades the chunk
struct ChunkTexture {
    std::vector<TexRect> rects;
    std::vector<std::vector<uint8_t>> levels;
    int shaded = 0; // level-0 texels under 255
};
inline void ChunkFull(const std::vector<ExtSrc>& srcs, float wx0, float wz0, ChunkTexture& out) {
    out = {};
    const TexRect r{0, 0, kChunkTexels - 1, kChunkTexels - 1};
    std::vector<uint8_t> px;
    out.shaded = ChunkTexels(srcs, wx0, wz0, r, px);
    if (!out.shaded) return; // nothing shaded: no texture
    out.rects.push_back(r);
    out.levels.push_back(std::move(px));
    for (int l = 1; l < kChunkLevels; l++) {
        const TexRect n{0, 0, (kChunkTexels >> l) - 1, (kChunkTexels >> l) - 1};
        std::vector<uint8_t> next;
        ChunkMip(out.levels.back(), out.rects.back(), n, next);
        out.rects.push_back(n);
        out.levels.push_back(std::move(next));
    }
}
// The texels over r0 (level 0) and the mip texels over them made again. old[l]: the texture's level-l texels over
// BlockRect(r0, l) (l < kChunkLevels - 1); oldShaded: its count of level-0 texels under 255. out: LevelRect(r0, l) per level,
// or no levels when nothing stays shaded (no texture). false: old does not fit r0
inline bool ChunkUpdate(const std::vector<ExtSrc>& srcs, float wx0, float wz0, const TexRect& r0, const std::vector<std::vector<uint8_t>>& old, int oldShaded,
                        ChunkTexture& out) {
    out = {};
    if (r0.Empty() || old.size() != static_cast<size_t>(kChunkLevels - 1)) return false;
    for (int l = 0; l < kChunkLevels - 1; l++)
        if (old[l].size() != BlockRect(r0, l).Size()) return false;
    std::vector<uint8_t> px;
    const int now = ChunkTexels(srcs, wx0, wz0, r0, px);
    std::vector<uint8_t> cur = old[0];
    TexRect cr = BlockRect(r0, 0);
    int before = 0;
    for (int j = r0.j0; j <= r0.j1; j++)
        for (int i = r0.i0; i <= r0.i1; i++) before += cur[static_cast<size_t>(j - cr.j0) * cr.W() + (i - cr.i0)] < 255;
    out.shaded = oldShaded - before + now;
    if (out.shaded <= 0) return out.shaded == 0; // nothing shaded: no texture
    PutRect(px, r0, cur, cr);
    out.rects.push_back(r0);
    out.levels.push_back(std::move(px));
    for (int l = 1; l < kChunkLevels; l++) {
        const TexRect rl = LevelRect(r0, l);
        std::vector<uint8_t> v;
        ChunkMip(cur, cr, rl, v);
        if (l < kChunkLevels - 1) {
            cur = old[l];
            cr = BlockRect(r0, l);
            PutRect(v, rl, cur, cr);
        }
        out.rects.push_back(rl);
        out.levels.push_back(std::move(v));
    }
    return true;
}

} // namespace GroundShadow
