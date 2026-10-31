// Offline checks of Walls shade lamp light near the ground on the GPU: the production engine (features/ground_shade_gpu.cpp)
// and shaders (shaders/ground_shade_hlsl.h) against the CPU (features/ground_lamp_shadow.h) on a D3D9 HAL device with a
// hidden window. Lots: ShadeJob's bytes and stamp weight, the lowest byte, the lot mask; the scenes of tools/ground_shade_test
// plus mesh fences and crowns, the edge rules, the penumbra off, a stress lot. Chunks: ChunkFull at every level from CPU
// shades, changed-box updates against the whole chunk made again (bit for bit, every level), the lot-to-chunk chain from the
// GPU's own ext textures, both chunk formats, and the readers' sampler states (the terrain and road copies read the chunk's
// red channel with LINEAR / LINEAR / LINEAR mips and CLAMP; the lot passes read the mask's rgb and alpha with LINEAR and no
// mips). Read-only: prints only.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <random>
#include <string>
#include <vector>
#define main ground_shade_cpu_main
#include "../ground_shade_test/check.cpp"
#undef main
#include "features/ground_shade_gpu.h"
#include "shaders/ground_shade_hlsl.h"

using namespace GroundShadow;
using Patch = WallOpenings::Patch;
using Crown = Canopy::Ellipsoid;

