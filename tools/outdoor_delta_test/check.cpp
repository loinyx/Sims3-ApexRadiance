// Outdoor lamp edits by delta (features/outdoor_delta_model.h, features/outdoor_delta.h): a synthetic room 0 is solved in
// full after every lamp edit and brought up to date by the delta from each light's kept part, and the two are compared texel
// by texel (8-bit levels, after Smooth light maps' dither, the game's pack and the class-2 wall blur). Also: the half floats
// the parts keep, the floor mask's alpha, the pack, the shared wall blur against the code it replaced, and the reach bound
// against the lamp law. Read-only: prints to stdout, writes no file.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <tuple>
#include <vector>
#include <xmmintrin.h>
#include "features/outdoor_delta_model.h"
#include "features/rt_lamp_model.h"
#include "features/gpu_lamp_law.h"
#include "features/room_lamp_move.h"
#include "features/open_rooms_above.h"
#include "features/rt_lamp_proxy.h"
#include "features/rt_lamp_blockers.h"
#include "features/object_legs.h"
#include "features/phys_lamp_pick.h"
#include "features/terrain_lighting_policy.h"
#include <string>
// The dither offset of 2.12.4's Smooth light maps (RoomMapDither::Offset, interleaved gradient noise): 2.12.3 + RT has no
// map dither (its relight passes 0), but the compose shader keeps the input, so the harness still checks it
namespace RoomMapDither {
inline float Offset(unsigned x, unsigned y) {
    const double f = std::fmod(0.06711056 * static_cast<double>(x) + 0.00583715 * static_cast<double>(y), 1.0);
    const double g = std::fmod(52.9829189 * f, 1.0);
    return static_cast<float>(g - 0.5);
}
} // namespace RoomMapDither

using namespace OutdoorDeltaModel;

int checks = 0, failures = 0;
void Check(bool ok, const char* text) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", text);
    }
}

// ---- the blur as LevelLightShare BlurWalls had it before it moved to the model (2.12.4) ----
namespace Old {
inline uint32_t Avg4(uint32_t a, uint32_t b) { return (a | b) - (((a ^ b) >> 1) & 0x7F7F7F7Fu); }
void BlurRow(uint32_t* p, uint32_t n) {
    if (n < 2) return;
    uint32_t prev = p[0];
    for (uint32_t j = 0; j + 1 < n; j++) {
        const uint32_t side = Avg4(p[j + 1], prev);
        prev = p[j];
        p[j] = Avg4(prev, side);
    }
    const uint32_t last = p[n - 1];
    p[n - 1] = Avg4(Avg4(last, prev), last);
}
void BlurColumn(uint32_t* p, int rows, int n) {
    uint32_t prev = p[0];
    for (int r = 0; r < rows; r++) {
        const uint32_t cur = p[static_cast<size_t>(r) * n], next = r + 1 < rows ? p[static_cast<size_t>(r + 1) * n] : cur;
        p[static_cast<size_t>(r) * n] = Avg4(Avg4(prev, next), cur);
        prev = cur;
    }
}
// one job of BlurWalls (ghost rows g: 4 x n, above / below as the job had them)
void Job(std::vector<uint32_t>& ext, const std::vector<uint32_t>& before, const std::vector<uint32_t>& g, int n, int rows, bool above, bool below, uint32_t passes) {
    const int top = above ? 2 : 0, total = top + rows + (below ? 2 : 0);
    ext.assign(static_cast<size_t>(total) * n, 0u);
    if (above) std::memcpy(ext.data(), g.data(), static_cast<size_t>(n) * 2 * 4);
    std::memcpy(ext.data() + static_cast<size_t>(top) * n, before.data(), before.size() * 4);
    if (below) std::memcpy(ext.data() + static_cast<size_t>(top + rows) * n, g.data() + static_cast<size_t>(n) * 2, static_cast<size_t>(n) * 2 * 4);
    for (uint32_t pass = 0; pass < passes; pass++) {
        for (int k = 0; k < total; k++) BlurRow(ext.data() + static_cast<size_t>(k) * n, n);
        for (int x = 0; x < n; x++) BlurColumn(ext.data() + x, total, n);
    }
    for (const int k : {0, rows - 1}) {
        if (k == 0 ? above : below) continue;
        uint32_t* row = ext.data() + static_cast<size_t>(top + k) * n;
        std::memcpy(row, before.data() + static_cast<size_t>(k) * n, static_cast<size_t>(n) * 4);
        for (uint32_t pass = 0; pass < passes; pass++) BlurRow(row, n);
    }
}
} // namespace Old

// ---- a synthetic room 0 and its lamps ----
struct Lamp {
    float pos[3];
    float colour[3];
    float W;        // k2 x range x intensity / k1
    float spill;    // a shaded lamp's cone outside: colour x spill (the bound's max(1, spill))
    float coneCos;  // down cone (cos of the half angle); < -1: none
    int mult = 1;   // how often the room's list holds it
};
struct Texel {
    float p[3], n[3];
    int map;      // 0 wall (blurred), 1 floor
    bool masked;  // an outdoor floor texel of a story >= 1
    bool gated;   // more than 2 m over its lot base
    uint16_t x, y;
    int block = -1;
};
struct Seg { // a wall that blocks lamp light on the ground: a vertical rectangle x = cx, z in [z0, z1], y up to top
    float cx, z0, z1, top;
};
const float kThr = 2.0f * 3.0f / 255.0f; // class 1 threshold
const float kCmax = 2.0f;

// The light one lamp adds at one texel, as the solve sums it (threshold drop and the soft cut, walls and a cone), with
// walls tested (the room's own sum, and the mask's first copy) or not (the mask's second copy). wrap: N.l ignored.
float Transmission(const Lamp& L, const float* p, const std::vector<Seg>& walls) {
    float t = 1.0f;
    for (const Seg& w : walls) {
        const float a = L.pos[0] - w.cx, b = p[0] - w.cx;
        if (a * b >= 0.0f) continue;
        const float f = a / (a - b);
        const float z = L.pos[2] + f * (p[2] - L.pos[2]), y = L.pos[1] + f * (p[1] - L.pos[1]);
        if (z >= w.z0 && z <= w.z1 && y <= w.top) t *= 0.25f; // a soft wall (the penumbra's shares)
    }
    return t;
}
void Eval(const Lamp& L, const Texel& s, bool walls, bool wrap, const std::vector<Seg>& segs, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    const float d[3] = {L.pos[0] - s.p[0], L.pos[1] - s.p[1], L.pos[2] - s.p[2]};
    const float d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (!(d2 > 1e-6f)) return;
    const float dl = std::sqrt(d2);
    float ndl = (d[0] * s.n[0] + d[1] * s.n[1] + d[2] * s.n[2]) / dl;
    if (wrap) ndl = 1.0f;
    if (ndl <= 0.0f) return;
    float cone = 1.0f;
    if (L.coneCos >= -1.0f) cone = (-d[1] / dl) >= L.coneCos ? 1.0f : L.spill;
    const float m = std::min(kCmax, L.W * ndl / d2) * cone;
    float c[3];
    for (int i = 0; i < 3; i++) c[i] = L.colour[i] * m;
    const float sum = (c[2] + c[1]) + c[0];
    if (!(sum >= kThr)) return;
    if (sum < 2.0f * kThr) { // the soft cut
        const float x = (sum - kThr) / kThr, f = x * x * (3.0f - 2.0f * x);
        for (float& v : c) v *= f;
    }
    const float t = walls ? Transmission(L, s.p, segs) : 1.0f;
    for (int i = 0; i < 3; i++) out[i] = t * c[i];
}
// The share of the ground atlas from lamps under the texel's plane (UnderFloorShare's form)
float Under(const std::vector<Lamp>& lamps, const Texel& s) {
    if (!s.gated) return -1.0f;
    float all = 0.0f, under = 0.0f;
    for (const Lamp& L : lamps) {
        if (L.mult <= 0) continue;
        const float h = L.pos[1];
        if (!(h > 0.0f)) continue;
        const float dx = L.pos[0] - s.p[0], dz = L.pos[2] - s.p[2], d2 = dx * dx + h * h + dz * dz;
        float st = std::min(0.4f * L.W * 10.0f * h / (d2 * std::sqrt(d2)), 1.0f);
        const float v = st * (L.colour[0] + L.colour[1] + L.colour[2]);
        if (!(v > 0.0f)) continue;
        all += v;
        if (L.pos[1] <= s.p[1] + 0.02f) under += v;
    }
    return all > 1e-4f ? std::clamp(under / all, 0.0f, 1.0f) : -1.0f;
}

struct Room {
    std::vector<Texel> t;
    std::vector<Seg> walls;
    struct Block {
        int x0, y0, x1, y1;
    };
    std::vector<Block> blocks;
    int mapW = 64, mapH = 64;
};
Room MakeRoom() {
    Room r;
    // a floor 24 x 24 m at 4 texels per metre on a deck 3 m up (masked, gated where over 2 m above the base 0), and two wall
    // faces (blocks of 13 x 9 texels) lit on their outside
    for (int z = 0; z < 48; z++)
        for (int x = 0; x < 48; x++) {
            Texel s{};
            s.p[0] = -6.0f + x * 0.25f, s.p[1] = 3.0f, s.p[2] = -6.0f + z * 0.25f;
            s.n[1] = 1.0f;
            s.map = 1;
            s.masked = true;
            s.gated = (x + z) % 3 != 0; // some texels gated
            s.x = static_cast<uint16_t>(x), s.y = static_cast<uint16_t>(z);
            r.t.push_back(s);
        }
    for (int b = 0; b < 2; b++) {
        Room::Block blk{b * 14, 50, b * 14 + 12, 58};
        r.blocks.push_back(blk);
        for (int y = blk.y0; y <= blk.y1; y++)
            for (int x = blk.x0; x <= blk.x1; x++) {
                Texel s{};
                s.p[0] = b == 0 ? 2.0f : -3.0f, s.p[1] = 3.0f + (blk.y1 - y) * 0.375f, s.p[2] = -2.0f + (x - blk.x0) * 0.33f;
                s.n[0] = b == 0 ? 1.0f : -1.0f;
                s.map = 0;
                s.x = static_cast<uint16_t>(x), s.y = static_cast<uint16_t>(y);
                s.block = b;
                r.t.push_back(s);
            }
    }
    r.walls.push_back({0.5f, -3.0f, 3.0f, 5.0f});
    r.walls.push_back({-1.5f, 1.0f, 6.0f, 4.0f});
    return r;
}

// The texel the solve packs from the sums (and the floor mask), the dither and the game's pack
uint32_t PackTexel(const Texel& s, const Totals& t, float under) {
    const float alpha = s.masked ? MaskAlpha(t.vis, t.all, under) : 0.0f;
    alignas(16) float c[4];
    PackInput(t.S, alpha, c);
    Dither(c, RoomMapDither::Offset(s.x, s.y) / 255.0f);
    return PackModel(c);
}
// The maps after the solve and the class-2 wall blur
struct Maps {
    std::vector<uint32_t> floor, wall; // 64 x 64 each
};
void Blur(const Room& r, std::vector<uint32_t>& wall) {
    for (const Room::Block& b : r.blocks) {
        const int n = b.x1 - b.x0 + 1, rows = b.y1 - b.y0 + 1;
        std::vector<uint32_t> before(static_cast<size_t>(n) * rows), ext(static_cast<size_t>(n) * rows);
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < n; x++) before[static_cast<size_t>(y) * n + x] = wall[static_cast<size_t>(b.y0 + y) * r.mapW + b.x0 + x];
        ext = before;
        BlurBlock(ext.data(), before.data(), n, rows, false, false, 2);
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < n; x++) wall[static_cast<size_t>(b.y0 + y) * r.mapW + b.x0 + x] = ext[static_cast<size_t>(y) * n + x];
    }
}
// The full solve: every light of the list, in the list's order, summed as LightPointWithAllLights sums it
Maps FullSolve(const Room& r, const std::vector<Lamp>& lamps, std::vector<Totals>* totals = nullptr) {
    Maps m;
    m.floor.assign(static_cast<size_t>(r.mapW) * r.mapH, 0);
    m.wall.assign(static_cast<size_t>(r.mapW) * r.mapH, 0);
    if (totals) totals->assign(r.t.size(), Totals{});
    for (size_t i = 0; i < r.t.size(); i++) {
        const Texel& s = r.t[i];
        Totals t;
        float vis[3] = {}, all[3] = {};
        for (const Lamp& L : lamps)
            for (int k = 0; k < L.mult; k++) {
                float c[3];
                Eval(L, s, true, false, r.walls, c);
                for (int q = 0; q < 3; q++) t.S[q] = c[q] + t.S[q];
                if (s.masked) {
                    Eval(L, s, true, true, r.walls, c);
                    for (int q = 0; q < 3; q++) vis[q] = c[q] + vis[q];
                    Eval(L, s, false, true, r.walls, c);
                    for (int q = 0; q < 3; q++) all[q] = c[q] + all[q];
                }
            }
        t.vis = Lum3(vis), t.all = Lum3(all);
        if (totals) (*totals)[i] = t;
        (s.map == 0 ? m.wall : m.floor)[static_cast<size_t>(s.y) * r.mapW + s.x] = PackTexel(s, t, Under(lamps, s));
    }
    Blur(r, m.wall);
    return m;
}

// ---- the record and the delta, as OutdoorDelta keeps and applies them ----
struct Record {
    std::vector<Totals> t;
    std::vector<uint32_t> packed;              // before the blur
    std::vector<std::vector<Contrib>> parts;   // per lamp index
    std::vector<int> mult;
    Maps maps;
};
Contrib Part(const Lamp& L, const Texel& s, uint32_t texel, const std::vector<Seg>& walls) {
    Contrib p;
    p.texel = texel;
    float vis[3] = {}, all[3] = {};
    for (int k = 0; k < L.mult; k++) {
        float c[3];
        Eval(L, s, true, false, walls, c);
        for (int q = 0; q < 3; q++) p.rgb[q] += c[q];
        if (s.masked) {
            Eval(L, s, true, true, walls, c);
            for (int q = 0; q < 3; q++) vis[q] += c[q];
            Eval(L, s, false, true, walls, c);
            for (int q = 0; q < 3; q++) all[q] += c[q];
        }
    }
    p.vis = s.masked ? ToHalf(Lum3(vis)) : 0;
    p.all = s.masked ? ToHalf(Lum3(all)) : 0;
    return p;
}
Record Capture(const Room& r, const std::vector<Lamp>& lamps) {
    Record rec;
    rec.maps = FullSolve(r, lamps, &rec.t);
    rec.parts.resize(lamps.size());
    rec.mult.resize(lamps.size());
    rec.packed.resize(r.t.size());
    for (size_t li = 0; li < lamps.size(); li++) {
        rec.mult[li] = lamps[li].mult;
        for (uint32_t i = 0; i < r.t.size(); i++) {
            const Contrib p = Part(lamps[li], r.t[i], i, r.walls);
            if (Nonzero(p)) rec.parts[li].push_back(p);
        }
    }
    for (size_t i = 0; i < r.t.size(); i++) rec.packed[i] = PackTexel(r.t[i], rec.t[i], Under(lamps, r.t[i]));
    return rec;
}
// One lamp changed (its new state in lamps[li]); the reach bound limits where it is evaluated again
long Delta(const Room& r, const std::vector<Lamp>& lamps, size_t li, Record& rec, long* evaluated) {
    std::vector<uint8_t> touched(r.t.size(), 0);
    for (const Contrib& p : rec.parts[li]) {
        Remove(rec.t[p.texel], p);
        touched[p.texel] = 1;
    }
    std::vector<Contrib> fresh;
    const Lamp& L = lamps[li];
    const float esum = std::max(L.colour[0], 0.0f) + std::max(L.colour[1], 0.0f) + std::max(L.colour[2], 0.0f);
    const float b2 = ReachBound2(L.W, esum, L.coneCos >= -1.0f ? L.spill : 1.0f, kThr);
    if (L.mult > 0 && b2 != 0.0f)
        for (uint32_t i = 0; i < r.t.size(); i++) {
            const float dx = L.pos[0] - r.t[i].p[0], dy = L.pos[1] - r.t[i].p[1], dz = L.pos[2] - r.t[i].p[2];
            if (b2 > 0.0f && dx * dx + dy * dy + dz * dz > b2) continue;
            (*evaluated)++;
            const Contrib p = Part(L, r.t[i], i, r.walls);
            if (Nonzero(p)) fresh.push_back(p);
        }
    for (const Contrib& p : fresh) {
        Add(rec.t[p.texel], p);
        touched[p.texel] = 1;
    }
    rec.parts[li] = fresh;
    rec.mult[li] = L.mult;
    long written = 0;
    std::vector<uint8_t> dirty(r.blocks.size(), 0);
    for (size_t i = 0; i < r.t.size(); i++) {
        if (!touched[i] && !r.t[i].gated) continue;
        const uint32_t v = PackTexel(r.t[i], rec.t[i], Under(lamps, r.t[i]));
        if (v == rec.packed[i]) continue;
        rec.packed[i] = v;
        written++;
        if (r.t[i].map == 0) dirty[static_cast<size_t>(r.t[i].block)] = 1;
        else rec.maps.floor[static_cast<size_t>(r.t[i].y) * r.mapW + r.t[i].x] = v;
    }
    for (size_t b = 0; b < r.blocks.size(); b++) {
        if (!dirty[b]) continue;
        const Room::Block& blk = r.blocks[b];
        const int n = blk.x1 - blk.x0 + 1, rows = blk.y1 - blk.y0 + 1;
        std::vector<uint32_t> before(static_cast<size_t>(n) * rows);
        for (size_t i = 0; i < r.t.size(); i++)
            if (r.t[i].map == 0 && r.t[i].block == static_cast<int>(b)) before[static_cast<size_t>(r.t[i].y - blk.y0) * n + (r.t[i].x - blk.x0)] = rec.packed[i];
        std::vector<uint32_t> ext = before;
        BlurBlock(ext.data(), before.data(), n, rows, false, false, 2);
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < n; x++) rec.maps.wall[static_cast<size_t>(blk.y0 + y) * r.mapW + blk.x0 + x] = ext[static_cast<size_t>(y) * n + x];
    }
    return written;
}
struct Diff {
    int max = 0;
    long at1 = 0, over1 = 0, n = 0;
};
void Compare(const Maps& a, const Maps& b, Diff& d) {
    for (int k = 0; k < 2; k++) {
        const std::vector<uint32_t>& x = k ? a.wall : a.floor;
        const std::vector<uint32_t>& y = k ? b.wall : b.floor;
        for (size_t i = 0; i < x.size(); i++) {
            const int m = MaxLevelDiff(x[i], y[i]);
            d.n++;
            d.max = std::max(d.max, m);
            if (m == 1) d.at1++;
            if (m > 1) d.over1++;
        }
    }
}

// ---- GPU room lamps stage 4: the directional maps packed again from their parts (OutdoorDelta BuildBasis, LampBasisNow,
// WriteBasis) against a model of the game's fill: FUN_0069f280 adds w_i x colour / sum w for the four basis directions
// (+-0.8944, 0.4472, 0), (0, 0.4472, +-0.8944), w_i = clamp(b_i . d, 0, 1), d the unit direction to the light ----
struct BasisLight {
    float pos[3], colour[3];
    float share = 1.0f; // Apex's tests: 1 the game's step in place, else contribution x share
    int mult = 1;
    bool baked = false; // a light the GPU does not draw (kept in the base)
};
void GameStep(const float* p, const BasisLight& L, float acc[16]) {
    static const float b[4][3] = {{0.8944f, 0.4472f, 0.0f}, {-0.8944f, 0.4472f, 0.0f}, {0.0f, 0.4472f, 0.8944f}, {0.0f, 0.4472f, -0.8944f}};
    float d[3] = {L.pos[0] - p[0], L.pos[1] - p[1], L.pos[2] - p[2]};
    const float d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], inv = 1.0f / std::sqrt(d2);
    for (float& v : d) v *= inv;
    const float fall = 1.0f / (1.0f + d2); // (the rig colour at p: any law of the place will do here)
    float w[4], sum = 0.0f;
    for (int i = 0; i < 4; i++) sum += w[i] = std::clamp(b[i][0] * d[0] + b[i][1] * d[1] + b[i][2] * d[2], 0.0f, 1.0f);
    if (!(sum > 1e-6f)) return;
    for (int i = 0; i < 4; i++)
        for (int c = 0; c < 4; c++) acc[4 * i + c] += w[i] * (c < 3 ? L.colour[c] * fall : 1.0f) / sum;
}
// The game's sample: every listing of every light in turn (Apex's share as LevelLightShare's BasisLightHook applies it)
void GameSample(const float* p, const std::vector<BasisLight>& ls, float acc[16], std::vector<std::array<float, 16>>* parts, std::vector<std::array<float, 16>>* steps) {
    std::fill(acc, acc + 16, 0.0f);
    for (size_t j = 0; j < ls.size(); j++) {
        std::array<float, 16> before{}, step{};
        std::copy(acc, acc + 16, before.begin());
        for (int k = 0; k < ls[j].mult; k++) {
            if (ls[j].share == 1.0f) GameStep(p, ls[j], acc);
            else {
                float c[16] = {};
                GameStep(p, ls[j], c);
                for (int i = 0; i < 16; i++) acc[i] += c[i] * ls[j].share;
            }
            GameStep(p, ls[j], step.data());
        }
        if (parts) {
            std::array<float, 16> d{};
            for (int i = 0; i < 16; i++) d[i] = acc[i] - before[i];
            parts->push_back(d), steps->push_back(step);
        }
    }
}
void BasisChecks() {
    std::mt19937 rng(20261102);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    std::vector<BasisLight> ls(6);
    for (BasisLight& L : ls) L = BasisLight{{U(rng) * 8.0f, 1.5f + U(rng), U(rng) * 8.0f}, {0.2f + U(rng), 0.2f + U(rng), 0.2f + U(rng)}};
    ls[1].share = 0.6f;  // (a lamp through an opening: Apex scales its step)
    ls[2].share = 0.0f;  // (behind the floor here: it adds nothing)
    ls[3].mult = 2;      // (listed twice)
    ls[5].baked = true;  // (no GPU law: in the base)
    const int n = 300;
    std::vector<std::array<float, 4>> pos(n);
    for (auto& p : pos) p = {U(rng) * 8.0f, 0.5f, U(rng) * 8.0f, 1.0f};
    // the capture: the base, each lamp's part and share; the check that the parts pack back to the game's texels
    std::vector<float> base(static_cast<size_t>(n) * 16, 0.0f), part(ls.size() * n * 16, 0.0f), pass(ls.size() * n, 0.0f);
    std::vector<uint32_t> game(static_cast<size_t>(n) * 4);
    long passKnown = 0, passRight = 0, packedBack = 0;
    for (int s = 0; s < n; s++) {
        float acc[16];
        std::vector<std::array<float, 16>> parts, steps;
        GameSample(pos[s].data(), ls, acc, &parts, &steps);
        for (int m = 0; m < 4; m++) {
            float in[4];
            BasisPackInput(acc, m, in);
            game[s * 4 + m] = PackModel(in);
        }
        for (size_t j = 0; j < ls.size(); j++) {
            if (ls[j].baked) {
                for (int i = 0; i < 16; i++) base[s * 16 + i] += parts[j][i];
                continue;
            }
            std::copy(parts[j].begin(), parts[j].end(), &part[(j * n + s) * 16]);
            float p = 0.0f;
            if (BasisPass(parts[j].data(), steps[j].data(), p)) passKnown++, passRight += std::fabs(p - ls[j].share) < 1e-4f;
            pass[j * n + s] = p;
        }
    }
    const auto repack = [&](const std::vector<float>& parts, int s, int m) {
        float acc[16];
        for (int i = 0; i < 16; i++) acc[i] = base[s * 16 + i];
        for (size_t j = 0; j < ls.size(); j++)
            for (int i = 0; i < 16; i++) acc[i] += parts[(j * n + s) * 16 + i];
        float in[4];
        BasisPackInput(acc, m, in);
        return PackModel(in);
    };
    for (int s = 0; s < n; s++)
        for (int m = 0; m < 4; m++) packedBack += MaxLevelDiff(repack(part, s, m), game[s * 4 + m]) <= 1;
    std::printf("  directional maps: %ld of %d texels packed back within 1/255, %ld of %ld shares right\n", packedBack, n * 4, passRight, passKnown);
    Check(packedBack == n * 4, "directional maps: a capture's parts pack back to the game's texels");
    Check(passKnown >= static_cast<long>((ls.size() - 1) * n) * 9 / 10 && passRight == passKnown, "directional maps: each part is the game's step times Apex's share");
    // edits: each lamp's part again from the step now (LampBasisNow), packed again against the game's fill with the edit
    const auto edit = [&](const char* what, size_t j, const BasisLight& now, bool off, bool move) {
        std::vector<BasisLight> after = ls;
        after[j] = now;
        if (off) after[j].colour[0] = after[j].colour[1] = after[j].colour[2] = 0.0f; // (the rig colour of a lamp switched off)
        std::vector<float> parts = part;
        long ok = 0, exact = 0;
        bool known = true;
        for (int s = 0; s < n; s++) {
            float one[16] = {}, v[16];
            GameStep(pos[s].data(), after[j], one); // (the game's step called once now)
            const float p = move ? 1.0f : pass[j * n + s];
            if (off) std::fill(v, v + 16, 0.0f);
            else known = known && BasisPartNow(p, now.mult, one, v);
            std::copy(v, v + 16, &parts[(j * n + s) * 16]);
            float acc[16];
            GameSample(pos[s].data(), after, acc, nullptr, nullptr);
            for (int m = 0; m < 4; m++) {
                float in[4];
                BasisPackInput(acc, m, in);
                const int d = MaxLevelDiff(repack(parts, s, m), PackModel(in));
                ok += d <= 1, exact += d == 0;
            }
        }
        std::printf("  %s: %ld of %d texels within 1/255 (%ld exact)\n", what, ok, n * 4, exact);
        Check(known && ok == n * 4, what);
    };
    BasisLight L0 = ls[0];
    edit("directional maps: a lamp switched off", 0, L0, true, false);
    BasisLight L1 = ls[1];
    for (float& c : L1.colour) c *= 0.37f;
    L1.colour[2] = 0.9f;
    edit("directional maps: a scaled lamp recoloured and dimmed", 1, L1, false, false);
    BasisLight L3 = ls[3];
    for (float& c : L3.colour) c *= 1.8f;
    edit("directional maps: a lamp listed twice made brighter", 3, L3, false, false);
    BasisLight L4 = ls[4];
    L4.pos[0] += 2.5f, L4.pos[2] -= 1.25f;
    edit("directional maps: a plain lamp moved", 4, L4, false, true);
    // a part that is not a multiple of the step, and a dark step, are refused / zero
    float step[16] = {}, partNot[16] = {}, out[16], p = 0.0f;
    step[0] = 0.5f, step[5] = 0.25f, partNot[0] = 0.5f, partNot[5] = 0.5f;
    Check(!BasisPass(partNot, step, p) && std::isnan(p), "directional maps: a part not the step times a share has no share");
    const float dark[16] = {};
    Check(BasisPartNow(std::nanf(""), 1, dark, out) && out[0] == 0.0f && !BasisPartNow(std::nanf(""), 1, step, out), "directional maps: an unknown share is refused only where the step is lit");
}
// Physical indoor lamps, stage 5 (OutdoorDelta BasisDuePhys): each law lamp's directional part is its room row x its physical
// share (the proxy's visibility, PhysLampPick::BasisShare, x here the game's 3D test, dropped under K_DROP) x the game's step; the base
// holds only the lights without a law. A room with a wall at x = 4 (lamp A behind it for x > 4) and a table top at y = 1
// under lamp B (listed twice, norm scale 0.8); lamp C has no law (in the base). Packed against the game's own fill with those
// shares; then the write rule: only texels still holding the expected value take the new one
void BasisPhysChecks() {
    using namespace RoomLightTests;
    std::mt19937 rng(20261105);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    RtLampProxy::Mesh mesh;
    const float w0[3] = {4.0f, -1.0f, -1.0f}, w1[3] = {4.0f, 4.0f, -1.0f}, w2[3] = {4.0f, 4.0f, 9.0f}, w3[3] = {4.0f, -1.0f, 9.0f};
    RtLampProxy::AddTri(mesh, w0, w1, w2), RtLampProxy::AddTri(mesh, w0, w2, w3);
    RtLampProxy::BuildGrid(mesh);
    RoomLightTests::Room tests;
    RoomLightTests::Record rc;
    rc.c = {6.0f, 1.0f, 2.0f}, rc.r2 = 1.0f;
    Blocker k{};
    k.vt = kQuadVt;
    const auto put = [&](int off, V v) { k.f[off / 4] = v.x, k.f[off / 4 + 1] = v.y, k.f[off / 4 + 2] = v.z; };
    put(0x70, {6.0f, 1.0f, 2.0f}), put(0x90, {0.0f, 1.0f, 0.0f}), put(0xA0, {1.0f, 0.0f, 0.0f}), put(0xB0, {0.0f, 0.0f, 1.0f});
    for (int j = 0; j < 4; j++) k.f[0xD0 / 4 + j] = 0.5f;
    rc.b.push_back(k);
    tests.rec.push_back(rc);
    std::vector<BasisLight> ls(3);
    ls[0] = BasisLight{{2.0f, 2.2f, 4.0f}, {0.9f, 0.8f, 0.6f}};
    ls[1] = BasisLight{{6.0f, 2.2f, 2.0f}, {0.5f, 0.7f, 1.0f}};
    ls[1].mult = 2;
    ls[2] = BasisLight{{1.0f, 2.0f, 7.0f}, {0.4f, 0.4f, 0.4f}};
    ls[2].baked = true;
    const float rows[2] = {1.0f, 2.0f * 0.8f}; // (mult x norm scale)
    const int n = 400;
    std::vector<uint32_t> stamp;
    uint32_t mark = 0;
    long ok = 0, exact = 0, wallDark = 0, tableDark = 0, dark = 0, darkRight = 0;
    std::vector<uint32_t> want(static_cast<size_t>(n) * 4), game(static_cast<size_t>(n) * 4);
    for (int s = 0; s < n; s++) {
        const float p[4] = {U(rng) * 8.0f, 0.5f, U(rng) * 8.0f, 1.0f};
        float acc[16] = {}, base[16] = {};
        GameStep(p, ls[2], base); // (the base: the light without a law, as the capture kept it)
        std::copy(base, base + 16, acc);
        std::vector<BasisLight> fill = ls; // (the game's fill with the physical shares)
        for (int j = 0; j < 2; j++) {
            float step[16] = {}, part[16];
            int unknown = 0;
            GameStep(p, ls[j], step);
            const float vis = PhysLampPick::BasisShare(mesh, ls[j].pos, 0.15f, p, stamp, mark);
            const float t3d = vis > 0.0f ? RoomLightTests::occlude(tests, {ls[j].pos[0], ls[j].pos[1], ls[j].pos[2]}, {p[0], p[1], p[2]}, &unknown) : 1.0f;
            BasisPartPhys(rows[j], vis * t3d, K_DROP, step, part);
            for (int i = 0; i < 16; i++) acc[i] += part[i];
            const float share = vis * t3d < K_DROP ? 0.0f : vis * t3d;
            fill[j].share = (j == 1 ? 0.8f : 1.0f) * share;
            if (share == 0.0f) {
                dark++;
                darkRight += std::all_of(part, part + 16, [](float v) { return v == 0.0f; }) ? 1 : 0;
                wallDark += j == 0 && vis == 0.0f, tableDark += j == 1 && vis > 0.0f; // (B's samples behind the wall left out)
            }
        }
        float g[16];
        GameSample(p, fill, g, nullptr, nullptr);
        for (int m = 0; m < 4; m++) {
            float in[4], gin[4];
            BasisPackInput(acc, m, in), BasisPackInput(g, m, gin);
            want[s * 4 + m] = PackModel(in), game[s * 4 + m] = PackModel(gin);
            const int d = MaxLevelDiff(want[s * 4 + m], game[s * 4 + m]);
            ok += d <= 1, exact += d == 0;
        }
    }
    std::printf("  physical directional maps: %ld of %d texels within 1/255 of the game's fill with the physical shares (%ld exact); lamp A behind the wall at %ld "
                "samples, lamp B under the table at %ld (%ld of %ld dark shares with a part 0)\n",
                ok, n * 4, exact, wallDark, tableDark, darkRight, dark);
    Check(ok == n * 4, "physical directional maps: base + row x share x step packs as the game's fill with the physical shares");
    Check(wallDark > n / 4 && tableDark > 0 && darkRight == dark, "physical directional maps: the wall and the table top leave their samples without the lamp");
    float step[16], part[16];
    std::fill(step, step + 16, 0.25f);
    BasisPartPhys(1.0f, 0.5f * K_DROP, K_DROP, step, part);
    const bool dropped = std::all_of(part, part + 16, [](float v) { return v == 0.0f; });
    BasisPartPhys(3.0f, 0.25f, K_DROP, step, part);
    Check(dropped && part[0] == 3.0f * 0.25f * 0.25f, "physical directional maps: a share under the game's drop adds nothing, else row x share x step");
    { // a still lamp takes the game's levels (its kept pass), a moved one the physical part x the scale learnt while still
        float st[32], acc[32] = {}, acc2[32] = {};
        std::fill(st, st + 32, 0.25f);
        const float pass[2] = {0.8f, 0.6f}, vis[2] = {0.5f, 0.5f};
        double game = 0.0, phys = 0.0;
        BasisLampAdd(true, pass, 1, 1.0f, vis, K_DROP, st, 2, 1.0f, acc, game, phys);
        const float scale = static_cast<float>(game / phys);
        BasisLampAdd(false, nullptr, 1, 1.0f, vis, K_DROP, st, 2, scale, acc2, game, phys);
        Check(acc[0] == 0.8f * 0.25f && acc[16] == 0.6f * 0.25f && std::fabs(scale - 1.4f) < 1e-5f && std::fabs(acc2[0] - 0.5f * 0.25f * scale) < 1e-6f,
              "physical directional maps: a still lamp writes the game's levels, a moved one the physical part at the game's scale");
    }
    { // the shares kept between writes: lamp A still, then moved, then with fewer samples lit, as traced afresh
        std::vector<float> P(static_cast<size_t>(n) * 3);
        for (float& v : P) v = U(rng) * 8.0f;
        for (int s = 0; s < n; s++) P[s * 3 + 1] = 0.5f;
        PhysLampPick::ShareCache c;
        std::vector<float> vis, fv;
        bool same = true;
        size_t traced[4] = {};
        float L[3] = {ls[0].pos[0], ls[0].pos[1], ls[0].pos[2]};
        for (int round = 0; round < 4; round++) {
            if (round == 2) L[0] += 0.5f; // (moved)
            const auto lit = [&](size_t i) { return round == 3 ? i % 3 != 0 : true; }; // (a value edit darkens some steps)
            traced[round] = PhysLampPick::BasisSharesCached(c, round != 0, n, lit, mesh, L, 0.15f, P.data(), stamp, mark, vis);
            fv.assign(n, 0.0f);
            for (int i = 0; i < n; i++)
                if (lit(i)) fv[i] = PhysLampPick::BasisShare(mesh, L, 0.15f, &P[i * 3], stamp, mark);
            same = same && std::memcmp(vis.data(), fv.data(), n * sizeof(float)) == 0;
        }
        Check(same && traced[0] == static_cast<size_t>(n) && traced[1] == 0 && traced[2] == static_cast<size_t>(n) && traced[3] == 0,
              "physical directional maps: a lamp's kept shares equal its shares traced afresh; only a moved lamp is traced again");
    }
    // the write rule: another solve wrote every 7th texel since the capture; two writes in turn
    std::vector<uint32_t> tex(game), expect(game);
    for (size_t i = 0; i < tex.size(); i += 7) tex[i] ^= 0x00010101u;
    long took = 0, kept = 0;
    for (int round = 0; round < 2; round++)
        for (size_t i = 0; i < tex.size(); i++) {
            const uint32_t w = want[i] ^ static_cast<uint32_t>(round); // (the second write another value)
            if (BasisWrite(tex[i], w, expect[i], game[i])) expect[i] = w, took++;
            else kept++;
        }
    bool rule = true;
    for (size_t i = 0; i < tex.size(); i++) rule = rule && (i % 7 == 0 ? tex[i] == (game[i] ^ 0x00010101u) : tex[i] == (want[i] ^ 1u));
    std::printf("  physical directional maps, the write rule: %ld writes taken, %ld texels left as another solve wrote them\n", took, kept);
    Check(rule && took == 2 * static_cast<long>(tex.size() - (tex.size() + 6) / 7), "physical directional maps: only texels holding the capture's or the last write's value are written");
    // the carry: the room's solve kept again (its levels in every texel), another room's solve writing every 11th since; Apex's
    // last write goes back over it
    std::vector<uint32_t> game2(game), last(expect);
    for (uint32_t& g : game2) g ^= 0x00020202u;
    std::vector<uint32_t> tex2(game2);
    for (size_t i = 0; i < tex2.size(); i += 11) tex2[i] ^= 0x00040404u;
    CarryBasis(last.data(), game.data(), game2.data(), last.size());
    bool carried = true;
    for (size_t i = 0; i < tex2.size(); i++) {
        carried = carried && BasisWrite(tex2[i], last[i], game2[i], game2[i]) == (i % 11 != 0);
        const bool apex = i % 7 != 0 && (want[i] ^ 1u) != game[i]; // (a write of the solve's own value changed nothing)
        carried = carried && tex2[i] == (i % 11 == 0 ? game2[i] ^ 0x00040404u : apex ? want[i] ^ 1u : game2[i]);
    }
    Check(carried, "physical directional maps: a kept solve's texels take Apex's last write again, never a texel Apex did not write or another solve wrote since");
    // a sibling room's solve copies the working set back: the texel holds this room's own solve value again, not Apex's last write
    {
        uint32_t t = 0x00303030u;
        const uint32_t own = 0x00303030u, lastW = 0x00505050u;
        const bool back = BasisWrite(t, lastW, 0x00404040u, own) && t == lastW;
        uint32_t other = 0x00313131u;
        Check(back && !BasisWrite(other, lastW, 0x00404040u, own) && other == 0x00313131u,
              "physical directional maps: a texel a sibling's solve put back to the room's own solve value is written again, another solve's value is not");
    }
}

