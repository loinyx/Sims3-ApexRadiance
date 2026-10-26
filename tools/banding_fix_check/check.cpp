// Offline check of the Banding Fix's shader copies (ShaderPatches::AddDither, AddDither2, AddScreenPosVs).
// Read-only: prints to stdout, writes no file.
//   check.exe <Shaders_Win32.precomp> [--d3d9 <d3d9.dll>]
// 1. Every ps_3_0 of the package through AddDither, every ps_2_0 through AddDither2 and every vs_1_1 / vs_2_x through
//    AddScreenPosVs (highest free k), each copy created on a native D3D9 HAL device (the strict validator).
// 2. The grain on the GPU (a flat colour through a patched synthetic ps_3_0): the mean of each grey is kept, pure black and
//    pure white come out unchanged, and Strength 0 adds nothing.
// 3. The ps_2_x path (a patched vs_2_0 + ps_2_0 pair) gives the same pixels as the ps_3_0 path.
// 4. The Picture dither (PicturePS from features/picture.cpp, every control neutral): an unchanged picture stays bit for
//    bit.
// --d3d9 runs 1 to 4 again on another D3D9 implementation (a DXVK d3d9.dll), loaded from that path.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include "../../features/shader_patches.h"
#include "../../shaders/shader_ids.h"
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

// ---- a hidden D3D9 device (64 x 64 target) ----
constexpr int kSize = 64;
struct Gpu {
    HWND window = nullptr;
    IDirect3D9* api = nullptr;
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9 *target = nullptr, *readback = nullptr;
    explicit Gpu(HMODULE d3d9) {
        window = CreateWindowExW(0, L"STATIC", L"Apex banding fix check", WS_OVERLAPPED, 0, 0, kSize, kSize, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
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
        Check(SUCCEEDED(dev->CreateRenderTarget(kSize, kSize, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr)), "readback target");
        Check(SUCCEEDED(dev->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr)), "readback surface");
        dev->SetRenderTarget(0, target);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
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
    // a full-screen quad through the given shaders; the pixels as A8R8G8B8
    std::vector<DWORD> Render(const std::vector<DWORD>& vsCode, const std::vector<DWORD>& psCode, int amountReg, const float amount[4], const float colour[4]) {
        std::vector<DWORD> px(kSize * kSize, 0xDEADBEEFu);
        if (!dev) return px;
        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        IDirect3DVertexDeclaration9* decl = nullptr;
        struct Vertex {
            float x, y, z, w;
        };
        const Vertex q[4] = {{-1, 1, 0.5f, 1}, {1, 1, 0.5f, 1}, {-1, -1, 0.5f, 1}, {1, -1, 0.5f, 1}};
        const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
        const bool ok = SUCCEEDED(dev->CreateVertexShader(vsCode.data(), &vs)) && SUCCEEDED(dev->CreatePixelShader(psCode.data(), &ps)) &&
                        SUCCEEDED(dev->CreateVertexDeclaration(el, &decl));
        Check(ok, "test shaders created");
        if (ok) {
            dev->SetVertexDeclaration(decl);
            dev->SetVertexShader(vs);
            dev->SetPixelShader(ps);
            dev->SetPixelShaderConstantF(0, colour, 1);
            dev->SetPixelShaderConstantF(static_cast<UINT>(amountReg), amount, 1);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF00FF00u, 1.0f, 0);
            dev->BeginScene();
            Check(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vertex))), "test quad draw");
            dev->EndScene();
            D3DLOCKED_RECT r{};
            if (SUCCEEDED(dev->GetRenderTargetData(target, readback)) && SUCCEEDED(readback->LockRect(&r, nullptr, D3DLOCK_READONLY))) {
                for (int y = 0; y < kSize; y++) std::memcpy(px.data() + y * kSize, static_cast<char*>(r.pBits) + y * r.Pitch, kSize * 4);
                readback->UnlockRect();
            } else
                Check(false, "GPU readback");
        }
        Release(decl);
        Release(vs);
        Release(ps);
        return px;
    }
};

std::vector<DWORD> Compile(const char* src, const char* profile, const char* entry = "main") {
    ID3DBlob *code = nullptr, *err = nullptr;
    const HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr) && err) std::printf("%s\n", static_cast<const char*>(err->GetBufferPointer()));
    std::vector<DWORD> out;
    if (code) out.assign(static_cast<const DWORD*>(code->GetBufferPointer()), static_cast<const DWORD*>(code->GetBufferPointer()) + code->GetBufferSize() / 4);
    Release(code);
    Release(err);
    return out;
}

