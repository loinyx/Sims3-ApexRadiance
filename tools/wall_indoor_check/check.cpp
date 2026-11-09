// Offline check of the outside walls' and outdoor floors' indoor texels (ShaderPatches::PatchWallScale / PatchBakedAtlasPs, the
// outdoor scale's .y and the night level). Read-only: prints to stdout, writes no file.
//   check.exe <Shaders_Win32.precomp> [capture folder ...]
// Every ps_3_0 ExteriorWall pixel shader of the precomp, the outdoor floor copies, and every captured PS_*.bin the wall patch
// takes, is patched and drawn on a native D3D9 device over random interpolants, textures and constants into a 32-bit float
// target. References: the copy without the indoor instructions, and the copy with each blend replaced by its indoor end.
//  - unmarked texels at night and marked texels by day (night level 0): the copy as before, bit for bit;
//  - marked texels at night: map + map alpha x the room's ambient, the sky reflection x its scale (within 1e-5: the blend's
//    rounding).
//  - marked texels matched to their room's map (IndoorPick rooms): the room's texel lit as InteriorWall lights it, through the
//    rest of the copy bit for bit as the copy draws it with that room texel as its map (fall-off ratio 1, the floor line), over
//    load -> day -> night -> day (and a ratio 0 night): the room's map, its ambient, its slot and the bound map (relit copy or the game's) change between
//    them, the UV map does not.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include "../../features/shader_patches.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace {

int g_checks = 0, g_failures = 0;
void Check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) {
        g_failures++;
        if (g_failures < 40) std::printf("  FAIL %s\n", what.c_str());
    }
}
template <class T> void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}
uint32_t Fnv1Lower(const char* s) {
    uint32_t h = 0x811C9DC5u;
    for (; *s; s++) h = (h * 0x01000193u) ^ static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(*s)));
    return h;
}

// ---- Shaders_Win32.precomp (as tools/room_map_smooth_check) ----
struct Blob {
    bool vs = false;
    std::vector<DWORD> code;
};
std::vector<DWORD> Trim(const unsigned char* p, size_t n) {
    for (size_t k = 0; k + 4 <= std::min<size_t>(n, 12); k += 4) {
        DWORD v;
        std::memcpy(&v, p + k, 4);
        if ((v & 0xFFFE0000u) != 0xFFFE0000u) continue;
        size_t j = k + 4;
        while (j + 4 <= n) {
            DWORD tok;
            std::memcpy(&tok, p + j, 4);
            if (tok == 0x0000FFFFu) {
                std::vector<DWORD> c((j + 4 - k) / 4);
                std::memcpy(c.data(), p + k, c.size() * 4);
                return c;
            }
            j += ((tok & 0xFFFF) == 0xFFFE) ? 4 + 4 * ((tok >> 16) & 0x7FFF) : 4 + 4 * ((tok >> 24) & 0xF);
        }
    }
    return {};
}
// the ps_3_0 pixel shaders of one technique
std::vector<std::vector<DWORD>> TechniquePs(const std::vector<unsigned char>& d, const char* name) {
    std::vector<Blob> blobs;
    const auto find = [&](const char* tag, size_t from, size_t to) -> size_t {
        for (size_t i = from; i + 4 <= to; i++)
            if (std::memcmp(&d[i], tag, 4) == 0) return i;
        return std::string::npos;
    };
    const size_t shdb = find("SHDB", 0, d.size());
    if (shdb == std::string::npos) return {};
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
        blobs.push_back({v, Trim(&d[j + 8], std::min<size_t>(n, d.size() - j - 8))});
        j += 8 + n;
    }
    const uint32_t want = Fnv1Lower(name);
    std::vector<uint32_t> ids;
    std::vector<size_t> at;
    for (size_t i = end; i + 4 <= d.size(); i++)
        if (std::memcmp(&d[i], "TECH", 4) == 0) at.push_back(i);
    for (size_t k = 0; k < at.size(); k++) {
        const size_t lim = k + 1 < at.size() ? at[k + 1] : d.size();
        uint32_t tn, np;
        std::memcpy(&tn, &d[at[k] + 4], 4);
        std::memcpy(&np, &d[at[k] + 8], 4);
        if (tn != want) continue;
        size_t q = at[k] + 12;
        for (uint32_t i = 0; i < np && i < 64; i++) {
            q = find("PASS", q, lim);
            if (q == std::string::npos || q + 20 > lim) break;
            uint32_t ps;
            std::memcpy(&ps, &d[q + 16], 4);
            if (std::find(ids.begin(), ids.end(), ps) == ids.end()) ids.push_back(ps);
            q += 4;
        }
    }
    std::vector<std::vector<DWORD>> out;
    for (uint32_t id : ids)
        if (id >= 1 && id <= blobs.size() && !blobs[id - 1].vs && !blobs[id - 1].code.empty() && blobs[id - 1].code[0] == 0xFFFF0300) out.push_back(blobs[id - 1].code);
    return out;
}

