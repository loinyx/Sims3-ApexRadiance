// Edge Smoothing (FXAA)
// Post-process anti-aliasing of the 3D scene, applied right after the game finishes the scene and before it draws
// any UI (the same point as Depth Blur), so pie menus, tooltips and panels stay sharp.
//
// Cost, per frame:
//  - one StretchRect of the backbuffer into a texture of the same size (a plain copy, no filtering);
//  - one full-screen pass of FXAA (the "quality" algorithm of FXAA 3.11: edge detection on luma, search along the
//    edge, sub-pixel blend). Pixels with no local contrast leave after 5 texture reads (dynamic branch), which is most
//    of the screen; the edge search length is the quality setting.
//  - Triggered by PostScene (before Depth Blur).
//  - Only the states the pass touches are saved and restored (no state block). The render target is not changed.
// Native Edge Smoothing must be off: SMAA/FXAA and depth effects use the single-sample scene.

#include "patch_base.h"
#include "apex_version.h"
#include "memory_patch.h"
#include "apex_log.h"
#include "d3d9_bootstrap.h"
#include "d3d9_hooks.h"
#include "d3d9_extra_hooks.h"
#include "depth_share.h"
#include "render_callbacks.h"
#include "post_scene.h"
#include "shader_cache.h"
#include "imgui.h"
#include "ui/violet_theme.h"
#include "ui/widgets.h"
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>
#include "build_flavor.h"
#include "third_party/smaa/AreaTex.h"
#include "third_party/smaa/SearchTex.h"
#include "third_party/smaa/smaa_hlsl.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace {

constexpr const char* kHookName = "EdgeSmoothing";
constexpr int kRetryFrames = 120;

// FXAA "quality" (after FXAA 3.11 by Timothy Lottes, NVIDIA), written for ps_3_0 with tex2Dlod.
// STEPS and the step sizes come from the quality level (macros).
const char* kShaderSource = R"HLSL(
sampler2D sColor : register(s0);
sampler2D sDepthLog : register(s5); // log2 of the view distance (APEX_DEPTH_EDGES), point
float4 cRcp    : register(c0); // xy = 1 / screen size
float4 cParams : register(c1); // x = sub-pixel amount, y = edge threshold, z = edge threshold minimum, w = debug view
float4 cSharp  : register(c2); // x = texture sharpening 0..1 (pixels the smoothing left alone)

static const float kStep[STEPS] = { STEP_SIZES };

float Luma(float2 uv) { return dot(tex2Dlod(sColor, float4(uv, 0, 0)).rgb, float3(0.299, 0.587, 0.114)); }

// Contrast-adaptive sharpening of one pixel from its 4 neighbours (after AMD FidelityFX CAS, MIT): less where the
// neighbourhood is already contrasty or near black / white, so no halos
float3 Sharpen(float3 c, float3 n, float3 s, float3 w, float3 e, float amount)
{
    float3 mn = min(c, min(min(n, s), min(w, e)));
    float3 mx = max(c, max(max(n, s), max(w, e)));
    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
    float3 wgt = amp * (-1.0 / lerp(8.0, 5.0, amount));
    return saturate((c + (n + s + w + e) * wgt) / (1.0 + 4.0 * wgt));
}

// A pixel FXAA leaves as it is: sharpened when asked
float3 Unsmoothed(float2 pos, float3 c, float2 rcp)
{
    [branch] if (cSharp.x > 0.0)
    {
        float3 n = tex2Dlod(sColor, float4(pos - float2(0, rcp.y), 0, 0)).rgb, s = tex2Dlod(sColor, float4(pos + float2(0, rcp.y), 0, 0)).rgb;
        float3 w = tex2Dlod(sColor, float4(pos - float2(rcp.x, 0), 0, 0)).rgb, e = tex2Dlod(sColor, float4(pos + float2(rcp.x, 0), 0, 0)).rgb;
        return Sharpen(c, n, s, w, e, cSharp.x);
    }
    return c;
}

float4 FxaaPS(float2 pos : TEXCOORD0) : COLOR0
{
    const float2 rcp = cRcp.xy;
    float3 rgbM = tex2Dlod(sColor, float4(pos, 0, 0)).rgb;
    float lumaM = dot(rgbM, float3(0.299, 0.587, 0.114));
    float lumaS = Luma(pos + float2(0, rcp.y));
    float lumaE = Luma(pos + float2(rcp.x, 0));
    float lumaN = Luma(pos - float2(0, rcp.y));
    float lumaW = Luma(pos - float2(rcp.x, 0));
    float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    float range = rangeMax - rangeMin;
#ifdef APEX_DEPTH_EDGES
    // a step in the scene depth next to this pixel (log2 of the view distance, apart by more than DEPTH_STEP): the edge
    // of an object, smoothed down to a lower contrast (walls against walls of the same colour, night scenes)
    float dM = tex2Dlod(sDepthLog, float4(pos, 0, 0)).r;
    float4 dN4 = float4(tex2Dlod(sDepthLog, float4(pos - float2(0, rcp.y), 0, 0)).r, tex2Dlod(sDepthLog, float4(pos + float2(0, rcp.y), 0, 0)).r,
                        tex2Dlod(sDepthLog, float4(pos - float2(rcp.x, 0), 0, 0)).r, tex2Dlod(sDepthLog, float4(pos + float2(rcp.x, 0), 0, 0)).r);
    float depthScale = any(abs(dN4 - dM) > DEPTH_STEP) ? DEPTH_EDGE_SCALE : 1.0;
    [branch] if (range < max(cParams.z, rangeMax * cParams.y) * depthScale)
        return float4(Unsmoothed(pos, rgbM, rcp), 1);
#else
    [branch] if (range < max(cParams.z, rangeMax * cParams.y))
        return float4(Unsmoothed(pos, rgbM, rcp), 1);
#endif

    float lumaNW = Luma(pos - rcp);
    float lumaSE = Luma(pos + rcp);
    float lumaNE = Luma(pos + float2(rcp.x, -rcp.y));
    float lumaSW = Luma(pos + float2(-rcp.x, rcp.y));
    float lumaNS = lumaN + lumaS;
    float lumaWE = lumaW + lumaE;
    float lumaNESE = lumaNE + lumaSE;
    float lumaNWNE = lumaNW + lumaNE;
    float lumaNWSW = lumaNW + lumaSW;
    float lumaSWSE = lumaSW + lumaSE;
    float edgeHorz = abs(-2.0 * lumaW + lumaNWSW) + abs(-2.0 * lumaM + lumaNS) * 2.0 + abs(-2.0 * lumaE + lumaNESE);
    float edgeVert = abs(-2.0 * lumaS + lumaSWSE) + abs(-2.0 * lumaM + lumaWE) * 2.0 + abs(-2.0 * lumaN + lumaNWNE);
    bool horzSpan = edgeHorz >= edgeVert;

    // sub-pixel aliasing amount (thin features)
    float subpixB = ((lumaNS + lumaWE) * 2.0 + lumaNWSW + lumaNESE) * (1.0 / 12.0) - lumaM;
    float subpixC = saturate(abs(subpixB) / range);
    float subpixF = (-2.0 * subpixC + 3.0) * subpixC * subpixC;
    float subpixH = subpixF * subpixF * cParams.x;

    if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }
    float lengthSign = horzSpan ? rcp.y : rcp.x;
    float gradientN = lumaN - lumaM;
    float gradientS = lumaS - lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    float lumaNN = (pairN ? lumaN : lumaS) + lumaM;

    // search both ways along the edge for its ends
    float2 posB = pos;
    float2 offNP = horzSpan ? float2(rcp.x, 0) : float2(0, rcp.y);
    if (horzSpan) posB.y += lengthSign * 0.5; else posB.x += lengthSign * 0.5;
    float2 posN = posB - offNP * kStep[0];
    float2 posP = posB + offNP * kStep[0];
    float gradientScaled = gradient * 0.25;
    float lumaMM = lumaM - lumaNN * 0.5;
    float lumaEndN = Luma(posN) - lumaNN * 0.5;
    float lumaEndP = Luma(posP) - lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN) posN -= offNP * kStep[1];
    if (!doneP) posP += offNP * kStep[1];
    [unroll] for (int i = 2; i < STEPS; i++)
    {
        [branch] if (!doneN || !doneP)
        {
            if (!doneN) lumaEndN = Luma(posN) - lumaNN * 0.5;
            if (!doneP) lumaEndP = Luma(posP) - lumaNN * 0.5;
            doneN = doneN || abs(lumaEndN) >= gradientScaled;
            doneP = doneP || abs(lumaEndP) >= gradientScaled;
            if (!doneN) posN -= offNP * kStep[i];
            if (!doneP) posP += offNP * kStep[i];
        }
    }
    float dstN = horzSpan ? pos.x - posN.x : pos.y - posN.y;
    float dstP = horzSpan ? posP.x - pos.x : posP.y - pos.y;
    bool directionN = dstN < dstP;
    bool goodSpan = ((directionN ? lumaEndN : lumaEndP) < 0.0) != (lumaMM < 0.0);
    float pixelOffset = goodSpan ? 0.5 - min(dstN, dstP) / (dstN + dstP) : 0.0;
    float offset = max(pixelOffset, subpixH);
    if (horzSpan) pos.y += offset * lengthSign; else pos.x += offset * lengthSign;
    float3 rgb = tex2Dlod(sColor, float4(pos, 0, 0)).rgb;
    if (cParams.w > 0.5) rgb = lerp(rgb, float3(1, 0, 0), 0.6); // debug: the pixels FXAA touched
    return float4(rgb, 1);
}
)HLSL";

