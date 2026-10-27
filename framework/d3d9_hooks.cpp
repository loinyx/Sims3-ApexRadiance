#include "d3d9_hooks.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "hook_chain.h"
#include "memory_patch.h"
#include "frame_profiler.h"
#include "hook_guard.h"
#include "perf_toggles.h"
#include "apex_memory.h"
#include <detours/detours.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <mutex>
#include <vector>

namespace D3D9Hooks {
namespace {

// ---- callback chains ----
// g_lock: held while callbacks are (un)registered, while a locked chain runs (Present, BeginScene, Create*) and while any
// chain runs on a thread other than the render thread. The draw and state chains run on the render thread without it
// (see d3d9_hooks.h and docs/architecture.md section 3.6).
std::recursive_mutex g_lock;
uint64_t g_sequence = 0; // registration order (ties between equal priorities)

template <typename Fn> struct Entry {
    // read by every dispatch (first, together): the plain call, the "off" flag and the re-issued draw filter
    typename Fn::Raw fn = nullptr;
    void* self = nullptr;
    std::atomic<bool>* offFlag = nullptr; // off.get() (the dispatch reads it without the shared pointer's indirection)
    bool observeReissue = false;          // runs for draws re-issued inside a ReissueScope (ObserveReissuedDraws)
    std::string name;
    std::string timingName; // the name the per-name timing reports ("<name> (Present)" on the Present chain)
    int timingSlot = -1;    // its slot in the Frame Profiler's table (development build, timed chains)
    Fn hook;                // owns the callable behind fn / self
    int priority = 0;
    uint64_t sequence = 0;
    // Set when the callback threw (07/10, players' Runtime Error): it is skipped from then on. Shared by every copy of the
    // entry in later lists, so a re-published list keeps it off.
    std::shared_ptr<std::atomic<bool>> off;
};

// Names whose draw callbacks observe re-issued draws (guarded by g_lock)
std::vector<std::string> g_reissueObservers;
bool ObservesReissue(const std::string& name) {
    return std::find(g_reissueObservers.begin(), g_reissueObservers.end(), name) != g_reissueObservers.end();
}

// Per-name timing of a chain's callbacks (Frame Profiler, development build)
enum class Timing : uint8_t {
    None,   // not timed per name
    Option, // while Advanced > "Per-hook registry timing" is checked (draw chains)
    Always, // while the profiler is on (Present chain: a few callbacks once per frame)
};

// Device calls a callback made itself through CallOriginal* on this thread, by method. After a callback threw they tell
// whether it had already issued the game's call itself; then the device call is not made a second time (07/10).
enum OrigSlot : int { kOrigNone = -1, kOrigDIP, kOrigDP, kOrigSRT, kOrigSPS, kOrigSVS, kOrigSTex, kOrigCRT, kOrigSVP, kOrigCPS, kOrigCVS, kOrigPSC, kOrigVSC, kOrigSlots };
thread_local uint32_t t_origCalls[kOrigSlots] = {};

// A chain's list is immutable once published: registering builds a new list and publishes it through an atomic pointer.
// Every list published is kept until a safe point (Present on the render thread outside any dispatch, or Uninstall), so
// a dispatch that read an older one (possibly the caller of Register, or the render thread's lock-free dispatch) keeps
// iterating valid memory.
template <typename Fn> struct Chain {
    using List = std::vector<Entry<Fn>>;
    const char* method;
    bool lockFree;            // dispatched without g_lock on the render thread (draw and state chains)
    bool reissueFilter;       // draw chains: inside a ReissueScope only the entries that observe re-issued draws run
    bool modTimed;            // its outermost dispatch is booked as "D3D hooks (mod)" (every chain but Present)
    Timing timing;
    const char* timingSuffix; // appended to the name for the per-name timing
    int origSlot;             // its CallOriginal* counter (t_origCalls), kOrigNone when there is none
    std::atomic<const List*> list{nullptr};
    std::atomic<size_t> count{0};             // lock-free "nothing registered" test for the hot paths
    std::vector<std::unique_ptr<List>> lists; // every list published, the last one current (guarded by g_lock)
    std::atomic<bool> offThreadLogged{false};

    Chain(const char* m, bool lf, bool mt, Timing t, const char* suffix, int slot = kOrigNone, bool reissueDraws = false)
        : method(m), lockFree(lf), reissueFilter(reissueDraws), modTimed(mt), timing(t), timingSuffix(suffix), origSlot(slot) {
        lists.push_back(std::make_unique<List>());
        list.store(lists.back().get());
    }
};

std::atomic<bool> g_retiredPending{false}; // some chain holds lists older than its current one

// Caller holds g_lock
template <typename Fn> void Publish(Chain<Fn>& chain, std::unique_ptr<typename Chain<Fn>::List> next) {
    chain.lists.push_back(std::move(next)); // owned first: a failed push_back cannot free a list already published
    const auto* cur = chain.lists.back().get();
    chain.count.store(cur->size(), std::memory_order_relaxed);
    chain.list.store(cur, std::memory_order_seq_cst); // xchg: globally visible before WaitForRenderThread's barrier
    g_retiredPending.store(true, std::memory_order_relaxed);
}

template <typename Fn> bool Add(Chain<Fn>& chain, const std::string& name, Fn fn, Priority priority) {
    if (!fn) return false;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto next = std::make_unique<typename Chain<Fn>::List>(*chain.list.load(std::memory_order_relaxed));
    Entry<Fn> e;
    e.fn = fn.fn();
    e.self = fn.self();
    e.off = std::make_shared<std::atomic<bool>>(false);
    e.offFlag = e.off.get();
    e.observeReissue = chain.reissueFilter && ObservesReissue(name);
    e.name = name;
    e.timingName = chain.timingSuffix ? name + chain.timingSuffix : name;
    if (!kPublicBuild && chain.timing != Timing::None) e.timingSlot = FrameProfiler::RegistryTimingSlot(e.timingName);
    e.hook = std::move(fn);
    e.priority = static_cast<int>(priority);
    e.sequence = ++g_sequence;
    next->push_back(std::move(e));
    std::stable_sort(next->begin(), next->end(), [](const Entry<Fn>& a, const Entry<Fn>& b) {
        return a.priority != b.priority ? a.priority < b.priority : a.sequence < b.sequence;
    });
    Publish(chain, std::move(next));
    return true;
}

// Caller holds g_lock. True when a callback was removed from a lock-free chain.
template <typename Fn> bool RemoveName(Chain<Fn>& chain, const std::string& name) {
    const auto* cur = chain.list.load(std::memory_order_relaxed);
    auto next = std::make_unique<typename Chain<Fn>::List>();
    for (const auto& e : *cur)
        if (e.name != name) next->push_back(e);
    if (next->size() == cur->size()) return false;
    Publish(chain, std::move(next));
    return chain.lockFree;
}

// Caller holds g_lock, at a safe point (no dispatch can still iterate an older list): frees every list but the current one
template <typename Fn> void FreeRetired(Chain<Fn>& chain) {
    if (chain.lists.size() > 1) chain.lists.erase(chain.lists.begin(), chain.lists.end() - 1);
}

// ---- render thread and lock-free dispatch ----
inline DWORD ThreadId() { return __readfsdword(0x24); } // TEB ClientId.UniqueThread, what GetCurrentThreadId returns

std::atomic<DWORD> g_renderTid{0};          // the thread of the first EndScene (Install)
int g_renderDepth = 0;                      // render thread only: nesting of its lock-free dispatches
std::atomic<int> g_renderInside{0};         // written by the render thread only: inside a lock-free dispatch
std::atomic<uint32_t> g_renderExits{0};     // written by the render thread only: outermost lock-free dispatches finished
std::atomic<uint32_t> g_offThread{0};       // lock-free chains dispatched from another thread (under the lock)
int g_lockedDepth = 0;                    // guarded by g_lock: dispatches running under the lock (the lock's owner only)
// (2.15.3) development build, per-name timing: the ticks of this thread's callbacks and driver draws that ran inside the
// callback being timed (subtracted from it: each callback is charged its own time only)
thread_local uint64_t t_regExcluded = 0;
thread_local int t_dispatchDepth = 0;       // development build: nesting of dispatches on this thread (the mod time)
thread_local int t_reissue = 0;             // ReissueScope nesting on this thread (lot_light_bridge.cpp's re-issued draws)
thread_local bool t_sampled = false;        // development build: the outermost dispatch on this thread is a timed sample
// (2.15.3) development build: the outermost dispatch on this thread is one of the 1 in 16 the dispatch cost line samples; the
// original draws callbacks issue inside it (Banding Fix's dithered re-issue, Night Lighting's replaced draws) are the
// driver's time, counted apart (g_dispatchDriverCycles) so the line reports the callbacks' own cost
thread_local bool t_costSampled = false;
uint64_t g_dispatchDriverCycles = 0;

// Render thread, around a lock-free dispatch. The "inside" store must precede the list load: the compiler barrier keeps
// that order in the code, the other thread's FlushProcessWriteBuffers covers the processor's store buffer.
struct RenderInside {
    RenderInside() {
        if (g_renderDepth++ == 0) {
            g_renderInside.store(1, std::memory_order_relaxed);
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
    }
    ~RenderInside() {
        if (--g_renderDepth == 0) {
            g_renderExits.store(g_renderExits.load(std::memory_order_relaxed) + 1, std::memory_order_release);
            g_renderInside.store(0, std::memory_order_release);
        }
    }
    RenderInside(const RenderInside&) = delete;
    RenderInside& operator=(const RenderInside&) = delete;
};

// A thread other than the render thread just published new lists (UnregisterAll): returns once the render thread can no
// longer be running a removed callback. After the barrier, a lock-free dispatch that starts later loads the new list;
// one that had started is visible through g_renderInside, and it is waited for (it ends when g_renderExits moves or the
// flag drops). Bounded: a render thread blocked on a lock the caller holds would otherwise never return. False when it
// timed out (the render thread may still be inside a dispatch).
bool WaitForRenderThread(const std::string& caller) {
    if (!g_renderTid.load(std::memory_order_relaxed)) return true; // not installed: no dispatch has ever run
    FlushProcessWriteBuffers();
    if (g_renderInside.load(std::memory_order_acquire) == 0) return true;
    const uint32_t exits = g_renderExits.load(std::memory_order_acquire);
    const ULONGLONG start = GetTickCount64();
    while (g_renderInside.load(std::memory_order_acquire) != 0 && g_renderExits.load(std::memory_order_acquire) == exits) {
        if (GetTickCount64() - start > 1000) {
            LOG_WARNING(std::format("[D3D9Hooks] {} from thread {}: the render thread did not leave its draw hook within 1 s; continuing", caller, ThreadId()));
            return false;
        }
        SwitchToThread();
    }
    return true;
}

template <typename Fn> void NoteOffThread(Chain<Fn>& chain) {
    g_offThread.fetch_add(1, std::memory_order_relaxed);
    if (!kPublicBuild)
        if (!chain.offThreadLogged.exchange(true))
            LOG_WARNING(std::format("[D3D9Hooks] {} called from thread {} (render thread {}): dispatched under the lock (first time only)", chain.method, ThreadId(),
                                    g_renderTid.load(std::memory_order_relaxed)));
}

// Development build: the outermost dispatch on a thread is booked as mod time ("D3D hooks (mod)") by the Frame Profiler,
// from before the first callback to after the last one, also when a callback returns Skip / Block. Nested dispatches (a
// callback calling the device, e.g. a replaced draw re-issued by Night Lighting) are part of it, except the driver's
// own time for a re-issued draw ("Re-issued draw (driver)", TimedReissueDraw).
// 14/10 (dispatch cost): one outermost dispatch in kModSample is timed and booked kModSample times (the profiler scales
// the sample and everything timed inside it); the others cost one counter increment.
constexpr uint32_t kModSample = 16;
// development build: which outermost dispatch is sampled, from a per-thread xorshift (a fixed stride would keep hitting the
// same method of the game's periodic SetVS / SetPS / SetTexture / draw sequence)
thread_local uint32_t t_sampleRng = 0x9E3779B9u;
inline bool SampleThisDispatch() {
    uint32_t x = t_sampleRng;
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    t_sampleRng = x;
    return (x & (kModSample - 1)) == 0;
}
struct ModTimeGuard {
    int token = -1;
    const void* key;
    bool outer = false;
    bool dev = false; // (read once: entry and exit use the same gate even while startup stores the setting)
    ModTimeGuard(bool modTimed, const void* k) : key(k), dev(!kPublicBuild) {
        if (!dev) return;
        if (t_dispatchDepth++ != 0) return;
        outer = true;
        // (PerfToggles::SampledDispatchTiming off: every outermost dispatch is timed, weight 1, as before the sampling)
        const bool sampled = PerfToggles::SampledDispatchTiming.load(std::memory_order_relaxed);
        if ((sampled && !SampleThisDispatch()) || !modTimed || !FrameProfiler::ModTimeActive()) return;
        token = FrameProfiler::BeginModTimeSampled(FrameProfiler::ModTime::D3DDispatch, key, sampled ? kModSample : 1);
        t_sampled = token >= 0;
    }
    ~ModTimeGuard() {
        if (!dev) return;
        --t_dispatchDepth;
        if (token >= 0) FrameProfiler::EndModTime(token, key);
        if (outer) t_sampled = false;
    }
    ModTimeGuard(const ModTimeGuard&) = delete;
    ModTimeGuard& operator=(const ModTimeGuard&) = delete;
};

// Development build: the driver call of an original draw issued inside a dispatch (a re-issued draw of Night Lighting or any
// callback's own CallOriginalDraw*): booked as "Re-issued draw (driver)" inside a timed sample (was only under ReissueScope,
// so the Banding Fix's re-issued draws counted as mod time), and its cycles kept apart from the dispatch cost line
template <typename Call> HRESULT TimedReissueDraw(Call call) {
    if (kPublicBuild || t_dispatchDepth == 0) return call();
    const uint64_t c0 = t_costSampled ? __rdtsc() : 0;
    const uint64_t r0 = FrameProfiler::RegistryHookTimingActive() ? FrameProfiler::Ticks() : 0;
    int anchor = 0;
    const int token = t_sampled ? FrameProfiler::BeginModTime(FrameProfiler::ModTime::ReissuedDraw, &anchor) : -1;
    const HRESULT hr = call();
    if (token >= 0) FrameProfiler::EndModTime(token, &anchor);
    if (c0) g_dispatchDriverCycles += __rdtsc() - c0;
    if (r0) t_regExcluded += FrameProfiler::Ticks() - r0; // the driver's draw: not the timed callback's own time
    return hr;
}

// From a catch block: names the callback without allocating
void NoteCallbackThrew(const char* method, const char* name) noexcept {
    char where[160];
    std::snprintf(where, sizeof where, "D3D9 %s callback \"%s\"", method, name);
    HookGuard::Note(where);
}

// Runs one list. false = a callback asked to skip the device call; result then holds what the game gets back.
// 07/10, players' Runtime Error: a callback's C++ exception would unwind into the game's device call and end the game. It is
// caught and that callback is skipped from now on. The chain goes on as if it had returned Continue, unless it had already
// made the game's call itself (CallOriginal*): then as if it had returned Skip. Either way the device call is made once.
// 14/10 (dispatch cost): one exception frame around the whole list instead of one per callback (re-entered after a
// callback threw, at the next one), and this thread's CallOriginal* counter read through a pointer taken once per dispatch.
// timed: per-name timing (development build); reissue: inside a ReissueScope on a draw chain (only observers run). Both
// are read once per dispatch by Run.
template <typename Fn, typename... Args>
bool RunList(const Chain<Fn>& chain, const typename Chain<Fn>::List& list, DeviceContext& ctx, HRESULT& result, const uint32_t* origCount, bool timed, bool reissue,
             Args... args) {
    const Entry<Fn>* const entries = list.data();
    const size_t n = list.size();
    size_t i = 0;
    while (i < n) {
        HookAction r = HookAction::Continue;
        bool returned = false;
        uint32_t orig0 = 0;
        try {
            for (; i < n; ++i) {
                const Entry<Fn>& e = entries[i];
                if (e.offFlag->load(std::memory_order_relaxed) || (reissue && !e.observeReissue)) continue;
                returned = false;
                orig0 = origCount ? *origCount : 0;
                if (timed) {
                    const uint64_t ex0 = t_regExcluded;
                    const uint64_t t0 = FrameProfiler::Ticks();
                    r = e.fn(e.self, ctx, args...);
                    returned = true;
                    const uint64_t dt = FrameProfiler::Ticks() - t0;
                    const uint64_t inner = t_regExcluded - ex0;
                    FrameProfiler::AddRegistryHookTimeAt(e.timingSlot, dt > inner ? dt - inner : 0);
                    t_regExcluded = ex0 + dt; // the callback around this one leaves all of it out
                } else {
                    r = e.fn(e.self, ctx, args...);
                    returned = true;
                }
                if (r != HookAction::Continue) break;
            }
        } catch (...) {
            const Entry<Fn>& e = entries[i];
            e.offFlag->store(true, std::memory_order_relaxed);
            NoteCallbackThrew(chain.method, e.name.c_str());
            if (!returned) r = origCount && *origCount != orig0 ? HookAction::Skip : HookAction::Continue;
        }
        if (r == HookAction::Skip) {
            result = S_OK;
            return false;
        }
        if (r == HookAction::Block) {
            result = E_FAIL;
            return false;
        }
        ++i; // (after a callback that threw and continues: the next one; after the whole list: past its end)
    }
    return true;
}

// Development build: the dispatch cost, sampled (one dispatch in 16 is timed with the time stamp counter; every dispatch is
// counted). Read and logged every 10 s by the Present hook.
uint32_t g_dispatchCount = 0;   // render thread mostly (a statistic: plain increments)
uint64_t g_dispatchSampled = 0, g_dispatchCycles = 0;

// Runs the chain. false = a callback asked to skip the device call; result then holds what the game gets back.
template <typename Fn, typename... Args> bool Run(Chain<Fn>& chain, IDirect3DDevice9* device, HRESULT& result, Args... args) {
    if (chain.count.load(std::memory_order_relaxed) == 0) return true;
    // a nested dispatch (a callback calling the hooked device) must not move the outer callback's CallOriginal* count
    uint32_t* const origCount = chain.origSlot >= 0 ? &t_origCalls[chain.origSlot] : nullptr;
    struct OrigScope {
        uint32_t* p; uint32_t v;
        ~OrigScope() { if (p) *p = v; }
    } origScope{origCount, origCount ? *origCount : 0u};
    DeviceContext ctx{device};
    ModTimeGuard mod(chain.modTimed, &ctx);
    // the development and timing flags, once per dispatch
    bool timed = false;
    uint64_t c0 = 0;
    if (!kPublicBuild) {
        timed = chain.timing == Timing::Always ? FrameProfiler::PresentHookTimingActive() : (chain.timing == Timing::Option && FrameProfiler::RegistryHookTimingActive());
        if ((++g_dispatchCount & 15) == 0 && t_dispatchDepth == 1) c0 = __rdtsc(); // (outermost only: nested ones are inside it)
    }
    const bool reissue = chain.reissueFilter && t_reissue != 0 && PerfToggles::ReissueFilter.load(std::memory_order_relaxed);
    struct Sample {
        uint64_t c0;
        bool was;
        ~Sample() {
            if (c0) g_dispatchCycles += __rdtsc() - c0, g_dispatchSampled++, t_costSampled = was;
        }
    } sample{c0, t_costSampled};
    if (c0) t_costSampled = true;
    if (chain.lockFree) {
        if (ThreadId() == g_renderTid.load(std::memory_order_relaxed)) {
            RenderInside inside;
            const auto* list = chain.list.load(std::memory_order_acquire);
            return RunList(chain, *list, ctx, result, origCount, timed, reissue, args...);
        }
        NoteOffThread(chain);
    }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    struct LockedDepth {
        LockedDepth() { ++g_lockedDepth; }
        ~LockedDepth() { --g_lockedDepth; }
    } depth;
    const auto* list = chain.list.load(std::memory_order_relaxed);
    return RunList(chain, *list, ctx, result, origCount, timed, reissue, args...);
}

// Development build, at Present: every 10 s a line with the dispatches a frame and their sampled cost
void LogDispatchCost() {
    if (kPublicBuild) return;
    static uint32_t s_frames = 0, s_count0 = 0;
    static uint64_t s_sampled0 = 0, s_cycles0 = 0, s_tsc0 = 0, s_driver0 = 0;
    static LARGE_INTEGER s_qpc0{};
    if (!s_tsc0) {
        s_tsc0 = __rdtsc();
        QueryPerformanceCounter(&s_qpc0);
        s_count0 = g_dispatchCount, s_sampled0 = g_dispatchSampled, s_cycles0 = g_dispatchCycles, s_driver0 = g_dispatchDriverCycles;
        return;
    }
    s_frames++;
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const double sec = freq.QuadPart ? static_cast<double>(now.QuadPart - s_qpc0.QuadPart) / static_cast<double>(freq.QuadPart) : 0.0;
    if (sec < 10.0) return;
    const uint64_t tsc = __rdtsc();
    const double cyclesPerMs = static_cast<double>(tsc - s_tsc0) / (sec * 1000.0);
    const uint32_t calls = g_dispatchCount - s_count0;
    const uint64_t sampled = g_dispatchSampled - s_sampled0, cycles = g_dispatchCycles - s_cycles0, driver = g_dispatchDriverCycles - s_driver0;
    const uint64_t own = cycles > driver ? cycles - driver : 0;
    const double perCallUs = sampled && cyclesPerMs > 0.0 ? static_cast<double>(own) / static_cast<double>(sampled) / cyclesPerMs * 1000.0 : 0.0;
    const double driverUs = sampled && cyclesPerMs > 0.0 ? static_cast<double>(driver) / static_cast<double>(sampled) / cyclesPerMs * 1000.0 : 0.0;
    const double perFrame = s_frames ? static_cast<double>(calls) / s_frames : 0.0;
    LOG_INFO(std::format("[D3D9Hooks] Dispatch cost over {:.0f} s: {:.0f} dispatches with callbacks a frame, {:.3f} us each in the callbacks (sampled 1 in 16), "
                         "about {:.3f} ms a frame; draws the callbacks issued themselves: {:.3f} ms a frame in the driver",
                         sec, perFrame, perCallUs, perFrame * perCallUs / 1000.0, perFrame * driverUs / 1000.0));
    s_frames = 0, s_tsc0 = tsc, s_qpc0 = now, s_count0 = g_dispatchCount, s_sampled0 = g_dispatchSampled, s_cycles0 = g_dispatchCycles, s_driver0 = g_dispatchDriverCycles;
}

Chain<DrawIndexedPrimitiveHook> g_dip("DrawIndexedPrimitive", true, true, Timing::Option, nullptr, kOrigDIP, true);
Chain<DrawPrimitiveHook> g_dp("DrawPrimitive", true, true, Timing::Option, nullptr, kOrigDP, true);
Chain<SetRenderTargetHook> g_srt("SetRenderTarget", true, true, Timing::Option, " (SetRenderTarget)", kOrigSRT);
Chain<SetPixelShaderHook> g_sps("SetPixelShader", true, true, Timing::Option, " (SetPixelShader)", kOrigSPS);
Chain<SetVertexShaderHook> g_svs("SetVertexShader", true, true, Timing::Option, " (SetVertexShader)", kOrigSVS);
Chain<SetTextureHook> g_stex("SetTexture", true, true, Timing::Option, " (SetTexture)", kOrigSTex);
Chain<PresentHook> g_present("Present", false, false, Timing::Always, " (Present)");
Chain<BeginSceneHook> g_begin("BeginScene", false, true, Timing::None, nullptr);
Chain<CreateTextureHook> g_ctex("CreateTexture", false, true, Timing::None, nullptr);
Chain<CreateRenderTargetHook> g_crt("CreateRenderTarget", false, true, Timing::None, nullptr, kOrigCRT);
Chain<SetViewportHook> g_svp("SetViewport", true, true, Timing::Option, " (SetViewport)", kOrigSVP);
Chain<CreatePixelShaderHook> g_cps("CreatePixelShader", false, true, Timing::None, nullptr, kOrigCPS);
Chain<CreateVertexShaderHook> g_cvs("CreateVertexShader", false, true, Timing::None, nullptr, kOrigCVS);
Chain<SetPixelShaderConstantFHook> g_psc("SetPixelShaderConstantF", true, true, Timing::Option, " (SetPixelShaderConstantF)", kOrigPSC);
Chain<SetVertexShaderConstantFHook> g_vsc("SetVertexShaderConstantF", true, true, Timing::Option, " (SetVertexShaderConstantF)", kOrigVSC);

// Caller holds g_lock, at a safe point
void FreeAllRetired() {
    FreeRetired(g_dip);
    FreeRetired(g_dp);
    FreeRetired(g_srt);
    FreeRetired(g_sps);
    FreeRetired(g_svs);
    FreeRetired(g_stex);
    FreeRetired(g_present);
    FreeRetired(g_begin);
    FreeRetired(g_ctex);
    FreeRetired(g_crt);
    FreeRetired(g_svp);
    FreeRetired(g_cps);
    FreeRetired(g_cvs);
    FreeRetired(g_psc);
    FreeRetired(g_vsc);
}

// Render thread, at Present: frees the retired lists when no dispatch can still iterate one. The render thread is outside
// its lock-free dispatches (depth 0), holding the lock excludes every other thread's locked dispatch, and a locked
// dispatch of this thread further up the stack shows in g_lockedDepth. Never blocks: a busy lock waits for the next frame.
void ReclaimRetired() {
    if (!g_retiredPending.load(std::memory_order_relaxed) || ThreadId() != g_renderTid.load(std::memory_order_relaxed) || g_renderDepth != 0) return;
    std::unique_lock<std::recursive_mutex> lock(g_lock, std::try_to_lock);
    if (!lock.owns_lock() || g_lockedDepth != 0) return;
    g_retiredPending.store(false, std::memory_order_relaxed);
    FreeAllRetired();
}

// ---- development build: state-call counters for the Frame Profiler (replaces six registered counting callbacks) ----
enum CallSlot : int { kCallSetTexture, kCallSetVS, kCallSetPS, kCallSetVSC, kCallSetPSC, kCallSetRT, kCallSlots };
std::atomic<uint32_t> g_calls[kCallSlots]{};
inline void CountCall(CallSlot s) {
    if (!kPublicBuild) g_calls[s].store(g_calls[s].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); // no locked instruction
}

// ---- detours (IDirect3DDevice9 vtable slots) ----
using DrawIndexedPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using SetRenderTarget_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetPixelShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetVertexShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using BeginScene_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using CreateTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
using CreateRenderTarget_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
using SetViewport_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using CreatePixelShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
using CreateVertexShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
using SetShaderConstantF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);

enum Slot : int {
    kCreateTexture = 23,
    kCreateRenderTarget = 28,
    kSetRenderTarget = 37,
    kBeginScene = 41,
    kSetViewport = 47,
    kSetTexture = 65,
    kDrawPrimitive = 81,
    kDrawIndexedPrimitive = 82,
    kCreateVertexShader = 91,
    kSetVertexShader = 92,
    kSetVertexShaderConstantF = 94,
    kCreatePixelShader = 106,
    kSetPixelShader = 107,
    kSetPixelShaderConstantF = 109,
    kPresent = 17,
};

DrawIndexedPrimitive_t o_dip = nullptr;
DrawPrimitive_t o_dp = nullptr;
SetRenderTarget_t o_srt = nullptr;
SetPixelShader_t o_sps = nullptr;
SetVertexShader_t o_svs = nullptr;
SetTexture_t o_stex = nullptr;
Present_t o_present = nullptr;
BeginScene_t o_begin = nullptr;
CreateTexture_t o_ctex = nullptr;
CreateRenderTarget_t o_crt = nullptr;
SetViewport_t o_svp = nullptr;
CreatePixelShader_t o_cps = nullptr;
CreateVertexShader_t o_cvs = nullptr;
SetShaderConstantF_t o_psc = nullptr;
SetShaderConstantF_t o_vsc = nullptr;
std::atomic<RenderTargetSubstitute> g_rtSubstitute{nullptr}; // SetRenderTargetSubstitute
inline IDirect3DSurface9* BoundFor(DWORD i, IDirect3DSurface9* s) {
    const RenderTargetSubstitute sub = g_rtSubstitute.load(std::memory_order_relaxed);
    return sub ? sub(i, s) : s;
}

std::atomic<bool> g_installed{false};

// ---- the shaders bound now (BoundPixelShader / BoundVertexShader) ----
// Render thread only; another thread's Set*Shader (or ShaderStateChanged) raises g_shadersDirty and the render thread
// reads them back at its next look.
// (per device: the detours serve every device, a second one's shaders are not the main device's; g_boundDev = the device
// whose shaders are followed, the last one read back)
IDirect3DDevice9* g_boundDev = nullptr;
IDirect3DPixelShader9* g_boundPs = nullptr;
IDirect3DVertexShader9* g_boundVs = nullptr;
bool g_boundPsKnown = false, g_boundVsKnown = false;
// (2.15.3) the textures of pixel samplers 0..15 (BoundTexture), on the same terms; bit s of g_boundTexKnown = s is known
IDirect3DBaseTexture9* g_boundTex[16] = {};
uint32_t g_boundTexKnown = 0;
uint32_t g_texChecks = 0, g_texMismatches = 0;
bool g_texFollowOff = false; // repeated differences from the device: read back from then on
std::atomic<bool> g_shadersDirty{false}; // (shaders and textures)
inline bool OnRenderThread() { return ThreadId() == g_renderTid.load(std::memory_order_relaxed); }
inline void ForgetBoundShaders() {
    if (OnRenderThread()) g_boundPsKnown = g_boundVsKnown = false, g_boundTexKnown = 0;
    else g_shadersDirty.store(true, std::memory_order_relaxed);
}
inline void NoteBoundTex(IDirect3DDevice9* d, DWORD st, IDirect3DBaseTexture9* t) {
    if (st >= 16) return;
    if (!OnRenderThread()) g_shadersDirty.store(true, std::memory_order_relaxed);
    else if (d == g_boundDev) g_boundTex[st] = t, g_boundTexKnown |= 1u << st;
}
inline void ForgetBoundTex(DWORD st) {
    if (st >= 16) return;
    if (!OnRenderThread()) g_shadersDirty.store(true, std::memory_order_relaxed);
    else g_boundTexKnown &= ~(1u << st);
}
inline void NoteBoundPs(IDirect3DDevice9* d, IDirect3DPixelShader9* s) {
    if (!OnRenderThread()) g_shadersDirty.store(true, std::memory_order_relaxed);
    else if (d == g_boundDev) g_boundPs = s, g_boundPsKnown = true;
}
inline void NoteBoundVs(IDirect3DDevice9* d, IDirect3DVertexShader9* s) {
    if (!OnRenderThread()) g_shadersDirty.store(true, std::memory_order_relaxed);
    else if (d == g_boundDev) g_boundVs = s, g_boundVsKnown = true;
}
// Render thread: follow this device's shaders from now on (another device's were followed)
inline void TakeBoundDevice(IDirect3DDevice9* d) {
    if (d != g_boundDev) g_boundDev = d, g_boundPsKnown = g_boundVsKnown = false, g_boundTexKnown = 0;
}

HRESULT STDMETHODCALLTYPE H_DrawIndexedPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT bv, UINT mv, UINT nv, UINT si, UINT pc) {
    HRESULT hr = S_OK;
    if (!Run(g_dip, d, hr, t, bv, mv, nv, si, pc)) return hr;
    if (!kPublicBuild && t_reissue) return TimedReissueDraw([&] { return o_dip(d, t, bv, mv, nv, si, pc); });
    return o_dip(d, t, bv, mv, nv, si, pc);
}
HRESULT STDMETHODCALLTYPE H_DrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT sv, UINT pc) {
    HRESULT hr = S_OK;
    if (!Run(g_dp, d, hr, t, sv, pc)) return hr;
    if (!kPublicBuild && t_reissue) return TimedReissueDraw([&] { return o_dp(d, t, sv, pc); });
    return o_dp(d, t, sv, pc);
}
HRESULT STDMETHODCALLTYPE H_SetRenderTarget(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) {
    CountCall(kCallSetRT);
    HRESULT hr = S_OK;
    if (!Run(g_srt, d, hr, i, s)) return hr;
    return o_srt(d, i, BoundFor(i, s));
}
HRESULT STDMETHODCALLTYPE H_SetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* s) {
    CountCall(kCallSetPS);
    HRESULT hr = S_OK;
    if (!Run(g_sps, d, hr, s)) return hr; // (a callback that skipped it and set one itself did so through CallOriginal*: followed there)
    hr = o_sps(d, s);
    if (SUCCEEDED(hr)) NoteBoundPs(d, s);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_SetVertexShader(IDirect3DDevice9* d, IDirect3DVertexShader9* s) {
    CountCall(kCallSetVS);
    HRESULT hr = S_OK;
    if (!Run(g_svs, d, hr, s)) return hr;
    hr = o_svs(d, s);
    if (SUCCEEDED(hr)) NoteBoundVs(d, s);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_SetTexture(IDirect3DDevice9* d, DWORD st, IDirect3DBaseTexture9* t) {
    CountCall(kCallSetTexture);
    HRESULT hr = S_OK;
    if (!Run(g_stex, d, hr, st, t)) return hr; // (a callback that skipped it and set one itself did so through CallOriginal*: followed there)
    hr = o_stex(d, st, t);
    if (SUCCEEDED(hr)) NoteBoundTex(d, st, t);
    else ForgetBoundTex(st);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_Present(IDirect3DDevice9* d, const RECT* sr, const RECT* dr, HWND w, const RGNDATA* rg) {
    HRESULT hr = S_OK;
    ForgetBoundShaders(); // (the menu overlay's state block restored them at EndScene; a Reset may follow)
    ReclaimRetired();
    if (!kPublicBuild && ThreadId() == g_renderTid.load(std::memory_order_relaxed)) LogDispatchCost();
    if (!Run(g_present, d, hr, sr, dr, w, rg)) return hr;
    return o_present(d, sr, dr, w, rg);
}
HRESULT STDMETHODCALLTYPE H_BeginScene(IDirect3DDevice9* d) {
    HRESULT hr = S_OK;
    ForgetBoundShaders();
    if (!Run(g_begin, d, hr)) return hr;
    return o_begin(d);
}
HRESULT STDMETHODCALLTYPE H_CreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** t, HANDLE* sh) {
    HRESULT hr = S_OK;
    if (!Run(g_ctex, d, hr, w, h, l, u, f, p, t, sh)) return hr;
    const uint64_t allocationStart = FrameProfiler::BeginTextureAllocation();
    hr = o_ctex(d, w, h, l, u, f, p, t, sh);
    FrameProfiler::EndTextureAllocation(allocationStart, SUCCEEDED(hr));
    // development builds (memory_owners): a texture the ASI itself asked for is counted until Direct3D destroys it
    if (DevSwitches::kCompiled && SUCCEEDED(hr) && t && *t) ApexMemory::NoteTexture(_ReturnAddress(), *t, w, h, l, u, f, p);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateRenderTarget(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL lk, IDirect3DSurface9** s, HANDLE* sh) {
    HRESULT hr = S_OK;
    if (!Run(g_crt, d, hr, w, h, f, m, q, lk, s, sh)) return hr;
    hr = o_crt(d, w, h, f, m, q, lk, s, sh);
    if (DevSwitches::kCompiled && SUCCEEDED(hr) && s && *s) ApexMemory::NoteSurface(_ReturnAddress(), *s, w, h, f, true);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_SetViewport(IDirect3DDevice9* d, const D3DVIEWPORT9* v) {
    HRESULT hr = S_OK;
    if (!Run(g_svp, d, hr, v)) return hr;
    return o_svp(d, v);
}
HRESULT STDMETHODCALLTYPE H_CreatePixelShader(IDirect3DDevice9* d, const DWORD* fn, IDirect3DPixelShader9** s) {
    HRESULT hr = S_OK;
    if (!Run(g_cps, d, hr, fn, s)) return hr;
    hr = o_cps(d, fn, s);
    if (DevSwitches::kCompiled && SUCCEEDED(hr)) ApexMemory::NoteShader(_ReturnAddress(), fn, true);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateVertexShader(IDirect3DDevice9* d, const DWORD* fn, IDirect3DVertexShader9** s) {
    HRESULT hr = S_OK;
    if (!Run(g_cvs, d, hr, fn, s)) return hr;
    hr = o_cvs(d, fn, s);
    if (DevSwitches::kCompiled && SUCCEEDED(hr)) ApexMemory::NoteShader(_ReturnAddress(), fn, false);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_SetPixelShaderConstantF(IDirect3DDevice9* d, UINT r, const float* c, UINT n) {
    CountCall(kCallSetPSC);
    HRESULT hr = S_OK;
    if (!Run(g_psc, d, hr, r, c, n)) return hr;
    return o_psc(d, r, c, n);
}
HRESULT STDMETHODCALLTYPE H_SetVertexShaderConstantF(IDirect3DDevice9* d, UINT r, const float* c, UINT n) {
    CountCall(kCallSetVSC);
    HRESULT hr = S_OK;
    if (!Run(g_vsc, d, hr, r, c, n)) return hr;
    return o_vsc(d, r, c, n);
}

struct Target {
    const char* name;
    int slot;
    void** original;
    void* detour;
};

// The table the detours are attached from (and detached in the same order)
std::vector<Target> Targets() {
    return {
        {"DrawIndexedPrimitive", kDrawIndexedPrimitive, reinterpret_cast<void**>(&o_dip), reinterpret_cast<void*>(&H_DrawIndexedPrimitive)},
        {"DrawPrimitive", kDrawPrimitive, reinterpret_cast<void**>(&o_dp), reinterpret_cast<void*>(&H_DrawPrimitive)},
        {"SetRenderTarget", kSetRenderTarget, reinterpret_cast<void**>(&o_srt), reinterpret_cast<void*>(&H_SetRenderTarget)},
        {"SetPixelShader", kSetPixelShader, reinterpret_cast<void**>(&o_sps), reinterpret_cast<void*>(&H_SetPixelShader)},
        {"SetVertexShader", kSetVertexShader, reinterpret_cast<void**>(&o_svs), reinterpret_cast<void*>(&H_SetVertexShader)},
        {"SetTexture", kSetTexture, reinterpret_cast<void**>(&o_stex), reinterpret_cast<void*>(&H_SetTexture)},
        {"Present", kPresent, reinterpret_cast<void**>(&o_present), reinterpret_cast<void*>(&H_Present)},
        {"BeginScene", kBeginScene, reinterpret_cast<void**>(&o_begin), reinterpret_cast<void*>(&H_BeginScene)},
        {"CreateTexture", kCreateTexture, reinterpret_cast<void**>(&o_ctex), reinterpret_cast<void*>(&H_CreateTexture)},
        {"CreateRenderTarget", kCreateRenderTarget, reinterpret_cast<void**>(&o_crt), reinterpret_cast<void*>(&H_CreateRenderTarget)},
        {"SetViewport", kSetViewport, reinterpret_cast<void**>(&o_svp), reinterpret_cast<void*>(&H_SetViewport)},
        {"CreatePixelShader", kCreatePixelShader, reinterpret_cast<void**>(&o_cps), reinterpret_cast<void*>(&H_CreatePixelShader)},
        {"CreateVertexShader", kCreateVertexShader, reinterpret_cast<void**>(&o_cvs), reinterpret_cast<void*>(&H_CreateVertexShader)},
        {"SetPixelShaderConstantF", kSetPixelShaderConstantF, reinterpret_cast<void**>(&o_psc), reinterpret_cast<void*>(&H_SetPixelShaderConstantF)},
        {"SetVertexShaderConstantF", kSetVertexShaderConstantF, reinterpret_cast<void**>(&o_vsc), reinterpret_cast<void*>(&H_SetVertexShaderConstantF)},
    };
}

void** VTable(IDirect3DDevice9* device) { return *reinterpret_cast<void***>(device); }

} // namespace

// ---- registration ----
bool RegisterDrawIndexedPrimitive(const std::string& n, DrawIndexedPrimitiveHook h, Priority p) { return Add(g_dip, n, std::move(h), p); }
bool RegisterDrawPrimitive(const std::string& n, DrawPrimitiveHook h, Priority p) { return Add(g_dp, n, std::move(h), p); }
bool RegisterSetRenderTarget(const std::string& n, SetRenderTargetHook h, Priority p) { return Add(g_srt, n, std::move(h), p); }
bool RegisterSetPixelShader(const std::string& n, SetPixelShaderHook h, Priority p) { return Add(g_sps, n, std::move(h), p); }
bool RegisterSetVertexShader(const std::string& n, SetVertexShaderHook h, Priority p) { return Add(g_svs, n, std::move(h), p); }
bool RegisterSetTexture(const std::string& n, SetTextureHook h, Priority p) { return Add(g_stex, n, std::move(h), p); }
bool RegisterPresent(const std::string& n, PresentHook h, Priority p) { return Add(g_present, n, std::move(h), p); }
bool RegisterBeginScene(const std::string& n, BeginSceneHook h, Priority p) { return Add(g_begin, n, std::move(h), p); }
bool RegisterCreateTexture(const std::string& n, CreateTextureHook h, Priority p) { return Add(g_ctex, n, std::move(h), p); }
bool RegisterCreateRenderTarget(const std::string& n, CreateRenderTargetHook h, Priority p) { return Add(g_crt, n, std::move(h), p); }
bool RegisterSetViewport(const std::string& n, SetViewportHook h, Priority p) { return Add(g_svp, n, std::move(h), p); }
bool RegisterCreatePixelShader(const std::string& n, CreatePixelShaderHook h, Priority p) { return Add(g_cps, n, std::move(h), p); }
bool RegisterCreateVertexShader(const std::string& n, CreateVertexShaderHook h, Priority p) { return Add(g_cvs, n, std::move(h), p); }
bool RegisterSetPixelShaderConstantF(const std::string& n, SetPixelShaderConstantFHook h, Priority p) { return Add(g_psc, n, std::move(h), p); }
bool RegisterSetVertexShaderConstantF(const std::string& n, SetVertexShaderConstantFHook h, Priority p) { return Add(g_vsc, n, std::move(h), p); }

void UnregisterAll(const std::string& name) {
    bool lockFreeChanged = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        lockFreeChanged |= RemoveName(g_dip, name);
        lockFreeChanged |= RemoveName(g_dp, name);
        lockFreeChanged |= RemoveName(g_srt, name);
        lockFreeChanged |= RemoveName(g_sps, name);
        lockFreeChanged |= RemoveName(g_svs, name);
        lockFreeChanged |= RemoveName(g_stex, name);
        lockFreeChanged |= RemoveName(g_present, name);
        lockFreeChanged |= RemoveName(g_begin, name);
        lockFreeChanged |= RemoveName(g_ctex, name);
        lockFreeChanged |= RemoveName(g_crt, name);
        lockFreeChanged |= RemoveName(g_svp, name);
        lockFreeChanged |= RemoveName(g_cps, name);
        lockFreeChanged |= RemoveName(g_cvs, name);
        lockFreeChanged |= RemoveName(g_psc, name);
        lockFreeChanged |= RemoveName(g_vsc, name);
    }
    // The render thread itself (inside or outside a dispatch) needs no wait: as before, a chain it is running keeps its
    // list until it returns. Any other thread waits, outside the lock, for the render thread to leave the dispatch that
    // may still run a removed callback (the locked dispatches of other threads were already excluded by the lock).
    if (lockFreeChanged && ThreadId() != g_renderTid.load(std::memory_order_relaxed)) WaitForRenderThread("UnregisterAll(\"" + name + "\")");
}

// Marks the draw callbacks of this name (registered now or later) as observers of re-issued draws. Caller holds g_lock.
template <typename Fn> void MarkObserver(Chain<Fn>& chain, const std::string& name) {
    const auto* cur = chain.list.load(std::memory_order_relaxed);
    bool any = false;
    for (const auto& e : *cur) any |= e.name == name && !e.observeReissue;
    if (!any) return;
    auto next = std::make_unique<typename Chain<Fn>::List>(*cur);
    for (auto& e : *next)
        if (e.name == name) e.observeReissue = true;
    Publish(chain, std::move(next));
}

void ObserveReissuedDraws(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!ObservesReissue(name)) g_reissueObservers.push_back(name);
    MarkObserver(g_dip, name);
    MarkObserver(g_dp, name);
}

void SetRenderTargetSubstitute(RenderTargetSubstitute fn) { g_rtSubstitute.store(fn); }

void BeginReissue() { ++t_reissue; }
void EndReissue() { --t_reissue; }

// ---- originals ----
HRESULT CallOriginalCreateRenderTarget(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL lk, IDirect3DSurface9** s, HANDLE* sh) {
    ++t_origCalls[kOrigCRT];
    return o_crt ? o_crt(d, w, h, f, m, q, lk, s, sh) : d->CreateRenderTarget(w, h, f, m, q, lk, s, sh);
}
HRESULT CallOriginalSetRenderTarget(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) {
    ++t_origCalls[kOrigSRT];
    s = BoundFor(i, s);
    return o_srt ? o_srt(d, i, s) : d->SetRenderTarget(i, s);
}
HRESULT CallOriginalSetViewport(IDirect3DDevice9* d, const D3DVIEWPORT9* v) {
    ++t_origCalls[kOrigSVP];
    return o_svp ? o_svp(d, v) : d->SetViewport(v);
}
HRESULT CallOriginalDrawIndexedPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT bv, UINT mv, UINT nv, UINT si, UINT pc) {
    ++t_origCalls[kOrigDIP];
    if (!kPublicBuild && t_dispatchDepth > 0) return TimedReissueDraw([&] { return o_dip ? o_dip(d, t, bv, mv, nv, si, pc) : d->DrawIndexedPrimitive(t, bv, mv, nv, si, pc); });
    return o_dip ? o_dip(d, t, bv, mv, nv, si, pc) : d->DrawIndexedPrimitive(t, bv, mv, nv, si, pc);
}
HRESULT CallOriginalDrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT sv, UINT pc) {
    ++t_origCalls[kOrigDP];
    if (!kPublicBuild && t_dispatchDepth > 0) return TimedReissueDraw([&] { return o_dp ? o_dp(d, t, sv, pc) : d->DrawPrimitive(t, sv, pc); });
    return o_dp ? o_dp(d, t, sv, pc) : d->DrawPrimitive(t, sv, pc);
}
HRESULT CallOriginalSetVertexShaderConstantF(IDirect3DDevice9* d, UINT r, const float* c, UINT n) {
    ++t_origCalls[kOrigVSC];
    return o_vsc ? o_vsc(d, r, c, n) : d->SetVertexShaderConstantF(r, c, n);
}
HRESULT CallOriginalSetPixelShaderConstantF(IDirect3DDevice9* d, UINT r, const float* c, UINT n) {
    ++t_origCalls[kOrigPSC];
    return o_psc ? o_psc(d, r, c, n) : d->SetPixelShaderConstantF(r, c, n);
}
HRESULT CallOriginalSetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* s) {
    ++t_origCalls[kOrigSPS];
    const HRESULT hr = o_sps ? o_sps(d, s) : d->SetPixelShader(s);
    if (SUCCEEDED(hr)) NoteBoundPs(d, s);
    else ForgetBoundShaders();
    return hr;
}
HRESULT CallOriginalCreatePixelShader(IDirect3DDevice9* d, const DWORD* fn, IDirect3DPixelShader9** s) {
    ++t_origCalls[kOrigCPS];
    return o_cps ? o_cps(d, fn, s) : d->CreatePixelShader(fn, s);
}
HRESULT CallOriginalCreateVertexShader(IDirect3DDevice9* d, const DWORD* fn, IDirect3DVertexShader9** s) {
    ++t_origCalls[kOrigCVS];
    return o_cvs ? o_cvs(d, fn, s) : d->CreateVertexShader(fn, s);
}
HRESULT CallOriginalSetVertexShader(IDirect3DDevice9* d, IDirect3DVertexShader9* s) {
    ++t_origCalls[kOrigSVS];
    const HRESULT hr = o_svs ? o_svs(d, s) : d->SetVertexShader(s);
    if (SUCCEEDED(hr)) NoteBoundVs(d, s);
    else ForgetBoundShaders();
    return hr;
}

