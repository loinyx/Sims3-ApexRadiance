// Shared trigger for the post-scene effects (see post_scene.h).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "post_scene.h"
#include "shader_cache.h"
#include "d3d9_hooks.h"
#include "depth_share.h"
#include "d3d9_extra_hooks.h"
#include "render_callbacks.h"
#include "apex_log.h"
#include "world_session.h"
#include "hook_guard.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>
#include "f10_study.h"

namespace {

constexpr const char* kHookName = "PostScene";
constexpr int kMinSceneDraws = 4; // backbuffer draws with depth test before the UI can start (8 until 06/10 night: an empty Edit in Game world drew 7, the bare terrain; 20 until 06/10: a CAW world in edit-in-game mode draws 13 scene draws at some angles, so the effects never ran there and Color came and went with the camera; the UI glass passes no longer count, see OnGameDraw)
constexpr float kDefaultDepthA = 1.00008f; // LightProbe-m80

std::mutex g_mutex;
std::vector<std::pair<int, PostScene::Effect>> g_effects; // sorted by order
std::vector<PostScene::Effect> g_noDepth; // of those, the ones that also run without the shared depth (Add needsDepth = false)
bool g_depthValid = true;               // SceneDepthValid: false while those run on a scene drawn with another depth-stencil
bool g_sceneOnSharedDepth = false;      // this frame: a scene draw had the shared INTZ depth bound (checked until one has)
bool g_hooks = false;
bool g_uiDrawSeen = false; // this frame: a depth-off back-buffer draw after scene draws (render thread)
IDirect3DSurface9* g_curRT0 = nullptr;     // identity only
IDirect3DSurface9* g_backBuffer = nullptr; // identity only
int g_sceneDraws = 0;
int g_depthWrites = 0; // of those, the ones that write depth (the drawn world; a frozen screen such as the save screen has almost none)
int g_lastDepthWrites = 0; // the count of the last complete frame (read at any time of the next frame: InWorld)
bool g_done = false;
bool g_tileBoundary = false;
IDirect3DSurface9* g_knownTileTarget = nullptr; // identity only, learned with UI visible; cleared at Reset
bool g_rejectedBoundary = false; // no late composite over UI already drawn after an invalid boundary

bool SceneDepthReady(IDirect3DDevice9* dev) {
    IDirect3DSurface9* expected = DepthShare::Surface();
    if (!expected) return true; // colour-only effects need no depth swap
    IDirect3DSurface9* bound = nullptr;
    ExtraHooks::RawGetDepthStencilSurface(dev, &bound);
    const bool ready = bound && bound == expected;
    if (bound) bound->Release();
    return ready;
}

// The effects of this boundary: all of them, or with the scene drawn on another depth-stencil than the shared one (06/10: a
// friend's Edit in Game; every boundary was rejected for it, so Color never ran until it was turned off and on, which dropped
// its depth request and with it the swap) only the ones that need no depth. The others stay pending as before.
// 07/10, players' Runtime Error: an effect that throws is caught (the game's draw or EndScene goes on), noted, and skipped
// from then on; the others keep running
HookGuard::OffList<16> g_failedEffects;
void RunEffects(IDirect3DDevice9* dev, bool depthOk) {
#ifdef APEX_F10_STUDY
    if (F10Study::active) {
        F10Study::Note("chain begin | " + PostScene::DiagText() + " depthValid=" + std::to_string(depthOk));
        IDirect3DSurface9* actual = nullptr;
        ExtraHooks::RawGetDepthStencilSurface(dev, &actual);
        F10Study::Note("rawDS=" + std::to_string(reinterpret_cast<uintptr_t>(actual)) +
                       " sharedDS=" + std::to_string(reinterpret_cast<uintptr_t>(DepthShare::Surface())));
        if (actual) actual->Release();
        F10Study::Sample(dev, "before-effects");
    }
#endif
    std::vector<std::pair<int, PostScene::Effect>> run;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& e : g_effects)
            if (depthOk || std::find(g_noDepth.begin(), g_noDepth.end(), e.second) != g_noDepth.end()) run.push_back(e);
    } catch (...) {
        HookGuard::Note("PostScene effect list");
        return;
    }
    g_depthValid = depthOk;
    for (const auto& e : run) {
        const void* fn = reinterpret_cast<const void*>(e.second);
        if (g_failedEffects.Has(fn)) continue;
        try {
            e.second(dev);
#ifdef APEX_F10_STUDY
            F10Study::Sample(dev, "after-order-" + std::to_string(e.first));
#endif
        } catch (...) {
            g_failedEffects.Add(fn);
            HookGuard::NoteAt("PostScene effect", fn);
        }
    }
    g_depthValid = true;
}


