// Shared trigger for the post-scene effects (see post_scene.h).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "post_scene.h"
#include "d3d9_hooks.h"
#include "depth_share.h"
#include "render_callbacks.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace {

constexpr const char* kHookName = "PostScene";
constexpr int kMinSceneDraws = 20; // backbuffer draws with depth test before the UI can start
constexpr float kDefaultDepthA = 1.00008f; // LightProbe-m80

std::mutex g_mutex;
std::vector<std::pair<int, PostScene::Effect>> g_effects; // sorted by order
bool g_hooks = false;
IDirect3DSurface9* g_curRT0 = nullptr;     // identity only
IDirect3DSurface9* g_backBuffer = nullptr; // identity only
int g_sceneDraws = 0;
bool g_done = false;

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
    IDirect3DSurface9* s = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &s)) && s) {
        g_backBuffer = s;
        s->Release();
    }
    if (!g_curRT0 && SUCCEEDED(dev->GetRenderTarget(0, &s)) && s) {
        g_curRT0 = s;
        s->Release();
    }
    g_sceneDraws = 0;
    g_done = false;
    g_nearDraws = 0;
    g_nearVotes.clear();
    g_vpVotes.clear();
    g_vpValid = false;
}

void OnGameDraw(IDirect3DDevice9* dev) {
    if (g_done || DepthShare::InternalPass()) return;
    if (!g_curRT0 || g_curRT0 != g_backBuffer) return;
    DWORD z = D3DZB_TRUE;
    dev->GetRenderState(D3DRS_ZENABLE, &z);
    if (z != D3DZB_FALSE) {
        g_sceneDraws++;
        if (g_nearDraws < kNearDraws && g_cameraWanted.load(std::memory_order_relaxed) > 0) VoteCamera(dev);
        return;
    }
    if (g_sceneDraws < kMinSceneDraws) return;
    g_done = true; // set first so a failure never retries within the frame
    std::vector<std::pair<int, PostScene::Effect>> run;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        run = g_effects;
    }
    for (const auto& e : run) e.second(dev);
}

// With the game's UI hidden there may be no depth-off UI draw to mark the end of the scene. In that case run the same
// ordered effects at the game's EndScene, before Apex's overlay and Picture's scene copy. Keep the draw-triggered path
// above for frames that do have a depth-off boundary (including its existing interior behavior).
void AtEndSceneBeforeOverlay(IDirect3DDevice9* dev) {
    if (!dev || g_done || g_sceneDraws < kMinSceneDraws || !g_backBuffer || g_curRT0 != g_backBuffer) return;
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
    const bool onBackBuffer = rt == g_backBuffer;
    rt->Release();
    if (!onBackBuffer) return;

    g_done = true; // no depth-off scene/UI draw occurred this frame; one fallback pass is enough
    std::vector<std::pair<int, PostScene::Effect>> run;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        run = g_effects;
    }
    for (const auto& e : run) e.second(dev);
}

// A Reset replaces the back buffer and sets render target 0 to it without a SetRenderTarget call: both are read again at
// the next frame boundary
void OnPreReset(IDirect3DDevice9*) {
    g_curRT0 = nullptr;
    g_backBuffer = nullptr;
    g_done = true;
}

void RegisterHooks() {
    using namespace D3D9Hooks;
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
        OnGameDraw(ctx.device);
        return HookAction::Continue;
    }, Priority::First);
    RegisterDrawPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE, UINT, UINT) {
        OnGameDraw(ctx.device);
        return HookAction::Continue;
    }, Priority::First);
}

} // namespace

namespace PostScene {

void Add(int order, Effect fn) {
    std::lock_guard<std::mutex> lock(g_mutex);
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
    if (g_effects.empty() && g_hooks) {
        g_hooks = false;
        D3D9Hooks::UnregisterAll(kHookName);
        RenderCallbacks::Remove(RenderCallbacks::endSceneBeforeOverlay, AtEndSceneBeforeOverlay);
        RenderCallbacks::Remove(RenderCallbacks::preReset, OnPreReset);
    }
}

void WantCamera(bool on) {
    if (on) g_cameraWanted.fetch_add(1);
    else if (g_cameraWanted.fetch_sub(1) <= 0) g_cameraWanted.store(0);
}

float CameraNear() { return g_near; }

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
