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
// - WP2 / WP3 (work package GB-02): albedo + material id (A8R8G8B8) and normal + roughness + motion class (A2B10G10R10)
//   written by the scene's ps_3_0 and ps_2_x copies (ShaderPatches::AddGBufferPs, bound by the scene binder of scene_dither.cpp)
//   as render targets 1 and 2 beside the unchanged back buffer. Needs 3 simultaneous targets, both formats as render
//   targets, no game multisampling, and independent bit depths while the High-precision scene stands in for the back
//   buffer; otherwise one log line and no G-buffer. view = 2 albedo, 3 normal, 4 material id, 5 motion class,
//   6 scene pixels nothing wrote (red). GB-03: every ps_3_0 and ps_2_0 of the package has a row; Night Lighting's
//   replacement shaders take the row of the game shader they were made from.
// Consumers (GB-04): Ambient Occlusion's developer switches gbufferNormals and temporal read this frame's targets and
// motion through GBuffer::Targets / GBuffer::Motion (gbuffer_motion.h). [patches.GBuffer], off by default.
//
// Part of Apex Radiance. Credits: @loinyx

#include "patch_base.h"
#include "apex_version.h"
#include "apex_log.h"
#include "d3d9_extra_hooks.h"
#include "render_callbacks.h"
#include "d3d9_bootstrap.h"
#include "d3d9_hooks.h"
#include "scene_dither.h"
#include "scene_target.h"
#include "depth_share.h"
#include "post_scene.h"
#include "world_session.h"
#include "shader_cache.h"
#include "gbuffer_motion.h"
#include "gbuffer_movers.h"
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <iterator>
#include <string>

namespace {

constexpr int kBoundaryOrder = PostScene::kAmbientOcclusion - 9; // first: the scene's extra targets are unbound
constexpr int kMotionOrder = PostScene::kAmbientOcclusion - 5; // before every effect that could take it
constexpr int kTaaOrder = PostScene::kEdgeSmoothing - 5;        // (GB-05) after Ambient Occlusion's composite, before Edge Smoothing
constexpr int kViewOrder = PostScene::kDepthBlur + 5;          // after the effects, before Picture's Color
constexpr D3DFORMAT kMotionFormat = D3DFMT_A16B16G16R16F;      // rg motion, b = 1: a mover's own vector (GB-05)
constexpr D3DFORMAT kTaaFormat = D3DFMT_A16B16G16R16F;
constexpr float kTaaDepthTolerance = 0.05f; // the history's 1/z against the camera's prediction (relative), as Ambient Occlusion's

const char* kShaderSource = R"HLSL(
sampler2D sIn : register(s0);     // the depth (MotionPS); the motion target or albedo + id (ViewPS)
sampler2D sNormal : register(s1); // normal + roughness + class
sampler2D sDepth : register(s2);
sampler2D sObj : register(s3);    // MotionPS (GB-05): the movers' target, (mv, device depth, 1) where a mover was drawn
float4 cD0 : register(c0); // rows of D = prevViewProj * inverse(viewProj)
float4 cD1 : register(c1);
float4 cD2 : register(c2);
float4 cD3 : register(c3);
float4 cFlags : register(c4); // x = 1: the whole frame is invalid; y = invalid value; z = view scale; w = view
float4 cJit : register(c5);   // MotionPS: xy = this frame's jitter (clip units), z = mover depth match, w = 1: movers' target bound

float4 MotionPS(float2 uv : TEXCOORD0) : COLOR0
{
    if (cFlags.x > 0.5) return float4(cFlags.y, cFlags.y, 0, 0);
    float d = tex2D(sIn, uv).r;
    if (cJit.w > 0.5) { // a mover drawn here and still the visible surface (its depth): its own vector
        float4 o = tex2D(sObj, uv);
        if (o.a > 0.5 && abs(o.b - d) <= cJit.z) return float4(o.xy, 1, 0);
    }
    // the pixel shows the surface point whose unjittered NDC is ndc(uv) - jitter
    float4 p = float4(uv.x * 2 - 1 - cJit.x, 1 - uv.y * 2 - cJit.y, d, 1);
    float4 q = float4(dot(cD0, p), dot(cD1, p), dot(cD2, p), dot(cD3, p));
    if (q.w <= 1e-6) return float4(cFlags.y, cFlags.y, 0, 0);
    return float4(p.xy - q.xy / q.w, 0, 0);
}

float4 ViewPS(float2 uv : TEXCOORD0) : COLOR0
{
    if (cFlags.w < 1.5) { // blue full: a mover's own vector (GB-05)
        float3 mv = tex2D(sIn, uv).rgb;
        if (max(abs(mv.x), abs(mv.y)) > 1000) return float4(1, 0, 1, 1);
        return float4(saturate(0.5 + mv.xy * cFlags.z), mv.z > 0.5 ? 1 : 0.5, 1);
    }
    float4 a = tex2D(sIn, uv);
    float4 n = tex2D(sNormal, uv);
    float id = floor(a.a * 255 + 0.5);
    if (cFlags.w > 5.5) // 6: a scene pixel (depth written) that no G-buffer copy wrote
        return id < 0.5 && tex2D(sDepth, uv).r < 0.99999 ? float4(1, 0, 0, 1) : float4(0.25 + 0.5 * a.rgb, 1);
    if (id < 0.5) return float4(0, 0, 0, 1); // nothing written: sky, or a draw without the outputs
    if (cFlags.w < 2.5) return id > 254.5 ? float4(0.5, 0.5, 0.5, 1) : float4(a.rgb, 1); // albedo (unknown: mid grey)
    if (cFlags.w < 3.5) { // normal: octahedral decode, 0.5 + 0.5 n (no normal written: dark grey)
        if (n.b <= 0) return float4(0.1, 0.1, 0.1, 1);
        float2 f = n.rg * 2 - 1;
        float3 d = float3(f, 1 - abs(f.x) - abs(f.y));
        float t = saturate(-d.z);
        d.xy += float2(d.x >= 0 ? -t : t, d.y >= 0 ? -t : t);
        return float4(0.5 + 0.5 * normalize(d), 1);
    }
    if (cFlags.w < 4.5) // material id: one colour per id, 255 (albedo unknown) magenta
        return id > 254.5 ? float4(1, 0, 1, 1) : float4(0.2 + 0.8 * frac(id * float3(0.618034, 0.754878, 0.569840) + 0.25), 1);
    float c = floor(n.a * 3 + 0.5); // motion class: 0 grey, 1 green, 2 yellow, 3 red
    return c < 0.5 ? float4(0.5, 0.5, 0.5, 1) : c < 1.5 ? float4(0, 1, 0, 1) : c < 2.5 ? float4(1, 1, 0, 1) : float4(1, 0, 0, 1);
}
)HLSL";

