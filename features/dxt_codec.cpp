// The Sims 3's CPU DXT1 / DXT5 encoders: the game's algorithm translated (Ref) and the same arithmetic four blocks at a time
// (Fast). See dxt_codec.h; the address-by-address notes are in docs/features/performance.md ("Faster Texture Compression").
//
// Part of Apex Radiance. Credits: @loinyx
#include "dxt_codec.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <intrin.h>
#include <atomic>
#include <cstring>
#include <mutex>

#if !defined(_M_IX86)
#error "dxt_codec.cpp is x86 only (the game is 32-bit and the x87 compare uses inline assembly)"
#endif
#if defined(__AVX__)
#error "Build dxt_codec.cpp with /arch:SSE2 (the default for x86): rcp/rsqrt must be the same legacy SSE instructions as the game's"
#endif

// Same IEEE operations as the game, one rounding per operation, no contraction
#pragma float_control(precise, on)
#pragma fp_contract(off)

namespace DxtCodec {
namespace {

inline uint32_t Bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
inline float Flt(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
inline uint32_t Load32(const uint8_t* p) {
    uint32_t u;
    std::memcpy(&u, p, 4);
    return u;
}
inline void Put16(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void Put32(uint8_t* p, uint32_t v) {
    Put16(p, v & 0xFFFF);
    Put16(p + 2, v >> 16);
}

// ---- constants, read from TS3W.exe .rdata (Steam 1.67.2) ----
constexpr uint32_t kB_Eps = 0x3727C5AC;     // 1e-5: [0x00FE82C0] (variance tests) and [0x00F9D2C8] (x87 range test)
constexpr uint32_t kB_ExtThr = 0x3B91A2B4;  // [0x00FE8288] squared endpoint distance below which the endpoints are pushed apart
constexpr uint32_t kB_1_31 = 0x3D042108;    // [0x00FE8284] 1/31 (push distance; decode of 5-bit channels)
constexpr uint32_t kB_1_63 = 0x3C820821;    // [0x00FAD528] 1/63 (decode of the 6-bit channel)
constexpr uint32_t kB_1_16 = 0x3D800000;    // [0x00FE82D0] / [0x00FE8280]
constexpr uint32_t kB_1_256 = 0x3B800000;   // [0x0108AF84]
constexpr uint32_t kB_248_255 = 0x3F78F8F9; // [0x00FE818C]
constexpr uint32_t kB_252_255 = 0x3F7CFCFD; // [0x00FE8190]
constexpr uint32_t kB_Magic5 = 0x48C00000;  // [0x00FE8178] 393216 = 1.5 * 2^18: adding it rounds to multiples of 1/32
constexpr uint32_t kB_Magic6 = 0x48400000;  // [0x00FE8174] 196608 = 1.5 * 2^17: rounds to multiples of 1/64
constexpr uint32_t kB_MagicI = 0x4B400000;  // [0x00FE8164] 12582912 = 1.5 * 2^23: rounds to integers
constexpr uint32_t kB_One = 0x3F800000;     // [0x0107A538]
constexpr uint32_t kB_Half = 0x3F000000;    // [0x00F9A5AC]
constexpr uint32_t kB_MinusOne = 0xBF800000; // [0x00FAA210]
constexpr uint32_t kB_PowEps = 0x34000000;  // [0x00FE8188] 2^-23
constexpr uint32_t kB_Three = 0x40400000;   // [0x01074010]
constexpr uint32_t kB_Two = 0x40000000;     // [0x00F98A58]
constexpr uint32_t kB_Sixteen = 0x41800000; // [0x00FA3130]
constexpr uint32_t kB_Eighth = 0x3E000000;  // [0x00FE8278]
constexpr uint32_t kB_2_25 = 0x40100000;    // [0x00FE8230]
constexpr uint32_t kB_FltMax = 0x7F7FFFFF;  // [0x00FAA218]
constexpr uint32_t kB_L0 = 0x3E99999A;      // 0.3  [0x00FE81A0] / [0x00F9D504]
constexpr uint32_t kB_L1 = 0x3F170A3D;      // 0.59 [0x00FE81A4] / [0x00FE827C]
constexpr uint32_t kB_L2 = 0x3DE147AE;      // 0.11 [0x00FE81A8] / [0x00FC1300]
// Ordered dither added to the index of pixel k (row-major): the 4x4 Bayer matrix / 16 - 0.5, [0x00FE8238]
constexpr uint32_t kB_Dither[16] = {0xBF000000, 0x00000000, 0xBEC00000, 0x3E000000, 0x3E800000, 0xBE800000, 0x3EC00000, 0xBE000000,
                                    0xBEA00000, 0x3E400000, 0xBEE00000, 0x3D800000, 0x3EE00000, 0xBD800000, 0x3EA00000, 0xBE400000};
// 4-colour blocks: rank along the axis 0..3 -> DXT code 0, 2, 3, 1 (words at [0x00FE8228])
constexpr uint32_t kMap4[4] = {0, 2, 3, 1};

inline __m128 Mask3() { return _mm_castsi128_ps(_mm_setr_epi32(-1, -1, -1, 0)); } // [0x00FE82A0]
inline __m128 Splat(uint32_t bits) { return _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(bits))); }
inline float Lane(__m128 v, int i) {
    alignas(16) float f[4];
    _mm_store_ps(f, v);
    return f[i];
}

// 0x0061473F..0x0061475B: fld [hi]; fsub [lo]; fabs; fld [0x00F9D2C8]; fcomip st,st(1); fstp st(0); jbe. True when the
// game adds 1.0 to hi: 1e-5 > |hi - lo|, computed by the x87 unit with the calling thread's precision control (Direct3D
// sets 24-bit on the device thread, other threads keep 53-bit), which is why this one test runs the same x87 code.
__declspec(noinline) bool X87RangeBelowEps(float hi, float lo) {
    static const uint32_t eps = kB_Eps;
    unsigned char r;
    __asm {
        fld dword ptr [hi]
        fsub dword ptr [lo]
        fabs
        fld dword ptr [eps]
        fcomip st(0), st(1)
        fstp st(0)
        seta al
        mov r, al
    }
    return r != 0;
}

// cvtsi2ss / 565 packing helpers (0x0061440C..0x006145F4). The clamp is the game's byte trick, kept as is (it differs from
// a plain clamp only for values no finite input reaches).
inline uint32_t ClampField(int32_t x, uint32_t maxv) {
    const uint8_t lo = static_cast<uint8_t>(static_cast<uint8_t>(x) & static_cast<uint8_t>(~static_cast<uint32_t>(x >> 31)));
    const uint8_t hi = static_cast<uint8_t>(static_cast<int32_t>(maxv - static_cast<uint32_t>(x)) >> 31);
    return static_cast<uint32_t>(lo | hi) & maxv;
}

// ---- the object the game's helpers work on (the drivers' local at esp+0x30, 16-aligned) ----
struct alignas(16) Block {
    __m128 axis;   // +0x000
    __m128 px[16]; // +0x010 {R/256, G/256, B/256, luma}, row-major
    __m128 ep0;    // +0x110 endpoint on the negative side of the axis
    __m128 ep1;    // +0x120 positive side
};

// 0x00614000 thiscall(block, src, pitch), ret 8: a full 4x4 block. R, G, B become exactly x/256 through float bit
// tricks (R: 7 low bits in the mantissa of 0.5, plus 0.5 or 1.0 for bit 7).
void FetchFull(Block& b, const uint8_t* src, int32_t pitch) {
    const __m128 mask = _mm_castsi128_ps(_mm_setr_epi32(0x007F0000, 0x0000FF00, 0x000000FF, 0x00800000));                     // [0x00FE81D0]
    const __m128 orv = _mm_castsi128_ps(_mm_setr_epi32(0x3F000000, 0x43000000, 0x47000000, 0x3F000000));                      // [0x00FE81C0]
    const __m128 addv = _mm_castsi128_ps(_mm_setr_epi32(static_cast<int>(0xBF800000), static_cast<int>(0xC3000000), static_cast<int>(0xC7000000), 0)); // [0x00FE81B0]
    const __m128 lw = _mm_castsi128_ps(_mm_setr_epi32(kB_L0, kB_L1, kB_L2, 0));                                               // [0x00FE81A0]
    for (int r = 0; r < 4; r++) {
        const uint8_t* row = src + static_cast<intptr_t>(pitch) * r;
        for (int c = 0; c < 4; c++) {
            __m128 x = _mm_castsi128_ps(_mm_cvtsi32_si128(static_cast<int>(Load32(row + 4 * c)))); // movss xmm0,[eax]
            x = _mm_shuffle_ps(x, x, 0);                                                            // shufps xmm0,xmm0,0
            x = _mm_add_ps(_mm_or_ps(_mm_and_ps(x, mask), orv), addv);                              // andps / orps / addps
            x = _mm_add_ss(x, _mm_shuffle_ps(x, x, 0xFF));                                          // shufps 0FFh; addss
            const __m128 m = _mm_mul_ps(x, lw);                                                     // mulps xmm0,[0x00FE81A0]
            __m128 t = _mm_add_ss(_mm_movehl_ps(m, m), m);                                          // movhlps xmm5,xmm0; addss xmm5,xmm0
            t = _mm_add_ss(_mm_shuffle_ps(m, m, 0x55), t);                                          // shufps 55h; addss xmm0,xmm5
            alignas(16) float f[4];
            _mm_store_ps(f, x);                   // movaps [ecx+ebx+110h],xmm0
            f[3] = _mm_cvtss_f32(t);              // movss [ecx+ebx+11Ch],xmm0 (luma)
            b.px[r * 4 + c] = _mm_load_ps(f);
        }
    }
}

// 0x00614C50 thiscall(block, src, pitch, cols, rows), ret 10h: a partial block. Each row's last pixel is repeated up to 4
// columns, then the last row is repeated up to 4 rows.
void FetchPartial(Block& b, const uint8_t* src, int32_t pitch, uint32_t cols, uint32_t rows) {
    const __m128 lw = _mm_castsi128_ps(_mm_setr_epi32(kB_L0, kB_L1, kB_L2, 0)); // xmm5 = {[0x00F9D504], [0x00FE827C], [0x00FC1300], 0}
    const __m128 scale = Splat(kB_1_256);                                        // xmm3 = [0x0108AF84] broadcast
    uint32_t k = 0;                                                              // esi
    const uint8_t* row = src;
    for (uint32_t r = 0; r < rows; r++, row += pitch) {
        for (uint32_t c = 0; c < cols; c++) {
            const uint8_t* p = row + 4 * c;
            __m128 v = _mm_setr_ps(static_cast<float>(p[2]), static_cast<float>(p[1]), static_cast<float>(p[0]), 0.0f); // cvtsi2ss x3; lane 3 = 0
            v = _mm_mul_ps(scale, v);                                           // mulps xmm0(scale),xmm1
            const __m128 m = _mm_mul_ps(v, lw);                                 // mulps xmm0,xmm5
            const __m128 h1 = _mm_add_ps(_mm_shuffle_ps(m, m, 0x0E), m);        // shufps xmm1,xmm0,0Eh; addps xmm1,xmm0
            const __m128 h0 = _mm_add_ps(_mm_shuffle_ps(h1, h1, 0x01), h1);     // shufps xmm0,xmm1,1; addps xmm0,xmm1
            alignas(16) float f[4];
            _mm_store_ps(f, v);
            f[3] = _mm_cvtss_f32(h0); // shufps xmm2,xmm0,0Ah; shufps xmm1,xmm2,84h: lane 3 = luma
            b.px[k++] = _mm_load_ps(f);
        }
        if (cols < 4)
            for (uint32_t c = cols; c < 4; c++, k++) b.px[k] = b.px[k - 1]; // 0x00614D76 loop
    }
    for (; k < 16; k++) b.px[k] = b.px[k - 4]; // 0x00614DB0 loop
}

// 0x00613F80 thiscall(alpha[16], src, pitch, cols, rows), ret 10h: the alpha of a partial-width block. The game's column
// fill writes one copy of the row's last value and does not advance (its loop at 0x00613FD1 is empty), so the rows end
// up packed cols apart; then the last 4 values are repeated to 16. Kept as the game does it (only right-edge blocks of
// images whose width is not a multiple of 4).
// When rows * cols < 4 (a 1..3-pixel block in the last row) the fill (rep movsd from [ecx+eax*4-10h]) starts BEFORE the
// array and copies the DXT5 driver's four locals that precede it (the array is at the driver's esp+40h): [esp+30h] =
// this block's source pointer (stored at 0x0061561E just before the call), [esp+34h] = y (pixel row of the block row),
// [esp+38h] = height, [esp+3Ch] = the destination row padding (pitch - blocksX * 16). `before` holds those four dwords.
// (Only the last-row path reaches this; with 4 rows n >= 4.)
struct AlphaLocals {
    int32_t v[4];
};
AlphaLocals DriverLocals(const uint8_t* blockSrc, uint32_t y, uint32_t height, uint32_t pad) {
    return AlphaLocals{{static_cast<int32_t>(reinterpret_cast<uintptr_t>(blockSrc)), static_cast<int32_t>(y), static_cast<int32_t>(height), static_cast<int32_t>(pad)}};
}
void FetchAlphaPartial(int32_t a[16], const uint8_t* src, int32_t pitch, uint32_t cols, uint32_t rows, const AlphaLocals& before) {
    int32_t buf[20]; // buf[0..3] = the driver's locals, buf[4..19] = the array
    std::memcpy(buf, before.v, sizeof before.v);
    int32_t* const arr = buf + 4;
    uint32_t n = 0; // eax
    const uint8_t* row = src + 3;
    for (uint32_t r = 0; r < rows; r++, row += pitch) {
        for (uint32_t c = 0; c < cols; c++) arr[n++] = row[4 * c];
        if (cols < 4) arr[n] = arr[n - 1];
    }
    for (; n < 16; n++) arr[n] = arr[static_cast<int32_t>(n) - 4]; // rep movsd from 16 bytes back (from the locals when n < 4)
    std::memcpy(a, arr, 16 * sizeof(int32_t));
}

// 0x00614DD0 thiscall(block, &mean, &sum, &var), ret 0Ch (mean unused): the axis of a block whose luma is flat. Power
// iteration (16 steps, rsqrtps + 2 Newton steps per normalisation) on the covariance matrix scaled by max(1, 1/|var|);
// start = the matrix row of the largest variance. Returns the luma weights when the product vanishes.
__m128 PowerAxis(const __m128* px, const __m128& sum, const __m128& var) {
    const __m128 zero = _mm_setzero_ps(), mask3 = Mask3();
    __m128 cross = zero; // xmm6
    for (int k = 0; k < 16; k++) {
        const __m128 x = px[k];
        const __m128 perm = _mm_and_ps(_mm_shuffle_ps(x, x, _MM_SHUFFLE(3, 0, 2, 1)), mask3); // [esp] = {G, B, R, 0}
        cross = _mm_add_ps(_mm_mul_ps(perm, x), cross);                                       // mulps xmm2,xmm0; addps xmm2,xmm6
    }
    const __m128 one = Splat(kB_One), half = Splat(kB_Half);
    const __m128 vv = _mm_mul_ps(var, var);                            // mulps xmm1,xmm0
    const __m128 h1 = _mm_add_ps(_mm_shuffle_ps(vv, vv, 0x0E), vv);    // shufps xmm2,xmm1,0Eh; addps xmm2,xmm1
    __m128 n2 = _mm_add_ps(_mm_shuffle_ps(h1, h1, 0x01), h1);          // shufps xmm1,xmm2,1; addps xmm1,xmm2
    n2 = _mm_shuffle_ps(n2, n2, 0);                                    // shufps xmm1,xmm1,0
    const __m128 y = _mm_rsqrt_ps(n2);                                 // rsqrtps xmm2,xmm1
    const __m128 t = _mm_mul_ps(_mm_mul_ps(y, y), n2);                 // mulps xmm7,xmm2; mulps xmm7,xmm1
    const __m128 y1 = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(one, t), _mm_mul_ps(half, y)), y); // subps; mulps; mulps; addps
    const __m128 scale = _mm_max_ps(one, y1);                          // movaps xmm2,xmm3; maxps xmm2,xmm1
    const __m128 sperm = _mm_and_ps(_mm_shuffle_ps(sum, sum, _MM_SHUFFLE(3, 0, 2, 1)), mask3); // {sG, sB, sR, 0}
    const __m128 s2 = _mm_mul_ps(_mm_mul_ps(sperm, sum), Splat(kB_1_256));                      // mulps xmm7,xmm5; mulps xmm7,xmm1
    const __m128 v = _mm_mul_ps(var, scale);                                                     // mulps xmm0,xmm2
    __m128 c = _mm_mul_ps(Splat(kB_1_16), cross);                                                // mulps xmm1,xmm6
    c = _mm_sub_ps(c, s2);                                                                       // subps xmm1,xmm7
    c = _mm_mul_ps(c, scale);                                                                    // mulps xmm1,xmm2
    // rows of the matrix (c = {cGR, cBG, cRB, .}, v = {vR, vG, vB, vL})
    const __m128 row1 = _mm_shuffle_ps(_mm_shuffle_ps(c, v, 0x10), c, 0x18); // {cGR, vG, cBG, cGR}
    const __m128 row0 = _mm_shuffle_ps(_mm_shuffle_ps(v, c, 0x00), c, 0x28); // {vR, cGR, cRB, cGR}
    const __m128 row2 = _mm_shuffle_ps(_mm_shuffle_ps(c, c, 0x12), v, 0x28); // {cRB, cBG, vB, vR}
    const float vR = Lane(v, 0), vG = Lane(v, 1), vB = Lane(v, 2);
    __m128 vec;
    if (vR > vG) vec = (vR > vB) ? row0 : row2; // comiss / jbe at 0x00614F61, 0x00614F82
    else vec = (vG > vB) ? row1 : row2;         // 0x00614F9A
    const __m128 negOne = Splat(kB_MinusOne), eps = Splat(kB_PowEps);
    for (int it = 0; it < 16; it++) {
        __m128 w = _mm_mul_ps(_mm_shuffle_ps(vec, vec, 0x55), row1);          // v1 * row1
        w = _mm_add_ps(w, _mm_mul_ps(_mm_shuffle_ps(vec, vec, 0x00), row0));  // + v0 * row0
        w = _mm_add_ps(w, _mm_mul_ps(_mm_shuffle_ps(vec, vec, 0xAA), row2));  // + v2 * row2
        const __m128 a = _mm_max_ps(_mm_mul_ps(negOne, w), w);               // |w|: mulps xmm0(-1),xmm1; maxps xmm0,xmm1
        if ((_mm_movemask_ps(_mm_cmple_ps(a, eps)) & 7) == 7)                 // cmpleps; movmskps; and 7
            return _mm_castsi128_ps(_mm_setr_epi32(kB_L0, kB_L1, kB_L2, 0));   // 0x006150B8: the luma weights
        const __m128 sq = _mm_mul_ps(w, w);
        __m128 n = _mm_add_ps(_mm_shuffle_ps(sq, sq, 0x02), _mm_shuffle_ps(sq, sq, 0x01)); // sq2 + sq1
        n = _mm_add_ps(n, sq);                                                              // + sq0
        n = _mm_shuffle_ps(n, n, 0);
        const __m128 y0 = _mm_rsqrt_ps(n);
        const __m128 t0 = _mm_mul_ps(_mm_mul_ps(y0, y0), n);
        const __m128 ya = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(one, t0), _mm_mul_ps(half, y0)), y0);
        const __m128 t1 = _mm_mul_ps(_mm_mul_ps(ya, ya), n);
        const __m128 yb = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(one, t1), _mm_mul_ps(half, ya)), ya);
        vec = _mm_mul_ps(yb, w);                                                            // mulps xmm0,xmm1
    }
    return _mm_and_ps(vec, mask3); // [esp+0Ch] = 0
}

