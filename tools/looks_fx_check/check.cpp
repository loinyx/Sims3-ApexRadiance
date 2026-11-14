// Offline check of the player's ReShade FX loading (features/looks_fx.cpp): runs the real compile path (ReShade's
// preprocessor and parser, its shader model 3.0 HLSL generator, D3DCompile vs_3_0 / ps_3_0) on every .fx of a folder laid
// out like Apex Radiance\Shaders\ and prints, per technique, OK or why it cannot run on Direct3D 9. With --run it also
// creates each runnable effect on a real Direct3D 9 device (hidden window) and runs every runnable technique once on a
// test picture. Console output only; nothing is written.
#include "looks_fx.h"
#include "looks_preset.h"
#include "looks_runner.h"
#include "apex_log.h"
#include "third_party/reshadefx/reshadefx_license.h"
#include <cmath>
#include <cstring>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")

using namespace Looks::Fx;

// ---- what the catalog (features/looks_fx_catalog.cpp) needs from the rest of Apex
static std::wstring g_apexDir;
namespace ApexPaths {
const std::wstring& ApexDirectory() { return g_apexDir; }
bool EnsureApexDirectory() { return !g_apexDir.empty(); }
}
namespace ApexLog { void Write(Level, const std::string& text, const std::source_location&) noexcept { fprintf(stderr, "log: %s\n", text.c_str()); } }
namespace CrashReport { void ThreadStart() {} }
namespace HookGuard { void Note(const char* where) noexcept { fprintf(stderr, "caught: %s\n", where); } }

static int g_checks = 0, g_failures = 0;
static void Check(bool ok, const std::string& what)
{
    ++g_checks;
    if (!ok) { ++g_failures; printf("FAIL %s\n", what.c_str()); }
}

// --catalog <Apex Radiance folder with a Shaders\ inside>: the real background scan, then a preset that names a user
// technique, a built-in one (also present as a user file), a missing one and one that can't run here
static void CatalogChecks(IDirect3DDevice9* dev)
{
    RequestScan();
    const DWORD t0 = GetTickCount();
    while ((Scanning() || !Scanned()) && GetTickCount() - t0 < 600000) { Poll(1920, 1080); Sleep(50); }
    Poll(1920, 1080);
    Check(Scanned() && !Scanning(), "the scan finished");
    const char* ini =
        "Techniques=DH_Anime_13@dh_anime.fx,Vibrance@Vibrance.fx,MXAO@MXAO.fx,DH_AhOh@dh_ahoh.fx\n"
        "TechniqueSorting=DH_Anime_13@dh_anime.fx,Vibrance@Vibrance.fx,MXAO@MXAO.fx,DH_AhOh@dh_ahoh.fx\n\n"
        "[dh_anime.fx]\nfBlackLineMultiply=0.123\n";
    Looks::Preset p;
    Check(Looks::ParsePreset(ini, p) && p.order.size() == 4, "the preset parses");
    int anime = -1;
    for (auto& e : p.order)
    {
        e.shader = Looks::FindShader(e.technique, e.file);
        if (e.technique == "DH_Anime_13") anime = e.shader;
        if (e.technique == "Vibrance") Check(e.shader >= 0 && !Looks::IsUserShader(e.shader), "Vibrance: the built-in port wins over the user file");
        if (e.technique == "MXAO") Check(e.shader < 0 && !HasFile(e.file), "MXAO: missing file");
        if (e.technique == "DH_AhOh") Check(e.shader < 0 && HasFile(e.file) && !Reason(e.technique, e.file).empty(), "DH_AhOh: present, can't run, with a reason");
    }
    Check(Looks::IsUserShader(anime) && Looks::ValidShader(anime), "DH_Anime_13 is a user shader");
    if (!Looks::IsUserShader(anime)) return;
    const LooksShader& d = Looks::Shader(anime);
    Check(d.description && std::string(d.description) == "dh_anime.fx" && d.needsDepth, "DH_Anime_13: description is the file, needs depth");
    const std::vector<float> v = Looks::PresetValues(p, d);
    Check(v.size() == (size_t)d.paramCount * 4, "values: 4 per parameter");
    bool found = false;
    for (int i = 0; i < d.paramCount; ++i)
        if (!strcmp(d.params[i].key, "fBlackLineMultiply")) { found = true; Check(std::fabs(v[i * 4] - 0.123f) < 1e-5f, "the .ini's value is used"); Check(d.params[i].kind == LooksParamKind::Float, "a float slider"); }
        else if (!strcmp(d.params[i].key, "framecount")) Check(d.params[i].kind == LooksParamKind::Source, "framecount is a source");
    Check(found, "fBlackLineMultiply is a parameter");
    int listed = 0, hiddenReasons = 0;
    for (const Listed& l : List()) { ++listed; hiddenReasons += l.index < 0; }
    printf("catalog: %d listed, %d can't run\n", listed, hiddenReasons);
    if (dev)
    {
        IDirect3DTexture9* t = nullptr; IDirect3DSurface9* s = nullptr;
        dev->CreateTexture(1920, 1080, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, nullptr);
        if (t) t->GetSurfaceLevel(0, &s);
        RunFrame f; f.dev = dev; f.target = s; f.width = 1920; f.height = 1080; f.frame = 1;
        std::string why;
        dev->BeginScene();
        Check(s && Run(f, anime - Looks::kUserShaderBase, v.data(), &why), "DH_Anime_13 runs through the catalog: " + why);
        f.width = 1280;
        Check(!Run(f, anime - Looks::kUserShaderBase, v.data(), &why) && why.find("compiling") == 0, "another size asks for a new compile");
        dev->EndScene();
        ReleaseGpu();
        if (s) s->Release();
        if (t) t->Release();
    }
}

