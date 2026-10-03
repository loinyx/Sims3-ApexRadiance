// Ambient Occlusion (GTAO, full resolution, deterministic)
// Soft shade where things meet (under furniture, in corners, where walls meet the floor, around houses and trees),
// computed from the scene depth right after the game finishes the 3D scene and before bloom and the UI.
//
// Designed offline on saved frames of the game (lab gtaolab.cpp, 30/09; docs/features/ambient-occlusion.md) after the
// earlier AO lines were judged dotty, dirty and weak (docs/removed-features.md). What it keeps from the last one:
//  - Full resolution, no per-frame noise, no accumulation: the same depth gives the same result bit for bit (a still
//    camera never changes it), so nothing twinkles or trails. Half resolution was the root of every "micro dots" report.
//  - A 1/z pyramid (9 levels, R32F, padded to a multiple of 256 so each level is the exact 2x2 average): 1/z is linear
//    across the screen on a plane, so flat surfaces stay flat at every level. The march reads it bilinearly within the
//    nearest level: trilinear cost twice as much for the same look (30/09, 4K: 2.4 vs 4.5 ms, image 0.04 levels apart).
//  - A fixed 4x4 Bayer interleave of the slice angle and the step offset, cancelled by a depth-aware 4x4 box, then a tent.
// What is new (GTAO, Jimenez 2016 / XeGTAO form):
//  - 8 slices x 4 geometric steps per side, cosine-weighted visibility with the projected normal, summed as a ratio to
//    the unoccluded arc: a flat floor or wall gets exactly no shade (the old HBAO darkened 64% of an indoor frame, this
//    46%, all of it where things meet).
//  - Two horizons from the same samples: contact (0.6 m, strong) and large (2.0 m near / 2.5 m far, weaker, only what
//    the contact one does not already cover). Falloff in metres; the depth part of a distance counts 1.3x (thin objects
//    in front of a wall leave no halo on it). Isolated pixels (leaf edges, thin rails) fade out; faded 150-400 m.
//  - Composite (lab "d"): a small dead zone (faint shade dropped), multi-bounce per colour channel (bright surfaces
//    keep more light and their colour, no grey film), and lamp-lit / bright pixels keep part of their light.
// Passes per frame: depth -> 1/z, 8 downsamples, the AO pass, 4 blur passes (box H/V, tent H/V), one composite over a
// copy of the scene (colour write RGB only).

#include "patch_base.h"
#include "apex_version.h"
#include "apex_log.h"
#include "d3d9_bootstrap.h"
#include "d3d9_hooks.h"
#include "d3d9_extra_hooks.h"
#include "render_callbacks.h"
#include "depth_share.h"
#include "post_scene.h"
#include "shader_cache.h"
#include "imgui.h"
#include "ui/violet_theme.h"
#include "ui/widgets.h"
#include "build_flavor.h"
#include "apex_paths.h"
#include "map_view.h"
#include "scene_dither.h"
#include "shader_patches.h"
#include "sim_receiver_ids.h"
#include "sim_occlusion.h"
#include <unordered_map>
#include <atomic>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>
#include <format>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