IDirect3DPixelShader9* BoundPixelShader(IDirect3DDevice9* d) {
    const bool render = OnRenderThread();
    if (render && g_shadersDirty.load(std::memory_order_relaxed) && g_shadersDirty.exchange(false)) g_boundPsKnown = g_boundVsKnown = false, g_boundTexKnown = 0;
    if (render) TakeBoundDevice(d);
    if (render && g_boundPsKnown && PerfToggles::FollowBoundShaders.load(std::memory_order_relaxed)) return g_boundPs;
    IDirect3DPixelShader9* s = nullptr;
    if (FAILED(d->GetPixelShader(&s))) s = nullptr;
    if (s) s->Release(); // the device keeps it alive
    if (render) g_boundPs = s, g_boundPsKnown = true;
    return s;
}

IDirect3DVertexShader9* BoundVertexShader(IDirect3DDevice9* d) {
    const bool render = OnRenderThread();
    if (render && g_shadersDirty.load(std::memory_order_relaxed) && g_shadersDirty.exchange(false)) g_boundPsKnown = g_boundVsKnown = false, g_boundTexKnown = 0;
    if (render) TakeBoundDevice(d);
    if (render && g_boundVsKnown && PerfToggles::FollowBoundShaders.load(std::memory_order_relaxed)) return g_boundVs;
    IDirect3DVertexShader9* s = nullptr;
    if (FAILED(d->GetVertexShader(&s))) s = nullptr;
    if (s) s->Release();
    if (render) g_boundVs = s, g_boundVsKnown = true;
    return s;
}

