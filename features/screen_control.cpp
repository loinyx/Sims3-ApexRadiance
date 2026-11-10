// Display settings: window mode, resolution and frame-rate limiter (see screen_control.h). Credits: @loinyx
#include "screen_control.h"
#include "frame_limiter_math.h"
#include "apex_config.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "imgui.h"
#include "memory_patch.h"
#include "overlay.h"
#include "platform.h"
#include "s3ss_detect.h"
#include "ui/i18n.h"
#include "ui/violet_theme.h"
#include "ui/widgets.h"
#include <toml++/toml.hpp>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace Screen {
namespace {

// ---- settings ----
std::mutex g_lock; // g_settings, the game's request and the device state below
Settings g_settings;
std::once_flag g_loadOnce, g_startLogOnce;

Settings Clamped(Settings s) {
    s.mode = std::clamp(s.mode, 0, 3);
    if (s.width < 320 || s.height < 240 || s.width > 16384 || s.height > 16384) s.width = s.height = 0;
    s.fps = s.fps <= 0 ? 0 : std::clamp(s.fps, 20, 1000);
    s.backgroundFps = s.backgroundFps <= 0 ? 0 : std::clamp(s.backgroundFps, 5, 1000);
    return s;
}

const char* ModeKey(int m) {
    static const char* const kKeys[] = {"game", "windowed", "borderless", "fullscreen"};
    return kKeys[std::clamp(m, 0, 3)];
}

void EnsureLoaded() {
    std::call_once(g_loadOnce, [] {
        toml::table root;
        if (!ApexConfig::ReadRoot(root)) return;
        const auto t = root["screen"];
        Settings s;
        const std::string mode = t["window_mode"].value_or(std::string("game"));
        for (int m = 0; m < 4; m++)
            if (mode == ModeKey(m)) s.mode = m;
        s.width = static_cast<int>(t["width"].value_or(int64_t{0}));
        s.height = static_cast<int>(t["height"].value_or(int64_t{0}));
        s.fps = static_cast<int>(t["fps_limit"].value_or(int64_t{0}));
        s.backgroundFps = static_cast<int>(t["background_fps_limit"].value_or(int64_t{0}));
        s.showFps = t["show_fps"].value_or(false);
        std::lock_guard<std::mutex> lock(g_lock);
        g_settings = Clamped(s);
    });
    std::call_once(g_startLogOnce, [] { // the settings this session starts with, for bug reports
        const Settings s = g_settings;
        LOG_INFO(std::format("[Screen] At start: window {}, resolution {}, limit {}, background {}, FPS counter {}", ModeKey(s.mode),
                             s.width ? std::format("{}x{}", s.width, s.height) : std::string("game"), s.fps ? std::format("{} fps", s.fps) : std::string("off"),
                             s.backgroundFps ? std::format("{} fps", s.backgroundFps) : std::string("same"), s.showFps ? "on" : "off"));
    });
}

Settings Current() {
    EnsureLoaded();
    std::lock_guard<std::mutex> lock(g_lock);
    return g_settings;
}

// ---- device parameters (render thread) ----
D3DPRESENT_PARAMETERS g_gameRequest{}; // what the game (and Sims3SettingsSetter, which runs outside Apex's hook) asked for
D3DPRESENT_PARAMETERS g_written{};     // what Apex handed to the device last time
D3DPRESENT_PARAMETERS g_current{};     // the parameters of the device now
bool g_haveRequest = false, g_haveCurrent = false;
IDirect3D9* g_d3d = nullptr; // the game's (alive as long as its device)
UINT g_adapter = 0;
std::atomic<bool> g_deviceWindowed{true};
std::atomic<UINT> g_bbWidth{0}, g_bbHeight{0}; // the device's back buffer (read on the window thread)
std::atomic<HWND> g_window{nullptr};
std::atomic<bool> g_resetPending{false};
std::atomic<bool> g_applyPending{false};
std::atomic<bool> g_refused{false}; // the device refused Apex's last request; the game's own is in use
std::atomic<bool> g_s3ssBorderless{false};
std::atomic<bool> g_gameExclusive{false}; // the game's own request is exclusive fullscreen (read on the window thread)
std::atomic<bool> g_dropSaved{false}; // the window style saved under the bridge's Borderless is stale (window thread clears it)
std::once_flag g_s3ssBorderlessOnce;

// Highest refresh rate of a display mode of this size, 0 when the adapter has none
UINT BestRefresh(IDirect3D9* d3d, UINT adapter, UINT w, UINT h) {
    UINT best = 0;
    if (!d3d) return 0;
    const UINT n = d3d->GetAdapterModeCount(adapter, D3DFMT_X8R8G8B8);
    for (UINT i = 0; i < n; i++) {
        D3DDISPLAYMODE m{};
        if (SUCCEEDED(d3d->EnumAdapterModes(adapter, D3DFMT_X8R8G8B8, i, &m)) && m.Width == w && m.Height == h) best = std::max(best, m.RefreshRate);
    }
    return best;
}

// Under the RTX Remix bridge, exclusive fullscreen is not used: the bridge's 64-bit server would restyle the game's window
// from its own process (messages to the game's window thread) while that thread waits for the server to answer the
// Reset: the game hung for good (2026-11-10, AppHangB1 30 s after "Reset: ... exclusive fullscreen"). Borderless instead.
// The first CreateDevice hangs the same way when the game itself asks for fullscreen (no Options.ini: 1024x768 exclusive)
int EffectiveMode(int mode) {
    const bool exclusive = mode == static_cast<int>(WindowMode::Fullscreen) || (mode == static_cast<int>(WindowMode::Game) && g_gameExclusive.load());
    return exclusive && Platform::RemixBridge() ? static_cast<int>(WindowMode::Borderless) : mode;
}

// The request Apex makes of the game's (lock held). The game lays out UI, picking and input at its own resolution
// (renderer+0x2C, Options.ini): the back buffer keeps the game's size; Apex changes that size through the game (AfterPresent)
D3DPRESENT_PARAMETERS Wanted(const Settings& s) {
    D3DPRESENT_PARAMETERS pp = g_gameRequest;
    const int mode = EffectiveMode(s.mode);
    switch (static_cast<WindowMode>(mode)) {
    case WindowMode::Windowed:
    case WindowMode::Borderless:
        pp.Windowed = TRUE;
        pp.FullScreen_RefreshRateInHz = 0;
        if (mode == static_cast<int>(WindowMode::Borderless)) { // the back buffer may be stretched to the monitor
            pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            pp.Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
        }
        break;
    case WindowMode::Fullscreen:
        if (const UINT hz = BestRefresh(g_d3d, g_adapter, pp.BackBufferWidth, pp.BackBufferHeight)) {
            pp.Windowed = FALSE;
            pp.FullScreen_RefreshRateInHz = hz;
            if (pp.BackBufferFormat == D3DFMT_UNKNOWN) pp.BackBufferFormat = D3DFMT_X8R8G8B8;
            if (pp.SwapEffect == D3DSWAPEFFECT_COPY) pp.BackBufferCount = 1;
        }
        break;
    default: // the game's own exclusive fullscreen: the highest rate its size has
        if (!pp.Windowed && s.width > 0)
            if (const UINT hz = BestRefresh(g_d3d, g_adapter, pp.BackBufferWidth, pp.BackBufferHeight)) pp.FullScreen_RefreshRateInHz = hz;
        break;
    }
    return pp;
}

// ---- the game's own resolution (render thread) ----
// TS3W 0x0077D1A0 (w, h, hz, fullscreen) queues message 0x07983246 on the game's messenger; its handler (0x007F1B70 ->
// 0x007F19E0) checks the size against the game's mode list (0x0058E510: an exact match at 1024x768 and above), stores
// it, saves Options.ini and resets the device through the game's own path (0x006115C0 -> 0x00610AB0)
constexpr uintptr_t kRequestResolution = 0x0077D1A0, kRendererGetter = 0x0060E2C0, kResolutionHandler = 0x007F1B70;
constexpr uintptr_t kRendererGlobal = 0x011CECC8, kUiGlobal = 0x011D8C48;
int g_askedW = 0, g_askedH = 0, g_askedMode = -1, g_asks = 0;
ULONGLONG g_askAt = 0;
bool g_askWarned = false;

bool ResolutionRequestAvailable() {
    static const bool ok = [] {
        static constexpr BYTE kRequest[] = {0x57, 0xE8, 0xCA, 0xF2, 0xCC, 0xFF};                                // push edi; call 0x0044C470
        static constexpr BYTE kGetter[] = {0xA1, 0xC8, 0xEC, 0x1C, 0x01, 0xC3};                                 // mov eax,[0x011CECC8]; ret
        static constexpr BYTE kHandler[] = {0x83, 0xEC, 0x30, 0x8B, 0x44, 0x24, 0x34, 0x3D, 0x46, 0x32, 0x98, 0x07}; // cmp eax,0x07983246
        const bool v = GameAddr::IsFixed() && MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kRequestResolution), kRequest, sizeof kRequest) &&
                       MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kRendererGetter), kGetter, sizeof kGetter) &&
                       MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kResolutionHandler), kHandler, sizeof kHandler);
        if (!v) LOG_INFO("[Screen] Game resolution request not available on this game version");
        return v;
    }();
    return ok;
}

