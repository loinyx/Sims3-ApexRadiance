// Depth Blur
// Blurs the scene behind what the camera looks at, from the scene depth buffer, applied right after the game finishes
// the 3D scene and before it draws any UI, so pie menus, tooltips and panels stay sharp.
//
// How it works (docs/features/depth-blur.md):
//  - The game renders the 3D scene straight into the backbuffer using the auto depth-stencil, then the bloom
//    composite and the UI, which are the first backbuffer draws with ZENABLE = FALSE after the scene.
//  - The auto depth-stencil is swapped for an INTZ depth texture (same size, readable by shaders) through the
//    SetDepthStencilSurface / GetDepthStencilSurface detours; the game never sees the swap.
//  - Right before the first ZENABLE = FALSE backbuffer draw of each frame (PostScene, after edge smoothing):
//      1. StretchRect the backbuffer to a full-res copy (no filtering)
//      2. Auto focus only: a 1x1 pass takes the 25th percentile of 16 depths in a small central window (sky ignored)
//         and eases the stored focus (A - d) toward it (two 1x1 float targets, ping-pong; no CPU readback)
//      3. Prep: each half-res texel reads its 2x2 full-res block (4 colours, 4 point depths): blur amount = the
//         smallest of the 4, colour = the pixels whose blur matches it, in linear light (float targets)
//      4. Separable gather (H then V) whose tap spread follows the pixel's own blur radius; taps are accepted only
//         where their own blur reaches the pixel (scatter-as-gather), so sharp pixels never bleed into blurred ones
//      5. Composite: bilateral upsample (4 texels, bilinear x blur-amount similarity) alpha-blended over the
//         backbuffer; pixels with no blur keep the original exactly
//  - Only the handful of states the passes touch are saved/restored (no full state block, which is CPU heavy).
// Requires the game's Edge Smoothing to be off (multisampled depth cannot be read in D3D9).

#include "patch_base.h"
#include "apex_version.h"
#include "memory_patch.h"
#include "apex_log.h"
#include "d3d9_bootstrap.h"
#include "d3d9_hooks.h"
#include "render_callbacks.h"
#include "depth_share.h"
#include "d3d9_extra_hooks.h"
#include "post_scene.h"
#include "world_session.h"
#include "map_view.h"
#include "shader_cache.h"
#include "imgui.h"
#include "ui/violet_theme.h"
#include "ui/widgets.h"
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include "build_flavor.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace {

constexpr const char* kHookName = "DepthBlur";
constexpr D3DFORMAT kFmtINTZ = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
constexpr int kRetryFrames = 120;
constexpr float kMapFadeSeconds = 0.3f;
// With the blur on as soon as the world gate opened (WorldSession::Settled, 3 s), the whole screen was blurred for about a
// second at the end of a load, before the world showed (user report, 2026-10-05; the cause, unfinished first frames, is
// inferred, not measured). So the blur waits this long after the gate opens and then fades in.
constexpr unsigned long long kWorldHoldMs = 2000;
constexpr float kWorldFadeInSeconds = 1.0f;
// The game's projection: d = A - near * A / z (LightProbe-m80; docs/engine/camera-and-map-view.md)
constexpr float kDepthA = 1.00008f;
// Only for the Developer read-out in metres: near changes with zoom (0.2 - 0.3), so the value is approximate
constexpr float kReadoutNear = 0.25f;
// Auto focus window: a square of this fraction of the screen height, centred
constexpr float kFocusWindow = 0.05f;
// Highlight weight in the gather: w *= 1 + kLampGain * k * max(luma - 0.8, 0) (at most 1.4x for pure white)
constexpr float kLampGain = 2.0f;
// Taps per side of the separable gather, per quality (Low, Medium, High, Ultra)
constexpr int kQualityTaps[4] = {4, 6, 8, 12};
constexpr const char* kQualityTapsText[4] = {"4", "6", "8", "12"};
// "Sharp area" presets: blur starts at S x the focus distance and is full at T x (thin-lens ratio z_f / z)
struct SharpArea {
    float start, full;
};
constexpr SharpArea kSharpAreas[3] = {{1.5f, 4.0f}, {2.0f, 6.0f}, {3.0f, 10.0f}};

const char* kShaderSource = R"HLSL(
#ifndef TAPS
#define TAPS 8
#endif
sampler2D sColor : register(s0);
sampler2D sBlur  : register(s1);
sampler2D sDepth : register(s2);
sampler2D sFocus : register(s3);
float4 cParams : register(c0); // x = start, y = range, w = far plane (Fixed focus); z = strength x (1 - map fade)
float4 cHalf   : register(c1); // xy = half-res size, zw = 1 / half-res size
float4 cDir    : register(c2); // xy = gather direction, z = max blur radius (half-res pixels), w = lamp weight
float4 cFlags  : register(c3); // x = blur sky, y = debug view, z = linear light, w = auto focus
float4 cFull   : register(c4); // xy = full-res size, zw = 1 / full-res size
float4 cFocus  : register(c5); // x = c0, y = 1 / (c1 - c0) (sharp area), z = A
float4 cEase   : register(c6); // x = ease 1 - exp(-dt / tau), y = 1: ignore the previous focus, zw = focus window half size (uv)

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

// Stored focus: A - d_f (> 0), or <= 0 while unknown (nothing seen yet)
float FocusValue()
{
    return tex2Dlod(sFocus, float4(0.5, 0.5, 0, 0)).r;
}

// Blur amount 0..1 of one depth sample, before strength
float BlurAmount(float d, float focus)
{
    // Fixed: the original heuristic curve (not metres)
    float lin = d / (cParams.w - d * (cParams.w - 1.0));
    float fixedK = saturate((lin - cParams.x) / max(cParams.y, 0.0001));
    // Auto: thin-lens ratio r = (A - d) / (A - d_f) = z_f / z (near cancels); c = 1 - r is 0 at the focus, 1 at infinity,
    // < 0 nearer than the focus (kept sharp: no near-field blur)
    float c = 1.0 - (cFocus.z - d) / max(focus, 1e-7);
    float autoK = focus > 0.0 ? saturate((c - cFocus.x) * cFocus.y) : 0.0;
    float k = cFlags.w > 0.5 ? autoK : fixedK;
    return d >= 0.99999 ? cFlags.x : k;
}

float3 ToLinear(float3 c)   { return cFlags.z > 0.5 ? pow(max(c, 1e-7), 2.2) : c; }
float3 FromLinear(float3 c) { return cFlags.z > 0.5 ? pow(max(c, 1e-7), 1.0 / 2.2) : c; }

// ---- Auto focus (1x1 target) ----

// One depth of the window, snapped to a full-res texel centre; sky = 2 (never counts)
float FocusSample(float2 o)
{
    float2 p = 0.5 + o * cEase.zw;
    p = (floor(p * cFull.xy) + 0.5) * cFull.zw;
    float d = tex2Dlod(sDepth, float4(p, 0, 0)).r;
    return d >= 0.99999 ? 2.0 : d;
}

float4 FocusRow(float y)
{
    return float4(FocusSample(float2(-1.0, y)), FocusSample(float2(-1.0 / 3.0, y)), FocusSample(float2(1.0 / 3.0, y)), FocusSample(float2(1.0, y)));
}

// x is a candidate when at least t samples are <= x; the smallest candidate is the t-th smallest sample
float Pick(float x, float t, float best, float4 r0, float4 r1, float4 r2, float4 r3)
{
    float4 xx = x.xxxx;
    float le = dot(step(r0, xx) + step(r1, xx) + step(r2, xx) + step(r3, xx), 1.0); // samples <= x
    return (le >= t && x < 1.5) ? min(best, x) : best;
}

