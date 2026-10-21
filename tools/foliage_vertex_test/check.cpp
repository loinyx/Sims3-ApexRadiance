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
        // the leaves between the vertex and the lamp only: max(0, min(exit, d) - min(entry, d))
        const float dist = std::sqrt(d2), exitT = std::max((-b + sq) / a, 0.f), entryT = std::max((-b - sq) / a, 0.f);
        const float t = std::max(std::min(exitT, dist) - std::min(entryT, dist), 0.f), tr = std::exp(-t * cn.kappa);
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

// Any foliage variant: patched with neutral lamps (1 km along the slot directions, no crown, no bounce, no transmission)
// gives every output of the original shader, the lamp terms within 1% of the brightest. v5: the instance index input.
static bool NeutralMatches(IDirect3DDevice9* dv, const std::vector<uint32_t>& original, const char* name, const float v5[4]) {
    std::vector<uint32_t> patched = original;
    int B = -1;
    if (!FoliageVertexLamps::Patch(patched, B)) {
        std::printf("FAIL: %s not patched\n", name);
        return false;
    }
    IDirect3DVertexShader9 *vsP = nullptr, *vsO = nullptr;
    if (FAILED(dv->CreateVertexShader(reinterpret_cast<const DWORD*>(patched.data()), &vsP)) ||
        FAILED(dv->CreateVertexShader(reinterpret_cast<const DWORD*>(original.data()), &vsO))) {
        std::printf("FAIL: %s patched shader rejected by D3D9\n", name);
        return false;
    }
    std::vector<float> C(256 * 4, 0.f);
    const auto set = [&](int r, float x, float y, float z, float ww) { C[r * 4] = x, C[r * 4 + 1] = y, C[r * 4 + 2] = z, C[r * 4 + 3] = ww; };
    const int I = 3; // instance 1
    set(I + 1, 1, 0, 0, 1);
    set(I + 2, 1, 0.5f, 0, 0);
    for (int r = 100; r < 130; r++) set(r, r % 4 == 1, r % 4 == 2, r % 4 == 3, r % 4 == 0); // view rows, sun, fog: anything fixed
    set(255, 1, 0, 0, 0);
    const V3 dirs[3] = {{0.6f, 0.3f, 0.74f}, {-0.2f, 0.95f, 0.24f}, {0.1f, -0.4f, -0.91f}};
    for (int k = 0; k < 3; k++) {
        const float dl = std::sqrt(Dot(dirs[k], dirs[k]));
        set(27 + I + k, dirs[k].x, dirs[k].y, dirs[k].z, 0);
        set(54 + I + k, 0.3f + 0.2f * k, 0.5f, 0.2f, 0);
        set(B + I + k, dirs[k].x / dl * 1000.0f, dirs[k].y / dl * 1000.0f, dirs[k].z / dl * 1000.0f, 1e12f);
        set(B + 27 + I + k, 0, 1e6f * dl, kMinD2, 0);
    }
    set(B + 54 + I, 0, 0, 0, 1);
    set(B + 55 + I, 1, 0, 0, 0);
    set(B + 56 + I, 1e30f, 1e30f, 1e30f, 0);
    const D3DVERTEXELEMENT9 in[] = {{0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITION, 0}, {0, 16, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_NORMAL, 0},
                                    {0, 32, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 0}, {0, 48, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 1},
                                    {0, 64, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 2}, {0, 80, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 3},
                                    D3DDECL_END()};
    IDirect3DVertexDeclaration9* di = nullptr;
    dv->CreateVertexDeclaration(in, &di);
    const int n = 1000;
    std::vector<float> vb(n * 24);
    srand(11);
    const auto rnd = [] { return rand() / static_cast<float>(RAND_MAX); };
    for (int i = 0; i < n; i++) {
        float* v = &vb[i * 24];
        v[0] = (rnd() * 2 - 1) * 1.4f, v[1] = 0.3f + rnd() * 3.4f, v[2] = (rnd() * 2 - 1) * 1.4f, v[3] = 1;
        for (int k = 4; k < 7; k++) v[k] = rnd() * 254;
        v[7] = 127;
        for (int j = 8; j < 20; j++) v[j] = 1;
        for (int j = 0; j < 4; j++) v[20 + j] = v5[j];
    }
    // every output: position, two colours, five 4-component texture coordinates
    const DWORD fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX5 | D3DFVF_TEXCOORDSIZE4(0) | D3DFVF_TEXCOORDSIZE4(1) |
                      D3DFVF_TEXCOORDSIZE4(2) | D3DFVF_TEXCOORDSIZE4(3) | D3DFVF_TEXCOORDSIZE4(4);
    const UINT stride = 16 + 4 + 4 + 5 * 16;
    IDirect3DVertexBuffer9 *src = nullptr, *outP = nullptr, *outO = nullptr;
    dv->CreateVertexBuffer(n * 96, 0, 0, D3DPOOL_SYSTEMMEM, &src, nullptr);
    dv->CreateVertexBuffer(n * stride, 0, fvf, D3DPOOL_SYSTEMMEM, &outP, nullptr);
    dv->CreateVertexBuffer(n * stride, 0, fvf, D3DPOOL_SYSTEMMEM, &outO, nullptr);
    void* pv = nullptr;
    src->Lock(0, 0, &pv, 0);
    std::memcpy(pv, vb.data(), n * 96);
    src->Unlock();
    dv->SetVertexDeclaration(di);
    dv->SetVertexShaderConstantF(0, C.data(), 256);
    dv->SetStreamSource(0, src, 0, 96);
    dv->SetVertexShader(vsP);
    const bool okP = SUCCEEDED(dv->ProcessVertices(0, 0, n, outP, nullptr, 0));
    dv->SetVertexShader(vsO);
    const bool okO = SUCCEEDED(dv->ProcessVertices(0, 0, n, outO, nullptr, 0));
    void *a = nullptr, *b = nullptr;
    outP->Lock(0, 0, &a, 0);
    outO->Lock(0, 0, &b, 0);
    float worst = 0, peak = 0;
    bool samePos = true;
    for (int i = 0; i < n; i++) {
        const float* p = reinterpret_cast<const float*>(static_cast<const char*>(a) + i * stride);
        const float* o = reinterpret_cast<const float*>(static_cast<const char*>(b) + i * stride);
        for (int k = 0; k < 4; k++) samePos = samePos && p[k] == o[k];
        for (int k = 6; k < 26; k++) worst = std::max(worst, std::fabs(p[k] - o[k])), peak = std::max(peak, std::fabs(o[k]));
    }
    outP->Unlock();
    outO->Unlock();
    worst /= std::max(peak, 1e-6f);
    std::printf("%s: patched (constants c%d..), neutral instance worst difference %.2e of the largest output\n", name, B, worst);
    return okP && okO && samePos && worst < 0.01f;
}