// Asks the game to switch its own resolution to the Apex one, every 10 s while the game has not taken it (a message sent
// before the game's UI listens is lost), at most kMaxAsks times, then one warning until the setting changes. The Apex
// setting takes precedence: a later change in the game's own Options is asked back, as at every start
constexpr int kMaxAsks = 6;
void RequestGameResolution(const Settings& s) {
    if (s.width <= 0 || g_resetPending.load() || !ResolutionRequestAvailable()) return;
    const uint8_t* renderer = *reinterpret_cast<uint8_t* const*>(kRendererGlobal);
    if (!renderer || !*reinterpret_cast<void* const*>(renderer + 0xC) || !*reinterpret_cast<void* const*>(kUiGlobal)) return;
    const UINT gw = *reinterpret_cast<const UINT*>(renderer + 0x2C), gh = *reinterpret_cast<const UINT*>(renderer + 0x30);
    const UINT w = static_cast<UINT>(s.width), h = static_cast<UINT>(s.height);
    if (g_askedW != s.width || g_askedH != s.height || g_askedMode != s.mode) g_askedW = s.width, g_askedH = s.height, g_askedMode = s.mode, g_asks = 0, g_askWarned = false;
    if (gw == w && gh == h) { // taken: a later change in the game's Options gets the full budget again
        g_asks = 0, g_askWarned = false;
        return;
    }
    if (renderer[0x8E]) return; // the game's own Reset for a size it took is still to come (0x0060E550)
    if (g_askWarned) return;
    const bool fs = !renderer[0x24]; // renderer+0x24: the game runs windowed
    // below 1024x768 the game snaps up to another mode (0x0058E510) and would save that one
    if (w < 1024 || h < 768) {
        LOG_INFO(std::format("[Screen] Game resolution {}x{} kept: {}x{} is below the game's 1024x768; the picture is scaled to the window", gw, gh, w, h));
        g_askWarned = true;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_asks && now - g_askAt < 10000) return;
    if (g_asks >= kMaxAsks) {
        LOG_WARNING(std::format("[Screen] The game did not take {}x{}: its resolution stays {}x{}, the picture is scaled to the window", w, h, gw, gh));
        g_askWarned = true;
        return;
    }
    IDirect3D9* d3d = nullptr;
    UINT adapter = 0;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        d3d = g_d3d, adapter = g_adapter;
    }
    // windowed: the game sets the rate to 0 itself; fullscreen: -1 would keep the current rate, which the new size may lack
    const UINT hz = fs ? BestRefresh(d3d, adapter, w, h) : 0xFFFFFFFF;
    if (fs && !hz) {
        LOG_WARNING(std::format("[Screen] No {}x{} display mode: the game's resolution stays {}x{}", w, h, gw, gh));
        g_askWarned = true;
        return;
    }
    LOG_INFO(std::format("[Screen] Game resolution {}x{} -> {}x{} (the game's own resolution change; it saves Options.ini)", gw, gh, w, h));
    reinterpret_cast<void(__cdecl*)(UINT, UINT, UINT, bool)>(kRequestResolution)(w, h, hz, fs);
    g_asks++;
    g_askAt = now;
}

