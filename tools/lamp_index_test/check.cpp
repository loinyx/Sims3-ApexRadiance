// Offline check of the lamp index cells (real-time GI S3-02: features/lamp_index.h, ShaderPatches::PatchObjectLampPs index).
// Read-only: prints to stdout, writes no file.
//   check.exe <lamp_index.hlsl> [Shaders_Win32.precomp]
// 1. Transport on the CPU: a model of the per-draw fix (the pick at the object, 8 lamps, the shader's sum, the ground share
//    and both height gates) against the same pixel lit from the index texture LampIndex::Build filled with that pick.
//    Asserted arms: open ground with objects anywhere, and walls with objects at cell centres (loose, set in a wall, gates in
//    a fence gap): 0 pixels over 1/255. Reported arms: walls with objects anywhere, tall objects, a dense lamp cluster.
// 2. The HLSL reference of the block (lamp_index.hlsl): compiled as ps_3_0, priced, accepted by native D3D9, and its pixels
//    on a hidden device against the CPU model of 1 (2 LSB).
// 3. With the precomp: every pixel shader the per-draw patch takes, patched with the index block: slots, temporaries (the
//    host's + 16, the cost table's bound, and the copy's own), native D3D9 accepts it (a looped shader on a device), and
//    its pixels equal the per-draw copy's (1 LSB) with the same lamps, without walls and with walls, flags set.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include "../../features/lamp_index.h"
#include "../../features/shader_patches.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace {

int g_checks = 0, g_failures = 0;
void Check(bool ok, const char* what) {
    g_checks++;
    if (!ok) {
        g_failures++;
        if (g_failures < 40) std::printf("  FAIL %s\n", what);
    }
}
template <class T> void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}
float Sat(float v) { return std::clamp(v, 0.0f, 1.0f); }

// ---- 1. the CPU model ----
struct SceneLamp {
    float row[16]; // (pos, 1/R^2), (colour, spill), cone 1, cone 2: the rows the per-draw fix uploads
    float radius;
};
struct Wall {
    float x0, z0, x1, z1, pass; // a lamp's light crossing it keeps `pass`
};
struct Scene {
    std::vector<SceneLamp> lamps;
    std::vector<Wall> walls;
};
struct Flags {
    bool inWall = false, gate = false;
};
constexpr float kLooseSkip = 0.2f, kWallSkip = 0.75f; // LotLightBridge: the nearest wall an object ignores
constexpr float kHost = 0.02f, kGroundTexel = 0.3f;   // the host's own light, the ground atlas (uniform)

