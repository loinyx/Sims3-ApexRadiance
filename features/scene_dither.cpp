// Banding Fix (scene dither)
// The game draws its 3D scene straight into an 8-bit back buffer, so a smooth light gradient (a lamp's pool of light on
// a wall, the fall-off of a room light map) is rounded to 256 steps per channel, and in the dark range each step is a
// visible ring. This adds a fixed, invisible grain to every scene pixel shader's colour before the rounding (triangular
// noise of +-1 step at the default Strength, from the R2 pattern of the pixel position, within half a step of black and
// white), so the rings become a fine grain. The alpha (the bloom mask) is never touched.
//
// How:
//  - ps_3_0 (ShaderPatches::AddDither): the pixel position is vPos. ps_2_0 / ps_2_x (AddDither2, 30/09 evening: in game
//    more than half of the scene draws, the walls among them, were ps_2_x): no vPos, so the position comes from a free
//    texture coordinate k that a copy of the paired vertex shader fills with the clip position (AddScreenPosVs), and
//    the pixel copy is ps_2_x (the grain does not fit in the slots of some ps_2_0).
//  - Copies are made at a shader's first draw in the 3D scene (05/10: before, every shader the game created got one at
//    once, shadow, reflection, UI and bloom ones too, and DXVK keeps every shader for the whole session; the game's memory
//    grew ~0.3 GB more in 30 min with Apex). At creation (last in the chain) only the old copies of a reused address go.
//  - At each draw (last in the chain, after every observer): when render target 0 is the back buffer and the depth
//    test is on and used (the 3D scene; the UI draws with it off, or on with ZFUNC ALWAYS and no depth write, and reads
//    the back buffer back many times per frame, where a dither would feed on itself), the copies are bound for the draw
//    with the amount constant, then the game's shaders and the constant's previous value are put back. Additive and
//    multiply blends get half the grain with a decorrelated phase (they add to a target that already has its grain).
//  - The mouse-pick pass draws into its own 16x16 target, shadows and reflections into their own targets: untouched.
// Offline checks (30/09, Shaders_Win32.precomp): ps_3_0 4907 / 4907 and ps_2_0 2996 / 3104 patched, vertex shaders
// vs_2_0 4035 / 4338, every copy accepted by native D3D9; a grey of 20.40 / 100.70 levels comes out as 20.355 / 100.657
// on average with the grain (20 / 101 without), for ps_3_0 and for a ps_2_0 + vs_2_0 pair alike.

#include "patch_base.h"
#include "apex_version.h"
#include "apex_log.h"
#include "d3d9_bootstrap.h"
#include "d3d9_hooks.h"
#include "render_callbacks.h"
#include "shader_patches.h"
#include "scene_dither.h"
#include "scene_target.h"
#include "f10_study.h"
#include "shader_lookup_cache.h"
#include "imgui.h"
#include "ui/violet_theme.h"
#include "ui/widgets.h"
#include "build_flavor.h"
#include "perf_toggles.h"
#ifndef APEX_NO_DEV_TOOLS
#include "shader_ids.h"
#include "gbuffer_ids.h"
#endif
#include <d3d9.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kHookName = "SceneDither";
constexpr auto kAfterEveryone = static_cast<D3D9Hooks::Priority>(1000); // after Last: every observer has seen the draw
constexpr size_t kMaxShaderTokens = 16384;                              // 64 KB

std::atomic<bool> g_on{false};
thread_local bool t_own = false; // our own creations pass through
std::mutex g_lock;
struct Copy {
    IDirect3DPixelShader9* ps = nullptr; // the dithered copy, null = none
    int amountReg = -1;                 // its constant holding the amount (set around the draw)
    int texcoord = -1;                  // ps_2_x copies: the texture coordinate with the clip position (-1: ps_3_0, vPos)
    DWORD version = 0;                  // the game shader's version token
    uint8_t gb = 0;                     // G-buffer outputs in the copy: 0 none, 1 from the table's row, 2 the unknown values,
                                        // 3 the row of the game's shader a replacement (Night Lighting) was made from
};
std::unordered_map<IDirect3DPixelShader9*, Copy> g_copies; // game pixel shader -> its copy
// game vertex shader -> its copies writing the clip position to TEXCOORDk (null = cannot), key = pointer * 8 + k
std::unordered_map<uint64_t, IDirect3DVertexShader9*> g_vsCopies;
ShaderLookupCache<Copy> g_pixelLookup;
ShaderLookupCache<IDirect3DVertexShader9*> g_vertexLookup;
// Render-thread front caches checked without the lock (14/10: two mutexes on every scene draw): direct-mapped, valid
// while their generation equals g_copyGen, which every change of the copies bumps (under g_lock, after the change).
std::atomic<uint32_t> g_copyGen{1};
struct PsFront {
    IDirect3DPixelShader9* ps = nullptr;
    uint32_t gen = 0;
    Copy copy;
};
struct VsFront {
    uint64_t key = 0;
    uint32_t gen = 0;
    IDirect3DVertexShader9* copy = nullptr;
};
constexpr size_t kFront = 64;
PsFront g_psFront[kFront];
VsFront g_vsFront[kFront];
inline size_t FrontSlot(uint64_t k) { return static_cast<size_t>((static_cast<uint32_t>(k ^ (k >> 32)) * 2654435761u) >> 26) & (kFront - 1); }
float g_strength = 1.0f;                  // the grain in 8-bit steps (triangular, peak): 1 = +-1 step
bool g_showCovered = false;               // Developer: a coarse grain where the fix applies (not saved)
constexpr float kShowCoveredSteps = 24.0f;
IDirect3DSurface9* g_backBuffer = nullptr; // identity only (render thread)
// render target 0, followed through SetRenderTarget as PostScene and Picture do (05/10: GetRenderTarget + Release on every
// draw before); read again at the frame boundary after a Reset (which sets it without a SetRenderTarget call)
IDirect3DSurface9* g_rt0 = nullptr;