// (GB-05) temporal anti-aliasing on the temporal core of GB-04: the scene drawn with a sub-pixel Halton jitter, resolved
// against last frame's resolved colour at the reprojected position (camera and mover motion), rejected by motion
// class 3 and by a history surface (material id, 1/z the camera predicts) none of the 3 x 3 pixels shows, the history
// clamped to the 3 x 3 neighbourhood of this frame's samples and blended by 1 / frames; a pixel without history shows this frame's colour at its unjittered centre (bilinear)
const char* kTaaSource = R"HLSL(
sampler2D sCur : register(s0);  // this frame's scene colour (jittered), bilinear
sampler2D sHist : register(s1); // last frame's resolved colour (rgb), bilinear
sampler2D sAux : register(s2);  // last frame's 1/z, material id / 255, frames (A16B16G16R16F), point
sampler2D sMot : register(s3);  // motion (rg) and the mover flag (b), point
sampler2D sAlb : register(s4);  // G-buffer target 1: material id / 255 (a), point
sampler2D sNrm : register(s5);  // G-buffer target 2: motion class / 3 (a), point
sampler2D sDep : register(s6);  // INTZ scene depth, point
float4 cSize : register(c0);    // xy = 1 / size, zw = size
float4 cJ : register(c1);       // xy = jitter in uv (the unjittered centre's point is drawn at uv + xy), z = 1: history valid, w = most frames
float4 cD3 : register(c2);      // row 3 of the camera's D (q.w = previous view depth / this one)
float4 cZ : register(c3);       // x = A, y = 1 / (near A) (1/z = (A - d) / (near A)), z = 1/z tolerance (relative)
float4 cS : register(c4);       // SharpenPS: x = amount

struct TaaOut { float4 color : COLOR0; float4 aux : COLOR1; };
TaaOut TaaPS(float2 uv : TEXCOORD0)
{
    TaaOut o;
    float3 c = tex2Dlod(sCur, float4(uv, 0, 0)).rgb;
    float3 v = tex2Dlod(sCur, float4(uv + cJ.xy, 0, 0)).rgb; // without history: the unjittered centre
    float d = tex2Dlod(sDep, float4(uv, 0, 0)).r;
    float w = d >= 0.99999 ? 0 : max(cZ.x - d, 1e-7) * cZ.y;
    float id = tex2Dlod(sAlb, float4(uv, 0, 0)).a;
    float cls = floor(tex2Dlod(sNrm, float4(uv, 0, 0)).a * 3 + 0.5);
    float4 m = tex2Dlod(sMot, float4(uv, 0, 0));
    if (m.b > 0.5) cls = 1;
    float2 puv = uv - m.xy * float2(0.5, -0.5);
    float2 pu = uv - cJ.xy; // the drawn point's unjittered position
    float qw = w > 0 ? dot(cD3, float4(pu.x * 2 - 1, 1 - pu.y * 2, d, 1)) : 1;
    float frames = 1;
    [branch] if (cJ.z > 0.5 && cls < 2.5 && max(abs(m.x), abs(m.y)) < 1000 && all(puv >= 0) && all(puv <= 1) && qw > 1e-6) {
        float4 a = tex2Dlod(sAux, float4(puv, 0, 0));
        // the history's surface (id, 1/z the camera predicts) must be one of the 3 x 3 surfaces this frame shows: at an
        // edge the jitter alternates between the two sides, which keeps their history; a surface that left the
        // neighbourhood (disocclusion, a cut) starts a new one
        float3 lo = c, hi = c;
        bool seen = false;
        [unroll] for (int y = -1; y <= 1; y++)
            [unroll] for (int x = -1; x <= 1; x++) {
                float2 nuv = uv + float2(x, y) * cSize.xy;
                float3 n = tex2Dlod(sCur, float4(nuv, 0, 0)).rgb;
                lo = min(lo, n);
                hi = max(hi, n);
                float nd = tex2Dlod(sDep, float4(nuv, 0, 0)).r;
                float nw = nd >= 0.99999 ? 0 : max(cZ.x - nd, 1e-7) * cZ.y;
                bool sameZ = nw > 0 ? a.r > 0 && abs(nw / (qw * a.r) - 1) < cZ.z : a.r == 0;
                seen = seen || (sameZ && abs(tex2Dlod(sAlb, float4(nuv, 0, 0)).a - a.g) < 0.5 / 255);
            }
        if (a.b > 0 && seen) {
            frames = min(a.b + 1, cJ.w);
            v = lerp(clamp(tex2Dlod(sHist, float4(puv, 0, 0)).rgb, lo, hi), c, 1 / frames);
        }
    }
    o.color = float4(v, 1);
    o.aux = float4(w, id, frames, 0);
    return o;
}