namespace {

constexpr const char* kHookName = "AmbientOcclusion";
constexpr int kRetryFrames = 120;
constexpr int kLevels = 9; // pyramid levels (0 = full resolution)
constexpr UINT kPad = 1u << (kLevels - 1);
constexpr float kTanHalfFovY = 1.0f / 4.293f; // fallback until the camera is read (measured y scale)
constexpr float kPi = 3.14159265f;
// The lab's recommended set (gtaolab.cpp P{}), all in metres unless noted
constexpr float kContactRadius = 0.6f, kLargeNear = 2.0f, kLargeFar = 2.5f;     // Rc, Rl near / far
constexpr float kContactK = 1.2f, kLargeKNear = 0.5f, kLargeKFar = 0.8f;       // kc, kl near / far
constexpr float kNearZ = 20.0f, kFarZ = 40.0f;                                 // near set -> far set between these depths
constexpr float kThin = 0.3f;                                                   // depth part of a distance x (1 + thin)
constexpr float kFade0 = 150.0f, kFade1 = 400.0f;                              // shade fades out between these depths
// Map view (30/09, lab on 3 captures of the map: view 950-1800 m, one depth step 5-20 cm with near ~1 m): radii for
// houses and trees at that distance, no fade (AO shift 0.35-0.48 levels, as close up)
constexpr float kMapContact = 4.0f, kMapLarge = 15.0f, kMapFade0 = 5000.0f, kMapFade1 = 6000.0f;
constexpr float kFirstStep4K = 2.0f, kMipOffset = 2.0f, kMaxRadius = 0.30f;    // px at 4K, levels, fraction of H
constexpr float kIsoK = 1.0f, kIsoT = 0.01f;                                    // isolated-pixel fade
constexpr float kBlurTolerance = 0.03f;                                         // of z
constexpr float kDeadZone = 0.05f;                                              // composite: faint shade dropped
// Quality by stored index (the saved "qualidade": 0 Low, 1 Medium, 2 High as in 2.1.0, then 3 Ultra, 4 Very Low) and the
// order the menu shows them in
constexpr int kQualityCount = 5;
constexpr int kQualitySlices[kQualityCount] = {4, 6, 8, 12, 2};
constexpr int kQualityShown[kQualityCount] = {4, 0, 1, 2, 3}; // Very Low, Low, Medium, High, Ultra

const char* kShaderSource = R"HLSL(
#ifndef SLICES
#define SLICES 8
#endif
#define STEPS 4
sampler2D sDepth : register(s0); // INTZ scene depth, point
sampler2D sZ     : register(s1); // 1/z pyramid (1/m, 0 = sky), point (exact texel reads)
sampler2D sZt    : register(s2); // the same pyramid, bilinear within the nearest level (the march)
sampler2D sAo    : register(s3); // AO + 1/z (G16R16F), point
sampler2D sColor : register(s4); // copy of the finished scene, point
sampler2D sSim : register(s5); // Sim receiver device-depth mask, point
sampler2D sHair : register(s6); // blended Sim body/hair: signed device depth and source coverage
float4 cView  : register(c0);  // x = tanX, y = tanY, z = H / (2 tanY) (pixels per metre times z), w = max radius (px)
float4 cSize  : register(c1);  // xy = screen size, zw = pyramid level-0 size (padded)
float4 cMarch : register(c2);  // x = first step (px), y = mip offset, z = (1 + thin)^2, w = 1 / (fade1 - fade0)
float4 cRad   : register(c3);  // x = contact radius (m), y = large radius near, z = large radius far, w = fade1 (m)
float4 cK     : register(c4);  // x = contact strength, y = large near, z = large far, w = blur tolerance (of z)
float4 cBlend : register(c5);  // x = near-set depth, y = 1 / (far-set depth - near-set depth), z = isolated strength, w = isolated threshold
float4 cDir   : register(c6);  // blur: xy = step (uv), z = 0 box / 1 tent; downsample: xy = source texel size (uv)
float4 cLook  : register(c7);  // composite: x = dead zone, y = 1 / (1 - dead zone), z = keep lamp light, w = 1 shading only
float4 cPos   : register(c8);  // view position = z * (p * cPos.xy + cPos.zw, 1)
float4 cRot   : register(c9);  // x = cos(pi / SLICES), y = sin(pi / SLICES)
float4 cDepth : register(c10); // x = A, y = 1 / (near A)   (1/z = (A - d) / (near A)), z = composite grain (Banding Fix strength / 255, 0 = off), w = its grain phase
float4 cSim : register(c11); // body strength, opaque mask available, hair strength, maximum shade
float4 cSimView : register(c12); // mask preview, transparent hair mask available

static const float PI = 3.14159265;

// scene depth -> 1/z in 1/metres, sky 0
float4 LinearizePS(float2 uv : TEXCOORD0) : COLOR0
{
    float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
    return d >= 0.99999 ? 0.0 : max(cDepth.x - d, 1e-7) * cDepth.y;
}

// one pyramid level: the average 1/z of the 2x2 below, sky texels left out (sky only if all four are)
float4 DownPS(float2 uv : TEXCOORD0) : COLOR0
{
    float4 w = float4(tex2Dlod(sZ, float4(uv + cDir.xy * float2(-0.5, -0.5), 0, 0)).r,
                      tex2Dlod(sZ, float4(uv + cDir.xy * float2( 0.5, -0.5), 0, 0)).r,
                      tex2Dlod(sZ, float4(uv + cDir.xy * float2(-0.5,  0.5), 0, 0)).r,
                      tex2Dlod(sZ, float4(uv + cDir.xy * float2( 0.5,  0.5), 0, 0)).r);
    float n = dot(float4(w > 0.0), 1.0);
    return n > 0.0 ? dot(w, 1.0) / n : 0.0;
}

float3 PosF(float2 p, float z) { return z * float3(p * cPos.xy + cPos.zw, 1); }
float WAt(float2 p) { return tex2Dlod(sZ, float4(p / cSize.zw, 0, 0)).r; }
float Bayer2(float a, float b) { return 2.0 * abs(a - b) + b; }
float Bayer4(float2 q) { float2 lo = fmod(q, 2.0), hi = floor(fmod(q, 4.0) * 0.5); return 4.0 * Bayer2(lo.x, lo.y) + Bayer2(hi.x, hi.y); }
float ACos(float x) { return acos(clamp(x, -1, 1)); }  // exact: the fast fit biased the shade by 0.5% (lab check)
float4 ACos4(float4 x) { return acos(clamp(x, -1, 1)); }

// GTAO: SLICES slices (angle (s + b1) pi / SLICES) x STEPS geometric steps per side (offset from b2 plus a golden-ratio
// phase per half-slice), contact + large horizon per side from the same samples. Out: R = visibility, G = 1/z (blur).
float4 GtaoPS(float2 uv : TEXCOORD0) : COLOR0
{
    float2 pix = uv * cSize.xy;
    float w0 = WAt(pix);
    [branch] if (w0 <= 0.0) return float4(1, 0, 0, 0);
    float z = 1.0 / w0;
    [branch] if (z >= cRad.w) return float4(1, w0, 0, 0);
    float3 c = PosF(pix, z);
    // normal from the neighbour with the smaller depth difference on each axis
    float zr = 1.0 / max(WAt(pix + float2(1, 0)), 1e-9), zl = 1.0 / max(WAt(pix - float2(1, 0)), 1e-9);
    float zd = 1.0 / max(WAt(pix + float2(0, 1)), 1e-9), zu = 1.0 / max(WAt(pix - float2(0, 1)), 1e-9);
    float3 dx = abs(zr - z) < abs(zl - z) ? PosF(pix + float2(1, 0), zr) - c : c - PosF(pix - float2(1, 0), zl);
    float3 dy = abs(zd - z) < abs(zu - z) ? PosF(pix + float2(0, 1), zd) - c : c - PosF(pix - float2(0, 1), zu);
    float3 n = cross(dx, dy);
    float nl = length(n);
    [branch] if (nl < 1e-12) return float4(1, w0, 0, 0);
    n /= nl;
    n = dot(n, c) > 0 ? -n : n;
    float iso = max(min(abs(zr - z), abs(zl - z)), min(abs(zd - z), abs(zu - z))) * w0;
    float3 view = -normalize(c);
    float fz = saturate((z - cBlend.x) * cBlend.y);
    float Rl = lerp(cRad.y, cRad.z, fz), kl = lerp(cK.y, cK.z, fz);
    float rMax = min(Rl * cView.z * w0, cView.w);
    [branch] if (rMax < 1.5 * cMarch.x) return float4(1, w0, 0, 0);
    float lg = log2(rMax / cMarch.x) / STEPS;
    float lodAdd = log2(1.0 - exp2(-lg)) - cMarch.y + log2(cMarch.x);
    float2 q = floor(pix);
    float b1 = (Bayer4(q) + 0.5) / 16.0, b2 = (Bayer4(q.yx + float2(1, 2)) + 0.5) / 16.0;
    float2 fMul = float2(-1.0 / (0.615 * cRad.x), -1.0 / (0.615 * Rl));
    const float fAdd = 0.385 / 0.615 + 1.0;
    float2 om;
    sincos(b1 * (PI / SLICES), om.y, om.x);                     // first slice; the others by a fixed rotation
    float3 acc = 0;                                             // visibility contact, large, unoccluded
    [loop] for (int s = 0; s < SLICES; s++)
    {
        float3 dir = float3(om.x, -om.y, 0);
        float3 ortho = dir - view * dot(dir, view);
        float3 axis = normalize(cross(ortho, view));
        float3 projN = n - axis * dot(n, axis);
        float projLen = length(projN);
        float cosN = saturate(dot(projN, view) / max(projLen, 1e-6));
        float sgn = dot(ortho, projN) >= 0 ? 1 : -1;
        float nA = sgn * ACos(cosN), sn = sgn * sqrt(1 - cosN * cosN);
        float4 low = float4(-sn, -sn, sn, sn);                  // (contact, large) side 0, (contact, large) side 1
        float4 hz = low;
        float ph0 = frac(b2 + (2 * s) * 0.618034), ph1 = frac(b2 + (2 * s + 1) * 0.618034);
        [unroll] for (int j = 0; j < STEPS; j++)
        {
            float2 lr = (j + float2(ph0, ph1)) * lg;
            float2 r = cMarch.x * exp2(lr);
            float4 sp = pix.xyxy + float4(om * r.x, -om * r.y);
            float2 ws = float2(tex2Dlod(sZt, float4(sp.xy / cSize.zw, 0, lr.x + lodAdd)).r,
                               tex2Dlod(sZt, float4(sp.zw / cSize.zw, 0, lr.y + lodAdd)).r);
            // off-screen samples count as sky (no occlusion), like the lab
            ws *= float2(all(sp.xy >= 0) && all(sp.xy < cSize.xy), all(sp.zw >= 0) && all(sp.zw < cSize.xy));
            float3 d0 = PosF(sp.xy, 1.0 / max(ws.x, 1e-9)) - c, d1 = PosF(sp.zw, 1.0 / max(ws.y, 1e-9)) - c;
            float2 cc = float2(dot(d0, view), dot(d1, view)) * rsqrt(max(float2(dot(d0, d0), dot(d1, d1)), 1e-12));
            float2 dt = sqrt(float2(dot(d0.xy, d0.xy), dot(d1.xy, d1.xy)) + float2(d0.z * d0.z, d1.z * d1.z) * cMarch.z);
            float4 w = saturate(dt.xxyy * fMul.xyxy + fAdd);
            hz = max(hz, low + (cc.xxyy - low) * w);
        }
        // arcs for the 4 horizons: h0 = -acos(side 1), h1 = acos(side 0); cos(2h - n) by the double angle
        float4 hcs = hz.zwxy;                                   // (h0 contact, h0 large, h1 contact, h1 large) cosines
        float4 sg = float4(-1, -1, 1, 1);
        float4 h = sg * ACos4(hcs);
        float4 sh = sg * sqrt(saturate(1 - hcs * hcs));
        float4 c2 = (2 * hcs * hcs - 1) * cosN + (2 * sh * hcs) * sn;
        float4 arc = (cosN + 2 * h * sn - c2) * 0.25;
        acc += projLen * float3(arc.x + arc.z, arc.y + arc.w, cosN + nA * sn);
        om = float2(om.x * cRot.x - om.y * cRot.y, om.x * cRot.y + om.y * cRot.x);
    }
    float oC = saturate(1 - acc.x / max(acc.z, 1e-6)), oL = saturate(1 - acc.y / max(acc.z, 1e-6));
    float occ = cK.x * oC + kl * max(0, oL - oC);
    occ *= saturate((cRad.w - z) * cMarch.w);
    occ *= 1 - cBlend.z * saturate((iso - cBlend.w) / cBlend.w);
    return float4(saturate(1 - occ), w0, 0, 0);
}

// separable depth-aware filter: box (0.5 1 1 1 0.5: one of each interleave offset) or tent (1 2 3 2 1)
static const float BX[5] = { 0.5, 1.0, 1.0, 1.0, 0.5 };
static const float TT[5] = { 1.0, 2.0, 3.0, 2.0, 1.0 };
float4 BlurPS(float2 uv : TEXCOORD0) : COLOR0
{
    float2 c0 = tex2Dlod(sAo, float4(uv, 0, 0)).rg;
    [branch] if (c0.y <= 0.0) return float4(1, 0, 0, 0);
    float sum = 0, ws = 0;
    [unroll] for (int t = -2; t <= 2; t++)
    {
        float2 v = tex2Dlod(sAo, float4(uv + cDir.xy * t, 0, 0)).rg;
        float w = (cDir.z > 0.5 ? TT[t + 2] : BX[t + 2]) * saturate(1.0 - abs(c0.y / max(v.y, 1e-9) - 1.0) / cK.w);
        sum += v.x * w;
        ws += w;
    }
    return float4(ws > 0 ? sum / ws : c0.x, c0.y, 0, 0);
}

// Triangular noise in (-1, 1) of the pixel position (the Banding Fix's grain): the composite rounds to 8 bits again
float TriNoise(float2 p, float phase)
{
    float r = 2.0 * frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)) + phase)) - 1.0;
    return sign(r) * (1.0 - sqrt(1.0 - abs(r)));
}

// Jimenez 2016 multi-bounce: bright surfaces bounce light back into their own shade
float3 MultiBounce(float v, float3 a)
{
    float3 A = 2.0404 * a - 0.3324, B = -4.7951 * a + 0.6417, C = 2.7552 * a + 0.6903;
    return max(v, ((v * A + B) * v + C) * v);
}
// Developer capture: the device depth as it is (the lab reads it back as floats)
float4 DepthPS(float2 uv : TEXCOORD0) : COLOR0 { return tex2Dlod(sDepth, float4(uv, 0, 0)).r; }