// ---- token walk (ps_3_0: the instruction length is in the opcode token) ----
struct Ins {
    size_t at;
    DWORD op, len;
};
std::vector<Ins> Walk(const std::vector<DWORD>& t) {
    std::vector<Ins> all;
    for (size_t i = 1; i < t.size() && t[i] != 0x0000FFFF;) {
        const DWORD op = t[i] & 0xFFFF;
        const DWORD len = op == 0xFFFE ? (t[i] >> 16) & 0x7FFF : (t[i] >> 24) & 0xF;
        all.push_back({i, op, len});
        i += 1 + len;
    }
    return all;
}
DWORD Num(DWORD r) { return r & 0x7FF; }
DWORD Type(DWORD r) { return ((r >> 28) & 0x7) | ((r >> 8) & 0x18); }

// ---- hidden D3D9 device, 64 x 64 float target ----
constexpr int kSize = 64;
struct Gpu {
    HWND window = nullptr;
    IDirect3D9* api = nullptr;
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9 *target = nullptr, *readback = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    struct V {
        float pos[4];
        float a[4], b[4];
    };
    std::vector<V> verts;
    Gpu() {
        window = CreateWindowExW(0, L"STATIC", L"Apex wall indoor check", WS_OVERLAPPED, 0, 0, kSize, kSize, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        api = Direct3DCreate9(D3D_SDK_VERSION);
        D3DPRESENT_PARAMETERS p{};
        p.Windowed = TRUE;
        p.SwapEffect = D3DSWAPEFFECT_DISCARD;
        p.hDeviceWindow = window;
        p.BackBufferWidth = p.BackBufferHeight = kSize;
        if (api) api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &p, &dev);
        if (!dev) return;
        if (FAILED(dev->CreateRenderTarget(kSize, kSize, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr)) ||
            FAILED(dev->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &readback, nullptr))) {
            Release(dev);
            return;
        }
        // every interpolant a ps_3_0 wall shader may declare, from two random vectors per vertex
        const char* src = R"(
struct I { float4 p : POSITION; float4 a : TEXCOORD0; float4 b : TEXCOORD1; };
struct O { float4 p : POSITION; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
           float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7; float4 c0 : COLOR0; float4 c1 : COLOR1; };
O main(I i) { O o; o.p = i.p; o.t0 = i.a; o.t1 = i.b; o.t2 = i.a.yzwx * 2 - 1; o.t3 = i.b.wzyx; o.t4 = i.a * i.b; o.t5 = float4(i.a.xy, 0.5 + 0.5 * i.b.z, 1);
  o.t6 = i.b.yxwz; o.t7 = float4(i.b.xy * 0.9 + 0.05, 0.5, 1); o.c0 = i.a.zwxy; o.c1 = i.b.zxyw; return o; })";
        ID3DBlob *code = nullptr, *err = nullptr;
        if (FAILED(D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", "vs_3_0", 0, 0, &code, &err)) ||
            FAILED(dev->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()), &vs)))
            Release(dev);
        Release(code);
        Release(err);
        if (!dev) return;
        const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITION, 0},
                                        {0, 16, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 0},
                                        {0, 32, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 1},
                                        D3DDECL_END()};
        dev->CreateVertexDeclaration(el, &decl);
        // an 8 x 8 grid of quads, every vertex its own random interpolants
        std::mt19937 rng(77);
        std::uniform_real_distribution<float> U(0.02f, 0.98f);
        constexpr int n = 8;
        std::vector<V> grid((n + 1) * (n + 1));
        for (int z = 0; z <= n; z++)
            for (int x = 0; x <= n; x++) {
                V& v = grid[z * (n + 1) + x];
                v.pos[0] = -1.0f + 2.0f * x / n, v.pos[1] = 1.0f - 2.0f * z / n, v.pos[2] = 0.5f, v.pos[3] = 1.0f;
                for (int k = 0; k < 4; k++) v.a[k] = U(rng), v.b[k] = U(rng);
            }
        for (int z = 0; z < n; z++)
            for (int x = 0; x < n; x++) {
                const V &a = grid[z * (n + 1) + x], &b = grid[z * (n + 1) + x + 1], &c = grid[(z + 1) * (n + 1) + x], &d = grid[(z + 1) * (n + 1) + x + 1];
                verts.insert(verts.end(), {a, b, c, b, d, c});
            }
        dev->SetRenderTarget(0, target);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    }
    ~Gpu() {
        Release(decl);
        Release(vs);
        Release(target);
        Release(readback);
        Release(dev);
        Release(api);
        if (window) DestroyWindow(window);
    }
    // the pixels (RGBA floats) a pixel shader draws; texkilled pixels keep the clear (-7)
    bool Render(const std::vector<DWORD>& ps, std::vector<float>& out) {
        IDirect3DPixelShader9* p = nullptr;
        if (FAILED(dev->CreatePixelShader(ps.data(), &p))) return false;
        dev->SetPixelShader(p);
        dev->SetVertexShader(vs);
        dev->SetVertexDeclaration(decl);
        dev->ColorFill(target, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
        dev->BeginScene();
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(verts.size() / 3), verts.data(), sizeof(V));
        dev->EndScene();
        dev->SetPixelShader(nullptr);
        Release(p);
        D3DLOCKED_RECT r{};
        if (FAILED(dev->GetRenderTargetData(target, readback)) || FAILED(readback->LockRect(&r, nullptr, D3DLOCK_READONLY))) return false;
        out.resize(kSize * kSize * 4);
        for (int y = 0; y < kSize; y++) std::memcpy(&out[y * kSize * 4], static_cast<const char*>(r.pBits) + y * r.Pitch, kSize * 16);
        readback->UnlockRect();
        return true;
    }
};

