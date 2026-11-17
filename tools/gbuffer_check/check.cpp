// Offline check of the G-buffer shader copies (ShaderPatches::AddGBufferPs merged with AddDither, shaders/gbuffer_ids.h).
// Read-only: prints to stdout, writes no file.
//   check.exe <Shaders_Win32.precomp> [--families <generate.cjs families file>] [--d3d9 <d3d9.dll>]
// 1. Every ps_3_0 and ps_2_0 of the package through AddDither / AddDither2 then AddGBufferPs, with the table's row or,
//    without one, the "unknown" values; each copy created on a D3D9 HAL device. AddGBufferPs alone must only insert:
//    with its own instructions taken out again, the shader is the game's token for token (a ps_2_0 becomes ps_2_x).
//    Counts per version and per technique family: accepted, created, albedo found, normal found, refused by reason.
// 2. On the GPU, three render targets: a synthetic ps_3_0 and ps_2_0 (one texture read, one cube read by a constant
//    direction) through the patch. Target 0 is bit for bit the unpatched shader's; target 1 holds the texture colour
//    and the id; target 2 decodes to the direction for 26 directions, with the roughness and the class.
// 3. With --ao <hlsl> (Ambient Occlusion's HLSL, taken from patches/ambient_occlusion_patch.cpp by run.ps1), its G-buffer
//    consumers on the GPU (GB-04): GtaoPS on a flat wall facing the camera leaves it unshaded with the depth normal and
//    with the G-buffer's (equal) normal, shades it with a tilted G-buffer normal, and ignores the G-buffer when off;
//    TemporalPS blends the history by 1 / frames, follows the motion vector, clips the history to the neighbourhood,
//    accepts a depth that the camera's depth ratio explains, and starts a new history on another id, motion class 3, a
//    1/z jump, invalid motion or no history.
// 4. With --rules <file> (generate.cjs's sampler classes of the albedo rules), every shader with a composited-albedo
//    rule (shaders/gbuffer_ids.h, kGBufferAlbedoRules) on the GPU with uniform textures by sampler class: target 1 must
//    not change when only the light-class textures change (the value is taken before the light), must change when the
//    albedo-class textures change, and must carry an id. Measured, not gated: how far target 1 is from the albedo
//    textures' colour (the reference when every albedo layer has that colour; the material step of the family, Sim skin
//    tone or terrain detail, moves it), per family; this is also the error of reading one albedo texture alone.
// --d3d9 runs all of it again on another D3D9 implementation (a DXVK d3d9.dll), loaded from that path.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include "../../features/shader_patches.h"
#include "../../shaders/shader_ids.h"
#include "../../shaders/gbuffer_ids.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <format>
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
    if (const GBufferAlbedoRule* rule = FindGBufferAlbedoRule(row->hash)) info.albedoAt = rule->at, info.albedoReg = rule->reg;
    info.albedoSampler = row->albedo == 255 ? -1 : row->albedo;
    info.probeSampler = row->probe == 255 ? -1 : row->probe;
    info.material = row->material;
    info.motionClass = row->motionClass;
    info.roughness = kGBufferRoughness[row->material];
    return info;
}

const char* Reason(ShaderPatches::GBufferResult r) {
    static const char* names[] = {"ok", "version", "unreadable", "subroutines", "relative constants", "no free register", "outputs used"};
    return names[static_cast<int>(r)];
}

// ---- 1. the game's shaders ----
// families: technique name -> the hashes of its pixel shaders (generate.cjs's families file; empty: totals only)
using Families = std::map<std::string, std::set<uint32_t>>;
Families ReadFamilies(const char* path) {
    Families out;
    std::ifstream f(path);
    std::string name;
    while (f >> name) {
        std::string line;
        std::getline(f, line);
        std::istringstream hashes(line);
        std::string h;
        while (hashes >> h) out[name].insert(static_cast<uint32_t>(std::stoul(h, nullptr, 16)));
    }
    return out;
}