float4 CompositePS(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0
{
    float3 col = tex2Dlod(sColor, float4(uv, 0, 0)).rgb;
    float ao = tex2Dlod(sAo, float4(uv, 0, 0)).r;
    float v = 1 - saturate((1 - ao - cLook.x) * cLook.y);       // dead zone for faint shade
    float sceneDepth = 0;
    [branch] if (cSim.y > 0.5 || cSimView.y > 0.5)
        sceneDepth = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
    float original = v;
    float3 coverage = 0;
    [branch] if (cSim.y > 0.5) {
        float2 receiver = tex2Dlod(sSim, float4(uv, 0, 0)).rg;
        if (abs(receiver.x) > 0 && abs(abs(receiver.x) - sceneDepth) <= 2.4e-7) {
            bool hair = receiver.x < 0;
            v = 1 - min((1 - original) * (hair ? cSim.z : cSim.x), cSim.w);
            coverage = hair ? float3(0, 1, 0) : float3(0, 0.5, 1);
        }
    }
    [branch] if (cSimView.y > 0.5) {
        float2 hair = tex2Dlod(sHair, float4(uv, 0, 0)).rg;
        // Blended Sim materials can be in front of the final scene depth.
        // Never extend coverage into neighbouring pixels or through foreground geometry.
        if (abs(hair.x) > 0 && abs(hair.x) <= sceneDepth + 2.4e-7) {
            float alpha = saturate(hair.y);
            bool isHair = hair.x < 0;
            v = lerp(v, 1 - min((1 - original) * (isHair ? cSim.z : cSim.x), cSim.w), alpha);
            coverage = lerp(coverage, isHair ? float3(0, 1, 0) : float3(0, 0.5, 1), alpha);
        }
    }
    if (cSimView.x > 0.5) return float4(coverage, 1);
    if (cLook.w > 0.5) return float4(v, v, v, 1);
    float3 alb = min(0.9, pow(max(col, 1e-6), 2.2));            // albedo guess from the lit colour
    float3 m = MultiBounce(v, alb);
    float luma = dot(col, float3(0.299, 0.587, 0.114));
    m = lerp(m, 1.0, saturate((luma - 0.35) * 2.5) * cLook.z);  // lamp-lit / bright pixels keep part of their light
    // grain (Banding Fix on): only where the shade changed the pixel (elsewhere the scene already has its own), with a
    // shifted pattern so it does not add up with the scene's grain
    float3 dm = 1.0 - m;
    float changed = saturate(max(dm.r, max(dm.g, dm.b)) * 50.0);
    return float4(col * m + TriNoise(vpos + float2(19.0, 47.0), cDepth.w) * cDepth.z * changed, 1);
}
)HLSL";

// Every variant is compiled at start-up on a background thread (framework/shader_cache.h)
ShaderCache::Id AddShader(const char* tag, const char* entry, int priority, const char* slices = nullptr) {
    ShaderCache::Desc d;
    d.tag = tag;
    d.source = kShaderSource;
    d.sourceName = "ambient_occlusion.hlsl";
    d.entry = entry;
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    if (slices) d.macros.emplace_back("SLICES", slices);
    d.priority = priority;
    return ShaderCache::Add(std::move(d));
}
const ShaderCache::Id kLinearPsId = AddShader("AO LinearizePS", "LinearizePS", 0);
const ShaderCache::Id kDownPsId = AddShader("AO DownPS", "DownPS", 0);
const ShaderCache::Id kBlurPsId = AddShader("AO BlurPS", "BlurPS", 0);
const ShaderCache::Id kCompositePsId = AddShader("AO CompositePS", "CompositePS", 0);
const ShaderCache::Id kDepthPsId = AddShader("AO DepthPS (Developer capture)", "DepthPS", 1);
const ShaderCache::Id kGtaoPsId[kQualityCount] = {AddShader("AO GtaoPS (Low, SLICES 4)", "GtaoPS", 1, "4"), AddShader("AO GtaoPS (Medium, SLICES 6)", "GtaoPS", 1, "6"),
                                                  AddShader("AO GtaoPS (High, SLICES 8)", "GtaoPS", 0, "8"), AddShader("AO GtaoPS (Ultra, SLICES 12)", "GtaoPS", 1, "12"),
                                                  AddShader("AO GtaoPS (Very Low, SLICES 2)", "GtaoPS", 1, "2")};
static_assert(kQualitySlices[0] == 4 && kQualitySlices[1] == 6 && kQualitySlices[2] == 8 && kQualitySlices[3] == 12 && kQualitySlices[4] == 2,
              "kGtaoPsId lists the SLICES of kQualitySlices");

struct Params {
    float strength = 1.68f; // user-approved default from the in-game configuration
    float reach = 1.3f; // user-approved default, scales the radii
    float protect = 0.38f; // lamp-lit / bright pixels keep this share of their light
    int quality = 2; // stored index into kQualitySlices (2 = High, 8 slices)
    bool inMapView = true; // the map view gets its own radii (else the shade fades out there, as it is far)
    float distance = 351.0f;
    float simStrength = 0.47f;
    bool simControls = false;
    float hairStrength = 0.38f;
    float simMaxShade = 0.47f;
    bool transparentHair = true;

};

struct State {
    bool active = false, ready = false, fixedTried = false;
    bool gtaoTried[kQualityCount] = {};
    bool showShade = false; // Advanced > Show the shade alone (not saved)
    bool showSimMask = false; // preview only, never persisted
    int retryCountdown = 0;
    UINT width = 0, height = 0, padW = 0, padH = 0;
    IDirect3DTexture9* zTex = nullptr; // pyramid, kLevels levels
    IDirect3DSurface9* zLevel[kLevels] = {};
    IDirect3DTexture9* tmp[kLevels] = {}; // one-level targets for levels 1.., copied into zTex
    IDirect3DSurface9* tmpSurf[kLevels] = {};
    IDirect3DTexture9 *aoA = nullptr, *aoB = nullptr, *colorTex = nullptr;
    IDirect3DSurface9 *aoASurf = nullptr, *aoBSurf = nullptr, *colorSurf = nullptr;
    IDirect3DPixelShader9 *psLinear = nullptr, *psDown = nullptr, *psBlur = nullptr, *psComposite = nullptr, *psDepth = nullptr;
    IDirect3DPixelShader9* psGtao[kQualityCount] = {};
    // GPU cost (timestamp queries, read a few frames later)
    static constexpr int kQ = 4;
    IDirect3DQuery9 *qDisjoint[kQ] = {}, *qBegin[kQ] = {}, *qEnd[kQ] = {}, *qFreq[kQ] = {};
    bool qIssued[kQ] = {};
    int qNext = 0, qKey = -1;
    float gpuMs = -1.0f;
    unsigned frames = 0;
    // last camera used (Developer read-out)
    float lastNear = 0, lastA = 0, lastTanX = 0, lastTanY = 0;
    bool lastCamera = false;
    // Developer: save the depth, colour and camera of the next frame (the lab's input)
    bool captureRequested = false;
    std::string captureNote;
    Params p;
    std::string status = "Off";
};
State g;
std::atomic<DWORD> simRenderThread{0};

struct SimMaskCopy {
    IDirect3DVertexShader9 *originalVs = nullptr, *vs = nullptr;
    IDirect3DPixelShader9 *originalPs = nullptr, *ps = nullptr;
    bool transparent = false;
};
struct SimMaskState {
    IDirect3DTexture9* texture = nullptr;
    IDirect3DSurface9* surface = nullptr;
    IDirect3DTexture9* hairTexture = nullptr;
    IDirect3DSurface9* hairSurface = nullptr;
    UINT width = 0, height = 0;
    bool cleared = false, failed = false;
    bool hairCleared = false;
    bool shaderKnown = false, shaderIsSim = false;
    unsigned draws = 0, lastDraws = 0, refused = 0;
    std::unordered_map<IDirect3DPixelShader9*, int> known;
    std::vector<SimMaskCopy> copies;
} simMask;

bool WantSimMask() {
    return g.p.simControls && (g.p.simStrength < 1 || g.p.hairStrength < 1 || g.p.simMaxShade < 1 || g.showSimMask);
}

template <typename T> void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

void ReleaseSimMask() {
    SafeRelease(simMask.surface); SafeRelease(simMask.texture);
    SafeRelease(simMask.hairSurface); SafeRelease(simMask.hairTexture);
    for (auto& [ps, accepted] : simMask.known) ps->Release();
    for (auto& copy : simMask.copies) {
        SafeRelease(copy.originalVs); SafeRelease(copy.originalPs);
        SafeRelease(copy.vs); SafeRelease(copy.ps);
    }
    simMask = {};
}

template <typename Shader> std::vector<DWORD> ReadSimShader(Shader* shader) {
    UINT bytes = 0;
    if (!shader || FAILED(shader->GetFunction(nullptr, &bytes)) || !bytes || bytes % 4 || bytes > 65536) return {};
    std::vector<DWORD> code(bytes / 4);
    if (FAILED(shader->GetFunction(code.data(), &bytes))) return {};
    return code;
}

int IsSimReceiver(IDirect3DPixelShader9* ps) {
    if (!ps) return 0;
    if (const auto it = simMask.known.find(ps); it != simMask.known.end()) return it->second;
    if (simMask.known.size() >= 2048) return false;
    const auto code = ReadSimShader(ps);
    const size_t bytes = code.size() * 4;
    const uint32_t hash = ShaderHash(code.data(), bytes);
    const bool accepted = std::any_of(std::begin(kSimReceiverPs), std::end(kSimReceiverPs),
                                     [&](const ShaderId& id) { return id.size == bytes && id.hash == hash; });
    const bool hair = accepted && std::any_of(std::begin(kSimHairReceiverPs), std::end(kSimHairReceiverPs),
                                     [&](const ShaderId& id) { return id.size == bytes && id.hash == hash; });
    const int kind = accepted ? (hair ? 2 : 1) : 0;
    simMask.known.emplace(ps, kind);
    ps->AddRef(); // prevent address reuse from inheriting a cached classification
    return kind;
}