// Edge search: step sizes per quality level (FXAA 3.11 presets 12, 20-ish and 29).
struct QualityLevel {
    const char* steps;
    const char* sizes;
};
// Extreme (30/09, user: "the anti-aliasing still does not leave things perfectly straight, even on Ultra, in both modes"):
// finer first steps and a longer reach (57.5 texels each way against High's 30.5), for long, nearly straight edges at 4K.
constexpr int kFxaaLevels = 4;
constexpr QualityLevel kQualities[kFxaaLevels] = {
    {"5", "1.0, 1.5, 2.0, 4.0, 12.0"},
    {"8", "1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0"},
    {"12", "1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0"},
    {"16", "1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 4.0, 8.0, 8.0, 16.0"},
};

// ---- SMAA 1x (Jimenez, Echevarria, Masia, Navarro, Gutierrez 2012; reference code, MIT license, in third_party/smaa) ----
// Three passes: luma edge detection -> blending weights (searches along each edge and reads the precomputed area of the
// pattern it found, including diagonals and corners) -> neighbourhood blending. The reference SMAA.hlsl is embedded
// unchanged; only the wrapper below is ours. It computes in the pixel shader the offsets the reference computes in its
// vertex shaders (the effects here draw pre-transformed quads with no vertex shader).
const char* kSmaaPrefix = R"HLSL(
float4 cMetrics : register(c0); // 1/w, 1/h, w, h
float4 cParams  : register(c1); // x = threshold, w = debug view
#define SMAA_RT_METRICS cMetrics
#define SMAA_HLSL_3
#define SMAA_THRESHOLD cParams.x
)HLSL";
const char* kSmaaSuffix = R"HLSL(
sampler2D colorTex  : register(s0); // copy of the scene, linear
sampler2D edgesTex  : register(s1); // linear
sampler2D areaTex   : register(s2); // A8L8, linear
sampler2D searchTex : register(s3); // L8, point
sampler2D blendTex  : register(s4); // linear
sampler2D depthTex  : register(s5); // log2 of the view distance (predication), point

float4 SmaaEdgePS(float2 uv : TEXCOORD0) : COLOR0
{
    float4 offset[3];
    SMAAEdgeDetectionVS(uv, offset);
#if SMAA_PREDICATION
#ifdef APEX_SMAA_COLOR_EDGES
    return float4(SMAAColorEdgeDetectionPS(uv, offset, colorTex, depthTex), 0, 0);
#else
    return float4(SMAALumaEdgeDetectionPS(uv, offset, colorTex, depthTex), 0, 0);
#endif
#else
#ifdef APEX_SMAA_COLOR_EDGES
    return float4(SMAAColorEdgeDetectionPS(uv, offset, colorTex), 0, 0); // every channel: also edges of equal brightness
#else
    return float4(SMAALumaEdgeDetectionPS(uv, offset, colorTex), 0, 0);
#endif
#endif
}
float4 cSubsample : register(c2); // Spatial SMAA 1x uses zero subsample indices
float4 SmaaWeightPS(float2 uv : TEXCOORD0) : COLOR0
{
    float2 pixcoord;
    float4 offset[3];
    SMAABlendingWeightCalculationVS(uv, pixcoord, offset);
    return SMAABlendingWeightCalculationPS(uv, pixcoord, offset, edgesTex, areaTex, searchTex, cSubsample);
}
float4 SmaaBlendPS(float2 uv : TEXCOORD0) : COLOR0
{
    float4 offset;
    SMAANeighborhoodBlendingVS(uv, offset);
    float4 c = SMAANeighborhoodBlendingPS(uv, offset, colorTex, blendTex);
    [branch] if (cParams.w > 0.5 || cParams.y > 0.0)
    {
        float4 a = float4(tex2Dlod(blendTex, float4(offset.xy, 0, 0)).a, tex2Dlod(blendTex, float4(offset.zw, 0, 0)).g, tex2Dlod(blendTex, float4(uv, 0, 0)).xz);
        bool blended = dot(a, 1.0) > 1e-5;
        if (!blended && cParams.y > 0.0) // texture sharpening, only where SMAA left the pixel as it was (edges stay smooth)
        {
            float2 r = SMAA_RT_METRICS.xy;
            float3 n = tex2Dlod(colorTex, float4(uv - float2(0, r.y), 0, 0)).rgb, s = tex2Dlod(colorTex, float4(uv + float2(0, r.y), 0, 0)).rgb;
            float3 w = tex2Dlod(colorTex, float4(uv - float2(r.x, 0), 0, 0)).rgb, e = tex2Dlod(colorTex, float4(uv + float2(r.x, 0), 0, 0)).rgb;
            float3 mn = min(c.rgb, min(min(n, s), min(w, e))), mx = max(c.rgb, max(max(n, s), max(w, e)));
            float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
            float3 wgt = amp * (-1.0 / lerp(8.0, 5.0, cParams.y)); // after AMD FidelityFX CAS (MIT)
            c.rgb = saturate((c.rgb + (n + s + w + e) * wgt) / (1.0 + 4.0 * wgt));
        }
        if (blended && cParams.w > 0.5) c.rgb = lerp(c.rgb, float3(1, 0, 0), 0.6); // debug: the pixels SMAA blended
    }
    return float4(c.rgb, 1);
}
)HLSL";

// ---- Edges found by depth too (30/09): the scene depth (INTZ, shared by Depth Blur / AO) turned into log2 of the view
// distance, so a step between two pixels is a relative distance (an object in front of another), the same near and far.
// SMAA: the reference's predication (threshold lowered where the depth steps, SMAA_PREDICATION_SCALE 1 so textures keep
// the preset's threshold). FXAA: the same idea on its contrast test.
const char* kDepthLogSource = R"HLSL(
sampler2D sDepth : register(s0); // INTZ, point
float4 cDepth : register(c2);    // x = A, y = 1 / (near A): 1/z = (A - d) / (near A)
float4 LogDepthPS(float2 uv : TEXCOORD0) : COLOR0
{
    float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
    return d >= 0.99999 ? 16.0 : -log2(max(cDepth.x - d, 1e-7) * cDepth.y); // sky: 64 km
}
)HLSL";


constexpr const char* kDepthStep = "0.02";      // log2 units: a 1.4% jump in distance between neighbours = an object edge
constexpr const char* kDepthEdgeScale = "0.4"; // the contrast needed there (x the usual threshold)