IDirect3DBaseTexture9* BoundTexture(IDirect3DDevice9* d, DWORD st) {
    const bool render = OnRenderThread() && st < 16;
    if (render && g_shadersDirty.load(std::memory_order_relaxed) && g_shadersDirty.exchange(false)) g_boundPsKnown = g_boundVsKnown = false, g_boundTexKnown = 0;
    if (render) TakeBoundDevice(d);
    const bool known = render && (g_boundTexKnown >> st & 1u) && !g_texFollowOff && PerfToggles::FollowBoundTextures.load(std::memory_order_relaxed);
    if (known && (++g_texChecks & 255u) != 0) return g_boundTex[st];
    IDirect3DBaseTexture9* t = nullptr;
    if (FAILED(d->GetTexture(st, &t))) t = nullptr;
    if (t) t->Release(); // the device keeps it alive
    if (known && t != g_boundTex[st] && ++g_texMismatches >= 8 && !g_texFollowOff) {
        g_texFollowOff = true;
        LOG_WARNING("[D3D9Hooks] The followed textures differed from the device's 8 times (a texture set past the device's SetTexture): read back from now on");
    }
    if (render) g_boundTex[st] = t, g_boundTexKnown |= 1u << st;
    return t;
}

HRESULT GetBoundTexture(IDirect3DDevice9* d, DWORD st, IDirect3DBaseTexture9** out) {
    if (!out) return E_POINTER;
    IDirect3DBaseTexture9* t = BoundTexture(d, st);
    if (t) t->AddRef();
    *out = t;
    return S_OK;
}