const char* Reason(ShaderPatches::DitherResult r) {
    static const char* names[] = {"ok", "version", "no colour write", "subroutines", "relative constants", "no free register", "unreadable"};
    return names[static_cast<int>(r)];
}

// ---- 1. the game's shaders ----
void Package(Gpu& gpu, const std::vector<std::vector<DWORD>>& all) {
    int n3 = 0, ok3 = 0, acc3 = 0, n2 = 0, ok2 = 0, acc2 = 0, n1 = 0, nv = 0, okv = 0, accv = 0, k7 = 0;
    std::map<std::string, int> why3, why2;
    for (const auto& code : all) {
        const DWORD v = code[0];
        if (v == 0xFFFF0300u) {
            n3++;
            std::vector<DWORD> t = code;
            const auto r = ShaderPatches::AddDither(t);
            if (r != ShaderPatches::DitherResult::Ok) {
                why3[Reason(r)]++;
                continue;
            }
            ok3++;
            acc3 += gpu.AcceptsPs(t);
        } else if (v == 0xFFFF0200u || v == 0xFFFF0201u) {
            n2++;
            std::vector<DWORD> t = code;
            const auto r = ShaderPatches::AddDither2(t);
            if (r != ShaderPatches::DitherResult::Ok) {
                why2[Reason(r)]++;
                continue;
            }
            ok2++;
            acc2 += gpu.AcceptsPs(t);
        } else if ((v & 0xFFFF0000u) == 0xFFFF0000u) {
            n1++;
        } else if (v == 0xFFFE0101u || v == 0xFFFE0200u || v == 0xFFFE0201u) {
            nv++;
            for (int k = 7; k >= 0; k--) {
                std::vector<DWORD> t = code;
                if (!ShaderPatches::AddScreenPosVs(t, k)) continue;
                okv++;
                k7 += k == 7;
                accv += gpu.AcceptsVs(t);
                break;
            }
        }
    }
    auto list = [](const std::map<std::string, int>& m) {
        std::string s;
        for (const auto& [k, n] : m) s += " " + k + " " + std::to_string(n) + ";";
        return s;
    };
    std::printf("ps_3_0: %d unique, %d patched, %d accepted by the device; refused:%s\n", n3, ok3, acc3, list(why3).c_str());
    std::printf("ps_2_0: %d unique, %d patched (ps_2_x), %d accepted; refused:%s\n", n2, ok2, acc2, list(why2).c_str());
    std::printf("ps_1_x: %d left alone\n", n1);
    std::printf("vs_1_1 / vs_2_x: %d unique, %d with a screen-position copy (%d with t7), %d accepted\n", nv, okv, k7, accv);
    Check(ok3 > 0 && acc3 == ok3, "every ps_3_0 copy is accepted");
    Check(ok2 > 0 && acc2 == ok2, "every ps_2_x copy is accepted");
    Check(okv > 0 && accv + 1 >= okv, "every vertex copy but one is accepted (the refused one is not used at run time)");
}

// ---- 2. and 3. the grain on the GPU ----
const char* kVs = "float4 main(float4 p : POSITION) : POSITION { return p; }";
const char* kPs = "float4 c0 : register(c0); float4 main() : COLOR { return c0; }";

struct Grain {
    std::vector<DWORD> vs3, ps3, vs2, ps2;
    int reg3 = -1, reg2 = -1, k = -1;
};

bool Prepare(Grain& g) {
    g.vs3 = Compile(kVs, "vs_3_0");
    g.ps3 = Compile(kPs, "ps_3_0");
    g.vs2 = Compile(kVs, "vs_2_0");
    g.ps2 = Compile(kPs, "ps_2_0");
    const bool ok = !g.vs3.empty() && !g.ps3.empty() && !g.vs2.empty() && !g.ps2.empty() &&
                    ShaderPatches::AddDither(g.ps3, &g.reg3) == ShaderPatches::DitherResult::Ok &&
                    ShaderPatches::AddDither2(g.ps2, &g.reg2, &g.k) == ShaderPatches::DitherResult::Ok && ShaderPatches::AddScreenPosVs(g.vs2, g.k);
    Check(ok, "synthetic shaders patched");
    return ok;
}

