#ifndef APEX_NO_DEV_TOOLS // developer builds only until a consumer exists (research 14 §6, WP0 and WP1)
// G-buffer, first steps (work package GB-01; docs/features/realtime-gi/wp/GB-01.md):
// - WP0: one log line with the device's multiple-render-target capabilities and the formats the G-buffer needs, read
//   once per device through whatever d3d9 the game runs on (DXVK, the bridge client).
// - WP1: camera motion vectors. A post-scene pass before Ambient Occlusion reads the shared INTZ depth and writes
//   mv = current NDC - previous NDC into a G16R16F target, from D = prevViewProj · inverse(viewProj) made in double on
//   the CPU (gbuffer_motion.h). Exact for everything that does not move by itself; movers are WP8. A camera cut or a
//   frame without a previous camera writes GBufferMotion::kInvalid everywhere.
// - view = 1 draws the target colour-coded over the scene, after Depth Blur and before the Color look: grey = still,
//   red / green = horizontal / vertical motion (0.025 NDC = full), magenta = invalid.
// No consumer yet: the target is made and kept, nothing reads it. [patches.GBuffer], off by default.
//
// Part of Apex Radiance. Credits: @loinyx

#include "patch_base.h"
#include "apex_version.h"
#include "apex_log.h"
#include "d3d9_extra_hooks.h"
#include "render_callbacks.h"
#include "depth_share.h"
#include "post_scene.h"
#include "world_session.h"
#include "shader_cache.h"
#include "gbuffer_motion.h"
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cstring>
#include <format>
#include <iterator>
#include <string>

namespace {

constexpr int kMotionOrder = PostScene::kAmbientOcclusion - 5; // before every effect that could take it
constexpr int kViewOrder = PostScene::kDepthBlur + 5;          // after the effects, before Picture's Color
constexpr D3DFORMAT kMotionFormat = D3DFMT_G16R16F;

const char* kShaderSource = R"HLSL(
sampler2D sIn : register(s0);
float4 cD0 : register(c0); // rows of D = prevViewProj * inverse(viewProj)
float4 cD1 : register(c1);
float4 cD2 : register(c2);
float4 cD3 : register(c3);
float4 cFlags : register(c4); // x = 1: the whole frame is invalid; y = invalid value; z = view scale

float4 MotionPS(float2 uv : TEXCOORD0) : COLOR0
{
    if (cFlags.x > 0.5) return float4(cFlags.y, cFlags.y, 0, 0);
    float4 p = float4(uv.x * 2 - 1, 1 - uv.y * 2, tex2D(sIn, uv).r, 1);
    float4 q = float4(dot(cD0, p), dot(cD1, p), dot(cD2, p), dot(cD3, p));
    if (q.w <= 1e-6) return float4(cFlags.y, cFlags.y, 0, 0);
    return float4(p.xy - q.xy / q.w, 0, 0);
}

float4 ViewPS(float2 uv : TEXCOORD0) : COLOR0
{
    float2 mv = tex2D(sIn, uv).rg;
    if (max(abs(mv.x), abs(mv.y)) > 1000) return float4(1, 0, 1, 1);
    return float4(saturate(0.5 + mv * cFlags.z), 0.5, 1);
}
)HLSL";

ShaderCache::Id AddShader(const char* tag, const char* entry) {
    ShaderCache::Desc d;
    d.tag = tag;
    d.source = kShaderSource;
    d.sourceName = "gbuffer.hlsl";
    d.entry = entry;
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    d.priority = 1;
    return ShaderCache::Add(std::move(d));
}
const ShaderCache::Id kMotionPsId = AddShader("GBuffer MotionPS", "MotionPS");
const ShaderCache::Id kViewPsId = AddShader("GBuffer ViewPS", "ViewPS");

template <class T> void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

struct State {
    bool active = false;
    int view = 0;
    IDirect3DDevice9* capsDevice = nullptr; // caps logged for this device
    IDirect3DPixelShader9* psMotion = nullptr;
    IDirect3DPixelShader9* psView = nullptr;
    bool shadersTried = false;
    IDirect3DTexture9* motion = nullptr;
    IDirect3DSurface9* motionSurf = nullptr;
    UINT width = 0, height = 0;
    bool createFailed = false;
    bool motionThisFrame = false; // the view shows only a target written this frame
    float prevVp[4][4] = {};
    bool havePrev = false;
    bool loggedRunning = false;
    // counters for the 10 s line
    unsigned frames = 0, cuts = 0, noCamera = 0;
    unsigned long long lastLog = 0;
} g;

const char* YesNo(bool b) { return b ? "yes" : "no"; }