// 07/10 actual copy diagnostics: the game's colour transform copies a 256x256 tile,
// starting at (0,0), into its 2048x1024 scratch target. With UI the ordered effects
// have already run; with F10 they otherwise wait until after this transform.
// Learn the scratch identity only from an already-applied, UI-visible frame; hidden
// frames may use that same identity and layout, never any arbitrary partial copy.
bool FirstColourTile(const RECT* src, const RECT* dst) {
    return src && dst && src->left == 0 && src->top == 0 && src->right == 256 && src->bottom == 256 &&
           dst->left == 0 && dst->top == 0 && dst->right == 256 && dst->bottom == 256;
}

void BeforeColourTile(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* srcRect,
                      IDirect3DSurface9* dst, const RECT* dstRect, D3DTEXTUREFILTERTYPE filter) {
#ifdef APEX_F10_STUDY
    if (dev && src == g_backBuffer) F10Study::Copy(dev, srcRect, dstRect, filter, dst, g_done, g_uiDrawSeen, dst == g_knownTileTarget);
#endif
    if (!dev || !src || !dst || filter != D3DTEXF_POINT || !FirstColourTile(srcRect, dstRect) ||
        g_rejectedBoundary || g_sceneDraws < kMinSceneDraws || !g_backBuffer || src != g_backBuffer ||
        g_curRT0 != g_backBuffer || (g_done && (!g_uiDrawSeen || g_knownTileTarget)) ||
        DepthShare::InternalPass() || !ShaderCache::PrecompileComplete()) return;
    // A hidden frame must use the scratch target observed in the correct UI path.
    if (!g_done && (g_uiDrawSeen || !g_knownTileTarget || dst != g_knownTileTarget)) return;
    if (!WorldSession::InWorld()) return;
    D3DSURFACE_DESC a{}, b{};
    if (FAILED(src->GetDesc(&a)) || FAILED(dst->GetDesc(&b)) ||
        a.Format != D3DFMT_A8R8G8B8 || b.Format != a.Format ||
        a.MultiSampleType != D3DMULTISAMPLE_NONE || b.MultiSampleType != D3DMULTISAMPLE_NONE ||
        !(b.Usage & D3DUSAGE_RENDERTARGET) || b.Width != 2048 || b.Height != 1024 ||
        a.Width < 256 || a.Height < 256) return;
    if (g_done) {
        g_knownTileTarget = dst;
        LOG_INFO("[PostScene] Learned the game's colour-tile copy from the UI-visible frame");
        return;
    }
    DWORD z = D3DZB_FALSE, write = TRUE, func = D3DCMP_ALWAYS, colour = 0;
    if (FAILED(dev->GetRenderState(D3DRS_ZENABLE, &z)) || z == D3DZB_FALSE ||
        FAILED(dev->GetRenderState(D3DRS_ZWRITEENABLE, &write)) || write ||
        FAILED(dev->GetRenderState(D3DRS_ZFUNC, &func)) || func != D3DCMP_LESSEQUAL ||
        FAILED(dev->GetRenderState(D3DRS_COLORWRITEENABLE, &colour)) || (colour != 7 && colour != 15) ||
        !g_sceneOnSharedDepth || !SceneDepthReady(dev)) return;
    g_done = true; // before nested effect copies; the complete ordered chain runs once
    g_tileBoundary = true;
    RunEffects(dev, true);
    static int notes = 0;
    if (notes < 3) {
        ++notes;
        LOG_INFO("[PostScene] Effects applied before the game's colour-tile copy (hidden UI)");
    }
}

// ---- camera (combined build's post_scene.cpp, tag combined-final) ----
// Every scene draw carries the camera projection in several vertex-constant blocks (LightProbe-m80: c0, c40, c180, c192
// and c216 all give near 0.250 in every draw; c132 / c171 are other projections). A 4-row block is a projection when
// row2 = A * row3 + (0, 0, 0, B) with A ~ 1 and rows 0 and 1 orthogonal to row 3; then near = -B. The value most blocks
// agree on over the frame's first scene draws wins. The view-projection is c40..c43 (translation = the camera's world
// position; a mirrored copy in the water reflection pass): the block most draws agree on, trusted when its near matches.
constexpr int kNearDraws = 24;
constexpr UINT kNearBlocks[] = {0, 4, 40, 180, 192, 216};
std::atomic<int> g_cameraWanted{0};
float g_near = 0.0f;
int g_nearDraws = 0;
std::vector<std::pair<float, int>> g_nearVotes;
struct VpVote {
    float m[4][4];
    int count;
};
std::vector<VpVote> g_vpVotes;
float g_vp[4][4] = {};
bool g_vpValid = false;

