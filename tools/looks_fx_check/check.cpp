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
#include <deque>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <psapi.h>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "psapi.lib")

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
#ifdef LOOKS_FX_CHECK_BASELINE // built against an older Looks (the sources before a change) to compare its pictures and memory
static bool IsChecked(const Listed&) { return false; }
#else
static bool IsChecked(const Listed& l) { return l.checked; }
#endif
static bool g_hashes = false; // --hashes: one line per picture run, to compare two builds' pictures (diff of the outputs)

// FNV-1a of a picture's RGB: two builds that print the same hash drew the same pixels (0 levels apart)
static unsigned long long PictureHash(const uint32_t* px, size_t n)
{
    unsigned long long h = 14695981039346656037ull;
    for (size_t i = 0; i < n; ++i) { const uint32_t c = px[i] & 0xFFFFFFu; for (int b = 0; b < 3; ++b) h = (h ^ ((c >> (b * 8)) & 0xFF)) * 1099511628211ull; }
    return h;
}

// The process's private bytes and its largest free block of address space (a 32-bit game: room matters more than bytes)
static void Memory(const char* when)
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof pmc);
    size_t largest = 0;
    MEMORY_BASIC_INFORMATION mi{};
    for (const char* p = nullptr; VirtualQuery(p, &mi, sizeof mi) == sizeof mi;)
    {
        if (mi.State == MEM_FREE) largest = std::max(largest, (size_t)mi.RegionSize);
        const char* next = (const char*)mi.BaseAddress + mi.RegionSize;
        if (next <= p) break;
        p = next;
    }
    printf("memory %s: private %.1f MB, largest free block %.1f MB\n", when, pmc.PrivateUsage / 1048576.0, largest / 1048576.0);
}
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
    for (const Listed& l : List()) { ++listed; hiddenReasons += l.index < 0 && !IsChecked(l); }
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
static void ListFx(const std::wstring& dir, int depth, std::vector<std::wstring>& out);