float ConeAt(const float* row, const float* p) {
    float l[3] = {row[0] - p[0], row[1] - p[1], row[2] - p[2]};
    const float d = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    if (!(d > 1e-3f)) return 1.0f;
    for (float& v : l) v /= d;
    const float t1 = l[0] * row[8] + l[1] * row[9] + l[2] * row[10] + row[11], t2 = l[0] * row[12] + l[1] * row[13] + l[2] * row[14] + row[15];
    return row[7] + (1.0f - row[7]) * Sat(std::max(t1, t2));
}
float Visibility(const Scene& s, const float* lamp, const float* p, float nearSkip) {
    float v = 1.0f;
    const float dx = p[0] - lamp[0], dz = p[2] - lamp[2], len = std::sqrt(dx * dx + dz * dz);
    for (const Wall& w : s.walls) {
        const float ex = w.x1 - w.x0, ez = w.z1 - w.z0, den = dx * ez - dz * ex;
        if (std::fabs(den) < 1e-9f) continue;
        const float t = ((w.x0 - lamp[0]) * ez - (w.z0 - lamp[2]) * ex) / den, u = ((w.x0 - lamp[0]) * dz - (w.z0 - lamp[2]) * dx) / den;
        if (t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f && (1.0f - t) * len > nearSkip) v *= w.pass;
    }
    return v;
}
// LotLightBridge PickObjectLampsImpl with SelectLamps: the 16 best by distance - radius within 40 m, the first 8 a wall
// leaves, the ground share and the highest head within 15 m
void Pick(const Scene& s, float x, float y, float z, bool inWall, LampIndex::PointPick& out) {
    struct Cand {
        float score;
        const SceneLamp* l;
    };
    std::vector<Cand> c;
    for (const SceneLamp& l : s.lamps) {
        const float dx = l.row[0] - x, dz = l.row[2] - z, score = std::sqrt(dx * dx + dz * dz) - l.radius;
        if (score <= 40.0f) c.push_back({score, &l});
    }
    std::stable_sort(c.begin(), c.end(), [](const Cand& a, const Cand& b) { return a.score < b.score; });
    if (c.size() > 16) c.resize(16);
    out = {};
    const float obj[3] = {x, y + 0.5f, z};
    float seen = 0.0f, all = 0.0f, top = -1e9f;
    for (const Cand& k : c) {
        const float* r = k.l->row;
        const float dx = r[0] - obj[0], dy = r[1] - obj[1], dz = r[2] - obj[2], rad = std::max(k.l->radius, 0.1f);
        if (dx * dx + dz * dz <= 15.0f * 15.0f) top = std::max(top, r[1]);
        const float w = (r[4] * 0.2126f + r[5] * 0.7152f + r[6] * 0.0722f) * ConeAt(r, obj) / (1.0f + (dx * dx + dy * dy + dz * dz) / (rad * rad));
        all += w;
        const float v = Visibility(s, r, obj, inWall ? kWallSkip : kLooseSkip);
        if (v < 0.01f) continue;
        seen += w * v;
        if (out.n >= LampIndex::kSlots) continue;
        std::memcpy(out.rows[out.n], r, sizeof out.rows[0]);
        out.vis[out.n++] = v;
    }
    if (all > 1e-5f) out.share = Sat(seen / all);
    out.top = top > -1e8f ? top : LampIndex::kNoTop;
}
// One lamp at a pixel: the patched shader's law and cones (AppendLampLaw and the cone rows), before colour and visibility
float Law(const float* row, const float* P, const float* N) {
    const float l[3] = {row[0] - P[0], row[1] - P[1], row[2] - P[2]};
    const float d2 = std::max(l[0] * l[0] + l[1] * l[1] + l[2] * l[2], 1e-4f), inv = 1.0f / std::sqrt(d2);
    const float L[3] = {l[0] * inv, l[1] * inv, l[2] * inv};
    const float s = Sat(1.0f - d2 * row[3]);
    const float t1 = L[0] * row[8] + L[1] * row[9] + L[2] * row[10] + row[11], t2 = L[0] * row[12] + L[1] * row[13] + L[2] * row[14] + row[15];
    const float t = Sat(std::max(t1, t2));
    return Sat(N[0] * L[0] + N[1] * L[1] + N[2] * L[2]) * s * s * (t - t * row[7] + row[7]);
}
void Finish(const float* Q, float lampStrength, float ground, const float* N, float rgb[3]) {
    for (int c = 0; c < 3; c++) rgb[c] = Sat(std::max(kHost + Q[c] * lampStrength, kGroundTexel * Sat(N[1]) * ground));
}
// The per-draw fix: the pick at the object's point, everything else decided once for the object (DrawObjectLamp)
void ShadePerDraw(const LampIndex::PointPick& pick, float originY, Flags f, const LampIndex::Frame& fr, const float* P, const float* N, float rgb[3]) {
    float Q[3] = {};
    for (int k = 0; k < pick.n; k++) {
        const float law = Law(pick.rows[k], P, N);
        for (int c = 0; c < 3; c++) Q[c] += pick.rows[k][4 + c] * pick.vis[k] * law;
    }
    const float height = pick.top < 1e8f ? Sat(1.0f - (originY - pick.top) / fr.fadeAbove) : 1.0f;
    const float pool = Sat((fr.poolNone - std::fabs(originY - fr.lotY)) / (fr.poolNone - fr.poolFull));
    Finish(Q, f.gate ? fr.gateStrength : fr.lampStrength, fr.groundStrength * pick.share * height * pool, N, rgb);
}
// The index block: the cell of the pixel, its entries, the share and the gates at the pixel (lamp_index.hlsl)
void ShadeIndex(const LampIndex::Atlas& a, Flags f, const LampIndex::Frame& fr, const float* P, const float* N, float rgb[3]) {
    int cx, cz;
    LampIndex::CellOf(a, P[0], P[2], cx, cz);
    const float* cell = LampIndex::Texel(a, cx * LampIndex::kCellTexels, cz);
    float Q[3] = {};
    for (int s = 0; s < LampIndex::kSlots; s++) {
        const float* e = cell + 4 * s;
        float row[16];
        for (int r = 0; r < 4; r++) std::memcpy(row + 4 * r, LampIndex::Texel(a, static_cast<int>(e[0]), LampIndex::kGrid + r), 16);
        const float k = Law(row, P, N) * (f.inWall ? e[2] : e[1]);
        for (int c = 0; c < 3; c++) Q[c] += row[4 + c] * k;
    }
    const float* g = cell + 4 * LampIndex::kSlots;
    const float span = fr.poolNone - fr.poolFull;
    const float ground = (f.inWall ? g[2] : g[0]) * Sat(1.0f + (g[1] - P[1]) / fr.fadeAbove) * Sat(fr.poolNone / span - std::fabs(P[1] - fr.lotY) / span) * fr.groundStrength;
    Finish(Q, f.gate ? fr.gateStrength : fr.lampStrength, ground, N, rgb);
}

Scene MakeScene(std::mt19937& rng, int lamps, float spread, bool walls, float lotY) {
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    Scene s;
    for (int i = 0; i < lamps; i++) {
        SceneLamp l{};
        l.radius = 2.0f + 8.0f * u(rng);
        l.row[0] = 1000.0f + (u(rng) - 0.5f) * spread, l.row[1] = lotY + 1.5f + 2.5f * u(rng), l.row[2] = 2000.0f + (u(rng) - 0.5f) * spread;
        l.row[3] = 1.0f / (l.radius * l.radius);
        for (int c = 0; c < 3; c++) l.row[4 + c] = 0.2f + 1.3f * u(rng);
        l.row[7] = 1.0f, l.row[11] = l.row[15] = 1.0f; // no cone
        if (i % 3 == 0) { // a spot pointing down and aside (LotLightBridge LampCone: t = l.k.xyz + k.w)
            const float ax = u(rng) - 0.5f, az = u(rng) - 0.5f, n = std::sqrt(ax * ax + 1.0f + az * az), k = 5.0f;
            l.row[8] = k * ax / n, l.row[9] = k / n, l.row[10] = k * az / n, l.row[11] = -k * 0.7f;
            l.row[12] = l.row[8], l.row[13] = l.row[9], l.row[14] = l.row[10], l.row[15] = -k * 0.8f;
            l.row[7] = 0.15f * u(rng);
        }
        s.lamps.push_back(l);
    }
    if (walls)
        for (int i = 0; i < 14; i++) {
            const float x = 1000.0f + (u(rng) - 0.5f) * spread * 0.8f, z = 2000.0f + (u(rng) - 0.5f) * spread * 0.8f, len = 4.0f + 8.0f * u(rng);
            const bool alongX = i % 2 == 0;
            s.walls.push_back({x, z, x + (alongX ? len : 0.0f), z + (alongX ? 0.0f : len), i % 5 == 0 ? 0.4f : 0.0f});
        }
    return s;
}