// 0x00615100 thiscall(block), ret: principal axis and the two endpoints (mean -/+ the centroids of the pixels on each
// side of the mean along the axis).
void Endpoints(Block& b) {
    const __m128 zero = _mm_setzero_ps();
    __m128 s = zero, q = zero, pl = zero; // xmm4, xmm5, xmm6
    for (int k = 0; k < 16; k++) {
        const __m128 x = b.px[k];
        s = _mm_add_ps(s, x);                                            // addps xmm4,xmm0
        q = _mm_add_ps(q, _mm_mul_ps(x, x));                             // mulps xmm0,xmm0; addps xmm5,xmm0
        pl = _mm_add_ps(pl, _mm_mul_ps(_mm_shuffle_ps(x, x, 0xFF), x));  // shufps 0FFh; mulps xmm1,xmm2; addps xmm6,xmm1
    }
    const __m128 c16 = Splat(kB_1_16);
    const __m128 mean = _mm_mul_ps(s, c16);                                     // mulps xmm4,[0x00FE82D0]
    const __m128 var = _mm_sub_ps(_mm_mul_ps(c16, q), _mm_mul_ps(mean, mean));  // mulps xmm1,xmm5; subps xmm1,xmm0
    const __m128 h = _mm_add_ps(_mm_movehl_ps(var, var), var);                  // movhlps xmm1,xmm1; addps xmm1,xmm2
    const float tot = _mm_cvtss_f32(_mm_add_ss(_mm_shuffle_ps(h, h, 0x55), h)); // (vL + vG) + (vB + vR)
    const float eps = Flt(kB_Eps);
    if (!(tot >= eps)) { // comiss; jae 0x00615188
        b.axis = _mm_castsi128_ps(_mm_setr_epi32(kB_One, kB_One, kB_One, 0)); // [0x00FE82B0]
        b.ep0 = mean;
        b.ep1 = mean;
        return;
    }
    b.ep0 = zero;
    b.ep1 = zero;
    if (Lane(var, 3) > eps) { // luma varies: axis = covariance of R, G, B with the luma (0x006151E0)
        const __m128 cov = _mm_sub_ps(_mm_mul_ps(pl, c16), _mm_mul_ps(_mm_shuffle_ps(mean, mean, 0xFF), mean));
        const __m128 sq = _mm_mul_ps(cov, cov);
        __m128 n = _mm_add_ss(_mm_movehl_ps(sq, sq), sq); // movhlps xmm0,xmm6; addss xmm0,xmm6: sqB + sqR
        n = _mm_add_ss(n, _mm_shuffle_ps(sq, sq, 0x55));   // + sqG
        n = _mm_rsqrt_ss(n);                               // rsqrtss xmm0,xmm0
        b.axis = _mm_mul_ps(_mm_and_ps(cov, Mask3()), _mm_shuffle_ps(n, n, 0));
    } else {
        b.axis = PowerAxis(b.px, s, var); // 0x006151D0
    }
    // 0x0061521E: signed projections; weighted sums of the pixels on each side
    __m128 w = zero, nsum = zero, psum = zero; // xmm5, xmm6, xmm7
    for (int k = 0; k < 16; k++) {
        const __m128 x = b.px[k];
        const __m128 d = _mm_mul_ps(_mm_sub_ps(x, mean), b.axis);  // subps xmm0,xmm4; mulps xmm0,[ecx]
        __m128 t = _mm_add_ss(_mm_movehl_ps(d, d), d);             // dB + dR
        t = _mm_add_ss(t, _mm_shuffle_ps(d, d, 0x55));             // + dG
        t = _mm_shuffle_ps(t, t, 0);
        const __m128 mn = _mm_min_ps(zero, t);                     // minps xmm0(0),xmm2
        const __m128 mx = _mm_max_ps(zero, t);                     // maxps xmm3(0),xmm2
        w = _mm_add_ps(w, _mm_movelh_ps(mn, mx));                  // {mn, mn, mx, mx}
        nsum = _mm_sub_ps(nsum, _mm_mul_ps(mn, x));                // mulps xmm0,xmm1; subps xmm6,xmm0
        psum = _mm_add_ps(psum, _mm_mul_ps(mx, x));                // mulps xmm3,xmm1; addps xmm7,xmm3
    }
    w = _mm_xor_ps(w, _mm_castsi128_ps(_mm_setr_epi32(static_cast<int>(0x80000000), 0, 0, 0))); // [0x00FE8290]
    __m128 rmx = _mm_rcp_ss(_mm_movehl_ps(w, w)); // movhlps xmm4,xmm5; rcpss xmm0,xmm4: 1 / sum(max)
    __m128 rmn = _mm_rcp_ss(w);                   // rcpss xmm1,xmm5: 1 / -sum(min)
    rmx = _mm_shuffle_ps(rmx, rmx, 0);
    rmn = _mm_shuffle_ps(rmn, rmn, 0);
    __m128 e0 = _mm_mul_ps(rmx, nsum);            // mulps xmm0,xmm6
    __m128 e1 = _mm_mul_ps(rmn, psum);            // mulps xmm1,xmm7
    const __m128 diff = _mm_sub_ps(e1, e0);
    const __m128 dd = _mm_mul_ps(diff, diff);
    const __m128 h2 = _mm_add_ps(_mm_movehl_ps(dd, dd), dd);                       // movhlps xmm3,xmm2; addps xmm3,xmm2
    const __m128 d2 = _mm_add_ss(h2, _mm_shuffle_ps(h2, h2, 0x55));                // (ddB + ddR) + (ddL + ddG)
    if (!(_mm_cvtss_f32(d2) >= Flt(kB_ExtThr))) {                                  // comiss; jae 0x006152D5
        __m128 sc = _mm_mul_ss(_mm_rsqrt_ss(d2), _mm_set_ss(Flt(kB_1_31)));       // rsqrtss; mulss [0x00FE8284]
        sc = _mm_shuffle_ps(sc, sc, 0);
        const __m128 ext = _mm_mul_ps(diff, sc);
        e1 = _mm_add_ps(e1, ext);
        e0 = _mm_sub_ps(e0, ext);
    }
    b.ep0 = e0;
    b.ep1 = e1;
}