// WP0: the capabilities the G-buffer needs (research 14 §3.1, §3.8), once per device
void LogCaps(IDirect3DDevice9* dev) {
    if (g.capsDevice == dev) return;
    g.capsDevice = dev;
    D3DCAPS9 caps{};
    if (FAILED(dev->GetDeviceCaps(&caps))) { LOG_WARNING("[GBuffer] Caps: GetDeviceCaps failed"); return; }
    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    D3DDISPLAYMODE mode{};
    D3DADAPTER_IDENTIFIER9 id{};
    std::string formats;
    if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d && SUCCEEDED(dev->GetCreationParameters(&cp)) &&
        SUCCEEDED(d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &mode))) {
        d3d->GetAdapterIdentifier(cp.AdapterOrdinal, 0, &id);
        const struct { D3DFORMAT f; const char* name; } kFormats[] = {
            {D3DFMT_A8R8G8B8, "A8R8G8B8"}, {D3DFMT_A2B10G10R10, "A2B10G10R10"}, {D3DFMT_G16R16F, "G16R16F"}, {D3DFMT_A16B16G16R16F, "A16B16G16R16F"}};
        for (const auto& f : kFormats) {
            const bool rt = SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, f.f));
            const bool blend = rt && SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                                      D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DRTYPE_TEXTURE, f.f));
            formats += std::format(" {}={}", f.name, rt ? (blend ? "rt+blend" : "rt") : "no");
        }
    } else {
        formats = " formats=unknown";
    }
    SafeRelease(d3d);
    const DWORD misc = caps.PrimitiveMiscCaps;
    LOG_INFO(std::format("[GBuffer] Caps: adapter=\"{}\" mrt={} independentWriteMasks={} mrtPostBlend={} independentBitDepths={} ps={}.{} "
                         "ps2x temps={} slots={}{}",
                         id.Description, caps.NumSimultaneousRTs, YesNo(misc & D3DPMISCCAPS_INDEPENDENTWRITEMASKS),
                         YesNo(misc & D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING), YesNo(misc & D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS),
                         D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion), D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion),
                         caps.PS20Caps.NumTemps, caps.PS20Caps.NumInstructionSlots, formats));
}

void ReleaseTarget() {
    SafeRelease(g.motionSurf);
    SafeRelease(g.motion);
    g.width = g.height = 0;
    g.havePrev = false;
    g.motionThisFrame = false;
}

void ReleaseShaders() {
    SafeRelease(g.psMotion);
    SafeRelease(g.psView);
    g.shadersTried = false;
}

bool EnsureShaders(IDirect3DDevice9* dev) {
    if (g.psMotion && g.psView) return true;
    if (g.shadersTried) return false;
    g.shadersTried = true;
    std::string msg;
    if (ShaderCache::CreatePixelShader(dev, kMotionPsId, &g.psMotion, &msg) != ShaderCache::Result::Ok ||
        ShaderCache::CreatePixelShader(dev, kViewPsId, &g.psView, &msg) != ShaderCache::Result::Ok) {
        LOG_ERROR("[GBuffer] Shaders unavailable: " + msg);
        return false;
    }
    return true;
}

bool EnsureTarget(IDirect3DDevice9* dev, UINT w, UINT h) {
    if (g.motion && g.width == w && g.height == h) return true;
    ReleaseTarget();
    if (g.createFailed) return false;
    if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, kMotionFormat, D3DPOOL_DEFAULT, &g.motion, nullptr)) || !g.motion ||
        FAILED(g.motion->GetSurfaceLevel(0, &g.motionSurf)) || !g.motionSurf) {
        ReleaseTarget();
        g.createFailed = true;
        LOG_ERROR(std::format("[GBuffer] Could not create the {}x{} G16R16F motion target; motion vectors off", w, h));
        return false;
    }
    g.width = w;
    g.height = h;
    return true;
}

// ---- what a pass touches, saved and put back (the same set as Depth Blur's, one sampler) ----
constexpr D3DRENDERSTATETYPE kRenderStates[] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE,
    D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE,
    D3DRS_CLIPPLANEENABLE, D3DRS_COLORWRITEENABLE};
constexpr D3DSAMPLERSTATETYPE kSamplerStates[] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE};
constexpr int kRS = static_cast<int>(std::size(kRenderStates));
constexpr int kSS = static_cast<int>(std::size(kSamplerStates));
constexpr UINT kPSConsts = 5; // c0..c4

struct SavedState {
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9* rt0 = nullptr;
    IDirect3DSurface9* ds = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    DWORD fvf = 0;
    IDirect3DVertexBuffer9* stream0 = nullptr;
    UINT stream0Offset = 0, stream0Stride = 0;
    IDirect3DBaseTexture9* tex = nullptr;
    DWORD rs[kRS] = {};
    DWORD ss[kSS] = {};
    float psConst[kPSConsts * 4] = {};
    D3DVIEWPORT9 viewport{};

