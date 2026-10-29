// Terrain lamp stamp, offline (read-only: prints to stdout). Runs the game's own terrain bake shaders (technique
// StaticTerrainLightmap, read from the Shaders_Win32.precomp given on the command line; nothing of the game is stored
// here) next to StampPS (shaders/lightmap_smooth_ps.hlsl, the copy the mod compiles) and a CPU reading of the law:
//   T0  the game's pixel shader against the CPU law, sloped normal maps and heights (the reading of the bytecode)
//   T1  StampPS against the game's shader on flat ground at the lamp's ground height
//   T2  8 lamps: StampPS's one pass against the game's 8 additive passes into an 8-bit target
//   T3  a chunk border: the P grid's 4 texels into the neighbour equal the neighbour's own texels, bit for bit
//   T4  the base kept: rgb = min(base + stamps, 1), alpha untouched
//   T5  GPU time of StampPS over a 264 x 264 P grid with 8 lamps
//   T6  the game's bake pixel shader is the one the mod recognises (size and FNV-1a, features/terrain_bake_cone.h)
//   T7  BakeConePS (shaders/terrain_bake_ps.hlsl) without cones against the game's shader, sloped ground: the same law
//   T8  BakeConePS with spot and shaded-lamp cones against the CPU law times TerrainBakeCone::Factor, sloped ground
//   T9  StampPS with the same cones against BakeConePS on flat ground
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include "shaders/lightmap_smooth_hlsl.h"
#include "shaders/terrain_bake_hlsl.h"
#include "features/terrain_bake_cone.h"

namespace {
int checks = 0, failures = 0;
void Check(bool ok, const std::string& text) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", text.c_str());
    }
}

// ---------------------------------------------------------------- the game's shaders (Shaders_Win32.precomp)
// SPKG, SHDB size, LINK count, then per blob a few header bytes, the tag VSHD / PSHD, its size and the bytecode; techniques
// "TECH" + FNV-1 name hash + pass count, per pass "PASS" + 2 dwords + VS and PS blob indices (1-based)
bool ReadTechnique(const char* path, uint32_t hash, std::vector<uint8_t>& vs, std::vector<uint8_t>& ps, size_t& blobCount) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto u32 = [&](size_t o) { uint32_t v = 0; std::memcpy(&v, d.data() + o, 4); return v; };
    if (d.size() < 32 || std::memcmp(d.data(), "SPKG", 4) != 0 || std::memcmp(d.data() + 8, "SHDB", 4) != 0) return false;
    const size_t end = 16 + u32(12);
    std::vector<std::pair<size_t, size_t>> blobs; // body offset, size
    size_t p = 16;
    while (p + 8 < end && p + 8 < d.size()) {
        size_t at = SIZE_MAX;
        for (size_t k = p; k <= p + 16 && k + 8 < d.size(); k++)
            if (std::memcmp(d.data() + k, "VSHD", 4) == 0 || std::memcmp(d.data() + k, "PSHD", 4) == 0) {
                at = k;
                break;
            }
        if (at == SIZE_MAX) break;
        const size_t size = u32(at + 4);
        blobs.emplace_back(at + 8, size);
        p = at + 8 + size;
    }
    blobCount = blobs.size();
    uint8_t pat[8] = {'T', 'E', 'C', 'H'};
    std::memcpy(pat + 4, &hash, 4);
    const auto t = std::search(d.begin(), d.end(), pat, pat + 8);
    if (t == d.end()) return false;
    const size_t tp = static_cast<size_t>(t - d.begin());
    const uint8_t passTag[4] = {'P', 'A', 'S', 'S'};
    const auto pp = std::search(d.begin() + tp, d.end(), passTag, passTag + 4);
    if (pp == d.end()) return false;
    const size_t po = static_cast<size_t>(pp - d.begin());
    const uint32_t vi = u32(po + 12), pi = u32(po + 16);
    const auto take = [&](uint32_t idx, std::vector<uint8_t>& out) {
        if (!idx || idx > blobs.size()) return false;
        const auto [off, size] = blobs[idx - 1];
        out.assign(d.begin() + off, d.begin() + off + size);
        for (size_t k = 0; k + 4 <= out.size(); k += 4) // up to the end token
            if (uint32_t v; std::memcpy(&v, out.data() + k, 4), v == 0x0000FFFF) {
                out.resize(k + 4);
                break;
            }
        return true;
    };
    return take(vi, vs) && take(pi, ps);
}