float4 FocusPS(float2 uv : TEXCOORD0) : COLOR0
{
    float4 r0 = FocusRow(-1.0);
    float4 r1 = FocusRow(-1.0 / 3.0);
    float4 r2 = FocusRow(1.0 / 3.0);
    float4 r3 = FocusRow(1.0);
    float n = dot(step(r0, 1.5), 1.0) + dot(step(r1, 1.5), 1.0) + dot(step(r2, 1.5), 1.0) + dot(step(r3, 1.5), 1.0);
    float prev = cEase.y > 0.5 ? -1.0 : FocusValue();
    // 25th percentile of the non-sky samples: the nearer quarter wins, so a subject covering a quarter of the
    // window holds the focus against the background behind it, while a thin post or leaf does not grab it
    float t = max(1.0, ceil(0.25 * n));
    float best = 2.0;
    float4 rows[4] = { r0, r1, r2, r3 };
    [unroll] for (int a = 0; a < 4; a++)
    {
        best = Pick(rows[a].x, t, best, r0, r1, r2, r3);
        best = Pick(rows[a].y, t, best, r0, r1, r2, r3);
        best = Pick(rows[a].z, t, best, r0, r1, r2, r3);
        best = Pick(rows[a].w, t, best, r0, r1, r2, r3);
    }
    float target = cFocus.z - best;
    float next = prev > 0.0 ? lerp(prev, target, cEase.x) : target;
    // every sample is sky: keep the previous value
    return float4(n > 0.5 ? next : prev, 0, 0, 1);
}

// ---- Prep: full-res 2x2 block -> one half-res texel (rgb = linear colour, a = blur amount) ----

static const float2 kBlock[4] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1) };

float4 PrepPS(float2 uv : TEXCOORD0) : COLOR0
{
    // top-left full-res texel centre of this texel's block (pixels)
    float2 base = floor(uv * cHalf.xy) * 2.0 + 0.5;
    float focus = FocusValue();
    float k[4];
    float3 c[4];
    [unroll] for (int i = 0; i < 4; i++)
    {
        float2 p = (base + kBlock[i]) * cFull.zw;
        k[i] = BlurAmount(tex2Dlod(sDepth, float4(p, 0, 0)).r, focus) * cParams.z;
        c[i] = ToLinear(tex2Dlod(sColor, float4(p, 0, 0)).rgb);
    }
    float4 k4 = float4(k[0], k[1], k[2], k[3]);
    // The block takes its sharpest pixel's amount: a block that touches a sharp edge counts as sharp, so the gather
    // never spreads it into the blurred background. Its colour comes from the pixels whose radius is within about a
    // half-res pixel of that one (the sharp side), so the texel's colour and amount describe the same surface.
    float kmin = min(min(k4.x, k4.y), min(k4.z, k4.w));
    float4 w = saturate(1.0 - (k4 - kmin) * max(cDir.z, 2.0));
    float3 col = (c[0] * w.x + c[1] * w.y + c[2] * w.z + c[3] * w.w) / dot(w, 1.0);
    return float4(col, kmin);
}

// ---- Gather (separable, half resolution) ----

float4 BlurPS(float2 uv : TEXCOORD0) : COLOR0
{
    float4 center = tex2Dlod(sColor, float4(uv, 0, 0));
    float r = center.a * cDir.z; // this pixel's blur radius, half-res pixels
    [branch] if (r < 0.05) return center;
    float spacing = r / TAPS;
    float2 stepUv = cDir.xy * cHalf.zw * spacing;
    float3 acc = 0;
    float wsum = 0;
    [unroll] for (int i = -TAPS; i <= TAPS; i++)
    {
        float4 s = (i == 0) ? center : tex2Dlod(sColor, float4(uv + stepUv * i, 0, 0));
        float x = (float)i / TAPS;
        // Gaussian of sigma r / 2 over the radius
        float w = exp2(-2.8853901 * x * x);
        // scatter-as-gather: the tap counts only where its own blur radius reaches this pixel
        w *= saturate(s.a * cDir.z - abs(i) * spacing + 1.0);
        // lamps stay bright: near-white taps weigh a little more in blurred areas
        w *= 1.0 + cDir.w * center.a * max(dot(s.rgb, kLuma) - 0.8, 0.0);
        acc += s.rgb * w;
        wsum += w;
    }
    return float4(acc / wsum, center.a);
}

// ---- Composite: drawn with alpha blending over the backbuffer ----

float4 CompositePS(float2 uv : TEXCOORD0) : COLOR0
{
    float2 pix = floor(uv * cFull.xy) + 0.5; // full-res pixel centre (pixels)
    float2 fuv = pix * cFull.zw;
    float k = BlurAmount(tex2Dlod(sDepth, float4(fuv, 0, 0)).r, FocusValue()) * cParams.z;
    if (cFlags.y > 0.5)
    {
        float3 dbg = k.xxx;
        if (cFlags.w > 0.5 && all(abs(fuv - 0.5) <= cEase.zw)) dbg = lerp(dbg, float3(0.5, 0.47, 0.87), 0.45); // focus window
        return float4(dbg, 1);
    }
    float r = k * cDir.z;
    float alpha = smoothstep(0.1, 1.0, r);
    [branch] if (alpha <= 0.0) return float4(0, 0, 0, 0); // sharp: the original pixel, exactly

    // bilateral upsample: the 4 nearest half-res texels, bilinear weights x similarity of their blur amount
    float2 hp = pix * cHalf.xy * cFull.zw - 0.5;
    float2 b = floor(hp);
    float2 f = hp - b;
    float2 t0 = (b + 0.5) * cHalf.zw;
    float4 q00 = tex2Dlod(sBlur, float4(t0, 0, 0));
    float4 q10 = tex2Dlod(sBlur, float4(t0 + float2(cHalf.z, 0), 0, 0));
    float4 q01 = tex2Dlod(sBlur, float4(t0 + float2(0, cHalf.w), 0, 0));
    float4 q11 = tex2Dlod(sBlur, float4(t0 + cHalf.zw, 0, 0));
    float4 bw = float4((1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y);
    float4 dr = abs(float4(q00.a, q10.a, q01.a, q11.a) - k) * max(cDir.z, 2.0); // radius difference, half-res pixels
    float4 w = bw * exp2(-2.0 * dr * dr);
    float wsum = dot(w, 1.0);
    float3 col = (q00.rgb * w.x + q10.rgb * w.y + q01.rgb * w.z + q11.rgb * w.w) / max(wsum, 1e-5);
    alpha *= saturate(wsum * 20.0); // no similar texel around: fall back to the original
    return float4(FromLinear(col), alpha);
}
)HLSL";

// Every variant is compiled at start-up on a background thread (framework/shader_cache.h); the render thread only
// creates the shader objects from the bytecode. Priority 0 = the default quality (High) and the fixed passes.
ShaderCache::Id AddShader(const char* tag, const char* entry, int priority, const char* taps = nullptr) {
    ShaderCache::Desc d;
    d.tag = tag;
    d.source = kShaderSource;
    d.sourceName = "depth_blur.hlsl";
    d.entry = entry;
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    if (taps) d.macros.emplace_back("TAPS", taps);
    d.priority = priority;
    return ShaderCache::Add(std::move(d));
}
const ShaderCache::Id kFocusPsId = AddShader("DepthBlur FocusPS", "FocusPS", 0);
const ShaderCache::Id kPrepPsId = AddShader("DepthBlur PrepPS", "PrepPS", 0);
const ShaderCache::Id kCompositePsId = AddShader("DepthBlur CompositePS", "CompositePS", 0);
const ShaderCache::Id kBlurPsId[4] = {AddShader("DepthBlur BlurPS (Low, TAPS 4)", "BlurPS", 1, "4"), AddShader("DepthBlur BlurPS (Medium, TAPS 6)", "BlurPS", 1, "6"),
                                      AddShader("DepthBlur BlurPS (High, TAPS 8)", "BlurPS", 0, "8"), AddShader("DepthBlur BlurPS (Ultra, TAPS 12)", "BlurPS", 1, "12")};
static_assert(kQualityTaps[0] == 4 && kQualityTaps[1] == 6 && kQualityTaps[2] == 8 && kQualityTaps[3] == 12, "kBlurPsId lists the TAPS of kQualityTaps");

