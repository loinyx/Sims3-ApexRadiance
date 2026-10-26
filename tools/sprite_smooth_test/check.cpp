// Offline checks of the Banding Fix's smoothed sprite textures (features/sprite_smooth_logic.h). Credits: @loinyx
//   check.exe [--glow <T_..._DXT5.png>] [--beam <T_..._DXT5.png>] [--scan <captures folder>]
// 1. The pixel shader family: hand-assembled texture x vertex colour shaders are accepted, near variants refused.
// 2. A synthetic radial glow (128 x 128) and a synthetic light beam (64 x 256, steep sides), white with the shape in
//    alpha like the captured headlight textures, encoded with the game's DXT5 encoder (features/dxt_codec.cpp, Ref) and
//    decoded: after the smoothing the mean is kept, the slope no longer jumps from texel to texel where the shape is a
//    gentle ramp (no ring steps), and the beam's sides keep their sharpness.
// 3. The gate refuses textured content and hard edges; the mip box filter keeps the mean; DXT1 transparent texels.
// 4. With --glow / --beam: the same measures on a captured texture (the light capture's lossless PNG of the decoded
//    texture and its _alpha.png). With --scan: every captured DXT texture of 16 to 512 texels a side under the folder,
//    with the gate's answer. Read-only.
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "dxt_codec.h"
#include "sprite_smooth_logic.h"
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

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

// ---- measures on the alpha channel ----
struct Image {
    int w = 0, h = 0;
    std::vector<float> rgba;
    float A(int x, int y) const { return rgba[4 * (static_cast<size_t>(y) * w + x) + 3]; }
};
double Mean(const Image& im, int c) {
    double s = 0.0;
    for (size_t i = 0; i < im.rgba.size() / 4; i++) s += im.rgba[4 * i + c];
    return s / (im.rgba.size() / 4);
}
constexpr float kRampStep = 12.0f; // levels per texel: a gentler step in the decoded texture is a ramp, a larger one a real edge
struct Measure {
    double kinkRms = 0.0, kinkMax = 0.0; // ramp steps: how much the slope changes from one texel to the next (|second difference|)
    double edgeKept = 1.0;               // real edges: sum of their steps after / before
    double changeRms = 0.0, changeMax = 0.0;
};
// Over the ramps of `ref` (three texels in a row or column, inside 1..251 levels, both steps at most kRampStep): bilinear
// magnification turns each change of slope into a crease, which reads as a ring
Measure Of(const Image& a, const Image& ref) {
    Measure m;
    size_t n = 0;
    double before = 0.0, after = 0.0;
    for (int y = 0; y < ref.h; y++)
        for (int x = 0; x < ref.w; x++)
            for (int d = 0; d < 2; d++) {
                const int dx = d == 0, dy = d == 1;
                if (x + dx < ref.w && y + dy < ref.h) {
                    const float step = std::fabs(ref.A(x + dx, y + dy) - ref.A(x, y));
                    if (step > kRampStep) before += step, after += std::fabs(a.A(x + dx, y + dy) - a.A(x, y));
                }
                if (x - dx < 0 || y - dy < 0 || x + dx >= ref.w || y + dy >= ref.h) continue;
                const float r0 = ref.A(x - dx, y - dy), r1 = ref.A(x, y), r2 = ref.A(x + dx, y + dy);
                if (r0 <= 1 || r1 <= 1 || r2 <= 1 || r0 >= 251 || r1 >= 251 || r2 >= 251 || std::fabs(r1 - r0) > kRampStep || std::fabs(r2 - r1) > kRampStep)
                    continue;
                const double v = a.A(x - dx, y - dy) - 2.0 * a.A(x, y) + a.A(x + dx, y + dy);
                m.kinkRms += v * v;
                m.kinkMax = std::max(m.kinkMax, std::fabs(v));
                n++;
            }
    m.kinkRms = n ? std::sqrt(m.kinkRms / n) : 0.0;
    m.edgeKept = before > 0.0 ? after / before : 1.0;
    for (size_t i = 3; i < a.rgba.size(); i += 4) {
        const double d = a.rgba[i] - ref.rgba[i];
        m.changeRms += d * d;
        m.changeMax = std::max(m.changeMax, std::fabs(d));
    }
    m.changeRms = std::sqrt(m.changeRms / (a.rgba.size() / 4));
    return m;
}

