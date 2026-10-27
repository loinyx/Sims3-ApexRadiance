// Offline check of Smooth light maps' shader side (ShaderPatches IsRoomMapPs / PatchRoomMapSmooth / RoomMapUvScale) and of
// the dither offset (RoomMapDither::Offset). Read-only: prints to stdout, writes no file.
//   check.exe <Shaders_Win32.precomp> [capture folder ...]
// 1. Every precomp pixel shader: the pattern matches only shaders of the room-lit techniques (InteriorFloor, InteriorWall
//    and their siblings that read the same room map), every InteriorWall shader and every InteriorFloor shader with a room
//    map, and nothing of ExteriorWall or any other technique.
// 2. Each match patched: D3DDisassemble accepts it; a native D3D9 device creates it when its PS20Caps allow it (temps,
//    slots, arbitrary swizzle; else the game's shader stays, as at run time); the copy differs from the game's only by the
//    read (one texld fewer, the B-spline block, the defs, ps_2_x for ps_2_0).
// 3. The vertex shaders paired with them: RoomMapUvScale finds their room map uv scale (1/4096 walls, 1/32767 floors, or a
//    constant read at the draw).
// 4. Native D3D9 pixels: a synthetic shader of the pattern, patched, over a texture of two 7-row bands (a wall atlas at
//    n = 2): every pixel equals the CPU B-spline with the rows clamped to the pixel's band within 3 LSB, and as one block
//    (floors) the B-spline with the rows clamped to the texture.
// 5. Optional capture folders (searched recursively): every distinct pixel shader .bin goes through 1-2.
// 6. The dither offset: within [-0.5, 0.5], mean about 0 over a map, the same at the same texel.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include "../../features/shader_patches.h"
#include "../../features/room_map_dither.h"
#include "../../features/arch_ambient.h"
#include <algorithm>
#include <cctype>
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
        if (g_failures < 60) std::printf("  FAIL %s\n", what);
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

// ---- Shaders_Win32.precomp (docs/engine/shaders.md) ----
struct Blob {
    bool vs = false;
    std::vector<DWORD> code;
};
struct Tech {
    uint32_t name = 0;
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
        std::memcpy(&t.name, &d[at[k] + 4], 4);
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

std::string Disasm(const std::vector<DWORD>& t) {
    ID3DBlob* b = nullptr;
    if (t.empty() || FAILED(D3DDisassemble(t.data(), t.size() * 4, 0, nullptr, &b)) || !b) return {};
    std::string s(static_cast<const char*>(b->GetBufferPointer()), b->GetBufferSize());
    b->Release();
    return s;
}
int CountOf(const std::string& text, const char* what) {
    int n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) n++;
    return n;
}