struct Params {
    bool autoFocus = false;   // Auto (follows what the camera looks at) or Fixed (the original start/range curve)
    float amount = 1.000000000f;     // max blur radius = amount x 1% of the screen height
    int sharpArea = 2;       // Auto: 0 Small, 1 Medium, 2 Large
    float focusSpeed = 0.100000001f; // Auto: easing time constant tau (s)
    bool glowLights = true;  // near-white taps weigh a little more
    float start = 0.318108052f;    // Fixed
    float range = 0.287828237f;     // Fixed
    float strength = 1.000000000f;   // multiplies the blur amount (both modes)
    float spread = 0.800000012f;     // legacy "tamanho" (the old Gaussian spread); kept so old configs load, no longer used
    float farPlane = 1000.000000000f; // Fixed
    bool blurSky = true;
    bool debugView = false;
    bool offInMapView = true; // no blur while the game's map view is open (everything is far away there)
    int quality = 1; // 0 Low, 1 Medium, 2 High, 3 Ultra
};

struct BlurState {
    WorldSession::Settled world;
    bool active = false; // the depth swap is running (Depth Blur on, or requested by another effect)
    bool blurOn = false; // the Depth Blur patch itself is on
    int requests = 0;    // DepthShare::Request from other effects
    bool ready = false;
    bool inBlur = false;
    bool internalPass = false; // another patch's own extra draw (e.g. the lake lamp pass with the depth-stencil unbound)
    int retryCountdown = 0;
    unsigned framesBlurred = 0;
    int lastTaps = 0;
    float mapFade = 0.0f;         // 0 = normal view, 1 = map view (blur fully off), eased over kMapFadeSeconds
    float worldIn = 0.0f;         // 0 = just loaded (blur off), 1 = full blur; kWorldHoldMs then kWorldFadeInSeconds
    LARGE_INTEGER lastFadeTick{}; // time of the previous frame step
    bool mapOpen = false;         // last map view state read from the game

    IDirect3DSurface9* curRT0 = nullptr;     // identity only
    IDirect3DSurface9* backBuffer = nullptr; // identity only
    UINT width = 0;
    UINT height = 0;

    IDirect3DSurface9* origDS = nullptr; // game's auto depth-stencil (reference held)
    IDirect3DTexture9* intzTex = nullptr;
    IDirect3DSurface9* intzSurf = nullptr;
    IDirect3DTexture9* fullTex = nullptr; // full-res copy of the backbuffer (Prep's colour source)
    IDirect3DSurface9* fullSurf = nullptr;
    IDirect3DTexture9* halfATex = nullptr;
    IDirect3DSurface9* halfASurf = nullptr;
    IDirect3DTexture9* halfBTex = nullptr;
    IDirect3DSurface9* halfBSurf = nullptr;
    D3DFORMAT halfFmt = D3DFMT_UNKNOWN;
    bool linearLight = false; // half targets are float: blur in linear light

    // Auto focus: two 1x1 float targets, ping-pong (read the previous, write the next)
    IDirect3DTexture9* focusTex[2] = {};
    IDirect3DSurface9* focusSurf[2] = {};
    D3DFORMAT focusFmt = D3DFMT_UNKNOWN; // UNKNOWN = no float target: Auto falls back to Fixed
    int focusCur = 0;                    // focusTex[focusCur] holds the latest value
    bool focusSnap = true;               // next focus pass ignores the previous value
    bool lastAuto = false;

    // Developer read-out of the focus (dev page only, throttled, never waited on)
    IDirect3DSurface9* readSurf = nullptr; // 1x1 system memory copy
    IDirect3DQuery9* readQuery = nullptr;  // event: the copy is done
    bool readPending = false;
    LARGE_INTEGER readWantUntil{};
    LARGE_INTEGER lastReadIssue{};
    float focusReadout = -1.0f; // the stored focus (A - d_f); <= 0 = nothing seen yet
    bool readoutValid = false;

    IDirect3DPixelShader9* psFocus = nullptr;
    IDirect3DPixelShader9* psPrep = nullptr;
    IDirect3DPixelShader9* psBlur[4] = {};
    bool blurTried[4] = {};
    IDirect3DPixelShader9* psComposite = nullptr;
    bool fixedTried = false; // focus / prep / composite compile was attempted

    // GPU cost (timestamp queries, read a few frames later)
    static constexpr int kQ = 4;
    IDirect3DQuery9 *qDisjoint[kQ] = {}, *qBegin[kQ] = {}, *qEnd[kQ] = {}, *qFreq[kQ] = {};
    bool qIssued[kQ] = {};
    int qNext = 0, qKey = -1;
    float gpuMs = -1.0f;

    bool gameAaOn = false; // the game's own multisampled Edge Smoothing is on: no readable depth (menu warning)
    std::string status = "Waiting for the game...";
    Params p;
};

BlurState g;

template <typename T> void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

IDirect3DSurface9* SubstituteDS(IDirect3DSurface9* requested) { return (g.ready && requested && requested == g.origDS) ? g.intzSurf : requested; }

IDirect3DSurface9* ReportDS(IDirect3DSurface9* actual) { return (g.ready && actual && actual == g.intzSurf) ? g.origDS : actual; }

void ReleaseResources(IDirect3DDevice9* dev) {
    ExtraHooks::SetDepthSubstitution(nullptr, nullptr);
    if (dev && g.intzSurf && g.origDS) {
        IDirect3DSurface9* cur = nullptr;
        if (SUCCEEDED(ExtraHooks::RawGetDepthStencilSurface(dev, &cur)) && cur) {
            if (cur == g.intzSurf) ExtraHooks::RawSetDepthStencilSurface(dev, g.origDS);
            cur->Release();
        }
    }
    g.ready = false;
    SafeRelease(g.intzSurf);
    SafeRelease(g.intzTex);
    SafeRelease(g.fullSurf);
    SafeRelease(g.fullTex);
    SafeRelease(g.halfASurf);
    SafeRelease(g.halfATex);
    SafeRelease(g.halfBSurf);
    SafeRelease(g.halfBTex);
    for (int i = 0; i < 2; i++) {
        SafeRelease(g.focusSurf[i]);
        SafeRelease(g.focusTex[i]);
    }
    SafeRelease(g.readSurf);
    SafeRelease(g.readQuery);
    g.readPending = false;
    g.readoutValid = false;
    g.focusSnap = true; // the new focus targets start empty
    for (int i = 0; i < BlurState::kQ; i++) {
        SafeRelease(g.qDisjoint[i]);
        SafeRelease(g.qBegin[i]);
        SafeRelease(g.qEnd[i]);
        SafeRelease(g.qFreq[i]);
        g.qIssued[i] = false;
    }
    SafeRelease(g.origDS);
}

void ReleaseShaders() {
    SafeRelease(g.psFocus);
    SafeRelease(g.psPrep);
    SafeRelease(g.psComposite);
    g.fixedTried = false;
    for (int q = 0; q < 4; q++) {
        SafeRelease(g.psBlur[q]);
        g.blurTried[q] = false;
    }
}

// Creates one pass from its precompiled bytecode (shader_cache.h: compiled at start-up, off the render thread)
IDirect3DPixelShader9* CreateShader(IDirect3DDevice9* dev, ShaderCache::Id id, const char* entry) {
    IDirect3DPixelShader9* ps = nullptr;
    std::string msg;
    switch (ShaderCache::CreatePixelShader(dev, id, &ps, &msg)) {
    case ShaderCache::Result::Ok:
        return ps;
    case ShaderCache::Result::CompileFailed:
        LOG_ERROR(std::format("[DepthBlur] Shader {} failed to compile: {}", entry, msg));
        return nullptr;
    case ShaderCache::Result::CreateFailed:
        LOG_ERROR(std::format("[DepthBlur] CreatePixelShader({}) failed", entry));
        return nullptr;
    }
    return nullptr;
}

// The gather of one quality (TAPS per side), created on first use from the precompiled bytecode
IDirect3DPixelShader9* BlurShader(IDirect3DDevice9* dev, int q) {
    q = q < 0 ? 0 : (q > 3 ? 3 : q);
    if (g.psBlur[q] || g.blurTried[q]) return g.psBlur[q];
    g.blurTried[q] = true;
    g.psBlur[q] = CreateShader(dev, kBlurPsId[q], "BlurPS");
    if (!g.psBlur[q]) g.status = "ERROR: the blur shaders did not compile (see ApexRadiance_LOG.txt)";
    return g.psBlur[q];
}