bool NearFromBlock(const float m[4][4], float& nearZ) {
    const float *r0 = m[0], *r1 = m[1], *r2 = m[2], *r3 = m[3];
    const float n3 = r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2];
    if (n3 < 1e-8f) return false;
    const float A = (r2[0] * r3[0] + r2[1] * r3[1] + r2[2] * r3[2]) / n3;
    if (A < 0.999f || A > 1.001f) return false;
    float res = 0;
    for (int k = 0; k < 3; k++) res += (r2[k] - A * r3[k]) * (r2[k] - A * r3[k]);
    if (res > 1e-8f * n3) return false;
    const float n0 = std::sqrt(r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2]), n1 = std::sqrt(r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2]);
    const float s3 = std::sqrt(n3);
    if (n0 < 1e-6f || n1 < 1e-6f) return false;
    if (std::fabs(r0[0] * r3[0] + r0[1] * r3[1] + r0[2] * r3[2]) > 1e-3f * n0 * s3 || std::fabs(r1[0] * r3[0] + r1[1] * r3[1] + r1[2] * r3[2]) > 1e-3f * n1 * s3) return false;
    const float B = r2[3] - A * r3[3];
    if (B > -0.01f || B < -5.0f) return false;
    nearZ = -B;
    return true;
}

void VoteCamera(IDirect3DDevice9* dev) {
    {
        float m[4][4], n;
        if (SUCCEEDED(dev->GetVertexShaderConstantF(40, &m[0][0], 4)) && NearFromBlock(m, n)) {
            bool found = false;
            for (auto& v : g_vpVotes)
                if (std::memcmp(v.m, m, sizeof m) == 0) {
                    v.count++;
                    found = true;
                }
            if (!found && g_vpVotes.size() < 8) {
                g_vpVotes.push_back({{}, 1});
                std::memcpy(g_vpVotes.back().m, m, sizeof m);
            }
        }
    }
    for (UINT base : kNearBlocks) {
        float m[4][4], n;
        if (FAILED(dev->GetVertexShaderConstantF(base, &m[0][0], 4)) || !NearFromBlock(m, n)) continue;
        bool found = false;
        for (auto& v : g_nearVotes)
            if (std::fabs(v.first - n) < 1e-3f * n) {
                v.second++;
                found = true;
            }
        if (!found && g_nearVotes.size() < 16) g_nearVotes.push_back({n, 1});
    }
    g_nearDraws++;
    int best = 0; // the current winner (frames with few scene draws still get a value)
    for (const auto& v : g_nearVotes)
        if (v.second > best) {
            best = v.second;
            g_near = v.first;
        }
    int bestVp = 0;
    for (const auto& v : g_vpVotes) {
        float n;
        if (v.count > bestVp && NearFromBlock(v.m, n) && std::fabs(n - g_near) < 1e-3f * g_near) {
            bestVp = v.count;
            std::memcpy(g_vp, v.m, sizeof g_vp);
        }
    }
    g_vpValid = bestVp > 0;
}

void OnFrameBoundary(IDirect3DDevice9* dev) {
#ifdef APEX_F10_STUDY
    F10Study::Advance();
#endif
    ExtraHooks::EnsureInstalled(dev);
    g_tileBoundary = false;
    IDirect3DSurface9* s = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &s)) && s) {
        g_backBuffer = s;
        s->Release();
    }
    // read again every frame (a render target change the hook did not see stays wrong only until the next frame)
    if (SUCCEEDED(dev->GetRenderTarget(0, &s)) && s) {
        g_curRT0 = s;
        s->Release();
    }
    g_lastDepthWrites = g_depthWrites;
    g_sceneDraws = 0;
    g_depthWrites = 0;
    g_done = false;
    g_uiDrawSeen = false;
    g_sceneOnSharedDepth = false;
    g_rejectedBoundary = false;
    g_nearDraws = 0;
    g_nearVotes.clear();
    g_vpVotes.clear();
    g_vpValid = false;
}