// ---- Rooms open to the story above (features/open_rooms_above.h) ----
// Lot C49C001BCF2DEA20 (F8, 2026-11-02 14:30:47): stories 1, 2 and 3 as the F8 prints them, rooms | floor per tile ('.' room 0,
// a base-36 room id, 'L' a lamp registered there: its room is taken from the tiles around it; floor '0' never built, '-'
// removed, 'a' the bare 40000000, '#' placed, '*' mixed: counted as placed). One room per tile: the F8 prints quadrant 0's.
// Story 2's stairwell (40 removed quadrants) is the house of the 2026-09-29 test ("what an opening is").
const char* const kLotC49C[3][40] = {
    {
        "..............................|000000000000000000000000000000",
        "..............................|000000000000000000000000000000",
        "..............................|00000000000000000000aa00000000",
        "....................L.........|0000000000000000000a**a0000000",
        "....................88........|000000000000000000a*##*a000000",
        "...................8L88.......|00000000000000000a*####*a00000",
        "..................888888......|0000000000000000a*######*aaaa0",
        ".................8888888L.....|000000000000000a*###########a0",
        "................L8888L8L......|000000000000000a*###########a0",
        ".................88888L.......|000000000aaaaaaaa*##########a0",
        "..........L11333338L8L........|000000000a##################a0",
        ".........LL1133333L8L.........|000000000a##################a0",
        ".........LL113333333..........|000000000a##########aaaaaaaaa0",
        ".........LL1133L3333..........|000000000a##########a000000000",
        "..........1L133333L3..........|000000000a#########*a000000000",
        "..........11133L333L..........|000000aaaa########*a0000000000",
        ".......444L2222L22............|000000a###########a00000000000",
        "..........L222222LL...........|000000aa##########a00000000000",
        "..........L2222222............|0000000a##########aaaaaaa00000",
        ".........LL22L222LL...........|0000000a################a00000",
        ".........LL2222222.L..........|0000000a################a00000",
        "...........22222222L22........|00000000aaa#############a00000",
        "...........2222222222LL.......|0000000000a#############a00000",
        "...........2222222L222........|0000000000a#############a00000",
        "...........22222222222........|0000000000a#############a00000",
        "..........LL222222222LL.......|0000000000a#############a00000",
        "...........222222L2222........|0000000000a#############a00000",
        "..........LL2222222222........|0000000000a#############a00000",
        "...........2222222222LL.......|0000000000a#############a00000",
        "..........LL222L222222........|0000000000a#############a00000",
        "...........2222222222LL.......|0000000000a#############a00000",
        "...........2222222222LL.......|0000000000a#############a00000",
        "...........2222222222L........|0000000000a#############a00000",
        "...............2222222........|00000000000aaaa*########a00000",
        "................LL2222........|000000000000000a*####*aaa00000",
        ".................L2L2.........|0000000000000000a*##*a00000000",
        "..................22L.........|00000000000000000a**a000000000",
        "..............................|000000000000000000aa0000000000",
        "..............................|000000000000000000000000000000",
        "..............................|000000000000000000000000000000",
    },
    {
        "..............................|000000000000000000000000000000",
        "..............................|000000000000000000000000000000",
        "..............................|00000000000000000000aa00000000",
        "..............................|0000000000000000000aaaa0000000",
        "..............................|000000000000000000aaaaaa000000",
        "..............................|00000000000000000aaaaaaaa00000",
        "..............................|0000000000000000aaaaaaaaaa0000",
        ".......................L......|000000000000000aaaaaaaaaaa0000",
        "........................L.....|000000000000000aaaaaaaaaa00000",
        "..............................|000000000aaaaaaaaaaaaaaa000000",
        "..........555555Lc..L.........|000000000a########*aaaa0000000",
        ".........LL55555ccc..L........|000000000a#########*aa00000000",
        ".........LL55555cccLL.........|000000000a##########a000000000",
        ".........LL55555cccc..........|000000000a##########a000000000",
        "..........55555LccLc..........|000000000a#########*a000000000",
        "..........555555cLcL..........|000000aaaa########*a0000000000",
        "..........5555555L............|000000a###########a00000000000",
        "..........5555555LL...........|000000a###########a00000000000",
        "..........55555555............|000000a###########a00000000000",
        "..........55L5555LL...........|00000000aa########a00000000000",
        "..........555555L5............|000000000a########aaaaa0000000",
        "...........7777.L.............|000000000aa###########a0000000",
        "...........777LL..............|0000000000a###########a0000000",
        "...........777LL..............|0000000000a###########a0000000",
        "...........L777...............|0000000000a--#########a0000000",
        "..........LL77LL..............|0000000000a--#########a0000000",
        "...........7777...............|0000000000a--#########a0000000",
        "..........LL77LL..............|0000000000a--#########a0000000",
        "...........7777...............|0000000000a--#########a0000000",
        "..........LL77LL..............|0000000000a###########a0000000",
        "...........7777666L...........|0000000000a###########a0000000",
        "...........77776666L..........|0000000000a###########a0000000",
        "...........7LL766666L.........|0000000000a###########a0000000",
        "...............666666L........|00000000000aaaa*######a0000000",
        "................6666L6........|000000000000000a*####*a0000000",
        ".................6666L........|0000000000000000a*##*a00000000",
        "..................66..........|00000000000000000a**a000000000",
        "..............................|000000000000000000aa0000000000",
        "..............................|000000000000000000000000000000",
        "..............................|000000000000000000000000000000",
    },
    {
        "..............................|000000000000000000000000000000",
        "..............................|000000000000000000000000000000",
        "..............................|000000000000000000000000000000",
        "..............................|000000000000000000000000000000",
        "..............................|000000000000000000000000000000",
        "..............................|000000000000000000000000000000",
        "..............................|000000000000000000000000000000",
        "..............................|000000000000000000000000000000",
        "..............................|000000000000000000000000000000",
        "..............................|000000000000000aaaa00000000000",
        ".................a............|0000000000aa00aa###a0000000000",
        "..............................|0000000000aa00aa####a000000000",
        ".................LL...........|0000000000aaaaaa####a000000000",
        "..............................|0000000000aaaaaa####a000000000",
        ".................LL...........|0000000000aaaaaa###*a000000000",
        "..............................|0000000000aaaaaa##*a0000000000",
        "..............................|0000000000aa00aaaaa00000000000",
        ".................LL...........|0000000000a000aaaaa00000000000",
        "..............................|0000000000000aaaaaa00000000000",
        ".................LL...........|000000000000aaaaaaa00000000000",
        "..............................|0000000000aaaaaaaaa00000000000",
        "..............................|0000000000aaaaa000000000000000",
        "..............................|0000000000aaaaa000000000000000",
        "..............................|0000000000aaaaa000000000000000",
        "..............................|0000000000aaaaa000000000000000",
        "..............LL..............|0000000000aaaaaa00000000000000",
        "..............................|0000000000aaaaaa00000000000000",
        "..............LL..............|0000000000aaaaaa00000000000000",
        "..............................|0000000000aaaaaa00000000000000",
        "..............LL..............|0000000000aaaaaaaaa00000000000",
        "..................L...........|0000000000aaaaa##*aa0000000000",
        "...................L..........|0000000000aaaaa#*aaaa000000000",
        "....................L.........|0000000000aaaaa*aaaaaa00000000",
        ".....................L........|00000000000000aaaaaaaaa0000000",
        "..............................|0000000000000000aaaaaaa0000000",
        "..............................|00000000000000000aaaaa00000000",
        "..............................|00000000000000000aaaa000000000",
        "..............................|000000000000000000aa0000000000",
        "..............................|000000000000000000000000000000",
        "..............................|000000000000000000000000000000",
    },
};
struct StoryCells {
    std::vector<int> room;  // per tile
    std::vector<char> floor; // per tile
};
StoryCells ReadStory(const char* const* rows) {
    StoryCells s;
    s.room.assign(40 * 30, 0), s.floor.assign(40 * 30, '0');
    const auto id = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : 0; };
    for (int z = 0; z < 40; z++)
        for (int x = 0; x < 30; x++) s.room[z * 30 + x] = rows[z][x] == 'L' ? -1 : id(rows[z][x]), s.floor[z * 30 + x] = rows[z][31 + x];
    for (int z = 0; z < 40; z++) // a lamp's tile: the room most of its 8 neighbours have
        for (int x = 0; x < 30; x++) {
            if (s.room[z * 30 + x] >= 0) continue;
            int votes[64] = {}, best = 0;
            for (int dz = -1; dz <= 1; dz++)
                for (int dx = -1; dx <= 1; dx++)
                    if (x + dx >= 0 && x + dx < 30 && z + dz >= 0 && z + dz < 40 && rows[z + dz][x + dx] != 'L') votes[id(rows[z + dz][x + dx])]++;
            for (int r = 1; r < 64; r++) best = votes[r] > votes[best] ? r : best;
            s.room[z * 30 + x] = best;
        }
    return s;
}
// story B over story B - 1: the rooms open to above (walls of story B assumed on every edge: the F8 maps have none), their
// floorless quadrants, and the openings of ReadOpenings (a removed floor over an indoor room)
void OpenOver(const StoryCells& below, const StoryCells& b, std::vector<int>& open, int& quads, int& removed) {
    const auto placed = [&](int x, int z, int) { const char f = b.floor[z * 30 + x]; return f == '#' || f == 'F' || f == '*'; };
    OpenAbove::Classify(30, 40, [&](int x, int z, int) { return below.room[z * 30 + x]; }, placed, [](int, int) { return true; }, [](int, int) { return true; }, open);
    quads = removed = 0;
    for (int i = 0; i < 40 * 30; i++) {
        quads += !placed(i % 30, i / 30, 0) && std::binary_search(open.begin(), open.end(), below.room[i]) ? 4 : 0;
        removed += below.room[i] > 0 && b.floor[i] == '-' ? 4 : 0;
    }
}
int OpenQuadsOf(const StoryCells& below, const StoryCells& b, int room) {
    int n = 0;
    for (int i = 0; i < 40 * 30; i++) n += below.room[i] == room && !(b.floor[i] == '#' || b.floor[i] == 'F' || b.floor[i] == '*') ? 4 : 0;
    return n;
}
// The ramp terms of texels a lamp saturated, filled from the map's well-split texels (GpuLampLaw::FitSplit / IndoorSplitOrFill)
void SplitFillChecks() {
    const float k = 1.0f, amb[3] = {0.21f, 0.18f, 0.15f};
    const auto rampAt = [](float y) { return 0.6f + 0.27f * y; };
    std::vector<std::array<float, 5>> good;
    for (int i = 0; i < 40; i++) {
        const float y = 0.1f * static_cast<float>(i % 20), x[3] = {0.05f, 0.05f, 0.04f}, r = rampAt(y), ra[3] = {r * amb[0], r * amb[1], r * amb[2]};
        float p[4], gotRa[3], got = 0.0f;
        GpuLampLaw::IndoorTexel(x, ra, r, k, p);
        if (GpuLampLaw::IndoorSplit(x, p, k, gotRa, got)) good.push_back({y, got, gotRa[0], gotRa[1], gotRa[2]});
    }
    const GpuLampLaw::SplitFill fill = GpuLampLaw::FitSplit(good);
    // a saturated texel (weight 0 at its solve's sum): the split fails, the fill gives the true terms
    const float hot[3] = {3.0f, 3.0f, 3.0f}, y = 0.55f, r = rampAt(y), ra[3] = {r * amb[0], r * amb[1], r * amb[2]};
    float p[4], outRa[3], out = 0.0f, splitRa[3], split = 0.0f;
    GpuLampLaw::IndoorTexel(hot, ra, r, k, p);
    const bool failed = !GpuLampLaw::IndoorSplit(hot, p, k, splitRa, split);
    const bool filled = GpuLampLaw::IndoorSplitOrFill(hot, p, k, y, fill, outRa, out);
    bool near = std::fabs(out - r) < 1e-3f;
    for (int q = 0; q < 3; q++) near = near && std::fabs(outRa[q] - ra[q]) < 1e-3f;
    // its solve's own texel is unchanged: weight 0 there
    float again[4];
    GpuLampLaw::IndoorTexel(hot, outRa, out, k, again);
    bool same = true;
    for (int q = 0; q < 4; q++) same = same && std::fabs(again[q] - p[q]) < 1e-5f;
    // a texel that splits keeps its own read-back
    const float dim[3] = {0.1f, 0.08f, 0.07f};
    GpuLampLaw::IndoorTexel(dim, ra, r, k, p);
    float a2[3], r2 = 0.0f;
    const bool own = GpuLampLaw::IndoorSplitOrFill(dim, p, k, y, fill, a2, r2) && std::fabs(r2 - r) < 1e-4f;
    std::printf("  saturated texels' ramp filled: fit %s, ramp %.4f (true %.4f)\n", fill.ok ? "ok" : "none", out, r);
    Check(fill.ok && failed && filled && near && same && own,
          "physical kept maps: a texel the solve's lamps saturated takes the map's fitted ramp and ambient, its solve's texel unchanged");
}
void OpenAboveChecks() {
    const StoryCells s1 = ReadStory(kLotC49C[0]), s2 = ReadStory(kLotC49C[1]), s3 = ReadStory(kLotC49C[2]);
    std::vector<int> open2, open3;
    int quads2 = 0, removed2 = 0, quads3 = 0, removed3 = 0;
    OpenOver(s1, s2, open2, quads2, removed2);
    OpenOver(s2, s3, open3, quads3, removed3);
    const auto has = [](const std::vector<int>& v, int r) { return std::binary_search(v.begin(), v.end(), r); };
    std::string o2, o3;
    for (int r : open2) o2 += " " + std::to_string(r);
    for (int r : open3) o3 += " " + std::to_string(r);
    std::printf("open to above, lot C49C: story 2 over story 1: rooms%s, %d floorless quadrants (removed-floor openings %d); story 3 over story 2: "
                "rooms%s, %d quadrants (room 5: %d, room 7: %d; removed %d)\n",
                o2.c_str(), quads2, removed2, o3.c_str(), quads3, OpenQuadsOf(s2, s3, 5), OpenQuadsOf(s2, s3, 7), removed3);
    Check(removed2 == 40 && removed3 == 0, "open to above: the removed-floor openings (ReadOpenings) are unchanged: 40 on story 2, none on story 3");
    Check(has(open3, 5) && has(open3, 7), "open to above: story 2's rooms 5 and 7 (the red lamps) are open to story 3");
    Check(OpenQuadsOf(s2, s3, 5) >= 230 && OpenQuadsOf(s2, s3, 5) <= 320, "open to above: 230-320 floorless quadrants over room 5 (whole tiles here: the F8 map has one room per tile)");
    Check(!has(open3, 12) && !has(open2, 1) && !has(open2, 2) && !has(open2, 3) && !has(open2, 4),
          "open to above: no room with a floor placed over it (story 2's room 12, story 1's rooms 1 to 4: their ceiling's bare ring stays closed)");
    // the walls: a 3 x 3 room (1) at x 1..3, z 1..3 of a 6 x 6 grid with no floor over it, and a 3 x 3 neighbour (2) at x 4..5
    const auto room = [](int x, int z, int) { return z >= 1 && z <= 3 ? x >= 1 && x <= 3 ? 1 : x >= 4 ? 2 : 0 : 0; };
    const auto bare = [](int, int, int) { return false; };
    std::vector<int> open;
    OpenAbove::Classify(6, 6, room, bare, [](int, int) { return true; }, [](int, int) { return true; }, open);
    Check(open == std::vector<int>{1, 2}, "open to above: rooms walled all round on the story above are open");
    Check(OpenAbove::SharedFromBelow(3) && OpenAbove::SharedFromBelow(5) && OpenAbove::SharedFromBelow(6) && !OpenAbove::SharedFromBelow(7) &&
              !OpenAbove::SharedFromBelow(8),
          "open to above: the room 0 above takes the open rooms' lamps, not their window lights (types 7 and 8)");
    OpenAbove::Classify(6, 6, room, bare, [](int, int Z) { return Z == 1; }, [](int, int) { return false; }, open);
    Check(open.empty(), "open to above: a single-storey room under its roof (a wall above along one side only) stays closed");
    // walls round both, none between them: one space two rooms wide
    OpenAbove::Classify(6, 6, room, bare, [](int, int Z) { return Z == 1 || Z == 4; }, [](int X, int) { return X == 1 || X == 6; }, open);
    Check(open == std::vector<int>{1, 2}, "open to above: two rooms open to above with no wall between them on the story above");
    // room 1 floored above but for a row along one wall (6 of its 9 tiles): it keeps its ceiling
    OpenAbove::Classify(6, 6, room, [](int x, int z, int) { return x <= 3 && z >= 2; }, [](int, int) { return true; }, [](int, int) { return true; }, open);
    Check(open == std::vector<int>{2}, "open to above: a room floored above but along one wall keeps its ceiling");
    // a room on half a tile cut by a diagonal (its quadrants 2 and 3 of tile (1, 1), diagonal 0 parting them from 0 and 1), no floor
    // over it and the story above's wall only along that diagonal (13-50-15: lot C49C's room 6 runs along diagonals): open with
    // the diagonal counted as its own wall, closed when only the tile sides are read
    {
        const auto half = [](int x, int z, int q) { return x == 1 && z == 1 && (q == 2 || q == 3) ? 1 : 0; };
        const auto none2 = [](int, int) { return false; };
        OpenAbove::Classify(4, 4, half, bare, none2, none2, [](int x, int z, int i) { return x == 1 && z == 1 && i == 0; }, open, true);
        const bool withDiag = open == std::vector<int>{1};
        OpenAbove::Classify(4, 4, half, bare, none2, none2, open, true);
        Check(withDiag && open.empty(), "open to above: a diagonal wall of the story above on the room's outline counts as its own wall");
    }
}