SimMaskCopy* SimCopy(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps, bool hair, bool transparent) {
    for (auto& copy : simMask.copies)
        if (copy.originalVs == vs && copy.originalPs == ps && copy.transparent == transparent) return &copy;
    if (!vs || simMask.copies.size() >= 512) return nullptr;
    auto v = ReadSimShader(vs), p = ReadSimShader(ps);
    SimMaskCopy copy;
    copy.originalVs = vs; copy.originalPs = ps;
    copy.transparent = transparent;
    if (ShaderPatches::MakeAoReceiverMask(v, p, hair, transparent)) {
        if (FAILED(D3D9Hooks::CallOriginalCreateVertexShader(dev, v.data(), &copy.vs)) ||
            FAILED(D3D9Hooks::CallOriginalCreatePixelShader(dev, p.data(), &copy.ps))) {
            SafeRelease(copy.vs); SafeRelease(copy.ps);
        }
    }
    if (!copy.ps) ++simMask.refused;
    try { simMask.copies.push_back(copy); }
    catch (...) { SafeRelease(copy.vs); SafeRelease(copy.ps); throw; }
    vs->AddRef(); ps->AddRef();
    return &simMask.copies.back();
}

template <typename Draw> void RecordSimReceiver(IDirect3DDevice9* dev, Draw draw) {
    if (GetCurrentThreadId() != simRenderThread.load(std::memory_order_relaxed)) return;
    if (!g.active || !g.ready || g.p.strength <= 0 || !WantSimMask() || !simMask.surface || simMask.failed) return;
    if (simMask.shaderKnown && !simMask.shaderIsSim) return;
    IDirect3DPixelShader9* ps = nullptr;
    if (FAILED(dev->GetPixelShader(&ps)) || !ps) return;
    const int accepted = IsSimReceiver(ps);
    if (!accepted) { ps->Release(); return; }
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr, *bb = nullptr, *extra = nullptr;
    dev->GetRenderTarget(0, &rt);
    ExtraHooks::RawGetDepthStencilSurface(dev, &ds);
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    dev->GetRenderTarget(1, &extra);
    DWORD z = 0, write = 0, blend = 0;
    const bool stateRead = SUCCEEDED(dev->GetRenderState(D3DRS_ZENABLE, &z)) &&
                           SUCCEEDED(dev->GetRenderState(D3DRS_ZWRITEENABLE, &write)) &&
                           SUCCEEDED(dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend));
    bool transparent = false;
    if (stateRead && blend && (accepted != 2 || g.p.transparentHair) && simMask.hairSurface) {
        DWORD src = 0, dst = 0, op = 0;
        transparent = SUCCEEDED(dev->GetRenderState(D3DRS_SRCBLEND, &src)) && src == D3DBLEND_SRCALPHA &&
                      SUCCEEDED(dev->GetRenderState(D3DRS_DESTBLEND, &dst)) && dst == D3DBLEND_INVSRCALPHA &&
                      SUCCEEDED(dev->GetRenderState(D3DRS_BLENDOP, &op)) && op == D3DBLENDOP_ADD;
    }
    const bool main = stateRead && rt && rt == bb && ds && ds == DepthShare::Surface() && z && ((write && !blend) || transparent) && !extra;
    SafeRelease(bb); SafeRelease(extra);
    if (!main) { SafeRelease(rt); SafeRelease(ds); ps->Release(); return; }
    IDirect3DVertexShader9* vs = nullptr;
    dev->GetVertexShader(&vs);
    auto* copy = SimCopy(dev, vs, ps, accepted == 2, transparent);
    if (copy && copy->ps && copy->vs) {
        D3DVIEWPORT9 vp{};
        if (FAILED(dev->GetViewport(&vp)) || vp.MinZ != 0.0f || vp.MaxZ != 1.0f) {
            SafeRelease(vs); SafeRelease(ps); SafeRelease(rt); SafeRelease(ds); return;
        }
        constexpr D3DRENDERSTATETYPE states[] = {D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE,
            D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_COLORWRITEENABLE, D3DRS_STENCILWRITEMASK, D3DRS_SCISSORTESTENABLE,
            D3DRS_ZFUNC};
        DWORD saved[std::size(states)]{};
        for (size_t i = 0; i < std::size(states); ++i)
            if (FAILED(dev->GetRenderState(states[i], &saved[i]))) {
                SafeRelease(vs); SafeRelease(ps); SafeRelease(rt); SafeRelease(ds); return;
            }
        auto* target = transparent ? simMask.hairSurface : simMask.surface;
        bool& cleared = transparent ? simMask.hairCleared : simMask.cleared;
        bool ok = SUCCEEDED(D3D9Hooks::CallOriginalSetRenderTarget(dev, 0, target));
        if (!cleared && ok) {
            ok = SUCCEEDED(dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE)) &&
                 SUCCEEDED(dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0));
            cleared = ok;
        }
        ok = SUCCEEDED(dev->SetViewport(&vp)) && ok;
        // The original draw already wrote depth. LESS must accept its equal-depth replay;
        // keep every other comparison and restore the game's state afterwards.
        const DWORD replayZ = write && saved[8] == D3DCMP_LESS ? D3DCMP_LESSEQUAL : saved[8];
        const DWORD maskStates[] = {FALSE, FALSE, FALSE, FALSE, FALSE, 15, 0, saved[7], replayZ};
        for (size_t i = 0; i < std::size(states); ++i) ok = SUCCEEDED(dev->SetRenderState(states[i], maskStates[i])) && ok;
        ok = ok && SUCCEEDED(D3D9Hooks::CallOriginalSetVertexShader(dev, copy->vs)) &&
                   SUCCEEDED(D3D9Hooks::CallOriginalSetPixelShader(dev, copy->ps));
        if (ok) ok = SUCCEEDED(draw());
        D3D9Hooks::CallOriginalSetPixelShader(dev, ps);
        D3D9Hooks::CallOriginalSetVertexShader(dev, vs);
        D3D9Hooks::CallOriginalSetRenderTarget(dev, 0, rt);
        ExtraHooks::RawSetDepthStencilSurface(dev, ds);
        dev->SetViewport(&vp);
        for (size_t i = 0; i < std::size(states); ++i) dev->SetRenderState(states[i], saved[i]);
        if (ok) ++simMask.draws;
        else { simMask.failed = true; LOG_WARNING("[AO] Sim receiver mask disabled after a device failure"); }
    }
    SafeRelease(vs); SafeRelease(ps); SafeRelease(rt); SafeRelease(ds);
}

void ReleaseResources() {
    g.ready = false;
    ReleaseSimMask();
    for (int i = 0; i < kLevels; i++) {
        SafeRelease(g.zLevel[i]);
        SafeRelease(g.tmpSurf[i]);
        SafeRelease(g.tmp[i]);
    }
    SafeRelease(g.zTex);
    SafeRelease(g.aoASurf);
    SafeRelease(g.aoBSurf);
    SafeRelease(g.colorSurf);
    SafeRelease(g.aoA);
    SafeRelease(g.aoB);
    SafeRelease(g.colorTex);
    for (int i = 0; i < State::kQ; i++) {
        SafeRelease(g.qDisjoint[i]);
        SafeRelease(g.qBegin[i]);
        SafeRelease(g.qEnd[i]);
        SafeRelease(g.qFreq[i]);
        g.qIssued[i] = false;
    }
}

void ReleaseShaders() {
    SafeRelease(g.psLinear);
    SafeRelease(g.psDown);
    SafeRelease(g.psBlur);
    SafeRelease(g.psComposite);
    SafeRelease(g.psDepth);
    for (int q = 0; q < kQualityCount; q++) {
        SafeRelease(g.psGtao[q]);
        g.gtaoTried[q] = false;
    }
    g.fixedTried = false;
}

// Creates one pass from its precompiled bytecode
IDirect3DPixelShader9* CreateShader(IDirect3DDevice9* dev, ShaderCache::Id id, const char* entry) {
    IDirect3DPixelShader9* ps = nullptr;
    std::string msg;
    switch (ShaderCache::CreatePixelShader(dev, id, &ps, &msg)) {
    case ShaderCache::Result::Ok:
        return ps;
    case ShaderCache::Result::CompileFailed:
        LOG_ERROR(std::format("[AO] Shader {} failed to compile: {}", entry, msg));
        return nullptr;
    case ShaderCache::Result::CreateFailed:
        LOG_ERROR(std::format("[AO] CreatePixelShader({}) failed", entry));
        return nullptr;
    }
    return nullptr;
}

// The AO pass of one quality, created on first use
IDirect3DPixelShader9* GtaoShader(IDirect3DDevice9* dev, int q) {
    q = std::clamp(q, 0, kQualityCount - 1);
    if (g.psGtao[q] || g.gtaoTried[q]) return g.psGtao[q];
    g.gtaoTried[q] = true;
    g.psGtao[q] = CreateShader(dev, kGtaoPsId[q], "GtaoPS");
    if (!g.psGtao[q]) g.status = "ERROR: the shader did not compile (see ApexRadiance_LOG.txt)";
    return g.psGtao[q];
}

bool EnsureShaders(IDirect3DDevice9* dev) {
    const bool fixedOk = g.psLinear && g.psDown && g.psBlur && g.psComposite;
    if (!fixedOk && g.fixedTried) return false; // failed once: logged, not retried every frame
    g.fixedTried = true;
    if (!g.psLinear) g.psLinear = CreateShader(dev, kLinearPsId, "LinearizePS");
    if (!g.psDown) g.psDown = CreateShader(dev, kDownPsId, "DownPS");
    if (!g.psBlur) g.psBlur = CreateShader(dev, kBlurPsId, "BlurPS");
    if (!g.psComposite) g.psComposite = CreateShader(dev, kCompositePsId, "CompositePS");
    const bool ok = g.psLinear && g.psDown && g.psBlur && g.psComposite && GtaoShader(dev, g.p.quality);
    if (!ok) g.status = "ERROR: the shader did not compile (see ApexRadiance_LOG.txt)";
    return ok;
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
    if (ok && filter) ok = SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, adapterFmt, D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, fmt));
    d3d->Release();
    return ok;
}