// 0x0061440C..0x006145F4: endpoint -> RGB565 (r * 248/255 + 1.5*2^18 rounds to 1/32, the bits minus the magic are the
// integer; clamped with the byte trick)
uint32_t Quantize565(const __m128& e) {
    alignas(16) float f[4];
    _mm_store_ps(f, e);
    float r = f[0] * Flt(kB_248_255);
    r = r + Flt(kB_Magic5);
    float g = f[1] * Flt(kB_252_255);
    g = g + Flt(kB_Magic6);
    float b = f[2] * Flt(kB_248_255);
    b = b + Flt(kB_Magic5);
    const uint32_t R = ClampField(static_cast<int32_t>(Bits(r) - kB_Magic5), 31);
    const uint32_t G = ClampField(static_cast<int32_t>(Bits(g) - kB_Magic6), 63);
    const uint32_t B = ClampField(static_cast<int32_t>(Bits(b) - kB_Magic5), 31);
    return (R << 11) | (G << 5) | B;
}

// 0x0061461D..0x00614675: RGB565 -> floats (cvtsi2ss, * 1/31 or 1/63), lane 3 = 0
__m128 Decode565(uint32_t c) {
    const float b = static_cast<float>(static_cast<int>(c & 0x1F)) * Flt(kB_1_31);
    const float g = static_cast<float>(static_cast<int>((c >> 5) & 0x3F)) * Flt(kB_1_63);
    const float r = static_cast<float>(static_cast<int>(c >> 11)) * Flt(kB_1_31);
    return _mm_setr_ps(r, g, b, 0.0f);
}

// mulps; shufps 0Eh; addps; shufps 1; addps: (x3 + x1) + (x2 + x0)
float Dot(const __m128& a, const __m128& b) {
    const __m128 x = _mm_mul_ps(a, b);
    const __m128 h1 = _mm_add_ps(_mm_shuffle_ps(x, x, 0x0E), x);
    const __m128 h0 = _mm_add_ps(_mm_shuffle_ps(h1, h1, 0x01), h1);
    return _mm_cvtss_f32(h0);
}

// The block's 8 colour bytes from the two colours and the index dwords (0x006149E9..0x00614C44)
void PackColor(uint8_t* out, uint32_t c0, uint32_t c1, bool three, uint32_t idx) {
    if (three) {
        idx ^= (((idx >> 1) ^ idx) & 0x55555555u) * 3u; // codes 1 <-> 2 (0x00614A89)
        if (c0 > c1) {                                  // 3-colour needs colour0 <= colour1: swap, codes 0 <-> 1
            idx ^= (~idx >> 1) & 0x55555555u;
            Put16(out, c1);
            Put16(out + 2, c0);
        } else {
            Put16(out, c0);
            Put16(out + 2, c1);
        }
    } else if (c0 > c1) {
        Put16(out, c0);
        Put16(out + 2, c1);
    } else { // 4-colour needs colour0 > colour1: swap, codes 0 <-> 1 and 2 <-> 3
        idx ^= 0x55555555u;
        Put16(out, c1);
        Put16(out + 2, c0);
    }
    Put32(out + 4, idx);
}

// 0x006143F0 thiscall(block, out, dxt1), ret 8: the colour block. dxt1 = also try the 3-colour mode (3 codes, error x 2.25).
void EncodeColor(const Block& b, uint8_t* out, bool dxt1) {
    const uint32_t c0 = Quantize565(b.ep0), c1 = Quantize565(b.ep1);
    if (c0 == c1) { // 0x00614600
        Put16(out, c0);
        Put16(out + 2, c0);
        Put32(out + 4, 0);
        return;
    }
    const float p0 = Dot(Decode565(c0), b.axis), p1 = Dot(Decode565(c1), b.axis);
    float lo = p0, hi = p1;
    if (p0 > p1) { // comiss xmm6,xmm0; jbe 0x0061473F
        lo = p1;
        hi = p0;
    }
    if (X87RangeBelowEps(hi, lo)) hi = hi + Flt(kB_One);
    const float range = hi - lo;
    const float inv = Flt(kB_One) / range;
    const float inv3 = inv * Flt(kB_Three);
    float t0 = Flt(kB_Eighth) - range;
    t0 = t0 * Flt(kB_Sixteen);
    const float s = Flt(kB_One) - t0;
    const float inv2 = inv * Flt(kB_Two);
    const uint32_t d1 = (Bits(s) & 0x80000000u) ? kB_One : Bits(t0); // dither strength clamped to [0, 1] with sign bits
    const float d = Flt((d1 & 0x80000000u) ? 0u : d1);
    const float magic = Flt(kB_MagicI);
    int32_t i3[16] = {}, i4[16] = {};
    float err3 = 0.0f, err4 = 0.0f;
    for (int k = 0; k < 16; k++) {
        const float dv = Flt(kB_Dither[k]) * d;
        const float u = Dot(b.axis, b.px[k]) - lo;
        if (dxt1) { // 0x00614823
            float v3 = u * inv2;
            v3 = v3 + dv;
            float v4 = u * inv3;
            v4 = v4 + dv;
            int32_t a = static_cast<int32_t>(Bits(v3 + magic) - kB_MagicI);
            int32_t c = static_cast<int32_t>(Bits(v4 + magic) - kB_MagicI);
            a = a < 0 ? 0 : (a > 2 ? 2 : a);
            c = c < 0 ? 0 : (c > 3 ? 3 : c);
            const float e3 = static_cast<float>(a) - v3;
            err3 = e3 * e3 + err3;
            const float e4 = static_cast<float>(c) - v4;
            err4 = e4 * e4 + err4;
            i3[k] = a;
            i4[k] = c;
        } else { // 0x00614942
            float v4 = u * inv3;
            v4 = v4 + dv;
            int32_t c = static_cast<int32_t>(Bits(v4 + magic) - kB_MagicI);
            c = c < 0 ? 0 : (c > 3 ? 3 : c);
            const float e4 = static_cast<float>(c) - v4;
            err4 = e4 * e4 + err4;
            i4[k] = c;
        }
    }
    const float thr = dxt1 ? err3 * Flt(kB_2_25) : Flt(kB_FltMax);
    const bool three = err4 > thr; // comiss xmm0,xmm6; jbe 0x00614AE9
    uint32_t idx = 0;
    for (int k = 15; k >= 0; k--) idx = (idx << 2) | (three ? static_cast<uint32_t>(i3[k]) : kMap4[i4[k]]);
    PackColor(out, c0, c1, three, idx);
}

