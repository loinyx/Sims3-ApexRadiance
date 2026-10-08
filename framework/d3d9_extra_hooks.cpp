#include "d3d9_extra_hooks.h"
#include "memory_patch.h"
#include "hook_guard.h"
#include "apex_log.h"
#include <atomic>
#include <format>
#include <vector>

namespace ExtraHooks {
namespace {

using Clear_t = HRESULT(__stdcall*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetDepthStencilSurface_t = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9*);
using GetDepthStencilSurface_t = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9**);
using StretchRect_t = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
using DrawPrimitiveUP_t = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUP_t = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);

Clear_t oClear = nullptr;
SetDepthStencilSurface_t oSetDepthStencilSurface = nullptr;
GetDepthStencilSurface_t oGetDepthStencilSurface = nullptr;
StretchRect_t oStretchRect = nullptr;
DrawPrimitiveUP_t oDrawPrimitiveUP = nullptr;
DrawIndexedPrimitiveUP_t oDrawIndexedPrimitiveUP = nullptr;

std::atomic<ClearObserver> g_clearObs{nullptr};
std::atomic<BeforeClear> g_beforeClear{nullptr};
std::atomic<SetDepthStencilObserver> g_setDsObs{nullptr};
std::atomic<StretchRectObserver> g_stretchObs{nullptr};
std::atomic<BeforeStretchRect> g_beforeStretch{nullptr};
std::atomic<DrawUPObserver> g_drawUpObs{nullptr};
std::atomic<DepthSubstitute> g_substitute{nullptr};
std::atomic<DepthReport> g_report{nullptr};

std::atomic<bool> g_installed{false};
std::atomic<bool> g_attempted{false};

HRESULT __stdcall HookedClear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
    if (auto before = g_beforeClear.load()) before(dev, count, rects, flags);
    if (auto obs = g_clearObs.load()) obs(dev, count, flags, color, z);
    return oClear(dev, count, rects, flags, color, z, stencil);
}

HRESULT __stdcall HookedSetDepthStencilSurface(IDirect3DDevice9* dev, IDirect3DSurface9* surface) {
    if (auto obs = g_setDsObs.load()) obs(dev, surface);
    IDirect3DSurface9* bind = surface;
    if (auto sub = g_substitute.load()) bind = sub(surface);
    return oSetDepthStencilSurface(dev, bind);
}

HRESULT __stdcall HookedGetDepthStencilSurface(IDirect3DDevice9* dev, IDirect3DSurface9** out) {
    HRESULT hr = oGetDepthStencilSurface(dev, out);
    if (SUCCEEDED(hr) && out && *out) {
        if (auto report = g_report.load()) {
            IDirect3DSurface9* shown = report(*out);
            if (shown && shown != *out) {
                shown->AddRef();
                (*out)->Release();
                *out = shown;
            }
        }
    }
    return hr;
}

HRESULT __stdcall HookedStretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* srcRect, IDirect3DSurface9* dst, const RECT* dstRect, D3DTEXTUREFILTERTYPE filter) {
    if (auto before = g_beforeStretch.load())
        HookGuard::Try("PostScene before scene copy", [&] { before(dev, src, srcRect, dst, dstRect, filter); });
    if (auto obs = g_stretchObs.load()) obs(dev, src, dst, filter);
    return oStretchRect(dev, src, srcRect, dst, dstRect, filter);
}

HRESULT __stdcall HookedDrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT prims, const void* data, UINT stride) {
    if (auto obs = g_drawUpObs.load()) obs(dev, "DPUP", type, prims);
    return oDrawPrimitiveUP(dev, type, prims, data, stride);
}

HRESULT __stdcall HookedDrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT minIndex, UINT numVerts, UINT prims, const void* idx, D3DFORMAT idxFmt, const void* data, UINT stride) {
    if (auto obs = g_drawUpObs.load()) obs(dev, "DIPUP", type, prims);
    return oDrawIndexedPrimitiveUP(dev, type, minIndex, numVerts, prims, idx, idxFmt, data, stride);
}

} // namespace

