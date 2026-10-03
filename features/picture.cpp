// Picture filters (see picture.h).
//
// Frame flow:
//  1. the game draws its scene into the back buffer; the post-scene effects run (Edge Smoothing, Depth Blur) at the
//     scene's end; the game adds its bloom (one full-screen DrawPrimitive strip right after the scene);
//  2. at every point where the game goes from depth-tested to depth-off back buffer drawing, the back buffer is copied
//     (sceneTex): the last copy of the frame is the scene without any UI (interiors have depth-off draws in the middle
//     of the scene, so an earlier copy would miss part of it);
//  3. the game draws its UI, Apex its overlay;
//  4. at the end (the first EndScene of the frame, render target = back buffer), the back buffer is copied (frameTex)
//     and filtered into itself: pixels equal to the scene copy get the filters, pixels the UI changed keep their colour.
//
// Hook order: the draw hooks run after PostScene's (Priority::First), so the scene copy already contains the
// post-scene effects and their pixels are not mistaken for UI; and before any feature that may skip a draw.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "picture.h"
#include "apex_config.h"
#include "apex_log.h"
#include "d3d9_hooks.h"
#include "depth_share.h"
#include "shader_cache.h"
#include "imgui.h"
#include "ui/i18n.h"
#include "ui/widgets.h"
#include "scene_dither.h"
#include <d3dcompiler.h>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <iterator>

#pragma comment(lib, "d3dcompiler.lib")

namespace {

constexpr const char* kHookName = "Picture";

// Why the pass last returned early (Picture::Problem)
enum Skip : int { kSkipNone, kSkipNoFrame, kSkipNoBackBuffer, kSkipNotBackBuffer, kSkipResources };
constexpr unsigned long long kProblemAfterMs = 2000; // on, but not applied for this long: a problem
// After PostScene (Priority::First = 0), before Early (25) and every feature that may skip a draw (Normal = 50).
constexpr auto kDrawPriority = static_cast<D3D9Hooks::Priority>(10);
constexpr int kMinSceneDraws = 20; // depth-tested back buffer draws before the UI can start (as PostScene)

const char* kShaderSource = R"HLSL(
sampler2D sFrame : register(s0); // the finished frame (scene + UI), point
sampler2D sScene : register(s1); // the scene before the UI, point
sampler2D sBase  : register(s2); // the scene at 1/8 size, bilinear (clarity: the local average)
float4 cLook   : register(c0);  // x = saturation, y = scene copy valid, z = compare
float4 cSize   : register(c1);  // xy = 1 / size, zw = size (pixels)
float4 cGrade  : register(c2);  // x = exposure (linear gain), y = contrast, z = blacks (fraction of white, + deepens)
float4 cWb     : register(c3);  // rgb = white balance gains (luminance of white kept)
float4 cDeband : register(c4);  // x = step threshold (encoded units), y = inner radius (px), z = outer radius (px), w = on
float4 cTone   : register(c5);  // x = midtone exponent (1 / midtones), y = shadows, z = highlights, w = vibrance
float4 cDetail : register(c6);  // x = sharpening, y = clarity
float4 cTintS  : register(c7);  // rgb = shadow colour (luminance 1), w = amount
float4 cTintH  : register(c8);  // rgb = highlight colour (luminance 1), w = amount
float4 cMixA   : register(c9);  // saturation of red, yellow, green, cyan
float4 cMixB   : register(c10); // x, y = saturation of blue, magenta; z = mixer on
float4 cVig    : register(c11); // x = amount, y = start radius, z = width / height, w = on
float4 cBase   : register(c12); // xy = size of the 1/8 scene, zw = 1 / that size
static const float3 kLum = float3(0.2126, 0.7152, 0.0722);

float3 Decode(float3 c)
{
    return pow(max(c, 0.0), 2.2);
}

// Gradient smoothing (deband): the game's textures are DXT (5-6 bit colour endpoints) and its light maps 8-bit, so
// smooth gradients arrive as steps. Each pixel averages the neighbours on two rings (fixed directions, no noise) that
// differ from it by less than a small step; an edge or a texture detail is larger than that and is left alone.
float3 Deband(float2 uv, float3 c)
{
    float3 sum = c;
    float n = 1.0;
    [unroll] for (int ring = 0; ring < 2; ring++)
    {
        float r = ring == 0 ? cDeband.y : cDeband.z;
        [unroll] for (int i = 0; i < 8; i++)
        {
            float a = (i + ring * 0.5) * 0.7853982;
            float3 v = tex2Dlod(sFrame, float4(uv + float2(cos(a), sin(a)) * r * cSize.xy, 0, 0)).rgb;
            float3 dd = abs(v - c);
            float w = max(dd.r, max(dd.g, dd.b)) < cDeband.x ? 1.0 : 0.0;
            sum += v * w;
            n += w;
        }
    }
    return sum / n;
}

// Cubic B-spline sample of the 1/8 scene from 4 bilinear taps: a smooth local average with no grid pattern
float3 SampleBase(float2 uv)
{
    float2 p = uv * cBase.xy - 0.5;
    float2 f = frac(p);
    p -= f;
    float2 f2 = f * f, f3 = f2 * f;
    float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    float2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    float2 w3 = f3 / 6.0;
    float2 g0 = w0 + w1, g1 = w2 + w3;
    float2 h0 = (p - 0.5 + w1 / g0) * cBase.zw; // p - 1 + w1 / g0, then + 0.5 to the texel centre
    float2 h1 = (p + 1.5 + w3 / g1) * cBase.zw; // p + 1 + w3 / g1, then + 0.5
    float3 a = tex2Dlod(sBase, float4(h0.x, h0.y, 0, 0)).rgb * g0.x + tex2Dlod(sBase, float4(h1.x, h0.y, 0, 0)).rgb * g1.x;
    float3 b = tex2Dlod(sBase, float4(h0.x, h1.y, 0, 0)).rgb * g0.x + tex2Dlod(sBase, float4(h1.x, h1.y, 0, 0)).rgb * g1.x;
    return a * g0.y + b * g1.y;
}

// Hue (0..6: red, yellow, green, cyan, blue, magenta) and saturation (0..1) of a linear colour, on gamma-2.2 values
float HueSat(float3 g, out float sat)
{
    float3 c = pow(max(g, 0.0), 1.0 / 2.2);
    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b)), dl = mx - mn;
    sat = mx > 1e-5 ? dl / mx : 0.0;
    if (dl < 1e-5) return 0.0;
    float h = mx == c.r ? (c.g - c.b) / dl : (mx == c.g ? 2.0 + (c.b - c.r) / dl : 4.0 + (c.r - c.g) / dl);
    return h < 0.0 ? h + 6.0 : h;
}

