// Offline checks of the High-precision scene (framework/scene_target_logic.h). Credits: @loinyx
// 1. Pure logic: refusals, when SetRenderTarget substitutes, when a copy resolves first, the resolve's grain (exact 8-bit
//    levels unchanged, never past 0 / 255, average equal to the 16-bit value).
// 2. Native D3D9 (skipped without a device): the resolve shader compiled from the same source draws a 16-bit target into an
//    8-bit one; every pixel is compared with the C++ copy of its math, alpha must pass unchanged.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "scene_target_logic.h"
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")

namespace L = SceneTargetLogic;
int g_fail = 0, g_checks = 0;
void Expect(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL: %s\n", what);
    }
}

float Round(float x) { return std::floor(x + 0.5f); }

void PureChecks() {
    Expect(L::Check(D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, true) == L::Refusal::Multisample, "multisampled back buffer refused");
    Expect(L::Check(D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, true) == L::Refusal::BackBufferFormat, "back buffer without alpha refused");
    Expect(L::Check(D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, false) == L::Refusal::NoSupport, "no 16-bit blending refused");
    Expect(L::Check(D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, true) == L::Refusal::None, "A8R8G8B8 accepted");

    int bb = 0, other = 0;
    Expect(L::Substitutes(true, 0, &bb, &bb), "back buffer at RT0 substituted");
    Expect(!L::Substitutes(false, 0, &bb, &bb), "not after the resolve");
    Expect(!L::Substitutes(true, 1, &bb, &bb), "only render target 0");
    Expect(!L::Substitutes(true, 0, &other, &bb), "other targets (pick, shadows, bloom) untouched");
    Expect(!L::Substitutes(true, 0, nullptr, nullptr), "no back buffer known");
    Expect(L::NeedsResolve(true, &bb, &other, &bb) && L::NeedsResolve(true, &other, &bb, &bb), "copy from or to the back buffer resolves");
    Expect(!L::NeedsResolve(true, &other, nullptr, &bb) && !L::NeedsResolve(false, &bb, nullptr, &bb), "other copies and resolved frames do not");

    // exact 8-bit levels pass unchanged, whatever the noise
    bool exact = true;
    for (int k = 0; k < 256; k++)
        for (float t : {-0.999f, -0.5f, 0.3f, 0.999f}) exact &= Round(L::ResolveLevel(k * 257 / 65535.0f, t, 1.0f)) == static_cast<float>(k);
    Expect(exact, "exact 8-bit levels unchanged");

    // never past 0 / 255 (no clamp, so no bias), and an unbiased average (noise quantiles of the triangular distribution)
    constexpr int kQ = 2048;
    std::vector<float> t(kQ);
    for (int i = 0; i < kQ; i++) {
        const float r = 2.0f * ((i + 0.5f) / kQ) - 1.0f;
        t[i] = (r < 0 ? -1.0f : 1.0f) * (1.0f - std::sqrt(1.0f - std::fabs(r)));
    }
    bool inRange = true;
    double worstMid = 0, worstEdge = 0, worstPlain = 0;
    for (int v16 = 0; v16 < 65536; v16 += 3) {
        const float v = v16 / 65535.0f, x = v * 255.0f;
        double sum = 0;
        for (float n : t) {
            const float y = L::ResolveLevel(v, n, 1.0f);
            inRange &= y >= -1e-4f && y <= 255.0001f;
            sum += Round(y);
        }
        const double err = std::fabs(sum / kQ - x);
        double& worst = x >= 1.0f && x <= 254.0f ? worstMid : worstEdge;
        worst = std::max(worst, err);
        worstPlain = std::max(worstPlain, static_cast<double>(std::fabs(Round(x) - x)));
    }
    Expect(inRange, "the grain never reaches past 0 or 255");
    Expect(worstMid < 0.01, "average equals the 16-bit value (1..254 levels)");
    Expect(worstEdge < 0.5, "near black and white no worse than plain rounding");
    std::printf("resolve average error: %.4f levels (1..254), %.4f near the ends; plain rounding up to %.4f\n", worstMid, worstEdge, worstPlain);

    double mean = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) mean += L::Noise(static_cast<float>(x), static_cast<float>(y), 0.0f);
    Expect(std::fabs(mean / 4096) < 0.02, "noise centred over a 64x64 block");
}

