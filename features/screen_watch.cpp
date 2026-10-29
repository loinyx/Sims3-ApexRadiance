// Screen pixels during a recording (see screen_watch.h).
#include "screen_watch.h"
#include "recorder.h"
#include "render_callbacks.h"
#include "overlay.h"
#include "apex_log.h"
#include <windows.h>
#include <algorithm>
#include <format>

namespace {

// rows above (-) and below (+) the mouse, as a share of the back buffer's height (at 2160: 162, 81, 40, 15 px)
constexpr float kRows[ScreenWatch::kColumn] = {-0.075f, -0.0375f, -0.0185f, -0.007f, 0.0f, 0.007f, 0.0185f, 0.0375f, 0.075f};
// the grid's columns and rows as shares of the screen (above the game's bottom bar)
constexpr float kGridXAt[ScreenWatch::kGridX] = {0.10f, 0.26f, 0.42f, 0.58f, 0.74f, 0.90f};
constexpr float kGridYAt[ScreenWatch::kGridY] = {0.18f, 0.38f, 0.58f, 0.78f};
constexpr int kRing = 4;             // copies in flight (read back once the GPU has done them)
constexpr size_t kMaxSamples = 4000; // 20 s at 200 fps

struct Slot {
    IDirect3DSurface9* rt = nullptr;
    IDirect3DQuery9* done = nullptr;
    DWORD tick = 0;
    bool pending = false;
};
Slot g_ring[kRing];
int g_next = 0, g_oldest = 0;
IDirect3DSurface9* g_sys = nullptr;
bool g_watching = false, g_failed = false, g_resetHooked = false, g_centre = false;
bool g_copied = false; // this frame's pixels were copied before the overlay (EndScene)
int g_x[ScreenWatch::kPoints] = {}, g_y[ScreenWatch::kPoints] = {}, g_w = 0, g_h = 0;
std::vector<ScreenWatch::Sample> g_samples;

void Free() {
    for (Slot& s : g_ring) {
        if (s.done) s.done->Release();
        if (s.rt) s.rt->Release();
        s = Slot{};
    }
    if (g_sys) g_sys->Release();
    g_sys = nullptr;
    g_next = g_oldest = 0;
}
void PreReset(IDirect3DDevice9*) {
    Free();
    if (g_watching) g_failed = true; // the rest of this recording has no pixels (its device objects are gone)
}

// The pixel under the mouse in back-buffer coordinates; false when the cursor is outside the game's window
bool MousePixel(IDirect3DDevice9* dev, const D3DSURFACE_DESC& bd, POINT& pixel) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    dev->GetCreationParameters(&cp);
    HWND wnd = cp.hFocusWindow ? cp.hFocusWindow : GetForegroundWindow();
    POINT p{};
    RECT cr{};
    if (!GetCursorPos(&p) || !ScreenToClient(wnd, &p) || !GetClientRect(wnd, &cr) || !PtInRect(&cr, p)) return false;
    const LONG cw = std::max<LONG>(1, cr.right - cr.left), ch = std::max<LONG>(1, cr.bottom - cr.top);
    pixel.x = std::clamp<LONG>(static_cast<LONG>(static_cast<double>(p.x) * bd.Width / cw), 0, static_cast<LONG>(bd.Width) - 1);
    pixel.y = std::clamp<LONG>(static_cast<LONG>(static_cast<double>(p.y) * bd.Height / ch), 0, static_cast<LONG>(bd.Height) - 1);
    return true;
}

bool Begin(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    D3DSURFACE_DESC bd{};
    bb->GetDesc(&bd);
    bb->Release();
    if (bd.Format != D3DFMT_X8R8G8B8 && bd.Format != D3DFMT_A8R8G8B8) return false;
    g_w = static_cast<int>(bd.Width);
    g_h = static_cast<int>(bd.Height);
    POINT pixel{static_cast<LONG>(bd.Width / 2), static_cast<LONG>(bd.Height / 2)};
    // the screen's centre when the mouse is elsewhere, or on the menu (the recording was started from it)
    g_centre = Overlay::MouseOverMenu() || !MousePixel(dev, bd, pixel);
    if (g_centre) pixel = POINT{static_cast<LONG>(bd.Width / 2), static_cast<LONG>(bd.Height / 2)};
    for (int i = 0; i < ScreenWatch::kColumn; i++) {
        g_x[i] = pixel.x;
        g_y[i] = std::clamp<int>(pixel.y + static_cast<int>(kRows[i] * static_cast<float>(bd.Height)), 0, g_h - 1);
    }
    for (int gy = 0; gy < ScreenWatch::kGridY; gy++)
        for (int gx = 0; gx < ScreenWatch::kGridX; gx++) {
            const int i = ScreenWatch::kColumn + gy * ScreenWatch::kGridX + gx;
            g_x[i] = std::clamp<int>(static_cast<int>(kGridXAt[gx] * static_cast<float>(bd.Width)), 0, g_w - 1);
            g_y[i] = std::clamp<int>(static_cast<int>(kGridYAt[gy] * static_cast<float>(bd.Height)), 0, g_h - 1);
        }
    GroundProfile::WatchBegin(pixel, g_w, g_h);
    for (Slot& s : g_ring)
        if (FAILED(dev->CreateRenderTarget(ScreenWatch::kPoints, 1, bd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &s.rt, nullptr)) ||
            FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &s.done))) {
            Free();
            return false;
        }
    if (FAILED(dev->CreateOffscreenPlainSurface(ScreenWatch::kPoints, 1, bd.Format, D3DPOOL_SYSTEMMEM, &g_sys, nullptr))) {
        Free();
        return false;
    }
    if (!g_resetHooked) {
        RenderCallbacks::Add(RenderCallbacks::preReset, PreReset);
        g_resetHooked = true;
    }
    return true;
}

