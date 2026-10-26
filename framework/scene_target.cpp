// High-precision scene (see scene_target.h).
// Part of Apex Radiance. Credits: @loinyx
#include "scene_target.h"
#include "scene_target_logic.h"
#include "apex_log.h"
#include "d3d9_extra_hooks.h"
#include "d3d9_hooks.h"
#include "hook_guard.h"
#include "post_scene.h"
#include "render_callbacks.h"
#include "scene_dither.h"
#include "shader_cache.h"
#include <d3dcompiler.h>
#include <format>
#include <iterator>
#include <string>

namespace SceneTarget {
namespace {

using SceneTargetLogic::Refusal;
constexpr const char* kHookName = "SceneTarget";
constexpr int kBoundaryOrder = 5; // before every other post-scene effect (PostScene::Order)

const ShaderCache::Id kResolvePsId = [] {
    ShaderCache::Desc d;
    d.tag = "SceneTarget ResolvePS";
    d.source = SceneTargetLogic::kResolveHlsl;
    d.sourceName = "scene_target.hlsl";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    return ShaderCache::Add(std::move(d));
}();

bool g_want = true; // the switch (on by default)
IDirect3DTexture9* g_tex = nullptr;
IDirect3DSurface9* g_surf = nullptr;
IDirect3DPixelShader9* g_ps = nullptr;
IDirect3DSurface9* g_bb = nullptr; // identity only, read at every Present
UINT g_w = 0, g_h = 0;
bool g_armed = false;    // this frame substitutes until resolved
bool g_resolved = false; // this frame's scene is in the back buffer
bool g_inResolve = false;
bool g_installed = false; // substitution callbacks set and the PostScene effect added
State g_state = State::Off;
D3DSURFACE_DESC g_refusedFor{}; // the back buffer a refusal was made for: checked again only when it changes
Stats g_stats;

template <class T> void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

bool Live() { return g_armed && !g_resolved; }

IDirect3DSurface9* SubstituteRt(DWORD index, IDirect3DSurface9* requested) {
    return SceneTargetLogic::Substitutes(Live(), index, requested, g_bb) ? g_surf : requested;
}
IDirect3DSurface9* ReportRt(IDirect3DSurface9* actual) { return actual && actual == g_surf && g_bb ? g_bb : actual; }

enum Cause { kBoundary, kCopy, kEndScene, kPresent };

struct QuadVertex {
    float x, y, z, rhw, u, v;
};

constexpr D3DRENDERSTATETYPE kStates[] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE,
                                          D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE,
                                          D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_COLORWRITEENABLE,
                                          D3DRS_FILLMODE};
constexpr DWORD kPassStates[] = {D3DZB_FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, FALSE, FALSE, FALSE, 0, 0xF, D3DFILL_SOLID};
constexpr D3DSAMPLERSTATETYPE kSamplerStates[] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV,
                                                  D3DSAMP_SRGBTEXTURE};
constexpr DWORD kPassSampler[] = {D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, 0};