// ---------------------------------------------------------------- the device
IDirect3DDevice9* g_dev = nullptr;
bool MakeDevice() {
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ApexTerrainStampTest";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA(wc.lpszClassName, "terrain stamp test", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d || !wnd) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 64, pp.BackBufferHeight = 64;
    pp.hDeviceWindow = wnd;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &g_dev);
    if (FAILED(hr)) hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &g_dev);
    D3DADAPTER_IDENTIFIER9 id{};
    d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
    std::printf("device: %s | adapter %s\n", SUCCEEDED(hr) ? "HAL" : "none", id.Description);
    d3d->Release();
    return SUCCEEDED(hr) && g_dev;
}

IDirect3DTexture9* MakeTex(int w, int h, D3DFORMAT f, DWORD usage, D3DPOOL pool) {
    IDirect3DTexture9* t = nullptr;
    return SUCCEEDED(g_dev->CreateTexture(w, h, 1, usage, f, pool, &t, nullptr)) ? t : nullptr;
}
// A8R8G8B8 bytes (r, g, b, a per texel) into a managed texture
void FillBytes(IDirect3DTexture9* t, int w, int h, const std::vector<uint8_t>& rgba) {
    D3DLOCKED_RECT lr{};
    t->LockRect(0, &lr, nullptr, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t* s = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            static_cast<uint32_t*>(static_cast<void*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch))[x] =
                static_cast<uint32_t>(s[3]) << 24 | static_cast<uint32_t>(s[0]) << 16 | static_cast<uint32_t>(s[1]) << 8 | s[2];
        }
    t->UnlockRect(0);
}
// Float RGBA into a managed A32B32G32R32F texture
void FillFloat(IDirect3DTexture9* t, int w, int h, const std::vector<float>& rgba) {
    D3DLOCKED_RECT lr{};
    t->LockRect(0, &lr, nullptr, 0);
    for (int y = 0; y < h; y++) std::memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, &rgba[static_cast<size_t>(y) * w * 4], w * 16);
    t->UnlockRect(0);
}
// Render target read back as float RGBA (A32B32G32R32F) or bytes (A8R8G8B8, as r, g, b, a)
std::vector<float> ReadFloat(IDirect3DTexture9* rt, int w, int h) {
    std::vector<float> out(static_cast<size_t>(w) * h * 4);
    IDirect3DSurface9 *s = nullptr, *sys = nullptr;
    rt->GetSurfaceLevel(0, &s);
    g_dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (s && sys && SUCCEEDED(g_dev->GetRenderTargetData(s, sys))) {
        D3DLOCKED_RECT lr{};
        sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
        for (int y = 0; y < h; y++) std::memcpy(&out[static_cast<size_t>(y) * w * 4], static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, w * 16);
        sys->UnlockRect();
    }
    if (sys) sys->Release();
    if (s) s->Release();
    return out;
}
std::vector<uint8_t> ReadBytes(IDirect3DTexture9* rt, int w, int h) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    IDirect3DSurface9 *s = nullptr, *sys = nullptr;
    rt->GetSurfaceLevel(0, &s);
    g_dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (s && sys && SUCCEEDED(g_dev->GetRenderTargetData(s, sys))) {
        D3DLOCKED_RECT lr{};
        sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint32_t v = static_cast<const uint32_t*>(static_cast<const void*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch))[x];
                uint8_t* o = &out[(static_cast<size_t>(y) * w + x) * 4];
                o[0] = v >> 16 & 255, o[1] = v >> 8 & 255, o[2] = v & 255, o[3] = v >> 24;
            }
        sys->UnlockRect();
    }
    if (sys) sys->Release();
    if (s) s->Release();
    return out;
}

void Target(IDirect3DTexture9* rt) {
    IDirect3DSurface9* s = nullptr;
    rt->GetSurfaceLevel(0, &s);
    g_dev->SetRenderTarget(0, s);
    s->Release();
}
void CommonState() {
    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    for (int s = 0; s < 2; s++) {
        g_dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        g_dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        g_dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        g_dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        g_dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        g_dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    }
}

// ---------------------------------------------------------------- the game's bake pass and StampPS
IDirect3DVertexShader9* g_gameVs = nullptr;
IDirect3DPixelShader9 *g_gamePs = nullptr, *g_stampPs = nullptr, *g_conePs = nullptr;
IDirect3DVertexDeclaration9* g_decl = nullptr;

struct Lamp {
    float col[3], w; // colour, range x intensity x 0.2
    float pos[3];
    float cone[3][4] = {{0, 0, 0, 1}, {-1, 0, 0, 0}, {0, 0, 0, TerrainBakeCone::kMagic}}; // no cone
};
constexpr float kHeightScale = 0.01f;       // PS c2.x: height = (255 g + b) x scale (bytes)
constexpr float kNmZero = 2.0f * 128 / 255; // (alpha, green) byte 128: 2 x 128 / 255 - 1.0039 ~ 0

