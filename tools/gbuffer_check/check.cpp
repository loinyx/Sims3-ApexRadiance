// Offline check of the G-buffer shader copies (ShaderPatches::AddGBufferPs merged with AddDither, shaders/gbuffer_ids.h).
// Read-only: prints to stdout, writes no file.
//   check.exe <Shaders_Win32.precomp> [--d3d9 <d3d9.dll>]
// 1. Every ps_3_0 of the package through AddDither then AddGBufferPs, with the table's row or, without one, the
//    "unknown" values; each copy created on a D3D9 HAL device. AddGBufferPs alone must only insert: with its own
//    instructions taken out again, the shader is the game's token for token (so every oC0 token is unchanged).
//    Counts per family of the table: accepted, created, albedo found, normal found, refused by reason.
// 2. On the GPU, three render targets: a synthetic ps_3_0 (one texture read, one cube read by a constant direction)
//    through the patch. Target 0 is bit for bit the unpatched shader's; target 1 holds the texture colour and the id;
//    target 2 decodes to the direction for 26 directions, with the roughness and the class.
// --d3d9 runs both again on another D3D9 implementation (a DXVK d3d9.dll), loaded from that path.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include "../../features/shader_patches.h"
#include "../../shaders/shader_ids.h"
#include "../../shaders/gbuffer_ids.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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

// ---- Shaders_Win32.precomp (docs/engine/shaders.md): every VSHD / PSHD blob of the SHDB chunk ----
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
std::vector<std::vector<DWORD>> LoadUnique(const std::vector<unsigned char>& d) {
    std::vector<std::vector<DWORD>> out;
    std::set<std::pair<size_t, uint32_t>> seen;
    size_t shdb = std::string::npos;
    for (size_t i = 0; i + 4 <= d.size() && shdb == std::string::npos; i++)
        if (std::memcmp(&d[i], "SHDB", 4) == 0) shdb = i;
    if (shdb == std::string::npos) return out;
    uint32_t size;
    std::memcpy(&size, &d[shdb + 4], 4);
    const size_t end = std::min(d.size(), shdb + 8 + size);
    for (size_t j = shdb + 8; j + 8 <= end;) {
        if (std::memcmp(&d[j], "VSHD", 4) != 0 && std::memcmp(&d[j], "PSHD", 4) != 0) {
            j++;
            continue;
        }
        uint32_t n;
        std::memcpy(&n, &d[j + 4], 4);
        std::vector<DWORD> c = Trim(&d[j + 8], std::min<size_t>(n, d.size() - j - 8));
        if (!c.empty() && seen.insert({c.size(), ShaderHash(c.data(), c.size() * 4)}).second) out.push_back(std::move(c));
        j += 8 + n;
    }
    return out;
}