// the resolved colour into the back buffer (rgb only: its alpha is the game's), with a light sharpen held to the cross
float4 SharpenPS(float2 uv : TEXCOORD0) : COLOR0
{
    float3 c = tex2Dlod(sCur, float4(uv, 0, 0)).rgb;
    float3 n = tex2Dlod(sCur, float4(uv - float2(0, cSize.y), 0, 0)).rgb, s = tex2Dlod(sCur, float4(uv + float2(0, cSize.y), 0, 0)).rgb;
    float3 e = tex2Dlod(sCur, float4(uv + float2(cSize.x, 0), 0, 0)).rgb, w = tex2Dlod(sCur, float4(uv - float2(cSize.x, 0), 0, 0)).rgb;
    float3 lo = min(c, min(min(n, s), min(e, w))), hi = max(c, max(max(n, s), max(e, w)));
    return float4(clamp(c + cS.x * (4 * c - n - s - e - w) * 0.25, lo, hi), 1);
}
)HLSL";

ShaderCache::Id AddShader(const char* tag, const char* entry, const char* source = kShaderSource) {
    ShaderCache::Desc d;
    d.tag = tag;
    d.source = source;
    d.sourceName = "gbuffer.hlsl";
    d.entry = entry;
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    d.priority = 1;
    return ShaderCache::Add(std::move(d));
}
const ShaderCache::Id kMotionPsId = AddShader("GBuffer MotionPS", "MotionPS");
const ShaderCache::Id kViewPsId = AddShader("GBuffer ViewPS", "ViewPS");
const ShaderCache::Id kTaaPsId = AddShader("GBuffer TaaPS", "TaaPS", kTaaSource);
const ShaderCache::Id kSharpenPsId = AddShader("GBuffer SharpenPS", "SharpenPS", kTaaSource);

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
    float dRow3[4] = {};          // row 3 of this frame's D (consumers' depth test), 0 when invalid
    bool gbWritten = false;       // the scene bound the targets this frame (consumers read them only then)
    // the G-buffer targets (render targets 1 and 2 of the scene)
    IDirect3DTexture9* gbTex[2] = {};
    IDirect3DSurface9* gbSurf[2] = {};
    UINT gbWidth = 0, gbHeight = 0;
    bool gbOff = false;      // refused for this device (logged once)
    bool gbLogged = false;
    bool mrt3 = false, rtFormats = false, independentBitDepths = false; // from the capability line
    SceneBinder::GBufferCounts scene; // summed for the 10 s line
    float prevVp[4][4] = {};
    bool havePrev = false;
    bool loggedRunning = false;
    // counters for the 10 s line
    unsigned frames = 0, cuts = 0, noCamera = 0;
    unsigned long long lastLog = 0, sceneLog = 0;
    // (GB-05) movers and temporal AA: settings (read when the G-buffer is switched on) and what was installed
    bool movers = false, taa = false;
    float taaSharpen = 0.25f;
    int taaFrames = 16;
    bool moversOn = false, taaOn = false;
    GBufferMotion::JitterClock clock; // the jitter set for the scene being drawn (Present -> boundary)
    float jitter[2] = {};         // this frame's scene jitter (clip units), for the boundary's passes
    bool jitterThisFrame = false;
    unsigned long long presents = 0, taaRanAt = 0; // Present count; the count of the frame the TAA history was written in
    IDirect3DPixelShader9* psTaa = nullptr;
    IDirect3DPixelShader9* psSharpen = nullptr;
    bool taaShadersTried = false, taaFailed = false, taaLogged = false;
    IDirect3DTexture9* taaCur = nullptr;      // copy of this frame's scene colour
    IDirect3DSurface9* taaCurSurf = nullptr;
    IDirect3DTexture9* taaColor[2] = {};      // resolved colour (rgb, A16B16G16R16F)
    IDirect3DTexture9* taaAux[2] = {};        // 1/z, material id / 255, frames
    IDirect3DSurface9* taaColorSurf[2] = {};
    IDirect3DSurface9* taaAuxSurf[2] = {};
    int taaPing = 0;
    UINT taaW = 0, taaH = 0;
    unsigned taaRuns = 0, taaNoInput = 0, moverFrames = 0;
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
    g.mrt3 = caps.NumSimultaneousRTs >= 3;
    g.rtFormats = formats.find("A8R8G8B8=rt") != std::string::npos && formats.find("A2B10G10R10=rt") != std::string::npos;
    g.independentBitDepths = (misc & D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS) != 0;
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