struct ArmResult {
    long pixels = 0, over = 0;
    float worst = 0.0f;
    LampIndex::Atlas atlas;
};
// centre: objects stand at cell centres with their pixels inside the cell (the transport case); else anywhere
ArmResult RunArm(const char* name, unsigned seed, int lamps, float spread, bool walls, bool centre, float tall, bool assert0) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    LampIndex::Frame fr;
    fr.lampStrength = 0.8f, fr.gateStrength = 0.5f, fr.groundStrength = 0.6f, fr.lotY = 10.0f;
    const Scene s = MakeScene(rng, lamps, spread, walls, fr.lotY);
    ArmResult r;
    LampIndex::Build(r.atlas, 1003.0f, 2001.0f, fr.lotY, [&](float x, float y, float z, bool inWall, LampIndex::PointPick& o) { Pick(s, x, y, z, inWall, o); });
    long truncated = 0;
    for (int o = 0; o < 600; o++) {
        float at[3] = {1000.0f + (u(rng) - 0.5f) * 100.0f, fr.lotY, 2000.0f + (u(rng) - 0.5f) * 100.0f};
        if (centre) {
            int cx, cz;
            LampIndex::CellOf(r.atlas, at[0], at[2], cx, cz);
            at[0] = r.atlas.ox + (cx + 0.5f) * LampIndex::kCell, at[2] = r.atlas.oz + (cz + 0.5f) * LampIndex::kCell;
        }
        const Flags f{o % 3 == 1, o % 4 == 2};
        LampIndex::PointPick pick;
        Pick(s, at[0], at[1], at[2], f.inWall, pick);
        // (more than 8 lamps in reach of the object: the per-draw fix drops some too, by its own order)
        int reach = 0;
        for (const SceneLamp& l : s.lamps) {
            const float dx = l.row[0] - at[0], dz = l.row[2] - at[2];
            if (std::sqrt(dx * dx + dz * dz) - l.radius < 2.5f) reach++;
        }
        if (reach > LampIndex::kSlots) truncated++;
        const float half = centre ? 0.98f : 0.6f;
        for (int p = 0; p < 64; p++) {
            const float P[3] = {at[0] + (u(rng) * 2.0f - 1.0f) * half, at[1] + u(rng) * tall, at[2] + (u(rng) * 2.0f - 1.0f) * half};
            float N[3] = {u(rng) - 0.5f, u(rng) * 1.2f - 0.2f, u(rng) - 0.5f};
            const float n = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]) + 1e-6f;
            for (float& v : N) v /= n;
            float a[3], b[3];
            ShadePerDraw(pick, at[1], f, fr, P, N, a);
            ShadeIndex(r.atlas, f, fr, P, N, b);
            const float d = std::max({std::fabs(a[0] - b[0]), std::fabs(a[1] - b[1]), std::fabs(a[2] - b[2])});
            r.pixels++, r.worst = std::max(r.worst, d);
            if (d > 1.0f / 255.0f) r.over++;
        }
    }
    std::printf("  %-44s %ld pixels, %ld over 1/255 (worst %.4f) | table %d lamps, %d lit cells, dropped %d (cell) %d (table), objects with over 8 lamps in reach %ld\n", name,
                r.pixels, r.over, r.worst, r.atlas.lamps, r.atlas.cellsLit, r.atlas.cellOverflow, r.atlas.tableOverflow, truncated);
    if (assert0) {
        char what[160];
        std::snprintf(what, sizeof what, "transport, %s: 0 pixels over 1/255", name);
        Check(r.over == 0 && r.pixels > 30000, what);
        Check(r.atlas.cellOverflow == 0 && r.atlas.tableOverflow == 0 && truncated == 0, "transport: the sample keeps to 8 lamps a cell");
        Check(r.atlas.lamps > 10 && r.atlas.cellsLit > 200 && r.atlas.cellsLit < LampIndex::kGrid * LampIndex::kGrid, "transport: the sample lights the window");
    }
    return r;
}
void Transport() {
    std::puts("Transport on the CPU (per-draw fix against the index texture):");
    RunArm("open ground, objects anywhere", 11, 40, 110.0f, false, false, 1.0f, true);
    RunArm("walls, objects at cell centres, flags", 12, 40, 110.0f, true, true, 1.0f, true);
    const ArmResult w = RunArm("walls, objects anywhere (reported)", 13, 40, 110.0f, true, false, 1.0f, false);
    const ArmResult t = RunArm("open ground, pixels up to 3 m (reported)", 14, 40, 110.0f, false, false, 3.0f, false);
    const ArmResult d = RunArm("dense cluster, 60 lamps in 20 m (reported)", 15, 60, 20.0f, false, false, 1.0f, false);
    Check(w.over > 0 && w.over < w.pixels / 4, "walls, objects anywhere: the visibility of the cell differs from the object's for a minority of pixels");
    Check(t.over > 0, "tall objects: the height gates at the pixel differ from the gates at the origin");
    Check(d.atlas.lamps <= LampIndex::kMaxLamps, "dense cluster: the table holds");
}

