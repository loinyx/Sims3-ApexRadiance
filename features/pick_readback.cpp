// The mouse pick read back without waiting for the GPU (see pick_readback.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "pick_readback.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "d3d9_hooks.h"
#include "game_version.h"
#include "memory_patch.h"
#include "overlay.h"
#include "render_callbacks.h"
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstring>
#include <format>

namespace PickReadback {
namespace {

// Steam 1.67.2: the pick manager's per-frame update (Scene::BeginFrame calls it at 0x006EBC62). With
// RenderPickDelayReadback on, one update renders the pick target and sets [this+0x14]; the next reads it back
// (0x006DFF45 -> 0x00610660, GetRenderTargetData) and clears it. That copy waited for the GPU: about 2.5 ms every other
// frame under the RTX Remix bridge (1.35 s in 10 s, a far camera pan in Sunset Valley).
constexpr uintptr_t kUpdate = 0x006DFEC0;
constexpr uint8_t kUpdateBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x84, 0x07, 0x00, 0x00};
constexpr uintptr_t kReadbackDue = 0x14; // byte: the pick was rendered, its read-back is due this update
constexpr uint32_t kMaxWaitFrames = 4;   // then the game's read-back, waiting as before

using FnUpdate = void(__fastcall*)(void* self, void* edx);
FnUpdate g_original = nullptr;
std::atomic<IDirect3DDevice9*> g_dev{nullptr}; // the game's device (identity; Present)
IDirect3DQuery9* g_query = nullptr;            // render thread
IDirect3DDevice9* g_queryDev = nullptr;        // the device g_query was made on (identity)
void* g_pending = nullptr;                     // the manager whose read-back waits on g_query
uint32_t g_waits = 0;
std::atomic<uint64_t> c_deferred{0}, c_frames{0}, c_timedOut{0};

// ---- PickSkipWhenStill ----
// (Steam 1.67.2, read from the code) From 0x006DFF64 the update renders a new pick only when [this+0x15] is clear (the last
// read-back was resolved: ResolvePick 0x006E01B0, called just before the update at 0x006EBC5A, clears it) and either a
// request waits ([this+0x38] >= 0, set by the input handler 0x005989C0 on mouse moves and buttons; the slot keeps the latest
// until a render takes it, so a skip delays a request but never drops it) or the camera's +0x6FD byte is set and a cursor
// position was resolved ([this+0x48] > 0): the re-pick at the last position. The camera controller writes +0x6FD each frame
// from 0x00972510, which always returns 1 (0x0096BE55), so with the camera still and the mouse still the game still picks
// every other frame. The pick reads the camera [this+0] at +0x0C..+0x1B and +0x30..+0x6F (axes and eye), the viewport
// [this+0x20..0x37] and the filter and mode bytes [this+0x18..0x1F], [this+0x101..0x104]. Nothing else in the update runs
// per frame; every reader of the result (hover, clicks, drag and drop, Build/Buy, CAS) reads the last stored one.
constexpr uint64_t kRefreshMs = 250;  // a still scene is picked again at least this often (a Sim walking under the cursor)
constexpr uint32_t kMovingEvery = 4;  // while the camera moves: a pick at most every 4th update
constexpr uint64_t kLogEveryMs = 10000;
std::atomic<bool> g_skipOn{false};
struct View {
    uint8_t camera[16 + 64]; // camera +0x0C..+0x1B, +0x30..+0x6F
    uint8_t mgr[32 + 4];     // [this+0x18..0x37], [this+0x101..0x104]
};
// render thread
View g_last{};
bool g_haveLast = false;   // false: the next update picks
UINT g_lastInput = 0;      // Overlay::InputEvents at the last pick
std::atomic<uint64_t> g_lastPickTick{0};     // (read by the click gate on the window thread)
std::atomic<uint64_t> g_lastUpdateTick{0};   // the last update while the switch is on
std::atomic<bool> g_skippedSincePick{false}; // the stored result is older than the game's would be
uint32_t g_sinceReadBack = 0; // updates since the last read-back (a deferred read-back's waits count from the read-back, not the render)
uint64_t g_lastLogTick = 0;
std::atomic<uint32_t> c_runs{0}, c_still{0}, c_moving{0}, c_readbacks{0}, c_mgrChanged{0}; // never reset (TakeCounters keeps its own base)
std::atomic<int64_t> c_readbackTicks{0};

// ---- The click hold ----
// (Steam 1.67.2, read from the code) Clicks reach the game as window messages: Canvas_WndProc -> UIInput_ToWindowMessage
// 0x005989C0, which sets the pick request (0x006DEFF0: [this+0x38], [this+0x50]) and posts a UI message; the click's
// handler then reads the stored result (ResolvePick 0x006E01B0 writes the position [this+0x48], the object [this+0x58],
// the raw pick [this+0x60] and the world point [this+0x70]). A button press the game would get, while a pick was skipped
// since the stored one and that one is older than kFreshMs, is held in Apex's window procedure (Overlay::SetMouseHoldGate).
// The press counts as an input event, so the next update picks; that pick is read back at once (waiting for the GPU, as
// the game does), and once ResolvePick has stored it (it runs just before the update, 0x006EBC5A) the held messages are
// posted back in order. About 3 frames for the press; the game itself answers a press from a pick about 2 frames old
enum : int { kHoldNone, kHoldRender, kHoldReadBack, kHoldResolve };
constexpr uint64_t kFreshMs = 50;       // a stored pick younger than this is used as the game would use it
constexpr uint64_t kHoldMaxMs = 200;    // then the press goes on with the stored result
constexpr uint64_t kUpdateGoneMs = 100; // no update this long (menus, loading): no hold
std::atomic<int> g_hold{kHoldNone};
std::atomic<uint64_t> g_holdStart{0};
std::atomic<uint32_t> c_holds{0}, c_holdTimeouts{0};
std::atomic<uint64_t> c_holdMs{0};
uint32_t t_holds = 0, t_holdTimeouts = 0; // at the last log line
uint64_t t_holdMs = 0;

void ReleaseHold(bool timedOut) {
    if (g_hold.exchange(kHoldNone) == kHoldNone) return; // (first: a press held after this starts its own hold)
    Overlay::ReleaseHeldMouse();
    c_holdMs.fetch_add(GetTickCount64() - g_holdStart.load(), std::memory_order_relaxed);
    if (timedOut) c_holdTimeouts.fetch_add(1, std::memory_order_relaxed);
}

// Window procedure, under the overlay's hold lock, for a button press with no message held: true holds it
bool HoldClick() {
    if (!g_skipOn.load(std::memory_order_relaxed) || !g_skippedSincePick.load()) return false;
    const uint64_t now = GetTickCount64();
    if (now - g_lastUpdateTick.load() > kUpdateGoneMs || now - g_lastPickTick.load() < kFreshMs) return false;
    g_holdStart.store(now);
    g_hold.store(kHoldRender);
    c_holds.fetch_add(1, std::memory_order_relaxed);
    return true;
}
uint32_t t_runs = 0, t_still = 0, t_moving = 0, t_readbacks = 0, t_mgrChanged = 0; // at the last log line
int64_t t_readbackTicks = 0;

void ReleaseQuery() {
    if (g_query) g_query->Release();
    g_query = nullptr;
    g_pending = nullptr;
}

// This update renders a new pick (the tests of 0x006DFF64..0x006DFF88); `view` gets what that pick reads. False also
// when the manager or its camera cannot be read
bool WillPick(const uint8_t* m, bool& request, View& view) {
    __try {
        if (m[0x15]) return false;
        const uint8_t* const cam = *reinterpret_cast<const uint8_t* const*>(m);
        request = *reinterpret_cast<const int32_t*>(m + 0x38) >= 0;
        if (!cam || (!request && (!cam[0x6FD] || *reinterpret_cast<const int32_t*>(m + 0x48) <= 0))) return false;
        std::memcpy(view.camera, cam + 0x0C, 16);
        std::memcpy(view.camera + 16, cam + 0x30, 64);
        std::memcpy(view.mgr, m + 0x18, 32);
        std::memcpy(view.mgr + 32, m + 0x101, 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Before the update would render a new pick ([this+0x14] clear). True: skip it; the game keeps the last pick's result
bool SkipPick(const uint8_t* m, uint64_t now) {
    bool request = false;
    View v;
    if (!WillPick(m, request, v)) return false;
    const UINT input = Overlay::InputEvents();
    // Without the window procedure a click cannot be told from a mouse move: every request picks
    const bool mgrChanged = g_haveLast && std::memcmp(v.mgr, g_last.mgr, sizeof v.mgr) != 0;
    bool pick = !g_haveLast || input != g_lastInput || (request && !Overlay::WndProcInstalled()) || mgrChanged;
    if (mgrChanged) c_mgrChanged.fetch_add(1, std::memory_order_relaxed);
    if (!pick) {
        if (std::memcmp(v.camera, g_last.camera, sizeof v.camera) != 0) { // the camera moved since the last pick
            if (g_sinceReadBack < kMovingEvery - 1) { // read back, 2 skips, pick: every 4th update without deferral
                c_moving.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        } else if (!request && now - g_lastPickTick.load() < kRefreshMs) {
            c_still.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    g_last = v;
    g_haveLast = true;
    g_lastInput = input;
    g_lastPickTick.store(now);
    return false;
}

void LogStats(uint64_t now) {
    if (now - g_lastLogTick < kLogEveryMs) return;
    const uint32_t runs = c_runs.load(), still = c_still.load(), moving = c_moving.load(), rb = c_readbacks.load();
    const int64_t ticks = c_readbackTicks.load();
    const uint32_t holds = c_holds.load(), holdTimeouts = c_holdTimeouts.load();
    const uint64_t holdMs = c_holdMs.load();
    const uint32_t mgrChanged = c_mgrChanged.load();
    if (g_lastLogTick) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        LOG_INFO(std::format("[PickReadback] Last {:.0f} s: {} picks, {} skipped with nothing changed, {} skipped while the camera moved, {} read-backs {:.1f} ms, "
                             "{} clicks held for a new pick ({} ms, {} timed out), {} picked for a mode or viewport change",
                             static_cast<double>(now - g_lastLogTick) / 1000.0, runs - t_runs, still - t_still, moving - t_moving, rb - t_readbacks,
                             static_cast<double>(ticks - t_readbackTicks) * 1000.0 / static_cast<double>(f.QuadPart), holds - t_holds, holdMs - t_holdMs,
                             holdTimeouts - t_holdTimeouts, mgrChanged - t_mgrChanged));
    }
    t_runs = runs, t_still = still, t_moving = moving, t_readbacks = rb, t_readbackTicks = ticks;
    t_holds = holds, t_holdTimeouts = holdTimeouts, t_holdMs = holdMs, t_mgrChanged = mgrChanged;
    g_lastLogTick = now;
}

void CallTimed(void* self, void* edx) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    g_original(self, edx);
    QueryPerformanceCounter(&b);
    c_readbacks.fetch_add(1, std::memory_order_relaxed);
    c_readbackTicks.fetch_add(b.QuadPart - a.QuadPart, std::memory_order_relaxed);
    g_sinceReadBack = 0;
}

// (Steam 1.67.2, read from the code) With the flag set the update only reads back and returns (0x006DFF64 onwards, the
// next pick's request and render, runs only with it clear), so a deferred update skips nothing else.
// The read-back is due: the first time, an event query goes in behind the pick's draws and the update waits for the next
// frame; it reads back once the GPU has passed the query (no wait in the copy), or after kMaxWaitFrames
void __fastcall Hook_Update(void* self, void* edx) {
    IDirect3DDevice9* const dev = g_dev.load(std::memory_order_relaxed);
    if (g_query && g_queryDev != dev) ReleaseQuery(); // (another device: a query belongs to the one that made it)
    const bool due = *reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(self) + kReadbackDue) != 0;
    if (g_pending == self && !due) g_pending = nullptr; // the game dropped it
    g_sinceReadBack++;
    const uint8_t* const m = static_cast<const uint8_t*>(self);
    const bool skipOn = g_skipOn.load(std::memory_order_relaxed);
    if (skipOn) g_lastUpdateTick.store(GetTickCount64());
    int hold = g_hold.load();
    if (hold == kHoldResolve && !m[0x15]) { // ResolvePick stored the held press's pick just before this update
        ReleaseHold(false);
        hold = kHoldNone;
    }
    if (!due) {
        if (skipOn) {
            const uint64_t now = GetTickCount64();
            if (!kPublicBuild) LogStats(now);
            if (SkipPick(m, now)) {
                g_skippedSincePick.store(true);
                return;
            }
        }
        bool request;
        View v;
        if (WillPick(m, request, v)) {
            g_skippedSincePick.store(false);
            c_runs.fetch_add(1, std::memory_order_relaxed);
            if (hold == kHoldRender) g_hold.compare_exchange_strong(hold, kHoldReadBack);
        } else if (hold == kHoldRender && !m[0x15]) {
            ReleaseHold(false); // no pick will come (no cursor position, picking off): the press goes on as in the game
        }
        g_original(self, edx); // renders the next pick (or returns at once: nothing to pick)
        return;
    }
    if (hold != kHoldNone) { // a press waits: read back now (the held press's pick, or one rendered before it)
        if (g_pending == self) g_pending = nullptr;
        CallTimed(self, edx);
        if (hold == kHoldReadBack) g_hold.compare_exchange_strong(hold, kHoldResolve);
        return;
    }
    if (dev) {
        if (g_pending != self) {
            if (!g_query && FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_query))) g_query = nullptr;
            g_queryDev = dev;
            if (g_query && SUCCEEDED(g_query->Issue(D3DISSUE_END))) {
                g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH); // submit the pick's draws now
                g_pending = self, g_waits = 0;
                c_deferred.fetch_add(1, std::memory_order_relaxed);
                return; // (the flag stays set: the next update reads back)
            }
        } else if (g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE && ++g_waits < kMaxWaitFrames) {
            c_frames.fetch_add(1, std::memory_order_relaxed);
            return;
        } else if (g_waits >= kMaxWaitFrames) {
            c_timedOut.fetch_add(1, std::memory_order_relaxed);
        }
        g_pending = nullptr;
    }
    CallTimed(self, edx);
}

void OnPreReset(IDirect3DDevice9*) { // (render thread) the game reads its pick back as before until a new query
    ReleaseQuery();
    g_haveLast = false; // and picks again after the reset
}

} // namespace

