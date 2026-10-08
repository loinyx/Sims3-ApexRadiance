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
#include "hotkeys.h"
#include "apex_config.h"
#include "apex_log.h"
#include "hook_guard.h"
#include "d3d9_hooks.h"
#include "depth_share.h"
#include "d3d9_extra_hooks.h"
#include "post_scene.h"
#include "world_session.h"
#include "shader_cache.h"
#include "imgui.h"
#include "ui/i18n.h"
#include "ui/widgets.h"
#include "scene_dither.h"
#include "apex_paths.h"
#include "apex_util.h"
#include <wincodec.h>
#include <shellapi.h>
#include <thread>
#include <vector>
#include <d3dcompiler.h>
#include <toml++/toml.hpp>
#include <algorithm>
#include <type_traits>
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
constexpr int kMinSceneDraws = 4; // depth-tested back buffer draws before the UI can start (as PostScene; 20 until 06/10, 8 until 06/10 night)

const char* kShaderSource = R"HLSL(
sampler2D sFrame : register(s0); // the finished frame (scene + UI), point
sampler2D sScene : register(s1); // the scene before the UI, point
sampler2D sBase  : register(s2); // the scene at 1/8 size, bilinear (clarity, glow, dreamy: the smooth local average)
sampler2D sDepth : register(s3); // the scene depth (INTZ, point), only while Emphasize runs
sampler2D sHalf  : register(s4); // the scene at 1/2 size, bilinear (tilt-shift)
sampler2D sQuart : register(s5); // the scene at 1/4 size, bilinear (tilt-shift, glow, halation)
sampler2D sLut   : register(s6); // the LUT strip (bilinear), only while LUT is on
sampler2D sAdapt : register(s7); // 1x1: the adapted scene luminance (auto exposure); AdaptPS reads the previous one here
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
// Filters (Color > Filters). Every filter is skipped by a branch on its own flag (1 = on), so a filter that is off costs
// nothing; the flags, amounts and colours are prepared on the CPU (picture.cpp, OnEndScene).
float4 cFlagA  : register(c13); // Technicolor 1, Technicolor 2, DPX Cineon, Colourfulness
float4 cFlagB  : register(c14); // Night Mode, Vintage, Cross-process, Black and white
float4 cFlagC  : register(c15); // Glow, Halation, Dreamy
float4 cFlagD  : register(c16); // Emphasize, Tilt-shift, Prism, Film grain
float4 cFlagE  : register(c17); // 3DFX, CRT, Levels, Filmic pass
float4 cTech1  : register(c18); // x = amount, y = cyan record (0 green .. 1 blue), z = saturation, w = brightness gain
float4 cTech2  : register(c19); // rgb = red / green / blue dye, w = amount
float4 cTech2b : register(c20); // x = saturation, y = brightness gain; z = DPX amount, w = DPX contrast
float4 cDpx    : register(c21); // rgb = DPX curve per channel, w = DPX saturation
float4 cColNit : register(c22); // x = Colourfulness amount, y = protect; z = Night amount, w = darkness
float4 cNitVin : register(c23); // x = Night blue, y = Night keep lamps; z = Vintage amount, w = fade
float4 cVinCro : register(c24); // x = Vintage warmth, y = Vintage colors; z = Cross amount, w = Cross contrast
float4 cBw     : register(c25); // x = amount, y = filter strength, z = toning amount, w = contrast
float4 cBwF    : register(c26); // rgb = luminance weights through the filter, w = brightness gain
float4 cBwT    : register(c27); // rgb = toning colour (luminance 1)
float4 cGlow   : register(c28); // x = amount, y = threshold, z = size, w = warmth
float4 cHal    : register(c29); // x = amount, y = threshold, z = radius (share of the screen height)
float4 cHalC   : register(c30); // rgb = halation colour
float4 cDream  : register(c31); // x = amount, y = softness, z = saturation
float4 cLevels : register(c32); // x = black point, y = white point (encoded 0..1), z = 1 / gamma
float4 cFilm   : register(c33); // x = Filmic amount, y = fade, z = contrast (0..2), w = bleach
float4 cEmph   : register(c34); // x = amount, y = grey, z = automatic focus, w = manual distance (m)
float4 cEmph2  : register(c35); // x = zone depth (fraction of the distance), y = softness, z = camera near, w = depth A
float4 cTilt   : register(c36); // x = amount, y = centre (0 top .. 1 bottom), z = sharp band height, w = saturation
float4 cPrism  : register(c37); // x = shift (px at a corner), y = start radius, z = samples
float4 cGrain  : register(c38); // x = amount, y = grain size (px), z = more in the shadows, w = seed (animated grain)
float4 cFx     : register(c39); // x = 3DFX amount, y = levels of red/blue, z = dither, w = scanlines
float4 cFx2    : register(c40); // x = soft pixels, y = 1 / gamma, z = line period (px), w = levels of green
float4 cCrt    : register(c41); // x = amount, y = curvature, z = phosphor mask, w = scanlines
float4 cCrt2   : register(c42); // x = edge darkening, y = line period (px)
float4 cFilm2  : register(c43); // x = Filmic saturation (-1..1), yzw = red / green / blue curve
float4 cFlagF  : register(c44); // x = Tint, y = Fake HDR
float4 cTintF  : register(c45); // rgb = Tint color (luminance 1), w = amount
float4 cFlagG  : register(c46); // x = Auto exposure, y = Adaptive sharpening, z = Color-blind mode
float4 cLut    : register(c47); // x = LUT amount, y = cells per side, z = LUT on
float4 cHdr    : register(c48); // x = amount, y = radius (0 fine .. 1 large), z = shadows, w = highlights
float4 cHdr2   : register(c49); // x = halo protection, y = saturation
// c50, c51: free (were the atmospheric fog, removed 06/10)
float4 cAuto   : register(c52); // x = amount, y = target luminance (linear), z = lowest gain, w = highest gain
float4 cCas    : register(c53); // x = sharpness (0 .. 1)
float4 cDalt   : register(c54); // x = type (0 protan, 1 deutan, 2 tritan), y = amount, z = simulate (show the color-blind view)
float4 cAdapt  : register(c56); // AdaptPS: x = blend toward this frame's average
float4 cExtA   : register(c57); // x = Technicolor 1 contrast, y = Vintage vignette, z/w = Cross-process cast rotation (cos, sin)
float4 cExtB   : register(c58); // x = Cross-process saturation, y = Tint preserve brightness, z = Tint balance, w = Tilt-shift extra blur
float4 cGlowC  : register(c59); // rgb = Glow color (luminance 1), w = color strength
float4 cExtC   : register(c60); // x = Film grain color
static const float3 kLum = float3(0.2126, 0.7152, 0.0722);

// A neighbour or shifted tap for the scene filters: from the scene copy (no UI) when it exists, so a filter that reads
// around the pixel (deband, prism, sharpen, CRT, 3DFX) never pulls the colour of a button or a panel into the world
float3 SceneTap(float2 p)
{
    return cLook.y > 0.5 ? tex2Dlod(sScene, float4(p, 0, 0)).rgb : tex2Dlod(sFrame, float4(p, 0, 0)).rgb;
}
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
            float3 v = SceneTap(uv + float2(cos(a), sin(a)) * r * cSize.xy);
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

float3 Saturate3(float3 g, float s)
{
    float L = dot(g, kLum);
    return max(lerp(float3(L, L, L), g, s), 0.0);
}

// View distance (m) from the device depth (d = A - near * A / z); the sky is far away
float ViewZ(float d)
{
    return d >= 1.0 ? 1e5 : cEmph2.z * cEmph2.w / max(cEmph2.w - d, 1e-6);
}

// Prism: red and blue pulled apart along the line from the centre, the shift growing toward the edges. Several taps
// weighted across a small spectrum so the fringes blend instead of doubling the image.
float3 Prism(float2 uv, float3 c)
{
    float2 d = (uv - 0.5) * float2(cSize.y * cSize.z, 1.0);
    float r = length(d) / length(float2(0.5 * cSize.y * cSize.z, 0.5)); // 1 at a corner
    float k = smoothstep(cPrism.y, 1.0, r) * cPrism.x;
    if (k < 0.05) return c;
    float2 dir = normalize(uv - 0.5 + 1e-6) * k * cSize.xy;
    float3 sum = 0.0, wsum = 0.0;
    int n = (int)cPrism.z;
    [loop] for (int i = 0; i < n; i++)
    {
        float t = (i + 0.5) / n * 2.0 - 1.0; // -1 .. 1
        float3 w = float3(saturate(t + 0.5), 1.0 - abs(t), saturate(0.5 - t));
        sum += SceneTap(uv + dir * t) * w;
        wsum += w;
    }
    return sum / wsum;
}

// 4x4 ordered dither threshold (-0.5 .. 0.5), the pattern 3D cards of the late 90s used for 16-bit color
float Bayer2(float2 a)
{
    a = floor(a);
    return frac(a.x / 2.0 + a.y * a.y * 0.75);
}
float Bayer4(float2 px)
{
    return Bayer2(0.5 * px) * 0.25 + Bayer2(px) - 0.5;
}

// Cheap per-pixel hash (0..1)
float Hash(float2 p);
// Smooth value noise (0..1): the hash at the lattice points, blended with a smoothstep
float GrainNoise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = Hash(i), b = Hash(i + float2(1, 0)), c = Hash(i + float2(0, 1)), d = Hash(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float Hash(float2 p)
{
    p = frac(p * float2(443.897, 441.423));
    p += dot(p, p.yx + 19.19);
    return frac((p.x + p.y) * p.x);
}
)HLSL"
                            R"HLSL(
// The color looks, in linear light (1 = white), each blended by its own amount
float3 ColorLooks(float2 uv, float3 g)
{
    [branch] if (cFlagA.x > 0.5) // Technicolor 1: two-strip, everything is a red record or a cyan record
    {
        float cyan = lerp(g.g, g.b, cTech1.y);
        float3 t = Saturate3(float3(g.r, cyan, cyan), cTech1.z) * cTech1.w; // brightness
        t = 0.18 * pow(max(t, 0.0) / 0.18, cExtA.x);                        // contrast around mid grey
        g = lerp(g, t, cTech1.x);
    }
    [branch] if (cFlagA.y > 0.5) // Technicolor 2: three-strip dye transfer, each dye subtracts its complement
    {
        float3 e = pow(saturate(g), 1.0 / 2.2);
        float3 other = (e.gbr + e.brg) * 0.5;
        float3 t = pow(saturate(e + (e - other) * cTech2.rgb * 0.6), 2.2) * cTech2b.y;
        g = lerp(g, Saturate3(t, cTech2b.x), cTech2.w);
    }
    [branch] if (cFlagA.z > 0.5) // DPX Cineon: a film S curve per channel (normalised so black and white stay)
    {
        float3 e = pow(saturate(g), 1.0 / 2.2);
        float3 k = (3.0 + 9.0 * cTech2b.w) * cDpx.rgb;
        float3 s0 = 1.0 / (1.0 + exp(k * 0.5)), s1 = 1.0 / (1.0 + exp(-k * 0.5));
        float3 t = pow(saturate((1.0 / (1.0 + exp(-k * (e - 0.5))) - s0) / (s1 - s0)), 2.2);
        g = lerp(g, Saturate3(t, cDpx.w), cTech2b.z);
    }
    [branch] if (cFlagA.w > 0.5) // Colourfulness: more chroma, less for colors that are already strong or bright
    {
        float L = dot(g, kLum);
        float3 d = g - L;
        float m = max(abs(d.r), max(abs(d.g), abs(d.b)));
        g = max(L + d * (1.0 + cColNit.x * (1.0 - cColNit.y * saturate(max(m * 3.0, L)))), 0.0);
    }
    [branch] if (cFlagB.x > 0.5) // Night Mode: darker and bluer, with less color; lamp-lit areas keep their light
    {
        float L = dot(g, kLum);
        float3 n = lerp(float3(L, L, L), g, 0.45) * float3(1.0 - 0.3 * cNitVin.x, 1.0 - 0.05 * cNitVin.x, 1.0 + 0.45 * cNitVin.x);
        n *= 1.0 - cColNit.w;
        float keep = smoothstep(0.25, 0.85, pow(saturate(L), 1.0 / 2.2)) * cNitVin.y;
        g = lerp(g, lerp(n, g, keep), cColNit.z);
    }
    [branch] if (cFlagB.y > 0.5) // Vintage: washed-out colors, warm cast, lifted (faded) blacks, softer whites
    {
        float3 e = pow(saturate(g), 1.0 / 2.2);
        float L = dot(e, kLum);
        e = lerp(e, float3(L, L, L), cVinCro.y * 0.7);
        e *= float3(1.0 + 0.10 * cVinCro.x, 1.0 + 0.02 * cVinCro.x, 1.0 - 0.12 * cVinCro.x);
        e = lerp(float3(0.08, 0.06, 0.05) * cNitVin.w * 1.6, float3(0.97, 0.95, 0.90), saturate(e));
        if (cExtA.y > 0.0) // vignette: darker corners, like an old lens (aspect corrected, 1 at a corner)
        {
            float2 q = (uv - 0.5) * float2(cSize.y * cSize.z, 1.0);
            float r = length(q) / length(float2(cSize.y * cSize.z, 1.0) * 0.5);
            e *= 1.0 - cExtA.y * 0.75 * smoothstep(0.3, 1.0, r);
        }
        g = lerp(g, pow(saturate(e), 2.2), cNitVin.z);
    }
    [branch] if (cFlagB.z > 0.5) // Cross-process: green-cyan shadows, yellow highlights, stronger contrast
    {
        float3 e = pow(saturate(g), 1.0 / 2.2);
        float k = 1.0 + cVinCro.w;
        float3 t;
        t.r = saturate(0.5 + (e.r - 0.5) * k * 1.15);
        t.g = saturate(e.g * (1.0 + 0.1 * k) + 0.03);
        t.b = saturate(0.12 + e.b * 0.72);
        // the color cast (the shift from the plain contrast curve) turned around the grey axis: 180 degrees = the original
        float3 base = saturate(0.5 + (e - 0.5) * k * 1.15);
        float3 sh = t - base;
        float3 ax = float3(0.57735, 0.57735, 0.57735);
        sh = sh * cExtA.z + cross(ax, sh) * cExtA.w + ax * dot(ax, sh) * (1.0 - cExtA.z);
        t = saturate(base + sh);
        t = saturate(lerp(dot(t, kLum), t, cExtB.x));
        g = lerp(g, pow(t, 2.2), cVinCro.z);
    }
    [branch] if (cFlagB.w > 0.5) // Black and white: luminance through a colored lens filter, contrast, then a toning
    {
        float L = dot(g, cBwF.rgb) * cBwF.w;
        L = 0.18 * pow(max(L, 0.0) / 0.18, 1.0 + cBw.w * 0.5);
        float e = saturate(pow(saturate(L), 1.0 / 2.2));
        float3 t = L * lerp(1.0, cBwT.rgb, cBw.z * (1.0 - e * 0.5));
        g = lerp(g, t, cBw.x);
    }
    // The next three work on the encoded picture (0..1 as shown), as in a photo editor
    [branch] if (cFlagE.w > 0.5) // Filmic pass: S-curve contrast, a brightness curve per channel, bleach bypass, saturation, fade
    {
        float3 e0 = pow(saturate(g), 1.0 / 2.2);
        float3 e = e0;
        e = saturate(lerp(e, e * e * (3.0 - 2.0 * e), cFilm.z));         // toward a smooth S curve (2 = twice as steep)
        e = pow(e, 1.0 / max(cFilm2.yzw, 0.1));                            // a curve above 1 brightens that channel
        float L = dot(e, kLum);
        float3 over = L < 0.5 ? 2.0 * e * L : 1.0 - 2.0 * (1.0 - e) * (1.0 - L); // bleach bypass: overlaid with its own grey
        e = lerp(e, saturate(over), cFilm.w);
        L = dot(e, kLum);
        e = max(lerp(float3(L, L, L), e, 1.0 + cFilm2.x), 0.0);
        e = lerp(e, 0.06 + e * 0.88, cFilm.y);                             // fade: lifted blacks, softer whites
        g = pow(saturate(lerp(e0, e, cFilm.x)), 2.2);
    }
    [branch] if (cFlagF.x > 0.5) // Tint: the picture's grey in one color (sepia by default), mixed in
    {
        float3 e = pow(saturate(g), 1.0 / 2.2);
        float L = dot(e, kLum);
        // preserve brightness 0: the color as a filter over the picture (darker), 1: the same brightness
        float3 col = lerp(cTintF.rgb / max(cTintF.r, max(cTintF.g, cTintF.b)), cTintF.rgb, cExtB.y);
        // balance: 0 the whole picture, -1 only the shadows, +1 only the highlights
        float zone = cExtB.z < 0.0 ? 1.0 - smoothstep(0.1, 0.7, L) : smoothstep(0.3, 0.9, L);
        float w = cTintF.w * lerp(1.0, zone, abs(cExtB.z));
        g = pow(saturate(lerp(e, L * col, w)), 2.2);
    }
    [branch] if (cFlagE.z > 0.5) // Levels: the black point goes to black and the white point to white
    {
        float3 e = pow(saturate(g), 1.0 / 2.2);
        e = saturate((e - cLevels.x) / max(cLevels.y - cLevels.x, 1.0 / 255.0));
        g = pow(e, 2.2 * cLevels.z); // gamma: above 1 brightens the midtones
    }
    [branch] if (cLut.z > 0.5) // LUT: the color looked up in a strip of size blue slices (each size x size: red across, green down)
    {
        float3 e = pow(saturate(g), 1.0 / 2.2);
        float s = cLut.y;
        float b = e.b * (s - 1.0);
        float b0 = floor(b);
        float2 p0 = float2((b0 * s + e.r * (s - 1.0) + 0.5) / (s * s), (e.g * (s - 1.0) + 0.5) / s);
        float3 c0 = tex2Dlod(sLut, float4(p0, 0, 0)).rgb;
        float3 c1 = tex2Dlod(sLut, float4(p0 + float2(1.0 / s, 0.0), 0, 0)).rgb;
        g = lerp(g, pow(max(lerp(c0, c1, b - b0), 0.0), 2.2), cLut.x);
    }
    [branch] if (cFlagG.z > 0.5) // color-blind mode (daltonize): what the eye cannot tell apart is moved into channels it can see
    {
        // 06/10 review (user: "does not seem to work right"): the simulation (Vienot et al. / daltonize.org matrices) needs
        // linear light, as before, but the error was also redistributed in linear light: the common algorithm (Fidaner et
        // al.) moves the error on gamma-encoded values, and in linear light the shift was far too weak in the darks and
        // clipped in the brights. Tritanopia used the red-green redistribution, which moved the lost blue into blue again.
        float3 lin = saturate(g); // the cone matrices take linear light
        float3 lms = float3(dot(lin, float3(17.8824, 43.5161, 4.11935)), dot(lin, float3(3.45565, 27.1554, 3.86714)),
                            dot(lin, float3(0.0299566, 0.184309, 1.46709)));
        float3 sim = lms;
        if (cDalt.x < 0.5) sim.x = 2.02344 * lms.y - 2.52581 * lms.z;       // protan: no long-wave cones
        else if (cDalt.x < 1.5) sim.y = 0.494207 * lms.x + 1.24827 * lms.z; // deutan: no medium-wave cones
        else sim.z = -0.395913 * lms.x + 0.801109 * lms.y;                  // tritan: no short-wave cones
        float3 seen = float3(dot(sim, float3(0.0809444479, -0.130504409, 0.116721066)), dot(sim, float3(-0.0102485335, 0.0540193266, -0.113614708)),
                             dot(sim, float3(-0.000365296938, -0.00412161469, 0.693511405)));
        seen = saturate(seen);
        float3 e = pow(lin, 1.0 / 2.2), es = pow(seen, 1.0 / 2.2);
        float3 err = e - es; // what the eye loses, on encoded values
        float3 fixd = cDalt.x < 1.5 ? e + float3(0.0, 0.7 * err.r + err.g, 0.7 * err.r + err.b) // protan / deutan: into green and blue
                                    : e + float3(err.r + 0.7 * err.b, err.g - 0.7 * err.b, 0.0); // tritan: into red against green
        float3 outE = cDalt.z > 0.5 ? es : saturate(fixd); // simulate: the picture as seen with this color blindness
        g = lerp(lin, pow(saturate(outE), 2.2), cDalt.y);
    }
    return g;
}