struct Result {
    Image smoothed;
    float edgeShare = 0.0f;
    bool accepted = false;
};
Result SmoothOf(const Image& in) {
    Result r{in};
    r.edgeShare = L::Smooth(r.smoothed.rgba.data(), in.w, in.h);
    r.accepted = L::SmoothEnough(in.rgba.data(), r.smoothed.rgba.data(), in.w, in.h, r.edgeShare);
    return r;
}

// Prints the measures and checks a glow or a beam: accepted, mean kept, ramp steps at most maxKinks of before, edges kept
void CheckShape(const char* name, const Image& decoded, double minEdgeKept, double maxKinks) {
    const Result r = SmoothOf(decoded);
    const Measure m0 = Of(decoded, decoded), m1 = Of(r.smoothed, decoded);
    const double mean0 = Mean(decoded, 3), mean1 = Mean(r.smoothed, 3);
    std::printf("%s %dx%d: ramp slope changes rms / max %.2f / %.1f -> %.2f / %.1f levels; real edges kept %.0f%%; change rms %.2f, max %.1f; "
                "mean alpha %.3f -> %.3f; edge texels %.1f%%; %s\n",
                name, decoded.w, decoded.h, m0.kinkRms, m0.kinkMax, m1.kinkRms, m1.kinkMax, 100.0 * m1.edgeKept, m1.changeRms, m1.changeMax, mean0,
                mean1, 100.0 * r.edgeShare, r.accepted ? "smoothed" : "refused");
    std::string what = std::string(name) + ": ";
    Expect(r.accepted, (what + "passes the gate").c_str());
    Expect(std::fabs(mean1 - mean0) < 0.05, (what + "mean alpha kept within 0.05 level").c_str());
    Expect(m1.kinkRms < maxKinks * m0.kinkRms, (what + "ramp slope changes reduced (no ring steps)").c_str());
    Expect(m1.edgeKept >= minEdgeKept, (what + "real edges kept").c_str());
}

Image Encoded(const std::vector<uint8_t>& bgra, int w, int h) {
    const uint32_t pitch = w / 4 * 16;
    std::vector<uint8_t> blocks(pitch * h / 4);
    DxtCodec::Src s{bgra.data(), 0, 0, w * 4, 0x3D};
    DxtCodec::Dst d{blocks.data(), static_cast<uint32_t>(w), static_cast<uint32_t>(h), pitch, 0};
    DxtCodec::Ref::EncodeDxt5(&d, &s);
    Image im{w, h, std::vector<float>(static_cast<size_t>(w) * h * 4)};
    Expect(L::Decode(D3DFMT_DXT5, blocks.data(), pitch, w, h, im.rgba.data()), "DXT5 decoded");
    return im;
}

void SyntheticChecks() {
    // a cone saturating at its centre, 0 outside 56 texels (the captured glow's shape)
    std::vector<uint8_t> glow(128 * 128 * 4);
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) {
            const float r = std::hypot(x + 0.5f - 64.0f, y + 0.5f - 64.0f);
            uint8_t* p = &glow[4 * (y * 128 + x)];
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<uint8_t>(std::clamp(255.0f * 1.25f * (1.0f - r / 56.0f), 0.0f, 255.0f) + 0.5f);
        }
    CheckShape("synthetic glow", Encoded(glow, 128, 128), 0.0, 0.5);
    // a beam: widening and dimming from the lamp (bottom) to the far end, its sides a quarter of its half-width (the captured beam)
    std::vector<uint8_t> beam(64 * 256 * 4);
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 64; x++) {
            const float t = y / 255.0f, half = 4.0f + 20.0f * (1.0f - t), peak = 2.0f + 200.0f * t * t;
            uint8_t* p = &beam[4 * (y * 64 + x)];
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<uint8_t>(peak * std::clamp((half - std::fabs(x + 0.5f - 32.0f)) / (1.5f + 0.25f * half), 0.0f, 1.0f) + 0.5f);
        }
    CheckShape("synthetic beam", Encoded(beam, 64, 256), 0.85, 0.6); // (its ramps include the sides' feet, kept on purpose)
}