// ---------------------------------------------------------------- scenes
struct GScene {
    std::string name;
    Grid grid;
    std::vector<Lamp> lamps, others;
    std::vector<Wall> walls;
    std::vector<Patch> patches;
    std::vector<Crown> crowns;
    bool penumbra = true;
    int minShadeRow = 0; // the CPU's first shaded grid row must be at least this (the scene's purpose holds)
};
static GScene FromCpuScene(const Scene& s, const char* name) {
    GScene g;
    g.name = name, g.grid = s.grid, g.lamps = s.lamps, g.others = s.others, g.walls = s.walls, g.patches = s.patches;
    return g;
}
static Crown MakeCrown(float x, float z, float base, float rx, float ry, float rz, float yaw) {
    const float cy = base + ry;
    return Crown{x, cy, z, rx, ry, rz, std::cos(yaw), std::sin(yaw), Canopy::LadFor(cy + ry - 10.0f)}; // (the scenes' ground is at y 10)
}
// The 40 m lot turned 30 degrees, soft, with mesh fences (pass 0.35) on half the garden walls and six crowns
static GScene CrownScene() {
    GScene g = FromCpuScene(MakeScene(0.5235988f, true), "40 m lot, 30 deg, soft, mesh fences + 6 crowns");
    for (size_t k = 10; k < 15 && k < g.walls.size(); k++) g.walls[k].pass = 0.35f;
    const float c = std::cos(0.5235988f), s = std::sin(0.5235988f);
    const float at[6][5] = {{8, 8, 2.0f, 3.0f, 2.5f}, {30, 22, 1.5f, 2.5f, 2.0f}, {18, 30, 0.0f, 1.2f, 0.9f}, {6, 30, 0.0f, 1.0f, 0.8f}, {34, 32, 2.5f, 3.0f, 2.8f}, {26, 8, 0.0f, 1.5f, 1.0f}};
    for (const auto& a : at) {
        const float x = c * a[0] - s * a[1] + 1000.0f, z = s * a[0] + c * a[1] + 2000.0f;
        g.crowns.push_back(MakeCrown(x, z, 10.0f + a[2], a[3], a[4], a[3] * 0.8f, a[0] * 0.3f));
    }
    return g;
}
// The rules no other scene reaches: a sconce 0.08 m off its own wall (kLampSkip), a raised foundation (the point on its
// base), an upper-story wall (the point at the ground), a wall over a lamp's head and one under the ground (culled), a lamp
// on a balcony 5 m up that sees the upper wall
static GScene EdgeScene(bool soft) {
    GScene g = FromCpuScene(MakeScene(0.5235988f, soft), soft ? "40 m lot, 30 deg, edge rules, soft on" : "40 m lot, 30 deg, edge rules, soft off");
    const float c = std::cos(0.5235988f), s = std::sin(0.5235988f), ty = 10.0f;
    const auto world = [&](float lx, float lz, float& x, float& z) { x = c * lx - s * lz + 1000.0f, z = s * lx + c * lz + 2000.0f; };
    const auto wall = [&](float ax, float az, float bx, float bz, float y0, float y1) {
        Wall w;
        world(ax, az, w.x0, w.z0), world(bx, bz, w.x1, w.z1), w.y0 = y0, w.y1 = y1;
        g.walls.push_back(w);
    };
    wall(26, 14, 32, 14, ty + 0.6f, ty + 2.6f);
    wall(26, 20, 34, 20, ty + 3.0f, ty + 6.0f);
    wall(6, 6, 12, 6, ty + 2.6f, ty + 5.0f);
    wall(30, 32, 36, 32, ty - 5.0f, ty - 2.5f);
    Lamp l = g.lamps[0];
    const auto lamp = [&](float lx, float lz, float h, float k) {
        world(lx, lz, l.x, l.z), l.y = ty + h, l.k = k;
        l.rx0 = l.x - 20.0f, l.rx1 = l.x + 20.0f, l.rz0 = l.z - 20.0f, l.rz1 = l.z + 20.0f;
        g.lamps.push_back(l);
    };
    lamp(9.92f, 14.5f, 2.0f, 6.0f);
    lamp(30.0f, 23.0f, 5.0f, 9.0f);
    return g;
}
// 32 tested lamps, 96 others, 150 walls with windows, doors and arches, 40 fence walls (half mesh), 20 crowns; an 88 m grid
static GScene StressScene(bool soft) {
    GScene g;
    g.name = std::string("stress 88 m grid, soft ") + (soft ? "on" : "off");
    std::mt19937 rng(9173);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    const float turn = 0.2967f, c = std::cos(turn), sn = std::sin(turn), tx = 3000.0f, tz = 1500.0f, ty = 10.0f;
    const auto world = [&](float lx, float lz, float& x, float& z) { x = c * lx - sn * lz + tx, z = sn * lx + c * lz + tz; };
    const auto lamp = [&](float lx, float lz, float h, float k, bool lift) {
        Lamp l;
        world(lx, lz, l.x, l.z);
        l.y = ty + h, l.k = k, l.liftable = lift;
        l.rx0 = l.x - 20.0f, l.rx1 = l.x + 20.0f, l.rz0 = l.z - 20.0f, l.rz1 = l.z + 20.0f;
        return l;
    };
    for (int i = 0; i < 32; i++) g.lamps.push_back(lamp(U(rng) * 40.0f, U(rng) * 40.0f, i % 4 == 0 ? 0.1f + U(rng) * 0.3f : 0.8f + U(rng) * 2.2f, 2.0f + U(rng) * 8.0f, i % 4 == 0));
    for (int i = 0; i < 96; i++) g.others.push_back(lamp(-20.0f + U(rng) * 80.0f, -20.0f + U(rng) * 80.0f, 1.5f + U(rng), 2.0f + U(rng) * 6.0f, false));
    const auto patch = [&](int kind) {
        Patch p;
        const float u0 = 0.2f + U(rng) * 0.3f, u1 = u0 + 0.25f;
        p.u0 = u0, p.u1 = u1, p.ou0 = u0 + 0.03f, p.ou1 = u1 - 0.03f, p.open = 1, p.jamb0 = p.jamb1 = 1;
        p.top0 = p.top1 = ty + 3.0f;
        p.sill = kind == 1 ? ty + 0.9f : ty, p.floorSill = kind == 1 ? 0 : 1, p.glass = kind == 1 ? WallOpenings::kWindowPass : 1.0f;
        for (int k = 0; k < WallOpenings::kProfile; k++) p.head[k] = ty + (kind == 1 ? 2.1f : 2.2f) + (kind == 3 ? 0.4f * std::sin(3.14159f * k / 4.0f) : 0.0f);
        return p;
    };
    const auto wall = [&](float ax, float az, float bx, float bz, float h, int kind, float pass = 0.0f) {
        Wall w;
        world(ax, az, w.x0, w.z0);
        world(bx, bz, w.x1, w.z1);
        w.y0 = ty, w.y1 = ty + h, w.pass = pass;
        if (kind) {
            w.p0 = static_cast<uint32_t>(g.patches.size());
            g.patches.push_back(patch(kind));
            if (kind == 1 && U(rng) < 0.5f) {
                Patch q;
                q.u0 = 0.7f, q.u1 = 0.9f, q.top0 = ty + 1.0f, q.top1 = ty + 1.4f;
                g.patches.push_back(q);
            }
            w.pn = static_cast<uint32_t>(g.patches.size()) - w.p0;
        }
        g.walls.push_back(w);
    };
    for (int b = 0; b < 12; b++) {
        const float x0 = 1.0f + (b % 4) * 9.8f, z0 = 1.0f + (b / 4) * 13.0f, x1 = x0 + 7.0f, z1 = z0 + 9.0f;
        const float cx[4] = {x0, x1, x1, x0}, cz[4] = {z0, z0, z1, z1};
        for (int s = 0; s < 4; s++) {
            const float ax = cx[s], az = cz[s], bx = cx[(s + 1) % 4], bz = cz[(s + 1) % 4];
            for (int r = 0; r < 3; r++)
                wall(ax + (bx - ax) * r / 3.0f, az + (bz - az) * r / 3.0f, ax + (bx - ax) * (r + 1) / 3.0f, az + (bz - az) * (r + 1) / 3.0f, 3.0f, r == 1 ? 1 + (b + s) % 3 : (s + r) % 2);
        }
    }
    for (int k = 0; k < 6; k++) {
        const float x = U(rng) * 36.0f, z = U(rng) * 36.0f;
        wall(x, z, x + 4.0f, z + (k % 2 ? 0.0f : 2.0f), 2.5f, 3);
    }
    for (int f = 0; f < 10; f++) {
        const float x = -4.0f + U(rng) * 44.0f, z = -4.0f + U(rng) * 44.0f, a = (f % 3) * 1.5707963f + (f == 7 ? 0.6f : 0.0f);
        const float dx = 2.0f * std::cos(a), dz = 2.0f * std::sin(a), h = 1.0f + U(rng) * 0.6f;
        for (int s = 0; s < 4; s++) wall(x + dx * s, z + dz * s, x + dx * (s + 1), z + dz * (s + 1), h, f == 2 && s == 1 ? 2 : 0, f < 5 ? 0.35f : 0.0f);
    }
    for (int k = 0; k < 20; k++) {
        float x, z;
        world(-6.0f + U(rng) * 52.0f, -6.0f + U(rng) * 52.0f, x, z);
        const bool bush = k % 3 == 0;
        g.crowns.push_back(MakeCrown(x, z, ty + (bush ? 0.0f : 1.5f + U(rng) * 2.0f), bush ? 0.6f + U(rng) : 1.5f + U(rng) * 2.0f, bush ? 0.5f + U(rng) * 0.5f : 1.2f + U(rng) * 2.0f,
                                     bush ? 0.6f + U(rng) : 1.5f + U(rng) * 2.0f, U(rng) * 6.28f));
    }
    Grid& gr = g.grid;
    gr.w = gr.h = 352, gr.m0 = c, gr.m2 = sn, gr.m8 = -sn, gr.m10 = c, gr.ty = ty, gr.soft = soft;
    world(-24.0f, -24.0f, gr.tx, gr.tz);
    return g;
}