const char* ViewName(int view) {
    static const char* const names[] = {"off", "motion", "albedo", "normal", "material", "class", "unwritten"};
    return names[view >= 0 && view <= 6 ? view : 0];
}

void ReleaseGBufferTargets(IDirect3DDevice9* dev) {
    g.gbWritten = false;
    SceneBinder::SetGBufferTargets(dev, nullptr, nullptr); // unbound first
    for (int i = 0; i < 2; i++) {
        SafeRelease(g.gbSurf[i]);
        SafeRelease(g.gbTex[i]);
    }
    g.gbWidth = g.gbHeight = 0;
}

// The scene's extra targets, the size of the back buffer; handed to the scene binder. Off with one log line when the
// device or the frame cannot have them
void EnsureGBufferTargets(IDirect3DDevice9* dev) {
    if (g.gbOff) return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC bd{};
    bb->GetDesc(&bd);
    SafeRelease(bb);
    const bool highPrecision = SceneTarget::CurrentState() == SceneTarget::State::Active; // a 16-bit target stands in for the back buffer
    const char* why = !g.mrt3 ? "fewer than 3 simultaneous render targets"
                      : !g.rtFormats ? "A8R8G8B8 or A2B10G10R10 is not a render target format"
                      : bd.MultiSampleType != D3DMULTISAMPLE_NONE ? "the game's anti-aliasing is on (multisampled back buffer)"
                      : highPrecision && !g.independentBitDepths ? "the High-precision scene needs independent bit depths" : nullptr;
    if (why) {
        if (g.mrt3 && g.rtFormats) { // may change with a setting: try again later
            if (g.gbTex[0]) ReleaseGBufferTargets(dev);
        } else
            g.gbOff = true;
        if (!g.gbLogged) LOG_WARNING(std::format("[GBuffer] Targets off: {}", why));
        g.gbLogged = true;
        return;
    }
    if (g.gbTex[0] && g.gbWidth == bd.Width && g.gbHeight == bd.Height) return;
    ReleaseGBufferTargets(dev);
    const D3DFORMAT formats[2] = {D3DFMT_A8R8G8B8, D3DFMT_A2B10G10R10};
    for (int i = 0; i < 2; i++)
        if (FAILED(dev->CreateTexture(bd.Width, bd.Height, 1, D3DUSAGE_RENDERTARGET, formats[i], D3DPOOL_DEFAULT, &g.gbTex[i], nullptr)) || !g.gbTex[i] ||
            FAILED(g.gbTex[i]->GetSurfaceLevel(0, &g.gbSurf[i])) || !g.gbSurf[i]) {
            ReleaseGBufferTargets(dev);
            g.gbOff = true;
            LOG_ERROR(std::format("[GBuffer] Could not create the {}x{} G-buffer targets; G-buffer off", bd.Width, bd.Height));
            return;
        }
    g.gbWidth = bd.Width;
    g.gbHeight = bd.Height;
    g.gbLogged = false;
    SceneBinder::SetGBufferTargets(dev, g.gbSurf[0], g.gbSurf[1]);
    LOG_INFO(std::format("[GBuffer] Targets: {}x{} A8R8G8B8 albedo + id, A2B10G10R10 normal + roughness + class", bd.Width, bd.Height));
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
constexpr UINT kPSConsts = 6; // c0..c5
constexpr DWORD kTextures = 7; // s0..s6 (the temporal AA reads seven)

struct SavedState {
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9* rt0 = nullptr;
    IDirect3DSurface9* rt1 = nullptr; // (the temporal AA writes two targets)
    IDirect3DSurface9* ds = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    DWORD fvf = 0;
    IDirect3DVertexBuffer9* stream0 = nullptr;
    UINT stream0Offset = 0, stream0Stride = 0;
    IDirect3DBaseTexture9* tex[kTextures] = {};
    DWORD rs[kRS] = {};
    DWORD ss[kTextures][kSS] = {};
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
        if (FAILED(d->GetRenderTarget(1, &rt1))) rt1 = nullptr;
        const HRESULT depth = ExtraHooks::RawGetDepthStencilSurface(d, &ds);
        ok &= SUCCEEDED(depth) || (depth == D3DERR_NOTFOUND && !ds);
        ok &= SUCCEEDED(d->GetPixelShader(&ps)) && SUCCEEDED(d->GetVertexShader(&vs));
        ok &= SUCCEEDED(d->GetVertexDeclaration(&decl)) && SUCCEEDED(d->GetFVF(&fvf));
        ExtraHooks::UseVertexInputSetLast(decl, fvf);
        ok &= SUCCEEDED(d->GetStreamSource(0, &stream0, &stream0Offset, &stream0Stride));
        for (DWORD t = 0; t < kTextures; t++) {
            ok &= SUCCEEDED(d->GetTexture(t, &tex[t]));
            for (int i = 0; i < kSS; i++) ok &= SUCCEEDED(d->GetSamplerState(t, kSamplerStates[i], &ss[t][i]));
        }
        for (int i = 0; i < kRS; i++) ok &= SUCCEEDED(d->GetRenderState(kRenderStates[i], &rs[i]));
        ok &= SUCCEEDED(d->GetPixelShaderConstantF(0, psConst, kPSConsts));
        ok &= SUCCEEDED(d->GetViewport(&viewport));
        if (ok) dev = d;
        return ok;
    }
    void Restore() {
        dev->SetRenderTarget(0, rt0); // resets the viewport, so it goes first
        dev->SetRenderTarget(1, rt1);
        ExtraHooks::RawSetDepthStencilSurface(dev, ds);
        for (DWORD t = 0; t < kTextures; t++) {
            dev->SetTexture(t, tex[t]);
            for (int i = 0; i < kSS; i++) dev->SetSamplerState(t, kSamplerStates[i], ss[t][i]);
        }
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
        SafeRelease(rt0); SafeRelease(rt1); SafeRelease(ds); SafeRelease(ps); SafeRelease(vs); SafeRelease(decl); SafeRelease(stream0);
        for (auto& t : tex) SafeRelease(t);
    }
};