// 0x00614150 thiscall(alpha[16] as int32, out), ret 4 (MMX): the DXT5 alpha block. With no 0 or 255 in the block: 8-alpha
// mode (a0 = max > a1 = min). Otherwise 6-alpha mode (a0 = min of the non-zero values, a1 = max of the values that are
// neither 0 nor 255; 192 / 63 when there are none) with codes 6 = 0 and 7 = 255 for those pixels. Codes: rank =
// ((a - min) * 32 * round(K * 4096 / range)) >> 16, then (x + 1) >> 1, clamped, K = 7 or 5.
void EncodeAlpha(const int32_t a[16], uint8_t* out) {
    auto sat16 = [](int32_t x) -> int32_t { return x < -32768 ? -32768 : (x > 32767 ? 32767 : x); }; // packssdw
    uint8_t v[16];
    bool ext[16];
    bool any = false;
    int mn = 255, mx = 0;
    for (int k = 0; k < 16; k++) {
        const int32_t w = sat16(a[k]);
        v[k] = static_cast<uint8_t>(w < 0 ? 0 : (w > 255 ? 255 : w)); // packuswb
        const bool z = v[k] == 0, f = v[k] == 255;
        ext[k] = z || f;
        any = any || ext[k];
        const int forMin = z ? 255 : v[k];    // por mm6,mm2
        const int forMax = ext[k] ? 0 : v[k]; // pandn mm2,mm4
        if (forMin < mn) mn = forMin;
        if (forMax > mx) mx = forMax;
    }
    if (mn > mx) { // pcmpgtw; pand [0x00FE8218] (192)
        mx += 192;
        mn -= 192;
    }
    uint32_t range = static_cast<uint32_t>(mx - mn) & 0xFFFF;
    if (range == 0) range = 1;
    const uint32_t factor = ((any ? 40960u : 57344u) + range) / (2u * range); // [0x00FE8210] / [0x00FE8214]; div esi
    out[0] = static_cast<uint8_t>(any ? mn : mx);
    out[1] = static_cast<uint8_t>(any ? mx : mn);
    const int16_t f16 = static_cast<int16_t>(static_cast<uint16_t>(factor));
    const int16_t maxIdx = any ? 5 : 7; // [0x00FE8200] / [0x00FE8208]
    const int16_t xr = any ? 0 : 7;     // [0x00FE81F0] / [0x00FE81F8]
    uint64_t bits = 0;
    for (int k = 0; k < 16; k++) {
        const int16_t w = static_cast<int16_t>(sat16(a[k]));
        int16_t d = static_cast<int16_t>(w - static_cast<int16_t>(mn));              // psubw
        d = static_cast<int16_t>(static_cast<uint16_t>(d) << 5);                     // psllw 5
        int16_t t = static_cast<int16_t>((static_cast<int32_t>(d) * f16) >> 16);     // pmulhw
        if (t < 0) t = 0;                                                            // pmaxsw 0
        t = static_cast<int16_t>((static_cast<uint32_t>(static_cast<uint16_t>(t)) + 1u) >> 1); // pavgw 0
        if (t > maxIdx) t = maxIdx;                                                  // pminsw
        t = static_cast<int16_t>(t ^ xr);
        int16_t tt = static_cast<int16_t>(t + (t > 0 ? 1 : 0) - (t == maxIdx ? maxIdx : 0));
        if (ext[k]) tt = static_cast<int16_t>(tt + 6); // [0x00FE81E8] bytes 6
        bits += static_cast<uint64_t>(static_cast<uint16_t>(tt)) << (3 * k); // pmaddwd {1, 8, 64, 512} + shifts: adds, not ors
    }
    for (int i = 0; i < 6; i++) out[2 + i] = static_cast<uint8_t>(bits >> (8 * i));
}

// ---- drivers ----
template <bool DXT5> void RefEncode(const Dst* dst, const Src* src) {
    if (DXT5 && src->format != 0x3D && src->format != 0x3E) return; // 0x006154F0
    const uint32_t width = dst->width, height = dst->height;
    const uint32_t fullW = width & ~3u, rem = width & 3u;
    const uint32_t bs = DXT5 ? 16 : 8;
    const uint32_t pad = dst->pitch - ((width + 3) & ~3u) * (bs / 4); // [esp+28h] / [esp+3Ch]
    const int32_t pitch = src->pitch;
    uint8_t* out = dst->ptr;
    const uint8_t* srow = src->ptr;
    Block b;
    int32_t a[16];
    for (uint32_t y = 0; y < height; y += 4) {
        const uint32_t rows = height - y;
        const bool partialRows = rows < 4;
        for (uint32_t x = 0; x < fullW; x += 4) {
            const uint8_t* s = srow + 4 * x;
            if (DXT5) {
                const uint32_t n = partialRows ? rows : 4;
                uint32_t k = 0;
                for (uint32_t r = 0; r < n; r++)
                    for (uint32_t c = 0; c < 4; c++) a[k++] = s[static_cast<intptr_t>(pitch) * r + 4 * c + 3];
                for (; k < 16; k++) a[k] = a[k - 4];
                EncodeAlpha(a, out);
            }
            if (partialRows) FetchPartial(b, s, pitch, 4, rows);
            else FetchFull(b, s, pitch);
            Endpoints(b);
            EncodeColor(b, out + (DXT5 ? 8 : 0), !DXT5);
            out += bs;
        }
        if (rem) {
            const uint8_t* s = srow + 4 * fullW;
            const uint32_t n = partialRows ? rows : 4;
            if (DXT5) {
                FetchAlphaPartial(a, s, pitch, rem, n, DriverLocals(s, y, height, dst->pitch - ((width + 3) & ~3u) * 4));
                EncodeAlpha(a, out);
            }
            FetchPartial(b, s, pitch, rem, n);
            Endpoints(b);
            EncodeColor(b, out + (DXT5 ? 8 : 0), !DXT5);
            if (DXT5 || !partialRows) out += bs; // the DXT1 driver's last-row path does not advance here (0x006153E3); nothing follows it
        }
        srow += static_cast<intptr_t>(pitch) * 4;
        out += pad;
    }
}

// ---- Fast: four blocks per SSE register ----

inline __m128 RcpSsLanes(__m128 v) { // rcpss per block, as the game does it
    alignas(16) float f[4];
    _mm_store_ps(f, v);
    for (float& x : f) x = _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x)));
    return _mm_load_ps(f);
}
inline __m128 RsqrtSsLanes(__m128 v) { // rsqrtss per block
    alignas(16) float f[4];
    _mm_store_ps(f, v);
    for (float& x : f) x = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(x)));
    return _mm_load_ps(f);
}
inline __m128 Blend(__m128 mask, __m128 a, __m128 b) { return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b)); } // mask ? a : b
// All ones where x is finite: its exponent bits are not all set (integer test, nothing for the optimiser to fold)
inline __m128 Finite(__m128 x) {
    const __m128i e = _mm_set1_epi32(0x7F800000);
    const __m128i notFinite = _mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(x), e), e);
    return _mm_castsi128_ps(_mm_xor_si128(notFinite, _mm_set1_epi32(-1)));
}
inline __m128i ClampI(__m128i x, int hi) {
    x = _mm_andnot_si128(_mm_srai_epi32(x, 31), x);          // < 0 -> 0
    const __m128i h = _mm_set1_epi32(hi);
    const __m128i gt = _mm_cmpgt_epi32(x, h);
    return _mm_or_si128(_mm_and_si128(gt, h), _mm_andnot_si128(gt, x));
}
// ClampField on four lanes (the game's byte trick: the low bits of x when 0 <= x <= max; all ones when max - x < 0)
inline __m128i QuantField(__m128 v, uint32_t kBits, uint32_t magicBits, int maxv) {
    const __m128 f = _mm_add_ps(_mm_mul_ps(v, Splat(kBits)), Splat(magicBits));
    const __m128i x = _mm_sub_epi32(_mm_castps_si128(f), _mm_set1_epi32(static_cast<int>(magicBits)));
    const __m128i m = _mm_set1_epi32(maxv);
    const __m128i lo = _mm_andnot_si128(_mm_srai_epi32(x, 31), x);
    const __m128i hi = _mm_srai_epi32(_mm_sub_epi32(m, x), 31);
    return _mm_and_si128(_mm_or_si128(lo, hi), m);
}

struct Job {
    const uint8_t* src;
    uint32_t cols, rows;
    uint8_t* out;
    uint32_t y, height, pad; // the DXT5 driver's locals that 0x00613F80 may read (see FetchAlphaPartial)
};

struct alignas(16) Group {
    __m128 R[16], G[16], B[16], L[16]; // pixel k of the four blocks
    alignas(16) uint32_t px[4][16];     // the blocks' pixels as the game fetches them
    alignas(16) uint8_t alpha[4][16];
    int32_t alphaInts[4][16]; // used instead of alpha when a value is outside 0..255 (the driver's locals, FetchAlphaPartial)
    bool alphaWide[4];
    Job job[4];
};

// The pixels the game fetches for this block (edge blocks: last column, then last row, repeated)
void LoadPixels(Group& g, int lane, const Job& j, int32_t pitch) {
    uint32_t* p = g.px[lane];
    if (j.cols == 4 && j.rows == 4) {
        for (int r = 0; r < 4; r++) std::memcpy(p + 4 * r, j.src + static_cast<intptr_t>(pitch) * r, 16);
        return;
    }
    for (uint32_t r = 0; r < 4; r++) {
        const uint8_t* row = j.src + static_cast<intptr_t>(pitch) * (r < j.rows ? r : j.rows - 1);
        for (uint32_t c = 0; c < 4; c++) p[4 * r + c] = Load32(row + 4 * (c < j.cols ? c : j.cols - 1));
    }
}

void LoadAlpha(Group& g, int lane, const Job& j, int32_t pitch) {
    uint8_t* a = g.alpha[lane];
    if (j.cols == 4) { // the drivers' own loop: rows, then the last row repeated
        for (int k = 0; k < 16; k++) a[k] = static_cast<uint8_t>(g.px[lane][k] >> 24);
        return;
    }
    int32_t* v = g.alphaInts[lane];
    FetchAlphaPartial(v, j.src, pitch, j.cols, j.rows, DriverLocals(j.src, j.y, j.height, j.pad));
    for (int k = 0; k < 16; k++) {
        if (v[k] < 0 || v[k] > 255) g.alphaWide[lane] = true;
        a[k] = static_cast<uint8_t>(v[k]);
    }
}

void EncodeAlphaFast(const uint8_t a[16], uint8_t* out);

// The block's alpha half: SSE2 on bytes, or the scalar translation when a value is not a byte (the game saturates the
// dwords to words and bytes, EncodeAlpha does the same)
void WriteAlpha(Group& g, int lane, const Job& j, int32_t pitch) {
    g.alphaWide[lane] = false;
    LoadAlpha(g, lane, j, pitch);
    if (g.alphaWide[lane]) EncodeAlpha(g.alphaInts[lane], j.out);
    else EncodeAlphaFast(g.alpha[lane], j.out);
}