void GateChecks() {
    Image img{64, 64, std::vector<float>(64 * 64 * 4)};
    // textured content: a checker of 4-texel squares between 64 and 192
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            for (int c = 0; c < 4; c++) img.rgba[4 * (y * 64 + x) + c] = ((x / 4 + y / 4) & 1) ? 192.0f : 64.0f;
    Expect(!SmoothOf(img).accepted, "textured content refused");
    // fine noise of +-6 levels (a detailed particle)
    uint32_t seed = 1;
    for (float& v : img.rgba) v = 128.0f + static_cast<float>(((seed = seed * 1664525u + 1013904223u) >> 24) % 13) - 6.0f;
    Expect(!SmoothOf(img).accepted, "fine detail refused");
    // a hard-edged disc (a cut-out)
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float* p = &img.rgba[4 * (y * 64 + x)];
            p[0] = p[1] = p[2] = 255.0f;
            p[3] = std::hypot(x - 31.5f, y - 31.5f) < 20.0f ? 255.0f : 0.0f;
        }
    Expect(!SmoothOf(img).accepted, "hard edge refused");
    // a flat texture passes and is unchanged
    for (float& v : img.rgba) v = 100.0f;
    const Result flat = SmoothOf(img);
    float most = 0.0f;
    for (float v : flat.smoothed.rgba) most = std::max(most, std::fabs(v - 100.0f));
    Expect(most < 1e-3f && flat.accepted, "flat texture unchanged");
    // the 2x2 mip keeps the mean
    for (int i = 0; i < 64 * 64 * 4; i++) img.rgba[i] = static_cast<float>(i % 251);
    std::vector<float> half(32 * 32 * 4);
    L::Half(img.rgba.data(), 64, 64, half.data());
    double a = 0.0, b = 0.0;
    for (size_t i = 3; i < img.rgba.size(); i += 4) a += img.rgba[i];
    for (size_t i = 3; i < half.size(); i += 4) b += half[i];
    Expect(std::fabs(a / (64 * 64) - b / (32 * 32)) < 1e-3, "the 2x2 mip keeps the mean");
    Expect(L::To16(255.0f) == 65535 && L::To16(0.0f) == 0 && L::To16(-1.0f) == 0 && L::To8(127.6f) == 128, "texel conversion");

    // DXT1: c0 <= c1 gives three colours and transparent black at index 3
    const uint8_t block[8] = {0x00, 0x00, 0xFF, 0xFF, 0xE4, 0xE4, 0xE4, 0xE4}; // c0 black, c1 white; indices 0,1,2,3 per row
    std::vector<float> px(16 * 4);
    Expect(L::Decode(D3DFMT_DXT1, block, 8, 4, 4, px.data()), "DXT1 decoded");
    Expect(px[3] == 255.0f && px[4 * 1 + 0] == 255.0f && std::fabs(px[4 * 2 + 0] - 127.5f) < 0.01f && px[4 * 3 + 3] == 0.0f && px[4 * 3 + 0] == 0.0f,
           "DXT1 three-colour block: midpoint and transparent black");
    Expect(!L::Decode(D3DFMT_A8R8G8B8, block, 8, 4, 4, px.data()), "other formats refused");
}