// target1: a second target (or null); linear: a bit per input sampled bilinear; writeMask: target 0's channels
void DrawPass(IDirect3DDevice9* dev, IDirect3DSurface9* target, IDirect3DBaseTexture9* const input[kTextures], IDirect3DPixelShader9* ps, const float c[kPSConsts][4],
              IDirect3DSurface9* target1 = nullptr, unsigned linear = 0, DWORD writeMask = 0xF) {
    ExtraHooks::RawSetDepthStencilSurface(dev, nullptr); // the depth is sampled, so it must not be bound
    dev->SetRenderTarget(0, target);
    dev->SetRenderTarget(1, target1);
    dev->SetVertexShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    for (int i = 0; i < kRS; i++) dev->SetRenderState(kRenderStates[i], FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, writeMask);
    for (DWORD t = 0; t < kTextures; t++) {
        dev->SetTexture(t, input[t]);
        const DWORD filter = (linear >> t) & 1 ? D3DTEXF_LINEAR : D3DTEXF_POINT;
        dev->SetSamplerState(t, D3DSAMP_MINFILTER, filter);
        dev->SetSamplerState(t, D3DSAMP_MAGFILTER, filter);
        dev->SetSamplerState(t, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(t, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(t, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(t, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    dev->SetPixelShader(ps);
    dev->SetPixelShaderConstantF(0, &c[0][0], kPSConsts);
    D3DSURFACE_DESC td{};
    target->GetDesc(&td);
    struct V { float x, y, z, rhw, u, v; };
    const float x1 = static_cast<float>(td.Width) - 0.5f, y1 = static_cast<float>(td.Height) - 0.5f;
    const V v[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {x1, -0.5f, 0, 1, 1, 0}, {-0.5f, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V));
}

// First at every scene boundary, with any depth: the scene's extra targets are unbound before an effect reads them or
// draws over the back buffer, and made (or made again in a new size) for the next scene draws
void BoundaryEffect(IDirect3DDevice9* dev) {
    LogCaps(dev);
    // (GB-05) the scene is drawn: what follows (effects, the UI) is not moved; this frame's passes un-jitter by g.jitter
    g.jitterThisFrame = g.clock.Boundary(g.jitter);
    if (g.jitterThisFrame) SceneBinder::SetFrameJitter(false, 0, 0);
    SceneBinder::GBufferUnbind(dev);
    EnsureGBufferTargets(dev);
    const SceneBinder::GBufferCounts c = SceneBinder::TakeGBufferCounts();
    g.gbWritten = c.binds > 0 && g.gbTex[0];
    g.scene.frames += c.frames, g.scene.patched += c.patched, g.scene.unknown += c.unknown, g.scene.ps2 += c.ps2, g.scene.replaced += c.replaced, g.scene.blended += c.blended, g.scene.masked += c.masked;
    g.scene.binds += c.binds, g.scene.unbinds += c.unbinds, g.scene.bindFailed += c.bindFailed, g.scene.maskToggles += c.maskToggles;
    const unsigned long long now = GetTickCount64();
    if (now - g.sceneLog < 10000) return;
    if (g.sceneLog && g.gbTex[0])
        LOG_INFO(std::format("[GBuffer] Scene: frames={} patched={} unknown={} ps2={} replaced={} blended={} masked={} binds={} unbinds={} bindFailed={} maskToggles={} view={}",
                             g.scene.frames, g.scene.patched, g.scene.unknown, g.scene.ps2, g.scene.replaced, g.scene.blended, g.scene.masked, g.scene.binds, g.scene.unbinds,
                             g.scene.bindFailed, g.scene.maskToggles, ViewName(g.view)));
    g.sceneLog = now;
    g.scene = {};
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
    float c[kPSConsts][4] = {
        {float(D[0][0]), float(D[0][1]), float(D[0][2]), float(D[0][3])},
        {float(D[1][0]), float(D[1][1]), float(D[1][2]), float(D[1][3])},
        {float(D[2][0]), float(D[2][1]), float(D[2][2]), float(D[2][3])},
        {float(D[3][0]), float(D[3][1]), float(D[3][2]), float(D[3][3])},
        {invalid ? 1.0f : 0.0f, GBufferMotion::kInvalid, 0, 0},
        {g.jitter[0], g.jitter[1], GBufferMovers::kDepthMatch, 0}};
    IDirect3DTexture9* movers = GBufferMovers::Target();
    if (movers) c[5][3] = 1.0f, g.moverFrames++;
    {
        SavedState saved;
        if (!saved.Capture(dev)) return;
        IDirect3DBaseTexture9* const in[kTextures] = {depth, nullptr, nullptr, movers};
        DrawPass(dev, g.motionSurf, in, g.psMotion, c);
    }
    for (int k = 0; k < 4; k++) g.dRow3[k] = invalid ? 0.0f : static_cast<float>(D[3][k]);
    if (haveCamera) std::memcpy(g.prevVp, vp, sizeof vp);
    g.havePrev = haveCamera;
    g.motionThisFrame = true;
    g.frames++;
    if (!g.loggedRunning) {
        g.loggedRunning = true;
        LOG_INFO(std::format("[GBuffer] Motion pass running: {}x{} A16B16G16R16F, view={}", g.width, g.height, ViewName(g.view)));
    }
    const unsigned long long now = GetTickCount64();
    if (now - g.lastLog >= 10000) {
        if (g.lastLog) LOG_INFO(std::format("[GBuffer] Motion: frames={} cuts={} noCamera={} view={}", g.frames, g.cuts, g.noCamera, ViewName(g.view)));
        if (g.lastLog && g.moversOn) {
            const GBufferMovers::Counts m = GBufferMovers::TakeCounts();
            LOG_INFO(std::format("[GBuffer] Movers: draws={} noSlice={} firstSeen={} still={} replayed={} noRoom={} capped={} frames={}", m.draws, m.noSlice,
                                 m.firstSeen, m.still, m.replayed, m.noRoom, m.capped, g.moverFrames));
        }
        if (g.lastLog && g.taaOn)
            LOG_INFO(std::format("[GBuffer] TAA: resolved={} noInput={} phases={} sharpen={:.2f}", g.taaRuns, g.taaNoInput, g.taaFrames, g.taaSharpen));
        g.lastLog = now;
        g.frames = g.cuts = g.noCamera = 0;
        g.moverFrames = g.taaRuns = g.taaNoInput = 0;
    }
}

void ViewEffect(IDirect3DDevice9* dev) {
    if (g.view < 1 || g.view > 6 || !EnsureShaders(dev)) return;
    if (g.view == 1 ? !g.motionThisFrame : !g.gbTex[0]) return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &bb)) || !bb) return;
    const float c[kPSConsts][4] = {{}, {}, {}, {}, {0, GBufferMotion::kInvalid, 20.0f, static_cast<float>(g.view)}, {}}; // 0.025 NDC = full colour
    IDirect3DBaseTexture9* const in[kTextures] = {g.view == 1 ? g.motion : g.gbTex[0], g.gbTex[1], g.view == 6 ? DepthShare::Texture() : nullptr};
    {
        SavedState saved;
        if (saved.Capture(dev)) DrawPass(dev, bb, in, g.psView, c);
    }
    SafeRelease(bb);
}


// ---- (GB-05) temporal anti-aliasing ----
void ReleaseTaa() {
    SafeRelease(g.taaCurSurf);
    SafeRelease(g.taaCur);
    for (int i = 0; i < 2; i++) SafeRelease(g.taaColorSurf[i]), SafeRelease(g.taaColor[i]), SafeRelease(g.taaAuxSurf[i]), SafeRelease(g.taaAux[i]);
    g.taaW = g.taaH = 0;
    g.taaRanAt = 0;
}

bool EnsureTaa(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT colorFormat) {
    if (!g.psTaa && !g.taaShadersTried) {
        g.taaShadersTried = true;
        std::string msg;
        if (ShaderCache::CreatePixelShader(dev, kTaaPsId, &g.psTaa, &msg) != ShaderCache::Result::Ok ||
            ShaderCache::CreatePixelShader(dev, kSharpenPsId, &g.psSharpen, &msg) != ShaderCache::Result::Ok) {
            LOG_ERROR("[GBuffer] TAA shaders unavailable: " + msg);
            g.taaFailed = true;
        }
    }
    if (g.taaFailed || !g.psTaa || !g.psSharpen) return false;
    if (g.taaCur && g.taaW == w && g.taaH == h) return true;
    ReleaseTaa();
    bool ok = SUCCEEDED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, colorFormat, D3DPOOL_DEFAULT, &g.taaCur, nullptr)) && g.taaCur &&
              SUCCEEDED(g.taaCur->GetSurfaceLevel(0, &g.taaCurSurf));
    for (int i = 0; i < 2 && ok; i++)
        ok = SUCCEEDED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, kTaaFormat, D3DPOOL_DEFAULT, &g.taaColor[i], nullptr)) && g.taaColor[i] &&
             SUCCEEDED(g.taaColor[i]->GetSurfaceLevel(0, &g.taaColorSurf[i])) &&
             SUCCEEDED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, kTaaFormat, D3DPOOL_DEFAULT, &g.taaAux[i], nullptr)) && g.taaAux[i] &&
             SUCCEEDED(g.taaAux[i]->GetSurfaceLevel(0, &g.taaAuxSurf[i]));
    if (!ok) {
        ReleaseTaa();
        g.taaFailed = true;
        LOG_ERROR(std::format("[GBuffer] Could not create the {}x{} TAA targets; TAA off", w, h));
        return false;
    }
    g.taaW = w, g.taaH = h;
    LOG_INFO(std::format("[GBuffer] TAA running: {}x{}, {} jitter phases, sharpen {:.2f}", w, h, g.taaFrames, g.taaSharpen));
    return true;
}

