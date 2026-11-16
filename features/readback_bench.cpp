// Read-back benchmark (see readback_bench.h). Credits: @loinyx
#include "readback_bench.h"
#include "build_flavor.h"

#ifdef APEX_NO_DEV_TOOLS
namespace ReadbackBench {
void Install() {} // the players' build has no developer tools
}
#else
#include "apex_config.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "d3d9_hooks.h"
#include "render_callbacks.h"
#include "world_session.h"
#include <toml++/toml.hpp>
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

namespace ReadbackBench {
namespace {

constexpr const char* kHookName = "ReadbackBench";
constexpr int kGetCalls = 1000;       // Get* calls per sample (the sample is the mean of one call)
constexpr int kSetCalls = 100;        // SetVertexShaderConstantF calls per sample and batch size
constexpr double kQueryWaitUs = 100000; // an event query not signalled after 100 ms counts as timed out

// One case runs per benchmark frame, in turn, so every read-back case starts with an ordinary frame's backlog
enum Case { Grtd1, Grtd4, Grtd64, Grtd256, Grtd1024, GrtdBack, PairSmall, BackThenSmall, StretchSmall, LockPending,
            LockSignalled, QueryPoll, GetCalls, SetConsts, kCases };
constexpr UINT kSizes[] = {1, 4, 64, 256, 1024}; // render-target edges; index 5 is the back buffer's size
enum Target { T1, T4, T64, T256, T1024, TBack, kTargets };

struct Row { std::string name; std::vector<double> us; };

int g_iterations = 60, g_everyFrames = 4, g_delayS = 20;
bool g_done = false;
ULONGLONG g_worldSince = 0;
uint64_t g_frame = 0;
int g_step = 0;
IDirect3DDevice9* g_dev = nullptr; // identity: resources belong to the device that made them
IDirect3DSurface9* g_rt[kTargets] = {};
IDirect3DSurface9* g_sys[kTargets] = {};
IDirect3DQuery9* g_query = nullptr;
UINT g_backW = 0, g_backH = 0;
std::vector<Row> g_rows;
uint64_t c_lockQueryDone = 0, c_firstGetDataOk = 0, c_queryTimeouts = 0, c_failures = 0;

LARGE_INTEGER g_freq{};
double NowUs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1e6 / static_cast<double>(g_freq.QuadPart);
}
void Add(const std::string& name, double us) {
    for (Row& r : g_rows)
        if (r.name == name) { r.us.push_back(us); return; }
    g_rows.push_back({name, {us}});
}

void Release() {
    for (auto* arr : {g_rt, g_sys})
        for (int i = 0; i < kTargets; i++)
            if (arr[i]) arr[i]->Release(), arr[i] = nullptr;
    if (g_query) g_query->Release(), g_query = nullptr;
    g_dev = nullptr;
}

// D3DPOOL_DEFAULT render targets and matching system-memory surfaces, A8R8G8B8 (the format of the game's 256x256
// compositor read-back), made once before the first sample: creates wait for the renderer and are not measured
bool Create(IDirect3DDevice9* dev) {
    IDirect3DSurface9* back = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return false;
    D3DSURFACE_DESC d{};
    back->GetDesc(&d);
    back->Release();
    g_backW = d.Width, g_backH = d.Height;
    g_dev = dev;
    for (int i = 0; i < kTargets; i++) {
        const UINT w = i == TBack ? g_backW : kSizes[i], h = i == TBack ? g_backH : kSizes[i];
        if (FAILED(dev->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_rt[i], nullptr)) ||
            FAILED(dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_sys[i], nullptr)))
            return false;
    }
    if (FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_query))) return false;
    LOG_INFO(std::format("[ReadbackBench] Start: back buffer {}x{} (format {}), {} samples a case, one case every {} frames",
                         g_backW, g_backH, static_cast<int>(d.Format), g_iterations, g_everyFrames));
    return true;
}

// The frame's back buffer into the target first (an async copy behind the frame's draws), so the read-back depends on
// the frame as the game's own read-backs do
void Fill(IDirect3DDevice9* dev, int t) {
    IDirect3DSurface9* back = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
        if (FAILED(dev->StretchRect(back, nullptr, g_rt[t], nullptr, D3DTEXF_NONE))) c_failures++;
        back->Release();
    }
}
double Grtd(IDirect3DDevice9* dev, int t) {
    const double t0 = NowUs();
    if (FAILED(dev->GetRenderTargetData(g_rt[t], g_sys[t]))) c_failures++;
    return NowUs() - t0;
}
std::string SizeName(int t) { return t == TBack ? std::format("{}x{} (back buffer)", g_backW, g_backH) : std::format("{}x{}", kSizes[t], kSizes[t]); }

// Polls the issued event query until it signals; returns the polls, or -1 after kQueryWaitUs
int WaitQuery(double& waitedUs) {
    const double t0 = NowUs();
    for (int polls = 1;; polls++) {
        if (g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_OK) { waitedUs = NowUs() - t0; return polls; }
        if (NowUs() - t0 > kQueryWaitUs) { waitedUs = NowUs() - t0; c_queryTimeouts++; return -1; }
        YieldProcessor();
    }
}

