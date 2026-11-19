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
#include "../../features/gbuffer_motion.h"
#include "../../features/gbuffer_movers.h"
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
    struct Stat { int n = 0, invariant = 0, sensitive = 0, leaf = 0, id = 0, within3 = 0; double sumErr = 0, maxErr = 0; };
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
        auto Draw = [&](DWORD albedo, DWORD light, DWORD out[3], DWORD material = 0x80808080u, DWORD leaf = 0) {
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
                    dev->SetTexture(s, kind[s] >= 2 ? Tex(c == 'A' ? albedo : c == 'F' ? (leaf ? leaf : material) : c == 'M' ? material : light, kind[s]) : nullptr);
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
        // (GB-05) passes whose albedo is the leaf litter (worldLeafMap, class F): target 1 follows that layer instead
        bool followsLeaf = false;
        if (!follows && !followsLow && cls->second.find('F') != std::string::npos) {
            DWORD a4[3] = {}, c4[3] = {};
            followsLeaf = Draw(kX, kL1, a4, kM, kX) && Draw(kX, kL1, c4, kM, kY) && (a4[1] & 0xFFFFFF) != (c4[1] & 0xFFFFFF);
            if (followsLeaf && s.leaf < 3) std::printf("    rule %08X: target 1 follows the leaf litter: %08lX / %08lX (layer %08lX / %08lX)\n", hash, a4[1], c4[1], kX, kY);
        }
        s.sensitive += follows || followsLow || followsLeaf;
        s.leaf += followsLeaf;
        s.id += (a[1] >> 24) == row->material;
        double err = 0;
        for (int k = 0; k < 3; k++) err = std::max(err, std::fabs(static_cast<double>((a[1] >> (8 * k)) & 255) - ((kX >> (8 * k)) & 255)));
        s.within3 += err <= 3, s.sumErr += err, s.maxErr = std::max(s.maxErr, err);
        if (!follows && !followsLow && !followsLeaf && s.n - s.sensitive <= 3) std::printf("    rule %08X: target 1 the same with another albedo: %08lX\n", hash, a[1]);
        if ((a[1] & 0xFFFFFF) != (b[1] & 0xFFFFFF) && s.n - s.invariant <= 3) std::printf("    rule %08X: target 1 changed with the light: %08lX / %08lX\n", hash, a[1], b[1]);
    }
    for (const auto& [name, s] : stats) {
        std::printf("  albedo rule %-18s %4d drawn: light-invariant %d, follows the albedo %d (the leaf litter %d), id %d; |target 1 - albedo colour| mean %.1f, max %.0f of 255, within 3: %d\n",
                    name.c_str(), s.n, s.invariant, s.sensitive, s.leaf, s.id, s.n ? s.sumErr / s.n : 0.0, s.maxErr, s.within3);
        Check(s.invariant == s.n, "albedo rule: target 1 does not change with the light-class textures");
        // (GB-05) the 25 TerrainHigh ps_3_0 leaf-litter passes follow WorldLeavesTexture (the leaf colour, material class)
        Check(s.sensitive == s.n, "albedo rule: target 1 follows the albedo-class textures (or the leaf-litter colour)");
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
        float k[19][4] = {
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
            bool ok = Pass(dev, gtao, t, k, 19, c, c, depthN);
            k[13][3] = 1;
            ok = ok && Pass(dev, gtao, t, k, 19, c, c, gbN);
            t[7] = tilted;
            ok = ok && Pass(dev, gtao, t, k, 19, c, c, tiltN);
            k[13][3] = 0;
            ok = ok && Pass(dev, gtao, t, k, 19, c, c, off);
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
                          *still = motion(0), *shift = motion(8.0f / kSize), *bad = motion(32768.0f),
                          *mover = FloatTexture(dev, D3DFMT_A32B32G32R32F, 1, [](int, int) { return A{0, 0, 1, 0}; });
        Check(ao && depth && albedo && n0 && n3 && h && hOut && hId && hFar && hRatio && still && shift && bad && mover, "temporal test textures");
        if (ao && depth && albedo && n0 && n3 && h && hOut && hId && hFar && hRatio && still && shift && bad && mover) {
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
                {"a mover's own vector (GB-05): class 1, history kept", h, mover, n3, 1, 1, 0.5f + (cur - 0.5f) / 4, 4},
                {"1/z jump: new history", hFar, still, n0, 1, 1, cur, 1},
                {"invalid motion: new history", h, bad, n0, 1, 1, cur, 1},
                {"no history this frame: new history", h, still, n0, 0, 1, cur, 1}};
            for (const Case& k : cases) {
                IDirect3DBaseTexture9* t[11] = {depth, nullptr, nullptr, ao, nullptr, nullptr, nullptr, k.gbN, k.hist, k.mv, albedo};
                const float cs[19][4] = {{}, {kSize, kSize, kSize, kSize}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {k.valid, 0.05f, 8, 1.5f}, {0, 0, 0, k.ratio}, {}};
                float out[4] = {};
                const bool ok = Pass(dev, temporal, t, cs, 19, c, c, out);
                const bool good = ok && std::fabs(out[0] - k.want) < 2e-3f && out[3] == k.frames && std::fabs(out[1] - 0.2f) < 1e-3f && std::fabs(out[2] - id) < 1e-3f;
                if (!good) std::printf("  TemporalPS %s: got %.4f frames %.0f, want %.4f frames %.0f\n", k.what, out[0], out[3], k.want, k.frames);
                Check(good, k.what);
            }
            std::printf("  TemporalPS: %zu cases\n", std::size(cases));
        }
        for (IDirect3DTexture9* t : {ao, depth, albedo, n0, n3, h, hOut, hId, hFar, hRatio, still, shift, bad, mover}) Release(t);
    }
}


