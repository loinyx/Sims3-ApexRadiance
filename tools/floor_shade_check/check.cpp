// Offline check of the ground lamp shade on outdoor floors (ShaderPatches PatchBakedAtlasPs / PatchFloor / PatchSnowFloor
// with the shade, WorldYOutput, GroundShadow::FloorShadeRows). Read-only: prints to stdout, writes no file.
//   check.exe <Shaders_Win32.precomp> [F7 capture folder ...]
// 1. Summer outdoor floors: every pixel shader of floor_atlas_table.h, with the TEXCOORD each paired vertex shader's copy
//    (PatchObjectLampVs) exports: patched with and without the shade; the shade is exactly the patch without it plus one
//    sampler declaration and the shade block right after the atlas max; D3DDisassemble within 512 slots; native D3D9 accepts it.
// 2. Winter floor tiles (kFloorVs, IsFloorVs) and snow on floors (IsSnowFloorVs, in the production classification order):
//    where their vertex shader writes world y (WorldYOutput), and their pixel shaders patched with and without the shade.
// 3. FloorShadeRows against GroundShadow::WorldTexel on a straight and a rotated lot, the gate and the off switch.
// 4. Native D3D9 pixels: a synthetic floor-shaped shader patched with the shade, a mask with a known alpha pattern (point
//    sampled), several heights over the lot's ground: out = map x (1 + gate (M - 1)) at every pixel (2 LSB).
// 5. Optional F7 capture folders: a captured summer floor copy (PS_*.bin) is found again as the patch of a table shader,
//    which takes the shaded patch.
// 6. Instanced lot structures (PatchInstancedLamps): every precomp pixel shader paired with an instanced vertex shader, both
//    laws, with and without the shade (as 1); native D3D9 pixels of a synthetic fence shader (ground light and one lamp, x the
//    shade); captured fence copies (PS_*.bin under the capture folders, recursively) found again.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include "../../features/ground_lamp_shadow.h"
#include "../../features/fence_cutout.h"
#include "../../features/lamp_cap_shadow.h"
#include "../../features/shader_patches.h"
#include "../../shaders/floor_atlas_table.h"
#include "../../shaders/shader_ids.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
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

// ---- Shaders_Win32.precomp (docs/engine/shaders.md) ----
struct Blob {
    bool vs = false;
    std::vector<DWORD> code;
};
struct Tech {
    std::vector<std::pair<uint32_t, uint32_t>> passes; // (vs, ps), 1-based blob indices
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
bool LoadPrecomp(const std::vector<unsigned char>& d, std::vector<Blob>& blobs, std::vector<Tech>& techs) {
    auto find = [&](const char* tag, size_t from, size_t to) -> size_t {
        for (size_t i = from; i + 4 <= to; i++)
            if (std::memcmp(&d[i], tag, 4) == 0) return i;
        return std::string::npos;
    };
    const size_t shdb = find("SHDB", 0, d.size());
    if (shdb == std::string::npos) return false;
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
    std::vector<size_t> at;
    for (size_t i = end; i + 4 <= d.size(); i++)
        if (std::memcmp(&d[i], "TECH", 4) == 0) at.push_back(i);
    for (size_t k = 0; k < at.size(); k++) {
        const size_t lim = k + 1 < at.size() ? at[k + 1] : d.size();
        Tech t;
        uint32_t np;
        std::memcpy(&np, &d[at[k] + 8], 4);
        size_t q = at[k] + 12;
        for (uint32_t i = 0; i < np && i < 64; i++) {
            q = find("PASS", q, lim);
            if (q == std::string::npos || q + 20 > lim) break;
            uint32_t vs, ps;
            std::memcpy(&vs, &d[q + 12], 4);
            std::memcpy(&ps, &d[q + 16], 4);
            t.passes.push_back({vs, ps});
            q += 4;
        }
        techs.push_back(std::move(t));
    }
    return !blobs.empty();
}

uint32_t Hash(const std::vector<DWORD>& t) { return ShaderHash(t.data(), t.size() * 4); }
bool InFloorTable(const std::vector<DWORD>& t) {
    const uint32_t h = Hash(t);
    for (const FloorAtlasEntry& e : kFloorAtlasTable)
        if (e.size == t.size() * 4 && e.hash == h) return true;
    return false;
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

// ---- hidden D3D9 device (64 x 64 target) ----
struct Gpu {
    HWND window = nullptr;
    IDirect3D9* api = nullptr;
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9 *target = nullptr, *readback = nullptr;
    Gpu() {
        window = CreateWindowExW(0, L"STATIC", L"Apex floor shade check", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        api = Direct3DCreate9(D3D_SDK_VERSION);
        D3DPRESENT_PARAMETERS p{};
        p.Windowed = TRUE;
        p.SwapEffect = D3DSWAPEFFECT_DISCARD;
        p.hDeviceWindow = window;
        p.BackBufferFormat = D3DFMT_UNKNOWN;
        p.BackBufferWidth = p.BackBufferHeight = 64;
        if (api) api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &p, &dev);
        Check(dev != nullptr, "native D3D9 device available");
        if (!dev) return;
        Check(SUCCEEDED(dev->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr)), "readback target");
        Check(SUCCEEDED(dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr)), "readback surface");
        dev->SetRenderTarget(0, target);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    }
    ~Gpu() {
        Release(target);
        Release(readback);
        Release(dev);
        Release(api);
        if (window) DestroyWindow(window);
    }
    bool AcceptsPs(const std::vector<DWORD>& t) {
        IDirect3DPixelShader9* s = nullptr;
        const bool ok = dev && SUCCEEDED(dev->CreatePixelShader(t.data(), &s));
        Release(s);
        return ok;
    }
    bool AcceptsVs(const std::vector<DWORD>& t) {
        IDirect3DVertexShader9* s = nullptr;
        const bool ok = dev && SUCCEEDED(dev->CreateVertexShader(t.data(), &s));
        Release(s);
        return ok;
    }
    // a full-screen quad, TEXCOORD0 = (0, 0) top left .. (1, 1) bottom right; D3D9 pixel centres: pixel x reads uv x / 64
    std::vector<DWORD> Render(IDirect3DPixelShader9* ps, IDirect3DVertexShader9* vs) {
        struct Vertex {
            float x, y, z, w, u, v;
        };
        const Vertex q[4] = {{-1, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 1, 0}, {-1, -1, 0, 1, 0, 1}, {1, -1, 0, 1, 1, 1}};
        IDirect3DVertexDeclaration9* decl = nullptr;
        const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                        {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                                        D3DDECL_END()};
        Check(SUCCEEDED(dev->CreateVertexDeclaration(el, &decl)), "test vertex layout");
        dev->SetVertexDeclaration(decl);
        dev->SetVertexShader(vs);
        dev->SetPixelShader(ps);
        dev->BeginScene();
        Check(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vertex))), "test quad draw");
        dev->EndScene();
        std::vector<DWORD> px(4096);
        D3DLOCKED_RECT r{};
        if (SUCCEEDED(dev->GetRenderTargetData(target, readback)) && SUCCEEDED(readback->LockRect(&r, nullptr, D3DLOCK_READONLY))) {
            for (int y = 0; y < 64; y++) std::memcpy(px.data() + y * 64, static_cast<char*>(r.pBits) + y * r.Pitch, 256);
            readback->UnlockRect();
        } else
            Check(false, "GPU readback");
        Release(decl);
        return px;
    }
};

std::vector<DWORD> Compile(const char* src, const char* profile) {
    ID3DBlob *code = nullptr, *err = nullptr;
    const HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr) && err) std::printf("%s\n", static_cast<const char*>(err->GetBufferPointer()));
    std::vector<DWORD> out;
    if (code) out.assign(static_cast<const DWORD*>(code->GetBufferPointer()), static_cast<const DWORD*>(code->GetBufferPointer()) + code->GetBufferSize() / 4);
    Release(code);
    Release(err);
    return out;
}

constexpr DWORD kDclSampler = 0x0200001Fu, kMaxOp = 0x0300000Bu, kMadOp = 0x04000004u, kMulOp = 0x03000005u;
constexpr size_t kShadeTokens = 36; // mad 5, mad 5, texld 4, add 4, mad 5, add 4, mad 5, mul 4

// with = without + (the sampler dcl: 3 tokens) + (the shade block: kShadeTokens, right after an atlas max, ending with the
// multiply of that max's destination). B = the destination register token's number.
// lead: tokens the block starts with before the shade (the fences' mad that moves the sample in front of the face: 5)
bool ShadeIsExactInsertion(const std::vector<DWORD>& with, const std::vector<DWORD>& without, DWORD S, size_t lead = 0) {
    const size_t tokens = lead + kShadeTokens;
    if (with.size() != without.size() + 3 + tokens) return false;
    size_t i = 0;
    while (i < without.size() && with[i] == without[i]) i++;
    if (i >= 2 && with[i - 2] == kDclSampler && with[i - 1] == 0x90000000u) i -= 2; // a dcl before another: the same first tokens
    std::vector<DWORD> rest(with.begin(), with.end());
    // the dcl is inserted at the first difference (the samplers' declarations)
    if (i + 3 > rest.size() || rest[i] != kDclSampler || rest[i + 1] != 0x90000000u || (rest[i + 2] & 0x7FF) != S) return false;
    rest.erase(rest.begin() + static_cast<ptrdiff_t>(i), rest.begin() + static_cast<ptrdiff_t>(i) + 3);
    size_t j = 0;
    while (j < without.size() && rest[j] == without[j]) j++;
    if (j > 0 && rest[j - 1] == kMadOp) j--; // the block's first opcode is the same as the game's mad after it
    if (j < 4 || j + tokens > rest.size() || without[j - 4] != kMaxOp) return false;
    const DWORD maxDst = without[j - 3];
    const DWORD* blk = &rest[j];
    if (blk[0] != kMadOp || (lead && blk[lead] != kMadOp) || blk[tokens - 4] != kMulOp || (blk[tokens - 3] & 0x7FF) != (maxDst & 0x7FF) ||
        ((blk[tokens - 3] >> 16) & 0xF) != 0x7)
        return false;
    rest.erase(rest.begin() + static_cast<ptrdiff_t>(j), rest.begin() + static_cast<ptrdiff_t>(j + tokens));
    return rest == without;
}

