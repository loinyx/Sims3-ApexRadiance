// Outdoor lamp edits by delta (features/outdoor_delta_model.h, features/outdoor_delta.h): a synthetic room 0 is solved in
// full after every lamp edit and brought up to date by the delta from each light's kept part, and the two are compared texel
// by texel (8-bit levels, after Smooth light maps' dither, the game's pack and the class-2 wall blur). Also: the half floats
// the parts keep, the floor mask's alpha, the pack, the shared wall blur against the code it replaced, and the reach bound
// against the lamp law. Read-only: prints to stdout, writes no file.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include "features/outdoor_delta_model.h"
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
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