// ---- 5. mover motion vectors (GB-05): the position slice and its replay shader ----
// Every vertex shader of the package through AnalyzeMotionSlice / MakeMotionReplayVs (probe form: one point per vertex at a
// chosen pixel, TEXCOORD0 / 1 into two A32B32G32R32F targets), 64 vertices with deterministic inputs (blend indices 0..3)
// and deterministic constants:
//   exact: slice A's position equals the whole program's (every instruction kept but the other outputs' writes);
//   copy: bank B uploaded with this frame's values gives bank A's position;
//   follows: bank B uploaded with another set gives the position slice A computes with that set.
constexpr int kProbeVerts = 64;
struct ProbeOut {
    std::vector<std::array<float, 4>> a, b;
};
float Det(uint32_t seed) { // deterministic in [-1, 1]
    seed = seed * 747796405u + 2891336453u;
    seed = ((seed >> ((seed >> 28) + 4)) ^ seed) * 277803737u;
    return static_cast<float>((seed >> 22) ^ seed) / 4294967295.0f * 2.0f - 1.0f;
}
bool Probe(IDirect3DDevice9* dev, const std::vector<DWORD>& vsCode, const std::vector<DWORD>& psCode, const std::vector<DWORD>& game, const float (*constA)[4],
           const std::vector<std::pair<int, std::array<float, 4>>>& upload, ProbeOut& out) {
    // the declaration from the game shader's input dcls (every input FLOAT4), plus the probe position at TEXCOORD15
    std::vector<D3DVERTEXELEMENT9> el;
    std::vector<int> inputs; // per element: usage
    for (size_t i = 1; i < game.size();) {
        const DWORD tok = game[i];
        if (tok == 0x0000FFFFu) break;
        if ((tok & 0xFFFF) == 0xFFFE) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }
        const size_t len = (tok >> 24) & 0xF;
        if ((tok & 0xFFFF) == 0x1F && len == 2 && RegType(game[i + 2]) == 1) {
            const DWORD u = game[i + 1];
            el.push_back({0, static_cast<WORD>(16 * el.size()), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, static_cast<BYTE>(u & 0x1F), static_cast<BYTE>((u >> 16) & 0xF)});
            inputs.push_back(static_cast<int>(u & 0x1F));
        }
        i += 1 + len;
    }
    const size_t nIn = el.size();
    el.push_back({0, static_cast<WORD>(16 * nIn), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 15});
    el.push_back(D3DDECL_END());
    std::vector<float> vb(kProbeVerts * (nIn + 1) * 4);
    for (int v = 0; v < kProbeVerts; v++)
        for (size_t k = 0; k <= nIn; k++) {
            float* p = &vb[(v * (nIn + 1) + k) * 4];
            if (k == nIn) { // pixel (v % 8, v / 8)
                p[0] = 2.0f * (v % 8) / kSize - 1, p[1] = 1 - 2.0f * (v / 8) / kSize, p[2] = 0.5f, p[3] = 1;
                continue;
            }
            // small and >= 0: an index the shader computes from an input (instance number in normal.w x 255) stays in its array
            for (int c = 0; c < 4; c++) p[c] = 0.05f * (1 + Det(static_cast<uint32_t>(v * 131 + k * 17 + c)));
            if (inputs[k] == D3DDECLUSAGE_BLENDINDICES)
                for (int c = 0; c < 4; c++) p[c] = static_cast<float>((v + c) & 3);
            else if (inputs[k] == D3DDECLUSAGE_BLENDWEIGHT)
                for (int c = 0; c < 4; c++) p[c] = 0.25f;
            else if (inputs[k] == D3DDECLUSAGE_POSITION) p[3] = 1;
        }
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    IDirect3DSurface9 *rt[2] = {}, *rb[2] = {};
    bool ok = SUCCEEDED(dev->CreateVertexShader(vsCode.data(), &vs)) && SUCCEEDED(dev->CreatePixelShader(psCode.data(), &ps)) &&
              SUCCEEDED(dev->CreateVertexDeclaration(el.data(), &decl));
    for (int i = 0; i < 2 && ok; i++)
        ok = SUCCEEDED(dev->CreateRenderTarget(kSize, kSize, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &rt[i], nullptr)) &&
             SUCCEEDED(dev->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &rb[i], nullptr));
    if (ok) {
        dev->SetRenderTarget(0, rt[0]);
        dev->SetRenderTarget(1, rt[1]);
        dev->SetRenderTarget(2, nullptr);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
        dev->SetVertexDeclaration(decl);
        dev->SetVertexShader(vs);
        dev->SetPixelShader(ps);
        dev->SetVertexShaderConstantF(0, &constA[0][0], 256);
        for (const auto& [reg, v] : upload) dev->SetVertexShaderConstantF(static_cast<UINT>(reg), v.data(), 1);
        const BOOL b[16] = {TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE};
        dev->SetVertexShaderConstantB(0, b, 16);
        int ic[16][4];
        for (auto& q : ic) q[0] = 2, q[1] = 0, q[2] = 1, q[3] = 0;
        dev->SetVertexShaderConstantI(0, &ic[0][0], 16);
        dev->BeginScene();
        ok = SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_POINTLIST, kProbeVerts, vb.data(), static_cast<UINT>((nIn + 1) * 16)));
        dev->EndScene();
        for (int i = 0; i < 2 && ok; i++) {
            D3DLOCKED_RECT r{};
            ok = SUCCEEDED(dev->GetRenderTargetData(rt[i], rb[i])) && SUCCEEDED(rb[i]->LockRect(&r, nullptr, D3DLOCK_READONLY));
            if (!ok) break;
            auto& dst = i ? out.b : out.a;
            dst.resize(kProbeVerts);
            for (int v = 0; v < kProbeVerts; v++) std::memcpy(dst[v].data(), static_cast<char*>(r.pBits) + (v / 8) * r.Pitch + (v % 8) * 16, 16);
            rb[i]->UnlockRect();
        }
        dev->SetRenderTarget(1, nullptr);
        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
    }
    for (int i = 0; i < 2; i++) Release(rt[i]), Release(rb[i]);
    Release(decl), Release(vs), Release(ps);
    return ok;
}
// a and b agree: bit for bit, or within 1e-5 relative where a driver rounds differently; both not finite also agrees
bool Same(const std::array<float, 4>& a, const std::array<float, 4>& b, bool* exact) {
    bool same = true;
    for (int k = 0; k < 4; k++) {
        if (std::memcmp(&a[k], &b[k], 4) != 0) *exact = false;
        if (!std::isfinite(a[k]) && !std::isfinite(b[k])) continue;
        same &= std::fabs(a[k] - b[k]) <= 1e-5f * std::max(1.0f, std::fabs(a[k]));
    }
    return same;
}
const char* SliceReason(ShaderPatches::MotionSliceResult r) {
    static const char* names[] = {"ok", "not vertex", "unreadable", "version", "subroutines", "no position", "pass-through", "predicated", "relative output",
                                  "unsupported", "no free temp", "no room"};
    return names[static_cast<int>(r)];
}

