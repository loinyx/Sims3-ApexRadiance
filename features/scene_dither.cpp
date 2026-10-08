// Banding Fix (scene dither)
// The game draws its 3D scene straight into an 8-bit back buffer, so a smooth light gradient (a lamp's pool of light on
// a wall, the fall-off of a room light map) is rounded to 256 steps per channel, and in the dark range each step is a
// visible ring. This adds a fixed, invisible grain to every scene pixel shader's colour before the rounding (triangular
// noise of +-1 step at the default Strength, from interleaved gradient noise of the pixel position), so the rings become
// a fine grain. The alpha (the bloom mask) is never touched.
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
//    test is on (the 3D scene; the UI draws with it off and reads the back buffer back many times per frame, where a
//    dither would feed on itself), the copies are bound for the draw with the amount constant, then the game's shaders
//    and the constant's previous value are put back.
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
#include "f10_study.h"
#include "shader_lookup_cache.h"
#include "imgui.h"
#include "ui/violet_theme.h"
#include "ui/widgets.h"
#include "build_flavor.h"
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
constexpr int kUsualTexcoord = 7;                                       // the texture coordinate most ps_2_x copies use

std::atomic<bool> g_on{false};
thread_local bool t_own = false; // our own creations pass through
std::mutex g_lock;
struct Copy {
    IDirect3DPixelShader9* ps = nullptr; // the dithered copy, null = none
    int amountReg = -1;                 // its constant holding the amount (set around the draw)
    int texcoord = -1;                  // ps_2_x copies: the texture coordinate with the clip position (-1: ps_3_0, vPos)
    DWORD version = 0;                  // the game shader's version token
};
std::unordered_map<IDirect3DPixelShader9*, Copy> g_copies; // game pixel shader -> its copy
// game vertex shader -> its copies writing the clip position to TEXCOORDk (null = cannot), key = pointer * 8 + k
std::unordered_map<uint64_t, IDirect3DVertexShader9*> g_vsCopies;
ShaderLookupCache<Copy> g_pixelLookup;
ShaderLookupCache<IDirect3DVertexShader9*> g_vertexLookup;
float g_strength = 1.0f;                  // the grain in 8-bit steps (triangular, peak): 1 = +-1 step
bool g_moving = false;                    // a new grain pattern every frame (high frame rates average it away)
unsigned g_frameIndex = 0;                // counts frames for the moving grain
bool g_showCovered = false;               // Developer: a coarse grain where the fix applies (not saved)
constexpr float kShowCoveredSteps = 24.0f;
IDirect3DSurface9* g_backBuffer = nullptr; // identity only (render thread)
// render target 0, followed through SetRenderTarget as PostScene and Picture do (05/10: GetRenderTarget + Release on every
// draw before); read again at the frame boundary after a Reset (which sets it without a SetRenderTarget call)
IDirect3DSurface9* g_rt0 = nullptr;

// statistics (Developer page, dev log)
std::atomic<unsigned> g_made{0}, g_madeVs{0}, g_refused[7] = {};
struct FrameCount {
    unsigned dithered3 = 0, dithered2 = 0; // 3D scene draws with the grain: ps_3_0, ps_2_x
    unsigned ps2NoPair = 0;                // ps_2_x with a copy whose vertex shader could not carry the position
    unsigned ps2Refused = 0, ps3Refused = 0, other = 0;
    unsigned jittered = 0, jitterRefused = 0, jitterNoShader = 0; // temporal AA: scene draws moved / whose vertex shader was refused / fixed-function
};
FrameCount g_frame, g_last;
unsigned long long g_lastLog = 0;
int g_logs = 0;

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