// statistics (Developer page, dev log)
std::atomic<unsigned> g_made{0}, g_madeVs{0}, g_refused[7] = {};
// blend classes of the dithered draws (Developer page): the grain is halved for additive and multiply blends (BlendOf)
enum Blend { kOpaque, kAlpha, kAdditive, kMultiply, kOtherBlend, kBlends };
struct FrameCount {
    unsigned dithered3 = 0, dithered2 = 0; // 3D scene draws with the grain: ps_3_0, ps_2_x
    unsigned ps2NoPair = 0;                // ps_2_x with a copy whose vertex shader could not carry the position
    unsigned ps2Refused = 0, ps3Refused = 0, other = 0;
    unsigned noDepth = 0;                  // depth on with ZFUNC ALWAYS and no depth write: the UI's passes, left alone
    unsigned noShader = 0;                 // fixed-function scene draws (no pixel shader): no grain
    unsigned blend[kBlends] = {};          // dithered draws by blend class
    unsigned jittered = 0, jitterRefused = 0, jitterNoShader = 0; // temporal AA: scene draws moved / whose vertex shader was refused / fixed-function
};
FrameCount g_frame, g_last;
unsigned long long g_lastLog = 0;
int g_logs = 0;

// ---- G-buffer (developer builds, patches/gbuffer_patch.cpp): the ps_3_0 copies also write oC1 / oC2, and render targets
// 1 and 2 are bound while the scene is drawn into the back buffer. Render thread except g_gbWanted ----
#ifndef APEX_NO_DEV_TOOLS
std::atomic<bool> g_gbWanted{false};
IDirect3DSurface9* g_gbRt[2] = {}; // identities, owned by the G-buffer feature
bool g_gbBound = false;            // they are render targets 1 and 2 now
bool g_gbMask = true;              // COLORWRITEENABLE1 / 2 are 0xF (the device default; 0 otherwise while bound)
bool g_gbCleared = false;          // this frame
SceneBinder::GBufferCounts g_gbCount;
bool GbActive() { return g_gbWanted.load(std::memory_order_relaxed) && g_gbRt[0] && g_gbRt[1]; }
void GbSetMask(IDirect3DDevice9* dev, bool on) {
    if (g_gbMask == on) return;
    g_gbMask = on;
    dev->SetRenderState(D3DRS_COLORWRITEENABLE1, on ? 0xFu : 0u);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE2, on ? 0xFu : 0u);
    g_gbCount.maskToggles++;
}
void GbBind(IDirect3DDevice9* dev) {
    if (g_gbBound) return;
    if (!g_gbCleared) { // id 0 = nothing written this frame
        dev->ColorFill(g_gbRt[0], nullptr, 0);
        dev->ColorFill(g_gbRt[1], nullptr, 0);
        g_gbCleared = true;
    }
    GbSetMask(dev, false); // only a G-buffer copy opens them
    if (FAILED(D3D9Hooks::CallOriginalSetRenderTarget(dev, 1, g_gbRt[0])) || FAILED(D3D9Hooks::CallOriginalSetRenderTarget(dev, 2, g_gbRt[1]))) {
        D3D9Hooks::CallOriginalSetRenderTarget(dev, 1, nullptr);
        GbSetMask(dev, true);
        g_gbCount.bindFailed++;
        g_gbRt[0] = g_gbRt[1] = nullptr; // not tried again for every draw: off until the targets are set again
        return;
    }
    g_gbBound = true;
    g_gbCount.binds++;
}
void GbUnbind(IDirect3DDevice9* dev) {
    if (!g_gbBound) return;
    D3D9Hooks::CallOriginalSetRenderTarget(dev, 1, nullptr);
    D3D9Hooks::CallOriginalSetRenderTarget(dev, 2, nullptr);
    g_gbBound = false;
    GbSetMask(dev, true); // the device default, for whoever uses several targets next
    g_gbCount.unbinds++;
}
// A back-buffer draw that writes no G-buffer (no copy with the outputs, the UI's passes): the targets stay closed
void GbMasked(IDirect3DDevice9* dev) {
    if (!g_gbBound) return;
    GbSetMask(dev, false);
    g_gbCount.masked++;
}
#else
constexpr bool GbActive() { return false; }
inline void GbBind(IDirect3DDevice9*) {}
inline void GbMasked(IDirect3DDevice9*) {}
#endif