void MoversOnGpu(Gpu& gpu, const std::vector<std::vector<DWORD>>& all, int limit) {
    IDirect3DDevice9* dev = gpu.dev;
    if (!dev) return;
    const std::vector<DWORD> ps = Compile("float4 c0 : COLOR0; struct O { float4 a : COLOR0; float4 b : COLOR1; };"
                                          "O main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) { O o; o.a = a; o.b = b; return o; }",
                                          "ps_3_0");
    Check(!ps.empty(), "probe pixel shader compiles");
    if (ps.empty()) return;
    static float A[256][4], B[256][4];
    for (int r = 0; r < 256; r++)
        for (int c = 0; c < 4; c++) A[r][c] = 0.5f + 0.5f * Det(r * 4 + c + 1000), B[r][c] = A[r][c] + 0.0625f * Det(r * 4 + c + 9000); // >= 0: computed indices stay in their arrays
    struct Count { int n = 0, ok = 0, created = 0, tested = 0, exact = 0, exactBits = 0, copy = 0, follows = 0, skinned = 0, keptSum = 0, insSum = 0; std::map<std::string, int> why; };
    Count v[2];
    int maxBank = 0, fits30 = 0, regions = 0;
    for (const auto& code : all) {
        if ((code[0] & 0xFFFF0000u) != 0xFFFE0000u) continue;
        const int ver = ((code[0] >> 8) & 0xFF) == 3 ? 0 : 1;
        Count& c = v[ver];
        c.n++;
        ShaderPatches::MotionSlice s;
        auto r = ShaderPatches::AnalyzeMotionSlice(code, &s);
        // 90 registers of indexed arrays in all (30 bones of 3 rows), shared by the arrays a shader indexes (bones, instances)
        std::vector<int> ext;
        for (const auto& g : s.regions) ext.push_back(std::min(g.maxExtent, std::max(g.lastOffset + 1, 90 / static_cast<int>(s.regions.size()))));
        ShaderPatches::MotionReplayLayout L;
        std::vector<DWORD> replay;
        if (r == ShaderPatches::MotionSliceResult::Ok) r = ShaderPatches::MakeMotionReplayVs(s, ext, &L, &replay, false); // the game's form
        if (r != ShaderPatches::MotionSliceResult::Ok) { if (c.why[SliceReason(r)]++ < 2) std::printf("    vs %08X refused: %s\n", ShaderHash(code.data(), code.size() * 4), SliceReason(r)); continue; }
        c.ok++;
        regions += !s.regions.empty();
        fits30 += !s.regions.empty();
        int bank = static_cast<int>(s.singles.size());
        for (int e : L.extents) bank += e;
        maxBank = std::max(maxBank, bank);
        IDirect3DVertexShader9* made = nullptr;
        const bool created = SUCCEEDED(dev->CreateVertexShader(replay.data(), &made));
        Release(made);
        c.created += created;
        if (!created && c.ok - c.created <= 3) std::printf("    vs %08X: replay not created (%zu tokens)\n", ShaderHash(code.data(), code.size() * 4), replay.size());
        if (!created || (limit && c.tested >= limit)) continue;
        if (ShaderPatches::MakeMotionReplayVs(s, ext, &L, &replay, true) != ShaderPatches::MotionSliceResult::Ok) { // the probe form
            c.why["probe: 16 inputs declared"]++;
            continue;
        }
        // the whole program (every instruction but the other outputs' writes): its position is the reference
        ShaderPatches::MotionSlice whole = s;
        int ins = 0;
        {
            DWORD posType = 4, posNum = 0; // oPos; vs_3_0: the output declared POSITION0
            for (size_t i = 1; i < code.size();) {
                const DWORD tok = code[i];
                if (tok == 0x0000FFFFu) break;
                if ((tok & 0xFFFF) == 0xFFFE) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }
                const size_t len = (tok >> 24) & 0xF;
                if ((tok & 0xFFFF) == 0x1F && len == 2 && RegType(code[i + 2]) == 6 && (code[i + 1] & 0x1F) == 0 && ((code[i + 1] >> 16) & 0xF) == 0)
                    posType = 6, posNum = code[i + 2] & 0x7FF;
                i += 1 + len;
            }
            size_t idx = 0;
            for (size_t i = 1; i < code.size();) {
                const DWORD tok = code[i];
                if (tok == 0x0000FFFFu) break;
                if ((tok & 0xFFFF) == 0xFFFE) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }
                const size_t len = (tok >> 24) & 0xF;
                const DWORD op = tok & 0xFFFF;
                if (op != 0x1F && op != 0x51 && op != 0x30 && op != 0x2F) {
                    ins++;
                    const bool control = op == 0 || op == 0x1B || op == 0x1D || (op >= 0x26 && op <= 0x2D) || op == 0x60;
                    const DWORD d = len ? code[i + 1] : 0;
                    const DWORD type = RegType(d);
                    const bool output = !control && len && (type == 4 || type == 5 || type == 6);
                    const bool pos = output && type == posType && (d & 0x7FF) == posNum;
                    whole.keep[idx] = output && !pos ? 0 : 1;
                }
                idx++;
                i += 1 + len;
            }
        }
        c.keptSum += s.kept, c.insSum += ins;
        std::vector<DWORD> wholeVs;
        ShaderPatches::MotionReplayLayout WL;
        if (ShaderPatches::MakeMotionReplayVs(whole, ext, &WL, &wholeVs, true) != ShaderPatches::MotionSliceResult::Ok) { c.why["whole program no room"]++; continue; }
        // bank B uploads: this frame's values (copy) or the other set's (follows)
        auto uploads = [&](const ShaderPatches::MotionReplayLayout& lay, const float (*src)[4]) {
            std::vector<std::pair<int, std::array<float, 4>>> u;
            for (size_t i = 0; i < s.singles.size(); i++) u.push_back({lay.singleTo[i], {src[s.singles[i]][0], src[s.singles[i]][1], src[s.singles[i]][2], src[s.singles[i]][3]}});
            for (size_t i = 0; i < s.regions.size(); i++)
                for (int k = 0; k < lay.extents[i]; k++) {
                    const int from = s.regions[i].base + k;
                    u.push_back({lay.regionTo[i] + k, {src[from][0], src[from][1], src[from][2], src[from][3]}});
                }
            u.push_back({lay.jitter, {0, 0, 0, 0}});
            return u;
        };
        ProbeOut slice, wholeOut, copy, follows, ref;
        bool ok = Probe(dev, replay, ps, code, A, uploads(L, A), copy) && Probe(dev, wholeVs, ps, code, A, uploads(WL, A), wholeOut) &&
                  Probe(dev, replay, ps, code, A, uploads(L, B), follows) && Probe(dev, replay, ps, code, B, uploads(L, B), ref);
        if (!ok) {
            if (c.why["probe draw failed"]++ < 2) std::printf("    vs %08X: probe draw failed\n", ShaderHash(code.data(), code.size() * 4));
            continue;
        }
        c.tested++;
        bool exact = true, e1 = true, e2 = true, e3 = true;
        bool sameWhole = true, sameCopy = true, sameFollows = true;
        for (int k = 0; k < kProbeVerts; k++) {
            sameWhole &= Same(copy.a[k], wholeOut.a[k], &exact);
            sameCopy &= Same(copy.b[k], copy.a[k], &e1);
            sameFollows &= Same(follows.b[k], ref.a[k], &e2);
            sameFollows &= Same(follows.a[k], copy.a[k], &e3);
        }
        c.exact += sameWhole, c.exactBits += exact, c.copy += sameCopy, c.follows += sameFollows;
        c.skinned += !s.regions.empty();
        if ((!sameWhole || !sameCopy || !sameFollows) && c.n - c.exact < 6)
            std::printf("    vs %08X: slice = whole %d, copy %d, follows %d (kept %d of %d)\n", ShaderHash(code.data(), code.size() * 4), sameWhole, sameCopy, sameFollows, s.kept, ins);
    }
    for (int k = 0; k < 2; k++) {
        const Count& c = v[k];
        std::string w;
        for (const auto& [name, n] : c.why) w += " " + name + " " + std::to_string(n) + ";";
        std::printf("  movers %s: %d shaders, replay made %d, created %d; probed %d (indexed arrays %d): slice = whole program %d (bit for bit %d), bank B copy %d, "
                    "bank B follows %d; slice keeps %.0f %% of the instructions%s%s\n",
                    k ? "vs_2_x" : "vs_3_0", c.n, c.ok, c.created, c.tested, c.skinned, c.exact, c.exactBits, c.copy, c.follows,
                    c.insSum ? 100.0 * c.keptSum / c.insSum : 0.0, w.empty() ? "" : " | refused:", w.c_str());
        Check(c.created == c.ok, "movers: every replay shader created");
        Check(c.ok * 100 >= c.n * 93, "movers: 93 % of the vertex shaders have a replay");
        Check(c.exact == c.tested, "movers: the slice's position is the whole program's");
        Check(c.copy == c.tested, "movers: bank B with this frame's values gives bank A's position");
        // (an index the shader computes from a constant may leave its array when only bank B's set changes: 1 of 400)
        Check(c.follows * 100 >= c.tested * 99, "movers: bank B follows its own constants (99 %)");
    }
    std::printf("  movers: %d shaders read an indexed array (bones), the largest bank B with 30 bones %d registers\n", regions, maxBank);
}

