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
#include <random>
#include <vector>
#include <xmmintrin.h>
#include "features/outdoor_delta_model.h"
#include "features/rt_lamp_model.h"
#include "features/room_lamp_move.h"
#include "features/open_rooms_above.h"
#include "features/rt_lamp_proxy.h"
#include "features/rt_lamp_blockers.h"
#include "features/phys_lamp_pick.h"
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
// share (PhysLampPick::BasisShare: the proxy's visibility x the game's 3D test, dropped under K_DROP) x the game's step; the base
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
            float step[16] = {}, part[16], vis = 0.0f, t3d = 0.0f;
            int unknown = 0;
            GameStep(p, ls[j], step);
            PhysLampPick::BasisShare(mesh, ls[j].pos, 0.15f, p, tests, ls[j].pos, p, stamp, mark, vis, t3d, &unknown);
            BasisPartPhys(rows[j], vis, t3d, K_DROP, step, part);
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
    BasisPartPhys(1.0f, 1.0f, 0.5f * K_DROP, K_DROP, step, part);
    const bool dropped = std::all_of(part, part + 16, [](float v) { return v == 0.0f; });
    BasisPartPhys(3.0f, 0.5f, 0.5f, K_DROP, step, part);
    Check(dropped && part[0] == 3.0f * 0.25f * 0.25f, "physical directional maps: a share under the game's drop adds nothing, else row x share x step");
    { // a still lamp takes the game's levels (its kept pass), a moved one the physical part x the scale learnt while still
        float st[32], acc[32] = {}, acc2[32] = {};
        std::fill(st, st + 32, 0.25f);
        const float pass[2] = {0.8f, 0.6f}, vis[2] = {0.5f, 0.5f}, t3d[2] = {1.0f, 1.0f};
        double game = 0.0, phys = 0.0;
        BasisLampAdd(true, pass, 1, 1.0f, vis, t3d, K_DROP, st, 2, 1.0f, acc, game, phys);
        const float scale = static_cast<float>(game / phys);
        BasisLampAdd(false, nullptr, 1, 1.0f, vis, t3d, K_DROP, st, 2, scale, acc2, game, phys);
        Check(acc[0] == 0.8f * 0.25f && acc[16] == 0.6f * 0.25f && std::fabs(scale - 1.4f) < 1e-5f && std::fabs(acc2[0] - 0.5f * 0.25f * scale) < 1e-6f,
              "physical directional maps: a still lamp writes the game's levels, a moved one the physical part at the game's scale");
    }
    { // the shares kept between writes: lamp A still, then moved, then with fewer samples lit, as traced afresh
        std::vector<float> P(static_cast<size_t>(n) * 3);
        for (float& v : P) v = U(rng) * 8.0f;
        for (int s = 0; s < n; s++) P[s * 3 + 1] = 0.5f;
        const auto pw = [&](size_t i) { return &P[i * 3]; };
        PhysLampPick::ShareCache c;
        std::vector<float> vis, t3d, fv, ft;
        bool same = true;
        size_t traced[4] = {};
        float L[3] = {ls[0].pos[0], ls[0].pos[1], ls[0].pos[2]};
        for (int round = 0; round < 4; round++) {
            if (round == 2) L[0] += 0.5f; // (moved)
            const auto lit = [&](size_t i) { return round == 3 ? i % 3 != 0 : true; }; // (a value edit darkens some steps)
            int unknown = 0;
            traced[round] = PhysLampPick::BasisSharesCached(c, round != 0, n, lit, mesh, L, 0.15f, P.data(), tests, L, pw, stamp, mark, vis, t3d, &unknown);
            fv.assign(n, 0.0f), ft.assign(n, 0.0f);
            for (int i = 0; i < n; i++)
                if (lit(i)) PhysLampPick::BasisShare(mesh, L, 0.15f, &P[i * 3], tests, L, &P[i * 3], stamp, mark, fv[i], ft[i], &unknown);
            same = same && std::memcmp(vis.data(), fv.data(), n * sizeof(float)) == 0 && std::memcmp(t3d.data(), ft.data(), n * sizeof(float)) == 0;
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
            if (BasisWrite(tex[i], w, expect[i])) expect[i] = w, took++;
            else kept++;
        }
    bool rule = true;
    for (size_t i = 0; i < tex.size(); i++) rule = rule && (i % 7 == 0 ? tex[i] == (game[i] ^ 0x00010101u) : tex[i] == (want[i] ^ 1u));
    std::printf("  physical directional maps, the write rule: %ld writes taken, %ld texels left as another solve wrote them\n", took, kept);
    Check(rule && took == 2 * static_cast<long>(tex.size() - (tex.size() + 6) / 7), "physical directional maps: only texels holding the capture's or the last write's value are written");
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
    OpenAbove::Classify(6, 6, room, bare, [](int, int Z) { return Z == 1; }, [](int, int) { return false; }, open);
    Check(open.empty(), "open to above: a single-storey room under its roof (a wall above along one side only) stays closed");
    // walls round both, none between them: one space two rooms wide
    OpenAbove::Classify(6, 6, room, bare, [](int, int Z) { return Z == 1 || Z == 4; }, [](int X, int) { return X == 1 || X == 6; }, open);
    Check(open == std::vector<int>{1, 2}, "open to above: two rooms open to above with no wall between them on the story above");
    // room 1 floored above but for a row along one wall (6 of its 9 tiles): it keeps its ceiling
    OpenAbove::Classify(6, 6, room, [](int x, int z, int) { return x <= 3 && z >= 2; }, [](int, int) { return true; }, [](int, int) { return true; }, open);
    Check(open == std::vector<int>{2}, "open to above: a room floored above but along one wall keeps its ceiling");
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
    {
        uintptr_t v[4] = {};
        Check(PhysLampPick::ListLen(nullptr, nullptr, 8) == 0 && PhysLampPick::ListLen(v, v + 3, 8) == 3 && PhysLampPick::ListLen(nullptr, v, 8) == -1 &&
                  PhysLampPick::ListLen(v + 2, v, 8) == -1 && PhysLampPick::ListLen(v, v + 4, 3) == -1,
              "physical pick: a room with no light buffer reads as an empty list; broken or too long bounds stay unreadable");
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
        Check(EditRefusal(r, at) != nullptr && at == 3, "physical edit: a room with no kept physical solve refuses it (even one not listing the lamp)");
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
void BlockerTableChecks() {
    using namespace RoomLightTests;
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
        // (LiveBlockers) a side table's top (a disc 0.36 m wide, 0.77 m up) dragged 1 m with its lamp: the table read again from the
        // moved records shades the floor under its new place, as a fresh keep's would, and no longer the old one
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
              "live blockers: a table dragged with its lamp shades the floor under its new place and no longer under the old one");
        // (Offset) the kept table at x = -1 moved 2 m by its record's offset casts exactly the shadow of the table kept at x = 1
        RtLampGpu::BlockerTable kept;
        const int hk = kept.Append(tableAt(-1.0f));
        kept.Offset(hk, 0, 2.0f, 0.0f);
        bool same = true;
        for (float x = -2.0f; x <= 2.0f; x += 0.05f) same = same && std::fabs(kept.Occlude(hk, lamp, V{x, 0.0f, 0.1f}) - live.Occlude(h, lamp, V{x, 0.0f, 0.1f})) < 1e-4f;
        Check(same, "blocker offset: a kept table moved by its record's offset shades exactly as the table kept at its new place");
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
    OpenAboveChecks();
    ProxyOpenTopsChecks();
    BlockerTableChecks();
    PhysPickChecks();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