// One game pass: chunk rect (x0, z0, size) on a w x h target; s0 normal map, s1 height map
void GamePass(int w, int h, float x0, float z0, float size, const Lamp& l, IDirect3DPixelShader9* ps = nullptr) {
    g_dev->SetVertexShader(g_gameVs);
    g_dev->SetPixelShader(ps ? ps : g_gamePs);
    g_dev->SetPixelShaderConstantF(5, &l.cone[0][0], 3); // (BakeConePS; the game's shader reads none of them)
    g_dev->SetVertexDeclaration(g_decl);
    const float vc[12] = {0.5f, -0.5f, 0.5f, 0.5f, 1, 1, -1.0f / w, 1.0f / h, x0, z0, size, size};
    g_dev->SetVertexShaderConstantF(0, vc, 3);
    const float pc[12] = {l.col[0], l.col[1], l.col[2], l.w, l.pos[0], l.pos[1], l.pos[2], 0, kHeightScale, 0, 0, 0};
    g_dev->SetPixelShaderConstantF(0, pc, 3);
    const float v[16] = {-1, 1, 0, 1, 1, 1, 0, 1, -1, -1, 0, 1, 1, -1, 0, 1};
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, 16);
}
// StampPS over a w x h P grid whose texel (0, 0) is at world (ox, oz), lamps at ground height gy; s0 = base
void StampPass(int w, int h, float ox, float oz, float gy, const std::vector<Lamp>& lamps, IDirect3DTexture9* base) {
    g_dev->SetVertexShader(nullptr);
    g_dev->SetPixelShader(g_stampPs);
    g_dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    g_dev->SetTexture(0, base);
    const float in[4] = {1.0f / w, 1.0f / h, static_cast<float>(w), static_cast<float>(h)};
    g_dev->SetPixelShaderConstantF(0, in, 1);
    const float o[4] = {ox, oz, gy, static_cast<float>(lamps.size())};
    g_dev->SetPixelShaderConstantF(2, o, 1);
    float lc[160] = {};
    for (size_t k = 0; k < lamps.size() && k < 8; k++) {
        float* c = lc + k * 20;
        c[0] = lamps[k].col[0], c[1] = lamps[k].col[1], c[2] = lamps[k].col[2], c[3] = lamps[k].w;
        c[4] = lamps[k].pos[0], c[5] = lamps[k].pos[1], c[6] = lamps[k].pos[2];
        std::memcpy(c + 8, lamps[k].cone, 48);
    }
    g_dev->SetPixelShaderConstantF(4, lc, 40);
    struct V { float x, y, z, rhw, u, v; };
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    const V q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {fw - 0.5f, -0.5f, 0, 1, fw, 0}, {-0.5f, fh - 0.5f, 0, 1, 0, fh}, {fw - 0.5f, fh - 0.5f, 0, 1, fw, fh}};
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
}

// The law as read from the game's pixel shader, at one texel (normal map bytes a, g; height map bytes g, b)
void LawCpu(const Lamp& l, float wx, float wz, uint8_t nmA, uint8_t nmG, uint8_t hmG, uint8_t hmB, float out[3]) {
    const float nx = 2.0f * nmA / 255.0f - 1.00392163f, nz = 2.0f * nmG / 255.0f - 1.00392163f;
    const float ny = std::sqrt(std::max(0.0f, 1.0f - std::min(1.0f, nx * nx + nz * nz)));
    const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
    const float y = (hmG * 255.0f + hmB) * kHeightScale; // (g x 255 + b) x c2.x x 255, g and b in 0..1
    const float L[3] = {wx - l.pos[0], y - l.pos[1], wz - l.pos[2]};
    const float ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    const float c = ll > 0 && nl > 0 ? -(L[0] * nx + L[1] * ny + L[2] * nz) / (ll * nl) : 0.0f;
    const float d2 = L[0] * L[0] + L[2] * L[2] + 1e-4f;
    const float a = std::min(1.0f, l.w / d2) * (c >= 0 ? std::min(1.0f, std::sqrt(c)) : 0.0f);
    for (int k = 0; k < 3; k++) out[k] = l.col[k] * a;
}