// The alpha block (EncodeAlpha) with SSE2 on all 16 pixels at once
void EncodeAlphaFast(const uint8_t a[16], uint8_t* out) {
    const __m128i v = _mm_load_si128(reinterpret_cast<const __m128i*>(a));
    const __m128i z = _mm_setzero_si128();
    const __m128i is0 = _mm_cmpeq_epi8(v, z);
    const __m128i ext = _mm_or_si128(is0, _mm_cmpeq_epi8(v, _mm_set1_epi8(static_cast<char>(0xFF))));
    const bool any = _mm_movemask_epi8(ext) != 0;
    __m128i lo = _mm_or_si128(v, is0);     // minimum: zeros count as 255
    __m128i hi = _mm_andnot_si128(ext, v); // maximum: 0 and 255 count as 0
    lo = _mm_min_epu8(lo, _mm_srli_si128(lo, 8));
    hi = _mm_max_epu8(hi, _mm_srli_si128(hi, 8));
    lo = _mm_min_epu8(lo, _mm_srli_si128(lo, 4));
    hi = _mm_max_epu8(hi, _mm_srli_si128(hi, 4));
    lo = _mm_min_epu8(lo, _mm_srli_si128(lo, 2));
    hi = _mm_max_epu8(hi, _mm_srli_si128(hi, 2));
    lo = _mm_min_epu8(lo, _mm_srli_si128(lo, 1));
    hi = _mm_max_epu8(hi, _mm_srli_si128(hi, 1));
    int mn = _mm_cvtsi128_si32(lo) & 0xFF, mx = _mm_cvtsi128_si32(hi) & 0xFF;
    if (mn > mx) {
        mx += 192;
        mn -= 192;
    }
    uint32_t range = static_cast<uint32_t>(mx - mn) & 0xFFFF;
    if (range == 0) range = 1;
    const uint32_t factor = ((any ? 40960u : 57344u) + range) / (2u * range);
    out[0] = static_cast<uint8_t>(any ? mn : mx);
    out[1] = static_cast<uint8_t>(any ? mx : mn);
    const int maxIdx = any ? 5 : 7;
    const __m128i mnW = _mm_set1_epi16(static_cast<short>(mn)), fW = _mm_set1_epi16(static_cast<short>(static_cast<uint16_t>(factor)));
    const __m128i maxW = _mm_set1_epi16(static_cast<short>(maxIdx)), xrW = _mm_set1_epi16(static_cast<short>(any ? 0 : 7)), six = _mm_set1_epi16(6);
    auto codes = [&](__m128i w, __m128i e) {
        __m128i t = _mm_mulhi_epi16(_mm_slli_epi16(_mm_sub_epi16(w, mnW), 5), fW);
        t = _mm_avg_epu16(_mm_max_epi16(t, z), z);
        t = _mm_xor_si128(_mm_min_epi16(t, maxW), xrW);
        const __m128i tt = _mm_sub_epi16(_mm_sub_epi16(t, _mm_cmpgt_epi16(t, z)), _mm_and_si128(_mm_cmpeq_epi16(t, maxW), maxW));
        return _mm_add_epi16(tt, _mm_and_si128(e, six));
    };
    const __m128i t0 = codes(_mm_unpacklo_epi8(v, z), _mm_unpacklo_epi8(ext, ext));
    const __m128i t1 = codes(_mm_unpackhi_epi8(v, z), _mm_unpackhi_epi8(ext, ext));
    const __m128i wts = _mm_setr_epi16(1, 8, 64, 512, 1, 8, 64, 512);
    alignas(16) uint32_t d0[4], d1[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(d0), _mm_madd_epi16(t0, wts));
    _mm_store_si128(reinterpret_cast<__m128i*>(d1), _mm_madd_epi16(t1, wts));
    const uint64_t bits = static_cast<uint64_t>((d0[0] + d0[1]) + ((d0[2] + d0[3]) << 12)) | (static_cast<uint64_t>((d1[0] + d1[1]) + ((d1[2] + d1[3]) << 12)) << 24);
    for (int i = 0; i < 6; i++) out[2 + i] = static_cast<uint8_t>(bits >> (8 * i));
}