static std::string Narrow(const std::wstring& w);

// --packs <Apex Radiance folder with Shaders\Packs\<pack>\ inside>: a pack's files are not compiled until wanted, then
// the scan lists their techniques under the pack and counts what runs
static void PackChecks()
{
    auto settle = [] {
        const DWORD t0 = GetTickCount();
        do { Poll(1920, 1080); Sleep(50); } while ((Scanning() || !Scanned()) && GetTickCount() - t0 < 600000);
        Poll(1920, 1080);
    };
    RequestScan();
    settle();
    const std::vector<std::string> packs = PackFolders();
    Check(!packs.empty(), "packs found in Shaders\\Packs");
    int before = 0;
    for (const Listed& l : List()) before += !l.pack.empty();
    Check(before == 0, "pack files are not compiled before they are wanted");
    BrowsePacks();
    settle();
    for (const std::string& p : packs)
    {
        const PackCount n = CountPack(p);
        int listed = 0;
        for (const Listed& l : List()) listed += l.pack == p;
        printf("pack %s: %d of %d run here (%s)\n", p.c_str(), n.runnable, n.total, n.checked ? "all compiled" : "not all compiled");
        Check(n.checked, p + ": every file compiled after BrowsePacks");
        Check(listed >= n.runnable, p + ": its techniques are listed under the pack");
    }
}