// ---- A TubeLight's law (gpu_lamp_law.h Tube) against the game's code: 0x006BFA70 over 0x006BD100 as dumpbin gives them (Steam
// 1.67.2, read 02/11), instruction by instruction with the same SSE approximations (rsqrtps + 2 Newton steps, rcpps + 1) ----
namespace GameTube {
inline float Rsqrt2(float d2) { // 0x006BD181 .. 0x006BD1D9
    const float y0 = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(d2)));
    const float y1 = (1.0f - y0 * y0 * d2) * (0.5f * y0) + y0;
    return (1.0f - y1 * y1 * d2) * (0.5f * y1) + y1;
}
// the light at p (normal n, w 0) of a tube from A to B of length len (+0x1A0), range x intensity ri, colour E
void Eval(const float A[3], const float B[3], float len, float ri, const float E[3], float k1, float k2, float cmax, const float p[3], const float n[3], float out[3]) {
    const float s[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, v[3] = {p[0] - A[0], p[1] - A[1], p[2] - A[2]};
    const float t = (s[2] * v[2] + s[1] * v[1]) + s[0] * v[0];
    float e[3];
    if (0.0f > t) std::memcpy(e, v, sizeof e);
    else if (t > len * len)
        for (int i = 0; i < 3; i++) e[i] = p[i] - B[i];
    else {
        const float ss = (s[2] * s[2] + s[1] * s[1]) + s[0] * s[0];
        for (int i = 0; i < 3; i++) e[i] = v[i] - t * s[i] / ss;
    }
    const float d2 = (e[2] * e[2] + e[1] * e[1]) + e[0] * e[0];
    const float y = Rsqrt2(d2), d = d2 == 0.0f ? 0.0f : y * d2;
    const float x = d * d * k1, r = _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x))), inv = (2.0f - x * r) * r;
    float val = k2 * (ri * inv);
    if (val > cmax) val = cmax;
    float dir[3];
    for (int i = 0; i < 3; i++) dir[i] = y * e[i];
    const float dot = ((0.0f - dir[2]) * n[2] + (0.0f - dir[1]) * n[1]) + (0.0f - dir[0]) * n[0];
    const float f = std::max(dot * 1.0f + 0.0f, 0.0f);
    for (int i = 0; i < 3; i++) out[i] = f * (val * E[i]);
}
} // namespace GameTube
void TubeChecks() {
    std::mt19937 rng(606);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    const float k1 = 1.0f, k2 = 0.075f, cmax = 2.0f;
    long n = 0, within = 0, lit = 0;
    float worst = 0.0f;
    for (int lamp = 0; lamp < 400; lamp++) {
        const float A[3] = {10.0f * U(rng), 1.0f + 2.0f * U(rng), 10.0f * U(rng)};
        float dirv[3] = {U(rng) - 0.5f, U(rng) - 0.5f, U(rng) - 0.5f};
        const float dl = std::sqrt(dirv[0] * dirv[0] + dirv[1] * dirv[1] + dirv[2] * dirv[2]) + 1e-6f, size = lamp % 4 ? 0.3f + 1.7f * U(rng) : 1.0f;
        const float B[3] = {A[0] + dirv[0] / dl * size, A[1] + dirv[1] / dl * size, A[2] + dirv[2] / dl * size};
        const float len = size * (lamp % 5 == 0 ? 0.9f : 1.0f), ri = (10.0f + 90.0f * U(rng)) * (0.5f + 1.5f * U(rng));
        const float E[3] = {U(rng), U(rng), U(rng)};
        GpuLampLaw::Lamp L;
        std::memcpy(L.pos, A, sizeof A);
        L.W = k2 * ri / k1, L.cmax = cmax;
        std::memcpy(L.colour, E, sizeof E);
        const float s[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]};
        GpuLampLaw::TubeShape(L, s, len);
        for (int j = 0; j < 500; j++) {
            const float p[3] = {A[0] + 8.0f * (U(rng) - 0.5f), A[1] + 4.0f * (U(rng) - 0.5f), A[2] + 8.0f * (U(rng) - 0.5f)};
            float nv[3] = {U(rng) - 0.5f, U(rng) - 0.5f, U(rng) - 0.5f};
            const float nl = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]) + 1e-6f;
            for (float& c : nv) c /= nl;
            float game[3], law[3];
            GameTube::Eval(A, B, len, ri, E, k1, k2, cmax, p, nv, game);
            GpuLampLaw::Eval(L, p, nv, 0.0f, law);
            if (!std::isfinite(game[0]) || game[0] + game[1] + game[2] > 1e30f) continue; // (a point on the segment)
            const float dmax = std::max({std::fabs(game[0] - law[0]), std::fabs(game[1] - law[1]), std::fabs(game[2] - law[2])});
            const float rel = dmax / std::max({game[0], game[1], game[2], 1e-3f});
            n++, lit += game[0] + game[1] + game[2] > 0.0f;
            within += dmax <= 2.0f / 255.0f && rel < 1e-3f;
            worst = std::max(worst, rel);
        }
    }
    std::printf("tube law vs the game's code: %ld points (%ld lit), %ld within 2/255 and 1e-3 relative, worst relative %.2g\n", n, lit, within, worst);
    Check(n > 150000 && lit > n / 4 && within == n, "TubeLight: the law is the game's evaluation (0x006BFA70) at every point");
    // the shaders read it as a tube from the off row's y (gOff.y), a move of its end alone is a move, its law is the game's (A x 1)
    GpuLampLaw::Lamp a;
    const float s[3] = {0.0f, 1.0f, 0.0f}, s2[3] = {0.0f, 0.0f, 1.0f}, p[3] = {0.1f, 0.0f, 0.0f}, wall[3] = {-1.0f, 0.0f, 0.0f};
    GpuLampLaw::TubeShape(a, s, 1.0f);
    GpuLampLaw::Lamp b = a;
    float rows[5][4];
    GpuLampLaw::Rows(a, rows);
    GpuLampLaw::TubeShape(b, s2, 1.0f);
    GpuLampLaw::Lamp bulb;
    Check(rows[3][1] == GpuLampLaw::kTube && rows[2][3] == 1.0f && GpuLampLaw::IsTube(a) && !GpuLampLaw::IsTube(bulb), "TubeLight: its rows carry the tube mark and len^2");
    Check(GpuLampLaw::Moved(a, b, 1e-4f) && !GpuLampLaw::Moved(a, a, 1e-4f) && GpuLampLaw::Moved(a, bulb, 1e-4f), "TubeLight: a turn about its position is a move");
    Check(RoomLampMove::GameOverLaw(a, p, wall) == 1.0f, "TubeLight: its law has no mount floor (GameOverLaw 1)");
}

// ---- The area lights' law (gpu_lamp_law.h Area) against the game's code: 0x006BFBA0 (rectangle), 0x006BFDC0 with 0x006BD7C0
// (disc), both over 0x006BFAF0, as dumpbin gives them (Steam 1.67.2, read 02/11): sqrtps for |D|, rcpps + 1 Newton step for
// 1 / (k1 d^2) and the disc's 1 / |xy|, rsqrtps + 2 for the facing ----
namespace GameArea {
// the light at p of an area at A with axes U (+0x170), V (+0x190), N (+0x180), e0 (+0x1A0), e1 (+0x1A4)
void Eval(bool disc, const float A[3], const float U[3], const float V[3], const float N[3], float e0, float e1, float ri, const float E[3], float k1, float k2, float cmax,
          const float p[3], float out[3]) {
    const float v[3] = {A[0] - p[0], A[1] - p[1], A[2] - p[2]};
    const float L[3] = {(U[2] * v[2] + U[1] * v[1]) + U[0] * v[0], (V[2] * v[2] + V[1] * v[1]) + V[0] * v[0], (N[2] * v[2] + N[1] * v[1]) + N[0] * v[0]};
    float C[3] = {std::min(std::max(L[0], -e0), e0), std::min(std::max(L[1], -e1), e1), std::min(std::max(L[2], 0.0f), 0.0f)};
    if (disc) { // 0x006BD7C0
        C[0] = L[0], C[1] = L[1], C[2] = 0.0f;
        const float s = (0.0f * 0.0f + L[1] * L[1]) + L[0] * L[0];
        if (s > e0) {
            const float sq = std::sqrt(s), r = _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(sq))), inv = (2.0f - r * sq) * r;
            for (int i = 0; i < 2; i++) C[i] = inv * L[i] * e1;
        }
    }
    const float D[3] = {L[0] - C[0], L[1] - C[1], L[2] - C[2]};
    const float d2 = (D[2] * D[2] + D[1] * D[1]) + D[0] * D[0], d = std::sqrt(d2);
    const float x = d * d * k1, r = _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x))), inv = (2.0f - x * r) * r;
    float val = k2 * (ri * 1.0f * inv);
    if (val > cmax) val = cmax;
    const float f = std::min(1.0f, std::max(0.0f, GameTube::Rsqrt2(d2) * D[2]));
    for (int i = 0; i < 3; i++) out[i] = val * (f * E[i]);
}
} // namespace GameArea
void AreaChecks() {
    std::mt19937 rng(910);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    const float k1 = 1.0f, k2 = 0.075f, cmax = 2.0f;
    long n[2] = {}, within[2] = {}, lit[2] = {}, near[2] = {};
    float worst[2] = {};
    const auto unit = [](float a[3]) {
        const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) + 1e-6f;
        for (int i = 0; i < 3; i++) a[i] /= l;
    };
    for (int lamp = 0; lamp < 800; lamp++) {
        const bool disc = lamp & 1;
        const float A[3] = {10.0f * U(rng), 1.0f + 2.0f * U(rng), 10.0f * U(rng)};
        // (a frame turned at random; every third one scaled and skewed, as an object's matrix could leave it)
        float N[3] = {U(rng) - 0.5f, U(rng) - 0.5f, U(rng) - 0.5f}, Ua[3] = {U(rng) - 0.5f, U(rng) - 0.5f, U(rng) - 0.5f};
        if (lamp % 4 == 0) N[0] = 0.0f, N[1] = 1.0f, N[2] = 0.0f, Ua[0] = 1.0f, Ua[1] = 0.0f, Ua[2] = 0.0f; // (a ceiling panel)
        unit(N);
        const float dn = Ua[0] * N[0] + Ua[1] * N[1] + Ua[2] * N[2];
        for (int i = 0; i < 3; i++) Ua[i] -= dn * N[i];
        unit(Ua);
        float Va[3] = {N[1] * Ua[2] - N[2] * Ua[1], N[2] * Ua[0] - N[0] * Ua[2], N[0] * Ua[1] - N[1] * Ua[0]};
        if (lamp % 3 == 0)
            for (int i = 0; i < 3; i++) Ua[i] *= 1.3f, Va[i] = Va[i] * 0.8f + 0.1f * N[i];
        const float r = 0.1f + 1.4f * U(rng);
        const float e0 = disc ? r * r * (lamp % 5 == 1 ? 0.8f : 1.0f) : 0.1f + 1.4f * U(rng), e1 = disc ? r : 0.1f + 1.4f * U(rng);
        const float ri = (10.0f + 90.0f * U(rng)) * (0.5f + 1.5f * U(rng)), E[3] = {U(rng), U(rng), U(rng)};
        GpuLampLaw::Lamp L;
        std::memcpy(L.pos, A, sizeof A);
        L.W = k2 * ri / k1, L.cmax = cmax;
        std::memcpy(L.colour, E, sizeof E);
        GpuLampLaw::AreaShape(L, Ua, Va, N, e0, e1, disc);
        for (int j = 0; j < 500; j++) {
            float p[3] = {A[0] + 8.0f * (U(rng) - 0.5f), A[1] + 4.0f * (U(rng) - 0.5f), A[2] + 8.0f * (U(rng) - 0.5f)};
            if (j % 5 == 0) { // (a point 1 mm to 5 cm off the area's plane, on either side: a ceiling texel at the panel)
                const float off = (j % 10 ? 1.0f : -1.0f) * (0.001f + 0.05f * U(rng)), a = 2.0f * (U(rng) - 0.5f), b = 2.0f * (U(rng) - 0.5f);
                for (int i = 0; i < 3; i++) p[i] = A[i] - off * N[i] + a * e1 * Ua[i] + b * e1 * Va[i];
            }
            float game[3], law[3];
            GameArea::Eval(disc, A, Ua, Va, N, e0, e1, ri, E, k1, k2, cmax, p, game);
            GpuLampLaw::Eval(L, p, N, 0.0f, law);
            if (!std::isfinite(game[0]) || game[0] + game[1] + game[2] > 1e30f) continue; // (a point on the area)
            const float dmax = std::max({std::fabs(game[0] - law[0]), std::fabs(game[1] - law[1]), std::fabs(game[2] - law[2])});
            const float rel = dmax / std::max({game[0], game[1], game[2], 1e-3f});
            n[disc]++, lit[disc] += game[0] + game[1] + game[2] > 0.0f, near[disc] += j % 5 == 0;
            within[disc] += dmax <= 2.0f / 255.0f && rel < 1e-3f;
            worst[disc] = std::max(worst[disc], rel);
        }
    }
    for (int d = 0; d < 2; d++) {
        std::printf("%s law vs the game's code: %ld points (%ld lit, %ld by its plane), %ld within 2/255 and 1e-3 relative, worst relative %.2g\n",
                    d ? "disc area" : "rectangle area", n[d], lit[d], near[d], within[d], worst[d]);
        Check(n[d] > 150000 && lit[d] > n[d] / 4 && within[d] == n[d],
              d ? "DiscAreaLight: the law is the game's evaluation (0x006BFDC0) at every point" : "RectangleAreaLight: the law is the game's evaluation (0x006BFBA0) at every point");
    }
    // at |D| = 0 (on the area: the game's NaN) the law gives 0; the rows carry the mark, U, V, N and the size; a turn or a resize is
    // a move; its law is the game's (A x 1)
    GpuLampLaw::Lamp a;
    const float u[3] = {1.0f, 0.0f, 0.0f}, v[3] = {0.0f, 0.0f, 1.0f}, nu[3] = {0.0f, 1.0f, 0.0f}, u2[3] = {0.0f, 0.0f, 1.0f}, v2[3] = {-1.0f, 0.0f, 0.0f};
    a.W = 5.0f, a.colour[0] = a.colour[1] = a.colour[2] = 1.0f;
    GpuLampLaw::AreaShape(a, u, v, nu, 0.5f, 0.3f, false);
    float on[3];
    const float at[3] = {0.2f, 0.0f, 0.1f}, wall[3] = {-1.0f, 0.0f, 0.0f};
    GpuLampLaw::Eval(a, at, nu, 0.0f, on);
    float rows[5][4];
    GpuLampLaw::Rows(a, rows);
    GpuLampLaw::Lamp turned = a, bigger = a, disc = a, tube;
    GpuLampLaw::AreaShape(turned, u2, v2, nu, 0.5f, 0.3f, false);
    GpuLampLaw::AreaShape(bigger, u, v, nu, 0.6f, 0.3f, false);
    GpuLampLaw::AreaShape(disc, u, v, nu, 0.25f, 0.5f, true);
    GpuLampLaw::TubeShape(tube, u, 1.0f);
    Check(on[0] == 0.0f && on[1] == 0.0f && on[2] == 0.0f, "area light: a point on the area (the game's NaN) takes 0");
    Check(rows[3][1] == GpuLampLaw::kRect && rows[2][0] == 1.0f && rows[4][2] == 1.0f && rows[3][2] == 0.0f && rows[2][3] == 0.0f && rows[3][0] == 1.0f && rows[3][3] == 0.5f &&
              rows[4][3] == 0.3f && GpuLampLaw::Kind(a) == 2 && GpuLampLaw::Kind(disc) == 3 && GpuLampLaw::Kind(tube) == 1 && !GpuLampLaw::IsTube(a),
          "area light: its rows carry the mark, U, V, N and its size");
    Check(GpuLampLaw::Moved(a, turned, 1e-4f) && GpuLampLaw::Moved(a, bigger, 1e-4f) && GpuLampLaw::Moved(a, disc, 1e-4f) && !GpuLampLaw::Moved(a, a, 1e-4f) &&
              !GpuLampLaw::Moved(tube, tube, 1e-4f),
          "area light: a turn or a resize about its position is a move");
    Check(RoomLampMove::GameOverLaw(a, at, wall) == 1.0f && RoomLampMove::GameOverLaw(disc, at, wall) == 1.0f, "area light: its law has no mount floor (GameOverLaw 1)");
}

// ---- The physical model's occluder proxy (RtLampProxy::Build ceilingsOpen, OpenTops) over the F8 maps of lot C49C: story 2's
// rooms (all roofed: the snapshot's room +0x18 is 0 for each) under story 3, its walls on every tile side ----
void ProxyOpenTopsChecks() {
    const StoryCells s2 = ReadStory(kLotC49C[1]), s3 = ReadStory(kLotC49C[2]);
    const auto isPlaced = [&](size_t t) { const char f = s3.floor[t]; return f == '#' || f == 'F' || f == '*'; };
    RtLampProxy::Lot lot;
    RtLampProxy::Story a, b;
    a.story = 2, b.story = 3;
    a.w = b.w = 30, a.h = b.h = 40;
    const size_t nt = 30 * 40;
    a.floorY.assign(nt, RtLampProxy::kNoFloor), a.ceil.assign(nt, RtLampProxy::kNoFloor), a.roofed.assign(nt, 0), a.room.assign(4 * nt, 0);
    b.floorY.assign(nt, RtLampProxy::kNoFloor), b.ceil.assign(nt, RtLampProxy::kNoFloor), b.roofed.assign(nt, 0), b.placed.assign(nt, 0);
    a.plane = 3.99f, b.plane = 6.99f;
    for (size_t t = 0; t < nt; t++) {
        if (s2.room[t] > 0) {
            a.floorY[t] = 3.99f, a.ceil[t] = 6.99f, a.roofed[t] = 0xF;
            for (int q = 0; q < 4; q++) a.room[4 * t + q] = s2.room[t];
        }
        if (isPlaced(t)) b.placed[t] = 0xF;
    }
    const auto edge = [&](float x0, float z0, float x1, float z1) {
        RtLampProxy::Edge e;
        e.x0 = x0, e.z0 = z0, e.x1 = x1, e.z1 = z1, e.foot = e.base = 6.99f, e.topL = e.topM = e.topR = 9.99f;
        b.edges.push_back(e);
    };
    for (int Z = 0; Z <= 40; Z++)
        for (int x = 0; x < 30; x++) edge(static_cast<float>(x), static_cast<float>(Z), static_cast<float>(x + 1), static_cast<float>(Z));
    for (int z = 0; z < 40; z++)
        for (int X = 0; X <= 30; X++) edge(static_cast<float>(X), static_cast<float>(z), static_cast<float>(X), static_cast<float>(z + 1));
    lot.stories = {a, b};
    RtLampProxy::Mesh before, after, noIds;
    RtLampProxy::Build(lot, before, false);
    RtLampProxy::Build(lot, after, true);
    // roof triangles over each room of story 2 (the tile under the triangle's centroid)
    const auto roofsOver = [&](const RtLampProxy::Mesh& m, int room) {
        int n = 0;
        for (size_t i = 0; i < m.tris.size(); i++) {
            if (m.kind[i] != RtLampProxy::Mesh::kRoof) continue;
            const RtLampProxy::Tri& t = m.tris[i];
            const int x = static_cast<int>(std::floor((t.v[0][0] + t.v[1][0] + t.v[2][0]) / 3.0f));
            const int z = static_cast<int>(std::floor((t.v[0][2] + t.v[1][2] + t.v[2][2]) / 3.0f));
            n += s2.room[z * 30 + x] == room ? 1 : 0;
        }
        return n;
    };
    const auto bareQuads = [&](int room) {
        int n = 0;
        for (size_t t = 0; t < nt; t++) n += s2.room[t] == room && !isPlaced(t) ? 4 : 0;
        return n;
    };
    const auto tilesOf = [&](int room) { return static_cast<int>(std::count(s2.room.begin(), s2.room.end(), room)); };
    std::vector<int> open;
    int quads = 0, removed = 0;
    OpenOver(s2, s3, open, quads, removed);
    std::printf("physical proxy, lot C49C story 2 under story 3: roof triangles over rooms 5 / 6 / 7 / 12 today %d / %d / %d / %d, with open tops %d / %d / %d / %d; "
                "quadrants opened %d (open to above %d)\n",
                roofsOver(before, 5), roofsOver(before, 6), roofsOver(before, 7), roofsOver(before, 12), roofsOver(after, 5), roofsOver(after, 6), roofsOver(after, 7),
                roofsOver(after, 12), after.openTops, quads);
    Check(tilesOf(5) > 0 && roofsOver(before, 5) == 4 * tilesOf(5) && roofsOver(before, 7) == 4 * tilesOf(7),
          "physical proxy: today a roofed ceiling at floor + 3 covers every quadrant of story 2's rooms 5 and 7 (open to story 3)");
    bool openOk = !open.empty();
    for (int r = 1; r < 64; r++) {
        const bool isOpen = std::binary_search(open.begin(), open.end(), r);
        openOk &= roofsOver(after, r) == 4 * tilesOf(r) - (isOpen ? bareQuads(r) : 0);
    }
    Check(openOk && after.openTops == quads && quads > 0, "physical proxy: ceilingsOpen drops the ceiling exactly over the floorless quadrants of rooms open to above");
    {
        std::vector<std::pair<int, int>> want;
        for (int r : open) want.emplace_back(a.story, r);
        Check(after.openRooms == want && before.openRooms.empty() && std::binary_search(want.begin(), want.end(), std::make_pair(a.story, 5)),
              "physical proxy: the open-top build lists the rooms it opened (story 2's room 5: its lamps light story 3's room 0 without the reach share)");
    }
    Check(tilesOf(12) > 0 && roofsOver(after, 12) == 4 * tilesOf(12), "physical proxy: a room with a floor placed over it (story 2's room 12) keeps its ceiling");
    // a ray from a lamp 1.5 m over room 5's floor up to 1 m over its ceiling line, inside one floorless tile
    int tile = -1;
    for (size_t t = 0; t < nt && tile < 0; t++)
        if (s2.room[t] == 5 && !isPlaced(t)) tile = static_cast<int>(t);
    const float x = static_cast<float>(tile % 30) + 0.3f, z = static_cast<float>(tile / 30) + 0.6f;
    const float L[3] = {x, 5.49f, z}, P[3] = {x + 0.4f, 7.99f, z - 0.3f};
    Check(tile >= 0 && !RtLampProxy::Clear(before, L, P) && RtLampProxy::Clear(after, L, P), "physical proxy: a lamp of room 5 reaches the walls of story 3 over it");
    // ceilingsOpen off (the outdoor lamps' build) or a story with no room ids: nothing opens
    lot.stories[0].room.clear();
    RtLampProxy::Build(lot, noIds, true);
    Check(noIds.openTops == 0 && noIds.roofTris == before.roofTris && before.openTops == 0 && before.roofTris == after.roofTris + quads,
          "physical proxy: the outdoor lamps' build (ceilingsOpen off) and a story with no room ids keep every ceiling");
    // the walls stories 2 and 3 really have (the probe's grids of 02/11 21:02, runs of tile edges: x, Z, length along x;
    // X, z, length along z): story 2's rooms 5, 6 and 7 stand under a sloped roof with story 3's walls on part of their
    // outlines only, and story 1's room 8 is a single-storey wing beside story 2's floored rooms
    {
        struct Run { int a, b, n; };
        const auto walls = [](std::vector<Run> z, std::vector<Run> x) {
            std::vector<uint8_t> wz(41 * 30, 0), wx(40 * 31, 0);
            for (const Run& r : z)
                for (int i = 0; i < r.n; i++) wz[r.b * 30 + r.a + i] = 1;
            for (const Run& r : x)
                for (int i = 0; i < r.n; i++) wx[(r.b + i) * 31 + r.a] = 1;
            return std::make_pair(wz, wx);
        };
        const auto w3 = walls({{10, 10, 8}, {17, 11, 1}, {10, 21, 8}, {11, 33, 4}}, {{15, 21, 12}, {17, 10, 1}, {18, 10, 11}});
        const auto w2 = walls({{10, 10, 8}, {18, 11, 1}, {19, 12, 1}, {16, 16, 2}, {7, 19, 3}, {10, 21, 8}, {15, 30, 3}, {11, 33, 4}},
                              {{7, 16, 3}, {10, 10, 11}, {11, 21, 12}, {15, 21, 12}, {16, 10, 6}, {18, 10, 1}, {18, 16, 5}, {19, 11, 1}, {20, 12, 2}});
        const StoryCells s1 = ReadStory(kLotC49C[0]);
        const auto over = [&](const StoryCells& lo, const StoryCells& up, const std::pair<std::vector<uint8_t>, std::vector<uint8_t>>& w, bool own) {
            std::vector<int> o;
            OpenAbove::Classify(
                30, 40, [&](int x, int z, int) { return lo.room[z * 30 + x]; },
                [&](int x, int z, int) { const char f = up.floor[z * 30 + x]; return f == '#' || f == 'F' || f == '*'; },
                [&](int x, int Z) { return w.first[Z * 30 + x] != 0; }, [&](int X, int z) { return w.second[z * 31 + X] != 0; }, o, own);
            return o;
        };
        const auto has = [](const std::vector<int>& v, int r) { return std::binary_search(v.begin(), v.end(), r); };
        const std::vector<int> wallsOnly = over(s2, s3, w3, false), own3 = over(s2, s3, w3, true), own2 = over(s1, s2, w2, true);
        // (room 6's walls of story 3 all stand on its floored part, as an upper room's outer wall flush over a single-storey
        // part would: without the roof's height the two cannot be told apart, so it stays closed, as the game draws it)
        Check(!has(wallsOnly, 5) && !has(wallsOnly, 7) && has(own3, 5) && !has(own3, 6) && has(own3, 7) && !has(own3, 12) && !has(own2, 8),
              "open to above with its own walls: story 2's rooms 5 and 7 under a sloped roof open (closed by the 3/4 walls rule with story 3's real walls); "
              "room 6, whose upper walls stand on its floored part, story 1's room 8, a wing beside floored rooms, and room 12 under a floor stay closed");
    }
    // story 2's removed floor (the '-' hole, 40 quadrants) over story 1's roofed room 2: its ceiling opens under the hole only,
    // with or without room ids
    {
        const StoryCells s1 = ReadStory(kLotC49C[0]);
        RtLampProxy::Story lo, up;
        lo.story = 1, up.story = 2, lo.w = up.w = 30, lo.h = up.h = 40;
        lo.floorY.assign(nt, RtLampProxy::kNoFloor), lo.ceil.assign(nt, RtLampProxy::kNoFloor), lo.roofed.assign(nt, 0);
        up.floorY.assign(nt, RtLampProxy::kNoFloor), up.ceil.assign(nt, RtLampProxy::kNoFloor), up.roofed.assign(nt, 0), up.placed.assign(nt, 0), up.removed.assign(nt, 0);
        int holes = 0, holeTile = -1;
        for (size_t t = 0; t < nt; t++) {
            if (s1.room[t] > 0) lo.floorY[t] = 0.99f, lo.ceil[t] = 3.99f, lo.roofed[t] = 0xF;
            if (s2.floor[t] == '#' || s2.floor[t] == 'F' || s2.floor[t] == '*') up.placed[t] = 0xF, up.floorY[t] = 3.99f;
            if (s2.floor[t] == '-') up.removed[t] = 0xF, holes += lo.roofed[t] ? 4 : 0, holeTile = holeTile < 0 && lo.roofed[t] ? static_cast<int>(t) : holeTile;
        }
        RtLampProxy::Lot two;
        two.stories = {lo, up};
        RtLampProxy::Mesh shut, opened;
        RtLampProxy::Build(two, shut, false);
        RtLampProxy::Build(two, opened, true);
        const float hx = static_cast<float>(holeTile % 30) + 0.5f, hz = static_cast<float>(holeTile / 30) + 0.5f;
        const float La[3] = {hx + 0.2f, 5.49f, hz}, Pb[3] = {hx - 0.2f, 1.49f, hz + 0.1f};
        Check(holes == 40 && opened.openTops == 40 && opened.roofTris == shut.roofTris - 40 && holeTile >= 0 && !RtLampProxy::Clear(shut, La, Pb) &&
                  RtLampProxy::Clear(opened, La, Pb),
              "physical proxy: a removed floor of the story above (40 quadrants) opens the roofed ceiling under it, and a lamp above lights the room below through it");
    }
}

// ---- The space rooms open to the story above continue into (RtLampProxy::InsideOf / InsideAt) over lot C49C's F8 maps and story
// 3's real walls (the probe's grids of 02/11, as ProxyOpenTopsChecks): story 2's rooms 5 and 7 open under the sloped roof, and the
// floored part of story 3 over room 12 beside them under the same roof (09/11: its walls and floor took the night sky) ----
// The room texel a texel of the space over an open room shows (RtLampProxy::TopMatch): a wall the room's top-row texel facing
// its way (not the nearer one behind the wall), a floor the nearest in plan; none without a top row
void CarryChecks() {
    struct Top {
        float pos[3], n[3];
    };
    const std::vector<Top> top = {{{0, 3, 0}, {1, 0, 0}}, {{0.2f, 3, 0}, {-1, 0, 0}}, {{5, 3, 0}, {1, 0, 0}}};
    const float p[3] = {0.1f, 5, 0}, p2[3] = {0.19f, 5, 0}, nx[3] = {1, 0, 0};
    Check(RtLampProxy::TopMatch(top, p, nx, true) == &top[0] && RtLampProxy::TopMatch(top, p2, nx, true) == &top[0],
          "match: a wall takes the room's top-row texel facing its way (not the nearer one behind the wall)");
    Check(RtLampProxy::TopMatch(top, p2, nx, false) == &top[1], "match: a floor takes the nearest top-row texel in plan");
    Check(!RtLampProxy::TopMatch(std::vector<Top>{}, p, nx, true), "match: no top row, no match (the texel keeps its own solve)");
}
void InsideChecks() {
    const StoryCells s2 = ReadStory(kLotC49C[1]), s3 = ReadStory(kLotC49C[2]);
    const size_t nt = 30 * 40;
    RtLampProxy::Story a, b;
    a.story = 2, b.story = 3, a.w = b.w = 30, a.h = b.h = 40;
    a.roofed.assign(nt, 0), a.room.assign(4 * nt, 0), b.placed.assign(nt, 0);
    for (size_t t = 0; t < nt; t++) {
        if (s2.room[t] > 0) {
            a.roofed[t] = 0xF;
            for (int q = 0; q < 4; q++) a.room[4 * t + q] = s2.room[t];
        }
        const char f = s3.floor[t];
        if (f == '#' || f == 'F' || f == '*') b.placed[t] = 0xF;
    }
    struct Run { int a, b, n; };
    const auto edge = [&](float x0, float z0, float x1, float z1) {
        RtLampProxy::Edge e;
        e.x0 = x0, e.z0 = z0, e.x1 = x1, e.z1 = z1;
        b.edges.push_back(e);
    };
    for (const Run& r : std::vector<Run>{{10, 10, 8}, {17, 11, 1}, {10, 21, 8}, {11, 33, 4}})
        for (int i = 0; i < r.n; i++) edge(static_cast<float>(r.a + i), static_cast<float>(r.b), static_cast<float>(r.a + i + 1), static_cast<float>(r.b));
    for (const Run& r : std::vector<Run>{{15, 21, 12}, {17, 10, 1}, {18, 10, 11}})
        for (int i = 0; i < r.n; i++) edge(static_cast<float>(r.a), static_cast<float>(r.b + i), static_cast<float>(r.a), static_cast<float>(r.b + i + 1));
    RtLampProxy::Inside in;
    const bool built = RtLampProxy::InsideOf(a, b, {5, 7}, in);
    int seeds = 0, spread = 0, over12 = 0, beyond = 0, yard = 0;
    for (size_t t = 0; t < nt; t++)
        for (int q = 0; q < 4; q++) {
            const int r = built ? in.room[4 * t + q] : 0;
            if (!r) continue;
            const bool floored = b.placed[t] != 0;
            (floored ? spread : seeds)++;
            over12 += s2.room[t] == 12;
            beyond += static_cast<int>(t % 30) >= 18 && s2.room[t] == 12;
            yard += s2.room[t] == 0;
        }
    std::printf("inside, lot C49C story 3: %d quadrants over the open rooms, %d more on its floors beside them (%d over room 12, %d past story 3's wall at x 18), "
                "%d over no roofed room\n", seeds, spread, over12, beyond, yard);
    Check(built && seeds > 0 && over12 > 0 && beyond == 0 && yard == 0,
          "inside: the attic over rooms 5 and 7 and its floored part over room 12 up to story 3's wall, never past a wall or over the yard");
    // a wall face of story 3 at x 16 facing -x (into the open space over room 5) and +x (onto the floored part), and one at x 18 facing
    // +x past that wall; a floor texel on the floored part
    const float Pw[3] = {16.0f, 7.5f, 12.5f}, Nm[3] = {-1, 0, 0}, Np[3] = {1, 0, 0}, Pb[3] = {18.0f, 7.5f, 12.5f}, Pf[3] = {16.5f, 6.99f, 12.5f}, Nu[3] = {0, 1, 0};
    std::vector<RtLampProxy::Inside> all{in};
    Check(RtLampProxy::InsideAt(all, 3, Pw, Nm) == 5 && RtLampProxy::InsideAt(all, 3, Pw, Np) == 5 && RtLampProxy::InsideAt(all, 3, Pb, Np) == 0 &&
              RtLampProxy::InsideAt(all, 3, Pf, Nu) == 5 && RtLampProxy::InsideAt(all, 2, Pw, Nm) == 0,
          "inside: a wall facing into the space and the floor under it take room 5; a face past story 3's wall and another story none");
    // nothing open: nothing inside
    Check(!RtLampProxy::InsideOf(a, b, {}, in), "inside: no room open to above, no space");
}