// The dithered copy of a game pixel shader (Copy::ps null when it cannot have one)
Copy MakeCopy(IDirect3DDevice9* dev, std::vector<DWORD> t) {
    Copy c;
    if (t.empty()) {
        g_refused[static_cast<int>(ShaderPatches::DitherResult::Unreadable)]++;
        return c;
    }
    c.version = t[0];
    const bool ps2 = t[0] == 0xFFFF0200u || t[0] == 0xFFFF0201u;
    const ShaderPatches::DitherResult r = ps2 ? ShaderPatches::AddDither2(t, &c.amountReg, &c.texcoord) : ShaderPatches::AddDither(t, &c.amountReg);
    if (r != ShaderPatches::DitherResult::Ok) {
        g_refused[static_cast<int>(r)]++;
        c.texcoord = -1;
        return c;
    }
    t_own = true;
    const HRESULT hr = D3D9Hooks::CallOriginalCreatePixelShader(dev, t.data(), &c.ps);
    t_own = false;
    if (FAILED(hr) || !c.ps) {
        c.ps = nullptr;
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
}

void RememberVs(uint64_t key, IDirect3DVertexShader9* copy) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_vertexLookup.Clear();
    auto [it, fresh] = g_vsCopies.try_emplace(key, copy);
    if (!fresh) {
        if (it->second) it->second->Release();
        it->second = copy;
    }
}

// A new game pixel shader at an address: its old copy goes
void Forget(IDirect3DPixelShader9* ps) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_pixelLookup.Clear();
    const auto it = g_copies.find(ps);
    if (it == g_copies.end()) return;
    if (it->second.ps) it->second.ps->Release();
    g_copies.erase(it);
}

// A new game vertex shader at an address: its old copies (every k) go
void ForgetVs(IDirect3DVertexShader9* vs) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_vertexLookup.Clear();
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
    for (auto& [game, copy] : g_copies)
        if (copy.ps) copy.ps->Release();
    g_copies.clear();
    for (auto& [key, copy] : g_vsCopies)
        if (copy) copy->Release();
    g_vsCopies.clear();
}

// Render thread: the copy for the bound pixel shader (made now when the shader is older than the feature)
Copy CopyOf(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps) {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        Copy cached;
        const auto key = reinterpret_cast<std::uintptr_t>(ps);
        if (g_pixelLookup.Find(key, cached)) return cached;
        const auto it = g_copies.find(ps);
        if (it != g_copies.end()) { g_pixelLookup.Store(key, it->second); return it->second; }
    }
    const Copy copy = MakeCopy(dev, ReadCode(ps));
    Remember(ps, copy);
    return copy;
}

// Render thread: the vertex copy writing TEXCOORDk (k -1: none) and / or moved by the jitter (made now when missing)
IDirect3DVertexShader9* VsCopyOf(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs, int k, bool jitter) {
    const uint64_t key = VsKey(vs, k, jitter);
    {
        std::lock_guard<std::mutex> lock(g_lock);
        IDirect3DVertexShader9* cached = nullptr;
        if (g_vertexLookup.Find(key, cached)) return cached;
        const auto it = g_vsCopies.find(key);
        if (it != g_vsCopies.end()) { g_vertexLookup.Store(key, it->second); return it->second; }
    }
    IDirect3DVertexShader9* copy = MakeVsCopy(dev, ReadCode(vs), k, jitter);
    RememberVs(key, copy);
    return copy;
}