// vertex copy key: the game shader, the texture coordinate k carrying the clip position (-1 none) and the temporal jitter
uint64_t VsKey(IDirect3DVertexShader9* vs, int k, bool jitter) {
    return (reinterpret_cast<uint64_t>(vs) << 5) | (jitter ? 16u : 0u) | static_cast<uint64_t>(k < 0 ? 8 : (k & 7));
}

// ---- the shared scene-draw binder (30/09): the Banding Fix's pixel copies and the temporal anti-aliasing's jittered vertex
// copies (Edge Smoothing, SMAA T2x) bound together in one final draw hook, as two features cannot both skip and redraw ----
std::atomic<int> g_hookUsers{0};  // Banding Fix and temporal AA (the hooks are registered while > 0)
bool g_jitterOn = false;          // render thread: this frame's scene draws are moved
std::atomic<bool> g_jitterWanted{false}; // temporal AA is on (vertex copies with the jitter made at creation)
float g_jitter[4] = {};           // clip-space offset (c252.xy)
constexpr int kJitterConst = 252; // no game vertex shader uses a constant above c241 (Shaders_Win32.precomp, 30/09)

// Tokens up to and with the end token (no length is given to Create*Shader), 0 when unreadable
size_t CodeLength(const DWORD* fn) {
    __try {
        if ((fn[0] >> 17) != 0x7FFFu) return 0; // 0xFFFF.... pixel, 0xFFFE.... vertex
        for (size_t i = 1; i < kMaxShaderTokens; i++)
            if (fn[i] == 0x0000FFFFu) return i + 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

std::vector<DWORD> ReadCode(const DWORD* fn) {
    const size_t n = CodeLength(fn);
    return n ? std::vector<DWORD>(fn, fn + n) : std::vector<DWORD>{};
}

template <typename Shader> std::vector<DWORD> ReadCode(Shader* s) {
    UINT size = 0;
    if (FAILED(s->GetFunction(nullptr, &size)) || size < 8 || size % 4) return {};
    std::vector<DWORD> t(size / 4);
    if (FAILED(s->GetFunction(t.data(), &size))) return {};
    return t;
}

// The dithered copy of a game pixel shader (Copy::ps null when it cannot have one). origin: the game's shader that t, a
// replacement bound in a re-issued draw, was made from (its row when t's own hash has none), or null
Copy MakeCopy(IDirect3DDevice9* dev, std::vector<DWORD> t, IDirect3DPixelShader9* origin = nullptr) {
    Copy c;
    if (t.empty()) {
        g_refused[static_cast<int>(ShaderPatches::DitherResult::Unreadable)]++;
        return c;
    }
    c.version = t[0];
    const bool ps2 = t[0] == 0xFFFF0200u || t[0] == 0xFFFF0201u;
#ifndef APEX_NO_DEV_TOOLS
    const bool gbuffer = g_gbWanted.load(std::memory_order_relaxed);
    const GBufferShaderId* row = gbuffer ? FindGBufferShader(ShaderHash(t.data(), t.size() * 4)) : nullptr; // (the game's bytes)
    bool viaOrigin = false;
    if (gbuffer && !row && origin) {
        const std::vector<DWORD> o = ReadCode(origin);
        row = o.empty() ? nullptr : FindGBufferShader(ShaderHash(o.data(), o.size() * 4));
        viaOrigin = row != nullptr;
    }
    std::vector<DWORD> dithered;
#endif
    const ShaderPatches::DitherResult r = ps2 ? ShaderPatches::AddDither2(t, &c.amountReg, &c.texcoord) : ShaderPatches::AddDither(t, &c.amountReg);
    if (r != ShaderPatches::DitherResult::Ok) {
        g_refused[static_cast<int>(r)]++;
        c.texcoord = -1;
        return c;
    }
#ifndef APEX_NO_DEV_TOOLS
    if (gbuffer) { // one copy carries the grain and the G-buffer outputs
        ShaderPatches::GBufferInfo info; // a shader without a row (another family, a mod, a replaced shader): the unknown values
        if (row) {
            const GBufferAlbedoRule* rule = viaOrigin ? nullptr : FindGBufferAlbedoRule(row->hash); // (its index is in the game shader's tokens)
            if (rule) info.albedoAt = rule->at, info.albedoReg = rule->reg; // composited albedo (family rule)
            info.albedoSampler = row->albedo == 255 ? -1 : row->albedo;
            info.probeSampler = row->probe == 255 ? -1 : row->probe;
            info.material = row->material;
            info.motionClass = row->motionClass;
            info.roughness = kGBufferRoughness[row->material];
        }
        dithered = t;
        if (ShaderPatches::AddGBufferPs(t, info) == ShaderPatches::GBufferResult::Ok) c.gb = viaOrigin ? 3 : row ? 1 : 2;
    }
#endif
    t_own = true;
    HRESULT hr = D3D9Hooks::CallOriginalCreatePixelShader(dev, t.data(), &c.ps);
#ifndef APEX_NO_DEV_TOOLS
    if ((FAILED(hr) || !c.ps) && c.gb) { // a device that refuses the outputs (ps_2_x limits) keeps at least the grain
        c.gb = 0, c.ps = nullptr;
        hr = D3D9Hooks::CallOriginalCreatePixelShader(dev, dithered.data(), &c.ps);
    }
#endif
    t_own = false;
    if (FAILED(hr) || !c.ps) {
        c.ps = nullptr;
        c.gb = 0;
        g_refused[static_cast<int>(ShaderPatches::DitherResult::Unreadable)]++;
        return c;
    }
    g_made++;
    return c;
}

// The copy of a game vertex shader that also writes the clip position to TEXCOORDk, or null
IDirect3DVertexShader9* MakeVsCopy(IDirect3DDevice9* dev, std::vector<DWORD> t, int k, bool jitter) {
    if (t.empty()) return nullptr;
    if (jitter && ShaderPatches::AddJitterVs(t, kJitterConst) != ShaderPatches::JitterResult::Ok) return nullptr;
    if (k >= 0 && !ShaderPatches::AddScreenPosVs(t, k)) return nullptr; // after the jitter: TEXCOORDk gets the moved position
    IDirect3DVertexShader9* copy = nullptr;
    t_own = true;
    const HRESULT hr = D3D9Hooks::CallOriginalCreateVertexShader(dev, t.data(), &copy);
    t_own = false;
    if (FAILED(hr)) return nullptr;
    g_madeVs++;
    return copy;
}

void Remember(IDirect3DPixelShader9* game, const Copy& copy) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_pixelLookup.Clear();
    auto [it, fresh] = g_copies.try_emplace(game, copy);
    if (!fresh) { // an address the game reused for a new shader
        if (it->second.ps) it->second.ps->Release();
        it->second = copy;
    }
    g_copyGen.fetch_add(1, std::memory_order_release);
}

void RememberVs(uint64_t key, IDirect3DVertexShader9* copy) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_vertexLookup.Clear();
    auto [it, fresh] = g_vsCopies.try_emplace(key, copy);
    if (!fresh) {
        if (it->second) it->second->Release();
        it->second = copy;
    }
    g_copyGen.fetch_add(1, std::memory_order_release);
}