void Lock(IDirect3DDevice9* dev, bool waitForQuery) {
    Fill(dev, T256);
    Grtd(dev, T256);
    g_query->Issue(D3DISSUE_END);
    if (waitForQuery) {
        double waited = 0;
        WaitQuery(waited);
    } else if (g_query->GetData(nullptr, 0, 0) == S_OK) c_lockQueryDone++; // already signalled: the "pending" sample is not
    D3DLOCKED_RECT lr{};
    const double t0 = NowUs();
    const HRESULT hr = g_sys[T256]->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    const double us = NowUs() - t0;
    if (SUCCEEDED(hr)) g_sys[T256]->UnlockRect(); else c_failures++;
    Add(waitForQuery ? "LockRect 256x256 system memory, query signalled" : "LockRect 256x256 system memory, query just issued", us);
}

void RunCase(IDirect3DDevice9* dev, int c) {
    switch (c) {
    case Grtd1: case Grtd4: case Grtd64: case Grtd256: case Grtd1024: case GrtdBack:
        Fill(dev, c);
        Add("GetRenderTargetData " + SizeName(c), Grtd(dev, c));
        break;
    case PairSmall:
        Fill(dev, T4), Fill(dev, T1);
        Add("Pair 4x4 then 1x1: first", Grtd(dev, T4));
        Add("Pair 4x4 then 1x1: second", Grtd(dev, T1));
        break;
    case BackThenSmall:
        Fill(dev, TBack), Fill(dev, T4);
        Add("Back buffer size then 4x4: back buffer size", Grtd(dev, TBack));
        Add("Back buffer size then 4x4: 4x4", Grtd(dev, T4));
        break;
    case StretchSmall: {
        IDirect3DSurface9* back = nullptr;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) { c_failures++; break; }
        const double t0 = NowUs();
        if (FAILED(dev->StretchRect(back, nullptr, g_rt[T4], nullptr, D3DTEXF_LINEAR))) c_failures++;
        const double t1 = NowUs();
        const double grtd = Grtd(dev, T4);
        back->Release();
        Add("StretchRect back buffer to 4x4", t1 - t0);
        Add("StretchRect back buffer to 4x4 + GetRenderTargetData", t1 - t0 + grtd);
        break;
    }
    case LockPending: Lock(dev, false); break;
    case LockSignalled: Lock(dev, true); break;
    case QueryPoll: {
        double t0 = NowUs();
        g_query->Issue(D3DISSUE_END);
        Add("Event query: Issue", NowUs() - t0);
        t0 = NowUs();
        const HRESULT first = g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
        Add("Event query: first GetData (flush)", NowUs() - t0);
        if (first == S_OK) c_firstGetDataOk++;
        double waited = 0;
        const int polls = first == S_OK ? 0 : WaitQuery(waited);
        if (polls >= 0) {
            Add("Event query: wait until signalled", waited);
            Add("Event query: polls until signalled (count, not us)", polls);
        }
        t0 = NowUs();
        g_query->GetData(nullptr, 0, 0);
        Add("Event query: GetData once signalled", NowUs() - t0);
        break;
    }
    case GetCalls: {
        DWORD v = 0;
        double t0 = NowUs();
        for (int i = 0; i < kGetCalls; i++) dev->GetRenderState(D3DRS_ZENABLE, &v);
        Add("GetRenderState (one call)", (NowUs() - t0) / kGetCalls);
        t0 = NowUs();
        for (int i = 0; i < kGetCalls; i++) {
            IDirect3DBaseTexture9* tex = nullptr;
            if (SUCCEEDED(dev->GetTexture(0, &tex)) && tex) tex->Release();
        }
        Add("GetTexture + Release (one call)", (NowUs() - t0) / kGetCalls);
        float f[16];
        t0 = NowUs();
        for (int i = 0; i < kGetCalls; i++) dev->GetVertexShaderConstantF(0, f, 4);
        Add("GetVertexShaderConstantF 4 registers (one call)", (NowUs() - t0) / kGetCalls);
        break;
    }
    case SetConsts: {
        // The game's registers 0..63 are kept and set back afterwards. Each call alternates between two values so the
        // renderer cannot drop it as a repeat; the original call skips Apex's own constant callbacks
        float saved[64 * 4], other[64 * 4];
        if (FAILED(dev->GetVertexShaderConstantF(0, saved, 64))) { c_failures++; break; }
        for (int i = 0; i < 64 * 4; i++) other[i] = saved[i] + 1.0f;
        for (const UINT n : {1u, 16u, 64u}) {
            const double t0 = NowUs();
            for (int i = 0; i < kSetCalls; i++) D3D9Hooks::CallOriginalSetVertexShaderConstantF(dev, 0, (i & 1) ? saved : other, n);
            Add(std::format("SetVertexShaderConstantF {} registers (one call)", n), (NowUs() - t0) / kSetCalls);
        }
        D3D9Hooks::CallOriginalSetVertexShaderConstantF(dev, 0, saved, 64);
        break;
    }
    }
}