bool Same(const D3DPRESENT_PARAMETERS& a, const D3DPRESENT_PARAMETERS& b) { return std::memcmp(&a, &b, sizeof a) == 0; }
// The device already runs the request: only the fields Apex sets, and a size of 0 (the window's) matches any. The runtime
// fills in sizes and formats on return, so a whole-structure compare would reset the device at every setting
bool Runs(const D3DPRESENT_PARAMETERS& want, const D3DPRESENT_PARAMETERS& now) {
    return !want.Windowed == !now.Windowed && (!want.BackBufferWidth || want.BackBufferWidth == now.BackBufferWidth) &&
           (!want.BackBufferHeight || want.BackBufferHeight == now.BackBufferHeight) && want.SwapEffect == now.SwapEffect &&
           (want.Windowed || want.FullScreen_RefreshRateInHz == now.FullScreen_RefreshRateInHz);
}

std::string Describe(const D3DPRESENT_PARAMETERS& pp) {
    return std::format("{}x{} {}{}", pp.BackBufferWidth, pp.BackBufferHeight, pp.Windowed ? "windowed" : "exclusive fullscreen",
                       pp.Windowed ? std::string() : std::format(" at {} Hz", pp.FullScreen_RefreshRateInHz));
}

// ---- the window (window thread) ----
constexpr LONG kFrameStyles = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME;
constexpr LONG kFrameExStyles = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;
LONG g_gameStyle = 0, g_gameExStyle = 0;
RECT g_gameRect{}; // the game's window before Apex changed it (Game setting puts it back)
bool g_styleSaved = false, g_applying = false;
ULONGLONG g_fightSecond = 0;
int g_fightCount = 0;
bool g_fightLogged = false;

UINT ApplyMessage() {
    static const UINT msg = RegisterWindowMessageW(L"ApexRadiance.Screen.Apply");
    return msg;
}