// ---- 6. mover scenes and temporal AA (GB-05): whole 64 x 64 images on the GPU ----
constexpr float GBufferDepthMatch = GBufferMovers::kDepthMatch;
using Img = std::vector<std::array<float, 4>>; // kSize x kSize, row by row

unsigned short ToHalf(float f) { // round to nearest; the test values stay in the normal range
    uint32_t u;
    std::memcpy(&u, &f, 4);
    const uint32_t sign = (u >> 16) & 0x8000u;
    const int e = static_cast<int>((u >> 23) & 0xFF) - 127 + 15;
    if ((u & 0x7FFFFFFFu) == 0 || e <= 0) return static_cast<unsigned short>(sign);
    if (e >= 31) return static_cast<unsigned short>(sign | 0x7BFFu);
    const uint32_t m = (u & 0x7FFFFFu) + 0x1000u; // + half of the dropped part
    return static_cast<unsigned short>(sign | ((static_cast<uint32_t>(e) << 10) + (m >> 13)));
}

struct Rt {
    IDirect3DTexture9* tex = nullptr;
    IDirect3DSurface9* surf = nullptr;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    bool Make(IDirect3DDevice9* dev, D3DFORMAT f) {
        fmt = f;
        return SUCCEEDED(dev->CreateTexture(kSize, kSize, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, &tex, nullptr)) && SUCCEEDED(tex->GetSurfaceLevel(0, &surf));
    }
    ~Rt() { Release(surf), Release(tex); }
};

bool ReadImg(IDirect3DDevice9* dev, const Rt& rt, Img& out) {
    IDirect3DSurface9* rb = nullptr;
    D3DLOCKED_RECT r{};
    bool ok = SUCCEEDED(dev->CreateOffscreenPlainSurface(kSize, kSize, rt.fmt, D3DPOOL_SYSTEMMEM, &rb, nullptr)) && SUCCEEDED(dev->GetRenderTargetData(rt.surf, rb)) &&
              SUCCEEDED(rb->LockRect(&r, nullptr, D3DLOCK_READONLY));
    if (ok) {
        out.assign(kSize * kSize, {});
        for (int y = 0; y < kSize; y++)
            for (int x = 0; x < kSize; x++) {
                const char* row = static_cast<const char*>(r.pBits) + y * r.Pitch;
                auto& o = out[y * kSize + x];
                if (rt.fmt == D3DFMT_A32B32G32R32F) std::memcpy(o.data(), row + 16 * x, 16);
                else
                    for (int k = 0; k < 4; k++) o[k] = Half(reinterpret_cast<const unsigned short*>(row)[4 * x + k]);
            }
        rb->UnlockRect();
    }
    Release(rb);
    return ok;
}

// a managed A16B16G16R16F texture (filterable everywhere) holding img
IDirect3DTexture9* HalfTexture(IDirect3DDevice9* dev, const Img& img) {
    IDirect3DTexture9* t = nullptr;
    D3DLOCKED_RECT r{};
    if (FAILED(dev->CreateTexture(kSize, kSize, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_MANAGED, &t, nullptr)) || FAILED(t->LockRect(0, &r, nullptr, 0))) {
        Release(t);
        return nullptr;
    }
    for (int y = 0; y < kSize; y++)
        for (int x = 0; x < kSize; x++)
            for (int k = 0; k < 4; k++) reinterpret_cast<unsigned short*>(static_cast<char*>(r.pBits) + y * r.Pitch)[4 * x + k] = ToHalf(img[y * kSize + x][k]);
    t->UnlockRect(0);
    return t;
}

// a full-screen pass of ps into rt0 (and rt1); tex[s] bilinear where bit s of linear is set; writeMask: rt0's channels
bool FullPass(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps, IDirect3DBaseTexture9* const tex[8], unsigned linear, const float (*c)[4], UINT nc,
              IDirect3DSurface9* rt0, IDirect3DSurface9* rt1, DWORD writeMask = 0xF) {
    dev->SetRenderTarget(0, rt0);
    dev->SetRenderTarget(1, rt1);
    dev->SetRenderTarget(2, nullptr);
    dev->SetDepthStencilSurface(nullptr);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, writeMask);
    dev->SetVertexShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetPixelShader(ps);
    for (DWORD s = 0; s < 8; s++) {
        dev->SetTexture(s, tex[s]);
        const DWORD f = (linear >> s) & 1 ? D3DTEXF_LINEAR : D3DTEXF_POINT;
        dev->SetSamplerState(s, D3DSAMP_MINFILTER, f);
        dev->SetSamplerState(s, D3DSAMP_MAGFILTER, f);
        dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    dev->SetPixelShaderConstantF(0, &c[0][0], nc);
    struct V { float x, y, z, rhw, u, v; };
    const float e = kSize - 0.5f;
    const V q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {e, -0.5f, 0, 1, 1, 0}, {-0.5f, e, 0, 1, 0, 1}, {e, e, 0, 1, 1, 1}};
    dev->BeginScene();
    const bool ok = SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V)));
    dev->EndScene();
    for (DWORD s = 0; s < 8; s++) dev->SetTexture(s, nullptr);
    dev->SetRenderTarget(1, nullptr);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    return ok;
}

IDirect3DPixelShader9* MakePs(IDirect3DDevice9* dev, const std::vector<DWORD>& code) {
    IDirect3DPixelShader9* ps = nullptr;
    if (code.empty() || FAILED(dev->CreatePixelShader(code.data(), &ps))) return nullptr;
    return ps;
}

// 3 x 3 inverse (row major); false when singular
bool Inverse3(const double m[3][3], double o[3][3]) {
    const double d = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::fabs(d) < 1e-300) return false;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            const int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
            o[r][c] = (m[r1][c1] * m[r2][c2] - m[r1][c2] * m[r2][c1]) / d;
        }
    return true;
}