// The light filters, added in linear light on top of the graded picture
float3 LightFilters(float2 uv, float3 g)
{
    // Glow (06/10: it read only the pixel's own blurred colour, so no halo spread past a lamp and averaging dimmed small
    // lamps below the threshold): the bright parts around the pixel gathered on a disc, thresholded per tap, screened in
    [branch] if (cFlagC.x > 0.5)
    {
        float r = 0.01 + 0.07 * cGlow.z; // radius, a share of the screen height
        float2 k = float2(r * cSize.x / cSize.y, r);
        float3 sum = 0.0;
        float wsum = 0.0;
        [unroll] for (int i = 0; i < 16; i++)
        {
            float a = i * 2.39996, d = sqrt((i + 0.5) / 16.0);
            float3 c = Decode(tex2Dlod(sQuart, float4(uv + float2(cos(a), sin(a)) * k * d, 0, 0)).rgb);
            float w = exp(-3.0 * d * d);
            sum += c * smoothstep(cGlow.y * 0.5, cGlow.y + 0.05, dot(c, kLum)) * w;
            wsum += w;
        }
        float3 bright = sum / wsum * float3(1.0 + 0.25 * cGlow.w, 1.0, 1.0 - 0.25 * cGlow.w) * cGlow.x * 2.5;
        bright *= lerp(float3(1.0, 1.0, 1.0), cGlowC.rgb, cGlowC.w); // the halo's own color
        g = 1.0 - (1.0 - saturate(g)) * (1.0 - saturate(bright)); // screened: it lifts, never clips
    }
    [branch] if (cFlagC.y > 0.5) // Halation: a tight coloured halo around the strongest light, gathered around the pixel
    {
        float2 k = float2(cHal.z * cSize.x / cSize.y, cHal.z);
        float h = 0.0;
        [unroll] for (int j = 0; j < 8; j++)
        {
            float a = j * 0.785398;
            float3 c = Decode(tex2Dlod(sQuart, float4(uv + float2(cos(a), sin(a)) * k, 0, 0)).rgb);
            h += smoothstep(cHal.y * 0.6, cHal.y + 0.05, dot(c, kLum));
        }
        g = 1.0 - (1.0 - saturate(g)) * (1.0 - saturate(cHalC.rgb * (h / 8.0) * cHal.x * 1.5));
    }
    [branch] if (cFlagC.z > 0.5) // Dreamy (Orton): the picture screened with a soft blurred copy, a little more color
    {
        float3 b = lerp(Decode(tex2Dlod(sQuart, float4(uv, 0, 0)).rgb), Decode(SampleBase(uv)), cDream.y);
        float3 s = 1.0 - (1.0 - saturate(g)) * (1.0 - saturate(b) * 0.6);
        s = Saturate3(s, 1.0 + cDream.z);
        g = lerp(g, s, cDream.x);
    }
    return g;
}

// Fake HDR: local contrast in log luminance. The pixel against a blurred average (its local "base"): the base is
// compressed (shadows lifted, highlights pulled down) and the detail on top boosted. Where pixel and base differ a lot
// (a roof against the sky) the detail boost is held back, so no dark or bright halos form around edges.
float3 FakeHdr(float2 uv, float3 g)
{
    float Lp = dot(g, kLum);
    if (Lp < 1e-5) return g;
    float Lh = dot(Decode(tex2Dlod(sHalf, float4(uv, 0, 0)).rgb), kLum);
    float Lq = dot(Decode(tex2Dlod(sQuart, float4(uv, 0, 0)).rgb), kLum);
    float Lb = cHdr.y < 0.5 ? lerp(Lh, Lq, cHdr.y * 2.0) : lerp(Lq, dot(Decode(SampleBase(uv)), kLum), cHdr.y * 2.0 - 1.0);
    Lb = max(Lb, 1e-5);
    float detail = log2(Lp / Lb);
    float base = log2(Lb / 0.18);
    base *= base < 0.0 ? 1.0 - 0.6 * cHdr.z * cHdr.x : 1.0 - 0.6 * cHdr.w * cHdr.x;
    float keep = 1.0 / (1.0 + detail * detail * cHdr2.x * 6.0);
    detail *= 1.0 + 1.5 * cHdr.x * keep;
    float Ln = 0.18 * exp2(base + detail);
    g *= clamp(Ln / Lp, 0.25, 4.0);
    return Saturate3(g, 1.0 + cHdr2.y * cHdr.x);
}
)HLSL"
                            R"HLSL(
float4 PicturePS(float2 uv : TEXCOORD0) : COLOR0
{
    float3 f = tex2Dlod(sFrame, float4(uv, 0, 0)).rgb;
    float3 s = tex2Dlod(sScene, float4(uv, 0, 0)).rgb;
    float3 d = abs(f - s);
    // no scene copy this frame (the game did not draw its scene into the back buffer): everything is filtered, UI included
    // UI mask (05/10, user: new strong filters still touched translucent panels and text shadows): the scene copy comes
    // from the same back buffer, so a pixel the UI never touched is identical bit for bit; any change of one 8-bit step
    // or more is UI and is left exactly as the game drew it. (It was saturate(diff x 64): a soft edge was half filtered.)
    // 06/10 (user: a box around the pie menu): a faint veil, like the pie menu's backing rectangle (a few steps darker),
    // was a box of unfiltered picture. Now only a clear change (from 4 to 24 steps, smoothly) counts as UI; under a faint
    // veil the scene copy is filtered and the veil applied again as the ratio frame / scene, as a translucent overlay
    // would be. A pixel the UI never touched is identical bit for bit (ratio 1); text, buttons and panels stay as drawn.
    float dmax = max(d.r, max(d.g, d.b));
    // 06/10 evening (user: every Color setting broken since 2.6.0): back to 2.6.0's model, the final frame graded and the
    // UI pixels (any difference to the scene copy) left alone. The scene-copy grading with a frame/scene "veil" multiplied
    // every pixel by the smallest difference between the copy and the frame and undid the grade across the picture.
    float ui = cLook.y > 0.5 ? saturate(dmax * 64.0) : 0.0;
    float3 veil = float3(1.0, 1.0, 1.0);
    bool scene = ui < 0.5;

    // CRT: the scene seen through curved glass (sampled further out toward the corners); outside it is black
    float2 suv = uv;
    float2 crtQ = uv * 2.0 - 1.0;
    [branch] if (cFlagE.y > 0.5 && scene)
    {
        crtQ *= 1.0 + cCrt.y * 0.12 * dot(crtQ, crtQ) * float2(0.7, 1.0);
        suv = lerp(uv, crtQ * 0.5 + 0.5, cCrt.x);
    }
    float3 fs = (cFlagE.y > 0.5 && scene) ? SceneTap(suv) : f; // the scene under any veil (see the UI mask)
    fs = (cDeband.w > 0.5 && scene) ? Deband(suv, fs) : fs; // the scene only; the UI keeps its sharp edges
    [branch] if (cFlagD.z > 0.5 && scene) fs = Prism(suv, fs);
    // 3DFX soft pixels: the old cards' output filter blurred each pixel with its horizontal neighbours
    [branch] if (cFlagE.x > 0.5 && cFx2.x > 0.0 && scene)
    {
        float3 l = SceneTap(suv - float2(cSize.x, 0)), r = SceneTap(suv + float2(cSize.x, 0));
        fs = lerp(fs, (l + fs * 2.0 + r) * 0.25, saturate(cFx2.x) * cFx.x);
    }
    // sharpening (scene only): the difference to the 4 neighbours added back, limited to their range (no halos)
    [branch] if (cDetail.x > 0.0 && scene)
    {
        float3 n0 = SceneTap(uv + float2(cSize.x, 0)), n1 = SceneTap(uv - float2(cSize.x, 0));
        float3 n2 = SceneTap(uv + float2(0, cSize.y)), n3 = SceneTap(uv - float2(0, cSize.y));
        float3 lo = min(min(n0, n1), min(n2, n3)), hi = max(max(n0, n1), max(n2, n3));
        fs = clamp(fs + cDetail.x * (fs - (n0 + n1 + n2 + n3) * 0.25), min(lo, fs), max(hi, fs));
    }
    // adaptive sharpening (scene only; the contrast-adaptive idea of AMD's CAS): a negative lobe on the 4 neighbours whose
    // weight shrinks where the local range is already high, so soft detail sharpens and hard edges get no halo
    [branch] if (cFlagG.y > 0.5 && scene)
    {
        float3 a = SceneTap(uv - float2(0, cSize.y)), b = SceneTap(uv - float2(cSize.x, 0));
        float3 d = SceneTap(uv + float2(cSize.x, 0)), e = SceneTap(uv + float2(0, cSize.y));
        float3 mn = min(fs, min(min(a, b), min(d, e))), mx = max(fs, max(max(a, b), max(d, e)));
        float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
        float3 w = -amp * lerp(0.125, 0.2, cCas.x);
        fs = saturate((fs + (a + b + d + e) * w) / (1.0 + 4.0 * w));
    }
    // Tilt-shift: outside a sharp horizontal band the picture blurs more and more (1/2 then 1/4 size copies)
    float tiltM = 0.0;
    [branch] if (cFlagD.y > 0.5 && scene)
    {
        float dist = abs(uv.y - cTilt.y) - cTilt.z * 0.5;
        tiltM = saturate(dist / 0.3);
        tiltM = tiltM * tiltM * (3.0 - 2.0 * tiltM) * cTilt.x;
        float3 b1 = tex2Dlod(sHalf, float4(uv, 0, 0)).rgb, b2 = tex2Dlod(sQuart, float4(uv, 0, 0)).rgb;
        float far = saturate(tiltM * 2.0 - 1.0);
        [branch] if (cExtB.w > 0.0 && far > 0.0) // stronger blur: a wide ring of taps on the 1/8 scene on top of the 1/4 one
        {
            float r = 0.03 * cExtB.w;
            float2 k = float2(r * cSize.x / cSize.y, r);
            float3 wide = SampleBase(uv);
            [unroll] for (int i = 0; i < 8; i++)
            {
                float a = i * 0.785398 + 0.39;
                wide += tex2Dlod(sBase, float4(uv + float2(cos(a), sin(a)) * k, 0, 0)).rgb;
            }
            b2 = lerp(b2, wide / 9.0, saturate(cExtB.w * 1.5));
        }
        fs = lerp(fs, lerp(b1, b2, far), saturate(tiltM * 2.0));
    }
    float3 g = Decode(fs);
    // auto exposure (scene only): toward a target brightness, from the scene average adapted over time (AdaptPS)
    [branch] if (cFlagG.x > 0.5 && scene)
    {
        float adapted = max(tex2Dlod(sAdapt, float4(0.5, 0.5, 0, 0)).r, 1e-4);
        g *= lerp(1.0, clamp(cAuto.y / adapted, cAuto.z, cAuto.w), cAuto.x);
    }

    // clarity: the pixel's luminance against the smooth local average, as a ratio (log2), limited so strong edges
    // (the ratio far from 1) get almost nothing and cannot form halos; midtones only
    [branch] if (cDetail.y != 0.0 && scene)
    {
        float Lp = dot(g, kLum), Lb = dot(Decode(SampleBase(uv)), kLum);
        if (Lp > 1e-5 && Lb > 1e-5)
        {
            float r = log2(Lp / Lb);
            float e = saturate(pow(Lp, 1.0 / 2.2));
            g *= exp2(cDetail.y * r / (1.0 + r * r) * 4.0 * e * (1.0 - e));
        }
    }
    [branch] if (cFlagF.y > 0.5 && scene) g = FakeHdr(uv, g);

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
    // saturation: all colours, times the colour mixer's value for this hue (tilt-shift adds the toy-model saturation)
    {
        float sat = cLook.x * (1.0 + cTilt.w * (cFlagD.y > 0.5 ? cTilt.x : 0.0));
        [branch] if (cMixB.z > 0.5) sat *= MixerSaturation(g);
        g = Saturate3(g, sat);
    }

    [branch] if (scene) g = ColorLooks(uv, g);

    // Emphasize: grey outside a band of distance around the focus (the centre of the screen, or a set distance)
    [branch] if (cFlagD.x > 0.5 && scene)
    {
        float zf = cEmph.w;
        if (cEmph.z > 0.5)
        {
            float o = 0.03;
            zf = min(min(ViewZ(tex2Dlod(sDepth, float4(0.5, 0.5, 0, 0)).r), ViewZ(tex2Dlod(sDepth, float4(0.5 - o, 0.5, 0, 0)).r)),
                     min(min(ViewZ(tex2Dlod(sDepth, float4(0.5 + o, 0.5, 0, 0)).r), ViewZ(tex2Dlod(sDepth, float4(0.5, 0.5 - o, 0, 0)).r)),
                         ViewZ(tex2Dlod(sDepth, float4(0.5, 0.5 + o, 0, 0)).r)));
            zf = min(zf, 2000.0);
        }
        float z = ViewZ(tex2Dlod(sDepth, float4(uv, 0, 0)).r);
        float half = zf * cEmph2.x * 0.5;
        float t = saturate((abs(z - zf) - half) / max(zf * cEmph2.y, 0.5));
        t = t * t * (3.0 - 2.0 * t);
        float L = dot(g, kLum);
        g = lerp(g, lerp(g, float3(L, L, L), cEmph.y), t * cEmph.x);
    }

    [branch] if (scene) g = LightFilters(uv, g);

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

    // back to gamma 2.2 for the 8-bit back buffer
    float3 o = pow(saturate(g), 1.0 / 2.2);
    float2 px = floor(uv * cSize.zw);
    // Film grain (rebuilt 05/10, user: the old blocky per-cell noise looked like dirt): smooth value noise in two sizes
    // (the grain and a finer one), multiplying the brightness like silver grain does, strongest in the midtones and
    // fading in the blacks and whites; "More in the shadows" moves it toward the darker tones. The same grain every frame.
    [branch] if (cFlagD.w > 0.5 && scene)
    {
        float2 gp = px + cGrain.w * float2(5.31, 3.77); // animated: a new seed every frame (0 = the same grain every frame)
        float n = GrainNoise(gp / cGrain.y) * 0.65 + GrainNoise(gp / (cGrain.y * 0.5) + 17.31) * 0.35;
        n = (n - 0.5) * 3.2; // about -1 .. 1
        float3 n3 = float3(n, n, n);
        [branch] if (cExtC.x > 0.0) // color grain: red and blue get their own grain
        {
            float nr = (GrainNoise(gp / cGrain.y + 41.7) - 0.5) * 3.2, nb = (GrainNoise(gp / cGrain.y + 83.1) - 0.5) * 3.2;
            n3 = lerp(n3, float3(nr, n, nb), cExtC.x);
        }
        float L = saturate(dot(o, kLum));
        float mid = 4.0 * L * (1.0 - L), dark = saturate(1.6 * (1.0 - L)) * saturate(L * 12.0);
        float w = lerp(mid, dark, cGrain.z);
        o *= 1.0 + n3 * cGrain.x * 0.32 * w;
    }
    bool posterized = false;
    [branch] if (cFlagE.x > 0.5 && scene)
    {
        // 3DFX: gamma, 16-bit style color (fewer red/blue levels than green) with an ordered dither, then scanlines
        float3 a = pow(saturate(o), cFx2.y);
        float3 lv = float3(cFx.y, cFx2.w, cFx.y);
        a = floor(a * lv + 0.5 + Bayer4(px) * cFx.z) / lv;
        float scan = fmod(px.y, cFx2.z) >= cFx2.z * 0.5 ? 1.0 - cFx.w * 0.5 : 1.0;
        o = lerp(o, saturate(a) * scan, cFx.x);
        posterized = true;
    }
    [branch] if (cFlagE.y > 0.5 && scene)
    {
        // CRT: phosphor stripes (an aperture grille), scanlines, darker glass edges, black outside the curve
        float3 a = o;
        float col = fmod(px.x, 3.0);
        float3 mask = col < 1.0 ? float3(1.0, 0.7, 0.7) : (col < 2.0 ? float3(0.7, 1.0, 0.7) : float3(0.7, 0.7, 1.0));
        a *= lerp(1.0, mask * 1.15, cCrt.z);
        float ph = frac(px.y / cCrt2.y);
        a *= 1.0 - cCrt.w * 0.6 * smoothstep(0.35, 0.5, abs(ph - 0.5));
        float2 e = abs(crtQ);
        a *= 1.0 - cCrt2.x * smoothstep(0.75, 1.0, max(e.x, e.y));
        if (e.x > 1.0 || e.y > 1.0) a = 0.0;
        o = lerp(o, saturate(a), cCrt.x);
    }
    // a fixed dither (interleaved gradient noise, the same pattern every frame, below one 8-bit step) so the grading does
    // not turn smooth gradients into steps
    if (!posterized) o += (frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715)))) - 0.5) / 255.0;
    o = saturate(o * veil); // a faint veil over the filtered scene again (1 where the UI never drew)
    if (before) o = f;
    if (divider) o = float3(1, 0, 0);
    return float4(lerp(o, f, ui), 1.0);
}

