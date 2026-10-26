// Offline checks of the Banding Fix's smoothed sprite textures (features/sprite_smooth_logic.h). Credits: @loinyx
// 1. The pixel shader family: hand-assembled texture x vertex colour shaders are accepted, near variants refused.
// 2. A synthetic radial glow (128 x 128, white, the glow in alpha, like the captured headlight texture) is encoded with the
//    game's DXT5 encoder (features/dxt_codec.cpp, Ref), decoded, smoothed: the mean is kept, the slope no longer jumps from
//    texel to texel (no ring steps) and the glow stays within a few levels of the source, peak included.
// 3. The detail gate refuses textured content and hard edges; the mip box filter keeps the mean; DXT1 transparent texels.
#define NOMINMAX
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "dxt_codec.h"
#include "sprite_smooth_logic.h"

namespace L = SpriteSmoothLogic;
int g_fail = 0, g_checks = 0;
void Expect(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL: %s\n", what);
    }
}

// ---- token helpers (D3D9 shader model 2 / 3 encoding) ----
DWORD Ins(DWORD op, DWORD len) { return op | (len << 24); }
DWORD Reg(DWORD type, DWORD n, DWORD low) { return 0x80000000u | ((type & 7) << 28) | ((type & 0x18) << 8) | n | low; }
DWORD Dst(DWORD type, DWORD n, DWORD mask = 0xF, DWORD mod = 2 /* _pp */) { return Reg(type, n, (mask << 16) | (mod << 20)); }
DWORD Src(DWORD type, DWORD n, DWORD swz = 0xE4) { return Reg(type, n, swz << 16); }
constexpr DWORD T = 0, V = 1, C = 2, TX = 3, OC = 8, S = 10;

std::vector<DWORD> Ps2(std::vector<DWORD> body, DWORD ver = 0xFFFF0200u) {
    std::vector<DWORD> t = {ver, Ins(0x1F, 2), 0x80000000u, Src(V, 0) & ~0x00FF0000u | 0x000F0000u, Ins(0x1F, 2), 0x80000000u, Dst(TX, 1, 3, 2),
                            Ins(0x1F, 2), 0x90000000u, Reg(S, 0, 0xF << 16)};
    t.insert(t.end(), body.begin(), body.end());
    t.push_back(0x0000FFFFu);
    return t;
}
bool Accepts(const std::vector<DWORD>& t) { return L::IsTintedSprite(t.data(), t.size()); }

void ShaderChecks() {
    const DWORD texld[] = {Ins(0x42, 3), Dst(T, 0), Src(TX, 1), Src(S, 0)};
    auto with = [&](std::vector<DWORD> rest, const DWORD* tex = nullptr) {
        std::vector<DWORD> b(tex ? tex : texld, (tex ? tex : texld) + 4);
        b.insert(b.end(), rest.begin(), rest.end());
        return Ps2(b);
    };
    // the captured headlight form: texld_pp r0, t1, s0; mul_pp r0, r0, v0; mov_pp oC0, r0
    Expect(Accepts(with({Ins(5, 3), Dst(T, 0), Src(T, 0), Src(V, 0), Ins(1, 2), Dst(OC, 0), Src(T, 0)})), "texld; mul r0, r0, v0; mov oC0, r0 accepted");
    Expect(Accepts(with({Ins(5, 3), Dst(OC, 0), Src(V, 0), Src(T, 0)})), "mul oC0, v0, r0 accepted");
    Expect(Accepts(with({Ins(5, 3), Dst(T, 2), Src(T, 0), Src(V, 1), Ins(1, 2), Dst(OC, 0), Src(T, 2)})), "other temp, v1 accepted");
    Expect(Accepts(Ps2({texld[0], texld[1], texld[2], texld[3], Ins(5, 3), Dst(OC, 0), Src(T, 0), Src(V, 0)}, 0xFFFF0201u)), "ps_2_x accepted");
    // near variants that are not the family
    Expect(!Accepts(with({Ins(1, 2), Dst(OC, 0), Src(T, 0)})), "texture alone refused (no vertex colour)");
    Expect(!Accepts(with({Ins(5, 3), Dst(T, 0), Src(T, 0), Src(C, 0), Ins(1, 2), Dst(OC, 0), Src(T, 0)})), "constant tint refused");
    Expect(!Accepts(with({Ins(5, 3), Dst(T, 0), Src(T, 0), Src(V, 0, 0xFF), Ins(1, 2), Dst(OC, 0), Src(T, 0)})), "swizzled colour refused");
    Expect(!Accepts(with({Ins(5, 3), Dst(T, 0, 7), Src(T, 0), Src(V, 0), Ins(1, 2), Dst(OC, 0), Src(T, 0)})), "partial write refused");
    Expect(!Accepts(with({Ins(5, 3), Dst(T, 0), Src(T, 0), Src(V, 0), Ins(2, 3), Dst(T, 0), Src(T, 0), Src(C, 0), Ins(1, 2), Dst(OC, 0), Src(T, 0)})),
           "extra arithmetic refused");
    const DWORD texld1[] = {Ins(0x42, 3), Dst(T, 0), Src(TX, 1), Src(S, 1)};
    Expect(!Accepts(with({Ins(5, 3), Dst(T, 0), Src(T, 0), Src(V, 0), Ins(1, 2), Dst(OC, 0), Src(T, 0)}, texld1)), "sampler 1 refused");
    Expect(!Accepts(with({Ins(5, 3), Dst(T, 1), Src(T, 0), Src(V, 0), Ins(1, 2), Dst(OC, 0), Src(T, 0)})), "mov of another temp refused");
    // ps_3_0: v1 declared COLOR is a tint, a TEXCOORD input is not
    auto ps3 = [&](DWORD usage1) {
        return std::vector<DWORD>{0xFFFF0300u, Ins(0x1F, 2), 0x80000000u | D3DDECLUSAGE_TEXCOORD, Dst(V, 0, 3, 0), Ins(0x1F, 2), 0x80000000u | usage1,
                                  Dst(V, 1, 0xF, 0), Ins(0x1F, 2), 0x90000000u, Reg(S, 0, 0xF << 16), Ins(0x42, 3), Dst(T, 0), Src(V, 0), Src(S, 0),
                                  Ins(5, 3), Dst(OC, 0), Src(T, 0), Src(V, 1), 0x0000FFFFu};
    };
    Expect(Accepts(ps3(D3DDECLUSAGE_COLOR)), "ps_3_0 with a COLOR input accepted");
    Expect(!Accepts(ps3(D3DDECLUSAGE_TEXCOORD)), "ps_3_0 times a texture coordinate refused");
}

