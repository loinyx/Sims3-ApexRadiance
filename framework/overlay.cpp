#include "overlay.h"
#include "overlay_clock.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "hook_chain.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "ui/logo.h"
#include "ui/violet_theme.h"
#include <atomic>
#include <cmath>
#include <format>
#include <mutex>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Overlay {
namespace {

Client* g_client = nullptr;
std::mutex g_imguiLock; // ImGui is fed from the window thread and drawn on the render thread
std::atomic<bool> g_ready{false};
std::atomic<bool> g_visible{false};
std::atomic<bool> g_captureSuppressed{false};
std::atomic<bool> g_clearInput{false};
std::atomic<bool> g_wndProcInstalled{false};
constexpr LPARAM kSyntheticGameKey = 1ll << 25; // reserved LPARAM bit: stripped before the game's original procedure
HWND g_window = nullptr;
WNDPROC g_original = nullptr;
WPARAM g_eatKeyUp = 0; // the toggle key's key-up is eaten too (window thread only)
bool g_eatUp[256] = {}; // keys claimed by Client::CaptureKey: their key-up is eaten too (window thread only)
bool g_eatChar = false; // ... and the character their key-down produces
std::atomic<UINT> g_bbWidth{0}, g_bbHeight{0};
float g_appliedScale = 0.0f;
ImGuiStyle g_baseStyle;

bool IsClientMouseMessage(UINT msg) {
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        return true;
    default:
        return false;
    }
}

bool IsMouseMessage(UINT msg) { return IsClientMouseMessage(msg) || msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL; }

bool IsKeyboardMessage(UINT msg) {
    switch (msg) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_UNICHAR:
        return true;
    default:
        return false;
    }
}

// Client-area mouse position -> back buffer pixels (they differ when the back buffer is stretched to the window).
LPARAM ScaleMouse(HWND hwnd, LPARAM lp) {
    const UINT bw = g_bbWidth.load(), bh = g_bbHeight.load();
    RECT rc{};
    if (!bw || !bh || !GetClientRect(hwnd, &rc)) return lp;
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0 || (static_cast<UINT>(cw) == bw && static_cast<UINT>(ch) == bh)) return lp;
    const int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
    const int sx = static_cast<int>(std::lround(static_cast<double>(x) * bw / cw));
    const int sy = static_cast<int>(std::lround(static_cast<double>(y) * bh / ch));
    return MAKELPARAM(static_cast<WORD>(static_cast<short>(sx)), static_cast<WORD>(static_cast<short>(sy)));
}

LRESULT CALLBACK ApexWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT result = 0;
    if (g_client && g_client->OnWindowMessage(hwnd, msg, wp, lp, &result)) return result;

    // The screenshot temporarily sends F10 to the game itself. Mark those posted messages so they bypass Apex's F10
    // screenshot shortcut and are forwarded with a normal key-message LPARAM to the game's window procedure.
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYUP) && (lp & kSyntheticGameKey)) {
        const LPARAM gameLp = lp & ~kSyntheticGameKey;
        const LRESULT forwarded = CallWindowProcW(g_original, hwnd, msg, wp, gameLp);
        if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && g_client)
            g_client->GameKeyDown(wp, (gameLp & (1 << 30)) != 0);
        return forwarded;
    }

    // Bare F10 belongs to the game, even while ImGui wants keyboard input or a legacy binding uses it.
    const bool nativeF10 = wp == VK_F10 && GetKeyState(VK_CONTROL) >= 0 &&
                           GetKeyState(VK_SHIFT) >= 0 && GetKeyState(VK_MENU) >= 0;
    if (nativeF10 && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYUP)) {
        const LRESULT forwarded = CallWindowProcW(g_original, hwnd, msg, wp, lp);
        if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && g_client)
            g_client->GameKeyDown(wp, (lp & (1 << 30)) != 0);
        return forwarded;
    }

    // Apex's toggle chord (auto-repeat ignored); its key-up is eaten as well so the game never sees half of it
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && g_client && g_client->IsToggleKey(wp)) {
        if (!(lp & (1 << 30))) SetVisible(!g_visible.load());
        g_eatKeyUp = wp;
        return 0;
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && g_client && g_client->HotkeyDown(wp, (lp & (1 << 30)) != 0)) {
        g_eatKeyUp = wp;
        g_eatChar = true; // Ctrl+letter makes a control character
        return 0;
    }
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && g_eatKeyUp && wp == g_eatKeyUp) {
        g_eatKeyUp = 0;
        return 0;
    }
    if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATEAPP && !wp)) { // Alt+Tab and the like: the key-ups go elsewhere
        for (bool& b : g_eatUp) b = false;
        g_eatChar = false;
    }
    // Key-ups (and the character) of keys claimed on key-down, also when the menu closed in between
    const bool keyUp = msg == WM_KEYUP || msg == WM_SYSKEYUP;
    const bool keyDown = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const bool claimedUp = keyUp && wp < 256 && g_eatUp[wp];
    if (claimedUp) g_eatUp[wp] = false;
    const bool claimedChar = (msg == WM_CHAR || msg == WM_SYSCHAR) && g_eatChar;
    if (claimedChar || keyDown) g_eatChar = false;

    if (g_visible.load() && g_ready.load() && (IsMouseMessage(msg) || IsKeyboardMessage(msg) || msg == WM_SETFOCUS || msg == WM_KILLFOCUS)) {
        bool wantMouse = false, wantKeyboard = false;
        {
            std::lock_guard<std::mutex> lock(g_imguiLock);
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, IsClientMouseMessage(msg) ? ScaleMouse(hwnd, lp) : lp);
            const ImGuiIO& io = ImGui::GetIO();
            wantMouse = io.WantCaptureMouse;
            wantKeyboard = io.WantCaptureKeyboard || io.WantTextInput;
        }
        if (keyDown && wp < 256 && g_client && g_client->CaptureKey(wp)) {
            g_eatUp[wp] = true;
            g_eatChar = true;
            return 0;
        }
        if ((IsMouseMessage(msg) && wantMouse) || (IsKeyboardMessage(msg) && wantKeyboard)) return 0;
    }
    if (claimedUp || claimedChar) return 0;
    if (keyDown && g_client) g_client->GameKeyDown(wp, (lp & (1 << 30)) != 0);
    return CallWindowProcW(g_original, hwnd, msg, wp, lp);
}

} // namespace

