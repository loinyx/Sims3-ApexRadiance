#pragma once
// Outdoor lamp edits by delta (developer, outdoor_delta.h): the arithmetic the in-game code and the offline check
// (tools/outdoor_delta_test) share. No game access here.
//
// A room 0 texel (an outdoor room, +0x18 set) is the plain sum of what each light of the room's list added at it, packed as
// min(sum, 1) (FUN_006a31d0, docs/engine/room-light-maps.md): no tone curve, no normalisation. Each light's part is kept
// per texel when the room is solved, so a changed light is taken out and its new part put in:
//   S' = S - part(old) + part(new).
// The floor mask of a story >= 1 (LevelLightShare SolvePointBatchImpl) is a ratio of two more sums of the same lights, kept
// the same way. Class-2 walls are blurred after the solve (FUN_0069f650 mode 0, as LevelLightShare BlurWalls replays it):
// a touched wall block is blurred again from its texels before the blur.
//
// Part of Apex Radiance. Credits: @loinyx
#include "half_float.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace OutdoorDeltaModel {

// ---- half floats (round to nearest even; every value kept is finite) ----
inline uint16_t ToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t ax = x & 0x7FFFFFFFu;
    if (ax >= 0x7F800000u) return static_cast<uint16_t>(sign | 0x7BFFu); // inf / NaN: the largest finite
    if (ax >= 0x477FF000u) return static_cast<uint16_t>(sign | 0x7BFFu); // over 65504 after rounding
    if (ax < 0x33000001u) return static_cast<uint16_t>(sign);           // under half the smallest subnormal
    int e = static_cast<int>(ax >> 23) - 127 + 15;
    uint32_t m = ax & 0x7FFFFFu;
    if (e <= 0) { // subnormal
        m |= 0x800000u;
        const int shift = 14 - e; // 14..24
        uint32_t h = m >> shift;
        const uint32_t rem = m & ((1u << shift) - 1u), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1u))) h++;
        return static_cast<uint16_t>(sign | h);
    }
    uint32_t h = (static_cast<uint32_t>(e) << 10) | (m >> 13);
    const uint32_t rem = m & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) h++; // a carry into the exponent is the right rounding
    return static_cast<uint16_t>(sign | h);
}
inline float FromHalf(uint16_t h) {
    const uint32_t e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v;
    if (e == 0) v = static_cast<float>(m) * Pow2(-24);
    else if (e == 31) v = 65504.0f;
    else v = static_cast<float>(m | 0x400u) * Pow2(static_cast<int>(e) - 25);
    return (h & 0x8000u) ? -v : v;
}

// LevelLightShare's luminance of a sum (the floor mask)
inline float Lum3(const float* c) { return c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f; }

// ---- a texel ----
// What one light added at one texel: the room's own sum (r, g, b) and, for a masked floor texel, the luminance it added to
// the mask's two copies (walls tested, no walls)
struct Contrib {
    uint32_t texel = 0;
    float rgb[3] = {};
    uint16_t vis = 0, all = 0; // halves
};
inline bool Nonzero(const Contrib& c) { return c.rgb[0] != 0.0f || c.rgb[1] != 0.0f || c.rgb[2] != 0.0f || c.vis != 0 || c.all != 0; }
// A texel's sums: the room's own (rgb), and the mask's two copies (luminance)
struct Totals {
    float S[3] = {};
    float vis = 0.0f, all = 0.0f;
};
inline void Remove(Totals& t, const Contrib& c) {
    for (int i = 0; i < 3; i++) t.S[i] -= c.rgb[i];
    t.vis -= FromHalf(c.vis);
    t.all -= FromHalf(c.all);
}
inline void Add(Totals& t, const Contrib& c) {
    for (int i = 0; i < 3; i++) t.S[i] += c.rgb[i];
    t.vis += FromHalf(c.vis);
    t.all += FromHalf(c.all);
}

// The floor mask's alpha (SolvePointBatchImpl): the share of the outdoor lamps' light the walls block at the texel, and at
// least `under`, the share of the ground atlas the lamps under the floor's plane put there (>= 0.999: 1; < 0: none)
inline float MaskAlpha(float visLum, float allLum, float under) {
    if (under >= 0.999f) return 1.0f;
    float a = 0.0f;
    if (allLum > 1e-4f) a = std::clamp(1.0f - visLum / allLum, 0.0f, 1.0f);
    if (under > a) a = under;
    return a;
}

