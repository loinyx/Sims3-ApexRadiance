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
#include "lot_lighting_motion.h"
#include "ground_profile_math.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
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
// The game's colour pass: after the scene it copies the back buffer in 256x256 tiles and draws each back with
// x (1 + c0) / (x + c0) (c0 0.4 in every capture). Looks' World only runs before it and compensates (ColourCurveLastFrame)
bool g_tilesStarted = false;  // this frame: the first tile was copied (set after that copy's boundary effects ran)
float g_tileCurve = 0.0f;     // this frame's c0, read at the second tile copy (still bound from tile 0's redraw)
float g_tileCurveLast = 0.0f; // the last complete frame's; 0 when it had no colour pass
PostScene::Effect g_beforeColourPass = nullptr; // SetBeforeColourPass
bool g_fromDraw = false;      // the effects run from a draw's hook (BoundaryOnDraw)

bool SceneDepthReady(IDirect3DDevice9* dev) {
    IDirect3DSurface9* expected = DepthShare::Surface();
    if (!expected) return true; // colour-only effects need no depth swap
    IDirect3DSurface9* bound = nullptr;
    ExtraHooks::RawGetDepthStencilSurface(dev, &bound);
    const bool ready = bound && bound == expected;
    if (bound) bound->Release();
    return ready;
}

// The effects of this boundary: all of them, or with the scene drawn on another depth-stencil than the shared one (one
// Edit in Game setup: every boundary was rejected for it, so Color never ran until it was turned off and on, which dropped
// its depth request and with it the swap) only the ones that need no depth. The others stay pending as before.
// 07/10, players' Runtime Error: an effect that throws is caught (the game's draw or EndScene goes on), noted, and skipped
// from then on; the others keep running
HookGuard::OffList<16> g_failedEffects;
std::map<int, long> g_ran; // TakeRan
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
    std::array<std::pair<int, PostScene::Effect>,16> run{};
    size_t count=0;
    std::vector<std::pair<int, PostScene::Effect>> overflow;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& e : g_effects)
            if (depthOk || std::find(g_noDepth.begin(), g_noDepth.end(), e.second) != g_noDepth.end()) {
                if (count<run.size()) run[count++]=e;
                else overflow.push_back(e);
            }
    } catch (...) {
        HookGuard::Note("PostScene effect list");
        return;
    }
    g_depthValid = depthOk;
    const auto invoke = [&](const auto& e) {
        const void* fn = reinterpret_cast<const void*>(e.second);
        if (g_failedEffects.Has(fn)) return;
        try {
            e.second(dev);
            g_ran[e.first]++;
#ifdef APEX_F10_STUDY
            F10Study::Sample(dev, "after-order-" + std::to_string(e.first));
#endif
        } catch (...) {
            g_failedEffects.Add(fn);
            HookGuard::NoteAt("PostScene effect", fn);
        }
    };
    for (size_t i=0;i<count;++i) invoke(run[i]);
    for (const auto& e:overflow) invoke(e);
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