// After Ambient Occlusion's composite: this frame's jittered scene and last frame's resolved colour -> a new resolved
// colour (history), sharpened into the back buffer (rgb; the alpha stays the game's)
void TaaEffect(IDirect3DDevice9* dev) {
    if (!g.taaOn) return;
    IDirect3DTexture9* gb[2] = {};
    float dRow3[4] = {};
    IDirect3DTexture9* motion = GBuffer::Motion(dRow3);
    IDirect3DTexture9* depth = DepthShare::Texture();
    if (!motion || !GBuffer::Targets(gb) || !depth) { g.taaNoInput++; return; }
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &bb)) || !bb) return;
    D3DSURFACE_DESC bd{};
    bb->GetDesc(&bd);
    const D3DFORMAT colorFormat = bd.Format == D3DFMT_X8R8G8B8 ? D3DFMT_A8R8G8B8 : bd.Format;
    if (!EnsureTaa(dev, bd.Width, bd.Height, colorFormat) || FAILED(dev->StretchRect(bb, nullptr, g.taaCurSurf, nullptr, D3DTEXF_NONE))) {
        SafeRelease(bb);
        return;
    }
    const float W = static_cast<float>(bd.Width), H = static_cast<float>(bd.Height);
    const float nearZ = PostScene::CameraNear() > 0.0f ? PostScene::CameraNear() : 0.25f, A = PostScene::CameraDepthA();
    const bool valid = g.taaRanAt && g.taaRanAt + 1 == g.presents; // written last frame, same targets
    const int cur = g.taaPing, next = cur ^ 1;
    const float c[kPSConsts][4] = {{1.0f / W, 1.0f / H, W, H},
                                   {0.5f * g.jitter[0], -0.5f * g.jitter[1], valid ? 1.0f : 0.0f, static_cast<float>(std::clamp(g.taaFrames, 4, 16))},
                                   {dRow3[0], dRow3[1], dRow3[2], dRow3[3]},
                                   {A, 1.0f / (nearZ * A), kTaaDepthTolerance, 0},
                                   {std::clamp(g.taaSharpen, 0.0f, 1.0f), 0, 0, 0},
                                   {}};
    {
        SavedState saved;
        if (saved.Capture(dev)) {
            IDirect3DBaseTexture9* const in[kTextures] = {g.taaCur, g.taaColor[cur], g.taaAux[cur], motion, gb[0], gb[1], depth};
            DrawPass(dev, g.taaColorSurf[next], in, g.psTaa, c, g.taaAuxSurf[next], 0x3 /* colour and history bilinear */);
            IDirect3DBaseTexture9* const out[kTextures] = {g.taaColor[next]};
            DrawPass(dev, bb, out, g.psSharpen, c, nullptr, 0, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        }
    }
    SafeRelease(bb);
    g.taaPing = next;
    g.taaRanAt = g.presents;
    g.taaRuns++;
}