IDirect3DBaseTexture9* RandomTexture(IDirect3DDevice9* dev, bool cube, std::mt19937& rng, int flagY = -1) {
    std::uniform_int_distribution<int> B(0, 255), S(0, 60);
    const auto fill = [&](D3DLOCKED_RECT& r, int n) {
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                // (the scale texture: x = the outdoor scale, y = the indoor flag)
                const DWORD c = flagY < 0 ? static_cast<DWORD>(B(rng) | (B(rng) << 8) | (B(rng) << 16) | (B(rng) << 24))
                                          : static_cast<DWORD>((S(rng) << 16) | ((flagY * 255) << 8) | S(rng) | (S(rng) << 24));
                std::memcpy(static_cast<char*>(r.pBits) + y * r.Pitch + x * 4, &c, 4);
            }
    };
    if (cube) {
        IDirect3DCubeTexture9* t = nullptr;
        if (FAILED(dev->CreateCubeTexture(8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
        for (int f = 0; f < 6; f++) {
            D3DLOCKED_RECT r{};
            t->LockRect(static_cast<D3DCUBEMAP_FACES>(f), 0, &r, nullptr, 0);
            fill(r, 8);
            t->UnlockRect(static_cast<D3DCUBEMAP_FACES>(f), 0);
        }
        return t;
    }
    IDirect3DTexture9* t = nullptr;
    if (FAILED(dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
    D3DLOCKED_RECT r{};
    t->LockRect(0, &r, nullptr, 0);
    fill(r, 16);
    t->UnlockRect(0);
    return t;
}

struct Stats {
    int shaders = 0, patched = 0, indoor = 0, spec = 0, strip = 0, probes = 0, probesCreated = 0, haze = 0, alphaAmb = 0, drawn = 0, sameOff = 0, sameDay = 0, sameOn = 0, changedOn = 0;
    int rooms = 0, roomsSame = 0; // (IndoorPick rooms) copies that read the rooms' maps, and those whose six states all matched, bit for bit
};
// floor: the outdoor floors' copy (PatchBakedAtlasPs at TEXCOORD7, with the ground shade), else the outside walls' (PatchWallScale)
void CheckShader(Gpu& gpu, const std::vector<DWORD>& game, const std::string& where, Stats& st, bool floor = false) {
    st.shaders++;
    // (the Light capture's probe copies, ShaderPatches::ProbeCopy) every texld and input of the shader as its output: each created natively
    {
        int made = 0, created = 0;
        for (int k = 0; k < 32; k++) {
            std::vector<DWORD> probe;
            std::string what;
            if (!ShaderPatches::ProbeCopy(game, k, probe, what)) break;
            made++;
            IDirect3DPixelShader9* ps = nullptr;
            if (SUCCEEDED(gpu.dev->CreatePixelShader(probe.data(), &ps)) && ps) created++, ps->Release();
        }
        st.probes += made, st.probesCreated += created;
        Check(made > 0 && created == made, where + ": the Light capture's probe copies are created natively");
    }
    std::vector<DWORD> c = game;
    DWORD sampler = 0, constReg = 0, mapSampler = 0, ambConst = 0;
    int roomSampler = -1, roomConst = -1;
    bool indoor = false, spec = false;
    if (floor) {
        ShaderPatches::FloorPatch fp;
        if (!ShaderPatches::PatchBakedAtlasPs(c, 7, fp, true) || fp.outScaleSampler < 0) return;
        sampler = static_cast<DWORD>(fp.outScaleSampler), constReg = static_cast<DWORD>(fp.outScaleConst), indoor = fp.indoor, spec = fp.spec;
        mapSampler = fp.mapSampler, ambConst = static_cast<DWORD>(fp.indoorConst); // (no rooms' maps: its texels carry their room's top row)
    } else {
        ShaderPatches::WallScalePatch wp;
        if (!ShaderPatches::PatchWallScale(c, wp)) return;
        sampler = wp.sampler, constReg = wp.constReg, indoor = wp.indoor, spec = wp.spec;
        mapSampler = wp.mapSampler, ambConst = wp.ambConst, roomSampler = wp.roomSampler, roomConst = wp.roomConst;
    }
    st.patched++;
    if (!indoor) return;
    st.indoor++;
    // the scale read's temp (texld rT, vK, sQ) and the flag's register: rT.y (walls), or rF.x of "mov rF.x, rT.y" (floors); the
    // patch's temps after it: N (flag x night), T2 (the indoor light), T3 (the indoor reflection)
    const auto all = Walk(c);
    DWORD T = 0xFFFF, F = 0xFFFF;
    for (size_t i = 0; i < all.size(); i++) {
        const Ins& x = all[i];
        if (x.op != 0x42 || Type(c[x.at + 3]) != 10 || Num(c[x.at + 3]) != sampler) continue;
        T = F = Num(c[x.at + 1]);
        // (a floor: the instruction right after the scale's texld, "mov rF.x, rT.y")
        if (floor && i + 1 < all.size() && all[i + 1].op == 0x01 && Num(c[all[i + 1].at + 2]) == T && ((c[all[i + 1].at + 2] >> 16) & 0xFF) == 0x55)
            F = Num(c[all[i + 1].at + 1]);
    }
    const DWORD N = F + 1, T2 = F + 2, T3 = F + 3, T4 = F + 4, T5 = F + 5;
    const auto dstIs = [&](const Ins& x, DWORD r) { return x.op != 0x1F && x.op != 0x51 && x.op != 0xFFFE && x.len > 0 && (c[x.at + 1] & 0x80000000u) && Type(c[x.at + 1]) == 0 && Num(c[x.at + 1]) == r; };
    const auto readsNeg = [&](const Ins& x) { return x.len >= 2 && Type(c[x.at + 2]) == 0 && Num(c[x.at + 2]) == N && (c[x.at + 2] & 0x0F000000u) == 0x01000000u; };
    // before: the copy without the indoor instructions (the reflection's operand back in its mad); indoorRef: each blend replaced by
    // its indoor end (the light's cmp as "mov rB, rT2", the reflection's lrp and cmp dropped: rT3 = rS x cA.w)
    std::vector<DWORD> before, indoorRef;
    DWORD specSrc = 0, stripSrc = 0, hazeSrc = 0;
    int lights = 0, specs = 0, strips = 0, hazes = 0, alphaAmb = 0;
    for (const Ins& x : all) {
        if (x.op == 0x05 && dstIs(x, T3)) specSrc = c[x.at + 2];
        if (x.op == 0x05 && dstIs(x, T4)) stripSrc = c[x.at + 2]; // (mul rT4.xyz, X, rT4.w: the albedo with the strip)
        if (x.op == 0x04 && dstIs(x, T5)) hazeSrc = c[x.at + 2];  // (mad rT5.x, rW.s, -rN.x, rW.s: the haze weight)
    }
    before.push_back(c[0]), indoorRef.push_back(c[0]);
    for (const Ins& x : all) {
        const auto copy = [&](std::vector<DWORD>& v) { v.insert(v.end(), c.begin() + x.at, c.begin() + x.at + 1 + x.len); };
        const bool inserted = dstIs(x, N) || dstIs(x, T2) || dstIs(x, T3) || dstIs(x, T4) || dstIs(x, T5) || (x.op == 0x58 && readsNeg(x));
        alphaAmb += x.op == 0x04 && dstIs(x, T2) && Type(c[x.at + 2]) == 0 && ((c[x.at + 2] >> 16) & 0xFF) == 0xFF; // (mad rT2.xyz, rA.w or rT2.w, cA, rA)
        if (!inserted) {
            copy(indoorRef);
            std::vector<DWORD> one(c.begin() + x.at, c.begin() + x.at + 1 + x.len);
            for (size_t k = 2; k < one.size(); k++)
                if (Type(one[k]) == 0 && Num(one[k]) == T3 && specSrc) one[k] = specSrc; // (the consumer's rS back)
                else if (Type(one[k]) == 0 && Num(one[k]) == T4 && stripSrc) one[k] = stripSrc; // (and its albedo)
                else if (Type(one[k]) == 0 && Num(one[k]) == T5 && hazeSrc) one[k] = hazeSrc;    // (and the haze weight)
            before.insert(before.end(), one.begin(), one.end());
            continue;
        }
        if (x.op == 0x58 && !dstIs(x, T2) && !dstIs(x, T3) && !dstIs(x, T4) && !dstIs(x, T5)) { // the light's cmp: rB = rT2
            indoorRef.insert(indoorRef.end(), {0x02000001u, c[x.at + 1], c[x.at + 4]});
            lights++;
            continue;
        }
        if ((x.op == 0x12 || x.op == 0x58) && (dstIs(x, T2) || dstIs(x, T3) || dstIs(x, T4) || dstIs(x, T5))) {
            // (the blends; the rooms' slot selects and their final pick, on rT3.w: dropped too, the UV map is 0 below)
            specs += dstIs(x, T3) && x.op == 0x58 && readsNeg(x);
            strips += dstIs(x, T4) && x.op == 0x58 && readsNeg(x);
            hazes += dstIs(x, T5) && x.op == 0x58 && readsNeg(x);
            continue;
        }
        copy(indoorRef);
    }
    before.push_back(0x0000FFFF), indoorRef.push_back(0x0000FFFF);
    Check(T != 0xFFFF && (!floor || F != T) && lights == 1 && specs == (spec ? 1 : 0), where + ": the light's blend and, with spec, the reflection's");
    if (lights != 1) return;
    st.spec += spec, st.alphaAmb += alphaAmb > 0, st.strip += strips > 0, st.haze += hazes > 0;
    // the samplers (cube or 2D), random; the scale sampler: x random, y the flag
    std::mt19937 rng(static_cast<unsigned>(st.shaders * 131));
    std::vector<std::pair<DWORD, bool>> samplers;
    for (const Ins& x : Walk(c)) // (the copy's: a floor's atlas and shade samplers too)
        if (x.op == 0x1F && Type(c[x.at + 2]) == 10 && Num(c[x.at + 2]) != sampler) samplers.push_back({Num(c[x.at + 2]), ((c[x.at + 1] >> 27) & 0xF) == 3});
    std::vector<IDirect3DBaseTexture9*> tex;
    for (const auto& [s, cube] : samplers) {
        tex.push_back(RandomTexture(gpu.dev, cube, rng));
        gpu.dev->SetTexture(s, tex.back());
    }
    // (rooms) the UV map: 0 (no texel matched: the indoor light as before) for the draws below; the rooms' maps random
    const auto uvMap = [&](int slot) { // (slot < 0: 0; else every texel its own place in a room's map of the same size)
        IDirect3DTexture9* t = nullptr;
        if (FAILED(gpu.dev->CreateTexture(16, 16, 1, 0, D3DFMT_A16B16G16R16, D3DPOOL_MANAGED, &t, nullptr))) return t;
        D3DLOCKED_RECT r{};
        t->LockRect(0, &r, nullptr, 0);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                uint16_t* px = reinterpret_cast<uint16_t*>(static_cast<char*>(r.pBits) + y * r.Pitch) + x * 4;
                px[0] = slot < 0 ? 0 : static_cast<uint16_t>(std::lround((x + 0.5) / 16.0 * 65535.0));
                px[1] = slot < 0 ? 0 : static_cast<uint16_t>(std::lround((y + 0.5) / 16.0 * 65535.0));
                px[2] = slot < 0 ? 0 : static_cast<uint16_t>(std::lround(slot * 65535.0 / 3.0)), px[3] = slot < 0 ? 0 : 0xFFFF;
            }
        t->UnlockRect(0);
        return t;
    };
    IDirect3DTexture9* uv0 = roomSampler >= 0 ? uvMap(-1) : nullptr;
    if (roomSampler >= 0) {
        gpu.dev->SetTexture(static_cast<DWORD>(roomSampler), uv0);
        for (int r = 1; r <= 4; r++) tex.push_back(RandomTexture(gpu.dev, false, rng)), gpu.dev->SetTexture(static_cast<DWORD>(roomSampler + r), tex.back());
    }
    for (DWORD s = 0; s < 16; s++) {
        gpu.dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        gpu.dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        gpu.dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    }
    std::uniform_real_distribution<float> U(0.0f, 1.5f);
    std::vector<float> consts(224 * 4);
    for (float& f : consts) f = U(rng);
    const auto night = [&](float n) { // (the draw: kScaleLog2, the night level)
        const float q[4] = {3.0f, n, 0.0f, 0.0f};
        std::memcpy(&consts[constReg * 4], q, sizeof q);
        gpu.dev->SetPixelShaderConstantF(0, consts.data(), 224);
    };
    std::vector<float> outBefore, outBefore1, outRef, off, day, on;
    std::mt19937 again = rng; // (the same outdoor scale under both flags)
    IDirect3DBaseTexture9* flag0 = RandomTexture(gpu.dev, false, rng, 0);
    IDirect3DBaseTexture9* flag1 = RandomTexture(gpu.dev, false, again, 1);
    bool ok = true;
    night(1.0f);
    gpu.dev->SetTexture(sampler, flag0);
    ok &= gpu.Render(before, outBefore) && gpu.Render(c, off);
    gpu.dev->SetTexture(sampler, flag1);
    ok &= gpu.Render(before, outBefore1) && gpu.Render(indoorRef, outRef) && gpu.Render(c, on);
    night(0.0f);
    ok &= gpu.Render(c, day);
    // (rooms) load -> day -> night -> day with no prepare between: in each state the room's map holds another solve (the game's,
    // a relit copy), its ambient and slot differ, and the attic's bound map is its relit copy (any texel: its scale's .z the fall-off
    // ratio, 1 at the floor line, constReg.z = 0) or the game's own map (its scale's .z 0, floored at 1 by constReg.z = 1); a last
    // night with the ratio 0 gives the copy drawn with a black map (the room's light x the scale's ratio, never the map's colour).
    // Reference: the same copy with no texel matched (UV map 0) drawn with that room texel as its map and its ambient as cA: the
    // indoor light then is the room's texel lit as InteriorWall lights it (map + map alpha x the room's ambient). The rgb out of
    // the whole copy, bit for bit, at the floor line. The outside light both blend from (the lamp mad rB = rL x cK.x + rC) is the
    // same in both (cK.x = 0: the lrp's (rT2 - rB) + rB rounds with rB)
    int roomStates = 0;
    if (roomSampler >= 0 && ok) {
        st.rooms++;
        for (size_t i = 1; i < all.size(); i++)
            if (dstIs(all[i], N) && all[i].op == 0x05 && all[i - 1].op == 0x04 && Type(c[all[i - 1].at + 3]) == 2) {
                consts[Num(c[all[i - 1].at + 3]) * 4] = 0.0f;
                break;
            }
        const auto flat = [&](DWORD v) {
            IDirect3DTexture9* t = nullptr;
            if (FAILED(gpu.dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return t;
            D3DLOCKED_RECT r{};
            t->LockRect(0, &r, nullptr, 0);
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) *reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits) + y * r.Pitch + x * 4) = v;
            t->UnlockRect(0);
            return t;
        };
        // (the relit copy's scale: y the mark, z the fall-off ratio, 1 or 0; x is taken x 0 below)
        IDirect3DTexture9 *ratio1 = flat(0x0000FFFFu), *ratio0 = flat(0x0000FF00u), *black = flat(0);
        const char* names[6] = {"load", "day", "night", "day again", "night, ratio 0", "the game's map with its last relight's ratio 0"};
        for (int k = 0; k < 6 && ok; k++) {
            const int slot = (k + 1) & 3;
            const bool zero = k >= 4; // (the scale's ratio 0: no light, the map's colour never read)
            // (5: the game's map shown between relights with the marks-only scale of its last relight: its ratio honoured, c.z 0, as
            // IndoorRoomsOf gives it, so the attic keeps the fall-off instead of switching to its room's top texel up the wall)
            const bool marks = k == 5;
            const bool relit = k != 1 && !marks; // (by day after the switch the game's own map is shown: no ratio in it)
            IDirect3DBaseTexture9* room = RandomTexture(gpu.dev, false, rng);
            IDirect3DBaseTexture9* game = RandomTexture(gpu.dev, false, rng);
            IDirect3DTexture9* uvK = uvMap(slot);
            std::vector<float> got, want;
            const float q[4] = {0.0f, 1.0f, relit || marks ? 0.0f : 1.0f, 0.0f};
            std::memcpy(&consts[constReg * 4], q, sizeof q);
            for (int r = 0; r < 4; r++)
                for (int i = 0; i < 3; i++) consts[(roomConst + r) * 4 + i] = U(rng); // (each room's ambient, this state's)
            std::memcpy(&consts[ambConst * 4], &consts[(roomConst + slot) * 4], 3 * sizeof(float)); // (cA.w, the reflection's scale, as is)
            gpu.dev->SetPixelShaderConstantF(0, consts.data(), 224);
            gpu.dev->SetTexture(sampler, zero || !relit ? ratio0 : ratio1); // (the game's map: its ratio floored at 1 by c.z)
            // the copy: the UV map to this slot, every room sampler a decoy but this slot's
            gpu.dev->SetTexture(static_cast<DWORD>(roomSampler), uvK);
            for (int r = 0; r < 4; r++) gpu.dev->SetTexture(static_cast<DWORD>(roomSampler + 1 + r), r == slot ? room : tex[tex.size() - 4 + static_cast<size_t>(r)]);
            gpu.dev->SetTexture(mapSampler, game); // (the relit copy holds light there: a decoy, the ratio is the scale's)
            ok &= gpu.Render(c, got);
            // the reference: no match, the room's texel as the map
            gpu.dev->SetTexture(static_cast<DWORD>(roomSampler), uv0);
            gpu.dev->SetTexture(mapSampler, zero ? static_cast<IDirect3DBaseTexture9*>(black) : room);
            ok &= gpu.Render(c, want);
            bool same = ok && got.size() == want.size();
            for (size_t i = 0; same && i < got.size(); i += 4) same = std::memcmp(&got[i], &want[i], 12) == 0;
            roomStates += same;
            if (!same) std::printf("  %s: %s (slot %d, %s): the attic's texel differs from its room's\n", where.c_str(), names[k], slot, relit ? "relit copy" : "the game's map");
            Release(room), Release(game), Release(uvK);
        }
        Release(ratio1), Release(ratio0), Release(black);
        st.roomsSame += roomStates == 6;
        Check(roomStates == 6, where + ": load, day, night, day, ratio 0, the game's map with its kept ratio: a matched texel is its room's texel lit as InteriorWall lights it, through the copy bit for bit");
    }
    Release(uv0);
    for (DWORD s = 0; s < 16; s++) gpu.dev->SetTexture(s, nullptr);
    for (auto* t : tex) Release(t);
    Release(flag0);
    Release(flag1);
    if (!ok) {
        const auto creates = [&](const std::vector<DWORD>& v) {
            IDirect3DPixelShader9* ps = nullptr;
            const bool r = SUCCEEDED(gpu.dev->CreatePixelShader(v.data(), &ps));
            Release(ps);
            return r;
        };
        std::printf("  %s: created natively: copy %d, before %d, indoor reference %d\n", where.c_str(), creates(c), creates(before), creates(indoorRef));
    }
    Check(ok, where + ": drawn natively");
    if (!ok) return;
    st.drawn++;
    const bool sameOff = std::memcmp(off.data(), outBefore.data(), off.size() * 4) == 0;
    const bool sameDay = std::memcmp(day.data(), outBefore1.data(), day.size() * 4) == 0;
    float worst = 0.0f;
    for (size_t i = 0; i < on.size(); i++) worst = std::max(worst, std::fabs(on[i] - outRef[i]) / std::max(1.0f, std::fabs(outRef[i])));
    int changed = 0;
    for (size_t i = 0; i < on.size(); i += 4) changed += std::memcmp(&on[i], &outBefore1[i], 12) != 0;
    st.sameOff += sameOff, st.sameDay += sameDay, st.sameOn += worst <= 1e-5f, st.changedOn += changed > 0;
    Check(sameOff, where + ": unmarked texels draw the copy as before, bit for bit");
    Check(sameDay, where + ": marked texels by day (night level 0) draw the copy as before, bit for bit");
    Check(worst <= 1e-5f, where + ": marked texels at night draw map + alpha x ambient and the reflection x its scale (1e-5)");
    if (!floor) Check(changed > kSize * kSize / 4, where + ": marked texels at night differ from the outside response"); // (a floor's random shade can blank most pixels)
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: check.exe <Shaders_Win32.precomp> [capture folder ...]\n");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    const std::vector<unsigned char> data{std::istreambuf_iterator<char>(f), {}};
    Gpu gpu;
    Check(gpu.dev != nullptr, "native D3D9 device with a 32-bit float target");
    if (!gpu.dev) {
        std::printf("%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }
    Stats st;
    const auto walls = TechniquePs(data, "ExteriorWall");
    for (size_t i = 0; i < walls.size(); i++) CheckShader(gpu, walls[i], "precomp ExteriorWall ps_3_0 #" + std::to_string(i), st);
    std::printf("Precomp ExteriorWall ps_3_0: %d shaders, %d patched, %d with the indoor light (%d with the room's ambient, %d with the reflection), %d drawn; "
                "unmarked as before %d, marked by day as before %d, marked at night the indoor light %d (changed %d)\n",
                st.shaders, st.patched, st.indoor, st.alphaAmb, st.spec, st.drawn, st.sameOff, st.sameDay, st.sameOn, st.changedOn);
    std::printf("  ExteriorWall: %d with the wall shade strip taken off the albedo of the indoor texels\n", st.strip);
    std::printf("  ExteriorWall: Light capture probe copies %d, created natively %d\n", st.probes, st.probesCreated);
    std::printf("  ExteriorWall: %d read their rooms' own maps, %d of them the room's texel in all six states, bit for bit\n", st.rooms, st.roomsSame);
    Check(st.rooms == st.shaders && st.roomsSame == st.rooms, "every ps_3_0 ExteriorWall copy reads its rooms' maps, the room's texel in every state");
    Check(st.strip == st.haze, "every copy that drops the strip also drops the outside's haze at the end");
    Check(st.strip == st.spec, "every copy that scales the reflection also takes the shade strip off (the albedo the light meets)");
    Check(st.shaders == 27 && st.patched == st.shaders && st.indoor == st.shaders && st.alphaAmb == st.shaders,
          "every ps_3_0 ExteriorWall shader takes the outdoor scale and the indoor light with the room's ambient");
    // the outdoor floors' copy (an upper outdoor floor under the roof: the attic's floored part over a room, 09/11)
    Stats fl;
    for (const char* tech : {"ExteriorFloors", "ExteriorFloor", "OutdoorFloor"}) {
        const auto floors = TechniquePs(data, tech);
        for (size_t i = 0; i < floors.size(); i++) CheckShader(gpu, floors[i], std::string("precomp ") + tech + " ps_3_0 #" + std::to_string(i), fl, true);
    }
    std::printf("Precomp outdoor floors ps_3_0: %d shaders, %d patched with the outdoor scale, %d with the indoor light (%d with the room's ambient, %d with the "
                "reflection), %d drawn; unmarked as before %d, marked by day as before %d, marked at night the indoor light %d (changed %d)\n",
                fl.shaders, fl.patched, fl.indoor, fl.alphaAmb, fl.spec, fl.drawn, fl.sameOff, fl.sameDay, fl.sameOn, fl.changedOn);
    // (a copy whose lamp mad writes its own map register, or with no temps to spare, keeps the outdoor response: counted, not refused)
    Check(fl.indoor > 0 && fl.drawn == fl.indoor && fl.sameOff == fl.drawn && fl.sameDay == fl.drawn && fl.sameOn == fl.drawn,
          "outdoor floors: every copy with the indoor light drawn, unmarked and by day bit for bit, at night the indoor light");
    Check(fl.rooms == 0, "outdoor floors: no copy reads the rooms' maps (their texels carry their room's top row)");
    Stats cap;
    for (int a = 2; a < argc; a++)
        for (const auto& e : std::filesystem::recursive_directory_iterator(argv[a])) {
            const std::string n = e.path().filename().string();
            if (!e.is_regular_file() || n.rfind("PS_", 0) != 0 || e.path().extension() != ".bin") continue;
            std::ifstream b(e.path(), std::ios::binary);
            const std::vector<unsigned char> raw{std::istreambuf_iterator<char>(b), {}};
            std::vector<DWORD> code = Trim(raw.data(), raw.size());
            if (!code.empty() && (code[0] == 0xFFFF0201 || code[0] == 0xFFFF0200)) { // (a ps_2_x interior copy: the Light capture's probes)
                int made = 0, created = 0;
                for (int k = 0; k < 32; k++) {
                    std::vector<DWORD> probe;
                    std::string what;
                    if (!ShaderPatches::ProbeCopy(code, k, probe, what)) break;
                    made++;
                    IDirect3DPixelShader9* ps = nullptr;
                    if (SUCCEEDED(gpu.dev->CreatePixelShader(probe.data(), &ps)) && ps) created++, ps->Release();
                }
                cap.probes += made, cap.probesCreated += created;
                Check(created == made, e.path().string() + ": the Light capture's probe copies of a ps_2_x shader are created natively");
            }
            if (code.empty() || code[0] != 0xFFFF0300) continue;
            CheckShader(gpu, code, e.path().string(), cap);
        }
    std::printf("Captured ps_3_0 pixel shaders: %d, %d taken by the patch, %d drawn, unmarked as before %d, by day as before %d, at night the indoor light %d, "
                "rooms' maps read %d, the room's texel in all six states %d\n",
                cap.shaders, cap.patched, cap.drawn, cap.sameOff, cap.sameDay, cap.sameOn, cap.rooms, cap.roomsSame);
    std::printf("Captured shaders' Light capture probe copies: %d, created natively %d\n", cap.probes, cap.probesCreated);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