void BeforeColourTileImpl(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* srcRect,
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

void BeforeColourTile(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* srcRect,
                      IDirect3DSurface9* dst, const RECT* dstRect, D3DTEXTUREFILTERTYPE filter) {
    BeforeColourTileImpl(dev, src, srcRect, dst, dstRect, filter);
    // noted after the effects of this copy ran: they are still before the colour pass
    if (!dev || !src || src != g_backBuffer || filter != D3DTEXF_POINT || !srcRect || srcRect->top != 0 ||
        srcRect->bottom != 256 || DepthShare::InternalPass()) return;
    if (srcRect->left == 0 && srcRect->right == 256 && !g_tilesStarted) {
        if (g_beforeColourPass) g_beforeColourPass(dev);
        g_tilesStarted = true;
    }
    else if (g_tilesStarted && srcRect->left == 256 && srcRect->right == 512 && g_tileCurve == 0.0f) {
        float c[4] = {};
        const bool ok = SUCCEEDED(dev->GetPixelShaderConstantF(0, c, 1)) && c[0] > 0.01f && c[0] < 10.0f;
        g_tileCurve = ok ? c[0] : 0.4f; // a getter that fails (a remote device) keeps the captured value
    }
}

// ---- camera (combined build's post_scene.cpp, tag combined-final) ----
// Every scene draw carries the camera projection in several vertex-constant blocks (LightProbe-m80: c0, c40, c180, c192
// and c216 all give near 0.250 in every draw; c132 / c171 are other projections). A 4-row block is a projection when
// row2 = A * row3 + (0, 0, 0, B) with A ~ 1 and rows 0 and 1 orthogonal to row 3; then near = -B. The value most blocks
// agree on over the frame's first scene draws wins. The view-projection is c40..c43 (translation = the camera's world
// position; a mirrored copy in the water reflection pass): the block most draws agree on, trusted when its near matches.
// c40 holds the camera only while a billboard pass wrote it last: in the precomp's parameter tables only billboard,
// billboard_alpha and billboardPick put WorldToClipSpaceMatrix there; skinned and instanced draws overwrite c40 with
// bones and orientations, and the other passes carry it at c0, c4, c8, c12, c80-c96, c180-c219. With no such c40 (the
// bridge runs: no camera in any frame) the vote also scans c0..c223 of each voted draw for a projection whose own camera
// position (the point rows 0, 1 and 3 send to 0) lies over the game's camera eye; an object's or a lot's
// object-to-clip block puts it elsewhere, the water reflection's mirrored copy puts it under the eye.
constexpr int kNearDraws = 24;
constexpr int kEyeSeekDraws = 128; // while only the scan can find the camera, it keeps voting up to this many scene draws
constexpr UINT kNearBlocks[] = {0, 4, 40, 180, 192, 216};
constexpr UINT kScanRegs = 224;    // the highest WorldToClipSpaceMatrix the game writes ends at c222
constexpr float kEyeMatchM = 4.0f; // horizontal; the eye read can trail the frame's camera by up to ~2.5 m while it moves
std::atomic<int> g_cameraWanted{0};
float g_near = 0.0f;
int g_nearDraws = 0;
std::vector<std::pair<float, int>> g_nearVotes;
struct VpVote {
    float m[4][4];
    int count;
    bool at40 = false; // seen in c40 (trusted when its near matches the frame's near)
    bool eye = false;  // its camera position lies over the game's eye
};
bool g_eyeKnown = false;
float g_eye[3] = {};
float g_eyeMiss = -1.0f; // this frame: the smallest horizontal distance of a scanned projection's camera to the eye (diagnostics)
// Diagnostics of that nearest scanned block (rerun2 1118: nearest 72.8 m, no camera): its register, camera position, near
// and sign of m[1][1]; and the c40 block's shape
UINT g_missReg = 0;
float g_missPos[3] = {}, g_missNear = 0.0f;
bool g_missMirrored = false;
int g_c40State = -1; // -1 not read, 0 not projection-shaped, 1 shaped
float g_c40Pos[3] = {};
bool g_c40PosOk = false;
std::vector<VpVote> g_vpVotes;
// The game camera object's own view-projection (bridge runs: no voted draw carries one; the camera object holds the
// camera-to-world rows right, up, back, eye at eye -0x30 and the projection at eye +0x10, near 0.25 like the near votes).
// Used when the vote finds none. Also counted: scanned blocks equal to it in world and in camera-relative form (eye at 0).
bool g_gameVpOk = false, g_vpFromGame = false;
float g_gameVp[4][4] = {}, g_gameVpRel[4][4] = {}, g_gameNear = 0.0f;
int g_gameEqWorld = 0, g_gameEqRel = 0;
UINT g_gameEqReg = 0;
bool SameBlock(const float a[4][4], const float b[4][4]) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            if (std::fabs(a[i][j] - b[i][j]) > 1e-3f * (1.0f + std::fabs(b[i][j]))) return false;
    return true;
}
float g_vp[4][4] = {};
bool g_vpValid = false;
float g_lastVp[4][4] = {}; // the last frame's (read at its Present, after the boundary cleared g_vp)
bool g_lastVpValid = false;
// The camera's own (not mirrored) view-projection for recording tools: with water in view the first scene draws are the
// reflection pass, so g_vp is its mirrored copy (vp[1][1] < 0). Effects keep reading g_vp.
std::atomic<int> g_plainWanted{0};
float g_plainVp[4][4] = {};
bool g_plainVpValid = false;
float g_lastPlainVp[4][4] = {};
bool g_lastPlainVpValid = false;
int g_plainSeek = 0;

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