// The oldest copies the GPU has done: read back without waiting
void Collect(IDirect3DDevice9* dev) {
    while (g_ring[g_oldest].pending) {
        Slot& s = g_ring[g_oldest];
        if (s.done->GetData(nullptr, 0, 0) != S_OK) return; // not done yet (no flush asked)
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(dev->GetRenderTargetData(s.rt, g_sys)) && SUCCEEDED(g_sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            ScreenWatch::Sample sample{};
            sample.tick = s.tick;
            const DWORD* px = static_cast<const DWORD*>(lr.pBits);
            for (int i = 0; i < ScreenWatch::kPoints; i++) {
                sample.rgb[i][0] = static_cast<unsigned char>((px[i] >> 16) & 0xFF);
                sample.rgb[i][1] = static_cast<unsigned char>((px[i] >> 8) & 0xFF);
                sample.rgb[i][2] = static_cast<unsigned char>(px[i] & 0xFF);
            }
            g_sys->UnlockRect();
            if (g_samples.size() < kMaxSamples) g_samples.push_back(sample);
        }
        s.pending = false;
        g_oldest = (g_oldest + 1) % kRing;
    }
}

// This frame's pixels into the next free slot (skipped when every slot is still in flight)
void Copy(IDirect3DDevice9* dev) {
    Slot& s = g_ring[g_next];
    if (s.pending) return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    if (!GroundProfile::WatchPoints(g_x + ScreenWatch::kGrid, g_y + ScreenWatch::kGrid))
        std::fill(g_x + ScreenWatch::kGrid, g_x + ScreenWatch::kPoints, 0), std::fill(g_y + ScreenWatch::kGrid, g_y + ScreenWatch::kPoints, 0);
    bool ok = true;
    for (int i = 0; i < ScreenWatch::kPoints && ok; i++) {
        const RECT src{g_x[i], g_y[i], g_x[i] + 1, g_y[i] + 1}, dst{i, 0, i + 1, 1};
        ok = SUCCEEDED(dev->StretchRect(bb, &src, s.rt, &dst, D3DTEXF_NONE));
    }
    bb->Release();
    if (!ok) {
        g_failed = true;
        return;
    }
    s.done->Issue(D3DISSUE_END);
    s.tick = GetTickCount();
    s.pending = true;
    g_next = (g_next + 1) % kRing;
}

// The end of the game's frame, right before the Apex overlay (menu, notices) is drawn on it: the frame as the game made it
void BeforeOverlay(IDirect3DDevice9* dev) {
    if (!g_watching || g_failed || g_copied || !dev || !g_sys) return;
    g_copied = true;
    Copy(dev);
}

} // namespace

namespace ScreenWatch {

void OnPresent(IDirect3DDevice9* dev) {
    const bool recording = Recorder::Active();
    if (recording && !g_watching) {
        g_samples.clear();
        g_samples.reserve(kMaxSamples);
        g_failed = !dev || !Begin(dev);
        g_watching = true;
        g_copied = false;
        if (g_failed) LOG_WARNING("[ScreenWatch] The screen pixels cannot be read on this device: the recording has none");
        else RenderCallbacks::Add(RenderCallbacks::filteredSceneBeforeOverlay, BeforeOverlay);
        return;
    }
    if (!recording && g_watching) {
        RenderCallbacks::Remove(RenderCallbacks::filteredSceneBeforeOverlay, BeforeOverlay);
        g_watching = false;
        GroundProfile::WatchEnd();
        if (dev && !g_failed) Collect(dev); // the copies already done
        Free();
        return;
    }
    if (!g_watching || g_failed || !dev || !g_sys) return;
    Collect(dev);
    if (!g_copied) Copy(dev); // no EndScene this frame reached the overlay: the frame as presented
    g_copied = false;
}

std::vector<Sample> Samples() { return g_samples; }
int PointX(int i) { return i >= 0 && i < kPoints ? g_x[i] : 0; }
int PointY(int i) { return i >= 0 && i < kPoints ? g_y[i] : 0; }
int Width() { return g_w; }
int Height() { return g_h; }
bool ColumnAtCentre() { return g_centre; }
bool Watching() { return g_watching && !g_failed; }
void Release() { Free(); }

} // namespace ScreenWatch