// One lamp with n occluders in its reach (past the 255 a ShadePS row takes: the lamp spans rows): short walls, most mesh
// fences (light passes, so every texel tests them all), some with openings, and crowns. pad: that many far walls, patches and
// crowns first, so the near ones sit past kWrapRows (the wrapped input textures)
static GScene CrowdScene(int n, bool soft, size_t pad = 0) {
    GScene g;
    g.name = std::format("one lamp, {} occluders in reach, soft {}{}", n, soft ? "on" : "off", pad ? std::format(", {} far ones first (inputs over 4096 rows)", pad) : "");
    std::mt19937 rng(4242 + n);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    const float ty = 10.0f, cx = 500.0f, cz = 700.0f;
    Lamp l;
    l.x = cx, l.z = cz, l.y = ty + 2.5f, l.k = 10.0f;
    l.rx0 = l.x - 20.0f, l.rx1 = l.x + 20.0f, l.rz0 = l.z - 20.0f, l.rz1 = l.z + 20.0f;
    g.lamps.push_back(l);
    const float R = Reach(l, ty);
    for (size_t k = 0; k < pad; k++) {
        Wall w{cx + 900.0f, cz + k * 0.01f, cx + 901.0f, cz + k * 0.01f, ty, ty + 2.0f};
        w.p0 = static_cast<uint32_t>(g.patches.size()), w.pn = 1;
        g.patches.push_back(Opening(0.3f, 0.6f, ty, ty + 1.5f));
        g.walls.push_back(w);
        g.crowns.push_back(MakeCrown(cx - 900.0f, cz + k * 0.01f, ty, 1.0f, 1.0f, 1.0f, 0.0f));
    }
    const int crowns = n >= 1000 ? 300 : n / 10;
    for (int k = 0; k < n - crowns; k++) {
        const float a = U(rng) * 6.2832f, d = 1.0f + U(rng) * (R - 2.0f), t = U(rng) * 3.1416f, len = 0.5f + U(rng) * 1.5f;
        const float x = cx + d * std::cos(a), z = cz + d * std::sin(a);
        Wall w{x, z, x + len * std::cos(t), z + len * std::sin(t), ty, ty + 0.8f + U(rng) * 2.0f};
        w.pass = k % 5 == 0 ? 0.0f : 0.85f;
        if (k % 7 == 0) {
            w.p0 = static_cast<uint32_t>(g.patches.size()), w.pn = 1;
            g.patches.push_back(Opening(0.3f, 0.7f, ty, ty + 0.7f));
        }
        g.walls.push_back(w);
    }
    for (int k = 0; k < crowns; k++) {
        const float a = U(rng) * 6.2832f, d = 1.5f + U(rng) * (R - 2.0f);
        g.crowns.push_back(MakeCrown(cx + d * std::cos(a), cz + d * std::sin(a), ty + U(rng) * 1.5f, 0.4f + U(rng) * 0.6f, 0.4f + U(rng) * 0.6f, 0.4f + U(rng) * 0.6f, U(rng) * 6.28f));
    }
    Grid& gr = g.grid;
    gr.w = gr.h = static_cast<int>(std::ceil(2.0f * (R + 1.0f) * kTexelsPerMetre));
    gr.m0 = gr.m10 = 1.0f, gr.m2 = gr.m8 = 0.0f, gr.ty = ty, gr.soft = soft;
    gr.tx = cx - (R + 1.0f), gr.tz = cz - (R + 1.0f);
    return g;
}