// The camera position of a projection block: the point rows 0, 1 and 3 send to 0 (false when they are degenerate)
bool CameraOfBlock(const float m[4][4], float p[3]) {
    const double a[3][3] = {{m[0][0], m[0][1], m[0][2]}, {m[1][0], m[1][1], m[1][2]}, {m[3][0], m[3][1], m[3][2]}};
    const double b[3] = {-double(m[0][3]), -double(m[1][3]), -double(m[3][3])};
    const auto det3 = [](const double c0[3], const double c1[3], const double c2[3]) {
        return c0[0] * (c1[1] * c2[2] - c1[2] * c2[1]) - c1[0] * (c0[1] * c2[2] - c0[2] * c2[1]) + c2[0] * (c0[1] * c1[2] - c0[2] * c1[1]);
    };
    const double c0[3] = {a[0][0], a[1][0], a[2][0]}, c1[3] = {a[0][1], a[1][1], a[2][1]}, c2[3] = {a[0][2], a[1][2], a[2][2]};
    const double d = det3(c0, c1, c2);
    if (!std::isfinite(d) || std::fabs(d) < 1e-12) return false;
    p[0] = float(det3(b, c1, c2) / d), p[1] = float(det3(c0, b, c2) / d), p[2] = float(det3(c0, c1, b) / d);
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

// One vote per distinct block per draw (the same block in several registers counts once)
void AddVpVote(const float m[4][4], bool at40, bool eye, std::vector<size_t>& thisDraw) {
    for (size_t i = 0; i < g_vpVotes.size(); i++) {
        VpVote& v = g_vpVotes[i];
        if (std::memcmp(v.m, m, sizeof v.m) != 0) continue;
        v.at40 = v.at40 || at40, v.eye = v.eye || eye;
        if (std::find(thisDraw.begin(), thisDraw.end(), i) == thisDraw.end()) v.count++, thisDraw.push_back(i);
        return;
    }
    if (g_vpVotes.size() >= 16) return;
    VpVote v{{}, 1, at40, eye};
    std::memcpy(v.m, m, sizeof v.m);
    thisDraw.push_back(g_vpVotes.size());
    g_vpVotes.push_back(v);
}

void VoteCamera(IDirect3DDevice9* dev) {
    if (g_nearDraws == 0) {
        g_eyeKnown = LotLightingMotion::CameraEye(g_eye), g_eyeMiss = -1.0f, g_c40State = -1, g_c40PosOk = false;
        float c2w[4][4], proj[4][4];
        g_gameVpOk = LotLightingMotion::CameraMatrices(c2w, proj) && GroundProfileMath::ViewProjFromCamera(c2w, proj, g_gameVp) &&
                     NearFromBlock(g_gameVp, g_gameNear);
        if (g_gameVpOk) c2w[3][0] = c2w[3][1] = c2w[3][2] = 0.0f, GroundProfileMath::ViewProjFromCamera(c2w, proj, g_gameVpRel);
        g_gameEqWorld = g_gameEqRel = 0;
    }
    float regs[kScanRegs][4];
    const bool haveRegs = SUCCEEDED(dev->GetVertexShaderConstantF(0, &regs[0][0], kScanRegs));
    float cameraBlock[4][4];
    const bool haveCameraBlock = haveRegs ? (std::memcpy(cameraBlock, regs[40], sizeof cameraBlock), true)
                                          : SUCCEEDED(dev->GetVertexShaderConstantF(40, &cameraBlock[0][0], 4));
    {
        std::vector<size_t> thisDraw;
        float n;
        if (haveCameraBlock && NearFromBlock(cameraBlock, n)) {
            AddVpVote(cameraBlock, true, false, thisDraw);
            if (g_c40State < 1) g_c40State = 1, g_c40PosOk = CameraOfBlock(cameraBlock, g_c40Pos);
        } else if (haveCameraBlock && g_c40State < 0)
            g_c40State = 0;
        if (haveRegs && g_eyeKnown)
            for (UINT r = 0; r + 4 <= kScanRegs; r++) {
                const float(*m)[4] = &regs[r];
                float p[3];
                if (!NearFromBlock(m, n)) continue;
                if (g_gameVpOk && (SameBlock(m, g_gameVp) || SameBlock(m, g_gameVpRel))) {
                    if (g_gameEqWorld + g_gameEqRel == 0) g_gameEqReg = r;
                    (SameBlock(m, g_gameVp) ? g_gameEqWorld : g_gameEqRel)++;
                }
                if (!CameraOfBlock(m, p)) continue;
                const float miss = std::hypot(p[0] - g_eye[0], p[2] - g_eye[2]);
                if (g_eyeMiss < 0 || miss < g_eyeMiss) {
                    g_eyeMiss = miss, g_missReg = r, g_missNear = n, g_missMirrored = m[1][1] < 0.0f;
                    std::memcpy(g_missPos, p, sizeof p);
                }
                if (miss < kEyeMatchM) AddVpVote(m, false, true, thisDraw);
            }
    }
    for (UINT base : kNearBlocks) {
        float m[4][4], n;
        // No device state is changed between the two votes: reuse the register read; read again only when it failed
        if (haveRegs) std::memcpy(m, regs[base], sizeof m);
        else if (base == 40 && haveCameraBlock) std::memcpy(m, cameraBlock, sizeof m);
        else if (FAILED(dev->GetVertexShaderConstantF(base, &m[0][0], 4))) continue;
        if (!NearFromBlock(m, n)) continue;
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
    // trusted: over the eye, or c40 with the frame's near
    const auto trusted = [](const VpVote& v, float& n) { return NearFromBlock(v.m, n) && (v.eye || (v.at40 && std::fabs(n - g_near) < 1e-3f * g_near)); };
    int bestVp = 0;
    float vpNear = 0.0f;
    for (const auto& v : g_vpVotes) {
        float n;
        if (v.count > bestVp && trusted(v, n)) {
            bestVp = v.count, vpNear = n;
            std::memcpy(g_vp, v.m, sizeof g_vp);
        }
    }
    g_vpValid = bestVp > 0;
    if (g_vpValid) g_near = vpNear; // (the same unless the block votes chose another projection's near)
    // no voted block: the game camera's own, when its near is the frame's (a misread camera object fails the shape or the near).
    // 1 %: the near of a float block with eye coordinates near 1600 m carries ~2e-4 rounding (bridge runs: 0.24988..0.25012
    // frame to frame against votes of 0.250023), which a 0.1 % gate turned into frames without a camera
    // shortcut: the camera object can trail the drawn frame by up to ~2.5 m while it moves (kEyeMatchM); a voted block wins whenever one exists
    g_vpFromGame = !g_vpValid && g_gameVpOk && (g_nearVotes.empty() || std::fabs(g_gameNear - g_near) < 1e-2f * g_near);
    if (g_vpFromGame) std::memcpy(g_vp, g_gameVp, sizeof g_vp), g_vpValid = true, g_near = g_gameNear;
    if (g_plainWanted.load(std::memory_order_relaxed) > 0) {
        int bestPlain = 0;
        for (const auto& v : g_vpVotes) {
            float n;
            if (v.count > bestPlain && v.m[1][1] >= 0 && trusted(v, n)) {
                bestPlain = v.count;
                std::memcpy(g_plainVp, v.m, sizeof g_plainVp);
            }
        }
        if (bestPlain > 0) g_plainVpValid = true;
    }
}

// After the vote: the first later scene draw whose c40 is a plain camera with this frame's near (recording tools only)
void SeekPlainCamera(IDirect3DDevice9* dev) {
    float m[4][4], n;
    if (FAILED(dev->GetVertexShaderConstantF(40, &m[0][0], 4)) || !NearFromBlock(m, n) || m[1][1] < 0 ||
        std::fabs(n - g_near) >= 1e-3f * g_near)
        return;
    std::memcpy(g_plainVp, m, sizeof g_plainVp);
    g_plainVpValid = true;
}

void OnFrameBoundary(IDirect3DDevice9* dev) {
#ifdef APEX_F10_STUDY
    F10Study::Advance();
#endif
    ExtraHooks::EnsureInstalled(dev);
    g_tileBoundary = false;
    g_tileCurveLast = g_tilesStarted ? (g_tileCurve > 0.0f ? g_tileCurve : 0.4f) : 0.0f;
    g_tilesStarted = false;
    g_tileCurve = 0.0f;
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
    // regression check of the game-camera view-projection (the first frames that use it, then every ~10 min at 60 fps)
    static int gameNotes = 0;
    static uint32_t gameFrames = 0;
    if (g_vpFromGame && (gameNotes < 3 || ++gameFrames % 36000 == 0))
        ++gameNotes, LOG_INFO(std::format("[PostScene] Camera from the game camera's matrices (eye -0x30 camera-to-world, eye +0x10 projection): near {:.6f}, "
                                          "voted draws {}, scanned blocks equal to it: world {}, camera-relative {} (first c{})",
                                          g_gameNear, g_nearDraws, g_gameEqWorld, g_gameEqRel, g_gameEqReg));
    g_vpFromGame = false;
    g_nearDraws = 0;
    g_nearVotes.clear();
    g_vpVotes.clear();
    if (g_vpValid) std::memcpy(g_lastVp, g_vp, sizeof g_vp);
    g_lastVpValid = g_vpValid;
    g_vpValid = false;
    if (g_plainVpValid) std::memcpy(g_lastPlainVp, g_plainVp, sizeof g_plainVp);
    g_lastPlainVpValid = g_plainVpValid;
    g_plainVpValid = false;
    g_plainSeek = 0;
}

void OnGameDraw(D3D9Hooks::DeviceContext& ctx) {
    IDirect3DDevice9* dev = ctx.device;
    if (g_done || DepthShare::InternalPass() || !ShaderCache::PrecompileComplete()) return;
    if (!g_curRT0 || g_curRT0 != g_backBuffer) return;
    const DWORD z = ctx.ZEnable();
    // A depth test that passes everything with no depth write uses no depth: neither scene nor the end of it (06/10: the
    // game's ~137 glass passes of the UI panels, a back-buffer copy drawn back with ZFUNC ALWAYS, counted as scene draws
    // after the UI had started; at angles with few scene draws they made up the count, and the effects ran over the UI)
    if (z != D3DZB_FALSE && !ctx.ZWriteEnable() && ctx.ZFunc() == D3DCMP_ALWAYS) return;
    if (z != D3DZB_FALSE) {
        if (g_rejectedBoundary && ctx.ZWriteEnable() && SceneDepthReady(dev)) g_rejectedBoundary = false; // real scene resumed
        g_sceneDraws++;
        if (!g_sceneOnSharedDepth && SceneDepthReady(dev)) g_sceneOnSharedDepth = true;
        if (ctx.ZWriteEnable()) g_depthWrites++;
        if (g_cameraWanted.load(std::memory_order_relaxed) > 0) {
            if (g_nearDraws < kNearDraws || (!g_vpValid && g_eyeKnown && g_nearDraws < kEyeSeekDraws)) VoteCamera(dev);
            else if (!g_plainVpValid && g_plainWanted.load(std::memory_order_relaxed) > 0 && g_plainSeek++ < 512) SeekPlainCamera(dev);
        }
        return;
    }
    if (g_sceneDraws > 0) g_uiDrawSeen = true; // a depth-off draw after scene draws: the UI may have started (EndScene fallback off)
    if (g_sceneDraws < kMinSceneDraws) return;
    if (g_rejectedBoundary) return;
    if (!SceneDepthReady(dev)) {
        g_rejectedBoundary = true;
        // the others stay pending for a later boundary, not for the UI after this one. A scene drawn on the shared depth and a
        // boundary without it is a pass inside the scene (the lake pass, the game's own): nothing runs there, as before
        if (!g_sceneOnSharedDepth) { g_fromDraw = true; RunEffects(dev, false); g_fromDraw = false; }
        return;
    }
    g_done = true; // set first so a failure never retries within the frame
    g_fromDraw = true;
    RunEffects(dev, true);
    g_fromDraw = false;
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
    g_tilesStarted = false;
    g_tileCurve = g_tileCurveLast = 0.0f;
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
    // (its scene draw counts include Night Lighting's re-issued draws, as they always did: Depth Blur's frozen-screen gate and
    // the minimum scene draws read them)
    ObserveReissuedDraws(kHookName);
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

// The draw hooks run while an effect is registered or someone wants the camera (caller holds g_mutex)
void SyncHooks() {
    const bool want = !g_effects.empty() || g_cameraWanted.load() > 0;
    if (want && !g_hooks) {
        g_hooks = true;
        g_curRT0 = nullptr;
        g_backBuffer = nullptr;
        g_sceneDraws = 0;
        g_done = true; // start at the next frame boundary
        RegisterHooks();
    } else if (!want && g_hooks) {
        g_hooks = false;
        D3D9Hooks::UnregisterAll(kHookName);
        RenderCallbacks::Remove(RenderCallbacks::endSceneBeforeOverlay, AtEndSceneBeforeOverlay);
        RenderCallbacks::Remove(RenderCallbacks::preReset, OnPreReset);
        ExtraHooks::SetBeforeStretchRect(nullptr);
        g_knownTileTarget = nullptr;
    }
}

void Add(int order, Effect fn, bool needsDepth) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!needsDepth && std::find(g_noDepth.begin(), g_noDepth.end(), fn) == g_noDepth.end()) g_noDepth.push_back(fn);
    for (const auto& e : g_effects)
        if (e.second == fn) return;
    g_effects.push_back({order, fn});
    std::stable_sort(g_effects.begin(), g_effects.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    SyncHooks();
}

void Remove(Effect fn) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_effects.erase(std::remove_if(g_effects.begin(), g_effects.end(), [&](const auto& e) { return e.second == fn; }), g_effects.end());
    g_noDepth.erase(std::remove(g_noDepth.begin(), g_noDepth.end(), fn), g_noDepth.end());
    SyncHooks();
}

void WantCamera(bool on, bool plain) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (on) g_cameraWanted.fetch_add(1);
    else if (g_cameraWanted.fetch_sub(1) <= 0) g_cameraWanted.store(0);
    if (plain) {
        if (on) g_plainWanted.fetch_add(1);
        else if (g_plainWanted.fetch_sub(1) <= 0) g_plainWanted.store(0);
    }
    SyncHooks();
}