// Saturation per hue: 6 overlapping bands (red, yellow, green, cyan, blue, magenta) whose weights always sum to 1
float MixerSaturation(float3 g)
{
    float sat0;
    float h = HueSat(g, sat0);
    if (sat0 < 1e-5) return 1.0;
    float s[6] = { cMixA.x, cMixA.y, cMixA.z, cMixA.w, cMixB.x, cMixB.y };
    float r = 0.0;
    [unroll] for (int i = 0; i < 6; i++)
    {
        float d = abs(h - i);
        d = min(d, 6.0 - d);
        r += saturate(1.0 - d) * s[i];
    }
    return r;
}

float4 PicturePS(float2 uv : TEXCOORD0) : COLOR0
{
    float3 f = tex2Dlod(sFrame, float4(uv, 0, 0)).rgb;
    float3 s = tex2Dlod(sScene, float4(uv, 0, 0)).rgb;
    float3 d = abs(f - s);
    // no scene copy this frame (the game did not draw its scene into the back buffer): everything is filtered, UI included
    float ui = cLook.y > 0.5 ? saturate(max(d.r, max(d.g, d.b)) * 64.0) : 0.0;
    float3 fs = (cDeband.w > 0.5 && ui < 0.5) ? Deband(uv, f) : f; // the scene only; the UI keeps its sharp edges
    // sharpening (scene only): the difference to the 4 neighbours added back, limited to their range (no halos)
    [branch] if (cDetail.x > 0.0 && ui < 0.5)
    {
        float3 n0 = tex2Dlod(sFrame, float4(uv + float2(cSize.x, 0), 0, 0)).rgb, n1 = tex2Dlod(sFrame, float4(uv - float2(cSize.x, 0), 0, 0)).rgb;
        float3 n2 = tex2Dlod(sFrame, float4(uv + float2(0, cSize.y), 0, 0)).rgb, n3 = tex2Dlod(sFrame, float4(uv - float2(0, cSize.y), 0, 0)).rgb;
        float3 lo = min(min(n0, n1), min(n2, n3)), hi = max(max(n0, n1), max(n2, n3));
        fs = clamp(fs + cDetail.x * (fs - (n0 + n1 + n2 + n3) * 0.25), min(lo, fs), max(hi, fs));
    }
    float3 g = Decode(fs);

    // clarity: the pixel's luminance against the smooth local average, as a ratio (log2), limited so strong edges
    // (the ratio far from 1) get almost nothing and cannot form halos; midtones only
    [branch] if (cDetail.y != 0.0 && ui < 0.5)
    {
        float Lp = dot(g, kLum), Lb = dot(Decode(SampleBase(uv)), kLum);
        if (Lp > 1e-5 && Lb > 1e-5)
        {
            float r = log2(Lp / Lb);
            float e = saturate(pow(Lp, 1.0 / 2.2));
            g *= exp2(cDetail.y * r / (1.0 + r * r) * 4.0 * e * (1.0 - e));
        }
    }

    // scene grade, in linear light (1 = white): exposure, white balance, contrast around mid grey
    g = g * cGrade.x * cWb.rgb;
    g = 0.18 * pow(max(g, 0.0) / 0.18, cGrade.y);
    // tone zones on luminance (hue kept): midtones (a power that leaves black and white in place), shadows, highlights
    float L0 = dot(g, kLum);
    if (L0 > 1e-6)
    {
        float Lm = L0 < 1.0 ? pow(L0, cTone.x) : L0;
        Lm *= (1.0 + 0.5 * cTone.y * (1.0 - smoothstep(0.0, 0.25, Lm))) * (1.0 + 0.3 * cTone.z * smoothstep(0.3, 1.0, Lm));
        g *= Lm / L0;
    }
    float b = cGrade.z;
    g = b >= 0.0 ? max(g - b, 0.0) / (1.0 - b) : g * (1.0 + b) - b;
    // split toning: a colour for the shadows and one for the highlights, each of luminance 1 so brightness stays
    {
        float e = saturate(pow(max(dot(g, kLum), 0.0), 1.0 / 2.2));
        g *= lerp(1.0, cTintS.rgb, cTintS.w * (1.0 - smoothstep(0.0, 0.55, e))) * lerp(1.0, cTintH.rgb, cTintH.w * smoothstep(0.45, 1.0, e));
    }
    // vibrance: saturates the dull colours more than the vivid ones
    {
        float mx = max(g.r, max(g.g, g.b)), mn = min(g.r, min(g.g, g.b));
        float sat0 = mx > 1e-6 ? (mx - mn) / mx : 0.0;
        float Lv = dot(g, kLum);
        g = max(lerp(float3(Lv, Lv, Lv), g, 1.0 + cTone.w * (1.0 - sat0)), 0.0);
    }
    // saturation: all colours, times the colour mixer's value for this hue
    {
        float sat = cLook.x;
        [branch] if (cMixB.z > 0.5) sat *= MixerSaturation(g);
        float Ls = dot(g, kLum);
        g = max(lerp(float3(Ls, Ls, Ls), g, sat), 0.0);
    }
    // vignette: darker towards the corners (aspect corrected, 0 at the centre, 1 at a corner)
    [branch] if (cVig.w > 0.5)
    {
        float2 q = (uv - 0.5) * float2(cVig.z, 1.0);
        float r = length(q) / length(float2(cVig.z, 1.0) * 0.5);
        float v = smoothstep(cVig.y, 1.0, r);
        g *= 1.0 - cVig.x * v * v * (3.0 - 2.0 * v);
    }
    bool before = cLook.z > 0.5 && uv.x < 0.5; // compare: the left half as it came from the game
    bool divider = cLook.z > 0.5 && abs(uv.x - 0.5) < cSize.x;

    // back to gamma 2.2 for the 8-bit back buffer, with a fixed dither (interleaved gradient noise, the same pattern every
    // frame, below one 8-bit step) so the grading does not turn smooth gradients into steps
    float3 o = pow(saturate(g), 1.0 / 2.2);
    float2 px = floor(uv * cSize.zw);
    o += (frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715)))) - 0.5) / 255.0;
    if (before) o = f;
    if (divider) o = float3(1, 0, 0);
    return float4(lerp(o, f, ui), 1.0);
}
)HLSL";

// Compiled at start-up on a background thread (framework/shader_cache.h); InitResources only creates the shader object.
ShaderCache::Id AddPictureShader() {
    ShaderCache::Desc d;
    d.tag = "Picture PicturePS";
    d.source = kShaderSource;
    d.sourceName = "picture.hlsl";
    d.entry = "PicturePS";
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    d.priority = 0;
    return ShaderCache::Add(std::move(d));
}
const ShaderCache::Id kPicturePsId = AddPictureShader();

