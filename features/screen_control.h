#pragma once
// Display settings (System > Display): the window mode, the resolution and a frame-rate limiter. Credits: @loinyx
//
// Window mode and resolution: applied to the game's own CreateDevice / Reset requests (framework/d3d9_bootstrap.cpp),
// on top of whatever the game and Sims3SettingsSetter asked for. A change while the game runs is applied through the
// game's own device-lost path (docs/features/display.md): the next Present reports D3DERR_DEVICELOST once, the game
// releases its resources and resets the device with its stored parameters (TS3W 0x00610AB0), and the Reset hook
// rewrites them. The window style and position are set on the window's thread (a message through Apex's window procedure).
//
// Frame-rate limiter: start-of-frame pacing right after Present on the render thread (features/frame_limiter_math.h),
// a high-resolution waitable timer and a short spin. When official Sims3SettingsSetter's Smooth Patch limiter call sits
// in the render frame (the 9 bytes at TS3W 0x00EC9FBA, found by pattern), its target is pointed at an Apex function that
// keeps only the game's 30 ms sleep while the device is lost, and put back when Apex's limiter is turned off.
//
// Turn off G-SYNC: VRR_APP_OVERRIDE set to "force off" in the NVIDIA driver profile of the game's exe and of the RTX Remix
// bridge's NvRemixBridge.exe (NVAPI driver settings, on a worker thread); the previous values are kept to put them back.
//
// Config: [screen] in ApexRadiance.toml (window_mode, width, height, fps_limit, background_fps_limit, show_fps, gsync_off,
// gsync_restore). Machine settings: not part of profiles.
#include <windows.h>
#include <d3d9.h>
#include <string>

namespace toml {
inline namespace v3 {
class table;
}
} // namespace toml

namespace Screen {

enum class WindowMode : int { Game = 0, Windowed = 1, Borderless = 2, Fullscreen = 3 };

struct Settings {
    int mode = 0;          // WindowMode
    int width = 0;         // 0 = the game's resolution
    int height = 0;
    int fps = 0;           // 0 = no Apex limit
    int backgroundFps = 0; // while another window has the focus; 0 = the same as fps
    bool showFps = false;  // a small frame counter in the corner of the screen
    bool operator==(const Settings&) const = default;
};

Settings Get();
void Set(const Settings& s); // saves; applies at once (a device reset when the mode or size changes)
void ResetDefaults();
void SaveToToml(toml::table& root); // [screen]

// D3D9 bootstrap (render thread). Adjust rewrites the game's request; true when it changed it (the caller retries with
// the game's own parameters when the changed request fails, then reports the result with AfterDevice).
bool AdjustPresentParams(IDirect3D9* d3d, UINT adapter, HWND window, D3DPRESENT_PARAMETERS* pp, const char* where);
void AfterDevice(HWND window, const D3DPRESENT_PARAMETERS* pp, bool usedApexRequest);
// D3D9Hooks after the original Present (render thread): the limiter's wait and a pending mode change
void AfterPresent(IDirect3DDevice9* device, HRESULT& hr);
// Apex's window procedure, before anything else; true = handled
bool OnWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);

void RenderUI(); // System > Display
// The frame counter (render thread, inside the overlay's ImGui frame, menu open or closed)
bool FpsCounterShown();
void DrawFpsCounter();
// Overview: the Frame rate half of the status card (render thread); true when the player clicked it (the caller opens
// the Display page)
bool RenderFpsTile();
// "Borderless · 3840 × 2160 · 165 fps": the window mode, the back buffer and the limit, translated
std::string Summary();

} // namespace Screen