void Install() {
    if (g_gameVersion != GameVersion::Steam) return; // (the address is Steam 1.67.2's; other builds keep the game's wait)
    if (std::memcmp(reinterpret_cast<const void*>(kUpdate), kUpdateBytes, sizeof kUpdateBytes) != 0) {
        LOG_WARNING("[PickReadback] The pick update's code differs from Steam 1.67.2's: left to the game");
        return;
    }
    g_original = reinterpret_cast<FnUpdate>(kUpdate);
    if (!DetourBatch::InstallHooks({{reinterpret_cast<void**>(&g_original), reinterpret_cast<void*>(&Hook_Update)}})) {
        g_original = nullptr;
        LOG_ERROR("[PickReadback] Could not hook the pick update");
        return;
    }
    D3D9Hooks::RegisterPresent("PickReadback", [](D3D9Hooks::DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        g_dev.store(ctx.device, std::memory_order_relaxed);
        if (g_hold.load() != kHoldNone && GetTickCount64() - g_holdStart.load() > kHoldMaxMs) ReleaseHold(true);
        return D3D9Hooks::HookAction::Continue;
    });
    RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
    Overlay::SetMouseHoldGate(&HoldClick);
    LOG_INFO("[PickReadback] On: the mouse pick is read back once the GPU has drawn it, without waiting");
}