// ---- captured textures (the light capture's PNGs: the decoded texture, its alpha in a separate _alpha.png) ----
bool LoadPng(const std::wstring& path, int& w, int& h, std::vector<uint8_t>& bgra) {
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
              SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));
    UINT uw = 0, uh = 0;
    if (ok) ok = SUCCEEDED(conv->GetSize(&uw, &uh));
    if (ok) {
        w = static_cast<int>(uw), h = static_cast<int>(uh);
        bgra.resize(static_cast<size_t>(w) * h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(bgra.size()), bgra.data()));
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{conv, frame, dec, f})
        if (u) u->Release();
    return ok;
}
// RGB from <path>, alpha from <path minus .png>_alpha.png when it exists (else 255)
bool LoadCaptured(const std::filesystem::path& path, Image& im) {
    int w = 0, h = 0, aw = 0, ah = 0;
    std::vector<uint8_t> rgb, alpha;
    if (!LoadPng(path.wstring(), w, h, rgb)) return false;
    std::filesystem::path ap = path;
    ap.replace_filename(path.stem().wstring() + L"_alpha.png");
    const bool hasAlpha = std::filesystem::exists(ap) && LoadPng(ap.wstring(), aw, ah, alpha) && aw == w && ah == h;
    im = Image{w, h, std::vector<float>(static_cast<size_t>(w) * h * 4)};
    for (size_t i = 0; i < static_cast<size_t>(w) * h; i++) {
        im.rgba[4 * i + 0] = rgb[4 * i + 2], im.rgba[4 * i + 1] = rgb[4 * i + 1], im.rgba[4 * i + 2] = rgb[4 * i + 0];
        im.rgba[4 * i + 3] = hasAlpha ? alpha[4 * i + 2] : 255.0f;
    }
    return true;
}

void Scan(const std::filesystem::path& root) {
    std::map<std::string, int> seen; // one line per distinct texture (same name, size and mean)
    int accepted = 0, refused = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
        const std::string name = e.path().filename().string();
        int w = 0, h = 0;
        char fmt[8] = {};
        if (name.find("_alpha") != std::string::npos || sscanf_s(name.c_str(), "T%*d_%dx%d_%7[A-Z0-9].png", &w, &h, fmt, 8) != 3) continue;
        if (std::string(fmt).rfind("DXT", 0) != 0 || w < 16 || h < 16 || w > 512 || h > 512) continue;
        Image im;
        if (!LoadCaptured(e.path(), im)) continue;
        char key[96];
        std::snprintf(key, sizeof(key), "%dx%d %s %.3f %.3f", w, h, fmt, Mean(im, 0), Mean(im, 3));
        if (seen[key]++) continue;
        const Result r = SmoothOf(im);
        double rms[4] = {};
        size_t over1 = 0, over2 = 0;
        for (size_t i = 0; i < im.rgba.size(); i++) over1 += std::fabs(r.smoothed.rgba[i] - im.rgba[i]) > 1.0f, over2 += std::fabs(r.smoothed.rgba[i] - im.rgba[i]) > 2.0f;
        for (size_t i = 0; i < im.rgba.size(); i++) rms[i % 4] += (r.smoothed.rgba[i] - im.rgba[i]) * (r.smoothed.rgba[i] - im.rgba[i]);
        std::printf("  %-60s edge %5.1f%%  change rms R %.2f G %.2f B %.2f A %.2f, >1 %4.1f%% >2 %4.1f%%  %s\n",
                    (e.path().parent_path().filename().string().substr(0, 19) + " " + name).c_str(), 100.0 * r.edgeShare,
                    std::sqrt(rms[0] / (w * h)), std::sqrt(rms[1] / (w * h)), std::sqrt(rms[2] / (w * h)), std::sqrt(rms[3] / (w * h)), 100.0 * over1 / im.rgba.size(), 100.0 * over2 / im.rgba.size(),
                    r.accepted ? "SMOOTHED" : "refused");
        (r.accepted ? accepted : refused)++;
    }
    std::printf("scan: %d distinct captured DXT textures, %d smoothed, %d refused\n", accepted + refused, accepted, refused);
}

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ShaderChecks();
    SyntheticChecks();
    GateChecks();
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string opt = argv[i];
        if (opt == "--scan") {
            Scan(argv[i + 1]);
            continue;
        }
        Image im;
        if (!LoadCaptured(argv[i + 1], im)) {
            Expect(false, ("captured texture loaded: " + std::string(argv[i + 1])).c_str());
            continue;
        }
        if (opt == "--glow") CheckShape("captured glow", im, 0.0, 0.5);
        if (opt == "--beam") CheckShape("captured beam", im, 0.85, 0.5);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
