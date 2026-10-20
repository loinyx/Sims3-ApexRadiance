// Offline checks of the per-vertex foliage lamp patch (features/foliage_vertex_lamps.cpp). Read-only: patches a captured
// foliage vertex shader with the production function, runs both shaders through D3D9 software vertex processing
// (ProcessVertices) and compares (1) the lamp output oT2 with a C++ reference of the same physics, (2) every other output
// with the unpatched shader. Credits: @loinyx
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include "features/foliage_vertex_lamps.h"
#include "features/lamp_reach.h"
#pragma comment(lib, "d3d9.lib")

struct V3 {
    float x, y, z;
};
static V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float LawNoGain(float d2, float w, float minD2) {
    float r0, R;
    LampReach::Radii(w, r0, R);
    d2 = std::max(d2, minD2);
    const float q = d2 / (R * R), win = std::clamp(1 - q * q, 0.f, 1.f);
    return 1 / d2 * std::min(1.f, r0 * r0 / d2 * win * win);
}
struct Lamp {
    V3 p;
    float w;
    V3 col;
    float h, scale, cap;
};
struct Crown {
    V3 c;
    float rh, rv, kappa;
};
static const float kMinD2 = 1e-4f;

static V3 Ref(V3 P, V3 N, const Lamp* L, const Crown& cn) {
    V3 out{0, 0, 0};
    const V3 s{1 / cn.rh, 1 / cn.rv, 1 / cn.rh}, p{(P.x - cn.c.x) * s.x, (P.y - cn.c.y) * s.y, (P.z - cn.c.z) * s.z};
    const float cc = Dot(p, p) - 1;
    for (int k = 0; k < 3; k++) {
        const Lamp& l = L[k];
        const V3 d = l.p - P;
        const float d2 = std::max(Dot(d, d), kMinD2), id = 1 / std::sqrt(d2);
        const V3 u{d.x * id, d.y * id, d.z * id};
        const float ndl = Dot(u, N), e = std::max(ndl, 0.f) + FoliageVertexLamps::kTransmitOverReflect * std::max(-ndl, 0.f); // c255 = (1, 0)
        const float ratio = std::min(LawNoGain(d2, l.w, kMinD2) * l.scale, l.cap);
        const V3 q{u.x * s.x, u.y * s.y, u.z * s.z};
        const float a = Dot(q, q), b = Dot(p, q), sq = std::sqrt(std::max(b * b - a * cc, 0.f));
        const float t = std::min(std::max((-b + sq) / a, 0.f) - std::max((-b - sq) / a, 0.f), std::sqrt(d2)), tr = std::exp(-t * cn.kappa);
        const float f = e * ratio * tr;
        out.x += f * l.col.x, out.y += f * l.col.y, out.z += f * l.col.z;
        if (k) continue;
        const float hor2 = d2 * (1 - u.y * u.y), dg2 = std::max(hor2 + l.h * l.h, kMinD2);
        const float eg = LawNoGain(dg2, l.w, kMinD2) * l.scale * l.h / std::sqrt(dg2) * (1 - N.y) * FoliageVertexLamps::kGroundHalfAlbedo;
        out.x += eg * l.col.x, out.y += eg * l.col.y, out.z += eg * l.col.z;
    }
    return out;
}