// the reference presets (SMAA.hlsl, "SMAA Presets"), with the threshold as a shader constant
struct SmaaPreset {
    float threshold;
    const char* steps;
    const char* stepsDiag; // nullptr = diagonal and corner detection off
    bool colorEdges;       // the reference's colour edge detection instead of luma
};
// Extreme (30/09, beyond the reference presets, within its ranges): at 4K one step of a nearly horizontal edge (a roof, a
// floor line) can be longer than Ultra's reach (32 search steps, 2 pixels each, per side), and SMAA then leaves it jagged;
// 112 steps (the reference's maximum) and 20 diagonal steps (its maximum) follow such edges, and the colour edge detection
// also catches edges between colours of the same brightness, which the luma detection misses
constexpr int kSmaaLevels = 5;
constexpr SmaaPreset kSmaaPresets[kSmaaLevels] = {{0.15f, "4", nullptr, false}, {0.1f, "8", nullptr, false}, {0.1f, "16", "8", true},
                                                  {0.05f, "32", "16", true}, {0.05f, "112", "20", true}};

// ---- every variant compiled at start-up on a background thread (framework/shader_cache.h) ----
// The render thread only creates the shader objects from the kept bytecode (first use, and after ReleaseShaders).
// Priority 0 = the default FXAA quality (Balanced) and SMAA preset (High).
ShaderCache::Id AddFxaa(int q, const char* tag, bool depth = false) {
    ShaderCache::Desc d;
    d.tag = tag;
    d.source = kShaderSource;
    d.sourceName = "edge_smoothing.hlsl";
    d.entry = "FxaaPS";
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    d.macros = {{"STEPS", kQualities[q].steps}, {"STEP_SIZES", kQualities[q].sizes}};
    if (depth) {
        d.macros.emplace_back("APEX_DEPTH_EDGES", "1");
        d.macros.emplace_back("DEPTH_STEP", kDepthStep);
        d.macros.emplace_back("DEPTH_EDGE_SCALE", kDepthEdgeScale);
    }
    d.priority = q == 1 ? 0 : 1;
    return ShaderCache::Add(std::move(d));
}
ShaderCache::Id AddDepthLog() {
    ShaderCache::Desc d;
    d.tag = "EdgeSmoothing depth (log2)";
    d.source = kDepthLogSource;
    d.sourceName = "edge_smoothing_depth.hlsl";
    d.entry = "LogDepthPS";
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    d.priority = 0;
    return ShaderCache::Add(std::move(d));
}
// pass 0..2 = edges, weights, blend; pass 3 = the edge pass with depth predication
ShaderCache::Id AddSmaa(int q, int pass, const char* tag) {
    static const char* const kEntries[3] = {"SmaaEdgePS", "SmaaWeightPS", "SmaaBlendPS"};
    ShaderCache::Desc d;
    d.tag = tag;
    d.source = std::string(kSmaaPrefix) + reinterpret_cast<const char*>(kSmaaHlsl) + kSmaaSuffix;
    d.sourceName = "SMAA.hlsl";
    d.entry = kEntries[pass == 3 ? 0 : pass];
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    d.macros.emplace_back("SMAA_MAX_SEARCH_STEPS", kSmaaPresets[q].steps);
    if (pass == 3) {
        d.macros.emplace_back("SMAA_PREDICATION", "1");
        d.macros.emplace_back("SMAA_PREDICATION_THRESHOLD", kDepthStep);
        d.macros.emplace_back("SMAA_PREDICATION_SCALE", "1.0");
        d.macros.emplace_back("SMAA_PREDICATION_STRENGTH", "0.6"); // threshold x 0.4 where the depth steps
    }
    if (kSmaaPresets[q].stepsDiag) {
        d.macros.emplace_back("SMAA_MAX_SEARCH_STEPS_DIAG", kSmaaPresets[q].stepsDiag);
        d.macros.emplace_back("SMAA_CORNER_ROUNDING", "25");
    } else {
        d.macros.emplace_back("SMAA_DISABLE_DIAG_DETECTION", "1");
        d.macros.emplace_back("SMAA_DISABLE_CORNER_DETECTION", "1");
    }
    if (kSmaaPresets[q].colorEdges) d.macros.emplace_back("APEX_SMAA_COLOR_EDGES", "1");
    d.priority = q == 2 ? 0 : 1;
    return ShaderCache::Add(std::move(d));
}
const ShaderCache::Id kFxaaPsId[kFxaaLevels] = {AddFxaa(0, "EdgeSmoothing FXAA (Fast)"), AddFxaa(1, "EdgeSmoothing FXAA (Balanced)"), AddFxaa(2, "EdgeSmoothing FXAA (High)"),
                                                AddFxaa(3, "EdgeSmoothing FXAA (Extreme)")};
const ShaderCache::Id kSmaaPsId[kSmaaLevels][3] = {
    {AddSmaa(0, 0, "EdgeSmoothing SMAA edges (Low)"), AddSmaa(0, 1, "EdgeSmoothing SMAA weights (Low)"), AddSmaa(0, 2, "EdgeSmoothing SMAA blend (Low)")},
    {AddSmaa(1, 0, "EdgeSmoothing SMAA edges (Medium)"), AddSmaa(1, 1, "EdgeSmoothing SMAA weights (Medium)"), AddSmaa(1, 2, "EdgeSmoothing SMAA blend (Medium)")},
    {AddSmaa(2, 0, "EdgeSmoothing SMAA edges (High)"), AddSmaa(2, 1, "EdgeSmoothing SMAA weights (High)"), AddSmaa(2, 2, "EdgeSmoothing SMAA blend (High)")},
    {AddSmaa(3, 0, "EdgeSmoothing SMAA edges (Ultra)"), AddSmaa(3, 1, "EdgeSmoothing SMAA weights (Ultra)"), AddSmaa(3, 2, "EdgeSmoothing SMAA blend (Ultra)")},
    {AddSmaa(4, 0, "EdgeSmoothing SMAA edges (Extreme)"), AddSmaa(4, 1, "EdgeSmoothing SMAA weights (Extreme)"), AddSmaa(4, 2, "EdgeSmoothing SMAA blend (Extreme)")},
};
const ShaderCache::Id kSmaaDepthEdgeId[kSmaaLevels] = {AddSmaa(0, 3, "EdgeSmoothing SMAA edges + depth (Low)"), AddSmaa(1, 3, "EdgeSmoothing SMAA edges + depth (Medium)"),
                                                       AddSmaa(2, 3, "EdgeSmoothing SMAA edges + depth (High)"), AddSmaa(3, 3, "EdgeSmoothing SMAA edges + depth (Ultra)"),
                                                       AddSmaa(4, 3, "EdgeSmoothing SMAA edges + depth (Extreme)")};
const ShaderCache::Id kFxaaDepthId[kFxaaLevels] = {AddFxaa(0, "EdgeSmoothing FXAA + depth (Fast)", true), AddFxaa(1, "EdgeSmoothing FXAA + depth (Balanced)", true),
                                                   AddFxaa(2, "EdgeSmoothing FXAA + depth (High)", true), AddFxaa(3, "EdgeSmoothing FXAA + depth (Extreme)", true)};
const ShaderCache::Id kDepthLogId = AddDepthLog();

struct Params {
    int method = 1;           // 0 FXAA, 1 SMAA
    int quality = 1;          // FXAA: 0 fast, 1 balanced, 2 high
    int smaaQuality = 2;      // SMAA: 0 low, 1 medium, 2 high, 3 ultra, 4 extreme
    float subpix = 0.5f;      // sub-pixel smoothing (thin lines, texture detail): 0 = off, 1 = soft
    float sensitivity = 0.125f; // edge threshold: lower = more edges smoothed
    bool depthEdges = true;   // also find object edges in the scene depth (fainter edges of objects smoothed)
    float sharpen = 0.0f;     // texture sharpening of the pixels the smoothing left alone, 0..1 (0 = off)
    bool debugView = false;
};