// --packs <Apex Radiance folder with Shaders\Packs\<pack>\ inside>: a pack's files are not compiled until wanted; browsing
// fills "<x> of <y> run here" and the Add a shader list from the check cache (Shaders\.cache), checking the files it does
// not know in the background and letting each effect go; a file a look wants compiles. Writes Shaders\.cache in that
// folder (use a scratch copy). The memory lines measure the browse.
static void PackChecks()
{
    auto settle = [] {
        const DWORD t0 = GetTickCount();
        do { Poll(1920, 1080); Sleep(50); } while ((Scanning() || !Scanned()) && GetTickCount() - t0 < 1800000);
        Poll(1920, 1080);
    };
    Memory("at start");
    DWORD t0 = GetTickCount();
    RequestScan();
    settle();
    printf("scan: %lu ms\n", GetTickCount() - t0);
    Memory("after the scan");
    const std::vector<std::string> packs = PackFolders();
    Check(!packs.empty(), "packs found in Shaders\\Packs");
    int before = 0;
    for (const Listed& l : List()) before += !l.pack.empty() && l.index >= 0;
    Check(before == 0, "pack files are not compiled before they are wanted");
    t0 = GetTickCount();
    BrowsePacks();
    settle();
    printf("browse: %lu ms\n", GetTickCount() - t0);
    Memory("after browsing every pack");
    int compiled = 0, listedAll = 0;
    for (const Listed& l : List()) { compiled += !l.pack.empty() && l.index >= 0; listedAll += !l.pack.empty() && (l.index >= 0 || IsChecked(l)); }
#ifndef LOOKS_FX_CHECK_BASELINE
    printf("browse: %d pack files checked in the background this run, %d pack techniques listed, %d compiled and kept\n", CheckedThisSession(), listedAll, compiled);
    Check(compiled == 0, "browsing keeps no pack effect compiled");
#else
    printf("browse: %d pack techniques listed, %d compiled and kept\n", listedAll, compiled);
#endif
    for (const std::string& p : packs)
    {
        const PackCount n = CountPack(p);
        int listed = 0;
        for (const Listed& l : List()) listed += l.pack == p && (l.index >= 0 || IsChecked(l));
        printf("pack %s: %d of %d run here (%s), %d listed\n", p.c_str(), n.runnable, n.total, n.checked ? "all checked" : "not all checked", listed);
        Check(n.checked, p + ": every file checked after BrowsePacks");
        Check(listed >= 1 || n.runnable == 0, p + ": its techniques are listed under the pack");
    }
#ifndef LOOKS_FX_CHECK_BASELINE
    // a look that names a pack file: that file compiles, the others stay as they were
    std::string file;
    for (const Listed& l : List()) if (IsChecked(l)) { file = l.file; break; }
    if (!file.empty())
    {
        t0 = GetTickCount();
        WantFiles({ file });
        settle();
        int now = 0;
        for (const Listed& l : List()) now += l.index >= 0 && !l.pack.empty() && _stricmp(l.file.c_str(), file.c_str()) == 0;
        printf("want %s: compiled in %lu ms\n", file.c_str(), GetTickCount() - t0);
        Check(now >= 1, "a wanted pack file compiles and its techniques run");
        // what a preset export names: read from the folders (also while Looks is off)
        const auto where = FindFiles({ file, "NoSuchFile.fx" });
        Check(where.size() == 1 && !where.begin()->second.empty(), "FindFiles: the pack that has the file, nothing for a missing one");
    }
    int hits = 0, misses = 0;
    CacheCounts(&hits, &misses);
    printf("compile cache: %d effects read from it, %d compiled and written\n", hits, misses);
#endif
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
                if (g_hashes)
                    printf("hash port %s | %s | %s %016llx original %016llx\n", d.technique, setName.c_str(), picName.c_str(), PictureHash(a.data(), a.size()), PictureHash(b.data(), b.size()));
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

#ifndef LOOKS_FX_CHECK_BASELINE
// ---- --state: what the passes change on the device, against what patches/looks_patch.cpp (SavedState) puts back by hand
struct Snap
{
    DWORD rs[256] = {}; bool rsOk[256] = {};
    DWORD ss[20][14] = {}; IDirect3DBaseTexture9* tex[20] = {};
    float psc[224 * 4] = {}, vsc[256 * 4] = {};
    int psi[16 * 4] = {}, vsi[16 * 4] = {}; BOOL psb[16] = {}, vsb[16] = {};
    DWORD tss[8][33] = {};
    IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexShader9* vs = nullptr; IDirect3DVertexDeclaration9* decl = nullptr; DWORD fvf = 0;
    IDirect3DVertexBuffer9* vb[16] = {}; UINT off[16] = {}, stride[16] = {}, freq[16] = {};
    IDirect3DIndexBuffer9* ib = nullptr;
    IDirect3DSurface9* rt[4] = {}; IDirect3DSurface9* ds = nullptr;
    D3DVIEWPORT9 vp{}; RECT sc{};
    float clip[6][4] = {};
};
static DWORD SamplerStage(int i) { return i < 16 ? (DWORD)i : D3DVERTEXTEXTURESAMPLER0 + (DWORD)(i - 16); }
static void Take(IDirect3DDevice9* dev, Snap& s)
{
    for (int r = 0; r < 256; ++r) s.rsOk[r] = SUCCEEDED(dev->GetRenderState((D3DRENDERSTATETYPE)r, &s.rs[r]));
    for (int i = 0; i < 20; ++i)
    {
        for (int t = 1; t < 14; ++t) dev->GetSamplerState(SamplerStage(i), (D3DSAMPLERSTATETYPE)t, &s.ss[i][t]);
        dev->GetTexture(SamplerStage(i), &s.tex[i]);
        if (s.tex[i]) s.tex[i]->Release(); // identity only
    }
    dev->GetPixelShaderConstantF(0, s.psc, 224); dev->GetVertexShaderConstantF(0, s.vsc, 256);
    dev->GetPixelShaderConstantI(0, s.psi, 16); dev->GetVertexShaderConstantI(0, s.vsi, 16);
    dev->GetPixelShaderConstantB(0, s.psb, 16); dev->GetVertexShaderConstantB(0, s.vsb, 16);
    for (int st = 0; st < 8; ++st) for (int t = 1; t < 33; ++t) dev->GetTextureStageState(st, (D3DTEXTURESTAGESTATETYPE)t, &s.tss[st][t]);
    auto rel = [](auto*& p) { if (p) p->Release(); };
    dev->GetPixelShader(&s.ps); rel(s.ps); dev->GetVertexShader(&s.vs); rel(s.vs); dev->GetVertexDeclaration(&s.decl); rel(s.decl); dev->GetFVF(&s.fvf);
    for (UINT i = 0; i < 16; ++i) { dev->GetStreamSource(i, &s.vb[i], &s.off[i], &s.stride[i]); rel(s.vb[i]); dev->GetStreamSourceFreq(i, &s.freq[i]); }
    dev->GetIndices(&s.ib); rel(s.ib);
    for (DWORD i = 0; i < 4; ++i) { s.rt[i] = nullptr; dev->GetRenderTarget(i, &s.rt[i]); rel(s.rt[i]); }
    s.ds = nullptr; dev->GetDepthStencilSurface(&s.ds); rel(s.ds);
    dev->GetViewport(&s.vp); dev->GetScissorRect(&s.sc);
    for (DWORD i = 0; i < 6; ++i) dev->GetClipPlane(i, s.clip[i]);
}
// Every difference must be one the hand restore covers: full = a player's effect ran
static int Uncovered(const Snap& a, const Snap& b, bool full, int psConsts, std::vector<std::string>& changed)
{
    int bad = 0;
    auto note = [&](bool covered, const std::string& what) { changed.push_back(what); if (!covered) { ++bad; printf("  not put back: %s\n", what.c_str()); } };
    const int rsN = full ? Looks::kSavedRenderStateCount : Looks::kBuiltInRenderStates;
    const int ssN = full ? Looks::kSavedSamplerStateCount : Looks::kBuiltInSamplerStates;
    const int stages = full ? 16 : Looks::kBuiltInSamplers;
    for (int r = 0; r < 256; ++r)
        if (a.rsOk[r] && a.rs[r] != b.rs[r])
        {
            bool in = false;
            for (int i = 0; i < rsN; ++i) in |= Looks::kSavedRenderStates[i] == r;
            note(in, "render state " + std::to_string(r));
        }
    for (int i = 0; i < 20; ++i)
    {
        for (int t = 1; t < 14; ++t)
            if (a.ss[i][t] != b.ss[i][t])
            {
                bool in = false;
                for (int k = 0; k < ssN; ++k) in |= Looks::kSavedSamplerStates[k] == t;
                // the vertex stages 16..19 (D3DVERTEXTEXTURESAMPLER0..3): SavedState keeps their texture and these five, full only
                const bool vertexKept = full && i >= 16 && (t == D3DSAMP_MINFILTER || t == D3DSAMP_MAGFILTER || t == D3DSAMP_MIPFILTER ||
                                                            t == D3DSAMP_ADDRESSU || t == D3DSAMP_ADDRESSV);
                note((in && i < stages) || vertexKept, "sampler " + std::to_string(i) + " state " + std::to_string(t));
            }
        if (a.tex[i] != b.tex[i]) note(i < stages || (full && i >= 16), "texture " + std::to_string(i));
    }
    for (int c = 0; c < 224 * 4; ++c) if (memcmp(&a.psc[c], &b.psc[c], 4)) { note(c / 4 < psConsts, "PS constant c" + std::to_string(c / 4)); c = c / 4 * 4 + 3; }
    for (int c = 0; c < 256 * 4; ++c) if (memcmp(&a.vsc[c], &b.vsc[c], 4)) { note(full, "VS constant c" + std::to_string(c / 4)); c = c / 4 * 4 + 3; }
    if (memcmp(a.psi, b.psi, sizeof a.psi) || memcmp(a.vsi, b.vsi, sizeof a.vsi) || memcmp(a.psb, b.psb, sizeof a.psb) || memcmp(a.vsb, b.vsb, sizeof a.vsb)) note(false, "integer or bool constants");
    if (memcmp(a.tss, b.tss, sizeof a.tss)) note(false, "texture stage states");
    if (memcmp(a.clip, b.clip, sizeof a.clip)) note(false, "clip planes");
    for (int i = 1; i < 16; ++i) if (a.vb[i] != b.vb[i] || a.off[i] != b.off[i] || a.stride[i] != b.stride[i]) note(false, "stream " + std::to_string(i));
    for (int i = 0; i < 16; ++i) if (a.freq[i] != b.freq[i]) note(false, "stream frequency " + std::to_string(i));
    if (a.ib != b.ib) note(false, "index buffer");
    // put back by hand in every run: shaders, declaration / FVF, stream 0, render target 0, viewport, scissor; depth-stencil
    // (unbound by the caller around every run); render targets 1..3 when a player's effect runs
    if (a.ps != b.ps) note(true, "pixel shader");
    if (a.vs != b.vs) note(true, "vertex shader");
    if (a.decl != b.decl || a.fvf != b.fvf) note(true, "vertex declaration / FVF");
    if (a.vb[0] != b.vb[0] || a.off[0] != b.off[0] || a.stride[0] != b.stride[0]) note(true, "stream 0");
    if (a.rt[0] != b.rt[0]) note(true, "render target 0");
    for (int i = 1; i < 4; ++i) if (a.rt[i] != b.rt[i]) note(full, "render target " + std::to_string(i));
    if (a.ds != b.ds) note(true, "depth-stencil");
    if (memcmp(&a.vp, &b.vp, sizeof a.vp)) note(true, "viewport");
    if (memcmp(&a.sc, &b.sc, sizeof a.sc)) note(true, "scissor rect");
    return bad;
}

// A game-like device state with a distinct value in everything a pass could touch
static void GameState(IDirect3DDevice9* dev, IDirect3DTexture9* dummy, IDirect3DVertexBuffer9* vb)
{
    for (int r = 7; r < 210; ++r) { DWORD v = 0; if (SUCCEEDED(dev->GetRenderState((D3DRENDERSTATETYPE)r, &v))) dev->SetRenderState((D3DRENDERSTATETYPE)r, v); }
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE); dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA); dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW); dev->SetRenderState(D3DRS_COLORWRITEENABLE, 7);
    dev->SetRenderState(D3DRS_STENCILREF, 3); dev->SetRenderState(D3DRS_FOGENABLE, TRUE); dev->SetRenderState(D3DRS_BLENDFACTOR, 0x80402010);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE); dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    for (int i = 0; i < 20; ++i)
    {
        const DWORD st = SamplerStage(i);
        dev->SetTexture(st, dummy);
        dev->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP); dev->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_MIRROR);
        dev->SetSamplerState(st, D3DSAMP_ADDRESSW, D3DTADDRESS_BORDER); dev->SetSamplerState(st, D3DSAMP_BORDERCOLOR, 0x11223344);
        dev->SetSamplerState(st, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC); dev->SetSamplerState(st, D3DSAMP_MAXANISOTROPY, 4);
        dev->SetSamplerState(st, D3DSAMP_MAXMIPLEVEL, 1); dev->SetSamplerState(st, D3DSAMP_SRGBTEXTURE, TRUE);
    }
    std::vector<float> c(256 * 4);
    for (size_t i = 0; i < c.size(); ++i) c[i] = 1000.0f + (float)i;
    dev->SetPixelShaderConstantF(0, c.data(), 224); dev->SetVertexShaderConstantF(0, c.data(), 256);
    dev->SetStreamSource(0, vb, 0, 16); dev->SetStreamSource(1, vb, 16, 16);
    RECT sc{ 1, 2, 300, 200 }; dev->SetScissorRect(&sc);
    D3DVIEWPORT9 vp{ 3, 4, 500, 400, 0.1f, 0.9f }; dev->SetViewport(&vp);
}