// Mover scenes: the replay of a rigid object turning (world matrix in single registers) and of a skinned mesh whose two
// parts follow two bones (a bone palette indexed by BLENDINDICES: c[a0 + 10]), drawn with and without the jitter; every
// covered pixel's MoverPS vector against the exact one (the plane's homography: this frame's NDC of the drawn point, its
// previous NDC). Then MotionPS: a mover's own vector where its depth is the visible one, the camera's (un-jittered) where
// something nearer covers it or where it left (disocclusion)
void MoverScenesOnGpu(Gpu& gpu, const std::string& moverHlsl, const std::string& gbHlsl) {
    IDirect3DDevice9* dev = gpu.dev;
    if (!dev || moverHlsl.empty() || gbHlsl.empty()) return;
    IDirect3DPixelShader9* moverPs = MakePs(dev, CompileEntry(moverHlsl, "MoverPS"));
    IDirect3DPixelShader9* motionPs = MakePs(dev, CompileEntry(gbHlsl, "MotionPS"));
    Check(moverPs && motionPs, "MoverPS and MotionPS created");
    const char* rigidSrc = "float4 vp[4] : register(c0); float4 world[3] : register(c4);"
                           "struct O { float4 p : POSITION; float4 t : TEXCOORD0; };"
                           "O main(float4 pos : POSITION, float4 bi : BLENDINDICES) { O o; float4 w = float4(dot(world[0], pos), dot(world[1], pos), dot(world[2], pos), 1);"
                           "o.p = float4(dot(vp[0], w), dot(vp[1], w), dot(vp[2], w), dot(vp[3], w)); o.t = bi; return o; }";
    const char* skinSrc = "float4 vp[4] : register(c0); float4 bones[24] : register(c10);"
                          "struct O { float4 p : POSITION; float4 t : TEXCOORD0; };"
                          "O main(float4 pos : POSITION, float4 bi : BLENDINDICES) { O o; int k = (int)(bi.x * 3);"
                          "float4 w = float4(dot(bones[k], pos), dot(bones[k + 1], pos), dot(bones[k + 2], pos), 1);"
                          "o.p = float4(dot(vp[0], w), dot(vp[1], w), dot(vp[2], w), dot(vp[3], w)); o.t = pos; return o; }";
    // camera: the game's kind of perspective (near 0.25, depth d = A - A near / z with A = 1.00008, tan 0.5) looking down
    // +z from the origin; rows of VP
    const double kA = 1.00008, vp[4][4] = {{2, 0, 0, 0}, {0, 2, 0, 0}, {0, 0, kA, -kA * 0.25}, {0, 0, 1, 0}};
    // a world matrix (3 rows): turned by yaw about y and roll about z, moved to (tx, ty, tz)
    auto world = [](double yaw, double roll, double tx, double ty, double tz, double m[3][4]) {
        const double cy = std::cos(yaw), sy = std::sin(yaw), cr = std::cos(roll), sr = std::sin(roll);
        const double r[3][3] = {{cy * cr, -cy * sr, sy}, {sr, cr, 0}, {-sy * cr, sy * sr, cy}};
        for (int i = 0; i < 3; i++) m[i][0] = r[i][0], m[i][1] = r[i][1], m[i][2] = r[i][2];
        m[0][3] = tx, m[1][3] = ty, m[2][3] = tz;
    };
    // NDC x, y, w rows of VP * world on the plane point (s, t, 0, 1): a homography of (s, t, 1); z row apart
    auto plane = [&](const double w[3][4], double H[3][3], double Z[3]) {
        const int rows[3] = {0, 1, 3};
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 3; k++) {
                const int col = k == 2 ? 3 : k; // s, t, translation
                double v = 0;
                for (int j = 0; j < 4; j++) v += vp[rows[i]][j] * (j < 3 ? w[j][col] : (col == 3 ? 1.0 : 0.0));
                H[i][k] = v;
            }
        for (int k = 0; k < 3; k++) {
            const int col = k == 2 ? 3 : k;
            double v = 0;
            for (int j = 0; j < 4; j++) v += vp[2][j] * (j < 3 ? w[j][col] : (col == 3 ? 1.0 : 0.0));
            Z[k] = v;
        }
    };
    struct Part { double cur[3][4], prev[3][4]; float bone; float s0, s1, t0, t1; };
    // the depth buffer as INTZ (the game's depth is read so, DepthShare): the motion pass compares the mover's depth with it
    IDirect3DTexture9* intz = nullptr;
    IDirect3DSurface9* depthSurf = nullptr;
    Check(SUCCEEDED(dev->CreateTexture(kSize, kSize, 1, D3DUSAGE_DEPTHSTENCIL, static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z')), D3DPOOL_DEFAULT, &intz, nullptr)) &&
              SUCCEEDED(intz->GetSurfaceLevel(0, &depthSurf)),
          "mover scene depth buffer (INTZ)");
    IDirect3DPixelShader9* copyPs = MakePs(dev, Compile("sampler2D s : register(s0); float4 main(float2 uv : TEXCOORD0) : COLOR0 { return tex2D(s, uv); }", "ps_3_0"));
    Rt zRead;
    Check(copyPs && zRead.Make(dev, D3DFMT_A32B32G32R32F), "depth read pass");
    Rt target;
    Check(target.Make(dev, D3DFMT_A32B32G32R32F), "mover target (A32B32G32R32F)");
    const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                    {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0}, D3DDECL_END()};
    IDirect3DVertexDeclaration9* decl = nullptr;
    dev->CreateVertexDeclaration(el, &decl);
    auto scene = [&](const char* what, const char* src, bool skinned, std::vector<Part> parts) {
        const std::vector<DWORD> code = Compile(src, "vs_3_0");
        ShaderPatches::MotionSlice s;
        auto r = code.empty() ? ShaderPatches::MotionSliceResult::Unreadable : ShaderPatches::AnalyzeMotionSlice(code, &s);
        std::vector<int> ext;
        for (const auto& g : s.regions) ext.push_back(std::max(g.lastOffset + 1, std::min(g.maxExtent, 24)));
        ShaderPatches::MotionReplayLayout L;
        std::vector<DWORD> replay;
        if (r == ShaderPatches::MotionSliceResult::Ok) r = ShaderPatches::MakeMotionReplayVs(s, ext, &L, &replay);
        IDirect3DVertexShader9* vs = nullptr;
        const bool made = r == ShaderPatches::MotionSliceResult::Ok && SUCCEEDED(dev->CreateVertexShader(replay.data(), &vs));
        Check(made && (skinned ? s.regions.size() == 1 : s.regions.empty()), skinned ? "skinned mesh: replay made (one indexed bone palette)" : "rigid object: replay made (world rows in single registers)");
        if (!made || !depthSurf || !target.surf || !moverPs) { Release(vs); return; }
        // bank A: this frame; bank B: the previous frame at the layout's registers
        static float A[256][4];
        std::memset(A, 0, sizeof A);
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 4; k++) A[i][k] = static_cast<float>(vp[i][k]);
        std::vector<std::pair<int, std::array<float, 4>>> bankB;
        auto rowsOf = [](const double m[3][4], int row) { return std::array<float, 4>{float(m[row][0]), float(m[row][1]), float(m[row][2]), float(m[row][3])}; };
        float prevRegs[256][4];
        std::memcpy(prevRegs, A, sizeof A);
        for (const Part& p : parts)
            for (int row = 0; row < 3; row++) {
                const int reg = skinned ? 10 + static_cast<int>(p.bone) * 3 + row : 4 + row;
                const auto c = rowsOf(p.cur, row), pv = rowsOf(p.prev, row);
                std::memcpy(A[reg], c.data(), 16);
                std::memcpy(prevRegs[reg], pv.data(), 16);
            }
        for (size_t i = 0; i < s.singles.size(); i++) bankB.push_back({L.singleTo[i], {prevRegs[s.singles[i]][0], prevRegs[s.singles[i]][1], prevRegs[s.singles[i]][2], prevRegs[s.singles[i]][3]}});
        for (size_t i = 0; i < s.regions.size(); i++)
            for (int k = 0; k < L.extents[i]; k++) {
                const int from = s.regions[i].base + k;
                bankB.push_back({L.regionTo[i] + k, {prevRegs[from][0], prevRegs[from][1], prevRegs[from][2], prevRegs[from][3]}});
            }
        for (const float jit : {0.0f, 0.37f}) { // jitter in pixels (x right, y down by 0.21 x it)
            const float jc[2] = {2.0f * jit / kSize, -2.0f * (0.21f * jit) / kSize};
            struct Vx { float p[4], bi[4]; };
            std::vector<Vx> tris;
            for (const Part& p : parts) {
                const Vx v[4] = {{{p.s0, p.t0, 0, 1}, {p.bone, 0, 0, 0}}, {{p.s1, p.t0, 0, 1}, {p.bone, 0, 0, 0}}, {{p.s0, p.t1, 0, 1}, {p.bone, 0, 0, 0}},
                                 {{p.s1, p.t1, 0, 1}, {p.bone, 0, 0, 0}}};
                for (int i : {0, 1, 2, 2, 1, 3}) tris.push_back(v[i]);
            }
            dev->SetRenderTarget(0, target.surf);
            dev->SetRenderTarget(1, nullptr);
            dev->SetDepthStencilSurface(depthSurf);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
            dev->SetRenderState(D3DRS_ZENABLE, TRUE);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            dev->SetVertexDeclaration(decl);
            dev->SetVertexShader(vs);
            dev->SetPixelShader(moverPs);
            dev->SetVertexShaderConstantF(0, &A[0][0], 256);
            for (const auto& [reg, v] : bankB) dev->SetVertexShaderConstantF(static_cast<UINT>(reg), v.data(), 1);
            const float j4[4] = {jc[0], jc[1], 0, 0};
            dev->SetVertexShaderConstantF(static_cast<UINT>(L.jitter), j4, 1);
            dev->BeginScene();
            bool ok = SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(tris.size() / 3), tris.data(), sizeof(Vx)));
            dev->EndScene();
            dev->SetDepthStencilSurface(nullptr);
            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            dev->SetVertexShader(nullptr);
            Img img, zbuf;
            IDirect3DBaseTexture9* const zt[8] = {intz};
            const float none[1][4] = {};
            ok = ok && ReadImg(dev, target, img) && FullPass(dev, copyPs, zt, 0, none, 1, zRead.surf, nullptr) && ReadImg(dev, zRead, zbuf);
            Check(ok, "mover scene drawn");
            if (!ok) continue;
            // each covered pixel: the drawn point (pixel centre minus the jitter; D3D9: pixel x's centre is NDC 2x / W - 1) on
            // the nearest part that contains it
            int covered = 0, worstAt = -1;
            double worst = 0, worstZ = 0, worstDrawn = 0;
            for (int y = 0; y < kSize; y++)
                for (int x = 0; x < kSize; x++) {
                    const auto& o = img[y * kSize + x];
                    if (o[3] < 0.5f) continue;
                    const double nx = x * 2.0 / kSize - 1 - jc[0], ny = 1 - y * 2.0 / kSize - jc[1];
                    double best = 1e30, mvx = 0, mvy = 0, d = 0;
                    for (const Part& p : parts) {
                        double Hc[3][3], Hp[3][3], Zc[3], Zp[3], Ic[3][3];
                        plane(p.cur, Hc, Zc);
                        plane(p.prev, Hp, Zp);
                        if (!Inverse3(Hc, Ic)) continue;
                        double st[3];
                        for (int i = 0; i < 3; i++) st[i] = Ic[i][0] * nx + Ic[i][1] * ny + Ic[i][2];
                        const double ss = st[0] / st[2], tt = st[1] / st[2];
                        if (ss < p.s0 - 1e-3 || ss > p.s1 + 1e-3 || tt < p.t0 - 1e-3 || tt > p.t1 + 1e-3) continue;
                        const double w = Hc[2][0] * ss + Hc[2][1] * tt + Hc[2][2], z = (Zc[0] * ss + Zc[1] * tt + Zc[2]) / w;
                        if (z >= best) continue;
                        best = z, d = z;
                        const double pw = Hp[2][0] * ss + Hp[2][1] * tt + Hp[2][2];
                        mvx = nx - (Hp[0][0] * ss + Hp[0][1] * tt + Hp[0][2]) / pw;
                        mvy = ny - (Hp[1][0] * ss + Hp[1][1] * tt + Hp[1][2]) / pw;
                    }
                    if (best > 1e29) continue; // an edge pixel the plane test missed by rounding
                    covered++;
                    const double e = std::max(std::fabs(o[0] - mvx), std::fabs(o[1] - mvy));
                    if (e > worst) worst = e, worstAt = y * kSize + x;
                    worstZ = std::max(worstZ, std::fabs(o[2] - d));
                    worstDrawn = std::max(worstDrawn, static_cast<double>(std::fabs(o[2] - zbuf[y * kSize + x][0])));
                }
            std::printf("  %s, jitter %.2f px: %d pixels, motion max error %.2g NDC (%.3f px), depth max error %.2g, from the depth buffer %.2g\n", what, jit, covered, worst,
                        worst * kSize / 2, worstZ, worstDrawn);
            if (worst >= 2e-4) std::printf("    worst at pixel (%d, %d): got (%.5f, %.5f)\n", worstAt % kSize, worstAt / kSize, img[worstAt][0], img[worstAt][1]);
            Check(covered > 200, "mover scene covers the screen");
            Check(worst < 2e-4, "mover vectors exact (2e-4 NDC) with and without the jitter");
            Check(worstDrawn <= GBufferDepthMatch, "mover depth matches the depth buffer within the motion pass's match");
        }
        Release(vs);
    };
    {
        Part p{};
        world(0.55, 0.20, 0.2, 0.0, 6.0, p.cur); // turned 0.05 rad (yaw) and 0.04 rad (roll) since last frame
        world(0.50, 0.16, 0.2, 0.0, 6.0, p.prev);
        p.bone = 0, p.s0 = -2.5f, p.s1 = 2.5f, p.t0 = -2.5f, p.t1 = 2.5f;
        scene("rotating object", rigidSrc, false, {p});
    }
    {
        Part a{}, b{};
        world(0.30, 0.0, -1.2, 0.3, 5.0, a.cur); // bone 2: the upper arm swings and the body walks
        world(0.22, 0.0, -1.35, 0.3, 5.0, a.prev);
        world(-0.2, 0.3, 1.0, -0.5, 5.5, b.cur); // bone 5: another part, its own motion
        world(-0.2, 0.25, 0.9, -0.5, 5.5, b.prev);
        a.bone = 2, a.s0 = -1.5f, a.s1 = 1.5f, a.t0 = -2.0f, a.t1 = 2.0f;
        b.bone = 5, b.s0 = -1.5f, b.s1 = 1.5f, b.t0 = -1.5f, b.t1 = 1.5f;
        scene("skinned mesh", skinSrc, true, {a, b});
    }
    Release(decl);
    Release(depthSurf), Release(intz), Release(copyPs);
    // MotionPS: the mover's vector in the left half (depth 0.5) except x < 8, where something nearer (0.3) covers it; the
    // right half (0.8) has no mover (where it was: disocclusion). Camera: zoom by 1.1 (D rows), jitter (0.3, -0.2) px
    if (motionPs) {
        const float jx = 0.3f, jy = -0.2f, jc[2] = {2 * jx / kSize, -2 * jy / kSize};
        Img depth(kSize * kSize), obj(kSize * kSize);
        for (int y = 0; y < kSize; y++)
            for (int x = 0; x < kSize; x++) {
                depth[y * kSize + x] = {x < 8 ? 0.3f : x < kSize / 2 ? 0.5f : 0.8f, 0, 0, 0};
                obj[y * kSize + x] = x < kSize / 2 ? std::array<float, 4>{0.05f, 0.02f, 0.5f, 1} : std::array<float, 4>{};
            }
        IDirect3DTexture9* dt = FloatTexture(dev, D3DFMT_A32B32G32R32F, 1, [&](int x, int y) { return depth[y * kSize + x]; });
        IDirect3DTexture9* ot = FloatTexture(dev, D3DFMT_A32B32G32R32F, 1, [&](int x, int y) { return obj[y * kSize + x]; });
        Rt motion;
        Check(dt && ot && motion.Make(dev, D3DFMT_A16B16G16R16F), "motion pass textures");
        if (dt && ot && motion.surf) {
            const float c[6][4] = {{1.1f, 0, 0, 0}, {0, 1.1f, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}, {0, 32768.0f, 0, 0}, {jc[0], jc[1], GBufferDepthMatch, 1}};
            IDirect3DBaseTexture9* const t[8] = {dt, nullptr, nullptr, ot};
            Img out;
            const bool ok = FullPass(dev, motionPs, t, 0, c, 6, motion.surf, nullptr) && ReadImg(dev, motion, out);
            Check(ok, "MotionPS draws");
            int mover = 0, camera = 0, bad = 0;
            for (int y = 0; y < kSize && ok; y++)
                for (int x = 0; x < kSize; x++) {
                    const auto& o = out[y * kSize + x];
                    const bool own = x >= 8 && x < kSize / 2;
                    const double px = (x + 0.5) * 2.0 / kSize - 1 - jc[0], py = 1 - (y + 0.5) * 2.0 / kSize - jc[1];
                    const double want[3] = {own ? 0.05 : -0.1 * px, own ? 0.02 : -0.1 * py, own ? 1.0 : 0.0};
                    const bool good = std::fabs(o[0] - want[0]) < 1e-3 && std::fabs(o[1] - want[1]) < 1e-3 && o[2] == want[2];
                    bad += !good;
                    (own ? mover : camera) += good;
                }
            std::printf("  MotionPS: %d mover pixels with their own vector, %d with the camera's (covered, disoccluded), %d wrong\n", mover, camera, bad);
            Check(ok && bad == 0, "motion pass: a mover's own vector where it is the visible surface, else the camera's, un-jittered");
        }
        Release(dt), Release(ot);
    }
    Release(moverPs), Release(motionPs);
}