void SetClient(Client* client) { g_client = client; }

void Init(IDirect3DDevice9* device, HWND window) {
    if (g_ready.load() || !device || !window) return;
    std::lock_guard<std::mutex> lock(g_imguiLock);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = ApexPaths::ImGuiIniFileUtf8(); // never the default imgui.ini in Game\Bin (S3SS's)
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
    VioletTheme::ApplyStyle(ImGui::GetStyle()); // Apex's own context: the Violet style is global here
    VioletTheme::LoadFonts(io);                   // before the first frame (the DX9 backend builds the atlas lazily)
    g_baseStyle = ImGui::GetStyle();
    if (!ImGui_ImplWin32_Init(window) || !ImGui_ImplDX9_Init(device)) {
        LOG_ERROR("[Overlay] ImGui backends failed to initialise");
        ImGui::DestroyContext();
        return;
    }
    g_window = window;
    ApexUi::SetLogoDevice(device);
    g_ready.store(true);
    LOG_INFO("[Overlay] ImGui ready (" + std::string(IMGUI_VERSION) + ")");
}

void InstallWndProc() {
    if (g_wndProcInstalled.load() || !g_window) return;
    const WNDPROC before = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_window, GWLP_WNDPROC));
    LOG_INFO("[Overlay] Window procedure before Apex: " + HookChain::AddressText(reinterpret_cast<const void*>(before)));
    SetLastError(0);
    g_original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ApexWndProc)));
    if (!g_original && GetLastError() != 0) {
        LOG_ERROR(std::format("[Overlay] SetWindowLongPtr failed ({})", GetLastError()));
        return;
    }
    g_wndProcInstalled.store(true);
    LOG_INFO("[Overlay] Window procedure subclassed (Apex first, capture-only)");
}

bool WndProcInstalled() { return g_wndProcInstalled.load(); }