bool InitResources(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    D3DSURFACE_DESC bd{};
    bb->GetDesc(&bd);
    bb->Release();
    if (bd.MultiSampleType != D3DMULTISAMPLE_NONE) {
        g.status = "Paused while the game's own Edge Smoothing is on";
        return false;
    }
    if (!FormatSupported(dev, D3DFMT_R32F, true) || !FormatSupported(dev, D3DFMT_G16R16F, false)) {
        g.status = "ERROR: the graphics card cannot use the float textures it needs";
        LOG_ERROR("[AO] R32F (filtered) or G16R16F render targets not supported");
        return false;
    }
    if (!EnsureShaders(dev)) return false;
    g.width = bd.Width;
    g.height = bd.Height;
    g.padW = (g.width + kPad - 1) / kPad * kPad;
    g.padH = (g.height + kPad - 1) / kPad * kPad;
    bool ok = SUCCEEDED(dev->CreateTexture(g.padW, g.padH, kLevels, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g.zTex, nullptr)) && g.zTex;
    for (int i = 0; ok && i < kLevels; i++) ok = SUCCEEDED(g.zTex->GetSurfaceLevel(i, &g.zLevel[i])) && g.zLevel[i];
    for (int i = 1; ok && i < kLevels; i++)
        ok = SUCCEEDED(dev->CreateTexture(g.padW >> i, g.padH >> i, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g.tmp[i], nullptr)) && g.tmp[i] &&
             SUCCEEDED(g.tmp[i]->GetSurfaceLevel(0, &g.tmpSurf[i])) && g.tmpSurf[i];
    auto make = [&](IDirect3DTexture9** tex, IDirect3DSurface9** surf, D3DFORMAT fmt) {
        return SUCCEEDED(dev->CreateTexture(g.width, g.height, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, tex, nullptr)) && *tex &&
               SUCCEEDED((*tex)->GetSurfaceLevel(0, surf)) && *surf;
    };
    ok = ok && make(&g.aoA, &g.aoASurf, D3DFMT_G16R16F) && make(&g.aoB, &g.aoBSurf, D3DFMT_G16R16F) && make(&g.colorTex, &g.colorSurf, bd.Format);
    if (!ok) {
        ReleaseResources();
        g.status = "ERROR: not enough video memory for the shade textures";
        LOG_ERROR(std::format("[AO] Could not create the textures ({}x{})", g.width, g.height));
        return false;
    }
    for (int i = 0; i < State::kQ; i++) {
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &g.qDisjoint[i]);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g.qBegin[i]);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g.qEnd[i]);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &g.qFreq[i]);
    }
    g.ready = true;
    g.status = "Active";
    LOG_INFO(std::format("[AO] Resources ready ({}x{}, pyramid {}x{})", g.width, g.height, g.padW, g.padH));
    return true;
}

struct QuadVertex {
    float x, y, z, rhw, u, v;
};
// a quad covering w x h pixels of the render target, uv 0..1
void DrawQuad(IDirect3DDevice9* dev, UINT w, UINT h) {
    const float x1 = static_cast<float>(w) - 0.5f, y1 = static_cast<float>(h) - 0.5f;
    const QuadVertex v[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {x1, -0.5f, 0, 1, 1, 0}, {-0.5f, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVertex));
}

// ---- minimal state save/restore (only what the passes touch) ----
constexpr D3DRENDERSTATETYPE kRenderStates[] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
                                                D3DRS_STENCILENABLE, D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE,
                                                D3DRS_CLIPPLANEENABLE, D3DRS_COLORWRITEENABLE};
constexpr D3DSAMPLERSTATETYPE kSamplerStates[] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE,
                                                  D3DSAMP_MAXMIPLEVEL, D3DSAMP_MIPMAPLODBIAS};
constexpr int kRS = static_cast<int>(sizeof(kRenderStates) / sizeof(kRenderStates[0]));
constexpr int kSS = static_cast<int>(sizeof(kSamplerStates) / sizeof(kSamplerStates[0]));
constexpr DWORD kSamplers = 7; // s5 opaque Sims, s6 transparent hair
constexpr UINT kPSConsts = 13; // c0..c12

struct SavedState {
    IDirect3DSurface9 *rt0 = nullptr, *ds = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    DWORD fvf = 0;
    IDirect3DVertexBuffer9* stream0 = nullptr;
    UINT stream0Offset = 0, stream0Stride = 0;
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
        if (decl) dev->SetVertexDeclaration(decl);
        else dev->SetFVF(fvf);
        dev->SetStreamSource(0, stream0, stream0Offset, stream0Stride); // DrawPrimitiveUP clears stream 0
        dev->SetViewport(&viewport);
        SafeRelease(rt0);
        SafeRelease(ds);
        SafeRelease(ps);
        SafeRelease(vs);
        SafeRelease(decl);
        SafeRelease(stream0);
        for (auto& t : tex) SafeRelease(t);
    }
};

void SetPassStates(IDirect3DDevice9* dev) {
    ExtraHooks::RawSetDepthStencilSurface(dev, nullptr); // the depth is sampled, so it must not be bound
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
        const bool march = s == 2; // sZt: the march's prefiltered reads, bilinear within the nearest level
        dev->SetSamplerState(s, D3DSAMP_MINFILTER, march ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        dev->SetSamplerState(s, D3DSAMP_MAGFILTER, march ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        dev->SetSamplerState(s, D3DSAMP_MIPFILTER, march ? D3DTEXF_POINT : D3DTEXF_NONE);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, 0);
        dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
        dev->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0);
    }
}