struct Gpu {
    bool ready = false, compileTried = false;
    UINT width = 0, height = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN; // the back buffer's (the status line)
    IDirect3DTexture9 *frameTex = nullptr, *sceneTex = nullptr;
    IDirect3DSurface9 *frameSurf = nullptr, *sceneSurf = nullptr;
    // the scene at 1/2, 1/4 and 1/8 size (each a 2x2 box of the previous): clarity's local average is the last one
    static constexpr int kChain = 3;
    IDirect3DTexture9* chainTex[kChain] = {};
    IDirect3DSurface9* chainSurf[kChain] = {};
    UINT baseW = 0, baseH = 0;
    IDirect3DPixelShader9* ps = nullptr;
    static constexpr int kQ = 4;
    IDirect3DQuery9 *qDisjoint[kQ] = {}, *qBegin[kQ] = {}, *qEnd[kQ] = {}, *qFreq[kQ] = {};
    bool qIssued[kQ] = {};
    int qNext = 0;
    // frame state (render thread)
    bool hooks = false;
    bool frameReady = true;      // the pass has not run yet this frame
    int sceneDraws = 0;          // depth-tested back buffer draws this frame
    bool lastWasScene = false;   // the last back buffer draw was depth-tested
    bool copyAfterStrip = false; // a bloom strip right after the scene: copy once it has drawn
    bool sceneCopied = false;
    IDirect3DSurface9* curRT0 = nullptr; // identity only
    IDirect3DSurface9* backBuffer = nullptr;
};
Gpu gpu;

template <typename T> void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// The settings in one line, for the log
std::string ParamsText(const PictureParams& q) {
    return std::format("on {}, exposure {:.2f}, contrast {:.2f}, midtones {:.2f}, shadows {:.2f}, highlights {:.2f}, blacks {:.2f}, temperature {:.2f}, tint {:.2f}, "
                       "saturation {:.2f}, vibrance {:.2f}, deband {:.2f}, sharpen {:.2f}, clarity {:.2f}, vignette {:.2f}, compare {}",
                       q.enabled, q.exposure, q.contrast, q.midtones, q.shadows, q.highlights, q.blacks, q.temperature, q.tint, q.saturation, q.vibrance, q.deband,
                       q.sharpen, q.clarity, q.vignette, q.compare);
}

// Copies the back buffer as "the scene" (see the frame flow above).
void CopyScene(IDirect3DDevice9* dev) {
    if (!gpu.ready) return;
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        gpu.sceneCopied = SUCCEEDED(dev->StretchRect(bb, nullptr, gpu.sceneSurf, nullptr, D3DTEXF_NONE));
        bb->Release();
    }
}

// Back buffer draws of the game: find the point between the scene (with its bloom) and the UI
void OnGameDraw(IDirect3DDevice9* dev, bool isStripOfTwo) {
    if (DepthShare::InternalPass()) return;
    if (!gpu.curRT0 || gpu.curRT0 != gpu.backBuffer) return;
    DWORD z = D3DZB_TRUE;
    dev->GetRenderState(D3DRS_ZENABLE, &z);
    if (z != D3DZB_FALSE) {
        gpu.sceneDraws++;
        gpu.lastWasScene = true;
        gpu.copyAfterStrip = false;
        return;
    }
    if (gpu.sceneDraws < kMinSceneDraws) return;
    if (gpu.lastWasScene) { // depth-tested -> depth-off: possibly the end of the scene
        gpu.lastWasScene = false;
        if (isStripOfTwo) { // the bloom composite strip: part of the scene, copy after it
            gpu.copyAfterStrip = true;
            return;
        }
        CopyScene(dev);
        return;
    }
    if (gpu.copyAfterStrip) {
        gpu.copyAfterStrip = false;
        CopyScene(dev);
    }
}

void OnFrameBoundary(IDirect3DDevice9* dev) {
    IDirect3DSurface9* s = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &s)) && s) {
        gpu.backBuffer = s;
        s->Release();
    }
    // after a device Reset (BeforeReset forgot it): render target 0 is the new back buffer, set by the Reset without any
    // SetRenderTarget call the hook could see
    if (!gpu.curRT0 && SUCCEEDED(dev->GetRenderTarget(0, &s)) && s) {
        gpu.curRT0 = s;
        s->Release();
    }
    gpu.frameReady = true;
    gpu.sceneDraws = 0;
    gpu.lastWasScene = false;
    gpu.copyAfterStrip = false;
    gpu.sceneCopied = false;
}

void RegisterHooks(IDirect3DDevice9* dev) {
    if (gpu.hooks) return;
    gpu.hooks = true;
    using namespace D3D9Hooks;
    IDirect3DSurface9* s = nullptr;
    if (SUCCEEDED(dev->GetRenderTarget(0, &s)) && s) {
        gpu.curRT0 = s;
        s->Release();
    }
    RegisterPresent(kHookName, [](DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        OnFrameBoundary(ctx.device);
        return HookAction::Continue;
    }, Priority::First);
    RegisterSetRenderTarget(kHookName, [](DeviceContext&, DWORD index, IDirect3DSurface9* rt) {
        if (index == 0) gpu.curRT0 = rt;
        return HookAction::Continue;
    }, Priority::First);
    RegisterDrawIndexedPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) {
        OnGameDraw(ctx.device, false);
        return HookAction::Continue;
    }, kDrawPriority);
    RegisterDrawPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE type, UINT, UINT prims) {
        OnGameDraw(ctx.device, type == D3DPT_TRIANGLESTRIP && prims == 2);
        return HookAction::Continue;
    }, kDrawPriority);
}

// The shader object from the precompiled bytecode (shader_cache.h)
IDirect3DPixelShader9* CompileShader(IDirect3DDevice9* dev) {
    IDirect3DPixelShader9* ps = nullptr;
    std::string msg;
    if (ShaderCache::CreatePixelShader(dev, kPicturePsId, &ps, &msg) == ShaderCache::Result::CompileFailed)
        LOG_ERROR("[Picture] Shader failed to compile: " + msg);
    return ps;
}

struct QuadVertex {
    float x, y, z, rhw, u, v;
};

// Fully saturated colour of a hue (degrees), linear RGB
void HueColour(float hueDeg, float rgb[3]) {
    const float h = std::fmod(std::fmod(hueDeg, 360.0f) + 360.0f, 360.0f) / 60.0f;
    const float x = 1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f);
    const int sector = static_cast<int>(h) % 6;
    const float table[6][3] = {{1, x, 0}, {x, 1, 0}, {0, 1, x}, {0, x, 1}, {x, 0, 1}, {1, 0, x}};
    for (int i = 0; i < 3; i++) rgb[i] = table[sector][i];
}