void OnGameDraw(D3D9Hooks::DeviceContext& ctx) {
    IDirect3DDevice9* dev = ctx.device;
    if (g_done || DepthShare::InternalPass() || !ShaderCache::PrecompileComplete()) return;
    if (!g_curRT0 || g_curRT0 != g_backBuffer) return;
    const DWORD z = ctx.ZEnable();
    // A depth test that passes everything with no depth write uses no depth: neither scene nor the end of it (06/10: the
    // game's ~137 glass passes of the UI panels, a back-buffer copy drawn back with ZFUNC ALWAYS, counted as scene draws
    // after the UI had started; at angles with few scene draws they made up the count, and the effects ran over the UI)
    if (z != D3DZB_FALSE && !ctx.ZWriteEnable()) {
        DWORD func = D3DCMP_LESSEQUAL;
        if (SUCCEEDED(dev->GetRenderState(D3DRS_ZFUNC, &func)) && func == D3DCMP_ALWAYS) return;
    }
    if (z != D3DZB_FALSE) {
        if (g_rejectedBoundary && ctx.ZWriteEnable() && SceneDepthReady(dev)) g_rejectedBoundary = false; // real scene resumed
        g_sceneDraws++;
        if (!g_sceneOnSharedDepth && SceneDepthReady(dev)) g_sceneOnSharedDepth = true;
        if (ctx.ZWriteEnable()) g_depthWrites++;
        if (g_nearDraws < kNearDraws && g_cameraWanted.load(std::memory_order_relaxed) > 0) VoteCamera(dev);
        return;
    }
    if (g_sceneDraws > 0) g_uiDrawSeen = true; // a depth-off draw after scene draws: the UI may have started (EndScene fallback off)
    if (g_sceneDraws < kMinSceneDraws) return;
    if (g_rejectedBoundary) return;
    if (!SceneDepthReady(dev)) {
        g_rejectedBoundary = true;
        // the others stay pending for a later boundary, not for the UI after this one. A scene drawn on the shared depth and a
        // boundary without it is a pass inside the scene (the lake pass, the game's own): nothing runs there, as before
        if (!g_sceneOnSharedDepth) RunEffects(dev, false);
        return;
    }
    g_done = true; // set first so a failure never retries within the frame
    RunEffects(dev, true);
}

// With the game's UI hidden there may be no depth-off UI draw to mark the end of the scene. In that case run the same
// ordered effects at the game's EndScene, before Apex's overlay and Picture's scene copy. Keep the draw-triggered path
// above for frames that do have a depth-off boundary (including its existing interior behavior).
void AtEndSceneBeforeOverlay(IDirect3DDevice9* dev) {
#ifdef APEX_F10_STUDY
    F10Study::Note("game EndScene | " + PostScene::DiagText());
    F10Study::Sample(dev, "game-end-scene");
#endif
    if (g_uiDrawSeen) return; // the UI is already drawn: never run the effects over it (06/10)
    if (!ShaderCache::PrecompileComplete()) return;
    if (!dev || g_done || g_rejectedBoundary || g_sceneDraws < kMinSceneDraws || !g_backBuffer || g_curRT0 != g_backBuffer) return;
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
    const bool onBackBuffer = rt == g_backBuffer;
    rt->Release();
    if (!onBackBuffer) return;

    g_done = true; // no depth-off scene/UI draw occurred this frame; one fallback pass is enough
    const bool depthOk = SceneDepthReady(dev);
    if (depthOk || !g_sceneOnSharedDepth) RunEffects(dev, depthOk);
}

// A Reset replaces the back buffer and sets render target 0 to it without a SetRenderTarget call: both are read again at
// the next frame boundary
void OnPreReset(IDirect3DDevice9*) {
    g_knownTileTarget = nullptr;
    g_tileBoundary = false;
    g_curRT0 = nullptr;
    g_backBuffer = nullptr;
    g_done = true;
    g_rejectedBoundary = false;
}

void RegisterHooks() {
    using namespace D3D9Hooks;
    ExtraHooks::SetBeforeStretchRect(BeforeColourTile);
    RenderCallbacks::Add(RenderCallbacks::endSceneBeforeOverlay, AtEndSceneBeforeOverlay);
    RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
    RegisterPresent(kHookName, [](DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        OnFrameBoundary(ctx.device);
        return HookAction::Continue;
    }, Priority::First);
    RegisterSetRenderTarget(kHookName, [](DeviceContext&, DWORD index, IDirect3DSurface9* rt) {
        if (index == 0) g_curRT0 = rt;
        return HookAction::Continue;
    }, Priority::First);
    RegisterDrawIndexedPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) {
        OnGameDraw(ctx);
        return HookAction::Continue;
    }, Priority::First);
    RegisterDrawPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE, UINT, UINT) {
        OnGameDraw(ctx);
        return HookAction::Continue;
    }, Priority::First);
}

} // namespace