// A new game pixel shader at an address: its old copy goes
void Forget(IDirect3DPixelShader9* ps) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_pixelLookup.Clear();
    g_copyGen.fetch_add(1, std::memory_order_release); // (also with no copy: a front entry may hold this address)
    const auto it = g_copies.find(ps);
    if (it == g_copies.end()) return;
    if (it->second.ps) it->second.ps->Release();
    g_copies.erase(it);
}

// A new game vertex shader at an address: its old copies (every k) go
void ForgetVs(IDirect3DVertexShader9* vs) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_vertexLookup.Clear();
    g_copyGen.fetch_add(1, std::memory_order_release);
    for (int j = 0; j < 2; j++)
        for (int k = -1; k < 8; k++) {
            const auto it = g_vsCopies.find(VsKey(vs, k, j != 0));
            if (it == g_vsCopies.end()) continue;
            if (it->second) it->second->Release();
            g_vsCopies.erase(it);
        }
}

void ReleaseCopies() {
    std::lock_guard<std::mutex> lock(g_lock);
    g_pixelLookup.Clear(); g_vertexLookup.Clear();
    g_copyGen.fetch_add(1, std::memory_order_release);
    for (auto& [game, copy] : g_copies)
        if (copy.ps) copy.ps->Release();
    g_copies.clear();
    for (auto& [key, copy] : g_vsCopies)
        if (copy) copy->Release();
    g_vsCopies.clear();
}

// Render thread: the copy for the bound pixel shader (made now when the shader is older than the feature). In a re-issued
// draw whose shader is not the one the game set, the game's shader is the replacement's origin (MakeCopy).
Copy CopyOf(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps) {
    const uint32_t gen = g_copyGen.load(std::memory_order_acquire); // (read before the lookup: a change after it makes the entry stale)
    PsFront& front = g_psFront[FrontSlot(reinterpret_cast<std::uintptr_t>(ps))];
    if (front.ps == ps && front.gen == gen && PerfToggles::SceneDitherFrontCache.load(std::memory_order_relaxed)) return front.copy;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        Copy cached;
        const auto key = reinterpret_cast<std::uintptr_t>(ps);
        if (g_pixelLookup.Find(key, cached)) {
            front.ps = ps, front.gen = gen, front.copy = cached;
            return cached;
        }
        const auto it = g_copies.find(ps);
        if (it != g_copies.end()) {
            g_pixelLookup.Store(key, it->second);
            front.ps = ps, front.gen = gen, front.copy = it->second;
            return it->second;
        }
    }
    IDirect3DPixelShader9* const caller = D3D9Hooks::Reissuing() ? D3D9Hooks::CallerPixelShader() : nullptr;
    const Copy copy = MakeCopy(dev, ReadCode(ps), caller != ps ? caller : nullptr);
    Remember(ps, copy);
    return copy;
}

// Render thread: the vertex copy writing TEXCOORDk (k -1: none) and / or moved by the jitter (made now when missing)
IDirect3DVertexShader9* VsCopyOf(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs, int k, bool jitter) {
    const uint64_t key = VsKey(vs, k, jitter);
    const uint32_t gen = g_copyGen.load(std::memory_order_acquire);
    VsFront& front = g_vsFront[FrontSlot(key)];
    if (front.key == key && front.gen == gen && PerfToggles::SceneDitherFrontCache.load(std::memory_order_relaxed)) return front.copy;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        IDirect3DVertexShader9* cached = nullptr;
        if (g_vertexLookup.Find(key, cached)) {
            front.key = key, front.gen = gen, front.copy = cached;
            return cached;
        }
        const auto it = g_vsCopies.find(key);
        if (it != g_vsCopies.end()) {
            g_vertexLookup.Store(key, it->second);
            front.key = key, front.gen = gen, front.copy = it->second;
            return it->second;
        }
    }
    IDirect3DVertexShader9* copy = MakeVsCopy(dev, ReadCode(vs), k, jitter);
    RememberVs(key, copy);
    return copy;
}