// ---- hidden D3D9 device (64 x 64 target), as tools/floor_shade_check ----
struct Gpu {
    HWND window = nullptr;
    IDirect3D9* api = nullptr;
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9 *target = nullptr, *readback = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    Gpu() {
        window = CreateWindowExW(0, L"STATIC", L"Apex lamp index check", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        api = Direct3DCreate9(D3D_SDK_VERSION);
        D3DPRESENT_PARAMETERS p{};
        p.Windowed = TRUE;
        p.SwapEffect = D3DSWAPEFFECT_DISCARD;
        p.hDeviceWindow = window;
        p.BackBufferWidth = p.BackBufferHeight = 64;
        if (api) api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &p, &dev);
        if (!dev) return;
        dev->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr);
        dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr);
        const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                        {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                                        D3DDECL_END()};
        dev->CreateVertexDeclaration(el, &decl);
        dev->SetRenderTarget(0, target);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        if (!target || !readback || !decl) Release(dev);
    }
    ~Gpu() {
        Release(decl);
        Release(target);
        Release(readback);
        Release(dev);
        Release(api);
        if (window) DestroyWindow(window);
    }
    IDirect3DPixelShader9* Ps(const std::vector<DWORD>& t) {
        IDirect3DPixelShader9* s = nullptr;
        if (t.empty() || FAILED(dev->CreatePixelShader(t.data(), &s))) return nullptr;
        return s;
    }
    // a full-screen quad, TEXCOORD0 = (0, 0) top left .. (1, 1) bottom right
    std::vector<DWORD> Render(IDirect3DPixelShader9* ps, IDirect3DVertexShader9* vs) {
        struct Vertex {
            float x, y, z, w, u, v;
        };
        const Vertex q[4] = {{-1, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 1, 0}, {-1, -1, 0, 1, 0, 1}, {1, -1, 0, 1, 1, 1}};
        dev->SetVertexDeclaration(decl);
        dev->SetVertexShader(vs);
        dev->SetPixelShader(ps);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00123456, 1.0f, 0);
        dev->BeginScene();
        const bool drawn = SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vertex)));
        dev->EndScene();
        std::vector<DWORD> px(4096);
        D3DLOCKED_RECT r{};
        if (drawn && SUCCEEDED(dev->GetRenderTargetData(target, readback)) && SUCCEEDED(readback->LockRect(&r, nullptr, D3DLOCK_READONLY))) {
            for (int y = 0; y < 64; y++) std::memcpy(px.data() + y * 64, static_cast<char*>(r.pBits) + y * r.Pitch, 256);
            readback->UnlockRect();
        } else
            Check(false, "GPU draw and readback");
        return px;
    }
};
std::vector<DWORD> Compile(const std::string& src, const char* profile) {
    ID3DBlob *code = nullptr, *err = nullptr;
    const HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr) && err) std::printf("%s\n", static_cast<const char*>(err->GetBufferPointer()));
    std::vector<DWORD> out;
    if (code) out.assign(static_cast<const DWORD*>(code->GetBufferPointer()), static_cast<const DWORD*>(code->GetBufferPointer()) + code->GetBufferSize() / 4);
    Release(code);
    Release(err);
    return out;
}
std::string Disasm(const std::vector<DWORD>& t) {
    ID3DBlob* b = nullptr;
    if (t.empty() || FAILED(D3DDisassemble(t.data(), t.size() * 4, 0, nullptr, &b)) || !b) return {};
    std::string s(static_cast<const char*>(b->GetBufferPointer()), b->GetBufferSize());
    b->Release();
    return s;
}
int Slots(const std::string& text) {
    const auto at = text.find("approximately ");
    return at == std::string::npos ? -1 : std::atoi(text.c_str() + at + 14);
}
int Temps(const std::string& text) { // registers r0 .. rN the listing names
    static const std::regex re("[ ,(-]r([0-9]+)");
    int top = -1;
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) top = std::max(top, std::atoi((*it)[1].str().c_str()));
    return top + 1;
}