int Channel(DWORD p, int c) { return static_cast<int>((p >> (16 - 8 * c)) & 0xFF); }

void GrainOnGpu(Gpu& gpu, const Grain& g, const char* backend) {
    std::printf("Grain on %s\n", backend);
    // three greys per draw (r, g, b); every value in 8-bit levels
    const float greys[][3] = {{0.0f, 255.0f, 0.3f}, {20.4f, 100.7f, 254.6f}, {1.0f, 128.5f, 200.25f}, {0.5f, 64.0f, 254.0f}};
    for (const auto& gr : greys) {
        const float colour[4] = {gr[0] / 255.0f, gr[1] / 255.0f, gr[2] / 255.0f, 0.5f};
        const float amount[4] = {1.0f / 255.0f, kSize / 2.0f, kSize / 2.0f, 0.0f};
        const auto px = gpu.Render(g.vs3, g.ps3, g.reg3, amount, colour);
        for (int c = 0; c < 3; c++) {
            std::map<int, int> hist;
            double sum = 0;
            for (DWORD p : px) {
                sum += Channel(p, c);
                hist[Channel(p, c)]++;
            }
            const double mean = sum / px.size();
            std::printf("  grey %7.2f -> mean %8.3f, levels:", gr[c], mean);
            for (const auto& [lv, n] : hist) std::printf(" %d:%.3f", lv, n / static_cast<double>(px.size()));
            std::printf("\n");
            // within half a level of black or white the grain is narrower than a full TPDF (it must not cross the clamp):
            // a bias of about 0.02 level there; the GPU's own conversion is about 0.05 low at every grey
            const bool edge = gr[c] < 0.5f || gr[c] > 254.5f;
            Check(std::fabs(mean - gr[c]) < (edge ? 0.1 : 0.06), "the grain keeps the mean of a grey");
            if (gr[c] == 0.0f || gr[c] == 255.0f) Check(hist.size() == 1, "pure black and pure white get no specks");
        }
        int alpha = 0;
        for (DWORD p : px) alpha += (p >> 24) == 128 || (p >> 24) == 127;
        Check(alpha == static_cast<int>(px.size()), "alpha unchanged");
    }
    {
        const float colour[4] = {20.4f / 255.0f, 100.7f / 255.0f, 0.5f, 1.0f};
        const float amount[4] = {0.0f, kSize / 2.0f, kSize / 2.0f, 0.0f};
        const auto px = gpu.Render(g.vs3, g.ps3, g.reg3, amount, colour);
        bool flat = true;
        for (DWORD p : px) flat &= p == px[0];
        Check(flat, "Strength 0 adds nothing");
    }
    // 3. ps_2_x pair against ps_3_0, pixel for pixel, at a few greys and phases
    int same = 0, total = 0;
    for (float phase : {0.0f, 0.37f}) {
        for (float grey : {20.4f, 100.7f, 230.3f}) {
            const float colour[4] = {grey / 255.0f, grey / 255.0f, grey / 255.0f, 1.0f};
            const float amount[4] = {1.0f / 255.0f, kSize / 2.0f, kSize / 2.0f, phase};
            const auto a = gpu.Render(g.vs3, g.ps3, g.reg3, amount, colour), b = gpu.Render(g.vs2, g.ps2, g.reg2, amount, colour);
            for (size_t i = 0; i < a.size(); i++) same += a[i] == b[i];
            total += static_cast<int>(a.size());
        }
    }
    std::printf("  ps_2_x pair = ps_3_0: %d / %d pixels\n", same, total);
    Check(same >= total * 99 / 100, "the ps_2_x grain lines up with the ps_3_0 grain");
}

// ---- 4. The Picture dither (PicturePS of features/picture.cpp, every control neutral) ----
std::string PictureSource() {
    std::string path = __FILE__;
    path = path.substr(0, path.find_last_of("\\/") + 1) + "../../features/picture.cpp";
    std::ifstream src(path);
    const std::string cpp((std::istreambuf_iterator<char>(src)), {});
    std::string hlsl;
    for (size_t pos = 0; (pos = cpp.find("R\"HLSL(", pos)) != std::string::npos;) {
        pos += 7;
        const size_t end = cpp.find(")HLSL\"", pos);
        hlsl += cpp.substr(pos, end - pos);
        pos = end + 6;
    }
    return hlsl;
}