// The blend class of a draw, read only for draws that get the grain (no SetRenderState hook follows the blend states)
Blend BlendOf(IDirect3DDevice9* dev) {
    DWORD on = FALSE, src = D3DBLEND_ONE, dst = D3DBLEND_ZERO, op = D3DBLENDOP_ADD;
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &on);
    if (!on) return kOpaque;
    dev->GetRenderState(D3DRS_SRCBLEND, &src);
    dev->GetRenderState(D3DRS_DESTBLEND, &dst);
    dev->GetRenderState(D3DRS_BLENDOP, &op);
    if (op != D3DBLENDOP_ADD) return kOtherBlend;
    if (src == D3DBLEND_SRCALPHA && dst == D3DBLEND_INVSRCALPHA) return kAlpha;
    if (src == D3DBLEND_DESTCOLOR || src == D3DBLEND_ZERO) return kMultiply; // decals: DESTCOLOR / SRCCOLOR, DESTCOLOR / ONE
    if (dst == D3DBLEND_ONE) return kAdditive;
    return kOtherBlend;
}

template <typename DrawFn> D3D9Hooks::HookAction OnDraw(D3D9Hooks::DeviceContext& ctx, DrawFn draw) {
    IDirect3DDevice9* dev = ctx.device;
    // (the High-precision scene grains its 16-bit scene once, at its resolve)
    const bool dither = g_on.load(std::memory_order_relaxed) && !SceneTarget::Substituting(), jitter = g_jitterOn;
    const bool gb = GbActive(); // (developer builds) with the High-precision scene too: the grain amount is then 0
    if ((!dither && !jitter && !gb) || !g_backBuffer) return D3D9Hooks::HookAction::Continue;
    if (!g_rt0 || g_rt0 != g_backBuffer) return D3D9Hooks::HookAction::Continue; // (the G-buffer targets were unbound at the switch)
    if (ctx.ZEnable() == D3DZB_FALSE) { // UI and 2D passes
        GbMasked(dev);
        return D3D9Hooks::HookAction::Continue;
    }
    // Depth on with ZFUNC ALWAYS and no depth write uses no depth (PostScene's rule): the UI's ~137 glass passes and
    // back-buffer copies drawn back (a light capture shows one under a panel: ps_2_0 reading a 2048x1024 copy). The grain
    // fed on itself there; the jitter already skipped them
    if (!ctx.ZWriteEnable() && ctx.ZFunc() == D3DCMP_ALWAYS) {
        g_frame.noDepth++;
        GbMasked(dev);
        return D3D9Hooks::HookAction::Continue;
    }
    if (gb) GbBind(dev);
    // the Banding Fix's pixel copy
    IDirect3DPixelShader9* ps = nullptr;
    Copy copy;
    if (dither || gb) {
        ps = D3D9Hooks::BoundPixelShader(dev); // (followed through Set*Shader; the device keeps it alive)
        if (!ps) g_frame.noShader++;
        if (ps) {
            copy = CopyOf(dev, ps);
            if (!copy.ps) {
                const DWORD major = (copy.version >> 8) & 0xFF;
                (major == 2 ? g_frame.ps2Refused : major == 3 ? g_frame.ps3Refused : g_frame.other)++;
            }
        }
    }
    bool useDither = copy.ps != nullptr && (dither || copy.gb); // the pixel copy is bound (for the grain, the G-buffer or both)
    const int k = useDither ? copy.texcoord : -1; // ps_2_x copies: the vertex shader must carry the position
    // the vertex copy: TEXCOORDk and / or the temporal jitter
    IDirect3DVertexShader9 *vs = nullptr, *vsCopy = nullptr;
    const DWORD zfunc = jitter ? ctx.ZFunc() : static_cast<DWORD>(D3DCMP_LESSEQUAL);
    // Depth enabled + ALWAYS is also used by screen-space composition draws;
    // ZENABLE alone does not identify a projected 3D surface (pool capture #202).
    bool useJitter = jitter && zfunc != D3DCMP_ALWAYS;
    if (useJitter || k >= 0) {
        vs = D3D9Hooks::BoundVertexShader(dev);
        if (!vs && useJitter) {
            g_frame.jitterNoShader++;
            useJitter = false;
        }
        if (vs && (useJitter || k >= 0)) {
            vsCopy = VsCopyOf(dev, vs, k, useJitter);
            if (!vsCopy && useJitter) { // refused with the jitter: at least the grain
                g_frame.jitterRefused++;
                useJitter = false;
                if (k >= 0) vsCopy = VsCopyOf(dev, vs, k, false);
            }
        }
        if (k >= 0 && !vsCopy) {
            g_frame.ps2NoPair++;
            useDither = false;
        }
    }
    if (!useDither && !useJitter) {
#ifdef APEX_F10_STUDY
        F10Study::CompositionShader(dev, ps, ps, vs, false);
#endif
        GbMasked(dev);
        return D3D9Hooks::HookAction::Continue;
    }
    float before[4] = {};
    Blend blend = kOpaque;
    if (useDither) {
        blend = BlendOf(dev);
        // Additive and multiply blends add their grain on top of the grain already in the target: half of it, with a phase
        // whose pattern does not correlate with the full one (R2 shifted by 0.19: correlation 0.006, offline)
        const bool half = blend == kAdditive || blend == kMultiply || blend == kOtherBlend;
        // (2.15.3) the viewport only for ps_2_x copies: the ps_3_0 grain reads the amount's .x and .w alone (AddDither), the
        // ps_2_x one rebuilds the pixel from .y / .z (AddDither2)
        D3DVIEWPORT9 vp{};
        if (k >= 0) dev->GetViewport(&vp);
        dev->GetPixelShaderConstantF(static_cast<UINT>(copy.amountReg), before, 1);
        // Developer "Show covered surfaces": a coarse grain only where the fix applies
        const float steps = !dither ? 0.0f : g_showCovered ? kShowCoveredSteps : SceneDither::Strength() * (half ? 0.5f : 1.0f);
        const float amount[4] = {steps / 255.0f, 0.5f * static_cast<float>(vp.Width), 0.5f * static_cast<float>(vp.Height),
                                 SceneDither::GrainPhase() + (half ? 0.19f : 0.0f)};
        D3D9Hooks::CallOriginalSetPixelShaderConstantF(dev, static_cast<UINT>(copy.amountReg), amount, 1);
        D3D9Hooks::CallOriginalSetPixelShader(dev, copy.ps);
    }
#ifndef APEX_NO_DEV_TOOLS
    if (g_gbBound) { // only an opaque draw of a copy with the outputs writes: a blended surface keeps what is behind it
        if (useDither && copy.gb && blend == kOpaque) {
            GbSetMask(dev, true);
            (copy.gb == 2 ? g_gbCount.unknown : g_gbCount.patched)++;
            g_gbCount.ps2 += k >= 0;
            g_gbCount.replaced += copy.gb == 3;
        } else if (useDither && copy.gb) {
            GbSetMask(dev, false);
            g_gbCount.blended++;
        } else
            GbMasked(dev);
    }
#endif
    if (useJitter) D3D9Hooks::CallOriginalSetVertexShaderConstantF(dev, kJitterConst, g_jitter, 1); // no game shader reads c252
    if (vsCopy) D3D9Hooks::CallOriginalSetVertexShader(dev, vsCopy);
#ifdef APEX_F10_STUDY
    F10Study::CompositionShader(dev, ps, useDither ? copy.ps : ps, vsCopy ? vsCopy : vs, useDither);
#endif
    draw();
    if (vsCopy) D3D9Hooks::CallOriginalSetVertexShader(dev, vs);
    if (useDither) {
        D3D9Hooks::CallOriginalSetPixelShader(dev, ps);
        D3D9Hooks::CallOriginalSetPixelShaderConstantF(dev, static_cast<UINT>(copy.amountReg), before, 1); // the game may cache it
        if (dither) {
            (k >= 0 ? g_frame.dithered2 : g_frame.dithered3)++;
            g_frame.blend[blend]++;
        }
    }
    if (useJitter) g_frame.jittered++;
    return D3D9Hooks::HookAction::Skip;
}