void Package(Gpu& gpu, const std::vector<std::vector<DWORD>>& all, const Families& families) {
    struct Count {
        int n = 0, accepted = 0, created = 0, albedo = 0, normal = 0, albedoWanted = 0, normalWanted = 0;
        std::map<std::string, int> why;
        int rule = 0; // of albedo: by the family rule
        void Add(bool ok, bool created_, const ShaderPatches::GBufferInfo& info, const ShaderPatches::GBufferMade& made, const char* refusal) {
            n++, accepted += ok, created += created_;
            if (ok)
                albedo += made.albedo, normal += made.normal, albedoWanted += info.albedoSampler >= 0 || info.albedoAt >= 0, normalWanted += info.probeSampler >= 0,
                    rule += made.albedo && info.albedoSampler < 0;
            else why[refusal]++;
        }
        std::string Line() const {
            std::string w;
            for (const auto& [k, c] : why) w += " " + k + " " + std::to_string(c) + ";";
            return std::format("{:5} accepted {}, created {}, albedo {} of {} (rule {}), normal {} of {}{}{}", n, accepted, created, albedo, albedoWanted, rule, normal, normalWanted,
                               w.empty() ? "" : " | refused:", w);
        }
    };
    Count total[2], noRow[2];                     // [ps_3_0, ps_2_0]
    std::map<std::string, Count> fam[2];
    int inserted = 0, rows = 0;
    for (const auto& code : all) {
        const int v = code[0] == 0xFFFF0300u ? 0 : code[0] == 0xFFFF0200u ? 1 : -1;
        if (v < 0) continue;
        const uint32_t hash = ShaderHash(code.data(), code.size() * 4);
        const GBufferShaderId* row = FindGBufferShader(hash);
        rows += row != nullptr;
        const ShaderPatches::GBufferInfo info = InfoOf(row);
        // alone: only insertions (a ps_2_0 also becomes ps_2_x)
        ShaderPatches::GBufferMade made;
        std::vector<DWORD> alone = code;
        const auto r = ShaderPatches::AddGBufferPs(alone, info, &made);
        if (r == ShaderPatches::GBufferResult::Ok) {
            std::vector<DWORD> stripped = Strip(alone, made);
            const bool onlyInserts = stripped[0] == (v ? 0xFFFF0201u : code[0]) && (stripped[0] = code[0], stripped == code);
            inserted += onlyInserts;
            Check(onlyInserts, "the patch only inserts instructions");
        }
        // merged with the Banding Fix copy, as the game gets it
        std::vector<DWORD> merged = code;
        const bool dithered = (v ? ShaderPatches::AddDither2(merged) : ShaderPatches::AddDither(merged)) == ShaderPatches::DitherResult::Ok;
        const auto rm = dithered ? ShaderPatches::AddGBufferPs(merged, info, &made) : ShaderPatches::GBufferResult::Unreadable;
        const bool ok = rm == ShaderPatches::GBufferResult::Ok, created = ok && gpu.AcceptsPs(merged);
        if (!ok) made = {};
        const char* refusal = dithered ? Reason(rm) : "Banding Fix refused";
        total[v].Add(ok, created, info, made, refusal);
        if (!row) noRow[v].Add(ok, created, info, made, refusal);
        for (const auto& [name, hashes] : families)
            if (hashes.count(hash)) fam[v][name].Add(ok, created, info, made, refusal);
    }
    Check(rows == static_cast<int>(std::size(kGBufferShaders)), "every table row is a pixel shader of the package");
    std::printf("  table rows found %d of %zu; only-insert check passed for %d shaders\n", rows, std::size(kGBufferShaders), inserted);
    for (int v = 0; v < 2; v++) {
        const char* ver = v ? "ps_2_0" : "ps_3_0";
        std::printf("  %s all      %s\n  %s no row   %s\n", ver, total[v].Line().c_str(), ver, noRow[v].Line().c_str());
        // the Banding Fix copy is the carrier: a shader it refuses has no G-buffer either (ps_2_0: 108 of 3,104 in 2.x)
        Check(total[v].accepted * 100 >= total[v].n * (v ? 95 : 99), v ? "ps_2_0: 95 % accepted" : "ps_3_0: 99 % accepted");
        Check(total[v].created == total[v].accepted, "every accepted copy created by the device");
        Check(total[v].albedo == total[v].albedoWanted && total[v].normal == total[v].normalWanted, "the table's albedo and probe reads found");
        std::vector<std::pair<std::string, const Count*>> big; // families with 16 shaders or more of this version, largest first
        for (const auto& [name, c] : fam[v])
            if (c.n >= 16) big.emplace_back(name, &c);
        std::sort(big.begin(), big.end(), [](const auto& a, const auto& b) { return a.second->n > b.second->n; });
        for (const auto& [name, c] : big) std::printf("    %-34s %s\n", name.c_str(), c->Line().c_str());
        if (!families.empty()) std::printf("    (%zu more families with fewer %s shaders)\n", fam[v].size() - big.size(), ver);
    }
}