// --compare <folder>: each built-in port (third_party/reshade_shaders) against its author's original .fx in <folder>
// (laid out like the Shaders folder), compiled by the real path above. Both run on the same pictures with the same values
// (the defaults, every parameter at a quarter, then at three quarters of its range, then the two in turn); the table gives the largest and
// the mean difference of any channel in 8-bit levels. A port more than 1 level from its original is a problem.
static int ComparePorts(IDirect3DDevice9* dev, const std::wstring& root, UINT W, UINT H, const std::vector<std::wstring>& images)
{
    Folders f;
    f.root = root;
    f.includeDirs = { root + L"Shaders\\", root };
    f.textureDirs = { root + L"Textures\\", root };
    IDirect3DTexture9* picTex = nullptr; IDirect3DSurface9* pic = nullptr; IDirect3DSurface9* sys = nullptr;
    dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &picTex, nullptr);
    if (picTex) picTex->GetSurfaceLevel(0, &pic);
    dev->CreateOffscreenPlainSurface(W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (!pic || !sys) { printf("compare: no picture\n"); return 1; }

    // the pictures: a gradient, per-pixel noise (sharpeners), and the given screenshots
    std::vector<std::pair<std::string, std::vector<uint32_t>>> pics;
    {
        std::vector<uint32_t> g((size_t)W * H), n((size_t)W * H);
        uint32_t r = 12345;
        for (UINT y = 0; y < H; ++y)
            for (UINT x = 0; x < W; ++x)
            {
                g[(size_t)y * W + x] = D3DCOLOR_XRGB(x * 255 / W, y * 255 / H, (x + y) * 255 / (W + H));
                r = r * 1664525u + 1013904223u;
                n[(size_t)y * W + x] = 0xFF000000u | (r >> 8);
            }
        pics.push_back({ "gradient", std::move(g) });
        pics.push_back({ "noise", std::move(n) });
        for (const std::wstring& img : images)
        {
            std::vector<uint8_t> bgra;
            if (!DecodeImage(img, W, H, bgra)) { printf("compare: %s can't be read\n", Narrow(img).c_str()); continue; }
            std::vector<uint32_t> p((size_t)W * H);
            memcpy(p.data(), bgra.data(), p.size() * 4);
            for (uint32_t& c : p) c |= 0xFF000000u;
            pics.push_back({ "screenshot " + std::to_string(pics.size() - 1), std::move(p) });
        }
    }
    auto put = [&](const std::vector<uint32_t>& p) {
        D3DLOCKED_RECT lr;
        if (FAILED(sys->LockRect(&lr, nullptr, 0))) return;
        for (UINT y = 0; y < H; ++y) memcpy((char*)lr.pBits + (size_t)y * lr.Pitch, p.data() + (size_t)y * W, W * 4);
        sys->UnlockRect();
        dev->UpdateSurface(sys, nullptr, pic, nullptr);
    };
    auto get = [&](std::vector<uint32_t>& p) {
        p.assign((size_t)W * H, 0);
        D3DLOCKED_RECT lr;
        if (FAILED(dev->GetRenderTargetData(pic, sys)) || FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
        for (UINT y = 0; y < H; ++y) memcpy(p.data() + (size_t)y * W, (char*)lr.pBits + (size_t)y * lr.Pitch, W * 4);
        sys->UnlockRect();
    };

    Looks::Runner runner;
    runner.SetFactory([](IDirect3DDevice9* d, int shader, int entry) -> IDirect3DPixelShader9* {
        const std::string src = Looks::FullSource(shader);
        const char* name = shader < 0 ? "gs_copy" : Looks::Shader(shader).entries[entry];
        ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        if (SUCCEEDED(D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, name, "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err)))
            d->CreatePixelShader((const DWORD*)blob->GetBufferPointer(), &ps);
        else if (err) printf("port %d does not compile: %s\n", shader, (const char*)err->GetBufferPointer());
        if (blob) blob->Release();
        if (err) err->Release();
        return ps;
    });

    int problems = 0;
    printf("| Port | Original | Values | Picture | Max diff | Mean diff |\n|---|---|---|---|---|---|\n");
    for (int s = 0; s < Looks::ShaderCount(); ++s)
    {
        const LooksShader& d = Looks::Shader(s);
        const std::wstring path = root + L"Shaders\\" + std::wstring(d.file, d.file + strlen(d.file));
        auto fx = std::make_shared<Effect>();
        if (!Compile(path, f, W, H, *fx)) { printf("| %s | %s | | | does not compile: %s | |\n", d.technique, d.file, fx->error.c_str()); ++problems; continue; }
        size_t tech = SIZE_MAX;
        for (size_t t = 0; t < fx->techniques.size(); ++t) if (fx->techniques[t].name == d.technique) tech = t;
        Instance inst;
        std::string err;
        if (tech == SIZE_MAX || !fx->techniques[tech].reason.empty() || !inst.Create(dev, fx, &err))
        { printf("| %s | %s | | | can't run: %s | |\n", d.technique, d.file, err.c_str()); ++problems; continue; }

        // value sets: 4 floats per port parameter
        std::vector<std::pair<std::string, std::vector<float>>> sets;
        std::vector<float> def((size_t)d.paramCount * 4, 0.0f);
        for (int i = 0; i < d.paramCount; ++i) memcpy(&def[(size_t)i * 4], d.params[i].def, sizeof(float) * 4);
        sets.push_back({ "defaults", def });
        for (float set : { 0.25f, 0.75f, -1.0f }) // -1: a quarter and three quarters in turn (Levels: black 64, white 191)
        {
            std::vector<float> v = def;
            for (int i = 0; i < d.paramCount; ++i)
            {
                const LooksParam& p = d.params[i];
                const float t = set >= 0.0f ? set : (i % 2 ? 0.75f : 0.25f);
                for (int c = 0; c < p.comps; ++c)
                {
                    float& x = v[(size_t)i * 4 + c];
                    switch (p.kind)
                    {
                    case LooksParamKind::Bool: x = t < 0.5f ? 0.0f : 1.0f; break;
                    case LooksParamKind::Source: break;
                    case LooksParamKind::Int: case LooksParamKind::Combo: x = std::round(p.min + t * (p.max - p.min)); break;
                    default: x = p.min + t * (p.max - p.min); break;
                    }
                }
            }
            sets.push_back({ set < 0.0f ? "1/4 and 3/4 in turn" : set < 0.5f ? "1/4 of each range" : "3/4 of each range", v });
        }
        for (auto& [setName, vals] : sets)
        {
            std::vector<float> regs;
            DefaultValues(*fx, regs);
            for (int i = 0; i < d.paramCount; ++i)
            {
                if (d.params[i].kind == LooksParamKind::Source) continue;
                for (size_t u = 0; u < fx->uniforms.size(); ++u)
                    if (fx->uniforms[u].name == d.params[i].key) SetUniform(*fx, u, &vals[(size_t)i * 4], (size_t)d.params[i].comps, regs);
            }
            SourceState st; st.timerMs = 1000; st.frametimeMs = 16; st.frame = 60;
            UpdateSources(*fx, regs, st);
            for (auto& [picName, pixels] : pics)
            {
                std::vector<uint32_t> a, b;
                put(pixels);
                Looks::Frame fr; fr.dev = dev; fr.target = pic; fr.width = W; fr.height = H; fr.frame = 1;
                dev->BeginScene();
                const bool ranPort = runner.Run(fr, { { s, vals.data() } });
                dev->EndScene();
                get(a);
                put(pixels);
                dev->BeginScene();
                const bool ranFx = inst.Run(dev, tech, pic, nullptr, regs);
                dev->EndScene();
                get(b);
                int mx = 0;
                double sum = 0;
                for (size_t k = 0; k < a.size(); ++k)
                    for (int c = 0; c < 3; ++c)
                    {
                        const int dlt = std::abs((int)((a[k] >> (c * 8)) & 0xFF) - (int)((b[k] >> (c * 8)) & 0xFF));
                        mx = std::max(mx, dlt);
                        sum += dlt;
                    }
                // Levels with the black point on the white point scales by 255 / 0.00025 (the .fx avoids a division by
                // zero so): a last-bit difference of the two compilers shows there, so that case is reported, not counted
                const bool degenerate = !strcmp(d.technique, "Levels") && d.paramCount >= 2 && vals[0] == vals[4];
                const bool bad = !ranPort || !ranFx || (mx > 1 && !degenerate);
                problems += bad;
                printf("| %s | %s | %s | %s | %d%s | %.4f |\n", d.technique, d.file, setName.c_str(), picName.c_str(), mx,
                       !ranPort ? " (port did not run)" : !ranFx ? " (original did not run)" : bad ? " (over 1)" : mx > 1 ? " (black point = white point)" : "", sum / (a.size() * 3.0));
            }
        }
    }
    runner.Release();
    sys->Release(); pic->Release(); picTex->Release();
    printf("compare: %d problems\n", problems);
    return problems;
}

static void ListFx(const std::wstring& dir, int depth, std::vector<std::wstring>& out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth > 0) ListFx(dir + n + L"\\", depth - 1, out); }
        else if (n.size() > 3 && _wcsicmp(n.c_str() + n.size() - 3, L".fx") == 0) out.push_back(dir + n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static std::string Narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s += c < 128 ? (char)c : '?';
    return s;
}

// The reason, without the per-shader detail, for the summary
static std::string Category(const std::string& r)
{
    for (const char* k : { "compute shaders", "more than 4 textures in one pass", "integer texture", "1D or 3D texture", "too many instructions",
                           "more registers", "cannot unroll", "gradients in a loop", "needs the texture file", "needs the game's",
                           "too many settings", "more than 256 MB", "does not compile", "more than 16 textures" })
        if (r.find(k) != std::string::npos) return k;
    return r.size() > 70 ? r.substr(0, 70) : r;
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: looks_fx_check <Shaders folder> [--run] [WxH] [--catalog <Apex Radiance folder>] [--compare [--image <file>]...]\n"); return 2; }
    wchar_t full[MAX_PATH];
    GetFullPathNameW(std::wstring(argv[1], argv[1] + strlen(argv[1])).c_str(), MAX_PATH, full, nullptr);
    std::wstring root = full;
    if (root.back() != L'\\') root += L'\\';
    bool run = false, compare = false;
    std::vector<std::wstring> images;
    UINT W = 1920, H = 1080;
    for (int i = 2; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--run")) run = true;
        else if (!strcmp(argv[i], "--compare")) run = compare = true;
        else if (!strcmp(argv[i], "--packs") && i + 1 < argc)
        {
            wchar_t dir[MAX_PATH];
            GetFullPathNameW(std::wstring(argv[i + 1], argv[i + 1] + strlen(argv[i + 1])).c_str(), MAX_PATH, dir, nullptr);
            g_apexDir = dir;
            if (g_apexDir.back() != L'\\') g_apexDir += L'\\';
            PackChecks();
            printf("packs: %d checks, %d failures\n%s\n", g_checks, g_failures, g_failures ? "PROBLEMS" : "OK");
            return g_failures ? 1 : 0;
        }
        else if (!strcmp(argv[i], "--image") && i + 1 < argc) { ++i; images.push_back(std::wstring(argv[i], argv[i] + strlen(argv[i]))); }
        else if (!strcmp(argv[i], "--catalog") && i + 1 < argc)
        {
            wchar_t dir[MAX_PATH];
            GetFullPathNameW(std::wstring(argv[i + 1], argv[i + 1] + strlen(argv[i + 1])).c_str(), MAX_PATH, dir, nullptr);
            g_apexDir = dir;
            if (g_apexDir.back() != L'\\') g_apexDir += L'\\';
            ++i;
        }
        else sscanf_s(argv[i], "%ux%u", &W, &H);
    }
    Folders f;
    f.root = root;
    f.includeDirs = { root + L"Shaders\\", root };
    f.textureDirs = { root + L"Textures\\", root };
    std::vector<std::wstring> files;
    ListFx(root, 0, files);
    ListFx(root + L"Shaders\\", 3, files);
    std::sort(files.begin(), files.end());

    IDirect3DDevice9* dev = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DTexture9* picTex = nullptr; IDirect3DSurface9* pic = nullptr; IDirect3DSurface9* sys = nullptr;
    if (run)
    {
        HWND wnd = CreateWindowW(L"STATIC", L"looks_fx_check", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
        d3d = Direct3DCreate9(D3D_SDK_VERSION);
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 64; pp.BackBufferHeight = 64; pp.hDeviceWindow = wnd;
        if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &dev)))
        { printf("no Direct3D 9 device: --run skipped\n"); run = false; }
        else
        {
            dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &picTex, nullptr);
            picTex->GetSurfaceLevel(0, &pic);
            dev->CreateOffscreenPlainSurface(W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
        }
    }
    if (compare)
    {
        const int problems = dev ? ComparePorts(dev, root, W, H, images) : 1;
        if (pic) pic->Release();
        if (picTex) picTex->Release();
        if (sys) sys->Release();
        if (dev) dev->Release();
        if (d3d) d3d->Release();
        printf(problems ? "PROBLEMS\n" : "OK\n");
        return problems ? 1 : 0;
    }
    // a test picture: a colour gradient
    auto fill = [&] {
        D3DLOCKED_RECT lr;
        if (FAILED(sys->LockRect(&lr, nullptr, 0))) return;
        for (UINT y = 0; y < H; ++y)
            for (UINT x = 0; x < W; ++x)
                ((DWORD*)((char*)lr.pBits + (size_t)y * lr.Pitch))[x] = D3DCOLOR_XRGB(x * 255 / W, y * 255 / H, (x + y) * 255 / (W + H));
        sys->UnlockRect();
        dev->UpdateSurface(sys, nullptr, pic, nullptr);
    };
    auto changed = [&]() -> int {
        if (FAILED(dev->GetRenderTargetData(pic, sys))) return -1;
        D3DLOCKED_RECT lr;
        if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return -1;
        size_t n = 0;
        for (UINT y = 0; y < H; y += 7)
            for (UINT x = 0; x < W; x += 7)
                n += (((DWORD*)((char*)lr.pBits + (size_t)y * lr.Pitch))[x] & 0xFFFFFF) != ((DWORD)D3DCOLOR_XRGB(x * 255 / W, y * 255 / H, (x + y) * 255 / (W + H)) & 0xFFFFFF) ? 1 : 0;
        sys->UnlockRect();
        return n > 0;
    };

    int techs = 0, ok = 0, files_ok = 0, ran = 0, runFailed = 0, rescued = 0;
    std::map<std::string, int> reasons;
    printf("| File | Technique | Result |\n|---|---|---|\n");
    for (const std::wstring& path : files)
    {
        Effect fx;
        const DWORD t0 = GetTickCount();
        const bool good = Compile(path, f, W, H, fx);
        const DWORD ms = GetTickCount() - t0;
        const std::string name = Narrow(path.substr(root.size()));
        if (!good)
        {
            printf("| %s | (file) | %s |\n", name.c_str(), fx.error.c_str());
            ++reasons[Category(fx.error)];
            continue;
        }
        ++files_ok;
        auto shared = std::make_shared<Effect>(std::move(fx));
        Instance inst;
        std::string createErr;
        bool created = false;
        for (size_t t = 0; t < shared->techniques.size(); ++t)
        {
            const Technique& tq = shared->techniques[t];
            ++techs;
            std::string res = tq.reason.empty() ? "OK" : tq.reason;
            if (tq.reason.empty() && !shared->notes.empty())
            {
                res += " (D3D9 fallback:";
                for (const std::string& n : shared->notes) res += " " + n + ";";
                res.back() = ')';
                ++rescued;
            }
            if (tq.reason.empty())
            {
                ++ok;
                if (run)
                {
                    if (!created) { created = true; if (!inst.Create(dev, shared, &createErr)) createErr = createErr.empty() ? "create failed" : createErr; }
                    if (inst.Ready())
                    {
                        fill();
                        std::vector<float> regs;
                        DefaultValues(*shared, regs);
                        SourceState st; st.timerMs = 1000; st.frametimeMs = 16; st.frame = 60;
                        UpdateSources(*shared, regs, st);
                        dev->BeginScene();
                        const bool r = inst.Run(dev, t, pic, nullptr, regs);
                        dev->EndScene();
                        const HRESULT lost = dev->TestCooperativeLevel();
                        const int ch = changed();
                        if (r && SUCCEEDED(lost)) { ++ran; res += ch > 0 ? ", ran (picture changed)" : ", ran (picture unchanged with the defaults)"; }
                        else { ++runFailed; res += ", run FAILED"; }
                    }
                    else { ++runFailed; res += ", create FAILED: " + createErr; }
                }
            }
            else ++reasons[Category(tq.reason)];
            printf("| %s | %s%s | %s |\n", name.c_str(), tq.name.c_str(), tq.needsDepth ? " (depth)" : "", res.c_str());
        }
        fprintf(stderr, "%s: %lu ms, %.1f MB GPU\n", name.c_str(), ms, shared->gpuBytes / 1048576.0);
    }
    printf("\n%zu files, %d compiled; %d techniques, %d can run on Direct3D 9", files.size(), files_ok, techs, ok);
    printf(" (%d with a D3D9 fallback)", rescued);
    if (run) printf("; %d ran on the device, %d failed", ran, runFailed);
    printf("\nReasons:\n");
    for (auto& [r, n] : reasons) printf("  %3d  %s\n", n, r.c_str());
    {
        // the licence text shown in Settings > About is the vendored LICENSE.md (run from the repository root)
        std::string lic;
        FILE* lf = nullptr;
        if (fopen_s(&lf, "third_party/reshadefx/LICENSE.md", "rb") == 0 && lf) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, lf)) > 0) lic.append(b, n); fclose(lf); }
        while (!lic.empty() && (lic.back() == '\n' || lic.back() == '\r' || lic.back() == ' ')) lic.pop_back();
        Check(!lic.empty() && lic == ReShadeFx::kLicense, "reshadefx_license.h matches third_party/reshadefx/LICENSE.md");
        if (g_failures) { printf("licence text differs\n"); runFailed += g_failures; g_failures = 0; }
    }
    if (!g_apexDir.empty())
    {
        CatalogChecks(dev);
        printf("catalog: %d checks, %d failures\n", g_checks, g_failures);
        runFailed += g_failures;
    }
    if (sys) sys->Release();
    if (pic) pic->Release();
    if (picTex) picTex->Release();
    if (dev) dev->Release();
    if (d3d) d3d->Release();
    printf(runFailed ? "PROBLEMS\n" : "OK\n");
    return runFailed ? 1 : 0;
}
