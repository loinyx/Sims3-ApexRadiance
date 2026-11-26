// Compositor tile readback without the GPU wait (see compositor_readback.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "compositor_readback.h"
#include "call_chain.h"
#include "game_addresses.h"
#include "render_callbacks.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "imgui.h"
#include "ui/widgets.h"
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <format>
#include <mutex>

namespace CompositorReadback {
namespace {

using CallChain::Layer;
using CallChain::Site;
using FnJob = int(__fastcall*)(void* builder, void* edx, void* job); // thiscall(builder; job), ret 4
using FnFmt = int(__fastcall*)(void* builder, void* edx, int format); // thiscall(builder; format), ret 4

// Texture builder fields (FUN_005fdef0 / FUN_005fdbf0 / FUN_005fc680)
constexpr uintptr_t kStaging = 0x20;  // the system-memory texture object GetRenderTargetData writes and FUN_00618df0 locks
constexpr uintptr_t kTiles = 0x60;    // tile array (0x60 bytes each)
constexpr uintptr_t kTile = 0x70;     // current tile
constexpr uintptr_t kState = 0x74;    // state machine (2 = render + read back, 3 = use the tile)
constexpr uintptr_t kDeviceId = 0x88; // device generation; a change resets the builder
// The game's texture object (FUN_00618df0: locks [t+0x30] when [t+0x2C] == 0)
constexpr uintptr_t kTexLocked = 0x2C, kTexD3D = 0x30;

constexpr uint32_t kMaxWaitFrames = 8; // then the game's path (its lock waits, as before)
constexpr double kMaxWaitMs = 250.0;

template <class T> T Field(void* p, uintptr_t off) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(p) + off); }

struct Pending {
    bool active = false;
    void* builder = nullptr;
    void* job = nullptr;
    uint32_t tile = 0, deviceId = 0;
    uintptr_t tiles = 0;
    uint32_t waits = 0;
    uint64_t issued = 0; // QPC
};

std::mutex g_ctrl;
bool g_started = false;
bool g_resetHooked = false; // g_ctrl
std::atomic<bool> g_on{false};
std::atomic<bool> g_defer{true}; // developer switch: off = every call passes through
std::mutex g_lock;               // g_p, g_query, g_queryDev (render thread in practice)
Pending g_p;
std::atomic<bool> g_hasPending{false};
IDirect3DQuery9* g_query = nullptr;
IDirect3DDevice9* g_queryDev = nullptr; // identity only
double g_qpcMs = 0.0;

thread_local void* t_queueBuilder = nullptr;  // the builder the queue loop is stepping
thread_local void* t_state2Builder = nullptr; // inside its state-2 call
thread_local void* t_state2Job = nullptr;
thread_local bool t_deferredNow = false;
thread_local void* t_resumeBuilder = nullptr; // FUN_005fdbf0 run again by Apex: skip the readback

std::atomic<uint64_t> c_deferred{0}, c_resumed{0}, c_wait{0}, c_timedOut{0}, c_dropped{0}, c_gamePath{0};
std::atomic<uint64_t> g_resumeTicks{0}, g_resumeMaxTicks{0}, g_state2Ticks{0};
uintptr_t g_fetchFn = 0;

uint64_t Qpc() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<uint64_t>(q.QuadPart);
}

void DropLocked() {
    g_p.active = false;
    g_hasPending.store(false, std::memory_order_release);
    c_dropped.fetch_add(1, std::memory_order_relaxed);
}

// True when the GPU has finished the copy, or when there is nothing left to wait on (no query, device lost)
bool SignalledLocked() {
    if (!g_query) return true;
    const HRESULT hr = g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    return hr != S_FALSE;
}

// After the game's readback for (builder, job) in the queue's state 2: issue the event query and remember the tile
bool TryDefer(void* b, void* job) {
    void* tex = Field<void*>(b, kStaging);
    if (!tex || Field<uint32_t>(tex, kTexLocked) != 0) return false;
    auto* d3dTex = Field<IDirect3DTexture9*>(tex, kTexD3D);
    if (!d3dTex) return false;
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_p.active) return false;
    IDirect3DDevice9* dev = nullptr;
    if (FAILED(d3dTex->GetDevice(&dev)) || !dev) return false;
    if (dev != g_queryDev && g_query) {
        g_query->Release();
        g_query = nullptr;
    }
    g_queryDev = dev;
    if (!g_query && FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_query))) g_query = nullptr;
    dev->Release();
    if (!g_query || FAILED(g_query->Issue(D3DISSUE_END))) return false;
    g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH); // submit the copy now
    g_p.active = true;
    g_p.builder = b;
    g_p.job = job;
    g_p.tile = Field<uint32_t>(b, kTile);
    g_p.tiles = Field<uintptr_t>(b, kTiles);
    g_p.deviceId = Field<uint32_t>(b, kDeviceId);
    g_p.waits = 0;
    g_p.issued = Qpc();
    g_hasPending.store(true, std::memory_order_release);
    return true;
}

// ---- hooks (render thread) ----