struct AaState {
    bool active = false;
    bool ready = false;
    int retryCountdown = 0;
    unsigned framesSmoothed = 0;
    UINT width = 0, height = 0;
    IDirect3DTexture9* copyTex = nullptr;
    IDirect3DSurface9* copySurf = nullptr;
    IDirect3DPixelShader9* ps[kFxaaLevels] = {};
    bool compileTried[kFxaaLevels] = {};
    // SMAA
    IDirect3DTexture9 *edgesTex = nullptr, *blendTex = nullptr, *areaTex = nullptr, *searchTex = nullptr;
    IDirect3DSurface9 *edgesSurf = nullptr, *blendSurf = nullptr;
    IDirect3DPixelShader9* smaaPs[kSmaaLevels][3] = {};
    bool smaaTried[kSmaaLevels] = {};
    // depth edges
    IDirect3DTexture9* depthLogTex = nullptr;
    IDirect3DSurface9* depthLogSurf = nullptr;
    IDirect3DPixelShader9* psDepthLog = nullptr;
    IDirect3DPixelShader9* smaaDepthEdgePs[kSmaaLevels] = {};
    IDirect3DPixelShader9* fxaaDepthPs[kFxaaLevels] = {};
    bool depthShadersTried = false;
    bool depthRequested = false; // DepthShare::Request(true) held
    bool depthUsed = false;      // the last frame's pass used the depth
    unsigned framesWithDepth = 0;
    // GPU cost (timestamp queries, read a few frames later)
    static constexpr int kQ = 4;
    IDirect3DQuery9 *qDisjoint[kQ] = {}, *qBegin[kQ] = {}, *qEnd[kQ] = {}, *qFreq[kQ] = {};
    bool qIssued[kQ] = {};
    int qNext = 0, qMethod = -1;
    float gpuMs = -1.0f;
    bool resolveFailed = false; // stop unsupported MSAA resolves until reset / settings change
    bool gameAaOn = false; // the game's own multisampled Edge Smoothing is on: paused (menu warning)
    std::string status = "Waiting for the game...";
    Params p;
};

AaState g;
std::atomic<bool> g_settingsChanged{false};

template <typename T> void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

void ReleaseResources() {
    g.ready = false;
    g.resolveFailed = false;
    SafeRelease(g.copySurf);
    SafeRelease(g.copyTex);
    SafeRelease(g.edgesSurf);
    SafeRelease(g.blendSurf);
    SafeRelease(g.edgesTex);
    SafeRelease(g.blendTex);
    SafeRelease(g.areaTex);
    SafeRelease(g.searchTex);
    SafeRelease(g.depthLogSurf);
    SafeRelease(g.depthLogTex);
    for (int i = 0; i < AaState::kQ; i++) {
        SafeRelease(g.qDisjoint[i]);
        SafeRelease(g.qBegin[i]);
        SafeRelease(g.qEnd[i]);
        SafeRelease(g.qFreq[i]);
        g.qIssued[i] = false;
    }
}

void ReleaseShaders() {
    for (int i = 0; i < kFxaaLevels; i++) {
        SafeRelease(g.ps[i]);
        g.compileTried[i] = false;
    }
    for (int q = 0; q < kSmaaLevels; q++) {
        for (auto& ps : g.smaaPs[q]) SafeRelease(ps);
        g.smaaTried[q] = false;
        SafeRelease(g.smaaDepthEdgePs[q]);
    }
    for (auto& ps : g.fxaaDepthPs) SafeRelease(ps);
    SafeRelease(g.psDepthLog);
    g.depthShadersTried = false;
}

// The depth-edge shaders (all variants, created on first use from the precompiled bytecode). False when any is missing:
// the effect then runs without the depth.
bool DepthShaders(IDirect3DDevice9* dev) {
    if (!g.depthShadersTried) {
        g.depthShadersTried = true;
        auto make = [&](ShaderCache::Id id, IDirect3DPixelShader9** ps, const char* what) {
            std::string msg;
            if (ShaderCache::CreatePixelShader(dev, id, ps, &msg) == ShaderCache::Result::CompileFailed)
                LOG_ERROR(std::format("[EdgeSmoothing] {} failed to compile: {}", what, msg));
        };
        make(kDepthLogId, &g.psDepthLog, "LogDepthPS");
        for (int q = 0; q < kSmaaLevels; q++) make(kSmaaDepthEdgeId[q], &g.smaaDepthEdgePs[q], "SMAA edges + depth");
        for (int q = 0; q < kFxaaLevels; q++) make(kFxaaDepthId[q], &g.fxaaDepthPs[q], "FXAA + depth");
    }
    if (!g.psDepthLog) return false;
    for (auto* ps : g.smaaDepthEdgePs)
        if (!ps) return false;
    for (auto* ps : g.fxaaDepthPs)
        if (!ps) return false;
    return true;
}

// The three SMAA passes of one preset (created on first use from the precompiled bytecode)
bool SmaaShaders(IDirect3DDevice9* dev, int q) {
    q = std::clamp(q, 0, kSmaaLevels - 1);
    if (g.smaaTried[q]) return g.smaaPs[q][0] && g.smaaPs[q][1] && g.smaaPs[q][2];
    g.smaaTried[q] = true;
    const char* entries[3] = {"SmaaEdgePS", "SmaaWeightPS", "SmaaBlendPS"};
    for (int i = 0; i < 3; i++) {
        std::string msg;
        switch (ShaderCache::CreatePixelShader(dev, kSmaaPsId[q][i], &g.smaaPs[q][i], &msg)) {
        case ShaderCache::Result::Ok:
            break;
        case ShaderCache::Result::CompileFailed:
            LOG_ERROR(std::format("[EdgeSmoothing] SMAA {} (preset {}) failed to compile: {}", entries[i], q, msg));
            g.status = "ERROR: SMAA did not compile (see ApexRadiance_LOG.txt)";
            break;
        case ShaderCache::Result::CreateFailed:
            LOG_ERROR(std::format("[EdgeSmoothing] CreatePixelShader({}) failed", entries[i]));
            break;
        }
    }
    return g.smaaPs[q][0] && g.smaaPs[q][1] && g.smaaPs[q][2];
}

// The two precomputed SMAA textures, from the reference headers: areaTex R8G8 -> A8L8 (the shader reads .ra, the
// reference's DX9 layout), searchTex R8 -> L8
bool CreateSmaaLookups(IDirect3DDevice9* dev) {
    auto upload = [&](IDirect3DTexture9** tex, UINT w, UINT h, D3DFORMAT fmt, const unsigned char* bytes, UINT pitch) {
        if (FAILED(dev->CreateTexture(w, h, 1, 0, fmt, D3DPOOL_MANAGED, tex, nullptr)) || !*tex) return false;
        D3DLOCKED_RECT lr{};
        if (FAILED((*tex)->LockRect(0, &lr, nullptr, 0))) return false;
        for (UINT y = 0; y < h; y++) std::memcpy(static_cast<unsigned char*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, bytes + static_cast<size_t>(y) * pitch, pitch);
        (*tex)->UnlockRect(0);
        return true;
    };
    return upload(&g.areaTex, AREATEX_WIDTH, AREATEX_HEIGHT, D3DFMT_A8L8, areaTexBytes, AREATEX_PITCH) &&
           upload(&g.searchTex, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, D3DFMT_L8, searchTexBytes, SEARCHTEX_PITCH);
}

IDirect3DPixelShader9* ShaderFor(IDirect3DDevice9* dev, int q) {
    q = std::clamp(q, 0, kFxaaLevels - 1);
    if (g.ps[q] || g.compileTried[q]) return g.ps[q];
    g.compileTried[q] = true;
    std::string msg;
    switch (ShaderCache::CreatePixelShader(dev, kFxaaPsId[q], &g.ps[q], &msg)) { // precompiled at start-up (shader_cache.h)
    case ShaderCache::Result::Ok:
        break;
    case ShaderCache::Result::CompileFailed:
        LOG_ERROR(std::format("[EdgeSmoothing] Shader (quality {}) failed to compile: {}", q, msg));
        g.status = "ERROR: the shader did not compile (see ApexRadiance_LOG.txt)";
        break;
    case ShaderCache::Result::CreateFailed:
        LOG_ERROR("[EdgeSmoothing] CreatePixelShader failed");
        break;
    }
    return g.ps[q];
}

