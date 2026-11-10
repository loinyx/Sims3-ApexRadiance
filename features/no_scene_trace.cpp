// The no-scene frame trace (see no_scene_trace.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "no_scene_trace.h"
#include "build_flavor.h"
#include "d3d9_extra_hooks.h"
#include "d3d9_hooks.h"
#include "platform.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <map>

namespace NoSceneTrace {
namespace {

constexpr int kMinSceneDraws = 4;   // depth-tested back-buffer draws of a scene frame (as Picture and PostScene)
constexpr int kMaxFrames = 300;     // no-scene frames written per recording
constexpr int kMaxEvents = 48;      // lines per frame

// Render thread only (Mine)
bool g_on = false;
DWORD g_tid = 0;
IDirect3DDevice9* g_dev = nullptr; // the device seen at the first Present
bool g_hooks = false;              // ExtraHooks' trace detours: FVF, declaration and scissor are followed
DWORD g_start = 0;
uint32_t g_frame = 0;              // 0 = the frame the trace was armed in (partial, not written)

// The state set last (identities only: no reference is held)
IDirect3DSurface9* g_bb = nullptr; // the back buffer (read at every Present)
IDirect3DSurface9* g_rt0 = nullptr;
bool g_rt0Sub = false; // bound through the render-target substitute (High-precision scene)
bool g_rt0Known = false;
D3DVIEWPORT9 g_vp{};
const char* g_vpFrom = nullptr; // nullptr = not set since the trace started
RECT g_sc{};
bool g_scKnown = false;
char g_input = '?';                               // 'F' = SetFVF last, 'D' = SetVertexDeclaration last
DWORD g_fvf = 0;
IDirect3DVertexDeclaration9* g_decl = nullptr;     // the last SetVertexDeclaration (kept when an FVF replaces it)
// Position element of each declaration seen, read with GetDeclaration when it is set (the object's own copy of its
// elements, not device state). shortcut: keyed by pointer, a freed and reused pointer keeps the old layout; fine for a
// diagnostic, key by the elements if it ever misleads.
std::map<IDirect3DVertexDeclaration9*, std::string> g_declPos;

// The last Apex pass that saved the vertex input
const char* g_site = nullptr;
uint32_t g_siteFrame = 0;
IDirect3DVertexDeclaration9* g_siteDecl = nullptr;
bool g_siteStale = false, g_siteStaleVp = false;
struct SiteCount {
    int saves = 0, stale = 0, staleVp = 0;
};
std::map<std::string, SiteCount> g_sites;

// This frame
int g_depthDraws = 0, g_otherDraws = 0, g_events = 0, g_dropped = 0;
std::atomic<int> g_foreign{0}; // calls from another thread or device this frame (not traced)
bool g_scene = false;
std::string g_frameStart, g_buf;
// The recording
std::string g_out;
int g_noScene = 0, g_logged = 0;

bool Mine(IDirect3DDevice9* dev) {
    if (!g_on) return false;
    if (GetCurrentThreadId() == g_tid && (!g_dev || dev == g_dev)) return true;
    g_foreign.fetch_add(1, std::memory_order_relaxed); // so an empty frame reads as "not seen", not "nothing drawn"
    return false;
}
bool OnBackBuffer() { return g_rt0Known && g_bb && g_rt0 == g_bb; }

std::string Ptr(const void* p) { return p ? std::format("@{:08X}", reinterpret_cast<uintptr_t>(p)) : std::string("null"); }

std::string Input() {
    switch (g_input) {
    case 'F':
        return std::format("FVF {:#x}{}", g_fvf, (g_fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW ? " (XYZRHW)" : "");
    case 'D':
    {
        const auto it = g_declPos.find(g_decl);
        return std::format("decl {} {}{}", Ptr(g_decl), it != g_declPos.end() ? it->second : std::string("layout ?"),
                           g_site && g_decl == g_siteDecl ? " (= the decl the last Apex pass read back)" : "");
    }
    default: return "input ?";
    }
}

std::string State(IDirect3DDevice9* dev) {
    std::string rt = !g_rt0Known ? "?" : OnBackBuffer() ? "bb" : Ptr(g_rt0);
    if (g_rt0Sub) rt += " (substituted)";
    const std::string vp = g_vpFrom ? std::format("{},{} {}x{} z{:.2f}-{:.2f} ({})", g_vp.X, g_vp.Y, g_vp.Width, g_vp.Height, g_vp.MinZ, g_vp.MaxZ, g_vpFrom)
                                    : std::string("?");
    DWORD scOn = 0;
    dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scOn); // (the device's answer: render states are not followed)
    const std::string sc = !scOn ? std::string("off") : g_scKnown ? std::format("on {},{},{},{}", g_sc.left, g_sc.top, g_sc.right, g_sc.bottom) : std::string("on ?");
    return std::format("rt={} vp={} scissor={} {} vs{} ps{}", rt, vp, sc, g_hooks ? Input() : std::string("input not followed"),
                       D3D9Hooks::BoundVertexShader(dev) ? "+" : "-", D3D9Hooks::BoundPixelShader(dev) ? "+" : "-");
}

void Event(std::string text) {
    if (g_scene) return;
    if (g_events >= kMaxEvents) {
        g_dropped++;
        return;
    }
    g_events++;
    g_buf += "    ";
    g_buf += text;
    g_buf += '\n';
}

void OnSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* requested, IDirect3DSurface9* bound) {
    if (index != 0 || !Mine(dev)) return;
    g_rt0 = requested;
    g_rt0Sub = bound != requested;
    g_rt0Known = true;
    // Direct3D 9 sets the viewport to the whole new target
    D3DSURFACE_DESC desc{};
    if (bound && SUCCEEDED(bound->GetDesc(&desc))) {
        g_vp = {0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
        g_vpFrom = "SetRenderTarget reset";
    } else {
        g_vpFrom = nullptr;
    }
    if (!g_scene) Event(std::format("SetRenderTarget(0) {} {}x{}{}", OnBackBuffer() ? "bb" : Ptr(requested), desc.Width, desc.Height, g_rt0Sub ? " (substituted)" : ""));
}

void OnSetViewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* v) {
    if (!v || !Mine(dev)) return;
    g_vp = *v;
    g_vpFrom = "SetViewport";
    if (!g_scene) Event(std::format("SetViewport {},{} {}x{} on rt={}", v->X, v->Y, v->Width, v->Height, OnBackBuffer() ? "bb" : Ptr(g_rt0)));
}

void OnSetScissor(IDirect3DDevice9* dev, const RECT* r) {
    if (!r || !Mine(dev)) return;
    g_sc = *r;
    g_scKnown = true;
}

void OnSetFvf(IDirect3DDevice9* dev, DWORD fvf) {
    if (!Mine(dev)) return;
    g_input = 'F';
    g_fvf = fvf;
}

void OnSetDecl(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl) {
    if (!Mine(dev)) return;
    g_input = 'D';
    g_decl = decl;
    if (!decl || g_declPos.count(decl)) return;
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1]{};
    UINT n = MAXD3DDECLLENGTH + 1;
    std::string pos = "no position";
    if (FAILED(decl->GetDeclaration(el, &n))) pos = "layout ?", n = 0;
    for (UINT i = 0; i < n && el[i].Stream != 0xFF; i++) {
        if (el[i].Usage != D3DDECLUSAGE_POSITION && el[i].Usage != D3DDECLUSAGE_POSITIONT) continue;
        static constexpr const char* kType[] = {"float1", "float2", "float3", "float4"};
        pos = std::format("{} {}", el[i].Usage == D3DDECLUSAGE_POSITIONT ? "POSITIONT" : "POSITION",
                          el[i].Type < 4 ? std::string(kType[el[i].Type]) : std::format("type{}", el[i].Type));
        break;
    }
    g_declPos.emplace(decl, std::move(pos));
}