// The style and rectangle the window should have; false = leave it alone
bool Target(HWND hwnd, int mode, LONG& style, LONG& ex, RECT& r) {
    mode = EffectiveMode(mode);
    style = GetWindowLongW(hwnd, GWL_STYLE);
    ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    if (mode == static_cast<int>(WindowMode::Borderless)) {
        style = (style & ~(kFrameStyles | WS_MAXIMIZE)) | WS_POPUP;
        ex &= ~kFrameExStyles;
        r = mi.rcMonitor;
        return true;
    }
    if (mode == static_cast<int>(WindowMode::Windowed)) {
        style = (style & ~(WS_POPUP | WS_MAXIMIZE | WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        const bool styled = style == GetWindowLongW(hwnd, GWL_STYLE) && ex == GetWindowLongW(hwnd, GWL_EXSTYLE);
        RECT c{0, 0, static_cast<LONG>(g_bbWidth.load()), static_cast<LONG>(g_bbHeight.load())};
        AdjustWindowRectEx(&c, style, FALSE, ex);
        const RECT w = mi.rcWork;
        // no larger than the work area (Windows would clamp it, and the window would be put back again and again)
        const LONG cw = std::min(c.right - c.left, w.right - w.left), ch = std::min(c.bottom - c.top, w.bottom - w.top);
        RECT have{};
        GetWindowRect(hwnd, &have);
        // centred when it becomes a window; then where the player moved it
        const LONG x = styled ? have.left : std::max(w.left, w.left + (w.right - w.left - cw) / 2);
        const LONG y = styled ? have.top : std::max(w.top, w.top + (w.bottom - w.top - ch) / 2);
        r = RECT{x, y, x + cw, y + ch};
        return true;
    }
    if (mode == static_cast<int>(WindowMode::Game) && g_styleSaved) { // back to the style and place the game had before Apex changed them
        style = g_gameStyle;
        ex = g_gameExStyle;
        r = g_gameRect;
        return true;
    }
    return false;
}

bool NeedsApply(HWND hwnd, int mode) {
    LONG style, ex;
    RECT want{}, have{};
    if (IsIconic(hwnd) || !g_deviceWindowed.load() || !Target(hwnd, mode, style, ex, want)) return false;
    GetWindowRect(hwnd, &have);
    return style != GetWindowLongW(hwnd, GWL_STYLE) || ex != GetWindowLongW(hwnd, GWL_EXSTYLE) || !EqualRect(&want, &have);
}

void Apply(HWND hwnd, int mode) {
    LONG style, ex;
    RECT r{};
    if (IsIconic(hwnd) || !g_deviceWindowed.load() || !Target(hwnd, mode, style, ex, r)) return;
    if (!g_styleSaved && mode != static_cast<int>(WindowMode::Game)) {
        g_gameStyle = GetWindowLongW(hwnd, GWL_STYLE);
        g_gameExStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
        GetWindowRect(hwnd, &g_gameRect);
        g_styleSaved = true;
    }
    g_applying = true;
    SetWindowLongW(hwnd, GWL_STYLE, style);
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    g_applying = false;
    if (mode == static_cast<int>(WindowMode::Game)) g_styleSaved = false;
    LOG_INFO(std::format("[Screen] Window: {} at ({}, {}), {}x{}", ModeKey(mode), r.left, r.top, r.right - r.left, r.bottom - r.top));
}

// ---- frame-rate limiter (render thread) ----
LARGE_INTEGER g_freq{};
FrameLimiterMath::Schedule g_schedule;
FrameLimiterMath::Stats g_stats;
FrameLimiterMath::SpinMargin g_margin;
int64_t g_lastStart = 0;
HANDLE g_timer = nullptr;
bool g_timerHighRes = false, g_timerTried = false;
std::atomic<float> g_measuredFps{0}, g_measuredSpread{0};
// Overview: one point per second (the last 4 minutes) and the hitches (frames over 50 ms) of that time (render thread)
constexpr int kHistory = 240;
float g_history[kHistory] = {};
int g_historyCount = 0, g_historyNext = 0, g_secondFrames = 0;
int64_t g_secondStart = 0;
std::vector<int64_t> g_hitches;

int64_t Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void WaitUntil(int64_t target) {
    if (!g_timerTried) {
        g_timerTried = true;
        g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        g_timerHighRes = g_timer != nullptr;
        if (!g_timer) g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        LOG_INFO(std::format("[Screen] Frame limiter timer: {}", g_timerHighRes ? "high resolution" : g_timer ? "standard" : "none (spin only)"));
    }
    const int64_t freq = g_freq.QuadPart, margin = g_margin.Ticks(freq, g_timerHighRes);
    int64_t now = Now();
    if (g_timer && target - now > margin) {
        const int64_t wakeAt = target - margin;
        LARGE_INTEGER due;
        due.QuadPart = -((wakeAt - now) * 10000000 / freq); // relative, 100 ns units
        if (SetWaitableTimerEx(g_timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
            // alertable like the game's own sleeps (0x004E1320): queued APCs run here
            while (WaitForSingleObjectEx(g_timer, 1000, TRUE) == WAIT_IO_COMPLETION) {}
            now = Now();
            g_margin.Note(now - wakeAt, freq);
        }
    }
    while (Now() < target) _mm_pause();
}

// ---- Sims3SettingsSetter's limiter call in the render frame ----
uintptr_t g_s3ssSite = 0; // the 9-byte site; its rel32 is at +3
int32_t g_s3ssRel = 0;    // S3SS's own target
bool g_overridden = false;
ULONGLONG g_nextScan = 0; // S3SS patches from its own thread after the game starts: a miss is tried again a few times
int g_scans = 0;
constexpr int kMaxScans = 6;
std::atomic<bool> g_s3ssTickOnce{false}, g_s3ssPaused{false};

// What the game's code at this site did before any limiter: sleep 30 ms while the device is lost (renderer+0x8D).
// __stdcall like S3SS's function: it pops its argument, and the 5E after the call is the render frame's own pop esi.
void __stdcall AfterFrameNoLimit(const uint8_t* renderer) {
    if (renderer && renderer[0x8D]) SleepEx(30, TRUE);
}

// Only while an Apex limit is in force (no image scan for players who set none), every 5 s, kMaxScans times at most
void ScanS3SSLimiter() {
    const ULONGLONG now = GetTickCount64();
    if (g_scans >= kMaxScans || now < g_nextScan) return;
    g_nextScan = now + 5000;
    BYTE* base = nullptr;
    size_t size = 0;
    if (!S3SSDetect::Scan().s3ssLoaded || !MemPatch::GetModuleInfo(GetModuleHandleW(nullptr), &base, &size)) return;
    g_scans++;
    // ds: push esi; call DelayAfterFramePresentation; pop esi; jmp (docs/engine/timers-and-sleeps.md)
    const uintptr_t hit = MemPatch::ScanPattern(base, size, "3E 56 E8 ?? ?? ?? ?? 5E EB");
    int32_t rel = 0;
    if (!hit || !MemPatch::ReadBytes(hit + 3, &rel, 4) || !S3SSDetect::IsInS3SS(hit + 7 + rel)) {
        if (g_scans == kMaxScans) LOG_INFO("[Screen] Sims3SettingsSetter's frame limiter call not found in the render frame");
        return;
    }
    g_s3ssSite = hit;
    g_s3ssRel = rel;
    g_s3ssTickOnce = S3SSDetect::S3SSPatchBoolSettingEnabled("SmoothPatchPrecise", "Tick at most once per frame", false);
    LOG_INFO(std::format("[Screen] Sims3SettingsSetter's frame limiter call at {:#010x}{}", hit,
                         g_s3ssTickOnce ? " ('Tick at most once per frame' on: it stays, the simulation waits for it)" : ""));
}

void OverrideS3SSLimiter(bool apexLimits) {
    if (!g_s3ssSite && apexLimits) ScanS3SSLimiter();
    if (!g_s3ssSite) return;
    const int32_t mine = MemPatch::CalculateRelativeOffset(g_s3ssSite + 2, reinterpret_cast<uintptr_t>(&AfterFrameNoLimit));
    // what the call holds now (S3SS may patch its own site again): neither ours nor S3SS's = no longer ours to change
    int32_t now = 0;
    if (!MemPatch::ReadBytes(g_s3ssSite + 3, &now, 4) || (now != mine && now != g_s3ssRel)) {
        LOG_WARNING("[Screen] Sims3SettingsSetter's frame limiter call changed: left as it is");
        g_s3ssSite = 0, g_overridden = false, g_s3ssPaused = false;
        return;
    }
    g_overridden = now == mine;
    g_s3ssPaused = g_overridden;
    const bool want = apexLimits && !g_s3ssTickOnce.load();
    if (want == g_overridden) return;
    const int32_t from = now;
    const int32_t to = want ? mine : g_s3ssRel;
    const std::vector<BYTE> expected(reinterpret_cast<const BYTE*>(&from), reinterpret_cast<const BYTE*>(&from) + 4);
    const std::vector<BYTE> bytes(reinterpret_cast<const BYTE*>(&to), reinterpret_cast<const BYTE*>(&to) + 4);
    // render thread, inside Present: this thread is the only one that runs the site and it is not there now
    if (!MemPatch::WriteBytes(g_s3ssSite + 3, bytes, nullptr, &expected)) {
        LOG_WARNING("[Screen] Sims3SettingsSetter's frame limiter call changed: left as it is");
        g_s3ssSite = 0;
        return;
    }
    g_overridden = want;
    g_s3ssPaused = want;
    LOG_INFO(want ? "[Screen] Sims3SettingsSetter's frame limiter paused: Apex's limiter takes precedence"
                  : "[Screen] Sims3SettingsSetter's frame limiter given back");
}

bool Foreground() {
    const HWND fg = GetForegroundWindow(), w = g_window.load();
    return !w || fg == w || (fg && GetAncestor(fg, GA_ROOTOWNER) == GetAncestor(w, GA_ROOTOWNER));
}

// ---- menu ----
struct Resolution {
    int w, h;
};
std::vector<Resolution> g_resolutions;
UINT g_monitorHz = 0;
bool g_modesListed = false;

// The sizes of the device's adapter (the monitor the device runs on, which fullscreen uses; render thread)
void ListModes() {
    IDirect3D9* d3d = nullptr;
    UINT adapter = 0;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        d3d = g_d3d, adapter = g_adapter;
    }
    if (!d3d) return; // (no device yet: listed at the next frame)
    g_modesListed = true;
    g_resolutions.clear();
    if (D3DDISPLAYMODE cur{}; SUCCEEDED(d3d->GetAdapterDisplayMode(adapter, &cur))) g_monitorHz = cur.RefreshRate;
    const UINT n = d3d->GetAdapterModeCount(adapter, D3DFMT_X8R8G8B8);
    for (UINT i = 0; i < n; i++) {
        D3DDISPLAYMODE m{};
        if (FAILED(d3d->EnumAdapterModes(adapter, D3DFMT_X8R8G8B8, i, &m)) || m.Width < 1024 || m.Height < 768) continue; // the game snaps a smaller size up (0x0058E510)
        const Resolution r{static_cast<int>(m.Width), static_cast<int>(m.Height)};
        if (std::none_of(g_resolutions.begin(), g_resolutions.end(), [&](const Resolution& o) { return o.w == r.w && o.h == r.h; }))
            g_resolutions.push_back(r);
    }
    std::sort(g_resolutions.begin(), g_resolutions.end(), [](const Resolution& a, const Resolution& b) { return a.w != b.w ? a.w > b.w : a.h > b.h; });
}

} // namespace

Settings Get() { return Current(); }

void Set(const Settings& in) {
    EnsureLoaded();
    const Settings s = Clamped(in);
    bool reset = false;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (s == g_settings) return;
        g_settings = s;
        reset = g_haveRequest && g_haveCurrent && !Runs(Wanted(s), g_current);
    }
    LOG_INFO(std::format("[Screen] Settings: window {}, resolution {}, limit {} fps, background {} fps, FPS counter {}", ModeKey(s.mode),
                         s.width ? std::format("{}x{}", s.width, s.height) : std::string("game"), s.fps, s.backgroundFps, s.showFps ? "on" : "off"));
    g_refused = false;
    if (reset) g_resetPending = true; // applied by the next Present (AfterPresent)
    g_applyPending = true;
    ApexConfig::RequestSave();
}