void ShaderStateChanged() { ForgetBoundShaders(); }
HRESULT CallOriginalSetTexture(IDirect3DDevice9* d, DWORD st, IDirect3DBaseTexture9* t) {
    ++t_origCalls[kOrigSTex];
    const HRESULT hr = o_stex ? o_stex(d, st, t) : d->SetTexture(st, t);
    if (SUCCEEDED(hr)) NoteBoundTex(d, st, t);
    else ForgetBoundTex(st);
    return hr;
}

// ---- development build counters ----
StateCallCounts ReadStateCallCounts() {
    StateCallCounts c;
    c.setTexture = g_calls[kCallSetTexture].load(std::memory_order_relaxed);
    c.setVertexShader = g_calls[kCallSetVS].load(std::memory_order_relaxed);
    c.setPixelShader = g_calls[kCallSetPS].load(std::memory_order_relaxed);
    c.setVertexConstants = g_calls[kCallSetVSC].load(std::memory_order_relaxed);
    c.setPixelConstants = g_calls[kCallSetPSC].load(std::memory_order_relaxed);
    c.setRenderTarget = g_calls[kCallSetRT].load(std::memory_order_relaxed);
    return c;
}

uint32_t OffThreadDispatches() { return g_offThread.load(std::memory_order_relaxed); }