struct OwnLampOut {
    float r, g, b;
};
// A picked lamp's constants (FoliageVertexLamps::SlotConstants) as the patched shader reads them: per channel, colour x
// min(law(d) x Q.y, cap)
static OwnLampOut ShaderLight(const FoliageVertexLamps::OwnLamp& l, float w, V3 p) {
    float lp[4], law[4], cap;
    const float eye[3] = {0, 0, 0};
    FoliageVertexLamps::SlotConstants(l, eye, 0.0f, 1.0f, lp, law, cap);
    const V3 d{lp[0] - p.x, lp[1] - p.y, lp[2] - p.z};
    const float d2 = std::max(Dot(d, d), kMinD2), q = d2 * law[0], win = std::clamp(1 - q * q, 0.f, 1.f); // the shader's law with its own r0^2 (lp.w)
    const float ratio = std::min(1 / d2 * std::min(1.f, lp[3] / d2 * win * win) * law[1], cap);
    (void)w;
    return {l.colour[0] * ratio, l.colour[1] * ratio, l.colour[2] * ratio};
}
static FoliageVertexLamps::OwnLamp MakeLamp(V3 p, V3 col, float w, float entries) {
    FoliageVertexLamps::OwnLamp l;
    float r0, R;
    LampReach::Radii(w, r0, R);
    l.pos[0] = p.x, l.pos[1] = p.y, l.pos[2] = p.z, l.colour[0] = col.x, l.colour[1] = col.y, l.colour[2] = col.z;
    l.r0sq = R * R, l.invR2 = 1 / (R * R), l.gain = LampReach::Gain(w, entries); // (foliage: r0 = R, FoliageLaw)
    return l;
}