void ResetDefaults() { Set(Settings{}); }

void SaveToToml(toml::table& root) {
    const Settings s = Current();
    toml::table t;
    t.insert("window_mode", ModeKey(s.mode));
    t.insert("width", static_cast<int64_t>(s.width));
    t.insert("height", static_cast<int64_t>(s.height));
    t.insert("fps_limit", static_cast<int64_t>(s.fps));
    t.insert("background_fps_limit", static_cast<int64_t>(s.backgroundFps));
    t.insert("show_fps", s.showFps);
    root.insert_or_assign("screen", std::move(t));
}

bool AdjustPresentParams(IDirect3D9* d3d, UINT adapter, HWND window, D3DPRESENT_PARAMETERS* pp, const char* where) {
    EnsureLoaded();
    if (!pp) return false;
    std::call_once(g_s3ssBorderlessOnce, [] { g_s3ssBorderless = S3SSDetect::S3SSBorderlessConfigured(); });
    if (window) g_window = window;
    std::lock_guard<std::mutex> lock(g_lock);
    if (d3d) {
        g_d3d = d3d;
        g_adapter = adapter;
    }
    // The game keeps its parameters in its own memory and hands the same structure to every Reset ([renderer+0xE0]+0x14C):
    // unchanged since Apex wrote it = the game asked for nothing new
    if (!g_haveRequest || !Same(*pp, g_written)) g_gameRequest = *pp;
    g_haveRequest = true;
    // Game setting under the bridge: the game's own switch to windowed ends the Borderless stand-in, and the style saved
    // then is the old fullscreen popup; the game's new windowed frame stays
    if (g_gameExclusive.exchange(!g_gameRequest.Windowed) && g_gameRequest.Windowed && g_settings.mode == static_cast<int>(WindowMode::Game) &&
        Platform::RemixBridge())
        g_dropSaved = true;
    const D3DPRESENT_PARAMETERS want = Wanted(g_settings);
    const bool changed = !Same(want, *pp);
    if (changed) {
        LOG_INFO(std::format("[Screen] {}: {} -> {} (Apex window {}{})", where, Describe(*pp), Describe(want), ModeKey(g_settings.mode),
                             g_s3ssBorderless.load() && g_settings.mode ? "; takes precedence over Sims3SettingsSetter's borderless window" : ""));
        if (EffectiveMode(g_settings.mode) == static_cast<int>(WindowMode::Fullscreen) && want.Windowed)
            LOG_WARNING(std::format("[Screen] No {}x{} display mode: fullscreen not used", want.BackBufferWidth, want.BackBufferHeight));
        *pp = want;
    }
    g_written = *pp;
    return changed;
}