// The law times the light's cone factor at the direction from the point (at its height) to the light
void LawConeCpu(const Lamp& l, float wx, float wz, uint8_t nmA, uint8_t nmG, uint8_t hmG, uint8_t hmB, float out[3]) {
    LawCpu(l, wx, wz, nmA, nmG, hmG, hmB, out);
    const float y = (hmG * 255.0f + hmB) * kHeightScale;
    float d[3] = {l.pos[0] - wx, l.pos[1] - y, l.pos[2] - wz};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (float& v : d) v /= len;
    float f[3];
    TerrainBakeCone::Factor(l.cone, d, f);
    for (int k = 0; k < 3; k++) out[k] *= f[k];
}
// A spot (type 4) or a shaded lamp (type 5) aimed roughly down, with the game's fields
void RandomCone(std::mt19937& rng, Lamp& l) {
    std::uniform_real_distribution<float> U(0, 1);
    float a[3] = {U(rng) - 0.5f, -0.5f - U(rng), U(rng) - 0.5f};
    const float n = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    for (float& v : a) v /= n;
    TerrainBakeCone::Fields f;
    if (U(rng) < 0.5f) {
        f.type = 4;
        for (int i = 0; i < 3; i++) f.axis4[i] = a[i];
        f.off4 = 0.5f + 0.4f * U(rng), f.scale4 = 2 + 6 * U(rng);
    } else {
        f.type = 5;
        for (int i = 0; i < 3; i++) f.a1[i] = a[i], f.a2[i] = -a[i], f.S[i] = 0.05f + 0.45f * U(rng);
        f.o1 = 0.2f + 0.6f * U(rng), f.o2 = 0.5f + 0.45f * U(rng);
    }
    TerrainBakeCone::Rows(f, 5.0f, l.cone);
}