// ---- The furniture blocker table (rt_lamp_blockers.h): the rows give back the game's blockers, and occlude from the rows is
// RoomLightTests::occlude bit for bit ----
// Physical indoor lamps, stage 4 (phys_lamp_pick.h): the lamps a kept map draws (reach over its box, never one counted twice),
// a fresh lamp's row, and when a lamp edit marks no room
void PhysPickChecks() {
    using namespace PhysLampPick;
    std::mt19937 rng(404);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    const float thr = 3.0f / 255.0f;
    // the reach: beyond it the law (any normal, any shape) is under the threshold; Meets is the box's distance against it
    {
        bool beyond = true, nearDrawn = true;
        int tested = 0;
        for (int i = 0; i < 20000; i++) {
            GpuLampLaw::Lamp L;
            L.pos[0] = 10.0f * U(rng), L.pos[1] = 3.0f * U(rng), L.pos[2] = 10.0f * U(rng), L.W = 0.5f + 30.0f * U(rng);
            for (float& c : L.colour) c = U(rng);
            GpuLampLaw::NoCone(L);
            const int shape = i % 3;
            if (shape == 1) {
                const float s[3] = {U(rng) - 0.5f, U(rng), U(rng) - 0.5f};
                GpuLampLaw::TubeShape(L, s, 0.5f + U(rng));
            } else if (shape == 2) {
                const float u[3] = {1, 0, 0}, v[3] = {0, 0, 1}, n[3] = {0, -1, 0};
                GpuLampLaw::AreaShape(L, u, v, n, 0.2f + U(rng), 0.2f + U(rng), i % 2 == 0);
            }
            const float scale = 0.5f + 2.0f * U(rng), r2 = Reach2(L, scale, thr);
            if (r2 <= 0.0f) continue;
            // a point at a random distance in a random direction, a random normal
            float d[3] = {U(rng) - 0.5f, U(rng) - 0.5f, U(rng) - 0.5f};
            const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            const float dist = std::sqrt(r2) * (0.2f + 1.6f * U(rng));
            float p[3], n[3] = {U(rng) - 0.5f, U(rng) - 0.5f, U(rng) - 0.5f}, out[3];
            for (int k = 0; k < 3; k++) p[k] = L.pos[k] + d[k] / dl * dist;
            const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (float& v : n) v /= nl;
            GpuLampLaw::Eval(L, p, n, 0.0f, out);
            const float lit = std::max(scale, 1.0f) * ((out[0] + out[1]) + out[2]);
            Box b;
            b.Grow(p);
            if (!Meets(b, L.pos, r2)) beyond = beyond && lit < thr, tested++;
            Box around; // (a box holding the lamp: always drawn)
            around.Grow(L.pos);
            nearDrawn = nearDrawn && Meets(around, L.pos, r2);
        }
        Check(tested > 1000 && beyond, "physical pick: a lamp whose reach misses a texel lights it under the threshold (bulbs, tubes, area lights)");
        Check(nearDrawn, "physical pick: a box holding the lamp always meets its reach");
        GpuLampLaw::Lamp off;
        off.W = 5.0f;
        Box b;
        const float o[3] = {0, 0, 0};
        b.Grow(o);
        Check(Reach2(off, 1.0f, thr) == 0.0f && !Meets(b, o, 0.0f), "physical pick: a lamp switched off is drawn nowhere");
        GpuLampLaw::Lamp on;
        on.W = 5.0f, on.colour[0] = 1.0f;
        Check(Reach2(on, 1.0f, 0.0f) < 0.0f && Meets(b, o, -1.0f) && !Meets(Box{}, o, 4.0f), "physical pick: no threshold draws it everywhere; an empty map draws none");
        float far[3] = {0.0f, 0.0f, 3.0f};
        Box unit;
        const float lo[3] = {0, 0, 0}, hi[3] = {1, 1, 1};
        unit.Grow(lo), unit.Grow(hi);
        Check(Meets(unit, far, 4.0f) && !Meets(unit, far, 3.99f), "physical pick: Meets measures to the box's nearest point");
    }
    // never counted twice: the base's lights are never drawn, the listed ones with their rows, the rest fresh
    {
        const std::vector<uintptr_t> listed = {0x100, 0x200}, baked = {0x300, 0x200};
        Check(UseOf(0x300, listed, baked) == Use::Base && UseOf(0x200, listed, baked) == Use::Base && UseOf(0x100, listed, baked) == Use::Listed &&
                  UseOf(0x400, listed, baked) == Use::Fresh,
              "physical pick: a light in the base is never drawn, a listed lamp is, a lamp the solve did not list is fresh");
    }
    // a fresh lamp's row: another room of the same story at its normalisation over this one's; else 1
    {
        Check(NormKFresh(0, 5, 2.0f, 0, 3, 1.0f, false) == InnerLight::NormScale(2.0f, 1.0f, false) && NormKFresh(0, 5, 2.0f, 0, 3, 4.0f, true) == InnerLight::NormScale(2.0f, 4.0f, true),
              "physical pick: a lamp of another room of the story takes that room's norm scale");
        Check(NormKFresh(1, 5, 2.0f, 0, 3, 1.0f, false) == 1.0f && NormKFresh(0, 3, 2.0f, 0, 3, 1.0f, false) == 1.0f && NormKFresh(0, 0, 2.0f, 0, 3, 1.0f, false) == 1.0f,
              "physical pick: a lamp of another story, of this room or outdoors: norm scale 1");
        Check(ReachFresh(false, true) && ReachFresh(true, false) && !ReachFresh(false, false), "physical pick: the reach share of a fresh lamp (outdoor room, or an outdoor lamp)");
        Check(!ReachFresh(false, true, true) && ReachFresh(true, true, true) && !ReachFresh(false, false, true),
              "physical pick: no reach share for a lamp of a room open into this room 0 (one space), an outdoor lamp keeps it");
        // room 0 takes a roofed room's lamp of the story above at that room's normalisation (innerUp); not a roofless room's, not
        // two stories up, not a room > 0
        Check(NormKFresh(2, 5, 2.0f, 1, 0, 1.0f, true, false) == InnerLight::NormScale(2.0f, 1.0f, true) && NormKFresh(2, 5, 2.0f, 1, 0, 1.0f, true, true) == 1.0f &&
                  NormKFresh(3, 5, 2.0f, 1, 0, 1.0f, true, false) == 1.0f && NormKFresh(2, 5, 2.0f, 1, 3, 1.0f, false, false) == 1.0f &&
                  NormKFresh(0, 5, 2.0f, 0, 3, 1.0f, false, false) == NormKFresh(0, 5, 2.0f, 0, 3, 1.0f, false),
              "physical pick: room 0 takes a lamp of the story above at its room's norm scale (else as before)");
        Check(ExitFresh(0, 0, 4, false) && ExitFresh(0, 1, 4, false) && !ExitFresh(1, 1, 4, false) && !ExitFresh(0, 2, 4, false) && !ExitFresh(0, 0, 0, true) &&
                  !ExitFresh(0, 0, 4, true),
              "physical pick: a fresh exit lamp of room 0 (story 0; a roofed room's lamp of story 0 or 1)");
    }
    // a lamp that left the registry is gone (no kept list draws it); one seen again is not; at most `max`, the oldest dropped
    {
        std::vector<uintptr_t> gone;
        NoteGone({1, 2, 3}, {1, 3, 4}, gone);
        bool ok = gone == std::vector<uintptr_t>{2};
        NoteGone({1, 3, 4}, {1, 3, 4}, gone);
        ok = ok && gone == std::vector<uintptr_t>{2};
        NoteGone({1, 3, 4}, {2, 4}, gone); // (2 a new light at a freed address)
        ok = ok && gone == std::vector<uintptr_t>{1, 3};
        NoteGone({1, 3, 4, 5, 6}, {}, gone, 3);
        ok = ok && gone == std::vector<uintptr_t>{4, 5, 6};
        Check(ok, "physical pick: the lamps gone from the registry (deleted), back when seen again, bounded");
    }
    // a lamp the registry drops for a moment stays in the snapshot (its last entry) until it has been missing for the grace;
    // one seen again is forgotten; another lot carries nothing
    {
        struct L {
            uintptr_t light;
            int story;
            bool operator==(const L&) const = default;
        };
        std::map<uintptr_t, uint32_t> missing;
        std::vector<L> was = {{1, 1}, {2, 1}, {3, 2}}, now = {{1, 1}};
        CarryMissing(was, now, true, 100, 1000, missing); // (2 and 3 missed at 100: carried)
        bool ok = now == std::vector<L>{{1, 1}, {2, 1}, {3, 2}} && missing.size() == 2;
        was = now, now = {{1, 1}, {3, 2}};
        CarryMissing(was, now, true, 600, 1000, missing); // (3 back; 2 still within its grace)
        ok = ok && now == std::vector<L>{{1, 1}, {3, 2}, {2, 1}} && missing.size() == 1 && missing.count(2);
        was = now, now = {{1, 1}, {3, 2}};
        CarryMissing(was, now, true, 1100, 1000, missing); // (2 missing for 1000 ms: dropped, then NoteGone takes it)
        ok = ok && now == std::vector<L>{{1, 1}, {3, 2}} && missing.empty();
        was = {{1, 1}, {5, 1}}, now = {{7, 1}};
        missing[9] = 0;
        CarryMissing(was, now, false, 2000, 1000, missing);
        ok = ok && now == std::vector<L>{{7, 1}} && missing.empty();
        Check(ok, "physical pick: a lamp the registry drops for a moment is carried for its grace, then gone; another lot carries none");
    }
    { // a room-0 wall facing into a room of the story below opened to it is that room's (RtLampProxy::OverOpenBelow)
        RtLampProxy::Lot lot;
        RtLampProxy::Story below, here;
        below.story = 1, here.story = 2, below.w = here.w = 3, below.h = here.h = 1;
        below.room = {0, 0, 0, 0, 5, 5, 5, 5, 7, 7, 7, 7}; // tiles x 0 (room 0), 1 (room 5, open), 2 (room 7, not open)
        here.placed = {0, 0, 0};
        lot.stories = {below, here};
        const std::vector<std::pair<int, int>> open = {{1, 5}};
        const float wall[3] = {1.0f, 4.0f, 0.5f}, in[3] = {1, 0, 0}, out[3] = {-1, 0, 0}, wall2[3] = {2.0f, 4.0f, 0.5f}, into7[3] = {1, 0, 0};
        bool ok = RtLampProxy::OverOpenBelow(lot, open, 2, wall, in) && !RtLampProxy::OverOpenBelow(lot, open, 2, wall, out) &&
                  !RtLampProxy::OverOpenBelow(lot, open, 2, wall2, into7) && !RtLampProxy::OverOpenBelow(lot, open, 1, wall, in);
        lot.stories[1].placed = {0, 0xF, 0}; // (a floor placed over room 5 on story 2: no longer one space)
        ok = ok && !RtLampProxy::OverOpenBelow(lot, open, 2, wall, in);
        Check(ok, "double-height room: a room-0 wall of the story above facing into the open room below is that room's; its outer face, a closed room or a floored quadrant is not");
    }
    { // a room's texel past its own diagonal wall is shadow-tested inside the room (RtLampProxy::IntoRoom)
        RtLampProxy::Lot lot;
        RtLampProxy::Story st;
        st.story = 1, st.w = 2, st.h = 1, st.room = {8, 0, 0, 8, 0, 0, 0, 0};
        lot.stories.push_back(st);
        float out[3] = {0.8f, 1.0f, 0.3f}, in[3] = {0.5f, 1.0f, 0.1f}, far[3] = {1.5f, 1.0f, 0.5f};
        const bool moved = RtLampProxy::IntoRoom(lot, 1, 8, out);
        const float fx = out[0], fz = out[2];
        const int q = std::fabs(fx - 0.5f) <= std::fabs(fz - 0.5f) ? (fz > 0.5f ? 2 : 0) : (fx > 0.5f ? 1 : 3);
        Check(moved && (q == 0 || q == 3) && out[1] == 1.0f && std::hypot(out[0] - 0.8f, out[2] - 0.3f) < 0.3f && !RtLampProxy::IntoRoom(lot, 1, 8, in) &&
                  !RtLampProxy::IntoRoom(lot, 1, 8, far) && !RtLampProxy::IntoRoom(lot, 2, 8, in),
              "physical shadow: a room's texel past its diagonal wall is looked up just inside the room; one inside, in a tile without the room or another story, is not moved");
    }
    { // a wall texel on a placed floor's plane is shadow-tested off it (RtLampProxy::OffFloorLine): story 1's room (x 1..4, z 1..7, floor 1,
      // ceiling 4) under story 2's placed floor at 4 (a stair hole at tile (1, 5)); the wall at x = 1 facing +x
        RtLampProxy::Story lo, up;
        lo.story = 1, up.story = 2, lo.w = up.w = lo.h = up.h = 8;
        const size_t nt = 64;
        lo.floorY.assign(nt, RtLampProxy::kNoFloor), lo.ceil.assign(nt, RtLampProxy::kNoFloor), lo.roofed.assign(nt, 0);
        up.floorY.assign(nt, RtLampProxy::kNoFloor), up.ceil.assign(nt, RtLampProxy::kNoFloor), up.roofed.assign(nt, 0), up.placed.assign(nt, 0);
        for (int z = 1; z < 7; z++)
            for (int x = 1; x < 4; x++) {
                const size_t t = static_cast<size_t>(z) * 8 + x;
                lo.floorY[t] = 1.0f, lo.ceil[t] = 4.0f, lo.roofed[t] = 0xF;
                if (!(x == 1 && z == 5)) up.placed[t] = 0xF, up.floorY[t] = 4.0f;
            }
        RtLampProxy::Lot lot;
        lot.stories = {lo, up};
        RtLampProxy::Mesh mesh;
        RtLampProxy::Build(lot, mesh, true);
        const float N[3] = {1, 0, 0}, R = 0.1f;
        const float above[3] = {2.5f, 4.3f, 2.5f}, ceilLamp[3] = {2.5f, 3.7f, 2.5f};
        float top[3] = {1.0f, 4.0f, 2.5f}, bottom2[3] = {1.0f, 4.0f, 2.5f}, hole[3] = {1.0f, 4.0f, 5.5f}, row11[3] = {1.0f, 1.0f + 11.0f * 3.0f / 12.0f, 2.5f};
        const float topRaw[3] = {top[0], top[1], top[2]}, row11Raw[3] = {row11[0], row11[1], row11[2]};
        const float leak = RtLampProxy::Visibility(mesh, above, R, topRaw);
        const bool mTop = RtLampProxy::OffFloorLine(lot, 1, top, N), mBottom = RtLampProxy::OffFloorLine(lot, 2, bottom2, N);
        const bool mHole = RtLampProxy::OffFloorLine(lot, 1, hole, N), mRow11 = RtLampProxy::OffFloorLine(lot, 1, row11, N);
        const float vAbove = RtLampProxy::Visibility(mesh, above, R, top), vCeil = RtLampProxy::Visibility(mesh, ceilLamp, R, top);
        const float vBottom = RtLampProxy::Visibility(mesh, ceilLamp, R, bottom2);
        std::printf("  off the floor line: story 1's top row %.2f m (story 2's lamp: share %.3f at the plane, %.3f moved; its own ceiling lamp %.3f), story 2's bottom row %.2f m "
                    "(story 1's lamp %.3f); by the stair hole %s, row 11 %s\n",
                    top[1], leak, vAbove, vCeil, bottom2[1], vBottom, mHole ? "moved" : "kept", mRow11 ? "moved" : "kept");
        Check(mTop && std::fabs(top[1] - 3.88f) < 1e-4f && leak > 0.5f && vAbove < 0.01f, "off the floor line: the top row under a slab is dark to a lamp of the story above");
        Check(vCeil > 0.99f, "off the floor line: the top row stays lit by its own room's ceiling lamp");
        Check(!mHole && hole[1] == 4.0f, "off the floor line: next to a stair hole (no placed floor) the point does not move");
        Check(!mRow11 && std::memcmp(row11, row11Raw, sizeof row11) == 0, "off the floor line: row 11, under the plane, does not move");
        Check(mBottom && std::fabs(bottom2[1] - 4.12f) < 1e-4f && vBottom < 0.01f, "off the floor line: story 2's bottom row is dark to a lamp of story 1");
    }
    {
        uintptr_t v[4] = {};
        Check(PhysLampPick::ListLen(nullptr, nullptr, 8) == 0 && PhysLampPick::ListLen(v, v + 3, 8) == 3 && PhysLampPick::ListLen(nullptr, v, 8) == -1 &&
                  PhysLampPick::ListLen(v + 2, v, 8) == -1 && PhysLampPick::ListLen(v, v + 4, 3) == -1,
              "physical pick: a room with no light buffer reads as an empty list; broken or too long bounds stay unreadable");
    }
    {
        using PhysLampPick::OccMove;
        // (03/11 13:56 recording) a table dragged with its lamp moves one occluder entry, 57B1E040, along with the head
        const OccMove tableDrag[] = {{0x57B1E040, 1000, 854.55f, 1237.48f}, {0x57B1E040, 1100, 854.43f, 1237.63f}};
        const OccMove stale[] = {{0x10, 100, 5.0f, 5.0f}, {0x20, 110, 5.0f, 5.0f}};
        const OccMove far[] = {{0x10, 100, 5.0f, 5.0f}, {0x20, 1010, 9.0f, 5.0f}};
        Check(PhysLampPick::FurnitureMovedWith(tableDrag, 2, 854.43f, 1237.63f, 1200, 600, 1.0f) &&
                  !PhysLampPick::FurnitureMovedWith(stale, 2, 5.0f, 5.0f, 1200, 600, 1.0f) &&
                  !PhysLampPick::FurnitureMovedWith(far, 2, 5.0f, 5.0f, 1200, 600, 1.0f),
              "physical pick: a table dragged with its lamp (one occluder entry moving) carries its furniture; old or far moves do not count");
        // (ShapeMoved, the carry's decision) the table's record names its object by its first blocker (0x5000): the table dragged
        // moves an entry with that shape, so its record is carried. The lamp dragged alone moves no entry of the table: an entry of
        // another object by the head (made again at a freed address, 14:36 of 03/11, now no move at all) or an old move of the
        // table leaves it in place; an unknown shape never carries
        const OccMove table[] = {{0x57B1E040, 1100, 854.43f, 1237.63f, 0x5000}};
        const OccMove other[] = {{0x3B1B6390, 1100, 846.82f, 1235.04f, 0x7000}, {0x57B1E040, 400, 854.43f, 1237.63f, 0x5000}};
        Check(PhysLampPick::ShapeMoved(table, 1, 0x5000, 1200, 600) && !PhysLampPick::ShapeMoved(other, 2, 0x5000, 1200, 600) &&
                  !PhysLampPick::ShapeMoved(table, 1, 0, 1200, 600) && !PhysLampPick::ShapeMoved(other, 2, 0, 1200, 600),
              "table drag: the record moves only when its own object's entry moved lately; a lamp dragged alone leaves its table in place");
        RoomLightTests::Room a, b;
        a.rec.resize(2), b.rec.resize(1);
        a.rec[0].shape = 0x10, a.rec[1].shape = 0x20, b.rec[0].shape = 0x5000;
        RtLampGpu::BlockerTable t;
        const int ha = t.Append(a), hb = t.Append(b);
        Check(t.ShapeOf(ha, 1) == 0x20 && t.ShapeOf(hb, 0) == 0x5000 && t.ShapeOf(hb, 3) == 0, "blocker table: each room's records keep their objects' first shapes");
    }
    {
        // (CarryStale, WritePhysCarry) a side table's top (a disc at 0.77 m) dragged from x = 0 to x = 2 with its lamp on it (head at
        // 1.34 m): the floor under its old place, and under its new one, takes the new solve's value; the stairs (a sample above the
        // lamp) and the floor far behind keep the last write. Unchanged records leave every sample to the carry
        const auto disc = [](float x) {
            RoomLightTests::Record rc;
            rc.c = {x, 0.77f, 0.0f}, rc.r2 = 1.0f;
            RoomLightTests::Blocker k{};
            k.vt = RoomLightTests::kDiscVt;
            const float c[3] = {x, 0.77f, 0.0f}, n[3] = {0.0f, 1.0f, 0.0f};
            std::memcpy(&k.f[0x20 / 4], c, 12), std::memcpy(&k.f[0x40 / 4], n, 12);
            k.f[0x70 / 4] = 0.357f * 0.357f;
            rc.b.push_back(k);
            return rc;
        };
        RoomLightTests::Room was, now, still;
        was.rec = {disc(0.0f), disc(-4.0f)}, now.rec = {disc(-4.0f), disc(2.0f)};
        const std::vector<std::array<float, 6>> lamps = {{0.0f, 1.34f, 0.0f, 2.0f, 1.34f, 0.0f}};
        const float samples[][3] = {{0.0f, 0.0f, 0.1f}, {2.0f, 0.0f, 0.1f}, {1.0f, 2.6f, 0.0f}, {8.0f, 0.0f, 6.0f}};
        const auto at = [&](size_t i) { return samples[i]; };
        const std::vector<char> s = PhysLampPick::CarryStale(was, now, lamps, 4, at), none = PhysLampPick::CarryStale(now, now, lamps, 4, at),
                                dark = PhysLampPick::CarryStale(was, still, {}, 4, at);
        Check(s.size() == 4 && s[0] && s[1] && !s[2] && !s[3] && std::count(none.begin(), none.end(), 1) == 0 && std::count(dark.begin(), dark.end(), 1) == 0,
              "carried write: samples a moved table shades, old place or new, take the solve's value; stairs above and far floor keep the carry");
        // (RecordKey) the game's own state in a blocker's bytes (+0x04, a reference count the gather raises) and the vectors'
        // addresses change nothing: an unchanged room keeps the whole carry
        RoomLightTests::Room again = now;
        for (RoomLightTests::Record& rc : again.rec) rc.b[0].f[1] = 7.0f, rc.shape = 0x1234;
        const std::vector<char> refs = PhysLampPick::CarryStale(now, again, lamps, 4, at);
        Check(std::count(refs.begin(), refs.end(), 1) == 0 && RoomLightTests::RecordKey(now.rec[1]) == RoomLightTests::RecordKey(again.rec[1]) &&
                  RoomLightTests::RecordKey(now.rec[0]) != RoomLightTests::RecordKey(now.rec[1]),
              "carried write: records alike in what the test reads count as the same, whatever else their bytes hold");
        // (Record::off, the directional maps' table carry) the table read at x = 0 carried +2 m shades as the table read at x = 2,
        // and nothing at its old place; the carried copy's write is stale where it shaded once the solve reads the table at x = 3
        RoomLightTests::Room carried, at2;
        carried.rec = {disc(0.0f)}, at2.rec = {disc(2.0f)};
        carried.rec[0].c.x += 2.0f, carried.rec[0].off[0] = 2.0f;
        int unknown = 0;
        bool alike = true;
        for (float x = -1.0f; x <= 5.0f; x += 0.05f) {
            const RoomLightTests::V lamp{2.0f, 1.34f, 0.0f}, p{x, 0.0f, 0.1f};
            alike = alike && std::fabs(RoomLightTests::occlude(carried, lamp, p, &unknown) - RoomLightTests::occlude(at2, lamp, p, &unknown)) < 1e-4f;
        }
        RoomLightTests::Room at3;
        at3.rec = {disc(3.0f)};
        const std::vector<std::array<float, 6>> drag = {{2.0f, 1.34f, 0.0f, 3.0f, 1.34f, 0.0f}};
        const float under[][3] = {{2.0f, 0.0f, 0.1f}, {3.0f, 0.0f, 0.1f}, {-3.0f, 0.0f, 0.1f}};
        const std::vector<char> c = PhysLampPick::CarryStale(carried, at3, drag, 3, [&](size_t i) { return under[i]; });
        Check(alike && RoomLightTests::occlude(carried, {2.0f, 1.34f, 0.0f}, {2.0f, 0.0f, 0.1f}, &unknown) < 0.01f &&
                  RoomLightTests::occlude(carried, {2.0f, 1.34f, 0.0f}, {0.0f, 0.0f, 0.1f}, &unknown) == 1.0f && c[0] && c[1] && !c[2],
              "directional table carry: the solve's table moved by its offset shades as the table read there; its write is stale under it");
    }
    {
        // a floor texel at (1, 2, 1) with 0.25 m steps; samples: position at 0..2, normal at 4..6
        const float s[8] = {1, 2, 1, 0, 0, 1, 0, 0}, r[8] = {1.25f, 2, 1, 0, 0, 1, 0, 0}, d[8] = {1, 2, 1.25f, 0, 0, 1, 0, 0};
        const float wall[8] = {0.75f, 2.2f, 1, 0, 1, 0, 0, 0}, step[8] = {1, 2, 0.75f, 0, 0, 1, 0, 0}, far[8] = {1, 2, 1.75f, 0, 0, 1, 0, 0};
        float o[3];
        const auto near = [&](float x, float y, float z) { return std::fabs(o[0] - x) < 1e-5f && std::fabs(o[1] - y) < 1e-5f && std::fabs(o[2] - z) < 1e-5f; };
        const float* all[4] = {r, nullptr, d, nullptr};
        PhysLampPick::SubTexelPos(s, all, 0.25f, -0.25f, o);
        bool ok = near(1.0625f, 2, 0.9375f);
        const float* edge[4] = {nullptr, wall, nullptr, step}; // (left is a wall's texel: another plane; up is on the plane: its step taken back)
        PhysLampPick::SubTexelPos(s, edge, 0.25f, 0.25f, o);
        ok = ok && near(1, 2, 1.0625f);
        const float* none[4] = {nullptr, nullptr, far, nullptr}; // (0.75 m away: not a neighbour)
        PhysLampPick::SubTexelPos(s, none, -0.25f, 0.25f, o);
        ok = ok && near(1, 2, 1);
        Check(ok, "sharper floors: a sub-texel steps toward its same-plane neighbours, takes an edge's step from the other side, never a wall's or a far texel's");
        const float l[8] = {0.75f, 2, 1, 0, 0, 1, 0, 0}, u[8] = {1, 2, 0.75f, 0, 0, 1, 0, 0};
        const float* inside[4] = {r, l, d, u};
        const float* byWall[4] = {r, wall, d, u};
        Check(std::fabs(PhysLampPick::SubTexelFootprint(s, inside, 0.25f) - 0.0625f) < 1e-6f && PhysLampPick::SubTexelFootprint(s, byWall, 0.25f) == 0.0f &&
                  PhysLampPick::SubTexelFootprint(s, all, 0.25f) == 0.0f,
              "sharper floors: a sub-texel's shadow footprint is a quarter texel inside a floor, none by a wall or at an edge");
    }
    {
        // room 0's floor on one side of a diagonal wall (texels x > y of an 8 x 8 grid, 0.25 m apart), the half tiles past it (x <= y) on
        // the same plane: its own texels as neighbours only, a texel by the diagonal takes no footprint and its sub-texels stay on its side
        std::vector<std::array<float, 8>> grid(64);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) grid[y * 8 + x] = {(x + 0.5f) * 0.25f, 0, (y + 0.5f) * 0.25f, 0, 0, 1, 0, 0};
        const auto own = [&](int x, int y) -> const float* { return x >= 0 && y >= 0 && x < 8 && y < 8 && x > y ? grid[y * 8 + x].data() : nullptr; };
        const auto both = [&](int x, int y) -> const float* { return x >= 0 && y >= 0 && x < 8 && y < 8 ? grid[y * 8 + x].data() : nullptr; };
        float sub[4][3];
        bool ok = PhysLampPick::SubTexels(grid[2 * 8 + 3].data(), 3, 2, own, sub) == 0.0f && PhysLampPick::SubTexels(grid[2 * 8 + 3].data(), 3, 2, both, sub) > 0.0f &&
                  PhysLampPick::SubTexels(grid[2 * 8 + 5].data(), 5, 2, own, sub) > 0.0f;
        for (int y = 0; y < 8; y++)
            for (int x = y + 1; x < 8; x++) {
                PhysLampPick::SubTexels(grid[y * 8 + x].data(), x, y, own, sub);
                for (const auto& q : sub) ok = ok && q[0] > q[2];
            }
        Check(ok, "sharper floors: room 0's sub-texels by a diagonal wall take its own texels only (no footprint there), and stay on its side");
    }
    {
        // the split tiles' atlas: a half tile's strip (0.25 m steps along x) ends at x = 2; its map neighbour x = 3 starts another half
        // tile's strip 0.36 m on, same plane (14:36 scene): not a neighbour, so the sub-texels of x = 2 take the step from x = 1
        std::array<float, 8> t[4];
        for (int x = 0; x < 3; x++) t[x] = {x * 0.25f, 0, 0, 0, 0, 1, 0, 0};
        t[3] = {0.5f + 0.36f, 0, 0, 0, 0, 1, 0, 0};
        const auto at = [&](int x, int y) -> const float* { return y == 0 && x >= 0 && x < 4 ? t[x].data() : nullptr; };
        float sub[4][3];
        const float fp = PhysLampPick::SubTexels(t[2].data(), 2, 0, at, sub);
        Check(fp == 0.0f && std::fabs(sub[1][0] - (0.5f + 0.0625f)) < 1e-5f && std::fabs(sub[0][0] - (0.5f - 0.0625f)) < 1e-5f,
              "sharper floors: a split-atlas strip's end takes no step toward the next strip's texel (1.4 steps away), only its own strip's");
    }
    {
        const auto at = [](uint32_t x, uint32_t y) { return y << 16 | x; };
        const std::vector<uint32_t> a = {at(0, 0), at(1, 0), at(1, 0)}, b = {at(2, 0), at(0, 1)}, c = {at(1, 0)}, past = {at(9, 9), at(0, 1)}, far = {at(9, 9)};
        Check(PhysLampPick::WritesDisjoint(4, 4, {&a, &b}) && !PhysLampPick::WritesDisjoint(4, 4, {&a, &b, &c}) &&
                  !PhysLampPick::WritesDisjoint(4, 4, {&b, &past}) && PhysLampPick::WritesDisjoint(4, 4, {&a, &far}),
              "partial relight: rooms whose places never meet are disjoint (a room writing its own place twice too); a shared place is told, a place past the map is not written");
    }
    // the rooms an edit taken without their solve leaves stale in the game's maps: the rooms listing it, its own and every room 0;
    // forgotten at their solve, each sent once on a fallback, all on a full drop
    {
        TouchedRooms t;
        t.NoteEdit(7, 1, {{1, 3}, {2, 5}}, 4);
        bool ok = t.rooms.size() == 2 + 1 + 8 && t.rooms.count({7, 1, 3}) && t.rooms.count({7, 2, 5}) && t.rooms.count({7, 1, 4}) && t.rooms.count({7, 0, 0}) &&
                  t.rooms.count({7, 7, 0});
        t.NoteEdit(7, 1, {{1, 3}}, -1); // (the same rooms again: once each)
        ok = ok && t.rooms.size() == 11;
        t.Solved({7, 1, 3});
        ok = ok && !t.Take({7, 1, 3}) && t.Take({7, 2, 5}) && !t.Take({7, 2, 5});
        const auto all = t.TakeAll();
        ok = ok && all.size() == 9 && t.rooms.empty() && t.TakeAll().empty();
        Check(ok, "physical edits: the rooms stale in the game's maps until their solve, sent once on a fallback");
    }
    // a step under 10 cm is a move only within an edit going on (a drag); an animated lamp's wobble is none
    {
        const float m = 0.10f;
        bool ok = EditKind(true, 0.0f, m, false, true, false) == 2 && EditKind(false, 0.2f * 0.2f, m, false, true, false) == 1 &&
                  EditKind(false, 0.0f, m, true, true, false) == 0 && EditKind(false, 0.0f, m, false, true, true) == -1;
        ok = ok && EditKind(false, 0.03f * 0.03f, m, false, true, true) == 1;   // (a slow drag's step)
        ok = ok && EditKind(false, 0.03f * 0.03f, m, false, true, false) == -1; // (a wobble: the game's path)
        ok = ok && EditKind(false, 0.03f * 0.03f, m, true, true, false) == 0;   // (a wobble with a value change: the value)
        ok = ok && EditKind(false, 0.03f * 0.03f, m, false, false, true) == -1; // (the replica model: never)
        ok = ok && EditKind(false, std::nanf(""), m, false, true, true) == -1;
        // (a signature change in place, as a drag starts or the lamp is put down: a value physically, the game's path otherwise)
        ok = ok && EditKind(false, 0.0f, m, false, true, true, true) == 0 && EditKind(false, 0.0f, m, false, true, false, true) == 0;
        ok = ok && EditKind(false, 0.0f, m, false, false, true, true) == -1 && EditKind(false, 0.03f * 0.03f, m, false, true, false, true) == -1;
        ok = ok && EditKind(false, std::nanf(""), m, false, true, true, true) == -1;
        Check(ok, "physical edits: a step under 10 cm is a move only while a drag goes on; a change in place is a value");
    }
    // the occluder mark: a new lamp only with the registry snapshot's evidence; an occluder is the lamp's own object only by the
    // head and moving with it
    {
        bool ok = NewLampEvidence(false, true, false) && !NewLampEvidence(true, true, false) && !NewLampEvidence(false, false, false) &&
                  !NewLampEvidence(false, true, true);
        const float lw[3] = {2.0f, 1.5f, 3.0f}, ln[3] = {2.4f, 1.5f, 3.3f}, still[3] = {2.0f, 1.5f, 3.0f};
        const float ow[2] = {2.1f, 3.0f}, on[2] = {2.5f, 3.3f}, sofaW[2] = {1.6f, 3.0f}, sofaN[2] = {2.2f, 3.1f};
        ok = ok && OwnObjectStep(true, ow, on, lw, ln, 0.05f, 0.5f);             // (its object, moved with it)
        ok = ok && !OwnObjectStep(true, sofaW, sofaN, lw, ln, 0.05f, 0.5f);      // (another object moved the other way, ending by the lamp)
        ok = ok && !OwnObjectStep(true, sofaW, sofaN, still, still, 0.05f, 0.5f); // (a sofa moved by a lamp whose value changes)
        ok = ok && OwnObjectStep(true, ow, ow, still, still, 0.05f, 0.5f);       // (a switch: neither moved)
        const float far[2] = {2.7f, 3.0f}; // (0.7 m from the head)
        ok = ok && OwnObjectStep(false, ow, ow, still, still, 0.05f, 0.5f) && !OwnObjectStep(false, far, far, still, still, 0.05f, 0.5f);
        ok = ok && !OwnObjectStep(true, far, far, still, still, 0.05f, 0.5f); // (beyond the head's 0.5 m: not its object)
        Check(ok, "physical edits: an occluder mark is dropped only for a new lamp's or the lamp's own object");
    }
    // the edit: every story 0 .. 7 drawn, every room > 0 a kept physical solve drawn without the lamp in its base
    {
        std::vector<RoomFact> rooms(4);
        rooms[0].story = 0, rooms[0].id = 0, rooms[0].drawn = true;
        rooms[1].story = 0, rooms[1].id = 2, rooms[1].kept = rooms[1].drawn = true, rooms[1].lists = true;
        rooms[2].story = 1, rooms[2].id = 0, rooms[2].drawn = true;
        rooms[3].story = 1, rooms[3].id = 4, rooms[3].kept = rooms[3].drawn = true;
        int at = 0;
        Check(EditRefusal(rooms, at) == nullptr && at == -1, "physical edit: every story drawn takes the edit");
        auto r = rooms;
        r[2].drawn = false;
        Check(EditRefusal(r, at) != nullptr && at == 2, "physical edit: a room 0 not drawn physically refuses it");
        r = rooms, r[3].kept = r[3].drawn = false;
        Check(EditRefusal(r, at) == nullptr, "physical edit: a room with no kept physical solve not listing the lamp does not refuse it");
        r[3].lists = true;
        Check(EditRefusal(r, at) != nullptr && at == 3, "physical edit: a room listing the lamp with no kept physical solve refuses it");
        r = rooms, r[1].drawn = false;
        Check(EditRefusal(r, at) != nullptr && at == 1, "physical edit: a kept solve not drawn refuses it");
        r = rooms, r[1].baked = true;
        Check(EditRefusal(r, at) != nullptr && at == 1, "physical edit: a solve with the lamp in its base refuses it");
        {
            const uint64_t was[3] = {11, 22, 33}, door[3] = {12, 23, 33}, moved[3] = {11, 22, 34}, unread[3] = {0, 0, 0};
            Check(PhysLampPick::BlockersKey(was) == PhysLampPick::BlockersKey(door) && PhysLampPick::BlockersKey(was) != PhysLampPick::BlockersKey(moved) &&
                      PhysLampPick::BlockersKey(unread) == 0 && PhysLampPick::BlockersKey(was) != 0,
                  "physical edit: a room's blockers key follows its 3D blockers alone (a door of the lot or its wall list leaves it; a moved table does not)");
        }
        r = rooms;
        RoomFact basement;
        basement.story = -1, basement.id = 1;
        r.push_back(basement);
        Check(EditRefusal(r, at) == nullptr, "physical edit: a basement room not listing the lamp is not needed");
        r.back().lists = true;
        Check(EditRefusal(r, at) != nullptr && at == 4, "physical edit: a basement room listing the lamp refuses it");
        Check(EditRefusal({}, at) != nullptr, "physical edit: no story found refuses it");
    }
}
// (sun_lamp_level) The lamp terms' level through a dawn and a dusk: the sky's level turns in half a second of frame time at any clock, the
// sun's part of the game's curve moves with the clock. The sun's height from the 05/11 log's snapshots (-21.9 at 4.443 h, -6.9 at 5.508 h)
void DawnChecks() {
    using namespace TerrainLightingPolicy;
    constexpr float kPi = 3.14159265f, kPerHour = 15.0f / 1.065f, kClockPerSec = 0.015f, kStep = kClockPerSec / 60.0f; // (a 60 fps frame, the log's clock)
    Check(SunLampTerm(1.0f) == 0.0f && SunLampTerm(-0.04f) == 0.0f && SunLampTerm(-1.0f) == 1.0f && SunLampTerm(std::nanf("")) == 0.0f, "lamp level: no sun term by day or unreadable");
    const auto sine = [&](float deg) { return std::sin(deg * kPi / 180.0f); };
    Check(std::fabs(SunLampTerm(sine(-6.9f)) - 0.40f) < 0.01f, "lamp level: 0.40 at the capture's -6.9 degrees (manmade 0.551 with M about 0.25)");
    for (const bool dusk : {false, true})
        for (const float turnAt : {-13.0f, -7.0f, -3.0f}) { // (the sky's turn: -7 at the log's dawn, the rest unknown, so tried)
            float worstOld = 0.0f, worstNew = 0.0f, prevOld = -1.0f, prevNew = -1.0f, darker = 0.0f, level = dusk ? 0.0f : 1.0f, lamp = level, lag = 0.0f;
            bool day = true, night = true;
            for (float t = 0.0f; t < 4.0f; t += kStep) {
                const float deg = dusk ? 10.0f - t * kPerHour : -30.0f + t * kPerHour;
                const float rate = (1.0f / 0.6f) * (kStep / kClockPerSec); // (the level's move in a frame)
                if (dusk ? deg <= turnAt : deg >= turnAt) level = dusk ? std::min(1.0f, level + rate) : std::max(0.0f, level - rate);
                const float want = LampLevel(level, sine(deg));
                lamp = LampLevelStep(lamp, want, 1.0f / 60.0f);
                lag = std::max(lag, std::fabs(lamp - want));
                const float oldDay = LampDay(level), newDay = LampDay(lamp);
                if (prevOld >= 0.0f) worstOld = std::max(worstOld, std::fabs(oldDay - prevOld)), worstNew = std::max(worstNew, std::fabs(newDay - prevNew));
                prevOld = oldDay, prevNew = newDay;
                darker += oldDay - newDay > 1e-6f ? 1.0f / 60.0f : 0.0f;
                if (deg > (dusk ? 5.0f : 2.0f)) day = day && newDay == kLampDayFloor;
                if (deg < -20.0f) night = night && newDay == 1.0f;
            }
            std::printf("  %s, sky turning at %.0f deg: largest lamp step in a frame %.4f, with the sun %.4f; at most %.2f of level behind\n", dusk ? "dusk" : "dawn", turnAt, worstOld, worstNew, lag);
            Check(dusk || darker == 0.0f, "lamp level: a dawn never darker than the sky's level alone");
            Check(!dusk || darker <= kLampFadeSeconds, "lamp level: a dusk stays behind the sky's turn for the fade's seconds alone");
            Check(day && night, "lamp level: 3% in daylight and 1 in the dark, as before");
            Check(worstNew < 0.25f * worstOld, "lamp level: fades where the sky's level alone snaps");
        }
}
// (chair_arm_caps, live_object_tables) DrawnBlockers, Bound and LiveMoves
void SeatChecks() {
    using namespace RoomLightTests;
    const auto quad = [](V A, V N, V U, V W, float hu, float hw) {
        Blocker k{};
        k.vt = kQuadVt;
        const auto put = [&](int off, V p) { k.f[off / 4] = p.x, k.f[off / 4 + 1] = p.y, k.f[off / 4 + 2] = p.z; };
        put(0x70, A), put(0x80, A), put(0x90, N), put(0xA0, U), put(0xB0, W);
        k.f[0xD0 / 4] = k.f[0xD8 / 4] = hu, k.f[0xD4 / 4] = k.f[0xDC / 4] = hw;
        return k;
    };
    // a box-type armchair: seat and back as one slant, a back sheet, two arm sheets at the box's full height (record 10 of the 19-37-16 scene)
    const float s = 0.7071068f;
    const auto seat = [&](float armTop) {
        RoomLightTests::Record r;
        r.c = {0, 0.4f, 0}, r.r2 = 1.0f;
        r.b.push_back(quad({0, 0.4f, 0}, {0, s, s}, {1, 0, 0}, {0, s, -s}, 0.45f, 0.5f));
        r.b.push_back(quad({0, 0.4f, -0.4f}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, 0.45f, 0.4f));
        for (const float x : {-0.45f, 0.45f}) r.b.push_back(quad({x, armTop * 0.5f, 0}, {1, 0, 0}, {0, 0, 1}, {0, 1, 0}, 0.4f, armTop * 0.5f));
        return r;
    };
    std::vector<RoomLightTests::Record> box{seat(0.8f)};
    RtLampGpu::DrawnBlockers(box);
    const Blocker& cap = box[0].b.back();
    Check(box[0].b.size() == 5 && cap.f[0xF0 / 4] == kCapMark && cap.v(0x90).y == 1.0f, "seat: a box-type chair takes a cap");
    Check(std::fabs(box[0].b[2].f[0xD4 / 4] - 0.2f) < 1e-6f && std::fabs(box[0].b[2].f[0x70 / 4 + 1] - 0.2f) < 1e-6f && std::fabs(cap.f[0x70 / 4 + 1] - 0.4f) < 1e-6f,
          "seat: its arms half as tall on the same base, the cap at their top");
    Check(std::fabs(cap.f[0x70 / 4]) < 1e-6f && std::fabs(cap.f[0x70 / 4 + 2]) < 1e-6f && cap.f[0xD0 / 4] == 0.45f && cap.f[0xD4 / 4] == 0.4f, "seat: the cap over the box: the seat's width, the arms' depth");
    const float over[3] = {0.0f, 0.9f, 0.0f};
    Check(!StandsOn(cap, over) && !StandsOn(box[0], over), "seat: nothing stands on a cap");
    {
        RoomLightTests::Room raw, drawn;
        raw.rec = {seat(0.8f)}, drawn.rec = box;
        RtLampGpu::BlockerTable tr, td;
        const int hr = tr.Append(raw), hd = td.Append(drawn);
        const int firstBlocker = static_cast<int>(td.rows[static_cast<size_t>(td.rows[static_cast<size_t>(hd)].v[0]) + 1].v[0]);
        Check(!td.StandsOn(hd, 0, over) && td.BlockerAt(firstBlocker + 4 * RtLampGpu::BlockerTable::kBlockerRows).f[0xF0 / 4] == kCapMark, "seat: the table keeps the cap's mark");
        // a lamp edge-on to the slant, its light through an arm's top: that sheet alone drew a streak past the shadow, the short arm and the cap do not
        const V lamp{-1.02f, 3.2f, -2.4f}, far{0.9f, 0.0f, 0.8f};
        const float streak = tr.Occlude(hr, lamp, far), gone = td.Occlude(hd, lamp, far);
        std::printf("  seat: light along an arm 3 m past the chair %.2f, with the cap %.2f\n", streak, gone);
        Check(streak < 0.3f && gone > 0.75f, "seat: the streak past the shadow is gone");
        Check(td.Occlude(hd, {0, 2.5f, 0}, {0, 0, 0}) < 0.05f, "seat: a lamp over it still has its shadow under it");
    }
    // left alone: a model with stepped arms, a record with no back, one with two slants, a table's top
    {
        std::vector<RoomLightTests::Record> others{seat(0.5f), seat(0.8f), seat(0.8f), RoomLightTests::Record{{0, 0, 0}, 1.0f, {quad({0, 0.7f, 0}, {0, 1, 0}, {1, 0, 0}, {0, 0, 1}, 0.5f, 0.5f)}}};
        others[1].b.erase(others[1].b.begin() + 1);
        others[2].b.push_back(others[2].b[0]);
        const std::vector<RoomLightTests::Record> before = others;
        RtLampGpu::DrawnBlockers(others);
        bool same = true;
        for (size_t i = 0; i < others.size(); i++) same = same && RecordKey(others[i]) == RecordKey(before[i]);
        Check(same, "seat: stepped arms, no back, two slants and a table top are left alone");
    }
    Check(PhysLampPick::RetryMs(0) == 5000u && PhysLampPick::RetryMs(1) == 10000u && PhysLampPick::RetryMs(6) == 320000u && PhysLampPick::RetryMs(1000) == 320000u, "retry: a failed map again from 5 s to 5 min, never given up");
    // Bound: the game's sphere (a rectangle and a disc)
    {
        RoomLightTests::Record r;
        r.b.push_back(quad({1, 2, 3}, {0, 1, 0}, {1, 0, 0}, {0, 0, 1}, 0.5f, 0.25f));
        Blocker d{};
        d.vt = kDiscVt;
        d.f[0x20 / 4] = 3.0f, d.f[0x20 / 4 + 1] = 1.0f, d.f[0x20 / 4 + 2] = 3.0f, d.f[0x70 / 4] = 0.25f;
        r.b.push_back(d);
        Bound(r);
        Check(std::fabs(r.c.x - 2.0f) < 1e-6f && std::fabs(r.c.y - 1.5f) < 1e-6f && std::fabs(r.c.z - 3.0f) < 1e-6f && std::fabs(r.r2 - (1.5f * 1.5f + 0.25f + 0.25f)) < 1e-5f,
              "bound: the box of a rectangle's corners and a disc's circle, its middle and half diagonal");
    }
    // LiveMoves: the game's blockers as a table of addresses
    {
        RoomLightTests::Record a = seat(0.8f), b = seat(0.8f);
        for (Blocker& k : b.b) k.f[0x70 / 4] += 5.0f, k.f[0x80 / 4] += 5.0f;
        Bound(a), Bound(b);
        std::vector<Blocker> mem;
        std::vector<RoomLightTests::Record> kept{a, b};
        for (RoomLightTests::Record& r : kept)
            for (const Blocker& k : r.b) r.at.push_back(mem.size() + 1), mem.push_back(k);
        bool readable = true;
        const auto read = [&](uintptr_t at, Blocker& k) {
            if (!readable) return false;
            k = mem[at - 1];
            return true;
        };
        std::vector<LiveMove> m;
        Check(LiveMoves(kept, read, m) == 0 && m.empty(), "live table: nothing moved, no key");
        // the first chair moved 1.5 m and turned a quarter: every field the setter writes
        const auto turned = [](Blocker k) {
            for (const int off : {0x70, 0x80}) {
                const float x = k.f[off / 4], z = k.f[off / 4 + 2];
                k.f[off / 4] = z + 1.5f, k.f[off / 4 + 2] = -x;
            }
            for (const int off : {0x90, 0xA0, 0xB0}) {
                const float x = k.f[off / 4], z = k.f[off / 4 + 2];
                k.f[off / 4] = z, k.f[off / 4 + 2] = -x;
            }
            return k;
        };
        for (size_t i = 0; i < a.b.size(); i++) mem[i] = turned(a.b[i]);
        const uint64_t key = LiveMoves(kept, read, m);
        Check(key != 0 && m.size() == 1 && m[0].rec == 0, "live table: the moved record alone");
        std::vector<RoomLightTests::Record> live = kept;
        live[0].b = m[0].b, Bound(live[0]);
        m.clear();
        Check(LiveMoves(kept, read, m) == key, "live table: the same place, the same key");
        RoomLightTests::Record want = a;
        for (Blocker& k : want.b) k = turned(k);
        Bound(want);
        RoomLightTests::Room lr, wr;
        lr.rec = {live[0]}, wr.rec = {want};
        RtLampGpu::BlockerTable lt, wt;
        lt.Append(lr), wt.Append(wr);
        Check(lt.rows.size() == wt.rows.size() && std::memcmp(lt.rows.data(), wt.rows.data(), lt.rows.size() * sizeof lt.rows[0]) == 0, "live table: the same rows as a table read at the new place");
        // an object gone: its memory unreadable, or another's
        m.clear(), readable = false;
        Check(LiveMoves(kept, read, m) == 0, "live table: unreadable blockers keep their old copy");
        readable = true;
        for (size_t i = 0; i < a.b.size(); i++) mem[i] = a.b[i], mem[i].vt = 0x1234;
        m.clear();
        Check(LiveMoves(kept, read, m) == 0, "live table: another vtable there is not a move");
        // a kind RecordKey hashes whole: its raw bytes are not a move
        RoomLightTests::Record odd = seat(0.8f);
        odd.b[0].vt = 0x77;
        odd.at = {1, 2, 3, 4};
        std::vector<Blocker> oddMem(odd.b.begin(), odd.b.end());
        oddMem[0].f[1] += 1.0f;
        m.clear();
        Check(LiveMoves(std::vector<RoomLightTests::Record>{odd}, [&](uintptr_t at, Blocker& k) { k = oddMem[at - 1]; return true; }, m) == 0, "live table: an unknown kind's bytes are not a move");
    }
    // LiveDue: a table dragged with its lamp, 60 frames a second at 6 m/s (23-38-26 recording of 05/11): the table's disc, the lamp 0.57 m over it
    {
        RoomLightTests::Record t;
        Blocker top{};
        top.vt = kDiscVt, top.f[0x24 / 4] = 0.77f, top.f[0x44 / 4] = 1.0f, top.f[0x70 / 4] = 0.1277f;
        t.b = {top}, t.at = {1}, Bound(t);
        const std::vector<RoomLightTests::Record> kept{t};
        // (the table and the lamp each move or stay, up to frame `stop`: the drop)
        const auto run = [&](bool perFrame, bool table, bool lampMoves, int stop, int& noFill, float& worst, int& flips) {
            Blocker mem = top;
            uint32_t gen = 0, seen = 0, readGen = 0, readStep = 0, reads = 0;
            uint64_t key = 0;
            float held = 0.0f;
            RoomLightTests::Room room;
            room.rec = kept;
            RtLampGpu::BlockerTable drawn;
            int header = drawn.Append(room), at = -1000;
            noFill = 0, worst = 0.0f, flips = 0;
            bool filled = true;
            for (int frame = 0; frame < 60; frame++) {
                const int ms = frame * 1000 / 60;
                const float x = 0.1f * static_cast<float>(std::min(frame, stop - 1) + 1);
                if (frame < stop) gen++; // (the game's move)
                if (table) mem.f[0x20 / 4] = x;
                if (seen != gen && ms - at >= 100) seen = gen, at = ms; // (LiveGen)
                for (int map = 0; map < 4; map++) { // (a round's maps)
                    if (!(perFrame ? LiveDue(seen, readGen, gen, readStep, key) : seen != readGen)) continue;
                    std::vector<LiveMove> m;
                    key = LiveMoves(kept, [&](uintptr_t, Blocker& k) { k = mem; return true; }, m);
                    readGen = seen, readStep = gen, reads++;
                    room.rec = kept;
                    if (!m.empty()) room.rec[0].b = m[0].b, Bound(room.rec[0]);
                    drawn = {}, header = drawn.Append(room), held = room.rec[0].b[0].f[0x20 / 4];
                }
                const float lamp[3] = {lampMoves ? x : 0.0f, 1.34f, 0.0f};
                const bool fill = drawn.FillOf(header, lamp) != 0.0f;
                noFill += !fill, flips += fill != filled, filled = fill;
                worst = std::max(worst, mem.f[0x20 / 4] - held);
            }
            return reads;
        };
        int noFill = 0, flips = 0;
        float lag = 0.0f;
        const uint32_t paced = run(false, true, true, 60, noFill, lag, flips);
        Check(paced < 12 && noFill > 0 && lag > 0.25f && flips > 2, "live table: paced alone, a table falls behind its lamp and the fill flips off and on");
        const uint32_t every = run(true, true, true, 60, noFill, lag, flips);
        Check(every == 60 && noFill == 0 && lag == 0.0f && flips == 0, "live table: a moved record is read once a frame, on its lamp's place, the fill never off");
        const uint32_t dropped = run(true, true, true, 30, noFill, lag, flips);
        Check(dropped == 31 && noFill == 0 && lag == 0.0f && flips == 0, "live table: at the drop the reads stop (a last paced one, the same key), the record on its last place, the fill on");
        const uint32_t alone = run(true, false, true, 60, noFill, lag, flips);
        Check(alone < 12 && flips == 1 && noFill > 0, "live table: a lamp dragged off its still table stays paced, its fill goes off once");
        run(true, true, false, 60, noFill, lag, flips);
        Check(flips == 1 && lag == 0.0f, "live table: a table dragged from under its still lamp follows each frame, the fill goes off once");
        Check(!LiveDue(5, 5, 9, 8, 0) && LiveDue(6, 5, 8, 8, 0) && LiveDue(5, 5, 9, 8, 7) && !LiveDue(5, 5, 8, 8, 7), "live table: a room with nothing moved waits for the paced read");
    }
}
// BlockerTable::Join, the panes and ToneSoft (cross_room_shadows, glass_share, tone_soft)
void AcrossRoomsChecks() {
    using namespace RoomLightTests;
    const auto plate = [](float x, float z) { // a table top 1 m wide at (x, 0.75, z)
        Blocker k{};
        k.vt = kQuadVt;
        const float A[3] = {x, 0.75f, z}, N[3] = {0, 1, 0}, U[3] = {1, 0, 0}, W[3] = {0, 0, 1};
        for (int c = 0; c < 3; c++) k.f[0x70 / 4 + c] = k.f[0x80 / 4 + c] = A[c], k.f[0x90 / 4 + c] = N[c], k.f[0xA0 / 4 + c] = U[c], k.f[0xB0 / 4 + c] = W[c], k.f[0xE0 / 4 + c] = 7.0f, k.f[0xF0 / 4 + c] = -3.0f;
        k.f[0xD0 / 4] = k.f[0xD4 / 4] = k.f[0xD8 / 4] = k.f[0xDC / 4] = 0.5f;
        RoomLightTests::Record r{{x, 0.75f, z}, 4.0f, {k}};
        r.shape = static_cast<uintptr_t>(x);
        return r;
    };
    RoomLightTests::Room own, next;
    own.rec.push_back(plate(2.0f, 2.0f)), own.rec.push_back(plate(4.0f, 2.0f));
    next.rec.push_back(plate(2.0f, 8.0f));
    RtLampGpu::BlockerTable a, b;
    const int ha = a.Append(own), hb = b.Append(next);
    RtLampGpu::BlockerTable j = RtLampGpu::BlockerTable::Join({&a, ha}, {{&b, hb}});
    const V lamp1{2.0f, 2.5f, 2.0f}, floor1{2.0f, 0.0f, 2.0f}, lamp2{2.0f, 2.5f, 8.0f}, floor2{2.0f, 0.0f, 8.0f}, clear{9.0f, 0.0f, 9.0f};
    bool same = true;
    for (const V& l : {lamp1, lamp2})
        for (const V& p : {floor1, floor2, clear}) same = same && j.Occlude(0, l, p) == a.Occlude(ha, l, p);
    Check(same, "join: row 0 walks exactly the room's own records, a ceiling lamp's shadows bit for bit");
    Check(j.Occlude(0, lamp2, floor2) == 1.0f && j.Occlude(1, lamp2, floor2) < 0.05f, "join: row 1 also meets the next room's table");
    Check(j.Occlude(1, lamp1, floor1) == a.Occlude(ha, lamp1, floor1), "join: row 1 keeps the room's own records");
    Check(j.ShapeOf(1, 2) == 2 && j.ShapeOf(0, 1) == 4, "join: each record keeps its shape, own first");
    const float standing[3] = {2.0f, 0.85f, 8.0f};
    Check(j.FillOf(0, standing) == 0.0f && j.FillOf(1, standing) == RtLampGpu::kFurnitureFill, "join: a lamp on the next room's table keeps its fill from row 1");
    j.Offset(0, 2, 1.0f, 0.0f);
    Check(j.Occlude(1, lamp2, floor2) > 0.99f, "join: a carried record moves");
    RtLampGpu::BlockerTable empty = RtLampGpu::BlockerTable::Join({nullptr, -1}, {});
    Check(empty.rows.size() == 2 && empty.Occlude(0, lamp1, floor1) == 1.0f, "join: a room with no records walks none");
    // panes
    const std::vector<RtLampProxy::Pane> panes = {{0, 5, 1, 5, 0.5f, 2.0f}, {1, 5, 2, 5, 0.5f, 2.0f}, {0, 3, 2, 3, 0, 2.5f}};
    const float la[3] = {1.0f, 1.0f, 7.0f}, one[3] = {0.5f, 1.0f, 4.0f}, two[3] = {0.5f, 1.0f, 1.0f}, seam[3] = {1.0f, 1.0f, 4.0f}, over[3] = {0.5f, 3.0f, 4.1f}, side[3] = {6.0f, 1.0f, 4.0f};
    Check(RtLampProxy::PaneCount(panes, la, one) == 1 && RtLampProxy::PaneCount(panes, la, two) == 2, "panes: one a span crossed");
    Check(RtLampProxy::PaneCount(panes, la, seam) == 1, "panes: two spans side by side count once at their seam");
    Check(RtLampProxy::PaneCount(panes, la, over) == 0 && RtLampProxy::PaneCount(panes, la, side) == 0, "panes: over the head or past the span, none");
    Check(std::fabs(RtLampProxy::PaneShare(panes, la, two) - WallOpenings::kWindowPass * WallOpenings::kWindowPass) < 1e-6f, "panes: the share is the pass per pane");
    RtLampProxy::Lot lot;
    lot.stories.resize(1);
    RtLampProxy::Edge e;
    e.x0 = 2, e.z0 = 2, e.x1 = 2, e.z1 = 3, e.base = 1, e.open = true, e.glazed = 1, e.o.u0 = 0.25f, e.o.u1 = 0.75f, e.o.sill = 0.5f;
    e.o.head[2] = 2.4f;
    lot.stories[0].edges = {e};
    e.glazed = 0;
    lot.stories[0].edges.push_back(e);
    std::vector<RtLampProxy::Pane> got;
    RtLampProxy::PanesOf(lot, got);
    Check(got.size() == 1 && got[0].z0 == 2.25f && got[0].z1 == 2.75f && got[0].y0 == 1.5f && got[0].y1 == 3.4f, "panes: a glazed opening's span, sill and head; an arch gives none");
    // the tone: no step, and the game's curve past the knee
    bool rising = true, sameFar = true;
    float last = 0.0f;
    for (int i = 1; i < 4000; i++) {
        const float m = static_cast<float>(i) * 2.5e-5f, x[3] = {m, m, m};
        float c[3];
        GpuLampLaw::ToneSoft(x, c);
        rising = rising && c[1] > last;
        last = c[1];
    }
    for (const float m : {0.03f, 0.2f, 1.0f}) {
        const float x[3] = {m, m, m};
        float c[3], g[3];
        GpuLampLaw::ToneSoft(x, c), GpuLampLaw::Tone(x, g);
        sameFar = sameFar && std::fabs(c[1] - g[1]) < 1e-6f;
    }
    const float faint[3] = {0.003f, 0.003f, 0.003f};
    float c[3];
    GpuLampLaw::ToneSoft(faint, c);
    Check(rising, "tone: more light never shows darker");
    Check(sameFar && c[1] == 0.003f, "tone: the game's curve past 0.02, x under 0.004");
}
void BlockerTableChecks() {
    using namespace RoomLightTests;
    {
        GpuLampLaw::Lamp tube{};
        const float up[3] = {0.0f, 1.0f, 0.0f}, down[3] = {0.0f, -1.0f, 0.0f}, side[3] = {1.0f, 0.0f, 0.0f};
        float y = 2.0f;
        GpuLampLaw::TubeShape(tube, up, 1.0f);
        Check(GpuLampLaw::TubeStep(tube, y) == 0.25f && y == 2.125f, "tube tests: an upright tube steps up from its low end");
        y = 2.0f;
        GpuLampLaw::TubeShape(tube, down, 1.0f);
        Check(GpuLampLaw::TubeStep(tube, y) == 0.25f && y == 1.125f, "tube tests: a tube pointing down steps from its other end");
        y = 2.0f;
        GpuLampLaw::TubeShape(tube, side, 1.0f);
        Check(GpuLampLaw::TubeStep(tube, y) == 0.0f && y == 2.0f, "tube tests: a level tube keeps the game's one point");
        GpuLampLaw::Lamp lamp{};
        Check(GpuLampLaw::TubeStep(lamp, y) == 0.0f, "tube tests: a lamp keeps the game's one point");
    }
    std::mt19937 rng(20261102);
    std::uniform_real_distribution<float> U(-1.0f, 1.0f), P(0.0f, 1.0f);
    const auto unit = [&]() {
        const V v{U(rng), U(rng), U(rng)};
        const float l = std::sqrt(dot(v, v));
        return V{v.x / l, v.y / l, v.z / l};
    };
    const auto cross = [](V p, V q) { return V{p.y * q.z - p.z * q.y, p.z * q.x - p.x * q.z, p.x * q.y - p.y * q.x}; };
    const auto put = [](RoomLightTests::Blocker& k, int off, V v) { k.f[off / 4] = v.x, k.f[off / 4 + 1] = v.y, k.f[off / 4 + 2] = v.z; };
    std::vector<RoomLightTests::Room> rooms(6);
    int blockers = 0, unknown = 0;
    for (RoomLightTests::Room& room : rooms) {
        const int recs = 3 + static_cast<int>(P(rng) * 8);
        for (int r = 0; r < recs; r++) {
            RoomLightTests::Record rc;
            rc.c = {U(rng) * 2, U(rng) * 2, U(rng) * 2};
            rc.r2 = 0.5f + P(rng) * 3;
            const int nb = static_cast<int>(P(rng) * 5);
            for (int i = 0; i < nb; i++) {
                RoomLightTests::Blocker k{};
                for (float& f : k.f) f = U(rng) * 9; // (every other field noise: the "miss" offsets among them)
                const V c{rc.c.x + U(rng) * 0.5f, rc.c.y + U(rng) * 0.5f, rc.c.z + U(rng) * 0.5f}, N = unit();
                const V u = cross(N, unit());
                const float ul = std::sqrt(dot(u, u));
                const V Uu{u.x / ul, u.y / ul, u.z / ul}, Vv = cross(N, Uu);
                const V c2{c.x + N.x * 0.3f, c.y + N.y * 0.3f, c.z + N.z * 0.3f};
                const float sel = P(rng);
                k.f[4] = P(rng) < 0.5f ? 0.0f : (P(rng) < 0.5f ? 1.0f : 2.5f);
                if (sel < 0.45f) {
                    k.vt = kQuadVt;
                    put(k, 0x70, c), put(k, 0x80, c2), put(k, 0x90, N), put(k, 0xA0, Uu), put(k, 0xB0, Vv);
                    for (int j = 0; j < 4; j++) k.f[0xD0 / 4 + j] = 0.2f + P(rng);
                } else if (sel < 0.9f) {
                    k.vt = kDiscVt;
                    put(k, 0x20, c), put(k, 0x30, c2), put(k, 0x40, N);
                    k.f[0x70 / 4] = 0.05f + P(rng) * 0.5f;
                } else {
                    k.vt = 0x00FF0000u;
                    unknown++;
                }
                rc.b.push_back(k);
                blockers++;
            }
            room.rec.push_back(rc);
        }
    }
    RtLampGpu::BlockerTable table;
    std::vector<int> headers;
    for (const RoomLightTests::Room& r : rooms) headers.push_back(table.Append(r));
    static const int kQuadF[] = {4, 28, 29, 30, 32, 33, 34, 36, 37, 38, 40, 41, 42, 44, 45, 46, 52, 53, 54, 55, 56, 57, 58, 60, 61, 62};
    static const int kDiscF[] = {4, 8, 9, 10, 12, 13, 14, 16, 17, 18, 28, 32, 33, 34, 36, 37, 38};
    bool fieldsOk = true;
    for (size_t r = 0; r < rooms.size(); r++) {
        const auto& h = table.rows[static_cast<size_t>(headers[r])];
        fieldsOk &= static_cast<size_t>(h.v[1]) == rooms[r].rec.size();
        for (size_t i = 0; i < rooms[r].rec.size(); i++) {
            const RoomLightTests::Record& rc = rooms[r].rec[i];
            const auto& c = table.rows[static_cast<size_t>(h.v[0]) + 2 * i];
            const auto& ix = table.rows[static_cast<size_t>(h.v[0]) + 2 * i + 1];
            fieldsOk &= c.v[0] == rc.c.x && c.v[1] == rc.c.y && c.v[2] == rc.c.z && c.v[3] == rc.r2;
            int j = 0;
            for (const RoomLightTests::Blocker& k : rc.b) {
                if (k.vt != kQuadVt && k.vt != kDiscVt) continue;
                const RoomLightTests::Blocker back = table.BlockerAt(static_cast<int>(ix.v[0]) + j++ * RtLampGpu::BlockerTable::kBlockerRows);
                fieldsOk &= back.vt == k.vt;
                if (k.vt == kQuadVt)
                    for (int f : kQuadF) fieldsOk &= std::memcmp(&back.f[f], &k.f[f], 4) == 0;
                else
                    for (int f : kDiscF) fieldsOk &= std::memcmp(&back.f[f], &k.f[f], 4) == 0;
            }
            fieldsOk &= static_cast<int>(ix.v[1]) == j;
        }
    }
    Check(fieldsOk && table.skipped == unknown, "blocker table: the rows give back each record and every field the rectangle and disc tests read");
    {
        // a side table's top (a disc 0.36 m wide, 0.77 m up) kept at its new place 1 m away: it shades the floor under that place,
        // as a fresh keep's would, and no longer the old one
        const auto tableAt = [&](float x) {
            RoomLightTests::Room room;
            RoomLightTests::Record rc;
            rc.c = {x, 0.77f, 0.0f}, rc.r2 = 1.0f;
            RoomLightTests::Blocker k{};
            k.vt = kDiscVt;
            put(k, 0x20, V{x, 0.77f, 0.0f}), put(k, 0x30, V{x, 0.79f, 0.0f}), put(k, 0x40, V{0.0f, 1.0f, 0.0f});
            k.f[0x70 / 4] = 0.357f * 0.357f;
            rc.b.push_back(k);
            room.rec.push_back(rc);
            return room;
        };
        const RoomLightTests::Room moved = tableAt(1.0f);
        RtLampGpu::BlockerTable live;
        const int h = live.Append(moved);
        const V lamp{1.0f, 1.34f, 0.0f};
        int unk = 0;
        const float under = live.Occlude(h, lamp, V{1.0f, 0.0f, 0.1f}), old = live.Occlude(h, lamp, V{-1.0f, 0.0f, 0.1f}); // (where it stood: its shadow there)
        Check(under < 0.5f && old == 1.0f && under == RoomLightTests::occlude(moved, lamp, V{1.0f, 0.0f, 0.1f}, &unk),
              "blocker table: a table kept at its new place shades the floor under it and no longer under the old one");
        // (the drag's dropped top, StandsOnFurniture) a lamp on the table's top stands on it; a chandelier over it or a lamp beside it does not
        const float onTop[3] = {1.0f, 1.34f, 0.0f}, over[3] = {1.0f, 2.5f, 0.0f}, beside[3] = {2.2f, 1.34f, 0.0f}, below[3] = {1.0f, 0.3f, 0.0f};
        const auto stands = [&](const float* p) { return RoomLightTests::StandsOn(moved.rec[0], p); };
        // (13:56 scene, record 23) a sofa: three upright or tilted rectangles in a sphere of r 1.69 m whose centre is 1.2 m off the lamp
        RoomLightTests::Record sofa;
        sofa.c = {-0.87f, 0.0f, -0.82f}, sofa.r2 = 1.69f * 1.69f;
        for (const V n : {V{0.87f, 0.0f, -0.5f}, V{0.39f, -0.62f, 0.68f}, V{0.5f, 0.0f, 0.87f}}) {
            RoomLightTests::Blocker k{};
            k.vt = kQuadVt;
            put(k, 0x70, sofa.c), put(k, 0x90, n), put(k, 0xA0, V{-n.z, 0.0f, n.x}), put(k, 0xB0, V{0.0f, 1.0f, 0.0f});
            k.f[0xD0 / 4] = k.f[0xD8 / 4] = 1.28f, k.f[0xD4 / 4] = k.f[0xDC / 4] = 0.61f;
            sofa.b.push_back(k);
        }
        const float bySofa[3] = {0.0f, 0.86f, 0.0f};
        Check(stands(onTop) && !stands(over) && !stands(beside) && !stands(below) && !RoomLightTests::StandsOn(sofa, bySofa),
              "stands on: a table lamp yes; a chandelier over the table, a lamp beside or under it, or beside a sofa no");
        // ([CarryTable], CarryOf) the 16:25 recording of 03/11: the lamp stood on the floor, 0.54 m up, under no top (no record
        // decided, the log's "shape 00000000"); on its table it is carried only while the table's own entry moved
        Check(PhysLampPick::CarryOf(stands(below), false) == 0 && PhysLampPick::CarryOf(stands(onTop), true) == 2 &&
                  PhysLampPick::CarryOf(stands(onTop), false) == 1 && PhysLampPick::CarryOf(stands(over), true) == 0,
              "carry: a lamp on the floor stands on no record; on its table the table is carried when it moved, else left in place");
        // (MarkUnchanged, TailFanout) the physical mode drops the unchanged mark of a lamp its GPU took lately and parks a GPU
        // edit's fan-out until quiet; Classic, other lamps and other walks keep both
        Check(PhysLampPick::MarkUnchanged(false, false) && !PhysLampPick::MarkUnchanged(false, true) && !PhysLampPick::MarkUnchanged(true, false) &&
                  PhysLampPick::TailFanout(true, false) && PhysLampPick::TailFanout(false, true) && !PhysLampPick::TailFanout(true, true),
              "drag flicker: physical drops a dragged lamp's unchanged mark and parks a GPU edit's fan-out; Classic and other walks keep them");
        // (FillOf) only the lamp on the table's top keeps the furniture fill under furniture; a chandelier over it none. A carried
        // table keeps it for its lamp where both are drawn now, not where they were
        RtLampGpu::BlockerTable carried = live;
        carried.Offset(h, 0, 2.0f, 0.0f);
        const float onCarried[3] = {3.0f, 1.34f, 0.0f};
        Check(live.FillOf(h, onTop) == RtLampGpu::kFurnitureFill && live.FillOf(h, over) == 0.0f && live.FillOf(-1, onTop) == 0.0f &&
                  carried.FillOf(h, onCarried) == RtLampGpu::kFurnitureFill && carried.FillOf(h, onTop) == 0.0f && RtLampGpu::FurnitureFill(0.0f, 0.0f) == 0.0f &&
                  RtLampGpu::FurnitureFill(1.0f, RtLampGpu::kFurnitureFill) == 1.0f && RtLampGpu::FurnitureFill(0.0f, RtLampGpu::kFurnitureFill) == RtLampGpu::kFurnitureFill,
              "furniture fill: a table lamp's own top lifts its shadow to the fill, carried or not; any other lamp keeps the game's 3D test");
        // (Offset, the table drag) the table of a solve at x = 1 carried by the lamp's move (+2 m) shades exactly as the table read at
        // x = 3, the edged test too; the copy it came from is unchanged
        {
            RtLampGpu::BlockerTable at3;
            const int h3 = at3.Append(tableAt(3.0f));
            const V lamp3{3.0f, 1.34f, 0.0f};
            bool same = true;
            for (float x = 0.0f; x <= 6.0f; x += 0.05f)
                for (const float edge : {0.0f, 0.1f})
                    same = same && std::fabs(carried.Occlude(h, lamp3, V{x, 0.0f, 0.1f}, edge) - at3.Occlude(h3, lamp3, V{x, 0.0f, 0.1f}, edge)) < 1e-4f;
            Check(same && carried.Occlude(h, lamp3, V{3.0f, 0.0f, 0.1f}) < 0.01f && carried.Occlude(h, lamp3, V{1.0f, 0.0f, 0.1f}) == 1.0f &&
                      live.Occlude(h, lamp, V{1.0f, 0.0f, 0.1f}) < 0.01f,
                  "table drag: the solve's table carried by its lamp's move shades as the table read at its new place; none left at the old one");
        }
        // (legs, Record::posts) a 0.75 m square top, its sphere the game's (it leaves out the corners of the band), four legs
        {
            const auto legged = [&](float x, bool legs) {
                RoomLightTests::Room room;
                RoomLightTests::Record rc;
                rc.c = {x, 0.75f, 0.0f}, rc.r2 = 0.726f * 0.726f;
                RoomLightTests::Blocker k{};
                k.vt = RoomLightTests::kQuadVt;
                put(k, 0x70, V{x, 0.75f, 0.0f}), put(k, 0x90, V{0.0f, 1.0f, 0.0f}), put(k, 0xA0, V{1.0f, 0.0f, 0.0f}), put(k, 0xB0, V{0.0f, 0.0f, 1.0f});
                for (int i = 0; i < 4; i++) k.f[0xD0 / 4 + i] = 0.376f;
                rc.b.push_back(k);
                if (legs)
                    for (const float sx : {-0.335f, 0.335f})
                        for (const float sz : {-0.335f, 0.335f}) rc.posts.push_back({{x + sx, 0.03f, sz}, {x + sx, 0.7f, sz}, 0.02f});
                room.rec.push_back(rc);
                return room;
            };
            RtLampGpu::BlockerTable t, plain, at3;
            const RoomLightTests::Room room = legged(0.0f, true);
            const int h = t.Append(room), hp = plain.Append(legged(0.0f, false)), h3 = at3.Append(legged(3.0f, true));
            const auto& row = t.rows[static_cast<size_t>(t.rows[static_cast<size_t>(h)].v[0]) + 1];
            const int firstPost = static_cast<int>(row.v[0]) + RtLampGpu::BlockerTable::kBlockerRows;
            const RoomLightTests::Blocker back = t.BlockerAt(firstPost);
            const RoomLightTests::Post& p0 = room.rec[0].posts[0];
            Check(static_cast<int>(row.v[1]) == 5 && back.vt == RoomLightTests::kPostVt && back.v(0x20).y == p0.a.y && back.v(0x30).x == p0.b.x && back.f[0x70 / 4] == p0.r,
                  "legs: a post's rows give back its ends and radius after the record's blockers");
            // a lamp past kPostGateM over the legs' top: every texel as the top alone, bit for bit, past the game's sphere too. A ceiling lamp under
            // it: every texel no leg's streak reaches the same, bit for bit, and a texel a streak reaches only darker, by a soft share
            bool high = true, ceiling = true;
            int culled = 0, streaked = 0, clear = 0;
            for (const V lamp : {V{0.0f, 3.8f, 0.0f}, V{1.2f, 3.75f, 0.8f}, V{0.0f, 2.6f, 0.0f}, V{1.2f, 2.3f, 0.8f}})
                for (float x = -2.5f; x <= 2.5f; x += 0.05f)
                    for (float z = -2.5f; z <= 2.5f; z += 0.05f)
                        for (const float edge : {0.0f, 0.1f}) {
                            const float a = t.Occlude(h, lamp, V{x, 0.0f, z}, edge, 0.15f), b = plain.Occlude(hp, lamp, V{x, 0.0f, z}, edge, 0.15f);
                            float legs = 1.0f;
                            for (int i = 0; i < 4; i++) legs *= RtLampGpu::PostT(t.BlockerAt(firstPost + i * RtLampGpu::BlockerTable::kBlockerRows), lamp, V{x, 0.0f, z}, 0.15f);
                            if (lamp.y > 0.7f + RtLampGpu::kPostGateM) high = high && a == b;
                            else ceiling = ceiling && (legs == 1.0f ? a == b : a <= b && legs > 0.5f);
                            culled += a == 1.0f, streaked += legs < 1.0f, clear += legs == 1.0f;
                        }
            Check(high && culled > 0, "legs: a lamp past the gate's shadow of a legged table is the top's alone, bit for bit");
            Check(ceiling && streaked > 0 && clear > streaked, "legs: a ceiling lamp's top and bands stay bit for bit where no streak reaches, and a streak only darkens, softly");
            // a lamp beside the table at 1.3 m: the floor past the near leg, on the line from the lamp through its middle, takes a streak; off it none
            const V side{-1.0f, 1.3f, 0.0f}, past{-1.0f + 0.665f * 1.3f / 0.95f, 0.0f, 0.335f * 1.3f / 0.95f}, off{-0.5f, 0.0f, 1.5f};
            const float streak = t.Occlude(h, side, past, 0.0f, 0.15f), bare = plain.Occlude(hp, side, past, 0.0f, 0.15f);
            Check(streak < bare - 0.05f && streak > 0.3f * bare && t.Occlude(h, side, off, 0.0f, 0.15f) == plain.Occlude(hp, side, off, 0.0f, 0.15f),
                  "legs: a lamp beside the table casts a soft streak past a leg and none off it");
            // a lamp on the top keeps its fill, a lamp over a post alone stands on nothing
            const float onTop[3] = {0.0f, 1.2f, 0.0f}, offTop[3] = {0.335f, 0.5f, 0.335f};
            Check(t.FillOf(h, onTop) == RtLampGpu::kFurnitureFill && t.FillOf(h, offTop) == 0.0f && !RoomLightTests::StandsOn(back, onTop),
                  "legs: a post is no top: stands on and the furniture fill read the blockers alone");
            // (Offset) the legs go with a dragged table
            RtLampGpu::BlockerTable moved = t;
            moved.Offset(h, 0, 3.0f, 0.0f);
            bool same = true;
            const V side3{2.0f, 1.3f, 0.0f};
            for (float x = 1.0f; x <= 6.0f; x += 0.05f)
                for (float z = -1.5f; z <= 1.5f; z += 0.1f) same = same && std::fabs(moved.Occlude(h, side3, V{x, 0.0f, z}, 0.0f, 0.15f) - at3.Occlude(h3, side3, V{x, 0.0f, z}, 0.0f, 0.15f)) < 1e-3f;
            Check(same, "legs: a carried table's legs shade as the table read at its new place");
            // the baked table: sorted for its search; a model's capsules through a matrix turned a quarter about y and moved
            bool sorted = true;
            for (size_t i = 1; i < std::size(ObjectLegs::kModels); i++) sorted = sorted && ObjectLegs::kModels[i - 1].lite < ObjectLegs::kModels[i].lite;
            const ObjectLegs::Model& first = ObjectLegs::kModels[0];
            const ObjectLegs::Capsule& c0 = ObjectLegs::kCapsules[first.first];
            const float m[16] = {0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 10, 2, 20, 1};
            std::vector<RoomLightTests::Post> placed;
            ObjectLegs::Place(first, m, placed);
            Check(sorted && ObjectLegs::Baked(first.lite) == &first && !ObjectLegs::Baked(first.lite + 1) && static_cast<int>(placed.size()) == first.count &&
                      std::fabs(placed[0].a.x - (10.0f + c0.z0 * 1e-3f)) < 1e-4f && std::fabs(placed[0].a.y - (2.0f + c0.y0 * 1e-3f)) < 1e-4f && std::fabs(placed[0].a.z - (20.0f - c0.x0 * 1e-3f)) < 1e-4f,
                  "legs: the baked table finds its models and places their capsules as the blockers' matrix does");
        }
        // (true-size edges, Params::edgeR) a 0.8 m square table top at 0.75 m under a lamp at 1.33 m: its floor shadow ends where the
        // top's edge projects (0.4 x 1.33 / 0.58 = 0.92 m), soft over the lamp ball's width there; the game's band shades to ~1.7 m
        {
            RoomLightTests::Room sq;
            RoomLightTests::Record rc;
            rc.c = {0.0f, 0.75f, 0.0f}, rc.r2 = 1.0f;
            RoomLightTests::Blocker k{};
            k.vt = RoomLightTests::kQuadVt;
            put(k, 0x70, V{0.0f, 0.75f, 0.0f}), put(k, 0x90, V{0.0f, 1.0f, 0.0f}), put(k, 0xA0, V{1.0f, 0.0f, 0.0f}), put(k, 0xB0, V{0.0f, 0.0f, 1.0f});
            for (int i = 0; i < 4; i++) k.f[0xD0 / 4 + i] = 0.4f;
            rc.b.push_back(k);
            sq.rec.push_back(rc);
            RtLampGpu::BlockerTable t;
            const int hs = t.Append(sq);
            const V top{0.0f, 1.33f, 0.0f};
            const float r = 0.15f, in = t.Occlude(hs, top, V{0.5f, 0.0f, 0.0f}, r), edge = t.Occlude(hs, top, V{0.92f, 0.0f, 0.0f}, r),
                        out = t.Occlude(hs, top, V{1.3f, 0.0f, 0.0f}, r), game = t.Occlude(hs, top, V{1.3f, 0.0f, 0.0f});
            Check(in < 0.01f && std::fabs(edge - 0.5f) < 0.1f && out > 0.999f && game < 0.9f,
                  "true-size edges: a square table's floor shadow ends at its top's projected edge, half shaded there; the game's band shades past it");
            // (the S edge: flat at both its ends, no crease in the floor; half shaded at the edge itself)
            Check(RtLampGpu::EdgeOp(0.5f, 0.5f, 0.1f) == 0.5f && RtLampGpu::EdgeOp(0.399f, 0.5f, 0.1f) == 1.0f && RtLampGpu::EdgeOp(0.601f, 0.5f, 0.1f) == 0.0f &&
                      RtLampGpu::EdgeOp(0.41f, 0.5f, 0.1f) - RtLampGpu::EdgeOp(0.42f, 0.5f, 0.1f) < RtLampGpu::EdgeOp(0.49f, 0.5f, 0.1f) - RtLampGpu::EdgeOp(0.5f, 0.5f, 0.1f),
                  "true-size edges: an S from shaded to lit, flat at its ends");
            // (the fill's glow: more towards the shadow's edge than at its middle, nothing in the light)
            Check(t.OccludeFill(hs, top, V{0.5f, 0.0f, 0.0f}, r) < t.OccludeFill(hs, top, V{0.8f, 0.0f, 0.0f}, r) && t.OccludeFill(hs, top, V{1.3f, 0.0f, 0.0f}, r) > 0.999f,
                  "furniture fill: its glow rises from the shadow's middle to its edge");
            const V onFloor{0.95f, 0.0f, 0.0f}; // (a disc of the same table as the offset test's: its edge at its radius, soft as the ball)
            Check(live.Occlude(h, lamp, V{1.0f, 0.0f, 0.1f}, r) < 0.01f && live.Occlude(h, lamp, V{2.5f, 0.0f, 0.1f}, r) == 1.0f &&
                      t.Occlude(hs, top, onFloor, 0.0f) == t.Occlude(hs, top, onFloor),
                  "true-size edges: a disc table shades under it and not far off; edgeR 0 is the game's test");
            // (sphere_cut_fade) a sphere of 0.3 m around that top: the band, 0.75 m wide, stops at a step on the game's test and fades with
            // FadeCut; the shadow's core, true-size edges and a Join keep what they had, and posts enlarging the record's sphere move nothing
            RoomLightTests::Room cut = sq;
            cut.rec[0].r2 = 0.09f;
            RoomLightTests::Room posted = cut;
            posted.rec[0].posts.push_back({{-2.0f, 0.0f, 0.0f}, {-2.0f, 0.7f, 0.0f}, 0.03f});
            RtLampGpu::BlockerTable step, fade, fadePosted;
            const int hc = step.Append(cut), hf = fade.Append(cut), hp = fadePosted.Append(posted);
            fade.FadeCut(hf), fadePosted.FadeCut(hp);
            const RtLampGpu::BlockerTable join = RtLampGpu::BlockerTable::Join({&fade, hf}, {});
            float stepJump = 0.0f, fadeJump = 0.0f;
            bool same = true;
            for (float x = 0.0f; x < 3.0f; x += 0.01f) {
                const V a{x, 0.0f, 0.0f}, b{x + 0.01f, 0.0f, 0.0f};
                stepJump = std::max(stepJump, std::fabs(step.Occlude(hc, top, a) - step.Occlude(hc, top, b)));
                fadeJump = std::max(fadeJump, std::fabs(fade.Occlude(hf, top, a) - fade.Occlude(hf, top, b)));
                same = same && step.Occlude(hc, top, a, r) == fade.Occlude(hf, top, a, r) && join.Occlude(0, top, a) == fade.Occlude(hf, top, a) &&
                       join.Occlude(1, top, a) == fade.Occlude(hf, top, a) && fadePosted.Occlude(hp, top, a) == fade.Occlude(hf, top, a);
            }
            std::printf("sphere fade: largest jump on 1 cm of floor %.3f with the step, %.3f with the fade\n", stepJump, fadeJump);
            Check(stepJump > 0.2f && fadeJump < 0.08f && same && step.Occlude(hc, top, V{0.3f, 0.0f, 0.0f}) == fade.Occlude(hf, top, V{0.3f, 0.0f, 0.0f}),
                  "sphere fade: the band's end is a fade, its core, true-size edges, a Join and posts unchanged");
        }
    }
    RtLampGpu::BlockerTable wide;
    wide.rows.resize(5000);
    int tx = 0, ty = 0;
    wide.Texel(4097, tx, ty);
    Check(wide.Width() == 4096 && wide.Height() == 2 && tx == 1 && ty == 1 && table.Width() == 4096,
          "blocker table: row i sits at texel (i % 4096, i / 4096)");
    long n = 0, same = 0, shaded = 0;
    for (int s = 0; s < 200000; s++) {
        const size_t r = static_cast<size_t>(P(rng) * static_cast<float>(rooms.size())) % rooms.size();
        const V a{U(rng) * 3, U(rng) * 3, U(rng) * 3}, b{U(rng) * 3, U(rng) * 3, U(rng) * 3};
        int unk = 0;
        const float want = occlude(rooms[r], a, b, &unk), got = table.Occlude(headers[r], a, b);
        n++;
        same += std::memcmp(&want, &got, 4) == 0 ? 1 : 0;
        shaded += want < 1.0f ? 1 : 0;
    }
    std::printf("blocker table: %zu rows (%d blockers, %d of another vtable left out); occlude from the rows %ld / %ld identical (%ld shaded)\n", table.rows.size(),
                blockers, table.skipped, same, n, shaded);
    Check(same == n && shaded > n / 10, "blocker table: occlude from the rows is RoomLightTests::occlude bit for bit");
    // its texture: always kMaxWidth wide (the check above), only taller
    {
        using RtLampGpu::BlockerTable;
        const int w = BlockerTable::kMaxWidth;
        bool ok = BlockerTable{}.Height() == 1;
        ok = ok && BlockerTable::TexHeight(0, 0) == 1 && BlockerTable::TexHeight(1, 0) == 1 && BlockerTable::TexHeight(w, 0) == 1 &&
             BlockerTable::TexHeight(w + 1, 0) == 2 && BlockerTable::TexHeight(5, 3) == 3 && BlockerTable::TexHeight(3 * w + 1, 3) == 4;
        Check(ok, "blocker table: its texture is 4096 wide and grows only in height");
    }
}

