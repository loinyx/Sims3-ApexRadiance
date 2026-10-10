#include "d3d9_bootstrap.h"
#include "apex_log.h"
#include "d3d9_hooks.h"
#include "hook_chain.h"
#include "hook_guard.h"
#include "memory_patch.h"
#include "overlay.h"
#include "render_callbacks.h"
#include "s3ss_detect.h"
#include "picture.h"
#include "captures.h"
#include "vulkan_driver_guard.h"
#include <detours/detours.h>
#include <atomic>
#include <cstring>
#include <format>
#include "load_timing.h"
#include <mutex>
#include <string>

namespace ApexD3D {
namespace {

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using EndScene_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

constexpr int kCreateDeviceSlot = 16; // IDirect3D9
constexpr int kResetSlot = 16;        // IDirect3DDevice9
constexpr int kEndSceneSlot = 42;     // IDirect3DDevice9

Direct3DCreate9_t o_create9 = nullptr;
CreateDevice_t o_createDevice = nullptr;
EndScene_t o_endScene = nullptr;
Reset_t o_reset = nullptr;

std::mutex g_installLock; // Detours transactions of this module, one at a time
std::atomic<bool> g_create9Hooked{false};
std::atomic<bool> g_createDeviceHooked{false};
std::atomic<bool> g_deviceHooked{false};
std::once_flag g_createDeviceOnce;

std::atomic<IDirect3DDevice9*> g_device{nullptr};
std::atomic<HWND> g_window{nullptr};
std::atomic<bool> g_frameInit{false};
std::atomic<bool> g_restartEffects{false};
std::atomic<bool> g_presentSeen{false};
std::atomic<unsigned long long> g_firstPresentTick{0};
std::atomic<bool> g_overlayDrawnThisFrame{false};
thread_local bool t_inEndScene = false;

bool Attach(void** target, void* detour, const char* what) {
    std::lock_guard<std::mutex> lock(g_installLock);
    LOG_INFO(std::format("[D3D] {} at {}: {}", what, HookChain::AddressText(*target), HookChain::DescribePrologue(*target)));
    std::lock_guard<std::recursive_mutex> detoursLock(DetourBatch::Lock());
    if (DetourTransactionBegin() != NO_ERROR) return false;
    DetourUpdateThread(GetCurrentThread());
    LONG r = DetourAttach(target, detour);
    if (r != NO_ERROR) {
        DetourTransactionAbort();
        LOG_ERROR(std::format("[D3D] DetourAttach({}) failed: {}", what, r));
        return false;
    }
    r = DetourTransactionCommit();
    if (r != NO_ERROR) {
        LOG_ERROR(std::format("[D3D] Detours commit ({}) failed: {}", what, r));
        return false;
    }
    return true;
}

// ---- per frame ----

void OnPresent(IDirect3DDevice9* dev) {
    // Consume after scene/overlay draws on the render thread, never during a UI callback.
    if (g_restartEffects.load() && dev && SUCCEEDED(dev->TestCooperativeLevel())) {
        g_restartEffects.store(false);
        RenderCallbacks::Fire(RenderCallbacks::restartEffects, dev);
        HookGuard::Try("Manual Picture recovery", [] { Picture::Get().RestartGraphics(); });
        LOG_INFO("[Recovery] Apex screen resources invalidated; settings retained; recreation follows on demand");
    }
    g_overlayDrawnThisFrame.store(false);
    HookGuard::ReportPending(); // exceptions caught since the last frame reach the log here, outside any catch block
    if (g_presentSeen.exchange(true)) return;
    g_firstPresentTick.store(GetTickCount64());
    LOG_INFO("[D3D] First Present");
    Overlay::InstallWndProc(); // after S3SS subclassed the window in its first EndScene: Apex sees messages first
    S3SSDetect::Rescan(); // every ASI is loaded by now
}

HWND DeviceWindow(IDirect3DDevice9* dev) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    HWND w = nullptr;
    if (SUCCEEDED(dev->GetCreationParameters(&cp))) w = cp.hFocusWindow;
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
        D3DPRESENT_PARAMETERS pp{};
        if (SUCCEEDED(sc->GetPresentParameters(&pp)) && pp.hDeviceWindow) w = pp.hDeviceWindow;
        sc->Release();
    }
    return w;
}