namespace PostScene {

void Add(int order, Effect fn, bool needsDepth) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!needsDepth && std::find(g_noDepth.begin(), g_noDepth.end(), fn) == g_noDepth.end()) g_noDepth.push_back(fn);
    for (const auto& e : g_effects)
        if (e.second == fn) return;
    g_effects.push_back({order, fn});
    std::stable_sort(g_effects.begin(), g_effects.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (!g_hooks) {
        g_hooks = true;
        g_curRT0 = nullptr;
        g_backBuffer = nullptr;
        g_sceneDraws = 0;
        g_done = true; // start at the next frame boundary
        RegisterHooks();
    }
}

void Remove(Effect fn) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_effects.erase(std::remove_if(g_effects.begin(), g_effects.end(), [&](const auto& e) { return e.second == fn; }), g_effects.end());
    g_noDepth.erase(std::remove(g_noDepth.begin(), g_noDepth.end(), fn), g_noDepth.end());
    if (g_effects.empty() && g_hooks) {
        g_hooks = false;
        D3D9Hooks::UnregisterAll(kHookName);
        RenderCallbacks::Remove(RenderCallbacks::endSceneBeforeOverlay, AtEndSceneBeforeOverlay);
        RenderCallbacks::Remove(RenderCallbacks::preReset, OnPreReset);
        ExtraHooks::SetBeforeStretchRect(nullptr);
        g_knownTileTarget = nullptr;
    }
}

void WantCamera(bool on) {
    if (on) g_cameraWanted.fetch_add(1);
    else if (g_cameraWanted.fetch_sub(1) <= 0) g_cameraWanted.store(0);
}

float CameraNear() { return g_near; }

int DepthWritesThisFrame() { return g_depthWrites; }
int DepthWritesLastFrame() { return g_lastDepthWrites; }
bool Counting() { return g_hooks; }
bool SceneDepthValid() { return g_depthValid; }
std::string DiagText() {
    return "scene draws " + std::to_string(g_sceneDraws) + " (depth writes " + std::to_string(g_depthWrites) + ", last frame " + std::to_string(g_lastDepthWrites) +
           "), boundary " + (g_done ? "done" : "not yet") + (g_rejectedBoundary ? ", rejected" : "") + (g_sceneOnSharedDepth ? ", scene on the shared depth" : ", scene not on the shared depth") + (DepthShare::Surface() ? "" : " (no depth swap)") + (g_uiDrawSeen ? ", UI seen" : "") +
           (g_tileBoundary ? ", before colour tile" : "") +
           ", RT0 " + (!g_curRT0 ? "unknown" : g_curRT0 == g_backBuffer ? "back buffer" : "other") + ", effects " + std::to_string(g_effects.size()) +
           (g_hooks ? "" : ", hooks off");
}
// This frame's effects (ambient occlusion, edge smoothing, Depth Blur) will still run at a later scene boundary: Picture
// does not copy the scene before them (06/10: since Picture took a depth test with ALWAYS and no depth write for the end of
// the scene, it copied before these effects, which still count such draws as scene; every pixel they changed then differed
// from the copy, its UI mask kept them as the game drew them, and the Color page did nothing visible). Render thread.
bool EffectsPending() {
    if (!g_hooks || g_done || g_rejectedBoundary || g_sceneDraws < kMinSceneDraws || !g_curRT0 || g_curRT0 != g_backBuffer ||
        !ShaderCache::PrecompileComplete())
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_effects.empty();
}

// (row2 . row3) / |row3|^2 of the camera block: 1.00008 in LightProbe-m80, a far plane near 3 km
float CameraDepthA() {
    if (!g_vpValid) return kDefaultDepthA;
    const double r3 = double(g_vp[3][0]) * g_vp[3][0] + double(g_vp[3][1]) * g_vp[3][1] + double(g_vp[3][2]) * g_vp[3][2];
    if (r3 < 1e-12) return kDefaultDepthA;
    return float((double(g_vp[2][0]) * g_vp[3][0] + double(g_vp[2][1]) * g_vp[3][1] + double(g_vp[2][2]) * g_vp[3][2]) / r3);
}

bool CameraViewProj(float vp[4][4]) {
    if (!g_vpValid) return false;
    std::memcpy(vp, g_vp, sizeof g_vp);
    return true;
}

} // namespace PostScene