std::map<int, long> TakeRan() { return std::exchange(g_ran, {}); }

float CameraNear() { return g_near; }

int DepthWritesThisFrame() { return g_depthWrites; }
int DepthWritesLastFrame() { return g_lastDepthWrites; }
bool Counting() { return g_hooks; }
bool SceneDepthValid() { return g_depthValid; }
// The nearest scanned block in words: where its camera sits, a guess of its kind, and the game camera's vec3s around the eye
// offset (is +0x60 the eye, or another point such as the look-at target?) with their horizontal distance to that camera
static std::string CameraDiagText() {
    const auto v3 = [](const float p[3]) { return std::format("({:.2f}, {:.2f}, {:.2f})", p[0], p[1], p[2]); };
    std::string t = g_c40State < 0 ? "; c40 not read" : g_c40State == 0 ? "; c40 not projection-shaped" : "; c40 shaped, its camera " + (g_c40PosOk ? v3(g_c40Pos) : std::string("degenerate"));
    if (!g_eyeKnown || g_eyeMiss < 0) return t + "; no projection-shaped block scanned";
    const float dy = g_missPos[1] - g_eye[1];
    // shortcut: kind is a guess from heights only (a view-projection's camera sits above the ground, near the eye's height)
    const char* kind = g_missMirrored ? "mirrored (reflection?)" : std::fabs(dy) < 4.0f ? "view-projection at another xz (eye read wrong?)" : dy < -20.0f ? "object-to-clip? (below the eye)" : "unknown";
    t += std::format("; nearest scanned c{} camera {} near {:.4f}, {:.1f} m above the eye, kind guess {}; game camera vec3s by offset from the eye's:", g_missReg, v3(g_missPos), g_missNear, dy, kind);
    for (int d = -0x40; d <= 0x40; d += 0x10) {
        float q[3];
        if (!LotLightingMotion::CameraVecNearEye(d, q)) continue;
        t += std::format(" {}0x{:x} {} ({:.1f} m)", d < 0 ? "-" : "+", d < 0 ? -d : d, v3(q), std::hypot(g_missPos[0] - q[0], g_missPos[2] - q[2]));
    }
    return t;
}