// ---- install ----
bool Install(IDirect3DDevice9* device) {
    if (g_installed.load() || !device) return g_installed.load();
    void** vt = VTable(device);
    const std::vector<Target> targets = Targets();
    for (const Target& t : targets) {
        *t.original = vt[t.slot];
        LOG_INFO(std::format("[D3D9Hooks] {} (slot {}) at {}: {}", t.name, t.slot, HookChain::AddressText(vt[t.slot]), HookChain::DescribePrologue(vt[t.slot])));
    }
    // Two slots sharing one function (a wrapper folding methods) would be detoured twice: refuse the whole set.
    for (size_t i = 0; i < targets.size(); i++)
        for (size_t j = i + 1; j < targets.size(); j++)
            if (*targets[i].original == *targets[j].original) {
                LOG_ERROR(std::format("[D3D9Hooks] {} and {} share one function: device hooks not installed", targets[i].name, targets[j].name));
                for (const Target& t : targets) *t.original = nullptr;
                return false;
            }
    // Install runs on the render thread (the game's first EndScene): its draw and state dispatches go lock-free
    g_renderTid.store(ThreadId());
    std::lock_guard<std::recursive_mutex> detoursLock(DetourBatch::Lock());
    if (DetourTransactionBegin() != NO_ERROR) return false;
    DetourUpdateThread(GetCurrentThread());
    for (const Target& t : targets) {
        const LONG r = DetourAttach(t.original, t.detour);
        if (r != NO_ERROR) {
            LOG_ERROR(std::format("[D3D9Hooks] DetourAttach({}) failed: {}", t.name, r));
            DetourTransactionAbort();
            for (const Target& u : targets) *u.original = nullptr;
            return false;
        }
    }
    const LONG r = DetourTransactionCommit();
    if (r != NO_ERROR) {
        LOG_ERROR(std::format("[D3D9Hooks] Commit failed: {}", r));
        for (const Target& u : targets) *u.original = nullptr;
        return false;
    }
    g_installed.store(true);
    LOG_INFO(std::format("[D3D9Hooks] Device hooks installed (render thread {}: draw and state hooks dispatched without a lock)", ThreadId()));
    return true;
}

bool IsInstalled() { return g_installed.load(); }

void Uninstall() {
    if (!g_installed.exchange(false)) return;
    std::lock_guard<std::recursive_mutex> detoursLock(DetourBatch::Lock());
    if (DetourTransactionBegin() != NO_ERROR) return;
    DetourUpdateThread(GetCurrentThread());
    for (const Target& t : Targets())
        if (*t.original) DetourDetach(t.original, t.detour);
    DetourTransactionCommit();
    // No new dispatch can start; wait for one the render thread may still be running, then free the retired lists. When
    // the wait timed out, or this is the render thread inside a dispatch (or any thread inside a locked one), they stay.
    const bool renderOut = ThreadId() != g_renderTid.load(std::memory_order_relaxed) ? WaitForRenderThread("Uninstall") : g_renderDepth == 0;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (renderOut && g_lockedDepth == 0) FreeAllRetired();
    else LOG_INFO("[D3D9Hooks] Uninstall: a dispatch may still be running, older hook lists kept");
}

} // namespace D3D9Hooks