// An outdoor texel's sum keeps its hue where it passes 1: divided by its largest channel (SolvePointBatch, before the
// game's min). Cut channel by channel, a strong red sconce's R stopped at 1 while a weaker yellow light filled G, and the
// courtyard by the lamp showed yellow (an F7 of a red sconce: lot map (1, 1, 0.41) beside it). A sum within 1 is unchanged.
inline void KeepHue(float c[3]) {
    const float m = std::max({c[0], c[1], c[2]});
    if (m > 1.0f && std::isfinite(m))
        for (int i = 0; i < 3; i++) c[i] /= m;
}
// The colour FUN_006a31d0 packs for an outdoor texel: min(sum, 1) and min(alpha, 1) (0x006A34C2), the sum's hue kept
// (KeepHue). A sum taken apart and put together again can end a rounding under 0, where the true sum is 0 or more.
inline void PackInput(const float S[3], float alpha, float out[4]) {
    for (int i = 0; i < 3; i++) {
        const float v = std::isfinite(S[i]) ? S[i] : 0.0f;
        out[i] = std::max(v, 0.0f);
    }
    KeepHue(out);
    for (int i = 0; i < 3; i++) out[i] = std::min(out[i], 1.0f);
    out[3] = std::min(std::isfinite(alpha) ? std::max(alpha, 0.0f) : 0.0f, 1.0f);
}
// Smooth light maps' dither (RoomMapDither DitherPack): d (in units of the colour, the level offset / 255) added to r, g, b
inline void Dither(float c[4], float d) {
    for (int i = 0; i < 3; i++) {
        const float v = c[i] + d;
        c[i] = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : c[i];
    }
}
// The game's pack 0x0060CD10 (the in-game code calls the game's own): x 255, + 1.5 x 2^23 (round to nearest), A8R8G8B8
inline uint32_t PackModel(const float c[4]) {
    uint32_t ch[4];
    for (int i = 0; i < 4; i++) {
        volatile float t = c[i] * 255.0f;
        volatile float u = t + 12582912.0f;
        uint32_t bits;
        const float uu = u;
        std::memcpy(&bits, &uu, 4);
        ch[i] = bits & 0xFFu;
    }
    return (ch[3] << 24) | (ch[0] << 16) | (ch[1] << 8) | ch[2];
}
// The largest difference of two packed texels, in 8-bit levels
inline int MaxLevelDiff(uint32_t a, uint32_t b) {
    int m = 0;
    for (int s = 0; s < 32; s += 8) m = std::max(m, std::abs(static_cast<int>((a >> s) & 0xFFu) - static_cast<int>((b >> s) & 0xFFu)));
    return m;
}