void AfterDevice(HWND window, const D3DPRESENT_PARAMETERS* pp, bool usedApexRequest) {
    if (window) g_window = window;
    if (!pp) return;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_current = *pp;
        g_haveCurrent = true;
        g_written = *pp; // after a fallback the game's own request is in its memory now
    }
    g_deviceWindowed = pp->Windowed != FALSE;
    g_bbWidth = pp->BackBufferWidth;
    g_bbHeight = pp->BackBufferHeight;
    if (!usedApexRequest) {
        g_refused = true;
        LOG_WARNING("[Screen] The device refused Apex's display request: the game's own is in use");
    }
    g_applyPending = true;
}

void AfterPresent(IDirect3DDevice9*, HRESULT& hr) {
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    const Settings s = Current();
    const bool fg = Foreground();
    const int fps = fg || !s.backgroundFps ? s.fps : s.backgroundFps;
    OverrideS3SSLimiter(fps > 0); // (the limit in force now: a background-only limit leaves S3SS's own in front)
    if (SUCCEEDED(hr)) {
        const int64_t until = g_schedule.Plan(Now(), fps > 0 ? g_freq.QuadPart / fps : 0);
        if (until) WaitUntil(until);
    } else {
        g_schedule.Restart();
    }
    const int64_t start = Now(); // the next frame starts here
    if (g_lastStart) {
        const double ms = static_cast<double>(start - g_lastStart) * 1000.0 / static_cast<double>(g_freq.QuadPart);
        g_stats.Add(ms);
        // a hitch: well past both 50 ms and the frame time the limit asks for (a 10 fps background limit is no hitch)
        if (SUCCEEDED(hr) && ms > std::max(50.0, fps > 0 ? 1500.0 / fps : 0.0) && ms < 5000.0) g_hitches.push_back(start);
    }
    g_lastStart = start;
    ++g_secondFrames;
    if (!g_secondStart) g_secondStart = start;
    if (start - g_secondStart >= g_freq.QuadPart) {
        g_history[g_historyNext] = static_cast<float>(static_cast<double>(g_secondFrames) * static_cast<double>(g_freq.QuadPart) / static_cast<double>(start - g_secondStart));
        g_historyNext = (g_historyNext + 1) % kHistory;
        g_historyCount = std::min(g_historyCount + 1, kHistory);
        g_secondFrames = 0;
        g_secondStart = start;
        const int64_t cutoff = start - g_freq.QuadPart * kHistory;
        g_hitches.erase(g_hitches.begin(), std::lower_bound(g_hitches.begin(), g_hitches.end(), cutoff));
    }
    if (g_stats.count && g_stats.at % 30 == 0) {
        const double mean = g_stats.MeanMs();
        g_measuredFps = mean > 0 ? static_cast<float>(1000.0 / mean) : 0.0f;
        g_measuredSpread = static_cast<float>(g_stats.SpreadMs());
    }
    static int64_t s_nextLog = 0; // once a minute while a limit is set: what it achieves (bug reports)
    if (fps > 0 && start >= s_nextLog) {
        if (s_nextLog) LOG_INFO(std::format("[Screen] Limit {} fps{}: measured {:.1f} fps, frame time varies by {:.2f} ms (timer late by up to {:.2f} ms)", fps,
                                            fg ? "" : " (background)", g_measuredFps.load(), g_measuredSpread.load(),
                                            static_cast<double>(g_margin.worst) * 1000.0 / static_cast<double>(g_freq.QuadPart)));
        s_nextLog = start + g_freq.QuadPart * 60;
    }
    if (SUCCEEDED(hr)) RequestGameResolution(s);
    // A new window mode or resolution: the game's own device-lost path resets the device (TS3W 0x0061172C -> 0x00610AB0)
    if (SUCCEEDED(hr) && g_resetPending.exchange(false)) {
        LOG_INFO("[Screen] Display change: the game resets its device");
        hr = D3DERR_DEVICELOST;
        g_schedule.Restart();
    }
    const HWND w = g_window.load();
    if (w && Overlay::WndProcInstalled() && g_applyPending.exchange(false)) PostMessageW(w, ApplyMessage(), 0, 0);
}

bool OnWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM, LRESULT* result) {
    if (msg == ApplyMessage() && msg) {
        if (g_dropSaved.exchange(false)) g_styleSaved = false;
        const int mode = EffectiveMode(Current().mode);
        if (NeedsApply(hwnd, mode)) Apply(hwnd, mode);
        if (result) *result = 0;
        return true;
    }
    if (msg == WM_DISPLAYCHANGE) g_modesListed = false;
    // Someone else changed the style or moved the window (the game, Sims3SettingsSetter's borderless window): put it
    // back, at most 5 times a second
    const int mode = EffectiveMode(Current().mode);
    if (!g_applying && (mode == static_cast<int>(WindowMode::Windowed) || mode == static_cast<int>(WindowMode::Borderless)) &&
        hwnd == g_window.load() && ((msg == WM_STYLECHANGED && wp == static_cast<WPARAM>(GWL_STYLE)) || msg == WM_WINDOWPOSCHANGED || msg == WM_DISPLAYCHANGE) &&
        NeedsApply(hwnd, mode)) {
        const ULONGLONG second = GetTickCount64() / 1000;
        if (second != g_fightSecond) {
            g_fightSecond = second;
            g_fightCount = 0;
        }
        if (++g_fightCount <= 5) PostMessageW(hwnd, ApplyMessage(), 0, 0);
        else if (!g_fightLogged) {
            g_fightLogged = true;
            LOG_WARNING("[Screen] Another program keeps changing the game window");
        }
    }
    return false;
}