// ---- 2. the outputs on the GPU ----
void OnGpu(Gpu& gpu, const char* psProfile, const char* vsProfile) {
    const std::vector<DWORD> vs = Compile("float4 main(float4 p : POSITION) : POSITION { return p; }", vsProfile);
    const std::vector<DWORD> ps = Compile("sampler2D sD : register(s0); samplerCUBE sP : register(s1); float4 cN : register(c0);\n"
                                          "float4 main() : COLOR0 { float3 a = tex2D(sD, cN.ww).rgb; float3 p = texCUBE(sP, cN.xyz * 3).rgb;\n"
                                          "  return float4(a * 0.5 + p * 0.25 + 0.125, 0.75); }",
                                          psProfile);
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
    std::printf("  GPU %s: %d directions, worst normal error %.3f degrees\n", psProfile, n, worst);
}

// ---- composited albedo rules on the GPU (GB-04) ----
using RuleClasses = std::map<uint32_t, std::string>; // hash -> class of samplers 0..15 (A, M, L, -)
RuleClasses ReadRuleClasses(const char* path) {
    RuleClasses out;
    std::ifstream f(path);
    std::string h, c;
    while (f >> h >> c) out[static_cast<uint32_t>(std::stoul(h, nullptr, 16))] = c;
    return out;
}

