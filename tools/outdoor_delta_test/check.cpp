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
#include "features/outdoor_delta_model.h"
#include "features/rt_lamp_model.h"
#include "features/room_lamp_move.h"
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
    BasisChecks();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