bool InitResources(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    D3DSURFACE_DESC bd{};
    bb->GetDesc(&bd);
    bb->Release();
    g.gameAaOn = bd.MultiSampleType != D3DMULTISAMPLE_NONE;
    if (g.gameAaOn) {
        g.status = "The game's Edge Smoothing is on: turn it off in Options > Graphics to use this one";
        return false;
    }
    g.width = bd.Width;
    g.height = bd.Height;
    if (FAILED(dev->CreateTexture(g.width, g.height, 1, D3DUSAGE_RENDERTARGET, bd.Format, D3DPOOL_DEFAULT, &g.copyTex, nullptr)) || !g.copyTex ||
        FAILED(g.copyTex->GetSurfaceLevel(0, &g.copySurf)) || !g.copySurf) {
        ReleaseResources();
        g.status = "ERROR: not enough video memory for the screen copy";
        return false;
    }
    auto make = [&](IDirect3DTexture9** tex, IDirect3DSurface9** surf) {
        return SUCCEEDED(dev->CreateTexture(g.width, g.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, tex, nullptr)) && *tex &&
               SUCCEEDED((*tex)->GetSurfaceLevel(0, surf)) && *surf;
    };
    if (!make(&g.edgesTex, &g.edgesSurf) || !make(&g.blendTex, &g.blendSurf) || !CreateSmaaLookups(dev)) {
        ReleaseResources();
        g.status = "ERROR: not enough video memory for SMAA";
        return false;
    }
    // the depth-edge target (optional: without it the effect runs on colour alone)
    if (!g.gameAaOn && (FAILED(dev->CreateTexture(g.width, g.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g.depthLogTex, nullptr)) || !g.depthLogTex ||
        FAILED(g.depthLogTex->GetSurfaceLevel(0, &g.depthLogSurf)) || !g.depthLogSurf)) {
        SafeRelease(g.depthLogSurf);
        SafeRelease(g.depthLogTex);
        LOG_WARNING("[EdgeSmoothing] No video memory for the depth-edge target: edges from colour only");
    }
    for (int i = 0; i < AaState::kQ; i++) {
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &g.qDisjoint[i]);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g.qBegin[i]);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g.qEnd[i]);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &g.qFreq[i]);
    }
    g.ready = true;
    g.status = "Active";
    LOG_INFO(std::format("[EdgeSmoothing] Resources ready ({}x{})", g.width, g.height));
    return true;
}

struct QuadVertex {
    float x, y, z, rhw, u, v;
};

// ---- minimal state save/restore (only what the pass touches) ----
constexpr D3DRENDERSTATETYPE kRenderStates[] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
                                                D3DRS_STENCILENABLE, D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE,
                                                D3DRS_COLORWRITEENABLE, D3DRS_MULTISAMPLEANTIALIAS, D3DRS_MULTISAMPLEMASK};
constexpr D3DSAMPLERSTATETYPE kSamplerStates[] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE,
                                                  D3DSAMP_MAXMIPLEVEL, D3DSAMP_MIPMAPLODBIAS};
constexpr int kRS = static_cast<int>(sizeof(kRenderStates) / sizeof(kRenderStates[0]));
constexpr int kSS = static_cast<int>(sizeof(kSamplerStates) / sizeof(kSamplerStates[0]));
constexpr UINT kPSConsts = 3;

// The depth-edge pass: the scene depth (INTZ, bound as the depth-stencil right now) -> log2 of the view distance in
// depthLogTex. True when it ran (the AA passes then read it at s5). Saves and restores everything it touches.
bool RunDepthPass(IDirect3DDevice9* dev) {
    if (!g.p.depthEdges || g.gameAaOn || !g.depthLogSurf) return false;
    IDirect3DTexture9* depth = DepthShare::Texture();
    if (!depth || !DepthShaders(dev)) return false;
    IDirect3DSurface9* ds = nullptr; // the scene depth must be the one bound now (not a reflection or UI pass), as AO checks
    ExtraHooks::RawGetDepthStencilSurface(dev, &ds);
    const bool sceneDepth = ds && ds == DepthShare::Surface();
    if (ds) ds->Release();
    if (!sceneDepth) return false;
    IDirect3DSurface9* oldRt = nullptr;
    IDirect3DPixelShader9* oldPs = nullptr;
    IDirect3DVertexShader9* oldVs = nullptr;
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    DWORD oldFvf = 0;
    IDirect3DVertexBuffer9* oldStream = nullptr;
    UINT oldOffset = 0, oldStride = 0;
    IDirect3DBaseTexture9* oldTex = nullptr;
    DWORD rs[kRS], ss[kSS];
    float oldConst[4];
    D3DVIEWPORT9 oldVp{};
    dev->GetRenderTarget(0, &oldRt);
    dev->GetPixelShader(&oldPs);
    dev->GetVertexShader(&oldVs);
    dev->GetVertexDeclaration(&oldDecl);
    dev->GetFVF(&oldFvf);
    dev->GetStreamSource(0, &oldStream, &oldOffset, &oldStride);
    dev->GetTexture(0, &oldTex);
    for (int i = 0; i < kRS; i++) dev->GetRenderState(kRenderStates[i], &rs[i]);
    for (int i = 0; i < kSS; i++) dev->GetSamplerState(0, kSamplerStates[i], &ss[i]);
    dev->GetPixelShaderConstantF(2, oldConst, 1);
    dev->GetViewport(&oldVp);

    dev->SetRenderTarget(0, g.depthLogSurf);
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
    dev->SetTexture(0, depth);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 0);
    dev->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
    dev->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);
    const float nearZ = PostScene::CameraNear() > 0.0f ? PostScene::CameraNear() : 0.25f, A = PostScene::CameraDepthA();
    const float c[4] = {A, 1.0f / (nearZ * A), 0, 0};
    dev->SetPixelShaderConstantF(2, c, 1);
    dev->SetPixelShader(g.psDepthLog);
    const float x1 = static_cast<float>(g.width) - 0.5f, y1 = static_cast<float>(g.height) - 0.5f;
    const QuadVertex v[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {x1, -0.5f, 0, 1, 1, 0}, {-0.5f, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVertex));

    dev->SetRenderTarget(0, oldRt); // resets the viewport, restored below
    dev->SetTexture(0, oldTex);
    for (int i = 0; i < kSS; i++) dev->SetSamplerState(0, kSamplerStates[i], ss[i]);
    for (int i = 0; i < kRS; i++) dev->SetRenderState(kRenderStates[i], rs[i]);
    dev->SetPixelShaderConstantF(2, oldConst, 1);
    dev->SetPixelShader(oldPs);
    dev->SetVertexShader(oldVs);
    if (oldDecl) dev->SetVertexDeclaration(oldDecl);
    else dev->SetFVF(oldFvf);
    dev->SetStreamSource(0, oldStream, oldOffset, oldStride); // DrawPrimitiveUP clears stream 0
    dev->SetViewport(&oldVp);
    SafeRelease(oldRt);
    SafeRelease(oldTex);
    SafeRelease(oldPs);
    SafeRelease(oldVs);
    SafeRelease(oldDecl);
    SafeRelease(oldStream);
    return true;
}