// ---- 1. summer outdoor floors ----
void SummerFloors(Gpu& gpu, const std::vector<Blob>& blobs, const std::vector<Tech>& techs) {
    std::printf("Summer outdoor floors (floor_atlas_table.h, %zu entries)\n", std::size(kFloorAtlasTable));
    std::map<uint32_t, std::set<uint32_t>> vsOf; // ps -> its vertex shaders
    for (const Tech& t : techs)
        for (auto [vs, ps] : t.passes)
            if (ps >= 1 && ps <= blobs.size() && vs >= 1 && vs <= blobs.size()) vsOf[ps].insert(vs);
    std::set<uint32_t> tableHashes;
    int found = 0, pairs = 0, shaded = 0, accepted = 0, maxSlots = 0, noRoom = 0, exitN = 0, exitMaxSlots = 0, scaled = 0;
    std::map<int, int> tcCount, wkCount;
    std::map<uint32_t, int> vsTc; // vs index -> TEXCOORD of its copy (-1 none)
    for (size_t i = 0; i < blobs.size(); i++) {
        const Blob& b = blobs[i];
        if (b.vs || b.code.empty() || !InFloorTable(b.code)) continue;
        if (!tableHashes.insert(Hash(b.code)).second) continue; // the same shader twice in the file
        found++;
        std::set<int> tcs;
        for (uint32_t v : vsOf[static_cast<uint32_t>(i + 1)]) {
            auto it = vsTc.find(v);
            if (it == vsTc.end()) {
                std::vector<DWORD> c = blobs[v - 1].code;
                int tc = -1, wk = -1;
                const bool ok = !c.empty() && ShaderPatches::PatchObjectLampVs(c, false, &tc, &wk);
                if (ok) {
                    Check(gpu.AcceptsVs(c), "summer floor: native D3D9 accepts the vertex shader copy");
                    wkCount[wk]++;
                }
                it = vsTc.emplace(v, ok ? tc : -1).first;
            }
            if (it->second >= 0) tcs.insert(it->second);
        }
        Check(!tcs.empty(), "summer floor: a paired vertex shader exports world xzy");
        for (int tc : tcs) {
            pairs++;
            tcCount[tc]++;
            std::vector<DWORD> a = b.code, n = b.code;
            ShaderPatches::FloorPatch fa, fn;
            const bool pa = ShaderPatches::PatchBakedAtlasPs(a, tc, fa, true), pn = ShaderPatches::PatchBakedAtlasPs(n, tc, fn, false);
            Check(pa && pn, "summer floor: the table's shader takes the patch, with and without the shade");
            if (!pa || !pn) continue;
            accepted++;
            // (2.15.3) the indoor light on the ground: taken where the shader has room, else exactly the shaded copy
            {
                std::vector<DWORD> e = b.code;
                ShaderPatches::FloorPatch fe;
                Check(ShaderPatches::PatchBakedAtlasPs(e, tc, fe, true, true), "summer floor exit: the table's shader takes the patch");
                if (!fe.exitLight) Check(e == a && !fe.exitLight, "summer floor exit: without room the copy is the shaded one");
                else {
                    exitN++;
                    const int s = Slots(Disasm(e));
                    exitMaxSlots = std::max(exitMaxSlots, s);
                    Check(s > 0 && s <= 512, "summer floor exit: disassembles within 512 slots");
                    Check(gpu.AcceptsPs(e), "summer floor exit: native D3D9 accepts the copy");
                    Check(fe.atlasSampler == fa.atlasSampler && fe.atlasConst == fa.atlasConst && fe.heightConst == fa.heightConst && fe.scaleConst == fa.scaleConst &&
                              fe.shadeSampler == fa.shadeSampler && fe.shadeConst == fa.shadeConst && fe.maskAlpha,
                          "summer floor exit: the same registers as the shaded copy");
                }
            }
            Check(fn.shadeSampler == -1 && fn.shadeConst == -1, "summer floor: no shade fields without the shade");
            // the outdoor scale: one sampler and one constant after every other the copy may take, the same in each variant;
            // none when that sampler would pass s15 (the floor keeps the cut at 1)
            const bool scaleRoom = fa.atlasSampler + 2 <= 15;
            Check(fn.outScaleSampler == fa.outScaleSampler && fn.outScaleConst == fa.outScaleConst &&
                      (scaleRoom ? fa.outScaleSampler == static_cast<int>(fa.atlasSampler) + 2 && fa.outScaleConst == fa.heightConst + 6 &&
                                       fa.mapSampler != static_cast<DWORD>(fa.outScaleSampler)
                                 : fa.outScaleSampler == -1 && fa.outScaleConst == -1),
                  "summer floor: the outdoor scale's sampler and constant follow the atlas, shade and indoor light registers");
            if (scaleRoom) scaled++;
            Check(fa.atlasSampler == fn.atlasSampler && fa.atlasConst == fn.atlasConst && fa.heightConst == fn.heightConst && fa.scaleConst == fn.scaleConst &&
                      fa.maskAlpha == fn.maskAlpha,
                  "summer floor: the shade leaves the atlas, weight and scale registers alone");
            if (fa.shadeSampler < 0) { // no sampler or constants to spare: the floor as before
                noRoom++;
                Check(a == n, "summer floor: without room the patch is the one before the shade");
                Check(gpu.AcceptsPs(n), "summer floor: native D3D9 accepts the copy without the shade");
                continue;
            }
            shaded++;
            Check(fa.shadeSampler == static_cast<int>(fa.atlasSampler) + 1 && fa.shadeConst == fa.heightConst + 1, "summer floor: shade registers after the atlas ones");
            const bool exact = ShadeIsExactInsertion(a, n, static_cast<DWORD>(fa.shadeSampler));
            Check(exact, "summer floor: the shade is the sampler dcl plus the block after the atlas max");
            const std::string text = Disasm(a);
            Check(!text.empty(), "summer floor: patched shader disassembles");
            Check(text.find("dcl_2d s" + std::to_string(fa.shadeSampler)) != std::string::npos && text.find("_abs") != std::string::npos,
                  "summer floor: the disassembly shows the mask sampler and the gate");
            const int slots = Slots(text);
            maxSlots = std::max(maxSlots, slots);
            Check(slots > 0 && slots <= 512, "summer floor: within 512 instruction slots");
            Check(Disasm(n) != "" && gpu.AcceptsPs(n), "summer floor: the patch without the shade still assembles");
            Check(gpu.AcceptsPs(a), "summer floor: native D3D9 accepts the shaded copy");
        }
    }
    Check(found == static_cast<int>(std::size(kFloorAtlasTable)), "summer floor: every table entry is in the precomp");
    std::printf("  table shaders found %d, (shader, TEXCOORD) pairs %d, patched %d, shaded %d (no sampler to spare %d), with the outdoor scale %d, most slots %d; TEXCOORD:",
                found, pairs, accepted, shaded, noRoom, scaled, maxSlots);
    for (auto [tc, n] : tcCount) std::printf(" %d x%d", tc, n);
    std::printf("; vertex shader copies' world rows from:");
    for (auto [wk, n] : wkCount) std::printf(" c%d x%d", wk, n);
    std::printf("\n  indoor light on the ground: %d pairs take it, most slots %d\n", exitN, exitMaxSlots);
}

// ---- 2. winter floor tiles and snow on floors ----
// The production classification (lot_light_bridge ClassifyVs) before a vertex shader is a snow floor (class 11)
bool ClaimedBeforeSnowFloor(std::vector<DWORD> t) {
    DWORD m = 0;
    if (ShaderPatches::IsRoadVs(t, m) || ShaderPatches::IsInstancedStructureVs(t) || ShaderPatches::IsSnowCoverVs(t) || ShaderPatches::IsSnowReliefVs(t) ||
        ShaderPatches::IsFloorVs(t))
        return true;
    std::vector<DWORD> f = t;
    if (ShaderPatches::PatchFoliageVs(f)) return true;
    std::vector<DWORD> o = t;
    int wk = -1, vl = -1;
    return ShaderPatches::PatchObjectLampVs(o, true, nullptr, &wk, &vl);
}
void WinterFloors(Gpu& gpu, const std::vector<Blob>& blobs, const std::vector<Tech>& techs) {
    std::map<uint32_t, std::set<uint32_t>> psOf; // vs -> its pixel shaders
    for (const Tech& t : techs)
        for (auto [vs, ps] : t.passes)
            if (ps >= 1 && ps <= blobs.size() && vs >= 1 && vs <= blobs.size()) psOf[vs].insert(ps);
    for (int kind = 0; kind < 2; kind++) {
        const char* name = kind == 0 ? "Winter floor tiles" : "Snow on floors";
        int vsN = 0, vsY = 0, psN = 0, psShaded = 0, psRefusedBoth = 0, psNoRoom = 0, psExit = 0, psGainOnly = 0;
        std::set<uint32_t> seenVs;
        std::map<std::string, int> yAt;
        for (size_t i = 0; i < blobs.size(); i++) {
            const Blob& b = blobs[i];
            if (!b.vs || b.code.empty() || !seenVs.insert(Hash(b.code)).second) continue;
            int tc = 7;
            const bool isFloor = IsShader(kFloorVs, b.code.data(), b.code.size() * 4) || ShaderPatches::IsFloorVs(b.code);
            const bool isSnow = !isFloor && ShaderPatches::IsSnowFloorVs(b.code, tc) && !ClaimedBeforeSnowFloor(b.code);
            if (kind == 0 ? !isFloor : !isSnow) continue;
            vsN++;
            ShaderPatches::WorldY y;
            const bool hasY = ShaderPatches::WorldYOutput(b.code, y);
            if (hasY) {
                vsY++;
                yAt[std::string("TEXCOORD") + std::to_string(y.texcoord) + "." + "xyzw"[y.comp]]++;
            }
            for (uint32_t p : psOf[static_cast<uint32_t>(i + 1)]) {
                const std::vector<DWORD>& ps = blobs[p - 1].code;
                if (ps.empty() || ps[0] != 0xFFFF0300u) continue;
                std::vector<DWORD> a = ps, n = ps;
                ShaderPatches::FloorPatch fa, fn;
                const bool pa = kind == 0 ? ShaderPatches::PatchFloor(a, fa, y) : ShaderPatches::PatchSnowFloor(a, tc, fa, y);
                const bool pn = kind == 0 ? ShaderPatches::PatchFloor(n, fn) : ShaderPatches::PatchSnowFloor(n, tc, fn);
                Check(pa == pn, "winter: the shade never changes which shaders the patch takes");
                if (!pn) {
                    Check(a == ps, "winter: a refused shader is left unchanged");
                    psRefusedBoth++;
                    continue;
                }
                psN++;
                if (kind == 0 && fn.gainOnly) { // no sampler left for the atlas: the game's bytes, the lamp scale known
                    psGainOnly++;
                    Check(fa.gainOnly && a == ps && n == ps && fn.scaleConst >= 0 && fa.scaleConst == fn.scaleConst, "winter: a gain-only floor keeps the game's bytes and names its lamp scale");
                    continue;
                }
                Check(fn.shadeSampler == -1, "winter: no shade without world y");
                Check(fa.atlasSampler == fn.atlasSampler && fa.atlasConst == fn.atlasConst && fa.heightConst == fn.heightConst && fa.scaleConst == fn.scaleConst,
                      "winter: the shade leaves the atlas, weight and scale registers alone");
                if (!hasY) {
                    Check(a == n && fa.shadeSampler == -1, "winter: without world y the patch is the one before the shade");
                    continue;
                }
                if (fa.shadeSampler < 0) { // no sampler or constants to spare: the floor as before
                    psNoRoom++;
                    Check(a == n, "winter: without room the patch is the one before the shade");
                    continue;
                }
                psShaded++;
                Check(fa.shadeSampler == static_cast<int>(fa.atlasSampler) + 1 && fa.shadeConst == static_cast<int>(fa.atlasConst) + 2, "winter: shade registers");
                {
                    // (2.15.3) the indoor light on the ground, else exactly the shaded copy
                    std::vector<DWORD> e = ps;
                    ShaderPatches::FloorPatch fe;
                    const bool pe = kind == 0 ? ShaderPatches::PatchFloor(e, fe, y, true) : ShaderPatches::PatchSnowFloor(e, tc, fe, y, true);
                    Check(pe, "winter exit: the shader takes the patch");
                    if (pe && !fe.exitLight) Check(e == a, "winter exit: without room the copy is the shaded one");
                    if (pe && fe.exitLight) {
                        psExit++;
                        const int s = Slots(Disasm(e));
                        Check(s > 0 && s <= 512, "winter exit: disassembles within 512 slots");
                        Check(gpu.AcceptsPs(e), "winter exit: native D3D9 accepts the copy");
                        Check(fe.atlasSampler == fa.atlasSampler && fe.atlasConst == fa.atlasConst && fe.heightConst == fa.heightConst && fe.scaleConst == fa.scaleConst &&
                                  fe.shadeSampler == fa.shadeSampler && fe.shadeConst == fa.shadeConst && fe.maskAlpha,
                              "winter exit: the same registers as the shaded copy");
                    }
                }
                const std::string text = Disasm(a);
                Check(!text.empty() && Slots(text) > 0 && Slots(text) <= 512, "winter: patched shader disassembles within 512 slots");
                // world y read from the TEXCOORD the vertex shader writes it to
                Check(text.find(std::string("dcl_texcoord") + (y.texcoord ? std::to_string(y.texcoord) : std::string())) != std::string::npos,
                      "winter: the pixel shader declares the world y TEXCOORD");
                Check(gpu.AcceptsPs(a), "winter: native D3D9 accepts the shaded copy");
                Check(gpu.AcceptsPs(n), "winter: native D3D9 accepts the copy without the shade");
            }
        }
        Check(psRefusedBoth == 0, "winter: every pixel shader of these floors takes the patch (no weather variant left at the game's lamp scale)");
        std::printf("%s: %d vertex shaders, %d write world y (", name, vsN, vsY);
        for (auto& [k, v] : yAt) std::printf(" %s x%d", k.c_str(), v);
        std::printf(" ); pixel shaders patched %d (lamp scale only %d), shaded %d (no sampler to spare %d), refused by the patch %d; indoor light on the ground %d\n", psN,
                    psGainOnly, psShaded, psNoRoom, psRefusedBoth, psExit);
    }
}

