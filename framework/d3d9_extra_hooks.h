#pragma once
#include <d3d9.h>

// Detours for IDirect3DDevice9 methods that the D3D9Hooks registry does not cover.
// Installed once (on first use) and kept for the lifetime of the process; with no callbacks set they only cost an atomic load.
namespace ExtraHooks {

// Observers (one slot each, used by the Frame Capture diagnostic)
using ClearObserver = void (*)(IDirect3DDevice9* device, DWORD count, DWORD flags, D3DCOLOR color, float z);
using SetDepthStencilObserver = void (*)(IDirect3DDevice9* device, IDirect3DSurface9* requested);
using StretchRectObserver = void (*)(IDirect3DDevice9* device, IDirect3DSurface9* src, IDirect3DSurface9* dst, D3DTEXTUREFILTERTYPE filter);
using DrawUPObserver = void (*)(IDirect3DDevice9* device, const char* kind, D3DPRIMITIVETYPE type, UINT prims);

// Depth-stencil substitution (single owner, used by the depth blur):
//  - substitute: given the surface the game asks to bind, return the surface to actually bind
//  - report: given the surface actually bound, return the surface the game should see from GetDepthStencilSurface
using DepthSubstitute = IDirect3DSurface9* (*)(IDirect3DSurface9* requested);
using DepthReport = IDirect3DSurface9* (*)(IDirect3DSurface9* actual);

bool EnsureInstalled(IDirect3DDevice9* device);
bool IsInstalled();

void SetClearObserver(ClearObserver fn);
// Called before every Clear, separate from the observer slot (PostScene: a partial depth clear ends the scene)
using BeforeClear = void (*)(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags);
void SetBeforeClear(BeforeClear fn);
void SetSetDepthStencilObserver(SetDepthStencilObserver fn);
void SetStretchRectObserver(StretchRectObserver fn);
// Separate from the diagnostic observer: run before the game's scene snapshot is copied.
using BeforeStretchRect = void (*)(IDirect3DDevice9* device, IDirect3DSurface9* src, const RECT* srcRect,
                                  IDirect3DSurface9* dst, const RECT* dstRect, D3DTEXTUREFILTERTYPE filter);
void SetBeforeStretchRect(BeforeStretchRect fn);
void SetDrawUPObserver(DrawUPObserver fn);
void SetDepthSubstitution(DepthSubstitute substitute, DepthReport report);

// Call the real device methods, bypassing substitution
HRESULT RawSetDepthStencilSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface);
HRESULT RawGetDepthStencilSurface(IDirect3DDevice9* device, IDirect3DSurface9** surface);

} // namespace ExtraHooks