void OnClear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color) {
    if (!Mine(dev) || g_scene || !OnBackBuffer()) return;
    const std::string r = count && rects ? std::format(" first {},{},{},{}", rects[0].x1, rects[0].y1, rects[0].x2, rects[0].y2) : std::string();
    Event(std::format("Clear {}{}{} colour {:08X} rects {}{} | {}", flags & D3DCLEAR_TARGET ? "T" : "", flags & D3DCLEAR_ZBUFFER ? "Z" : "",
                      flags & D3DCLEAR_STENCIL ? "S" : "", color, count, r, State(dev)));
}

const char* Prim(D3DPRIMITIVETYPE t) {
    switch (t) {
    case D3DPT_TRIANGLELIST: return "tri";
    case D3DPT_TRIANGLESTRIP: return "strip";
    case D3DPT_TRIANGLEFAN: return "fan";
    case D3DPT_LINELIST: return "line";
    case D3DPT_LINESTRIP: return "linestrip";
    default: return "point";
    }
}

UINT Verts(D3DPRIMITIVETYPE t, UINT prims) {
    switch (t) {
    case D3DPT_TRIANGLELIST: return prims * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN: return prims + 2;
    case D3DPT_LINELIST: return prims * 2;
    case D3DPT_LINESTRIP: return prims + 1;
    default: return prims;
    }
}