// Temporal AA (TaaPS, SharpenPS): a disc drawn with the 16 Halton offsets as the game's jittered scene would show it.
// Static: the resolve converges to the supersampled image. Moving 2 px a frame (its own motion vector, mover flag):
// no pixel more than 1.5 px from its edge keeps an old colour (no ghost beyond one frame). Sharpen: the back buffer's
// alpha (the game's) is kept and amount 0 is a copy
void TaaOnGpu(Gpu& gpu, const std::string& taaHlsl) {
    IDirect3DDevice9* dev = gpu.dev;
    if (!dev || taaHlsl.empty()) return;
    IDirect3DPixelShader9* taa = MakePs(dev, CompileEntry(taaHlsl, "TaaPS"));
    IDirect3DPixelShader9* sharpen = MakePs(dev, CompileEntry(taaHlsl, "SharpenPS"));
    Check(taa && sharpen, "TaaPS and SharpenPS created");
    Rt color[2], aux[2], out;
    bool ok = taa && sharpen && out.Make(dev, D3DFMT_A16B16G16R16F);
    for (int i = 0; i < 2 && ok; i++) ok = color[i].Make(dev, D3DFMT_A16B16G16R16F) && aux[i].Make(dev, D3DFMT_A16B16G16R16F);
    Check(ok, "temporal AA targets");
    if (!ok) { Release(taa), Release(sharpen); return; }
    const float R = 17.3f, kDisc = 1.0f, kBack = 0.2f;
    auto inDisc = [&](double x, double y, double cx) { return (x - cx) * (x - cx) + (y - 32.2) * (y - 32.2) < R * R; };
    auto reference = [&](double cx) { // 16 x 16 samples a pixel
        Img r(kSize * kSize);
        for (int y = 0; y < kSize; y++)
            for (int x = 0; x < kSize; x++) {
                int n = 0;
                for (int j = 0; j < 16; j++)
                    for (int i = 0; i < 16; i++) n += inDisc(x + (i + 0.5) / 16, y + (j + 0.5) / 16, cx);
                const float v = kBack + (kDisc - kBack) * n / 256.0f;
                r[y * kSize + x] = {v, v, v, 1};
            }
        return r;
    };
    GBufferMotion::JitterClock clock;
    int ping = 0, frame = 0;
    // one frame: the scene with this frame's jitter, its G-buffer (id, depth), motion (dx px a frame for the disc), the resolve
    auto step = [&](double cx, double dx, Img* resolved) {
        clock.Present(16, kSize, kSize);
        float jc[2];
        clock.Boundary(jc);
        const double jx = jc[0] * kSize / 2, jy = -jc[1] * kSize / 2; // pixels, y down
        Img scene(kSize * kSize), alb(kSize * kSize), nrm(kSize * kSize), dep(kSize * kSize), mot(kSize * kSize);
        for (int y = 0; y < kSize; y++)
            for (int x = 0; x < kSize; x++) {
                const bool d = inDisc(x + 0.5 - jx, y + 0.5 - jy, cx);
                const float v = d ? kDisc : kBack;
                const int i = y * kSize + x;
                scene[i] = {v, v, v, 1};
                alb[i] = {0.5f, 0.5f, 0.5f, (d ? 9 : 3) / 255.0f};
                nrm[i] = {0.5f, 0.5f, 0.5f, 0};
                dep[i] = {d ? 0.5f : 0.9f, 0, 0, 0};
                mot[i] = {d ? static_cast<float>(2 * dx / kSize) : 0.0f, 0, d && dx != 0 ? 1.0f : 0.0f, 0};
            }
        IDirect3DTexture9* cur = HalfTexture(dev, scene);
        auto f32 = [&](const Img& m) { return FloatTexture(dev, D3DFMT_A32B32G32R32F, 1, [&](int x, int y) { return m[y * kSize + x]; }); };
        IDirect3DTexture9 *ta = f32(alb), *tn = f32(nrm), *tm = f32(mot);
        IDirect3DTexture9* td = FloatTexture(dev, D3DFMT_R32F, 1, [&](int x, int y) { return dep[y * kSize + x]; });
        const float c[5][4] = {{1.0f / kSize, 1.0f / kSize, kSize, kSize},
                               {static_cast<float>(jx / kSize), static_cast<float>(jy / kSize), frame ? 1.0f : 0.0f, 16},
                               {0, 0, 0, 1},
                               {1, 1, 0.05f, 0},
                               {0.25f, 0, 0, 0}};
        IDirect3DBaseTexture9* const t[8] = {cur, color[ping].tex, aux[ping].tex, tm, ta, tn, td};
        const int next = ping ^ 1;
        bool good = cur && ta && tn && tm && td && FullPass(dev, taa, t, 0x3, c, 5, color[next].surf, aux[next].surf);
        good = good && (!resolved || ReadImg(dev, color[next], *resolved));
        for (IDirect3DTexture9* x : {cur, ta, tn, tm, td}) Release(x);
        ping = next, frame++;
        return good;
    };
    // static: 48 frames (the first frame's estimate, 1 / 16 of the history after 16 frames, has faded)
    Img res;
    bool drawn = true;
    for (int f = 0; f < 48; f++) drawn &= step(30.0, 0, f == 47 ? &res : nullptr);
    Check(drawn, "temporal AA frames drawn");
    if (drawn) {
        const Img ref = reference(30.0);
        double eTaa = 0, eAlias = 0, eFlat = 0;
        int edge = 0;
        for (int y = 0; y < kSize; y++)
            for (int x = 0; x < kSize; x++) {
                const int i = y * kSize + x;
                const float alias = inDisc(x + 0.5, y + 0.5, 30.0) ? kDisc : kBack;
                if (ref[i][0] > kBack + 1e-4f && ref[i][0] < kDisc - 1e-4f) edge++, eTaa += std::fabs(res[i][0] - ref[i][0]), eAlias += std::fabs(alias - ref[i][0]);
                else eFlat = std::max(eFlat, static_cast<double>(std::fabs(res[i][0] - ref[i][0])));
            }
        std::printf("  TAA static disc: %d edge pixels, mean error %.4f (one sample at the centre: %.4f); flat pixels max %.4f\n", edge, eTaa / edge, eAlias / edge, eFlat);
        Check(edge > 50 && eTaa < 0.35 * eAlias, "TAA: a still scene converges to the supersampled image (edge error under 35 % of one sample's)");
        Check(eFlat < 0.01, "TAA: flat areas keep their colour (the first frame's bilinear estimate fades by 15 / 16 a frame)");
    }
    // moving: 2 px a frame for 4 frames. A pixel more than 1.5 px from the disc's edge now and from where it was last
    // frame must show the current image (a colour older than one frame is a ghost); the one-frame trail is counted apart
    double cx = 30.0;
    int ghosts = 0, checked = 0, trail = 0;
    double worstGhost = 0;
    for (int f = 0; f < 4 && drawn; f++) {
        const double prevCx = cx;
        cx += 2.0;
        drawn &= step(cx, 2.0, &res);
        const Img ref = reference(cx);
        for (int y = 0; y < kSize; y++)
            for (int x = 0; x < kSize; x++) {
                const double r = std::hypot(x + 0.5 - cx, y + 0.5 - 32.2), rPrev = std::hypot(x + 0.5 - prevCx, y + 0.5 - 32.2);
                if (std::fabs(r - R) <= 1.5) continue;
                const double e = std::fabs(res[y * kSize + x][0] - ref[y * kSize + x][0]);
                if (rPrev <= R + 1.5 && r > R) { trail += e > 0.02; continue; }
                checked++;
                worstGhost = std::max(worstGhost, e);
                if (e > 0.02 && ghosts++ < 4)
                    std::printf("    frame %d pixel (%d, %d), %.1f px from the edge: %.3f, want %.3f\n", f, x, y, r - R, res[y * kSize + x][0], ref[y * kSize + x][0]);
            }
    }
    std::printf("  TAA moving disc (2 px a frame, 4 frames): %d pixels checked, %d keep a colour older than one frame (worst %.4f); one-frame trail %d pixels\n",
                checked, ghosts, worstGhost, trail);
    Check(drawn && ghosts == 0, "TAA: a moving object leaves no ghost beyond one frame (1.5 px from its edge)");
    // sharpen: alpha 0.25 (the game's, in the back buffer) kept under the RGB mask; amount 0 copies
    {
        const float c[5][4] = {{1.0f / kSize, 1.0f / kSize, kSize, kSize}, {}, {}, {}, {0, 0, 0, 0}};
        dev->SetRenderTarget(0, out.surf);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_COLORVALUE(0, 0, 0, 0.25f), 1.0f, 0);
        IDirect3DBaseTexture9* const t[8] = {color[ping].tex};
        Img o, src;
        const bool good = FullPass(dev, sharpen, t, 0, c, 5, out.surf, nullptr, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE) &&
                          ReadImg(dev, out, o) && ReadImg(dev, color[ping], src);
        bool alpha = good, copy = good;
        for (size_t i = 0; good && i < o.size(); i++) alpha &= std::fabs(o[i][3] - 0.25f) < 4e-3f, copy &= std::fabs(o[i][0] - src[i][0]) < 1e-3f;
        Check(alpha, "sharpen writes RGB only: the back buffer's alpha (the game's) is kept");
        Check(copy, "sharpen amount 0 is a copy");
    }
    Release(taa), Release(sharpen);
}