// The instance's own lamps. The case of a lighting snapshot: a red garden lantern (type 3, range 15, intensity 1:
// w 15, a story-0 lot lamp, 2 entries; r0 2 m, R 6.3 m) 5 cm above the snow, 13.7 m from the trunk of a bare tree whose
// crown (8 m across each way, 5 m up and down, centre 10 m up) the old rig colour lit whole and saturated.
static void OwnLampChecks() {
    const float w = 15.0f, entries = 2.0f;
    const auto red = MakeLamp({0, 0.05f, 0}, {1, 0, 0}, w, entries);
    srand(7);
    const auto rnd = [] { return rand() / static_cast<float>(RAND_MAX); };
    float brightest = 0.0f, worst = 0.0f;
    for (int i = 0; i < 20000; i++) {
        float x, y, z;
        do x = rnd() * 2 - 1, y = rnd() * 2 - 1, z = rnd() * 2 - 1; while (x * x + y * y + z * z > 1);
        const V3 p{9.7f + 8 * x, 10 + 5 * y, -9.7f + 8 * z};
        const OwnLampOut o = ShaderLight(red, w, p);
        brightest = std::max(brightest, o.r);
        const float d2 = Dot(p - V3{0, 0.05f, 0}, p - V3{0, 0.05f, 0}), law = std::min(FoliageVertexLamps::FoliageLaw(d2, w, entries, kMinD2), 1.0f);
        worst = std::max(worst, std::fabs(o.r - law) / std::max(law, 1e-6f));
    }
    std::printf("Red lantern on a bare crown: brightest crown light %.5f (was the rig colour, 1, on every vertex)\n", brightest);
    Check(brightest <= 1.0f / 255.0f && worst < 1e-4f, "a 6.3 m lantern 13.7 m from the trunk leaves the crown unlit (the foliage law)");
    // a street lamp (world lamp, range 42.5, intensity 1: one list entry; r0 2.5 m, R 10.6 m) 5 m from the near side of a
    // crown (a lighting snapshot: the pavement under it lit, the trees beside it dark): the inverse square, not 1 / d^4
    {
        const float ws = 42.5f, d2 = 25.0f;
        const auto street = MakeLamp({0, 4, 0}, {1, 0.92f, 0.63f}, ws, 1.0f);
        const float lit = ShaderLight(street, ws, {5, 4, 0}).r, walls = LampReach::PixelLaw(d2, ws, 1.0f);
        std::printf("Street lamp on a crown 5 m away: %.3f (the walls' law %.3f); 9 m: %.3f; 11 m: %.4f\n", lit, walls, ShaderLight(street, ws, {9, 4, 0}).r,
                    ShaderLight(street, ws, {11, 4, 0}).r);
        Check(lit > 0.3f && lit > 3.0f * walls, "a street lamp lights a crown 5 m away by the inverse square (visible)");
        Check(ShaderLight(street, ws, {11, 4, 0}).r == 0.0f, "nothing past the lamp's reach (no whole-crown glow)");
    }
    // a warm garden post (F0 x intensity 3.4) next to a leaf: no channel above 1, the hue kept
    const auto warm = MakeLamp({0, 1, 0}, {3.4f, 3.4f * 0.81f, 3.4f * 0.56f}, 42.5f * 3.4f, 2.0f);
    const OwnLampOut n = ShaderLight(warm, 42.5f * 3.4f, {0.3f, 1.2f, 0});
    Check(std::fabs(n.r - 1) < 1e-5f && std::fabs(n.g - 0.81f) < 1e-5f && std::fabs(n.b - 0.56f) < 1e-5f, "a bright lamp next to a leaf: capped at 1, the hue kept (no whitening)");
    const OwnLampOut f = ShaderLight(warm, 42.5f * 3.4f, {9, 1, 0});
    Check(f.r < 1 && std::fabs(f.g / f.r - 0.81f) < 1e-4f && std::fabs(f.b / f.r - 0.56f) < 1e-4f, "farther from it: dimmer, the same hue");
    // the three brightest, brightest first (the ground bounce takes slot 0)
    FoliageVertexLamps::OwnLamp out[3];
    int kept = 0;
    for (float s : {0.2f, 0.9f, 0.0f, 0.1f, 0.5f, 0.3f}) {
        FoliageVertexLamps::OwnLamp l;
        l.score = s;
        kept = FoliageVertexLamps::KeepBrightest(l, out, kept);
    }
    Check(kept == 3 && out[0].score == 0.9f && out[1].score == 0.5f && out[2].score == 0.3f, "the three brightest lamps kept, brightest first; a lamp without light never");
    const float c[3] = {0, 5, 0}, e[3] = {2, 5, 2}, in[3] = {1, 3, -1}, outP[3] = {5, 0, 0};
    Check(FoliageVertexLamps::BoxDistance2(in, c, e) == 0.0f && std::fabs(FoliageVertexLamps::BoxDistance2(outP, c, e) - 9.0f) < 1e-5f, "distance to the mesh box");
}