// GPU time of the passes, from timestamp queries of an earlier frame (never waits)
void ReadTimings() {
    for (int i = 0; i < State::kQ; i++) {
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

// ---- Developer capture (the lab's input, 30/09: AO in the map view) ----
// Writes Documents\...\Apex Radiance\Profundidade\profundidade_N_WxH.f32 (the device depth, floats, top-down rows),
// cor_N.bmp (the scene before the AO and the UI, 24-bit) and info_N.txt (camera + the VS constants of the last scene
// draw, in the "[i](x y z w)" form the lab reads). Three files per request, only on the Developer button.
int NextCaptureNumber(const std::filesystem::path& dir) {
    int best = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        int k = 0;
        if (std::sscanf(n.c_str(), "info_%d.txt", &k) == 1) best = std::max(best, k);
    }
    return best + 1;
}

void CaptureFrame(IDirect3DDevice9* dev, IDirect3DTexture9* depth, float nearZ, float A, float tanX, float tanY) {
    g.captureRequested = false;
    if (!g.psDepth) g.psDepth = CreateShader(dev, kDepthPsId, "DepthPS");
    if (!g.psDepth) {
        g.captureNote = "Not saved: the depth shader could not be created";
        return;
    }
    const UINT W = g.width, H = g.height;
    // the device depth into level 0 of the pyramid (the 1/z pass overwrites it right after)
    dev->SetRenderTarget(0, g.zLevel[0]);
    const D3DVIEWPORT9 vpScreen{0, 0, W, H, 0.0f, 1.0f};
    dev->SetViewport(&vpScreen);
    dev->SetTexture(0, depth);
    dev->SetPixelShader(g.psDepth);
    DrawQuad(dev, W, H);
    dev->SetTexture(0, nullptr);
    std::vector<float> d(size_t(W) * H);
    std::vector<uint8_t> bgr(size_t(W) * H * 3);
    bool ok = false;
    IDirect3DSurface9 *sysD = nullptr, *sysC = nullptr;
    D3DSURFACE_DESC cd{};
    g.colorSurf->GetDesc(&cd);
    if (SUCCEEDED(dev->CreateOffscreenPlainSurface(g.padW, g.padH, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &sysD, nullptr)) &&
        SUCCEEDED(dev->CreateOffscreenPlainSurface(W, H, cd.Format, D3DPOOL_SYSTEMMEM, &sysC, nullptr)) && SUCCEEDED(dev->GetRenderTargetData(g.zLevel[0], sysD)) &&
        SUCCEEDED(dev->GetRenderTargetData(g.colorSurf, sysC)) && (cd.Format == D3DFMT_X8R8G8B8 || cd.Format == D3DFMT_A8R8G8B8)) {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(sysD->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            for (UINT y = 0; y < H; y++) std::memcpy(&d[size_t(y) * W], static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, W * 4);
            sysD->UnlockRect();
            if (SUCCEEDED(sysC->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                for (UINT y = 0; y < H; y++) {
                    const uint8_t* row = static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch;
                    for (UINT x = 0; x < W; x++) std::memcpy(&bgr[(size_t(y) * W + x) * 3], row + x * 4, 3);
                }
                sysC->UnlockRect();
                ok = true;
            }
        }
    }
    SafeRelease(sysD);
    SafeRelease(sysC);
    if (!ok) {
        g.captureNote = "Not saved: the frame could not be read back";
        return;
    }
    float vs[256][4] = {};
    dev->GetVertexShaderConstantF(0, &vs[0][0], 256);
    const std::filesystem::path dir = std::filesystem::path(ApexPaths::ApexDirectory()) / "Profundidade";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const int n = NextCaptureNumber(dir);
    bool written = false;
    if (FILE* fd = _wfopen((dir / std::format(L"profundidade_{}_{}x{}.f32", n, W, H)).c_str(), L"wb")) {
        written = std::fwrite(d.data(), 4, d.size(), fd) == d.size();
        std::fclose(fd);
    }
    if (FILE* fc = _wfopen((dir / std::format(L"cor_{}.bmp", n)).c_str(), L"wb")) {
        const UINT row = (W * 3 + 3) & ~3u;
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof fh + sizeof ih;
        fh.bfSize = fh.bfOffBits + row * H;
        ih.biSize = sizeof ih;
        ih.biWidth = static_cast<LONG>(W);
        ih.biHeight = static_cast<LONG>(H); // bottom-up
        ih.biPlanes = 1;
        ih.biBitCount = 24;
        ih.biSizeImage = row * H;
        std::fwrite(&fh, sizeof fh, 1, fc);
        std::fwrite(&ih, sizeof ih, 1, fc);
        std::vector<uint8_t> line(row, 0);
        for (UINT y = 0; y < H; y++) {
            std::memcpy(line.data(), &bgr[size_t(H - 1 - y) * W * 3], size_t(W) * 3);
            std::fwrite(line.data(), 1, row, fc);
        }
        written = written && std::ferror(fc) == 0;
        std::fclose(fc);
    }
    if (FILE* fi = _wfopen((dir / std::format(L"info_{}.txt", n)).c_str(), L"w")) {
        std::fprintf(fi, "largura %u altura %u | tanX %.9g tanY %.9g | near %.6g A %.9g (%s) | map view %s | AO fade %.0f-%.0f m\n", W, H, tanX, tanY, nearZ, A,
                     g.lastCamera ? "camera read this frame" : "fallback", MapView::IsOpen() ? "open" : "closed", kFade0, kFade1);
        std::fprintf(fi, "VS c0..c255 no momento (ultimo desenho da cena), zeros omitidos:\n");
        for (int i = 0; i < 256; i++)
            if (vs[i][0] != 0 || vs[i][1] != 0 || vs[i][2] != 0 || vs[i][3] != 0) std::fprintf(fi, "[%d](%.9g %.9g %.9g %.9g)\n", i, vs[i][0], vs[i][1], vs[i][2], vs[i][3]);
        std::fclose(fi);
    } else {
        written = false;
    }
    g.captureNote = written ? std::format("Saved capture {} ({}x{}, map view {}) in Profundidade", n, W, H, MapView::IsOpen() ? "open" : "closed")
                            : "Not saved: the files could not be written";
    LOG_INFO("[AO] " + g.captureNote);
}

void RunAo(IDirect3DDevice9* dev, IDirect3DTexture9* depth, IDirect3DSurface9* bb, IDirect3DPixelShader9* psGtao) {
    SavedState saved;
    saved.Capture(dev);
    dev->StretchRect(bb, nullptr, g.colorSurf, nullptr, D3DTEXF_NONE);
    SetPassStates(dev);

    const float W = static_cast<float>(g.width), H = static_cast<float>(g.height);
    const float nearZ = PostScene::CameraNear() > 0.0f ? PostScene::CameraNear() : 0.25f, A = PostScene::CameraDepthA();
    float vp[4][4];
    float tanX = kTanHalfFovY * W / H, tanY = kTanHalfFovY;
    g.lastCamera = PostScene::CameraViewProj(vp);
    if (g.lastCamera) {
        const float px = std::sqrt(vp[0][0] * vp[0][0] + vp[0][1] * vp[0][1] + vp[0][2] * vp[0][2]);
        const float py = std::sqrt(vp[1][0] * vp[1][0] + vp[1][1] * vp[1][1] + vp[1][2] * vp[1][2]);
        if (px > 1e-6f && py > 1e-6f) {
            tanX = 1.0f / px;
            tanY = 1.0f / py;
        }
    }
    g.lastNear = nearZ;
    g.lastA = A;
    g.lastTanX = tanX;
    g.lastTanY = tanY;
    const int slices = kQualitySlices[std::clamp(g.p.quality, 0, kQualityCount - 1)];
    const float s = std::clamp(g.p.strength, 0.0f, 2.0f), reach = std::clamp(g.p.reach, 0.5f, 2.0f);
    const bool map = g.p.inMapView && MapView::IsOpen();
    const float distance = std::clamp(g.p.distance, 25.0f, 1000.0f);
    const float fadeStart = distance * (kFade0 / kFade1);
    const float c[kPSConsts][4] = {
        {tanX, tanY, H / (2.0f * tanY), kMaxRadius * H},
        {W, H, static_cast<float>(g.padW), static_cast<float>(g.padH)},
        {kFirstStep4K * H / 2160.0f, kMipOffset, (1.0f + kThin) * (1.0f + kThin), 1.0f / (map ? kMapFade1 - kMapFade0 : distance - fadeStart)},
        {(map ? kMapContact : kContactRadius) * reach, (map ? kMapLarge : kLargeNear) * reach, (map ? kMapLarge : kLargeFar) * reach, map ? kMapFade1 : distance},
        {kContactK * s, kLargeKNear * s, kLargeKFar * s, kBlurTolerance},
        {kNearZ, 1.0f / (kFarZ - kNearZ), kIsoK, kIsoT},
        {0, 0, 0, 0},
        {kDeadZone, 1.0f / (1.0f - kDeadZone), std::clamp(g.p.protect, 0.0f, 1.0f), g.showShade ? 1.0f : 0.0f},
        {2.0f * tanX / W, -2.0f * tanY / H, -tanX, tanY},
        {std::cos(kPi / slices), std::sin(kPi / slices), 0, 0},
        {A, 1.0f / (nearZ * A), SceneDither::On() ? SceneDither::Strength() / 255.0f : 0.0f, SceneDither::GrainPhase()},
        {std::clamp(g.p.simStrength, 0.0f, 1.0f), WantSimMask() && simMask.cleared && !simMask.failed ? 1.0f : 0.0f,
         std::clamp(g.p.hairStrength, 0.0f, 1.0f), std::clamp(g.p.simMaxShade, 0.0f, 1.0f)},
        {g.p.simControls && g.showSimMask ? 1.0f : 0.0f, WantSimMask() && simMask.hairCleared && !simMask.failed ? 1.0f : 0.0f, 0, 0}};
    dev->SetPixelShaderConstantF(0, &c[0][0], kPSConsts);

    if (!kPublicBuild)
        if (g.captureRequested) CaptureFrame(dev, depth, nearZ, A, tanX, tanY);

    // 1. 1/z into level 0 of the pyramid (the screen area of it; the padding stays sky = 0 from the clear)
    dev->SetRenderTarget(0, g.zLevel[0]);
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    const D3DVIEWPORT9 vpScreen{0, 0, g.width, g.height, 0.0f, 1.0f};
    dev->SetViewport(&vpScreen);
    dev->SetTexture(0, depth);
    dev->SetPixelShader(g.psLinear);
    DrawQuad(dev, g.width, g.height);
    dev->SetTexture(0, nullptr);

    // 2. levels 1..8: each rendered from the one below into a one-level target, then copied into the pyramid
    dev->SetPixelShader(g.psDown);
    for (int i = 1; i < kLevels; i++) {
        const UINT w = g.padW >> i, h = g.padH >> i;
        dev->SetRenderTarget(0, g.tmpSurf[i]);
        dev->SetTexture(1, i == 1 ? static_cast<IDirect3DBaseTexture9*>(g.zTex) : g.tmp[i - 1]);
        const float lv[4] = {1.0f / (w * 2), 1.0f / (h * 2), 0, 0};
        dev->SetPixelShaderConstantF(6, lv, 1);
        DrawQuad(dev, w, h);
        dev->SetTexture(1, nullptr);
        dev->StretchRect(g.tmpSurf[i], nullptr, g.zLevel[i], nullptr, D3DTEXF_NONE);
    }

    // 3. GTAO at full resolution -> aoA (R = visibility, G = 1/z)
    dev->SetRenderTarget(0, g.aoASurf);
    dev->SetTexture(1, g.zTex);
    dev->SetTexture(2, g.zTex);
    dev->SetPixelShader(psGtao);
    DrawQuad(dev, g.width, g.height);
    dev->SetTexture(2, nullptr);

    // 4. filter: box H, box V (cancels the 4x4 interleave), tent H, tent V; a -> b -> a -> b -> a
    dev->SetPixelShader(g.psBlur);
    for (int pass = 0; pass < 4; pass++) {
        const float dir[4] = {pass & 1 ? 0.0f : 1.0f / W, pass & 1 ? 1.0f / H : 0.0f, pass < 2 ? 0.0f : 1.0f, 0};
        dev->SetPixelShaderConstantF(6, dir, 1);
        dev->SetRenderTarget(0, pass & 1 ? g.aoASurf : g.aoBSurf);
        dev->SetTexture(3, pass & 1 ? g.aoB : g.aoA);
        DrawQuad(dev, g.width, g.height);
    }

    // 5. composite over the scene copy
    dev->SetRenderTarget(0, bb);
    dev->SetTexture(3, g.aoA);
    dev->SetTexture(4, g.colorTex);
    dev->SetTexture(0, depth);
    dev->SetTexture(5, simMask.texture);
    dev->SetTexture(6, simMask.hairTexture);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    dev->SetPixelShader(g.psComposite);
    DrawQuad(dev, g.width, g.height);

    saved.Restore(dev);
    g.frames++;
}

// PostScene effect (order kAmbientOcclusion): first, before edge smoothing, Depth Blur, bloom and the UI
void AoEffect(IDirect3DDevice9* dev) {
    if (!g.ready || (g.p.strength <= 0.0f && !g.showShade)) return;
    IDirect3DTexture9* depth = DepthShare::Texture();
    if (!depth) return;
    // the scene depth must be the one bound right now (not a reflection or UI pass)
    IDirect3DSurface9* ds = nullptr;
    ExtraHooks::RawGetDepthStencilSurface(dev, &ds);
    const bool sceneDepth = ds && ds == DepthShare::Surface();
    if (ds) ds->Release();
    if (!sceneDepth) return;
    IDirect3DPixelShader9* psGtao = GtaoShader(dev, g.p.quality);
    if (!psGtao) return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &bb)) || !bb) return;
    D3DSURFACE_DESC bd{};
    bb->GetDesc(&bd);
    if (bd.Width != g.width || bd.Height != g.height) { // the back buffer changed without a Reset: rebuild next frame
        bb->Release();
        ReleaseResources();
        g.retryCountdown = 0;
        return;
    }
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
    RunAo(dev, depth, bb, psGtao);
    bb->Release();
    if (timed) {
        g.qEnd[qi]->Issue(D3DISSUE_END);
        g.qFreq[qi]->Issue(D3DISSUE_END);
        g.qDisjoint[qi]->Issue(D3DISSUE_END);
        g.qIssued[qi] = true;
        g.qNext = (qi + 1) % State::kQ;
    }
}