// ---- a hidden D3D9 device with the G-buffer's three targets (64 x 64) ----
constexpr int kSize = 64;
constexpr D3DFORMAT kFormats[3] = {D3DFMT_A8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_A2B10G10R10};
struct Gpu {
    HWND window = nullptr;
    IDirect3D9* api = nullptr;
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9 *target[3] = {}, *readback[3] = {};
    IDirect3DTexture9* albedo = nullptr;
    bool mrt = false;
    explicit Gpu(HMODULE d3d9) {
        window = CreateWindowExW(0, L"STATIC", L"Apex G-buffer check", WS_OVERLAPPED, 0, 0, kSize, kSize, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        using Create = IDirect3D9*(WINAPI*)(UINT);
        const auto create = reinterpret_cast<Create>(GetProcAddress(d3d9, "Direct3DCreate9"));
        api = create ? create(D3D_SDK_VERSION) : nullptr;
        D3DPRESENT_PARAMETERS p{};
        p.Windowed = TRUE;
        p.SwapEffect = D3DSWAPEFFECT_DISCARD;
        p.hDeviceWindow = window;
        p.BackBufferFormat = D3DFMT_UNKNOWN;
        p.BackBufferWidth = p.BackBufferHeight = kSize;
        if (api) api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &p, &dev);
        Check(dev != nullptr, "D3D9 device available");
        if (!dev) return;
        D3DCAPS9 caps{};
        dev->GetDeviceCaps(&caps);
        mrt = caps.NumSimultaneousRTs >= 3;
        Check(mrt, "three simultaneous render targets");
        for (int i = 0; i < 3; i++) {
            Check(SUCCEEDED(dev->CreateRenderTarget(kSize, kSize, kFormats[i], D3DMULTISAMPLE_NONE, 0, FALSE, &target[i], nullptr)), "render target");
            Check(SUCCEEDED(dev->CreateOffscreenPlainSurface(kSize, kSize, kFormats[i], D3DPOOL_SYSTEMMEM, &readback[i], nullptr)), "readback surface");
        }
        D3DLOCKED_RECT r{};
        if (SUCCEEDED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &albedo, nullptr)) && SUCCEEDED(albedo->LockRect(0, &r, nullptr, 0))) {
            *static_cast<DWORD*>(r.pBits) = 0xFF8040C0u; // r 0x80, g 0x40, b 0xC0
            albedo->UnlockRect(0);
        } else
            Check(false, "test texture");
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    }
    ~Gpu() {
        for (int i = 0; i < 3; i++) {
            Release(target[i]);
            Release(readback[i]);
        }
        Release(albedo);
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
    // a full-screen quad through the shaders into `targets` targets; the centre pixel of each
    bool Render(const std::vector<DWORD>& vsCode, const std::vector<DWORD>& psCode, const float n[4], int targets, DWORD out[3]) {
        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        IDirect3DVertexDeclaration9* decl = nullptr;
        struct Vertex {
            float x, y, z, w;
        };
        const Vertex q[4] = {{-1, 1, 0.5f, 1}, {1, 1, 0.5f, 1}, {-1, -1, 0.5f, 1}, {1, -1, 0.5f, 1}};
        const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
        bool ok = dev && SUCCEEDED(dev->CreateVertexShader(vsCode.data(), &vs)) && SUCCEEDED(dev->CreatePixelShader(psCode.data(), &ps)) &&
                  SUCCEEDED(dev->CreateVertexDeclaration(el, &decl));
        if (ok) {
            for (int i = 0; i < 3; i++) dev->SetRenderTarget(static_cast<DWORD>(i), i < targets ? target[i] : nullptr);
            dev->SetVertexDeclaration(decl);
            dev->SetVertexShader(vs);
            dev->SetPixelShader(ps);
            dev->SetTexture(0, albedo);
            dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            dev->SetPixelShaderConstantF(0, n, 1);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
            dev->BeginScene();
            ok = SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vertex)));
            dev->EndScene();
            for (int i = 0; i < targets && ok; i++) {
                D3DLOCKED_RECT r{};
                ok = SUCCEEDED(dev->GetRenderTargetData(target[i], readback[i])) && SUCCEEDED(readback[i]->LockRect(&r, nullptr, D3DLOCK_READONLY));
                if (!ok) break;
                std::memcpy(&out[i], static_cast<char*>(r.pBits) + (kSize / 2) * r.Pitch + (kSize / 2) * 4, 4);
                readback[i]->UnlockRect();
            }
            for (DWORD i = 1; i < 3; i++) dev->SetRenderTarget(i, nullptr);
        }
        Release(decl);
        Release(vs);
        Release(ps);
        return ok;
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

// ---- token helpers for the "only inserts" check ----
DWORD RegType(DWORD r) { return ((r >> 28) & 7) | (((r >> 11) & 3) << 3); }
// t without the instructions AddGBufferPs added: defs of its 2 constants, writes to its 3 temps, writes to oC1 / oC2
std::vector<DWORD> Strip(const std::vector<DWORD>& t, const ShaderPatches::GBufferMade& made) {
    std::vector<DWORD> out{t[0]};
    for (size_t i = 1; i < t.size();) {
        const DWORD tok = t[i];
        if (tok == 0x0000FFFFu) {
            out.push_back(tok);
            break;
        }
        const bool comment = (tok & 0xFFFF) == 0xFFFE;
        const size_t len = comment ? (tok >> 16) & 0x7FFF : (tok >> 24) & 0xF;
        bool ours = false;
        if (!comment && len >= 1) {
            const DWORD op = tok & 0xFFFF, d = t[i + 1], type = RegType(d), num = d & 0x7FF;
            if (op == 0x51) ours = type == 2 && (num == static_cast<DWORD>(made.firstConst) || num == static_cast<DWORD>(made.firstConst) + 1);
            else if (op != 0x1F && op != 0x41 && (d & 0x80000000u)) // not dcl / texkill: operand 1 is the destination
                ours = (type == 0 && num >= static_cast<DWORD>(made.firstTemp) && num < static_cast<DWORD>(made.firstTemp) + 3) || (type == 8 && num != 0);
        }
        if (!ours) out.insert(out.end(), t.begin() + i, t.begin() + i + 1 + len);
        i += 1 + len;
    }
    return out;
}