// ---- hidden D3D9 device (64 x 64 target) ----
struct Gpu {
    HWND window = nullptr;
    IDirect3D9* api = nullptr;
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9 *target = nullptr, *readback = nullptr;
    D3DCAPS9 caps{};
    Gpu() {
        window = CreateWindowExW(0, L"STATIC", L"Apex room map check", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
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
        dev->GetDeviceCaps(&caps);
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
    // what Apex checks before it draws with a copy (lot_light_bridge RoomMapCapsOk)
    bool CapsAllow(const ShaderPatches::RoomMapPatch& p) const {
        if (!p.ps2x) return caps.PixelShaderVersion >= D3DPS_VERSION(3, 0);
        return caps.PixelShaderVersion >= D3DPS_VERSION(2, 0) && (caps.PS20Caps.Caps & D3DPS20CAPS_ARBITRARYSWIZZLE) && caps.PS20Caps.NumTemps >= p.temps &&
               caps.PS20Caps.NumInstructionSlots >= p.slots;
    }
    bool AcceptsPs(const std::vector<DWORD>& t) {
        IDirect3DPixelShader9* s = nullptr;
        const bool ok = dev && SUCCEEDED(dev->CreatePixelShader(t.data(), &s));
        Release(s);
        return ok;
    }
    // a full-screen quad, TEXCOORD1 = (0, 0) top left .. (1, 1) bottom right (pixel x reads uv (x + 0.5) / 64)
    std::vector<DWORD> Render(IDirect3DPixelShader9* ps) {
        struct Vertex {
            float x, y, z, rhw, u0, v0, u1, v1;
        };
        const Vertex q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0, 0, 0}, {63.5f, -0.5f, 0, 1, 1, 0, 1, 0}, {-0.5f, 63.5f, 0, 1, 0, 1, 0, 1}, {63.5f, 63.5f, 0, 1, 1, 1, 1, 1}};
        dev->SetVertexShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX2);
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

struct PatchStats {
    int matched = 0, patched = 0, disasmOk = 0, created = 0, capsRefused = 0, texldOk = 0, combined = 0, combinedCreated = 0, arch = 0, archCreated = 0, walls = 0, wallsCreated = 0;
};
// one match: patch it and check the copy
void CheckPatch(const std::vector<DWORD>& code, Gpu& gpu, PatchStats& st, const char* where) {
    std::vector<DWORD> t = code;
    ShaderPatches::RoomMapPatch p;
    if (!ShaderPatches::PatchRoomMapSmooth(t, p)) {
        std::printf("  %s: matched but not patched (registers)\n", where);
        Check(false, "a matched room-lit shader is patched");
        return;
    }
    st.patched++;
    const std::string before = Disasm(code), after = Disasm(t);
    const bool dis = !after.empty();
    st.disasmOk += dis;
    Check(dis, "the copy disassembles");
    // the read became 4 taps and the bound the own texel and the 4 x 4 (17 texel reads): 20 texld more
    const bool taps = CountOf(after, "texld") == CountOf(before, "texld") + 20;
    st.texldOk += taps;
    Check(taps, "the copy has the game's texture reads less one plus four taps and four bound reads");
    Check(t[0] == (code[0] == 0xFFFF0200 ? 0xFFFF0201u : code[0]), "ps_2_0 becomes ps_2_x, ps_3_0 stays");
    Check(p.sizeConst + 3 == p.bandConst, "size and band constants");
    // a shader that also reads the basis maps: both smooth reads in one copy (lot_light_bridge DrawBasisSmooth)
    int basis[4];
    if (ShaderPatches::BasisSamplers(code, basis)) {
        std::vector<DWORD> both = code;
        ShaderPatches::BasisSmoothPatch bp;
        ShaderPatches::RoomMapPatch rp;
        const bool ok = ShaderPatches::PatchBasisSmooth(both, bp) && ShaderPatches::PatchRoomMapSmooth(both, rp);
        st.combined++;
        Check(ok && !Disasm(both).empty(), "basis and room map reads both patched in one copy, which disassembles");
        if (ok && gpu.dev && gpu.CapsAllow(rp)) {
            const bool made = gpu.AcceptsPs(both);
            st.combinedCreated += made;
            Check(made, "native D3D9 creates the combined copy");
        }
    }
    // (arch_ambient) the copy with the neighbours' ambient near arches, alone and with the basis maps: native too (ps_3_0)
    if (code[0] == 0xFFFF0300 && gpu.dev) {
        std::vector<DWORD> a = code;
        ShaderPatches::RoomMapPatch ap;
        if (ShaderPatches::PatchRoomMapSmooth(a, ap, true) && ap.archConst >= 0) {
            st.arch++;
            const bool made = gpu.AcceptsPs(a);
            st.archCreated += made;
            Check(made, "native D3D9 creates the copy with the arch ambient");
        }
        int bs[4];
        if (ShaderPatches::BasisSamplers(code, bs)) {
            std::vector<DWORD> both = code;
            ShaderPatches::BasisSmoothPatch bp;
            ShaderPatches::RoomMapPatch rp;
            if (ShaderPatches::PatchBasisSmooth(both, bp, {.preserveShadows = true}) && ShaderPatches::PatchRoomMapSmooth(both, rp, true) && rp.archConst >= 0) {
                st.arch++;
                const bool made = gpu.AcceptsPs(both);
                st.archCreated += made;
                Check(made, "native D3D9 creates the combined copy with the arch ambient");
            }
        }
    }
    // (basis_wall_filter) the floor copy that leaves out the tiles beyond a wall: one texld more (the own tile's walls)
    {
        std::vector<DWORD> w = code;
        ShaderPatches::RoomMapPatch wp;
        if (ShaderPatches::PatchRoomMapSmooth(w, wp, false, true)) {
            st.walls++;
            const std::string wd = Disasm(w);
            Check(!wd.empty() && CountOf(wd, "texld") == CountOf(after, "texld") + 1, "the walls copy disassembles, with one texld more than the copy");
            Check(wp.maskSampler >= 0 && wp.maskConst >= 0 && wp.sizeConst == p.sizeConst && wp.bandConst == p.bandConst, "the walls copy's sampler and constants");
            if (gpu.dev && gpu.CapsAllow(wp)) {
                const bool made = gpu.AcceptsPs(w);
                st.wallsCreated += made;
                Check(made, "native D3D9 creates the walls copy");
            }
        }
    }
    if (gpu.dev) {
        if (!gpu.CapsAllow(p)) {
            st.capsRefused++;
            return;
        }
        const bool ok = gpu.AcceptsPs(t);
        st.created += ok;
        if (!ok) std::printf("  %s: refused by D3D9 (%d temps, %d slots)\n%s\n", where, p.temps, p.slots, after.c_str());
        Check(ok, "native D3D9 creates the copy");
    }
}

// ---- CPU reference: the cubic B-spline of an A8R8G8B8 map at texel coordinate (cx, cy) (texel k's centre at k + 0.5),
// columns clamped to the map, rows to [rowLo, rowHi] ----
float Bw(int i, float f) {
    const float a = 1.0f - f;
    switch (i) {
    case 0: return a * a * a / 6.0f;
    case 1: return (3.0f * f * f * f - 6.0f * f * f + 4.0f) / 6.0f;
    case 2: return (-3.0f * f * f * f + 3.0f * f * f + 3.0f * f + 1.0f) / 6.0f;
    default: return f * f * f / 6.0f;
    }
}
void Reference(const std::vector<DWORD>& map, int W, int H, float cx, float cy, int rowLo, int rowHi, float out[4], bool bound = true, bool sides = true) {
    (void)H;
    const float px = cx - 0.5f, py = cy - 0.5f;
    const int ix = static_cast<int>(std::floor(px)), iy = static_cast<int>(std::floor(py));
    const float fx = px - static_cast<float>(ix), fy = py - static_cast<float>(iy);
    for (int c = 0; c < 4; c++) out[c] = 0.0f;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            const int x = std::clamp(ix - 1 + i, 0, W - 1), y = std::clamp(iy - 1 + j, rowLo, rowHi);
            const DWORD v = map[static_cast<size_t>(y) * W + x];
            const float w = Bw(i, fx) * Bw(j, fy);
            out[0] += w * static_cast<float>((v >> 16) & 0xFF);
            out[1] += w * static_cast<float>((v >> 8) & 0xFF);
            out[2] += w * static_cast<float>(v & 0xFF);
            out[3] += w * static_cast<float>(v >> 24);
        }
    // (PatchRoomMapSmooth) with the pixel's own texel at alpha 0.125 or more, the B-spline of the 4 x 4 texels with alpha
    // 4/255 or more alone (room 0's beyond a wall left out), its weights over their sum, all four channels; else the
    // B-spline. sides false: the 2 x 2 bound of 866cfb0, every texel counted (the comparison)
    const int ox = fx >= 0.5f ? 1 : 0, oy = fy >= 0.5f ? 1 : 0;
    const float ownA = static_cast<float>(map[static_cast<size_t>(std::clamp(iy + oy, rowLo, rowHi)) * W + std::clamp(ix + ox, 0, W - 1)] >> 24) / 255.0f;
    if (!bound || (sides && ownA < 0.125f)) return;
    if (sides) {
        float sum[4] = {}, ws = 0.0f;
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++) {
                const DWORD v = map[static_cast<size_t>(std::clamp(iy - 1 + j, rowLo, rowHi)) * W + std::clamp(ix - 1 + i, 0, W - 1)];
                if (static_cast<float>(v >> 24) / 255.0f < 4.0f / 255.0f) continue;
                const float w = Bw(i, fx) * Bw(j, fy);
                sum[0] += w * static_cast<float>((v >> 16) & 0xFF), sum[1] += w * static_cast<float>((v >> 8) & 0xFF);
                sum[2] += w * static_cast<float>(v & 0xFF), sum[3] += w * static_cast<float>(v >> 24), ws += w;
            }
        for (int c = 0; c < 4; c++) out[c] = sum[c] / ws;
        return;
    }
    float hi[3] = {};
    const int lo = sides ? -1 : 0, n = sides ? 3 : 2;
    for (int j = lo; j < n; j++)
        for (int i = lo; i < n; i++) {
            const DWORD v = map[static_cast<size_t>(std::clamp(iy + j, rowLo, rowHi)) * W + std::clamp(ix + i, 0, W - 1)];
            if (sides && static_cast<float>(v >> 24) / 255.0f < 4.0f / 255.0f) continue;
            hi[0] = std::max(hi[0], static_cast<float>((v >> 16) & 0xFF));
            hi[1] = std::max(hi[1], static_cast<float>((v >> 8) & 0xFF));
            hi[2] = std::max(hi[2], static_cast<float>(v & 0xFF));
        }
    for (int c = 0; c < 3; c++) out[c] = std::min(out[c], hi[c] + 2.0f);
}