struct Fixture {
    IDirect3DVertexShader9 *vs = nullptr, *vs1 = nullptr; // vs1: COLOR1 in the place of TEXCOORD7 (11 outputs at most)
    IDirect3DTexture9 *white = nullptr, *ground = nullptr, *index = nullptr;
    IDirect3DCubeTexture9* cube = nullptr;
    void Free() {
        Release(vs);
        Release(vs1);
        Release(white);
        Release(ground);
        Release(index);
        Release(cube);
    }
};
bool MakeFixture(Gpu& gpu, Fixture& f) {
    // every input a game object shader declares: TEXCOORD0..7 (an upward normal where one is read), COLOR0 (COLOR1: vs1), and the world
    // position (x, z, y) in TEXCOORD8 as PatchObjectLampVs writes it
    const std::vector<DWORD> vs = Compile("float4 rows[3]:register(c0);\n"
                                          "struct O{float4 p:POSITION;float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;float4 t4:TEXCOORD4;"
                                          "float4 t5:TEXCOORD5;float4 t6:TEXCOORD6;float4 t7:TEXCOORD7;float4 w:TEXCOORD8;float4 c0:COLOR0;};\n"
                                          "O main(float4 p:POSITION,float2 uv:TEXCOORD0){O o;o.p=p;o.t0=float4(uv,0.5,1);float4 n=float4(0.3,0.85,0.4,1);"
                                          "o.t1=n;o.t2=n;o.t3=n;o.t4=n;o.t5=n;o.t6=n;o.t7=n;o.w=float4(rows[0].xyz+uv.x*rows[1].xyz+uv.y*rows[2].xyz,1);"
                                          "o.c0=float4(0.03,0.03,0.03,1);return o;}",
                                          "vs_3_0");
    if (vs.empty() || FAILED(gpu.dev->CreateVertexShader(vs.data(), &f.vs))) return false;
    const std::vector<DWORD> vs1 = Compile("float4 rows[3]:register(c0);\n"
                                           "struct O{float4 p:POSITION;float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;float4 t4:TEXCOORD4;"
                                           "float4 t5:TEXCOORD5;float4 t6:TEXCOORD6;float4 w:TEXCOORD8;float4 c0:COLOR0;float4 c1:COLOR1;};\n"
                                           "O main(float4 p:POSITION,float2 uv:TEXCOORD0){O o;o.p=p;o.t0=float4(uv,0.5,1);float4 n=float4(0.3,0.85,0.4,1);"
                                           "o.t1=n;o.t2=n;o.t3=n;o.t4=n;o.t5=n;o.t6=n;o.w=float4(rows[0].xyz+uv.x*rows[1].xyz+uv.y*rows[2].xyz,1);"
                                           "o.c0=float4(0.03,0.03,0.03,1);o.c1=float4(0.5,0.5,0.5,1);return o;}",
                                           "vs_3_0");
    if (vs1.empty() || FAILED(gpu.dev->CreateVertexShader(vs1.data(), &f.vs1))) return false;
    const auto solid = [&](DWORD argb) {
        IDirect3DTexture9* t = nullptr;
        D3DLOCKED_RECT r{};
        if (SUCCEEDED(gpu.dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) && SUCCEEDED(t->LockRect(0, &r, nullptr, 0))) {
            *static_cast<DWORD*>(r.pBits) = argb;
            t->UnlockRect(0);
        }
        return t;
    };
    f.white = solid(0xFFFFFFFFu);
    f.ground = solid(0xFF4D4D4Du); // 77 / 255 = 0.302
    if (SUCCEEDED(gpu.dev->CreateCubeTexture(1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &f.cube, nullptr)))
        for (int face = 0; face < 6; face++) {
            D3DLOCKED_RECT r{};
            if (SUCCEEDED(f.cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face), 0, &r, nullptr, 0))) {
                *static_cast<DWORD*>(r.pBits) = 0xFF101010u;
                f.cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face), 0);
            }
        }
    gpu.dev->CreateTexture(LampIndex::kWidth, LampIndex::kHeight, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &f.index, nullptr);
    return f.white && f.ground && f.cube && f.index;
}
void Upload(Fixture& f, const LampIndex::Atlas& a) {
    D3DLOCKED_RECT r{};
    if (FAILED(f.index->LockRect(0, &r, nullptr, 0))) return;
    for (int y = 0; y < LampIndex::kHeight; y++) std::memcpy(static_cast<char*>(r.pBits) + y * r.Pitch, LampIndex::Texel(a, 0, y), LampIndex::kWidth * 16);
    f.index->UnlockRect(0);
}
void PointClamp(IDirect3DDevice9* dev, DWORD s) {
    dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
}
// the quad: world (x, z, y) = corner + u * (du, 0, 0) + v * (0, dv, 0)
void SetQuad(IDirect3DDevice9* dev, float x, float z, float y, float size) {
    const float rows[3][4] = {{x, z, y, 0}, {size, 0, 0, 0}, {0, size, 0, 0}};
    dev->SetVertexShaderConstantF(0, rows[0], 3);
}
int WorstDiff(const std::vector<DWORD>& a, const std::vector<DWORD>& b, int* differing = nullptr) {
    int worst = 0, n = 0;
    for (size_t i = 0; i < a.size(); i++) {
        int d = 0;
        for (int sh = 0; sh < 24; sh += 8) d = std::max(d, std::abs(static_cast<int>((a[i] >> sh) & 255) - static_cast<int>((b[i] >> sh) & 255)));
        worst = std::max(worst, d);
        if (d) n++;
    }
    if (differing) *differing = n;
    return worst;
}