// The colour blocks of the four lanes. out[lane] gets 8 bytes; ok[lane] = false: a value was not finite (use the fallback).
void EncodeColorGroup(Group& g, bool dxt1, uint8_t out[4][8], bool ok[4], FastCounters& cnt) {
    const __m128 zero = _mm_setzero_ps();
    // ---- 0x0061510E: sums, squares, products with the luma ----
    __m128 sR = zero, sG = zero, sB = zero, sL = zero, qR = zero, qG = zero, qB = zero, qL = zero, pR = zero, pG = zero, pB = zero, pL = zero;
    for (int k = 0; k < 16; k++) {
        const __m128 r = g.R[k], gg = g.G[k], b = g.B[k], l = g.L[k];
        sR = _mm_add_ps(sR, r);
        sG = _mm_add_ps(sG, gg);
        sB = _mm_add_ps(sB, b);
        sL = _mm_add_ps(sL, l);
        qR = _mm_add_ps(qR, _mm_mul_ps(r, r));
        qG = _mm_add_ps(qG, _mm_mul_ps(gg, gg));
        qB = _mm_add_ps(qB, _mm_mul_ps(b, b));
        qL = _mm_add_ps(qL, _mm_mul_ps(l, l));
        pR = _mm_add_ps(pR, _mm_mul_ps(l, r));
        pG = _mm_add_ps(pG, _mm_mul_ps(l, gg));
        pB = _mm_add_ps(pB, _mm_mul_ps(l, b));
        pL = _mm_add_ps(pL, _mm_mul_ps(l, l));
    }
    const __m128 c16 = Splat(kB_1_16), eps = Splat(kB_Eps);
    const __m128 mR = _mm_mul_ps(sR, c16), mG = _mm_mul_ps(sG, c16), mB = _mm_mul_ps(sB, c16), mL = _mm_mul_ps(sL, c16);
    const __m128 vR = _mm_sub_ps(_mm_mul_ps(c16, qR), _mm_mul_ps(mR, mR));
    const __m128 vG = _mm_sub_ps(_mm_mul_ps(c16, qG), _mm_mul_ps(mG, mG));
    const __m128 vB = _mm_sub_ps(_mm_mul_ps(c16, qB), _mm_mul_ps(mB, mB));
    const __m128 vL = _mm_sub_ps(_mm_mul_ps(c16, qL), _mm_mul_ps(mL, mL));
    const __m128 tot = _mm_add_ps(_mm_add_ps(vL, vG), _mm_add_ps(vB, vR));
    const __m128 notSolid = _mm_cmpge_ps(tot, eps);
    const __m128 lumaM = _mm_and_ps(notSolid, _mm_cmpgt_ps(vL, eps));
    const int solidBits = ~_mm_movemask_ps(notSolid) & 15, lumaBits = _mm_movemask_ps(lumaM);
    // ---- axis: luma covariance (0x006151E0) ----
    const __m128 cR = _mm_sub_ps(_mm_mul_ps(pR, c16), _mm_mul_ps(mL, mR));
    const __m128 cG = _mm_sub_ps(_mm_mul_ps(pG, c16), _mm_mul_ps(mL, mG));
    const __m128 cB = _mm_sub_ps(_mm_mul_ps(pB, c16), _mm_mul_ps(mL, mB));
    const __m128 n2 = _mm_add_ps(_mm_add_ps(_mm_mul_ps(cB, cB), _mm_mul_ps(cR, cR)), _mm_mul_ps(cG, cG));
    const __m128 rs = RsqrtSsLanes(n2);
    alignas(16) float aR[4], aG[4], aB[4], a3[4];
    _mm_store_ps(aR, _mm_mul_ps(cR, rs));
    _mm_store_ps(aG, _mm_mul_ps(cG, rs));
    _mm_store_ps(aB, _mm_mul_ps(cB, rs));
    _mm_store_ps(a3, _mm_mul_ps(zero, rs));
    // ... solid blocks (the axis is not used), power iteration (0x00614DD0) for the flat-luma blocks
    for (int j = 0; j < 4; j++) {
        if (solidBits & (1 << j)) {
            aR[j] = aG[j] = aB[j] = Flt(kB_One);
            a3[j] = 0.0f;
        } else if (!(lumaBits & (1 << j))) {
            alignas(16) __m128 px[16];
            for (int k = 0; k < 16; k++) px[k] = _mm_setr_ps(Lane(g.R[k], j), Lane(g.G[k], j), Lane(g.B[k], j), Lane(g.L[k], j));
            const __m128 sum = _mm_setr_ps(Lane(sR, j), Lane(sG, j), Lane(sB, j), Lane(sL, j));
            const __m128 var = _mm_setr_ps(Lane(vR, j), Lane(vG, j), Lane(vB, j), Lane(vL, j));
            alignas(16) float ax[4];
            _mm_store_ps(ax, PowerAxis(px, sum, var));
            aR[j] = ax[0];
            aG[j] = ax[1];
            aB[j] = ax[2];
            a3[j] = ax[3];
            cnt.powerAxis++;
        }
    }
    const __m128 axR = _mm_load_ps(aR), axG = _mm_load_ps(aG), axB = _mm_load_ps(aB), ax3 = _mm_load_ps(a3);
    // ---- 0x0061521E: endpoints ----
    __m128 wN = zero, wX = zero, nR = zero, nG = zero, nB = zero, nL = zero, xR = zero, xG = zero, xB = zero, xL = zero;
    for (int k = 0; k < 16; k++) {
        const __m128 r = g.R[k], gg = g.G[k], b = g.B[k], l = g.L[k];
        const __m128 dR = _mm_mul_ps(_mm_sub_ps(r, mR), axR), dG = _mm_mul_ps(_mm_sub_ps(gg, mG), axG), dB = _mm_mul_ps(_mm_sub_ps(b, mB), axB);
        const __m128 t = _mm_add_ps(_mm_add_ps(dB, dR), dG);
        const __m128 mn = _mm_min_ps(zero, t), mx = _mm_max_ps(zero, t);
        wN = _mm_add_ps(wN, mn);
        wX = _mm_add_ps(wX, mx);
        nR = _mm_sub_ps(nR, _mm_mul_ps(mn, r));
        nG = _mm_sub_ps(nG, _mm_mul_ps(mn, gg));
        nB = _mm_sub_ps(nB, _mm_mul_ps(mn, b));
        nL = _mm_sub_ps(nL, _mm_mul_ps(mn, l));
        xR = _mm_add_ps(xR, _mm_mul_ps(mx, r));
        xG = _mm_add_ps(xG, _mm_mul_ps(mx, gg));
        xB = _mm_add_ps(xB, _mm_mul_ps(mx, b));
        xL = _mm_add_ps(xL, _mm_mul_ps(mx, l));
    }
    const __m128 rX = RcpSsLanes(wX), rN = RcpSsLanes(_mm_xor_ps(wN, _mm_set1_ps(-0.0f)));
    __m128 e0R = _mm_mul_ps(rX, nR), e0G = _mm_mul_ps(rX, nG), e0B = _mm_mul_ps(rX, nB), e0L = _mm_mul_ps(rX, nL);
    __m128 e1R = _mm_mul_ps(rN, xR), e1G = _mm_mul_ps(rN, xG), e1B = _mm_mul_ps(rN, xB), e1L = _mm_mul_ps(rN, xL);
    const __m128 fR = _mm_sub_ps(e1R, e0R), fG = _mm_sub_ps(e1G, e0G), fB = _mm_sub_ps(e1B, e0B), fL = _mm_sub_ps(e1L, e0L);
    const __m128 d2 = _mm_add_ps(_mm_add_ps(_mm_mul_ps(fB, fB), _mm_mul_ps(fR, fR)), _mm_add_ps(_mm_mul_ps(fL, fL), _mm_mul_ps(fG, fG)));
    const __m128 pushApart = _mm_cmpnge_ps(d2, Splat(kB_ExtThr)); // !(d2 >= thr), NaN included
    if (_mm_movemask_ps(pushApart)) {
        const __m128 sc = _mm_mul_ps(RsqrtSsLanes(d2), Splat(kB_1_31));
        const __m128 gR = _mm_mul_ps(fR, sc), gG = _mm_mul_ps(fG, sc), gB = _mm_mul_ps(fB, sc), gL = _mm_mul_ps(fL, sc);
        e1R = Blend(pushApart, _mm_add_ps(e1R, gR), e1R);
        e1G = Blend(pushApart, _mm_add_ps(e1G, gG), e1G);
        e1B = Blend(pushApart, _mm_add_ps(e1B, gB), e1B);
        e1L = Blend(pushApart, _mm_add_ps(e1L, gL), e1L);
        e0R = Blend(pushApart, _mm_sub_ps(e0R, gR), e0R);
        e0G = Blend(pushApart, _mm_sub_ps(e0G, gG), e0G);
        e0B = Blend(pushApart, _mm_sub_ps(e0B, gB), e0B);
        e0L = Blend(pushApart, _mm_sub_ps(e0L, gL), e0L);
    }
    // non-finite values in a non-solid block: the fallback encodes it
    __m128 fin = _mm_and_ps(_mm_and_ps(Finite(axR), Finite(axG)), _mm_and_ps(Finite(axB), Finite(ax3)));
    fin = _mm_and_ps(fin, _mm_and_ps(_mm_and_ps(Finite(e0R), Finite(e0G)), _mm_and_ps(Finite(e0B), Finite(e0L))));
    fin = _mm_and_ps(fin, _mm_and_ps(_mm_and_ps(Finite(e1R), Finite(e1G)), _mm_and_ps(Finite(e1B), Finite(e1L))));
    fin = _mm_or_ps(fin, _mm_xor_ps(notSolid, _mm_castsi128_ps(_mm_set1_epi32(-1))));
    // solid blocks: both endpoints are the mean
    e0R = Blend(notSolid, e0R, mR);
    e0G = Blend(notSolid, e0G, mG);
    e0B = Blend(notSolid, e0B, mB);
    e1R = Blend(notSolid, e1R, mR);
    e1G = Blend(notSolid, e1G, mG);
    e1B = Blend(notSolid, e1B, mB);
    // ---- 0x006143F0: 565 colours ----
    const __m128i c0 = _mm_or_si128(_mm_or_si128(_mm_slli_epi32(QuantField(e0R, kB_248_255, kB_Magic5, 31), 11), _mm_slli_epi32(QuantField(e0G, kB_252_255, kB_Magic6, 63), 5)),
                                    QuantField(e0B, kB_248_255, kB_Magic5, 31));
    const __m128i c1 = _mm_or_si128(_mm_or_si128(_mm_slli_epi32(QuantField(e1R, kB_248_255, kB_Magic5, 31), 11), _mm_slli_epi32(QuantField(e1G, kB_252_255, kB_Magic6, 63), 5)),
                                    QuantField(e1B, kB_248_255, kB_Magic5, 31));
    const int eqBits = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(c0, c1)));
    const int finBits = _mm_movemask_ps(fin);
    const __m128 k31 = Splat(kB_1_31), k63 = Splat(kB_1_63);
    const __m128i m5 = _mm_set1_epi32(31), m6 = _mm_set1_epi32(63);
    auto project = [&](__m128i c) {
        const __m128 r = _mm_mul_ps(_mm_cvtepi32_ps(_mm_srli_epi32(c, 11)), k31);
        const __m128 gg = _mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(c, 5), m6)), k63);
        const __m128 b = _mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(c, m5)), k31);
        return _mm_add_ps(_mm_add_ps(_mm_mul_ps(zero, ax3), _mm_mul_ps(gg, axG)), _mm_add_ps(_mm_mul_ps(b, axB), _mm_mul_ps(r, axR)));
    };
    const __m128 p0 = project(c0), p1 = project(c1);
    const __m128 gt = _mm_cmpgt_ps(p0, p1);
    const __m128 lo = Blend(gt, p1, p0);
    __m128 hi = Blend(gt, p0, p1);
    {
        alignas(16) float h[4], l[4];
        _mm_store_ps(h, hi);
        _mm_store_ps(l, lo);
        for (int j = 0; j < 4; j++) // only where the game gets this far (colours differ) and the values are finite
            if (!(eqBits & (1 << j)) && (finBits & (1 << j)) && X87RangeBelowEps(h[j], l[j])) h[j] = h[j] + Flt(kB_One);
        hi = _mm_load_ps(h);
    }
    const __m128 one = Splat(kB_One);
    const __m128 range = _mm_sub_ps(hi, lo);
    const __m128 inv = _mm_div_ps(one, range);
    const __m128 inv3 = _mm_mul_ps(inv, Splat(kB_Three));
    const __m128 t0 = _mm_mul_ps(_mm_sub_ps(Splat(kB_Eighth), range), Splat(kB_Sixteen));
    const __m128 s = _mm_sub_ps(one, t0);
    const __m128 inv2 = _mm_mul_ps(inv, Splat(kB_Two));
    const __m128 sm = _mm_castsi128_ps(_mm_srai_epi32(_mm_castps_si128(s), 31));
    const __m128 d1 = Blend(sm, one, t0);
    const __m128 d = _mm_andnot_ps(_mm_castsi128_ps(_mm_srai_epi32(_mm_castps_si128(d1), 31)), d1);
    // ---- index loop ----
    const __m128 magic = Splat(kB_MagicI);
    const __m128i magicBits = _mm_set1_epi32(static_cast<int>(kB_MagicI));
    const __m128i three = _mm_set1_epi32(3);
    __m128 err3 = zero, err4 = zero;
    __m128i pk3 = _mm_setzero_si128(), pk4 = _mm_setzero_si128();
    for (int k = 0; k < 16; k++) {
        const __m128 p = _mm_add_ps(_mm_add_ps(_mm_mul_ps(ax3, g.L[k]), _mm_mul_ps(axG, g.G[k])), _mm_add_ps(_mm_mul_ps(axB, g.B[k]), _mm_mul_ps(axR, g.R[k])));
        const __m128 u = _mm_sub_ps(p, lo);
        const __m128 dv = _mm_mul_ps(Splat(kB_Dither[k]), d);
        const __m128 v4 = _mm_add_ps(_mm_mul_ps(u, inv3), dv);
        const __m128i i4 = ClampI(_mm_sub_epi32(_mm_castps_si128(_mm_add_ps(v4, magic)), magicBits), 3);
        const __m128 e4 = _mm_sub_ps(_mm_cvtepi32_ps(i4), v4);
        err4 = _mm_add_ps(_mm_mul_ps(e4, e4), err4);
        // rank -> code 0, 2, 3, 1: +1 when > 0, -3 when 3
        const __m128i m4 = _mm_sub_epi32(_mm_sub_epi32(i4, _mm_cmpgt_epi32(i4, _mm_setzero_si128())), _mm_and_si128(_mm_cmpeq_epi32(i4, three), three));
        const __m128i sh = _mm_cvtsi32_si128(2 * k);
        pk4 = _mm_or_si128(pk4, _mm_sll_epi32(m4, sh));
        if (dxt1) {
            const __m128 v3 = _mm_add_ps(_mm_mul_ps(u, inv2), dv);
            const __m128i i3 = ClampI(_mm_sub_epi32(_mm_castps_si128(_mm_add_ps(v3, magic)), magicBits), 2);
            const __m128 e3 = _mm_sub_ps(_mm_cvtepi32_ps(i3), v3);
            err3 = _mm_add_ps(_mm_mul_ps(e3, e3), err3);
            pk3 = _mm_or_si128(pk3, _mm_sll_epi32(i3, sh));
        }
    }
    const __m128 thr = dxt1 ? _mm_mul_ps(err3, Splat(kB_2_25)) : Splat(kB_FltMax);
    const int threeBits = _mm_movemask_ps(_mm_cmpgt_ps(err4, thr));
    const int errFin = _mm_movemask_ps(_mm_and_ps(_mm_and_ps(Finite(err4), Finite(err3)), Finite(inv)));
    alignas(16) uint32_t C0[4], C1[4], P3[4], P4[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(C0), c0);
    _mm_store_si128(reinterpret_cast<__m128i*>(C1), c1);
    _mm_store_si128(reinterpret_cast<__m128i*>(P3), pk3);
    _mm_store_si128(reinterpret_cast<__m128i*>(P4), pk4);
    for (int j = 0; j < 4; j++) {
        const uint32_t bit = 1u << j;
        if (solidBits & bit) cnt.solid++;
        if (eqBits & bit) { // equal colours (always for solid blocks)
            ok[j] = (finBits & bit) != 0;
            Put16(out[j], C0[j]);
            Put16(out[j] + 2, C0[j]);
            Put32(out[j] + 4, 0);
            continue;
        }
        ok[j] = (finBits & bit) && (errFin & bit);
        if (!ok[j]) continue;
        const bool three3 = (threeBits & bit) != 0;
        PackColor(out[j], C0[j], C1[j], three3, three3 ? P3[j] : P4[j]);
    }
}