// The eye correction (in game: lamp light on trees moved with the camera and crowns blinked): places learned with
// the camera still; with the camera moving, the read eye ahead of the game's by a drift; the draw's instances (relative to
// the game's eye) must give back the game's eye
static void EyeAnchorChecks() {
    FoliageVertexLamps::EyeAnchors a;
    const float trees[4][3] = {{440.0f, 17.5f, 525.0f}, {452.3f, 17.6f, 531.8f}, {447.1f, 17.4f, 540.2f}, {449.0f, 17.5f, 527.5f}};
    const float scales[4] = {1.2f, 1.0f, 0.9f, 1.1f};
    for (int i = 0; i < 4; i++) a.Learn(7, scales[i], trees[i]);
    a.Learn(7, scales[0], trees[0]); // seen again: kept once
    Check(a.Count() == 4, "a place learned twice is kept once");
    const float gameEye[3] = {470.3f, 40.2f, 560.9f}, drift[3] = {0.8f, -0.1f, -0.6f};
    const float readEye[3] = {gameEye[0] + drift[0], gameEye[1] + drift[1], gameEye[2] + drift[2]};
    float rel[4][3];
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++) rel[i][k] = trees[i][k] - gameEye[k];
    float o[3] = {};
    const bool ok = a.Offset(7, scales, rel, 4, readEye, o);
    float err = 0.0f;
    for (int k = 0; k < 3; k++) err = std::max(err, std::fabs(readEye[k] + o[k] - gameEye[k]));
    Check(ok && err < 1e-3f, "a moving camera: the game's eye comes back from the learned places");
    Check(!a.Offset(8, scales, rel, 4, readEye, o), "another mesh never matches");
    float away[1][3] = {{rel[0][0] + 9.0f, rel[0][1], rel[0][2]}};
    Check(!a.Offset(7, scales, away, 1, readEye, o), "a place farther than the largest drift is not matched");
    // two instances that disagree (a moved object): no offset
    float bad[2][3] = {{rel[0][0], rel[0][1], rel[0][2]}, {rel[1][0] + 1.0f, rel[1][1], rel[1][2]}};
    const float s2[2] = {scales[0], scales[1]};
    Check(!a.Offset(7, s2, bad, 2, readEye, o), "instances that disagree give no offset");
}