void RulesOnGpu(Gpu& gpu, const std::vector<std::vector<DWORD>>& all, const RuleClasses& classes, const Families& families) {
    IDirect3DDevice9* dev = gpu.dev;
    if (!dev || !gpu.mrt || classes.empty()) return;
    // every input a pixel shader can declare, varying over the quad
    const char* vsSrc = "struct O { float4 p : POSITION; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
                        "  float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7; float4 c0 : COLOR0; float4 c1 : COLOR1; };\n"
                        "O main(float4 p : POSITION) { O o; float4 q = float4(p.xy * 0.5 + 0.5, 0.3, 0.7); o.p = p; o.t0 = q; o.t1 = q.yxzw; o.t2 = q.zwxy;\n"
                        "  o.t3 = q.wzyx; o.t4 = q * 0.5; o.t5 = q.xzyw; o.t6 = q.ywxz; o.t7 = 1 - q; o.c0 = q; o.c1 = q.wzyx; return o; }";
    const std::vector<DWORD> vs3 = Compile(vsSrc, "vs_3_0"), vs2 = Compile(vsSrc, "vs_2_0");
    Check(!vs3.empty() && !vs2.empty(), "rule test vertex shaders compile");
    if (vs3.empty() || vs2.empty()) return;
    // uniform 4x4 textures of each kind: albedo X and Y, neutral material grey, light 1 and 2
    const DWORD kX = 0xFF996633u, kY = 0xFF3380B3u, kM = 0x80808080u, kM2 = 0x40C04020u, kL1 = 0x994DB380u, kL2 = 0x4DE61A33u;
    std::map<std::pair<DWORD, int>, IDirect3DBaseTexture9*> tex; // (colour, kind 2 2D / 3 cube / 4 volume)
    auto Tex = [&](DWORD colour, int kind) -> IDirect3DBaseTexture9* {
        auto& t = tex[{colour, kind}];
        if (t) return t;
        D3DLOCKED_RECT r{};
        D3DLOCKED_BOX b{};
        if (kind == 3) {
            IDirect3DCubeTexture9* c = nullptr;
            if (SUCCEEDED(dev->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &c, nullptr)))
                for (int f = 0; f < 6; f++)
                    if (SUCCEEDED(c->LockRect(static_cast<D3DCUBEMAP_FACES>(f), 0, &r, nullptr, 0))) {
                        for (int y = 0; y < 4; y++) std::fill_n(reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits) + y * r.Pitch), 4, colour);
                        c->UnlockRect(static_cast<D3DCUBEMAP_FACES>(f), 0);
                    }
            t = c;
        } else if (kind == 4) {
            IDirect3DVolumeTexture9* v = nullptr;
            if (SUCCEEDED(dev->CreateVolumeTexture(4, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &v, nullptr)) && SUCCEEDED(v->LockBox(0, &b, nullptr, 0))) {
                for (int z = 0; z < 4; z++)
                    for (int y = 0; y < 4; y++) std::fill_n(reinterpret_cast<DWORD*>(static_cast<char*>(b.pBits) + z * b.SlicePitch + y * b.RowPitch), 4, colour);
                v->UnlockBox(0);
            }
            t = v;
        } else {
            IDirect3DTexture9* p = nullptr;
            if (SUCCEEDED(dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &p, nullptr)) && SUCCEEDED(p->LockRect(0, &r, nullptr, 0))) {
                for (int y = 0; y < 4; y++) std::fill_n(reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits) + y * r.Pitch), 4, colour);
                p->UnlockRect(0);
            }
            t = p;
        }
        return t;
    };
    std::vector<float> consts(224 * 4, 0.5f);
    struct Stat { int n = 0, invariant = 0, sensitive = 0, id = 0, within3 = 0; double sumErr = 0, maxErr = 0; };
    std::map<std::string, Stat> stats;
    auto Family = [&](uint32_t hash) -> std::string {
        for (const char* f : {"SimSkin", "TerrainHigh"}) {
            const auto it = families.find(f);
            if (it != families.end() && it->second.count(hash)) return f;
        }
        return "other";
    };
    for (const auto& code : all) {
        const bool p3 = code[0] == 0xFFFF0300u;
        if (!p3 && code[0] != 0xFFFF0200u) continue;
        const uint32_t hash = ShaderHash(code.data(), code.size() * 4);
        const GBufferShaderId* row = FindGBufferShader(hash);
        const auto cls = classes.find(hash);
        if (!row || cls == classes.end() || !FindGBufferAlbedoRule(hash)) continue;
        std::vector<DWORD> patched = code;
        ShaderPatches::GBufferMade made;
        if (ShaderPatches::AddGBufferPs(patched, InfoOf(row), &made) != ShaderPatches::GBufferResult::Ok || !made.albedo) continue; // (counted in Package)
        int kind[16] = {}; // the sampler declarations: texture type per sampler
        for (size_t i = 1; i < code.size() && code[i] != 0x0000FFFFu;) {
            const DWORD tok = code[i], op = tok & 0xFFFF;
            const size_t len = op == 0xFFFE ? (tok >> 16) & 0x7FFF : (tok >> 24) & 0xF;
            if (op == 0x1F && len == 2 && RegType(code[i + 2]) == 10 && (code[i + 2] & 0x7FF) < 16) kind[code[i + 2] & 0x7FF] = static_cast<int>((code[i + 1] >> 27) & 0xF);
            i += 1 + len;
        }
        auto Draw = [&](DWORD albedo, DWORD light, DWORD out[3], DWORD material = 0x80808080u) {
            IDirect3DVertexShader9* v = nullptr;
            IDirect3DPixelShader9* p = nullptr;
            IDirect3DVertexDeclaration9* decl = nullptr;
            const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
            const auto& vsCode = p3 ? vs3 : vs2;
            bool ok = SUCCEEDED(dev->CreateVertexShader(vsCode.data(), &v)) && SUCCEEDED(dev->CreatePixelShader(patched.data(), &p)) && SUCCEEDED(dev->CreateVertexDeclaration(el, &decl));
            if (ok) {
                for (DWORD i = 0; i < 3; i++) dev->SetRenderTarget(i, gpu.target[i]);
                dev->SetVertexDeclaration(decl);
                dev->SetVertexShader(v);
                dev->SetPixelShader(p);
                dev->SetPixelShaderConstantF(0, consts.data(), p3 ? 224 : 32);
                for (DWORD s = 0; s < 16; s++) {
                    const char c = s < cls->second.size() ? cls->second[s] : '-';
                    dev->SetTexture(s, kind[s] >= 2 ? Tex(c == 'A' ? albedo : c == 'M' ? material : light, kind[s]) : nullptr);
                    dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                    dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                }
                struct V { float x, y, z, w; };
                const V q[4] = {{-1, 1, 0.5f, 1}, {1, 1, 0.5f, 1}, {-1, -1, 0.5f, 1}, {1, -1, 0.5f, 1}};
                dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
                dev->BeginScene();
                ok = SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V)));
                dev->EndScene();
                for (int i = 0; i < 2 && ok; i++) {
                    D3DLOCKED_RECT r{};
                    ok = SUCCEEDED(dev->GetRenderTargetData(gpu.target[i], gpu.readback[i])) && SUCCEEDED(gpu.readback[i]->LockRect(&r, nullptr, D3DLOCK_READONLY));
                    if (!ok) break;
                    std::memcpy(&out[i], static_cast<char*>(r.pBits) + (kSize / 2) * r.Pitch + (kSize / 2) * 4, 4);
                    gpu.readback[i]->UnlockRect();
                }
                for (DWORD i = 1; i < 3; i++) dev->SetRenderTarget(i, nullptr);
                for (DWORD s = 0; s < 16; s++) dev->SetTexture(s, nullptr);
            }
            Release(decl), Release(v), Release(p);
            return ok;
        };
        DWORD a[3] = {}, b[3] = {}, c[3] = {};
        const bool drawn = Draw(kX, kL1, a) && Draw(kX, kL2, b) && Draw(kY, kL1, c);
        Check(drawn, "rule shader draws");
        if (!drawn) continue;
        Stat& s = stats[Family(hash) + (p3 ? " ps_3_0" : " ps_2_0")];
        s.n++;
        s.invariant += (a[1] & 0xFFFFFF) == (b[1] & 0xFFFFFF);
        // a material texture at a blend threshold can hide the albedo (a weight of 0 or 1): then other material values
        DWORD a2[3] = {}, c2[3] = {};
        const bool follows = (a[1] & 0xFFFFFF) != (c[1] & 0xFFFFFF) || (Draw(kX, kL1, a2, kM2) && Draw(kY, kL1, c2, kM2) && (a2[1] & 0xFFFFFF) != (c2[1] & 0xFFFFFF));
        bool followsLow = false; // constant-driven blends (snow, height): at constants 0.5 some weights saturate; at 0.1 the layers show
        if (!follows) {
            std::fill(consts.begin(), consts.end(), 0.1f);
            DWORD a3[3] = {}, c3[3] = {};
            followsLow = Draw(kX, kL1, a3) && Draw(kY, kL1, c3) && (a3[1] & 0xFFFFFF) != (c3[1] & 0xFFFFFF);
            std::fill(consts.begin(), consts.end(), 0.5f);
        }
        s.sensitive += follows || followsLow;
        s.id += (a[1] >> 24) == row->material;
        double err = 0;
        for (int k = 0; k < 3; k++) err = std::max(err, std::fabs(static_cast<double>((a[1] >> (8 * k)) & 255) - ((kX >> (8 * k)) & 255)));
        s.within3 += err <= 3, s.sumErr += err, s.maxErr = std::max(s.maxErr, err);
        if (!follows && !followsLow && s.n - s.sensitive <= 3) std::printf("    rule %08X: target 1 the same with another albedo: %08lX\n", hash, a[1]);
        if ((a[1] & 0xFFFFFF) != (b[1] & 0xFFFFFF) && s.n - s.invariant <= 3) std::printf("    rule %08X: target 1 changed with the light: %08lX / %08lX\n", hash, a[1], b[1]);
    }
    for (const auto& [name, s] : stats) {
        std::printf("  albedo rule %-18s %4d drawn: light-invariant %d, follows the albedo %d, id %d; |target 1 - albedo colour| mean %.1f, max %.0f of 255, within 3: %d\n",
                    name.c_str(), s.n, s.invariant, s.sensitive, s.id, s.n ? s.sumErr / s.n : 0.0, s.maxErr, s.within3);
        Check(s.invariant == s.n, "albedo rule: target 1 does not change with the light-class textures");
        // 94 %: 25 TerrainHigh ps_3_0 leaf-litter passes take their colour from worldLeafMap (material class), not the layers
        Check(s.sensitive * 100 >= s.n * 94, "albedo rule: target 1 follows the albedo-class textures (94 %)");
        Check(s.id == s.n, "albedo rule: target 1 carries the material id");
    }
    for (auto& [k, t] : tex) Release(t);
}

