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

// Render-target substitution (single owner, the High-precision scene, scene_target.cpp). Its detours (GetRenderTargetData,
// ColorFill, GetRenderTarget) are installed by EnsureTargetHooks only, when that feature first turns on:
//  - access: called before StretchRect, ColorFill and GetRenderTargetData with the surfaces they read or write (a copy of
//    the back buffer must find the scene in it)
//  - report: given the surface actually bound at an index, return the surface GetRenderTarget should give the caller
using SurfaceAccess = void (*)(IDirect3DDevice9* device, IDirect3DSurface9* a, IDirect3DSurface9* b);
using TargetReport = IDirect3DSurface9* (*)(IDirect3DSurface9* actual);
bool EnsureTargetHooks(IDirect3DDevice9* device);
void SetTargetSubstitution(SurfaceAccess access, TargetReport report);
HRESULT RawGetRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9** surface);

#ifndef APEX_NO_DEV_TOOLS
// Development builds (the Frame Profiler's per-frame timeline): while counting is on, the GetRenderTargetData detour adds
// up its calls and their QueryPerformanceCounter time (off: one relaxed atomic load per call). Take returns both since
// its last call and starts over; false while the render-target detours are not installed (nothing is counted). The same
// calls are tallied by return address and the 4 costliest callers logged about every 10 s ("[ExtraHooks]
// GetRenderTargetData callers"); numbered development builds count and log them with the profiler off too.
void CountGetRenderTargetData(bool on);
bool TakeGetRenderTargetData(DWORD& calls, LONGLONG& qpcTicks);
#endif

// The diagnostic trace's own detours (SetScissorRect, SetVertexDeclaration, SetFVF; D3D9Hooks::Trace), installed with
// EnsureInstalled's on the first call and kept; until a trace is set they only cost an atomic load. The SetFVF and
// SetVertexDeclaration detours always note which of the two was called last (installed at the first frame under the RTX
// Remix bridge, d3d9_bootstrap.cpp).
bool EnsureTraceHooks(IDirect3DDevice9* device);

// For a pass that saved the vertex input with GetVertexDeclaration / GetFVF and restores it with "declaration if any,
// else FVF": when SetFVF was the call seen last, drops the declaration (released) and takes that FVF. RTX Remix bridges
// before patch 0012 keep the old declaration after SetFVF, and restoring it drew the game's fixed-function UI with a
// game shader's layout (a black quadrant on the loading screen). Unchanged while the detours are not installed. Under
// d3d9 and DXVK the result is the same: restoring the FVF set last. Call it right after the reads.
void UseVertexInputSetLast(IDirect3DVertexDeclaration9*& decl, DWORD& fvf);

// What UseVertexInputSetLast knows, for a state block around Apex's own work: Apply puts the vertex input back inside
// the runtime, unseen by the detours, so take a mark before the work and give it back after Apply (or the pass's own
// SetFVF would be restored over the game's declaration by the next pass).
struct VertexInputMark { bool fvfLast = false; DWORD fvf = 0; };
VertexInputMark MarkVertexInput();
void RestoreVertexInput(VertexInputMark mark);

} // namespace ExtraHooks