// The target drawn into the back buffer with the grain. Everything it changes is put back; render target 0 becomes the back
// buffer when it was the target (the game asked for the back buffer), otherwise stays what it was.
void Resolve(IDirect3DDevice9* dev, Cause cause) {
    if (!Live() || g_inResolve || !dev || !g_ps || !g_tex) return;
    g_resolved = true; // first: from here a SetRenderTarget(back buffer) binds the back buffer
    g_inResolve = true;
    LARGE_INTEGER t0{}, t1{}, freq{};
    QueryPerformanceCounter(&t0);

    IDirect3DSurface9* rt[4] = {};
    for (DWORD i = 0; i < 4; i++)
        if (FAILED(ExtraHooks::RawGetRenderTarget(dev, i, &rt[i]))) rt[i] = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    IDirect3DVertexBuffer9* stream0 = nullptr;
    IDirect3DBaseTexture9* tex0 = nullptr;
    UINT offset0 = 0, stride0 = 0;
    DWORD fvf = 0, rs[std::size(kStates)] = {}, ss[std::size(kSamplerStates)] = {};
    float c0[4] = {};
    D3DVIEWPORT9 vp{};
    RECT scissor{};
    dev->GetPixelShader(&ps);
    dev->GetVertexShader(&vs);
    dev->GetVertexDeclaration(&decl);
    dev->GetFVF(&fvf);
    dev->GetStreamSource(0, &stream0, &offset0, &stride0);
    D3D9Hooks::GetBoundTexture(dev, 0, &tex0);
    for (size_t i = 0; i < std::size(kStates); i++) dev->GetRenderState(kStates[i], &rs[i]);
    for (size_t i = 0; i < std::size(kSamplerStates); i++) dev->GetSamplerState(0, kSamplerStates[i], &ss[i]);
    dev->GetPixelShaderConstantF(0, c0, 1);
    dev->GetViewport(&vp);
    dev->GetScissorRect(&scissor);

    D3D9Hooks::CallOriginalSetRenderTarget(dev, 0, g_bb); // resets the viewport to the whole back buffer
    for (DWORD i = 1; i < 4; i++)
        if (rt[i]) D3D9Hooks::CallOriginalSetRenderTarget(dev, i, nullptr);
    for (size_t i = 0; i < std::size(kStates); i++) dev->SetRenderState(kStates[i], kPassStates[i]);
    for (size_t i = 0; i < std::size(kSamplerStates); i++) dev->SetSamplerState(0, kSamplerStates[i], kPassSampler[i]);
    const float amount[4] = {SceneDither::Strength(), SceneDither::GrainPhase(), 0.0f, 0.0f};
    D3D9Hooks::CallOriginalSetPixelShaderConstantF(dev, 0, amount, 1);
    D3D9Hooks::CallOriginalSetVertexShader(dev, nullptr);
    D3D9Hooks::CallOriginalSetPixelShader(dev, g_ps);
    D3D9Hooks::CallOriginalSetTexture(dev, 0, g_tex);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    const float x1 = static_cast<float>(g_w) - 0.5f, y1 = static_cast<float>(g_h) - 0.5f;
    const QuadVertex quad[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {x1, -0.5f, 0, 1, 1, 0}, {-0.5f, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVertex));

    D3D9Hooks::CallOriginalSetTexture(dev, 0, tex0);
    D3D9Hooks::CallOriginalSetPixelShader(dev, ps);
    D3D9Hooks::CallOriginalSetVertexShader(dev, vs);
    D3D9Hooks::CallOriginalSetPixelShaderConstantF(dev, 0, c0, 1);
    if (decl) dev->SetVertexDeclaration(decl);
    else dev->SetFVF(fvf);
    dev->SetStreamSource(0, stream0, offset0, stride0); // DrawPrimitiveUP cleared stream 0
    for (size_t i = 0; i < std::size(kStates); i++) dev->SetRenderState(kStates[i], rs[i]);
    for (size_t i = 0; i < std::size(kSamplerStates); i++) dev->SetSamplerState(0, kSamplerStates[i], ss[i]);
    for (DWORD i = 1; i < 4; i++)
        if (rt[i]) D3D9Hooks::CallOriginalSetRenderTarget(dev, i, rt[i]);
    if (rt[0] && rt[0] != g_surf) D3D9Hooks::CallOriginalSetRenderTarget(dev, 0, rt[0]);
    dev->SetViewport(&vp); // (SetRenderTarget reset both)
    dev->SetScissorRect(&scissor);
    for (auto*& s : rt) SafeRelease(s);
    SafeRelease(ps);
    SafeRelease(vs);
    SafeRelease(decl);
    SafeRelease(stream0);
    SafeRelease(tex0);

    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    const float ms = static_cast<float>(1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart));
    g_stats.resolveMs = g_stats.resolveMs > 0.0f ? g_stats.resolveMs + 0.05f * (ms - g_stats.resolveMs) : ms;
    unsigned* counts[] = {&g_stats.atBoundary, &g_stats.atCopy, &g_stats.atEndScene, &g_stats.atPresent};
    ++*counts[cause];
    g_inResolve = false;
}

void BeforeAccess(IDirect3DDevice9* dev, IDirect3DSurface9* a, IDirect3DSurface9* b) {
    if (SceneTargetLogic::NeedsResolve(Live(), a, b, g_bb)) Resolve(dev, kCopy);
}

void AtBoundary(IDirect3DDevice9* dev) { Resolve(dev, kBoundary); }