// ---- the class-2 wall blur (FUN_0069f650 mode 0, LevelLightShare BlurWalls) ----
inline uint32_t Avg4(uint32_t a, uint32_t b) { return (a | b) - (((a ^ b) >> 1) & 0x7F7F7F7Fu); }
// The game's pass along one row
inline void BlurRow(uint32_t* p, uint32_t n) {
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
// Its pass down one column of `rows` rows (stride n), clamped at both ends
inline void BlurColumn(uint32_t* p, int rows, int n) {
    uint32_t prev = p[0];
    for (int r = 0; r < rows; r++) {
        const uint32_t cur = p[static_cast<size_t>(r) * n], next = r + 1 < rows ? p[static_cast<size_t>(r + 1) * n] : cur;
        p[static_cast<size_t>(r) * n] = Avg4(Avg4(prev, next), cur);
        prev = cur;
    }
}
// One wall block blurred (BlurWalls): `ext` holds total = top + rows + (below ? 2 : 0) rows of n (the two rows lit beyond
// the top edge, the block as the solve left it, the two beyond the bottom edge), top = above ? 2 : 0; on return its rows
// top .. top + rows - 1 are the block's texels. An edge with nothing beyond it keeps its own light, blurred along the wall
// only. Returns the edges kept that way.
inline long BlurBlock(uint32_t* ext, const uint32_t* before, int n, int rows, bool above, bool below, uint32_t passes) {
    const int top = above ? 2 : 0, total = top + rows + (below ? 2 : 0);
    for (uint32_t pass = 0; pass < passes; pass++) {
        for (int k = 0; k < total; k++) BlurRow(ext + static_cast<size_t>(k) * n, static_cast<uint32_t>(n));
        for (int x = 0; x < n; x++) BlurColumn(ext + x, total, n);
    }
    long edges = 0;
    for (const int k : {0, rows - 1}) {
        if (k == 0 ? above : below) continue;
        uint32_t* row = ext + static_cast<size_t>(top + k) * n;
        std::memcpy(row, before + static_cast<size_t>(k) * n, static_cast<size_t>(n) * 4);
        for (uint32_t pass = 0; pass < passes; pass++) BlurRow(row, static_cast<uint32_t>(n));
        edges++;
    }
    return edges;
}

// ---- how far a light can reach a texel ----
// The lamp classes' evaluation (bare bulb 0x006BDE90, street lamp 0x006BE020, shaded lamp 0x006BE1C0, spot 0x006BFFB0;
// docs/engine/light-objects-and-rigs.md) gives E0 x min(Cmax, W x max(0, N.l) / d^2) x cone, W = k2 x range x intensity /
// k1 (x 3.333 for the street-lamp class), the cone at most max(1, spill); every Apex test after it only lowers it, and the
// solve drops a light whose r + g + b is under the room's threshold. So beyond
//   d^2 = margin x W x esum x max(1, spill) / thr
// a light adds nothing (esum = r + g + b of its lit colour). margin covers rounding (1.25: 12 % farther). -1 = no bound
// (every batch is solved again), 0 = it adds nothing anywhere.
inline float ReachBound2(float W, float esum, float spill, float thr, float margin = 1.25f) {
    if (!(thr > 1e-5f) || !(W > 0.0f) || !(esum > 0.0f) || !std::isfinite(W) || !std::isfinite(esum)) return W > 0.0f && esum > 0.0f ? -1.0f : 0.0f;
    const float s = std::isfinite(spill) ? std::max(spill, 1.0f) : 1.0f;
    const float d2 = margin * W * esum * s / thr;
    return std::isfinite(d2) ? d2 : -1.0f;
}

// The room lamp probe (OutdoorDelta::SetRoomProbe): one lamp's part of a texel as the solve gave it (g) against the GPU's law
// unshadowed (m). Unlit = both under half a level; Match = within 2 levels; Dark = the solve gave under a level (a test the law
// has no shadow for shaded it); Extra = the law gives under a level; Strength = both lit, a different amount
enum class ProbeClass { Unlit, Match, Dark, Extra, Strength };
inline ProbeClass ProbeClassify(const float g[3], const float m[3]) {
    constexpr float kLevel = 1.0f / 255.0f;
    const float gm = std::max({g[0], g[1], g[2]}), mm = std::max({m[0], m[1], m[2]});
    if (gm < 0.5f * kLevel && mm < 0.5f * kLevel) return ProbeClass::Unlit;
    if (std::max({std::fabs(g[0] - m[0]), std::fabs(g[1] - m[1]), std::fabs(g[2] - m[2])}) <= 2.0f * kLevel) return ProbeClass::Match;
    if (gm < kLevel) return ProbeClass::Dark;
    if (mm < kLevel) return ProbeClass::Extra;
    return ProbeClass::Strength;
}

// ---- GPU room lamps stage 4: the directional maps (FUN_006a09f0; docs/features/night-lighting/gpu-room-lamps.md) ----
// A sample's accumulator is 16 floats, map i's r, g, b, w at 4i: zero, then FUN_0069f280 (the game's step) adds each light of
// the room's list. A light's part (the accumulator after it less before) is kept with its share of the step called again
// there: part = pass x step. False when the step is dark there or the part is not such a multiple (Apex's tests did more
// than scale it); pass is then NaN.
inline bool BasisPass(const float part[16], const float step[16], float& pass) {
    pass = std::nanf("");
    float ss = 0.0f, ps = 0.0f, top = 0.0f;
    for (int i = 0; i < 16; i++) {
        if ((i & 3) == 3) continue; // (w is not packed)
        ss += step[i] * step[i], ps += part[i] * step[i], top = std::max({top, std::fabs(step[i]), std::fabs(part[i])});
    }
    if (!(ss > 1e-24f) || !std::isfinite(ss) || !std::isfinite(ps)) return false;
    const float p = ps / ss;
    for (int i = 0; i < 16; i++)
        if ((i & 3) != 3 && !(std::fabs(part[i] - p * step[i]) <= 2e-6f + 1e-4f * top)) return false; // (the game's acc += rounds)
    pass = p;
    return true;
}
// A lamp's part again from the step now: pass x step (mult: how often the list holds it). False when the step is lit and
// the pass is not known
inline bool BasisPartNow(float pass, int mult, const float step[16], float out[16]) {
    bool lit = false;
    for (int i = 0; i < 16; i++) lit = lit || ((i & 3) != 3 && step[i] != 0.0f);
    if (!lit) {
        std::fill(out, out + 16, 0.0f);
        return true;
    }
    if (!std::isfinite(pass)) return false;
    for (int i = 0; i < 16; i++) out[i] = pass * static_cast<float>(mult) * step[i];
    return true;
}
// Map m's pack input of an accumulator: r, g, b in [0, 1], alpha 1 (0x006A0C93..0x006A0CB6), then the game's pack 0x0060CD10
inline void BasisPackInput(const float acc[16], int m, float out[4]) {
    for (int i = 0; i < 3; i++) {
        const float v = acc[4 * m + i];
        out[i] = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
    }
    out[3] = 1.0f;
}
// A directional texel takes `want` only while it holds `expect` (what the room's capture or Apex's last write left there:
// another room's solve may have written it since); true when it did
inline bool BasisWrite(uint32_t& texel, uint32_t want, uint32_t expect) {
    if (texel != expect) return false;
    texel = want;
    return true;
}
// (physical) Apex's last write (`last`, 4 per sample) carried over the room's next kept solve of the same samples: a texel Apex
// wrote keeps its value; one still expecting the earlier solve's (another room's solve wrote it) takes the new solve's (no change)
inline void CarryBasis(uint32_t* last, const uint32_t* prevGame, const uint32_t* game, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (last[i] == prevGame[i]) last[i] = game[i];
}
// (physical indoor lamps, stage 5) a lamp's directional part at a sample: its room row (mult x norm scale, as the room's maps
// draw it) x its physical share there x the game's step called now; the share is the proxy's visibility x the game's 3D test,
// dropped under `drop` as the game drops it (K_DROP). The raw 3D test, no furniture fill (RtLampGpu::FurnitureFill): a still
// lamp's parts are the game's levels (BasisPartNow), so a fill here alone would light objects under a table only while it moves
inline void BasisPartPhys(float row, float vis, float t3d, float drop, const float step[16], float out[16]) {
    float share = vis * t3d;
    if (drop > share || !std::isfinite(share)) share = 0.0f;
    for (int i = 0; i < 16; i++) out[i] = row * share * step[i];
}
// (physical) a lamp's directional parts over a room's n samples added to acc. A lamp where it stood at the room's kept solve
// (`still`, pass: its kept shares, mult) takes the game's part (BasisPartNow: the game's levels exactly, a value edit
// included); a moved one the physical part x scale (the game's over the physical levels when it last stood still, 1 unknown).
// Furniture lit by these maps showed the physical levels (0.74x the game's in story 1 room 2, [BasisRatio] of 03/11) until a
// game solve wrote its own, then the physical again: objects brighter for a few seconds after each edit. game / phys: the
// still lamp's summed levels both ways (the next move's scale)
inline void BasisLampAdd(bool still, const float* pass, int mult, float row, const float* vis, const float* t3d, float drop, const float* step, size_t n, float scale,
                         float* acc, double& game, double& phys) {
    float p[16], g[16];
    for (size_t i = 0; i < n; i++) {
        BasisPartPhys(row, vis[i], t3d[i], drop, &step[i * 16], p);
        if (still && BasisPartNow(pass[i], mult, &step[i * 16], g)) {
            for (int q = 0; q < 16; q++) {
                acc[i * 16 + q] += g[q];
                if ((q & 3) != 3) game += g[q], phys += p[q];
            }
        } else
            for (int q = 0; q < 16; q++) acc[i * 16 + q] += p[q] * scale;
    }
}

} // namespace OutdoorDeltaModel