// The bound of the B-spline (the brightest of the bilinear read's 2 x 2 texels + 2 levels), CPU: a hall's floor texels at 8
// beside a lit courtyard's at 255 across a wall with no texel row on it (F7 of a hall beside a glass door: a warm band along the
// wall) take nothing of the courtyard beyond the bilinear read's own half texel; a lamp's falloff (4 texels a metre, the
// lamp 1 to 2.5 m over the floor) is not changed, the softest shadow edge (1.5 texels) by at most 6 levels on its dark side
void BoundChecks() {
    constexpr int W = 16, H = 16, K = 8; // rows < K hall, >= K courtyard
    std::vector<DWORD> strip(W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) strip[static_cast<size_t>(y) * W + x] = y < K ? 0xFF080808u : 0x008C3C14u | (255u << 16);
    float worstBefore = 0, worstAfter = 0;
    for (float cy = 4.0f; cy <= K - 0.5f; cy += 0.05f) { // up to half a texel from the wall line
        float a[4], b[4];
        Reference(strip, W, H, 8.3f, cy, 0, H - 1, a, false);
        Reference(strip, W, H, 8.3f, cy, 0, H - 1, b, true);
        worstBefore = std::max(worstBefore, a[0] - 8.0f), worstAfter = std::max(worstAfter, b[0] - 8.0f);
    }
    std::printf("  hall beside a lit courtyard, beyond the bilinear read's half texel: up to +%.1f levels of red before the bound, +%.1f with it\n", worstBefore, worstAfter);
    Check(worstBefore > 30.0f && worstAfter <= 2.0f + 1e-3f, "the hall takes no band of the courtyard's light beyond the bilinear read");
    float lampWorst = 0;
    for (float h : {1.0f, 1.5f, 2.5f})
        for (float k : {0.3f, 1.0f, 4.0f}) {
            std::vector<DWORD> m(48 * 48);
            for (int y = 0; y < 48; y++)
                for (int x = 0; x < 48; x++) {
                    const float dx = (x + 0.5f) / 4.0f - 6.0f, dz = (y + 0.5f) / 4.0f - 6.0f;
                    const float v = std::min(255.0f, 255.0f * k * h / std::pow(h * h + dx * dx + dz * dz, 1.5f));
                    const DWORD c = static_cast<DWORD>(std::lround(v));
                    m[static_cast<size_t>(y) * 48 + x] = 0xFF000000u | c << 16 | c << 8 | c;
                }
            for (float cy = 4.0f; cy < 44.0f; cy += 0.37f)
                for (float cx = 4.0f; cx < 44.0f; cx += 0.37f) {
                    float a[4], b[4];
                    Reference(m, 48, 48, cx, cy, 0, 47, a, false);
                    Reference(m, 48, 48, cx, cy, 0, 47, b, true);
                    lampWorst = std::max(lampWorst, a[0] - b[0]);
                }
        }
    std::printf("  a lamp's falloff: largest change from the bound %.3f levels\n", lampWorst);
    Check(lampWorst < 0.01f, "a lamp's falloff inside a room is not changed by the bound");
    std::vector<DWORD> soft(W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const DWORD c = static_cast<DWORD>(std::lround(150.0f / (1.0f + std::exp(-((x + 0.5f) / 4.0f - 2.0f) / 0.09f))));
            soft[static_cast<size_t>(y) * W + x] = 0xFF000000u | c << 16 | c << 8 | c;
        }
    float softWorst = 0;
    for (float cx = 2.0f; cx < 14.0f; cx += 0.05f) {
        float a[4], b[4];
        Reference(soft, W, H, cx, 8.0f, 0, H - 1, a, false);
        Reference(soft, W, H, cx, 8.0f, 0, H - 1, b, true);
        softWorst = std::max(softWorst, a[0] - b[0]);
    }
    std::printf("  a soft shadow edge (0.375 m): largest change %.2f levels\n", softWorst);
    Check(softWorst <= 6.0f, "the softest shadow edge (1.5 texels) keeps its B-spline within 6 levels on its dark side");
}