// Split-toning colour: 1 plus half the hue's chroma (its colour minus its own luminance), so its luminance is exactly 1
// and multiplying by it shifts the colour without changing the brightness
void SplitToneColour(float hueDeg, float out[3]) {
    float c[3];
    HueColour(hueDeg, c);
    const float lum = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    for (int i = 0; i < 3; i++) out[i] = 1.0f + 0.5f * (c[i] - lum);
}

void ReadTimings(float& ms) {
    for (int i = 0; i < Gpu::kQ; i++) {
        if (!gpu.qIssued[i]) continue;
        BOOL disjoint = TRUE;
        UINT64 t0 = 0, t1 = 0, freq = 0;
        if (gpu.qDisjoint[i]->GetData(&disjoint, sizeof disjoint, 0) != S_OK || gpu.qBegin[i]->GetData(&t0, sizeof t0, 0) != S_OK ||
            gpu.qEnd[i]->GetData(&t1, sizeof t1, 0) != S_OK || gpu.qFreq[i]->GetData(&freq, sizeof freq, 0) != S_OK)
            continue;
        gpu.qIssued[i] = false;
        if (!disjoint && freq && t1 > t0) {
            const float v = static_cast<float>(double(t1 - t0) * 1000.0 / double(freq));
            ms = ms < 0 ? v : ms * 0.9f + v * 0.1f;
        }
    }
}

// The keys of [qol.picture] (the same as the combined build)
const char* const kKeys[] = {"enabled", "exposure", "contrast", "midtones", "shadows", "highlights", "blacks", "temperature", "tint", "saturation", "vibrance",
                             "shadow_hue", "shadow_tint", "highlight_hue", "highlight_tint", "mixer", "deband", "sharpen", "clarity", "vignette", "vignette_size"};

} // namespace

// ---- device ----

void Picture::ReleaseResources() {
    gpu.ready = false;
    SafeRelease(gpu.frameSurf);
    SafeRelease(gpu.sceneSurf);
    SafeRelease(gpu.frameTex);
    SafeRelease(gpu.sceneTex);
    for (int i = 0; i < Gpu::kChain; i++) {
        SafeRelease(gpu.chainSurf[i]);
        SafeRelease(gpu.chainTex[i]);
    }
    for (int i = 0; i < Gpu::kQ; i++) {
        SafeRelease(gpu.qDisjoint[i]);
        SafeRelease(gpu.qBegin[i]);
        SafeRelease(gpu.qEnd[i]);
        SafeRelease(gpu.qFreq[i]);
        gpu.qIssued[i] = false;
    }
}

// A Reset replaces the back buffer and sets render target 0 to it without a SetRenderTarget call: the remembered render
// target must be read again (it was: Picture on from the start stayed on the loading screen's back buffer after the game's
// Reset, never saw the scene on the back buffer, and changed nothing)
void Picture::BeforeReset() {
    ReleaseResources();
    gpu.curRT0 = nullptr;
    gpu.backBuffer = nullptr;
    gpu.frameReady = false;
}

bool Picture::InitResources(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    auto fail = [this](const std::string& why) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_resourceError = why;
        return false;
    };
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return fail("the back buffer could not be read");
    D3DSURFACE_DESC bd{};
    bb->GetDesc(&bd);
    bb->Release();
    gpu.width = bd.Width;
    gpu.format = bd.Format;
    gpu.height = bd.Height;
    HRESULT lastHr = S_OK;
    auto make = [&](UINT w, UINT h, IDirect3DTexture9** t, IDirect3DSurface9** s) {
        lastHr = dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, bd.Format, D3DPOOL_DEFAULT, t, nullptr);
        return SUCCEEDED(lastHr) && *t && SUCCEEDED((*t)->GetSurfaceLevel(0, s)) && *s;
    };
    bool ok = make(bd.Width, bd.Height, &gpu.frameTex, &gpu.frameSurf) && make(bd.Width, bd.Height, &gpu.sceneTex, &gpu.sceneSurf);
    UINT cw = bd.Width, ch = bd.Height;
    for (int i = 0; ok && i < Gpu::kChain; i++) {
        cw = std::max(1u, (cw + 1) / 2);
        ch = std::max(1u, (ch + 1) / 2);
        ok = make(cw, ch, &gpu.chainTex[i], &gpu.chainSurf[i]);
    }
    if (!ok) {
        ReleaseResources();
        return fail(std::format("its {}x{} render targets in the back buffer's format {} could not be created (0x{:08X})", bd.Width, bd.Height, static_cast<int>(bd.Format),
                                static_cast<unsigned>(lastHr)));
    }
    gpu.baseW = cw;
    gpu.baseH = ch;
    if (!gpu.compileTried) {
        gpu.compileTried = true;
        gpu.ps = CompileShader(dev);
    }
    if (!gpu.ps) {
        ReleaseResources();
        return fail("its shader could not be created");
    }
    for (int i = 0; i < Gpu::kQ; i++) {
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &gpu.qDisjoint[i]);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &gpu.qBegin[i]);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &gpu.qEnd[i]);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &gpu.qFreq[i]);
    }
    gpu.ready = true;
    LOG_INFO(std::format("[Picture] Resources ready ({}x{}, format {})", bd.Width, bd.Height, static_cast<int>(bd.Format)));
    return true;
}

// ---- end of frame ----

// Smooth gradients (deband) is on the Color page's Banding tab since 30/09 and follows the Banding Fix's switch, not
// Picture's: with Picture off the pass still runs, with only the deband (every other control neutral), while the Banding
// Fix is on. That smoothing-only pass is skipped in a frame without a copy of the scene (it would smooth the game's menus).
static PictureParams Effective(const PictureParams& q) {
    PictureParams e = q.enabled ? q : PictureParams{};
    e.deband = SceneDither::On() ? q.deband : 0.0f;
    e.enabled = q.enabled || e.deband > 0.001f;
    if (!q.enabled) e.compare = false;
    return e;
}

void Picture::BeforeOverlay(IDirect3DDevice9* dev) {
    // the frame ended on the scene (no game UI after it): the copy is the scene as it is now
    if (Effective(GetParams()).enabled && dev && gpu.frameReady && gpu.sceneDraws >= kMinSceneDraws && (gpu.lastWasScene || gpu.copyAfterStrip)) CopyScene(dev);
}