    SavedState() = default;
    SavedState(const SavedState&) = delete;
    SavedState& operator=(const SavedState&) = delete;
    ~SavedState() {
        if (dev) Restore();
        Release();
    }
    bool Capture(IDirect3DDevice9* d) {
        bool ok = SUCCEEDED(d->GetRenderTarget(0, &rt0)) && rt0;
        const HRESULT depth = ExtraHooks::RawGetDepthStencilSurface(d, &ds);
        ok &= SUCCEEDED(depth) || (depth == D3DERR_NOTFOUND && !ds);
        ok &= SUCCEEDED(d->GetPixelShader(&ps)) && SUCCEEDED(d->GetVertexShader(&vs));
        ok &= SUCCEEDED(d->GetVertexDeclaration(&decl)) && SUCCEEDED(d->GetFVF(&fvf));
        ExtraHooks::UseVertexInputSetLast(decl, fvf);
        ok &= SUCCEEDED(d->GetStreamSource(0, &stream0, &stream0Offset, &stream0Stride));
        ok &= SUCCEEDED(d->GetTexture(0, &tex));
        for (int i = 0; i < kSS; i++) ok &= SUCCEEDED(d->GetSamplerState(0, kSamplerStates[i], &ss[i]));
        for (int i = 0; i < kRS; i++) ok &= SUCCEEDED(d->GetRenderState(kRenderStates[i], &rs[i]));
        ok &= SUCCEEDED(d->GetPixelShaderConstantF(0, psConst, kPSConsts));
        ok &= SUCCEEDED(d->GetViewport(&viewport));
        if (ok) dev = d;
        return ok;
    }
    void Restore() {
        dev->SetRenderTarget(0, rt0); // resets the viewport, so it goes first
        ExtraHooks::RawSetDepthStencilSurface(dev, ds);
        dev->SetTexture(0, tex);
        for (int i = 0; i < kSS; i++) dev->SetSamplerState(0, kSamplerStates[i], ss[i]);
        for (int i = 0; i < kRS; i++) dev->SetRenderState(kRenderStates[i], rs[i]);
        dev->SetPixelShaderConstantF(0, psConst, kPSConsts);
        dev->SetPixelShader(ps);
        dev->SetVertexShader(vs);
        if (decl) dev->SetVertexDeclaration(decl);
        else dev->SetFVF(fvf);
        dev->SetStreamSource(0, stream0, stream0Offset, stream0Stride); // DrawPrimitiveUP cleared stream 0
        dev->SetViewport(&viewport);
        dev = nullptr;
    }
    void Release() {
        SafeRelease(rt0); SafeRelease(ds); SafeRelease(ps); SafeRelease(vs); SafeRelease(decl); SafeRelease(stream0); SafeRelease(tex);
    }
};

void DrawPass(IDirect3DDevice9* dev, IDirect3DSurface9* target, IDirect3DBaseTexture9* input, IDirect3DPixelShader9* ps, const float c[kPSConsts][4]) {
    ExtraHooks::RawSetDepthStencilSurface(dev, nullptr); // the depth is sampled, so it must not be bound
    dev->SetRenderTarget(0, target);
    dev->SetVertexShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    for (int i = 0; i < kRS; i++) dev->SetRenderState(kRenderStates[i], FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetTexture(0, input);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetPixelShader(ps);
    dev->SetPixelShaderConstantF(0, &c[0][0], kPSConsts);
    D3DSURFACE_DESC td{};
    target->GetDesc(&td);
    struct V { float x, y, z, rhw, u, v; };
    const float x1 = static_cast<float>(td.Width) - 0.5f, y1 = static_cast<float>(td.Height) - 0.5f;
    const V v[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {x1, -0.5f, 0, 1, 1, 0}, {-0.5f, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V));
}

void MotionEffect(IDirect3DDevice9* dev) {
    g.motionThisFrame = false;
    LogCaps(dev);
    if (!WorldSession::InWorld()) return;
    IDirect3DTexture9* depth = DepthShare::Texture();
    if (!depth || !EnsureShaders(dev)) return;
    IDirect3DSurface9* ds = nullptr; // the scene depth must be the one bound right now (not a reflection or UI pass)
    ExtraHooks::RawGetDepthStencilSurface(dev, &ds);
    const bool sceneDepth = ds && ds == DepthShare::Surface();
    SafeRelease(ds);
    if (!sceneDepth) return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &bb)) || !bb) return;
    D3DSURFACE_DESC bd{};
    bb->GetDesc(&bd);
    SafeRelease(bb);
    if (!EnsureTarget(dev, bd.Width, bd.Height)) return;

    float vp[4][4];
    const bool haveCamera = PostScene::CameraViewProj(vp);
    double D[4][4] = {};
    bool invalid = !haveCamera || !g.havePrev || !GBufferMotion::Reproject(vp, g.prevVp, D);
    if (!invalid && GBufferMotion::IsCut(D)) invalid = true, g.cuts++;
    if (!haveCamera) g.noCamera++;
    const float c[kPSConsts][4] = {
        {float(D[0][0]), float(D[0][1]), float(D[0][2]), float(D[0][3])},
        {float(D[1][0]), float(D[1][1]), float(D[1][2]), float(D[1][3])},
        {float(D[2][0]), float(D[2][1]), float(D[2][2]), float(D[2][3])},
        {float(D[3][0]), float(D[3][1]), float(D[3][2]), float(D[3][3])},
        {invalid ? 1.0f : 0.0f, GBufferMotion::kInvalid, 0, 0}};
    {
        SavedState saved;
        if (!saved.Capture(dev)) return;
        DrawPass(dev, g.motionSurf, depth, g.psMotion, c);
    }
    if (haveCamera) std::memcpy(g.prevVp, vp, sizeof vp);
    g.havePrev = haveCamera;
    g.motionThisFrame = true;
    g.frames++;
    if (!g.loggedRunning) {
        g.loggedRunning = true;
        LOG_INFO(std::format("[GBuffer] Motion pass running: {}x{} G16R16F, view={}", g.width, g.height, g.view == 1 ? "motion" : "off"));
    }
    const unsigned long long now = GetTickCount64();
    if (now - g.lastLog >= 10000) {
        if (g.lastLog) LOG_INFO(std::format("[GBuffer] Motion: frames={} cuts={} noCamera={} view={}", g.frames, g.cuts, g.noCamera, g.view == 1 ? "motion" : "off"));
        g.lastLog = now;
        g.frames = g.cuts = g.noCamera = 0;
    }
}