// First EndScene of the game's device (render thread)
void FrameInit(IDirect3DDevice9* dev) {
    g_device.store(dev);
    const HWND w = DeviceWindow(dev);
    if (w) g_window.store(w);
    if (!D3D9Hooks::Install(dev)) LOG_ERROR("[D3D] Device hooks not installed: effects and Night Lighting cannot draw");
    D3D9Hooks::RegisterPresent("ApexCore", [](D3D9Hooks::DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        OnPresent(ctx.device);
        return D3D9Hooks::HookAction::Continue;
    }, static_cast<D3D9Hooks::Priority>(-2000)); // before everything else (the profiler's frame start is -1000)
    Overlay::Init(dev, g_window.load());
    LOG_INFO("[D3D] Frame hooks ready");
}

HRESULT STDMETHODCALLTYPE Hooked_EndScene(IDirect3DDevice9* dev) {
    if (t_inEndScene || !dev) return o_endScene(dev);
    t_inEndScene = true;
    // 07/10, players' Runtime Error: nothing Apex does here may throw into the game's EndScene. Each step is caught on its
    // own (HookGuard: noted, then that step stays off), and the game's EndScene always runs, once.
    if (!g_frameInit.exchange(true)) HookGuard::Try("D3D frame init", [dev] { FrameInit(dev); });
    if (SUCCEEDED(dev->TestCooperativeLevel())) {
        const auto pictureEndScene = [dev] { Picture::Get().OnEndScene(dev); }; // one switch for both calls below
        RenderCallbacks::Fire(RenderCallbacks::endSceneBeforeOverlay, dev);
        HookGuard::Run("Picture before the overlay", [dev] { Picture::Get().BeforeOverlay(dev); });
        if (Overlay::IsVisible() && Captures::ScreenshotPending()) {
            // A report photo needs the grade too, without photographing the Apex menu.
            // Picture consumes frameReady, so the normal call below will not grade twice.
            HookGuard::Run("Picture at EndScene", pictureEndScene);
        }
        RenderCallbacks::Fire(RenderCallbacks::filteredSceneBeforeOverlay, dev);
        // once per frame (the Present hook clears the flag); every EndScene if the device hooks are missing
        if (!D3D9Hooks::IsInstalled() || !g_overlayDrawnThisFrame.exchange(true)) Overlay::Frame(dev);
        HookGuard::Run("Picture at EndScene", pictureEndScene);
    }
    const HRESULT hr = o_endScene(dev);
    t_inEndScene = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE Hooked_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    // 07/10, players' Runtime Error: each step around the game's Reset is caught on its own and runs again at the next
    // Reset (a release skipped for good would make every later Reset fail); the game's Reset always runs, once
    HookGuard::Try("Overlay before Reset", [] { Overlay::BeforeReset(); });
    RenderCallbacks::Fire(RenderCallbacks::preReset, dev);
    HookGuard::Try("Picture before Reset", [] { Picture::Get().BeforeReset(); });
    HRESULT hr = o_reset(dev, pp);
    if (SUCCEEDED(hr)) {
        LOG_INFO(std::format("[D3D] Device reset: {}x{}, {}", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
                             pp && pp->Windowed ? "windowed" : "exclusive fullscreen"));
        RenderCallbacks::Fire(RenderCallbacks::postReset, dev);
        HookGuard::Try("Overlay after Reset", [] { Overlay::AfterReset(); });
    } else {
        LOG_WARNING(std::format("[D3D] Reset failed (0x{:08X})", static_cast<unsigned>(hr)));
    }
    return hr;
}