static int g_checks = 0, g_failures = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), {});
    if (raw.size() < 8 || raw.size() % 4) return 2;
    std::vector<uint32_t> original(raw.size() / 4);
    std::memcpy(original.data(), raw.data(), raw.size());
    std::vector<uint32_t> patched = original;
    int B = -1;
    Check(FoliageVertexLamps::Patch(patched, B) && B > 0, "captured foliage shader patched");
    std::vector<uint32_t> again = patched;
    int B2 = -1;
    Check(!FoliageVertexLamps::Patch(again, B2), "a patched shader is not patched twice");
    std::vector<uint32_t> notFoliage = {0xFFFE0200u, 0x0000FFFFu};
    Check(!FoliageVertexLamps::Patch(notFoliage, B2), "a shader without the lamp pattern is left alone");
    if (g_failures) return 1;

    HWND w = CreateWindowA("STATIC", "t", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    IDirect3D9* d3 = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE, pp.SwapEffect = D3DSWAPEFFECT_DISCARD, pp.hDeviceWindow = w;
    IDirect3DDevice9* dv = nullptr;
    HRESULT hc = d3 ? d3->CreateDevice(0, D3DDEVTYPE_HAL, w, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dv) : E_FAIL;
    if (FAILED(hc) && d3) hc = d3->CreateDevice(0, D3DDEVTYPE_REF, w, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dv);
    if (FAILED(hc)) {
        std::printf("FAIL: no D3D9 device (%08lX)\n", hc);
        return 1;
    }
    IDirect3DVertexShader9 *vsP = nullptr, *vsO = nullptr;
    Check(SUCCEEDED(dv->CreateVertexShader(reinterpret_cast<const DWORD*>(patched.data()), &vsP)), "the D3D9 runtime accepts the patched shader (vs_2_0 limits)");
    Check(SUCCEEDED(dv->CreateVertexShader(reinterpret_cast<const DWORD*>(original.data()), &vsO)), "the original shader loads");
    if (g_failures) return 1;
    const D3DVERTEXELEMENT9 in[] = {{0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITION, 0}, {0, 16, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_NORMAL, 0},
                                    {0, 32, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 0}, {0, 48, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 1},
                                    {0, 64, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 2}, {0, 80, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 3},
                                    D3DDECL_END()};
    IDirect3DVertexDeclaration9* di = nullptr;
    dv->CreateVertexDeclaration(in, &di);
    // the scene: instance 1 (a0.w = 3), its crown an ellipsoid 1.5 x 2 m at y = 2; three lamps, one inside the crown's reach
    std::vector<float> C(256 * 4, 0.f);
    const auto set = [&](int r, float x, float y, float z, float ww) { C[r * 4] = x, C[r * 4 + 1] = y, C[r * 4 + 2] = z, C[r * 4 + 3] = ww; };
    const int I = 3;
    set(I + 1, 1, 0, 0, 0);
    set(I + 2, 1, 0, 0, 0);
    for (int r = 117; r < 121; r++) set(r, r == 117, r == 118, r == 119, r == 120);
    set(129, 1, 0, 0, 0);
    set(255, 1, 0, 0, 0); // the reach-law weights: plain Lambert
    Lamp L[3] = {{{0.6f, 2.3f, 0.2f}, 85, {1, 0.8f, 0.6f}, 0.4f, 0, 0}, {{-1, 5, 0}, 150, {0.5f, 0.5f, 1}, 5, 0, 0}, {{0, 1, -6}, 60, {0.2f, 1, 0.2f}, 1, 0, 0}};
    const V3 centre{0, 2, 0};
    const Crown cn{{0, 2, 0}, 1.5f, 2.f, 0.369f};
    for (int k = 0; k < 3; k++) {
        float r0, R;
        LampReach::Radii(L[k].w, r0, R);
        const V3 dc = L[k].p - centre;
        L[k].scale = 1 / LawNoGain(Dot(dc, dc), L[k].w, kMinD2) * 1.6f; // a colour that was capped: the true law is 1.6 x
        L[k].cap = 1.0f / std::max({L[k].col.x, L[k].col.y, L[k].col.z});
        set(B + I + k, L[k].p.x, L[k].p.y, L[k].p.z, r0 * r0);
        set(B + 27 + I + k, 1 / (R * R), L[k].scale, kMinD2, L[k].h);
        set(54 + I + k, L[k].col.x, L[k].col.y, L[k].col.z, 0);
    }
    set(B + 54 + I, cn.c.x, cn.c.y, cn.c.z, 1 / cn.rh);
    set(B + 55 + I, 1 / cn.rv, cn.kappa / std::log(2.f), 0, 0);
    set(B + 56 + I, L[0].cap, L[1].cap, L[2].cap, FoliageVertexLamps::kTransmitOverReflect);
    set(B + 81, FoliageVertexLamps::kTransmitOverReflect, FoliageVertexLamps::kGroundHalfAlbedo, 0, 1);
    const int n = 2000;
    std::vector<float> vb(n * 24);
    std::vector<V3> P(n), N(n);
    srand(7);
    const auto rnd = [] { return rand() / static_cast<float>(RAND_MAX); };
    for (int i = 0; i < n; i++) {
        P[i] = {(rnd() * 2 - 1) * 1.4f, 0.3f + rnd() * 3.4f, (rnd() * 2 - 1) * 1.4f};
        V3 nn{rnd() * 2 - 1, rnd() * 2 - 1, rnd() * 2 - 1};
        const float l = std::sqrt(Dot(nn, nn)) + 1e-6f;
        nn = {nn.x / l, nn.y / l, nn.z / l};
        float* v = &vb[i * 24];
        v[0] = P[i].x, v[1] = P[i].y, v[2] = P[i].z, v[3] = 1;
        v[4] = (nn.x + 1) * 127, v[5] = (nn.y + 1) * 127, v[6] = (nn.z + 1) * 127, v[7] = 127; // no wind blend
        for (int j = 8; j < 20; j++) v[j] = 1;
        v[20] = v[21] = v[22] = 0, v[23] = I / 3.f;
        N[i] = {v[4] * 0.00787401572f - 1, v[5] * 0.00787401572f - 1, v[6] * 0.00787401572f - 1};
    }
    const DWORD fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX3 | D3DFVF_TEXCOORDSIZE3(0) | D3DFVF_TEXCOORDSIZE4(1) | D3DFVF_TEXCOORDSIZE3(2);
    IDirect3DVertexBuffer9 *src = nullptr, *outP = nullptr, *outO = nullptr;
    dv->CreateVertexBuffer(n * 96, 0, 0, D3DPOOL_SYSTEMMEM, &src, nullptr);
    dv->CreateVertexBuffer(n * 60, 0, fvf, D3DPOOL_SYSTEMMEM, &outP, nullptr);
    dv->CreateVertexBuffer(n * 60, 0, fvf, D3DPOOL_SYSTEMMEM, &outO, nullptr);
    void* pv = nullptr;
    src->Lock(0, 0, &pv, 0);
    std::memcpy(pv, vb.data(), n * 96);
    src->Unlock();
    dv->SetVertexDeclaration(di);
    dv->SetVertexShaderConstantF(0, C.data(), 256);
    dv->SetStreamSource(0, src, 0, 96);
    dv->SetVertexShader(vsP);
    Check(SUCCEEDED(dv->ProcessVertices(0, 0, n, outP, nullptr, 0)), "the patched shader runs");
    dv->SetVertexShader(vsO);
    Check(SUCCEEDED(dv->ProcessVertices(0, 0, n, outO, nullptr, 0)), "the original shader runs");
    void *a = nullptr, *b = nullptr;
    outP->Lock(0, 0, &a, 0);
    outO->Lock(0, 0, &b, 0);
    float maxErr = 0, maxRef = 0;
    bool samePos = true, sameRest = true, capped = false;
    for (int i = 0; i < n; i++) {
        const float* p = reinterpret_cast<const float*>(static_cast<const char*>(a) + i * 60);
        const float* o = reinterpret_cast<const float*>(static_cast<const char*>(b) + i * 60);
        const V3 r = Ref(P[i], N[i], L, cn);
        const float e = std::max({std::fabs(p[12] - r.x), std::fabs(p[13] - r.y), std::fabs(p[14] - r.z)}) / std::max(1e-3f, std::max({r.x, r.y, r.z}));
        maxErr = std::max(maxErr, e);
        maxRef = std::max(maxRef, r.x);
        for (int k = 0; k < 4; k++) samePos = samePos && p[k] == o[k];
        // oD0 (dword) and oT0 / oT1 are unchanged; oT2 is the lamp term
        sameRest = sameRest && std::memcmp(p + 4, o + 4, 4) == 0 && std::memcmp(p + 5, o + 5, 7 * 4) == 0;
        for (int k = 0; k < 3; k++) capped = capped || LawNoGain(Dot(L[k].p - P[i], L[k].p - P[i]), L[k].w, kMinD2) * L[k].scale > L[k].cap;
    }
    outP->Unlock();
    outO->Unlock();
    std::printf("Per-vertex foliage lamps: constants c%d.., %d vertices, max relative error %.2e (largest reference %.3f)\n", B, n, maxErr, maxRef);
    Check(maxErr < 1e-3f, "lamp light matches the reference (law, Lambert, transmission, crown, ground bounce, cap)");
    Check(capped, "the cap is exercised (some vertices are brighter than the colour's point)");
    Check(samePos, "the position is unchanged");
    Check(sameRest, "the colour and texture coordinate outputs other than the lamp term are unchanged");

    // A neutral instance (lamps that could not be confirmed: lot_light_bridge FoliageVertexConstants) gives the old light:
    // instance 2 (a0.w = 6), its lamps 1 km away along the slot directions, no crown, no bounce, no transmission
    {
        const int J = 6;
        set(J + 1, 1, 0, 0, 0);
        set(J + 2, 1, 0, 0, 0);
        const V3 dirs[3] = {{0.6f, 0.3f, 0.74f}, {-0.2f, 0.95f, 0.24f}, {0.1f, -0.4f, -0.91f}};
        for (int k = 0; k < 3; k++) {
            const float dl = std::sqrt(Dot(dirs[k], dirs[k]));
            set(27 + J + k, dirs[k].x, dirs[k].y, dirs[k].z, 0);
            set(54 + J + k, 0.3f + 0.2f * k, 0.5f, 0.2f, 0);
            set(B + J + k, dirs[k].x / dl * 1000.0f, dirs[k].y / dl * 1000.0f, dirs[k].z / dl * 1000.0f, 1e12f);
            set(B + 27 + J + k, 0, 1e6f * dl, kMinD2, 0);
        }
        set(B + 54 + J, 0, 0, 0, 1);
        set(B + 55 + J, 1, 0, 0, 0);
        set(B + 56 + J, 1e30f, 1e30f, 1e30f, 0);
        for (int i = 0; i < n; i++) vb[i * 24 + 23] = J / 3.f;
        src->Lock(0, 0, &pv, 0);
        std::memcpy(pv, vb.data(), n * 96);
        src->Unlock();
        dv->SetVertexShaderConstantF(0, C.data(), 256);
        dv->SetVertexShader(vsP);
        dv->ProcessVertices(0, 0, n, outP, nullptr, 0);
        dv->SetVertexShader(vsO);
        dv->ProcessVertices(0, 0, n, outO, nullptr, 0);
        outP->Lock(0, 0, &a, 0);
        outO->Lock(0, 0, &b, 0);
        float worst = 0, peak = 0;
        for (int i = 0; i < n; i++) {
            const float* p = reinterpret_cast<const float*>(static_cast<const char*>(a) + i * 60);
            const float* o = reinterpret_cast<const float*>(static_cast<const char*>(b) + i * 60);
            for (int k = 12; k < 15; k++) worst = std::max(worst, std::fabs(p[k] - o[k])), peak = std::max(peak, std::fabs(o[k]));
        }
        outP->Unlock();
        outO->Unlock();
        worst /= std::max(peak, 1e-6f);
        std::printf("Neutral instance: worst difference from the old shader %.2e of its brightest light\n", worst);
        Check(worst < 0.01f, "a neutral instance draws the old per-instance light (within 1%)");
    }
    std::printf("Foliage vertex lamps: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