void OnFrameBoundary(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        g_backBuffer = bb;
        bb->Release();
    }
    if (!g_rt0 && SUCCEEDED(dev->GetRenderTarget(0, &bb)) && bb) {
        g_rt0 = bb;
        bb->Release();
    }
#ifndef APEX_NO_DEV_TOOLS
    GbUnbind(dev);
    g_gbCleared = false;
    g_gbCount.frames++;
#endif
    g_last = g_frame;
    g_frame = {};
    // Development build: the coverage in the log now and then (the first 12 times, every 20 s of scene)
    if (!kPublicBuild) {
        const unsigned long long now = GetTickCount64();
        const unsigned total = g_last.dithered3 + g_last.dithered2 + g_last.ps2NoPair + g_last.ps2Refused + g_last.ps3Refused + g_last.other;
        if (total > 50 && g_logs < 12 && now - g_lastLog >= 20000) {
            g_lastLog = now;
            g_logs++;
            LOG_INFO(std::format("[SceneDither] Last frame, 3D scene draws: {} dithered (ps_3_0), {} dithered (ps_2_x), {} ps_2_x whose vertex shader "
                                 "could not carry the position, {} ps_2_x refused, {} ps_3_0 refused, {} other | copies made: pixel {}, vertex {}; "
                                 "refused: no colour write {}, subroutines {}, relative constants {}, no free register {}, unreadable {}",
                                 g_last.dithered3, g_last.dithered2, g_last.ps2NoPair, g_last.ps2Refused, g_last.ps3Refused, g_last.other, g_made.load(),
                                 g_madeVs.load(), g_refused[2].load(), g_refused[3].load(), g_refused[4].load(), g_refused[5].load(), g_refused[6].load()));
        }
    }
}