void ViewEffect(IDirect3DDevice9* dev) {
    if (g.view != 1 || !g.motionThisFrame || !g.psView) return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &bb)) || !bb) return;
    const float c[kPSConsts][4] = {{}, {}, {}, {}, {0, GBufferMotion::kInvalid, 20.0f, 0}}; // 0.025 NDC = full colour
    {
        SavedState saved;
        if (saved.Capture(dev)) DrawPass(dev, bb, g.motion, g.psView, c);
    }
    SafeRelease(bb);
}

void OnPreReset(IDirect3DDevice9*) { ReleaseTarget(); }
void RestartEffects(IDirect3DDevice9*) {
    ReleaseTarget();
    ReleaseShaders();
    g.createFailed = false;
}

} // namespace

class GBufferPatch : public ApexPatch {
  public:
    GBufferPatch() : ApexPatch("GBuffer", nullptr) {
        RegisterIntSetting(&g.view, "view", 0, 0, 1, "Developer view: 0 = off, 1 = camera motion vectors over the scene");
    }

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        DepthShare::Request(true);
        PostScene::WantCamera(true);
        RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
        RenderCallbacks::Add(RenderCallbacks::restartEffects, RestartEffects);
        PostScene::Add(kMotionOrder, MotionEffect);
        PostScene::Add(kViewOrder, ViewEffect);
        g.active = true;
        g.createFailed = false;
        g.loggedRunning = false;
        g.capsDevice = nullptr;
        isEnabled = true;
        LOG_INFO(std::format("[GBuffer] On: motion=camera view={}", g.view == 1 ? "motion" : "off"));
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        PostScene::Remove(ViewEffect);
        PostScene::Remove(MotionEffect);
        RenderCallbacks::Remove(RenderCallbacks::preReset, OnPreReset);
        RenderCallbacks::Remove(RenderCallbacks::restartEffects, RestartEffects);
        ReleaseTarget();
        ReleaseShaders();
        PostScene::WantCamera(false);
        DepthShare::Request(false);
        g.active = false;
        isEnabled = false;
        LOG_INFO("[GBuffer] Off");
        return true;
    }

    // view is read live every frame; never reinstall (that would tear down the depth swap from the wrong thread)
    void Update() override { pendingReinstall = false; }
};

APEX_REGISTER_FEATURE(GBufferPatch, {.displayName = "G-buffer (developer)",
                                     .description = "Developer test of the G-buffer for real-time lighting: camera motion vectors from the scene depth and a "
                                                    "colour-coded view of them. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                                     .category = "Experimental",
                                     .experimental = true,
                                     .enabledByDefault = false,
                                     .supportedVersions = VERSION_ALL,
                                     .technicalDetails = {"Logs the multiple-render-target capabilities and G-buffer formats once per device.",
                                                          "Post-scene pass before Ambient Occlusion: INTZ depth and prevViewProj x inverse(viewProj) (double, CPU) into a G16R16F target."}})
#endif