template <bool DXT5> void FlushGroup(Group& g, int n, int32_t pitch, uint32_t format, BlockFallback fb, void* ctx, FastCounters& cnt) {
    for (int j = 0; j < n; j++) LoadPixels(g, j, g.job[j], pitch);
    for (int j = n; j < 4; j++) std::memcpy(g.px[j], g.px[n - 1], sizeof g.px[j]); // unused lanes: a copy of the last block
    // to floats, one block per lane (R/256 etc. are exact; the luma as the game: 0.59G + (0.11B + 0.3R))
    const __m128 s = Splat(kB_1_256), w0 = Splat(kB_L0), w1 = Splat(kB_L1), w2 = Splat(kB_L2);
    const __m128i ff = _mm_set1_epi32(0xFF);
    for (int r = 0; r < 4; r++) {
        __m128 a0 = _mm_load_ps(reinterpret_cast<const float*>(g.px[0] + 4 * r)), a1 = _mm_load_ps(reinterpret_cast<const float*>(g.px[1] + 4 * r));
        __m128 a2 = _mm_load_ps(reinterpret_cast<const float*>(g.px[2] + 4 * r)), a3 = _mm_load_ps(reinterpret_cast<const float*>(g.px[3] + 4 * r));
        _MM_TRANSPOSE4_PS(a0, a1, a2, a3); // a_c = pixel (r, c) of the four blocks (bit patterns only moved)
        const __m128 col[4] = {a0, a1, a2, a3};
        for (int c = 0; c < 4; c++) {
            const __m128i v = _mm_castps_si128(col[c]);
            const int k = 4 * r + c;
            const __m128 R = _mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(v, 16), ff)), s);
            const __m128 G = _mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(v, 8), ff)), s);
            const __m128 B = _mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(v, ff)), s);
            g.R[k] = R;
            g.G[k] = G;
            g.B[k] = B;
            g.L[k] = _mm_add_ps(_mm_mul_ps(G, w1), _mm_add_ps(_mm_mul_ps(B, w2), _mm_mul_ps(R, w0)));
        }
    }
    uint8_t color[4][8];
    bool ok[4] = {true, true, true, true};
    EncodeColorGroup(g, !DXT5, color, ok, cnt);
    for (int j = 0; j < n; j++) {
        const Job& job = g.job[j];
        if (!ok[j]) {
            fb(ctx, DXT5, job.src, pitch, job.cols, job.rows, format, job.out);
            cnt.delegated++;
            if (DXT5) { // the one-block image of the fallback has other driver locals: the alpha half is recomputed here
                WriteAlpha(g, j, job, pitch);
            }
            continue;
        }
        if (DXT5) {
            WriteAlpha(g, j, job, pitch);
            std::memcpy(job.out + 8, color[j], 8);
        } else {
            std::memcpy(job.out, color[j], 8);
        }
    }
    cnt.blocks += static_cast<uint32_t>(n);
}

inline uint32_t BlockRows(uint32_t height) { return (height >> 2) + ((height & 3) ? 1u : 0u); }

// Rows of blocks [rowBegin, rowEnd) of the image. dst / src are always the WHOLE image's descriptors and the rows are
// absolute, so every block's Job (source pointer, cols, rows, output pointer, and the DXT5 driver locals y, height and
// padding that the edge-alpha quirk reads) is the one the full serial loop builds for it. Groups of four never span the
// range's ends; the lanes are independent, so the grouping does not change any block's bytes.
template <bool DXT5> void FastEncodeRows(const Dst* dst, const Src* src, uint32_t rowBegin, uint32_t rowEnd, BlockFallback fb, void* ctx, FastCounters& cnt) {
    const uint32_t width = dst->width, height = dst->height;
    const uint32_t full = width >> 2, rem = width & 3, blocksX = full + (rem ? 1 : 0);
    const uint32_t bs = DXT5 ? 16 : 8;
    const int32_t pitch = src->pitch;
    const uint32_t pad = dst->pitch - ((width + 3) & ~3u) * 4; // [esp+3Ch] of the DXT5 driver
    Group g;
    int n = 0;
    const uint8_t* previousPixels=nullptr;
    const uint8_t* previousOutput=nullptr;
    uint32_t previousSolid=0,previousPower=0;
    // the same addresses the serial walk reaches by adding pitch * 4 (source) and dst->pitch (output) once per row of
    // blocks (32-bit wrap-around arithmetic either way)
    const uint8_t* srow = src->ptr + static_cast<intptr_t>(pitch) * 4 * static_cast<intptr_t>(rowBegin);
    uint8_t* orow = dst->ptr + static_cast<uintptr_t>(dst->pitch) * rowBegin; // the game advances by blocksX * bs + padding per row of blocks
    for (uint32_t by = rowBegin; by < rowEnd; by++) {
        const uint32_t y = 4 * by;
        const uint32_t rows = height - y < 4 ? height - y : 4;
        for (uint32_t bx = 0; bx < blocksX; bx++) {
            Job& j = g.job[n++];
            j.src = srow + 16 * static_cast<uintptr_t>(bx);
            j.cols = bx < full ? 4 : rem;
            j.rows = rows;
            j.out = orow + static_cast<uintptr_t>(bx) * bs;
            j.y = y;
            j.height = height;
            j.pad = pad;
            if (n == 4) {
                // Repeated complete 16x4 strips have identical encoder inputs, including alpha.
                // Partial blocks retain the native edge/driver-local path.
                const bool complete=rows==4&&g.job[0].cols==4&&g.job[3].cols==4&&g.job[0].y==y;
                bool repeated=complete&&previousPixels&&Load32(g.job[0].src)==Load32(previousPixels);
                for(int r=0;repeated&&r<4;++r)repeated=std::memcmp(g.job[0].src+static_cast<intptr_t>(pitch)*r,previousPixels+static_cast<intptr_t>(pitch)*r,64)==0;
                bool reusable=complete;
                if(repeated){std::memmove(g.job[0].out,previousOutput,4*bs);cnt.blocks+=4;cnt.solid+=previousSolid;cnt.powerAxis+=previousPower;}
                else{
                    const auto solid=cnt.solid,power=cnt.powerAxis,delegated=cnt.delegated;
                    FlushGroup<DXT5>(g, 4, pitch, src->format, fb, ctx, cnt);
                    previousSolid=cnt.solid-solid;previousPower=cnt.powerAxis-power;
                    reusable=reusable&&cnt.delegated==delegated;
                }
                previousPixels=reusable?g.job[0].src:nullptr;
                previousOutput=reusable?g.job[0].out:nullptr;
                n = 0;
            }
        }
        srow += static_cast<intptr_t>(pitch) * 4;
        orow += dst->pitch;
    }
    if (n) FlushGroup<DXT5>(g, n, pitch, src->format, fb, ctx, cnt);
}

template <bool DXT5> void FastEncode(const Dst* dst, const Src* src, BlockFallback fb, void* ctx, FastCounters* counters) {
    if (DXT5 && src->format != 0x3D && src->format != 0x3E) return;
    FastCounters local;
    FastCounters& cnt = counters ? *counters : local;
    FastEncodeRows<DXT5>(dst, src, 0, BlockRows(dst->height), fb, ctx, cnt);
}

// ---- Parallel: the worker pool ----

uint64_t Qpc() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<uint64_t>(q.QuadPart);
}
double QpcMs() {
    static const double ms = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 / static_cast<double>(f.QuadPart);
    }();
    return ms;
}

// The floating-point state a block's bytes depend on: the x87 control word (precision and rounding of the range test)
// and MXCSR (rounding, FTZ, DAZ of every SSE operation)
struct FpState {
    uint16_t x87;
    uint32_t mxcsr;
};
FpState ReadFp() {
    uint16_t cw;
    __asm fnstcw cw
    return FpState{cw, _mm_getcsr()};
}
void WriteFp(const FpState& s) {
    uint16_t cw = s.x87;
    __asm {
        fnclex
        fldcw cw
    }
    _mm_setcsr(s.mxcsr & ~0x3Fu); // control bits only; the sticky exception flags start clear
}
bool SameFp(const FpState& a, const FpState& b) { return (a.x87 & 0x1F3F) == (b.x87 & 0x1F3F) && (a.mxcsr & 0xFFC0) == (b.mxcsr & 0xFFC0); }

constexpr uint32_t kClosed = 0x80000000u;    // Pool::state: the task takes no more threads
constexpr uint32_t kCountMask = 0x7FFFFFFFu; // Pool::state: threads checked in
constexpr uint32_t kChunksPerThread = 4;     // load balance: faster threads take more chunks
constexpr SIZE_T kWorkerStack = 256 * 1024;  // reserved address space per worker (the game is 32-bit)

struct Task {
    bool dxt5 = false;
    const Dst* dst = nullptr;
    const Src* src = nullptr;
    BlockFallback fb = nullptr;
    void* ctx = nullptr;
    uint32_t blockRows = 0, chunkRows = 0, chunks = 0;
    FpState fp{};
    std::atomic<uint32_t> next{0};
    std::atomic<uint32_t> byWorkers{0}, helpers{0}, fpMismatch{0};
    std::atomic<uint32_t> blocks{0}, delegated{0}, power{0}, solid{0};
    std::atomic<uint64_t> workTicks{0};
};

// Takes chunks until none is left
void RunChunks(Task& t, bool worker) {
    FastCounters c;
    uint64_t ticks = 0;
    uint32_t mine = 0;
    for (;;) {
        const uint32_t i = t.next.fetch_add(1, std::memory_order_relaxed);
        if (i >= t.chunks) break;
        const uint32_t r0 = i * t.chunkRows, r1 = t.blockRows - r0 < t.chunkRows ? t.blockRows : r0 + t.chunkRows;
        const uint64_t q0 = Qpc();
        if (t.dxt5) FastEncodeRows<true>(t.dst, t.src, r0, r1, t.fb, t.ctx, c);
        else FastEncodeRows<false>(t.dst, t.src, r0, r1, t.fb, t.ctx, c);
        ticks += Qpc() - q0;
        mine++;
    }
    t.blocks.fetch_add(c.blocks, std::memory_order_relaxed);
    t.delegated.fetch_add(c.delegated, std::memory_order_relaxed);
    t.power.fetch_add(c.powerAxis, std::memory_order_relaxed);
    t.solid.fetch_add(c.solid, std::memory_order_relaxed);
    t.workTicks.fetch_add(ticks, std::memory_order_relaxed);
    if (worker && mine) {
        t.byWorkers.fetch_add(mine, std::memory_order_relaxed);
        t.helpers.fetch_add(1, std::memory_order_relaxed);
    }
}

// Created once, never destroyed (the threads sleep until the process ends; nothing to tear down at exit).
// Hand-off protocol (state = kClosed flag | number of threads checked in):
//   caller: owns the pool (inUse), writes the task while closed, opens it (release), wakes k workers, takes chunks,
//           closes it, then waits until no thread is checked in. Only then may the task be rewritten.
//   worker: wakes, checks in (fetch_add). Closed: it touches nothing and leaves (a late wake from an earlier task).
//           Open: it sets the caller's FP state, takes chunks, restores its own, leaves. The thread whose leaving makes the
//           count 0 while closed sets `done`; the caller re-checks the count around every wait, so a stale `done` is
//           harmless and none is lost.
struct Pool {
    std::atomic<bool> inUse{false};
    std::atomic<uint32_t> state{kClosed};
    std::atomic<uint32_t> created{0};
    std::mutex createLock;
    bool createFailed = false; // guarded by createLock
    HANDLE done = nullptr;
    HANDLE wake[Parallel::kMaxWorkers] = {};
    struct Arg {
        Pool* pool;
        uint32_t index;
    } args[Parallel::kMaxWorkers] = {};
    Task task;