// 0x006082F8: the queue loop steps the head builder. While its copy is in flight, stop for this frame (2).
int __fastcall Hook_Queue(void* b, void* edx, void* job) {
    const FnJob next = reinterpret_cast<FnJob>(CallChain::Next(Site::CompQueue, Layer::CompositorReadback));
    if (g_hasPending.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(g_lock);
        if (g_p.active && g_p.builder == b) {
            if (Field<uint32_t>(b, kState) != 3) DropLocked(); // reset: state 3 comes only after a new state 2
            else if (g_on.load(std::memory_order_relaxed) && !SignalledLocked()) {
                const double waited = static_cast<double>(Qpc() - g_p.issued) * g_qpcMs;
                if (g_p.waits < kMaxWaitFrames && waited < kMaxWaitMs) {
                    g_p.waits++;
                    c_wait.fetch_add(1, std::memory_order_relaxed);
                    return 2;
                }
                c_timedOut.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    void* const prev = t_queueBuilder;
    t_queueBuilder = b;
    const int r = next(b, edx, job);
    t_queueBuilder = prev;
    return r;
}

// 0x005FDFEC: state 2 (render the tile, read it back). Deferral is allowed only here, called by the queue loop.
int __fastcall Hook_State2(void* b, void* edx, void* job) {
    const FnJob next = reinterpret_cast<FnJob>(CallChain::Next(Site::CompState2, Layer::CompositorReadback));
    if (t_queueBuilder != b || !g_on.load(std::memory_order_relaxed) || !g_defer.load(std::memory_order_relaxed)) return next(b, edx, job);
    const uint64_t t0 = Qpc();
    t_state2Builder = b;
    t_state2Job = job;
    t_deferredNow = false;
    const int r = next(b, edx, job);
    t_state2Builder = nullptr;
    t_state2Job = nullptr;
    if (t_deferredNow) {
        t_deferredNow = false;
        g_state2Ticks.fetch_add(Qpc() - t0, std::memory_order_relaxed);
    }
    return r;
}

// 0x005FDC8A: FUN_005fdbf0's GetRenderTargetData (FUN_005fbce0). 0 = copied; any other value ends FUN_005fdbf0.
int __fastcall Hook_Readback(void* b, void* edx, int format) {
    const FnFmt next = reinterpret_cast<FnFmt>(CallChain::Next(Site::CompReadback, Layer::CompositorReadback));
    if (t_resumeBuilder == b) return 0; // copied in an earlier frame, the query signalled: straight to the locks
    if (g_hasPending.load(std::memory_order_acquire)) { // a fresh readback of a builder with a deferred tile: forget it
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_p.active && g_p.builder == b) DropLocked();
    }
    const int r = next(b, edx, format);
    if (r != 0 || t_state2Builder != b) return r;
    if (TryDefer(b, t_state2Job)) {
        t_deferredNow = true;
        c_deferred.fetch_add(1, std::memory_order_relaxed);
        return 2; // FUN_005fdbf0 and FUN_005fdde0 return it; the builder goes to state 3, the queue stops for the frame
    }
    c_gamePath.fetch_add(1, std::memory_order_relaxed);
    return r;
}

// 0x005FDFFF: state 3. A deferred tile first gets the rest of its state 2 (FUN_005fdbf0 without the copy).
int __fastcall Hook_State3(void* b, void* edx, void* job) {
    const FnJob next = reinterpret_cast<FnJob>(CallChain::Next(Site::CompState3, Layer::CompositorReadback));
    if (g_hasPending.load(std::memory_order_acquire)) {
        bool resume = false, skipCopy = false;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (g_p.active && g_p.builder == b) {
                resume = true;
                // The copy in the staging texture is this tile's only if nothing moved; otherwise copy again (the game's state 2)
                skipCopy = g_p.tile == Field<uint32_t>(b, kTile) && g_p.tiles == Field<uintptr_t>(b, kTiles) && g_p.deviceId == Field<uint32_t>(b, kDeviceId);
                g_p.active = false;
                g_hasPending.store(false, std::memory_order_release);
            }
        }
        if (resume) {
            const uint64_t t0 = Qpc();
            void* const prev = t_resumeBuilder;
            t_resumeBuilder = skipCopy ? b : nullptr;
            const int r = reinterpret_cast<FnJob>(g_fetchFn)(b, nullptr, job);
            t_resumeBuilder = prev;
            const uint64_t dt = Qpc() - t0;
            g_resumeTicks.fetch_add(dt, std::memory_order_relaxed);
            uint64_t m = g_resumeMaxTicks.load(std::memory_order_relaxed);
            while (dt > m && !g_resumeMaxTicks.compare_exchange_weak(m, dt, std::memory_order_relaxed)) {}
            c_resumed.fetch_add(1, std::memory_order_relaxed);
            if (r != 0) return r; // 3: the dispatcher resets the builder; 4: the build fails, as the game's state 2 would
        }
    }
    return next(b, edx, job);
}

void OnPreReset(IDirect3DDevice9*) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_query) g_query->Release();
    g_query = nullptr; // a pending tile resumes without waiting on it (its device id changes, so the game resets it anyway)
    g_queryDev = nullptr;
}