// Binds (or restores) sampler 5 for the depth-edge variants
struct DepthSampler {
    IDirect3DDevice9* dev;
    bool on;
    IDirect3DBaseTexture9* oldTex = nullptr;
    DWORD ss[kSS] = {};
    DepthSampler(IDirect3DDevice9* d, bool use) : dev(d), on(use) {
        if (!on) return;
        dev->GetTexture(5, &oldTex);
        for (int i = 0; i < kSS; i++) dev->GetSamplerState(5, kSamplerStates[i], &ss[i]);
        dev->SetTexture(5, g.depthLogTex);
        dev->SetSamplerState(5, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(5, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(5, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(5, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(5, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(5, D3DSAMP_SRGBTEXTURE, 0);
        dev->SetSamplerState(5, D3DSAMP_MAXMIPLEVEL, 0);
        dev->SetSamplerState(5, D3DSAMP_MIPMAPLODBIAS, 0);
    }
    ~DepthSampler() {
        if (!on) return;
        dev->SetTexture(5, oldTex);
        for (int i = 0; i < kSS; i++) dev->SetSamplerState(5, kSamplerStates[i], ss[i]);
        SafeRelease(oldTex);
    }
};

void RunFxaa(IDirect3DDevice9* dev, bool useDepth) {
    IDirect3DPixelShader9* ps = useDepth ? g.fxaaDepthPs[std::clamp(g.p.quality, 0, kFxaaLevels - 1)] : ShaderFor(dev, g.p.quality);
    if (!ps) return;
    DepthSampler depthSampler(dev, useDepth);
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    if (FAILED(dev->StretchRect(bb, nullptr, g.copySurf, nullptr, D3DTEXF_NONE))) {
        bb->Release();
        return;
    }
    // save
    IDirect3DPixelShader9* oldPs = nullptr;
    IDirect3DVertexShader9* oldVs = nullptr;
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    DWORD oldFvf = 0;
    IDirect3DVertexBuffer9* oldStream = nullptr;
    UINT oldOffset = 0, oldStride = 0;
    IDirect3DBaseTexture9* oldTex = nullptr;
    DWORD rs[kRS], ss[kSS];
    float oldConst[kPSConsts * 4];
    D3DVIEWPORT9 oldVp{};
    dev->GetPixelShader(&oldPs);
    dev->GetVertexShader(&oldVs);
    dev->GetVertexDeclaration(&oldDecl);
    dev->GetFVF(&oldFvf);
    dev->GetStreamSource(0, &oldStream, &oldOffset, &oldStride);
    dev->GetTexture(0, &oldTex);
    for (int i = 0; i < kRS; i++) dev->GetRenderState(kRenderStates[i], &rs[i]);
    for (int i = 0; i < kSS; i++) dev->GetSamplerState(0, kSamplerStates[i], &ss[i]);
    dev->GetPixelShaderConstantF(0, oldConst, kPSConsts);
    dev->GetViewport(&oldVp);

    // pass: copy -> backbuffer (the render target is already the backbuffer)
    const D3DVIEWPORT9 vp{0, 0, g.width, g.height, 0.0f, 1.0f};
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
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    dev->SetTexture(0, g.copyTex);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); // FXAA reads between texels on purpose
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 0);
    dev->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
    dev->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);
    const float c[kPSConsts][4] = {{1.0f / static_cast<float>(g.width), 1.0f / static_cast<float>(g.height), 0, 0},
                                   {g.p.subpix, g.p.sensitivity, g.p.sensitivity / 3.0f, (!kPublicBuild && g.p.debugView) ? 1.0f : 0.0f},
                                   {std::clamp(g.p.sharpen, 0.0f, 1.0f), 0, 0, 0}};
    dev->SetPixelShaderConstantF(0, &c[0][0], kPSConsts);
    dev->SetPixelShader(ps);
    const float x1 = static_cast<float>(g.width) - 0.5f, y1 = static_cast<float>(g.height) - 0.5f;
    const QuadVertex v[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {x1, -0.5f, 0, 1, 1, 0}, {-0.5f, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVertex));

    // restore
    dev->SetTexture(0, oldTex);
    for (int i = 0; i < kSS; i++) dev->SetSamplerState(0, kSamplerStates[i], ss[i]);
    for (int i = 0; i < kRS; i++) dev->SetRenderState(kRenderStates[i], rs[i]);
    dev->SetPixelShaderConstantF(0, oldConst, kPSConsts);
    dev->SetPixelShader(oldPs);
    dev->SetVertexShader(oldVs);
    if (oldDecl) dev->SetVertexDeclaration(oldDecl);
    else dev->SetFVF(oldFvf);
    dev->SetStreamSource(0, oldStream, oldOffset, oldStride); // DrawPrimitiveUP clears stream 0
    dev->SetViewport(&oldVp);
    SafeRelease(oldTex);
    SafeRelease(oldPs);
    SafeRelease(oldVs);
    SafeRelease(oldDecl);
    SafeRelease(oldStream);
    bb->Release();
    g.framesSmoothed++;
}

void DrawQuad(IDirect3DDevice9* dev) {
    const float x1 = static_cast<float>(g.width) - 0.5f, y1 = static_cast<float>(g.height) - 0.5f;
    const QuadVertex v[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {x1, -0.5f, 0, 1, 1, 0}, {-0.5f, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVertex));
}

// SMAA's three passes. Saves and restores what they touch (render target, samplers 0..4, shaders, constants c0..c1).
void RunSmaa(IDirect3DDevice9* dev, bool useDepth, const float subsample[4]) {
    const int q = std::clamp(g.p.smaaQuality, 0, kSmaaLevels - 1);
    if (!SmaaShaders(dev, q)) return;
    DepthSampler depthSampler(dev, useDepth);
    IDirect3DSurface9 *bb = nullptr, *oldRt = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    const HRESULT copyResult = dev->StretchRect(bb, nullptr, g.copySurf, nullptr, D3DTEXF_NONE);
    if (FAILED(copyResult)) {
        // Unsupported resolve: keep the native image and suspend instead of retrying every draw.
        g.status = "ERROR: could not copy the scene for SMAA";
        LOG_WARNING(std::format("[EdgeSmoothing] Scene copy/resolve failed ({:#x}); suspended until reset or settings change", static_cast<unsigned>(copyResult)));
        g.resolveFailed = true;
        bb->Release();
        return;
    }
    IDirect3DSurface9* oldDs = nullptr;
    dev->GetDepthStencilSurface(&oldDs);
    if (FAILED(dev->SetDepthStencilSurface(nullptr))) {
        SafeRelease(oldDs);
        bb->Release();
        return;
    }
    constexpr DWORD kSamplers = 5;
    IDirect3DPixelShader9* oldPs = nullptr;
    IDirect3DVertexShader9* oldVs = nullptr;
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    DWORD oldFvf = 0;
    IDirect3DVertexBuffer9* oldStream = nullptr;
    UINT oldOffset = 0, oldStride = 0;
    IDirect3DBaseTexture9* oldTex[kSamplers] = {};
    DWORD rs[kRS], ss[kSamplers][kSS];
    float oldConst[kPSConsts * 4];
    D3DVIEWPORT9 oldVp{};
    dev->GetRenderTarget(0, &oldRt);
    dev->GetPixelShader(&oldPs);
    dev->GetVertexShader(&oldVs);
    dev->GetVertexDeclaration(&oldDecl);
    dev->GetFVF(&oldFvf);
    dev->GetStreamSource(0, &oldStream, &oldOffset, &oldStride);
    for (DWORD s = 0; s < kSamplers; s++) {
        dev->GetTexture(s, &oldTex[s]);
        for (int i = 0; i < kSS; i++) dev->GetSamplerState(s, kSamplerStates[i], &ss[s][i]);
    }
    for (int i = 0; i < kRS; i++) dev->GetRenderState(kRenderStates[i], &rs[i]);
    dev->GetPixelShaderConstantF(0, oldConst, kPSConsts);
    dev->GetViewport(&oldVp);

    dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
    dev->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
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
    for (DWORD s = 0; s < kSamplers; s++) { // the reference: linear and clamp everywhere (the search texture read at texel centres)
        const DWORD f = s == 3 ? D3DTEXF_POINT : D3DTEXF_LINEAR;
        dev->SetSamplerState(s, D3DSAMP_MINFILTER, f);
        dev->SetSamplerState(s, D3DSAMP_MAGFILTER, f);
        dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, 0);
        dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
        dev->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0);
    }
    const float W = static_cast<float>(g.width), H = static_cast<float>(g.height);
    const float c[kPSConsts][4] = {{1.0f / W, 1.0f / H, W, H}, {kSmaaPresets[q].threshold, std::clamp(g.p.sharpen, 0.0f, 1.0f), 0, (!kPublicBuild && g.p.debugView) ? 1.0f : 0.0f}, {subsample[0], subsample[1], subsample[2], subsample[3]}};
    dev->SetPixelShaderConstantF(0, &c[0][0], kPSConsts);