void Picture::OnEndScene(IDirect3DDevice9* dev) {
    const PictureParams raw = GetParams(), q = Effective(raw);
    if (!dev || !q.enabled) {
        m_gpuMs = -1.0f;
        // Off: the scene-copy hooks go too (they counted every back buffer draw each frame while off; registered again at
        // the next frame it is on, which reads the current render target)
        if (gpu.hooks) {
            D3D9Hooks::UnregisterAll(kHookName);
            gpu.hooks = false;
            gpu.curRT0 = nullptr;
            gpu.frameReady = false;
        }
        return;
    }
    RegisterHooks(dev);
    const unsigned long long now = GetTickCount64();
    if (dev != m_lastDevice) { // the game has two devices (a tiny one first): which one the frames end on
        if (m_lastDevice && m_deviceChanges++ < 10)
            LOG_INFO(std::format("[Picture] Frames now end on device {:#x} (was {:#x})", reinterpret_cast<uintptr_t>(dev), reinterpret_cast<uintptr_t>(m_lastDevice)));
        m_lastDevice = dev;
    }
    if (m_resetDiag.exchange(false)) {
        m_appliedLogged = false;
        m_loggedProblem.clear();
    }
    // frames resuming after a pause (loading screens, start-up): the 2 s count starts again
    if (now - m_lastEndScene.load() >= kProblemAfterMs) m_enabledAt.store(now);
    m_lastEndScene.store(now);
    Problem(false); // logs a new reason once
    if (!gpu.frameReady) { // once per frame (the game may end more than one scene)
        m_skip.store(kSkipNoFrame);
        return;
    }
    if (now < m_holdUntil.load()) return; // hold to compare: the original picture (the cost keeps its last value)
    IDirect3DSurface9 *bb = nullptr, *rt = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) {
        m_skip.store(kSkipNoBackBuffer);
        return;
    }
    dev->GetRenderTarget(0, &rt);
    const bool onBackBuffer = rt == bb;
    SafeRelease(rt);
    if (!onBackBuffer || (!gpu.ready && !InitResources(dev))) {
        m_skip.store(onBackBuffer ? kSkipResources : kSkipNotBackBuffer);
        bb->Release();
        return;
    }
    gpu.frameReady = false;
    if (!raw.enabled && !gpu.sceneCopied) { // smoothing only (Picture off): never over the game's menus
        bb->Release();
        return;
    }
    m_skip.store(kSkipNone);
    m_lastApplied.store(now);
    if (gpu.sceneCopied) m_lastSceneCopy.store(now);
    m_passes++;
    if (gpu.sceneCopied) m_passesWithScene++;
    if (now - m_lastStatus >= 60000) {
        if (m_lastStatus)
            LOG_INFO(std::format("[Picture] Last minute: {} passes, {} with the scene copy; {}x{} format {} on device {:#x}; pass check: {}; settings: {}", m_passes,
                                 m_passesWithScene, gpu.width, gpu.height, static_cast<int>(gpu.format), reinterpret_cast<uintptr_t>(dev),
                                 m_lastCheck.empty() ? "not run yet" : m_lastCheck, ParamsText(q)));
        m_lastStatus = now;
        m_passes = m_passesWithScene = 0;
    }
    {
        const bool tinted = MenusTinted();
        if (tinted != m_menusTintedLogged) {
            m_menusTintedLogged = tinted;
            LOG_INFO(tinted ? "[Picture] No copy of the scene before the UI for 2 s (the game does not draw its scene straight into the back buffer; "
                              "its own Edge Smoothing is the usual reason): the whole picture is filtered, the game's menus included"
                            : "[Picture] The scene is copied before the UI again: the game's menus keep their colours");
        }
    }
    if (!m_appliedLogged) {
        m_appliedLogged = true;
        m_loggedProblem.clear();
        LOG_INFO(std::format("[Picture] Applied to the game's picture ({}x{})", gpu.width, gpu.height));
    }

    ReadTimings(m_gpuMs);
    const int qi = gpu.qNext;
    const bool timed = !gpu.qIssued[qi] && gpu.qDisjoint[qi] && gpu.qBegin[qi] && gpu.qEnd[qi] && gpu.qFreq[qi];
    if (timed) {
        gpu.qDisjoint[qi]->Issue(D3DISSUE_BEGIN);
        gpu.qBegin[qi]->Issue(D3DISSUE_END);
    }
    dev->StretchRect(bb, nullptr, gpu.frameSurf, nullptr, D3DTEXF_NONE);
    // clarity: the scene (without the UI when the copy exists) reduced to 1/8 through 2x2 boxes
    const bool clarity = std::fabs(q.clarity) > 0.001f;
    if (clarity) {
        IDirect3DSurface9* src = gpu.sceneCopied ? gpu.sceneSurf : gpu.frameSurf;
        for (int i = 0; i < Gpu::kChain; i++) {
            dev->StretchRect(src, nullptr, gpu.chainSurf[i], nullptr, D3DTEXF_LINEAR);
            src = gpu.chainSurf[i];
        }
    }

    // save what the pass touches (the game continues from here next frame)
    constexpr DWORD kSamplers = 3;
    constexpr UINT kConsts = 13;
    constexpr D3DRENDERSTATETYPE kRS[] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
                                          D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_COLORWRITEENABLE};
    constexpr D3DSAMPLERSTATETYPE kSS[] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE};
    DWORD rs[std::size(kRS)], ss[kSamplers][std::size(kSS)];
    IDirect3DBaseTexture9* oldTex[kSamplers] = {};
    IDirect3DPixelShader9* oldPs = nullptr;
    IDirect3DVertexShader9* oldVs = nullptr;
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    IDirect3DVertexBuffer9* oldStream = nullptr;
    UINT oldOffset = 0, oldStride = 0;
    DWORD oldFvf = 0;
    float oldConst[kConsts * 4];
    D3DVIEWPORT9 oldVp{};
    for (size_t i = 0; i < std::size(kRS); i++) dev->GetRenderState(kRS[i], &rs[i]);
    for (DWORD s = 0; s < kSamplers; s++) {
        dev->GetTexture(s, &oldTex[s]);
        for (size_t i = 0; i < std::size(kSS); i++) dev->GetSamplerState(s, kSS[i], &ss[s][i]);
    }
    dev->GetPixelShader(&oldPs);
    dev->GetVertexShader(&oldVs);
    dev->GetVertexDeclaration(&oldDecl);
    dev->GetFVF(&oldFvf);
    dev->GetStreamSource(0, &oldStream, &oldOffset, &oldStride);
    dev->GetPixelShaderConstantF(0, oldConst, kConsts);
    dev->GetViewport(&oldVp);

    const D3DVIEWPORT9 vp{0, 0, gpu.width, gpu.height, 0.0f, 1.0f};
    dev->SetViewport(&vp);
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
        const DWORD filter = s == 2 ? D3DTEXF_LINEAR : D3DTEXF_POINT; // s2 (the 1/8 scene, B-spline taps) bilinear
        dev->SetSamplerState(s, D3DSAMP_MINFILTER, filter);
        dev->SetSamplerState(s, D3DSAMP_MAGFILTER, filter);
        dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, 0);
    }
    dev->SetTexture(0, gpu.frameTex);
    dev->SetTexture(1, gpu.sceneTex);
    dev->SetTexture(2, gpu.chainTex[Gpu::kChain - 1]);
    // white balance: a gentle red/blue tilt, normalised so white keeps its luminance
    const float tt = std::clamp(q.temperature, -1.0f, 1.0f) * 0.08f;
    const float wbG = 1.0f - std::clamp(q.tint, -1.0f, 1.0f) * 0.06f; // tint: + = magenta (less green), - = green
    const float wbR = 1.0f + tt, wbB = 1.0f - tt, wbL = 0.2126f * wbR + 0.7152f * wbG + 0.0722f * wbB;
    const float pxScale = static_cast<float>(gpu.height) / 2160.0f; // the deband radii were chosen at 4K
    float tintS[3], tintH[3];
    SplitToneColour(q.shadowHue, tintS);
    SplitToneColour(q.highlightHue, tintH);
    bool mixer = false;
    for (float m : q.mixer) mixer |= std::fabs(m - 1.0f) > 0.001f;
    auto mix = [&](int i) { return std::clamp(q.mixer[i], 0.0f, 2.0f); };
    const float W = static_cast<float>(gpu.width), H = static_cast<float>(gpu.height);
    const float c[kConsts][4] = {
        {std::clamp(q.saturation, 0.0f, 2.0f), gpu.sceneCopied ? 1.0f : 0.0f, q.compare ? 1.0f : 0.0f, 0},
        {1.0f / W, 1.0f / H, W, H},
        {std::exp2(std::clamp(q.exposure, -3.0f, 3.0f)), std::clamp(q.contrast, 0.5f, 1.8f), std::clamp(q.blacks, -1.0f, 1.0f) * 0.02f, 0},
        {wbR / wbL, wbG / wbL, wbB / wbL, 0},
        {std::clamp(q.deband, 0.0f, 2.0f) * 6.0f / 255.0f, 12.0f * pxScale, 32.0f * pxScale, q.deband > 0.001f ? 1.0f : 0.0f},
        {1.0f / std::clamp(q.midtones, 0.5f, 2.0f), std::clamp(q.shadows, -1.0f, 1.0f), std::clamp(q.highlights, -1.0f, 1.0f), std::clamp(q.vibrance, -1.0f, 1.0f)},
        {std::clamp(q.sharpen, 0.0f, 1.5f), clarity ? std::clamp(q.clarity, -1.0f, 1.0f) : 0.0f, 0, 0},
        {tintS[0], tintS[1], tintS[2], std::clamp(q.shadowTint, 0.0f, 1.0f)},
        {tintH[0], tintH[1], tintH[2], std::clamp(q.highlightTint, 0.0f, 1.0f)},
        {mix(0), mix(1), mix(2), mix(3)},
        {mix(4), mix(5), mixer ? 1.0f : 0.0f, 0},
        {std::clamp(q.vignette, 0.0f, 0.8f), std::clamp(q.vignetteSize, 0.0f, 0.95f), W / H, q.vignette > 0.001f ? 1.0f : 0.0f},
        {static_cast<float>(gpu.baseW), static_cast<float>(gpu.baseH), 1.0f / static_cast<float>(gpu.baseW), 1.0f / static_cast<float>(gpu.baseH)}};
    // the shader first, then its constants: a hook that looks at the bound shader to handle constants sees this one
    dev->SetPixelShader(gpu.ps);
    dev->SetPixelShaderConstantF(0, &c[0][0], kConsts);
    // After a settings change, read back what reached the device: another mod's hook could change the constants or the
    // shader on the way (a slider that moves but changes nothing)
    if (m_checkPasses.load(std::memory_order_relaxed) > 0) {
        m_checkPasses.fetch_sub(1, std::memory_order_relaxed);
        float back[kConsts * 4] = {};
        IDirect3DPixelShader9* bound = nullptr;
        const bool readOk = SUCCEEDED(dev->GetPixelShaderConstantF(0, back, kConsts));
        dev->GetPixelShader(&bound);
        std::string result;
        if (bound != gpu.ps) result = std::format("another pixel shader is bound for the pass ({:#x} instead of Apex's {:#x})", reinterpret_cast<uintptr_t>(bound), reinterpret_cast<uintptr_t>(gpu.ps));
        else if (!readOk) result = "the constants could not be read back";
        else
            for (UINT i = 0; i < kConsts * 4 && result.empty(); i++)
                if (back[i] != (&c[0][0])[i])
                    result = std::format("constant c{}.{} reached the device as {} instead of {} (another mod changed it)", i / 4, "xyzw"[i % 4], back[i], (&c[0][0])[i]);
        SafeRelease(bound);
        if (result.empty()) result = "ok";
        if (result != m_lastCheck) {
            m_lastCheck = result;
            if (result == "ok") LOG_INFO("[Picture] Pass check: its shader and settings reached the device as set");
            else LOG_WARNING("[Picture] Pass check: " + result);
        }
    }
    const float x1 = W - 0.5f, y1 = H - 0.5f;
    const QuadVertex v[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {x1, -0.5f, 0, 1, 1, 0}, {-0.5f, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVertex));

    for (size_t i = 0; i < std::size(kRS); i++) dev->SetRenderState(kRS[i], rs[i]);
    for (DWORD s = 0; s < kSamplers; s++) {
        dev->SetTexture(s, oldTex[s]);
        for (size_t i = 0; i < std::size(kSS); i++) dev->SetSamplerState(s, kSS[i], ss[s][i]);
        SafeRelease(oldTex[s]);
    }
    dev->SetPixelShaderConstantF(0, oldConst, kConsts);
    dev->SetPixelShader(oldPs);
    dev->SetVertexShader(oldVs);
    if (oldDecl) dev->SetVertexDeclaration(oldDecl);
    else dev->SetFVF(oldFvf);
    dev->SetStreamSource(0, oldStream, oldOffset, oldStride); // DrawPrimitiveUP clears stream 0
    dev->SetViewport(&oldVp);
    SafeRelease(oldPs);
    SafeRelease(oldVs);
    SafeRelease(oldDecl);
    SafeRelease(oldStream);
    bb->Release();
    if (timed) {
        gpu.qEnd[qi]->Issue(D3DISSUE_END);
        gpu.qFreq[qi]->Issue(D3DISSUE_END);
        gpu.qDisjoint[qi]->Issue(D3DISSUE_END);
        gpu.qIssued[qi] = true;
        gpu.qNext = (qi + 1) % Gpu::kQ;
    }
}