void RenderUI() {
    using ApexUi::IconId;
    Settings s = Current();
    if (!g_modesListed) ListModes();

    static const char* const kModes[] = {"Game setting", "Window", "Borderless", "Fullscreen"};
    static const char* const kModeTips[] = {"Leaves it to the game and other mods", "A window with a title bar, the size of the resolution",
                                            "No frame, covering the whole monitor", "Exclusive fullscreen at the monitor's highest refresh rate"};
    static const IconId kModeIcons[] = {IconId::None, IconId::AppWindow, IconId::Maximize, IconId::Monitor};
    if (ApexUi::SegmentedRow("Window mode", "How the game sits on your screen", "##ScreenMode", &s.mode, kModes, 4, kModeTips, kModeIcons, 0)) Set(s);

    // Resolution: the game's own, then the monitor's sizes (numbers: never translated)
    std::vector<std::string> names{I18n::Tr("Game setting")};
    int current = 0;
    for (const Resolution& r : g_resolutions) {
        names.push_back(std::format("{} \xC3\x97 {}", r.w, r.h));
        if (r.w == s.width && r.h == s.height) current = static_cast<int>(names.size()) - 1;
    }
    if (s.width && !current) { // a saved size this monitor does not list
        names.push_back(std::format("{} \xC3\x97 {}", s.width, s.height));
        current = static_cast<int>(names.size()) - 1;
    }
    std::vector<const char*> labels;
    for (const std::string& n : names) labels.push_back(n.c_str());
    if (ApexUi::SelectRow("Resolution", "The size the game draws at", "##ScreenResolution", &current, labels.data(), static_cast<int>(labels.size()), 220.0f, 0)) {
        const size_t i = static_cast<size_t>(current);
        s.width = i == 0 ? 0 : i - 1 < g_resolutions.size() ? g_resolutions[i - 1].w : s.width;
        s.height = i == 0 ? 0 : i - 1 < g_resolutions.size() ? g_resolutions[i - 1].h : s.height;
        Set(s);
    }

    // Frame-rate limit: off, common rates, just under the monitor's refresh rate (for G-SYNC / FreeSync), custom
    std::vector<int> rates{0, 30, 60, 120, 144, 165, 240};
    const int vrr = g_monitorHz > 60 ? static_cast<int>(g_monitorHz) - 3 : 0;
    if (vrr) rates.push_back(vrr);
    std::vector<std::string> rateNames{I18n::Tr("Off")};
    for (size_t i = 1; i < rates.size(); i++)
        rateNames.push_back(static_cast<int>(rates[i]) == vrr ? I18n::Trf("{} fps (variable refresh)", vrr) : std::format("{} fps", rates[i]));
    rateNames.push_back(I18n::Tr("Custom"));
    const int customIndex = static_cast<int>(rateNames.size()) - 1;
    static bool s_custom = false;
    int rate = customIndex;
    for (size_t i = 0; i < rates.size(); i++)
        if (rates[i] == s.fps) rate = static_cast<int>(i);
    if (!s.fps) s_custom = false; // (switched off elsewhere: the Overview, Turn off Apex, Reset all)
    if (s_custom) rate = customIndex;
    std::vector<const char*> rateLabels;
    for (const std::string& n : rateNames) rateLabels.push_back(n.c_str());
    if (ApexUi::SelectRow("Frame-rate limit", "Caps how many frames the game draws each second", "##ScreenFps", &rate, rateLabels.data(),
                          static_cast<int>(rateLabels.size()), 220.0f, 0)) {
        s_custom = rate == customIndex;
        if (s_custom) s.fps = s.fps ? s.fps : 120;
        else s.fps = rates[static_cast<size_t>(rate)];
        Set(s);
    }
    if (rate == customIndex) {
        float custom = static_cast<float>(s.fps);
        ApexUi::SliderOptions o;
        o.format = "%.0f fps";
        o.tooltip = "Any limit from 20 to 500";
        o.alignWidth = 220.0f; // the Frame-rate limit select's column, the value left of the track
        if (ApexUi::Slider("Custom limit", &custom, 20.0f, 500.0f, o)) {
            s.fps = static_cast<int>(custom + 0.5f);
            Set(s);
        }
    }
    static const int kBackground[] = {0, 10, 15, 30, 60};
    const std::string kBackgroundText[] = {I18n::Tr("Same limit"), "10 fps", "15 fps", "30 fps", "60 fps"}; // (numbers: never translated)
    const char* const kBackgroundNames[] = {kBackgroundText[0].c_str(), kBackgroundText[1].c_str(), kBackgroundText[2].c_str(), kBackgroundText[3].c_str(), kBackgroundText[4].c_str()};
    int bg = 0;
    for (int i = 0; i < 5; i++)
        if (kBackground[i] == s.backgroundFps) bg = i;
    if (ApexUi::SelectRow("In the background", "The limit while another window is in front", "##ScreenBackground", &bg, kBackgroundNames, 5, 220.0f, 0)) {
        s.backgroundFps = kBackground[bg];
        Set(s);
    }

    if (ApexUi::SwitchRow("Show FPS", &s.showFps, "A small frame counter in the corner of the screen", false)) Set(s);

    // What the limiter measures: frames per second and how even the frames are
    if (const float fps = g_measuredFps.load(); fps > 0) {
        const std::string value = I18n::Trf("{} fps, varies by {} ms", std::format("{:.1f}", fps), std::format("{:.2f}", g_measuredSpread.load()));
        if (ApexUi::BeginControlRow("Measured", "Frames per second and how much the frame time varies", ImGui::CalcTextSize(value.c_str()).x)) {
            ImGui::AlignTextToFramePadding();
            ApexUi::MutedText(value.c_str());
            ApexUi::EndControlRow();
        }
    }
    if (ApexUi::FilterActive()) return; // the notes are not drawn in the search results
    if (g_refused.load()) ApexUi::IconNote(IconId::TriangleAlert, "The graphics card refused this mode: the game's own is in use", VioletTheme::kWarning);
    if (s.mode != static_cast<int>(WindowMode::Borderless) && EffectiveMode(s.mode) == static_cast<int>(WindowMode::Borderless))
        ApexUi::IconNote(IconId::Info, "With the RTX Remix bridge, fullscreen runs as Borderless");
    // Sims3SettingsSetter: one note, only while it is loaded and Apex overrides it now (its "Tick at most once per frame",
    // which Apex leaves alone, is in the log only)
    const bool limit = g_s3ssPaused.load(), window = s.mode && g_s3ssBorderless.load() && S3SSDetect::Scan().s3ssLoaded;
    if (limit || window)
        ApexUi::IconNote(IconId::Info, limit && window ? "Takes precedence over Sims3SettingsSetter's frame-rate limit and borderless window"
                                       : limit         ? "Takes precedence over Sims3SettingsSetter's frame-rate limit"
                                                       : "Takes precedence over Sims3SettingsSetter's borderless window");
}