// The fixed passes (created once; also after an Uninstall released them while another effect kept the depth swap)
bool EnsureShaders(IDirect3DDevice9* dev) {
    const bool fixedOk = g.psFocus && g.psPrep && g.psComposite;
    if (!fixedOk && g.fixedTried) return false; // failed once: logged, not retried every frame
    g.fixedTried = true;
    if (!g.psFocus) g.psFocus = CreateShader(dev, kFocusPsId, "FocusPS");
    if (!g.psPrep) g.psPrep = CreateShader(dev, kPrepPsId, "PrepPS");
    if (!g.psComposite) g.psComposite = CreateShader(dev, kCompositePsId, "CompositePS");
    return g.psFocus && g.psPrep && g.psComposite && BlurShader(dev, g.p.quality);
}

bool CreateRT(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, IDirect3DTexture9** tex, IDirect3DSurface9** surf) {
    if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, tex, nullptr)) || !*tex) return false;
    return SUCCEEDED((*tex)->GetSurfaceLevel(0, surf)) && *surf;
}

// The format can be a render-target texture (and, when asked, filtered linearly) on this adapter
bool FormatSupported(IDirect3DDevice9* dev, D3DFORMAT fmt, bool filter) {
    IDirect3D9* d3d = nullptr;
    if (FAILED(dev->GetDirect3D(&d3d)) || !d3d) return false;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    bool ok = SUCCEEDED(dev->GetCreationParameters(&cp));
    D3DFORMAT adapterFmt = D3DFMT_X8R8G8B8;
    D3DDISPLAYMODE mode{};
    if (ok && SUCCEEDED(d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &mode)) && mode.Format != D3DFMT_UNKNOWN) adapterFmt = mode.Format;
    ok = ok && SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, adapterFmt, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, fmt));
    if (ok && filter) ok = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, adapterFmt, D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, fmt) == D3D_OK;
    d3d->Release();
    return ok;
}

const char* FormatName(D3DFORMAT f) {
    switch (f) {
    case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_R32F: return "R32F";
    case D3DFMT_R16F: return "R16F";
    default: return "?";
    }
}

float HalfToFloat(uint16_t h) {
    const int exponent = (h >> 10) & 0x1F;
    const int mantissa = h & 0x3FF;
    float v;
    if (exponent == 0) v = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31) v = mantissa ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(mantissa | 0x400), exponent - 25);
    return (h & 0x8000) ? -v : v;
}

bool InitResources(IDirect3DDevice9* dev) {
    if (!ExtraHooks::EnsureInstalled(dev)) {
        g.status = "ERROR: could not install the depth hooks";
        return false;
    }

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    D3DSURFACE_DESC bd{};
    bb->GetDesc(&bd);
    bb->Release();

    g.gameAaOn = bd.MultiSampleType != D3DMULTISAMPLE_NONE;
    if (g.gameAaOn) {
        g.status = "Edge Smoothing is on: turn it off in the game's Options > Graphics";
        return false;
    }

    IDirect3DSurface9* ds = nullptr;
    if (FAILED(ExtraHooks::RawGetDepthStencilSurface(dev, &ds)) || !ds) {
        g.status = "Waiting for the game (no depth buffer yet)";
        return false;
    }
    D3DSURFACE_DESC dd{};
    ds->GetDesc(&dd);
    if (dd.Width != bd.Width || dd.Height != bd.Height || dd.MultiSampleType != D3DMULTISAMPLE_NONE || (dd.Format != D3DFMT_D24S8 && dd.Format != D3DFMT_D24X8)) {
        ds->Release();
        g.status = "Waiting for the game (depth buffer is not the screen's)";
        return false;
    }

    g.origDS = ds; // keep the reference
    g.width = bd.Width;
    g.height = bd.Height;
    const UINT hw = (g.width + 1) / 2;
    const UINT hh = (g.height + 1) / 2;

    bool ok = SUCCEEDED(dev->CreateTexture(g.width, g.height, 1, D3DUSAGE_DEPTHSTENCIL, kFmtINTZ, D3DPOOL_DEFAULT, &g.intzTex, nullptr)) && g.intzTex &&
              SUCCEEDED(g.intzTex->GetSurfaceLevel(0, &g.intzSurf)) && g.intzSurf;
    if (!ok) {
        ReleaseResources(dev);
        g.status = "ERROR: the graphics card/driver does not support INTZ depth textures";
        return false;
    }
    if (!CreateRT(dev, g.width, g.height, bd.Format, &g.fullTex, &g.fullSurf)) {
        ReleaseResources(dev);
        g.status = "ERROR: not enough video memory for the blur textures";
        return false;
    }
    // Half-res targets: float (linear light, no banding; alpha = blur amount), else 8-bit in gamma space
    g.halfFmt = D3DFMT_UNKNOWN;
    if (FormatSupported(dev, D3DFMT_A16B16G16R16F, true) && CreateRT(dev, hw, hh, D3DFMT_A16B16G16R16F, &g.halfATex, &g.halfASurf) &&
        CreateRT(dev, hw, hh, D3DFMT_A16B16G16R16F, &g.halfBTex, &g.halfBSurf)) {
        g.halfFmt = D3DFMT_A16B16G16R16F;
    } else {
        SafeRelease(g.halfASurf);
        SafeRelease(g.halfATex);
        SafeRelease(g.halfBSurf);
        SafeRelease(g.halfBTex);
        LOG_WARNING("[DepthBlur] 16-bit float render targets not available, using A8R8G8B8 (blur in gamma space)");
        if (CreateRT(dev, hw, hh, D3DFMT_A8R8G8B8, &g.halfATex, &g.halfASurf) && CreateRT(dev, hw, hh, D3DFMT_A8R8G8B8, &g.halfBTex, &g.halfBSurf))
            g.halfFmt = D3DFMT_A8R8G8B8;
    }
    if (g.halfFmt == D3DFMT_UNKNOWN) {
        ReleaseResources(dev);
        g.status = "ERROR: not enough video memory for the blur textures";
        return false;
    }
    g.linearLight = g.halfFmt == D3DFMT_A16B16G16R16F;

    // Auto focus targets (1x1). Without a float target, Auto falls back to the Fixed curve.
    g.focusFmt = D3DFMT_UNKNOWN;
    for (D3DFORMAT f : {D3DFMT_R32F, D3DFMT_R16F, D3DFMT_A16B16G16R16F}) {
        if (!FormatSupported(dev, f, false)) continue;
        if (CreateRT(dev, 1, 1, f, &g.focusTex[0], &g.focusSurf[0]) && CreateRT(dev, 1, 1, f, &g.focusTex[1], &g.focusSurf[1])) {
            g.focusFmt = f;
            break;
        }
        for (int i = 0; i < 2; i++) {
            SafeRelease(g.focusSurf[i]);
            SafeRelease(g.focusTex[i]);
        }
    }
    if (g.focusFmt == D3DFMT_UNKNOWN) LOG_WARNING("[DepthBlur] No 1x1 float render target (R32F / R16F / A16B16G16R16F): Auto focus falls back to Fixed");
    g.focusCur = 0;
    g.focusSnap = true;

    if (!EnsureShaders(dev)) {
        ReleaseResources(dev);
        g.status = "ERROR: the blur shaders did not compile (see ApexRadiance_LOG.txt)";
        return false;
    }

    for (int i = 0; i < BlurState::kQ; i++) {
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &g.qDisjoint[i]);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g.qBegin[i]);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g.qEnd[i]);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &g.qFreq[i]);
    }
    g.gpuMs = -1.0f;

    IDirect3DSurface9* rt0 = nullptr;
    if (SUCCEEDED(dev->GetRenderTarget(0, &rt0)) && rt0) {
        g.curRT0 = rt0;
        rt0->Release();
    }

    ExtraHooks::SetDepthSubstitution(SubstituteDS, ReportDS);
    g.ready = true;
    ExtraHooks::RawSetDepthStencilSurface(dev, g.intzSurf); // the auto depth-stencil is bound right now
    g.status = "Active";
    LOG_INFO(std::format("[DepthBlur] Resources ready ({}x{}, INTZ depth swapped in, blur targets {}, focus target {})", g.width, g.height, FormatName(g.halfFmt),
                         g.focusFmt == D3DFMT_UNKNOWN ? "none" : FormatName(g.focusFmt)));
    return true;
}