void HookDevice(IDirect3DDevice9* dev) {
    if (g_deviceHooked.exchange(true)) {
        void** vt = *reinterpret_cast<void***>(dev);
        LOG_INFO(std::format("[D3D] Another HAL device created (EndScene {}): Apex stays on the first one", HookChain::AddressText(vt[kEndSceneSlot])));
        return;
    }
    void** vt = *reinterpret_cast<void***>(dev);
    o_endScene = reinterpret_cast<EndScene_t>(vt[kEndSceneSlot]);
    o_reset = reinterpret_cast<Reset_t>(vt[kResetSlot]);
    const bool ok = Attach(reinterpret_cast<void**>(&o_endScene), reinterpret_cast<void*>(&Hooked_EndScene), "IDirect3DDevice9::EndScene") &&
                    Attach(reinterpret_cast<void**>(&o_reset), reinterpret_cast<void*>(&Hooked_Reset), "IDirect3DDevice9::Reset");
    if (!ok) LOG_ERROR("[D3D] EndScene/Reset not hooked: Apex cannot draw");
}

HRESULT STDMETHODCALLTYPE Hooked_CreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    const bool hal = type == D3DDEVTYPE_HAL;
    HRESULT hr = o_createDevice(self, adapter, type, focus, flags, pp, out);
    if (SUCCEEDED(hr) && hal && out && *out) {
        LoadTiming::NoteDeviceCreated();
        LOG_INFO(std::format("[D3D] Game device created: {}x{}, {} (device {:#x}, back buffer format {}, multisample {}, flags {:#x})", pp ? pp->BackBufferWidth : 0,
                             pp ? pp->BackBufferHeight : 0, pp && pp->Windowed ? "windowed" : "exclusive fullscreen", reinterpret_cast<uintptr_t>(*out),
                             pp ? static_cast<int>(pp->BackBufferFormat) : 0, pp ? static_cast<int>(pp->MultiSampleType) : 0, flags));
        // The graphics card and driver (bug reports: vendor-specific behaviour)
        D3DADAPTER_IDENTIFIER9 id{};
        if (SUCCEEDED(self->GetAdapterIdentifier(adapter, 0, &id))) {
            const LARGE_INTEGER v = id.DriverVersion;
            LOG_INFO(std::format("[D3D] Graphics card: {} (vendor {:#06x}, device {:#06x}), driver {} {}.{}.{}.{}", id.Description, id.VendorId, id.DeviceId, id.Driver,
                                 HIWORD(v.HighPart), LOWORD(v.HighPart), HIWORD(v.LowPart), LOWORD(v.LowPart)));
        }
        LOG_INFO(std::format("[D3D] Adapter {} of {}; AMD Vulkan driver in the game: {}", adapter, self->GetAdapterCount(),
                             GetModuleHandleW(L"amdvlk32.dll") ? "loaded" : "not loaded"));
        HookDevice(*out);
        const HWND w = pp && pp->hDeviceWindow ? pp->hDeviceWindow : focus;
        if (w) g_window.store(w);
    }
    return hr;
}

void HookCreateDevice(IDirect3D9* d3d) {
    void** vt = *reinterpret_cast<void***>(d3d);
    o_createDevice = reinterpret_cast<CreateDevice_t>(vt[kCreateDeviceSlot]);
    if (Attach(reinterpret_cast<void**>(&o_createDevice), reinterpret_cast<void*>(&Hooked_CreateDevice), "IDirect3D9::CreateDevice")) {
        g_createDeviceHooked.store(true);
    } else {
        o_createDevice = nullptr;
        LOG_ERROR("[D3D] CreateDevice not hooked: Apex will not see the game's device");
    }
}

IDirect3D9* WINAPI Hooked_Direct3DCreate9(UINT sdk) {
    VulkanDriverGuard::BeforeDirect3DCreate(); // before DXVK loads the Vulkan loader (vulkan_driver_guard.h)
    IDirect3D9* d3d = o_create9(sdk);
    // The first caller installs the CreateDevice detour, on its own thread, before it returns (see the header).
    if (d3d) std::call_once(g_createDeviceOnce, [d3d] { HookCreateDevice(d3d); });
    return d3d;
}