ShaderPatches::GBufferInfo InfoOf(const GBufferShaderId* row) {
    ShaderPatches::GBufferInfo info; // without a row: the "unknown" values
    if (!row) return info;
    info.albedoSampler = row->albedo == 255 ? -1 : row->albedo;
    info.probeSampler = row->probe == 255 ? -1 : row->probe;
    info.material = row->material;
    info.motionClass = row->classAndFamily & 3;
    info.roughness = kGBufferRoughness[row->material];
    return info;
}

const char* Reason(ShaderPatches::GBufferResult r) {
    static const char* names[] = {"ok", "version", "unreadable", "subroutines", "relative constants", "no free register", "outputs used"};
    return names[static_cast<int>(r)];
}

// ---- 1. the game's shaders ----
void Package(Gpu& gpu, const std::vector<std::vector<DWORD>>& all) {
    constexpr int kFamilies = static_cast<int>(std::size(kGBufferFamilies));
    struct Count {
        int n = 0, accepted = 0, created = 0, albedo = 0, normal = 0, albedoWanted = 0, normalWanted = 0;
        std::map<std::string, int> why;
    };
    Count fam[kFamilies + 1]; // last: every ps_3_0 without a row
    int inserted = 0, rows = 0;
    for (const auto& code : all) {
        if (code[0] != 0xFFFF0300u) continue;
        const GBufferShaderId* row = FindGBufferShader(ShaderHash(code.data(), code.size() * 4));
        rows += row != nullptr;
        const ShaderPatches::GBufferInfo info = InfoOf(row);
        // alone: only insertions
        ShaderPatches::GBufferMade made;
        std::vector<DWORD> alone = code;
        const auto r = ShaderPatches::AddGBufferPs(alone, info, &made);
        const bool onlyInserts = r == ShaderPatches::GBufferResult::Ok && Strip(alone, made) == code;
        if (r == ShaderPatches::GBufferResult::Ok) {
            inserted += onlyInserts;
            Check(onlyInserts, "the patch only inserts instructions");
        }
        // merged with the Banding Fix copy, as the game gets it
        std::vector<DWORD> merged = code;
        const bool dithered = ShaderPatches::AddDither(merged) == ShaderPatches::DitherResult::Ok;
        const auto rm = dithered ? ShaderPatches::AddGBufferPs(merged, info, &made) : ShaderPatches::GBufferResult::Unreadable;
        const bool ok = rm == ShaderPatches::GBufferResult::Ok, created = ok && gpu.AcceptsPs(merged);
        for (int f = 0; f <= kFamilies; f++) {
            if (f < kFamilies ? !(row && ((row->classAndFamily >> 2) >> f) & 1) : row != nullptr) continue;
            Count& c = fam[f];
            c.n++;
            c.accepted += ok;
            c.created += created;
            c.albedoWanted += info.albedoSampler >= 0;
            c.normalWanted += info.probeSampler >= 0;
            if (ok) {
                c.albedo += made.albedo;
                c.normal += made.normal;
            } else
                c.why[dithered ? Reason(rm) : "Banding Fix refused"]++;
        }
    }
    Check(rows == static_cast<int>(std::size(kGBufferShaders)), "every table row is a ps_3_0 of the package");
    std::printf("  table rows found %d of %zu; only-insert check passed for %d shaders\n", rows, std::size(kGBufferShaders), inserted);
    for (int f = 0; f <= kFamilies; f++) {
        const Count& c = fam[f];
        std::string why;
        for (const auto& [k, n] : c.why) why += " " + k + " " + std::to_string(n) + ";";
        std::printf("  %-12s %4d ps_3_0: accepted %d, created %d, albedo %d of %d, normal %d of %d%s%s\n", f < kFamilies ? kGBufferFamilies[f] : "(no row)", c.n,
                    c.accepted, c.created, c.albedo, c.albedoWanted, c.normal, c.normalWanted, why.empty() ? "" : " | refused:", why.c_str());
        if (f < kFamilies) {
            Check(c.n > 0 && c.accepted == c.n, "every shader of the family accepted");
            Check(c.created == c.accepted, "every copy of the family created by the device");
            Check(c.albedo == c.albedoWanted && c.normal == c.normalWanted, "the table's albedo and probe reads found");
        }
    }
}