void OnDraw(IDirect3DDevice9* dev, const char* kind, D3DPRIMITIVETYPE type, UINT prims, const void* up, UINT stride, UINT verts) {
    if (!Mine(dev) || g_scene) return;
    if (!OnBackBuffer()) {
        g_otherDraws++;
        return;
    }
    if (!std::strcmp(kind, "DIP") || !std::strcmp(kind, "DP")) { // as Picture counts the scene (not the UP draws)
        DWORD z = D3DZB_TRUE;
        dev->GetRenderState(D3DRS_ZENABLE, &z);
        if (z != D3DZB_FALSE && ++g_depthDraws >= kMinSceneDraws) {
            g_scene = true; // a scene frame: nothing of it is written
            std::string().swap(g_buf);
            return;
        }
    }
    // The area the vertices cover (first 4 floats: x, y, z, rhw for XYZRHW; clip-space x, y, z, w otherwise)
    std::string v0;
    if (up && stride >= 16) {
        const UINT n = std::min(verts ? verts : Verts(type, prims), 4096u);
        const float* f = static_cast<const float*>(up);
        float x0 = f[0], x1 = f[0], y0 = f[1], y1 = f[1], w0 = f[3], w1 = f[3];
        for (UINT i = 1; i < n; i++) {
            const float* v = reinterpret_cast<const float*>(static_cast<const char*>(up) + size_t(i) * stride);
            x0 = std::min(x0, v[0]), x1 = std::max(x1, v[0]), y0 = std::min(y0, v[1]), y1 = std::max(y1, v[1]);
            w0 = std::min(w0, v[3]), w1 = std::max(w1, v[3]);
        }
        v0 = std::format(" v0=({:.1f}, {:.1f}, {:.4f}, {:.4f}) {} vert(s) x {:.1f}..{:.1f} y {:.1f}..{:.1f} 4th {:.4f}..{:.4f} stride {}", f[0], f[1], f[2],
                         f[3], n, x0, x1, y0, y1, w0, w1, stride);
    }
    Event(std::format("{} {} x{} | {}{}", kind, Prim(type), prims, State(dev), v0));
}

void OnCopy(IDirect3DDevice9* dev, const char* kind, IDirect3DSurface9* dst, const RECT* r) {
    if (!Mine(dev) || g_scene || !g_bb || dst != g_bb) return;
    Event(std::format("{} into bb {}", kind, r ? std::format("{},{},{},{}", r->left, r->top, r->right, r->bottom) : std::string("whole")));
}

void EndFrame(int foreign) {
    if (g_scene) return;
    g_noScene++;
    if (g_logged >= kMaxFrames) return;
    g_logged++;
    g_out += std::format("[no-scene] frame {} +{:.3f} s: {} depth-tested back-buffer draw(s), {} draw(s) to other targets{}{}\n", g_frame,
                         (GetTickCount() - g_start) / 1000.0, g_depthDraws, g_otherDraws, g_dropped ? std::format(", {} line(s) over the cap", g_dropped) : std::string(),
                         foreign ? std::format(", {} call(s) from another thread/device (not traced)", foreign) : std::string());
    g_out += g_frameStart;
    g_out += g_buf;
}

void OnPresent(IDirect3DDevice9* dev) {
    if (!g_on) return;
    if (GetCurrentThreadId() != g_tid) {
        g_foreign.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!g_dev) {
        g_dev = dev;
        g_hooks = ExtraHooks::EnsureTraceHooks(dev);
        IDirect3DSurface9* rt = nullptr; // render target 0 now (once; followed through SetRenderTarget from here on)
        if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt) {
            g_rt0 = rt, g_rt0Known = true;
            rt->Release();
        }
    }
    if (dev != g_dev) {
        g_foreign.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        g_bb = bb;
        bb->Release();
    }
    const int foreign = g_foreign.exchange(0, std::memory_order_relaxed);
    if (g_frame) EndFrame(foreign);
    g_frame++;
    g_depthDraws = g_otherDraws = g_events = g_dropped = 0;
    g_scene = false;
    g_buf.clear();
    g_frameStart = std::format("    at start | {}{}\n", State(dev),
                               g_site ? std::format(" | last Apex save: {} in frame {}{}{}", g_site, g_siteFrame, g_siteStale ? " STALE" : "", g_siteStaleVp ? " STALE vp" : "") : std::string());
}