    // 1. edges (the targets are cleared every frame, alpha too: the passes discard where there is nothing to do)
    dev->SetRenderTarget(0, g.edgesSurf);
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    dev->SetTexture(0, g.copyTex);
    dev->SetPixelShader(useDepth ? g.smaaDepthEdgePs[q] : g.smaaPs[q][0]);
    DrawQuad(dev);
    // 2. blending weights
    dev->SetRenderTarget(0, g.blendSurf);
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    dev->SetTexture(0, nullptr);
    dev->SetTexture(1, g.edgesTex);
    dev->SetTexture(2, g.areaTex);
    dev->SetTexture(3, g.searchTex);
    dev->SetPixelShader(g.smaaPs[q][1]);
    DrawQuad(dev);
    // 3. neighbourhood blending into the backbuffer (colour only: its alpha stays the game's)
    dev->SetRenderTarget(0, bb);
    dev->SetTexture(1, nullptr);
    dev->SetTexture(0, g.copyTex);
    dev->SetTexture(4, g.blendTex);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    dev->SetPixelShader(g.smaaPs[q][2]);
    DrawQuad(dev);

    // Restore the original target before its multisampled depth attachment.
    dev->SetRenderTarget(0, oldRt); // resets the viewport, restored below
    dev->SetDepthStencilSurface(oldDs);
    SafeRelease(oldDs);
    for (DWORD s = 0; s < kSamplers; s++) {
        dev->SetTexture(s, oldTex[s]);
        for (int i = 0; i < kSS; i++) dev->SetSamplerState(s, kSamplerStates[i], ss[s][i]);
        SafeRelease(oldTex[s]);
    }
    for (int i = 0; i < kRS; i++) dev->SetRenderState(kRenderStates[i], rs[i]);
    dev->SetPixelShaderConstantF(0, oldConst, kPSConsts);
    dev->SetPixelShader(oldPs);
    dev->SetVertexShader(oldVs);
    if (oldDecl) dev->SetVertexDeclaration(oldDecl);
    else dev->SetFVF(oldFvf);
    dev->SetStreamSource(0, oldStream, oldOffset, oldStride); // DrawPrimitiveUP clears stream 0
    dev->SetViewport(&oldVp);
    SafeRelease(oldRt);
    SafeRelease(oldPs);
    SafeRelease(oldVs);
    SafeRelease(oldDecl);
    SafeRelease(oldStream);
    bb->Release();
    g.framesSmoothed++;
}