    Pool() { done = CreateEventW(nullptr, FALSE, FALSE, nullptr); }

    static DWORD WINAPI WorkerMain(void* p) {
        const Arg* a = static_cast<const Arg*>(p);
        Pool& pool = *a->pool;
        const HANDLE wakeMe = pool.wake[a->index];
        for (;;) {
            if (WaitForSingleObject(wakeMe, INFINITE) != WAIT_OBJECT_0) {
                Sleep(10);
                continue;
            }
            const uint32_t s = pool.state.fetch_add(1, std::memory_order_acq_rel);
            if (!(s & kClosed)) {
                Task& t = pool.task;
                const FpState own = ReadFp();
                WriteFp(t.fp);
                if (!SameFp(ReadFp(), t.fp)) t.fpMismatch.fetch_add(1, std::memory_order_relaxed);
                RunChunks(t, true);
                WriteFp(own);
            }
            if (pool.state.fetch_sub(1, std::memory_order_acq_rel) == (kClosed | 1u)) SetEvent(pool.done);
        }
    }

    // At least n threads when possible; returns how many can be used (<= n)
    uint32_t EnsureWorkers(uint32_t n) {
        if (n > Parallel::kMaxWorkers) n = Parallel::kMaxWorkers;
        uint32_t have = created.load(std::memory_order_acquire);
        if (have >= n) return n;
        std::lock_guard<std::mutex> lock(createLock);
        have = created.load(std::memory_order_relaxed);
        if (!done) createFailed = true;
        using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        static const auto setDescription = reinterpret_cast<SetDescription>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
        while (have < n && !createFailed) {
            wake[have] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!wake[have]) {
                createFailed = true;
                break;
            }
            args[have] = Arg{this, have};
            // Normal priority on purpose: the calling thread (render / loader thread) is blocked on this image, so a
            // lower priority would only let other game threads delay it. The work is bounded (one image) and at least two
            // logical processors are left free by DefaultWorkers().
            const HANDLE h = CreateThread(nullptr, kWorkerStack, &Pool::WorkerMain, &args[have], STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
            if (!h) {
                CloseHandle(wake[have]);
                wake[have] = nullptr;
                createFailed = true;
                break;
            }
            if (setDescription) setDescription(h, L"Apex DXT worker");
            CloseHandle(h); // never joined: the threads live as long as the process
            have++;
            created.store(have, std::memory_order_release);
        }
        return have < n ? have : n;
    }
};

Pool& ThePool() {
    static Pool* const pool = new Pool(); // intentionally leaked (see Pool)
    return *pool;
}

} // namespace

bool CpuHasSse2() {
    int r[4] = {};
    __cpuid(r, 1);
    return (r[3] & (1 << 26)) != 0;
}

const char* CpuFeatureText() {
    static const char* text = [] {
        static char buf[64] = {};
        int r[4] = {};
        __cpuid(r, 0);
        const int maxLeaf = r[0];
        __cpuid(r, 1);
        const bool sse2 = (r[3] & (1 << 26)) != 0, sse41 = (r[2] & (1 << 19)) != 0, osxsave = (r[2] & (1 << 27)) != 0, avx = (r[2] & (1 << 28)) != 0;
        bool avxOs = false;
        if (osxsave && avx) avxOs = (_xgetbv(0) & 6) == 6;
        bool avx2 = false;
        if (maxLeaf >= 7 && avxOs) {
            __cpuidex(r, 7, 0);
            avx2 = (r[1] & (1 << 5)) != 0;
        }
        size_t n = 0;
        auto add = [&](const char* s) {
            while (*s && n + 1 < sizeof buf) buf[n++] = *s++;
            buf[n] = 0;
        };
        add(sse2 ? "SSE2" : "no SSE2");
        if (sse41) add(", SSE4.1");
        if (avxOs) add(", AVX");
        if (avx2) add(", AVX2");
        return static_cast<const char*>(buf);
    }();
    return text;
}

namespace Ref {
void EncodeDxt1(const Dst* dst, const Src* src) { RefEncode<false>(dst, src); }
void EncodeDxt5(const Dst* dst, const Src* src) { RefEncode<true>(dst, src); }
void EncodeBlock(void*, bool dxt5, const uint8_t* src, int32_t pitch, uint32_t cols, uint32_t rows, uint32_t format, uint8_t* out) {
    Dst d{out, cols, rows, dxt5 ? 16u : 8u, 0};
    Src s{src, 0, 0, pitch, format};
    if (dxt5) RefEncode<true>(&d, &s);
    else RefEncode<false>(&d, &s);
}
} // namespace Ref

namespace Fast {
void EncodeDxt1(const Dst* dst, const Src* src, BlockFallback fallback, void* ctx, FastCounters* counters) { FastEncode<false>(dst, src, fallback, ctx, counters); }
void EncodeDxt5(const Dst* dst, const Src* src, BlockFallback fallback, void* ctx, FastCounters* counters) { FastEncode<true>(dst, src, fallback, ctx, counters); }
} // namespace Fast

namespace Parallel {

uint32_t DefaultWorkers() {
    static const uint32_t n = [] {
        uint32_t cpus = 0;
        DWORD_PTR proc = 0, sys = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && proc) {
            for (DWORD_PTR m = proc; m; m &= m - 1) cpus++;
        } else {
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            cpus = si.dwNumberOfProcessors;
        }
        if (cpus <= 2) return 0u;
        return cpus - 2 < kMaxWorkers ? cpus - 2 : kMaxWorkers;
    }();
    return n;
}

void Prepare(uint32_t workers) {
    if (workers) ThePool().EnsureWorkers(workers);
}

uint32_t WorkersCreated() { return ThePool().created.load(std::memory_order_acquire); }

Result Encode(bool dxt5, const Dst* dst, const Src* src, BlockFallback fallback, void* ctx, FastCounters* counters, const Options& options) {
    Result r;
    const uint64_t t0 = Qpc();
    FastCounters local;
    FastCounters& cnt = counters ? *counters : local;
    auto serial = [&] {
        if (dxt5) FastEncode<true>(dst, src, fallback, ctx, &cnt);
        else FastEncode<false>(dst, src, fallback, ctx, &cnt);
        r.wallMs = static_cast<double>(Qpc() - t0) * QpcMs();
        r.workMs = r.wallMs;
        return r;
    };
    if (dxt5 && src->format != 0x3D && src->format != 0x3E) return r; // the game encodes nothing
    const uint32_t width = dst->width, height = dst->height;
    const uint32_t workers = options.workers < kMaxWorkers ? options.workers : kMaxWorkers;
    if (!workers || !width || static_cast<uint64_t>(width) * height < options.minPixels) return serial();
    // chunks of whole rows of blocks: about kChunksPerThread per thread, at least minBlocksPerChunk blocks each
    const uint32_t blocksX = (width >> 2) + ((width & 3) ? 1u : 0u), blockRows = BlockRows(height);
    const uint32_t minBlocks = options.minBlocksPerChunk ? options.minBlocksPerChunk : 1u;
    const uint32_t minRows = minBlocks / blocksX + ((minBlocks % blocksX) ? 1u : 0u);
    const uint32_t wanted = (workers + 1) * kChunksPerThread;
    uint32_t chunkRows = (blockRows + wanted - 1) / wanted;
    if (chunkRows < minRows) chunkRows = minRows;
    if (chunkRows < 1) chunkRows = 1;
    const uint32_t chunks = (blockRows + chunkRows - 1) / chunkRows;
    if (chunks < 2) return serial();
    Pool& p = ThePool();
    if (p.inUse.exchange(true, std::memory_order_acquire)) { // another thread's image has the pool
        r.busy = true;
        return serial();
    }
    const uint32_t usefulWorkers=std::min(workers,chunks-1);
    uint32_t k = p.EnsureWorkers(usefulWorkers);
    if (!k) {
        p.inUse.store(false, std::memory_order_release);
        return serial();
    }
    // The task is written while the state is closed: threads that check in now (late wakes) leave without reading it
    Task& t = p.task;
    t.dxt5 = dxt5;
    t.dst = dst;
    t.src = src;
    t.fb = fallback;
    t.ctx = ctx;
    t.blockRows = blockRows;
    t.chunkRows = chunkRows;
    t.chunks = chunks;
    t.fp = ReadFp(); // the caller's x87 control word and MXCSR: every worker encodes with them
    t.next.store(0, std::memory_order_relaxed);
    t.byWorkers.store(0, std::memory_order_relaxed);
    t.helpers.store(0, std::memory_order_relaxed);
    t.fpMismatch.store(0, std::memory_order_relaxed);
    t.blocks.store(0, std::memory_order_relaxed);
    t.delegated.store(0, std::memory_order_relaxed);
    t.power.store(0, std::memory_order_relaxed);
    t.solid.store(0, std::memory_order_relaxed);
    t.workTicks.store(0, std::memory_order_relaxed);
    p.state.fetch_and(~kClosed, std::memory_order_release); // open
    for (uint32_t i = 0; i < k; i++) SetEvent(p.wake[i]);
    RunChunks(t, false); // the caller takes chunks too; when it finds none left, every chunk is taken
    p.state.fetch_or(kClosed, std::memory_order_acq_rel); // no thread joins from here on
    // wait for the threads still encoding their last chunk (a short spin first: that is usually microseconds)
    for (int spin = 0; (p.state.load(std::memory_order_acquire) & kCountMask) != 0; spin++) {
        if (spin < 4000) _mm_pause();
        else WaitForSingleObject(p.done, 50);
    }
    cnt.blocks += t.blocks.load(std::memory_order_relaxed);
    cnt.delegated += t.delegated.load(std::memory_order_relaxed);
    cnt.powerAxis += t.power.load(std::memory_order_relaxed);
    cnt.solid += t.solid.load(std::memory_order_relaxed);
    r.parallel = true;
    r.chunks = chunks;
    r.chunksByWorkers = t.byWorkers.load(std::memory_order_relaxed);
    r.participants = t.helpers.load(std::memory_order_relaxed) + (r.chunksByWorkers < chunks ? 1u : 0u);
    r.fpStateMismatches = t.fpMismatch.load(std::memory_order_relaxed);
    r.workMs = static_cast<double>(t.workTicks.load(std::memory_order_relaxed)) * QpcMs();
    p.inUse.store(false, std::memory_order_release);
    r.wallMs = static_cast<double>(Qpc() - t0) * QpcMs();
    return r;
}

} // namespace Parallel

} // namespace DxtCodec