// ---- 2. the HLSL reference ----
void Reference(Gpu& gpu, Fixture& fx, const std::string& hlsl) {
    const std::vector<DWORD> code = Compile(hlsl, "ps_3_0");
    Check(!code.empty(), "reference: lamp_index.hlsl compiles as ps_3_0");
    if (code.empty()) return;
    const std::string text = Disasm(code);
    std::printf("HLSL reference (stand-in host and the block): %d slots, %d temporaries\n", Slots(text), Temps(text));
    Check(Slots(text) > 0 && Slots(text) <= 512 && Temps(text) <= 32, "reference: within ps_3_0's 512 slots and 32 temporaries");
    Check(text.find("rep i") != std::string::npos || text.find("loop aL") != std::string::npos, "reference: the lamps are a loop");
    if (!gpu.dev) return;
    IDirect3DPixelShader9* ps = gpu.Ps(code);
    Check(ps != nullptr, "reference: native D3D9 accepts the looped shader");
    if (!ps) return;
    std::mt19937 rng(21);
    LampIndex::Frame fr;
    fr.lampStrength = 0.8f, fr.gateStrength = 0.5f, fr.groundStrength = 0.6f, fr.lotY = 10.0f;
    const Scene s = MakeScene(rng, 40, 60.0f, true, fr.lotY);
    LampIndex::Atlas a;
    LampIndex::Build(a, 1003.0f, 2001.0f, fr.lotY, [&](float x, float y, float z, bool inWall, LampIndex::PointPick& o) { Pick(s, x, y, z, inWall, o); });
    Upload(fx, a);
    float c[LampIndex::kConstRows][4];
    LampIndex::FrameConstants(a, fr, c);
    gpu.dev->SetPixelShaderConstantF(LampIndex::kFirstConst, c[0], LampIndex::kConstRows);
    const float host[4] = {kHost, kHost, kHost, 0};
    gpu.dev->SetPixelShaderConstantF(0, host, 1);
    gpu.dev->SetTexture(0, fx.white);
    gpu.dev->SetTexture(1, fx.ground);
    gpu.dev->SetTexture(2, fx.index);
    for (DWORD i = 0; i < 3; i++) PointClamp(gpu.dev, i);
    float N[3] = {0.3f, 0.85f, 0.4f};
    const float n = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
    for (float& v : N) v /= n;
    long compared = 0, over = 0, lit = 0;
    int worst = 0;
    for (int run = 0; run < 12; run++) {
        const Flags f{run % 3 == 1, run % 4 == 2};
        float row[4];
        LampIndex::ObjectRow(f.inWall, f.gate, row);
        gpu.dev->SetPixelShaderConstantF(1, row, 1);
        const float x0 = 975.013f + 4.7f * run, z0 = 1978.031f + 3.9f * run, y = fr.lotY + 0.1f * run, size = 6.0f;
        SetQuad(gpu.dev, x0, z0, y, size);
        const std::vector<DWORD> px = gpu.Render(ps, fx.vs);
        for (int j = 0; j < 64; j++)
            for (int i = 0; i < 64; i++) {
                const float P[3] = {x0 + size * i / 64.0f, y, z0 + size * j / 64.0f}; // (D3D9: pixel i reads uv i / 64)
                // a pixel within 3 mm of a cell border may fall in either cell on the GPU
                const float bx = std::fabs(std::remainder(P[0] - a.ox, LampIndex::kCell)), bz = std::fabs(std::remainder(P[2] - a.oz, LampIndex::kCell));
                if (bx < 0.003f || bz < 0.003f) continue;
                float want[3];
                ShadeIndex(a, f, fr, P, N, want);
                int d = 0;
                for (int ch = 0; ch < 3; ch++)
                    d = std::max(d, std::abs(static_cast<int>((px[j * 64 + i] >> (16 - 8 * ch)) & 255) - static_cast<int>(std::lround(want[ch] * 255.0f))));
                compared++, worst = std::max(worst, d);
                if (d > 2) over++;
                if (want[0] > kHost + 0.02f) lit++;
            }
    }
    std::printf("HLSL reference on the device against the CPU model: %ld pixels (%ld lit by a lamp or the ground), %ld over 2 LSB (worst %d)\n", compared, lit, over, worst);
    Check(over == 0 && compared > 30000 && lit > 5000, "reference: the device's pixels equal the CPU model within 2 LSB");
    gpu.dev->SetPixelShader(nullptr);
    Release(ps);
}