bool Installed() { return g_original != nullptr; }

void SetSkipWhenStill(bool on) {
    if (g_skipOn.exchange(on) == on) return;
    if (!on) ReleaseHold(false);
    LOG_INFO(on ? "[PickReadback] Skip when still: on (re-picks wait while nothing changed, every 4th update while the camera moves)"
                : "[PickReadback] Skip when still: off");
}

std::string StatsText() {
    return std::format("{} pick read-backs deferred, {} frames waited, {} waited out; {} picks, {} skipped with nothing changed, {} skipped while moving; "
                       "{} clicks held for a new pick ({} ms, {} timed out)",
                       c_deferred.load(), c_frames.load(), c_timedOut.load(), c_runs.load(), c_still.load(), c_moving.load(), c_holds.load(),
                       c_holdMs.load(), c_holdTimeouts.load());
}

void TakeCounters(uint32_t& runs, uint32_t& still, uint32_t& moving, uint32_t& readbacks, int64_t& readbackTicks, uint32_t& holds) {
    static uint32_t r0 = 0, s0 = 0, m0 = 0, b0 = 0, h0 = 0;
    static int64_t k0 = 0;
    const uint32_t r = c_runs.load(), s = c_still.load(), m = c_moving.load(), b = c_readbacks.load(), h = c_holds.load();
    const int64_t k = c_readbackTicks.load();
    runs = r - r0, still = s - s0, moving = m - m0, readbacks = b - b0, readbackTicks = k - k0, holds = h - h0;
    r0 = r, s0 = s, m0 = m, b0 = b, k0 = k, h0 = h;
}

} // namespace PickReadback