// ---- settings ----

void Picture::SetParams(const PictureParams& p, bool save) {
    bool switched;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        switched = m_p.enabled != p.enabled;
        m_p = p;
    }
    m_checkPasses.store(3, std::memory_order_relaxed);
    if (save) LOG_INFO("[Picture] Settings saved: " + ParamsText(p));
    if (switched) {
        m_enabledAt.store(GetTickCount64());
        m_resetDiag.store(true);
        LOG_INFO(p.enabled ? "[Picture] On" : "[Picture] Off");
    }
    if (save) ApexConfig::RequestSave();
}

void Picture::HoldBypass() { m_holdUntil.store(GetTickCount64() + 150); }

bool Picture::MenusTinted() const {
    if (!GetParams().enabled) return false;
    const unsigned long long now = GetTickCount64();
    const unsigned long long since = std::max(m_enabledAt.load(), m_lastSceneCopy.load());
    return now - m_lastApplied.load() < kProblemAfterMs && now - since >= kProblemAfterMs;
}

std::string Picture::Problem(bool translated) {
    if (!GetParams().enabled) return {};
    const unsigned long long now = GetTickCount64();
    const unsigned long long since = std::max(m_enabledAt.load(), m_lastApplied.load());
    if (now - since < kProblemAfterMs || now < m_holdUntil.load() + kProblemAfterMs) return {};
    auto tr = [translated](const char* s) -> std::string { return translated ? I18n::Tr(s) : s; };
    std::string why;
    if (now - m_lastEndScene.load() >= kProblemAfterMs) why = tr("the end of the game's frames does not reach it (EndScene)");
    else switch (m_skip.load()) {
        case kSkipNoFrame: why = tr("the frame boundary does not reach it (another mod may have taken over Present)"); break;
        case kSkipNoBackBuffer: why = tr("the game's back buffer could not be read"); break;
        case kSkipNotBackBuffer: why = tr("the game ends its frames on another render target"); break;
        case kSkipResources: {
            std::lock_guard<std::mutex> lock(m_mutex);
            why = m_resourceError.empty() ? tr("its resources could not be created") : m_resourceError; // technical detail: English
            break;
        }
        default: return {};
    }
    if (!translated) {
        if (why != m_loggedProblem) {
            m_loggedProblem = why;
            LOG_WARNING("[Picture] On, but not applied for 2 s: " + why);
        }
        return why;
    }
    if (m_resetDiag.exchange(false)) {
        m_appliedLogged = false;
        m_loggedProblem.clear();
    }
    Problem(false); // the log gets it in English too
    return I18n::Trf("Color is on but is not being applied: {}", why);
}