struct QuadVertex {
    float x, y, z, rhw, u, v;
};

void DrawQuad(IDirect3DDevice9* dev, UINT w, UINT h) {
    const float x0 = -0.5f, y0 = -0.5f;
    const float x1 = static_cast<float>(w) - 0.5f, y1 = static_cast<float>(h) - 0.5f;
    const QuadVertex v[4] = {{x0, y0, 0, 1, 0, 0}, {x1, y0, 0, 1, 1, 0}, {x0, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVertex));
}

// ---- minimal state save/restore (only what the passes touch) ----

constexpr D3DRENDERSTATETYPE kRenderStates[] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
    D3DRS_BLENDOP, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE,
    D3DRS_COLORWRITEENABLE};
constexpr D3DSAMPLERSTATETYPE kSamplerStates[] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE,
    D3DSAMP_MAXMIPLEVEL, D3DSAMP_MIPMAPLODBIAS};
constexpr int kRS = static_cast<int>(sizeof(kRenderStates) / sizeof(kRenderStates[0]));
constexpr int kSS = static_cast<int>(sizeof(kSamplerStates) / sizeof(kSamplerStates[0]));
constexpr DWORD kSamplers = 4; // s0 colour, s1 blurred, s2 depth, s3 focus
constexpr UINT kPSConsts = 7;  // c0..c6

struct SavedState {
    IDirect3DSurface9* rt0 = nullptr;
    IDirect3DSurface9* ds = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    DWORD fvf = 0;
    IDirect3DVertexBuffer9* stream0 = nullptr;
    UINT stream0Offset = 0;
    UINT stream0Stride = 0;
    IDirect3DBaseTexture9* tex[kSamplers] = {};
    DWORD rs[kRS] = {};
    DWORD ss[kSamplers][kSS] = {};
    float psConst[kPSConsts * 4] = {};
    D3DVIEWPORT9 viewport{};

    void Capture(IDirect3DDevice9* dev) {
        dev->GetRenderTarget(0, &rt0);
        ExtraHooks::RawGetDepthStencilSurface(dev, &ds);
        dev->GetPixelShader(&ps);
        dev->GetVertexShader(&vs);
        dev->GetVertexDeclaration(&decl);
        dev->GetFVF(&fvf);
        dev->GetStreamSource(0, &stream0, &stream0Offset, &stream0Stride);
        for (DWORD s = 0; s < kSamplers; s++) {
            dev->GetTexture(s, &tex[s]);
            for (int i = 0; i < kSS; i++) dev->GetSamplerState(s, kSamplerStates[i], &ss[s][i]);
        }
        for (int i = 0; i < kRS; i++) dev->GetRenderState(kRenderStates[i], &rs[i]);
        dev->GetPixelShaderConstantF(0, psConst, kPSConsts);
        dev->GetViewport(&viewport);
    }

    void Restore(IDirect3DDevice9* dev) {
        dev->SetRenderTarget(0, rt0); // resets the viewport, so it goes first
        ExtraHooks::RawSetDepthStencilSurface(dev, ds);
        for (DWORD s = 0; s < kSamplers; s++) {
            dev->SetTexture(s, tex[s]);
            for (int i = 0; i < kSS; i++) dev->SetSamplerState(s, kSamplerStates[i], ss[s][i]);
        }
        for (int i = 0; i < kRS; i++) dev->SetRenderState(kRenderStates[i], rs[i]);
        dev->SetPixelShaderConstantF(0, psConst, kPSConsts);
        dev->SetPixelShader(ps);
        dev->SetVertexShader(vs);
        if (decl) {
            dev->SetVertexDeclaration(decl);
        } else {
            dev->SetFVF(fvf);
        }
        // DrawPrimitiveUP clears stream 0, the game's next draw needs it back
        dev->SetStreamSource(0, stream0, stream0Offset, stream0Stride);
        dev->SetViewport(&viewport);
        Release();
    }

    void Release() {
        SafeRelease(rt0);
        SafeRelease(ds);
        SafeRelease(ps);
        SafeRelease(vs);
        SafeRelease(decl);
        SafeRelease(stream0);
        for (auto& t : tex) SafeRelease(t);
    }
};

void SetFilter(IDirect3DDevice9* dev, DWORD s, DWORD filter) {
    dev->SetSamplerState(s, D3DSAMP_MINFILTER, filter);
    dev->SetSamplerState(s, D3DSAMP_MAGFILTER, filter);
}

void SetPassStates(IDirect3DDevice9* dev) {
    dev->SetVertexShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    for (DWORD s = 0; s < kSamplers; s++) {
        SetFilter(dev, s, D3DTEXF_POINT); // s0 turns linear for the gather only
        dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, 0);
        dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
        dev->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0);
    }
}

bool AutoFocusActive() { return g.p.autoFocus && g.focusFmt != D3DFMT_UNKNOWN; }

float SecondsSince(const LARGE_INTEGER& t) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return t.QuadPart ? static_cast<float>(now.QuadPart - t.QuadPart) / static_cast<float>(freq.QuadPart) : 1e9f;
}

// Developer read-out: collect a finished copy of the focus value (never waits)
void CollectFocusReadout() {
    if (!g.readPending || !g.readQuery || !g.readSurf) return;
    if (g.readQuery->GetData(nullptr, 0, 0) != S_OK) return;
    g.readPending = false;
    D3DLOCKED_RECT lr{};
    if (FAILED(g.readSurf->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
    if (g.focusFmt == D3DFMT_R32F) {
        std::memcpy(&g.focusReadout, lr.pBits, sizeof(float));
    } else { // R16F, or A16B16G16R16F whose first half is R
        uint16_t h = 0;
        std::memcpy(&h, lr.pBits, sizeof h);
        g.focusReadout = HalfToFloat(h);
    }
    g.readSurf->UnlockRect();
    g.readoutValid = true;
}

// Developer read-out: copy the new focus value to system memory, at most 4 times a second, only while the Developer
// page asks for it (the effect itself never reads it back)
void IssueFocusReadout(IDirect3DDevice9* dev, IDirect3DSurface9* focusSurf) {
    if (g.readPending || !g.readWantUntil.QuadPart) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (now.QuadPart > g.readWantUntil.QuadPart || SecondsSince(g.lastReadIssue) < 0.25f) return;
    g.lastReadIssue = now; // also throttles retries when a creation below fails
    if (!g.readSurf && FAILED(dev->CreateOffscreenPlainSurface(1, 1, g.focusFmt, D3DPOOL_SYSTEMMEM, &g.readSurf, nullptr))) {
        g.readSurf = nullptr;
        return;
    }
    if (!g.readQuery && FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &g.readQuery))) {
        g.readQuery = nullptr;
        return;
    }
    if (FAILED(dev->GetRenderTargetData(focusSurf, g.readSurf))) return;
    g.readQuery->Issue(D3DISSUE_END);
    g.readPending = true;
}