// ---- 3. CPU rows ----
void Rows() {
    using namespace GroundShadow;
    const int before = g_checks;
    Grid straight;
    straight.w = 160, straight.h = 120, straight.tx = 100.0f, straight.tz = 50.0f, straight.ty = 3.0f;
    Grid rot = straight;
    rot.m0 = 0, rot.m2 = 1, rot.m8 = -1, rot.m10 = 0, rot.tx = 140.0f; // world x = 140 - lz, world z = 50 + lx
    for (const Grid* g : {&straight, &rot})
        for (float s : {1.0f, 2.0f}) {
            float rows[12];
            Check(FloorShadeRows(*g, s, true, rows), "rows: a lot grid gives rows");
            float worst = 0;
            for (int k = 0; k < 50; k++) {
                const float lx = 0.37f + k * 0.71f, lz = 0.11f + k * 0.53f;
                const float wx = g->m0 * lx + g->m8 * lz + g->tx, wz = g->m2 * lx + g->m10 * lz + g->tz;
                float fi, fj;
                WorldTexel(*g, wx, wz, fi, fj);
                const float u = wx / s * rows[0] + wz / s * rows[4] + rows[2], v = wx / s * rows[1] + wz / s * rows[5] + rows[3];
                worst = std::max({worst, std::fabs(u * g->w - fi), std::fabs(v * g->h - fj)});
            }
            Check(worst < 2e-3f, "rows: mask uv x size = WorldTexel (straight and rotated lots, world xz and xz / 2)");
            const auto gate = [&](float dy) { return std::clamp(std::fabs(g->ty + dy + rows[6]) * rows[9] + rows[8], 0.0f, 1.0f); };
            Check(rows[7] == 1.0f && gate(0) == 1.0f && gate(-0.9f) == 1.0f && std::fabs(gate(1.0f) - 1.0f) < 1e-5f && gate(1.25f) == 0.0f && gate(3.0f) == 0.0f &&
                      gate(-2.9f) == 0.0f && std::fabs(gate(1.125f) - 0.5f) < 1e-5f,
                  "rows: gate 1 within 1 m of the ground, 0 from 1.25 m, linear between");
            Check(FloorShadeRows(*g, s, false, rows) && rows[8] == 0.0f && rows[9] == 0.0f, "rows: off = gate 0 everywhere");
        }
    Grid bad;
    float rows[12];
    bad.m0 = bad.m10 = 0;
    bad.w = bad.h = 4;
    Check(!FloorShadeRows(bad, 1.0f, true, rows) && rows[8] == 0.0f && rows[9] == 0.0f, "rows: a singular matrix gives the floor as before");
    std::printf("FloorShadeRows: %d checks\n", g_checks - before);
}

// ---- 4. GPU pixels ----
void Pixels(Gpu& gpu) {
    if (!gpu.dev) return;
    const int before = g_checks;
    // a floor-shaped shader: "texld rL, v0, s0" then "mad rX.xyz, rL, c0.x, rB" (rB = a second texture)
    const std::vector<DWORD> ps = Compile("sampler2D map:register(s0); sampler2D base:register(s1); float4 k:register(c0); float4 s:register(c2);\n"
                                          "float4 main(float2 uv:TEXCOORD0):COLOR0{float4 m=tex2D(map,uv);float3 b=tex2D(base,uv).rgb;return float4((m.rgb*k.x+b)*s.x,1);}",
                                          "ps_3_0");
    const std::vector<DWORD> vsCode = Compile("float4 rows[3]:register(c0);\n"
                                              "struct O{float4 p:POSITION;float2 uv:TEXCOORD0;float3 w:TEXCOORD7;};\n"
                                              "O main(float4 p:POSITION,float2 uv:TEXCOORD0){O o;o.p=p;o.uv=uv;o.w=rows[0].xyz+uv.x*rows[1].xyz+uv.y*rows[2].xyz;return o;}",
                                              "vs_3_0");
    std::vector<DWORD> code = ps;
    ShaderPatches::FloorPatch f;
    const bool ok = !code.empty() && ShaderPatches::PatchBakedAtlasPs(code, 7, f, true) && f.shadeSampler >= 0;
    Check(ok, "pixels: the synthetic floor shader takes the shaded patch");
    if (!ok) {
        std::printf("%s\n", Disasm(ps).c_str());
        return;
    }
    IDirect3DPixelShader9* shader = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(code.data(), &shader)), "pixels: D3D9 accepts the patched synthetic shader");
    Check(!vsCode.empty() && SUCCEEDED(gpu.dev->CreateVertexShader(vsCode.data(), &vs)), "pixels: fixture vertex shader");
    // textures: the map 0.8 grey (alpha 0: the atlas weight's input), the base and the atlas black, the mask's alpha a pattern
    const auto solid = [&](DWORD argb, int w, int h, auto fill) {
        IDirect3DTexture9* t = nullptr;
        if (FAILED(gpu.dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return t;
        D3DLOCKED_RECT r{};
        t->LockRect(0, &r, nullptr, 0);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits) + y * r.Pitch)[x] = fill ? fill(x, y) : argb;
        t->UnlockRect(0);
        return t;
    };
    using Fill = DWORD (*)(int, int);
    IDirect3DTexture9* map = solid(0x00CCCCCCu, 4, 4, Fill(nullptr));
    IDirect3DTexture9* black = solid(0x00000000u, 4, 4, Fill(nullptr));
    IDirect3DTexture9* white = solid(0xFFFFFFFFu, 1, 1, Fill(nullptr));
    const auto alphaOf = [](int i, int j) { return static_cast<DWORD>((i * 37 + j * 91 + (i * j) % 17 * 13) & 255); };
    IDirect3DTexture9* mask = solid(0, 160, 120, +[](int i, int j) { return ((static_cast<DWORD>((i * 37 + j * 91 + (i * j) % 17 * 13) & 255)) << 24) | 0xFFFFFFu; });
    if (!shader || !vs || !map || !black || !white || !mask) {
        Check(false, "pixels: fixture resources");
    } else {
        gpu.dev->SetTexture(0, map);
        gpu.dev->SetTexture(1, black);
        gpu.dev->SetTexture(f.atlasSampler, black);
        const DWORD samplers[] = {0, 1, f.atlasSampler, static_cast<DWORD>(f.shadeSampler)};
        for (DWORD s : samplers) {
            gpu.dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            gpu.dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            gpu.dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            gpu.dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
        const float k[4] = {1, 0, 0, 0}, one[4] = {1, 0, 0, 0}, atlasC[4] = {0, 0, 0, 0}, weight[4] = {1, 0, 0, 0};
        gpu.dev->SetPixelShaderConstantF(0, k, 1);
        gpu.dev->SetPixelShaderConstantF(2, one, 1);
        gpu.dev->SetPixelShaderConstantF(f.atlasConst, atlasC, 1);
        gpu.dev->SetPixelShaderConstantF(static_cast<UINT>(f.heightConst), weight, 1);
        GroundShadow::Grid straight;
        straight.w = 160, straight.h = 120, straight.tx = 100.0f, straight.tz = 50.0f, straight.ty = 3.0f;
        GroundShadow::Grid rot = straight;
        rot.m0 = 0, rot.m2 = 1, rot.m8 = -1, rot.m10 = 0, rot.tx = 140.0f;
        for (const GroundShadow::Grid* g : {&straight, &rot})
            for (int mode = 0; mode < 3; mode++)        // 0 shade, 1 off (gate 0), 2 white mask
                for (float dy : {0.0f, -0.6f, 0.9f, 1.1f, 1.2f, 2.0f, 3.0f}) {
                    if (mode && dy != 0.0f) continue;
                    float rows[12];
                    GroundShadow::FloorShadeRows(*g, 1.0f, mode != 1, rows);
                    gpu.dev->SetPixelShaderConstantF(static_cast<UINT>(f.shadeConst), rows, 3);
                    gpu.dev->SetTexture(static_cast<DWORD>(f.shadeSampler), mode == 2 ? white : mask);
                    // pixel (x, y) at lot-local (0.3 + 39 u, 0.3 + 29 v): world (x, z, y) through the lot matrix
                    const float lx0 = 0.3f, lz0 = 0.3f, du = 39.0f, dv = 29.0f, y = g->ty + dy;
                    const float vrows[3][4] = {{g->m0 * lx0 + g->m8 * lz0 + g->tx, g->m2 * lx0 + g->m10 * lz0 + g->tz, y, 0},
                                               {g->m0 * du, g->m2 * du, 0, 0},
                                               {g->m8 * dv, g->m10 * dv, 0, 0}};
                    gpu.dev->SetVertexShaderConstantF(0, vrows[0], 3);
                    const auto px = gpu.Render(shader, vs);
                    const float gate = mode == 0 ? std::clamp(5.0f - 4.0f * std::fabs(dy), 0.0f, 1.0f) : 0.0f;
                    int worst = 0;
                    for (int py = 0; py < 64; py++)
                        for (int pxi = 0; pxi < 64; pxi++) {
                            const float lx = lx0 + du * pxi / 64.0f, lz = lz0 + dv * py / 64.0f;
                            const int i = std::min(159, static_cast<int>(lx * 4.0f)), j = std::min(119, static_cast<int>(lz * 4.0f));
                            const float M = mode == 2 ? 1.0f : alphaOf(i, j) / 255.0f;
                            const float want = 0.8f * (1.0f + gate * (M - 1.0f));
                            const int got = static_cast<int>((px[py * 64 + pxi] >> 8) & 255);
                            worst = std::max(worst, std::abs(got - static_cast<int>(std::lround(want * 255.0f))));
                        }
                    char what[160];
                    std::snprintf(what, sizeof what, "pixels: %s lot, %s, %.1f m over the ground: out = map x (1 + gate (M - 1)) within 2 LSB (worst %d)",
                                  g == &straight ? "straight" : "rotated", mode == 0 ? "shade" : mode == 1 ? "off" : "white mask", dy, worst);
                    Check(worst <= 2, what);
                    if (worst > 2) std::printf("  %s\n", what);
                }
    }
    for (DWORD s = 0; s < 16; s++) gpu.dev->SetTexture(s, nullptr);
    gpu.dev->SetPixelShader(nullptr);
    gpu.dev->SetVertexShader(nullptr);
    Release(shader);
    Release(vs);
    Release(map);
    Release(black);
    Release(white);
    Release(mask);
    std::printf("Floor shade pixels: %d checks\n", g_checks - before);
}