void OnFrameBoundary(IDirect3DDevice9* dev) {
    if (!g.active) return;
    simRenderThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    simMask.lastDraws = simMask.draws;
    simMask.draws = 0;
    simMask.cleared = false;
    simMask.hairCleared = false;
    simMask.shaderKnown = false;
    if (!g.ready && --g.retryCountdown <= 0) {
        g.retryCountdown = kRetryFrames;
        InitResources(dev);
    }
    if (g.ready && g.status.rfind("ERROR: ", 0) != 0) g.status = DepthShare::Texture() ? "Active" : "Waiting for the scene depth: " + DepthShare::Status();
    if (!WantSimMask()) {
        if (simMask.texture || !simMask.known.empty()) ReleaseSimMask();
    } else if (g.ready && !simMask.texture && !simMask.failed) {
        const HRESULT hr = dev->CreateTexture(g.width, g.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G32R32F,
                                             D3DPOOL_DEFAULT, &simMask.texture, nullptr);
        if (FAILED(hr) || FAILED(simMask.texture->GetSurfaceLevel(0, &simMask.surface))) {
            SafeRelease(simMask.texture); SafeRelease(simMask.surface);
            simMask.failed = true;
            LOG_WARNING("[AO] Sim receiver mask unavailable; retaining original shade");
        } else { simMask.width = g.width; simMask.height = g.height; }
    }
    if (WantSimMask() && g.ready && !simMask.hairTexture && !simMask.failed) {
        if (FAILED(dev->CreateTexture(g.width, g.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G32R32F,
                                     D3DPOOL_DEFAULT, &simMask.hairTexture, nullptr)) ||
            FAILED(simMask.hairTexture->GetSurfaceLevel(0, &simMask.hairSurface))) {
            SafeRelease(simMask.hairTexture); SafeRelease(simMask.hairSurface);
            simMask.failed = true;
        }
    } else if (!WantSimMask()) {
        SafeRelease(simMask.hairTexture); SafeRelease(simMask.hairSurface);
    }
}

void OnPreReset(IDirect3DDevice9*) {
    if (!g.active) return;
    ReleaseResources();
    g.status = "Recreating after a video change...";
}

void OnPostReset(IDirect3DDevice9*) {
    if (g.active) g.retryCountdown = 0;
}

} // namespace

void SimOcclusion::RenderUI(ApexPatch* patch) {
    if (!patch) return;
    static const Params defaults{};
    bool changed = false;
    ImGui::PushID("SimOcclusion");
    if (ApexUi::BeginCard("##SimOcclusion")) {
        ApexUi::HeaderExtra headerBadge;
        headerBadge.badge = "Experimental";
        headerBadge.badgeTooltip = "Still being tested: if anything looks wrong or the game crashes, turn it off";
        ImGui::BeginDisabled(!patch->IsEnabled());
        changed |= ApexUi::CardHeader(ApexUi::IconId::UserRound, "Sim Occlusion", "Softer shade on Sims and hair",
                                     "Adjust occlusion on Sims separately from the scene", &g.p.simControls, true, &headerBadge);
        ImGui::EndDisabled();
        if (g.p.simControls || !patch->IsEnabled()) ApexUi::CardDivider();
        if (!patch->IsEnabled())
            ApexUi::IconNote(ApexUi::IconId::Info, "Needs Ambient Occlusion");
        if (g.p.simControls) {
            ImGui::BeginDisabled(!patch->IsEnabled());
            changed |= ApexUi::SliderPercent("Sim intensity", &g.p.simStrength, 0.0f, 1.0f,
                                            "Shade on the body, face and clothes; hair has its own control", defaults.simStrength);
            changed |= ApexUi::SliderPercent("Hair intensity", &g.p.hairStrength, 0.0f, 1.0f,
                                            "Shade on recognized hair; 0% removes it, 100% keeps the original", defaults.hairStrength);
            changed |= ApexUi::SliderPercent("Maximum darkening", &g.p.simMaxShade, 0.0f, 1.0f,
                                            "Limit the maximum added shade on Sims and hair", defaults.simMaxShade);
            if (ApexUi::BeginAdvanced("Advanced##SimOcclusion")) {
                changed |= ApexUi::SwitchRow("Transparent hair", &g.p.transparentHair,
                                            "Also adjust supported transparent hair strands", defaults.transparentHair);
                ApexUi::SwitchRow("Show Sim coverage", &g.showSimMask,
                                  "Blue shows Sims, green shows hair, black is unrecognized; preview is not saved");
                ApexUi::EndAdvanced();
            }
            if (WantSimMask() && simMask.failed)
                ApexUi::IconNote(ApexUi::IconId::TriangleAlert, "Sim shading control is unavailable; original shade is kept", VioletTheme::kWarning);
            ImGui::EndDisabled();
        }
    }
    ApexUi::EndCard();
    ImGui::PopID();
    if (changed) patch->NotifySettingChanged();
}