void RunBlur(IDirect3DDevice9* dev, float dt) {
    if (!EnsureShaders(dev)) return;
    IDirect3DPixelShader9* psBlur = BlurShader(dev, g.p.quality);
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;

    // 1. Backbuffer -> full-res copy (Prep reads its 2x2 blocks)
    if (FAILED(dev->StretchRect(bb, nullptr, g.fullSurf, nullptr, D3DTEXF_NONE))) {
        bb->Release();
        return;
    }

    g.inBlur = true;
    SavedState saved;
    saved.Capture(dev);

    const UINT hw = (g.width + 1) / 2;
    const UINT hh = (g.height + 1) / 2;
    const float W = static_cast<float>(g.width), H = static_cast<float>(g.height);

    // Depth must not be bound while it is sampled
    ExtraHooks::RawSetDepthStencilSurface(dev, nullptr);
    SetPassStates(dev);

    const bool autoFocus = AutoFocusActive();
    if (autoFocus && !g.lastAuto) g.focusSnap = true; // switched to Auto: start from what is on screen now
    g.lastAuto = autoFocus;

    const int area = g.p.sharpArea < 0 ? 0 : (g.p.sharpArea > 2 ? 2 : g.p.sharpArea);
    const float fc0 = 1.0f - 1.0f / kSharpAreas[area].start;
    const float fc1 = 1.0f - 1.0f / kSharpAreas[area].full;
    const float maxRadius = g.p.amount * 0.01f * H * 0.5f; // half-res pixels
    const float tau = std::fmax(g.p.focusSpeed, 0.01f);
    const float ease = g.focusSnap ? 1.0f : 1.0f - std::exp(-dt / tau);

    const float c[kPSConsts][4] = {
        {g.p.start, g.p.range, g.p.strength * (1.0f - g.mapFade) * g.worldIn, g.p.farPlane},
        {static_cast<float>(hw), static_cast<float>(hh), 1.0f / static_cast<float>(hw), 1.0f / static_cast<float>(hh)},
        {1.0f, 0.0f, maxRadius, g.p.glowLights ? kLampGain : 0.0f},
        {g.p.blurSky ? 1.0f : 0.0f, (!kPublicBuild && g.p.debugView) ? 1.0f : 0.0f, g.linearLight ? 1.0f : 0.0f, autoFocus ? 1.0f : 0.0f},
        {W, H, 1.0f / W, 1.0f / H},
        {fc0, 1.0f / (fc1 - fc0), kDepthA, 0.0f},
        {ease, g.focusSnap ? 1.0f : 0.0f, 0.5f * kFocusWindow * H / W, 0.5f * kFocusWindow},
    };
    dev->SetPixelShaderConstantF(0, &c[0][0], kPSConsts);
    dev->SetTexture(2, g.intzTex);

    // 2. Auto focus: previous (s3) -> next (1x1), then everything reads the next
    if (autoFocus) {
        CollectFocusReadout();
        const int next = 1 - g.focusCur;
        dev->SetPixelShader(g.psFocus);
        dev->SetRenderTarget(0, g.focusSurf[next]);
        dev->SetTexture(3, g.focusTex[g.focusCur]);
        DrawQuad(dev, 1, 1);
        g.focusCur = next;
        g.focusSnap = false;
        IssueFocusReadout(dev, g.focusSurf[next]);
    }
    dev->SetTexture(3, autoFocus ? g.focusTex[g.focusCur] : nullptr);

    // 3. Prep: full-res copy (s0, point) -> halfA (linear colour + blur amount)
    dev->SetPixelShader(g.psPrep);
    dev->SetRenderTarget(0, g.halfASurf);
    dev->SetTexture(0, g.fullTex);
    DrawQuad(dev, hw, hh);

    // 4. Gather H (halfA -> halfB) then V (halfB -> halfA), bilinear taps
    SetFilter(dev, 0, D3DTEXF_LINEAR);
    const float cH[4] = {1.0f, 0.0f, maxRadius, g.p.glowLights ? kLampGain : 0.0f};
    const float cV[4] = {0.0f, 1.0f, maxRadius, g.p.glowLights ? kLampGain : 0.0f};
    dev->SetPixelShader(psBlur);
    dev->SetRenderTarget(0, g.halfBSurf);
    dev->SetTexture(0, g.halfATex);
    dev->SetPixelShaderConstantF(2, cH, 1);
    DrawQuad(dev, hw, hh);
    dev->SetRenderTarget(0, g.halfASurf);
    dev->SetTexture(0, g.halfBTex);
    dev->SetPixelShaderConstantF(2, cV, 1);
    DrawQuad(dev, hw, hh);
    g.lastTaps = kQualityTaps[g.p.quality < 0 ? 0 : (g.p.quality > 3 ? 3 : g.p.quality)];

    // 5. Composite over the backbuffer: bilateral upsample of halfA (s1, point), alpha = how blurred (RGB only)
    dev->SetRenderTarget(0, bb);
    dev->SetTexture(0, nullptr);
    dev->SetTexture(1, g.halfATex);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    dev->SetPixelShader(g.psComposite);
    DrawQuad(dev, g.width, g.height);

    saved.Restore(dev);
    bb->Release();
    g.inBlur = false;
    g.framesBlurred++;
}

// GPU time of the effect, from timestamp queries of an earlier frame (never waits); restarts when the quality changes
void ReadTimings() {
    for (int i = 0; i < BlurState::kQ; i++) {
        if (!g.qIssued[i]) continue;
        BOOL disjoint = TRUE;
        UINT64 t0 = 0, t1 = 0, freq = 0;
        if (g.qDisjoint[i]->GetData(&disjoint, sizeof disjoint, 0) != S_OK || g.qBegin[i]->GetData(&t0, sizeof t0, 0) != S_OK ||
            g.qEnd[i]->GetData(&t1, sizeof t1, 0) != S_OK || g.qFreq[i]->GetData(&freq, sizeof freq, 0) != S_OK)
            continue;
        g.qIssued[i] = false;
        if (!disjoint && freq && t1 > t0) {
            const float ms = static_cast<float>(double(t1 - t0) * 1000.0 / double(freq));
            g.gpuMs = g.gpuMs < 0 ? ms : g.gpuMs * 0.9f + ms * 0.1f;
        }
    }
}

// Frame time since the previous PostScene call (clamped to 0.1 s so a hitch or a long pause does not skip the fades)
float StepTime() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = g.lastFadeTick.QuadPart ? SecondsSince(g.lastFadeTick) : 0.0f;
    g.lastFadeTick = now;
    return dt > 0.1f ? 0.1f : dt;
}

// Eases the map view fade toward the game's current state (once per frame, from the PostScene trigger)
void StepMapFade(float dt) {
    const bool wasOpen = g.mapOpen;
    g.mapOpen = g.p.offInMapView && MapView::IsOpen();
    if (wasOpen && !g.mapOpen) g.focusSnap = true; // back from the map: focus on the new view at once
    const float target = g.mapOpen ? 1.0f : 0.0f;
    const float step = dt / kMapFadeSeconds;
    g.mapFade = (g.mapFade < target) ? std::fmin(g.mapFade + step, target) : std::fmax(g.mapFade - step, target);
}

// PostScene effect (order kDepthBlur): after edge smoothing, before the UI
void BlurEffect(IDirect3DDevice9* dev) {
    if (!g.blurOn || !g.ready || g.inBlur || g.internalPass) return;
    if (!g.world.ready || !WorldSession::IsActive()) {
        // Loading/menu depth can be stale. Skip every blur/debug GPU pass and
        // snap autofocus when gameplay returns, without releasing shared depth.
        g.focusSnap = true;
        g.lastFadeTick = {};
        g.mapOpen = false;
        g.mapFade = 0;
        g.worldIn = 0;
        return;
    }
    {
        const unsigned long long since = GetTickCount64() - g.world.activeAt; // ready: at least the 3 s settle
        const float t = (static_cast<float>(since) - 3000.0f - static_cast<float>(kWorldHoldMs)) / (kWorldFadeInSeconds * 1000.0f);
        g.worldIn = t <= 0.0f ? 0.0f : t >= 1.0f ? 1.0f : t * t * (3.0f - 2.0f * t);
        if (g.worldIn <= 0.0f) { // still settling after the load: no pass, autofocus starts from the first faded frame
            g.focusSnap = true;
            g.lastFadeTick = {};
            return;
        }
    }
    const float dt = StepTime();
    StepMapFade(dt);
    if ((kPublicBuild || !g.p.debugView) && (g.p.strength * (1.0f - g.mapFade) * g.worldIn <= 0.0f || g.p.amount <= 0.0f)) return; // map view / no blur: no GPU work at all

    const int key = g.p.quality;
    if (key != g.qKey) {
        g.qKey = key;
        g.gpuMs = -1.0f;
        for (bool& b : g.qIssued) b = false;
    }
    ReadTimings();
    const int qi = g.qNext;
    const bool timed = !g.qIssued[qi] && g.qDisjoint[qi] && g.qBegin[qi] && g.qEnd[qi] && g.qFreq[qi];
    if (timed) {
        g.qDisjoint[qi]->Issue(D3DISSUE_BEGIN);
        g.qBegin[qi]->Issue(D3DISSUE_END);
    }
    RunBlur(dev, dt);
    if (timed) {
        g.qEnd[qi]->Issue(D3DISSUE_END);
        g.qFreq[qi]->Issue(D3DISSUE_END);
        g.qDisjoint[qi]->Issue(D3DISSUE_END);
        g.qIssued[qi] = true;
        g.qNext = (qi + 1) % BlurState::kQ;
    }
}