// ---- the radial glow ----
constexpr int kSide = 128;
float Glow(int x, int y) { // a cone saturating at its centre, 0 outside 56 texels (the captured glow's shape)
    const float r = std::hypot(x + 0.5f - kSide / 2.0f, y + 0.5f - kSide / 2.0f);
    return std::clamp(255.0f * 1.25f * (1.0f - r / 56.0f), 0.0f, 255.0f);
}
double Mean(const std::vector<float>& img, int c) {
    double s = 0.0;
    for (size_t i = 0; i < img.size() / 4; i++) s += img[4 * i + c];
    return s / (img.size() / 4);
}
// The ring steps: how much the alpha's slope changes from one texel to the next (|second difference|) along rows and
// columns, over the ramp (both neighbours strictly inside 4..251 levels in the source). A smooth cone has almost none;
// bilinear magnification turns each change into a crease, which reads as a ring.
struct Kinks {
    double rms = 0.0, max = 0.0;
};
Kinks KinksOf(const std::vector<float>& img) {
    Kinks k;
    size_t n = 0;
    for (int y = 1; y < kSide - 1; y++)
        for (int x = 1; x < kSide - 1; x++)
            for (int d = 0; d < 2; d++) {
                const int dx = d == 0, dy = d == 1;
                const float s0 = Glow(x - dx, y - dy), s2 = Glow(x + dx, y + dy);
                if (s0 <= 4.0f || s0 >= 251.0f || s2 <= 4.0f || s2 >= 251.0f) continue;
                const double v = img[4 * ((y - dy) * kSide + x - dx) + 3] - 2.0 * img[4 * (y * kSide + x) + 3] + img[4 * ((y + dy) * kSide + x + dx) + 3];
                k.rms += v * v;
                k.max = std::max(k.max, std::fabs(v));
                n++;
            }
    k.rms = std::sqrt(k.rms / n);
    return k;
}