// ---- 3. the game's shaders ----
std::vector<std::vector<DWORD>> PixelShaders(const std::vector<unsigned char>& d) { // docs/engine/shaders.md
    std::vector<std::vector<DWORD>> out;
    size_t shdb = std::string::npos;
    for (size_t i = 0; i + 8 <= d.size() && shdb == std::string::npos; i++)
        if (std::memcmp(&d[i], "SHDB", 4) == 0) shdb = i;
    if (shdb == std::string::npos) return out;
    uint32_t size;
    std::memcpy(&size, &d[shdb + 4], 4);
    const size_t end = std::min(d.size(), shdb + 8 + size);
    for (size_t j = shdb + 8; j + 8 <= end;) {
        const bool v = std::memcmp(&d[j], "VSHD", 4) == 0, p = std::memcmp(&d[j], "PSHD", 4) == 0;
        if (!v && !p) {
            j++;
            continue;
        }
        uint32_t n;
        std::memcpy(&n, &d[j + 4], 4);
        const unsigned char* b = &d[j + 8];
        const size_t len = std::min<size_t>(n, d.size() - j - 8);
        for (size_t k = 0; p && k + 4 <= std::min<size_t>(len, 12); k += 4) {
            DWORD ver;
            std::memcpy(&ver, b + k, 4);
            if (ver != 0xFFFF0300u) continue;
            for (size_t q = k + 4; q + 4 <= len;) {
                DWORD tok;
                std::memcpy(&tok, b + q, 4);
                if (tok == 0x0000FFFFu) {
                    std::vector<DWORD> c((q + 4 - k) / 4);
                    std::memcpy(c.data(), b + k, c.size() * 4);
                    out.push_back(std::move(c));
                    break;
                }
                q += ((tok & 0xFFFF) == 0xFFFE) ? 4 + 4 * ((tok >> 16) & 0x7FFF) : 4 + 4 * ((tok >> 24) & 0xF);
            }
            break;
        }
        j += 8 + n;
    }
    return out;
}
// The host's declarations: false when the fixture cannot feed it. cube[s]: sampler s is a cube.
bool HostInputs(const std::vector<DWORD>& t, bool cube[16], bool& color1) {
    bool tc7 = false;
    for (size_t i = 1; i < t.size();) {
        const DWORD tok = t[i];
        if (tok == 0x0000FFFFu) break;
        if ((tok & 0xFFFF) == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const size_t len = (tok >> 24) & 0xF;
        if ((tok & 0xFFFF) == 0x1F && len == 2) {
            const DWORD use = t[i + 1] & 0x1F, idx = (t[i + 1] >> 16) & 0xF, r = t[i + 2], type = ((r >> 28) & 7) | (((r >> 11) & 3) << 3);
            if (type == 10) {
                const DWORD kind = (t[i + 1] >> 27) & 0xF;
                if (kind != 2 && kind != 3) return false;
                cube[r & 15] = kind == 3;
            } else if (type != 1 || !((use == 5 && idx <= 7) || (use == 10 && idx <= 1)))
                return false;
            else
                tc7 |= use == 5 && idx == 7, color1 |= use == 10 && idx == 1;
        }
        i += 1 + len;
    }
    return !(tc7 && color1);
}
void GameShaders(Gpu& gpu, Fixture& fx, const std::vector<std::vector<DWORD>>& shaders) {
    std::set<std::vector<DWORD>> seen;
    int hosts = 0, taken = 0, refused = 0, accepted = 0, slotsOk = 0, plus16 = 0, worstHostTemps = 0, worstCopyTemps = 0, worstSlots = 0, drawn = 0, sensitive = 0, equal = 0;
    int worstDiff = 0;
    std::mt19937 rng(31);
    LampIndex::Frame fr;
    fr.lampStrength = 0.8f, fr.gateStrength = 0.5f, fr.groundStrength = 0.6f, fr.lotY = 10.0f;
    fr.groundUv[2] = fr.groundUv[3] = 0.5f;
    // two samples: five lamps on open ground under a 6 m quad over nine cells; walls, with the quad inside one cell
    Scene open = MakeScene(rng, 5, 8.0f, false, fr.lotY), walled = MakeScene(rng, 12, 14.0f, true, fr.lotY);
    LampIndex::Atlas atlas[2];
    const Scene* scene[2] = {&open, &walled};
    for (int k = 0; k < 2; k++)
        LampIndex::Build(atlas[k], 1003.0f, 2001.0f, fr.lotY, [&](float x, float y, float z, bool inWall, LampIndex::PointPick& o) { Pick(*scene[k], x, y, z, inWall, o); });
    for (const std::vector<DWORD>& code : shaders) {
        std::vector<DWORD> plain = code, index = code;
        ShaderPatches::ObjectLampPatch pp, ip;
        if (!seen.insert(code).second || !ShaderPatches::PatchObjectLampPs(plain, pp)) continue;
        hosts++;
        const int hostTemps = Temps(Disasm(code));
        worstHostTemps = std::max(worstHostTemps, hostTemps);
        if (hostTemps + 16 <= 32) plus16++;
        if (!ShaderPatches::PatchObjectLampPs(index, ip, false, true)) {
            refused++;
            const std::string host = Disasm(code);
            int top = -1;
            for (int sm = 0; sm < 16; sm++)
                if (host.find(" s" + std::to_string(sm) + "\n") != std::string::npos) top = sm;
            std::printf("  index copy refused, the per-draw copy keeps drawing it: %d slots, %d temporaries, highest sampler s%d (no room, or an integer constant of its own)\n", Slots(host), hostTemps, top);
            continue;
        }
        taken++;
        const std::string text = Disasm(index);
        const int slots = Slots(text);
        if (slots > 0 && slots <= 512) slotsOk++;
        worstSlots = std::max(worstSlots, slots);
        worstCopyTemps = std::max(worstCopyTemps, Temps(text));
        if (!gpu.dev) continue;
        IDirect3DPixelShader9 *a = gpu.Ps(plain), *b = gpu.Ps(index);
        if (b) accepted++;
        bool cube[16] = {}, color1 = false;
        if (a && b && HostInputs(code, cube, color1)) {
            IDirect3DVertexShader9* hostVs = color1 ? fx.vs1 : fx.vs;
            drawn++;
            for (DWORD s = 0; s < 16; s++) {
                gpu.dev->SetTexture(s, cube[s] ? static_cast<IDirect3DBaseTexture9*>(fx.cube) : fx.white);
                PointClamp(gpu.dev, s);
            }
            float all[40][4];
            for (auto& r : all) r[0] = r[1] = r[2] = r[3] = 0.25f;
            std::memset(all[5], 0, sizeof(float) * 12); // the rig's lamps, zeroed by the draw
            bool hostEqual = true, hostSensitive = false;
            for (int k = 0; k < 2; k++)
                for (int flag = 0; flag < (k ? 4 : 1); flag++) {
                    const Flags f{(flag & 1) != 0, (flag & 2) != 0};
                    // the object's point: the middle of the quad; k = 1: a cell's centre, the quad inside that cell
                    int cx, cz;
                    LampIndex::CellOf(atlas[k], 1000.3f, 2000.4f, cx, cz);
                    const float size = k ? 1.9f : 6.0f, y = fr.lotY + 0.3f;
                    const float at[3] = {atlas[k].ox + (cx + 0.5f) * LampIndex::kCell, y, atlas[k].oz + (cz + 0.5f) * LampIndex::kCell};
                    SetQuad(gpu.dev, at[0] - 0.5f * size, at[2] - 0.5f * size, y, size);
                    LampIndex::PointPick pick;
                    Pick(*scene[k], at[0], at[1], at[2], f.inWall, pick);
                    // the per-draw fix's rows (DrawObjectLamp): parameters, 8 x (pos, 1/R^2) (colour x visibility, spill), 8 x cones
                    float lamps[33][4] = {};
                    lamps[0][0] = 1.0f, lamps[0][1] = f.gate ? fr.gateStrength : fr.lampStrength, lamps[0][3] = 1e-4f;
                    for (int s = 0; s < 8; s++) {
                        lamps[1 + 2 * s][0] = lamps[1 + 2 * s][2] = 1e6f;
                        lamps[2 + 2 * s][3] = lamps[17 + 2 * s][3] = lamps[18 + 2 * s][3] = 1.0f;
                    }
                    for (int s = 0; s < pick.n; s++) {
                        std::memcpy(lamps[1 + 2 * s], pick.rows[s], 32);
                        for (int ch = 0; ch < 3; ch++) lamps[2 + 2 * s][ch] *= pick.vis[s];
                        std::memcpy(lamps[17 + 2 * s], pick.rows[s] + 8, 32);
                    }
                    const float height = pick.top < 1e8f ? Sat(1.0f - (y - pick.top) / fr.fadeAbove) : 1.0f;
                    const float pool = Sat((fr.poolNone - std::fabs(y - fr.lotY)) / (fr.poolNone - fr.poolFull));
                    const float strength[4] = {fr.groundStrength * pick.share * height * pool, 0, 0, 0};
                    gpu.dev->SetPixelShaderConstantF(0, all[0], 40);
                    gpu.dev->SetPixelShaderConstantF(pp.atlasConst, fr.groundUv, 1);
                    gpu.dev->SetPixelShaderConstantF(pp.strengthConst, strength, 1);
                    gpu.dev->SetPixelShaderConstantF(pp.lampParamConst, lamps[0], 33);
                    gpu.dev->SetTexture(pp.atlasSampler, fx.ground);
                    const std::vector<DWORD> perDraw = gpu.Render(a, hostVs);
                    if (k == 0) { // the lamps and the ground light show in this host's picture
                        const float off[4] = {};
                        gpu.dev->SetPixelShaderConstantF(pp.strengthConst, off, 1);
                        gpu.dev->SetPixelShaderConstantF(pp.lampParamConst + 1, off, 1); // lamp 0 moved to the origin: out of reach
                        float dark[4] = {1.0f, 0.0f, 0.0f, 1e-4f};
                        gpu.dev->SetPixelShaderConstantF(pp.lampParamConst, dark, 1);
                        int differing = 0;
                        WorstDiff(perDraw, gpu.Render(a, hostVs), &differing);
                        hostSensitive = differing > 400;
                    }
                    gpu.dev->SetTexture(pp.atlasSampler, cube[pp.atlasSampler] ? static_cast<IDirect3DBaseTexture9*>(fx.cube) : fx.white);
                    // the index copy: the frame block, one row, two textures
                    Upload(fx, atlas[k]);
                    float c[LampIndex::kConstRows][4], row[4];
                    LampIndex::FrameConstants(atlas[k], fr, c);
                    LampIndex::ObjectRow(f.inWall, f.gate, row);
                    gpu.dev->SetPixelShaderConstantF(0, all[0], 40);
                    gpu.dev->SetPixelShaderConstantF(LampIndex::kFirstConst, c[0], LampIndex::kConstRows);
                    gpu.dev->SetPixelShaderConstantF(ip.objectConst, row, 1);
                    gpu.dev->SetTexture(ip.atlasSampler, fx.ground);
                    gpu.dev->SetTexture(ip.indexSampler, fx.index);
                    const int d = WorstDiff(perDraw, gpu.Render(b, hostVs));
                    worstDiff = std::max(worstDiff, d);
                    if (d > 1) hostEqual = false;
                }
            if (hostEqual) equal++;
            if (hostSensitive) sensitive++;
        }
        Release(a);
        Release(b);
    }
    std::printf("Game object shaders the per-draw patch takes: %d | index copy taken %d, refused %d | within 512 slots %d (most %d) | host temporaries most %d, "
                "hosts with + 16 <= 32: %d | the copy's temporaries most %d\n",
                hosts, taken, refused, slotsOk, worstSlots, worstHostTemps, plus16, worstCopyTemps);
    Check(hosts > 20, "game shaders: the precomp has object shaders the per-draw patch takes");
    Check(refused * 50 <= hosts, "game shaders: at most 2% of them refuse the index block (they keep the per-draw copy)");
    Check(slotsOk == taken && worstCopyTemps <= 32, "game shaders: every index copy within 512 slots and 32 temporaries");
    Check(plus16 == hosts, "game shaders: host temporaries + 16 <= 32 (the cost table's bound for the HLSL form)");
    if (!gpu.dev) return;
    std::printf("  native D3D9 accepts %d of %d index copies | drawn %d: equal to the per-draw copy within 1 LSB %d (worst %d LSB), pictures the lamps change %d\n", accepted, taken,
                drawn, equal, worstDiff, sensitive);
    Check(accepted == taken, "game shaders: native D3D9 accepts every index copy");
    Check(drawn > 20 && equal == drawn, "game shaders: the index copy draws the per-draw copy's pixels (1 LSB)");
    Check(sensitive * 2 > drawn, "game shaders: the lamps show in most of the compared pictures");
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::puts("usage: check <lamp_index.hlsl> [Shaders_Win32.precomp]");
        return 2;
    }
    Transport();
    std::ifstream h(argv[1], std::ios::binary);
    const std::string hlsl((std::istreambuf_iterator<char>(h)), {});
    Check(!hlsl.empty(), "lamp_index.hlsl read");
    Gpu gpu;
    Fixture fx;
    if (gpu.dev && !MakeFixture(gpu, fx)) Check(false, "device fixture (float texture, cube, vertex shader)");
    if (!gpu.dev) std::puts("SKIP device checks: no native D3D9 device");
    Reference(gpu, fx, hlsl);
    if (argc > 2) {
        std::ifstream f(argv[2], std::ios::binary);
        const std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), {});
        GameShaders(gpu, fx, PixelShaders(d));
    } else
        std::puts("SKIP game shaders: no precomp given");
    if (gpu.dev) {
        for (DWORD s = 0; s < 16; s++) gpu.dev->SetTexture(s, nullptr);
        gpu.dev->SetPixelShader(nullptr);
        gpu.dev->SetVertexShader(nullptr);
    }
    fx.Free();
    std::printf("%d checks, %d failures\n%s\n", g_checks, g_failures, g_failures ? "PROBLEMS" : "OK");
    return g_failures ? 1 : 0;
}