std::string DiagText() {
    return "scene draws " + std::to_string(g_sceneDraws) + " (depth writes " + std::to_string(g_depthWrites) + ", last frame " + std::to_string(g_lastDepthWrites) +
           "), boundary " + (g_done ? "done" : "not yet") + (g_rejectedBoundary ? ", rejected" : "") + (g_sceneOnSharedDepth ? ", scene on the shared depth" : ", scene not on the shared depth") + (DepthShare::Surface() ? "" : " (no depth swap)") + (g_uiDrawSeen ? ", UI seen" : "") +
           (g_tileBoundary ? ", before colour tile" : "") +
           ", RT0 " + (!g_curRT0 ? "unknown" : g_curRT0 == g_backBuffer ? "back buffer" : "other") + ", effects " + std::to_string(g_effects.size()) +
           (g_hooks ? "" : ", hooks off") +
           // the camera vote: why CameraViewProj has no view-projection (no c40 block with a projection's shape, or one whose near
           // differs from the agreed near)
           ", camera wanted " + std::to_string(g_cameraWanted.load()) + ", voted draws " + std::to_string(g_nearDraws) + ", near votes " +
           std::to_string(g_nearVotes.size()) + " (near " + std::to_string(g_near) + "), view-projection blocks " + std::to_string(g_vpVotes.size()) +
           (g_eyeKnown ? ", eye (" + std::to_string(g_eye[0]) + ", " + std::to_string(g_eye[1]) + ", " + std::to_string(g_eye[2]) + "), nearest scanned camera " + std::to_string(g_eyeMiss) + " m"
                       : std::string(", eye unknown")) + CameraDiagText() +
           (g_gameVpOk ? std::format("; game camera matrices near {:.6f}, equal scanned blocks world {} camera-relative {}", g_gameNear, g_gameEqWorld, g_gameEqRel)
                       : std::string("; game camera matrices unread or not a camera")) +
           (g_vpValid ? (g_vpFromGame ? ", camera valid (game camera's matrices)" : ", camera valid") : ", no camera") + (g_lastVpValid ? ", last frame's valid" : "");
}
// This frame's effects (ambient occlusion, edge smoothing, Depth Blur) will still run at a later scene boundary: Picture
// does not copy the scene before them (06/10: since Picture took a depth test with ALWAYS and no depth write for the end of
// the scene, it copied before these effects, which still count such draws as scene; every pixel they changed then differed
// from the copy, its UI mask kept them as the game drew them, and the Color page did nothing visible). Render thread.
bool ColourTilesStarted() { return g_tilesStarted; }
void SetBeforeColourPass(Effect fn) { g_beforeColourPass = fn; }
bool BoundaryOnDraw() { return g_fromDraw; }
float ColourCurveLastFrame() { return g_tileCurveLast; }

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
bool RecentCameraViewProj(float vp[4][4]) {
    if (g_plainVpValid || g_lastPlainVpValid) {
        std::memcpy(vp, g_plainVpValid ? g_plainVp : g_lastPlainVp, sizeof g_plainVp);
        return true;
    }
    if (CameraViewProj(vp)) return true;
    if (!g_lastVpValid) return false;
    std::memcpy(vp, g_lastVp, sizeof g_lastVp);
    return true;
}

} // namespace PostScene