void RunSections(Gpu& gpu, const std::vector<std::vector<DWORD>>& all, const Families& families, const RuleClasses& ruleClasses, const std::string& aoHlsl,
                 const std::string& only, int moverLimit, const std::string& gbHlsl, const std::string& taaHlsl, const std::string& moverHlsl) {
    auto on = [&](const char* name) { return only.empty() || only.find(name) != std::string::npos; };
    if (on("package")) Package(gpu, all, families);
    if (on("outputs")) OnGpu(gpu, "ps_3_0", "vs_3_0"), OnGpu(gpu, "ps_2_0", "vs_2_0");
    if (on("rules")) RulesOnGpu(gpu, all, ruleClasses, families);
    if (on("ao") && !aoHlsl.empty()) AoOnGpu(gpu, aoHlsl);
    if (on("movers")) MoversOnGpu(gpu, all, moverLimit);
    if (on("scenes")) MoverScenesOnGpu(gpu, moverHlsl, gbHlsl);
    if (on("taa")) TaaOnGpu(gpu, taaHlsl);
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
    std::string aoHlsl, gbHlsl, taaHlsl, moverHlsl;
    RuleClasses ruleClasses;
    std::string only; // --only movers,taa: those sections alone (a quick run)
    int moverLimit = 0; // --mover-limit N: probe at most N shaders per version (0: all)
    for (int a = 2; a + 1 < argc; a++) {
        if (std::strcmp(argv[a], "--only") == 0) only = argv[a + 1];
        if (std::strcmp(argv[a], "--mover-limit") == 0) moverLimit = std::atoi(argv[a + 1]);
        if (std::strcmp(argv[a], "--families") == 0) families = ReadFamilies(argv[a + 1]);
        if (std::strcmp(argv[a], "--rules") == 0) ruleClasses = ReadRuleClasses(argv[a + 1]);
        for (auto [flag, dst] : {std::pair{"--ao", &aoHlsl}, std::pair{"--gb", &gbHlsl}, std::pair{"--taa", &taaHlsl}, std::pair{"--mover", &moverHlsl}}) {
            if (std::strcmp(argv[a], flag) != 0) continue;
            std::ifstream h(argv[a + 1], std::ios::binary);
            dst->assign(std::istreambuf_iterator<char>(h), {});
            Check(!dst->empty(), "HLSL read");
        }
    }
    {
        std::puts("Native D3D9");
        Gpu gpu(LoadLibraryW(L"d3d9.dll"));
        RunSections(gpu, all, families, ruleClasses, aoHlsl, only, moverLimit, gbHlsl, taaHlsl, moverHlsl);
    }
    for (int a = 2; a + 1 < argc; a++) {
        if (std::strcmp(argv[a], "--d3d9") != 0) continue;
        HMODULE other = LoadLibraryA(argv[a + 1]);
        Check(other != nullptr, "the other d3d9.dll loads");
        if (!other) continue;
        std::printf("%s\n", argv[a + 1]);
        Gpu gpu(other);
        RunSections(gpu, all, families, ruleClasses, aoHlsl, only, moverLimit, gbHlsl, taaHlsl, moverHlsl);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