void GlowChecks() {
    std::vector<uint8_t> src(kSide * kSide * 4);
    std::vector<float> ideal(kSide * kSide * 4);
    for (int y = 0; y < kSide; y++)
        for (int x = 0; x < kSide; x++) {
            const float a = std::floor(Glow(x, y) + 0.5f);
            uint8_t* p = &src[4 * (y * kSide + x)];
            p[0] = p[1] = p[2] = 255; // B, G, R
            p[3] = static_cast<uint8_t>(a);
            float* q = &ideal[4 * (y * kSide + x)];
            q[0] = q[1] = q[2] = 255.0f;
            q[3] = a;
        }
    const uint32_t pitch = kSide / 4 * 16;
    std::vector<uint8_t> blocks(pitch * kSide / 4);
    DxtCodec::Src s{src.data(), 0, 0, kSide * 4, 0x3D};
    DxtCodec::Dst d{blocks.data(), kSide, kSide, pitch, 0};
    DxtCodec::Ref::EncodeDxt5(&d, &s);
    std::vector<float> dec(kSide * kSide * 4);
    Expect(L::Decode(D3DFMT_DXT5, blocks.data(), pitch, kSide, kSide, dec.data()), "DXT5 decoded");
    float decErr = 0.0f;
    for (size_t i = 0; i < dec.size(); i++) decErr = std::max(decErr, std::fabs(dec[i] - ideal[i]));
    Expect(decErr < 12.0f, "the decoded glow is the source glow within DXT5 error");

    const Kinks kIdeal = KinksOf(ideal), kDxt = KinksOf(dec);
    std::printf("glow 128x128 DXT5 (game encoder): decode error max %.1f levels; kinks rms / max: source %.2f / %.2f, DXT5 %.2f / %.2f\n", decErr,
                kIdeal.rms, kIdeal.max, kDxt.rms, kDxt.max);
    Expect(kDxt.max > 2.0 * kIdeal.max + 1.0, "the DXT5 glow has ring steps (the problem reproduces)");
    for (float sigma : {1.0f, 1.5f, 2.0f}) {
        std::vector<float> sm = dec;
        L::Gaussian(sm.data(), kSide, kSide, sigma);
        const Kinks k = KinksOf(sm);
        float err = 0.0f;
        for (size_t i = 3; i < sm.size(); i += 4) err = std::max(err, std::fabs(sm[i] - ideal[i]));
        const float peak = sm[4 * ((kSide / 2) * kSide + kSide / 2) + 3];
        std::printf("  sigma %.1f: kinks rms / max %.2f / %.2f; max |smoothed - source| %.1f levels; peak %.1f; mean alpha %.3f -> %.3f\n", sigma, k.rms,
                    k.max, err, peak, Mean(dec, 3), Mean(sm, 3));
        if (sigma != L::kSigma) continue;
        Expect(std::fabs(Mean(sm, 3) - Mean(dec, 3)) < 0.01, "smoothing keeps the mean alpha");
        Expect(std::fabs(Mean(sm, 0) - Mean(dec, 0)) < 0.01, "smoothing keeps the mean colour");
        Expect(k.max < 0.5 * kDxt.max && k.rms < 0.5 * kDxt.rms, "no ring steps: the slope changes at most half as much as the DXT5 glow");
        Expect(k.max < 2.0f, "no ring steps: no crease of 2 levels or more per texel");
        Expect(err < 8.0f, "the smoothed glow stays within 8 levels of the source");
        Expect(peak >= 253.0f, "the peak is kept");
        Expect(L::SmoothEnough(dec.data(), sm.data(), kSide, kSide), "the glow passes the detail gate");
        // its mip chain keeps the mean
        std::vector<float> half((kSide / 2) * (kSide / 2) * 4);
        L::Half(sm.data(), kSide, kSide, half.data());
        Expect(std::fabs(Mean(half, 3) - Mean(sm, 3)) < 1e-3, "the 2x2 mip keeps the mean");
        // 16-bit storage: the smoothed ramp keeps sub-level steps
        Expect(L::To16(255.0f) == 65535 && L::To16(0.0f) == 0 && L::To16(-1.0f) == 0 && L::To8(127.6f) == 128, "texel conversion");
    }
}

void GateChecks() {
    // textured content: a checker of 4-texel squares between 64 and 192
    std::vector<float> img(64 * 64 * 4), sm;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            for (int c = 0; c < 4; c++) img[4 * (y * 64 + x) + c] = ((x / 4 + y / 4) & 1) ? 192.0f : 64.0f;
    sm = img;
    L::Gaussian(sm.data(), 64, 64, L::kSigma);
    Expect(!L::SmoothEnough(img.data(), sm.data(), 64, 64), "textured content refused");
    // a hard-edged disc (a cut-out)
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) img[4 * (y * 64 + x) + 3] = std::hypot(x - 31.5f, y - 31.5f) < 20.0f ? 255.0f : 0.0f, img[4 * (y * 64 + x)] = 255.0f,
                                     img[4 * (y * 64 + x) + 1] = 255.0f, img[4 * (y * 64 + x) + 2] = 255.0f;
    sm = img;
    L::Gaussian(sm.data(), 64, 64, L::kSigma);
    Expect(!L::SmoothEnough(img.data(), sm.data(), 64, 64), "hard edge refused");
    // a flat texture passes and is unchanged
    for (float& v : img) v = 100.0f;
    sm = img;
    L::Gaussian(sm.data(), 64, 64, L::kSigma);
    float flat = 0.0f;
    for (float v : sm) flat = std::max(flat, std::fabs(v - 100.0f));
    Expect(flat < 1e-3f && L::SmoothEnough(img.data(), sm.data(), 64, 64), "flat texture unchanged");

    // DXT1: c0 <= c1 gives three colours and transparent black at index 3
    const uint8_t block[8] = {0x00, 0x00, 0xFF, 0xFF, 0xE4, 0xE4, 0xE4, 0xE4}; // c0 black, c1 white; indices 0,1,2,3 per row
    std::vector<float> px(16 * 4);
    Expect(L::Decode(D3DFMT_DXT1, block, 8, 4, 4, px.data()), "DXT1 decoded");
    Expect(px[3] == 255.0f && px[4 * 1 + 0] == 255.0f && std::fabs(px[4 * 2 + 0] - 127.5f) < 0.01f && px[4 * 3 + 3] == 0.0f && px[4 * 3 + 0] == 0.0f,
           "DXT1 three-colour block: midpoint and transparent black");
    Expect(!L::Decode(D3DFMT_A8R8G8B8, block, 8, 4, 4, px.data()), "other formats refused");
}

int main() {
    ShaderChecks();
    GlowChecks();
    GateChecks();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