// Present: the next frame's sub-pixel offset (Halton 2, 3 over taaFrames phases), set for its scene draws until the boundary
void OnPresent(IDirect3DDevice9* dev) {
    g.presents++;
    if (!g.taaOn || !g.gbTex[0]) return;
    (void)dev;
    g.clock.Present(static_cast<unsigned>(g.taaFrames), g.gbWidth, g.gbHeight);
    SceneBinder::SetFrameJitter(true, g.clock.offset[0], g.clock.offset[1]);
}

void OnPreReset(IDirect3DDevice9* dev) {
    ReleaseTarget();
    ReleaseGBufferTargets(dev);
    ReleaseTaa();
}
void RestartEffects(IDirect3DDevice9* dev) {
    ReleaseTarget();
    ReleaseGBufferTargets(dev);
    ReleaseTaa();
    SafeRelease(g.psTaa);
    SafeRelease(g.psSharpen);
    g.taaShadersTried = g.taaFailed = false;
    ReleaseShaders();
    g.createFailed = false;
    g.gbOff = g.gbLogged = false;
}

} // namespace

bool GBuffer::Targets(IDirect3DTexture9* out[2]) {
    if (!g.active || !g.gbWritten) return false;
    out[0] = g.gbTex[0], out[1] = g.gbTex[1];
    return true;
}