struct Hook {
    Site site;
    void* fn;
    const char* what;
};
const Hook kHooks[] = {
    {Site::CompState3, reinterpret_cast<void*>(&Hook_State3), "state 3"}, // completing hooks first, deferring ones last
    {Site::CompQueue, reinterpret_cast<void*>(&Hook_Queue), "queue step"},
    {Site::CompReadback, reinterpret_cast<void*>(&Hook_Readback), "readback"},
    {Site::CompState2, reinterpret_cast<void*>(&Hook_State2), "state 2"},
};

void RemoveAll() {
    for (int i = static_cast<int>(std::size(kHooks)) - 1; i >= 0; i--) CallChain::Remove(kHooks[i].site, Layer::CompositorReadback);
}

} // namespace

bool Start(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_started) return true;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::string missing;
    if (!GameAddr::GroupAvailable("CompositorReadback", &missing)) return fail(GameAddr::NotAvailable(missing));
    g_fetchFn = GameAddr::Get(GameAddr::Id::CompTileFetch);
    if (!g_fetchFn) return fail(GameAddr::NotAvailable("CompTileFetch"));
    if (g_qpcMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    for (const Hook& h : kHooks) {
        std::string err;
        if (!CallChain::Install(h.site, Layer::CompositorReadback, h.fn, &err)) {
            if (!g_hasPending.load()) RemoveAll();
            return fail(std::format("Could not hook the texture builder's {}: {}", h.what, err));
        }
    }
    if (!g_resetHooked) RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
    g_resetHooked = true;
    g_on.store(true, std::memory_order_release);
    g_started = true;
    LOG_INFO(std::format("[CompositorReadback] On: texture builder tiles read back without waiting for the GPU (queue {:#010x}, readback {:#010x}, state 3 {:#010x})",
                         CallChain::CallAddress(Site::CompQueue), CallChain::CallAddress(Site::CompReadback), CallChain::CallAddress(Site::CompState3)));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_started) return;
    g_on.store(false, std::memory_order_release); // no new deferral; a pending tile still completes through state 3
    g_started = false;
    const Stats s = GetStats();
    if (g_hasPending.load(std::memory_order_acquire)) {
        // Removing state 3 now would hand FUN_005fd420 a tile that was never locked: the hooks stay and pass calls through
        LOG_INFO(std::format("[CompositorReadback] Off ({} tiles deferred); hooks kept until a tile in flight completes (they pass calls through)", s.deferred));
        return;
    }
    RemoveAll();
    RenderCallbacks::Remove(RenderCallbacks::preReset, OnPreReset);
    g_resetHooked = false;
    LOG_INFO(std::format("[CompositorReadback] Off ({} tiles deferred, {} resumed, {} frames waited, {} timed out, {} dropped; deferred locks {:.1f} ms, slowest {:.2f} ms)",
                         s.deferred, s.resumed, s.waitFrames, s.timedOut, s.dropped, s.resumeMs, s.resumeMaxMs));
}

bool Running() { return g_on.load(std::memory_order_acquire); }

Stats GetStats() {
    Stats s;
    s.deferred = c_deferred.load();
    s.resumed = c_resumed.load();
    s.waitFrames = c_wait.load();
    s.timedOut = c_timedOut.load();
    s.dropped = c_dropped.load();
    s.gamePath = c_gamePath.load();
    s.resumeMs = static_cast<double>(g_resumeTicks.load()) * g_qpcMs;
    s.resumeMaxMs = static_cast<double>(g_resumeMaxTicks.load()) * g_qpcMs;
    s.state2Ms = static_cast<double>(g_state2Ticks.load()) * g_qpcMs;
    return s;
}

std::string StatusText() {
    if (!Running()) return "Off";
    const Stats s = GetStats();
    if (!s.deferred) return "On (no texture built yet)";
    return std::format("On: {} texture tiles read back without waiting ({} frames waited on the GPU, locks {:.2f} ms each on average)", s.deferred, s.waitFrames,
                       s.resumed ? s.resumeMs / static_cast<double>(s.resumed) : 0.0);
}

void RenderDeveloperUI() {
    if (kPublicBuild) return;
    if (!ImGui::GetCurrentContext()) return;
    const Stats s = GetStats();
    ImGui::TextUnformatted(("Compositor readback: " + StatusText()).c_str());
    ImGui::TextDisabled("Deferred %llu, resumed %llu, waited %llu frames, timed out %llu, dropped %llu, left to the game %llu",
                        static_cast<unsigned long long>(s.deferred), static_cast<unsigned long long>(s.resumed), static_cast<unsigned long long>(s.waitFrames),
                        static_cast<unsigned long long>(s.timedOut), static_cast<unsigned long long>(s.dropped), static_cast<unsigned long long>(s.gamePath));
    ImGui::TextDisabled("Deferred locks %.1f ms (slowest %.2f ms); state 2 without its lock %.1f ms", s.resumeMs, s.resumeMaxMs, s.state2Ms);
    bool defer = g_defer.load();
    if (ApexUi::Checkbox("Defer compositor tile locks (off = the game's own wait, for A/B)##CrDefer", &defer)) g_defer.store(defer);
}

} // namespace CompositorReadback