// Auto exposure's 1x1 pass: the scene's average luminance (log average of an 8x8 grid of the 1/8 scene, centre weighted;
// the scene copy has no UI) blended into the previous value, so the eye adapts over time instead of jumping
float4 AdaptPS(float2 uv : TEXCOORD0) : COLOR0
{
    float sum = 0.0, wsum = 0.0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
        {
            float2 p = (float2(x, y) + 0.5) / 8.0;
            float w = 1.0 - 0.6 * saturate(length(p - 0.5) * 1.6);
            sum += log2(max(dot(Decode(tex2Dlod(sBase, float4(p, 0, 0)).rgb), kLum), 1e-4)) * w;
            wsum += w;
        }
    float now = exp2(sum / wsum);
    float prev = tex2Dlod(sAdapt, float4(0.5, 0.5, 0, 0)).r;
    float v = prev > 0.0 ? lerp(prev, now, cAdapt.x) : now; // 0 = just created: start at the current value
    return float4(v, v, v, 1.0);
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
const ShaderCache::Id kDepthCopyPsId = [] { // the scene depth into R32F (see Gpu::depthTex)
    ShaderCache::Desc d;
    d.tag = "Picture DepthCopyPS";
    d.source = "sampler2D sDepth : register(s0);\nfloat4 DepthCopyPS(float2 uv : TEXCOORD0) : COLOR0 { return tex2Dlod(sDepth, float4(uv, 0, 0)).r; }\n";
    d.sourceName = "picture_depth_copy.hlsl";
    d.entry = "DepthCopyPS";
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    d.priority = 1;
    return ShaderCache::Add(std::move(d));
}();
const ShaderCache::Id kAdaptPsId = [] { // auto exposure's 1x1 pass, from the same source
    ShaderCache::Desc d;
    d.tag = "Picture AdaptPS";
    d.source = kShaderSource;
    d.sourceName = "picture.hlsl";
    d.entry = "AdaptPS";
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    d.priority = 1;
    return ShaderCache::Add(std::move(d));
}();

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
    // auto exposure: two 1x1 float targets, the adapted luminance (one read, the other written each frame)
    IDirect3DTexture9* adaptTex[2] = {};
    IDirect3DSurface9* adaptSurf[2] = {};
    int adaptCur = 0;
    IDirect3DPixelShader9* adaptPs = nullptr;
    bool adaptTried = false;
    LARGE_INTEGER lastPass{};
    // LUT: the loaded strip (managed), its cells per side and the file it came from
    IDirect3DTexture9* lutTex = nullptr;
    int lutSize = 0;
    std::string lutLoaded; // the file name tried last (loaded or not)
    static constexpr int kQ = 4;
    IDirect3DQuery9 *qDisjoint[kQ] = {}, *qBegin[kQ] = {}, *qEnd[kQ] = {}, *qFreq[kQ] = {};
    bool qIssued[kQ] = {};
    int qNext = 0;
    // frame state (render thread)
    bool hooks = false;
    bool frameReady = true;      // the pass has not run yet this frame
    int sceneDraws = 0;          // depth-tested back buffer draws this frame
    int runDraws = 0;            // depth-tested back buffer draws since the last depth-off one
    bool lastWasScene = false;   // the last back buffer draw was depth-tested
    bool copyAfterStrip = false; // a bloom strip right after the scene: copy once it has drawn
    bool sceneCopied = false;
    // the scene depth copied with the scene (R32F, the device depth as it is), for Emphasize: at the end of the frame the
    // live depth has holes (06/10, user: a grey box around the pie menu with the old fog filter on: the Sim portrait
    // clears the depth of its 256x256 square before drawing the head, and it was taken as infinitely far)
    IDirect3DTexture9* depthTex = nullptr;
    IDirect3DSurface9* depthSurf = nullptr;
    IDirect3DPixelShader9* depthPs = nullptr;
    bool depthTried = false, depthCopied = false;
    IDirect3DSurface9* curRT0 = nullptr; // identity only
    IDirect3DSurface9* backBuffer = nullptr;
};
Gpu gpu;

// Emphasize reads the scene depth: the INTZ swap and the camera are requested only while it is on (reference counted)
bool g_depthRequested = false;
void RequestDepth(bool on) {
    if (on == g_depthRequested) return;
    g_depthRequested = on;
    DepthShare::Request(on);
    PostScene::WantCamera(on);
}

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

struct QuadVertex {
    float x, y, z, rhw, u, v;
};

// The scene depth (INTZ) into gpu.depthTex, in the middle of the game's drawing: every state it touches is put back
void CopyDepth(IDirect3DDevice9* dev) {
    IDirect3DTexture9* depth = DepthShare::Texture();
    if (!depth || !gpu.width) return;
    if (!gpu.depthTex) {
        if (gpu.depthTried) return;
        gpu.depthTried = true;
        if (FAILED(dev->CreateTexture(gpu.width, gpu.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &gpu.depthTex, nullptr)) || !gpu.depthTex ||
            FAILED(gpu.depthTex->GetSurfaceLevel(0, &gpu.depthSurf)) || !gpu.depthSurf) {
            SafeRelease(gpu.depthSurf);
            SafeRelease(gpu.depthTex);
            LOG_WARNING("[Picture] No depth copy (R32F target): Emphasize reads the live depth");
            return;
        }
    }
    if (!gpu.depthPs) {
        std::string msg;
        if (ShaderCache::CreatePixelShader(dev, kDepthCopyPsId, &gpu.depthPs, &msg) != ShaderCache::Result::Ok) return; // not compiled yet: next frame
    }
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    IDirect3DBaseTexture9* tex0 = nullptr;
    IDirect3DVertexBuffer9* stream = nullptr;
    UINT streamOffset = 0, streamStride = 0;
    DWORD fvf = 0;
    D3DVIEWPORT9 vp{};
    constexpr D3DRENDERSTATETYPE kStates[] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
                                              D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_COLORWRITEENABLE,
                                              D3DRS_CLIPPLANEENABLE, D3DRS_SEPARATEALPHABLENDENABLE};
    constexpr D3DSAMPLERSTATETYPE kSamp[] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE};
    DWORD rs[std::size(kStates)]{}, ss[std::size(kSamp)]{};
    dev->GetRenderTarget(0, &rt);
    ExtraHooks::RawGetDepthStencilSurface(dev, &ds);
    dev->GetPixelShader(&ps);
    dev->GetVertexShader(&vs);
    dev->GetVertexDeclaration(&decl);
    dev->GetFVF(&fvf);
    dev->GetTexture(0, &tex0);
    dev->GetStreamSource(0, &stream, &streamOffset, &streamStride);
    dev->GetViewport(&vp);
    for (size_t i = 0; i < std::size(kStates); i++) dev->GetRenderState(kStates[i], &rs[i]);
    for (size_t i = 0; i < std::size(kSamp); i++) dev->GetSamplerState(0, kSamp[i], &ss[i]);

    ExtraHooks::RawSetDepthStencilSurface(dev, nullptr); // the depth is read, so it must not be bound
    dev->SetRenderTarget(0, gpu.depthSurf);
    dev->SetPixelShader(gpu.depthPs);
    dev->SetVertexShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetTexture(0, depth);
    const DWORD passStates[std::size(kStates)] = {D3DZB_FALSE, FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, FALSE, FALSE, FALSE, 0xF, 0, FALSE};
    for (size_t i = 0; i < std::size(kStates); i++) dev->SetRenderState(kStates[i], passStates[i]);
    const DWORD passSamp[std::size(kSamp)] = {D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, 0};
    for (size_t i = 0; i < std::size(kSamp); i++) dev->SetSamplerState(0, kSamp[i], passSamp[i]);
    const float x1 = static_cast<float>(gpu.width) - 0.5f, y1 = static_cast<float>(gpu.height) - 0.5f;
    const QuadVertex v[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {x1, -0.5f, 0, 1, 1, 0}, {-0.5f, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    gpu.depthCopied = SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVertex)));

    dev->SetRenderTarget(0, rt); // resets the viewport: restored below
    ExtraHooks::RawSetDepthStencilSurface(dev, ds);
    dev->SetPixelShader(ps);
    dev->SetVertexShader(vs);
    if (decl) dev->SetVertexDeclaration(decl);
    else dev->SetFVF(fvf);
    dev->SetTexture(0, tex0);
    dev->SetStreamSource(0, stream, streamOffset, streamStride); // DrawPrimitiveUP clears stream 0
    for (size_t i = 0; i < std::size(kStates); i++) dev->SetRenderState(kStates[i], rs[i]);
    for (size_t i = 0; i < std::size(kSamp); i++) dev->SetSamplerState(0, kSamp[i], ss[i]);
    dev->SetViewport(&vp);
    SafeRelease(rt);
    SafeRelease(ds);
    SafeRelease(ps);
    SafeRelease(vs);
    SafeRelease(decl);
    SafeRelease(tex0);
    SafeRelease(stream);
}

// Copies the back buffer as "the scene" (see the frame flow above); with Emphasize on, the depth too.
void CopyScene(IDirect3DDevice9* dev) {
    if (!gpu.ready) return;
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        gpu.sceneCopied = SUCCEEDED(dev->StretchRect(bb, nullptr, gpu.sceneSurf, nullptr, D3DTEXF_NONE));
        bb->Release();
    }
    if (g_depthRequested) CopyDepth(dev);
}

// Back buffer draws of the game: find the point between the scene (with its bloom) and the UI
void OnGameDraw(D3D9Hooks::DeviceContext& ctx, bool isStripOfTwo) {
    IDirect3DDevice9* dev = ctx.device;
    if (DepthShare::InternalPass()) return;
    if (!gpu.curRT0 || gpu.curRT0 != gpu.backBuffer) return;
    DWORD z = ctx.ZEnable();
    // The depth test on but passing everything with no depth write uses no depth: not scene (06/10, frame capture with
    // the pie menu open: ~25 full-screen copies of the back buffer at the end of the frame, z on / ALWAYS / no write,
    // counted as more scene, so the end-of-frame copy took the UI and the Sim portrait's cleared depth: a grey box)
    if (z != D3DZB_FALSE && !ctx.ZWriteEnable()) {
        DWORD func = D3DCMP_LESSEQUAL;
        if (SUCCEEDED(dev->GetRenderState(D3DRS_ZFUNC, &func)) && func == D3DCMP_ALWAYS) z = D3DZB_FALSE;
    }
    if (z != D3DZB_FALSE) {
        gpu.sceneDraws++;
        gpu.runDraws++;
        gpu.lastWasScene = true;
        gpu.copyAfterStrip = false;
        return;
    }
    const int run = gpu.runDraws;
    gpu.runDraws = 0;
    if (gpu.sceneDraws < kMinSceneDraws) return;
    if (gpu.lastWasScene) { // depth-tested -> depth-off: possibly the end of the scene
        gpu.lastWasScene = false;
        // After the first copy, a short depth-tested run is part of the UI, not more scene: the Sim portrait of the pie
        // menu is drawn in 3D over the UI, and copying after it put the menu into "the scene" (Emphasize turned it grey).
        // Interiors draw depth-off pieces in the middle of the scene and then many more depth-tested draws: those still copy.
        if (gpu.sceneCopied && run < kMinSceneDraws) return;
        // the bloom composite strip: part of the scene, copy after it; and while ambient occlusion, edge smoothing or Depth
        // Blur have still to run this frame (PostScene counts a depth test with ALWAYS as scene and runs them later), the
        // copy waits for them, or every pixel they change reads as UI and keeps the game's colours (06/10: Color did nothing)
        if (isStripOfTwo || PostScene::EffectsPending()) {
            gpu.copyAfterStrip = true;
            return;
        }
        CopyScene(dev);
        return;
    }
    if (gpu.copyAfterStrip && !PostScene::EffectsPending()) {
        gpu.copyAfterStrip = false;
        CopyScene(dev);
    }
}

// Color at the end of the scene (06/10 evening): the pass runs as the last post-scene effect, on the finished scene before the
// game draws its UI, with no UI mask; the end-of-frame pass is only the fallback for a frame without that boundary. Comparing
// a scene copy with the final frame failed: the game redraws the whole picture from a reduced copy after its UI (137 times
// a frame in a capture), so every pixel read as UI and no setting showed.
bool g_atBoundary = false, g_boundaryDone = false;
void BoundaryEffect(IDirect3DDevice9* dev) {
    if (g_boundaryDone) return; // once a frame (a boundary without the shared depth may come before a later one with it)
    g_atBoundary = true;
    Picture::Get().OnEndScene(dev);
    g_atBoundary = false;
}

void OnFrameBoundary(IDirect3DDevice9* dev) {
    g_boundaryDone = false;
    IDirect3DSurface9* s = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &s)) && s) {
        gpu.backBuffer = s;
        D3DSURFACE_DESC bd{};
        // a back buffer of another size than the resources (a Reset BeforeReset never saw): made again at the next pass
        if (gpu.ready && SUCCEEDED(s->GetDesc(&bd)) && (bd.Width != gpu.width || bd.Height != gpu.height)) Picture::Get().BeforeReset();
        s->Release();
    }
    // Render target 0 read again every frame, not only after a Reset (user 06/10: a friend's game with dxwrapper and
    // Sims3SettingsSetter showed no Color filter on entering CAW until it was turned off and on, which reads it again; a
    // change made where the SetRenderTarget hook does not see it left the remembered one stale for good)
    if (SUCCEEDED(dev->GetRenderTarget(0, &s)) && s) {
        gpu.curRT0 = s;
        s->Release();
    }
    gpu.frameReady = true;
    gpu.sceneDraws = 0;
    gpu.runDraws = 0;
    gpu.lastWasScene = false;
    gpu.copyAfterStrip = false;
    gpu.sceneCopied = false;
    gpu.depthCopied = false;
}