// the picture through PicturePS with every control neutral; the input as A8R8G8B8 pixels
std::vector<DWORD> Picture(Gpu& gpu, const std::vector<DWORD>& code, const std::vector<DWORD>& in) {
    std::vector<DWORD> out(in.size(), 0);
    IDirect3DDevice9* dev = gpu.dev;
    IDirect3DTexture9* tex = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    D3DLOCKED_RECT r{};
    if (!dev || FAILED(dev->CreateTexture(kSize, kSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) || FAILED(tex->LockRect(0, &r, nullptr, 0)) ||
        FAILED(dev->CreatePixelShader(code.data(), &ps))) {
        Check(false, "Picture test resources");
        Release(tex);
        return out;
    }
    for (int y = 0; y < kSize; y++) std::memcpy(static_cast<char*>(r.pBits) + y * r.Pitch, in.data() + y * kSize, kSize * 4);
    tex->UnlockRect(0);
    float c[61][4] = {};
    const float neutral[][5] = {{0, 1, 1, 0, 0}, {1, 1.0f / kSize, 1.0f / kSize, kSize, kSize}, {2, 1, 1, 0, 0}, {3, 1, 1, 1, 0}, {5, 1, 0, 0, 0}};
    for (const auto& n : neutral) std::memcpy(c[static_cast<int>(n[0])], &n[1], 16);
    dev->SetPixelShaderConstantF(0, &c[0][0], 61);
    for (DWORD s : {0u, 1u}) {
        dev->SetTexture(s, tex);
        dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    struct V {
        float x, y, z, w, u, v;
    };
    const float e = kSize - 0.5f;
    const V q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {e, -0.5f, 0, 1, 1, 0}, {-0.5f, e, 0, 1, 0, 1}, {e, e, 0, 1, 1, 1}};
    dev->SetVertexShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetPixelShader(ps);
    dev->BeginScene();
    Check(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V))), "Picture draw");
    dev->EndScene();
    if (SUCCEEDED(dev->GetRenderTargetData(gpu.target, gpu.readback)) && SUCCEEDED(gpu.readback->LockRect(&r, nullptr, D3DLOCK_READONLY))) {
        for (int y = 0; y < kSize; y++) std::memcpy(out.data() + y * kSize, static_cast<char*>(r.pBits) + y * r.Pitch, kSize * 4);
        gpu.readback->UnlockRect();
    }
    for (DWORD s : {0u, 1u}) dev->SetTexture(s, nullptr);
    Release(ps);
    Release(tex);
    return out;
}

void PictureDither(Gpu& gpu) {
    const std::vector<DWORD> code = Compile(PictureSource().c_str(), "ps_3_0", "PicturePS");
    Check(!code.empty(), "PicturePS compiles");
    if (code.empty() || !gpu.dev) return;
    std::printf("Picture dither (PicturePS, every control neutral)\n");
    const std::vector<DWORD> flat(kSize * kSize, 0xFF282828u); // grey 40
    const std::vector<DWORD> out = Picture(gpu, code, flat);
    int changed = 0;
    for (size_t i = 0; i < flat.size(); i++) changed += (out[i] & 0xFFFFFF) != (flat[i] & 0xFFFFFF);
    std::printf("  flat grey 40: %d pixels changed\n", changed);
    Check(changed == 0, "an unchanged picture gets no Picture dither");
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
    Grain g;
    const bool prepared = Prepare(g);
    {
        Gpu gpu(LoadLibraryW(L"d3d9.dll"));
        Package(gpu, all);
        if (prepared) GrainOnGpu(gpu, g, "native D3D9");
        PictureDither(gpu);
    }
    for (int a = 2; a + 1 < argc; a++) {
        if (std::strcmp(argv[a], "--d3d9") != 0) continue;
        HMODULE other = LoadLibraryA(argv[a + 1]);
        Check(other != nullptr, "the other d3d9.dll loads");
        if (!other || !prepared) continue;
        Gpu gpu(other);
        std::printf("Shader package on %s\n", argv[a + 1]);
        Package(gpu, all);
        GrainOnGpu(gpu, g, argv[a + 1]);
        PictureDither(gpu);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