bool GBuffer::Jitter(float clip[2]) {
    clip[0] = g.active && g.jitterThisFrame ? g.jitter[0] : 0.0f;
    clip[1] = g.active && g.jitterThisFrame ? g.jitter[1] : 0.0f;
    return g.active && g.jitterThisFrame;
}

IDirect3DTexture9* GBuffer::Motion(float dRow3[4]) {
    if (!g.active || !g.motionThisFrame) return nullptr;
    std::memcpy(dRow3, g.dRow3, sizeof g.dRow3);
    return g.motion;
}

class GBufferPatch : public ApexPatch {
  public:
    GBufferPatch() : ApexPatch("GBuffer", nullptr) {
        RegisterIntSetting(&g.view, "view", 0, 0, 6,
                           "Developer view over the scene: 0 = off, 1 = camera motion vectors, 2 = albedo, 3 = normal, 4 = material id, 5 = motion class, "
                           "6 = scene pixels nothing wrote");
        // (GB-05) read when the G-buffer is switched on
        RegisterBoolSetting(&g.movers, "movers", false, "Developer: motion vectors of moving objects and Sims (a replay of each draw whose position constants changed)");
        RegisterBoolSetting(&g.taa, "taa", false, "Developer: temporal anti-aliasing (sub-pixel jitter of the 3D scene, resolved with the motion vectors)");
        RegisterFloatSetting(&g.taaSharpen, "taaSharpen", SettingWidget::Slider, 0.25f, 0.0f, 1.0f, "Developer: sharpening after the temporal AA resolve");
        RegisterIntSetting(&g.taaFrames, "taaFrames", 16, 4, 16, "Developer: jitter phases of the temporal AA (Halton 2, 3) and the most frames it blends");
    }

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        DepthShare::Request(true);
        PostScene::WantCamera(true);
        RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
        RenderCallbacks::Add(RenderCallbacks::restartEffects, RestartEffects);
        PostScene::Add(kBoundaryOrder, BoundaryEffect, false);
        PostScene::Add(kMotionOrder, MotionEffect);
        SceneBinder::AcquireGBuffer();
        PostScene::Add(kViewOrder, ViewEffect);
        g.moversOn = g.movers;
        if (g.moversOn) GBufferMovers::Acquire();
        g.taaOn = g.taa;
        g.clock.armed = g.jitterThisFrame = false;
        g.taaRanAt = 0;
        if (g.taaOn) {
            SceneBinder::AcquireJitter();
            PostScene::Add(kTaaOrder, TaaEffect);
        }
        D3D9Hooks::RegisterPresent("GBuffer", [](D3D9Hooks::DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
            OnPresent(ctx.device);
            return D3D9Hooks::HookAction::Continue;
        });
        g.active = true;
        g.createFailed = false;
        g.loggedRunning = false;
        g.capsDevice = nullptr;
        g.gbOff = g.gbLogged = false;
        isEnabled = true;
        LOG_INFO(std::format("[GBuffer] On: motion={} view={}{}", g.moversOn ? "camera+movers" : "camera", ViewName(g.view), g.taaOn ? " taa=on" : ""));
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        PostScene::Remove(ViewEffect);
        D3D9Hooks::UnregisterAll("GBuffer");
        if (g.taaOn) {
            PostScene::Remove(TaaEffect);
            SceneBinder::ReleaseJitter();
        }
        if (g.moversOn) GBufferMovers::Release(ApexD3D::Device());
        g.moversOn = g.taaOn = false;
        g.clock.armed = g.jitterThisFrame = false;
        ReleaseTaa();
        SafeRelease(g.psTaa);
        SafeRelease(g.psSharpen);
        g.taaShadersTried = g.taaFailed = false;
        PostScene::Remove(MotionEffect);
        PostScene::Remove(BoundaryEffect);
        ReleaseGBufferTargets(ApexD3D::Device());
        SceneBinder::ReleaseGBuffer(ApexD3D::Device());
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
                                     .description = "Developer test of the G-buffer for real-time lighting: camera motion vectors from the scene depth, "
                                                    "albedo, normal, material and motion class written by the scene's shaders, and colour-coded views. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                                     .category = "Experimental",
                                     .experimental = true,
                                     .enabledByDefault = false,
                                     .supportedVersions = VERSION_ALL,
                                     .technicalDetails = {"Logs the multiple-render-target capabilities and G-buffer formats once per device.",
                                                          "The scene's ps_3_0 and ps_2_x copies also write albedo + material id and normal + roughness + motion class to render targets 1 and 2.",
                                                          "Post-scene pass before Ambient Occlusion: INTZ depth and prevViewProj x inverse(viewProj) (double, CPU) into a G16R16F target."}})
#endif