// ---- (physical) the instant day/night switch: the cache rule (PhaseCache) over the 16-54-11 and 17-11-35 sessions' switches ----
namespace PC = OutdoorDeltaModel::PhaseCache;
struct PcRoom {
    size_t bytes = 0;
    int room = 0;
    PC::Stamp start, kept;
};
// A session's night level (fades of 0.6 s from the departures of its log) and per room its kept ticks (each solve begun 250 ms
// before), in ms from the session's minute
struct Fade {
    uint32_t at;
    float from, to;
};
struct Session {
    std::vector<Fade> fades;
    std::vector<std::vector<uint32_t>> kept;
    uint32_t end;
};
constexpr uint32_t kFadeMs = 600, kModeLead = 300; // (the window lights' mode change ~0.2-0.3 s before the level moves: 187-219 ms in 17-11-35's log)
float SessionLevel(const Session& ss, uint32_t t) {
    float level = 0.0f;
    for (const Fade& f : ss.fades) {
        if (t < f.at) break;
        level = t >= f.at + kFadeMs ? f.to : f.from + (f.to - f.from) * static_cast<float>(t - f.at) / kFadeMs;
    }
    return level;
}
struct SwitchResult {
    PC::End to;
    int rooms = 0, swapped = 0, solved = 0, never = 0, reasoned = 0, sent = 0;
    std::map<std::string, int> why;
};
// The light tree thread's part (PhaseSwitchDue, KeepEnd) driven by the session: the game's solves as its log kept them (each begun
// 250 ms before), the rooms Apex sends at a departure solved from 0.9 s after it, 250 ms apart, in turn (and once more when that
// solve began in the fade). lampsAsKept(room, t): whether the room's lamps are as its cached solve kept them at the swap
// 16-54-11 (16:52:00 + ms): to night at the load (8.3-8.86 s), to day 65.88, to night 95.78, to day 102.47, to night 134.09, to
// day 137.13; the 13 rooms of lot CF2DEA20, the first five kept only in the load's fade
const Session kSession1654 = {
    {{8300, 0.0f, 1.0f}, {65877, 1.0f, 0.0f}, {95778, 0.0f, 1.0f}, {102469, 1.0f, 0.0f}, {134086, 0.0f, 1.0f}, {137125, 1.0f, 0.0f}},
    {
        {8418}, {8454}, {8462}, {8492}, {8615}, // s3r10, s2r9, s2r11, s1r4, s0r13
        {9312, 71806, 79400, 98868, 105284, 134010, 138491},                                                  // s2r7
        {10287, 66743, 81552, 100524, 116894, 136498, 147951},                                                // s1r1
        {10430, 70669, 78541, 81049, 82714, 83846, 90980, 97766, 98389, 104130, 143504, 147719},              // s1r2
        {10552, 79621, 81417, 101115, 102057, 115548, 116675, 135791, 136346, 140721, 141440},                // s1r3
        {10569, 79524, 81256, 100271, 101472, 114847, 116120, 135374, 136004, 140294, 141051},                // s1r8
        {10769, 72255, 79226, 99121, 112759, 134302, 138939},                                                 // s2r6
        {10859, 73499, 79111, 82977, 84236, 99668, 113899, 134843, 139652},                                   // s2r5
        {10871, 73764, 81722, 99807, 114145, 135038, 139896},                                                 // s2r12
    },
    160000};