void RegisterHooks(IDirect3DDevice9* dev) {
    if (gpu.hooks) return;
    gpu.hooks = true;
    PostScene::Add(PostScene::kPicture, BoundaryEffect, false); // Color on the finished scene, before the UI (see BoundaryEffect); runs without the shared depth too
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
        OnGameDraw(ctx, false);
        return HookAction::Continue;
    }, kDrawPriority);
    RegisterDrawPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE type, UINT, UINT prims) {
        OnGameDraw(ctx, type == D3DPT_TRIANGLESTRIP && prims == 2);
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


// ---- LUT files: Apex Radiance\LUTs\*.png, horizontal strips of size x size cells (1024x32, 4096x64, ...) ----
std::mutex g_lutMutex;
std::string g_lutStatus; // what the LUT card shows (written by the render thread)

std::wstring LutFolder() { return ApexPaths::ApexDirectory() + L"LUTs\\"; }

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::vector<std::string> ListLuts() {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW((LutFolder() + L"*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(ApexUtil::ToUtf8(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

void SetLutStatus(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_lutMutex);
    g_lutStatus = s;
}

// Decodes the PNG (WIC, any bit depth -> 32-bit BGRA) into a managed texture; render thread, on a change of file only
bool LoadLut(IDirect3DDevice9* dev, const std::string& file) {
    SafeRelease(gpu.lutTex);
    gpu.lutSize = 0;
    if (file.empty()) {
        SetLutStatus("No LUT chosen");
        return false;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT w = 0, h = 0;
    std::vector<BYTE> px;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateDecoderFromFilename((LutFolder() + Widen(file)).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));
    const bool shape = ok && h >= 8 && h <= 128 && w == h * h;
    if (shape) {
        px.resize(static_cast<size_t>(w) * h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data()));
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    if (!ok) {
        SetLutStatus("Could not read " + file);
        return false;
    }
    if (!shape) {
        SetLutStatus(std::format("{} is {}x{}: a LUT strip is size x size cells side by side (for example 1024x32 or 4096x64)", file, w, h));
        return false;
    }
    D3DLOCKED_RECT lr{};
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &gpu.lutTex, nullptr)) || !gpu.lutTex ||
        FAILED(gpu.lutTex->LockRect(0, &lr, nullptr, 0))) {
        SafeRelease(gpu.lutTex);
        SetLutStatus("The LUT texture could not be created");
        return false;
    }
    for (UINT y = 0; y < h; y++)
        std::memcpy(static_cast<BYTE*>(lr.pBits) + static_cast<size_t>(lr.Pitch) * y, px.data() + static_cast<size_t>(w) * 4 * y, static_cast<size_t>(w) * 4);
    gpu.lutTex->UnlockRect(0);
    gpu.lutSize = static_cast<int>(h);
    SetLutStatus(std::format("{}: {} x {} x {}", file, h, h, h));
    LOG_INFO(std::format("[Picture] LUT loaded: {} ({} cells per side)", file, h));
    return true;
}

// Explorer on the LUTs folder (created if needed), on a short-lived thread with COM: ShellExecuteW called from the menu frame
// pumps the game window's messages and re-enters the overlay (captures.cpp, 30/09 crash)
void ShowLutFolder() {
    const std::wstring folder = LutFolder();
    CreateDirectoryW(folder.c_str(), nullptr);
    // (07/10, players' Runtime Error: started and run under HookGuard::StartDetached, so neither can end the game)
    HookGuard::StartDetached("Picture: open the LUTs folder", [folder] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (SUCCEEDED(com)) CoUninitialize();
    });
}

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
// [qol.picture.filters]: one row per setting, used both to save and to load (so the two cannot drift apart)
struct FilterBoolKey {
    const char* key;
    bool PictureParams::*field;
};
struct FilterFloatKey {
    const char* key;
    float PictureParams::*field;
};
struct FilterArrayKey {
    const char* key;
    float (PictureParams::*field)[3];
};
const FilterBoolKey kFilterBools[] = {
    {"technicolor1", &PictureParams::tech1}, {"technicolor2", &PictureParams::tech2}, {"dpx", &PictureParams::dpx},
    {"colourfulness", &PictureParams::colourful}, {"night_mode", &PictureParams::night}, {"vintage", &PictureParams::vintage},
    {"cross_process", &PictureParams::crossProcess}, {"black_and_white", &PictureParams::bw}, {"glow", &PictureParams::glow},
    {"halation", &PictureParams::halation}, {"dreamy", &PictureParams::dreamy}, 
    {"fake_hdr", &PictureParams::fakeHdr},
    {"emphasize", &PictureParams::emphasize}, {"emphasize_auto", &PictureParams::emphAuto}, {"tilt_shift", &PictureParams::tiltShift},
    {"prism", &PictureParams::prism}, {"grain", &PictureParams::grain},
    {"retro_3dfx", &PictureParams::retro3dfx}, {"crt", &PictureParams::crt},
    {"filmic_pass", &PictureParams::filmic}, {"tint_filter", &PictureParams::tintFilter}, {"levels", &PictureParams::levels},
    {"lut", &PictureParams::lut}, {"auto_exposure", &PictureParams::autoExposure}, {"cas", &PictureParams::cas},
    {"daltonize", &PictureParams::daltonize}, {"daltonize_simulate", &PictureParams::daltonSimulate}, {"grain_animated", &PictureParams::grainAnimated}};
// Exclude auxiliary mode switches (auto focus, simulation and animated grain).
const FilterBoolKey kToggleFilters[] = {
    {"technicolor1", &PictureParams::tech1},
    {"technicolor2", &PictureParams::tech2},
    {"dpx", &PictureParams::dpx},
    {"colourfulness", &PictureParams::colourful},
    {"night_mode", &PictureParams::night},
    {"vintage", &PictureParams::vintage},
    {"cross_process", &PictureParams::crossProcess},
    {"black_and_white", &PictureParams::bw},
    {"glow", &PictureParams::glow},
    {"halation", &PictureParams::halation},
    {"dreamy", &PictureParams::dreamy},
    {"fake_hdr", &PictureParams::fakeHdr},
    {"emphasize", &PictureParams::emphasize},
    {"tilt_shift", &PictureParams::tiltShift},
    {"prism", &PictureParams::prism},
    {"grain", &PictureParams::grain},
    {"retro_3dfx", &PictureParams::retro3dfx},
    {"crt", &PictureParams::crt},
    {"filmic_pass", &PictureParams::filmic},
    {"tint_filter", &PictureParams::tintFilter},
    {"levels", &PictureParams::levels},
    {"lut", &PictureParams::lut},
    {"auto_exposure", &PictureParams::autoExposure},
    {"cas", &PictureParams::cas},
    {"daltonize", &PictureParams::daltonize},
 };
static_assert(std::size(kToggleFilters) == PictureParams::kFilterCount);
std::atomic<int> g_filterRecording{-1};
std::string g_filterRecordingName; // render-thread UI label of the filter being edited
std::atomic<unsigned> g_filterRequests{0};
std::mutex g_filterCaptureMutex;
FilterShortcut g_filterCaptured;
std::atomic<bool> g_filterCaptureCancelled{false};

FilterShortcut HeldFilterKeys() {
    FilterShortcut b;
    for (unsigned k=7; k<256; ++k) {
        if (k>=VK_LSHIFT && k<=VK_RMENU) continue;
        if (GetKeyState(k)<0) b.Add(k);
    }
    return b;
}
std::string FilterKeyText(const FilterShortcut& b) {
    std::string text;
    auto append=[&](unsigned k) { if (!text.empty()) text += "+"; text += ApexConfig::KeyName(k); };
    for (unsigned k : {unsigned(VK_CONTROL),unsigned(VK_SHIFT),unsigned(VK_MENU)}) if (b.Has(k)) append(k);
    for (unsigned k=7;k<256;++k) if (k!=VK_CONTROL && k!=VK_SHIFT && k!=VK_MENU && b.Has(k)) append(k);
    return text;
}
FilterShortcut SingleChord(const ApexConfig::KeyChord& k) {
    FilterShortcut b; if (!k.vk) return b;
    b.Add(k.vk); if(k.ctrl)b.Add(VK_CONTROL); if(k.shift)b.Add(VK_SHIFT); if(k.alt)b.Add(VK_MENU); return b;
}
bool ReservedFilterKeys(const FilterShortcut& b) {
    if(b.Empty()) return false;
    const auto ui=ApexConfig::GetUi();
    if (b==SingleChord(ui.toggle) || b==SingleChord(ui.searchKey) || b==SingleChord(ui.peekKey) ||
        b==SingleChord(ui.pictureCompareKey) || b==SingleChord({VK_F10,false,false,false})) return true;
    for (int i=0;i<int(Hotkeys::Action::Count);++i) {
        if (i==int(Hotkeys::Action::Screenshot) && !ui.screenshotShortcutEnabled) continue;
        if (b==SingleChord(Hotkeys::Key(Hotkeys::Action(i)))) return true;
    }
    return false;
}

const FilterFloatKey kFilterFloats[] = {
    {"technicolor1_amount", &PictureParams::tech1Amount}, {"technicolor1_cyan", &PictureParams::tech1Cyan},
    {"technicolor1_saturation", &PictureParams::tech1Saturation}, {"technicolor2_amount", &PictureParams::tech2Amount},
    {"technicolor2_saturation", &PictureParams::tech2Saturation}, {"technicolor2_brightness", &PictureParams::tech2Brightness},
    {"dpx_amount", &PictureParams::dpxAmount}, {"dpx_contrast", &PictureParams::dpxContrast}, {"dpx_saturation", &PictureParams::dpxSaturation},
    {"colourfulness_amount", &PictureParams::colourfulAmount}, {"colourfulness_protect", &PictureParams::colourfulProtect},
    {"night_amount", &PictureParams::nightAmount}, {"night_darkness", &PictureParams::nightDarkness}, {"night_blue", &PictureParams::nightBlue},
    {"night_keep_lamps", &PictureParams::nightKeepLamps}, {"vintage_amount", &PictureParams::vintageAmount},
    {"vintage_fade", &PictureParams::vintageFade}, {"vintage_warmth", &PictureParams::vintageWarmth}, {"vintage_colors", &PictureParams::vintageColors},
    {"cross_amount", &PictureParams::crossAmount}, {"cross_contrast", &PictureParams::crossContrast}, {"bw_amount", &PictureParams::bwAmount},
    {"bw_filter_hue", &PictureParams::bwFilterHue}, {"bw_filter", &PictureParams::bwFilter}, {"bw_tone_hue", &PictureParams::bwToneHue},
    {"bw_tone", &PictureParams::bwTone}, {"bw_contrast", &PictureParams::bwContrast}, {"glow_amount", &PictureParams::glowAmount},
    {"glow_threshold", &PictureParams::glowThreshold}, {"glow_size", &PictureParams::glowSize}, {"glow_warmth", &PictureParams::glowWarmth},
    {"halation_amount", &PictureParams::halationAmount}, {"halation_threshold", &PictureParams::halationThreshold},
    {"halation_hue", &PictureParams::halationHue}, {"dreamy_amount", &PictureParams::dreamyAmount}, {"dreamy_softness", &PictureParams::dreamySoftness},
    {"dreamy_saturation", &PictureParams::dreamySaturation},
    
    {"fake_hdr_amount", &PictureParams::hdrAmount}, {"fake_hdr_radius", &PictureParams::hdrRadius},
    {"fake_hdr_shadows", &PictureParams::hdrShadows}, {"fake_hdr_highlights", &PictureParams::hdrHighlights}, {"fake_hdr_halo", &PictureParams::hdrHalo},
    {"fake_hdr_saturation", &PictureParams::hdrSaturation}, {"emphasize_amount", &PictureParams::emphAmount},
    {"emphasize_distance", &PictureParams::emphDistance}, {"emphasize_width", &PictureParams::emphWidth},
    {"emphasize_softness", &PictureParams::emphSoftness}, {"emphasize_grey", &PictureParams::emphGrey}, {"tilt_amount", &PictureParams::tiltAmount},
    {"tilt_center", &PictureParams::tiltCenter}, {"tilt_width", &PictureParams::tiltWidth}, {"tilt_saturation", &PictureParams::tiltSaturation},
    {"prism_amount", &PictureParams::prismAmount}, {"prism_start", &PictureParams::prismStart}, {"prism_quality", &PictureParams::prismQuality},
    {"grain_amount", &PictureParams::grainAmount}, {"grain_size", &PictureParams::grainSize}, {"grain_shadows", &PictureParams::grainShadows},
    {"3dfx_amount", &PictureParams::fxAmount}, {"3dfx_color_depth", &PictureParams::fxDepth}, {"3dfx_scanlines", &PictureParams::fxScanlines},
    {"3dfx_dither", &PictureParams::fxDither}, {"3dfx_soft_pixels", &PictureParams::fxPixelWidth}, {"3dfx_gamma", &PictureParams::fxGamma},
    {"crt_amount", &PictureParams::crtAmount}, {"crt_curvature", &PictureParams::crtCurvature}, {"crt_mask", &PictureParams::crtMask},
    {"crt_scanlines", &PictureParams::crtScanlines}, {"crt_edges", &PictureParams::crtEdges},
    {"filmic_amount", &PictureParams::filmicAmount}, {"filmic_fade", &PictureParams::filmicFade}, {"filmic_contrast", &PictureParams::filmicContrast},
    {"filmic_bleach", &PictureParams::filmicBleach}, {"filmic_saturation", &PictureParams::filmicSaturation},
    {"tint_filter_hue", &PictureParams::tintFilterHue}, {"tint_filter_amount", &PictureParams::tintFilterAmount},
    {"levels_black", &PictureParams::levelsBlack}, {"levels_white", &PictureParams::levelsWhite},
    {"lut_amount", &PictureParams::lutAmount},
    {"auto_exposure_amount", &PictureParams::autoAmount}, {"auto_exposure_target", &PictureParams::autoTarget},
    {"auto_exposure_speed", &PictureParams::autoSpeed}, {"auto_exposure_range", &PictureParams::autoRange}, {"cas_amount", &PictureParams::casAmount},
    {"daltonize_type", &PictureParams::daltonType}, {"daltonize_amount", &PictureParams::daltonAmount},
    {"technicolor1_brightness", &PictureParams::tech1Brightness}, {"technicolor1_contrast", &PictureParams::tech1Contrast},
    {"vintage_vignette", &PictureParams::vintageVignette}, {"cross_hue", &PictureParams::crossHue}, {"cross_saturation", &PictureParams::crossSaturation},
    {"bw_brightness", &PictureParams::bwBrightness}, {"tint_filter_preserve", &PictureParams::tintPreserve}, {"tint_filter_balance", &PictureParams::tintBalance},
    {"levels_gamma", &PictureParams::levelsGamma}, {"glow_hue", &PictureParams::glowHue}, {"glow_color", &PictureParams::glowColor},
    {"halation_size", &PictureParams::halationSize}, {"tilt_blur", &PictureParams::tiltBlur}, {"grain_color", &PictureParams::grainColor}};
const FilterArrayKey kFilterArrays[] = {{"technicolor2_dye", &PictureParams::tech2Dye}, {"dpx_curve", &PictureParams::dpxCurve},
                                        {"filmic_curve", &PictureParams::filmicCurve}};

const char* const kKeys[] = {"enabled", "exposure", "contrast", "midtones", "shadows", "highlights", "blacks", "temperature", "tint", "saturation", "vibrance",
                             "shadow_hue", "shadow_tint", "highlight_hue", "highlight_tint", "mixer", "deband", "sharpen", "clarity", "vignette", "vignette_size", "basic_enabled", "tones_enabled", "color_enabled", "detail_enabled", "filters_enabled", "filters", "filter_shortcuts"};

} // namespace

// ---- device ----