const char* const* Picture::Keys(size_t& count) {
    count = std::size(kKeys);
    return kKeys;
}

void Picture::SaveToToml(toml::table& qolTable) const {
    PictureParams q;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        q = m_p;
    }
    ParamsToToml(q, qolTable);
}

void Picture::ParamsToToml(const PictureParams& q, toml::table& qolTable) {
    toml::table pt;
    pt.insert("enabled", q.enabled);
    pt.insert("exposure", static_cast<double>(q.exposure));
    pt.insert("contrast", static_cast<double>(q.contrast));
    pt.insert("midtones", static_cast<double>(q.midtones));
    pt.insert("shadows", static_cast<double>(q.shadows));
    pt.insert("highlights", static_cast<double>(q.highlights));
    pt.insert("blacks", static_cast<double>(q.blacks));
    pt.insert("temperature", static_cast<double>(q.temperature));
    pt.insert("tint", static_cast<double>(q.tint));
    pt.insert("saturation", static_cast<double>(q.saturation));
    pt.insert("vibrance", static_cast<double>(q.vibrance));
    pt.insert("shadow_hue", static_cast<double>(q.shadowHue));
    pt.insert("shadow_tint", static_cast<double>(q.shadowTint));
    pt.insert("highlight_hue", static_cast<double>(q.highlightHue));
    pt.insert("highlight_tint", static_cast<double>(q.highlightTint));
    toml::array mixer;
    for (float m : q.mixer) mixer.push_back(static_cast<double>(m));
    pt.insert("mixer", std::move(mixer));
    pt.insert("deband", static_cast<double>(q.deband));
    pt.insert("sharpen", static_cast<double>(q.sharpen));
    pt.insert("clarity", static_cast<double>(q.clarity));
    pt.insert("vignette", static_cast<double>(q.vignette));
    pt.insert("vignette_size", static_cast<double>(q.vignetteSize));
    qolTable.insert_or_assign("picture", std::move(pt));
}

void Picture::LoadFromToml(const toml::table& qolTable) {
    PictureParams q;
    if (!ParamsFromToml(qolTable, q)) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_p = q;
}

bool Picture::ParamsFromToml(const toml::table& qolTable, PictureParams& out) {
    const toml::table* pic = qolTable["picture"].as_table();
    if (!pic) return false;
    const toml::table& t = *pic;
    PictureParams q;
    auto f = [&](const char* key, float& v) { v = static_cast<float>(t[key].value_or(static_cast<double>(v))); };
    q.enabled = t["enabled"].value_or(false);
    f("exposure", q.exposure);
    f("contrast", q.contrast);
    f("midtones", q.midtones);
    f("shadows", q.shadows);
    f("highlights", q.highlights);
    f("blacks", q.blacks);
    f("temperature", q.temperature);
    f("tint", q.tint);
    f("saturation", q.saturation);
    f("vibrance", q.vibrance);
    f("deband", q.deband);
    f("sharpen", q.sharpen);
    f("shadow_hue", q.shadowHue);
    f("shadow_tint", q.shadowTint);
    f("highlight_hue", q.highlightHue);
    f("highlight_tint", q.highlightTint);
    f("clarity", q.clarity);
    f("vignette", q.vignette);
    f("vignette_size", q.vignetteSize);
    if (auto a = t["mixer"].as_array())
        for (size_t i = 0; i < 6 && i < a->size(); i++) q.mixer[i] = static_cast<float>((*a)[i].value_or(1.0));
    q.compare = false;
    out = q;
    return true;
}

// ---- UI (menu: Image > Color, tabs Basic / Tones / Color / Detail) ----

namespace {
// A hue's full colour, for a slider swatch
ImU32 HueSwatch(float hueDeg) {
    float c[3];
    HueColour(hueDeg, c);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], 1.0f)); // the slider applies the style alpha
}
} // namespace