void Frame(IDirect3DDevice9* device) {
    if (!g_ready.load() || !device) return;
    static FrameClock frameClock;
    const auto frameStart = FrameClock::Clock::now();
    const float frameDelta = frameClock.Step(frameStart);
    IDirect3DSurface9* bb = nullptr;
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        bb->GetDesc(&desc);
        bb->Release();
        g_bbWidth.store(desc.Width);
        g_bbHeight.store(desc.Height);
    }
    const bool always = g_client && g_client->AlwaysDraw(); // every frame (the client runs its shortcuts there)
    const bool draw = !g_captureSuppressed.load() && (g_visible.load() || always);
    if (!draw || !g_client) return;

    const auto beforeLock = FrameClock::Clock::now();
    std::lock_guard<std::mutex> lock(g_imguiLock);
    const auto afterLock = FrameClock::Clock::now();
    ImGuiIO& io = ImGui::GetIO();
    if (g_clearInput.exchange(false)) {
        io.ClearInputKeys();
        io.ClearInputMouse();
    }
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    io.DeltaTime = frameDelta;
    const auto prepared = FrameClock::Clock::now();
    if (desc.Width && desc.Height) io.DisplaySize = ImVec2(static_cast<float>(desc.Width), static_cast<float>(desc.Height));

    // Size of the UI: the user's text size times the resolution (1080p = 1). Paddings, spacing and rounding scale with the
    // text, so the layout keeps its proportions at every text size.
    // Grows slower than the resolution (4K = 1.57, not 2): high-resolution monitors are also bigger, so a straight 2x
    // looked oversized. 0.9 at 1080p keeps the menu compact there too.
    const float h = desc.Height ? std::fmin(std::fmax(static_cast<float>(desc.Height) / 1080.0f, 0.75f), 3.0f) : 1.0f;
    const float res = 0.9f * std::pow(h, 0.8f);
    const float scale = res * std::fmin(std::fmax(g_client->FontScale(), 0.5f), 3.0f);
    if (std::fabs(scale - g_appliedScale) > 0.001f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style = g_baseStyle;
        style.ScaleAllSizes(scale);
        g_appliedScale = scale;
    }
    ImGui::GetStyle().FontScaleMain = scale;
    // Keep the compact frame height exact even when Segoe UI falls back to a different base font.
    ImGui::GetStyle().FramePadding.y = std::fmax(0.0f, (VioletTheme::kControlCompact - VioletTheme::BaseFontSize()) * 0.5f * scale);

    ImGui::NewFrame();
    g_client->Draw();
    ImGui::EndFrame();
    ImGui::Render();
    const auto built = FrameClock::Clock::now();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    const auto submitted = FrameClock::Clock::now();
    const auto ms = [](auto from, auto to) { return std::chrono::duration<double, std::milli>(to - from).count(); };
    static auto lastSlowLog = FrameClock::Clock::time_point{};
    if (g_visible.load() && (ms(frameStart, submitted) > 50.0 || frameDelta > 0.05f) && submitted - lastSlowLog > std::chrono::seconds(10)) {
        lastSlowLog = submitted;
        LOG_WARNING(std::format("[Overlay] Slow panel: total {:.2f} ms; pre-lock {:.2f}, lock {:.2f}, backend {:.2f}, UI {:.2f}, DX9 submit {:.2f}; game frame {:.2f} ms; vertices {}, indices {}",
            ms(frameStart, submitted), ms(frameStart, beforeLock), ms(beforeLock, afterLock), ms(afterLock, prepared), ms(prepared, built), ms(built, submitted), frameDelta * 1000.0f,
            ImGui::GetDrawData()->TotalVtxCount, ImGui::GetDrawData()->TotalIdxCount));
    }
}

void BeforeReset() {
    if (!g_ready.load()) return;
    std::lock_guard<std::mutex> lock(g_imguiLock);
    ImGui_ImplDX9_InvalidateDeviceObjects();
}

void AfterReset() {
    if (!g_ready.load()) return;
    std::lock_guard<std::mutex> lock(g_imguiLock);
    ImGui_ImplDX9_CreateDeviceObjects();
}

void Shutdown() {
    if (g_wndProcInstalled.exchange(false) && g_window && g_original) SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original));
    if (g_ready.exchange(false)) {
        std::lock_guard<std::mutex> lock(g_imguiLock);
        ApexUi::ReleaseLogo();
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}

bool IsVisible() { return g_visible.load(); }

void SetVisible(bool visible) {
    if (visible && g_client && !g_client->CanOpen()) return;
    if (g_visible.exchange(visible) != visible && !visible) g_clearInput.store(true); // no stuck keys when it opens again
}

void SetCaptureSuppressed(bool suppressed) { g_captureSuppressed.store(suppressed); }

bool PostGameKeyPress(WPARAM vk) {
    if (!g_window || !g_original || !IsWindow(g_window)) return false;
    // Windows delivers a bare F10 as WM_SYSKEYDOWN/WM_SYSKEYUP: post it the same way a real press arrives.
    const bool sys = vk == VK_F10;
    const UINT downMsg = sys ? WM_SYSKEYDOWN : WM_KEYDOWN, upMsg = sys ? WM_SYSKEYUP : WM_KEYUP;
    const LPARAM scan = static_cast<LPARAM>((MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC) << 16) | 1 | kSyntheticGameKey);
    if (!PostMessageW(g_window, downMsg, vk, scan)) return false;
    const LPARAM up = scan | (1ll << 30) | (1ll << 31);
    if (!PostMessageW(g_window, upMsg, vk, up) && !PostMessageW(g_window, upMsg, vk, up))
        LOG_WARNING("[Overlay] Could not post the synthetic game key release");
    return true;
}

HWND Window() { return g_window; }

} // namespace Overlay