// The game's EndScene, before the Apex overlay: only with the target bound (a scene of other targets, the mouse pick, may end
// first)
void AtEndScene(IDirect3DDevice9* dev) {
    if (!Live()) return;
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(ExtraHooks::RawGetRenderTarget(dev, 0, &rt)) || !rt) return;
    const bool bound = rt == g_surf;
    rt->Release();
    if (bound) Resolve(dev, kEndScene);
}

// Unbinds and releases the target (the back buffer goes back to render target 0). Resolves first when asked
void ReleaseTarget(IDirect3DDevice9* dev) {
    g_armed = false;
    if (dev && g_surf) {
        IDirect3DSurface9* rt = nullptr;
        if (SUCCEEDED(ExtraHooks::RawGetRenderTarget(dev, 0, &rt)) && rt) {
            if (rt == g_surf && g_bb) D3D9Hooks::CallOriginalSetRenderTarget(dev, 0, g_bb);
            rt->Release();
        }
    }
    SafeRelease(g_surf);
    SafeRelease(g_tex);
}

void TurnOff(IDirect3DDevice9* dev) {
    ReleaseTarget(dev);
    if (g_installed) {
        g_installed = false;
        D3D9Hooks::SetRenderTargetSubstitute(nullptr);
        ExtraHooks::SetTargetSubstitution(nullptr, nullptr);
        PostScene::Remove(AtBoundary);
        LOG_INFO("[SceneTarget] Off");
    }
    SafeRelease(g_ps);
    g_refusedFor = {};
    g_state = State::Off;
}

bool Supported(IDirect3DDevice9* dev) {
    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    D3DDISPLAYMODE mode{};
    if (FAILED(dev->GetDirect3D(&d3d)) || !d3d) return false;
    bool ok = SUCCEEDED(dev->GetCreationParameters(&cp)) && SUCCEEDED(dev->GetDisplayMode(0, &mode));
    ok = ok && d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format, D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING,
                                      D3DRTYPE_TEXTURE, SceneTargetLogic::kFormat) == D3D_OK;
    // the game's depth-stencil (D24S8, or the INTZ copy Depth Blur binds; DepthShare) stays bound with the target
    IDirect3DSurface9* ds = nullptr;
    if (ok && SUCCEEDED(ExtraHooks::RawGetDepthStencilSurface(dev, &ds)) && ds) {
        D3DSURFACE_DESC dd{};
        if (SUCCEEDED(ds->GetDesc(&dd)))
            ok = d3d->CheckDepthStencilMatch(cp.AdapterOrdinal, cp.DeviceType, mode.Format, SceneTargetLogic::kFormat, dd.Format) == D3D_OK;
        ds->Release();
    }
    d3d->Release();
    return ok;
}