// ---- Ambient Occlusion's G-buffer consumers (GB-04) ----
float Half(unsigned short h) {
    const unsigned e = (h >> 10) & 31, m = h & 1023;
    const float v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : e == 31 ? 65504.0f : std::ldexp(static_cast<float>(m | 1024), static_cast<int>(e) - 25);
    return h & 0x8000 ? -v : v;
}
// a kSize x kSize A32B32G32R32F (or R32F: x only) texture with every mip level filled by f(x, y)
template <class F> IDirect3DTexture9* FloatTexture(IDirect3DDevice9* dev, D3DFORMAT fmt, UINT levels, F f) {
    IDirect3DTexture9* t = nullptr;
    if (FAILED(dev->CreateTexture(kSize, kSize, levels, 0, fmt, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
    for (UINT l = 0; l < t->GetLevelCount(); l++) {
        D3DLOCKED_RECT r{};
        if (FAILED(t->LockRect(l, &r, nullptr, 0))) { Release(t); return nullptr; }
        const int n = kSize >> l;
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                const std::array<float, 4> v = f(x << l, y << l);
                float* p = reinterpret_cast<float*>(static_cast<char*>(r.pBits) + y * r.Pitch);
                if (fmt == D3DFMT_R32F) p[x] = v[0];
                else std::memcpy(p + 4 * x, v.data(), 16);
            }
        t->UnlockRect(l);
    }
    return t;
}
// a full-screen pass of ps (pre-transformed quad, uv 0..1) into an A16B16G16R16F target; the value at pixel (px, py)
bool Pass(IDirect3DDevice9* dev, const std::vector<DWORD>& code, IDirect3DBaseTexture9* const tex[11], const float (*c)[4], UINT nc, int px, int py, float out[4]) {
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DSurface9 *rt = nullptr, *rb = nullptr;
    bool ok = SUCCEEDED(dev->CreatePixelShader(code.data(), &ps)) &&
              SUCCEEDED(dev->CreateRenderTarget(kSize, kSize, D3DFMT_A16B16G16R16F, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr)) &&
              SUCCEEDED(dev->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &rb, nullptr));
    if (ok) {
        dev->SetRenderTarget(0, rt);
        for (DWORD i = 1; i < 3; i++) dev->SetRenderTarget(i, nullptr);
        dev->SetVertexShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->SetPixelShader(ps);
        for (DWORD s = 0; s < 11; s++) {
            dev->SetTexture(s, tex[s]);
            dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
        dev->SetPixelShaderConstantF(0, &c[0][0], nc);
        struct V { float x, y, z, rhw, u, v; };
        const float e = kSize - 0.5f;
        const V q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {e, -0.5f, 0, 1, 1, 0}, {-0.5f, e, 0, 1, 0, 1}, {e, e, 0, 1, 1, 1}};
        dev->BeginScene();
        ok = SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V)));
        dev->EndScene();
        D3DLOCKED_RECT r{};
        ok = ok && SUCCEEDED(dev->GetRenderTargetData(rt, rb)) && SUCCEEDED(rb->LockRect(&r, nullptr, D3DLOCK_READONLY));
        if (ok) {
            const unsigned short* p = reinterpret_cast<const unsigned short*>(static_cast<char*>(r.pBits) + py * r.Pitch) + 4 * px;
            for (int i = 0; i < 4; i++) out[i] = Half(p[i]);
            rb->UnlockRect();
        }
        for (DWORD s = 0; s < 11; s++) dev->SetTexture(s, nullptr);
    }
    Release(rb);
    Release(rt);
    Release(ps);
    return ok;
}