bool HookCreate9(HMODULE d3d9) {
    if (g_create9Hooked.load()) return true;
    o_create9 = reinterpret_cast<Direct3DCreate9_t>(GetProcAddress(d3d9, "Direct3DCreate9"));
    if (!o_create9) {
        LOG_ERROR("[D3D] d3d9.dll has no Direct3DCreate9 export");
        return false;
    }
    if (!Attach(reinterpret_cast<void**>(&o_create9), reinterpret_cast<void*>(&Hooked_Direct3DCreate9), "d3d9!Direct3DCreate9")) {
        o_create9 = nullptr;
        return false;
    }
    g_create9Hooked.store(true);
    return true;
}

} // namespace

bool InstallFromDllMain() {
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9) {
        LOG_WARNING("[D3D] d3d9.dll is not loaded at Apex's DllMain: the init thread will retry");
        return false;
    }
    return HookCreate9(d3d9);
}

void EnsureInstalled() {
    if (g_create9Hooked.load()) return;
    // Late path (d3d9.dll was not loaded at DllMain). Wait for it, up to 30 s.
    HMODULE d3d9 = nullptr;
    for (int i = 0; i < 600 && !(d3d9 = GetModuleHandleW(L"d3d9.dll")); i++) Sleep(50);
    if (!d3d9) {
        LOG_ERROR("[D3D] d3d9.dll never loaded: no D3D features this session");
        return;
    }
    // If official S3SS is here, it may be attaching its own CreateDevice detour right now from its thread: wait (up to
    // ~3 s) until that prologue is a jump into S3SS, so the two Detours transactions never overlap.
    if (S3SSDetect::Rescan().s3ssLoaded) {
        auto create9 = reinterpret_cast<Direct3DCreate9_t>(GetProcAddress(d3d9, "Direct3DCreate9"));
        if (IDirect3D9* probe = create9 ? create9(D3D_SDK_VERSION) : nullptr) {
            const uintptr_t cd = reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(probe))[kCreateDeviceSlot]);
            bool s3ssFirst = false;
            for (int i = 0; i < 60 && !s3ssFirst; i++) {
                BYTE b[5] = {};
                if (MemPatch::ReadBytes(cd, b, sizeof b) && b[0] == 0xE9) {
                    int32_t rel = 0;
                    std::memcpy(&rel, b + 1, 4);
                    s3ssFirst = S3SSDetect::IsInS3SS(cd + 5 + rel);
                }
                if (!s3ssFirst) Sleep(50);
            }
            LOG_INFO(std::string("[D3D] Late install: S3SS's CreateDevice hook ") + (s3ssFirst ? "is in place" : "not seen after 3 s"));
            probe->Release();
        }
    }
    if (!HookCreate9(d3d9)) return;
    LOG_WARNING("[D3D] Direct3DCreate9 hooked late: if the game already created its device, Apex will not see it");
}

void Shutdown() {
    Overlay::Shutdown();
    D3D9Hooks::Uninstall();
    std::lock_guard<std::mutex> lock(g_installLock);
    std::lock_guard<std::recursive_mutex> detoursLock(DetourBatch::Lock());
    if (DetourTransactionBegin() != NO_ERROR) return;
    DetourUpdateThread(GetCurrentThread());
    if (o_endScene && g_deviceHooked.load()) DetourDetach(reinterpret_cast<void**>(&o_endScene), reinterpret_cast<void*>(&Hooked_EndScene));
    if (o_reset && g_deviceHooked.load()) DetourDetach(reinterpret_cast<void**>(&o_reset), reinterpret_cast<void*>(&Hooked_Reset));
    if (o_createDevice && g_createDeviceHooked.load()) DetourDetach(reinterpret_cast<void**>(&o_createDevice), reinterpret_cast<void*>(&Hooked_CreateDevice));
    if (o_create9 && g_create9Hooked.load()) DetourDetach(reinterpret_cast<void**>(&o_create9), reinterpret_cast<void*>(&Hooked_Direct3DCreate9));
    DetourTransactionCommit();
}

IDirect3DDevice9* Device() { return g_device.load(); }
HWND Window() { return g_window.load(); }
bool PresentSeen() { return g_presentSeen.load(); }
unsigned long long FirstPresentTick() { return g_firstPresentTick.load(); }

void RequestEffectsRestart() { g_restartEffects.store(true); }
bool EffectsRestartPending() { return g_restartEffects.load(); }

} // namespace ApexD3D