void OnFrameBoundary(IDirect3DDevice9* dev) {
    if (!g.active) return;
    g.world.Update(WorldSession::IsActive(), GetTickCount64());
    if (!g.world.ready) {
        g.focusSnap = true;
        g.lastFadeTick = {};
        g.mapOpen = false;
        g.mapFade = 0;
        g.worldIn = 0;
    }
    if (!g.ready && --g.retryCountdown <= 0) {
        g.retryCountdown = kRetryFrames;
        InitResources(dev);
    }
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        g.backBuffer = bb;
        bb->Release();
    }
}

void OnPreReset(IDirect3DDevice9* dev) {
    if (!g.active) return;
    g.world.Update(false, 0);
    ReleaseResources(dev);
    g.status = "Recreating after a video change...";
}

void OnPostReset(IDirect3DDevice9*) {
    if (!g.active) return;
    g.retryCountdown = 0;
    g.backBuffer = nullptr;
    g.curRT0 = nullptr;
}

// The depth swap runs while Depth Blur is on or another effect asked for the depth (DepthShare::Request).
void StartDepth() {
    if (g.active) return;
    D3D9Hooks::RegisterPresent(kHookName, [](D3D9Hooks::DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        OnFrameBoundary(ctx.device);
        return D3D9Hooks::HookAction::Continue;
    }, D3D9Hooks::Priority::First);
    RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
    RenderCallbacks::Add(RenderCallbacks::postReset, OnPostReset);
    g.active = true;
    g.retryCountdown = 0;
    g.status = "Waiting for the game...";
}

void StopDepth() {
    if (!g.active) return;
    g.world.Update(false, 0);
    g.active = false;
    D3D9Hooks::UnregisterAll(kHookName);
    RenderCallbacks::Remove(RenderCallbacks::preReset, OnPreReset);
    RenderCallbacks::Remove(RenderCallbacks::postReset, OnPostReset);
    ReleaseResources(ApexD3D::Device());
    g.gameAaOn = false;
}

void UpdateDepth() {
    if (g.blurOn || g.requests > 0) StartDepth();
    else StopDepth();
}

} // namespace

namespace DepthShare {
IDirect3DTexture9* Texture() { return g.ready ? g.intzTex : nullptr; }
IDirect3DSurface9* Surface() { return g.ready ? g.intzSurf : nullptr; }
void SetInternalPass(bool on) { g.internalPass = on; }
bool InternalPass() { return g.internalPass; }
void Request(bool on) {
    g.requests = on ? g.requests + 1 : (g.requests > 0 ? g.requests - 1 : 0);
    UpdateDepth();
}
std::string Status() { return g.status; }
} // namespace DepthShare