// The target, the shader and the hooks for this back buffer; false = not this frame (g_state says why)
bool Ready(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    D3DSURFACE_DESC bd{};
    bb->GetDesc(&bd);
    g_bb = bb;
    bb->Release();
    if (g_tex && bd.Width == g_w && bd.Height == g_h) return true;
    ReleaseTarget(dev); // another size: made again
    if (g_state != State::Waiting && g_state != State::Active && g_state != State::Off && bd.Width == g_refusedFor.Width &&
        bd.Height == g_refusedFor.Height && bd.Format == g_refusedFor.Format && bd.MultiSampleType == g_refusedFor.MultiSampleType)
        return false;
    g_refusedFor = bd;
    const Refusal why = SceneTargetLogic::Check(bd.Format, bd.MultiSampleType, bd.MultiSampleType == D3DMULTISAMPLE_NONE && Supported(dev));
    if (why != Refusal::None) {
        g_state = why == Refusal::Multisample ? State::Multisample : why == Refusal::BackBufferFormat ? State::BackBufferFormat : State::NoSupport;
        LOG_WARNING(std::format("[SceneTarget] Not used: back buffer {}x{} format {} multisample {}, 16-bit target supported: {}", bd.Width, bd.Height,
                                static_cast<int>(bd.Format), static_cast<int>(bd.MultiSampleType), why == Refusal::NoSupport ? "no" : "not checked"));
        return false;
    }
    if (!ShaderCache::PrecompileComplete()) { // never waits for the compile on the render thread
        g_state = State::Waiting;
        g_refusedFor = {};
        return false;
    }
    if (!g_ps) {
        std::string error;
        if (ShaderCache::CreatePixelShader(dev, kResolvePsId, &g_ps, &error) != ShaderCache::Result::Ok) {
            LOG_ERROR("[SceneTarget] Resolve shader not created: " + error);
            g_state = State::NoSupport;
            return false;
        }
    }
    if (!ExtraHooks::EnsureInstalled(dev) || !ExtraHooks::EnsureTargetHooks(dev)) {
        g_state = State::NoHooks;
        return false;
    }
    if (FAILED(dev->CreateTexture(bd.Width, bd.Height, 1, D3DUSAGE_RENDERTARGET, SceneTargetLogic::kFormat, D3DPOOL_DEFAULT, &g_tex, nullptr)) || !g_tex ||
        FAILED(g_tex->GetSurfaceLevel(0, &g_surf)) || !g_surf) {
        SafeRelease(g_surf);
        SafeRelease(g_tex);
        g_state = State::NoMemory;
        LOG_WARNING(std::format("[SceneTarget] The {}x{} 16-bit target could not be created", bd.Width, bd.Height));
        return false;
    }
    g_w = bd.Width, g_h = bd.Height;
    if (!g_installed) {
        g_installed = true;
        D3D9Hooks::SetRenderTargetSubstitute(SubstituteRt);
        ExtraHooks::SetTargetSubstitution(BeforeAccess, ReportRt);
        PostScene::Add(kBoundaryOrder, AtBoundary, false);
    }
    g_state = State::Active;
    LOG_INFO(std::format("[SceneTarget] 16-bit scene target ready ({}x{}, {} MB)", g_w, g_h, VideoMemoryMB()));
    return true;
}

// Present, before every other callback: this frame's scene reaches the back buffer
void ResolveAtPresent(IDirect3DDevice9* dev) {
    if (Live()) Resolve(dev, kPresent);
}

// Present, after every other callback: the next frame starts in the target (a Clear before its first SetRenderTarget lands
// in it). Present shows the back buffer whatever is bound.
void ArmAtPresent(IDirect3DDevice9* dev) {
    if (!g_want || !SceneDither::On()) {
        if (g_installed || g_tex || g_ps) TurnOff(dev);
        return;
    }
    if (g_state == State::Off) g_state = State::Waiting;
    if (!Ready(dev)) {
        g_armed = false;
        return;
    }
    g_armed = true;
    g_resolved = false;
    IDirect3DSurface9* rt = nullptr;
    if (SUCCEEDED(ExtraHooks::RawGetRenderTarget(dev, 0, &rt)) && rt) {
        if (rt == g_bb) D3D9Hooks::CallOriginalSetRenderTarget(dev, 0, g_bb); // (substituted)
        rt->Release();
    }
}

void OnPreReset(IDirect3DDevice9* dev) {
    ReleaseTarget(dev);
    g_bb = nullptr;
    g_refusedFor = {};
    if (g_state != State::Off) g_state = State::Waiting;
}

} // namespace

void Init() {
    using namespace D3D9Hooks;
    RegisterPresent(kHookName, [](DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        HookGuard::Try("High-precision scene resolve", [&] { ResolveAtPresent(ctx.device); });
        return HookAction::Continue;
    }, static_cast<Priority>(-3000)); // before ApexCore (-2000) and every observer of the frame's end
    RegisterPresent(kHookName, [](DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        HookGuard::Try("High-precision scene", [&] { ArmAtPresent(ctx.device); });
        return HookAction::Continue;
    }, static_cast<Priority>(100000));
    RenderCallbacks::Add(RenderCallbacks::endSceneBeforeOverlay, AtEndScene);
    RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
}

bool* Setting() { return &g_want; }
bool Substituting() { return Live(); }
State CurrentState() { return g_want && SceneDither::On() ? g_state : State::Off; }
unsigned VideoMemoryMB() {
    return g_tex ? static_cast<unsigned>((static_cast<unsigned long long>(g_w) * g_h * SceneTargetLogic::kBytesPerPixel + (1u << 19)) >> 20) : 0;
}
Stats ReadStats() { return g_stats; }

} // namespace SceneTarget