class AmbientOcclusionPatch : public ApexPatch {
  public:
    AmbientOcclusionPatch() : ApexPatch("AmbientOcclusion", nullptr) {
        RegisterFloatSetting(&g.p.strength, "forca", SettingWidget::Slider, Params{}.strength, 0.0f, 2.0f, "How dark the shade gets where things meet");
        RegisterFloatSetting(&g.p.reach, "alcance", SettingWidget::Slider, Params{}.reach, 0.5f, 2.0f, "How far the shade spreads from where things meet (scales the radii)");
        RegisterFloatSetting(&g.p.protect, "protegerLuz", SettingWidget::Slider, Params{}.protect, 0.0f, 1.0f, "Lamp-lit and bright spots keep this share of their light");
        RegisterEnumSetting(&g.p.quality, "qualidade", Params{}.quality, "Directions per pixel: higher is smoother and costs more GPU", {"Low", "Medium", "High", "Ultra", "Very Low"});
        RegisterBoolSetting(&g.p.inMapView, "noMapa", true, "Also shade the map view (radii for houses and trees seen from far away)");
        RegisterFloatSetting(&g.p.distance, "distance", SettingWidget::Slider, Params{}.distance, 25.0f, 1000.0f, "Distance at which the shade fades out outside map view");
        RegisterFloatSetting(&g.p.simStrength, "simStrength", SettingWidget::Slider, Params{}.simStrength, 0.0f, 1.0f, "Shade on supported Sim materials; 0% removes it, 100% keeps the original");
        RegisterBoolSetting(&g.p.simControls, "simControls", false, "Adjust occlusion on Sims separately from the scene");
        RegisterFloatSetting(&g.p.hairStrength, "hairStrength", SettingWidget::Slider, Params{}.hairStrength, 0.0f, 1.0f, "Shade on recognized hair; 0% removes it, 100% keeps the original");
        RegisterFloatSetting(&g.p.simMaxShade, "simMaxShade", SettingWidget::Slider, Params{}.simMaxShade, 0.0f, 1.0f, "Limit the maximum added shade on Sims and hair");
        RegisterBoolSetting(&g.p.transparentHair, "transparentHair", true, "Also adjust supported transparent hair strands");
    }

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        DepthShare::Request(true); // keeps the INTZ depth swap running even with Depth Blur off
        PostScene::WantCamera(true);
        D3D9Hooks::RegisterSetPixelShader(kHookName, [](D3D9Hooks::DeviceContext&, IDirect3DPixelShader9* ps) {
            if (GetCurrentThreadId() != simRenderThread.load(std::memory_order_relaxed)) return D3D9Hooks::HookAction::Continue;
            if (g.active && g.ready && WantSimMask() && !simMask.failed) {
                try {
                    simMask.shaderIsSim = IsSimReceiver(ps);
                    simMask.shaderKnown = true;
                } catch (...) { simMask.failed = true; }
            }
            return D3D9Hooks::HookAction::Continue;
        }, D3D9Hooks::Priority::Early);
        D3D9Hooks::RegisterDrawIndexedPrimitive(kHookName, [](D3D9Hooks::DeviceContext& ctx, D3DPRIMITIVETYPE type, INT base, UINT min, UINT count, UINT start, UINT prims) {
            try { RecordSimReceiver(ctx.device, [&] { return D3D9Hooks::CallOriginalDrawIndexedPrimitive(ctx.device, type, base, min, count, start, prims); }); }
            catch (...) { simMask.failed = true; }
            return D3D9Hooks::HookAction::Continue;
        }, D3D9Hooks::Priority::Early);
        D3D9Hooks::RegisterDrawPrimitive(kHookName, [](D3D9Hooks::DeviceContext& ctx, D3DPRIMITIVETYPE type, UINT start, UINT prims) {
            try { RecordSimReceiver(ctx.device, [&] { return D3D9Hooks::CallOriginalDrawPrimitive(ctx.device, type, start, prims); }); }
            catch (...) { simMask.failed = true; }
            return D3D9Hooks::HookAction::Continue;
        }, D3D9Hooks::Priority::Early);
        D3D9Hooks::RegisterPresent(kHookName, [](D3D9Hooks::DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
            OnFrameBoundary(ctx.device);
            return D3D9Hooks::HookAction::Continue;
        }, D3D9Hooks::Priority::First);
        RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
        RenderCallbacks::Add(RenderCallbacks::postReset, OnPostReset);
        PostScene::Add(PostScene::kAmbientOcclusion, AoEffect);
        g.active = true;
        g.retryCountdown = 0;
        g.status = "Waiting for the game...";
        isEnabled = true;
        LOG_INFO("[AO] Installed");
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        lastError.clear();
        PostScene::Remove(AoEffect);
        g.active = false;
        D3D9Hooks::UnregisterAll(kHookName);
        RenderCallbacks::Remove(RenderCallbacks::preReset, OnPreReset);
        RenderCallbacks::Remove(RenderCallbacks::postReset, OnPostReset);
        ReleaseResources();
        ReleaseShaders();
        PostScene::WantCamera(false);
        DepthShare::Request(false);
        g.status = "Off";
        isEnabled = false;
        LOG_INFO("[AO] Uninstalled");
        return true;
    }

    // Settings are read live every frame, never reinstall (that would tear down the depth swap from the wrong thread)
    void Update() override { pendingReinstall = false; }

    // Settings revisions let newer builds supply defaults for newly added keys. Stored values always take precedence;
    // older configs and profiles fill only keys they do not contain.
    void SaveToToml(toml::table& table) const override {
        ApexPatch::SaveToToml(table);
        table.insert_or_assign(kRevisionKey, kSettingsRevision);
    }
    bool LoadFromToml(const toml::table& table) override {
        if (Revision(table) >= kSettingsRevision) return ApexPatch::LoadFromToml(table);
        LOG_INFO(std::format("[AO] Settings of revision {}: preserving saved values and filling missing keys from revision {} defaults", Revision(table), kSettingsRevision));
        const bool ok = ApexPatch::LoadFromToml(WithDefaults(table));
        PatchManager::Get().SetUnsavedChanges(true); // saved again with the current revision
        return ok;
    }
    void ApplyTableLive(const toml::table& table) override { ApexPatch::ApplyTableLive(Revision(table) >= kSettingsRevision ? table : WithDefaults(table)); }

    float GpuCostMs() const override { return (isEnabled.load() && g.ready && g.gpuMs >= 0.0f) ? g.gpuMs : -1.0f; }

    // The card's controls (menu: Image > Ambient Occlusion page). Settings are read live every frame.
    void RenderCustomUI() override {
        SAFE_IMGUI_BEGIN();
        using ApexUi::IconId;
        static const Params kDefaults{};
        bool changed = false;
        if (g.status.rfind("ERROR: ", 0) == 0) ApexUi::IconNote(IconId::TriangleAlert, g.status.c_str() + 7, VioletTheme::kError);

        changed |= ApexUi::SliderPercent("Strength", &g.p.strength, 0.0f, 2.0f, "How dark the shade gets where things meet",
                                         kDefaults.strength);
        changed |= ApexUi::Slider("Distance", &g.p.distance, 25.0f, 1000.0f,
                                 {.format = "%.0f m", .tooltip = "Distance at which the shade fades out outside map view", .defaultValue = kDefaults.distance});
        // Quality: shown from the lightest to the smoothest; the saved value keeps 2.1.0's indices (kQualityShown)
        static const char* const kQualities[] = {"Very Low", "Low", "Medium", "High", "Ultra"};
        static const char* const kQualityTips[] = {"Lightest; a little more shimmer while the camera moves", "Light", "Balanced",
                                                   "The default", "Steadiest while the camera moves; costs the most"};
        int shown = 3;
        for (int i = 0; i < kQualityCount; i++)
            if (kQualityShown[i] == g.p.quality) shown = i;
        if (ApexUi::SegmentedRow("Quality", "Lower is lighter on the graphics card", "##Quality", &shown, kQualities, kQualityCount, kQualityTips, nullptr, 3)) {
            g.p.quality = kQualityShown[std::clamp(shown, 0, kQualityCount - 1)];
            changed = true;
        }
        changed |= ApexUi::SwitchRow("Also in map view", &g.p.inMapView, "Soft shade around houses and trees when the map view is open", kDefaults.inMapView);
        if (ApexUi::BeginAdvanced("Advanced##AmbientOcclusion")) {
            changed |= ApexUi::SliderPercent("Reach", &g.p.reach, 0.5f, 2.0f, "How far the shade spreads from where things meet", kDefaults.reach);
            changed |= ApexUi::SliderPercent("Keep lamp light", &g.p.protect, 0.0f, 1.0f, "Lamp-lit and bright spots keep more of their light; 0% shades everything alike",
                                             kDefaults.protect);
            ApexUi::SwitchRow("Show the shade alone", &g.showShade, "Shows only the shade, in grey, to see what it does while you adjust it (not saved)");
            ApexUi::EndAdvanced();
        }
        if (changed) NotifySettingChanged();
    }

  private:
    // Bump with every change of the AO's look or options. 1 = 2.1.0 (saved no key), 2 = 30/09 (bilinear march, five
    // qualities, shade preview), 3 = 30/09 evening (grain in the composite), 4 = 30/09 night (the map view), 5 = 30/09 night
    // (the composite grain follows the Banding Fix, only where the shade changed the pixel); 6 = distance and Sim receivers;
    // 7 = independent hair, transparency coverage and a separate Sim card; 8 = Sim Occlusion defaults off;
    // 9 = user-approved default configuration for scene and Sim controls.
    static constexpr int kSettingsRevision = 9;
    static constexpr const char* kRevisionKey = "revisao";
    static int Revision(const toml::table& table) { return static_cast<int>(table[kRevisionKey].value<int64_t>().value_or(1)); }
    // Start from current defaults, then overlay every value that was explicitly saved.
    toml::table WithDefaults(const toml::table& table) const {
        toml::table d;
        DefaultsToToml(d);
        for (const auto& [key, value] : table) d.insert_or_assign(key, value);
        return d;
    }

  public:
    // Developer page > Debug views
    void RenderDeveloperUI() override {
        SAFE_IMGUI_BEGIN();
        ImGui::TextWrapped("Status: %s", g.status.c_str());
        ImGui::TextDisabled("Sim mask: %u draws last frame, %u refused pairs, %zu cached pairs%s",
                            simMask.lastDraws, simMask.refused, simMask.copies.size(), simMask.failed ? " (unavailable)" : "");
        if (g.ready) {
            if (g.gpuMs >= 0) ImGui::TextDisabled("GPU cost: %.2f ms per frame (%d slices)", g.gpuMs, kQualitySlices[std::clamp(g.p.quality, 0, kQualityCount - 1)]);
            ImGui::TextDisabled("Frames shaded: %u  |  screen %ux%u, depth pyramid %ux%u (%d levels)", g.frames, g.width, g.height, g.padW, g.padH, kLevels);
            ImGui::TextDisabled("Camera: near %.3f m, A %.6f, tan %.4f x %.4f (%s)", g.lastNear, g.lastA, g.lastTanX, g.lastTanY,
                                g.lastCamera ? "read this frame" : "fallback");
        }
        ImGui::Checkbox("Show the shade alone", &g.showShade);
        if (ImGui::Button("Save depth and colour##AoCapture")) {
            g.captureRequested = true;
            g.captureNote = g.ready ? "Saving at the next frame..." : "Turn Ambient Occlusion on first";
        }
        ApexUi::Tooltip("Saves the next frame's depth, colour (before the AO and the menus) and camera into Documents > ... > Apex Radiance > Profundidade, for the offline AO lab. Open the map view first to capture it");
        if (!g.captureNote.empty()) ImGui::TextDisabled("%s", g.captureNote.c_str());
        ApexUi::Tooltip("Shows the ambient occlusion in grey instead of the image: white = no shade (not saved)");
    }
};

APEX_REGISTER_FEATURE(AmbientOcclusionPatch,
                      {.displayName = "Ambient Occlusion",
                       .description = "Soft shade where things meet: under furniture, in corners, where walls meet the floor and around houses and trees. "
                                      "Computed at full resolution with no noise, so it stays still when the camera does. Works with the game's own Edge "
                                      "Smoothing turned off. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                       .category = "Graphics",
                       .enabledByDefault = true,
                       .supportedVersions = VERSION_ALL,
                       .technicalDetails = {"Reads the INTZ scene depth shared by the Depth Blur module (kept running even with Depth Blur off).",
                                            "GTAO at full resolution, deterministic: 2 to 12 slices (quality) x 4 geometric steps per side over a 9-level "
                                            "1/z pyramid read bilinearly within the nearest level, contact and large horizons, 4x4 Bayer interleave "
                                            "cancelled by a 4x4 box, then a tent.",
                                            "Composite: dead zone, Jimenez multi-bounce per channel, lamp-lit pixels keep part of their light.",
                                            "Runs first in the PostScene chain (before edge smoothing and Depth Blur); saves/restores only the states it touches."}})