// ---- 6b. Mesh fences let the lamps' light through (GroundShadow::Wall::pass): the cutout of every precomp instanced pixel
// shader with a texkill (ShaderPatches::FindCutout), the open share of a texture's alpha (FenceCutout::OpenShare) in each
// format, and a wall's pass in the ground shade, hard and soft
void FenceCutoutChecks(const std::vector<std::vector<DWORD>>& shaders) {
    const int before = g_checks;
    int kills = 0, found = 0, tinted = 0;
    std::map<float, int> scales;
    for (const auto& ps : shaders) {
        const std::string text = Disasm(ps);
        if (text.find("texkill") == std::string::npos) continue;
        kills++;
        ShaderPatches::Cutout c;
        if (!ShaderPatches::FindCutout(ps, c)) continue;
        found++;
        tinted += c.tintConst >= 0;
        scales[c.scale]++;
        // the sampler found is the one a texld of the shader reads
        Check(text.find(", s" + std::to_string(c.sampler) + "\n") != std::string::npos || text.find(", s" + std::to_string(c.sampler) + "\r") != std::string::npos,
              "fence cutout: the sampler is one the shader reads");
    }
    Check(kills > 0 && found == kills, "fence cutout: every instanced pixel shader with a texkill has its cutout found");
    std::printf("Fence cutouts: %d of %d instanced pixel shaders with a texkill (%d tinted); scales:", found, kills, tinted);
    for (auto [s, n] : scales) std::printf(" %.0f x%d", s, n);
    std::printf("\n");
    // the open share: half the texels under the kept alpha, in each format
    using FenceCutout::Fmt;
    uint8_t argb[4 * 4 * 4] = {};
    for (int i = 0; i < 16; i++) argb[i * 4 + 3] = i < 8 ? 0 : 255;
    Check(std::fabs(FenceCutout::OpenShare(Fmt::Argb, argb, 16, 4, 4, 128) - 0.5f) < 1e-6f, "fence open share: A8R8G8B8, half transparent");
    const uint8_t dxt1[8] = {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00}; // c0 < c1: index 3 transparent; rows 0-1 index 3, rows 2-3 index 0
    Check(std::fabs(FenceCutout::OpenShare(Fmt::Dxt1, dxt1, 8, 4, 4, 128) - 0.5f) < 1e-6f, "fence open share: DXT1, half transparent");
    uint8_t dxt3[16] = {0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    Check(std::fabs(FenceCutout::OpenShare(Fmt::Dxt3, dxt3, 16, 4, 4, 128) - 0.5f) < 1e-6f, "fence open share: DXT3, half transparent");
    uint8_t dxt5[16] = {255, 0}; // a0 > a1: index 0 = 255, index 1 = 0; texels 8..15 take index 1
    for (int i = 8; i < 16; i++) {
        const int bit = 16 + 3 * i;
        dxt5[bit >> 3] |= static_cast<uint8_t>(1u << (bit & 7));
    }
    Check(std::fabs(FenceCutout::OpenShare(Fmt::Dxt5, dxt5, 16, 4, 4, 128) - 0.5f) < 1e-6f, "fence open share: DXT5, half transparent");
    // the ground shade: a lamp 3 m up at the origin, a 2 m fence at x = 2, the point at x = 4 behind it
    GroundShadow::Lamp L{0.0f, 3.0f, 0.0f, 1.0f};
    GroundShadow::Wall w{2.0f, -5.0f, 2.0f, 5.0f, 0.0f, 2.0f};
    Check(GroundShadow::Pass(L, 4.0f, 0.0f, 0.0f, w) == 0.0f, "fence pass: a solid wall blocks (hard)");
    w.pass = 0.8f;
    Check(std::fabs(GroundShadow::Pass(L, 4.0f, 0.0f, 0.0f, w) - 0.8f) < 1e-6f, "fence pass: a mesh fence lets its open share through (hard)");
    w.free0 = w.free1 = true;
    const float soft = GroundShadow::Pass(L, 4.0f, 0.0f, 0.0f, w, nullptr, true);
    Check(std::fabs(soft - 0.8f) < 0.01f, "fence pass: and the same in the middle of its soft shadow");
    Check(GroundShadow::Pass(L, 1.0f, 0.0f, 0.0f, w) == 1.0f, "fence pass: in front of the fence the light is whole");
    std::printf("Fence pass: %d checks (soft middle %.3f)\n", g_checks - before, soft);
}

// ---- 4c. Snow on objects mixed with the fog in its last instructions (PatchSnowCover without the final "mul oC0.xyz"): the
// ground atlas x strength is added to the moon light right after "mad rL.xyz, rX.w, c0, rY", on native D3D9
void SnowCoverFogPixels(Gpu& gpu) {
    if (!gpu.dev) return;
    const int before = g_checks;
    const std::vector<DWORD> ps = Compile("sampler2D snow:register(s0); float4 moon:register(c0); float4 sky:register(c1); float4 mixv:register(c2); float4 k:register(c3);\n"
                                          "float4 main(float4 col:COLOR0, float2 uv:TEXCOORD0, float2 w:TEXCOORD3):COLOR0{float sh=saturate(col.a*k.x+w.x*k.y+w.y*k.z);"
                                          "float3 l=sh*moon.rgb+col.rgb*sky.rgb; float3 s=tex2D(snow,uv).rgb*l; return float4(lerp(col.rgb,s,mixv.w),1);}",
                                          "ps_3_0");
    std::vector<DWORD> code = ps;
    ShaderPatches::SnowCoverPatch p;
    const bool ok = !code.empty() && ShaderPatches::PatchSnowCover(code, p);
    Check(ok, "snow cover with the fog mix: the light mad takes the ground atlas");
    if (!ok) {
        std::printf("%s\n", Disasm(ps).c_str());
        return;
    }
    IDirect3DPixelShader9* shader = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(code.data(), &shader)), "snow cover fog: D3D9 accepts the copy");
    const std::vector<DWORD> vsCode = Compile("struct O{float4 p:POSITION;float4 c:COLOR0;float2 uv:TEXCOORD0;float2 w:TEXCOORD3;};\n"
                                              "O main(float4 p:POSITION,float2 uv:TEXCOORD0){O o;o.p=p;o.c=float4(0,0,0,0.5);o.uv=uv;o.w=uv;return o;}",
                                              "vs_3_0");
    Check(!vsCode.empty() && SUCCEEDED(gpu.dev->CreateVertexShader(vsCode.data(), &vs)), "snow cover fog: fixture vertex shader");
    const auto solid = [&](DWORD argb) {
        IDirect3DTexture9* t = nullptr;
        if (FAILED(gpu.dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return t;
        D3DLOCKED_RECT r{};
        t->LockRect(0, &r, nullptr, 0);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits) + y * r.Pitch)[x] = argb;
        t->UnlockRect(0);
        return t;
    };
    IDirect3DTexture9 *snow = solid(0xFFFFFFFFu), *atlas = solid(0xFF666666u);
    if (shader && vs && snow && atlas) {
        gpu.dev->SetTexture(0, snow);
        gpu.dev->SetTexture(p.atlasSampler, atlas);
        const float zero[4] = {}, mixv[4] = {0, 0, 0, 1}, kc[4] = {2, 0, 0, 0}, atlasC[4] = {1, 1, 0, 0};
        gpu.dev->SetPixelShaderConstantF(0, zero, 1);
        gpu.dev->SetPixelShaderConstantF(1, zero, 1);
        gpu.dev->SetPixelShaderConstantF(2, mixv, 1);
        gpu.dev->SetPixelShaderConstantF(3, kc, 1);
        gpu.dev->SetPixelShaderConstantF(p.atlasConst, atlasC, 1);
        for (float s : {1.0f, 0.5f, 0.0f}) {
            const float sc[4] = {s, 0, 0, 0};
            gpu.dev->SetPixelShaderConstantF(p.strengthConst, sc, 1);
            const auto px = gpu.Render(shader, vs);
            int worst = 0;
            for (DWORD q : px) worst = std::max(worst, std::abs(static_cast<int>((q >> 8) & 255) - static_cast<int>(std::lround(0.4f * s * 255.0f))));
            char what[112];
            std::snprintf(what, sizeof what, "snow cover fog pixels: snow x (atlas 0.4 x strength %.1f) within 2 LSB (worst %d)", s, worst);
            Check(worst <= 2, what);
            if (worst > 2) std::printf("  %s\n", what);
        }
    } else {
        Check(false, "snow cover fog: fixture resources");
    }
    for (DWORD s = 0; s < 16; s++) gpu.dev->SetTexture(s, nullptr);
    gpu.dev->SetPixelShader(nullptr);
    gpu.dev->SetVertexShader(nullptr);
    Release(shader);
    Release(vs);
    Release(snow);
    Release(atlas);
    std::printf("Snow cover fog pixels: %d checks\n", g_checks - before);
}

// ---- 4a. The weather floors' map read (PatchFloor, the map straight into the lamp scale: "mad rY.xyz, rM, cK.x, rZ"):
// out = max(map, atlas x weight) x k + base, with the atlas at TEXCOORD0.zw, on native D3D9
void WeatherFloorPixels(Gpu& gpu) {
    if (!gpu.dev) return;
    const int before = g_checks;
    const std::vector<DWORD> ps = Compile("sampler2D map:register(s0); sampler2D base:register(s1); float4 k:register(c0); float4 d:register(c1);\n"
                                          "float4 main(float4 uv:TEXCOORD0, float2 muv:TEXCOORD2):COLOR0{float3 l=tex2D(map,muv).rgb*k.x+tex2D(base,uv.xy).rgb;"
                                          "return float4(l/(1+uv.x*d.x),1);}",
                                          "ps_3_0");
    std::vector<DWORD> code = ps;
    ShaderPatches::FloorPatch f;
    const bool ok = !code.empty() && ShaderPatches::PatchFloor(code, f) && f.scaleConst == 0 && f.heightConst >= 0;
    Check(ok, "weather floor pixels: the map-into-scale shader takes the patch, lamp scale c0");
    if (!ok) {
        std::printf("%s\n", Disasm(ps).c_str());
        return;
    }
    IDirect3DPixelShader9* shader = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(code.data(), &shader)), "weather floor pixels: D3D9 accepts the copy");
    const std::vector<DWORD> vsCode = Compile("struct O{float4 p:POSITION;float4 uv:TEXCOORD0;float2 m:TEXCOORD2;};\n"
                                              "O main(float4 p:POSITION,float2 uv:TEXCOORD0){O o;o.p=p;o.uv=float4(uv,uv);o.m=uv;return o;}",
                                              "vs_3_0");
    Check(!vsCode.empty() && SUCCEEDED(gpu.dev->CreateVertexShader(vsCode.data(), &vs)), "weather floor pixels: fixture vertex shader");
    const auto solid = [&](DWORD argb) {
        IDirect3DTexture9* t = nullptr;
        if (FAILED(gpu.dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return t;
        D3DLOCKED_RECT r{};
        t->LockRect(0, &r, nullptr, 0);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits) + y * r.Pitch)[x] = argb;
        t->UnlockRect(0);
        return t;
    };
    IDirect3DTexture9 *map = solid(0x00333333u), *base = solid(0xFF000000u), *atlasHigh = solid(0xFF999999u), *atlasLow = solid(0xFF1A1A1Au);
    if (shader && vs && map && base && atlasHigh && atlasLow) {
        gpu.dev->SetTexture(0, map);
        gpu.dev->SetTexture(1, base);
        const float atlasC[4] = {1, 1, 0, 0}, weight[4] = {1, 0, 0, 0}, none[4] = {};
        gpu.dev->SetPixelShaderConstantF(f.atlasConst, atlasC, 1);
        gpu.dev->SetPixelShaderConstantF(static_cast<UINT>(f.heightConst), weight, 1);
        gpu.dev->SetPixelShaderConstantF(1, none, 1);
        for (IDirect3DTexture9* atlas : {atlasHigh, atlasLow})
            for (float k : {1.0f, 0.263f}) {
                gpu.dev->SetTexture(f.atlasSampler, atlas);
                const float kc[4] = {k, 0, 0, 0};
                gpu.dev->SetPixelShaderConstantF(0, kc, 1);
                const auto px = gpu.Render(shader, vs);
                const float want = std::max(0.2f, atlas == atlasHigh ? 0.6f : 0.102f) * k;
                int worst = 0;
                for (DWORD p : px) worst = std::max(worst, std::abs(static_cast<int>((p >> 8) & 255) - static_cast<int>(std::lround(want * 255.0f))));
                char what[128];
                std::snprintf(what, sizeof what, "weather floor pixels: max(map 0.2, atlas %.1f) x %.3f within 2 LSB (worst %d)", atlas == atlasHigh ? 0.6f : 0.1f, k, worst);
                Check(worst <= 2, what);
                if (worst > 2) std::printf("  %s\n", what);
            }
    } else {
        Check(false, "weather floor pixels: fixture resources");
    }
    for (DWORD s = 0; s < 16; s++) gpu.dev->SetTexture(s, nullptr);
    gpu.dev->SetPixelShader(nullptr);
    gpu.dev->SetVertexShader(nullptr);
    Release(shader);
    Release(vs);
    Release(map);
    Release(base);
    Release(atlasHigh);
    Release(atlasLow);
    std::printf("Weather floor pixels: %d checks\n", g_checks - before);
}