// 17-11-35 (17:12:00 + ms): night from the load, to day 104.69, to night 119.10, to day 121.63, to night 153.69 (the departure
// lines); the same 13 rooms, the first five solved first at the switch to day
const Session kSession1711 = {
    {{0, 1.0f, 1.0f}, {104687, 1.0f, 0.0f}, {119102, 0.0f, 1.0f}, {121628, 1.0f, 0.0f}, {153690, 0.0f, 1.0f}},
    {
        {104632, 104961, 119302, 122118, 153922},                                                                // s3r10
        {104789, 120293, 122446, 127862, 154957},                                                                // s0r13
        {105003, 119328, 122139, 153949},                                                                        // s2r9
        {105011, 121354, 124108, 155717},                                                                        // s2r11
        {105052, 121480, 124652, 155727},                                                                        // s1r4
        {43024, 44536, 108560, 119257, 119962, 120785, 123151, 153637, 154454, 155244, 158132, 160614},          // s2r7
        {44555, 114095, 120520, 127587, 155114, 158348, 162346},                                                 // s1r1
        {44733, 116255, 117812, 127352, 158694, 162952},                                                         // s1r2
        {44857, 109138, 119634, 123445, 153874, 158198, 160711},                                                 // s2r6
        {44914, 110290, 121184, 124024, 155644, 158283, 160865},                                                 // s2r5
        {44932, 111223, 112564, 113799, 124644, 125371, 161014, 161169},                                         // s1r8
        {45000, 111855, 113168, 125021, 125713, 161342},                                                         // s1r3
        {47776, 110573, 121346, 124100, 167057},                                                                 // s2r12
    },
    170000};