// The pixel's side of a wall (PatchRoomMapSmooth): a hall's texels (ambient weight 0.75, green 3) beside a courtyard's lit
// green at 255 (alpha 0, room 0) with no texel row on the wall line (F7 of a hall beside a glass door: a band, then a line
// of the courtyard's green along the wall). The hall within 2 texels of the wall takes none of it; the courtyard's pixels,
// light sent out of the hall (alpha 10/255) and a bright hall texel (alpha under 0.125) keep the B-spline. A jamb's
// penumbra inside the hall (0 to 120 over a texel): the 2 x 2 bound cut the B-spline's tail on its dark side at the texel
// lines (steps a texel wide beside a glass door, in game); the 4 x 4 of the room's own texels never does.
std::vector<DWORD> SideMap(int W, int H, int K, DWORD hallA, DWORD yardA) {
    std::vector<DWORD> m(static_cast<size_t>(W) * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) m[static_cast<size_t>(y) * W + x] = y < K ? (hallA << 24) | (2u << 16) | (3u << 8) | 2u : (yardA << 24) | (40u << 16) | (255u << 8) | 20u;
    return m;
}
void SideChecks() {
    constexpr int W = 16, H = 16, K = 8;
    const std::vector<DWORD> hall = SideMap(W, H, K, 191, 0), sent = SideMap(W, H, K, 191, 10), bright = SideMap(W, H, K, 25, 0);
    float plain = 0, square = 0, now = 0, keepMoved = 0;
    for (float cy = K - 2.0f; cy < K; cy += 0.01f) { // the hall within 2 texels of the wall
        float a[4], b[4], c[4];
        Reference(hall, W, H, 7.3f, cy, 0, H - 1, a, false);
        Reference(hall, W, H, 7.3f, cy, 0, H - 1, b, true, false);
        Reference(hall, W, H, 7.3f, cy, 0, H - 1, c, true, true);
        plain = std::max(plain, a[1] - 3.0f), square = std::max(square, b[1] - 3.0f), now = std::max(now, c[1] - 3.0f);
        for (const std::vector<DWORD>* m : {&sent, &bright}) {
            Reference(*m, W, H, 7.3f, cy, 0, H - 1, a, false);
            Reference(*m, W, H, 7.3f, cy, 0, H - 1, c, true, true);
            for (int k = 0; k < 4; k++) keepMoved = std::max(keepMoved, std::fabs(a[k] - c[k]));
        }
    }
    for (float cy = K; cy < H - 1; cy += 0.05f) { // the courtyard's pixels
        float a[4], c[4];
        Reference(hall, W, H, 7.3f, cy, 0, H - 1, a, false);
        Reference(hall, W, H, 7.3f, cy, 0, H - 1, c, true, true);
        for (int k = 0; k < 4; k++) keepMoved = std::max(keepMoved, std::fabs(a[k] - c[k]));
    }
    std::printf("  a hall within 2 texels of a lit courtyard's wall: up to +%.1f levels of green with the plain B-spline, +%.1f with the 2 x 2 bound, +%.1f now\n",
                plain, square, now);
    Check(plain > 30.0f && square > 30.0f && now <= 2.0f + 1e-3f, "a hall's floor takes no band or line of the courtyard's light along the wall");
    Check(keepMoved < 1e-3f, "the courtyard's own pixels, light sent out of the hall and a bright hall texel keep the B-spline");
    // a jamb's penumbra inside the hall, along x (0 to 120 over about a texel), every texel the hall's
    std::vector<DWORD> jamb(W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const DWORD v = static_cast<DWORD>(std::lround(120.0f / (1.0f + std::exp(-((x + 0.5f) - 8.0f) / 0.35f))));
            jamb[static_cast<size_t>(y) * W + x] = (191u << 24) | (v << 8);
        }
    float squareCut = 0, nowCut = 0;
    for (float cx = 3.0f; cx < 13.0f; cx += 0.01f) {
        float a[4], b[4], c[4];
        Reference(jamb, W, H, cx, 6.3f, 0, H - 1, a, false);
        Reference(jamb, W, H, cx, 6.3f, 0, H - 1, b, true, false);
        Reference(jamb, W, H, cx, 6.3f, 0, H - 1, c, true, true);
        squareCut = std::max(squareCut, a[1] - b[1]), nowCut = std::max(nowCut, a[1] - c[1]);
    }
    std::printf("  a jamb's penumbra in the hall: the B-spline cut by up to %.1f levels at texel lines with the 2 x 2 bound, %.1f now\n", squareCut, nowCut);
    Check(squareCut > 3.0f && nowCut < 1e-3f, "a jamb's penumbra keeps the B-spline's smooth read (no steps at the texel lines)");
    // a room's floor under a window (F7: courtyard 255 | wall | 0, 20, 47, 45, 41 of green, alpha 0.84 to 0.69): the wall below
    // the sill shades the first texels. Held under the brightest texel of the room's side, the B-spline lifted the first with
    // the courtyard's light and left the second dark (a black line between two lit bands); the room's side alone rises from
    // the wall and keeps the room's ambient weight to the wall
    const int prof[6] = {0, 20, 47, 45, 41, 37};
    const DWORD profA[6] = {214, 196, 173, 176, 179, 181};
    std::vector<DWORD> win(W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            win[static_cast<size_t>(y) * W + x] = x < K ? (255u << 8) : (profA[std::min(x - K, 5)] << 24) | (static_cast<DWORD>(prof[std::min(x - K, 5)]) << 8);
    float oldDip = 0, nowDip = 0, oldLift = 0, nowLift = 0, prevOld = -1, prevNow = -1, peakOld = 0, peakNow = 0;
    for (float cx = K + 0.01f; cx < K + 2.5f; cx += 0.01f) {
        float b[4], c[4];
        Reference(win, W, H, cx, 6.3f, 0, H - 1, c, true, true);
        // the 4 x 4 bound of eedcb23 (the comparison)
        Reference(win, W, H, cx, 6.3f, 0, H - 1, b, false);
        float hi = 0;
        const int ix = static_cast<int>(std::floor(cx - 0.5f));
        for (int i = -1; i < 3; i++) {
            const DWORD v = win[static_cast<size_t>(6) * W + std::clamp(ix + i, 0, W - 1)];
            if (v >> 24 >= 4) hi = std::max(hi, static_cast<float>((v >> 8) & 0xFF));
        }
        b[1] = std::min(b[1], hi + 2.0f);
        if (prevOld >= 0) peakOld = std::max(peakOld, prevOld), oldDip = std::max(oldDip, peakOld - b[1]);
        if (prevNow >= 0) peakNow = std::max(peakNow, prevNow), nowDip = std::max(nowDip, peakNow - c[1]);
        if (cx < K + 0.5f) oldLift = std::max(oldLift, b[1]), nowLift = std::max(nowLift, c[1]);
        prevOld = b[1], prevNow = c[1];
        if (cx > K + 2.0f) break;
    }
    std::printf("  a room's floor under a window: the 4 x 4 bound lit the wall's half texel to %.0f and fell %.1f levels after it; now %.0f, falling %.1f\n", oldLift, oldDip,
                nowLift, nowDip);
    Check(oldDip > 10.0f && nowDip <= 0.5f && nowLift < 10.0f, "a room's floor under a window rises from the wall with no dark line");
}
void SidePixelCheck(Gpu& gpu) {
    if (!gpu.dev) return;
    const char* src = "sampler2D s0 : register(s0); float4 amb : register(c0);\n"
                      "float4 main(float2 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1) : COLOR0 { float4 m = tex2D(s0, uv1); m.rgb = m.a * amb.rgb + m.rgb; return m; }";
    for (const char* profile : {"ps_2_0"}) { // (the ps_3_0 copies of the game's shaders are created above)
        std::vector<DWORD> t = Compile(src, profile);
        ShaderPatches::RoomMapPatch p;
        if (t.empty() || !ShaderPatches::PatchRoomMapSmooth(t, p)) {
            Check(false, "the synthetic shader is patched (sides)");
            continue;
        }
        if (!gpu.CapsAllow(p)) continue;
        IDirect3DPixelShader9* ps = nullptr;
        Check(SUCCEEDED(gpu.dev->CreatePixelShader(t.data(), &ps)), "the synthetic copy is created (sides)");
        if (!ps) continue;
        constexpr int W = 16, H = 16, K = 8;
        const std::vector<DWORD> map = SideMap(W, H, K, 191, 0);
        IDirect3DTexture9* tex = nullptr;
        gpu.dev->CreateTexture(W, H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
        if (!tex) {
            Release(ps);
            continue;
        }
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
            for (int y = 0; y < H; y++) std::memcpy(static_cast<char*>(lr.pBits) + y * lr.Pitch, &map[static_cast<size_t>(y) * W], W * 4);
            tex->UnlockRect(0);
        }
        gpu.dev->SetTexture(p.sampler, tex);
        gpu.dev->SetSamplerState(p.sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        gpu.dev->SetSamplerState(p.sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        gpu.dev->SetSamplerState(p.sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        gpu.dev->SetSamplerState(p.sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        gpu.dev->SetSamplerState(p.sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        const float amb[4] = {0, 0, 0, 0}, size[4] = {static_cast<float>(W), static_cast<float>(H), 1.0f / W, 1.0f / H}, bc[4] = {1.0f / 65536.0f, 65536.0f, 0.5f, 65535.0f};
        gpu.dev->SetPixelShaderConstantF(0, amb, 1);
        gpu.dev->SetPixelShaderConstantF(p.sizeConst, size, 1);
        gpu.dev->SetPixelShaderConstantF(p.bandConst, bc, 1);
        const std::vector<DWORD> px = gpu.Render(ps);
        int worst = 0, line = 0;
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                const float cx = (x + 0.5f) / 64.0f * W, cy = (y + 0.5f) / 64.0f * H;
                float ref[4];
                Reference(map, W, H, cx, cy, 0, H - 1, ref);
                const DWORD v = px[static_cast<size_t>(y) * 64 + x];
                const int got[4] = {static_cast<int>((v >> 16) & 0xFF), static_cast<int>((v >> 8) & 0xFF), static_cast<int>(v & 0xFF), static_cast<int>(v >> 24)};
                for (int c = 0; c < 4; c++) worst = std::max(worst, static_cast<int>(std::lround(std::fabs(got[c] - ref[c]))));
                if (cy < K && cy > K - 0.5f) line = std::max(line, got[1] - 3);
            }
        std::printf("  pixel check, the pixel's side (%s): largest difference from the CPU reference %d LSB; the hall's last half texel at most +%d green\n", profile, worst, line);
        Check(worst <= 3, "GPU bound with the pixel's side = CPU reference (3 LSB)");
        Check(line <= 3, "GPU: no line of the courtyard's light along the hall's wall");
        gpu.dev->SetTexture(p.sampler, nullptr);
        Release(tex);
        Release(ps);
    }
}

// (basis_wall_filter) Two roofed rooms either side of a wall (F7 19-26-24: a dark hall's floor along the wall of a lit living
// room, a warm band about 0.4 m wide): a 32 x 32 floor map at 4 texels a tile (8 x 8 tiles), the lit room in tiles x 0..3
// (green 200, alpha 5: a bright texel has little ambient weight), the dark one in tiles x 4..7 (green 3, alpha 191), a wall
// between them except at tiles z 3..4 (a doorway). CPU: the room's own texels as before, less the taps of a tile beyond a
// walled side of the pixel's own tile; the GPU copy against it, and the dark room takes nothing through the wall while the
// doorway keeps the read without walls.
struct TileWalls {
    bool xm = false, xp = false, zm = false, zp = false;
};
void RefWalls(const std::vector<DWORD>& map, int W, int H, float cx, float cy, const std::vector<TileWalls>& walls, int tiles, int n, float split, float out[4]) {
    Reference(map, W, H, cx, cy, 0, H - 1, out); // (the read without walls: kept where the pixel's own texel is a bright one)
    const float px = cx - 0.5f, py = cy - 0.5f;
    const int ix = static_cast<int>(std::floor(px)), iy = static_cast<int>(std::floor(py));
    const float fx = px - static_cast<float>(ix), fy = py - static_cast<float>(iy);
    const int ox = ix + (fx >= 0.5f ? 1 : 0), oy = iy + (fy >= 0.5f ? 1 : 0);
    const float ownA = static_cast<float>(map[static_cast<size_t>(std::clamp(oy, 0, H - 1)) * W + std::clamp(ox, 0, W - 1)] >> 24) / 255.0f;
    if (ownA < 0.125f) return;
    const int tx = static_cast<int>(std::floor((ox + 0.5f) / n)), tz = static_cast<int>(std::floor((oy + 0.5f) / n));
    const bool on = ox + 0.5f < split;
    const TileWalls w = on ? walls[static_cast<size_t>(std::clamp(tz, 0, tiles - 1)) * tiles + std::clamp(tx, 0, tiles - 1)] : TileWalls{};
    float sum[4] = {}, ws = 0.0f;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            const int x = ix - 1 + i, y = iy - 1 + j;
            const int dx = static_cast<int>(std::floor((x + 0.5f) / n)) - tx, dz = static_cast<int>(std::floor((y + 0.5f) / n)) - tz;
            if ((dx < 0 && w.xm) || (dx > 0 && w.xp) || (dz < 0 && w.zm) || (dz > 0 && w.zp)) continue;
            const DWORD v = map[static_cast<size_t>(std::clamp(y, 0, H - 1)) * W + std::clamp(x, 0, W - 1)];
            if (static_cast<float>(v >> 24) / 255.0f < 4.0f / 255.0f) continue;
            const float wt = Bw(i, fx) * Bw(j, fy);
            sum[0] += wt * static_cast<float>((v >> 16) & 0xFF), sum[1] += wt * static_cast<float>((v >> 8) & 0xFF);
            sum[2] += wt * static_cast<float>(v & 0xFF), sum[3] += wt * static_cast<float>(v >> 24), ws += wt;
        }
    for (int c = 0; c < 4; c++) out[c] = sum[c] / ws;
}
void WallPixelCheck(Gpu& gpu) {
    if (!gpu.dev) return;
    const char* src = "sampler2D s0 : register(s0); float4 amb : register(c0);\n"
                      "float4 main(float2 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1) : COLOR0 { float4 m = tex2D(s0, uv1); m.rgb = m.a * amb.rgb + m.rgb; return m; }";
    constexpr int W = 32, H = 32, n = 4, tiles = 8;
    std::vector<DWORD> map(W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) map[static_cast<size_t>(y) * W + x] = x < 16 ? (5u << 24) | (200u << 8) | 30u : (191u << 24) | (3u << 8) | 2u;
    std::vector<TileWalls> walls(tiles * tiles);
    for (int z = 0; z < tiles; z++)
        if (z != 3 && z != 4) walls[static_cast<size_t>(z) * tiles + 3].xp = walls[static_cast<size_t>(z) * tiles + 4].xm = true;
    std::vector<DWORD> mask(64 * 64, 0); // A8R8G8B8: r -x, g +x, b -z, a +z
    for (int z = 0; z < tiles; z++)
        for (int x = 0; x < tiles; x++) {
            const TileWalls& t = walls[static_cast<size_t>(z) * tiles + x];
            mask[static_cast<size_t>(z) * 64 + x] = (t.zp ? 0xFF000000u : 0) | (t.xm ? 0x00FF0000u : 0) | (t.xp ? 0x0000FF00u : 0) | (t.zm ? 0x000000FFu : 0);
        }
    for (const char* profile : {"ps_2_0"}) { // (the ps_3_0 walls copies of the game's shaders are created above)
        std::vector<DWORD> t = Compile(src, profile);
        ShaderPatches::RoomMapPatch p;
        if (t.empty() || !ShaderPatches::PatchRoomMapSmooth(t, p, false, true)) {
            Check(false, "the synthetic shader is patched (walls)");
            continue;
        }
        if (!gpu.CapsAllow(p)) continue;
        IDirect3DPixelShader9* ps = nullptr;
        Check(SUCCEEDED(gpu.dev->CreatePixelShader(t.data(), &ps)), "the synthetic walls copy is created");
        IDirect3DTexture9 *tex = nullptr, *mtex = nullptr;
        gpu.dev->CreateTexture(W, H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
        gpu.dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &mtex, nullptr);
        if (!ps || !tex || !mtex) {
            Release(ps), Release(tex), Release(mtex);
            continue;
        }
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
            for (int y = 0; y < H; y++) std::memcpy(static_cast<char*>(lr.pBits) + y * lr.Pitch, &map[static_cast<size_t>(y) * W], W * 4);
            tex->UnlockRect(0);
        }
        if (SUCCEEDED(mtex->LockRect(0, &lr, nullptr, 0))) {
            for (int y = 0; y < 64; y++) std::memcpy(static_cast<char*>(lr.pBits) + y * lr.Pitch, &mask[static_cast<size_t>(y) * 64], 64 * 4);
            mtex->UnlockRect(0);
        }
        const auto bind = [&](DWORD s, IDirect3DTexture9* x, DWORD filter) {
            gpu.dev->SetTexture(s, x);
            gpu.dev->SetSamplerState(s, D3DSAMP_MINFILTER, filter);
            gpu.dev->SetSamplerState(s, D3DSAMP_MAGFILTER, filter);
            gpu.dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            gpu.dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            gpu.dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        };
        bind(p.sampler, tex, D3DTEXF_LINEAR);
        bind(static_cast<DWORD>(p.maskSampler), mtex, D3DTEXF_POINT);
        const float amb[4] = {0, 0, 0, 0}, size[4] = {static_cast<float>(W), static_cast<float>(H), 1.0f / W, 1.0f / H}, bc[4] = {1.0f / 65536.0f, 65536.0f, 0.5f, 65535.0f};
        gpu.dev->SetPixelShaderConstantF(0, amb, 1);
        gpu.dev->SetPixelShaderConstantF(p.sizeConst, size, 1);
        gpu.dev->SetPixelShaderConstantF(p.bandConst, bc, 1);
        // on, the whole map laid out by tile; off (cM.z = 0); on with the split tiles' atlas from column 16 (the dark room's
        // texels then keep the read without walls)
        for (int mode = 0; mode < 3; mode++) {
            const float split = mode == 2 ? 16.0f : static_cast<float>(W);
            const float k[4] = {1.0f / n, split, mode == 1 ? 0.0f : 1.0f, 1.0f / 64.0f};
            gpu.dev->SetPixelShaderConstantF(static_cast<UINT>(p.maskConst), k, 1);
            const std::vector<DWORD> px = gpu.Render(ps);
            int worst = 0, walled = 0, door = 0, doorRef = 0;
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++) {
                    const float cx = (x + 0.5f) / 64.0f * W, cy = (y + 0.5f) / 64.0f * H;
                    float ref[4];
                    if (mode == 1) Reference(map, W, H, cx, cy, 0, H - 1, ref);
                    else RefWalls(map, W, H, cx, cy, walls, tiles, n, split, ref);
                    const DWORD v = px[static_cast<size_t>(y) * 64 + x];
                    const int got[4] = {static_cast<int>((v >> 16) & 0xFF), static_cast<int>((v >> 8) & 0xFF), static_cast<int>(v & 0xFF), static_cast<int>(v >> 24)};
                    for (int c = 0; c < 4; c++) worst = std::max(worst, static_cast<int>(std::lround(std::fabs(got[c] - ref[c]))));
                    const int tz = static_cast<int>(cy / n);
                    if (cx >= 16.0f && cx < 18.0f) {
                        if (tz != 3 && tz != 4 && tz != 2 && tz != 5) walled = std::max(walled, got[1] - 3); // (away from the doorway's tiles)
                        if (tz == 3 || tz == 4) {
                            float plain[4];
                            Reference(map, W, H, cx, cy, 0, H - 1, plain);
                            door = std::max(door, static_cast<int>(std::lround(std::fabs(got[1] - plain[1])))), doorRef = std::max(doorRef, static_cast<int>(plain[1]));
                        }
                    }
                }
            const char* what[3] = {"on", "off", "on, atlas from column 16"};
            std::printf("  walls pixel check (%s, %s): largest difference from the CPU reference %d LSB; the dark room's first half metre along the wall +%d green; at the doorway %d LSB from the read without walls (which reaches %d)\n",
                        profile, what[mode], worst, walled, door, doorRef);
            Check(worst <= 3, "GPU walls read = CPU reference (3 LSB)");
            if (mode == 0) {
                Check(walled <= 3, "GPU: the dark room takes no light through the wall");
                Check(door <= 3 && doorRef > 30, "GPU: the doorway keeps the read without walls (the lit room's light comes through it)");
            } else Check(walled > 30, "with the walls off, the band through the wall is there (the test shows it)");
        }
        gpu.dev->SetTexture(p.sampler, nullptr);
        gpu.dev->SetTexture(static_cast<DWORD>(p.maskSampler), nullptr);
        Release(tex), Release(mtex), Release(ps);
    }
}

void PixelCheck(Gpu& gpu) {
    if (!gpu.dev) return;
    // the pattern: texld r0, t1, s0; mad r0.xyz, r0.w, c0, r0 (c0 = 0 here, so the output is the map read; alpha kept)
    const char* src = "sampler2D s0 : register(s0); float4 amb : register(c0);\n"
                      "float4 main(float2 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1) : COLOR0 { float4 m = tex2D(s0, uv1); m.rgb = m.a * amb.rgb + m.rgb; return m; }";
    std::vector<DWORD> code = Compile(src, "ps_2_0");
    Check(!code.empty(), "the synthetic room-lit shader compiles");
    Check(ShaderPatches::IsRoomMapPs(code), "the synthetic shader has the pattern");
    std::vector<DWORD> t = code;
    ShaderPatches::RoomMapPatch p;
    if (!ShaderPatches::PatchRoomMapSmooth(t, p)) {
        Check(false, "the synthetic shader is patched");
        std::printf("%s\n", Disasm(code).c_str());
        return;
    }
    if (!gpu.CapsAllow(p)) {
        std::printf("  (this device's PS20Caps refuse the copy: pixel check skipped)\n");
        return;
    }
    IDirect3DPixelShader9* ps = nullptr;
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(t.data(), &ps)), "the synthetic copy is created");
    if (!ps) return;
    // a 24 x 14 map: two bands of 7 rows (n = 2), strong contrast across the band line and texel noise
    constexpr int W = 24, H = 14, S = 7;
    std::vector<DWORD> map(W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const bool upper = y < S;
            const DWORD r = upper ? 40 + 9 * ((x * 7 + y * 3) % 23) : 250 - 5 * ((x * 5 + y) % 13);
            const DWORD g = upper ? 10 * (x % 9) + 3 * y : 20 + 7 * ((x + 2 * y) % 11);
            const DWORD b = upper ? 200 - 6 * x : 15 * (y - S) + 4 * (x % 5);
            const DWORD a = upper ? 30 + 20 * (y % 4) : 255 - 13 * (x % 7);
            map[static_cast<size_t>(y) * W + x] = (a << 24) | (std::min<DWORD>(r, 255) << 16) | (std::min<DWORD>(g, 255) << 8) | std::min<DWORD>(b, 255);
        }
    IDirect3DTexture9* tex = nullptr;
    Check(SUCCEEDED(gpu.dev->CreateTexture(W, H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)), "test map");
    if (!tex) {
        Release(ps);
        return;
    }
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
        for (int y = 0; y < H; y++) std::memcpy(static_cast<char*>(lr.pBits) + y * lr.Pitch, &map[static_cast<size_t>(y) * W], W * 4);
        tex->UnlockRect(0);
    }
    gpu.dev->SetTexture(p.sampler, tex);
    gpu.dev->SetSamplerState(p.sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    gpu.dev->SetSamplerState(p.sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    gpu.dev->SetSamplerState(p.sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    gpu.dev->SetSamplerState(p.sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    gpu.dev->SetSamplerState(p.sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    const float amb[4] = {0, 0, 0, 0};
    gpu.dev->SetPixelShaderConstantF(0, amb, 1);
    const float size[4] = {static_cast<float>(W), static_cast<float>(H), 1.0f / W, 1.0f / H};
    gpu.dev->SetPixelShaderConstantF(p.sizeConst, size, 1);
    for (int mode = 0; mode < 2; mode++) { // 0: wall bands of S rows; 1: one block (floors)
        const float band = mode == 0 ? static_cast<float>(S) : 65536.0f;
        const float bc[4] = {1.0f / band, band, 0.5f, band - 1.0f};
        gpu.dev->SetPixelShaderConstantF(p.bandConst, bc, 1);
        const std::vector<DWORD> px = gpu.Render(ps);
        int worst = 0;
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                const float cx = (x + 0.5f) / 64.0f * W, cy = (y + 0.5f) / 64.0f * H;
                const int row = mode == 0 ? static_cast<int>(std::floor(cy / S)) : 0;
                const int lo = mode == 0 ? row * S : 0, hi = mode == 0 ? row * S + S - 1 : H - 1;
                float ref[4];
                Reference(map, W, H, cx, cy, lo, hi, ref);
                const DWORD v = px[static_cast<size_t>(y) * 64 + x];
                const int got[4] = {static_cast<int>((v >> 16) & 0xFF), static_cast<int>((v >> 8) & 0xFF), static_cast<int>(v & 0xFF), static_cast<int>(v >> 24)};
                for (int c = 0; c < 4; c++) worst = std::max(worst, static_cast<int>(std::lround(std::fabs(got[c] - ref[c]))));
            }
        std::printf("  pixel check (%s): largest difference from the CPU B-spline %d LSB\n", mode == 0 ? "wall bands" : "one block", worst);
        Check(worst <= 3, mode == 0 ? "GPU B-spline within each wall band = CPU reference (3 LSB)" : "GPU B-spline over one block = CPU reference (3 LSB)");
    }
    // the band clamp matters: right at the band line the unclamped B-spline differs from the clamped one by far more
    {
        float a[4], b[4];
        Reference(map, W, H, 10.5f, S - 0.5f, 0, S - 1, a);
        Reference(map, W, H, 10.5f, S - 0.5f, 0, H - 1, b);
        Check(std::fabs(a[0] - b[0]) + std::fabs(a[2] - b[2]) > 10.0f, "the test map's bands differ enough to show a bleed");
    }
    gpu.dev->SetTexture(p.sampler, nullptr);
    Release(tex);
    Release(ps);
}

// The arch ambient (arch_ambient): a floor map of 64 x 64 texels = 16 x 16 m, black with full ambient weight, the room's
// ambient 0.1 grey, an arch on the line x = 8 m whose room's ambient is 0.2 more red: the GPU adds alpha x 0.2 x w(d) to red,
// w the CPU's ArchAmbient::Weight, and nothing to green and blue
void ArchPixelCheck(Gpu& gpu) {
    if (!gpu.dev) return;
    // the game's pattern in ps_3_0 (the HLSL compiler writes that mad straight to oC0): assembled
    const char* asmSrc = "ps_3_0\ndcl_texcoord1 v0.xy\ndcl_2d s0\ntexld r0, v0, s0\nmad r0.xyz, r0.w, c0, r0\nmov oC0, r0\n";
    std::vector<DWORD> code;
    {
        using Assemble = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, UINT, ID3DBlob**, ID3DBlob**);
        const auto lib = LoadLibraryW(L"d3dcompiler_47.dll");
        const auto assemble = lib ? reinterpret_cast<Assemble>(GetProcAddress(lib, "D3DAssemble")) : nullptr;
        ID3DBlob *blob = nullptr, *err = nullptr;
        if (assemble && SUCCEEDED(assemble(asmSrc, std::strlen(asmSrc), nullptr, nullptr, nullptr, 0, &blob, &err)) && blob) {
            code.resize(blob->GetBufferSize() / 4);
            std::memcpy(code.data(), blob->GetBufferPointer(), blob->GetBufferSize());
        }
        Release(blob);
        Release(err);
        if (lib) FreeLibrary(lib);
    }
    std::vector<DWORD> t = code;
    ShaderPatches::RoomMapPatch p;
    if (code.empty() || !ShaderPatches::PatchRoomMapSmooth(t, p, true) || p.archConst < 0 || p.ambConst != 0) {
        Check(false, "the synthetic ps_3_0 shader is patched with the arch ambient (cA = c0)");
        std::printf("  (archConst %d, ambConst %d)\n%s\n", p.archConst, p.ambConst, Disasm(code).c_str());
        return;
    }
    IDirect3DPixelShader9* ps = nullptr;
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(t.data(), &ps)), "the arch copy is created");
    IDirect3DTexture9* tex = nullptr;
    gpu.dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
    if (!ps || !tex) {
        Release(ps);
        Release(tex);
        return;
    }
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) reinterpret_cast<DWORD*>(static_cast<char*>(lr.pBits) + y * lr.Pitch)[x] = 0xFF000000u;
        tex->UnlockRect(0);
    }
    gpu.dev->SetTexture(p.sampler, tex);
    gpu.dev->SetSamplerState(p.sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    gpu.dev->SetSamplerState(p.sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    gpu.dev->SetSamplerState(p.sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    gpu.dev->SetSamplerState(p.sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    const float amb[4] = {0.1f, 0.1f, 0.1f, 0.0f}, size[4] = {64, 64, 1.0f / 64, 1.0f / 64}, band[4] = {1.0f / 65536, 65536, 0.5f, 65535};
    gpu.dev->SetPixelShaderConstantF(0, amb, 1);
    gpu.dev->SetPixelShaderConstantF(p.sizeConst, size, 1);
    gpu.dev->SetPixelShaderConstantF(p.bandConst, band, 1);
    float arch[2 * ShaderPatches::kArchCells * 4] = {};
    // the arch from (8, 2) to (8, 14): seg (a.x, a.z, e.x, e.z), D (0.2, 0, 0, 1 / |e|^2)
    arch[0] = 8.0f, arch[1] = 2.0f, arch[2] = 0.0f, arch[3] = 12.0f;
    arch[4] = 0.2f, arch[5] = 0.0f, arch[6] = 0.0f, arch[7] = 1.0f / 144.0f;
    gpu.dev->SetPixelShaderConstantF(static_cast<UINT>(p.archConst), arch, 2 * ShaderPatches::kArchCells);
    const std::vector<DWORD> px = gpu.Render(ps);
    int worst = 0, gb = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const float lx = (x + 0.5f) / 64.0f * 16.0f, lz = (y + 0.5f) / 64.0f * 16.0f;
            const float w = ArchAmbient::Weight(ArchAmbient::SegDist(lx, lz, 8.0f, 2.0f, 8.0f, 14.0f));
            const float ref = (0.1f + 0.2f * w) * 255.0f;
            const DWORD v = px[static_cast<size_t>(y) * 64 + x];
            worst = std::max(worst, static_cast<int>(std::lround(std::fabs(static_cast<float>((v >> 16) & 0xFF) - ref))));
            gb = std::max({gb, std::abs(static_cast<int>((v >> 8) & 0xFF) - 26), std::abs(static_cast<int>(v & 0xFF) - 26)});
        }
    std::printf("  arch ambient pixel check: red against the CPU law %d LSB, green and blue off the room's own %d LSB\n", worst, gb);
    Check(worst <= 2, "GPU arch ambient = CPU law (2 LSB)");
    Check(gb <= 1, "only the channel the neighbour has more of is raised");
    // nothing passed (all D = 0): the room's own ambient alone
    std::fill(std::begin(arch), std::end(arch), 0.0f);
    gpu.dev->SetPixelShaderConstantF(static_cast<UINT>(p.archConst), arch, 2 * ShaderPatches::kArchCells);
    const std::vector<DWORD> none = gpu.Render(ps);
    int off = 0;
    for (DWORD v : none) off = std::max({off, std::abs(static_cast<int>((v >> 16) & 0xFF) - 26), std::abs(static_cast<int>((v >> 8) & 0xFF) - 26)});
    Check(off <= 1, "no arch passed: the room's own ambient, unchanged");
    gpu.dev->SetTexture(p.sampler, nullptr);
    Release(tex);
    Release(ps);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: check.exe <Shaders_Win32.precomp> [capture folder ...]\n");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    const std::vector<unsigned char> data{std::istreambuf_iterator<char>(f), {}};
    std::vector<Blob> blobs;
    std::vector<Tech> techs;
    Check(LoadPrecomp(data, blobs, techs), "precomp read");
    Gpu gpu;
    if (gpu.dev)
        std::printf("Device: PS %X, PS20Caps temps %d slots %d arbitrary swizzle %s\n", gpu.caps.PixelShaderVersion & 0xFFFF, gpu.caps.PS20Caps.NumTemps,
                    gpu.caps.PS20Caps.NumInstructionSlots, (gpu.caps.PS20Caps.Caps & D3DPS20CAPS_ARBITRARYSWIZZLE) ? "yes" : "no");
    const char* roomLit[] = {"InteriorFloor", "InteriorWall", "InteriorWallWithBlacklight", "InteriorWallWithStrobelight", "InteriorWallWithStrobeAndBlacklight",
                             "FloorTileCeiling", "FloorWithBlacklight", "FloorWithStrobelight", "FloorWithStrobelightAndBlacklight", "Hideable"};
    const char* watched[] = {"ExteriorWall", "Ceiling", "TerrainLight_Indoor", "TerrainLight_IndoorWithStrobeLight", "InteriorFloorUnlit"};
    std::map<uint32_t, std::string> names;
    std::set<uint32_t> lit;
    for (const char* n : roomLit) names[Fnv1Lower(n)] = n, lit.insert(Fnv1Lower(n));
    for (const char* n : watched) names[Fnv1Lower(n)] = n;
    // the techniques of every pixel shader
    std::map<uint32_t, std::set<uint32_t>> techOf; // ps blob (1-based) -> technique name hashes
    std::map<uint32_t, std::set<uint32_t>> vsOf;   // ps blob -> its vertex shaders
    for (const Tech& t : techs)
        for (const auto& [vs, ps] : t.passes) {
            techOf[ps].insert(t.name);
            vsOf[ps].insert(vs);
        }
    // 1-2: every precomp pixel shader
    PatchStats st;
    std::map<std::string, std::pair<int, int>> perTech; // name -> (matched, distinct PS)
    std::set<uint32_t> matchedPs;
    int outside = 0, total = 0;
    for (uint32_t i = 0; i < blobs.size(); i++) {
        const Blob& b = blobs[i];
        if (b.vs || b.code.empty()) continue;
        total++;
        const bool m = ShaderPatches::IsRoomMapPs(b.code);
        const auto tt = techOf.find(i + 1);
        bool anyLit = false;
        if (tt != techOf.end())
            for (uint32_t h : tt->second) {
                anyLit |= lit.count(h) != 0;
                if (const auto n = names.find(h); n != names.end()) {
                    perTech[n->second].second++;
                    perTech[n->second].first += m;
                }
            }
        if (!m) continue;
        st.matched++;
        matchedPs.insert(i + 1);
        if (!anyLit) {
            outside++;
            std::printf("  match outside the room-lit techniques: blob %u\n", i);
        }
        char where[64];
        std::snprintf(where, sizeof where, "precomp PS blob %u", i);
        CheckPatch(b.code, gpu, st, where);
    }
    std::printf("Precomp: %d pixel shaders, %d match (%d patched, %d disassemble, %d with the taps and bound reads, %d created natively, %d left to the game by the caps)\n", total, st.matched,
                st.patched, st.disasmOk, st.texldOk, st.created, st.capsRefused);
    std::printf("  with the basis maps too: %d (both reads in one copy, %d created natively)\n", st.combined, st.combinedCreated);
    std::printf("  with the arch ambient (ps_3_0): %d copies, %d created natively\n", st.arch, st.archCreated);
    Check(st.arch > 0 && st.arch == st.archCreated, "every copy with the arch ambient is created natively");
    std::printf("  with the walls (floors, basis_wall_filter): %d copies of %d patched, %d created natively\n", st.walls, st.patched, st.wallsCreated);
    Check(st.walls > 0 && st.walls == st.wallsCreated, "every walls copy made is created natively");
    for (const auto& [n, c] : perTech) std::printf("  %-38s %3d of %3d\n", n.c_str(), c.first, c.second);
    // The outdoor scale on the outside walls (ShaderPatches::PatchWallScale): every ps_3_0 ExteriorWall pixel shader takes it
    // and the copy is created natively (the ps_2_x ones, lower shader settings, keep the cut at 1)
    {
        const uint32_t ew = Fnv1Lower("ExteriorWall");
        int walls = 0, patched = 0, created = 0, ps2 = 0;
        for (uint32_t i = 0; i < blobs.size(); i++) {
            const Blob& b = blobs[i];
            const auto tt = techOf.find(i + 1);
            if (b.vs || b.code.empty() || tt == techOf.end() || !tt->second.count(ew)) continue;
            if (b.code[0] != 0xFFFF0300) {
                ps2++;
                continue;
            }
            walls++;
            std::vector<DWORD> c = b.code;
            ShaderPatches::WallScalePatch wp;
            if (!ShaderPatches::PatchWallScale(c, wp)) continue;
            patched++;
            if (gpu.dev && gpu.AcceptsPs(c)) created++;
        }
        std::printf("  outside walls with the outdoor scale: %d of %d ps_3_0 ExteriorWall shaders patched, %d created natively (%d ps_2_x left as they are)\n", patched, walls,
                    created, ps2);
        Check(walls > 0 && patched == walls && (!gpu.dev || created == walls), "every ps_3_0 ExteriorWall shader takes the outdoor scale and is created natively");
    }
    Check(outside == 0, "no match outside the room-lit techniques");
    Check(perTech["InteriorWall"].first == perTech["InteriorWall"].second && perTech["InteriorWall"].second > 0, "every InteriorWall shader matches");
    Check(perTech["InteriorFloor"].first >= 48, "the InteriorFloor shaders with a room map match (48 of 58: 10 have none)");
    for (const char* n : watched) Check(perTech[n].first == 0, "no ExteriorWall, Ceiling, TerrainLight_Indoor* or InteriorFloorUnlit shader matches");
    // 3: the vertex shaders paired with the matches
    std::set<uint32_t> vss;
    for (uint32_t ps : matchedPs)
        for (uint32_t vs : vsOf[ps]) vss.insert(vs);
    int wall = 0, floor = 0, runtime = 0, other = 0, none = 0;
    for (uint32_t vs : vss) {
        if (vs == 0 || vs > blobs.size() || !blobs[vs - 1].vs) continue;
        ShaderPatches::RoomMapUv uv;
        if (!ShaderPatches::RoomMapUvScale(blobs[vs - 1].code, uv)) {
            none++;
            continue;
        }
        if (!uv.defined) runtime++;
        else if (std::fabs(uv.value * 4096.0f - 1.0f) < 1e-3f) wall++;
        else if (std::fabs(uv.value * 32767.0f - 1.0f) < 1e-3f) floor++;
        else other++;
    }
    std::printf("Paired vertex shaders: %zu; uv scale 1/4096 (walls) %d, 1/32767 (floors) %d, set at the draw %d, other %d, not found %d\n", vss.size(), wall, floor,
                runtime, other, none);
    Check(wall > 0 && floor > 0, "wall and floor vertex shaders are told apart by their uv scale");
    Check(other == 0, "no paired vertex shader has another defined uv scale");
    // 4: pixels
    BoundChecks();
    SideChecks();
    PixelCheck(gpu);
    SidePixelCheck(gpu);
    WallPixelCheck(gpu);
    ArchPixelCheck(gpu);
    // 5: captures
    std::set<std::vector<DWORD>> seen;
    int capPs = 0, capMatched = 0;
    PatchStats cst;
    for (int a = 2; a < argc; a++) {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(argv[a], ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            const auto& p = it->path();
            if (!it->is_regular_file() || p.extension() != ".bin" || p.filename().string().rfind("PS_", 0) != 0) continue;
            std::ifstream in(p, std::ios::binary);
            std::vector<unsigned char> raw{std::istreambuf_iterator<char>(in), {}};
            if (raw.size() < 8 || raw.size() % 4) continue;
            std::vector<DWORD> code(raw.size() / 4);
            std::memcpy(code.data(), raw.data(), raw.size());
            if ((code[0] & 0xFFFF0000u) != 0xFFFF0000u || !seen.insert(code).second) continue;
            capPs++;
            if (!ShaderPatches::IsRoomMapPs(code)) continue;
            capMatched++;
            cst.matched++;
            CheckPatch(code, gpu, cst, p.string().c_str());
        }
    }
    if (argc > 2) std::printf("Captures: %d distinct pixel shaders, %d match (%d created natively)\n", capPs, capMatched, cst.created);
    // 6: dither offset
    {
        double sum = 0.0;
        float lo = 1.0f, hi = -1.0f;
        for (unsigned y = 0; y < 512; y++)
            for (unsigned x = 0; x < 1024; x++) {
                const float o = RoomMapDither::Offset(x, y);
                sum += o;
                lo = std::min(lo, o), hi = std::max(hi, o);
            }
        Check(lo >= -0.5f && hi <= 0.5f, "dither: offsets within half a level");
        Check(std::fabs(sum / (512.0 * 1024.0)) < 0.01, "dither: mean about 0 (the light keeps its level)");
        Check(RoomMapDither::Offset(37, 91) == RoomMapDither::Offset(37, 91), "dither: the same texel gets the same offset (no shimmer)");
        // neighbours differ (a grain, not a flat shift)
        int same = 0;
        for (unsigned x = 0; x < 256; x++) same += std::fabs(RoomMapDither::Offset(x, 5) - RoomMapDither::Offset(x + 1, 5)) < 0.01f;
        Check(same < 16, "dither: neighbouring texels take different offsets");
    }
    std::printf("%d checks, %d failures\n%s\n", g_checks, g_failures, g_failures ? "PROBLEMS" : "OK");
    return g_failures ? 1 : 0;
}