// ---- 4b. (2.15.3) GPU pixels of the indoor light on the ground: the synthetic floor shader patched with exitLight, a map
// whose rgb and alpha vary per pixel (a = 1 in the first column: a roofless room's floor), a grey atlas, the lot mask's
// alpha pattern; at seven heights, with the shade, with the gate rows over the white mask (the shade off), off (gate 0) and
// with the atlas weight constant (1, -1) and (1, 0): out = max(L - ex, atlas tw) (1 + g (M - 1)) + ex within 2 LSB.
void ExitPixels(Gpu& gpu) {
    if (!gpu.dev) return;
    const int before = g_checks;
    const std::vector<DWORD> ps = Compile("sampler2D map:register(s0); sampler2D base:register(s1); float4 k:register(c0); float4 s:register(c2);\n"
                                          "float4 main(float2 uv:TEXCOORD0):COLOR0{float4 m=tex2D(map,uv);float3 b=tex2D(base,uv).rgb;return float4((m.rgb*k.x+b)*s.x,1);}",
                                          "ps_3_0");
    const std::vector<DWORD> vsCode = Compile("float4 rows[3]:register(c0);\n"
                                              "struct O{float4 p:POSITION;float2 uv:TEXCOORD0;float3 w:TEXCOORD7;};\n"
                                              "O main(float4 p:POSITION,float2 uv:TEXCOORD0){O o;o.p=p;o.uv=uv;o.w=rows[0].xyz+uv.x*rows[1].xyz+uv.y*rows[2].xyz;return o;}",
                                              "vs_3_0");
    std::vector<DWORD> code = ps;
    ShaderPatches::FloorPatch f;
    const bool ok = !code.empty() && ShaderPatches::PatchBakedAtlasPs(code, 7, f, true, true) && f.exitLight;
    Check(ok, "exit pixels: the synthetic floor shader takes the indoor light on the ground");
    if (!ok) return;
    IDirect3DPixelShader9* shader = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(code.data(), &shader)), "exit pixels: D3D9 accepts the patched synthetic shader");
    Check(!vsCode.empty() && SUCCEEDED(gpu.dev->CreateVertexShader(vsCode.data(), &vs)), "exit pixels: fixture vertex shader");
    const auto make = [&](int w, int h, DWORD (*fill)(int, int)) {
        IDirect3DTexture9* t = nullptr;
        if (FAILED(gpu.dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return t;
        D3DLOCKED_RECT r{};
        t->LockRect(0, &r, nullptr, 0);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits) + y * r.Pitch)[x] = fill(x, y);
        t->UnlockRect(0);
        return t;
    };
    const auto mapOf = [](int x, int y) -> DWORD {
        const DWORD a = x == 0 ? 255u : static_cast<DWORD>((x * 3 + y * 5) & 255), r = static_cast<DWORD>((x * 4 + 10) & 255), g = static_cast<DWORD>((y * 4 + 20) & 255),
                    b = static_cast<DWORD>(((x + y) * 2) & 255);
        return a << 24 | r << 16 | g << 8 | b;
    };
    const auto alphaOf = [](int i, int j) { return static_cast<DWORD>((i * 37 + j * 91 + (i * j) % 17 * 13) & 255); };
    IDirect3DTexture9* map = make(64, 64, +[](int x, int y) -> DWORD {
        const DWORD a = x == 0 ? 255u : static_cast<DWORD>((x * 3 + y * 5) & 255), r = static_cast<DWORD>((x * 4 + 10) & 255), g = static_cast<DWORD>((y * 4 + 20) & 255),
                    b = static_cast<DWORD>(((x + y) * 2) & 255);
        return a << 24 | r << 16 | g << 8 | b;
    });
    IDirect3DTexture9* black = make(4, 4, +[](int, int) -> DWORD { return 0u; });
    IDirect3DTexture9* atlas = make(4, 4, +[](int, int) -> DWORD { return 0x00707070u; });
    IDirect3DTexture9* white = make(1, 1, +[](int, int) -> DWORD { return 0xFFFFFFFFu; });
    IDirect3DTexture9* mask = make(160, 120, +[](int i, int j) -> DWORD { return ((static_cast<DWORD>((i * 37 + j * 91 + (i * j) % 17 * 13) & 255)) << 24) | 0xFFFFFFu; });
    if (!shader || !vs || !map || !black || !atlas || !white || !mask) {
        Check(false, "exit pixels: fixture resources");
    } else {
        gpu.dev->SetTexture(0, map);
        gpu.dev->SetTexture(1, black);
        gpu.dev->SetTexture(f.atlasSampler, atlas);
        const DWORD samplers[] = {0, 1, f.atlasSampler, static_cast<DWORD>(f.shadeSampler)};
        for (DWORD s : samplers) {
            gpu.dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            gpu.dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            gpu.dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            gpu.dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
        const float k[4] = {1, 0, 0, 0}, one[4] = {1, 0, 0, 0}, atlasC[4] = {0, 0, 0, 0};
        gpu.dev->SetPixelShaderConstantF(0, k, 1);
        gpu.dev->SetPixelShaderConstantF(2, one, 1);
        gpu.dev->SetPixelShaderConstantF(f.atlasConst, atlasC, 1);
        GroundShadow::Grid g;
        g.w = 160, g.h = 120, g.tx = 100.0f, g.tz = 50.0f, g.ty = 3.0f;
        int worstAll = 0;
        for (float cHy : {-1.0f, 0.0f}) {
            const float weight[4] = {1, cHy, 0, 0};
            gpu.dev->SetPixelShaderConstantF(static_cast<UINT>(f.heightConst), weight, 1);
            for (int mode = 0; mode < 3; mode++) // 0 shade, 1 gate rows over the white mask (shade off), 2 off (gate 0)
                for (float dy : {0.0f, -0.6f, 0.9f, 1.1f, 1.2f, 2.0f, 3.0f}) {
                    if (mode == 2 && dy != 0.0f) continue;
                    float rows[12];
                    GroundShadow::FloorShadeRows(g, 1.0f, mode != 2, rows);
                    gpu.dev->SetPixelShaderConstantF(static_cast<UINT>(f.shadeConst), rows, 3);
                    gpu.dev->SetTexture(static_cast<DWORD>(f.shadeSampler), mode == 0 ? mask : white);
                    const float lx0 = 0.3f, lz0 = 0.3f, du = 39.0f, dv = 29.0f, y = g.ty + dy;
                    const float vrows[3][4] = {{lx0 + g.tx, lz0 + g.tz, y, 0}, {du, 0, 0, 0}, {0, dv, 0, 0}};
                    gpu.dev->SetVertexShaderConstantF(0, vrows[0], 3);
                    const auto px = gpu.Render(shader, vs);
                    const float gate = mode == 2 ? 0.0f : std::clamp(5.0f - 4.0f * std::fabs(dy), 0.0f, 1.0f);
                    int worst = 0;
                    for (int py = 0; py < 64; py++)
                        for (int pxi = 0; pxi < 64; pxi++) {
                            const float lx = lx0 + du * pxi / 64.0f, lz = lz0 + dv * py / 64.0f;
                            const int i = std::min(159, static_cast<int>(lx * 4.0f)), j = std::min(119, static_cast<int>(lz * 4.0f));
                            const float M = mode == 0 ? alphaOf(i, j) / 255.0f : 1.0f;
                            const DWORD m = mapOf(pxi, py);
                            const float L[3] = {((m >> 16) & 255) / 255.0f, ((m >> 8) & 255) / 255.0f, (m & 255) / 255.0f}, a = (m >> 24) / 255.0f;
                            const float lum = 0.2126f * L[0] + 0.7152f * L[1] + 0.0722f * L[2];
                            const float tw = std::clamp((1.0f + a * cHy) * (1.0f + 254.0f * gate), 0.0f, 1.0f);
                            const float s = gate * std::clamp(a / std::max(lum, 1.0f / 255.0f), 0.0f, 1.0f);
                            for (int c = 0; c < 3; c++) {
                                const float ex = L[c] * s;
                                const float want = std::clamp(std::max(L[c] - ex, 0x70 / 255.0f * tw) * (1.0f + gate * (M - 1.0f)) + ex, 0.0f, 1.0f);
                                const int got = static_cast<int>((px[py * 64 + pxi] >> (16 - 8 * c)) & 255);
                                worst = std::max(worst, std::abs(got - static_cast<int>(std::lround(want * 255.0f))));
                            }
                        }
                    worstAll = std::max(worstAll, worst);
                    char what[200];
                    std::snprintf(what, sizeof what, "exit pixels: weight (1, %.0f), %s, %.1f m over the ground: out = max(L - ex, atlas tw) (1 + g (M - 1)) + ex within 2 LSB (worst %d)",
                                  cHy, mode == 0 ? "shade" : mode == 1 ? "gate over the white mask" : "off", dy, worst);
                    Check(worst <= 2, what);
                    if (worst > 2) std::printf("  %s\n", what);
                }
        }
        std::printf("Indoor light on the ground, floor pixels: largest difference %d LSB\n", worstAll);
    }
    for (DWORD s = 0; s < 16; s++) gpu.dev->SetTexture(s, nullptr);
    gpu.dev->SetPixelShader(nullptr);
    gpu.dev->SetVertexShader(nullptr);
    Release(shader);
    Release(vs);
    Release(map);
    Release(black);
    Release(atlas);
    Release(white);
    Release(mask);
    std::printf("Indoor light on the ground, floor pixels: %d checks\n", g_checks - before);
}

// ---- 5. captured outdoor floor copies (optional F7 folders): each PS_*.bin that is the summer floor patch (without the
// shade) of a table shader is found again, and that shader takes the shaded patch
void Captured(const std::vector<Blob>& blobs, const char* folder) {
    int tested = 0, matched = 0;
    for (const auto& e : std::filesystem::directory_iterator(folder)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("PS_", 0) != 0 || e.path().extension() != ".bin") continue;
        std::ifstream f(e.path(), std::ios::binary);
        const std::vector<unsigned char> raw((std::istreambuf_iterator<char>(f)), {});
        if (raw.size() < 8 || raw.size() % 4) continue;
        std::vector<DWORD> cap(raw.size() / 4);
        std::memcpy(cap.data(), raw.data(), raw.size());
        tested++;
        for (const Blob& b : blobs) {
            if (b.vs || b.code.empty() || !InFloorTable(b.code)) continue;
            for (int tc = 7; tc <= 9; tc++) {
                std::vector<DWORD> n = b.code, a = b.code;
                ShaderPatches::FloorPatch fn, fa;
                if (!ShaderPatches::PatchBakedAtlasPs(n, tc, fn, false) || n != cap) continue;
                matched++;
                const bool ok = ShaderPatches::PatchBakedAtlasPs(a, tc, fa, true) && fa.shadeSampler >= 0;
                Check(ok, "captured: the captured floor's shader takes the shaded patch");
                std::printf("Captured %s: the summer floor copy of a %zu-byte table shader (TEXCOORD%d); shaded copy: mask s%d, rows c%d..c%d\n", name.c_str(),
                            b.code.size() * 4, tc, fa.shadeSampler, fa.shadeConst, fa.shadeConst + 2);
            }
        }
    }
    std::printf("Captured %s: %d pixel shaders, %d summer floor copies recognised\n", folder, tested, matched);
}