constexpr D3D9Hooks::Trace kTrace{OnSetRenderTarget, OnSetViewport, OnDraw, OnCopy, OnClear, OnSetFvf, OnSetDecl, OnSetScissor, OnPresent};

} // namespace

void Begin() {
    if (kPublicBuild && !Platform::RemixBridge()) return;
    g_on = true;
    g_tid = GetCurrentThreadId();
    g_dev = nullptr;
    g_hooks = false;
    g_start = GetTickCount();
    g_frame = 0;
    g_bb = g_rt0 = nullptr;
    g_rt0Known = g_rt0Sub = g_scKnown = false;
    g_vpFrom = nullptr;
    g_input = '?';
    g_decl = nullptr;
    g_site = nullptr;
    g_siteDecl = nullptr;
    g_siteStale = g_siteStaleVp = false;
    g_sites.clear();
    g_declPos.clear();
    g_foreign = 0;
    g_out.clear();
    g_buf.clear();
    g_noScene = g_logged = 0;
    D3D9Hooks::SetTrace(&kTrace);
}

std::string End() {
    if (!g_on) return "";
    D3D9Hooks::SetTrace(nullptr);
    g_on = false;
    std::string s = std::format("==== No-scene frame trace: {} frame(s), {} without a scene, {} written{} ====\n", g_frame ? g_frame - 1 : 0, g_noScene, g_logged,
                                g_noScene > g_logged ? std::format(" (the first {})", kMaxFrames) : std::string());
    s += "Values as SET through Apex's hooks (the game, other modules and Apex alike), not the device's Get* answers. bb = the back buffer; "
         "vp = the viewport in effect (SetRenderTarget resets it to the whole target); input = SetFVF or SetVertexDeclaration, whichever came last; "
         "vs/ps + = a shader is bound; decl layout = its position element (POSITIONT = pre-transformed, POSITION = transformed as clip space); "
         "v0 = the first vertex of a DrawPrimitiveUP / DrawIndexedPrimitiveUP (x, y, z, rhw for XYZRHW), then the x / y / 4th-float range of all its "
         "vertices; StretchRect / ColorFill into bb are listed with their rectangle.\n";
    if (!g_hooks) s += "WARNING: the FVF, declaration and scissor detours were not installed: those values are not followed.\n";
    s += "Apex passes that read the vertex input back to restore it (STALE = the declaration read back was set before the last SetFVF: a bridge "
         "client before patch 0012; the pass restores the FVF set last instead; STALE vp = the device answered a viewport other than the one set last, as after "
         "SetRenderTarget(0) on a stale client):\n";
    if (g_sites.empty()) s += "    none ran\n";
    for (const auto& [site, c] : g_sites) s += std::format("    {}: {} save(s), {} STALE, {} STALE vp\n", site, c.saves, c.stale, c.staleVp);
    s += g_out;
    std::string().swap(g_out);
    std::string().swap(g_buf);
    std::string().swap(g_frameStart);
    g_sites.clear();
    g_declPos.clear();
    return s;
}

void Saved(const char* site, IDirect3DVertexDeclaration9* decl, DWORD fvf) {
    if (!g_on || GetCurrentThreadId() != g_tid) return;
    // The device answered decl while the last call was SetFVF: under Direct3D 9 that is a declaration made for the FVF,
    // a stale one when it is the declaration set before the FVF
    const bool stale = g_hooks && g_input == 'F' && decl && decl == g_decl;
    // The viewport the pass reads back next (same answer: nothing is set in between) against the one set last
    D3DVIEWPORT9 vp{};
    const bool staleVp = g_dev && g_vpFrom && SUCCEEDED(g_dev->GetViewport(&vp)) &&
                         (vp.X != g_vp.X || vp.Y != g_vp.Y || vp.Width != g_vp.Width || vp.Height != g_vp.Height);
    g_site = site;
    g_siteFrame = g_frame;
    g_siteDecl = decl;
    g_siteStale = stale;
    g_siteStaleVp = staleVp;
    SiteCount& c = g_sites[site];
    c.saves++;
    c.stale += stale;
    c.staleVp += staleVp;
    if (!g_scene)
        Event(std::format("Apex save {}: read back decl {} FVF {:#x} vp {},{} {}x{}; set last: {}{}{}", site, Ptr(decl), fvf, vp.X, vp.Y, vp.Width,
                          vp.Height, g_hooks ? Input() : std::string("not followed"), stale ? " -> STALE" : "", staleVp ? " -> STALE vp" : ""));
}

} // namespace NoSceneTrace