void OnPreReset(IDirect3DDevice9* dev) { // read again at the next frame boundary
#ifndef APEX_NO_DEV_TOOLS
    GbUnbind(dev);
#else
    (void)dev;
#endif
    g_backBuffer = nullptr;
    g_rt0 = nullptr;
}

void RegisterHooks() {
    using namespace D3D9Hooks;
    RegisterPresent(kHookName, [](DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        OnFrameBoundary(ctx.device);
        return HookAction::Continue;
    }, Priority::First);
    RegisterSetRenderTarget(kHookName, [](DeviceContext& ctx, DWORD index, IDirect3DSurface9* rt) {
        if (index == 0) g_rt0 = rt;
#ifndef APEX_NO_DEV_TOOLS
        // D3D9 wants every bound target the size of target 0: ours go before the game's call (shadows, reflections, the
        // pick pass). A caller binding its own second target takes the slot over.
        if (index == 0 && rt != g_backBuffer) GbUnbind(ctx.device);
        else if (index != 0 && g_gbBound) g_gbBound = false, GbSetMask(ctx.device, true);
#endif
        return HookAction::Continue;
    }, Priority::First);
    RegisterCreatePixelShader(kHookName, [](DeviceContext& ctx, const DWORD* fn, IDirect3DPixelShader9** out) {
        if (t_own || !fn || !out || !g_on.load()) return HookAction::Continue;
        // create the game's shader here (every earlier callback already ran) to learn its pointer, then its copy
        if (FAILED(CallOriginalCreatePixelShader(ctx.device, fn, out)) || !*out) return HookAction::Block;
        Forget(*out); // a reused address: its old copy goes; the new one is made at its first 3D scene draw (CopyOf)
        return HookAction::Skip;
    }, kAfterEveryone);
    RegisterCreateVertexShader(kHookName, [](DeviceContext& ctx, const DWORD* fn, IDirect3DVertexShader9** out) {
        const bool dither = g_on.load(), jitter = g_jitterWanted.load();
        if (t_own || !fn || !out || (!dither && !jitter)) return HookAction::Continue;
        if (FAILED(CallOriginalCreateVertexShader(ctx.device, fn, out)) || !*out) return HookAction::Block;
        ForgetVs(*out); // a reused address: the old shader's copies go; new ones are made at its first 3D scene draw (VsCopyOf)
        return HookAction::Skip;
    }, kAfterEveryone);
    ObserveReissuedDraws(kHookName); // dithers Night Lighting's replacement shaders too (they run in the re-issued draw)
    RegisterDrawIndexedPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE type, INT bvi, UINT minV, UINT numV, UINT start, UINT prims) {
        return OnDraw(ctx, [&] { CallOriginalDrawIndexedPrimitive(ctx.device, type, bvi, minV, numV, start, prims); });
    }, kAfterEveryone);
    RegisterDrawPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE type, UINT start, UINT prims) {
        return OnDraw(ctx, [&] { CallOriginalDrawPrimitive(ctx.device, type, start, prims); });
    }, kAfterEveryone);
    RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
}

// The draw hooks run while the Banding Fix or the temporal AA uses them
void AcquireHooks() {
    if (g_hookUsers.fetch_add(1) == 0) {
        g_backBuffer = nullptr;
        g_rt0 = nullptr;
        RegisterHooks();
    }
}
void ReleaseHooks() {
    if (g_hookUsers.fetch_sub(1) != 1) return;
    D3D9Hooks::UnregisterAll(kHookName);
    RenderCallbacks::Remove(RenderCallbacks::preReset, OnPreReset);
    ReleaseCopies(); // an address reused while off must never meet an old copy
    g_backBuffer = nullptr;
}

} // namespace