bool FpsCounterShown() { return Current().showFps; }

// A plain number in the top-left corner, with a dark outline so it reads on any picture; never in Apex screenshots
// (the caller skips it while one is pending)
void DrawFpsCounter() {
    const float fps = g_measuredFps.load();
    if (fps <= 0) return;
    char text[16];
    std::snprintf(text, sizeof text, "%.0f", fps);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float u = ApexUi::Unit();
    const ImVec2 p(ImGui::GetMainViewport()->Pos.x + 10.0f * u, ImGui::GetMainViewport()->Pos.y + 8.0f * u);
    for (const ImVec2 o : {ImVec2(-1, 0), ImVec2(1, 0), ImVec2(0, -1), ImVec2(0, 1)}) dl->AddText(ImVec2(p.x + o.x, p.y + o.y), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(p, IM_COL32(255, 255, 255, 230), text);
}

bool RenderFpsTile() {
    using VioletTheme::Col;
    const float u = ApexUi::Unit();
    const ImVec2 halfMin = ImGui::GetCursorScreenPos();
    const float halfW = ImGui::GetContentRegionAvail().x;
    const Settings s = Current();
    const float fps = g_measuredFps.load();
    // 1% low: the frame time 99% of the last 240 frames stay under
    float low = 0;
    if (g_stats.count >= 20) {
        std::vector<double> ms(g_stats.ms, g_stats.ms + g_stats.count);
        const size_t k = ms.size() * 99 / 100;
        std::nth_element(ms.begin(), ms.begin() + static_cast<std::ptrdiff_t>(k), ms.end());
        if (ms[k] > 0) low = static_cast<float>(1000.0 / ms[k]);
    }
    // Frame rate | limit, the rate large with a small line of the last 4 minutes beside it, then one line of figures
    const std::string limit = s.fps ? I18n::Trf("Limit {}", s.fps) : std::string(I18n::Tr("No limit"));
    ApexUi::StatusHeader("Frame rate", limit.c_str());
    char big[16];
    std::snprintf(big, sizeof big, fps > 0 ? "%.0f" : "-", fps);
    ImVec2 line{};
    const float lineH = ApexUi::StatusValue(big, "fps", &line);
    {
        // the sparkline: right-aligned, as tall as the value line and the band under it; the limit as a faint dashed line
        const float w = std::min(140.0f * u, ImGui::GetContentRegionAvail().x * 0.45f), h = lineH;
        const ImVec2 p1(line.x + ImGui::GetContentRegionAvail().x, line.y + h), p0(p1.x - w, line.y);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float top = static_cast<float>(s.fps), bottom = top > 0 ? top : 1e9f;
        for (int i = 0; i < g_historyCount; i++) {
            top = std::max(top, g_history[i]);
            bottom = std::min(bottom, g_history[i]);
        }
        const float lo = std::max(0.0f, std::min(bottom, top) * 0.8f), hi = std::max(top * 1.05f, lo + 1.0f);
        auto Y = [&](float v) { return p1.y - 2.0f * u - (h - 4.0f * u) * std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f); };
        auto X = [&](int i) { return p0.x + w * static_cast<float>(kHistory - g_historyCount + i) / static_cast<float>(kHistory - 1); };
        if (s.fps)
            for (float x = p0.x; x < p1.x; x += 6.0f * u)
                dl->AddLine(ImVec2(x, Y(static_cast<float>(s.fps))), ImVec2(std::min(x + 2.0f * u, p1.x), Y(static_cast<float>(s.fps))),
                            ImGui::GetColorU32(Col(VioletTheme::kAccent, 0.4f)), 1.0f * u);
        for (int i = 1; i < g_historyCount; i++) {
            const float a = g_history[(g_historyNext - g_historyCount + i - 1 + kHistory) % kHistory], b = g_history[(g_historyNext - g_historyCount + i + kHistory) % kHistory];
            dl->AddLine(ImVec2(X(i - 1), Y(a)), ImVec2(X(i), Y(b)), ImGui::GetColorU32(Col(VioletTheme::kAccent)), 1.6f * u);
        }
    }
    ApexUi::StatusBand(); // (empty here: Game memory draws its bar in it)
    const std::string footer = low > 0 ? I18n::Trf("1% low {} fps \xC2\xB7 varies {} ms \xC2\xB7 {} hitches in 4 min", std::format("{:.0f}", low),
                                                   std::format("{:.2f}", g_measuredSpread.load()), g_hitches.size())
                                       : std::string();
    ApexUi::StatusFooter(footer);
    // The whole half: click to open the Display page (limit, window mode)
    const ImVec2 halfMax(halfMin.x + halfW, ImGui::GetCursorScreenPos().y);
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || !ImGui::IsMouseHoveringRect(halfMin, halfMax, false)) return false;
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

std::string Summary() {
    static const char* const kNames[] = {"Game setting", "Window", "Borderless", "Fullscreen"};
    const Settings s = Current();
    std::string text = I18n::Tr(kNames[std::clamp(s.mode, 0, 3)]);
    if (const UINT w = g_bbWidth.load(), h = g_bbHeight.load(); w && h) text += std::format(" \xC2\xB7 {} \xC3\x97 {}", w, h);
    return text;
}

} // namespace Screen