class DepthBlurPatch : public ApexPatch {
  public:
    DepthBlurPatch() : ApexPatch("DepthBlur", nullptr) {
        RegisterBoolSetting(&g.p.autoFocus, "focoAuto", Params{}.autoFocus, "Auto focus: keep what the camera looks at sharp (off = Fixed distance)");
        RegisterFloatSetting(&g.p.amount, "quantidade", SettingWidget::Slider, Params{}.amount, 0.0f, 1.0f, "Blur amount: largest blur radius, 1 = 1% of the screen height");
        RegisterEnumSetting(&g.p.sharpArea, "areaNitida", Params{}.sharpArea, "Auto focus: how much around the focus stays sharp", {"Small", "Medium", "Large"});
        RegisterFloatSetting(&g.p.focusSpeed, "velocidadeFoco", SettingWidget::Slider, Params{}.focusSpeed, 0.1f, 1.0f, "Auto focus: seconds the focus takes to follow the camera");
        RegisterBoolSetting(&g.p.glowLights, "realceLuzes", Params{}.glowLights, "Lamps stay bright in the blur");
        RegisterFloatSetting(&g.p.start, "distancia", SettingWidget::Slider, Params{}.start, 0.0f, 0.5f, "Fixed focus: where the blur starts. Higher = further from the camera.",
            {{"Near", 0.25f}, {"Medium", 0.349f}, {"Far", 0.45f}});
        RegisterFloatSetting(&g.p.range, "transicao", SettingWidget::Slider, Params{}.range, 0.01f, 0.5f,
            "Fixed focus: how far the blur takes to reach full strength. Lower = sharper transition.");
        RegisterFloatSetting(&g.p.strength, "forca", SettingWidget::Slider, Params{}.strength, 0.0f, 1.0f, "Multiplies the blur everywhere. 0 = none, 1 = full.");
        RegisterFloatSetting(&g.p.spread, "tamanho", SettingWidget::Slider, Params{}.spread, 0.5f, 6.0f, "Legacy blur size (no longer used; Blur amount replaces it)");
        RegisterEnumSetting(&g.p.quality, "qualidade", Params{}.quality, "Blur quality. Higher = smoother large blur, costs more GPU.", {"Low", "Medium", "High", "Ultra"});
        RegisterFloatSetting(&g.p.farPlane, "farPlane", SettingWidget::InputBox, Params{}.farPlane, 10.0f, 10000.0f, "Fixed focus: depth linearization (advanced)");
        RegisterBoolSetting(&g.p.blurSky, "blurSky", Params{}.blurSky, "Blur the sky");
        RegisterBoolSetting(&g.p.offInMapView, "offInMapView", Params{}.offInMapView, "Turn the blur off while the map view is open");
        RegisterBoolSetting(&g.p.debugView, "debugView", false, "Show the blur amount (white = blurred, black = sharp)");
    }

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        g.blurOn = true;
        g.focusSnap = true; // focus on what is on screen at once
        UpdateDepth();
        PostScene::Add(PostScene::kDepthBlur, BlurEffect);
        isEnabled = true;
        LOG_INFO("[DepthBlur] Installed");
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        lastError.clear();
        PostScene::Remove(BlurEffect);
        g.blurOn = false;
        UpdateDepth(); // keeps the depth swap while another effect still needs it
        ReleaseShaders();
        if (!g.active) g.status = "Off";
        isEnabled = false;
        LOG_INFO("[DepthBlur] Uninstalled");
        return true;
    }

    // Settings are read live every frame, never reinstall (that would tear down the depth swap from the wrong thread)
    void Update() override { pendingReinstall = false; }

    // Overview row and card header chip (all passes, timed with timestamp queries)
    float GpuCostMs() const override { return (isEnabled.load() && g.ready && g.gpuMs >= 0.0f) ? g.gpuMs : -1.0f; }
    const char* OverviewSummary() const override { return g.p.autoFocus ? "Automatic focus" : "Fixed focus"; }

    // The card's controls (menu: Image > Depth Blur page). Settings are read live every frame; the change notice only keeps
    // the base class informed and saves.
    void RenderCustomUI() override {
        SAFE_IMGUI_BEGIN();
        using ApexUi::IconId;
        static const Params kDefaults{}; // the registered defaults (changed dots and per-row Reset)
        bool changed = false;
        if (g.status.rfind("ERROR: ", 0) == 0) ApexUi::IconNote(IconId::TriangleAlert, g.status.c_str() + 7, VioletTheme::kError);

        // Focus: Auto (follows the camera) or Fixed (the original distance curve)
        static const char* const kFocusModes[] = {"Auto", "Fixed"};
        static const char* const kFocusTips[] = {"Keeps what the camera looks at sharp", "Blur starts at a set distance, like before"};
        int focusMode = g.p.autoFocus ? 0 : 1;
        if (ApexUi::SegmentedRow("Focus", "How the sharp part is chosen", "##Focus", &focusMode, kFocusModes, 2, kFocusTips, nullptr, kDefaults.autoFocus ? 0 : 1)) {
            g.p.autoFocus = focusMode == 0;
            changed = true;
        }
        changed |= ApexUi::SliderPercent("Blur amount", &g.p.amount, 0.0f, 1.0f, "How soft the background gets", kDefaults.amount);

        if (g.p.autoFocus) {
            static const char* const kAreas[] = {"Small", "Medium", "Large"};
            static const char* const kAreaTips[] = {"Only the focus stays sharp", "Some space around the focus stays sharp", "The default; a wide sharp zone around the focus"};
            changed |= ApexUi::SegmentedRow("Sharp area", "How much around the focus stays sharp", "##SharpArea", &g.p.sharpArea, kAreas, 3, kAreaTips, nullptr,
                                            kDefaults.sharpArea);
        } else {
            // Distance: named steps, then fine-tuning (shown as 0-100% of its 0..0.5 range)
            static const char* const kDistances[] = {"Near", "Medium", "Far"};
            static const char* const kDistanceTips[] = {"The blur starts close to the camera", "The blur starts at a middle distance", "Only the far background blurs"};
            static constexpr float kDistanceValues[] = {0.25f, 0.349f, 0.45f};
            int distance = -1; // a fine-tuned value matches none of the steps
            for (int i = 0; i < 3; i++)
                if (std::fabs(g.p.start - kDistanceValues[i]) < 0.0005f) distance = i;
            if (ApexUi::SegmentedRow("Distance", "Where the blur begins", "##Distance", &distance, kDistances, 3, kDistanceTips, nullptr, 1) && distance >= 0) {
                g.p.start = kDistanceValues[distance];
                changed = true;
            }
            {
                ApexUi::SliderOptions o;
                o.format = "%.0f%%";
                o.displayScale = 200.0f;
                o.tooltip = "0% starts at the camera, 100% far away";
                o.defaultValue = kDefaults.start;
                changed |= ApexUi::Slider("Fine-tune distance", &g.p.start, 0.0f, 0.5f, o);
            }
            {
                ApexUi::SliderOptions o;
                o.format = "%.0f%%";
                o.displayScale = 200.0f;
                o.tooltip = "How gradually the blur fades in; lower gives a sharper line";
                o.defaultValue = kDefaults.range;
                changed |= ApexUi::Slider("Transition", &g.p.range, 0.01f, 0.5f, o);
            }
        }
        changed |= ApexUi::SwitchRow("Sharp in map view", &g.p.offInMapView, "Fades the blur out in map view so lots stay sharp", kDefaults.offInMapView);
        if (g.p.offInMapView && !MapView::Available()) ApexUi::IconNote(IconId::Info, "Map view can't be detected on this game version");

        // The rare knobs of the look
        if (ApexUi::BeginAdvanced("Advanced##DepthBlur")) {
            changed |= ApexUi::SliderPercent("Strength", &g.p.strength, 0.0f, 1.0f, "Scales the blur everywhere; 100% is the default", kDefaults.strength);
            static const char* const kQualities[] = {"Low", "Medium", "High", "Ultra"};
            static const char* const kQualityTips[] = {"Fastest", "The default; smoother", "Smoother still", "Smoothest large blur; costs the most"};
            changed |= ApexUi::SegmentedRow("Quality", "Higher is smoother and costs a bit more", "##Quality", &g.p.quality, kQualities, 4, kQualityTips, nullptr, kDefaults.quality);
            if (g.p.autoFocus) {
                ApexUi::SliderOptions o;
                o.format = "%.1f s";
                o.tooltip = "How long the focus takes to catch up; lower is quicker";
                o.defaultValue = kDefaults.focusSpeed;
                changed |= ApexUi::Slider("Focus speed", &g.p.focusSpeed, 0.1f, 1.0f, o);
            }
            changed |= ApexUi::SwitchRow("Blur the sky", &g.p.blurSky, "Also blur the sky behind the scenery", kDefaults.blurSky);
            changed |= ApexUi::SwitchRow("Glowing lights", &g.p.glowLights, "Lamps stay bright in the blur", kDefaults.glowLights);
            ApexUi::EndAdvanced();
        }

        if (changed) NotifySettingChanged();
    }

    // Developer page > Debug views
    void RenderDeveloperUI() override {
        SAFE_IMGUI_BEGIN();
        bool changed = false;

        changed |= ApexUi::Checkbox("Show blur amount", &g.p.debugView);
        ApexUi::Tooltip("Shows the blur amount instead of the image: white = blurred, black = sharp; the Auto focus window is tinted violet");
        ImGui::SetNextItemWidth(120.0f * ApexUi::Unit());
        if (ImGui::InputFloat("Far plane", &g.p.farPlane, 0.0f, 0.0f, "%.1f")) {
            g.p.farPlane = std::fmin(std::fmax(g.p.farPlane, 10.0f), 10000.0f);
            changed = true;
        }
        ApexUi::Tooltip("Fixed focus only: the far plane of the curve that turns the depth buffer into distance (10 - 10000)");
        if (changed) NotifySettingChanged();
        if (ApexUi::BeginAdvanced("DiagnosticDetails", "Focus and rendering details")) {
        ImGui::TextWrapped("Status: %s", g.status.c_str());
        // Ask the effect for the focus read-out while this is drawn (the next half second)
        {
            LARGE_INTEGER now, freq;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            g.readWantUntil.QuadPart = now.QuadPart + freq.QuadPart / 2;
        }
        const bool autoOk = AutoFocusActive();
        const char* mode = !g.p.autoFocus ? "Fixed" : (g.ready && g.focusFmt == D3DFMT_UNKNOWN) ? "Auto (no float target: using Fixed)" : "Auto";
        ImGui::TextDisabled("Focus: %s", mode);
        if (g.p.autoFocus && autoOk) {
            if (!g.readoutValid)
                ImGui::TextDisabled("Focus depth: reading...");
            else if (g.focusReadout > 0.0f)
                ImGui::TextDisabled("Focus depth A - d: %.6f  |  about %.1f m (approx, assumes near %.2f)", g.focusReadout, kReadoutNear * kDepthA / g.focusReadout, kReadoutNear);
            else
                ImGui::TextDisabled("Focus depth: none yet (only sky in the window)");
        }
        if (g.ready) {
            if (g.gpuMs >= 0) ImGui::TextDisabled("GPU cost: %.2f ms per frame", g.gpuMs);
            ImGui::TextDisabled("Blurred frames: %u  |  taps per side: %d  |  blur targets: %s", g.framesBlurred, g.lastTaps, FormatName(g.halfFmt));
        }
        ImGui::TextDisabled("Map view: %s  |  fade %.2f", g.mapOpen ? "open" : "closed", g.mapFade);
            ApexUi::EndAdvanced();
        }
    }
};

APEX_REGISTER_FEATURE(DepthBlurPatch, {.displayName = "Depth Blur",
                                   .description = "Softly blurs the background behind what the camera looks at, like a real camera lens, while menus stay sharp. "
                                                  "Works with the game's own Edge Smoothing turned off. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                                   .category = "Graphics",
                                   .experimental = true,
                                             .enabledByDefault = true,
                                   .supportedVersions = VERSION_ALL,
                                   .technicalDetails = {"Swaps the auto depth-stencil for an INTZ texture via Set/GetDepthStencilSurface detours (transparent to the game).",
                                       "Runs before the first ZENABLE=FALSE backbuffer draw after the scene (bloom composite / UI start).",
                                       "Auto focus: 25th percentile of 16 central depths in a 1x1 float target, eased on the GPU (no readback).",
                                       "Half-res 2x2 prep, separable scatter-as-gather blur with a per-pixel radius (fraction of the screen height), "
                                       "FP16 linear light, bilateral upsample alpha-blended over the backbuffer.",
                                       "Saves/restores only the states it touches (no full state block). GPU cost measured with timestamp queries.",
                                       "Multisampled depth cannot be sampled in D3D9, so Edge Smoothing must be off."}})