class SceneDitherPatch : public ApexPatch {
  public:
    SceneDitherPatch() : ApexPatch("SceneDither", nullptr) {
        RegisterFloatSetting(&g_strength, "forca", SettingWidget::Slider, 1.0f, 0.0f, 1.0f, "Grain in 8-bit steps (triangular peak); 1 = +-1 step, 0 = none");
        // "graoEmMovimento" (Moving grain, removed) is left in old files and ignored: the grain is always fixed
        RegisterBoolSetting(SceneTarget::Setting(), "cenaAltaPrecisao", true, "The 3D scene in a 16-bit target, resolved once (scene_target.cpp)");
    }

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        g_on = true;
        AcquireHooks();
        isEnabled = true;
        LOG_INFO("[SceneDither] Installed");
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        lastError.clear();
        g_on = false;
        ReleaseHooks();
        isEnabled = false;
        LOG_INFO(std::format("[SceneDither] Uninstalled (copies made this session: pixel {}, vertex {})", g_made.load(), g_madeVs.load()));
        return true;
    }

    // Read live every draw
    void Update() override { pendingReinstall = false; }

    void RenderCustomUI() override {
        SAFE_IMGUI_BEGIN();
        if (ApexUi::SliderPercent("Strength", &g_strength, 0.0f, 1.0f, "How much grain hides the color steps; 100% hides them fully, 0% is off", 1.0f))
            NotifySettingChanged();
    }

    // Developer page > Debug views
    void RenderDeveloperUI() override {
        SAFE_IMGUI_BEGIN();
        size_t pairs = 0, vsPairs = 0;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            pairs = g_copies.size();
            vsPairs = g_vsCopies.size();
        }
        ApexUi::Checkbox("Show covered surfaces", &g_showCovered);
        ApexUi::Tooltip("A coarse grain on every surface the fix covers; smooth surfaces are not covered (not saved)");
        if (ApexUi::BeginAdvanced("ShaderCoverage", "Shader coverage")) {
        ImGui::TextDisabled("Pixel shaders seen: %zu (copies %u)  |  vertex copies asked: %zu (made %u)", pairs, g_made.load(), vsPairs, g_madeVs.load());
        ImGui::TextDisabled("Refused: no colour write %u, subroutines %u, relative constants %u, no free register %u, unreadable %u", g_refused[2].load(),
                            g_refused[3].load(), g_refused[4].load(), g_refused[5].load(), g_refused[6].load());
        const unsigned dithered = g_last.dithered3 + g_last.dithered2;
        const unsigned total = dithered + g_last.ps2NoPair + g_last.ps2Refused + g_last.ps3Refused + g_last.other;
        ImGui::TextDisabled("Last frame, 3D scene draws: %u with the grain (%.0f%%: ps_3_0 %u, ps_2_x %u); without: %u ps_2_x (vertex shader), %u ps_2_x, "
                            "%u ps_3_0 refused, %u other",
                            dithered, total ? 100.0 * dithered / total : 0.0, g_last.dithered3, g_last.dithered2, g_last.ps2NoPair, g_last.ps2Refused,
                            g_last.ps3Refused, g_last.other);
        ImGui::TextDisabled("Dithered by blend: opaque %u, alpha %u, additive %u (half), multiply %u (half), other %u (half)  |  left alone: "
                            "%u depth ALWAYS without depth write (UI), %u without a pixel shader",
                            g_last.blend[kOpaque], g_last.blend[kAlpha], g_last.blend[kAdditive], g_last.blend[kMultiply], g_last.blend[kOtherBlend],
                            g_last.noDepth, g_last.noShader);
            ApexUi::EndAdvanced();
        }
    }
};

APEX_REGISTER_FEATURE(SceneDitherPatch, {.displayName = "Banding Fix",
                                         .description = "Removes the color steps (banding) in lamp light, shadows and other smooth gradients of the 3D world: "
                                                        "an invisible, fixed grain where the game rounds its colors, so light fades smoothly. Menus are "
                                                        "untouched. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                                         .category = "Graphics",
                                         .experimental = false,
                                         .enabledByDefault = true,
                                         .supportedVersions = VERSION_ALL,
                                         .technicalDetails = {"A dithered copy of every pixel shader (triangular noise from the R2 pattern of the pixel position, "
                                                              "+-1/255 at Strength 100% on RGB, within half a step of black and white; alpha untouched), made at the "
                                                              "shader's first draw in the 3D scene.",
                                                              "ps_2_x copies take the pixel position from a texture coordinate that a copy of the vertex shader fills "
                                                              "with the clip position.",
                                                              "Bound only for draws into the back buffer that use the depth test (the 3D scene; not ZFUNC ALWAYS without "
                                                              "depth write), last in the draw chain, at half strength for additive and multiply blends; "
                                                              "the amount constant's previous value is put back after the draw."}})

namespace SceneBinder {
void AcquireJitter() {
    g_jitterWanted = true;
    AcquireHooks();
}
void ReleaseJitter() {
    g_jitterWanted = false;
    g_jitterOn = false;
    ReleaseHooks();
}
void SetFrameJitter(bool on, float clipX, float clipY) {
    g_jitterOn = on && g_jitterWanted.load();
    g_jitter[0] = clipX;
    g_jitter[1] = clipY;
}
Coverage LastCoverage() { return {g_last.jittered, g_last.jitterRefused, g_last.jitterNoShader}; }
#ifndef APEX_NO_DEV_TOOLS
void SetGBufferTargets(IDirect3DDevice9* dev, IDirect3DSurface9* rt1, IDirect3DSurface9* rt2) {
    if (dev) GbUnbind(dev);
    g_gbBound = false;
    g_gbRt[0] = rt1;
    g_gbRt[1] = rt2;
}
void AcquireGBuffer() {
    g_gbWanted = true;
    AcquireHooks();
    ReleaseCopies(); // the copies made so far have no G-buffer outputs: made again at their next scene draw
}
void ReleaseGBuffer(IDirect3DDevice9* dev) {
    SetGBufferTargets(dev, nullptr, nullptr);
    g_gbWanted = false;
    ReleaseCopies();
    ReleaseHooks();
}
void GBufferUnbind(IDirect3DDevice9* dev) { GbUnbind(dev); }
void GBufferMaskOff(IDirect3DDevice9* dev) {
    if (g_gbBound) GbSetMask(dev, false);
}
GBufferCounts TakeGBufferCounts() {
    const GBufferCounts c = g_gbCount;
    g_gbCount = {};
    return c;
}
#endif
} // namespace SceneBinder

namespace SceneDither {
bool On() { return g_on.load(std::memory_order_relaxed); }
float Strength() { return std::clamp(g_strength, 0.0f, 1.0f); }
float GrainPhase() { return 0.0f; }
} // namespace SceneDither