template <typename DrawFn> D3D9Hooks::HookAction OnDraw(D3D9Hooks::DeviceContext& ctx, DrawFn draw) {
    IDirect3DDevice9* dev = ctx.device;
    const bool dither = g_on.load(std::memory_order_relaxed), jitter = g_jitterOn;
    if ((!dither && !jitter) || !g_backBuffer) return D3D9Hooks::HookAction::Continue;
    if (!g_rt0 || g_rt0 != g_backBuffer) return D3D9Hooks::HookAction::Continue;
    if (ctx.ZEnable() == D3DZB_FALSE) return D3D9Hooks::HookAction::Continue; // UI and 2D passes
    // the Banding Fix's pixel copy
    IDirect3DPixelShader9* ps = nullptr;
    Copy copy;
    if (dither) {
        dev->GetPixelShader(&ps);
        if (ps) {
            ps->Release(); // the device keeps it alive
            copy = CopyOf(dev, ps);
            if (!copy.ps) {
                const DWORD major = (copy.version >> 8) & 0xFF;
                (major == 2 ? g_frame.ps2Refused : major == 3 ? g_frame.ps3Refused : g_frame.other)++;
            }
        }
    }
    bool useDither = copy.ps != nullptr;
    const int k = useDither ? copy.texcoord : -1; // ps_2_x copies: the vertex shader must carry the position
    // the vertex copy: TEXCOORDk and / or the temporal jitter
    IDirect3DVertexShader9 *vs = nullptr, *vsCopy = nullptr;
    DWORD zfunc = D3DCMP_LESSEQUAL;
    if (jitter) dev->GetRenderState(D3DRS_ZFUNC, &zfunc);
    // Depth enabled + ALWAYS is also used by screen-space composition draws;
    // ZENABLE alone does not identify a projected 3D surface (pool capture #202).
    bool useJitter = jitter && zfunc != D3DCMP_ALWAYS;
    if (useJitter || k >= 0) {
        dev->GetVertexShader(&vs);
        if (vs) vs->Release();
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
        return D3D9Hooks::HookAction::Continue;
    }
    float before[4] = {};
    if (useDither) {
        D3DVIEWPORT9 vp{};
        dev->GetViewport(&vp);
        dev->GetPixelShaderConstantF(static_cast<UINT>(copy.amountReg), before, 1);
        // Developer "Show covered surfaces": a coarse grain only where the fix applies
        const float amount[4] = {(g_showCovered ? kShowCoveredSteps : SceneDither::Strength()) / 255.0f, 0.5f * static_cast<float>(vp.Width),
                                 0.5f * static_cast<float>(vp.Height), SceneDither::GrainPhase()};
        D3D9Hooks::CallOriginalSetPixelShaderConstantF(dev, static_cast<UINT>(copy.amountReg), amount, 1);
        D3D9Hooks::CallOriginalSetPixelShader(dev, copy.ps);
    }
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
        (k >= 0 ? g_frame.dithered2 : g_frame.dithered3)++;
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
    g_frameIndex++;
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

void OnPreReset(IDirect3DDevice9*) { // read again at the next frame boundary
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
        RegisterBoolSetting(&g_moving, "graoEmMovimento", false, "A new grain pattern every frame (vanishes at high frame rates)");
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
        bool changed = ApexUi::SliderPercent("Strength", &g_strength, 0.0f, 1.0f, "How much grain hides the color steps; 100% hides them fully, 0% is off", 1.0f);
        changed |= ApexUi::SwitchRow("Moving grain", &g_moving, "A new grain each frame: invisible at high frame rates, a faint shimmer at low ones",
                                     false);
        if (changed) NotifySettingChanged();
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
            ApexUi::EndAdvanced();
        }
    }
};

APEX_REGISTER_FEATURE(SceneDitherPatch, {.displayName = "Banding Fix",
                                         .description = "Removes the color steps (banding) in lamp light, shadows and other smooth gradients of the 3D world: "
                                                        "an invisible, fixed grain where the game rounds its colors, so light fades smoothly. Menus are "
                                                        "untouched. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                                         .category = "Graphics",
                                         .experimental = true,
                                         .enabledByDefault = true,
                                         .supportedVersions = VERSION_ALL,
                                         .technicalDetails = {"A dithered copy of every pixel shader (triangular noise from interleaved gradient noise of the pixel "
                                                              "position, +-1/255 at Strength 100% on RGB; alpha untouched), made at the shader's first draw in the 3D scene.",
                                                              "ps_2_x copies take the pixel position from a texture coordinate that a copy of the vertex shader fills "
                                                              "with the clip position.",
                                                              "Bound only for draws into the back buffer with the depth test on (the 3D scene), last in the draw chain; "
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
} // namespace SceneBinder

namespace SceneDither {
bool On() { return g_on.load(std::memory_order_relaxed); }
float Strength() { return std::clamp(g_strength, 0.0f, 1.0f); }
float GrainPhase() {
    if (!g_moving) return 0.0f;
    const double v = g_frameIndex * 0.6180339887498949; // golden ratio steps: every frame a different, even phase
    return static_cast<float>(v - static_cast<double>(static_cast<unsigned long long>(v)));
}
} // namespace SceneDither