void NativeChecks() {
    constexpr UINT W = 256, H = 64;
    HWND window = CreateWindowExW(0, L"STATIC", L"Scene target check", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE, pp.SwapEffect = D3DSWAPEFFECT_DISCARD, pp.hDeviceWindow = window, pp.BackBufferWidth = W, pp.BackBufferHeight = H;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    IDirect3DDevice9* dev = nullptr;
    if (!window || !d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev))) {
        std::printf("SKIP: no D3D9 HAL device, native resolve not checked\n");
        if (d3d) d3d->Release();
        return;
    }
    D3DDISPLAYMODE mode{};
    d3d->GetAdapterDisplayMode(0, &mode);
    const bool blending = d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, mode.Format, D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING,
                                                 D3DRTYPE_TEXTURE, L::kFormat) == D3D_OK;
    D3DADAPTER_IDENTIFIER9 id{};
    d3d->GetAdapterIdentifier(0, 0, &id);
    std::printf("%s: 16-bit render target with blending %s\n", id.Description, blending ? "supported" : "NOT supported");

    ID3DBlob *code = nullptr, *errors = nullptr;
    const HRESULT compiled = D3DCompile(L::kResolveHlsl, std::strlen(L::kResolveHlsl), "scene_target.hlsl", nullptr, nullptr, "main", "ps_3_0",
                                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    Expect(SUCCEEDED(compiled), "resolve shader compiles (ps_3_0)");
    if (FAILED(compiled)) {
        if (errors) std::printf("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        return;
    }
    IDirect3DPixelShader9* ps = nullptr;
    Expect(SUCCEEDED(dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &ps)), "resolve shader created");

    // the 16-bit scene: R a ramp inside and across levels, G exact levels, B the dark end, A the bloom mask (exact and between)
    std::vector<uint16_t> src(W * H * 4);
    for (UINT y = 0; y < H; y++)
        for (UINT x = 0; x < W; x++) {
            uint16_t* p = &src[(y * W + x) * 4];
            p[0] = static_cast<uint16_t>((x * 256 + y * 4) % 65536);
            p[1] = static_cast<uint16_t>(((x + y) % 256) * 257);
            p[2] = static_cast<uint16_t>(x * 3 + y);
            p[3] = static_cast<uint16_t>(y < 32 ? (x % 256) * 257 : x * 255 + y);
        }
    IDirect3DTexture9* tex = nullptr;
    IDirect3DSurface9 *texSurf = nullptr, *upload = nullptr, *out = nullptr, *read = nullptr;
    bool ok = SUCCEEDED(dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, L::kFormat, D3DPOOL_DEFAULT, &tex, nullptr)) &&
              SUCCEEDED(tex->GetSurfaceLevel(0, &texSurf)) &&
              SUCCEEDED(dev->CreateOffscreenPlainSurface(W, H, L::kFormat, D3DPOOL_SYSTEMMEM, &upload, nullptr));
    Expect(ok, "16-bit target created");
    if (!ok) return;
    D3DLOCKED_RECT lr{};
    upload->LockRect(&lr, nullptr, 0);
    for (UINT y = 0; y < H; y++) std::memcpy(static_cast<char*>(lr.pBits) + y * lr.Pitch, &src[y * W * 4], W * 8);
    upload->UnlockRect();
    Expect(SUCCEEDED(dev->UpdateSurface(upload, nullptr, texSurf, nullptr)), "16-bit scene uploaded");

    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &out);
    dev->CreateOffscreenPlainSurface(W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &read, nullptr);
    // the resolve at this strength, read back as A8R8G8B8 words
    const auto resolve = [&](float strength) {
        std::vector<DWORD> px(W * H);
        dev->BeginScene();
        dev->SetRenderTarget(0, out);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        const float amount[4] = {strength, 0.0f, 0.0f, 0.0f};
        dev->SetPixelShaderConstantF(0, amount, 1);
        dev->SetPixelShader(ps);
        dev->SetTexture(0, tex);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        struct V { float x, y, z, rhw, u, v; };
        const float x1 = W - 0.5f, y1 = H - 0.5f;
        const V quad[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {x1, -0.5f, 0, 1, 1, 0}, {-0.5f, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
        Expect(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V))), "resolve drawn");
        dev->EndScene();
        Expect(SUCCEEDED(dev->GetRenderTargetData(out, read)), "result read back");
        D3DLOCKED_RECT r{};
        read->LockRect(&r, nullptr, D3DLOCK_READONLY);
        for (UINT y = 0; y < H; y++) std::memcpy(&px[y * W], static_cast<const char*>(r.pBits) + y * r.Pitch, W * 4);
        read->UnlockRect();
        return px;
    };
    const auto channel = [](DWORD c, int ch) { return static_cast<int>(ch == 3 ? c >> 24 : (c >> (16 - 8 * ch)) & 255); };

    // strength 0: the hardware's own float -> 8-bit conversion, which the game's 8-bit scene goes through too. Measured
    // (measured on an RTX 4070 Ti SUPER): nearest, except that values up to ~0.06 level above a half round down
    const std::vector<DWORD> plain = resolve(0.0f);
    int notNearest = 0, unexplained = 0;
    double worstDown = 0;
    for (UINT i = 0; i < W * H; i++)
        for (int ch = 0; ch < 4; ch++) {
            const double x = src[i * 4 + ch] / 65535.0 * 255.0;
            const int got = channel(plain[i], ch), nearest = static_cast<int>(std::floor(x + 0.5));
            if (got == nearest) continue;
            ++notNearest;
            const double f = x - std::floor(x);
            if (got == static_cast<int>(std::floor(x)) && f >= 0.5 && f < 0.6) worstDown = std::max(worstDown, f);
            else ++unexplained;
        }
    std::printf("native strength 0: %d of %u channels not rounded to nearest (all just above a half: up to %.4f rounds down), %d otherwise\n",
                notNearest, W * H * 4, worstDown, unexplained);
    Expect(unexplained == 0, "the output rounds to nearest, or down just above a half");
    const std::vector<DWORD> px = resolve(1.0f);
    int off1 = 0, offMore = 0, exactBad = 0, alphaBad = 0;
    double rampErr = 0, mirrorErr = 0;
    for (UINT y = 0; y < H; y++)
        for (UINT x = 0; x < W; x++) {
            const DWORD c = px[y * W + x];
            const uint16_t* p = &src[(y * W + x) * 4];
            const float t = L::Noise(static_cast<float>(x), static_cast<float>(y), 0.0f);
            for (int ch = 0; ch < 3; ch++) {
                const int want = static_cast<int>(Round(L::ResolveLevel(p[ch] / 65535.0f, t, 1.0f)));
                const int d = std::abs(channel(c, ch) - want);
                off1 += d == 1, offMore += d > 1;
                if (ch == 0) mirrorErr += want - p[0] / 65535.0 * 255.0;
            }
            exactBad += channel(c, 1) != p[1] / 257;
            alphaBad += channel(c, 3) != channel(plain[y * W + x], 3);
            rampErr += channel(c, 0) - p[0] / 65535.0 * 255.0;
        }
    const int n = W * H * 3;
    std::printf("native resolve: %d of %d channels differ from the C++ copy by one level, %d by more; ramp mean error %.4f levels (C++ copy %.4f)\n",
                off1, n, offMore, rampErr / (W * H), mirrorErr / (W * H));
    Expect(offMore == 0, "GPU never differs from the C++ copy by more than one level");
    Expect(off1 * 20 < n, "GPU matches the C++ copy (under 5% one-level differences: float precision of the noise)");
    Expect(exactBad == 0, "exact 8-bit levels unchanged on the GPU");
    Expect(alphaBad == 0, "alpha (bloom mask) is the plain rounding of the 16-bit alpha");
    Expect(std::fabs(rampErr / (W * H)) < 0.05, "ramp keeps its average");
    read->Release();
    out->Release();
    upload->Release();
    texSurf->Release();
    tex->Release();
    ps->Release();
    code->Release();
    dev->Release();
    d3d->Release();
    DestroyWindow(window);
}

int main() {
    PureChecks();
    NativeChecks();
    std::printf("%s: %d of %d checks\n", g_fail ? "FAILED" : "PASS", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