// ---------------------------------------------------------------- the device
static IDirect3D9* g_d3d = nullptr;
static IDirect3DDevice9* g_dev = nullptr;
static void* g_shaders[GroundShadeGpu::kShaderCount] = {};
static IDirect3DPixelShader9* g_readPs = nullptr; // the readers' check
static RtLampGpu::DeviceCalls PlainCalls() {
    RtLampGpu::DeviceCalls c;
    c.SetTexture = [](IDirect3DDevice9* d, DWORD s, IDirect3DBaseTexture9* t) { return d->SetTexture(s, t); };
    c.SetRenderTarget = [](IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) { return d->SetRenderTarget(i, s); };
    c.SetDepthStencil = [](IDirect3DDevice9* d, IDirect3DSurface9* s) { return d->SetDepthStencilSurface(s); };
    c.GetDepthStencil = [](IDirect3DDevice9* d, IDirect3DSurface9** s) { return d->GetDepthStencilSurface(s); };
    c.SetVertexShader = [](IDirect3DDevice9* d, IDirect3DVertexShader9* s) { return d->SetVertexShader(s); };
    c.SetPixelShader = [](IDirect3DDevice9* d, IDirect3DPixelShader9* s) { return d->SetPixelShader(s); };
    c.SetVsConst = [](IDirect3DDevice9* d, UINT r, const float* f, UINT n) { return d->SetVertexShaderConstantF(r, f, n); };
    c.SetPsConst = [](IDirect3DDevice9* d, UINT r, const float* f, UINT n) { return d->SetPixelShaderConstantF(r, f, n); };
    c.DrawPrimitive = [](IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT s, UINT n) { return d->DrawPrimitive(t, s, n); };
    c.SetViewport = [](IDirect3DDevice9* d, const D3DVIEWPORT9* v) { return d->SetViewport(v); };
    return c;
}
static bool MakeDevice() {
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ApexGroundShadeGpu";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA(wc.lpszClassName, "ground shade gpu", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d || !wnd) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE, pp.SwapEffect = D3DSWAPEFFECT_DISCARD, pp.BackBufferWidth = 64, pp.BackBufferHeight = 64, pp.hDeviceWindow = wnd;
    pp.EnableAutoDepthStencil = TRUE, pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    const HRESULT hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &g_dev);
    D3DADAPTER_IDENTIFIER9 id{};
    g_d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
    char path[MAX_PATH] = {};
    GetModuleFileNameA(GetModuleHandleA("d3d9.dll"), path, MAX_PATH);
    std::printf("device: %s | adapter %s | d3d9.dll %s\n", SUCCEEDED(hr) ? "HAL" : "none", id.Description, path);
    return SUCCEEDED(hr) && g_dev;
}
static bool CompileAll() {
    for (int i = 0; i < GroundShadeGpu::kShaderCount; i++) {
        const auto id = static_cast<GroundShadeGpu::ShaderId>(i);
        ID3DBlob *code = nullptr, *err = nullptr;
        const auto t0 = std::chrono::steady_clock::now();
        const HRESULT hr = D3DCompile(kGroundShadeHlsl, std::strlen(kGroundShadeHlsl), "ground_shade", nullptr, nullptr, GroundShadeGpu::EntryOf(id),
                                      GroundShadeGpu::IsVertex(id) ? "vs_3_0" : "ps_3_0", GroundShadeGpu::FlagsOf(id), 0, &code, &err);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (FAILED(hr)) {
            std::printf("compile %s failed: %s\n", GroundShadeGpu::EntryOf(id), err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return false;
        }
        ID3DBlob* dis = nullptr;
        if (SUCCEEDED(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr, &dis))) {
            const char* t = static_cast<const char*>(dis->GetBufferPointer());
            const char* a = std::strstr(t, "// approximately");
            std::printf("shader %s: %.*s (compile %.0f ms)\n", GroundShadeGpu::EntryOf(id), a ? static_cast<int>(std::strcspn(a, "\r\n")) : 0, a ? a : "", ms);
            dis->Release();
        }
        HRESULT cr;
        if (GroundShadeGpu::IsVertex(id)) cr = g_dev->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()), reinterpret_cast<IDirect3DVertexShader9**>(&g_shaders[i]));
        else cr = g_dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), reinterpret_cast<IDirect3DPixelShader9**>(&g_shaders[i]));
        code->Release();
        if (FAILED(cr)) return std::printf("create %s failed\n", GroundShadeGpu::EntryOf(id)), false;
    }
    // the readers: a chunk read at (uv, lod) through LINEAR / LINEAR / LINEAR mips (r), a mask read LINEAR (rgb and a)
    const char* src = "sampler2D s0 : register(s0); float4 c0 : register(c0); float4 c1 : register(c1);\n"
                      "float4 main(float2 vpos : VPOS) : COLOR { float k = floor(vpos.x + 0.25) + 64.0 * floor(vpos.y + 0.25);\n"
                      " float2 uv = frac(float2(k * 0.6180339, k * 0.7548776) + c0.xy) * c0.zw - 0.25 * c0.zw + 0.5 * (1 - c0.zw);\n"
                      " float4 t = tex2Dlod(s0, float4(uv, 0, c1.x > 0.5 ? frac(k * 0.3819660) * 10.0 : 0.0)); return c1.y > 0.5 ? t : t.rrrr; }";
    ID3DBlob *code = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(src, std::strlen(src), "reader", nullptr, nullptr, "main", "ps_3_0", 0, 0, &code, &err))) return std::printf("reader compile failed\n"), false;
    g_dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &g_readPs);
    code->Release();
    return g_readPs != nullptr;
}
static IDirect3DSurface9* Level(IDirect3DTexture9* t, UINT l = 0) {
    IDirect3DSurface9* s = nullptr;
    t->GetSurfaceLevel(l, &s);
    if (s) s->Release(); // (the texture keeps it)
    return s;
}
// A texture level's texels (DEFAULT through GetRenderTargetData, MANAGED by lock); bytes per texel bpp
static std::vector<uint8_t> ReadLevel(IDirect3DTexture9* t, UINT l, int& w, int& h, int& bpp) {
    D3DSURFACE_DESC d{};
    t->GetLevelDesc(l, &d);
    w = static_cast<int>(d.Width), h = static_cast<int>(d.Height);
    bpp = d.Format == D3DFMT_L8 ? 1 : d.Format == D3DFMT_G32R32F ? 8 : d.Format == D3DFMT_A32B32G32R32F ? 16 : 4;
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * bpp);
    IDirect3DTexture9* sys = nullptr;
    IDirect3DSurface9* ss = nullptr;
    if (d.Pool == D3DPOOL_DEFAULT) {
        if (FAILED(g_dev->CreateTexture(d.Width, d.Height, 1, 0, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) || !sys) return {};
        sys->GetSurfaceLevel(0, &ss);
        if (FAILED(g_dev->GetRenderTargetData(Level(t, l), ss))) return ss->Release(), sys->Release(), std::vector<uint8_t>{};
    } else
        t->GetSurfaceLevel(l, &ss);
    D3DLOCKED_RECT lr{};
    ss->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    for (int y = 0; y < h; y++) std::memcpy(&out[static_cast<size_t>(y) * w * bpp], static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, static_cast<size_t>(w) * bpp);
    ss->UnlockRect();
    ss->Release();
    if (sys) sys->Release();
    return out;
}
// A chunk level as one byte a texel (r of an A8R8G8B8 fallback)
static std::vector<uint8_t> ChunkLevel(IDirect3DTexture9* t, UINT l) {
    int w, h, bpp;
    std::vector<uint8_t> raw = ReadLevel(t, l, w, h, bpp);
    if (bpp == 1) return raw;
    std::vector<uint8_t> out(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < out.size() && i * bpp + 2 < raw.size(); i++) out[i] = raw[i * bpp + 2];
    return out;
}
static void Wait() {
    IDirect3DQuery9* q = nullptr;
    g_dev->CreateQuery(D3DQUERYTYPE_EVENT, &q);
    q->Issue(D3DISSUE_END);
    while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {}
    q->Release();
}
static double Ms(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
static void GridBox(const Grid& g, float& x0, float& z0, float& x1, float& z1) {
    x0 = z0 = 1e30f, x1 = z1 = -1e30f;
    const float W = g.w / kTexelsPerMetre, H = g.h / kTexelsPerMetre;
    for (const float lx : {0.0f, W})
        for (const float lz : {0.0f, H}) {
            const float x = g.m0 * lx + g.m8 * lz + g.tx, z = g.m2 * lx + g.m10 * lz + g.tz;
            x0 = std::min(x0, x), x1 = std::max(x1, x), z0 = std::min(z0, z), z1 = std::max(z1, z);
        }
}

// ---------------------------------------------------------------- lots
struct LotOut {
    ExtSrc cpu;                    // the CPU's shade (for ChunkFull)
    IDirect3DTexture9* gpu = nullptr; // the GPU's ext texture
};
static LotOut RunLot(GroundShadeGpu::Engine& eng, const GScene& s, int cpuReps, int gpuReps) {
    LotOut out;
    Penumbra::g_on.store(s.penumbra);
    const int W = s.grid.w, H = s.grid.h;
    ShadeJob job;
    double cpuMs = 1e30;
    for (int r = 0; r < cpuReps; r++) {
        const auto t0 = std::chrono::steady_clock::now();
        job.Init(s.grid, s.lamps, s.walls, s.patches, s.crowns, s.others);
        job.Step([] { return true; });
        cpuMs = std::min(cpuMs, Ms(t0));
    }
    auto ext = std::make_shared<ExtData>(ExtData{job.Result(), job.StampWeight()});
    out.cpu.grid = s.grid, out.cpu.v = ext, out.cpu.lot = 1;
    GridBox(s.grid, out.cpu.x0, out.cpu.z0, out.cpu.x1, out.cpu.z1);
    GroundShadeGpu::Inputs in;
    std::string why, err;
    double prepMs = 1e30;
    bool fits = false;
    for (int r = 0; r < 5; r++) {
        const auto t0 = std::chrono::steady_clock::now();
        fits = GroundShadeGpu::Prepare(s.grid, s.lamps, s.others, s.walls, s.patches, s.crowns, in, why);
        prepMs = std::min(prepMs, Ms(t0));
    }
    Check(fits, "lot: within the shader's bounds");
    if (!fits) return std::printf("%s: %s\n", s.name.c_str(), why.c_str()), out;
    g_dev->BeginScene();
    eng.Begin(g_dev);
    IDirect3DQuery9* shades = nullptr;
    bool ok = eng.BuildLot(g_dev, in, out.gpu, err, &shades);
    eng.End(g_dev);
    Wait();
    double gpuMs = -1, wallMs = 0;
    for (int r = 0; r < gpuReps && ok; r++) {
        const auto t0 = std::chrono::steady_clock::now();
        eng.Begin(g_dev);
        ok = eng.BuildLot(g_dev, in, out.gpu, err);
        eng.End(g_dev);
        Wait();
        wallMs += Ms(t0) / gpuReps;
        for (int k = 0; k < 100 && (gpuMs = eng.LastGpuMs()) < 0; k++) Sleep(1);
    }
    g_dev->EndScene();
    Check(ok, "lot: BuildLot");
    if (!ok) return std::printf("%s: %s\n", s.name.c_str(), err.c_str()), out;
    int ew, eh, bpp;
    const std::vector<uint8_t> raw = ReadLevel(out.gpu, 0, ew, eh, bpp);
    const auto texel = [&](int i, int j, int c) { float f; std::memcpy(&f, &raw[(static_cast<size_t>(j) * ew + i) * 8 + c * 4], 4); return f; };
    int maxd = 0, differ = 0, over1 = 0, notByte = 0;
    double relW = 0;
    uint8_t lo = 255;
    int firstRow = -1;
    for (int j = 0; j < H; j++)
        for (int i = 0; i < W; i++) {
            const size_t k = static_cast<size_t>(j) * W + i;
            if (firstRow < 0 && ext->v[k] < 255) firstRow = j;
            const float v = texel(i, j, 0), w = texel(i, j, 1);
            notByte += v != std::floor(v) || v < 0 || v > 255;
            const int d = std::abs(static_cast<int>(v) - ext->v[k]);
            maxd = std::max(maxd, d), differ += d > 0, over1 += d > 1;
            relW = std::max(relW, std::fabs(double(w) - ext->w[k]) / std::max(1e-6, double(ext->w[k])));
            lo = std::min(lo, ext->v[k]);
        }
    const float flag = texel(W, 0, 0);
    int dark = 0;
    for (uint8_t v : ext->v) dark += v < 128;
    std::printf("\n== %s: grid %d x %d, %zu lamps + %zu others, %zu walls (%zu patches), %zu crowns, %d lamp-occluder pairs (%d lamp rows), %d dark texels\n", s.name.c_str(), W, H,
                s.lamps.size(), s.others.size(), s.walls.size(), s.patches.size(), s.crowns.size(), in.entries, in.tested, dark);
    std::printf("  CPU ShadeJob %.2f ms | GPU build %.3f ms by timestamps (%.3f ms with the CPU's wait) | CPU setup (Prepare) %.3f ms\n", cpuMs, gpuMs, wallMs, prepMs);
    std::printf("  bytes vs ShadeJob::Result: max %d, %d of %d differ, %d by more than 1 | stamp weight max rel d %.3g | lowest byte %.0f (CPU %d)\n", maxd, differ, W * H, over1,
                relW, flag, lo);
    Check(notByte == 0, "lot: ext r holds whole bytes");
    Check(maxd <= 1 && differ * 1000 <= W * H, "lot: within 1/255 of the CPU and under 0.1 % of texels differ");
    Check(relW < 1e-3, "lot: the stamp weight as the CPU's (GPU square roots and divisions are not correctly rounded)");
    Check(std::fabs(flag - lo) <= 1.0f, "lot: the lowest byte at texel (w, 0)");
    {
        DWORD n = 0;
        while (shades && shades->GetData(&n, sizeof n, D3DGETDATA_FLUSH) == S_FALSE) {}
        std::printf("  occlusion query (a texel under 255): %lu (CPU: %s, first shaded row %d)\n", static_cast<unsigned long>(n), lo < 255 ? "yes" : "no", firstRow);
        if (s.minShadeRow) Check(firstRow >= s.minShadeRow, "lot: the scene's shade lies only past the query's first rows");
        Check(shades && (n > 0) == (lo < 255), "lot: the occlusion query says whether the grid shades a texel");
        if (shades) shades->Release();
    }
    // the lot mask: a lot part 4 texels in from the grid's edge, a room mask with rooms (0), soft edges and the outside (255)
    {
        const int ox = 4, oz = 4, w = W - 8, h = H - 8;
        std::vector<uint8_t> room(static_cast<size_t>(w) * h);
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) room[static_cast<size_t>(j) * w + i] = (i / 16 + j / 16) % 3 == 0 ? 0 : (i / 16 + j / 16) % 3 == 1 ? 255 : static_cast<uint8_t>((i * 7 + j * 3) & 255);
        IDirect3DTexture9* mask = nullptr;
        g_dev->BeginScene();
        eng.Begin(g_dev);
        const bool mok = eng.BuildMask(g_dev, out.gpu, ew, eh, ox, oz, w, h, room, mask, err);
        eng.End(g_dev);
        g_dev->EndScene();
        Check(mok, "mask: BuildMask");
        if (mok) {
            int mw, mh, mb;
            const std::vector<uint8_t> m = ReadLevel(mask, 0, mw, mh, mb);
            int rgbBad = 0, aBad = 0;
            for (int j = 0; j < h; j++)
                for (int i = 0; i < w; i++) {
                    const uint8_t* p = &m[(static_cast<size_t>(j) * mw + i) * 4]; // b g r a
                    const uint8_t l = room[static_cast<size_t>(j) * w + i];
                    const uint8_t a = l ? static_cast<uint8_t>(texel(ox + i, oz + j, 0)) : 255; // (MakeMaskTexture: 255 inside rooms)
                    rgbBad += p[0] != l || p[1] != l || p[2] != l;
                    aBad += p[3] != a;
                }
            std::printf("  mask (%d x %d): rgb as the room mask: %d texels differ; alpha as MakeMaskTexture from the ext bytes: %d differ\n", w, h, rgbBad, aBad);
            Check(rgbBad == 0 && aBad == 0, "mask: MakeMaskTexture's texels from the GPU's shade, bit for bit");
            mask->Release();
        }
    }
    Penumbra::g_on.store(true);
    return out;
}