int main(int argc, char** argv) {
    OwnLampChecks();
    EyeAnchorChecks();
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
    // the last 4: outside the crown, lamp 2 (0, 1, -6) between them and the crown (its entry beyond the lamp): no leaf on
    // the way, the light must not be attenuated (the 2.14.0 review: min(exit - entry, d) took 1.5 m of leaves there)
    const V3 beyondLamp[4] = {{0, 1, -7.5f}, {0.2f, 1.1f, -8}, {-0.2f, 0.9f, -7}, {0, 1.2f, -6.6f}};
    for (int i = 0; i < n; i++) {
        P[i] = {(rnd() * 2 - 1) * 1.4f, 0.3f + rnd() * 3.4f, (rnd() * 2 - 1) * 1.4f};
        if (i >= n - 4) P[i] = beyondLamp[i - (n - 4)];
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
    float maxErr = 0, maxRef = 0, beyondErr = 0;
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
        if (i >= n - 4) beyondErr = std::max(beyondErr, e);
    }
    outP->Unlock();
    outO->Unlock();
    {   // the case is exercised: the old path min(exit - entry, d) would have crossed leaves there (the reference crosses none)
        const V3 s{1 / cn.rh, 1 / cn.rv, 1 / cn.rh};
        float oldPath = 0;
        for (int i = n - 4; i < n; i++) {
            const V3 d = L[2].p - P[i], p{(P[i].x - cn.c.x) * s.x, (P[i].y - cn.c.y) * s.y, (P[i].z - cn.c.z) * s.z};
            const float dist = std::sqrt(Dot(d, d));
            const V3 q{d.x / dist * s.x, d.y / dist * s.y, d.z / dist * s.z};
            const float qa = Dot(q, q), qb = Dot(p, q), sq = std::sqrt(std::max(qb * qb - qa * (Dot(p, p) - 1), 0.f));
            oldPath = std::max(oldPath, std::min(std::max((-qb + sq) / qa, 0.f) - std::max((-qb - sq) / qa, 0.f), dist));
        }
        std::printf("Lamp between a vertex outside the crown and the crown: the old path crossed %.2f m of leaves, now none; shader error %.2e\n", oldPath, beyondErr);
        Check(oldPath > 0.5f && beyondErr < 1e-3f, "a lamp between the vertex and the crown: no leaf on the way, as the reference");
    }
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
        for (int i = 0; i < n - 4; i++) { // (not the 4 placed 7 to 8 m out: a lamp 1 km away changes by 1.6% over 8 m)
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
    {
        const float v5Canopy[4] = {0, 0, 0, 1}; // a0.w = 3 v5.w
        Check(NeutralMatches(dv, original, "Canopy variant", v5Canopy), "canopy variant: a neutral instance gives every output of the original");
    }
    if (argc > 2) { // the trunk and branch variant ("tree 2"): a0.x = v5.z, a0.z = v5.w, a0.w = 3 v5.y
        std::ifstream g(argv[2], std::ios::binary);
        std::vector<char> rawTrunk((std::istreambuf_iterator<char>(g)), {});
        std::vector<uint32_t> trunk(rawTrunk.size() / 4);
        std::memcpy(trunk.data(), rawTrunk.data(), trunk.size() * 4);
        const float v5Trunk[4] = {0, 1, 3, 3};
        Check(trunk.size() > 2 && NeutralMatches(dv, trunk, "Trunk variant", v5Trunk), "trunk variant: patched, loads, and a neutral instance gives every output of the original");
    }
    if (argc > 3) { // the sun's shadow on trees over every pixel shader of the game's Shaders_Win32.precomp
        std::ifstream g(argv[3], std::ios::binary);
        std::vector<unsigned char> d((std::istreambuf_iterator<char>(g)), {});
        int shaders = 0, treePatched = 0, loaded = 0, treeLike = 0, originalsLoaded = 0, alreadyShadowed = 0;
        for (size_t i = 0; i + 8 <= d.size(); i++) {
            uint32_t v;
            std::memcpy(&v, &d[i], 4);
            if (v != 0xFFFF0200u && v != 0xFFFF0300u) continue;
            size_t j = i + 4;
            bool ok = false;
            while (j + 4 <= d.size() && j - i < 65536) {
                uint32_t tok;
                std::memcpy(&tok, &d[j], 4);
                if (tok == 0x0000FFFFu) { ok = true; j += 4; break; }
                if ((tok & 0xFFFF) == 0xFFFE) j += 4 + 4 * ((tok >> 16) & 0x7FFF);
                else if (tok & 0x80000000u) break;
                else j += 4 + 4 * ((tok >> 24) & 0x0F);
            }
            if (!ok) continue;
            shaders++;
            std::vector<uint32_t> ps((j - i) / 4);
            std::memcpy(ps.data(), &d[i], j - i);
            // a tree shader: reads t1.wzyx with _sat into .xy (the lamp 0 weight and the sun's N.l)
            bool tree = false;
            for (size_t k = 1; k + 2 < ps.size() && !tree; k++)
                tree = (ps[k] & 0xFFFF) == 1 && (ps[k + 1] & 0x00100000u) && ((ps[k + 1] >> 16) & 0xF) == 0x3 && (ps[k + 2] & 0x7FF) == 1 &&
                       ((ps[k + 2] >> 28) & 7) == 3 && ((ps[k + 2] >> 16) & 0xFF) == 0x1B; // t1 (texture register type 3)
            treeLike += tree ? 1 : 0;
            if (tree) { // the game's own shader must load on this device, or the check below says nothing
                IDirect3DPixelShader9* s = nullptr;
                if (SUCCEEDED(dv->CreatePixelShader(reinterpret_cast<const DWORD*>(ps.data()), &s)) && s) originalsLoaded++, s->Release();
            }
            int base = -1;
            if (!FoliageVertexLamps::PatchTreeSunShadow(ps, base)) {
                if (tree && std::strcmp(FoliageVertexLamps::LastRefusal(), "t5, t6 or s5 already declared") == 0) alreadyShadowed++;
                else if (tree) std::printf("tree shader at offset %zu (%zu bytes) refused: %s\n", i, j - i, FoliageVertexLamps::LastRefusal());
                i = j - 1;
                continue;
            }
            treePatched++;
            IDirect3DPixelShader9* s = nullptr;
            if (SUCCEEDED(dv->CreatePixelShader(reinterpret_cast<const DWORD*>(ps.data()), &s)) && s) loaded++, s->Release();
            else std::printf("FAIL: patched tree shader at offset %zu rejected by D3D9\n", i);
            i = j - 1;
        }
        std::printf("Tree sun shadow: %d pixel shaders in the precomp, %d of the tree family, %d patched, %d load in D3D9 (the game's own: %d of %d load)\n", shaders, treeLike, treePatched, loaded,
                    originalsLoaded, treeLike);
        std::printf("Tree sun shadow: %d tree shaders already read the shadow map (left as they are)\n", alreadyShadowed);
        Check(treePatched + alreadyShadowed == treeLike && treePatched >= 8, "every tree pixel shader of the game without a shadow is patched (and nothing else)");
        Check(loaded == treePatched, "every patched tree pixel shader loads in D3D9");
    }
    std::printf("Foliage vertex lamps: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