void Picture::ReleaseResources() {
    gpu.ready = false;
    for (int i = 0; i < 2; i++) {
        SafeRelease(gpu.adaptSurf[i]);
        SafeRelease(gpu.adaptTex[i]);
    }
    SafeRelease(gpu.lutTex);
    gpu.lutSize = 0;
    gpu.lutLoaded = "\x01"; // never a file name: loaded again on the next frame that needs it
    SafeRelease(gpu.frameSurf);
    SafeRelease(gpu.sceneSurf);
    SafeRelease(gpu.frameTex);
    SafeRelease(gpu.sceneTex);
    SafeRelease(gpu.depthSurf);
    SafeRelease(gpu.depthTex);
    gpu.depthTried = false;
    gpu.depthCopied = false;
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
    // auto exposure: optional (without them the filter does nothing); a fresh target holds 0 = "no value yet"
    for (int i = 0; i < 2; i++) {
        if (FAILED(dev->CreateTexture(1, 1, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &gpu.adaptTex[i], nullptr)) || !gpu.adaptTex[i] ||
            FAILED(gpu.adaptTex[i]->GetSurfaceLevel(0, &gpu.adaptSurf[i]))) {
            SafeRelease(gpu.adaptTex[i]);
            break;
        }
        dev->ColorFill(gpu.adaptSurf[i], nullptr, 0);
    }
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

// Smooth gradients (deband) is on the Banding Fix page (a Color tab from 30/09, its own page since 06/10) and follows the Banding Fix's switch, not
// Picture's: with Picture off the pass still runs, with only the deband (every other control neutral), while the Banding
// Fix is on. That smoothing-only pass is skipped in a frame without a copy of the scene (it would smooth the game's menus).
static PictureParams Effective(const PictureParams& q) {
    PictureParams e = q.enabled ? q : PictureParams{};
    const PictureParams neutral{};
    if (!q.basicEnabled) {
        e.exposure=neutral.exposure; e.contrast=neutral.contrast; e.saturation=neutral.saturation;
        e.temperature=neutral.temperature; e.sharpen=neutral.sharpen;
    }
    if (!q.tonesEnabled) { e.midtones=neutral.midtones; e.shadows=neutral.shadows; e.highlights=neutral.highlights; e.blacks=neutral.blacks; }
    if (!q.colorEnabled) {
        e.tint=neutral.tint; e.vibrance=neutral.vibrance; e.shadowTint=neutral.shadowTint; e.highlightTint=neutral.highlightTint;
        std::copy(std::begin(neutral.mixer),std::end(neutral.mixer),std::begin(e.mixer));
    }
    if (!q.detailEnabled) { e.clarity=neutral.clarity; e.vignette=neutral.vignette; }
    if (!q.filtersEnabled) for (const auto& f : kToggleFilters) e.*f.field=false;
    e.deband = SceneDither::On() ? q.deband : 0.0f;
    e.enabled = q.enabled || e.deband > 0.001f;
    if (!q.enabled) e.compare = false;
    return e;
}

void Picture::BeforeOverlay(IDirect3DDevice9* dev) {
    // the frame ended on the scene (no game UI after it): the copy is the scene as it is now
    if (Effective(GetParams()).enabled && dev && gpu.frameReady && gpu.sceneDraws >= kMinSceneDraws &&
        ((gpu.lastWasScene && (!gpu.sceneCopied || gpu.runDraws >= kMinSceneDraws)) || gpu.copyAfterStrip))
        CopyScene(dev);
}

// Diagnostics while Color is on (user 06/10: a friend's Edit in Game showed no Color until it was turned off and on, and
// nothing in the code explained it): once per frame, every gate between Color and the screen. A line when one of them changes
// (at most every 300 ms), and a full line every 10 s; at most 400 lines a session.
static void LogGates(IDirect3DDevice9* dev, int skip) {
    static std::string lastKey;
    static unsigned long long lastLine = 0, lastFull = 0;
    static int lines = 0;
    if (lines >= 400) return;
    const unsigned long long now = GetTickCount64();
    const bool inWorld = WorldSession::InWorld();
    const bool rtBack = gpu.curRT0 && gpu.curRT0 == gpu.backBuffer;
    const int draws = gpu.sceneDraws;
    const char* drawBand = draws == 0 ? "0" : draws < kMinSceneDraws ? "few" : "enough";
    std::string key = std::format("world {} | applied at the scene end {} | scene copy {} | resources {} | frame {} | RT0 {} | hooks {} | shaders {} | skip {} | draws {} | device {:#x}",
                                  inWorld ? "yes" : "no", g_boundaryDone ? "yes" : "no", gpu.sceneCopied ? "yes" : "no", gpu.ready ? "ready" : "no",
                                  gpu.frameReady ? "ready" : "no", rtBack ? "back buffer" : gpu.curRT0 ? "other" : "unknown", gpu.hooks ? "on" : "off",
                                  ShaderCache::PrecompileComplete() ? "ready" : "compiling", skip, drawBand, reinterpret_cast<uintptr_t>(dev));
    const bool changed = key != lastKey;
    if ((changed && now - lastLine >= 300) || now - lastFull >= 10000) {
        lastKey = key;
        lastLine = now;
        if (now - lastFull >= 10000) lastFull = now;
        lines++;
        LOG_INFO("[Picture gates] " + key + " (" + std::to_string(draws) + ") || " + WorldSession::GateText() + " || " + PostScene::DiagText());
    }
}

void Picture::OnEndScene(IDirect3DDevice9* dev) {
    ProcessFilterKeys();
    const PictureParams raw = GetParams(), q = Effective(raw);
    if (!g_atBoundary && raw.enabled) LogGates(dev, m_skip.load());
    if (!dev || !q.enabled) {
        m_gpuMs = -1.0f;
        RequestDepth(false);
        // Off: the scene-copy hooks go too (they counted every back buffer draw each frame while off; registered again at
        // the next frame it is on, which reads the current render target)
        if (gpu.hooks) {
            D3D9Hooks::UnregisterAll(kHookName);
            PostScene::Remove(BoundaryEffect);
            gpu.hooks = false;
            gpu.curRT0 = nullptr;
            gpu.frameReady = false;
        }
        return;
    }
    if (!ShaderCache::PrecompileComplete()) return; // keep loading frames moving while bytecode compiles
    RegisterHooks(dev);
    if (!g_atBoundary && g_boundaryDone) return; // already applied on the finished scene this frame
    // The end-of-frame fallback never filters without a scene copy (06/10, user: at some angles Color went over the game's
    // UI while Ambient Occlusion never did): with too few scene draws for the boundary, AO skips the frame, but this pass
    // filtered the whole picture, the UI included. Now Color skips that frame too.
    if (!g_atBoundary && !gpu.sceneCopied) return;
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
    if (!WorldSession::InWorld()) { // main menu, load screens: the game's own picture (idle on purpose, not a problem)
        m_skip.store(kSkipNone);
        m_lastApplied.store(now);
        bb->Release();
        return;
    }
    if (!raw.enabled && !gpu.sceneCopied) { // smoothing only (Picture off): never over the game's menus
        bb->Release();
        return;
    }
    if (g_atBoundary) g_boundaryDone = true;
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
    // Filters: effective switches (the filter on with a non-zero amount)
    auto on = [](bool f, float amount) { return f && std::fabs(amount) > 0.001f; };
    const bool fTech1 = on(q.tech1, q.tech1Amount), fTech2 = on(q.tech2, q.tech2Amount), fDpx = on(q.dpx, q.dpxAmount);
    const bool fColour = on(q.colourful, q.colourfulAmount), fNight = on(q.night, q.nightAmount), fVintage = on(q.vintage, q.vintageAmount);
    const bool fCross = on(q.crossProcess, q.crossAmount), fBw = on(q.bw, q.bwAmount);
    const bool fGlow = on(q.glow, q.glowAmount), fHal = on(q.halation, q.halationAmount), fDream = on(q.dreamy, q.dreamyAmount);
    const bool fTilt = on(q.tiltShift, q.tiltAmount), fPrism = on(q.prism, q.prismAmount);
    const bool fGrain = on(q.grain, q.grainAmount), fFx = on(q.retro3dfx, q.fxAmount), fCrt = on(q.crt, q.crtAmount);
    // Emphasize reads the scene depth: not on a scene drawn with another depth-stencil than the shared one (PostScene)
    const bool wantEmph = on(q.emphasize, q.emphAmount);
    const bool depthUsable = !g_atBoundary || PostScene::SceneDepthValid(); // the request stays (no swap restarted every frame)
    const bool fHdr = on(q.fakeHdr, q.hdrAmount);
    const bool fFilm = on(q.filmic, q.filmicAmount), fTintF = on(q.tintFilter, q.tintFilterAmount);
    const bool fLevels = q.levels && (q.levelsBlack > 0.001f || q.levelsWhite < 0.999f || std::fabs(q.levelsGamma - 1.0f) > 0.001f);
    const bool fCas = on(q.cas, q.casAmount), fDalt = on(q.daltonize, q.daltonAmount);
    const bool fAuto = on(q.autoExposure, q.autoAmount) && gpu.adaptTex[0] && gpu.adaptTex[1];
    // LUT: (re)loaded on the frame its file changes; on only with a usable strip
    if (q.lut && gpu.lutLoaded != q.lutFile) {
        gpu.lutLoaded = q.lutFile;
        LoadLut(dev, q.lutFile);
    }
    const bool fLut = on(q.lut, q.lutAmount) && gpu.lutTex && gpu.lutSize > 0;
    // Emphasize reads the scene depth: requested only while it is on
    RequestDepth(wantEmph);
    // the depth copied with the scene (the live one has the Sim portrait's cleared square by now); else the live one
    IDirect3DTexture9* depth = wantEmph && depthUsable ? (gpu.depthCopied && gpu.depthTex && !g_atBoundary ? gpu.depthTex : DepthShare::Texture()) : nullptr;
    const float camNear = PostScene::CameraNear(), camA = PostScene::CameraDepthA();
    const bool fEmph = wantEmph && depth && camNear > 0.0f;
    // clarity, glow, halation, dreamy, tilt-shift and Fake HDR: the scene (without the UI when the copy exists)
    // reduced to 1/2, 1/4 and 1/8 through 2x2 boxes
    const bool clarity = std::fabs(q.clarity) > 0.001f;
    if (clarity || fGlow || fHal || fDream || fTilt || fHdr || fAuto) {
        IDirect3DSurface9* src = (gpu.sceneCopied && !g_atBoundary) ? gpu.sceneSurf : gpu.frameSurf; // on the finished scene: the frame itself
        for (int i = 0; i < Gpu::kChain; i++) {
            dev->StretchRect(src, nullptr, gpu.chainSurf[i], nullptr, D3DTEXF_LINEAR);
            src = gpu.chainSurf[i];
        }
    }

    // save what the pass touches (the game continues from here next frame)
    constexpr DWORD kSamplers = 8;
    constexpr UINT kConsts = 61; // c0..c60 (c50, c51, c55 unused; c56 is AdaptPS's)
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
        const DWORD filter = (s == 2 || s == 4 || s == 5 || s == 6) ? D3DTEXF_LINEAR : D3DTEXF_POINT; // the reduced scene copies bilinear
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
    dev->SetTexture(3, fEmph ? depth : nullptr);
    dev->SetTexture(4, gpu.chainTex[0]);
    dev->SetTexture(5, gpu.chainTex[1]);
    dev->SetTexture(6, fLut ? gpu.lutTex : nullptr);
    // Auto exposure: the 1x1 pass first (into the other target, reading the previous one in s7), then the main pass reads it
    {
        LARGE_INTEGER now{}, freq{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        const double dt = gpu.lastPass.QuadPart ? std::clamp(static_cast<double>(now.QuadPart - gpu.lastPass.QuadPart) / static_cast<double>(freq.QuadPart), 0.0, 0.5) : 0.0;
        gpu.lastPass = now;
        if (fAuto && !gpu.adaptPs && !gpu.adaptTried) {
            gpu.adaptTried = true;
            std::string msg;
            if (ShaderCache::CreatePixelShader(dev, kAdaptPsId, &gpu.adaptPs, &msg) == ShaderCache::Result::CompileFailed) LOG_ERROR("[Picture] AdaptPS failed to compile: " + msg);
        }
        if (fAuto && gpu.adaptPs) {
            IDirect3DSurface9* rt0 = nullptr;
            dev->GetRenderTarget(0, &rt0);
            const int next = gpu.adaptCur ^ 1;
            const float speed = 0.3f + 4.7f * std::clamp(q.autoSpeed, 0.0f, 1.0f) * std::clamp(q.autoSpeed, 0.0f, 1.0f); // 1 / s
            const float blend[4] = {static_cast<float>(1.0 - std::exp(-dt * speed)), 0, 0, 0};
            D3D9Hooks::CallOriginalSetRenderTarget(dev, 0, gpu.adaptSurf[next]);
            const D3DVIEWPORT9 one{0, 0, 1, 1, 0.0f, 1.0f};
            dev->SetViewport(&one);
            dev->SetPixelShader(gpu.adaptPs);
            dev->SetPixelShaderConstantF(56, blend, 1);
            dev->SetTexture(7, gpu.adaptTex[gpu.adaptCur]);
            const QuadVertex a[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {0.5f, -0.5f, 0, 1, 1, 0}, {-0.5f, 0.5f, 0, 1, 0, 1}, {0.5f, 0.5f, 0, 1, 1, 1}};
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, a, sizeof(QuadVertex));
            D3D9Hooks::CallOriginalSetRenderTarget(dev, 0, rt0);
            SafeRelease(rt0);
            dev->SetViewport(&vp);
            gpu.adaptCur = next;
        }
    }
    dev->SetTexture(7, fAuto && gpu.adaptPs ? gpu.adaptTex[gpu.adaptCur] : nullptr);
    const float scale = static_cast<float>(gpu.height) / 2160.0f; // pixel sizes were chosen at 4K
    const float fxLevelsRB = std::exp2(std::round(5.0f - 2.0f * std::clamp(q.fxDepth, 0.0f, 1.0f))) - 1.0f; // 5 bits .. 3 bits
    const float fxLevelsG = std::exp2(std::round(6.0f - 2.0f * std::clamp(q.fxDepth, 0.0f, 1.0f))) - 1.0f;  // 6 bits .. 4 bits
    // Black and white: luminance weights through a colored lens filter (summing to 1, so grey stays grey), and the toning
    float bwW[3], bwT[3], halC[3], tintF[3], glowC[3];
    {
        float fc[3];
        HueColour(q.bwFilterHue, fc);
        const float lum[3] = {0.2126f, 0.7152f, 0.0722f};
        const float k = std::clamp(q.bwFilter, 0.0f, 1.0f);
        float sum = 0.0f;
        for (int i = 0; i < 3; i++) sum += (bwW[i] = std::max(0.0f, lum[i] * (1.0f + k * (2.0f * fc[i] - 1.0f))));
        for (float& w : bwW) w = sum > 1e-5f ? w / sum : 1.0f / 3.0f;
        SplitToneColour(q.bwToneHue, bwT);
        SplitToneColour(q.tintFilterHue, tintF);
        HueColour(q.halationHue, halC);
        // glow color: 1 plus the hue's chroma, luminance 1 (the halo keeps its brightness)
        float gh[3];
        HueColour(q.glowHue, gh);
        const float gl = 0.2126f * gh[0] + 0.7152f * gh[1] + 0.0722f * gh[2];
        for (int i = 0; i < 3; i++) glowC[i] = 1.0f + (gh[i] - gl);
    }
    // Cross-process: the cast turned by (hue - 180) degrees around the grey axis; animated grain: a new seed each pass
    const float crossAngle = (q.crossHue - 180.0f) * 3.14159265f / 180.0f;
    static unsigned s_grainFrame = 0;
    s_grainFrame = (s_grainFrame + 1) % 61;
    {
    }
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
        {std::clamp(q.saturation, 0.0f, 2.0f), (gpu.sceneCopied && !g_atBoundary) ? 1.0f : 0.0f, q.compare ? 1.0f : 0.0f, 0},
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
        {static_cast<float>(gpu.baseW), static_cast<float>(gpu.baseH), 1.0f / static_cast<float>(gpu.baseW), 1.0f / static_cast<float>(gpu.baseH)},
        {fTech1 ? 1.0f : 0.0f, fTech2 ? 1.0f : 0.0f, fDpx ? 1.0f : 0.0f, fColour ? 1.0f : 0.0f},
        {fNight ? 1.0f : 0.0f, fVintage ? 1.0f : 0.0f, fCross ? 1.0f : 0.0f, fBw ? 1.0f : 0.0f},
        {fGlow ? 1.0f : 0.0f, fHal ? 1.0f : 0.0f, fDream ? 1.0f : 0.0f, 0},
        {fEmph ? 1.0f : 0.0f, fTilt ? 1.0f : 0.0f, fPrism ? 1.0f : 0.0f, fGrain ? 1.0f : 0.0f},
        {fFx ? 1.0f : 0.0f, fCrt ? 1.0f : 0.0f, fLevels ? 1.0f : 0.0f, fFilm ? 1.0f : 0.0f},
        {std::clamp(q.tech1Amount, 0.0f, 1.0f), (std::clamp(q.tech1Cyan, -1.0f, 1.0f) + 1.0f) * 0.5f, std::clamp(q.tech1Saturation, 0.0f, 2.0f),
         std::exp2(std::clamp(q.tech1Brightness, -1.0f, 1.0f) * 0.5f)},
        {std::clamp(q.tech2Dye[0], 0.0f, 2.0f), std::clamp(q.tech2Dye[1], 0.0f, 2.0f), std::clamp(q.tech2Dye[2], 0.0f, 2.0f), std::clamp(q.tech2Amount, 0.0f, 1.0f)},
        {std::clamp(q.tech2Saturation, 0.0f, 2.0f), std::exp2(std::clamp(q.tech2Brightness, -1.0f, 1.0f) * 0.5f), std::clamp(q.dpxAmount, 0.0f, 1.0f),
         std::clamp(q.dpxContrast, 0.0f, 1.0f)},
        {std::clamp(q.dpxCurve[0], 0.5f, 1.5f), std::clamp(q.dpxCurve[1], 0.5f, 1.5f), std::clamp(q.dpxCurve[2], 0.5f, 1.5f), std::clamp(q.dpxSaturation, 0.0f, 2.0f)},
        {std::clamp(q.colourfulAmount, -1.0f, 1.0f), std::clamp(q.colourfulProtect, 0.0f, 1.0f), std::clamp(q.nightAmount, 0.0f, 1.0f), std::clamp(q.nightDarkness, 0.0f, 0.8f)},
        {std::clamp(q.nightBlue, 0.0f, 1.0f), std::clamp(q.nightKeepLamps, 0.0f, 1.0f), std::clamp(q.vintageAmount, 0.0f, 1.0f), std::clamp(q.vintageFade, 0.0f, 1.0f)},
        {std::clamp(q.vintageWarmth, -1.0f, 1.0f), std::clamp(q.vintageColors, 0.0f, 1.0f), std::clamp(q.crossAmount, 0.0f, 1.0f), std::clamp(q.crossContrast, 0.0f, 1.0f)},
        {std::clamp(q.bwAmount, 0.0f, 1.0f), std::clamp(q.bwFilter, 0.0f, 1.0f), std::clamp(q.bwTone, 0.0f, 1.0f), std::clamp(q.bwContrast, -1.0f, 1.0f)},
        {bwW[0], bwW[1], bwW[2], std::exp2(std::clamp(q.bwBrightness, -1.0f, 1.0f))},
        {bwT[0], bwT[1], bwT[2], 0},
        {std::clamp(q.glowAmount, 0.0f, 1.0f), std::clamp(q.glowThreshold, 0.0f, 0.95f), std::clamp(q.glowSize, 0.0f, 1.0f), std::clamp(q.glowWarmth, -1.0f, 1.0f)},
        {std::clamp(q.halationAmount, 0.0f, 1.0f), std::clamp(q.halationThreshold, 0.0f, 0.95f), 0.004f + 0.026f * std::clamp(q.halationSize, 0.0f, 1.0f), 0},
        {halC[0], halC[1], halC[2], 0},
        {std::clamp(q.dreamyAmount, 0.0f, 1.0f), std::clamp(q.dreamySoftness, 0.0f, 1.0f), std::clamp(q.dreamySaturation, 0.0f, 1.0f), 0},
        {std::clamp(q.levelsBlack, 0.0f, 0.9f), std::clamp(q.levelsWhite, 0.1f, 1.0f), 1.0f / std::clamp(q.levelsGamma, 0.5f, 2.0f), 0},
        {std::clamp(q.filmicAmount, 0.0f, 1.0f), std::clamp(q.filmicFade, 0.0f, 1.0f), std::clamp(q.filmicContrast, 0.0f, 2.0f), std::clamp(q.filmicBleach, 0.0f, 1.0f)},
        {std::clamp(q.emphAmount, 0.0f, 1.0f), std::clamp(q.emphGrey, 0.0f, 1.0f), q.emphAuto ? 1.0f : 0.0f, std::clamp(q.emphDistance, 1.0f, 500.0f)},
        {std::clamp(q.emphWidth, 0.0f, 2.0f), std::clamp(q.emphSoftness, 0.05f, 1.0f), camNear, camA},
        {std::clamp(q.tiltAmount, 0.0f, 1.0f), std::clamp(q.tiltCenter, 0.0f, 1.0f), std::clamp(q.tiltWidth, 0.0f, 1.0f), std::clamp(q.tiltSaturation, 0.0f, 1.0f)},
        {std::clamp(q.prismAmount, 0.0f, 1.0f) * 12.0f * scale, std::clamp(q.prismStart, 0.0f, 0.95f), q.prismQuality < 0.34f ? 3.0f : (q.prismQuality < 0.67f ? 5.0f : 9.0f), 0},
        {std::clamp(q.grainAmount, 0.0f, 1.0f), std::max(0.6f, (0.8f + 2.4f * std::clamp(q.grainSize, 0.0f, 1.0f)) * scale), std::clamp(q.grainShadows, 0.0f, 1.0f),
         q.grainAnimated ? static_cast<float>(s_grainFrame) : 0.0f},
        {std::clamp(q.fxAmount, 0.0f, 1.0f), fxLevelsRB, std::clamp(q.fxDither, 0.0f, 1.0f), std::clamp(q.fxScanlines, 0.0f, 1.0f)},
        {std::clamp(q.fxPixelWidth, 0.0f, 1.0f), 1.0f / std::clamp(q.fxGamma, 0.5f, 2.0f), std::max(2.0f, std::round(4.0f * scale)), fxLevelsG},
        {std::clamp(q.crtAmount, 0.0f, 1.0f), std::clamp(q.crtCurvature, 0.0f, 1.0f), std::clamp(q.crtMask, 0.0f, 1.0f), std::clamp(q.crtScanlines, 0.0f, 1.0f)},
        {std::clamp(q.crtEdges, 0.0f, 1.0f), std::max(2.0f, std::round(4.0f * scale)), 0, 0},
        {std::clamp(q.filmicSaturation, -1.0f, 1.0f), std::clamp(q.filmicCurve[0], 0.5f, 1.5f), std::clamp(q.filmicCurve[1], 0.5f, 1.5f), std::clamp(q.filmicCurve[2], 0.5f, 1.5f)},
        {fTintF ? 1.0f : 0.0f, fHdr ? 1.0f : 0.0f, 0, 0},
        {tintF[0], tintF[1], tintF[2], std::clamp(q.tintFilterAmount, 0.0f, 1.0f)},
        {fAuto && gpu.adaptPs ? 1.0f : 0.0f, fCas ? 1.0f : 0.0f, fDalt ? 1.0f : 0.0f, 0},
        {std::clamp(q.lutAmount, 0.0f, 1.0f), static_cast<float>(std::max(gpu.lutSize, 2)), fLut ? 1.0f : 0.0f, 0},
        {std::clamp(q.hdrAmount, 0.0f, 1.0f), std::clamp(q.hdrRadius, 0.0f, 1.0f), std::clamp(q.hdrShadows, 0.0f, 1.0f), std::clamp(q.hdrHighlights, 0.0f, 1.0f)},
        {std::clamp(q.hdrHalo, 0.0f, 1.0f), std::clamp(q.hdrSaturation, 0.0f, 1.0f), 0, 0},
        {0, 0, 0, 0}, // c50, c51: free
        {0, 0, 0, 0},
        {std::clamp(q.autoAmount, 0.0f, 1.0f), 0.05f + 0.25f * std::clamp(q.autoTarget, 0.0f, 1.0f), 1.0f / (1.0f + 1.5f * std::clamp(q.autoRange, 0.0f, 1.0f)),
         1.0f + 3.0f * std::clamp(q.autoRange, 0.0f, 1.0f)},
        {std::clamp(q.casAmount, 0.0f, 1.0f), 0, 0, 0},
        {std::round(std::clamp(q.daltonType, 0.0f, 2.0f)), std::clamp(q.daltonAmount, 0.0f, 1.0f), q.daltonSimulate ? 1.0f : 0.0f, 0},
        {0, 0, 0, 0},
        {0, 0, 0, 0},
        {std::clamp(q.tech1Contrast, 0.5f, 1.5f), std::clamp(q.vintageVignette, 0.0f, 1.0f), std::cos(crossAngle), std::sin(crossAngle)},
        {std::clamp(q.crossSaturation, 0.0f, 2.0f), std::clamp(q.tintPreserve, 0.0f, 1.0f), std::clamp(q.tintBalance, -1.0f, 1.0f), std::clamp(q.tiltBlur, 1.0f, 2.0f) - 1.0f},
        {glowC[0], glowC[1], glowC[2], std::clamp(q.glowColor, 0.0f, 1.0f)},
        {std::clamp(q.grainColor, 0.0f, 1.0f), 0, 0, 0}};
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
    const HRESULT drawResult=dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVertex));
    if (g_atBoundary && SUCCEEDED(drawResult)) m_lastSceneBoundary.store(now);

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
        m_filterKeysPresent.store(std::any_of(p.filterShortcuts.begin(),p.filterShortcuts.end(),[](const auto& b){return !b.Empty();}));
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
    if (!GetParams().enabled || !WorldSession::InWorld()) return false;
    const unsigned long long now = GetTickCount64();
    const unsigned long long since = std::max({m_enabledAt.load(), m_lastSceneCopy.load(), m_lastSceneBoundary.load()});
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
    pt.insert("basic_enabled",q.basicEnabled); pt.insert("tones_enabled",q.tonesEnabled);
    pt.insert("color_enabled",q.colorEnabled); pt.insert("detail_enabled",q.detailEnabled); pt.insert("filters_enabled",q.filtersEnabled);
    toml::table shortcuts;
    for (size_t i=0;i<std::size(kToggleFilters);++i) if (!q.filterShortcuts[i].Empty()) {
        toml::array keys;
        for (unsigned k=7;k<256;++k) if(q.filterShortcuts[i].Has(k)) keys.push_back(int(k));
        shortcuts.insert(kToggleFilters[i].key,std::move(keys));
    }
    pt.insert("filter_shortcuts",std::move(shortcuts));
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
    {
        // Color > Filters, in their own sub-table
        toml::table ft;
        for (const auto& k : kFilterBools) ft.insert(k.key, q.*k.field);
        for (const auto& k : kFilterFloats) ft.insert(k.key, static_cast<double>(q.*k.field));
        for (const auto& k : kFilterArrays) {
            toml::array a;
            for (float v : q.*k.field) a.push_back(static_cast<double>(v));
            ft.insert(k.key, std::move(a));
        }
        ft.insert("lut_file", q.lutFile);
        pt.insert("filters", std::move(ft));
    }
    qolTable.insert_or_assign("picture", std::move(pt));
}