// ---- 6. instanced lot structures (fences, curbs, railings, stairs: PatchInstancedLamps) ----
// Every pixel shader the precomp pairs with an instanced structure vertex shader (IsInstancedStructureVs), both laws, with
// and without the shade: the shade never changes which shaders are taken; the shaded copy is the copy without it plus one
// sampler declaration and the shade block after the lamp max; within 512 slots; native D3D9 accepts both.
std::vector<std::vector<DWORD>> InstancedPs(const std::vector<Blob>& blobs, const std::vector<Tech>& techs) {
    std::set<uint32_t> instVs, seen;
    for (size_t i = 0; i < blobs.size(); i++)
        if (blobs[i].vs && !blobs[i].code.empty() && ShaderPatches::IsInstancedStructureVs(blobs[i].code)) instVs.insert(static_cast<uint32_t>(i + 1));
    std::vector<std::vector<DWORD>> out;
    for (const Tech& t : techs)
        for (auto [vs, ps] : t.passes)
            if (instVs.count(vs) && ps >= 1 && ps <= blobs.size() && !blobs[ps - 1].code.empty() && blobs[ps - 1].code[0] == 0xFFFF0300u &&
                seen.insert(Hash(blobs[ps - 1].code)).second)
                out.push_back(blobs[ps - 1].code);
    return out;
}
// what: the line's label; returns the number of shaders shaded by the first law
int Instanced(Gpu& gpu, const std::vector<std::vector<DWORD>>& shaders, const char* what) {
    constexpr DWORD N = ShaderPatches::kObjectPixelLamps;
    int patched[2] = {}, shaded[2] = {}, noLamps[2] = {}, refused[2] = {}, maxSlots = 0;
    for (const auto& ps : shaders)
        for (int reach = 0; reach < 2; reach++) {
            std::vector<DWORD> a = ps, n = ps;
            ShaderPatches::InstancedPatch ia, in;
            const bool pa = ShaderPatches::PatchInstancedLamps(a, ia, reach != 0, true), pn = ShaderPatches::PatchInstancedLamps(n, in, reach != 0, false);
            Check(pa == pn, "instanced: the shade never changes which shaders the patch takes");
            if (!pn) {
                Check(a == ps && n == ps, "instanced: a refused shader is left unchanged");
                refused[reach]++;
                continue;
            }
            patched[reach]++;
            Check(in.shadeSampler == -1 && in.shadeConst == -1, "instanced: no shade fields without the shade");
            Check(ia.atlasSampler == in.atlasSampler && ia.atlasConst == in.atlasConst && ia.strengthConst == in.strengthConst && ia.pixelLamps == in.pixelLamps &&
                      ia.lampParamConst == in.lampParamConst && ia.lampConst == in.lampConst && ia.groundWeightConst == in.groundWeightConst &&
                      ia.coneConst == in.coneConst && ia.reachConst == in.reachConst,
                  "instanced: the shade leaves the atlas, lamp, cone and reach registers alone");
            if (ia.shadeSampler < 0) { // no per-pixel lamps (no world y), or no room: the copy as before
                noLamps[reach] += !ia.pixelLamps;
                Check(a == n, "instanced: without the shade the copy is the one before it");
                Check(gpu.AcceptsPs(n), "instanced: native D3D9 accepts the copy without the shade");
                continue;
            }
            shaded[reach]++;
            Check(ia.pixelLamps, "instanced: the shade only with the per-pixel lamps");
            Check(ia.shadeSampler == static_cast<int>(ia.atlasSampler) + 1 &&
                      ia.shadeConst == static_cast<int>(reach ? ia.reachConst + 2 : ia.coneConst + 2 * N),
                  "instanced: shade registers after the atlas sampler and after the last lamp rows (one more upload)");
            Check(ShadeIsExactInsertion(a, n, static_cast<DWORD>(ia.shadeSampler), 5),
                  "instanced: the shade is the sampler dcl plus the block after the lamp max (the sample moved in front of the face first)");
            const std::string text = Disasm(a);
            const int slots = Slots(text);
            maxSlots = std::max(maxSlots, slots);
            Check(slots > 0 && slots <= 512, "instanced: shaded copy within 512 instruction slots");
            Check(text.find("dcl_2d s" + std::to_string(ia.shadeSampler)) != std::string::npos && text.find("_abs") != std::string::npos,
                  "instanced: the disassembly shows the mask sampler and the gate");
            Check(gpu.AcceptsPs(a), "instanced: native D3D9 accepts the shaded copy");
            Check(gpu.AcceptsPs(n), "instanced: native D3D9 accepts the copy without the shade");
            std::vector<DWORD> cap = ps;
            ShaderPatches::InstancedPatch ic;
            const bool capOk = ShaderPatches::PatchInstancedLamps(cap, ic, reach != 0, true, true);
            Check(capOk && ic.capConst > 0, "instanced: emitter floor shadows cover the native shader");
            if (capOk) {
                Check(ic.lampParamConst == ia.lampParamConst && ic.shadeConst == ia.shadeConst,
                      "instanced: emitter caps preserve the existing constant layout");
                Check(gpu.AcceptsPs(cap), "instanced: native D3D9 accepts emitter floor shadows");
            }
        }
    std::printf("Instanced structures (%s): %zu pixel shaders; first law patched %d, shaded %d (no per-pixel lamps %d), refused %d; reach patched %d, shaded %d "
                "(no per-pixel lamps %d), refused %d; most slots %d\n",
                what, shaders.size(), patched[0], shaded[0], noLamps[0], refused[0], patched[1], shaded[1], noLamps[1], refused[1], maxSlots);
    return shaded[0];
}
// Captured shaders (optional folders, searched recursively: F7 captures, refused shaders): every distinct ps_3_0 *.bin
// through the same checks as the precomp's; and each PS_*.bin that is a precomp instanced shader's copy without the shade
// (either law) is found again; that shader takes the shaded copy of the same law
void InstancedCaptured(Gpu& gpu, const std::vector<Blob>& blobs, const std::vector<Tech>& techs, const char* folder) {
    const auto shaders = InstancedPs(blobs, techs);
    std::map<uint32_t, std::pair<size_t, int>> copies; // hash of the copy without the shade -> (shader, law)
    for (size_t i = 0; i < shaders.size(); i++)
        for (int reach = 0; reach < 2; reach++) {
            std::vector<DWORD> n = shaders[i];
            ShaderPatches::InstancedPatch in;
            if (ShaderPatches::PatchInstancedLamps(n, in, reach != 0, false)) copies[Hash(n)] = {i, reach};
        }
    int tested = 0, matched = 0;
    std::set<uint32_t> seen;
    std::vector<std::vector<DWORD>> all;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file() || it->path().extension() != ".bin" || it->file_size() > 262144) continue;
        std::ifstream f(it->path(), std::ios::binary);
        const std::vector<unsigned char> raw((std::istreambuf_iterator<char>(f)), {});
        if (raw.size() < 8 || raw.size() % 4) continue;
        std::vector<DWORD> cap(raw.size() / 4);
        std::memcpy(cap.data(), raw.data(), raw.size());
        if (cap[0] != 0xFFFF0300u || !seen.insert(Hash(cap)).second) continue;
        tested++;
        all.push_back(cap);
        const auto m = copies.find(Hash(cap));
        if (m == copies.end()) continue;
        std::vector<DWORD> n = shaders[m->second.first], a = n;
        ShaderPatches::InstancedPatch in, ia;
        ShaderPatches::PatchInstancedLamps(n, in, m->second.second != 0, false);
        if (n != cap) continue;
        matched++;
        const bool ok = ShaderPatches::PatchInstancedLamps(a, ia, m->second.second != 0, true) && ia.shadeSampler >= 0;
        Check(ok, "instanced captured: the captured copy's shader takes the shaded copy");
        std::printf("  %s (%s law): shaded copy mask s%d, rows c%d..c%d\n", it->path().string().c_str(), m->second.second ? "reach" : "first", ia.shadeSampler,
                    ia.shadeConst, ia.shadeConst + 2);
    }
    std::printf("Instanced captured %s: %d distinct ps_3_0 shaders, %d fence/stairs copies recognised\n", folder, tested, matched);
    Instanced(gpu, all, folder);
}
// Native D3D9 pixels: a synthetic fence-shaped shader (COLOR0 added once, TEXCOORD1.zw world xz, TEXCOORD2 normal and world
// y) patched with the shade, over the mask pattern of Pixels. Ground light only (the atlas white x 0.8 on a face turned up):
// out = 0.8 (1 + gate (M - 1)). One lamp only: out = the copy's own output with the white mask x (1 + gate (M - 1)). 2 LSB.
void InstancedPixels(Gpu& gpu) {
    if (!gpu.dev) return;
    const int before = g_checks;
    constexpr unsigned N = ShaderPatches::kObjectPixelLamps;
    const std::vector<DWORD> ps = Compile("sampler2D base:register(s0); float4 k:register(c0);\n"
                                          "float4 main(float4 c:COLOR0,float4 uv:TEXCOORD1,float4 n:TEXCOORD2):COLOR0{\n"
                                          "float3 s=max(tex2D(base,uv.xy).rgb,k.xyz);float3 t=s+c.rgb;return float4(t*k.w,n.w*k.w);}",
                                          "ps_3_0");
    const std::vector<DWORD> vsCode = Compile("float4 rows[3]:register(c0);\n"
                                              "struct O{float4 p:POSITION;float4 c:COLOR0;float4 uv:TEXCOORD1;float4 n:TEXCOORD2;};\n"
                                              "O main(float4 p:POSITION,float2 uv:TEXCOORD0){O o;o.p=p;o.c=0;\n"
                                              "float3 w=rows[0].xyz+uv.x*rows[1].xyz+uv.y*rows[2].xyz;o.uv=float4(uv,w.x,w.z);o.n=float4(rows[0].w,rows[1].w,rows[2].w,w.y);return o;}",
                                              "vs_3_0");
    IDirect3DVertexShader9* vs = nullptr;
    Check(!vsCode.empty() && SUCCEEDED(gpu.dev->CreateVertexShader(vsCode.data(), &vs)), "instanced pixels: fixture vertex shader");
    const auto make = [&](int w, int h, bool pattern, DWORD argb) {
        IDirect3DTexture9* t = nullptr;
        if (FAILED(gpu.dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return t;
        D3DLOCKED_RECT r{};
        t->LockRect(0, &r, nullptr, 0);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits) + y * r.Pitch)[x] = pattern ? ((static_cast<DWORD>((x * 37 + y * 91 + (x * y) % 17 * 13) & 255)) << 24) | 0xFFFFFFu : argb;
        t->UnlockRect(0);
        return t;
    };
    const auto alphaOf = [](int i, int j) { return static_cast<DWORD>((i * 37 + j * 91 + (i * j) % 17 * 13) & 255); };
    IDirect3DTexture9 *black = make(1, 1, false, 0), *white = make(1, 1, false, 0xFFFFFFFFu), *mask = make(160, 120, true, 0);
    for (bool reach : {false, true}) {
        std::vector<DWORD> code = ps;
        ShaderPatches::InstancedPatch o;
        const bool ok = !code.empty() && ShaderPatches::PatchInstancedLamps(code, o, reach, true) && o.pixelLamps && o.shadeSampler >= 0;
        Check(ok, "instanced pixels: the synthetic fence shader takes the shaded copy");
        IDirect3DPixelShader9* shader = nullptr;
        if (!ok || !vs || !black || !white || !mask || FAILED(gpu.dev->CreatePixelShader(code.data(), &shader))) {
            Check(false, "instanced pixels: fixture resources");
            Release(shader);
            continue;
        }
        const DWORD samplers[] = {0, o.atlasSampler, static_cast<DWORD>(o.shadeSampler)};
        for (DWORD s : samplers) {
            gpu.dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            gpu.dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            gpu.dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            gpu.dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
        gpu.dev->SetTexture(0, black);
        const float base[4] = {0, 0, 0, 1}, atlasC[4] = {1, 1, 0, 0}, weight[4] = {1, 0, 1, 0};
        gpu.dev->SetPixelShaderConstantF(0, base, 1);
        gpu.dev->SetPixelShaderConstantF(o.atlasConst, atlasC, 1);
        gpu.dev->SetPixelShaderConstantF(o.groundWeightConst, weight, 1);
        GroundShadow::Grid g;
        g.w = 160, g.h = 120, g.tx = 100.0f, g.tz = 50.0f, g.ty = 3.0f;
        const float lx0 = 0.3f, lz0 = 0.3f, du = 39.0f, dv = 29.0f;
        for (int lampMode = 0; lampMode < 2; lampMode++)
            for (float dy : {0.0f, 0.6f, 1.1f, 2.0f}) {
                // the lamps: none (ground light only) or one 2 m over the middle, 40 m reach, colour 0.6 (first law; under 1, so
                // the white-mask reference is not clipped) or 30 (reach law: its sum is held under 1), on a face turned up
                float lamps[1 + 4 * N + 2][4] = {};
                lamps[0][0] = 1, lamps[0][1] = 1, lamps[0][3] = 1e-4f;
                for (unsigned k = 0; k < N; k++) {
                    lamps[1 + 2 * k][0] = lamps[1 + 2 * k][2] = 1e6f;
                    lamps[2 + 2 * k][3] = lamps[1 + 2 * N + 2 * k][3] = lamps[2 + 2 * N + 2 * k][3] = 1.0f;
                }
                if (lampMode) {
                    lamps[1][0] = g.tx + 20.0f, lamps[1][1] = g.ty + dy + 2.0f, lamps[1][2] = g.tz + 15.0f, lamps[1][3] = 1.0f / (40.0f * 40.0f);
                    lamps[2][0] = lamps[2][1] = lamps[2][2] = reach ? 30.0f : 0.6f;
                    lamps[1 + 4 * N][0] = 100.0f;
                }
                const float strength[4] = {lampMode ? 0.0f : 0.8f, 0, 0, 0};
                gpu.dev->SetPixelShaderConstantF(o.strengthConst, strength, 1);
                gpu.dev->SetPixelShaderConstantF(o.lampParamConst, lamps[0], 1 + 4 * N + (reach ? 2 : 0));
                gpu.dev->SetTexture(o.atlasSampler, white);
                const float vrows[3][4] = {{g.tx + lx0, g.ty + dy, g.tz + lz0, 0}, {du, 0, 0, 1}, {0, 0, dv, 0}}; // normal (0, 1, 0)
                gpu.dev->SetVertexShaderConstantF(0, vrows[0], 3);
                float rows[12];
                GroundShadow::FloorShadeRows(g, 1.0f, true, rows);
                gpu.dev->SetPixelShaderConstantF(static_cast<UINT>(o.shadeConst), rows, 3);
                gpu.dev->SetTexture(static_cast<DWORD>(o.shadeSampler), white);
                const auto ref = gpu.Render(shader, vs);
                gpu.dev->SetTexture(static_cast<DWORD>(o.shadeSampler), mask);
                const auto px = gpu.Render(shader, vs);
                const float gate = std::clamp(5.0f - 4.0f * std::fabs(dy), 0.0f, 1.0f);
                int worst = 0, lit = 0;
                for (int py = 0; py < 64; py++)
                    for (int pxi = 0; pxi < 64; pxi++) {
                        const float lx = lx0 + du * pxi / 64.0f, lz = lz0 + dv * py / 64.0f;
                        const int i = std::min(159, static_cast<int>(lx * 4.0f)), j = std::min(119, static_cast<int>(lz * 4.0f));
                        const float f = 1.0f + gate * (alphaOf(i, j) / 255.0f - 1.0f);
                        const int r = static_cast<int>((ref[py * 64 + pxi] >> 8) & 255);
                        lit += r > 8;
                        const float want = lampMode ? r / 255.0f * f : 0.8f * f;
                        if (!lampMode) worst = std::max(worst, std::abs(r - 204)); // the white mask: the sum unchanged
                        worst = std::max(worst, std::abs(static_cast<int>((px[py * 64 + pxi] >> 8) & 255) - static_cast<int>(std::lround(want * 255.0f))));
                    }
                char what[200];
                std::snprintf(what, sizeof what, "instanced pixels: %s law, %s, %.1f m over the ground: out = sum x (1 + gate (M - 1)) within 2 LSB (worst %d, lit %d)",
                              reach ? "reach" : "first", lampMode ? "one lamp" : "ground light", dy, worst, lit);
                Check(worst <= 2 && lit > 1000, what);
                if (worst > 2 || lit <= 1000) std::printf("  %s\n", what);
            }
        for (DWORD s = 0; s < 16; s++) gpu.dev->SetTexture(s, nullptr);
        gpu.dev->SetPixelShader(nullptr);
        gpu.dev->SetVertexShader(nullptr);
        Release(shader);
    }
    // A lantern above a short solid pillar: the group can see it from one end,
    // but pixels behind its complete cap must still be shadowed independently.
    for (bool reach : {false, true}) {
        auto code = ps;
        ShaderPatches::InstancedPatch o;
        const bool ok = ShaderPatches::PatchInstancedLamps(code, o, reach, true, true);
        IDirect3DPixelShader9* shader = nullptr;
        const HRESULT capHr = ok ? gpu.dev->CreatePixelShader(code.data(), &shader) : E_FAIL;
        Check(ok && SUCCEEDED(capHr), "cap pixels: native shader created");
        if (FAILED(capHr)) {
            const auto text = Disasm(code);
            std::printf("cap shader: reach %d HRESULT %08lX slots %d\n", reach, static_cast<unsigned long>(capHr), Slots(text));
            using Assemble = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, UINT, ID3DBlob**, ID3DBlob**);
            const auto compiler = LoadLibraryW(L"d3dcompiler_47.dll");
            const auto assemble = compiler ? reinterpret_cast<Assemble>(GetProcAddress(compiler, "D3DAssemble")) : nullptr;
            ID3DBlob *assembled = nullptr, *errors = nullptr;
            if (assemble) assemble(text.data(), text.size(), nullptr, nullptr, nullptr, 0, &assembled, &errors);
            if (errors) std::printf("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
            Release(assembled); Release(errors);
            if (compiler) FreeLibrary(compiler);
        }
        if (!shader) continue;
        const float base[4] = {0, 0, 0, 1}, zero[4]{}, one[4] = {1, 0, 1, 0};
        gpu.dev->SetPixelShaderConstantF(0, base, 1);
        gpu.dev->SetPixelShaderConstantF(o.strengthConst, zero, 1);
        gpu.dev->SetPixelShaderConstantF(o.groundWeightConst, one, 1);
        gpu.dev->SetTexture(0, black);
        gpu.dev->SetTexture(o.atlasSampler, black);
        gpu.dev->SetTexture(static_cast<DWORD>(o.shadeSampler), white);
        float shade[12]{}; shade[7] = 1;
        gpu.dev->SetPixelShaderConstantF(static_cast<UINT>(o.shadeConst), shade, 3);
        float lamps[1 + 4 * N + 2][4]{};
        lamps[0][0] = lamps[0][1] = 1, lamps[0][3] = 1e-4f;
        for (unsigned k = 0; k < N; ++k) {
            lamps[1 + 2 * k][0] = lamps[1 + 2 * k][2] = 1e6f;
            lamps[2 + 2 * k][3] = lamps[1 + 2 * N + 2 * k][3] = lamps[2 + 2 * N + 2 * k][3] = 1;
        }
        lamps[1][0] = lamps[1][2] = 0, lamps[1][1] = 2.372f, lamps[1][3] = 1.0f / 36.0f;
        lamps[2][0] = 0.6f;
        lamps[3][1] = 2.372f, lamps[3][3] = 1.0f / 36.0f;
        lamps[4][1] = 0.3f;
        lamps[1 + 4 * N][0] = 4;
        lamps[1 + 4 * N][1] = 4;
        gpu.dev->SetPixelShaderConstantF(o.lampParamConst, lamps[0], 1 + 4 * N + (reach ? 2 : 0));
        for (int layout : {0, 1}) for (float height : {0.75f, 1.25f, 2.0f}) {
            const float cx = layout ? 636.775f : 0.0f, cz = layout ? 762.2866f : 0.0f;
            const float co = layout ? 0.9902679f : 1.0f, si = layout ? 0.1391745f : 0.0f;
            lamps[1][0] = cx, lamps[1][2] = cz, lamps[3][0] = cx + 3, lamps[3][2] = cz;
            gpu.dev->SetPixelShaderConstantF(o.lampParamConst, lamps[0], 1 + 4 * N + (reach ? 2 : 0));
            const float vr[3][4] = {{cx - 3, height, cz - 3, 0}, {6, 0, 0, 1}, {0, 0, 6, 0}};
            gpu.dev->SetVertexShaderConstantF(0, vr[0], 3);
            float caps[2 * N + 1][4]{};
            caps[2 * N][0] = caps[2 * N][2] = 0.5f;
            caps[2 * N][1] = 0.15f, caps[2 * N][3] = 1e-4f;
            gpu.dev->SetPixelShaderConstantF(o.capConst, caps[0], 2 * N + 1);
            const auto beforeCap = gpu.Render(shader, vs);
            caps[0][0] = co, caps[0][1] = -si, caps[0][2] = -co * cx + si * cz, caps[0][3] = 0.497f;
            caps[1][0] = si, caps[1][1] = co, caps[1][2] = -si * cx - co * cz;
            gpu.dev->SetPixelShaderConstantF(o.capConst, caps[0], 2 * N + 1);
            const auto afterCap = gpu.Render(shader, vs);
            int worst = 0, darkened = 0, greenDifference = 0;
            const float head[3] = {cx, 2.372f, cz};
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) {
                    const float point[3] = {cx - 3 + 6.0f * x / 64.0f, height, cz - 3 + 6.0f * y / 64.0f};
                    const float pass = LampCapShadow::Pass(reinterpret_cast<const float (*)[4]>(caps), head, point, 0.15f);
                    const int a = (beforeCap[y * 64 + x] >> 16) & 255, b = (afterCap[y * 64 + x] >> 16) & 255;
                    worst = std::max(worst, std::abs(b - static_cast<int>(std::lround(a * pass))));
                    darkened += a > b + 2;
                    greenDifference = std::max(greenDifference, std::abs(static_cast<int>((beforeCap[y * 64 + x] >> 8) & 255) - static_cast<int>((afterCap[y * 64 + x] >> 8) & 255)));
                }
            Check(worst <= 2, "cap pixels: native shader agrees with the CPU ray and soft edge within two LSB");
            Check(height < 1.875f ? darkened > 100 : darkened == 0, "cap pixels: only receivers below the cap lose the blocked lamp");
            Check(greenDifference <= 1, "cap pixels: the unblocked green lamp keeps its contribution");
            if (worst > 2) std::printf("cap pixels: reach %d height %.3f worst %d\n", reach, height, worst);
        }
        Release(shader);
    }
    for (DWORD s = 0; s < 16; ++s) gpu.dev->SetTexture(s, nullptr);
    gpu.dev->SetPixelShader(nullptr);
    gpu.dev->SetVertexShader(nullptr);
    Release(vs);
    Release(black);
    Release(white);
    Release(mask);
    std::printf("Instanced shade pixels: %d checks\n", g_checks - before);
}