// Every row goes through ApexUi::Slider with its label as the stable id (unique within the tab's card).
void Picture::RenderUI(int tab) {
    using ApexUi::IconId;
    static const PictureParams kDef{}; // the defaults (changed dots and per-row Reset)
    PictureParams q = GetParams();
    bool changed = false, save = false;
    // Every slider applies live while dragging and is saved when the user lets go of it (SliderCommitted)
    auto slide = [&](const char* label, float* v, float lo, float hi, const ApexUi::SliderOptions& o) {
        if (ApexUi::Slider(label, v, lo, hi, o)) changed = true;
        save |= ApexUi::SliderCommitted();
    };
    auto percent = [&](const char* label, float* v, float lo, float hi, const char* desc, float def) {
        ApexUi::SliderOptions o;
        o.format = "%.0f%%";
        o.displayScale = 100.0f;
        o.tooltip = desc;
        o.defaultValue = def;
        slide(label, v, lo, hi, o);
    };
    // -100..+100 around 0, with optional end labels
    auto signedAmount = [&](const char* label, float* v, float def, const char* desc, const char* left = nullptr, const char* right = nullptr) {
        ApexUi::SliderOptions o;
        o.format = "%+.0f";
        o.displayScale = 100.0f;
        o.tooltip = desc;
        o.defaultValue = def;
        o.leftLabel = left;
        o.rightLabel = right;
        slide(label, v, -1.0f, 1.0f, o);
    };
    // A hue: the colour itself instead of a number, on a hue-circle track
    auto hue = [&](const char* label, float* v, float def, const char* desc) {
        ApexUi::SliderOptions o;
        o.valueText = "";
        o.swatch = HueSwatch(*v);
        o.tooltip = desc;
        o.hueTrack = true;
        o.defaultValue = def;
        slide(label, v, 0.0f, 360.0f, o);
    };

    if (!q.enabled) ImGui::BeginDisabled(); // visible but greyed out while Picture is off
    switch (tab) {
    case TabTones:
        percent("Midtones", &q.midtones, 0.6f, 1.6f, "Brighten or darken the middle tones; great for dark rooms", kDef.midtones);
        signedAmount("Shadows", &q.shadows, kDef.shadows, "Raise for more detail in dark areas, lower for a moodier look");
        signedAmount("Highlights", &q.highlights, kDef.highlights, "Lower to bring back detail in bright skies and lamps");
        signedAmount("Blacks", &q.blacks, kDef.blacks, "Soft, faded blacks or deep, pure blacks", "Faded", "Deeper");
        break;
    case TabColor: {
        signedAmount("Tint", &q.tint, kDef.tint, "Shift colors toward green or magenta", "Green", "Magenta");
        signedAmount("Vibrance", &q.vibrance, kDef.vibrance, "Boosts dull colors and keeps skin tones natural");
        ApexUi::GroupLabel("FILM TONES");
        hue("Shadow color", &q.shadowHue, kDef.shadowHue, "Teal or blue is the classic film look");
        percent("Shadow amount", &q.shadowTint, 0.0f, 1.0f, "How strongly shadows take that color; 0% is off", kDef.shadowTint);
        hue("Highlight color", &q.highlightHue, kDef.highlightHue, "Orange or gold is the classic film look");
        percent("Highlight amount", &q.highlightTint, 0.0f, 1.0f, "How strongly highlights take that color; 0% is off", kDef.highlightTint);
        ApexUi::GroupLabel("COLOR MIXER");
        static const char* const names[6] = {"Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas"};
        static const char* const descs[6] = {"Red flowers, brick and clothing", "Sunlight, sand and autumn leaves", "Grass and leaves; lower it for less neon plants",
                                             "Pools and pale skies", "Sky and water", "Pink and purple flowers and clothing"};
        static const float hues[6] = {0.0f, 60.0f, 120.0f, 180.0f, 240.0f, 300.0f};
        for (int i = 0; i < 6; i++) {
            ApexUi::SliderOptions o;
            o.format = "%.0f%%";
            o.displayScale = 100.0f;
            o.swatch = HueSwatch(hues[i]);
            o.tooltip = descs[i];
            o.defaultValue = kDef.mixer[i];
            slide(names[i], &q.mixer[i], 0.0f, 2.0f, o);
        }

        break;
    }
    case TabDetail: {
        signedAmount("Clarity", &q.clarity, kDef.clarity, "More texture and depth on surfaces, or softer; no halos", "Softer", "Crisper");
        ApexUi::SliderOptions o;
        o.format = "%.0f%%";
        o.displayScale = 125.0f; // 0..0.8 shown as 0..100%
        o.tooltip = "Darker corners that draw the eye inward; 0% is off";
        o.defaultValue = kDef.vignette;
        slide("Vignette", &q.vignette, 0.0f, 0.8f, o);
        percent("Vignette size", &q.vignetteSize, 0.0f, 0.95f, "Where the darkening starts; lower is closer to the center", kDef.vignetteSize);
        break;
    }
    default: {
        // Exposure is stored in stops; shown as the brightness it gives (0 = 100%, +1 = 200%, -1 = 50%)
        char exposure[16];
        std::snprintf(exposure, sizeof exposure, "%.0f%%", std::pow(2.0f, q.exposure) * 100.0f);
        ApexUi::SliderOptions o;
        o.valueText = exposure;
        o.tooltip = "Overall brightness of the world; menus stay the same";
        o.defaultValue = kDef.exposure;
        slide("Brightness", &q.exposure, -2.0f, 2.0f, o);
        percent("Contrast", &q.contrast, 0.6f, 1.5f, "Deeper darks and brighter brights; 100% is unchanged", kDef.contrast);
        percent("Saturation", &q.saturation, 0.0f, 1.6f, "How colorful everything is; 0% is black and white", kDef.saturation);
        signedAmount("Temperature", &q.temperature, kDef.temperature, "A cooler, bluer or warmer, more golden picture", "Cooler", "Warmer");
        percent("Sharpness", &q.sharpen, 0.0f, 1.5f, "Crisper fine detail without bright outlines; 0% is off", kDef.sharpen);
        break;
    }
    }

    if (!q.enabled) ImGui::EndDisabled();
    if (changed) SetParams(q, save);
}

void Picture::RenderDeveloperUI() {
    ImGui::TextDisabled("Runs on the 8-bit image with a fixed dither: the grading adds no banding.");
    if (m_gpuMs >= 0) ImGui::TextDisabled("GPU cost: %.2f ms per frame", m_gpuMs);
    else ImGui::TextDisabled("GPU cost: not measured yet (Picture off, or no frame drawn)");
}