bool EnsureInstalled(IDirect3DDevice9* dev) {
    if (g_installed.load()) return true;
    if (!dev || g_attempted.exchange(true)) return false;

    void** vt = *reinterpret_cast<void***>(dev);
    // Indices already detoured by d3d9_hook.cpp / d3d9_hook_registry.cpp
    const int owned[] = {16, 17, 23, 28, 37, 41, 42, 47, 65, 81, 82, 91, 92, 94, 106, 107, 109};
    const int mine[] = {34, 39, 40, 43, 83, 84};
    for (int m : mine) {
        for (int o : owned) {
            if (vt[m] == vt[o]) {
                LOG_ERROR(std::format("[ExtraHooks] vtable[{}] shares code with vtable[{}], not installing", m, o));
                return false;
            }
        }
    }

    oStretchRect = reinterpret_cast<StretchRect_t>(vt[34]);
    oSetDepthStencilSurface = reinterpret_cast<SetDepthStencilSurface_t>(vt[39]);
    oGetDepthStencilSurface = reinterpret_cast<GetDepthStencilSurface_t>(vt[40]);
    oClear = reinterpret_cast<Clear_t>(vt[43]);
    oDrawPrimitiveUP = reinterpret_cast<DrawPrimitiveUP_t>(vt[83]);
    oDrawIndexedPrimitiveUP = reinterpret_cast<DrawIndexedPrimitiveUP_t>(vt[84]);

    std::vector<DetourBatch::Hook> hooks = {
        {reinterpret_cast<void**>(&oStretchRect), reinterpret_cast<void*>(HookedStretchRect)},
        {reinterpret_cast<void**>(&oSetDepthStencilSurface), reinterpret_cast<void*>(HookedSetDepthStencilSurface)},
        {reinterpret_cast<void**>(&oGetDepthStencilSurface), reinterpret_cast<void*>(HookedGetDepthStencilSurface)},
        {reinterpret_cast<void**>(&oClear), reinterpret_cast<void*>(HookedClear)},
        {reinterpret_cast<void**>(&oDrawPrimitiveUP), reinterpret_cast<void*>(HookedDrawPrimitiveUP)},
        {reinterpret_cast<void**>(&oDrawIndexedPrimitiveUP), reinterpret_cast<void*>(HookedDrawIndexedPrimitiveUP)},
    };
    if (!DetourBatch::InstallHooks(hooks)) {
        // a failed batch leaves nothing attached: the next caller may try again (a few times)
        static int failures = 0;
        const bool retry = ++failures < 5;
        LOG_ERROR(std::format("[ExtraHooks] Failed to install detours (attempt {}{})", failures, retry ? ", tried again later" : ", giving up"));
        if (retry) g_attempted.store(false);
        return false;
    }
    g_installed.store(true);
    LOG_INFO("[ExtraHooks] Installed (StretchRect, Set/GetDepthStencilSurface, Clear, DrawPrimitiveUP, DrawIndexedPrimitiveUP)");
    return true;
}

bool IsInstalled() { return g_installed.load(); }

void SetClearObserver(ClearObserver fn) { g_clearObs.store(fn); }
void SetBeforeClear(BeforeClear fn) { g_beforeClear.store(fn); }
void SetSetDepthStencilObserver(SetDepthStencilObserver fn) { g_setDsObs.store(fn); }
void SetStretchRectObserver(StretchRectObserver fn) { g_stretchObs.store(fn); }
void SetBeforeStretchRect(BeforeStretchRect fn) { g_beforeStretch.store(fn); }
void SetDrawUPObserver(DrawUPObserver fn) { g_drawUpObs.store(fn); }

void SetDepthSubstitution(DepthSubstitute substitute, DepthReport report) {
    g_substitute.store(substitute);
    g_report.store(report);
}

HRESULT RawSetDepthStencilSurface(IDirect3DDevice9* dev, IDirect3DSurface9* surface) {
    if (!g_installed.load()) return dev->SetDepthStencilSurface(surface);
    return oSetDepthStencilSurface(dev, surface);
}

HRESULT RawGetDepthStencilSurface(IDirect3DDevice9* dev, IDirect3DSurface9** surface) {
    if (!g_installed.load()) return dev->GetDepthStencilSurface(surface);
    return oGetDepthStencilSurface(dev, surface);
}

} // namespace ExtraHooks