// ---- 7. (2.15.3) The snowy, rug-cut and melting-snow lot light passes (PatchLotTerrainMax): the precomp's three shaders
// with the room mask, and with the mask and the indoor light on the ground (exit). Both disassemble within 512 slots and
// native D3D9 accepts them; on a hidden device with every input varied, the exit copy with the gate c202.x = 0, or with
// the gate 1 over a lot map of alpha 0, draws exactly the pixels of the masked copy, and with the gate 1 over a lot map
// whose alpha varies it changes them.
IDirect3DTexture9* Pattern(Gpu& gpu, DWORD seed, bool alpha) {
    IDirect3DTexture9* t = nullptr;
    if (FAILED(gpu.dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
    D3DLOCKED_RECT r{};
    t->LockRect(0, &r, nullptr, 0);
    for (DWORD y = 0; y < 64; y++)
        for (DWORD x = 0; x < 64; x++) {
            const DWORD v = (x * 7 + y * 13 + seed * 29) & 255, w = (x * 3 + y * 5 + seed * 11) & 255, z = (x * 11 + y * 2 + seed * 7) & 255;
            reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits) + y * r.Pitch)[x] = (alpha ? ((x * 4 + y) & 255) << 24 : 0u) | v << 16 | w << 8 | z;
        }
    t->UnlockRect(0);
    return t;
}
void SnowLotPasses(Gpu& gpu, const std::vector<Blob>& blobs) {
    const int before = g_checks;
    struct Pass {
        ShaderId id;
        DWORD last, terrain;
        const char* name;
    } const passes[] = {{kSnowLotPs, 11, 12, "snowy lot pass"}, {kSnowLotCutPs, 12, 13, "snowy lot pass under rugs"}, {kMeltLotPs, 13, 14, "melting-snow lot pass"}};
    const auto vsCode = Compile(R"(
struct Out { float4 pos : POSITION; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
             float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7; float4 c : COLOR0; };
Out main(float4 pos : POSITION, float2 uv : TEXCOORD0) {
    Out o; o.pos = pos;
    o.t0 = float4(uv, uv.yx); o.t1 = float4(uv * 0.9 + 0.05, 1 - uv); o.t2 = float4(uv * 0.5 + 0.25, 0.5, 1);
    o.t3 = float4(uv.yx, uv); o.t4 = float4(normalize(float3(uv.x - 0.5, 1, uv.y - 0.5)), 1); o.t5 = float4(uv * 0.7 + 0.1, uv);
    o.t6 = float4(uv, uv * 0.8); o.t7 = float4(uv.x * 4, 0.5, uv.y * 4, 0.6); o.c = float4(uv, 0.5, 0.3); return o;
})", "vs_3_0");
    IDirect3DVertexShader9* vs = nullptr;
    Check(!vsCode.empty() && SUCCEEDED(gpu.dev->CreateVertexShader(vsCode.data(), &vs)), "snowy passes: test vertex shader");
    IDirect3DCubeTexture9* cube = nullptr;
    gpu.dev->CreateCubeTexture(1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, nullptr);
    if (cube)
        for (int f = 0; f < 6; f++) {
            D3DLOCKED_RECT r{};
            cube->LockRect(static_cast<D3DCUBEMAP_FACES>(f), 0, &r, nullptr, 0);
            *static_cast<DWORD*>(r.pBits) = 0xFF405060u + static_cast<DWORD>(f) * 0x00101010u;
            cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(f), 0);
        }
    IDirect3DTexture9* tex[16] = {};
    for (DWORD s = 2; s < 16; s++) tex[s] = Pattern(gpu, s, true);
    IDirect3DTexture9* lotNoAlpha = Pattern(gpu, 2, false);
    for (const Pass& p : passes) {
        const Blob* blob = nullptr;
        for (const Blob& b : blobs)
            if (!b.vs && IsShader(p.id, b.code.data(), b.code.size() * 4)) blob = &b;
        Check(blob != nullptr, "snowy passes: the shader is in the precomp");
        if (!blob) continue;
        std::vector<DWORD> masked = blob->code, exit = blob->code;
        const bool pm = ShaderPatches::PatchLotTerrainMax(masked, p.last, p.terrain, true), pe = ShaderPatches::PatchLotTerrainMax(exit, p.last, p.terrain, true, true);
        Check(pm && pe, "snowy passes: the masked and the exit copies are made");
        if (!pm || !pe) {
            std::printf("  %s: masked %d, exit %d\n", p.name, pm, pe);
            continue;
        }
        const int sm = Slots(Disasm(masked)), se = Slots(Disasm(exit));
        Check(sm > 0 && se > 0 && se <= 512, "snowy passes: both copies disassemble within 512 slots");
        Check(gpu.AcceptsPs(masked) && gpu.AcceptsPs(exit), "snowy passes: native D3D9 accepts both copies");
        IDirect3DPixelShader9 *psM = nullptr, *psE = nullptr;
        gpu.dev->CreatePixelShader(masked.data(), &psM);
        gpu.dev->CreatePixelShader(exit.data(), &psE);
        if (!psM || !psE || !vs) {
            Release(psM);
            Release(psE);
            continue;
        }
        gpu.dev->SetTexture(0, cube);
        gpu.dev->SetTexture(1, cube);
        for (DWORD s = 2; s < 16; s++) {
            gpu.dev->SetTexture(s, tex[s]);
            gpu.dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            gpu.dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            gpu.dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        }
        float k[16][4];
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 4; j++) k[i][j] = 0.05f + 0.07f * static_cast<float>((i * 5 + j * 3) % 11);
        gpu.dev->SetPixelShaderConstantF(0, &k[0][0], 16);
        const float rows[8] = {0.9f, 0.1f, 0.05f, 0, 0.1f, 0.8f, 0.1f, 0};
        gpu.dev->SetPixelShaderConstantF(ShaderPatches::kLotMaskConst, rows, 2);
        const float off[4] = {0, 0, 0, 0}, on[4] = {1, 0, 0, 0};
        gpu.dev->SetPixelShaderConstantF(ShaderPatches::kLotMaskConst + 2, off, 1);
        const auto ref = gpu.Render(psM, vs), gate0 = gpu.Render(psE, vs);
        gpu.dev->SetPixelShaderConstantF(ShaderPatches::kLotMaskConst + 2, on, 1);
        const auto gate1 = gpu.Render(psE, vs);
        gpu.dev->SetTexture(2, lotNoAlpha);
        const auto ref0 = gpu.Render(psM, vs), alpha0 = gpu.Render(psE, vs);
        gpu.dev->SetPixelShaderConstantF(ShaderPatches::kLotMaskConst + 2, off, 1);
        int lit = 0, changed = 0;
        for (size_t i = 0; i < ref.size(); i++) {
            lit += (ref[i] & 0xFFFFFF) != 0;
            changed += gate1[i] != ref[i];
        }
        Check(gate0 == ref, "snowy passes: the exit copy with the gate 0 draws the masked copy's pixels exactly");
        Check(alpha0 == ref0, "snowy passes: the exit copy over a lot map of alpha 0 draws the masked copy's pixels exactly");
        Check(changed > 0 || lit == 0, "snowy passes: the exit copy with the gate 1 adds the lot map's alpha share");
        auto edgeM = masked, edgeE = exit, terrainOnly = masked;
        auto unpatched = blob->code;
        Check(!ShaderPatches::PatchLotTerrainEdges(unpatched) && unpatched == blob->code,
              "winter borders: an unrecognised light accumulator stays unchanged");
        auto missingExit = masked;
        Check(!ShaderPatches::PatchLotTerrainEdges(missingExit, true) && missingExit == masked,
              "winter borders: the indoor copy requires its complete original gate");
        for (size_t cut = 0; cut < masked.size(); ++cut) {
            auto truncated = std::vector<DWORD>(masked.begin(), masked.begin() + cut);
            const auto saved = truncated;
            Check(!ShaderPatches::PatchLotTerrainEdges(truncated) && truncated == saved,
                  "winter borders: every truncation is refused without changes");
        }
        Check(ShaderPatches::PatchLotTerrainEdges(edgeM) && ShaderPatches::PatchLotTerrainEdges(edgeE, true),
              "winter borders: both native pass copies take the transition");
        bool forced = false;
        for (size_t at = 1; at + 3 < terrainOnly.size();) {
            const DWORD op = terrainOnly[at] & 0xFFFF;
            const size_t len = (terrainOnly[at] >> 24) & 15;
            if (op == 0xFFFE) { at += 1 + ((terrainOnly[at] >> 16) & 0x7FFF); continue; }
            if (op == 0x0B && len == 3 && (terrainOnly[at + 1] & 0x7FF) == 0 &&
                (terrainOnly[at + 2] & 0x7FF) == 0 && (terrainOnly[at + 3] & 0x7FF) == 7) {
                terrainOnly[at] = 0x02000001;
                terrainOnly[at + 2] = terrainOnly[at + 3];
                terrainOnly.erase(terrainOnly.begin() + at + 3);
                forced = true;
                break;
            }
            if (!len) break;
            at += 1 + len;
        }
        Check(forced, "winter borders: reference uses only the terrain lamp term");
        IDirect3DPixelShader9 *psEM = nullptr, *psEE = nullptr, *psTerrain = nullptr;
        Check(SUCCEEDED(gpu.dev->CreatePixelShader(edgeM.data(), &psEM)) &&
              SUCCEEDED(gpu.dev->CreatePixelShader(edgeE.data(), &psEE)) &&
              SUCCEEDED(gpu.dev->CreatePixelShader(terrainOnly.data(), &psTerrain)),
              "winter borders: native D3D9 accepts every patched shader");
        if (psEM && psEE && psTerrain) {
            gpu.dev->SetTexture(2, tex[2]);
            const float disabled[12] = {0,0,0,0, 0,0,0,0, 0,1,3,2};
            gpu.dev->SetPixelShaderConstantF(ShaderPatches::kLotEdgeConst, disabled, 3);
            Check(gpu.Render(psEM, vs) == ref, "winter borders: unknown lot keeps the previous masked pixels");
            gpu.dev->SetPixelShaderConstantF(ShaderPatches::kLotMaskConst + 2, on, 1);
            Check(gpu.Render(psEE, vs) == gate1, "winter borders: unknown lot keeps indoor light unchanged");
            const float interior[12] = {0,0,4,20, 0,0,4,20, 1.0f/3,0,3,2};
            gpu.dev->SetPixelShaderConstantF(ShaderPatches::kLotEdgeConst, interior, 3);
            const auto inside = gpu.Render(psEE, vs);
            Check(inside == gate1, "winter borders: the lot interior keeps its previous light exactly");
            const float border[12] = {0,0,0,20, 0,0,4,20, 1.0f/3,0,3,2};
            gpu.dev->SetPixelShaderConstantF(ShaderPatches::kLotEdgeConst, border, 3);
            const auto target = gpu.Render(psTerrain, vs), borderM = gpu.Render(psEM, vs), borderE = gpu.Render(psEE, vs);
            Check(borderM == target && borderE == target, "winter borders: both copies meet terrain light exactly at the boundary");
            const float gradient[12] = {4.0f/0.9f,0,-0.2f/0.9f,20, 0,0,4,20, 1.0f/3,0,3,2};
            gpu.dev->SetPixelShaderConstantF(ShaderPatches::kLotEdgeConst, gradient, 3);
            const auto smooth = gpu.Render(psEE, vs);
            int worst = 0;
            for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
                float w = std::clamp((4.0f * x / 64) / 3, 0.0f, 1.0f);
                w = w * w * (3 - 2 * w);
                for (int channel : {0,8,16}) {
                    const size_t pixel = static_cast<size_t>(y * 64 + x);
                    const float a = static_cast<float>((target[pixel] >> channel) & 255), b = static_cast<float>((inside[pixel] >> channel) & 255);
                    worst = std::max(worst, std::abs(static_cast<int>((smooth[pixel] >> channel) & 255) - static_cast<int>(std::lround(a + (b - a) * w))));
                }
            }
            Check(worst <= 2, "winter borders: pixels follow the smooth transition within two LSB");
            std::printf("Winter border %s: worst gradient error %d LSB\n", p.name, worst);
        }
        Release(psEM); Release(psEE); Release(psTerrain);
        std::printf("Snowy pass %s: masked %d slots, exit %d slots; %d of %d pixels lit, %d changed by the gate\n", p.name, sm, se, lit, static_cast<int>(ref.size()), changed);
        for (DWORD s = 0; s < 16; s++) gpu.dev->SetTexture(s, nullptr);
        gpu.dev->SetPixelShader(nullptr);
        gpu.dev->SetVertexShader(nullptr);
        Release(psM);
        Release(psE);
    }
    for (auto*& t : tex) Release(t);
    Release(lotNoAlpha);
    Release(cube);
    Release(vs);
    std::printf("Snowy lot passes: %d checks\n", g_checks - before);
}

} // namespace