// ---- 2. the outputs on the GPU ----
void OnGpu(Gpu& gpu) {
    const std::vector<DWORD> vs = Compile("float4 main(float4 p : POSITION) : POSITION { return p; }", "vs_3_0");
    const std::vector<DWORD> ps = Compile("sampler2D sD : register(s0); samplerCUBE sP : register(s1); float4 cN : register(c0);\n"
                                          "float4 main() : COLOR0 { float3 a = tex2D(sD, cN.ww).rgb; float3 p = texCUBE(sP, cN.xyz * 3).rgb;\n"
                                          "  return float4(a * 0.5 + p * 0.25 + 0.125, 0.75); }",
                                          "ps_3_0");
    Check(!vs.empty() && !ps.empty(), "test shaders compile");
    if (vs.empty() || ps.empty() || !gpu.mrt) return;
    ShaderPatches::GBufferInfo info;
    info.albedoSampler = 0, info.probeSampler = 1, info.material = 15, info.motionClass = 2, info.roughness = 0.6f;
    ShaderPatches::GBufferMade made;
    std::vector<DWORD> patched = ps;
    Check(ShaderPatches::AddGBufferPs(patched, info, &made) == ShaderPatches::GBufferResult::Ok && made.albedo && made.normal, "test shader patched with albedo and normal");
    std::vector<DWORD> flagged = ps; // neither sampler: the unknown outputs
    info.albedoSampler = info.probeSampler = -1;
    Check(ShaderPatches::AddGBufferPs(flagged, info) == ShaderPatches::GBufferResult::Ok, "test shader patched without samplers");
    double worst = 0;
    int n = 0;
    for (int x = -1; x <= 1; x++)
        for (int y = -1; y <= 1; y++)
            for (int z = -1; z <= 1; z++) {
                if (!x && !y && !z) continue;
                const float len = std::sqrt(static_cast<float>(x * x + y * y + z * z));
                const float dir[4] = {x / len, y / len, z / len, 0.5f};
                DWORD plain[3] = {}, out[3] = {};
                if (!gpu.Render(vs, ps, dir, 1, plain) || !gpu.Render(vs, patched, dir, 3, out)) {
                    Check(false, "test draws");
                    return;
                }
                n++;
                Check(out[0] == plain[0], "target 0 is the unpatched shader's, bit for bit");
                Check(out[1] == 0x0F8040C0u, "target 1 = the texture colour with the material id");
                // A2B10G10R10: r = oct x, g = oct y, b = roughness, a = class
                const double ox = (out[2] & 0x3FF) / 1023.0 * 2 - 1, oy = ((out[2] >> 10) & 0x3FF) / 1023.0 * 2 - 1;
                double d[3] = {ox, oy, 1 - std::fabs(ox) - std::fabs(oy)};
                const double t = std::max(-d[2], 0.0);
                d[0] += d[0] >= 0 ? -t : t;
                d[1] += d[1] >= 0 ? -t : t;
                const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                const double err = std::acos(std::min(1.0, (d[0] * dir[0] + d[1] * dir[1] + d[2] * dir[2]) / l)) * 57.29578;
                worst = std::max(worst, err);
                Check(err < 0.5, "target 2 decodes to the direction within 0.5 degrees");
                Check(((out[2] >> 20) & 0x3FF) == 614 && (out[2] >> 30) == 2, "target 2 = roughness 0.6 and class 2");
            }
    const float up[4] = {0, 1, 0, 0.5f};
    DWORD out[3] = {};
    if (gpu.Render(vs, flagged, up, 3, out)) {
        Check(out[1] == 0xFF000000u, "no albedo: black with id 255");
        const DWORD x = out[2] & 0x3FF, y = (out[2] >> 10) & 0x3FF; // 0.5 of 1023 rounds either way
        Check((x == 511 || x == 512) && (y == 511 || y == 512) && ((out[2] >> 20) & 0x3FF) == 0 && (out[2] >> 30) == 2, "no normal: roughness 0, the class kept");
        if (out[1] != 0xFF000000u || (out[2] >> 20) != 0x800) std::printf("  without samplers: target 1 %08lX, target 2 %08lX\n", out[1], out[2]);
    } else
        Check(false, "test draw without samplers");
    std::printf("  GPU: %d directions, worst normal error %.3f degrees\n", n, worst);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("usage: check <Shaders_Win32.precomp> [--d3d9 <d3d9.dll>]");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    const std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), {});
    const auto all = LoadUnique(d);
    std::printf("%zu unique shaders in %s\n", all.size(), argv[1]);
    Check(!all.empty(), "shader package read");
    {
        std::puts("Native D3D9");
        Gpu gpu(LoadLibraryW(L"d3d9.dll"));
        Package(gpu, all);
        OnGpu(gpu);
    }
    for (int a = 2; a + 1 < argc; a++) {
        if (std::strcmp(argv[a], "--d3d9") != 0) continue;
        HMODULE other = LoadLibraryA(argv[a + 1]);
        Check(other != nullptr, "the other d3d9.dll loads");
        if (!other) continue;
        std::printf("%s\n", argv[a + 1]);
        Gpu gpu(other);
        Package(gpu, all);
        OnGpu(gpu);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