static double Now() { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart * 1000.0 / f.QuadPart; }

static int StateChecks(IDirect3DDevice9* dev, const std::wstring& root, UINT W, UINT H)
{
    int problems = 0;
    IDirect3DTexture9* picTex = nullptr; IDirect3DSurface9* pic = nullptr; IDirect3DTexture9* dummy = nullptr; IDirect3DVertexBuffer9* vb = nullptr;
    dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &picTex, nullptr);
    if (picTex) picTex->GetSurfaceLevel(0, &pic);
    dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &dummy, nullptr);
    dev->CreateVertexBuffer(256, 0, 0, D3DPOOL_MANAGED, &vb, nullptr);
    if (!pic || !dummy || !vb) { printf("state: no picture\n"); return 1; }
    dev->SetRenderTarget(0, pic);
    Looks::Runner runner;
    runner.SetFactory([](IDirect3DDevice9* d, int shader, int entry) -> IDirect3DPixelShader9* {
        const std::string src = Looks::FullSource(shader);
        const char* name = shader < 0 ? "gs_copy" : Looks::Shader(shader).entries[entry];
        ID3DBlob* blob = nullptr; IDirect3DPixelShader9* ps = nullptr;
        if (SUCCEEDED(D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, name, "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, nullptr)))
            d->CreatePixelShader((const DWORD*)blob->GetBufferPointer(), &ps);
        if (blob) blob->Release();
        return ps;
    });
    // every built-in port with its defaults, in one run
    std::vector<std::vector<float>> defs(Looks::ShaderCount());
    std::vector<Looks::Step> all;
    for (int s = 0; s < Looks::ShaderCount(); ++s)
    {
        const LooksShader& d = Looks::Shader(s);
        for (int i = 0; i < d.paramCount; ++i) defs[s].insert(defs[s].end(), d.params[i].def, d.params[i].def + 4);
        if (defs[s].empty()) defs[s].push_back(0.0f);
        all.push_back({ s, defs[s].data() });
    }
    Looks::Frame f; f.dev = dev; f.target = pic; f.width = W; f.height = H; f.frame = 1;
    for (float mix : { 1.0f, 0.5f })
    {
        f.mix = mix;
        GameState(dev, dummy, vb);
        Snap a, b;
        Take(dev, a);
        dev->BeginScene(); const bool drew = runner.Run(f, all); dev->EndScene();
        Take(dev, b);
        std::vector<std::string> changed;
        const int bad = Uncovered(a, b, false, Looks::PsConstsUsed(all), changed);
        int above31 = 0; // a hand restore of c0..c31 alone (as before PsConstsUsed) left these to the state block
        for (const std::string& c : changed) above31 += c.rfind("PS constant c", 0) == 0 && atoi(c.c_str() + 13) >= 32;
        printf("state: the 17 built-in ports (mix %.0f%%): %zu device states changed, %d not put back by hand; PS constants saved c0..c%d (%d changed above c31)\n",
               mix * 100.0f, changed.size(), bad, Looks::PsConstsUsed(all) - 1, above31);
        problems += bad + !drew;
    }
    // with players' effects after them (full: a run with a user effect)
    Folders fo; fo.root = root; fo.includeDirs = { root + L"Shaders\\", root }; fo.textureDirs = { root + L"Textures\\", root };
    std::vector<std::wstring> files;
    extern void ListFxPublic(const std::wstring&, int, std::vector<std::wstring>&);
    ListFxPublic(root, 0, files); ListFxPublic(root + L"Shaders\\", 3, files);
    std::sort(files.begin(), files.end());
    std::vector<std::unique_ptr<Instance>> insts;
    std::vector<std::pair<Instance*, size_t>> techs;
    for (const std::wstring& path : files)
    {
        if (techs.size() >= 24) break;
        auto fx = std::make_shared<Effect>();
        if (!Compile(path, fo, W, H, *fx)) continue;
        auto inst = std::make_unique<Instance>();
        std::string err;
        if (!inst->Create(dev, fx, &err)) continue;
        for (size_t t = 0; t < fx->techniques.size() && techs.size() < 24; ++t) if (fx->techniques[t].reason.empty()) techs.push_back({ inst.get(), t });
        insts.push_back(std::move(inst));
    }
    if (!techs.empty())
    {
        f.mix = 1.0f;
        GameState(dev, dummy, vb);
        Snap a, b;
        Take(dev, a);
        dev->BeginScene();
        runner.Run(f, all);
        for (auto& [inst, t] : techs)
        {
            std::vector<float> regs;
            DefaultValues(*inst->Fx(), regs);
            inst->Run(dev, t, pic, nullptr, regs);
        }
        dev->EndScene();
        Take(dev, b);
        std::vector<std::string> changed;
        const int bad = Uncovered(a, b, true, 224, changed);
        printf("state: the ports and %zu techniques of players' effects: %zu device states changed, %d not put back by hand\n", techs.size(), changed.size(), bad);
        problems += bad;
    }
    insts.clear();

    // costs on this device: the D3DSBT_ALL state block (captured and applied around every run before), and the chain that
    // Strength 0 no longer runs
    IDirect3DStateBlock9* sb = nullptr;
    dev->CreateStateBlock(D3DSBT_ALL, &sb);
    if (sb)
    {
        GameState(dev, dummy, vb);
        for (int i = 0; i < 50; ++i) { sb->Capture(); sb->Apply(); }
        const int n = 2000;
        const double t0 = Now();
        for (int i = 0; i < n; ++i) { sb->Capture(); sb->Apply(); }
        printf("cost: D3DSBT_ALL Capture + Apply %.1f us each (CPU)\n", (Now() - t0) * 1000.0 / n);
        sb->Release();
    }
    IDirect3DQuery9* q = nullptr;
    dev->CreateQuery(D3DQUERYTYPE_EVENT, &q);
    auto timeRun = [&](const char* what, const std::vector<Looks::Step>& steps, float mix) {
        f.mix = mix;
        auto once = [&] { dev->BeginScene(); runner.Run(f, steps); dev->EndScene(); if (q) { q->Issue(D3DISSUE_END); while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {} } };
        for (int i = 0; i < 5; ++i) once();
        const int n = 40;
        const double t0 = Now();
        for (int i = 0; i < n; ++i) once();
        printf("cost: %s at %ux%u, Strength %.0f%%: %.2f ms a frame (CPU + GPU, flushed); Strength 0 skips it\n", what, W, H, mix * 100.0f, (Now() - t0) / n);
    };
    std::vector<Looks::Step> natural;
    for (const char* t : { "Levels", "Vibrance", "LumaSharpen" })
        for (int s = 0; s < Looks::ShaderCount(); ++s) if (!strcmp(Looks::Shader(s).technique, t)) natural.push_back(all[s]);
    timeRun("Natural (Levels, Vibrance, LumaSharpen)", natural, 0.5f);
    timeRun("every built-in port", all, 0.5f);
    if (q) q->Release();
    runner.Release();
    vb->Release(); dummy->Release(); pic->Release(); picTex->Release();
    printf("state: %d problems\n", problems);
    return problems;
}

// ---- --cache <Shaders folder>: the compile cache (Folders::cacheDir, under <folder>\.cache-check\, written): every .fx
// compiled cold, then warm from the cache; the bytecode, notes and reasons must be the same as without a cache; a change
// in the .fx or in an .fxh it includes must compile again
static bool SameEffect(const Effect& a, const Effect& b)
{
    if (a.error != b.error || a.notes != b.notes || a.shaders.size() != b.shaders.size() || a.techniques.size() != b.techniques.size()) return false;
    for (size_t i = 0; i < a.shaders.size(); ++i) if (a.shaders[i].code != b.shaders[i].code || a.shaders[i].error != b.shaders[i].error) return false;
    for (size_t i = 0; i < a.techniques.size(); ++i) if (a.techniques[i].reason != b.techniques[i].reason) return false;
    return true;
}
static int CacheChecks(const std::wstring& root, const std::vector<std::wstring>& files, UINT W, UINT H)
{
    int problems = 0;
    Folders plain; plain.root = root; plain.includeDirs = { root + L"Shaders\\", root }; plain.textureDirs = { root + L"Textures\\", root };
    Folders cached = plain;
    cached.cacheDir = root + L".cache-check\\";
    double tPlain = 0, tCold = 0, tWarm = 0, worstCold = 0, worstWarm = 0;
    std::string worst;
    int same = 0;
    for (const std::wstring& path : files)
    {
        Effect a, b, c;
        double t0 = Now();
        Compile(path, plain, W, H, a);
        double t1 = Now();
        Compile(path, cached, W, H, b); // cold: compiles and writes (or reads an entry an earlier run wrote)
        double t2 = Now();
        Compile(path, cached, W, H, c); // warm: from the cache
        double t3 = Now();
        tPlain += t1 - t0; tCold += t2 - t1; tWarm += t3 - t2;
        if (t1 - t0 > worstCold) { worstCold = t1 - t0; worstWarm = t3 - t2; worst = Narrow(path.substr(root.size())); }
        const bool ok = SameEffect(a, b) && SameEffect(a, c);
        same += ok;
        if (!ok) { ++problems; printf("cache: %s differs from a compile without the cache\n", Narrow(path).c_str()); }
    }
    int hits = 0, misses = 0;
    CacheCounts(&hits, &misses);
    printf("cache: %zu files, %d the same with and without the cache; no cache %.0f ms, first %.0f ms, again %.0f ms; slowest %s %.0f ms -> %.0f ms\n",
           files.size(), same, tPlain, tCold, tWarm, worst.c_str(), worstCold, worstWarm);
    // invalidation: a test effect and its include, in the cache folder's own subfolder
    const std::wstring dir = cached.cacheDir + L"src\\";
    CreateDirectoryW(cached.cacheDir.c_str(), nullptr); CreateDirectoryW(dir.c_str(), nullptr);
    auto write = [](const std::wstring& p, const std::string& text) { FILE* fp = nullptr; if (_wfopen_s(&fp, p.c_str(), L"wb") == 0 && fp) { fwrite(text.data(), 1, text.size(), fp); fclose(fp); } };
    const std::wstring fx = dir + L"cache_test.fx", fxh = dir + L"cache_test.fxh";
    auto source = [](float k) { return "#include \"cache_test.fxh\"\nfloat4 PS(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(uv * " + std::to_string(k) + " * KVALUE, 0, 1); }\n"
                                      "void VS(in uint id : SV_VertexID, out float4 p : SV_Position, out float2 uv : TEXCOORD) { uv = float2((id == 2) ? 2.0 : 0.0, (id == 1) ? 2.0 : 0.0); p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
                                      "technique CacheTest { pass { VertexShader = VS; PixelShader = PS; } }\n"; };
    write(fx, source(0.5f)); write(fxh, "#define KVALUE 0.25\n");
    Folders t = cached; t.root = cached.cacheDir; t.includeDirs = { dir };
    auto run = [&](const char* what, bool expectHit) {
        int h0, m0; CacheCounts(&h0, &m0);
        Effect e, ref;
        Compile(fx, t, W, H, e);
        Folders noCache = t; noCache.cacheDir.clear();
        Compile(fx, noCache, W, H, ref);
        int h1, m1; CacheCounts(&h1, &m1);
        const bool hit = h1 > h0;
        const bool ok = hit == expectHit && SameEffect(e, ref) && e.techniques.size() == 1 && e.techniques[0].reason.empty();
        printf("cache: %s: %s%s\n", what, hit ? "read from the cache" : "compiled", ok ? "" : " (PROBLEM)");
        problems += !ok;
    };
    run("a new file", false);
    run("the same file again", true);
    Sleep(20);
    write(fxh, "#define KVALUE 0.75\n");
    run("its .fxh changed", false);
    run("again", true);
    write(fx, source(0.6f));
    run("the .fx changed", false);
    t.defines = { { "APEX_CACHE_TEST", "1" } }; // another set of definitions: its own cache file, the first one kept
    run("with a definition", false);
    run("with it again", true);
    t.defines.clear();
    run("without it again", true);
    run("another picture size", (W = W / 2, false));
    DeleteFileW(fx.c_str()); DeleteFileW(fxh.c_str()); RemoveDirectoryW(dir.c_str());
    printf("cache: %d problems\n", problems);
    return problems;
}

// ---- --params <folder>...: every .fx under the folders (a file name once), each uniform mapped to the menu's control
// (DescribeParams + LooksControlOf, as the Shaders page shows them); a uniform that gets no control and is neither hidden
// nor set by ReShade is reported. Then the preprocessor definitions: the ones each effect tests, and a round trip of one
// (an .ini's PreprocessorDefinitions -> compile with it -> another cache key, the .fx default kept -> written back).
static bool AsIntChk(const std::string& s, int& v) { char* e = nullptr; const long x = strtol(s.c_str(), &e, 10); v = (int)x; return !s.empty() && !*e; }
static const char* DefineControl(const Macro& m)
{
    int a = 0, b = 0;
    if ((m.value == "0" || m.value == "1") && (m.def == "0" || m.def == "1") && (m.hi < m.lo || (m.lo >= 0 && m.hi <= 1))) return "switch";
    if (m.hi >= m.lo && m.hi - m.lo <= 7 && AsIntChk(m.value, a) && AsIntChk(m.def, b)) return "segmented";
    char* e = nullptr;
    strtod(m.value.c_str(), &e);
    return !m.value.empty() && !*e ? "number field" : "text field";
}
static int ParamChecks(const std::vector<std::wstring>& dirs, UINT W, UINT H)
{
    int problems = 0;
    // root: the folders' common parent (includes must stay inside it); includes: each folder, its Shaders\ and their subfolders
    std::wstring root = dirs[0];
    for (const std::wstring& d : dirs)
        while (!root.empty() && _wcsnicmp(d.c_str(), root.c_str(), root.size()) != 0) root.resize(root.find_last_of(L'\\', root.size() - 2) + 1);
    Folders f;
    f.root = root;
    std::vector<std::wstring> files;
    std::map<std::wstring, bool> seen;
    for (const std::wstring& d : dirs)
    {
        for (const std::wstring& inc : { d + L"Shaders\\", d }) f.includeDirs.push_back(inc);
        f.textureDirs.push_back(d + L"Textures\\");
        std::vector<std::wstring> list;
        ListFx(d, 5, list);
        std::sort(list.begin(), list.end());
        for (const std::wstring& p : list)
        {
            std::wstring n = p.substr(p.find_last_of(L'\\') + 1);
            for (wchar_t& c : n) c = towlower(c);
            if (seen.emplace(n, true).second) files.push_back(p);
        }
    }
    std::map<std::string, int> controls, defineControls;
    int uniforms = 0, shown = 0, hidden = 0, sourced = 0, unmapped = 0, compiled = 0, withText = 0, categories = 0, macroFiles = 0, macros = 0;
    const Effect* sample = nullptr; // an effect whose 0/1 definition makes the round trip
    std::vector<std::shared_ptr<Effect>> keep;
    for (const std::wstring& path : files)
    {
        auto fx = std::make_shared<Effect>();
        if (!Compile(path, f, W, H, *fx)) continue; // the file cannot run here: nothing to show
        ++compiled;
        std::deque<std::string> text;
        std::vector<LooksParam> params;
        std::vector<int> uni;
        DescribeParams(*fx, text, params, uni);
        std::string lastCat;
        for (size_t p = 0; p < params.size(); ++p)
        {
            const LooksParam& pd = params[p];
            const Uniform& un = fx->uniforms[uni[p]];
            ++uniforms;
            const bool hasSource = FindAnn(un.ann, "source") != nullptr;
            const LooksControl c = LooksControlOf(pd);
            if (hasSource) { ++sourced; continue; }
            if (c == LooksControl::None)
            {
                if (pd.flags & kParamHidden) { ++hidden; continue; }
                ++unmapped;
                printf("params: %s %s: no control (%s)\n", Narrow(path.substr(root.size())).c_str(), pd.key,
                       un.array ? "an array" : un.cols > 1 ? "a matrix" : "unknown");
                continue;
            }
            static const char* const kNames[] = { "none", "switch", "slider", "choice", "color", "vector", "field", "text" };
            std::string name = kNames[(int)c];
            if (c == LooksControl::Choice)
            {
                const auto items = LooksItems(pd);
                size_t chars = 0;
                for (const char* s : items) chars += strlen(s);
                name += items.size() <= 4 && chars <= 32 ? " (segmented)" : " (dropdown)";
                if (pd.max >= (float)items.size()) printf("params: note: %s %s: ui_max %g past its %zu items\n", Narrow(path).c_str(), pd.key, pd.max, items.size());
            }
            ++controls[name];
            ++shown;
            withText += pd.text != nullptr;
            const std::string cat = pd.category ? pd.category : "";
            if (!cat.empty() && cat != lastCat) ++categories;
            lastCat = cat;
        }
        macros += (int)fx->macros.size();
        macroFiles += !fx->macros.empty();
        for (const Macro& m : fx->macros)
        {
            ++defineControls[DefineControl(m)];
            if (!strcmp(DefineControl(m), "text field")) printf("params: text definition: %s %s=%s\n", fx->file.c_str(), m.name.c_str(), m.value.c_str());
            if (!sample && !strcmp(DefineControl(m), "switch") && fx->cacheKey) { sample = fx.get(); keep.push_back(fx); }
        }
    }
    printf("params: %zu files (a name once), %d compiled; %d uniforms: %d shown, %d hidden, %d set by ReShade, %d with no control\n",
           files.size(), compiled, uniforms, shown, hidden, sourced, unmapped);
    for (auto& [n, k] : controls) printf("params:   %5d %s\n", k, n.c_str());
    printf("params: %d text blocks (ui_text), %d groups (ui_category)\n", withText, categories);
    printf("params: %d definitions in %d files\n", macros, macroFiles);
    for (auto& [n, k] : defineControls) printf("params:   %5d %s\n", k, n.c_str());
    problems += unmapped;

    // the round trip: .ini -> compile with the definitions -> another cache key and the value applied -> .ini again
    if (!sample) { printf("params: no effect with a 0/1 definition (round trip skipped)\n"); return problems + 1; }
    const Macro& m = *std::find_if(sample->macros.begin(), sample->macros.end(), [](const Macro& x) { return !strcmp(DefineControl(x), "switch"); });
    const std::string flipped = m.value == "1" ? "0" : "1";
    Looks::Preset ini;
    Looks::ParsePreset("PreprocessorDefinitions=UNUSED_NAME=7\nTechniques=X@" + sample->file + "\n[" + sample->file + "]\nPreprocessorDefinitions=" + m.name + "=" + flipped + "\n", ini);
    Folders g = f;
    g.defines = Looks::PresetDefines(ini, sample->file);
    Effect again;
    Compile(sample->path, g, W, H, again);
    const Macro* m2 = nullptr;
    for (const Macro& x : again.macros) if (x.name == m.name) m2 = &x;
    const bool ok = m2 && m2->value == flipped && m2->def == m.value && again.cacheKey != sample->cacheKey && MacrosMatch(again, g.defines) &&
                    !MacrosMatch(*sample, g.defines) && MacrosMatch(*sample, {}) && Looks::ParseDefines(Looks::FormatDefines(g.defines)) == g.defines &&
                    Looks::FormatDefines(g.defines) == "UNUSED_NAME=7," + m.name + "=" + flipped;
    printf("params: definitions round trip on %s, %s %s -> %s: %s\n", sample->file.c_str(), m.name.c_str(), m.value.c_str(), flipped.c_str(), ok ? "ok" : "PROBLEM");
    return problems + !ok;
}
#endif

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

void ListFxPublic(const std::wstring& dir, int depth, std::vector<std::wstring>& out) { ListFx(dir, depth, out); }

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
    if (argc < 2) { printf("usage: looks_fx_check <Shaders folder> [--run] [WxH] [--catalog <Apex Radiance folder>] [--compare [--image <file>]...] [--hashes] [--state] [--cache] [--packs <Apex Radiance folder>] [--params <folder>...]\n"); return 2; }
    wchar_t full[MAX_PATH];
    GetFullPathNameW(std::wstring(argv[1], argv[1] + strlen(argv[1])).c_str(), MAX_PATH, full, nullptr);
    std::wstring root = full;
    if (root.back() != L'\\') root += L'\\';
    bool run = false, compare = false, state = false, cache = false;
    std::vector<std::wstring> images;
    UINT W = 1920, H = 1080;
    for (int i = 2; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--run")) run = true;
        else if (!strcmp(argv[i], "--hashes")) g_hashes = true;
        else if (!strcmp(argv[i], "--state")) run = state = true;
        else if (!strcmp(argv[i], "--cache")) cache = true;
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
#ifndef LOOKS_FX_CHECK_BASELINE
        else if (!strcmp(argv[i], "--params") && i + 1 < argc)
        {
            std::vector<std::wstring> dirs;
            for (++i; i < argc && strncmp(argv[i], "--", 2) != 0; ++i)
            {
                wchar_t dir[MAX_PATH];
                GetFullPathNameW(std::wstring(argv[i], argv[i] + strlen(argv[i])).c_str(), MAX_PATH, dir, nullptr);
                dirs.push_back(dir);
                if (dirs.back().back() != L'\\') dirs.back() += L'\\';
            }
            const int problems = ParamChecks(dirs, W, H);
            printf(problems ? "PROBLEMS\n" : "OK\n");
            return problems ? 1 : 0;
        }
#endif
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
#ifndef LOOKS_FX_CHECK_BASELINE
    if (state || cache)
    {
        int problems = 0;
        if (state) problems += dev ? StateChecks(dev, root, W, H) : 1;
        if (cache) problems += CacheChecks(root, files, W, H);
        if (pic) pic->Release();
        if (picTex) picTex->Release();
        if (sys) sys->Release();
        if (dev) dev->Release();
        if (d3d) d3d->Release();
        printf(problems ? "PROBLEMS\n" : "OK\n");
        return problems ? 1 : 0;
    }
#endif
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
                        if (g_hashes)
                        {
                            std::vector<uint32_t> px((size_t)W * H);
                            D3DLOCKED_RECT lr;
                            if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
                            {
                                for (UINT y = 0; y < H; ++y) memcpy(px.data() + (size_t)y * W, (char*)lr.pBits + (size_t)y * lr.Pitch, W * 4);
                                sys->UnlockRect();
                            }
                            printf("hash run %s | %s %016llx\n", name.c_str(), tq.name.c_str(), PictureHash(px.data(), px.size()));
                        }
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