std::vector<DWORD> CompileEntry(const std::string& src, const char* entry) {
    ID3DBlob *code = nullptr, *err = nullptr;
    const HRESULT hr = D3DCompile(src.data(), src.size(), "ambient_occlusion.hlsl", nullptr, nullptr, entry, "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr) && err) std::printf("%s\n", static_cast<const char*>(err->GetBufferPointer()));
    std::vector<DWORD> out;
    if (code) out.assign(static_cast<const DWORD*>(code->GetBufferPointer()), static_cast<const DWORD*>(code->GetBufferPointer()) + code->GetBufferSize() / 4);
    Release(code);
    Release(err);
    return out;
}

void AoOnGpu(Gpu& gpu, const std::string& hlsl) {
    IDirect3DDevice9* dev = gpu.dev;
    if (!dev) return;
    const std::vector<DWORD> gtao = CompileEntry(hlsl, "GtaoPS"), temporal = CompileEntry(hlsl, "TemporalPS");
    Check(!gtao.empty() && !temporal.empty(), "Ambient Occlusion GtaoPS and TemporalPS compile");
    if (gtao.empty() || temporal.empty()) return;
    using A = std::array<float, 4>;
    constexpr int c = kSize / 2; // the pixel read
    // GtaoPS: a wall facing the camera at 5 m (1/z = 0.2 at every pyramid level), tan 0.5, world = view (rows = identity)
    {
        const float W = kSize, tanV = 0.5f;
        float k[18][4] = {
            {tanV, tanV, W / (2 * tanV), 0.3f * W}, {W, W, W, W}, {2.0f * W / 2160.0f, 2.0f, 1.69f, 1.0f / 200.0f}, {0.6f, 2.0f, 2.5f, 750.0f},
            {1.2f, 0.5f, 0.8f, 0.03f}, {20.0f, 1.0f / 20.0f, 1.0f, 0.01f}, {}, {}, {2 * tanV / W, -2 * tanV / W, -tanV, tanV},
            {std::cos(3.14159265f / 8), std::sin(3.14159265f / 8), 0, 0}, {}, {}, {},
            {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {}, {}};
        IDirect3DTexture9* z = FloatTexture(dev, D3DFMT_R32F, 0, [](int, int) { return A{0.2f, 0, 0, 0}; });
        // octahedral (0, 0, -1) is (1, 1); tilted 60 degrees up: (0, 0.866, -0.5) folds to (0.366, 1) -> stored (0.683, 1)
        IDirect3DTexture9* facing = FloatTexture(dev, D3DFMT_A32B32G32R32F, 1, [](int, int) { return A{1, 1, 0.5f, 0}; });
        IDirect3DTexture9* tilted = FloatTexture(dev, D3DFMT_A32B32G32R32F, 1, [](int, int) { return A{0.683f, 1, 0.5f, 0}; });
        Check(z && facing && tilted, "AO test textures");
        if (z && facing && tilted) {
            IDirect3DBaseTexture9* t[11] = {nullptr, z, z, nullptr, nullptr, nullptr, nullptr, facing};
            float depthN[4] = {}, gbN[4] = {}, tiltN[4] = {}, off[4] = {};
            k[13][3] = 0;
            bool ok = Pass(dev, gtao, t, k, 18, c, c, depthN);
            k[13][3] = 1;
            ok = ok && Pass(dev, gtao, t, k, 18, c, c, gbN);
            t[7] = tilted;
            ok = ok && Pass(dev, gtao, t, k, 18, c, c, tiltN);
            k[13][3] = 0;
            ok = ok && Pass(dev, gtao, t, k, 18, c, c, off);
            Check(ok, "GtaoPS draws");
            std::printf("  GtaoPS wall: depth normal %.4f, G-buffer normal %.4f, tilted G-buffer normal %.4f, switch off %.4f\n", depthN[0], gbN[0], tiltN[0], off[0]);
            Check(depthN[0] > 0.999f && gbN[0] > 0.999f, "a flat wall is unshaded with the depth normal and with the G-buffer normal");
            Check(tiltN[0] < 0.95f, "a tilted G-buffer normal is used (the wall cuts its hemisphere: shade)");
            Check(off[0] == depthN[0], "switch off: the G-buffer normal is ignored");
        }
        Release(z), Release(facing), Release(tilted);
    }
    // TemporalPS: shade 0.4 / 0.6 in a checkerboard (3x3 mean 0.4 + 0.2 * 4 / 9 or 0.6 - 0.2 * 4 / 9, sigma 0.0994), 1/z 0.2,
    // id 7, device depth 0.9; the history 0.5 everywhere except column c - 4 (0.45), frames 3
    {
        auto shadeAt = [](int x, int y) { return ((x + y) & 1) ? 0.6f : 0.4f; };
        const float id = 7.0f / 255.0f;
        IDirect3DTexture9* ao = FloatTexture(dev, D3DFMT_A32B32G32R32F, 1, [&](int x, int y) { return A{shadeAt(x, y), 0.2f, 0, 0}; });
        IDirect3DTexture9* depth = FloatTexture(dev, D3DFMT_R32F, 1, [](int, int) { return A{0.9f, 0, 0, 0}; });
        IDirect3DTexture9* albedo = FloatTexture(dev, D3DFMT_A32B32G32R32F, 1, [&](int, int) { return A{0.5f, 0.5f, 0.5f, id}; });
        auto normal = [&](float cls) { return FloatTexture(dev, D3DFMT_A32B32G32R32F, 1, [=](int, int) { return A{0.5f, 0.5f, 0.5f, cls / 3}; }); };
        auto history = [&](float wz, float hid, float frames, float rest) {
            return FloatTexture(dev, D3DFMT_A32B32G32R32F, 1, [=](int x, int) { return A{x == c - 4 ? 0.45f : rest, wz, hid, frames}; });
        };
        auto motion = [&](float mx) { return FloatTexture(dev, D3DFMT_A32B32G32R32F, 1, [=](int, int) { return A{mx, 0, 0, 0}; }); };
        IDirect3DTexture9 *n0 = normal(0), *n3 = normal(3), *h = history(0.2f, id, 3, 0.5f), *hOut = history(0.2f, id, 3, 0.0f),
                          *hId = history(0.2f, 8.0f / 255.0f, 3, 0.5f), *hFar = history(0.2f / 1.2f, id, 3, 0.5f), *hRatio = history(0.2f / 1.25f, id, 3, 0.5f),
                          *still = motion(0), *shift = motion(8.0f / kSize), *bad = motion(32768.0f);
        Check(ao && depth && albedo && n0 && n3 && h && hOut && hId && hFar && hRatio && still && shift && bad, "temporal test textures");
        if (ao && depth && albedo && n0 && n3 && h && hOut && hId && hFar && hRatio && still && shift && bad) {
            const float cur = shadeAt(c, c), mean = cur == 0.4f ? 0.4f + 0.2f * 4 / 9 : 0.6f - 0.2f * 4 / 9;
            const float sigma = std::sqrt(20.0f / 81.0f) * 0.2f; // checkerboard 3x3: 5 of one value, 4 of the other
            struct Case { const char* what; IDirect3DTexture9 *hist, *mv, *gbN; float valid, ratio, want, frames; };
            const Case cases[] = {
                {"history blended by 1 / frames", h, still, n0, 1, 1, 0.5f + (cur - 0.5f) / 4, 4},
                {"history followed along the motion vector", h, shift, n0, 1, 1, 0.45f + (cur - 0.45f) / 4, 4},
                {"history clipped to the neighbourhood", hOut, still, n0, 1, 1, (mean - 1.5f * sigma) + (cur - (mean - 1.5f * sigma)) / 4, 4},
                {"depth explained by the camera's depth ratio kept", hRatio, still, n0, 1, 1.25f, 0.5f + (cur - 0.5f) / 4, 4},
                {"another material id: new history", hId, still, n0, 1, 1, cur, 1},
                {"motion class 3: new history", h, still, n3, 1, 1, cur, 1},
                {"1/z jump: new history", hFar, still, n0, 1, 1, cur, 1},
                {"invalid motion: new history", h, bad, n0, 1, 1, cur, 1},
                {"no history this frame: new history", h, still, n0, 0, 1, cur, 1}};
            for (const Case& k : cases) {
                IDirect3DBaseTexture9* t[11] = {depth, nullptr, nullptr, ao, nullptr, nullptr, nullptr, k.gbN, k.hist, k.mv, albedo};
                const float cs[18][4] = {{}, {kSize, kSize, kSize, kSize}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {k.valid, 0.05f, 8, 1.5f}, {0, 0, 0, k.ratio}};
                float out[4] = {};
                const bool ok = Pass(dev, temporal, t, cs, 18, c, c, out);
                const bool good = ok && std::fabs(out[0] - k.want) < 2e-3f && out[3] == k.frames && std::fabs(out[1] - 0.2f) < 1e-3f && std::fabs(out[2] - id) < 1e-3f;
                if (!good) std::printf("  TemporalPS %s: got %.4f frames %.0f, want %.4f frames %.0f\n", k.what, out[0], out[3], k.want, k.frames);
                Check(good, k.what);
            }
            std::printf("  TemporalPS: %zu cases\n", std::size(cases));
        }
        for (IDirect3DTexture9* t : {ao, depth, albedo, n0, n3, h, hOut, hId, hFar, hRatio, still, shift, bad}) Release(t);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // a crash keeps what was printed
    if (argc < 2) {
        std::puts("usage: check <Shaders_Win32.precomp> [--families <file>] [--d3d9 <d3d9.dll>]");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    const std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), {});
    const auto all = LoadUnique(d);
    std::printf("%zu unique shaders in %s\n", all.size(), argv[1]);
    Check(!all.empty(), "shader package read");
    Families families;
    std::string aoHlsl;
    RuleClasses ruleClasses;
    for (int a = 2; a + 1 < argc; a++) {
        if (std::strcmp(argv[a], "--families") == 0) families = ReadFamilies(argv[a + 1]);
        if (std::strcmp(argv[a], "--rules") == 0) ruleClasses = ReadRuleClasses(argv[a + 1]);
        if (std::strcmp(argv[a], "--ao") == 0) {
            std::ifstream h(argv[a + 1], std::ios::binary);
            aoHlsl.assign(std::istreambuf_iterator<char>(h), {});
            Check(!aoHlsl.empty(), "Ambient Occlusion HLSL read");
        }
    }
    {
        std::puts("Native D3D9");
        Gpu gpu(LoadLibraryW(L"d3d9.dll"));
        Package(gpu, all, families);
        OnGpu(gpu, "ps_3_0", "vs_3_0");
        OnGpu(gpu, "ps_2_0", "vs_2_0");
        RulesOnGpu(gpu, all, ruleClasses, families);
        if (!aoHlsl.empty()) AoOnGpu(gpu, aoHlsl);
    }
    for (int a = 2; a + 1 < argc; a++) {
        if (std::strcmp(argv[a], "--d3d9") != 0) continue;
        HMODULE other = LoadLibraryA(argv[a + 1]);
        Check(other != nullptr, "the other d3d9.dll loads");
        if (!other) continue;
        std::printf("%s\n", argv[a + 1]);
        Gpu gpu(other);
        Package(gpu, all, families);
        OnGpu(gpu, "ps_3_0", "vs_3_0");
        OnGpu(gpu, "ps_2_0", "vs_2_0");
        RulesOnGpu(gpu, all, ruleClasses, families);
        if (!aoHlsl.empty()) AoOnGpu(gpu, aoHlsl);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