// ---------------------------------------------------------------- chunks
static GroundShadeGpu::ChunkSrc SrcOf(const ExtSrc& e, IDirect3DTexture9* tex, float wx0, float wz0) {
    GroundShadeGpu::ChunkSrc s;
    s.ext = tex, s.grid = e.grid, s.box = ChunkRectOf(wx0, wz0, e.x0, e.z0, e.x1, e.z1);
    return s;
}
// every level of tex against ChunkFull's; returns the worst difference, *differ the texels that differ
static int CompareChunk(IDirect3DTexture9* tex, const ChunkTexture& full, int* differ, std::string* per = nullptr) {
    int worst = 0;
    *differ = 0;
    for (int l = 0; l < kChunkLevels; l++) {
        const std::vector<uint8_t> g = ChunkLevel(tex, static_cast<UINT>(l));
        const std::vector<uint8_t> want = full.levels.empty() ? std::vector<uint8_t>(g.size(), 255) : full.levels[l];
        int d = 0;
        for (size_t i = 0; i < g.size() && i < want.size(); i++) {
            const int x = std::abs(int(g[i]) - int(want[i]));
            worst = std::max(worst, x), d += x > 0;
        }
        if (g.size() != want.size()) worst = 999;
        *differ += d;
        if (per) *per += " L" + std::to_string(l) + ":" + std::to_string(d);
    }
    return worst;
}
static bool SameTextures(IDirect3DTexture9* a, IDirect3DTexture9* b) {
    for (int l = 0; l < kChunkLevels; l++)
        if (ChunkLevel(a, l) != ChunkLevel(b, l)) return false;
    return true;
}
static bool BuildChunk(GroundShadeGpu::Engine& eng, const std::vector<GroundShadeGpu::ChunkSrc>& srcs, float wx0, float wz0, TexRect r0, IDirect3DTexture9*& tex) {
    std::string err;
    g_dev->BeginScene();
    eng.Begin(g_dev);
    const bool ok = eng.BuildChunk(g_dev, srcs, wx0, wz0, r0, tex, err);
    eng.End(g_dev);
    g_dev->EndScene();
    if (!ok) std::printf("chunk build failed: %s\n", err.c_str());
    return ok;
}
static void ChunkRuns(GroundShadeGpu::Engine& eng, const char* fmtName) {
    const float wx0 = 512.0f, wz0 = 768.0f;
    std::mt19937 rng(20251);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    // the whole chunk from CPU shades (uploaded as the bridge uploads a CPU build's)
    {
        std::vector<ExtSrc> lots;
        for (uint64_t lot = 1; lot <= 14; lot++) lots.push_back(RandomLot(rng, lot * 7919, wx0, wz0));
        lots.push_back(RandomLot(rng, 99, wx0, wz0, 352));
        std::vector<IDirect3DTexture9*> tx(lots.size());
        std::vector<GroundShadeGpu::ChunkSrc> srcs;
        for (size_t k = 0; k < lots.size(); k++) {
            GroundShadeGpu::Engine::UploadExt(g_dev, lots[k].grid, lots[k].v->v, lots[k].v->w, tx[k]);
            srcs.push_back(SrcOf(lots[k], tx[k], wx0, wz0));
        }
        ChunkTexture full;
        auto t0 = std::chrono::steady_clock::now();
        ChunkFull(lots, wx0, wz0, full);
        const double cpuMs = Ms(t0);
        IDirect3DTexture9* tex = nullptr;
        BuildChunk(eng, srcs, wx0, wz0, {}, tex);
        Wait();
        t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < 10; r++) {
            BuildChunk(eng, srcs, wx0, wz0, {0, 0, kChunkTexels - 1, kChunkTexels - 1}, tex);
            Wait();
        }
        const double gpuMs = Ms(t0) / 10;
        int differ = 0;
        std::string per;
        const int worst = tex ? CompareChunk(tex, full, &differ, &per) : 999;
        std::printf("\n== chunk (%s), 15 lots (one an 88 m grid), from CPU shades: CPU ChunkFull %.2f ms | GPU whole chunk %.3f ms with the wait\n", fmtName, cpuMs, gpuMs);
        std::printf("  bytes vs ChunkFull: max %d, %d texels differ (per level%s)\n", worst, differ, per.c_str());
        Check(worst == 0, "chunk: the whole chunk from CPU shades bit for bit as ChunkFull, every level");
        // changed boxes: a lot moved, added or removed; the update over its box only = the whole chunk made again
        int updates = 0, same = 0, asCpu = 0;
        double updMs = 0;
        for (int round = 0; round < 40 && tex; round++) {
            const size_t k = static_cast<size_t>(U(rng) * lots.size()) % lots.size();
            float bx0 = lots[k].x0, bz0 = lots[k].z0, bx1 = lots[k].x1, bz1 = lots[k].z1;
            const int kind = round % 4;
            if (kind == 3) lots[k].v = std::make_shared<ExtData>(ExtData{std::vector<uint8_t>(lots[k].v->v.size(), 255), lots[k].v->w}); // shades nothing now
            else lots[k] = RandomLot(rng, lots[k].lot, wx0, wz0);
            bx0 = std::min(bx0, lots[k].x0), bz0 = std::min(bz0, lots[k].z0), bx1 = std::max(bx1, lots[k].x1), bz1 = std::max(bz1, lots[k].z1);
            GroundShadeGpu::Engine::UploadExt(g_dev, lots[k].grid, lots[k].v->v, lots[k].v->w, tx[k]);
            srcs[k] = SrcOf(lots[k], tx[k], wx0, wz0);
            const TexRect r0 = ChunkRectOf(wx0, wz0, bx0, bz0, bx1, bz1);
            if (r0.Empty()) continue;
            Wait();
            t0 = std::chrono::steady_clock::now();
            BuildChunk(eng, srcs, wx0, wz0, r0, tex);
            Wait();
            updMs += Ms(t0);
            IDirect3DTexture9* whole = nullptr;
            BuildChunk(eng, srcs, wx0, wz0, {}, whole);
            updates++;
            same += whole && SameTextures(tex, whole);
            std::vector<ExtSrc> shading; // (as the bridge: a lot whose shade is 255 everywhere has no ext, and the GPU skips it by its lowest byte)
            for (const ExtSrc& e : lots)
                if (*std::min_element(e.v->v.begin(), e.v->v.end()) < 255) shading.push_back(e);
            ChunkFull(shading, wx0, wz0, full);
            int d = 0;
            asCpu += CompareChunk(tex, full, &d) == 0;
            if (whole) whole->Release();
        }
        std::printf("  %d updates over a changed box (moved, new or shading nothing): %d equal the whole chunk made again, %d equal ChunkFull, every level; %.3f ms an update with the wait\n",
                    updates, same, asCpu, updates ? updMs / updates : 0.0);
        Check(updates > 20 && same == updates, "chunk: an update over the changed box = the whole chunk made again, bit for bit, every level");
        Check(asCpu == updates, "chunk: every update = ChunkFull, every level");
        // the readers: the chunk read as the terrain and road copies read it, against the same bytes in a MANAGED L8 texture
        if (tex) {
            IDirect3DTexture9* ref = nullptr;
            g_dev->CreateTexture(kChunkTexels, kChunkTexels, kChunkLevels, 0, D3DFMT_L8, D3DPOOL_MANAGED, &ref, nullptr);
            for (int l = 0; ref && l < kChunkLevels; l++) {
                D3DLOCKED_RECT lr{};
                ref->LockRect(l, &lr, nullptr, 0);
                const int n = kChunkTexels >> l;
                for (int y = 0; y < n; y++) std::memcpy(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, &full.levels[l][static_cast<size_t>(y) * n], n);
                ref->UnlockRect(l);
            }
            IDirect3DTexture9* outT = nullptr;
            g_dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &outT, nullptr);
            const auto read = [&](IDirect3DTexture9* t, float lodOn, float uvScale) {
                g_dev->BeginScene();
                IDirect3DSurface9* old = nullptr;
                g_dev->GetRenderTarget(0, &old);
                g_dev->SetRenderTarget(0, Level(outT));
                g_dev->SetDepthStencilSurface(nullptr);
                g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
                g_dev->SetTexture(0, t);
                const DWORD st[][2] = {{D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP}, {D3DSAMP_MINFILTER, D3DTEXF_LINEAR},
                                       {D3DSAMP_MAGFILTER, D3DTEXF_LINEAR}, {D3DSAMP_MIPFILTER, D3DTEXF_LINEAR}, {D3DSAMP_SRGBTEXTURE, FALSE}, {D3DSAMP_MAXMIPLEVEL, 0}};
                for (const auto& s : st) g_dev->SetSamplerState(0, static_cast<D3DSAMPLERSTATETYPE>(s[0]), s[1]);
                const float c[8] = {0.37f, 0.11f, uvScale, uvScale, lodOn, 0, 0, 0};
                g_dev->SetPixelShaderConstantF(0, c, 2);
                g_dev->SetPixelShader(g_readPs);
                const float tri[6] = {-1, -1, -1, 3, 3, -1};
                g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, 8);
                g_dev->SetRenderTarget(0, old);
                old->Release();
                g_dev->SetTexture(0, nullptr);
                g_dev->EndScene();
                int w, h, b;
                std::vector<uint8_t> raw = ReadLevel(outT, 0, w, h, b);
                std::vector<float> f(raw.size() / 4);
                std::memcpy(f.data(), raw.data(), raw.size());
                return f;
            };
            double worstRead = 0;
            for (const float lodOn : {0.0f, 1.0f})
                for (const float scale : {0.05f, 0.4f, 1.0f}) {
                    const std::vector<float> a = read(tex, lodOn, scale), b = read(ref, lodOn, scale);
                    for (size_t i = 0; i < a.size(); i += 4) worstRead = std::max(worstRead, std::fabs(double(a[i]) - b[i]));
                }
            std::printf("  read as the terrain and road copies read it (red, LINEAR / LINEAR / LINEAR mips, CLAMP), 4096 points at lods 0..10: max |d| %.6f vs the CPU's L8 texture\n", worstRead);
            Check(worstRead <= 1.0 / 255.0 * 0.5, "readers: the chunk's red channel read as from the CPU's L8 texture");
            if (outT) outT->Release();
            if (ref) ref->Release();
        }
        if (tex) tex->Release();
        for (auto* t : tx)
            if (t) t->Release();
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (!MakeDevice()) return 2;
    D3DCAPS9 caps{};
    g_dev->GetDeviceCaps(&caps);
    std::printf("caps: PS %lu.%lu, max PS instructions executed %lu, PS 3.0 instruction slots %lu\n", D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion),
                D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion), caps.MaxPShaderInstructionsExecuted, caps.MaxPixelShader30InstructionSlots);
    if (!CompileAll()) return 2;
    GroundShadeGpu::Engine eng(PlainCalls());
    std::string err;
    if (!eng.Init(g_dev, g_shaders, err)) return std::printf("engine: %s\n", err.c_str()), 2;
    std::printf("chunk format: %s\n", eng.ChunkFormat() == D3DFMT_L8 ? "L8" : "A8R8G8B8");

    std::vector<GScene> scenes;
    scenes.push_back(FromCpuScene(MakeScene(0.0f, false), "40 m lot, 0 deg, soft off"));
    scenes.push_back(FromCpuScene(MakeScene(0.0f, true), "40 m lot, 0 deg, soft on"));
    scenes.push_back(FromCpuScene(MakeScene(0.5235988f, false), "40 m lot, 30 deg, soft off"));
    scenes.push_back(FromCpuScene(MakeScene(0.5235988f, true), "40 m lot, 30 deg, soft on"));
    scenes.push_back(CrownScene());
    scenes.push_back(EdgeScene(false));
    scenes.push_back(EdgeScene(true));
    for (bool soft : {true, false}) { // the Tester switch "Shadow softness" off: the narrowest rim
        GScene s = EdgeScene(soft);
        s.penumbra = false, s.name += ", penumbra off";
        scenes.push_back(s);
    }
    scenes.push_back(StressScene(true));
    scenes.push_back(StressScene(false));
    { // walls out of every lamp's reach: the shade is 255 everywhere (no ext on the CPU, the occlusion query 0)
        GScene s = FromCpuScene(MakeScene(0.0f, true), "40 m lot, walls 500 m away (shades nothing)");
        for (Wall& w : s.walls) w.x0 += 500.0f, w.x1 += 500.0f;
        scenes.push_back(s);
    }
    { // the shade only in the grid's far rows: the occlusion query reads every row of its last 4 x 4 block (a 200 x 200 grid
      // reduces 200 -> 50 -> 13 -> 4, its row 0 covering grid rows 0..63)
        GScene s = FromCpuScene(MakeScene(0.0f, true), "50 m grid, shade only past grid row 64");
        s.grid.w = s.grid.h = 200;
        const float zCut = s.grid.tz + 36.0f; // (turn 0: grid rows run along z, 4 a metre)
        std::erase_if(s.walls, [&](const Wall& w) { return std::min(w.z0, w.z1) < zCut || w.pn; });
        std::erase_if(s.lamps, [&](const Lamp& l) { return l.z < zCut - 4.0f; });
        s.minShadeRow = 64;
        scenes.push_back(s);
    }
    for (int n : {300, 600, 1000}) scenes.push_back(CrowdScene(n, true));
    scenes.push_back(CrowdScene(1000, false));
    { // past kMaxEntries: refused (a CPU build)
        const GScene s = CrowdScene(GroundShadeGpu::kMaxEntries + 100, true);
        GroundShadeGpu::Inputs in;
        std::string why;
        const bool fits = GroundShadeGpu::Prepare(s.grid, s.lamps, s.others, s.walls, s.patches, s.crowns, in, why);
        std::printf("\n== %s: %d pairs, Prepare %s\n", s.name.c_str(), in.entries, fits ? "accepted" : why.c_str());
        Check(!fits && in.entries > GroundShadeGpu::kMaxEntries, "lot: over kMaxEntries lamp-occluder pairs refused");
    }
    scenes.push_back(CrowdScene(300, true, 4200));
    std::vector<LotOut> outs;
    for (const GScene& s : scenes) outs.push_back(RunLot(eng, s, s.grid.w > 300 ? 2 : 5, s.grid.w > 300 ? 5 : 20));

    // the lot-to-chunk chain on the GPU: the lots' own ext textures read by the chunk pass (no CPU copy), against ChunkFull
    // of the CPU's shades of the same lots (moved into one chunk side by side)
    {
        const float wx0 = 768.0f, wz0 = 1792.0f;
        std::vector<ExtSrc> cpu;
        std::vector<GroundShadeGpu::ChunkSrc> gpu;
        for (size_t k = 0; k < 7 && k < outs.size(); k++) { // the 40 m lots (their grids 66 m)
            if (!outs[k].gpu) continue;
            ExtSrc e = outs[k].cpu;
            e.grid.tx += -1000.0f + wx0 + 70.0f * static_cast<float>(k % 3) - 60.0f, e.grid.tz += -2000.0f + wz0 + 80.0f * static_cast<float>(k / 3) - 50.0f;
            GridBox(e.grid, e.x0, e.z0, e.x1, e.z1);
            e.lot = k + 1;
            cpu.push_back(e);
            gpu.push_back(SrcOf(e, outs[k].gpu, wx0, wz0));
        }
        ChunkTexture full;
        ChunkFull(cpu, wx0, wz0, full);
        IDirect3DTexture9* tex = nullptr;
        BuildChunk(eng, gpu, wx0, wz0, {}, tex);
        int differ = 0;
        std::string per;
        const int worst = tex ? CompareChunk(tex, full, &differ, &per) : 999;
        std::printf("\n== chunk from the GPU's own lot shades (%zu lots): vs ChunkFull of the CPU's shades: max %d, %d texels differ (per level%s)\n", gpu.size(), worst, differ,
                    per.c_str());
        Check(tex && worst <= 2 && differ <= 64, "chain: the chunk from the GPU's lot shades within 2/255 of the CPU's, at most 64 texels differ");
        if (tex) tex->Release();
    }
    ChunkRuns(eng, eng.ChunkFormat() == D3DFMT_L8 ? "L8" : "A8R8G8B8");
    // the fallback format (A8R8G8B8 where L8 is no render target): the same bytes, read in red
    {
        GroundShadeGpu::Engine argb(PlainCalls());
        if (argb.Init(g_dev, g_shaders, err, true)) ChunkRuns(argb, "A8R8G8B8 fallback");
        else Check(false, "engine with the A8R8G8B8 chunk");
    }
    for (LotOut& o : outs)
        if (o.gpu) o.gpu->Release();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