double Pct(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size() - 1) + 0.5))];
}

void Report() {
    const std::wstring dir = ApexPaths::DiagnosticsDirectory();
    FILE* f = nullptr;
    std::wstring path;
    if (ApexPaths::EnsureApexDirectory()) {
        CreateDirectoryW(dir.c_str(), nullptr);
        path = dir + std::format(L"ReadbackBench-{}.csv", GetCurrentProcessId());
        f = _wfopen(path.c_str(), L"w");
    }
    if (f) std::fprintf(f, "case,samples,p50_us,p95_us,max_us,mean_us\n");
    LOG_INFO(std::format("[ReadbackBench] {:<62} {:>7} {:>10} {:>10} {:>10}", "Case (us)", "samples", "p50", "p95", "max"));
    for (const Row& r : g_rows) {
        double sum = 0;
        for (double x : r.us) sum += x;
        const double p50 = Pct(r.us, 0.5), p95 = Pct(r.us, 0.95), mx = Pct(r.us, 1.0), mean = sum / static_cast<double>(r.us.size());
        LOG_INFO(std::format("[ReadbackBench] {:<62} {:>7} {:>10.3f} {:>10.3f} {:>10.3f}", r.name, r.us.size(), p50, p95, mx));
        if (f) std::fprintf(f, "\"%s\",%zu,%.3f,%.3f,%.3f,%.3f\n", r.name.c_str(), r.us.size(), p50, p95, mx, mean);
    }
    if (f) std::fclose(f);
    LOG_INFO(std::format("[ReadbackBench] Done: first GetData already signalled {} of {}, query signalled before the 'just issued' lock {} of {}, "
                         "query waits timed out {}, failed calls {}; CSV {}",
                         c_firstGetDataOk, g_iterations, c_lockQueryDone, g_iterations, c_queryTimeouts, c_failures, f ? "written" : "not written"));
}

void OnPreReset(IDirect3DDevice9*);
void Finish() {
    g_done = true;
    Release();
    RenderCallbacks::preReset.Remove(OnPreReset);
    D3D9Hooks::UnregisterAll(kHookName); // (from inside its own Present callback: the running chain keeps its list)
}

void OnPreReset(IDirect3DDevice9*) {
    if (g_done || !g_dev) return;
    LOG_WARNING("[ReadbackBench] Aborted: the device was reset during the benchmark");
    Release();
    g_done = true;
}

D3D9Hooks::HookAction OnPresent(D3D9Hooks::DeviceContext& ctx) {
    if (g_done || kPublicBuild.load(std::memory_order_relaxed)) return D3D9Hooks::HookAction::Continue;
    if (!WorldSession::InWorld()) { g_worldSince = 0; return D3D9Hooks::HookAction::Continue; } // samples only in a shown world
    const ULONGLONG now = GetTickCount64();
    if (!g_worldSince) g_worldSince = now;
    if (now - g_worldSince < static_cast<ULONGLONG>(g_delayS) * 1000 || ++g_frame % static_cast<uint64_t>(g_everyFrames)) return D3D9Hooks::HookAction::Continue;
    if (g_dev && g_dev != ctx.device) Release(); // another device: start the resources again
    if (!g_dev && !Create(ctx.device)) {
        LOG_ERROR("[ReadbackBench] Could not create the benchmark's surfaces or query: stopped");
        Release();
        Finish();
        return D3D9Hooks::HookAction::Continue;
    }
    RunCase(ctx.device, g_step % kCases);
    if (++g_step >= g_iterations * kCases) {
        Report();
        Finish();
    }
    return D3D9Hooks::HookAction::Continue;
}

} // namespace

void Install() {
    toml::table root;
    if (!ApexConfig::ReadRoot(root)) return;
    const toml::table* t = root["readback_bench"].as_table();
    if (!t || !(*t)["run"].value_or(false)) return;
    if (kPublicBuild.load(std::memory_order_relaxed)) {
        LOG_INFO("[ReadbackBench] Not started: developer mode is off");
        return;
    }
    g_iterations = static_cast<int>(std::clamp<int64_t>((*t)["iterations"].value_or(int64_t{60}), 5, 1000));
    g_everyFrames = static_cast<int>(std::clamp<int64_t>((*t)["every_frames"].value_or(int64_t{4}), 1, 600));
    g_delayS = static_cast<int>(std::clamp<int64_t>((*t)["delay_s"].value_or(int64_t{20}), 0, 600));
    QueryPerformanceFrequency(&g_freq);
    RenderCallbacks::preReset.Add(OnPreReset);
    D3D9Hooks::RegisterPresent(kHookName, [](D3D9Hooks::DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) { return OnPresent(ctx); });
    LOG_INFO(std::format("[ReadbackBench] Armed: {} samples of {} cases, one case every {} frames, {} s after a world is shown",
                         g_iterations, static_cast<int>(kCases), g_everyFrames, g_delayS));
}

} // namespace ReadbackBench
#endif
