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
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = 65504.0f;
    else v = std::ldexp(static_cast<float>(m | 0x400u), static_cast<int>(e) - 25);
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

} // namespace OutdoorDeltaModel