Lamp RandomLamp(std::mt19937& rng, float x0, float z0, float gy) {
    std::uniform_real_distribution<float> U(0, 1);
    Lamp l{};
    l.pos[0] = x0 - 40 + 336 * U(rng), l.pos[2] = z0 - 40 + 336 * U(rng), l.pos[1] = gy + 0.3f + 8 * U(rng);
    const float range = 5 + 95 * U(rng), inten = 0.3f + 1.7f * U(rng);
    l.w = range * inten * 0.2f;
    for (float& c : l.col) c = 0.2f + 1.0f * U(rng);
    return l;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: terrain_stamp_test <Shaders_Win32.precomp>\n");
        return 2;
    }
    std::vector<uint8_t> vsCode, psCode;
    size_t blobs = 0;
    const bool read = ReadTechnique(argv[1], 0x86a5dfa4u, vsCode, psCode, blobs); // StaticTerrainLightmap
    std::printf("precomp: %zu shader blobs; StaticTerrainLightmap VS %zu bytes, PS %zu bytes\n", blobs, vsCode.size(), psCode.size());
    Check(read && blobs == 15689 && !vsCode.empty() && !psCode.empty(), "the game's bake shaders read from the precomp");
    if (!read || !MakeDevice()) {
        std::printf("\n%d checks, %d failures\n", checks, failures + 1);
        return 1;
    }
    g_dev->CreateVertexShader(reinterpret_cast<const DWORD*>(vsCode.data()), &g_gameVs);
    g_dev->CreatePixelShader(reinterpret_cast<const DWORD*>(psCode.data()), &g_gamePs);
    const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    g_dev->CreateVertexDeclaration(el, &g_decl);
    ID3DBlob *code = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kLightmapSmoothHlsl, std::strlen(kLightmapSmoothHlsl), "lightmap_smooth", nullptr, nullptr, "StampPS", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        std::printf("StampPS: %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return 1;
    }
    g_dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &g_stampPs);
    if (FAILED(D3DCompile(kTerrainBakeHlsl, std::strlen(kTerrainBakeHlsl), "terrain_bake_ps.hlsl", nullptr, nullptr, "BakeConePS", "ps_2_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        std::printf("BakeConePS: %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return 1;
    }
    g_dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &g_conePs);
    Check(g_gameVs && g_gamePs && g_stampPs && g_conePs && g_decl, "shaders made");
    CommonState();
    g_dev->BeginScene();

    constexpr int N = 256;
    const float x0 = 1280.0f, z0 = 896.0f, gy = 23.66f;
    std::mt19937 rng(2810);
    IDirect3DTexture9* rtF = MakeTex(N, N, D3DFMT_A32B32G32R32F, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
    IDirect3DTexture9* rt8 = MakeTex(N, N, D3DFMT_A8R8G8B8, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
    IDirect3DTexture9* nm = MakeTex(N, N, D3DFMT_A8R8G8B8, 0, D3DPOOL_MANAGED);
    IDirect3DTexture9* hm = MakeTex(N, N, D3DFMT_A8R8G8B8, 0, D3DPOOL_MANAGED);
    IDirect3DTexture9* zero = MakeTex(N, N, D3DFMT_A32B32G32R32F, 0, D3DPOOL_MANAGED);
    Check(rtF && rt8 && nm && hm && zero, "textures made");
    if (!(rtF && rt8 && nm && hm && zero)) return 1;
    FillFloat(zero, N, N, std::vector<float>(static_cast<size_t>(N) * N * 4, 0.0f));
    const int units = static_cast<int>(std::lround(gy / kHeightScale)); // the ground height in the height map's bytes
    const uint8_t flatG = static_cast<uint8_t>(units / 255), flatB = static_cast<uint8_t>(units % 255);

    // ---- T0: the game's shader against the CPU law, sloped normals and varied heights
    {
        std::vector<uint8_t> nmB(static_cast<size_t>(N) * N * 4), hmB(static_cast<size_t>(N) * N * 4);
        std::uniform_int_distribution<int> tilt(128 - 80, 128 + 80), dh(-500, 500);
        for (size_t i = 0; i < static_cast<size_t>(N) * N; i++) {
            nmB[i * 4 + 3] = static_cast<uint8_t>(tilt(rng)), nmB[i * 4 + 1] = static_cast<uint8_t>(tilt(rng));
            const int u = std::clamp(units + dh(rng), 0, 255 * 255 + 254);
            hmB[i * 4 + 1] = static_cast<uint8_t>(u / 255), hmB[i * 4 + 2] = static_cast<uint8_t>(u % 255);
        }
        FillBytes(nm, N, N, nmB);
        FillBytes(hm, N, N, hmB);
        g_dev->SetTexture(0, nm), g_dev->SetTexture(1, hm);
        double worst = 0;
        for (int t = 0; t < 24; t++) {
            const Lamp l = RandomLamp(rng, x0, z0, gy);
            Target(rtF);
            g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 0, 0);
            g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            GamePass(N, N, x0, z0, static_cast<float>(N), l);
            const std::vector<float> g = ReadFloat(rtF, N, N);
            for (int y = 0; y < N; y++)
                for (int x = 0; x < N; x++) {
                    const size_t i = static_cast<size_t>(y) * N + x;
                    float ref[3];
                    LawCpu(l, x0 + x + 0.5f, z0 + y + 0.5f, nmB[i * 4 + 3], nmB[i * 4 + 1], hmB[i * 4 + 1], hmB[i * 4 + 2], ref);
                    for (int k = 0; k < 3; k++) worst = std::max(worst, static_cast<double>(std::fabs(g[i * 4 + k] - ref[k])));
                }
        }
        std::printf("\n[T0] the game's bake shader against the CPU reading of its law (24 lamps, sloped ground): largest difference %.2e\n", worst);
        Check(worst < 2e-3, "the CPU reading of the game's bake law");
    }

    // flat maps from here on: normal up, every texel at the ground height
    {
        std::vector<uint8_t> nmB(static_cast<size_t>(N) * N * 4, 0), hmB(static_cast<size_t>(N) * N * 4, 0);
        for (size_t i = 0; i < static_cast<size_t>(N) * N; i++) {
            nmB[i * 4 + 3] = 128, nmB[i * 4 + 1] = 128;
            hmB[i * 4 + 1] = flatG, hmB[i * 4 + 2] = flatB;
        }
        FillBytes(nm, N, N, nmB);
        FillBytes(hm, N, N, hmB);
    }
    const float flatY = (flatG * 255.0f + flatB) * kHeightScale;

    // ---- T1: StampPS against the game's shader, one lamp at a time, flat ground
    {
        double worst = 0;
        for (int t = 0; t < 48; t++) {
            const Lamp l = RandomLamp(rng, x0, z0, flatY);
            g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            Target(rtF);
            g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 0, 0);
            g_dev->SetTexture(0, nm), g_dev->SetTexture(1, hm);
            GamePass(N, N, x0, z0, static_cast<float>(N), l);
            const std::vector<float> g = ReadFloat(rtF, N, N);
            Target(rtF);
            StampPass(N, N, x0 + 0.5f, z0 + 0.5f, flatY, {l}, zero);
            const std::vector<float> s = ReadFloat(rtF, N, N);
            for (size_t i = 0; i < g.size(); i++)
                if (i % 4 != 3) worst = std::max(worst, static_cast<double>(std::fabs(std::min(g[i], 1.0f) - s[i])));
        }
        std::printf("\n[T1] StampPS against the game's bake shader (48 lamps, flat ground, float target): largest difference %.2e\n", worst);
        Check(worst < 2e-3, "StampPS reproduces the game's bake law on flat ground");
    }

    // ---- T2: 8 lamps; the game's 8 additive passes into an 8-bit target against StampPS's one pass
    {
        int worst = 0;
        long off = 0;
        for (int t = 0; t < 12; t++) {
            std::vector<Lamp> ls;
            for (int k = 0; k < 8; k++) ls.push_back(RandomLamp(rng, x0, z0, flatY));
            Target(rt8);
            g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 0, 0);
            g_dev->SetTexture(0, nm), g_dev->SetTexture(1, hm);
            g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE), g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            for (const Lamp& l : ls) GamePass(N, N, x0, z0, static_cast<float>(N), l);
            g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            const std::vector<uint8_t> g = ReadBytes(rt8, N, N);
            Target(rt8);
            StampPass(N, N, x0 + 0.5f, z0 + 0.5f, flatY, ls, zero);
            const std::vector<uint8_t> s = ReadBytes(rt8, N, N);
            for (size_t i = 0; i < g.size(); i++)
                if (i % 4 != 3) {
                    const int d = std::abs(static_cast<int>(g[i]) - s[i]);
                    worst = std::max(worst, d);
                    off += d > 1 ? 1 : 0;
                }
        }
        std::printf("\n[T2] 8 lamps, the game's additive passes (8-bit) against StampPS's one pass: largest difference %d / 255, %ld channels off by more than 1\n",
                    worst, off);
        Check(worst <= 4, "8 lamps within 4 / 255 of the game's additive 8-bit passes");
    }

    // ---- T3: a chunk border, P grids of two neighbouring chunks (264 x 264, 4 texels into each neighbour)
    {
        constexpr int P = 264;
        IDirect3DTexture9* rtP = MakeTex(P, P, D3DFMT_A32B32G32R32F, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
        IDirect3DTexture9* zeroP = MakeTex(P, P, D3DFMT_A32B32G32R32F, 0, D3DPOOL_MANAGED);
        FillFloat(zeroP, P, P, std::vector<float>(static_cast<size_t>(P) * P * 4, 0.0f));
        std::vector<Lamp> ls;
        Lamp l = RandomLamp(rng, x0, z0, flatY);
        l.pos[0] = x0 + 255.7f, l.pos[2] = z0 + 130.2f; // at the border with the chunk on the right
        ls.push_back(l);
        Target(rtP);
        StampPass(P, P, x0 - 3.5f, z0 - 3.5f, flatY, ls, zeroP);
        const std::vector<float> a = ReadFloat(rtP, P, P);
        Target(rtP);
        StampPass(P, P, x0 + 256 - 3.5f, z0 - 3.5f, flatY, ls, zeroP);
        const std::vector<float> b = ReadFloat(rtP, P, P);
        long differ = 0;
        for (int y = 0; y < P; y++)
            for (int k = 0; k < 8; k++) { // a's columns 256..263 are b's 0..7 (the same world texels)
                const float* pa = &a[(static_cast<size_t>(y) * P + 256 + k) * 4];
                const float* pb = &b[(static_cast<size_t>(y) * P + k) * 4];
                differ += std::memcmp(pa, pb, 12) != 0 ? 1 : 0;
            }
        std::printf("\n[T3] chunk border: %ld of %d shared texels differ\n", differ, P * 8);
        Check(differ == 0, "both chunks stamp the shared border texels bit for bit");
        rtP->Release(), zeroP->Release();
    }

    // ---- T4: the base kept (rgb + stamps clamped at 1, alpha untouched)
    {
        std::vector<float> baseV(static_cast<size_t>(N) * N * 4);
        std::uniform_real_distribution<float> U(0, 1);
        for (float& v : baseV) v = U(rng);
        IDirect3DTexture9* base = MakeTex(N, N, D3DFMT_A32B32G32R32F, 0, D3DPOOL_MANAGED);
        FillFloat(base, N, N, baseV);
        std::vector<Lamp> ls{RandomLamp(rng, x0, z0, flatY), RandomLamp(rng, x0, z0, flatY)};
        Target(rtF);
        StampPass(N, N, x0 + 0.5f, z0 + 0.5f, flatY, ls, zero);
        const std::vector<float> st = ReadFloat(rtF, N, N);
        Target(rtF);
        StampPass(N, N, x0 + 0.5f, z0 + 0.5f, flatY, ls, base);
        const std::vector<float> s = ReadFloat(rtF, N, N);
        double worst = 0;
        long alpha = 0;
        for (size_t i = 0; i < s.size(); i++) {
            if (i % 4 == 3) alpha += s[i] != baseV[i] ? 1 : 0;
            else worst = std::max(worst, static_cast<double>(std::fabs(s[i] - std::min(baseV[i] + st[i], 1.0f))));
        }
        std::printf("\n[T4] base kept: rgb against min(base + stamps, 1) largest difference %.2e; alpha changed at %ld texels\n", worst, alpha);
        Check(worst < 1e-5 && alpha == 0, "the base's rgb takes the stamps clamped at 1 and its alpha stays");
        base->Release();
    }

    // ---- T5: GPU time, StampPS over a 264 x 264 P grid with 8 lamps
    {
        constexpr int P = 264, runs = 200;
        IDirect3DTexture9* rtP = MakeTex(P, P, D3DFMT_A32B32G32R32F, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
        IDirect3DTexture9* zeroP = MakeTex(P, P, D3DFMT_A32B32G32R32F, 0, D3DPOOL_MANAGED);
        FillFloat(zeroP, P, P, std::vector<float>(static_cast<size_t>(P) * P * 4, 0.0f));
        std::vector<Lamp> ls;
        for (int k = 0; k < 8; k++) ls.push_back(RandomLamp(rng, x0, z0, flatY));
        IDirect3DQuery9 *dj = nullptr, *fq = nullptr, *t0 = nullptr, *t1 = nullptr;
        g_dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &dj), g_dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &fq);
        g_dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &t0), g_dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &t1);
        double ms = -1;
        if (dj && fq && t0 && t1) {
            Target(rtP);
            dj->Issue(D3DISSUE_BEGIN);
            t0->Issue(D3DISSUE_END);
            for (int r = 0; r < runs; r++) StampPass(P, P, x0 - 3.5f, z0 - 3.5f, flatY, ls, zeroP);
            t1->Issue(D3DISSUE_END);
            dj->Issue(D3DISSUE_END);
            fq->Issue(D3DISSUE_END);
            UINT64 a = 0, b = 0, f = 0;
            BOOL disjoint = TRUE;
            while (t1->GetData(&b, sizeof b, D3DGETDATA_FLUSH) == S_FALSE) {}
            t0->GetData(&a, sizeof a, D3DGETDATA_FLUSH);
            fq->GetData(&f, sizeof f, D3DGETDATA_FLUSH);
            dj->GetData(&disjoint, sizeof disjoint, D3DGETDATA_FLUSH);
            if (f && !disjoint) ms = static_cast<double>(b - a) * 1000.0 / f / runs;
        }
        std::printf("\n[T5] StampPS, 264 x 264 with 8 lamps: %.4f ms of GPU a pass (%d passes)\n", ms, runs);
        Check(ms >= 0 && ms < 1.0, "StampPS under 1 ms of GPU a chunk");
        for (IDirect3DQuery9* q : {dj, fq, t0, t1})
            if (q) q->Release();
        rtP->Release(), zeroP->Release();
    }

    // ---- T6: the shader the mod swaps is this one
    {
        const uint64_t h = TerrainBakeCone::Fnv(psCode.data(), psCode.size());
        std::printf("\n[T6] the game's bake pixel shader: %zu bytes, FNV-1a %016llx\n", psCode.size(), static_cast<unsigned long long>(h));
        Check(psCode.size() == TerrainBakeCone::kGamePsBytes && h == TerrainBakeCone::kGamePsFnv, "the mod recognises the game's bake pixel shader");
    }

    // sloped maps for T7 and T8
    std::vector<uint8_t> nmS(static_cast<size_t>(N) * N * 4), hmS(static_cast<size_t>(N) * N * 4);
    {
        std::uniform_int_distribution<int> tilt(128 - 80, 128 + 80), dh(-500, 500);
        for (size_t i = 0; i < static_cast<size_t>(N) * N; i++) {
            nmS[i * 4 + 3] = static_cast<uint8_t>(tilt(rng)), nmS[i * 4 + 1] = static_cast<uint8_t>(tilt(rng));
            const int u = std::clamp(units + dh(rng), 0, 255 * 255 + 254);
            hmS[i * 4 + 1] = static_cast<uint8_t>(u / 255), hmS[i * 4 + 2] = static_cast<uint8_t>(u % 255);
        }
        FillBytes(nm, N, N, nmS);
        FillBytes(hm, N, N, hmS);
    }

    // ---- T7: BakeConePS without cones (no-cone rows; rows without the stub's mark) against the game's shader
    {
        double worst = 0;
        for (int t = 0; t < 24; t++) {
            Lamp l = RandomLamp(rng, x0, z0, gy);
            if (t % 2) RandomCone(rng, l), l.cone[2][3] = 0; // cone rows, but no mark: the game's law
            g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            g_dev->SetTexture(0, nm), g_dev->SetTexture(1, hm);
            Target(rtF);
            g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 0, 0);
            GamePass(N, N, x0, z0, static_cast<float>(N), l);
            const std::vector<float> g = ReadFloat(rtF, N, N);
            Target(rtF);
            g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 0, 0);
            GamePass(N, N, x0, z0, static_cast<float>(N), l, g_conePs);
            const std::vector<float> c = ReadFloat(rtF, N, N);
            for (size_t i = 0; i < g.size(); i++) worst = std::max(worst, static_cast<double>(std::fabs(g[i] - c[i])));
        }
        std::printf("\n[T7] BakeConePS without cones against the game's bake shader (24 lamps, sloped ground): largest difference %.2e\n", worst);
        Check(worst < 1e-5, "BakeConePS without cones is the game's law");
    }

    // ---- T8: BakeConePS with cones against the CPU law times the cone factor
    {
        double worst = 0;
        long lit = 0, dark = 0;
        for (int t = 0; t < 48; t++) {
            Lamp l = RandomLamp(rng, x0, z0, gy);
            l.pos[0] = x0 + 40 + 176 * (t % 7) / 6.0f, l.pos[2] = z0 + 40 + 176 * (t % 5) / 4.0f; // over the chunk
            RandomCone(rng, l);
            g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            g_dev->SetTexture(0, nm), g_dev->SetTexture(1, hm);
            Target(rtF);
            g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 0, 0);
            GamePass(N, N, x0, z0, static_cast<float>(N), l, g_conePs);
            const std::vector<float> c = ReadFloat(rtF, N, N);
            for (int y = 0; y < N; y++)
                for (int x = 0; x < N; x++) {
                    const size_t i = static_cast<size_t>(y) * N + x;
                    float ref[3], plain[3];
                    LawConeCpu(l, x0 + x + 0.5f, z0 + y + 0.5f, nmS[i * 4 + 3], nmS[i * 4 + 1], hmS[i * 4 + 1], hmS[i * 4 + 2], ref);
                    LawCpu(l, x0 + x + 0.5f, z0 + y + 0.5f, nmS[i * 4 + 3], nmS[i * 4 + 1], hmS[i * 4 + 1], hmS[i * 4 + 2], plain);
                    if (plain[0] > 0.05f) (ref[0] > 0.9f * plain[0] ? lit : dark)++;
                    for (int k = 0; k < 3; k++) worst = std::max(worst, static_cast<double>(std::fabs(c[i * 4 + k] - ref[k])));
                }
        }
        std::printf("\n[T8] BakeConePS with cones against the CPU law x the cone factor (48 spots and shaded lamps, sloped ground): largest difference "
                    "%.2e; lit texels %ld inside the cones, %ld dimmed outside\n",
                    worst, lit, dark);
        Check(worst < 2e-3 && lit > 0 && dark > 0, "BakeConePS gives the law times the cones");
    }

    // ---- T9: StampPS with the same cones against BakeConePS, flat ground
    {
        std::vector<uint8_t> nmB(static_cast<size_t>(N) * N * 4, 0), hmB(static_cast<size_t>(N) * N * 4, 0);
        for (size_t i = 0; i < static_cast<size_t>(N) * N; i++) {
            nmB[i * 4 + 3] = 128, nmB[i * 4 + 1] = 128;
            hmB[i * 4 + 1] = flatG, hmB[i * 4 + 2] = flatB;
        }
        FillBytes(nm, N, N, nmB);
        FillBytes(hm, N, N, hmB);
        double worst = 0;
        for (int t = 0; t < 48; t++) {
            Lamp l = RandomLamp(rng, x0, z0, flatY);
            RandomCone(rng, l);
            g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            Target(rtF);
            g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 0, 0);
            g_dev->SetTexture(0, nm), g_dev->SetTexture(1, hm);
            GamePass(N, N, x0, z0, static_cast<float>(N), l, g_conePs);
            const std::vector<float> g = ReadFloat(rtF, N, N);
            Target(rtF);
            StampPass(N, N, x0 + 0.5f, z0 + 0.5f, flatY, {l}, zero);
            const std::vector<float> s = ReadFloat(rtF, N, N);
            for (size_t i = 0; i < g.size(); i++)
                if (i % 4 != 3) worst = std::max(worst, static_cast<double>(std::fabs(std::min(g[i], 1.0f) - s[i])));
        }
        std::printf("\n[T9] StampPS with cones against BakeConePS (48 lamps, flat ground, float target): largest difference %.2e\n", worst);
        Check(worst < 2e-3, "StampPS gives the bake's cones");
    }

    g_dev->EndScene();
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
