#pragma once
// Banding Fix, smoothed sprite textures (scene_dither.cpp): the pure parts, shared with the offline check
// (tools/sprite_smooth_test). Part of Apex Radiance. Credits: @loinyx
//
// A car headlight's glow (light capture 2026-10-26, draw #229) is a small DXT5 texture, white with a radial glow in its
// alpha, drawn additively by "texld r0, t1, s0; mul r0, r0, v0; mov oC0, r0" over a large part of the screen. DXT5 keeps
// 8 alpha levels per 4x4 block, so the glow's slope changes from texel to texel (+2, +6, +3 levels...) and the
// magnified, bilinear-filtered glow shows rings that neither the 16-bit scene nor the grain can remove. The fix decodes
// the texture once, smooths it with a small Gaussian (only when that changes it by no more than DXT noise) and draws the
// sprite with a 16-bit copy.
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace SpriteSmoothLogic {

// texels. On the captured headlight alpha the slope changed by up to 13 levels from one texel to the next; 1.0 left 1.9,
// 1.5 leaves 1.3 and moves the glow by at most 7.8 levels (its plateau's shoulder), 2.0 leaves 1.1 but moves it by 9.9
constexpr float kSigma = 1.5f;
constexpr UINT kMaxSide = 512;      // larger textures are not glow sprites (and would cost a visible hitch to convert)
constexpr UINT kMinSide = 16;       // below this the blur would reshape the whole sprite
constexpr float kMaxRms = 1.5f;     // levels: the blur may change each channel this much (the captured glow: 0.87)
constexpr float kMaxBigShare = 0.01f; // and at most 1% of texels by more than 4 levels (the glow: 0.4%; textured content 1.4% to 77%)

// Why a sampled texture gets no smoothed copy (developer counters)
enum class Refusal { None, Format, Size, Pool, Detail, Lock, Create, Srgb, Count };
inline const char* RefusalName(Refusal r) {
    static const char* const names[] = {"none", "format", "size", "pool", "detail", "lock", "create", "sRGB"};
    return names[static_cast<int>(r)];
}

// ---- the pixel shader family: colour = texture(s0) x vertex colour ----
// Accepted: ps_2_x "texld rA, tN, s0; mul rB, rA, vC (or vC, rA); mov oC0, rB" (or the mul writing oC0), every register
// full .xyzw, no source modifier or swizzle, _pp / _sat allowed; ps_3_0 the same with vC declared as a COLOR input.
// Anything else (a constant tint, a second texture, more arithmetic) is not this family.
namespace Sm {
inline DWORD Type(DWORD p) { return ((p >> 28) & 7) | ((p >> 8) & 0x18); }
inline DWORD Reg(DWORD p) { return p & 0x7FF; }
constexpr DWORD kTemp = 0, kInput = 1, kTexture = 3, kColorOut = 8, kSampler = 10;
inline bool FullDst(DWORD p) { return ((p >> 16) & 0xF) == 0xF && ((p >> 20) & 0xF & ~3u) == 0 && ((p >> 24) & 0xF) == 0; } // mask .xyzw, only _sat / _pp
inline bool PlainSrc(DWORD p) { return ((p >> 16) & 0xFF) == 0xE4 && ((p >> 24) & 0xF) == 0; }
} // namespace Sm

inline bool IsTintedSprite(const DWORD* t, size_t n) {
    using namespace Sm;
    if (!t || n < 2) return false;
    const DWORD ver = t[0];
    const bool ps3 = ver == 0xFFFF0300u;
    if (!(ver == 0xFFFF0200u || ver == 0xFFFF0201u || ps3)) return false;
    struct Ins {
        DWORD op;
        const DWORD* a;
        DWORD n;
    };
    std::vector<Ins> body;
    uint32_t colorInputs = 0; // ps_3_0: input registers declared COLOR
    for (size_t k = 1; k < n;) {
        const DWORD tok = t[k];
        if (tok == 0x0000FFFFu) break;
        if ((tok & 0xFFFF) == 0xFFFE) { // comment
            k += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const DWORD len = (tok >> 24) & 0xF, op = tok & 0xFFFF;
        if (k + 1 + len > n) return false;
        if (op == 0x1F) { // dcl
            if (ps3 && len == 2 && Type(t[k + 2]) == kInput && (t[k + 1] & 0x1F) == D3DDECLUSAGE_COLOR) colorInputs |= 1u << (Reg(t[k + 2]) & 31);
        } else if (op != 0x51) { // not def
            body.push_back({op, t + k + 1, len});
        }
        k += 1 + len;
    }
    if (body.size() != 2 && body.size() != 3) return false;
    const Ins& tex = body[0];
    const Ins& mul = body[1];
    if (tex.op != 0x42 || tex.n != 3 || mul.op != 0x05 || mul.n != 3) return false;
    if (Type(tex.a[0]) != kTemp || !FullDst(tex.a[0])) return false;
    const DWORD coord = Type(tex.a[1]);
    if (!(ps3 ? coord == kInput : coord == kTexture)) return false;
    if (Type(tex.a[2]) != kSampler || Reg(tex.a[2]) != 0) return false; // sampler 0
    const DWORD a = Reg(tex.a[0]);
    // mul: the texel times a colour input, in either order
    if (!FullDst(mul.a[0]) || !PlainSrc(mul.a[1]) || !PlainSrc(mul.a[2])) return false;
    const DWORD* s1 = &mul.a[1];
    const DWORD* s2 = &mul.a[2];
    if (Type(*s1) == kInput) std::swap(s1, s2);
    if (Type(*s1) != kTemp || Reg(*s1) != a || Type(*s2) != kInput) return false;
    if (ps3 && !(colorInputs & (1u << (Reg(*s2) & 31)))) return false; // ps_3_0: a texture coordinate input is not a tint
    if (ps3 && coord == kInput && (colorInputs & (1u << (Reg(tex.a[1]) & 31)))) return false;
    const DWORD out = mul.a[0];
    if (body.size() == 2) return Type(out) == kColorOut && Reg(out) == 0;
    const Ins& mov = body[2];
    return Type(out) == kTemp && mov.op == 0x01 && mov.n == 2 && Type(mov.a[0]) == kColorOut && Reg(mov.a[0]) == 0 && FullDst(mov.a[0]) &&
           PlainSrc(mov.a[1]) && Type(mov.a[1]) == kTemp && Reg(mov.a[1]) == Reg(out);
}

// ---- DXT1 / DXT3 / DXT5 decoding to RGBA floats, 0..255 per channel ----
// The interpolated colours and alphas are kept unrounded (the GPU rounds them its own way, within half a level).
inline void Rgb565(uint16_t c, float* o) {
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    o[0] = static_cast<float>((r << 3) | (r >> 2));
    o[1] = static_cast<float>((g << 2) | (g >> 4));
    o[2] = static_cast<float>((b << 3) | (b >> 2));
}

inline void ColourBlock(const uint8_t* b, bool dxt1, float* px /* 16 x RGBA */) {
    uint16_t c0, c1;
    uint32_t idx;
    std::memcpy(&c0, b, 2);
    std::memcpy(&c1, b + 2, 2);
    std::memcpy(&idx, b + 4, 4);
    float pal[4][4] = {};
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255.0f;
    const bool four = !dxt1 || c0 > c1; // DXT3 / DXT5 colour blocks always use four colours
    for (int c = 0; c < 3; c++) {
        if (four) {
            pal[2][c] = (2.0f * pal[0][c] + pal[1][c]) / 3.0f;
            pal[3][c] = (pal[0][c] + 2.0f * pal[1][c]) / 3.0f;
        } else {
            pal[2][c] = 0.5f * (pal[0][c] + pal[1][c]);
            pal[3][c] = 0.0f;
        }
    }
    if (!four) pal[3][3] = 0.0f; // DXT1 three-colour mode: index 3 is transparent black
    for (int i = 0; i < 16; i++) std::memcpy(px + 4 * i, pal[(idx >> (2 * i)) & 3], sizeof(float) * 4);
}

inline void AlphaBlock5(const uint8_t* b, float* px) {
    const float a0 = b[0], a1 = b[1];
    float pal[8] = {a0, a1};
    if (b[0] > b[1]) {
        for (int i = 2; i < 8; i++) pal[i] = ((8 - i) * a0 + (i - 1) * a1) / 7.0f;
    } else {
        for (int i = 2; i < 6; i++) pal[i] = ((6 - i) * a0 + (i - 1) * a1) / 5.0f;
        pal[6] = 0.0f;
        pal[7] = 255.0f;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; i++) px[4 * i + 3] = pal[(bits >> (3 * i)) & 7];
}

inline void AlphaBlock3(const uint8_t* b, float* px) {
    for (int i = 0; i < 16; i++) px[4 * i + 3] = static_cast<float>(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
}

// w and h multiples of 4; pitch = bytes from one row of blocks to the next. False for another format.
inline bool Decode(D3DFORMAT f, const uint8_t* src, int pitch, int w, int h, float* out) {
    if (f != D3DFMT_DXT1 && f != D3DFMT_DXT3 && f != D3DFMT_DXT5) return false;
    const int blockBytes = f == D3DFMT_DXT1 ? 8 : 16;
    float px[64];
    for (int by = 0; by < h / 4; by++)
        for (int bx = 0; bx < w / 4; bx++) {
            const uint8_t* b = src + by * pitch + bx * blockBytes;
            ColourBlock(f == D3DFMT_DXT1 ? b : b + 8, f == D3DFMT_DXT1, px);
            if (f == D3DFMT_DXT5) AlphaBlock5(b, px);
            if (f == D3DFMT_DXT3) AlphaBlock3(b, px);
            for (int y = 0; y < 4; y++) std::memcpy(out + 4 * ((by * 4 + y) * w + bx * 4), px + 16 * y, sizeof(float) * 16);
        }
    return true;
}

// ---- smoothing ----
// Separable Gaussian on all four channels, mirrored at the edges (each texel's weight sums to 1 in both directions, so
// the image's mean is kept exactly). Needs w, h > 3 sigma.
inline void Gaussian(float* rgba, int w, int h, float sigma) {
    const int r = static_cast<int>(std::ceil(3.0f * sigma));
    std::vector<float> k(2 * r + 1);
    float sum = 0.0f;
    for (int i = -r; i <= r; i++) sum += k[i + r] = std::exp(-0.5f * (i / sigma) * (i / sigma));
    for (float& v : k) v /= sum;
    auto mirror = [](int i, int n) { return i < 0 ? -i - 1 : i >= n ? 2 * n - i - 1 : i; };
    std::vector<float> tmp(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 4; c++) {
                float s = 0.0f;
                for (int i = -r; i <= r; i++) s += k[i + r] * rgba[4 * (y * w + mirror(x + i, w)) + c];
                tmp[4 * (y * w + x) + c] = s;
            }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 4; c++) {
                float s = 0.0f;
                for (int i = -r; i <= r; i++) s += k[i + r] * tmp[4 * (mirror(y + i, h) * w + x) + c];
                rgba[4 * (y * w + x) + c] = s;
            }
}

// A smooth gradient: the blur changed every channel by no more than DXT noise (kMaxRms, kMaxBigShare). Textured content
// (a detailed particle, a cut-out edge) changes more and keeps the game's texture.
inline bool SmoothEnough(const float* before, const float* after, int w, int h) {
    const size_t n = static_cast<size_t>(w) * h;
    for (int c = 0; c < 4; c++) {
        double sq = 0.0;
        size_t big = 0;
        for (size_t i = 0; i < n; i++) {
            const double d = before[4 * i + c] - after[4 * i + c];
            sq += d * d;
            big += std::fabs(d) > 4.0;
        }
        if (std::sqrt(sq / n) > kMaxRms || big > kMaxBigShare * n) return false;
    }
    return true;
}

// The next mip level: 2x2 box average (a side of 1 stays 1), which keeps the mean
inline void Half(const float* src, int w, int h, float* dst) {
    const int w2 = std::max(1, w / 2), h2 = std::max(1, h / 2);
    for (int y = 0; y < h2; y++)
        for (int x = 0; x < w2; x++)
            for (int c = 0; c < 4; c++) {
                const int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1), y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
                dst[4 * (y * w2 + x) + c] = 0.25f * (src[4 * (y0 * w + x0) + c] + src[4 * (y0 * w + x1) + c] + src[4 * (y1 * w + x0) + c] + src[4 * (y1 * w + x1) + c]);
            }
}

// One texel into the copy's format: A16B16G16R16 (R in the low 16 bits) or A8R8G8B8 (bytes B, G, R, A)
inline uint16_t To16(float v) { return static_cast<uint16_t>(std::clamp(v, 0.0f, 255.0f) * 257.0f + 0.5f); }
inline uint8_t To8(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f); }

} // namespace SpriteSmoothLogic