template <class LampsAsKept> std::vector<SwitchResult> ReplaySession(const Session& ss, const LampsAsKept& lampsAsKept, int& filledTotal, int& droppedTotal) {
    const std::vector<std::vector<uint32_t>>& kept = ss.kept;
    const int n = static_cast<int>(kept.size());
    std::multimap<uint32_t, int> solves; // kept tick -> room (begun 250 ms before)
    for (int r = 0; r < n; r++)
        for (const uint32_t t : kept[r]) solves.emplace(t, r);
    std::map<uint64_t, PC::Slots<std::shared_ptr<const PcRoom>>> cache;
    std::map<uint64_t, std::shared_ptr<const PcRoom>> now; // g_kept
    std::map<int, PC::Stamp> running;
    std::map<int, uint32_t> sentAt;
    std::set<int> resent;
    PC::Clock clk;
    uint32_t seen[2] = {}, depSeen = 0, switchesSeen = 0, burst = 0;
    std::vector<SwitchResult> out;
    struct Open {
        bool on = false;
        uint32_t from = 0;
        std::map<int, std::pair<bool, const char*>> shown; // room -> swapped, else its solve landed (why it was not swapped)
        std::map<int, const char*> why;
        std::set<int> sent;
        size_t index = 0;
    } open;
    const auto close = [&]() {
        if (!open.on) return;
        SwitchResult& r = out[open.index];
        for (int room = 0; room < n; room++) {
            if (!now.count(room)) continue;
            r.rooms++;
            const auto s = open.shown.find(room);
            const char* why = open.why.count(room) ? open.why[room] : nullptr;
            if (s != open.shown.end() && s->second.first) r.swapped++;
            else {
                (s != open.shown.end() ? r.solved : r.never)++;
                r.reasoned += why ? 1 : 0;
                r.why[why ? why : "(none)"]++;
            }
            r.sent += open.sent.count(room) ? 1 : 0;
        }
        open.on = false;
    };
    for (uint32_t t = 0; t < ss.end; t += 16) {
        for (const Fade& f : ss.fades)
            if (f.at >= kModeLead && t <= f.at - kModeLead && f.at - kModeLead < t + 16) burst = (f.at - kModeLead) | 1;
        clk.Observe(PC::EndOf(SessionLevel(ss, t)), t, SessionLevel(ss, t));
        // the solves begun and kept in this frame
        for (auto it = solves.begin(); it != solves.end() && it->first < t + 266;) {
            const int room = it->second;
            if (it->first - 250 >= t && it->first - 250 < t + 16) running[room] = PC::StampOf(clk, it->first - 250);
            if (it->first >= t && it->first < t + 16) {
                auto k = std::make_shared<PcRoom>();
                k->room = room, k->bytes = 1000, k->start = running[room], k->kept = PC::StampOf(clk, it->first);
                now[room] = k;
                if (PC::Whole(k->start, k->kept)) {
                    auto& slot = cache[room];
                    slot.at[k->kept.heading] = k, slot.keptAt[k->kept.heading] = it->first, slot.start[k->kept.heading] = k->start.tick;
                }
                if (sentAt.count(room) && static_cast<int32_t>(k->start.tick - sentAt[room]) >= 0 && k->kept.at != PC::kTwilight && !PC::Whole(k->start, k->kept) &&
                    resent.insert(room).second)
                    solves.emplace(it->first + 300, room);
                if (open.on && static_cast<int32_t>(k->start.tick - open.from) >= 0) open.shown.try_emplace(room, false, nullptr);
                it = solves.erase(it);
            } else
                ++it;
        }
        // departures (PhaseDeparture)
        for (int e = 0; e < 2; e++) {
            if (clk.leftN[e] == seen[e]) continue;
            const bool missed = clk.leftN[e] - seen[e] > 1 || clk.departures - depSeen > 1;
            seen[e] = clk.leftN[e];
            const uint32_t mode = PC::ModeChange(clk.leftAt[e], burst);
            droppedTotal += PC::PurgeDeparture(cache, static_cast<PC::End>(e), mode, missed);
            const PC::End o = e ? PC::kDay : PC::kNight;
            sentAt.clear(), resent.clear();
            int i = 0;
            for (const auto& [room, k] : now) {
                if (!missed && PC::FillOk(k->start, k->kept, static_cast<PC::End>(e), clk.departures - 1, mode)) {
                    auto& slot = cache[room];
                    if (slot.at[e] != k) slot.at[e] = k, slot.keptAt[e] = k->kept.tick, slot.start[e] = k->start.tick, filledTotal++;
                }
                if (!cache.count(room) || !cache[room].at[o]) sentAt[static_cast<int>(room)] = t, solves.emplace(t + 900 + 250 * i++, static_cast<int>(room));
            }
        }
        depSeen = clk.departures;
        // the arrival of a quick switch: every room tried (the swaps one per update land within frames: taken at once here)
        if (clk.switches != switchesSeen) {
            switchesSeen = clk.switches;
            close();
            open = Open{};
            open.on = true, open.from = clk.switchFrom, open.index = out.size();
            for (const auto& [room, at] : sentAt) open.sent.insert(room);
            SwitchResult r;
            r.to = clk.target;
            out.push_back(r);
            for (auto& [room, cur] : now) {
                PC::Facts f;
                const auto c = cache.find(room);
                f.cand = c != cache.end() && c->second.at[clk.target];
                f.cur = true, f.curStart = cur->start.tick, f.switchFrom = clk.switchFrom;
                f.candStart = f.cand ? c->second.start[clk.target] : 0;
                f.sameLot = f.sameGeo = f.sameTexels = f.sameLamps = f.basisOk = true;
                f.lampsAsKept = lampsAsKept(static_cast<int>(room), t);
                if (f.cand && c->second.at[clk.target] == cur) { // (as SwapRoom: the game's own solve when it began after the departure)
                    const bool game = static_cast<int32_t>(cur->start.tick - clk.switchFrom) >= 0;
                    open.shown.try_emplace(static_cast<int>(room), !game, nullptr);
                    if (game) open.why[static_cast<int>(room)] = "its solve since the switch landed";
                    continue;
                }
                if (const char* why = PC::SwapWhy(f)) open.why[static_cast<int>(room)] = why;
                else cur = c->second.at[clk.target], open.shown.try_emplace(static_cast<int>(room), true, nullptr);
            }
        }
        if (open.on && t - open.from >= 10000) close();
    }
    close();
    return out;
}
void PhaseCacheChecks() {
    // the clock: one departure per end left, a quick switch only on arriving at the other end within kQuickMs
    {
        PC::Clock c;
        uint32_t t = 1000;
        for (; t < 2000; t += 16) c.Observe(PC::kNight, t);
        for (; t < 22000; t += 16) c.Observe(PC::EndOf(1.0f - static_cast<float>(t - 2000) / 20000.0f), t); // a slow dusk, 20 s
        for (; t < 23000; t += 16) c.Observe(PC::kDay, t);
        Check(c.departures == 1 && c.leftN[PC::kNight] == 1 && c.switches == 0, "day/night cache: a slow dusk is no quick switch (left to the game)");
        PC::Clock j;
        j.Observe(PC::kDay, 100), j.Observe(PC::kNight, 116);
        Check(j.switches == 1 && j.target == PC::kNight && j.switchFrom == (116u | 1u), "day/night cache: a jump from day to night in one frame is a quick switch");
        PC::Clock b;
        b.Observe(PC::kDay, 100), b.Observe(PC::kTwilight, 116), b.Observe(PC::kDay, 400);
        Check(b.switches == 0 && b.leftN[PC::kDay] == 1, "day/night cache: leaving day and coming back is no switch (but a departure)");
        b.Observe(PC::kTwilight, 500), b.Observe(PC::kNight, 900), b.Observe(PC::kTwilight, 1000), b.Observe(PC::kNight, 1100);
        Check(b.switches == 1, "day/night cache: night left and reached again after a switch is not a second switch");
        PC::Clock h; // the 0.6 s fade: open half way, not again on arrival
        h.Observe(PC::kNight, 1000, 1.0f);
        uint32_t openedAt = 0;
        for (uint32_t t = 1016; t <= 1600; t += 16) {
            h.Observe(PC::EndOf(1.0f - static_cast<float>(t - 1000) / 600.0f), t, 1.0f - static_cast<float>(t - 1000) / 600.0f);
            if (h.switches == 1 && !openedAt) openedAt = t;
        }
        h.Observe(PC::kDay, 1616, 0.0f);
        Check(h.switches == 1 && h.target == PC::kDay && openedAt >= 1300 && openedAt <= 1320 && h.switchFrom == (1016u | 1u),
              "day/night cache: a quick switch opens half way through the fade (+0.3 s), once");
        PC::Clock tb; // turned back after it opened: a switch back to the end left
        tb.Observe(PC::kDay, 100, 0.0f), tb.Observe(PC::kTwilight, 116, 0.2f), tb.Observe(PC::kTwilight, 300, 0.6f);
        const uint32_t first = tb.switches;
        tb.Observe(PC::kTwilight, 400, 0.3f), tb.Observe(PC::kDay, 500, 0.0f);
        Check(first == 1 && tb.switches == 2 && tb.target == PC::kDay, "day/night cache: a fade turned back after its switch opened switches back");
    }
    // the mode change: the window marks' burst begun within 3 s before the departure, else 1 s before it
    {
        Check(PC::ModeChange(10000, 9700) == 9700 && PC::ModeChange(10000, 7000) == 7000, "day/night cache: the window marks' burst dates the mode change");
        Check(PC::ModeChange(10000, 6000) == 9000 && PC::ModeChange(10000, 0) == 9000 && PC::ModeChange(10000, 10300) == 9000,
              "day/night cache: no burst within 3 s before the departure (none, older, or after it): 1 s before it");
        Check(PC::AfterMode(9700, 9700) && !PC::AfterMode(9699, 9700), "day/night cache: a solve begun at the mode change or later is the other end's");
        const PC::Stamp s{PC::kNight, PC::kNight, 4, 5000}, k{PC::kNight, PC::kNight, 4, 5300};
        Check(PC::FillOk(s, k, PC::kNight, 4, 9700), "day/night cache: a solve of night in the stay just left joins the night cache");
        Check(PC::FillOk(PC::Stamp{PC::kTwilight, PC::kNight, 4, 4000}, k, PC::kNight, 4, 9700),
              "day/night cache: a solve begun in the fade towards night (after the level left day) is night's");
        const auto why = [](const char* w, const char* want) { return w && std::strcmp(w, want) == 0; };
        Check(why(PC::FillWhy(s, k, PC::kDay, 4, 9700), "begun heading the other end") && why(PC::FillWhy(s, k, PC::kNight, 5, 9700), "solved in an earlier stay") &&
                  why(PC::FillWhy(s, k, PC::kNight, 4, 5000), "begun after the mode change") &&
                  why(PC::FillWhy(PC::Stamp{PC::kTwilight, PC::kDay, 4, 5000}, k, PC::kNight, 4, 9700), "begun heading the other end") &&
                  why(PC::FillWhy(s, PC::Stamp{PC::kTwilight, PC::kDay, 5, 5300}, PC::kNight, 4, 9700), "the level left an end during its solve"),
              "day/night cache: not the end left, another stay, begun after the mode change, begun in a fade to the other end or kept past a departure: not cached, with the reason");
        PC::Clock ab; // (an aborted fade: heading back to the end it left)
        ab.Observe(PC::kNight, 100), ab.Observe(PC::kTwilight, 200);
        const PC::Stamp abStart = PC::StampOf(ab, 250);
        ab.Observe(PC::kNight, 400);
        Check(abStart.heading == PC::kDay && !PC::Whole(abStart, PC::StampOf(ab, 450)), "day/night cache: a solve begun in a fade the level turned back from is neither end's");
        std::map<uint64_t, PC::Slots<std::shared_ptr<const PcRoom>>> m;
        m[1].at[PC::kDay] = std::make_shared<PcRoom>(), m[1].start[PC::kDay] = 5000;
        m[2].at[PC::kDay] = std::make_shared<PcRoom>(), m[2].start[PC::kDay] = 9750;
        m[2].at[PC::kNight] = std::make_shared<PcRoom>(), m[2].start[PC::kNight] = 9800;
        Check(PC::PurgeDeparture(m, PC::kDay, 9700, false) == 1 && m.count(1) && !m[2].at[PC::kDay] && m[2].at[PC::kNight],
              "day/night cache: a departure drops only its end's candidates begun after the mode change");
        Check(PC::PurgeDeparture(m, PC::kNight, 30000, true) == 1 && m.size() == 1 && m.count(1), "day/night cache: a missed departure drops every candidate of that end");
    }
    // the session: from its second switch on every room is swapped or waits with a reason; from the third every room is swapped
    {
        int filled = 0, dropped = 0;
        std::vector<SwitchResult> r = ReplaySession(kSession1654, [](int, uint32_t) { return true; }, filled, dropped);
        Check(r.size() == 6, "day/night cache: the session's six quick switches (the load's included)");
        if (r.size() == 6) {
            for (const SwitchResult& s : r)
                std::printf("  session switch to %s: %d rooms, %d swapped, %d solved, %d not solved, %d sent by Apex;", s.to == PC::kNight ? "night" : "day", s.rooms, s.swapped,
                            s.solved, s.never, s.sent),
                    [&] {
                        for (const auto& [w, k] : s.why) std::printf(" %s x%d;", w.c_str(), k);
                        std::printf("\n");
                    }();
            Check(r[1].swapped == 0 && r[1].rooms == 13 && r[1].reasoned == 13 && r[1].sent == 13,
                  "day/night cache: the first switch after the load waits for every room and sends them all at its start");
            bool every = true;
            for (size_t i = 2; i < r.size(); i++) every = every && r[i].swapped + r[i].reasoned == r[i].rooms && r[i].rooms == 13;
            Check(every, "day/night cache: from the second switch on every room is swapped or has its reason");
            Check(r[2].swapped == 9 && r[2].why["no cached solve of that end"] == 4 && r[2].sent == 4,
                  "day/night cache: to night (2nd): 9 rooms solved at night since the load (its fade's included) swapped, 4 sent at its start");
            Check(r[3].swapped == 13 && r[4].swapped == 13 && r[5].swapped == 13, "day/night cache: from the third switch on all 13 rooms are swapped");
        }
        Check(dropped == 0, "day/night cache: no solve begun before the mode change dropped (the 1 s rule dropped 2 at each departure from night)");
        // lamps switched since (a dusk switch not yet back as kept): those rooms wait with that reason, the others swap
        std::vector<SwitchResult> l = ReplaySession(kSession1654, [](int room, uint32_t t) { return !(room < 3 && t > 100000 && t < 110000); }, filled, dropped);
        Check(l.size() == 6 && l[3].swapped == 10 && l[3].why[PC::kLampState] == 3, "day/night cache: a lamp not as its cached solve kept it: the room waits, with the reason");
        // 17-11-35: the switch's own solves (begun in its fade, the five Apex sent among them) are cached for the next switch
        std::vector<SwitchResult> q = ReplaySession(kSession1711, [](int, uint32_t) { return true; }, filled, dropped);
        for (const SwitchResult& sw : q) {
            std::printf("  17-11-35 switch to %s: %d rooms, %d swapped, %d solved, %d not solved, %d sent by Apex;", sw.to == PC::kNight ? "night" : "day", sw.rooms, sw.swapped,
                        sw.solved, sw.never, sw.sent);
            for (const auto& [w, k] : sw.why) std::printf(" %s x%d;", w.c_str(), k);
            std::printf("|\n");
        }
        bool all = q.size() == 4;
        for (size_t i = 1; i < q.size(); i++) all = all && q[i].swapped + q[i].reasoned == q[i].rooms && q[i].rooms == 13;
        Check(all, "day/night cache: 17-11-35, from the second switch on every room is swapped or has its reason");
        // (shown at once: swapped, or the game's own solve of the switch already landed by the arrival)
        const auto atOnce = [&](size_t i) { return q[i].swapped + q[i].why["its solve since the switch landed"]; };
        Check(q.size() == 4 && q[0].swapped == 0 && atOnce(1) == 9 && atOnce(2) == 12 && atOnce(3) == 13,
              "day/night cache: 17-11-35, the fade's solves cached: 9, 12 and 13 of 13 rooms at once from the 2nd switch (the game's log: 0, 5 and 3 swapped)");
    }
    // validity: each fact alone sends the room to its solve; all of them: swapped
    {
        PC::Facts ok;
        ok.cand = ok.cur = ok.sameLot = ok.sameGeo = ok.sameTexels = ok.sameLamps = ok.lampsAsKept = ok.basisOk = true;
        ok.candStart = 2000, ok.curStart = 9000, ok.switchFrom = 17250, ok.lastEdit = 0;
        Check(PC::SwapWhy(ok) == nullptr, "day/night cache: a valid candidate is swapped");
        const auto one = [&](auto set, const char* want) {
            PC::Facts f = ok;
            set(f);
            const char* w = PC::SwapWhy(f);
            return w && std::strcmp(w, want) == 0;
        };
        Check(one([](PC::Facts& f) { f.cand = false; }, "no cached solve of that end"), "day/night cache: no candidate waits");
        Check(one([](PC::Facts& f) { f.cur = false; }, "no kept solve now"), "day/night cache: a room not kept now waits");
        Check(one([](PC::Facts& f) { f.curStart = 17300; }, "its solve since the switch landed"), "day/night cache: the game's solve begun after the departure stays");
        Check(one([](PC::Facts& f) { f.solving = true; }, "being solved"), "day/night cache: a room being solved waits");
        Check(one([](PC::Facts& f) { f.sameLot = false; }, "another lot"), "day/night cache: lot change invalidates");
        Check(one([](PC::Facts& f) { f.lastEdit = 9000; }, "a player's lamp edit since its cached solve"), "day/night cache: a player's lamp edit since invalidates");
        Check(one([](PC::Facts& f) { f.lastEdit = 1600; }, "a player's lamp edit since its cached solve"), "day/night cache: an edit just before its start (its gather) invalidates");
        PC::Facts early = ok;
        early.lastEdit = 1400;
        Check(PC::SwapWhy(early) == nullptr, "day/night cache: an edit before the solve's gather keeps it");
        Check(one([](PC::Facts& f) { f.sameGeo = false; }, "its walls, doors or 3D blockers changed"), "day/night cache: a wall, door or object edit invalidates");
        Check(one([](PC::Facts& f) { f.sameTexels = false; }, "its texels changed"), "day/night cache: other texels invalidate");
        Check(one([](PC::Facts& f) { f.sameLamps = false; }, "a lamp lit now that its cached solve does not list"), "day/night cache: a lamp lit now that the cached solve lacks invalidates");
        Check(one([](PC::Facts& f) { f.lampsAsKept = false; }, PC::kLampState), "day/night cache: a lamp's state other than kept invalidates");
        Check(one([](PC::Facts& f) { f.basisOk = false; }, "its directional maps cannot follow"), "day/night cache: directional maps that cannot follow invalidate");
        PC::Facts w = ok; // (GetTickCount past 2^32)
        w.candStart = 0xFFFFF000u, w.curStart = 0xFFFFFF00u, w.switchFrom = 0x00000100u, w.lastEdit = 0xFFFFE000u;
        Check(PC::SwapWhy(w) == nullptr, "day/night cache: valid across the tick counter's wrap");
        // round 5 (17:54:28): a room solved since the departure with no cached solve shows the end: its reason says so
        Check(one([](PC::Facts& f) { f.cand = false, f.curStart = 17300; }, PC::kLanded), "day/night cache: solved since the departure counts as shown, cached or not");
        // a refused room shows the other end until a solve lands: Apex asks for one, unless one landed or runs
        PC::Facts edit = ok;
        edit.lastEdit = 9000;
        Check(PC::SendOnRefusal(PC::SwapWhy(edit), 100), "day/night cache: a room refused for a lamp edit is sent to its solve (17:54:28: 5 rooms showed night by day for 15 s)");
        Check(!PC::SendOnRefusal(PC::kLanded, 100) && !PC::SendOnRefusal(PC::kSolving, 100) && !PC::SendOnRefusal(PC::kNoKept, 100) && !PC::SendOnRefusal(nullptr, 100),
              "day/night cache: a swapped room, one solved since or being solved is not sent again");
        Check(!PC::SendOnRefusal(PC::kLampState, PC::kLampWaitMs - 1) && PC::SendOnRefusal(PC::kLampState, PC::kLampWaitMs),
              "day/night cache: a lamp's state is tried again first, then the room is sent");
        // the departure sends what the swap would refuse for an edit (the same test, the cached solve's start)
        Check(PC::EditSince(9000, 2000) && !PC::EditSince(1400, 2000) && !PC::EditSince(0, 2000), "day/night cache: a cached solve older than a lamp edit is sent at the departure");
        // a lamp edit's settle (17:53:55.487: 8 rooms solved again 2 s after the 22 lamps of dusk, each solved since by the switch)
        Check(PC::SettleSolved(53490, 53990 + 1) && PC::SettleSolved(53490, 55096), "settle: a room solved since the edit is left alone");
        Check(!PC::SettleSolved(53490, 53490 + PC::kEditMarginMs) && !PC::SettleSolved(53490, 50000), "settle: a solve begun before the edit or at its gather is solved again");
        Check(PC::SettleSolved(0xFFFFFF00u, 0x00000400u), "settle: across the tick counter's wrap");
    }
    // the swap: the cached object itself back in place (the same object, bit for bit), the bytes follow; the directional texels
    // holding what the room showed take the cached solve's own values exactly, a texel another solve wrote stays
    {
        std::map<uint64_t, std::shared_ptr<const PcRoom>> m;
        auto day = std::make_shared<PcRoom>(), night = std::make_shared<PcRoom>();
        day->bytes = 3000, night->bytes = 5000;
        m[7] = day;
        size_t bytes = 3000 + 100;
        const std::shared_ptr<const PcRoom> cand = night;
        const auto prev = PC::SwapIn(m, bytes, 7, cand);
        Check(prev == day && m[7] == cand && m[7].get() == night.get() && bytes == 5100, "day/night cache: the swap puts the cached solve itself back and moves the bytes");
        Check(PC::SwapIn(m, bytes, 8, cand) == nullptr && bytes == 5100 && !m.count(8), "day/night cache: no swap into a room not kept");
        std::mt19937 rng(17);
        std::vector<uint32_t> shownGame(400), expect(400), candGame(400), texel(400);
        for (size_t i = 0; i < 400; i++) shownGame[i] = rng(), candGame[i] = rng(), expect[i] = i % 3 ? shownGame[i] : rng(); // (a third: Apex's last write)
        for (size_t i = 0; i < 400; i++) texel[i] = i % 17 == 0 ? 0xDEADBEEFu : expect[i];                                 // (a 17th: another solve's)
        bool exact = true, left = true;
        for (size_t i = 0; i < 400; i++) {
            uint32_t v = texel[i];
            const bool wrote = BasisWrite(v, candGame[i], expect[i], candGame[i]);
            if (i % 17 == 0) left = left && !wrote && v == 0xDEADBEEFu;
            else exact = exact && wrote && v == candGame[i];
        }
        Check(exact && left, "day/night cache: the directional texels take the cached solve's values bit for bit, another solve's are left");
    }
}