void TerrainSamplerGaps(Gpu& gpu, const std::vector<Blob>& blobs) {
    const unsigned hashes[] = {0x30F50F2E, 0x473EF8DA, 0x751D1971, 0xC4AC1DD6};
    int found = 0;
    for (const Blob& b : blobs) {
        if (b.vs || std::find(std::begin(hashes), std::end(hashes), Hash(b.code)) == std::end(hashes)) continue;
        found++;
        auto code = b.code;
        DWORD spare = 999;
        Check(ShaderPatches::LightMapScaleConst(code, 8) >= 0, "terrain gap: native lamp scale identified");
        auto full = b.code;
        full.insert(full.begin() + 1, {0x0200001Fu, 0x90000000u, 0xA00F0805u});
        const auto saved = full;
        DWORD unchanged = 999;
        Check(!ShaderPatches::PatchTerrainNativeAlpha(full, 8, unchanged) && full == saved && unchanged == 999,
              "terrain gap: all occupied slots leave the shader and output untouched");
        const bool ok = ShaderPatches::PatchTerrainNativeAlpha(code, 8, spare);
        Check(ok && spare == 5, "terrain gap: unused s5 holds native alpha despite declared s15");
        if (ok) {
            Check(gpu.AcceptsPs(code), "terrain gap: native D3D9 accepts the patched variant");
            const auto text = Disasm(code);
            Check(text.find("dcl_2d s15") != std::string::npos, "terrain gap: original upper sampler preserved");
        }
    }
    Check(found == 4, "terrain gap: all four native variants tested");
}
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::puts("usage: check <Shaders_Win32.precomp> [F7 capture folder ...]");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    const std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), {});
    std::vector<Blob> blobs;
    std::vector<Tech> techs;
    Check(LoadPrecomp(d, blobs, techs), "precomp read");
    std::printf("%zu shaders, %zu techniques in %s\n", blobs.size(), techs.size(), argv[1]);
    Gpu gpu;
    TerrainSamplerGaps(gpu, blobs);
    SummerFloors(gpu, blobs, techs);
    WinterFloors(gpu, blobs, techs);
    Rows();
    Pixels(gpu);
    WeatherFloorPixels(gpu);
    SnowCoverFogPixels(gpu);
    ExitPixels(gpu);
    Check(Instanced(gpu, InstancedPs(blobs, techs), "precomp") > 0, "instanced: the precomp has instanced shaders that take the shade");
    FenceCutoutChecks(InstancedPs(blobs, techs));
    InstancedPixels(gpu);
    SnowLotPasses(gpu, blobs);
    for (int i = 2; i < argc; i++) {
        Captured(blobs, argv[i]);
        InstancedCaptured(gpu, blobs, techs, argv[i]);
    }
    std::printf("%d checks, %d failures\n%s\n", g_checks, g_failures, g_failures ? "PROBLEMS" : "OK");
    return g_failures ? 1 : 0;
}
