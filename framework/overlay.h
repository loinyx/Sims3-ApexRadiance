#pragma once
// Apex's ImGui overlay: its own ImGui context (separate from official S3SS's, which lives in its own DLL), its own
// layout file (apex_radiance_imgui.ini) and window-procedure subclass.
//
// Input policy (capture only): the subclass is installed at Apex's first Present, i.e. after S3SS subclassed the window
// in its first EndScene, so Apex sees messages first. While the Apex menu is open, messages are fed to Apex's ImGui and
// eaten only when that ImGui wants them (mouse over an Apex window, a text field being edited) or when they are Apex's
// toggle chord; everything else goes on to S3SS's menu and the game. While the menu is closed nothing is fed or eaten.
// The client may also claim single keys (Client::CaptureKey: Alt to peek, B to compare, while the mouse is over the
// menu); their key-up and character are eaten with them. Alt+Tab is handled by Windows before any window sees it.
// Mouse coordinates and the display size are scaled whenever the back buffer differs from the window's client area
// (borderless fullscreen, resolution spoofing).
#include <windows.h>
#include <d3d9.h>

namespace Overlay {

// What the application draws and decides (apex_gui.cpp)
class Client {
  public:
    virtual ~Client() = default;
    virtual void Draw() = 0;                  // between ImGui::NewFrame and ImGui::Render
    virtual bool AlwaysDraw() = 0;            // draw even with the menu closed (a warning banner)
    virtual bool IsToggleKey(WPARAM vk) = 0;  // the toggle chord's key, with its modifiers held
    virtual float FontScale() = 0;
    virtual bool CanOpen() { return true; } // cached readiness; called on the window thread too
    // Before other clients see the message; true = handled, *result is returned to Windows
    virtual bool OnWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) = 0;
    // While the menu is open, a key press (WM_KEYDOWN / WM_SYSKEYDOWN, after ImGui saw it) that the game must not see
    // even though ImGui does not capture the keyboard (e.g. Alt or B while the mouse is over the menu). Its key-up and
    // the character it produces are eaten too. Window thread.
    virtual bool CaptureKey(WPARAM vk) {
        (void)vk;
        return false;
    }
    // Any key press (menu open or closed): true = one of Apex's other shortcuts (Hotkeys), eaten with its key-up and
    // character so the game never sees it. repeat = keyboard auto-repeat. Window thread.
    virtual bool HotkeyDown(WPARAM vk, bool repeat) {
        (void)vk;
        (void)repeat;
        return false;
    }
    // A key-down that survived Apex's shortcut / ImGui interception and will be forwarded to the game.
    virtual void GameKeyDown(WPARAM vk, bool repeat) { (void)vk; (void)repeat; }
};

void SetClient(Client* client);

void Init(IDirect3DDevice9* device, HWND window); // first EndScene (render thread)
void InstallWndProc();                             // first Present (render thread)
void Frame(IDirect3DDevice9* device);              // once per frame, inside EndScene
void BeforeReset();
void AfterReset();
void Shutdown(); // FreeLibrary only: restores the window procedure

bool IsVisible();
void SetVisible(bool visible);
void SetCaptureSuppressed(bool suppressed); // skip all Apex ImGui draw data for a player screenshot frame
bool PostGameKeyPress(WPARAM vk); // post a synthetic key press to the game, bypassing Apex shortcut interception
HWND Window();
bool WndProcInstalled();

} // namespace Overlay