// Reflections after lamp edits: the turns of a drag, and what a drop still owes
void ReflectionChecks() {
    using Key = std::tuple<uintptr_t, int, int>;
    using OutdoorDeltaModel::ReflectionPick;
    const std::set<Key> rooms = {{1, 1, 2}, {1, 1, 3}, {1, 1, 8}};
    std::set<Key> owed = rooms;
    size_t next = 0;
    bool ownLast = false;
    Check(ReflectionPick(owed, next, ownLast, false, 50, 500, 1, 3) == owed.end(), "reflections: no turn between a drag's steps");
    Check(ReflectionPick(owed, next, ownLast, true, 0, 99, 1, 3) == owed.end(), "reflections: no turn within 100 ms of the last");
    std::set<int> seen;
    for (int i = 0; i < 3; i++) {
        owed = rooms; // (every step owes them all again)
        seen.insert(std::get<2>(*ReflectionPick(owed, next, ownLast, true, 0, 100, 1, 3)));
    }
    Check(seen.size() == 3, "reflections: a drag's steps turn through the rooms, not only the lamp's");
    owed = rooms;
    Check(std::get<2>(*ReflectionPick(owed, next, ownLast, false, 120, 100, 1, 3)) == 3, "reflections: after the last edit the lamp's room first");
    Check(ReflectionPick(owed, next, ownLast, false, 119, 100, 1, 3) == owed.end(), "reflections: the rest waits for 120 ms");
    int drained = 0;
    for (; drained < 4 && !owed.empty(); drained++) owed.erase(ReflectionPick(owed, next, ownLast, false, 500, 100, 1, 0));
    Check(drained == 3, "reflections: a lamp in no room: all are still taken, once each");
    std::set<Key> none;
    Check(ReflectionPick(none, next, ownLast, true, 500, 500, 1, 3) == none.end(), "reflections: nothing owed");
    // Edits at any pace (a slider's come 120-220 ms apart: only the silence after each gives a turn): no room goes without
    for (const uint32_t gap : {90u, 105u, 135u, 150u, 200u, 240u, 300u, 700u}) {
        int turns[9] = {};
        uint32_t turn = 0u - 1000u;
        owed.clear();
        for (uint32_t t = 0; t < 9000; t += 15) {
            const bool step = t % gap < 15;
            if (step) owed = rooms;
            const auto it = ReflectionPick(owed, next, ownLast, step, t % gap, t - turn, 1, 3);
            if (it != owed.end()) turns[std::get<2>(*it)]++, owed.erase(it), turn = t;
        }
        Check(turns[2] >= 5 && turns[3] >= 5 && turns[8] >= 5, "reflections: every room keeps its turns at any pace of edits");
    }
}

int main() {
    std::mt19937 rng(20261013);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);

    // ---- half floats ----
    {
        bool ok = true;
        for (int i = 0; i < 200000 && ok; i++) {
            const float v = std::ldexp(U(rng), static_cast<int>(U(rng) * 40.0f) - 30);
            const float h = FromHalf(ToHalf(v));
            const float tol = std::max(std::fabs(v) * (1.0f / 2048.0f), std::ldexp(1.0f, -25));
            ok = std::fabs(h - v) <= tol;
        }
        Check(ok, "half floats round to the nearest representable value");
        Check(FromHalf(ToHalf(1.0f)) == 1.0f && FromHalf(ToHalf(0.0f)) == 0.0f && FromHalf(ToHalf(65504.0f)) == 65504.0f && FromHalf(ToHalf(1e9f)) == 65504.0f,
              "half floats: exact values and the largest finite");
        Check(ToHalf(1.0f + 1.0f / 2048.0f) == ToHalf(1.0f) && ToHalf(1.0f + 3.0f / 2048.0f) == ToHalf(1.0f + 4.0f / 2048.0f), "half floats: ties to even");
    }
    // ---- the pack ----
    {
        bool ok = true;
        for (int i = 0; i < 100000 && ok; i++) {
            float c[4] = {U(rng), U(rng), U(rng), U(rng)};
            const uint32_t v = PackModel(c);
            for (int k = 0; k < 4 && ok; k++) {
                const int want = static_cast<int>(std::nearbyint(c[k] * 255.0f));
                const int got = static_cast<int>((v >> (k == 3 ? 24 : 16 - 8 * k)) & 0xFF);
                ok = want == got;
            }
        }
        Check(ok, "pack: each channel x 255 rounded to nearest, A8R8G8B8");
    }
    // ---- the outdoor texel's hue past 1 (KeepHue): a sum within 1 as min(sum, 1) bit for bit; past 1, its ratios kept ----
    {
        bool same = true, ratios = true;
        for (int i = 0; i < 100000; i++) {
            const float S[3] = {U(rng), U(rng), U(rng)};
            float c[4];
            PackInput(S, 0.5f, c);
            for (int k = 0; k < 3; k++) same = same && c[k] == std::min(std::max(S[k], 0.0f), 1.0f);
            const float T[3] = {S[0] * 6.0f, S[1] * 6.0f, S[2] * 6.0f};
            PackInput(T, 0.5f, c);
            const float m = std::max({T[0], T[1], T[2]});
            for (int k = 0; k < 3; k++) ratios = ratios && (m <= 1.0f ? c[k] == T[k] : std::fabs(c[k] * m - T[k]) <= 1e-5f * m);
        }
        Check(same, "hue kept: a sum within 1 packs as min(sum, 1)");
        Check(ratios, "hue kept: a sum past 1 keeps its channels' ratios, its largest at 1");
        {
            constexpr float L = 1.0f / 255.0f;
            const auto cls = [](float g, float m) {
                const float a[3] = {g, g, g}, b[3] = {m, m, m};
                return ProbeClassify(a, b);
            };
            Check(cls(0.0f, 0.4f * L) == ProbeClass::Unlit && cls(0.1f, 0.1f + 2.0f * L) == ProbeClass::Match && cls(0.0f, 0.2f) == ProbeClass::Dark &&
                      cls(0.2f, 0.5f * L) == ProbeClass::Extra && cls(0.2f, 0.1f) == ProbeClass::Strength,
                  "room lamp probe: unlit, within 2 levels, dark in the solve, lit only by the solve, another strength");
        }
        // a red sconce (two entries of 1.7 x 1.7 near the lamp) over a yellow light at 1 (the F7's courtyard by the lamp):
        // channel by channel it packed (255, 255, 105), yellow; now red, the yellow adding its share
        const float red = 2.0f * 1.7f * 1.7f;
        const float S[3] = {red + 1.0f, 1.0f, 0.41f};
        float c[4];
        PackInput(S, 0.0f, c);
        const uint32_t v = PackModel(c);
        const int R = static_cast<int>(v >> 16 & 0xFF), G = static_cast<int>(v >> 8 & 0xFF), B = static_cast<int>(v & 0xFF);
        std::printf("  red sconce over a yellow light: packed (%d, %d, %d) (channel by channel it was (255, 255, 105))\n", R, G, B);
        Check(R == 255 && G < 64 && B < 32 && G > B, "hue kept: the red lamp dominates the texel, the yellow light adds its share");
        const float Y[3] = {1.3f, 1.2f, 0.5f}; // the yellow light alone past 1 stays yellow (it went white-yellow, (1, 1, 0.5))
        PackInput(Y, 0.0f, c);
        Check(c[0] == 1.0f && std::fabs(c[1] - 1.2f / 1.3f) < 1e-6f && std::fabs(c[2] - 0.5f / 1.3f) < 1e-6f, "hue kept: a yellow light alone past 1 keeps its hue");
    }
    // ---- the floor mask's alpha ----
    Check(MaskAlpha(0.2f, 0.8f, -1.0f) == std::clamp(1.0f - 0.2f / 0.8f, 0.0f, 1.0f) && MaskAlpha(0.2f, 0.8f, 0.9f) == 0.9f && MaskAlpha(0.7f, 0.8f, 0.9995f) == 1.0f &&
              MaskAlpha(0.0f, 0.00005f, -1.0f) == 0.0f,
          "floor mask alpha: the walls' share, the gate's share at least, 1 under every lamp");
    // ---- the shared blur against BlurWalls' code before it moved ----
    {
        bool ok = true;
        for (int trial = 0; trial < 400 && ok; trial++) {
            const int n = 1 + static_cast<int>(U(rng) * 20), rows = 1 + static_cast<int>(U(rng) * 12);
            const bool above = U(rng) < 0.5f, below = U(rng) < 0.5f;
            std::vector<uint32_t> before(static_cast<size_t>(n) * rows), g(static_cast<size_t>(n) * 4), ref;
            for (uint32_t& v : before) v = static_cast<uint32_t>(rng());
            for (uint32_t& v : g) v = static_cast<uint32_t>(rng());
            Old::Job(ref, before, g, n, rows, above, below, 2);
            const int top = above ? 2 : 0, total = top + rows + (below ? 2 : 0);
            std::vector<uint32_t> ext(static_cast<size_t>(total) * n);
            if (above) std::memcpy(ext.data(), g.data(), static_cast<size_t>(n) * 2 * 4);
            std::memcpy(ext.data() + static_cast<size_t>(top) * n, before.data(), before.size() * 4);
            if (below) std::memcpy(ext.data() + static_cast<size_t>(top + rows) * n, g.data() + static_cast<size_t>(n) * 2, static_cast<size_t>(n) * 2 * 4);
            BlurBlock(ext.data(), before.data(), n, rows, above, below, 2);
            ok = ext == ref;
        }
        Check(ok, "wall blur: the shared BlurBlock gives BlurWalls' former result (400 random blocks, with and without rows beyond the edges)");
    }
    // ---- the reach bound against the lamp law ----
    {
        bool ok = true;
        long beyond = 0;
        for (int trial = 0; trial < 300000 && ok; trial++) {
            Lamp L{};
            L.W = 0.075f * (1.0f + U(rng) * 20.0f) * (0.2f + U(rng) * 3.0f) * (U(rng) < 0.2f ? 3.333f : 1.0f);
            for (float& c : L.colour) c = U(rng) * 1.5f;
            L.spill = U(rng) * 2.0f;
            L.coneCos = U(rng) < 0.5f ? -2.0f : U(rng);
            Texel s{};
            const float d = U(rng) * 60.0f, a = U(rng) * 6.2832f, h = (U(rng) - 0.3f) * 10.0f;
            s.p[0] = d * std::cos(a), s.p[2] = d * std::sin(a), s.p[1] = 0.0f;
            L.pos[1] = h;
            s.n[1] = 1.0f;
            float c[3];
            Eval(L, s, false, U(rng) < 0.5f, {}, c);
            const float esum = L.colour[0] + L.colour[1] + L.colour[2];
            const float b2 = ReachBound2(L.W, esum, L.coneCos >= -1.0f ? L.spill : 1.0f, kThr);
            const float d2 = s.p[0] * s.p[0] + h * h + s.p[2] * s.p[2];
            const bool lit = c[0] + c[1] + c[2] > 0.0f;
            if (lit && b2 >= 0.0f && d2 > b2) ok = false;
            if (!lit && b2 > 0.0f && d2 > b2) beyond++;
        }
        Check(ok, "reach bound: no light beyond it adds anything (300000 lamps and points)");
        Check(beyond > 0, "reach bound: it does leave far points out");
    }
    // ---- the delta against the full solve ----
    {
        const Room room = MakeRoom();
        std::vector<Lamp> lamps;
        for (int i = 0; i < 9; i++) {
            Lamp L{};
            L.pos[0] = -5.0f + U(rng) * 12.0f, L.pos[1] = 0.5f + U(rng) * 5.0f, L.pos[2] = -5.0f + U(rng) * 12.0f;
            for (float& c : L.colour) c = 0.3f + U(rng);
            L.W = 0.075f * (4.0f + U(rng) * 10.0f) * (0.5f + U(rng) * 2.0f) * (i == 3 ? 3.333f : 1.0f);
            L.spill = i == 5 ? 0.3f : 1.0f;
            L.coneCos = i == 5 ? 0.6f : -2.0f;
            L.mult = i == 2 ? 2 : 1; // a lamp the list holds twice (story 0's own outdoor lamps)
            lamps.push_back(L);
        }
        Record rec = Capture(room, lamps);
        { // physical indoor lamps, stage 1: the base is the sum less the measured parts of every lamp with a GPU law (KeepEnd,
          // ExportRoom0(physical)); here lamps 7 and 8 have none and stay in it
            std::vector<Totals> base = rec.t, baked;
            for (size_t li = 0; li < 7; li++)
                for (const Contrib& p : rec.parts[li]) Remove(base[p.texel], p);
            std::vector<Lamp> rest(lamps.begin() + 7, lamps.end());
            FullSolve(room, rest, &baked);
            float worst = 0.0f, worstMask = 0.0f, back = 0.0f;
            std::vector<Totals> sum = base;
            for (size_t li = 0; li < 7; li++)
                for (const Contrib& p : rec.parts[li]) Add(sum[p.texel], p);
            for (size_t i = 0; i < base.size(); i++) {
                for (int q = 0; q < 3; q++) {
                    worst = std::max(worst, std::fabs(base[i].S[q] - baked[i].S[q]));
                    back = std::max(back, std::fabs(sum[i].S[q] - rec.t[i].S[q]));
                }
                worstMask = std::max({worstMask, std::fabs(base[i].vis - baked[i].vis), std::fabs(base[i].all - baked[i].all)});
            }
            std::printf("physical base: %zu texels, largest difference from the solve of the lamps without a law %.2e (mask %.2e), base + parts vs sum %.2e\n",
                        base.size(), worst, worstMask, back);
            Check(worst <= 1e-5f, "physical base: the sum less every law lamp's parts is the light of the lamps without a law");
            Check(worstMask <= 2e-3f, "physical base: the floor mask's vis and all less every law lamp's parts (halves) likewise");
            Check(back <= 1e-6f, "physical base: base + the law lamps' parts gives the solve's sum back");
        }
        Diff total;
        long evaluated = 0, written = 0, steps = 0, full = 0;
        auto step = [&](size_t li) {
            written += Delta(room, lamps, li, rec, &evaluated);
            const Maps want = FullSolve(room, lamps);
            Diff d;
            Compare(want, rec.maps, d);
            total.max = std::max(total.max, d.max);
            total.at1 += d.at1, total.over1 += d.over1, total.n += d.n;
            steps++;
            full += static_cast<long>(room.t.size() * lamps.size());
        };
        // a drag: 200 steps of 5 cm
        for (int k = 0; k < 200; k++) {
            lamps[0].pos[0] += 0.05f;
            lamps[0].pos[2] += 0.02f;
            step(0);
        }
        // a recolour, a dimming, a switch off and on, a move through a wall, the twice-listed lamp
        lamps[1].colour[0] = 1.4f, lamps[1].colour[2] = 0.1f;
        step(1);
        lamps[3].W *= 0.4f;
        step(3);
        const float keep[3] = {lamps[4].colour[0], lamps[4].colour[1], lamps[4].colour[2]};
        for (float& c : lamps[4].colour) c = 0.0f;
        step(4);
        for (int q = 0; q < 3; q++) lamps[4].colour[q] = keep[q];
        step(4);
        lamps[6].pos[0] = -lamps[6].pos[0];
        step(6);
        lamps[2].pos[1] += 1.0f;
        step(2);
        // the lamp leaves the list (deleted, or switched off and gathered again), then comes back
        lamps[7].mult = 0;
        step(7);
        lamps[7].mult = 1;
        step(7);
        // a lamp under the deck (the gate's share changes for every gated texel)
        lamps[8].pos[1] = 2.0f;
        step(8);
        // the shaded lamp turned
        lamps[5].coneCos = 0.2f;
        step(5);
        std::printf("delta vs full solve: %ld edits, %ld texels compared, largest difference %d/255, %ld texels off by 1, %ld by more; %ld texels "
                    "rewritten; lamp x texel evaluations %ld (a full solve each time: %ld)\n",
                    steps, total.n, total.max, total.at1, total.over1, written, evaluated, full);
        Check(total.max <= 1, "delta vs full solve: no texel more than 1/255 off");
        Check(total.at1 <= total.n / 1000, "delta vs full solve: at most 1 texel in 1000 off by 1/255");
        Check(evaluated * 5 < full, "delta: far fewer evaluations than solving every light again");
    }
    // (RtLampModel::ComposeBase) an upper wall of a double-height room loses the street lamps' light, never under 0; any other texel
    // keeps the solve's base
    Check(std::fabs(RtLampModel::ComposeBase(0.5f, 0.2f, true) - 0.3f) < 1e-6f && RtLampModel::ComposeBase(0.1f, 0.2f, true) == 0.0f &&
              RtLampModel::ComposeBase(0.5f, 0.2f, false) == 0.5f && RtLampModel::ComposeBase(-1e-6f, 0.2f, false) == -1e-6f &&
              RtLampModel::ComposeBase(std::nanf(""), 0.0f, true) == 0.0f,
          "room-0 base: street lamps off an upper wall only, clamped there; any other texel's base as the solve gave it");
    { // GPU room lamps, stage 1: a kept part recomposed (RtLampModel::KeptAdd)
        const float thr = 3.0f / 255.0f, law[3] = {0.04f, 0.03f, 0.02f}; // r + g + b = 0.09
        float o[3] = {};
        RtLampModel::KeptAdd(law, 0.5f, 0.8f, 1.0f, 2.0f, false, true, thr, o); // 0.045 >= 2 thr: whole
        Check(std::fabs(o[0] - 2.0f * 0.02f * 0.8f) < 1e-7f && std::fabs(o[2] - 2.0f * 0.01f * 0.8f) < 1e-7f, "kept part: mult x law x A x B over twice the threshold");
        float f[3] = {};
        const float A = 1.5f * thr / 0.09f; // r + g + b = 1.5 thr: the soft cut's middle, smoothstep 0.5
        RtLampModel::KeptAdd(law, A, 1.0f, 1.0f, 1.0f, false, true, thr, f);
        Check(std::fabs((f[0] + f[1] + f[2]) - 0.5f * 1.5f * thr) < 1e-6f, "kept part: the soft light cut at its threshold");
        float s[3] = {};
        RtLampModel::KeptAdd(law, A, 1.0f, 0.5f, 1.0f, false, true, thr, s); // threshold x 0.5: 1.5 thr is over twice it
        Check(std::fabs((s[0] + s[1] + s[2]) - 1.5f * thr) < 1e-6f, "kept part: the threshold scale of a lamp through an opening");
        float e[3] = {}, g[3] = {}, d[3] = {};
        RtLampModel::KeptAdd(law, 0.9f * thr / 0.09f, 1.0f, 1.0f, 1.0f, true, true, thr, e);
        RtLampModel::KeptAdd(law, 0.9f * thr / 0.09f, 1.0f, 1.0f, 1.0f, false, false, thr, g);
        RtLampModel::KeptAdd(law, 1.2f * thr / 0.09f, 1.0f, 1.0f, 1.0f, false, false, thr, d);
        Check(e[0] == 0.0f && g[0] == 0.0f && std::fabs((d[0] + d[1] + d[2]) - 1.2f * thr) < 1e-6f, "kept part: the early-out and the game's drop under the threshold, whole over it");
        float z[3] = {};
        RtLampModel::KeptAdd(law, 0.5f, 0.0f, 1.0f, 2.0f, false, true, thr, z);
        Check(z[0] == 0.0f && z[1] == 0.0f && z[2] == 0.0f, "kept part: B = 0 (dropped by the wall and 3D tests) adds nothing");
        // A and B kept as halves: the recomposed part within 1e-3 of the exact one
        float h[3] = {}, x[3] = {};
        const float a = 3.217f, b = 0.6137f;
        RtLampModel::KeptAdd(law, FromHalf(ToHalf(a)), FromHalf(ToHalf(b)), FromHalf(ToHalf(1.0f)), 2.0f, false, true, thr, h);
        RtLampModel::KeptAdd(law, a, b, 1.0f, 2.0f, false, true, thr, x);
        Check(std::fabs((h[0] + h[1] + h[2]) - (x[0] + x[1] + x[2])) <= 1e-3f * (x[0] + x[1] + x[2]), "kept part: half-float A and B within 1e-3");
    }
    { // GPU room lamps, stage 2a: a kept texel's tail recomposed (RtLampModel::KeptTexel) against the solve's tail on the sum
        // the game's tail (FUN_006a31d0): x = norm S, toned, alpha = ramp x weight, + alpha x ambient when the pass adds it,
        // min 1, the roofed alpha floor, the dither, the pack
        const float k = 0.62f, floorA = 5.0f / 255.0f;
        const auto game = [&](const float S[3], float norm, float ramp, const float amb[3], bool addAmb, float d, float p[4]) {
            float x[3], c[3];
            for (int i = 0; i < 3; i++) x[i] = norm * S[i];
            GpuLampLaw::Tone(x, c);
            const float a = ramp * GpuLampLaw::Weight(c, k);
            for (int i = 0; i < 3; i++) p[i] = c[i] + (addAmb ? a * amb[i] : 0.0f);
            p[3] = a;
            float q[4] = {std::min(p[0], 1.0f), std::min(p[1], 1.0f), std::min(p[2], 1.0f), std::min(p[3], 1.0f)};
            if (q[3] < floorA) q[3] = floorA;
            Dither(q, d);
            return PackModel(q);
        };
        std::mt19937 rng2(77);
        std::uniform_real_distribution<float> U2(0.0f, 1.0f);
        long same = 0, edited = 0, editedOk = 0, n = 20000, unsplit = 0, unsplitOk = 0;
        for (long t = 0; t < n; t++) {
            float S[3], R[3], amb[3], p[4], q[4], c[4];
            const float scale = U2(rng2) < 0.2f ? 4.0f : 0.6f;
            for (int i = 0; i < 3; i++) S[i] = scale * U2(rng2) * U2(rng2), amb[i] = 0.3f * U2(rng2);
            const float norm = 0.5f + U2(rng2), ramp = 0.6f + 0.4f * U2(rng2), d = RoomMapDither::Offset(t % 97, t % 89) / 255.0f;
            const bool addAmb = (t & 1) != 0;
            const uint32_t g = game(S, norm, ramp, amb, addAmb, d, p);
            const bool split = RtLampModel::KeptTexel(S, S, p, norm, k, true, floorA, d, c);
            same += MaxLevelDiff(PackModel(c), g) <= 1;
            if (!split) {
                unsplit++;
                unsplitOk += MaxLevelDiff(PackModel(c), g) <= 1;
                continue;
            }
            // a lamp edit: the sum moves (brighter or darker), the ramp and ambient read back from the solve's input
            for (int i = 0; i < 3; i++) R[i] = std::max(S[i] + (U2(rng2) - 0.5f) * 0.4f, 0.0f);
            const uint32_t ge = game(R, norm, ramp, amb, addAmb, d, q);
            RtLampModel::KeptTexel(R, S, p, norm, k, true, floorA, d, c);
            edited++;
            editedOk += MaxLevelDiff(PackModel(c), ge) <= 1;
        }
        std::printf("  kept tail: %ld of %ld texels within 1/255 at their own sum (%ld without the ramp read back), %ld of %ld after an edit\n", same, n, unsplit, editedOk, edited);
        Check(same == n, "kept tail: the solve's own sum packs back to the game's texel within 1/255");
        Check(unsplit > 0 && unsplitOk == unsplit, "kept tail: a texel of weight 0 (no ramp read back) packs back too");
        Check(editedOk == edited, "kept tail: an edited sum takes the game's tail with the ramp and ambient read back");
        // (S8) the pass's ambient scaled by the room's ambient now over its solve's (AmbScale): unchanged, the compose is the one
        // without a scale bit for bit (so the game's texel as above); changed (Rooms at Night's sliders, a lamp edit's ambient
        // taken again), the game's tail with the new ambient within 1/255
        long ambSame = 0, ambExact = 0, ambMoved = 0, ambMovedOk = 0, ambN = 20000;
        for (long t = 0; t < ambN; t++) {
            float S[3], amb[3], now[3], scale[3], p[4], q[4], c0[4], c1[4];
            const float sc = U2(rng2) < 0.2f ? 4.0f : 0.6f;
            for (int i = 0; i < 3; i++) S[i] = sc * U2(rng2) * U2(rng2), amb[i] = 0.01f + 0.3f * U2(rng2);
            const float norm = 0.5f + U2(rng2), ramp = 0.6f + 0.4f * U2(rng2), d = RoomMapDither::Offset(t % 97, t % 89) / 255.0f;
            const bool addAmb = (t & 1) != 0;
            const uint32_t g = game(S, norm, ramp, amb, addAmb, d, p);
            std::memcpy(now, amb, sizeof now);
            RtLampModel::AmbScale(amb, now, scale);
            RtLampModel::KeptTexel(S, S, p, norm, k, true, floorA, d, c0);
            RtLampModel::KeptTexel(S, S, p, norm, k, true, floorA, d, c1, scale);
            ambSame += std::memcmp(c0, c1, sizeof c0) == 0 && scale[0] == 1.0f && scale[1] == 1.0f && scale[2] == 1.0f;
            ambExact += PackModel(c1) == g;
            for (int i = 0; i < 3; i++) now[i] = amb[i] * (0.2f + 2.3f * U2(rng2));
            RtLampModel::AmbScale(amb, now, scale);
            const uint32_t gn = game(S, norm, ramp, now, addAmb, d, q);
            if (!RtLampModel::KeptTexel(S, S, p, norm, k, true, floorA, d, c1, scale)) continue;
            ambMoved++;
            ambMovedOk += MaxLevelDiff(PackModel(c1), gn) <= 1;
        }
        const float zero3[3] = {}, one3[3] = {0.2f, 0.2f, 0.2f};
        float sc0[3];
        RtLampModel::AmbScale(zero3, one3, sc0);
        std::printf("  ambient now (S8): %ld of %ld texels composed bit for bit as without it (%ld the game's byte for byte), %ld of %ld within 1/255 of the game's "
                    "tail with a moved ambient\n",
                    ambSame, ambN, ambExact, ambMovedOk, ambMoved);
        Check(ambSame == ambN, "kept tail: an unchanged ambient composes bit for bit as before (scale exactly 1)");
        Check(ambMoved > 0 && ambMovedOk == ambMoved, "kept tail: a moved ambient gives the game's tail with it within 1/255");
        Check(sc0[0] == 1.0f && sc0[1] == 1.0f && sc0[2] == 1.0f, "kept tail: a solve's ambient of 0 keeps the scale 1");
        // the alpha floor and a dark roofed texel
        const float zero[3] = {}, pz[4] = {0.0f, 0.0f, 0.0f, 0.001f};
        float c[4];
        RtLampModel::KeptTexel(zero, zero, pz, 1.0f, k, true, floorA, 0.0f, c);
        Check(c[3] == floorA && c[0] == 0.0f, "kept tail: the roofed alpha floor");
        // roofless: the hue kept past 1, min 1, the solve's alpha
        const float Sr[3] = {1.6f, 0.4f, 0.2f}, pr[4] = {1.0f, 0.25f, 0.125f, 0.37f};
        RtLampModel::KeptTexel(Sr, Sr, pr, 1.0f, k, false, floorA, 0.0f, c);
        Check(c[0] == 1.0f && std::fabs(c[1] - 0.25f) < 1e-6f && std::fabs(c[2] - 0.125f) < 1e-6f && c[3] == 0.37f, "kept tail: a roofless texel keeps its hue and the solve's alpha");
    }
    { // (GPU room lamps stage 3b) a moved lamp's A is over the game's evaluation: x GameOverLaw it is over the law, which raises a
      // wall texel's N.l itself (MountedNdl): law x GameOverLaw x the hook's MountFactor gives the law back (no cap, under 8x), a
      // floor texel and a wall far from the lamp take 1
        GpuLampLaw::Lamp L;
        L.pos[0] = 0.15f, L.pos[1] = 1.6f, L.pos[2] = 0.0f, L.W = 2.0f, L.cmax = 1e6f;
        L.colour[0] = 1.0f, L.colour[1] = 0.8f, L.colour[2] = 0.6f;
        const float wall[3] = {1.0f, 0.0f, 0.0f}, up[3] = {0.0f, 1.0f, 0.0f};
        bool back = true, floorOne = true, farOne = true;
        long raised = 0;
        for (int i = 0; i < 200; i++) {
            const float p[3] = {0.0f, 0.2f + 0.015f * i, -1.5f + 0.015f * i};
            float law[3];
            GpuLampLaw::Eval(L, p, wall, 0.0f, law);
            const float gol = RoomLampMove::GameOverLaw(L, p, wall), mf = RoomLampMove::MountFactor(p, wall, L.pos);
            raised += gol < 1.0f ? 1 : 0;
            if (mf < 8.0f) back = back && std::fabs(law[0] * gol * mf - law[0]) <= 1e-4f * law[0];
            floorOne = floorOne && RoomLampMove::GameOverLaw(L, p, up) == 1.0f;
            const float far[3] = {-0.5f, p[1], p[2]};
            farOne = farOne && RoomLampMove::GameOverLaw(L, far, wall) == 1.0f;
        }
        Check(raised > 0 && back, "moved lamps: the law x GameOverLaw x the mount factor is the law (a wall texel the law raises)");
        Check(floorOne && farOne, "moved lamps: GameOverLaw is 1 where the law raises nothing");
    }
    TubeChecks();
    AreaChecks();
    BasisChecks();
    BasisPhysChecks();
    SplitFillChecks();
    OpenAboveChecks();
    ProxyOpenTopsChecks();
    CarryChecks();
    InsideChecks();
    BlockerTableChecks();
    AcrossRoomsChecks();
    DawnChecks();
    SeatChecks();
    PhysPickChecks();
    ReflectionChecks();
    PhaseCacheChecks();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