void Picture::LoadFromToml(const toml::table& qolTable) {
    PictureParams q;
    if (!ParamsFromToml(qolTable, q)) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_p = q;
    m_filterKeysPresent.store(std::any_of(q.filterShortcuts.begin(),q.filterShortcuts.end(),[](const auto& b){return !b.Empty();}));
}

bool Picture::ParamsFromToml(const toml::table& qolTable, PictureParams& out) {
    const toml::table* pic = qolTable["picture"].as_table();
    if (!pic) return false;
    const toml::table& t = *pic;
    PictureParams q;
    auto f = [&](const char* key, float& v) { v = static_cast<float>(t[key].value_or(static_cast<double>(v))); };
    q.enabled = t["enabled"].value_or(false);
    q.basicEnabled=t["basic_enabled"].value_or(true); q.tonesEnabled=t["tones_enabled"].value_or(true);
    q.colorEnabled=t["color_enabled"].value_or(true); q.detailEnabled=t["detail_enabled"].value_or(true); q.filtersEnabled=t["filters_enabled"].value_or(true);
    if (const auto* keys=t["filter_shortcuts"].as_table()) for (size_t i=0;i<std::size(kToggleFilters);++i) {
        if (const auto* a=(*keys)[kToggleFilters[i].key].as_array()) {
            FilterShortcut b; bool valid=true;
            for (const auto& node:*a) { const auto k=node.value<int>(); if(!k || *k<7 || *k>255) {valid=false;break;} b.Add(unsigned(*k)); }
            if(valid && b.HasMainKey()) q.filterShortcuts[i]=b;
        }
    }
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
    if (const toml::table* ft = t["filters"].as_table()) {
        const toml::table& x = *ft;
        for (const auto& k : kFilterBools) q.*k.field = x[k.key].value_or(q.*k.field);
        if (auto s = x["lut_file"].value<std::string>()) q.lutFile = *s;
        for (const auto& k : kFilterFloats) q.*k.field = static_cast<float>(x[k.key].value_or(static_cast<double>(q.*k.field)));
        for (const auto& k : kFilterArrays)
            if (auto a = x[k.key].as_array())
                for (size_t i = 0; i < 3 && i < a->size(); i++) (q.*k.field)[i] = static_cast<float>((*a)[i].value_or(static_cast<double>((q.*k.field)[i])));
        // Emphasize's zone depth was in metres in the first test builds (now a fraction of the focus distance, 0 .. 2):
        // an old value past the slider's range goes back to the default
        if (!(q.emphWidth >= 0.0f && q.emphWidth <= 2.0f)) q.emphWidth = PictureParams{}.emphWidth;
    }
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

    const bool disabled=!q.enabled || !q.Group(tab);
    if (disabled) ImGui::BeginDisabled(); // visible but greyed out while Picture is off
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
        if (ApexUi::BeginAdvanced("FilmTones", "Film tones")) {
            hue("Shadow color", &q.shadowHue, kDef.shadowHue, "Teal or blue is the classic film look");
            percent("Shadow amount", &q.shadowTint, 0.0f, 1.0f, "How strongly shadows take that color; 0% is off", kDef.shadowTint);
            hue("Highlight color", &q.highlightHue, kDef.highlightHue, "Orange or gold is the classic film look");
            percent("Highlight amount", &q.highlightTint, 0.0f, 1.0f, "How strongly highlights take that color; 0% is off", kDef.highlightTint);
            ApexUi::EndAdvanced();
        }
        if (ApexUi::BeginAdvanced("ColorMixer", "Color mixer")) {
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
            ApexUi::EndAdvanced();
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

    if (disabled) ImGui::EndDisabled();
    if (changed) SetParams(q, save);
}

// Color > Filters: one card per filter, its switch in the header and its own controls under it while it is on (fine
// tuning under Advanced), in four sections. Cards stay visible, greyed out, while Picture is off.
void Picture::RenderFiltersUI() {
    using ApexUi::IconId;
    static const PictureParams kDef{};
    PictureParams q = GetParams();
    bool changed = false, save = false;
    auto slide = [&](const char* label, float* v, float lo, float hi, const ApexUi::SliderOptions& o) {
        if (ApexUi::Slider(label, v, lo, hi, o)) changed = true;
        save |= ApexUi::SliderCommitted();
    };
    auto percent = [&](const char* label, float* v, float lo, float hi, const char* desc, float def, float shown = 100.0f) {
        ApexUi::SliderOptions o;
        o.format = "%.0f%%";
        o.displayScale = shown;
        o.tooltip = desc;
        o.defaultValue = def;
        slide(label, v, lo, hi, o);
    };
    auto signedAmount = [&](const char* label, float* v, float def, const char* desc, const char* left, const char* right) {
        ApexUi::SliderOptions o;
        o.format = "%+.0f";
        o.displayScale = 100.0f;
        o.tooltip = desc;
        o.defaultValue = def;
        o.leftLabel = left;
        o.rightLabel = right;
        slide(label, v, -1.0f, 1.0f, o);
    };
    auto hue = [&](const char* label, float* v, float def, const char* desc) {
        ApexUi::SliderOptions o;
        o.valueText = "";
        o.swatch = HueSwatch(*v);
        o.tooltip = desc;
        o.hueTrack = true;
        o.defaultValue = def;
        slide(label, v, 0.0f, 360.0f, o);
    };
    auto toggle = [&](const char* label, bool* v, const char* desc, bool def) {
        if (ApexUi::SwitchRow(label, v, desc, ApexUi::BoolDefault(def))) {
            changed = true;
            save = true;
        }
    };
    // One panel per family; every filter keeps its complete controls behind an independent disclosure.
    bool familyOpen=false, familyVisible=false;
    int requestEditor=-1;
    auto family=[&](const char* label) {
        if(familyOpen) ApexUi::EndCard();
        ApexUi::SectionLabel(label);
        familyVisible=ApexUi::BeginCard(label); familyOpen=true;
    };
    auto card = [&](const char* id, IconId icon, const char* name, const char* what, bool* on, auto&& controls) {
        if(!familyVisible) return;
        ImGui::PushID(id);
        if(ApexUi::FilterActive()) {
            if(ApexUi::CardHeader(icon,name,what,nullptr,on,q.enabled && q.filtersEnabled)) changed=save=true;
            ImGui::BeginDisabled(!q.enabled || !q.filtersEnabled || !*on);
            controls(); ImGui::EndDisabled(); ImGui::PopID(); return;
        }
        int index=-1;
        for(size_t i=0;i<std::size(kToggleFilters);++i) if(&(q.*kToggleFilters[i].field)==on) index=int(i);
        const std::string key=index>=0 ? FilterKeyText(q.filterShortcuts[index]) : "";
        const float u=ApexUi::Unit(), gap=ApexUi::kSpace2*u;
        const ImVec2 switchSize=ApexUi::ToggleSwitchSize();
        const float rowWidth=ImGui::GetContentRegionAvail().x;
        std::string tag=key;
        const float tagBudget=std::max(18.0f*u,rowWidth-(24.0f+24.0f)*u-switchSize.x-gap*3);
        bool shortened=false;
        while(!tag.empty() && ApexUi::ChipSize((tag+(shortened?"…":"")).c_str()).x>tagBudget) {
            const auto cut=tag.find_last_of('+');
            tag=cut==std::string::npos ? std::string() : tag.substr(0,cut);
            shortened=true;
        }
        if(shortened) tag+="…";
        const float chipW=tag.empty() ? 0.0f : ApexUi::ChipSize(tag.c_str()).x+gap;
        // Right-aligned cluster: shortcut tag, disclosure, actions, switch; retained in narrow layouts.
        const float controlsW=(24.0f+24.0f)*u+switchSize.x+gap*2+chipW;
        const ImVec2 rowStart=ImGui::GetCursorScreenPos();
        const bool visible=ApexUi::BeginControlRow(name,what,controlsW,icon,30.0f*u);
        if(visible) {
            bool expanded=ImGui::GetStateStorage()->GetBool(ImGui::GetID("Expanded"),false);
            const float centerY=ImGui::GetCursorScreenPos().y+15.0f*u;
            auto center=[&](float h){ImGui::SetCursorPosY(centerY-ImGui::GetWindowPos().y+ImGui::GetScrollY()-h*0.5f);};
            if(!key.empty()) {
                center(ApexUi::ChipSize(tag.c_str()).y); ApexUi::Chip(tag.c_str());
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",key.c_str());
                ImGui::SameLine(0,gap);
            }
            center(24*u);
            if(ApexUi::IconButton("##Adjust",expanded?IconId::ChevronUp:IconId::ChevronDown,"Adjust this filter",expanded)) {
                expanded=!expanded; ImGui::GetStateStorage()->SetBool(ImGui::GetID("Expanded"),expanded);
            }
            ImGui::SameLine(0,gap); center(24*u);
            if(ApexUi::IconButton("##Shortcuts",IconId::Ellipsis,"Filter shortcuts")) ImGui::OpenPopup("Filter actions");
            ImGui::SameLine(0,gap);
            center(switchSize.y);
            ImGui::BeginDisabled(!q.enabled || !q.filtersEnabled);
            if(ApexUi::ToggleSwitch("##On",on)) {changed=save=true;ApexUi::ReportChange(name);}
            ImGui::EndDisabled();
            ApexUi::EndControlRow();
            const ImVec2 rowEnd(rowStart.x+rowWidth,ImGui::GetCursorScreenPos().y);
            if(ImGui::IsMouseHoveringRect(rowStart,rowEnd) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("Filter actions");
            if(ImGui::BeginPopup("Filter actions")) {
                if(ImGui::MenuItem(I18n::Tr(key.empty()?"Assign shortcut":"Change shortcut"))) {
                    requestEditor=index; g_filterRecordingName=I18n::Tr(name);
                }
                if(ImGui::MenuItem(I18n::Tr("Remove shortcut"),nullptr,false,!key.empty())) {q.filterShortcuts[index]={};changed=save=true;}
                ImGui::EndPopup();
            }
            if(expanded) {
                ApexUi::Gap(ApexUi::kSpace3);
                ImGui::BeginDisabled(!q.enabled || !q.filtersEnabled || !*on);
                controls();
                ImGui::EndDisabled();
            }
        }
        ImGui::PopID();
    };
    // "Reset this filter": the given settings of the card back to their defaults (its switch stays as it is)
    auto resetFilter = [&](auto... fields) {
        if (ApexUi::IconTextButton("Reset this filter", IconId::RotateCcw, "Puts this filter's settings back to their defaults")) {
            auto put = [](auto& dst, const auto& src) {
                if constexpr (std::is_array_v<std::remove_reference_t<decltype(dst)>>) std::copy(std::begin(src), std::end(src), std::begin(dst));
                else dst = src;
            };
            (put(q.*fields, kDef.*fields), ...);
            changed = save = true;
            ApexUi::ReportChange("Filter reset");
        }
    };
    using P = PictureParams;
    auto depthNote = [&] {
        if (q.enabled && !DepthShare::Texture())
            ApexUi::IconNote(IconId::Info, "Needs the scene depth: turn off the game's Edge Smoothing (Options \xE2\x80\xBA Graphics)");
    };



    family("FILM LOOKS");
    card("Technicolor1", IconId::Palette, "Technicolor 1", "Classic two-strip film: everything turns red or cyan", &q.tech1, [&] {
        percent("Amount", &q.tech1Amount, 0.0f, 1.0f, "How much of the look is mixed in", kDef.tech1Amount);
        signedAmount("Cyan side", &q.tech1Cyan, kDef.tech1Cyan, "What the cyan half of the picture leans to", "Greener", "Bluer");
        percent("Saturation", &q.tech1Saturation, 0.0f, 2.0f, "How strong the red and cyan get", kDef.tech1Saturation);
        signedAmount("Brightness", &q.tech1Brightness, kDef.tech1Brightness, "Film prints were often a little darker or brighter", "Darker", "Brighter");
        percent("Contrast", &q.tech1Contrast, 0.5f, 1.5f, "How punchy the look is; 100% keeps the picture's own contrast", kDef.tech1Contrast);
        resetFilter(&P::tech1Amount, &P::tech1Cyan, &P::tech1Saturation, &P::tech1Brightness, &P::tech1Contrast);
    });
    card("Technicolor2", IconId::Palette, "Technicolor 2", "Three-strip film: rich, dense primary colors", &q.tech2, [&] {
        percent("Amount", &q.tech2Amount, 0.0f, 1.0f, "How much of the look is mixed in", kDef.tech2Amount);
        percent("Saturation", &q.tech2Saturation, 0.0f, 2.0f, "How colorful the film is", kDef.tech2Saturation);
        signedAmount("Brightness", &q.tech2Brightness, kDef.tech2Brightness, "Film prints were often a little darker or brighter", "Darker", "Brighter");
        if (ApexUi::BeginAdvanced("Technicolor2Dyes", "Dyes")) {
            percent("Red dye", &q.tech2Dye[0], 0.0f, 2.0f, "How strongly reds separate from the other colors", kDef.tech2Dye[0]);
            percent("Green dye", &q.tech2Dye[1], 0.0f, 2.0f, "How strongly greens separate from the other colors", kDef.tech2Dye[1]);
            percent("Blue dye", &q.tech2Dye[2], 0.0f, 2.0f, "How strongly blues separate from the other colors", kDef.tech2Dye[2]);
            ApexUi::EndAdvanced();
        }
        resetFilter(&P::tech2Amount, &P::tech2Saturation, &P::tech2Brightness, &P::tech2Dye);
    });
    card("DPX", IconId::Palette, "DPX Cineon", "Cinema film curve: rich midtones, soft highlights", &q.dpx, [&] {
        percent("Amount", &q.dpxAmount, 0.0f, 1.0f, "How much of the look is mixed in", kDef.dpxAmount);
        percent("Contrast", &q.dpxContrast, 0.0f, 1.0f, "How steep the film curve is", kDef.dpxContrast);
        percent("Saturation", &q.dpxSaturation, 0.0f, 2.0f, "How colorful the film is", kDef.dpxSaturation);
        if (ApexUi::BeginAdvanced("DPXCurves", "Color curves")) {
            percent("Red curve", &q.dpxCurve[0], 0.5f, 1.5f, "Contrast of the red layer; higher warms the shadows' edges", kDef.dpxCurve[0]);
            percent("Green curve", &q.dpxCurve[1], 0.5f, 1.5f, "Contrast of the green layer", kDef.dpxCurve[1]);
            percent("Blue curve", &q.dpxCurve[2], 0.5f, 1.5f, "Contrast of the blue layer; higher cools the shadows", kDef.dpxCurve[2]);
            ApexUi::EndAdvanced();
        }
        resetFilter(&P::dpxAmount, &P::dpxContrast, &P::dpxSaturation, &P::dpxCurve);
    });
    card("Vintage", IconId::Image, "Vintage", "A faded old photo: soft blacks, warm cast, washed-out colors", &q.vintage, [&] {
        percent("Amount", &q.vintageAmount, 0.0f, 1.0f, "How much of the look is mixed in", kDef.vintageAmount);
        percent("Fade", &q.vintageFade, 0.0f, 1.0f, "How grey and lifted the blacks get", kDef.vintageFade);
        signedAmount("Warmth", &q.vintageWarmth, kDef.vintageWarmth, "A yellowed print, or a cool, aged one", "Cooler", "Warmer");
        percent("Faded colors", &q.vintageColors, 0.0f, 1.0f, "How much the colors wash out", kDef.vintageColors);
        percent("Vignette", &q.vintageVignette, 0.0f, 1.0f, "Darker corners, like an old camera lens", kDef.vintageVignette);
        resetFilter(&P::vintageAmount, &P::vintageFade, &P::vintageWarmth, &P::vintageColors, &P::vintageVignette);
    });
    card("CrossProcess", IconId::Blend, "Cross-process", "Film developed in the wrong chemistry: green shadows, yellow highlights", &q.crossProcess, [&] {
        percent("Amount", &q.crossAmount, 0.0f, 1.0f, "How much of the look is mixed in", kDef.crossAmount);
        percent("Contrast", &q.crossContrast, 0.0f, 1.0f, "How punchy the shifted colors get", kDef.crossContrast);
        hue("Color cast", &q.crossHue, kDef.crossHue, "The tint the shadows take; the highlights shift along with it");
        percent("Saturation", &q.crossSaturation, 0.0f, 2.0f, "How colorful the film is", kDef.crossSaturation);
        resetFilter(&P::crossAmount, &P::crossContrast, &P::crossHue, &P::crossSaturation);
    });
    card("FilmicPass", IconId::Palette, "Filmic pass", "A filmic grade: softer blacks, film contrast and toned-down colors", &q.filmic, [&] {
        percent("Amount", &q.filmicAmount, 0.0f, 1.0f, "How much of the look is mixed in", kDef.filmicAmount);
        percent("Fade", &q.filmicFade, 0.0f, 1.0f, "Lifted blacks and softer whites, like an old print", kDef.filmicFade);
        percent("Contrast", &q.filmicContrast, 0.0f, 2.0f, "How strong the film curve is; 0% keeps the picture's own contrast", kDef.filmicContrast);
        percent("Bleach", &q.filmicBleach, 0.0f, 1.0f, "Bleach bypass: harsher contrast with silvery, muted colors", kDef.filmicBleach);
        signedAmount("Saturation", &q.filmicSaturation, kDef.filmicSaturation, "Fewer or more colors in the look", "Muted", "Vivid");
        if (ApexUi::BeginAdvanced("FilmicPassCurves", "Color curves")) {
            const char* names[3] = {"Red curve", "Green curve", "Blue curve"};
            for (int i = 0; i < 3; i++)
                percent(names[i], &q.filmicCurve[i], 0.5f, 1.5f, "Above 100% brightens this color in the look, below darkens it", kDef.filmicCurve[i]);
            ApexUi::EndAdvanced();
        }
        resetFilter(&P::filmicAmount, &P::filmicFade, &P::filmicContrast, &P::filmicBleach, &P::filmicSaturation, &P::filmicCurve);
    });
    card("BlackAndWhite", IconId::Contrast, "Black and white", "Black and white photo, with a lens filter and an optional toning", &q.bw, [&] {
        percent("Amount", &q.bwAmount, 0.0f, 1.0f, "Partly colored, or fully black and white", kDef.bwAmount);
        hue("Filter color", &q.bwFilterHue, kDef.bwFilterHue, "Like a photographer's filter: red darkens skies, yellow softens skin, green lightens leaves");
        percent("Filter strength", &q.bwFilter, 0.0f, 1.0f, "How much the filter changes the greys; 0% is neutral", kDef.bwFilter);
        signedAmount("Contrast", &q.bwContrast, kDef.bwContrast, "Softer greys, or deeper blacks and brighter whites", "Softer", "Punchier");
        signedAmount("Brightness", &q.bwBrightness, kDef.bwBrightness, "Darker or brighter greys, like a shorter or longer exposure", "Darker", "Brighter");
        if (ApexUi::BeginAdvanced("BlackAndWhiteToning", "Toning")) {
            hue("Tone color", &q.bwToneHue, kDef.bwToneHue, "Brown for sepia, blue for cyanotype");
            percent("Tone amount", &q.bwTone, 0.0f, 1.0f, "How strongly the greys take that color; 0% is neutral", kDef.bwTone);
            ApexUi::EndAdvanced();
        }
        resetFilter(&P::bwAmount, &P::bwFilterHue, &P::bwFilter, &P::bwContrast, &P::bwBrightness, &P::bwToneHue, &P::bwTone);
    });
    card("Tint", IconId::Image, "Tint", "The whole picture in one color: sepia by default", &q.tintFilter, [&] {
        percent("Amount", &q.tintFilterAmount, 0.0f, 1.0f, "How much of the color is mixed in", kDef.tintFilterAmount);
        hue("Color", &q.tintFilterHue, kDef.tintFilterHue, "Brown for sepia, blue for a cold, moonlit look");
        percent("Preserve brightness", &q.tintPreserve, 0.0f, 1.0f, "100% keeps the picture as bright; lower darkens it like a colored filter", kDef.tintPreserve);
        signedAmount("Shadows / highlights", &q.tintBalance, kDef.tintBalance, "Where the color goes: the whole picture at 0, only shadows or highlights at the ends",
                     "Shadows", "Highlights");
        resetFilter(&P::tintFilterAmount, &P::tintFilterHue, &P::tintPreserve, &P::tintBalance);
    });
    family("COLOR AND MOOD");
    card("Colourfulness", IconId::Rainbow, "Colorfulness", "Livelier colors without blowing out the bright ones", &q.colourful, [&] {
        signedAmount("Amount", &q.colourfulAmount, kDef.colourfulAmount, "More vivid, or more muted", "Muted", "Vivid");
        if (ApexUi::BeginAdvanced("ColourfulnessAdvanced")) {
            percent("Protect bright colors", &q.colourfulProtect, 0.0f, 1.0f, "Keeps strong and bright colors from going over the top", kDef.colourfulProtect);
            ApexUi::EndAdvanced();
        }
        resetFilter(&P::colourfulAmount, &P::colourfulProtect);
    });
    card("NightMode", IconId::Moon, "Night Mode", "A cooler, darker evening tone; lamp light stays warm", &q.night, [&] {
        percent("Amount", &q.nightAmount, 0.0f, 1.0f, "How much of the look is mixed in", kDef.nightAmount);
        percent("Darkness", &q.nightDarkness, 0.0f, 0.8f, "How much darker the night gets", kDef.nightDarkness, 125.0f);
        percent("Blue tint", &q.nightBlue, 0.0f, 1.0f, "How blue the night looks", kDef.nightBlue);
        percent("Keep lamp light", &q.nightKeepLamps, 0.0f, 1.0f, "Lamp-lit and bright areas keep their own color", kDef.nightKeepLamps);
        resetFilter(&P::nightAmount, &P::nightDarkness, &P::nightBlue, &P::nightKeepLamps);
    });
    card("Levels", IconId::Contrast, "Levels", "Set the black and white points: deeper blacks and cleaner whites", &q.levels, [&] {
        auto level = [&](const char* label, float* v, float def, const char* desc) {
            ApexUi::SliderOptions o;
            o.format = "%.0f";
            o.displayScale = 255.0f;
            o.tooltip = desc;
            o.defaultValue = def;
            slide(label, v, 0.0f, 1.0f, o);
        };
        level("Black point", &q.levelsBlack, kDef.levelsBlack, "Everything at or below this level becomes black (0 = unchanged)");
        level("White point", &q.levelsWhite, kDef.levelsWhite, "Everything at or above this level becomes white (255 = unchanged)");
        if (q.levelsWhite < q.levelsBlack + 0.02f) q.levelsWhite = std::min(1.0f, q.levelsBlack + 0.02f);
        {
            ApexUi::SliderOptions o;
            o.format = "%.2f";
            o.tooltip = "Midtones: above 1 brightens them, below darkens them; 1 is unchanged";
            o.defaultValue = kDef.levelsGamma;
            slide("Gamma", &q.levelsGamma, 0.5f, 2.0f, o);
        }
        resetFilter(&P::levelsBlack, &P::levelsWhite, &P::levelsGamma);
    });
    card("Lut", IconId::Layers, "LUT", "A ready-made color look from a LUT file, like Lightroom or ReShade LUT packs", &q.lut, [&] {
        static std::vector<std::string> files;
        static double listedAt = -10.0;
        if (ImGui::GetTime() - listedAt > 2.0) { // the folder is read at most every 2 s
            files = ListLuts();
            listedAt = ImGui::GetTime();
        }
        if (files.empty()) {
            ApexUi::IconNote(IconId::Info, "Put LUT files in the LUTs folder: PNG strips such as 1024x32 or 4096x64");
        } else {
            std::vector<const char*> names;
            int cur = 0;
            for (size_t i = 0; i < files.size(); i++) {
                names.push_back(files[i].c_str());
                if (files[i] == q.lutFile) cur = static_cast<int>(i);
            }
            if (ApexUi::SelectRow("File", "The LUT that gives the look", "LutFile", &cur, names.data(), static_cast<int>(names.size()))) {
                q.lutFile = files[static_cast<size_t>(cur)];
                changed = save = true;
            } else if (std::find(files.begin(), files.end(), q.lutFile) == files.end()) { // none chosen yet, or the file is gone
                q.lutFile = files.front();
                changed = save = true;
            }
            percent("Amount", &q.lutAmount, 0.0f, 1.0f, "How much of the look is mixed in", kDef.lutAmount);
            resetFilter(&P::lutAmount);
            std::string status;
            {
                std::lock_guard<std::mutex> lock(g_lutMutex);
                status = g_lutStatus;
            }
            if (!status.empty()) ApexUi::IconNote(IconId::Info, status.c_str());
        }
        if (ApexUi::IconTextButton("Open the LUTs folder", IconId::ExternalLink, "Opens Apex Radiance\\LUTs in Explorer (created if needed)")) ShowLutFolder();
    });

    family("LIGHT AND DETAIL");
    card("AutoExposure", IconId::SunMedium, "Auto exposure", "The picture slowly adapts to dark and bright views, like your eyes", &q.autoExposure, [&] {
        percent("Amount", &q.autoAmount, 0.0f, 1.0f, "How much the brightness follows the view", kDef.autoAmount);
        percent("Target brightness", &q.autoTarget, 0.0f, 1.0f, "The brightness the picture adapts toward", kDef.autoTarget);
        percent("Speed", &q.autoSpeed, 0.0f, 1.0f, "How fast the eyes adapt", kDef.autoSpeed);
        percent("Range", &q.autoRange, 0.0f, 1.0f, "How far it may brighten dark views or darken bright ones", kDef.autoRange);
        // the 1x1 adaptation targets or their shader could not be made: the filter cannot run
        if (q.enabled && gpu.ready && (!gpu.adaptTex[0] || !gpu.adaptTex[1] || (gpu.adaptTried && !gpu.adaptPs)))
            ApexUi::IconNote(IconId::Info, "Auto exposure could not start on this graphics card");
        resetFilter(&P::autoAmount, &P::autoTarget, &P::autoSpeed, &P::autoRange);
    });
    card("AdaptiveSharpening", IconId::Gem, "Adaptive sharpening", "Crisper soft detail without halos on hard edges", &q.cas, [&] {
        percent("Sharpness", &q.casAmount, 0.0f, 1.0f, "How strong the sharpening is", kDef.casAmount);
        resetFilter(&P::casAmount);
    });
    card("Glow", IconId::Lightbulb, "Glow", "A soft halo around lamps, windows and other bright areas", &q.glow, [&] {
        percent("Amount", &q.glowAmount, 0.0f, 1.0f, "How bright the halo is", kDef.glowAmount);
        percent("Threshold", &q.glowThreshold, 0.0f, 0.95f, "How bright something must be to glow; lower makes more of the picture glow", kDef.glowThreshold);
        percent("Size", &q.glowSize, 0.0f, 1.0f, "How far the halo spreads", kDef.glowSize);
        if (ApexUi::BeginAdvanced("GlowAdvanced")) {
            signedAmount("Warmth", &q.glowWarmth, kDef.glowWarmth, "A cooler or warmer halo", "Cooler", "Warmer");
            hue("Color", &q.glowHue, kDef.glowHue, "The color the halo takes");
            percent("Color strength", &q.glowColor, 0.0f, 1.0f, "0% keeps the halo the color of the light", kDef.glowColor);
            ApexUi::EndAdvanced();
        }
        resetFilter(&P::glowAmount, &P::glowThreshold, &P::glowSize, &P::glowWarmth, &P::glowHue, &P::glowColor);
    });
    card("Halation", IconId::Flame, "Halation", "The reddish halo film leaves around strong light", &q.halation, [&] {
        percent("Amount", &q.halationAmount, 0.0f, 1.0f, "How strong the halo is", kDef.halationAmount);
        percent("Threshold", &q.halationThreshold, 0.0f, 0.95f, "Only light brighter than this gets the halo", kDef.halationThreshold);
        percent("Size", &q.halationSize, 0.0f, 1.0f, "How far the halo reaches around the light", kDef.halationSize);
        if (ApexUi::BeginAdvanced("HalationAdvanced")) {
            hue("Halo color", &q.halationHue, kDef.halationHue, "Red-orange is how real film looks");
            ApexUi::EndAdvanced();
        }
        resetFilter(&P::halationAmount, &P::halationThreshold, &P::halationSize, &P::halationHue);
    });
    card("Dreamy", IconId::Sparkles, "Dreamy", "A soft, glowing, slightly more colorful picture", &q.dreamy, [&] {
        percent("Amount", &q.dreamyAmount, 0.0f, 1.0f, "How much of the look is mixed in", kDef.dreamyAmount);
        percent("Softness", &q.dreamySoftness, 0.0f, 1.0f, "How soft and wide the glow is", kDef.dreamySoftness);
        percent("Saturation", &q.dreamySaturation, 0.0f, 1.0f, "Extra color in the glow", kDef.dreamySaturation);
        resetFilter(&P::dreamyAmount, &P::dreamySoftness, &P::dreamySaturation);
    });
    card("FakeHdr", IconId::Mountain, "Fake HDR", "More detail in dark and bright areas, like an HDR photo", &q.fakeHdr, [&] {
        percent("Amount", &q.hdrAmount, 0.0f, 1.0f, "How strong the effect is", kDef.hdrAmount);
        percent("Shadows", &q.hdrShadows, 0.0f, 1.0f, "How much the dark areas are lifted", kDef.hdrShadows);
        percent("Highlights", &q.hdrHighlights, 0.0f, 1.0f, "How much of the sky and bright light is brought back", kDef.hdrHighlights);
        percent("Radius", &q.hdrRadius, 0.0f, 1.0f, "Fine detail, or large areas of light and shade", kDef.hdrRadius);
        if (ApexUi::BeginAdvanced("FakeHdrAdvanced")) {
            percent("Halo protection", &q.hdrHalo, 0.0f, 1.0f, "Keeps dark or bright outlines from forming around roofs and Sims", kDef.hdrHalo);
            percent("Saturation", &q.hdrSaturation, 0.0f, 1.0f, "A little extra color, since HDR tends to look washed out", kDef.hdrSaturation);
            ApexUi::EndAdvanced();
        }
        resetFilter(&P::hdrAmount, &P::hdrRadius, &P::hdrShadows, &P::hdrHighlights, &P::hdrHalo, &P::hdrSaturation);
    });

    family("CAMERA");
    card("Emphasize", IconId::Crosshair, "Emphasize", "Full color on what you look at, the rest fades to grey", &q.emphasize, [&] {
        percent("Amount", &q.emphAmount, 0.0f, 1.0f, "How strong the effect is", kDef.emphAmount);
        percent("Focus depth", &q.emphWidth, 0.0f, 2.0f, "How deep the colorful zone is, compared with its distance", kDef.emphWidth);
        percent("Grey amount", &q.emphGrey, 0.0f, 1.0f, "How grey the areas outside the zone get", kDef.emphGrey);
        if (ApexUi::BeginAdvanced("EmphasizeAdvanced")) {
            toggle("Automatic focus", &q.emphAuto, "The colorful zone follows what is at the center of the screen", kDef.emphAuto);
            if (!q.emphAuto) {
                ApexUi::SliderOptions o;
                o.format = "%.0f m";
                o.tooltip = "How far from the camera the colorful zone is";
                o.defaultValue = kDef.emphDistance;
                slide("Focus distance", &q.emphDistance, 1.0f, 300.0f, o);
            }
            percent("Edge softness", &q.emphSoftness, 0.05f, 1.0f, "How gently color fades out past the zone", kDef.emphSoftness);
            ApexUi::EndAdvanced();
        }
        depthNote();
        resetFilter(&P::emphAmount, &P::emphWidth, &P::emphGrey, &P::emphAuto, &P::emphDistance, &P::emphSoftness);
    });
    card("TiltShift", IconId::Aperture, "Tilt-shift", "Miniature effect: a sharp band, blurred top and bottom", &q.tiltShift, [&] {
        percent("Amount", &q.tiltAmount, 0.0f, 1.0f, "How strong the blur is", kDef.tiltAmount);
        percent("Position", &q.tiltCenter, 0.0f, 1.0f, "Where the sharp band sits: 0% is the top, 100% the bottom", kDef.tiltCenter);
        percent("Sharp band", &q.tiltWidth, 0.0f, 1.0f, "How tall the sharp band is", kDef.tiltWidth);
        percent("Blur strength", &q.tiltBlur, 1.0f, 2.0f, "Above 100% blurs the top and bottom even more", kDef.tiltBlur);
        if (ApexUi::BeginAdvanced("TiltShiftAdvanced")) {
            percent("Toy colors", &q.tiltSaturation, 0.0f, 1.0f, "Extra saturation that makes the scene look like a model", kDef.tiltSaturation);
            ApexUi::EndAdvanced();
        }
        resetFilter(&P::tiltAmount, &P::tiltCenter, &P::tiltWidth, &P::tiltBlur, &P::tiltSaturation);
    });
    card("Prism", IconId::Diamond, "Prism", "Colored fringes toward the edges, like a real lens", &q.prism, [&] {
        percent("Amount", &q.prismAmount, 0.0f, 1.0f, "How far the colors split at the corners", kDef.prismAmount);
        percent("Edge start", &q.prismStart, 0.0f, 0.95f, "Where the fringes begin; lower reaches closer to the center", kDef.prismStart);
        if (ApexUi::BeginAdvanced("PrismAdvanced")) {
            ApexUi::SliderOptions o;
            o.valueText = q.prismQuality < 0.34f ? I18n::Tr("Low") : (q.prismQuality < 0.67f ? I18n::Tr("Medium") : I18n::Tr("High"));
            o.tooltip = "Smoother color fringes cost a little more";
            o.defaultValue = kDef.prismQuality;
            slide("Quality", &q.prismQuality, 0.0f, 1.0f, o);
            ApexUi::EndAdvanced();
        }
        resetFilter(&P::prismAmount, &P::prismStart, &P::prismQuality);
    });
    card("Grain", IconId::Scan, "Film grain", "Fine film grain over the picture", &q.grain, [&] {
        percent("Amount", &q.grainAmount, 0.0f, 1.0f, "How visible the grain is", kDef.grainAmount);
        percent("Grain size", &q.grainSize, 0.0f, 1.0f, "Fine or coarse grain", kDef.grainSize);
        toggle("Animated", &q.grainAnimated, "The grain changes every frame, like real film", kDef.grainAnimated);
        if (ApexUi::BeginAdvanced("GrainAdvanced")) {
            percent("More in the shadows", &q.grainShadows, 0.0f, 1.0f, "Real film shows more grain in dark areas", kDef.grainShadows);
            percent("Color grain", &q.grainColor, 0.0f, 1.0f, "0% is grey grain; higher adds colored specks", kDef.grainColor);
            ApexUi::EndAdvanced();
        }
        resetFilter(&P::grainAmount, &P::grainSize, &P::grainAnimated, &P::grainShadows, &P::grainColor);
    });

    family("RETRO AND STYLE");
    card("3DFX", IconId::Gamepad2, "3DFX", "Late-90s 3D card: 16-bit color, dithering and fine lines", &q.retro3dfx, [&] {
        percent("Amount", &q.fxAmount, 0.0f, 1.0f, "How much of the look is mixed in", kDef.fxAmount);
        {
            const int bits = 5 - static_cast<int>(std::round(2.0f * std::clamp(q.fxDepth, 0.0f, 1.0f)));
            char depthText[24];
            std::snprintf(depthText, sizeof depthText, "%d-bit", bits * 3 + 1);
            ApexUi::SliderOptions o;
            o.valueText = depthText;
            o.tooltip = "Fewer colors give stronger banding and dithering";
            o.defaultValue = kDef.fxDepth;
            slide("Color depth", &q.fxDepth, 0.0f, 1.0f, o);
        }
        percent("Scanlines", &q.fxScanlines, 0.0f, 1.0f, "Dark lines between rows, like an old monitor", kDef.fxScanlines);
        if (ApexUi::BeginAdvanced("3DFXAdvanced")) {
            percent("Dithering", &q.fxDither, 0.0f, 1.0f, "The fine dot pattern that hides the missing colors", kDef.fxDither);
            percent("Soft pixels", &q.fxPixelWidth, 0.0f, 1.0f, "The slight horizontal blur of the old cards' output", kDef.fxPixelWidth);
            percent("Gamma", &q.fxGamma, 0.5f, 2.0f, "Brighter or darker midtones, as on old monitors", kDef.fxGamma);
            ApexUi::EndAdvanced();
        }
        resetFilter(&P::fxAmount, &P::fxDepth, &P::fxScanlines, &P::fxDither, &P::fxPixelWidth, &P::fxGamma);
    });
    card("CRT", IconId::Monitor, "CRT", "An old TV: curved glass, phosphor stripes and scanlines", &q.crt, [&] {
        percent("Amount", &q.crtAmount, 0.0f, 1.0f, "How much of the look is mixed in", kDef.crtAmount);
        percent("Curvature", &q.crtCurvature, 0.0f, 1.0f, "How curved the glass is", kDef.crtCurvature);
        percent("Phosphor mask", &q.crtMask, 0.0f, 1.0f, "The red, green and blue stripes of the screen", kDef.crtMask);
        percent("Scanlines", &q.crtScanlines, 0.0f, 1.0f, "Dark lines between rows", kDef.crtScanlines);
        if (ApexUi::BeginAdvanced("CRTAdvanced")) {
            percent("Dark edges", &q.crtEdges, 0.0f, 1.0f, "Darker corners of the glass", kDef.crtEdges);
            ApexUi::EndAdvanced();
        }
        resetFilter(&P::crtAmount, &P::crtCurvature, &P::crtMask, &P::crtScanlines, &P::crtEdges);
    });
    family("ACCESSIBILITY");
    card("ColorBlind", IconId::Eye, "Color-blind mode", "Moves the colors you cannot tell apart into ones you can", &q.daltonize, [&] {
        static const char* const kTypes[] = {"Red (protanopia)", "Green (deuteranopia)", "Blue (tritanopia)"};
        int type = static_cast<int>(std::lround(std::clamp(q.daltonType, 0.0f, 2.0f)));
        if (ApexUi::SelectRow("Type", "Which colors are hard to tell apart", "DaltonType", &type, kTypes, 3, 220.0f, static_cast<int>(kDef.daltonType))) {
            q.daltonType = static_cast<float>(type);
            changed = save = true;
        }
        percent("Amount", &q.daltonAmount, 0.0f, 1.0f, "How strongly the colors are moved", kDef.daltonAmount);
        toggle("Simulate", &q.daltonSimulate, "Shows the picture as seen with this color blindness, to check it", kDef.daltonSimulate);
        resetFilter(&P::daltonType, &P::daltonAmount, &P::daltonSimulate);
    });
    if(familyOpen) ApexUi::EndCard();
    if(requestEditor>=0) {
        {std::lock_guard<std::mutex> lock(g_filterCaptureMutex);g_filterCaptured={};}
        g_filterCaptureCancelled=false; g_filterRecording=requestEditor;
        ImGui::OpenPopup(I18n::Tr("Filter shortcut"));
    }
    bool editorOpen=true;
    const float modalWidth=std::max(1.0f,std::min(480*ApexUi::Unit(),ImGui::GetIO().DisplaySize.x-32.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(modalWidth,0),ImVec2(modalWidth,FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(modalWidth,0),ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,ImVec2(.5f,.5f));
    if(ImGui::BeginPopupModal(I18n::Tr("Filter shortcut"),&editorOpen,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_AlwaysAutoResize)) {
        {
            ApexUi::ControlSizeScope size(ApexUi::ControlSize::Primary);
            bool close=false;
            ApexUi::HeaderExtra extra; extra.iconOff=extra.iconOn=IconId::X; extra.value=&close; extra.tooltip="Cancel";
            ApexUi::CardHeader(IconId::Keyboard,"Filter shortcut",g_filterRecordingName.c_str(),nullptr,nullptr,true,&extra);
            ApexUi::CardDivider();
            ApexUi::MutedText(I18n::Tr("Press the keys together, then choose Save"));
            ApexUi::Gap(ApexUi::kSpace2);
            FilterShortcut binding;
            {std::lock_guard<std::mutex> lock(g_filterCaptureMutex);binding=g_filterCaptured;}
            const auto text=FilterKeyText(binding);
            std::string shown=text.empty()?I18n::Tr("Waiting for keys"):text;
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##RecordedKeys",shown.data(),shown.size()+1,ImGuiInputTextFlags_ReadOnly);
            if(ImGui::IsItemHovered()&&!text.empty()) ImGui::SetTooltip("%s",text.c_str());
            const int recording=g_filterRecording.load();
            bool conflict=ReservedFilterKeys(binding);
            for(size_t i=0;i<q.filterShortcuts.size();++i) if(int(i)!=recording && !binding.Empty() && binding==q.filterShortcuts[i]) conflict=true;
            if(conflict) ApexUi::IconNote(IconId::TriangleAlert,"This shortcut is already in use");
            ApexUi::Gap(ApexUi::kSpace3);
            ApexUi::CardDivider();
            const float available=ImGui::GetContentRegionAvail().x;
            const float cancelWidth=ApexUi::ButtonWidth("Cancel",false),saveWidth=ApexUi::ButtonWidth("Save",false);
            const float total=cancelWidth+ImGui::GetStyle().ItemSpacing.x+saveWidth;
            const bool oneRow=total<=available;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX()+std::max(0.0f,available-(oneRow?total:cancelWidth)));
            if(ApexUi::TextButton("Cancel") || close || !editorOpen || g_filterCaptureCancelled.exchange(false)) {
                g_filterRecording=-1; ImGui::CloseCurrentPopup();
            }
            if(oneRow) ImGui::SameLine();
            else ImGui::SetCursorPosX(ImGui::GetCursorPosX()+std::max(0.0f,ImGui::GetContentRegionAvail().x-saveWidth));
            ImGui::BeginDisabled(recording<0 || g_filterRecording.load()!=recording || !binding.HasMainKey() || conflict);
            if(ApexUi::TextButton("Save",nullptr,ApexUi::ButtonKind::Primary)) {
                q.filterShortcuts[recording]=binding; changed=save=true;
                g_filterRecording=-1;ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    } else if(g_filterRecording>=0) g_filterRecording=-1;
    if (changed) SetParams(q, save);
}

void Picture::RenderDeveloperUI() {
    ImGui::TextDisabled("Runs on the 8-bit image with a fixed dither: the grading adds no banding.");
    if (m_gpuMs >= 0) ImGui::TextDisabled("GPU cost: %.2f ms per frame", m_gpuMs);
    else ImGui::TextDisabled("GPU cost: not measured yet (Picture off, or no frame drawn)");
}

bool Picture::RecordingFilterShortcut() { return g_filterRecording.load()>=0; }
bool Picture::FilterKeyDown(WPARAM vk,bool repeat) {
    if(g_filterRecording.load()>=0) {
        if(vk==VK_ESCAPE) g_filterCaptureCancelled=true;
        else if(!repeat) {auto b=HeldFilterKeys();b.Add(unsigned(vk));if(b.HasMainKey()) {std::lock_guard<std::mutex> lock(g_filterCaptureMutex);g_filterCaptured=b;}}
        return true;
    }
    if(!m_filterKeysPresent.load()) return false;
    const auto q=GetParams();
    int chosen=-1; unsigned count=0;
    for(size_t i=0;i<q.filterShortcuts.size();++i) {
        const auto& b=q.filterShortcuts[i];
        if(b.Matches(unsigned(vk),[](unsigned key){return GetKeyState(key)<0;}) && b.Count()>count && !ReservedFilterKeys(b)) {chosen=int(i);count=b.Count();}
    }
    if(chosen<0) return false;
    if(!repeat) g_filterRequests.fetch_xor(1u<<chosen); return true;
}
void Picture::ProcessFilterKeys() {
    const unsigned requests=g_filterRequests.exchange(0); if(!requests) return;
    auto q=GetParams();
    for(size_t i=0;i<std::size(kToggleFilters);++i) if(requests&(1u<<i)) q.*kToggleFilters[i].field=!(q.*kToggleFilters[i].field);
    SetParams(q,true);
}