// GPU time of the effect, from timestamp queries of an earlier frame (never waits); restarts when the method changes
void ReadTimings() {
    for (int i = 0; i < AaState::kQ; i++) {
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

// PostScene effect (order kEdgeSmoothing): before Depth Blur and the UI
void FxaaEffect(IDirect3DDevice9* dev) {
    if (!g.ready || g.resolveFailed || (g.gameAaOn)) return;
    const int method = g.p.method == 1 ? 1 : 0;
    const int key = method * 10 + (method ? g.p.smaaQuality : g.p.quality);
    if (key != g.qMethod) {
        g.qMethod = key;
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
    const bool depthRan = RunDepthPass(dev);
    const bool useDepth = depthRan && g.p.depthEdges;
    g.depthUsed = depthRan;
    if (depthRan) g.framesWithDepth++;
    static const float kNoSubsample[4] = {0, 0, 0, 0};
    if (method == 1) RunSmaa(dev, useDepth, kNoSubsample);
    else RunFxaa(dev, useDepth);
    if (timed) {
        g.qEnd[qi]->Issue(D3DISSUE_END);
        g.qFreq[qi]->Issue(D3DISSUE_END);
        g.qDisjoint[qi]->Issue(D3DISSUE_END);
        g.qIssued[qi] = true;
        g.qNext = (qi + 1) % AaState::kQ;
    }
}

void OnFrameBoundary(IDirect3DDevice9* dev) {
    if (!g.active) return;
    if (g_settingsChanged.exchange(false)) { g.retryCountdown = 0; g.resolveFailed = false; }
    // The shared depth is requested only for spatial depth edges.
    const bool wantDepth = g.p.depthEdges && !g.gameAaOn;
    if (wantDepth != g.depthRequested) {
        DepthShare::Request(wantDepth);
        g.depthRequested = wantDepth;
    }
    if (g.gameAaOn && g.ready) ReleaseResources();
    if (!g.ready && --g.retryCountdown <= 0) {
        g.retryCountdown = kRetryFrames;
        InitResources(dev);
    }
}

void OnPreReset(IDirect3DDevice9*) {
    if (!g.active) return;
    ReleaseResources();
    g.status = "Recreating after a video change...";
}

void OnPostReset(IDirect3DDevice9*) {
    if (!g.active) return;
    g.retryCountdown = 0;
}

} // namespace

class EdgeSmoothingPatch : public ApexPatch {
  public:
    EdgeSmoothingPatch() : ApexPatch("EdgeSmoothing", nullptr) {
        RegisterEnumSetting(&g.p.method, "metodo", 1, "SMAA: smoother long edges and sharp textures (3 passes). FXAA: lighter, a little blurrier.",
                            {"FXAA", "SMAA"});
        RegisterEnumSetting(&g.p.smaaQuality, "qualidadeSmaa", 2,
                            "SMAA: the reference presets, and Extreme. High and above also handle diagonals and corners; Ultra catches fainter edges "
                            "(good at night); Extreme follows very long edges (4K) and colour edges.",
                            {"Low", "Medium", "High", "Ultra", "Extreme"});
        RegisterEnumSetting(&g.p.quality, "qualidade", 1,
                            "FXAA: how far each edge is followed. Higher = smoother long, nearly straight edges, costs a little more.",
                            {"Fast", "Balanced", "High", "Extreme"});
        RegisterFloatSetting(&g.p.subpix, "suavidade", SettingWidget::Slider, 0.5f, 0.0f, 1.0f,
                             "Also smooths thin lines and sub-pixel detail. Higher = smoother, but textures get slightly softer.");
        RegisterFloatSetting(&g.p.sensitivity, "sensibilidade", SettingWidget::Slider, 0.125f, 0.063f, 0.333f,
                             "Minimum contrast for an edge to be smoothed. Lower = catches more edges (also in dark night scenes).");
        RegisterBoolSetting(&g.p.depthEdges, "depthEdges", true,
                            "Also finds the edges of objects from the scene depth, so edges with little contrast (walls against walls of the same colour, "
                            "night scenes) are smoothed too, while textures stay sharp.");
        RegisterFloatSetting(&g.p.sharpen, "sharpen", SettingWidget::Slider, 0.0f, 0.0f, 1.0f,
                             "Sharpens the textures the smoothing leaves alone (the smoothed edges stay smooth). 0% is off.");
        RegisterBoolSetting(&g.p.debugView, "debugView", false, "Show the smoothed pixels in red");
    }

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        using namespace D3D9Hooks;
        RegisterPresent(kHookName, [](DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
            OnFrameBoundary(ctx.device);
            return HookAction::Continue;
        }, Priority::First);
        RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
        RenderCallbacks::Add(RenderCallbacks::postReset, OnPostReset);
        PostScene::Add(PostScene::kEdgeSmoothing, FxaaEffect);
        g.active = true;
        g.retryCountdown = 0;
        g.status = "Waiting for the game...";
        isEnabled = true;
        LOG_INFO("[EdgeSmoothing] Installed");
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        lastError.clear();
        PostScene::Remove(FxaaEffect);
        g.active = false;
        if (g.depthRequested) DepthShare::Request(false);
        g.depthRequested = false;
        D3D9Hooks::UnregisterAll(kHookName);
        RenderCallbacks::Remove(RenderCallbacks::preReset, OnPreReset);
        RenderCallbacks::Remove(RenderCallbacks::postReset, OnPostReset);
        ReleaseResources();
        ReleaseShaders();
        g.status = "Off";
        g.gameAaOn = false;
        isEnabled = false;
        LOG_INFO("[EdgeSmoothing] Uninstalled");
        return true;
    }

    // Settings are read live every frame
    void Update() override { if (pendingReinstall) g_settingsChanged.store(true); pendingReinstall = false; }

    // Overview row and card header chip (the smoothing pass, timed with timestamp queries)
    float GpuCostMs() const override { return (isEnabled.load() && g.ready && g.gpuMs >= 0.0f) ? g.gpuMs : -1.0f; }
    const char* OverviewSummary() const override {
        static const char* const smaa[] = {"SMAA - Low", "SMAA - Medium", "SMAA - High", "SMAA - Ultra", "SMAA - Extreme"};
        static const char* const fxaa[] = {"FXAA - Fast", "FXAA - Balanced", "FXAA - High", "FXAA - Extreme"};
        return g.p.method == 1 ? smaa[std::clamp(g.p.smaaQuality, 0, 4)] : fxaa[std::clamp(g.p.quality, 0, 3)];
    }

    // The card's controls (menu: System > Display, Anti-aliasing tab). Settings are read live every frame; the change notice only
    // keeps the base class informed and saves.
    void RenderCustomUI() override {
        SAFE_IMGUI_BEGIN();
        using ApexUi::IconId;
        static const Params kDefaults{}; // the registered defaults (changed dots and per-row Reset)
        bool changed = false;
        if (g.status.rfind("ERROR: ", 0) == 0) ApexUi::IconNote(IconId::TriangleAlert, g.status.c_str() + 7, VioletTheme::kError);

        // Method: FXAA first; UI order matches the stored method values.
        static const char* const kMethods[] = {"FXAA (recommended)", "SMAA"};
        static const char* const kMethodTips[] = {"Lighter on your graphics card, a little blurrier", "Clean, smooth edges while textures stay sharp"};
        int method = g.p.method == 1 ? 1 : 0;
        if (ApexUi::SegmentedRow("Method", "FXAA is recommended for lower GPU cost; SMAA is an alternative", "##Method", &method, kMethods, 2, kMethodTips, nullptr, kDefaults.method)) {
            g.p.method = method;
            changed = true;
        }
        if (g.p.method == 1) {
            static const char* const kSmaa[] = {"Low", "Medium", "High", "Ultra", "Extreme"};
            static const char* const kSmaaTips[] = {"Fastest; smooths the clearest edges", "A good balance", "Also finds edges between different colors of similar brightness",
                                                    "Finds faint color edges too, useful at night", "Straightest long edges, best at 4K; costs the most"};
            changed |= ApexUi::SegmentedRow("Quality##Smaa", "Higher smooths more edges and costs a bit more", "##SmaaQuality", &g.p.smaaQuality, kSmaa, 5, kSmaaTips, nullptr, kDefaults.smaaQuality);
        } else {
            static const char* const kFxaa[] = {"Fast", "Balanced", "High", "Extreme"};
            static const char* const kFxaaTips[] = {"Fastest", "A good balance", "Smoother long edges; costs a little more", "Straightest long edges, best at 4K; costs the most"};
            changed |= ApexUi::SegmentedRow("Quality##Fxaa", "Higher smooths more edges and costs a bit more", "##FxaaQuality", &g.p.quality, kFxaa, 4, kFxaaTips, nullptr, kDefaults.quality);
            // FXAA's own tuning (SMAA uses its reference presets), shown with FXAA only
            changed |= ApexUi::SliderPercent("Softness", &g.p.subpix, 0.0f, 1.0f, "Also smooths thin lines; higher softens textures a bit", kDefaults.subpix);
            // Shown as 0-100% (higher catches fainter edges); stored as the edge threshold (lower catches more)
            constexpr float kHi = 0.333f, kLo = 0.063f;
            float sensitivity = (kHi - g.p.sensitivity) / (kHi - kLo);
            if (ApexUi::SliderPercent("Sensitivity", &sensitivity, 0.0f, 1.0f, "Higher catches fainter edges, also at night", (kHi - kDefaults.sensitivity) / (kHi - kLo))) {
                g.p.sensitivity = kHi - sensitivity * (kHi - kLo);
                changed = true;
            }
        }
        // both methods (30/09): object edges from the scene depth, and texture sharpening where nothing was smoothed
        changed |= ApexUi::SwitchRow("Edges from depth", &g.p.depthEdges, "Also smooths faint edges of objects, while textures stay sharp", kDefaults.depthEdges);
        if (g.p.depthEdges && g.ready && g.framesSmoothed > 60 && !g.depthUsed)
            ApexUi::IconNote(IconId::Info, "The scene depth is not available right now: edges come from colour only");
        changed |= ApexUi::SliderPercent("Sharpen textures", &g.p.sharpen, 0.0f, 1.0f, "Crisper textures; the smoothed edges stay smooth. 0% is off", kDefaults.sharpen);
        // 30/09 (user: "many players at 1080p showed the game very jagged", with this on): at 1200 lines or fewer, the driver's
        // own supersampling (render at a higher resolution, shown on the same screen) smooths what no post-process AA can
        // (thin rails, wires, leaves)
        if (g.height > 0 && g.height <= 1200) {
            ApexUi::IconNote(IconId::Info, "Smoothest at 1080p: NVIDIA DSR or AMD VSR with a higher game resolution");
            ApexUi::Tooltip("Turn it on in the NVIDIA Control Panel (DSR) or AMD Software (VSR), then pick 1440p or 4K in the game; "
                            "it costs more and the game's interface gets smaller");
        }

        if (changed) NotifySettingChanged();
    }

    // Developer page > Debug views
    void RenderDeveloperUI() override {
        SAFE_IMGUI_BEGIN();
        ImGui::TextWrapped("Status: %s", g.status.c_str());
        if (g.ready && g.gpuMs >= 0) ImGui::TextDisabled("GPU cost: %.2f ms per frame", g.gpuMs);
        bool changed = ImGui::Checkbox("Show smoothed pixels in red", &g.p.debugView);
        ApexUi::Tooltip("Tints every pixel the smoothing changed red, to see which edges it catches");
        if (g.ready) ImGui::TextDisabled("Frames smoothed: %u (with the scene depth %u)", g.framesSmoothed, g.framesWithDepth);
        if (changed) NotifySettingChanged();
    }
};

APEX_REGISTER_FEATURE(EdgeSmoothingPatch, {.displayName = "Edge Smoothing (SMAA / FXAA)",
                                    .description = "Smooths the jagged edges of the world while menus and text stay sharp. Works with the game's own "
                                                   "Edge Smoothing turned off. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                                    .category = "Graphics",
                                    .experimental = true,
                                             .enabledByDefault = true,
                                    .supportedVersions = VERSION_ALL,
                                    .technicalDetails = {"Runs before the first ZENABLE=FALSE backbuffer draw after the scene (bloom composite / UI start), like Depth Blur.",
                                                         "SMAA 1x: the reference SMAA.hlsl (MIT, third_party/smaa), luma edges -> blending weights (area/search textures) -> neighbourhood blending; presets Low..Ultra.",
                                                         "FXAA: one full-screen FXAA 3.11-style pass (luma edge detection, edge search, sub-pixel blend).",
                                                         "One StretchRect copy of the backbuffer; saves/restores only the states it touches. GPU cost measured with timestamp queries.",
                                                         "Native Edge Smoothing pauses the effect; keep it off for SMAA/FXAA and depth-based effects."}})
