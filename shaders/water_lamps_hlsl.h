// Generated from water_lamps_ps.hlsl
#pragma once
static const char* kWaterLampsHlsl = R"RAW(
// Pass drawn right after the game's lake water (blend ONE / INVSRCALPHA, premultiplied):
//  - reflection: screen-space ray march against the scene depth (Depth Blur's INTZ in s7) along the reflected ray, colour
//    from the scene copy the game already binds for refraction (s6). No depth: no screen reflection.
//  - reflections and glow of nearby lamps.
// Uses the game's own wave normal maps (s0, s1) and constants (c1 = camera, c5 = wave normal scales).
// SEA: the same pass for the sea water that never reads the game's planar reflection (Twinbrook, kSeaNoReflPs): its
// camera is in c0, its wave scales in c4, its world position in TEXCOORD2 and its fog amount in COLOR1.w.
// WEATHER: the lake water of rain and snow (kLakeRainPs, kLakeSnowPs): camera c3, wave scales c9, wave maps s2 / s3, the
// scene copy s8; inputs as the lake's.
#ifdef SEA
float4 c1 : register(c0);
float4 c5 : register(c4);
#elif defined(WEATHER)
float4 c1 : register(c3);
float4 c5 : register(c9);
#else
float4 c1 : register(c1);
float4 c5 : register(c5);
#endif
float4 lampPos[16] : register(c20); // xyz = lamp head, w = visual radius
float4 lampCol[16] : register(c36); // rgb = colour x intensity x night fade
float4 params : register(c52);      // x = lamp strength, y = count, z = filter specular, w = preserve bright lamp colors
float4 wvp[4] : register(c53);      // local -> clip of the water mesh
float4 worldT : register(c57);      // xyz = world translation of the water mesh
float4 reflParams : register(c58);  // x = reflection strength
float4 depthParams : register(c59); // x = A, y = B (device z = A + B / w), z = 1 when s7 holds the scene depth
#ifdef WEATHER
sampler2D sWave0 : register(s2);
sampler2D sWave1 : register(s3);
sampler2D sScene : register(s8);
#else
sampler2D sWave0 : register(s0);
sampler2D sWave1 : register(s1);
sampler2D sScene : register(s6);
#endif
sampler2D sDepth : register(s7);
struct PSIn {
    float4 uv : TEXCOORD0;   // xy = wave map 0, zw = wave map 1
#ifdef SEA
    float4 pos : TEXCOORD2;  // xyz = world position
    float4 fog : COLOR1;     // w = fog amount
#else
    float4 pos : TEXCOORD1;  // xyz = world position
    float4 fog : TEXCOORD3;  // w = fog amount
#endif
};

float4 Project(float3 world) {
    float4 p = float4(world - worldT.xyz, 1);
    return float4(dot(wvp[0], p), dot(wvp[1], p), dot(wvp[2], p), dot(wvp[3], p));
}
float2 ClipToUv(float4 c) { return float2(c.x / c.w * 0.5 + 0.5, -c.y / c.w * 0.5 + 0.5); }
float SceneW(float2 uv) { return depthParams.y / (tex2Dlod(sDepth, float4(uv, 0, 0)).r - depthParams.x); }
float EdgeFade(float2 uv) { float2 e = saturate(min(uv, 1 - uv) * 8); e = e * e * (3 - 2 * e); return e.x * e.y; }

float4 main(PSIn i) : COLOR0 {
    const bool haveDepth = depthParams.z > 0.5;
    float4 cp = Project(i.pos.xyz);
    float4 a = tex2D(sWave0, i.uv.xy);
    float4 b = tex2D(sWave1, i.uv.zw);
    float3 n = normalize(float3(a.x * c5.x + b.x * c5.y, a.z + b.z, a.y * c5.x + b.y * c5.y));
    // Derivatives must be evaluated before depth rejection / divergent reflection marching.
    // Filter only the lamp lobe; the scenery reflection keeps its original normals and Fresnel.
    float exponent = 250.0;
    float peakScale = 1.0;
    if (params.z > 0.5) {
        float3 dx = ddx(n), dy = ddy(n);
        float variance = min(0.25 * (dot(dx, dx) + dot(dy, dy)), 0.016);
        exponent = 2.0 / (2.0 / 252.0 + variance) - 2.0;
        peakScale = (exponent + 2.0) / 252.0;
    }
    // Depth test in the shader (the depth-stencil is unbound during this pass so its texture can be read).
    if (haveDepth && cp.w > SceneW(ClipToUv(cp)) + 0.3 + cp.w * 0.01) return 0;
    float3 v = normalize(c1.xyz - i.pos.xyz);
    float fres = 0.25 + 0.75 * pow(1 - saturate(dot(n, v)), 5);
    float fogKeep = 1 - saturate(i.fog.w);

    float3 nr = normalize(float3(n.x * 0.5, 1, n.z * 0.5));
    float3 r = reflect(-v, nr);
    float3 refl = 0;
    float cover = 0;
    float tHit = 1e9, block = 0; // where the reflected ray met the scene, and how surely (lamps behind it are hidden there)
    if (haveDepth) {
        // Only real hits are used; a ray that leaves the screen or finds nothing keeps the game's own sky reflection.
        // Every hit fades out smoothly (screen edges, ray length, how well the depth matched), so there is no hard line
        // between pixels that found a surface and pixels that did not.
        float t = 0.4, tPrev = 0;
        [loop] for (int s = 0; s < 48; s++) {
            float4 c = Project(i.pos.xyz + r * t);
            if (c.w <= 0.05) break;
            float2 uv = ClipToUv(c);
            if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1) break;
            float diff = c.w - SceneW(uv);
            float thick = 1.5 + t * 0.3;
            if (diff > 0 && diff < thick) { // behind a surface, close enough to be that surface
                float lo = tPrev, hi = t;
                [unroll] for (int k = 0; k < 5; k++) {
                    float m = (lo + hi) * 0.5;
                    float4 cm = Project(i.pos.xyz + r * m);
                    if (cm.w - SceneW(ClipToUv(cm)) > 0) hi = m; else lo = m;
                }
                float4 ch = Project(i.pos.xyz + r * hi);
                float2 uh = ClipToUv(ch);
                float match = 1 - saturate((ch.w - SceneW(uh)) / thick);
                refl = tex2Dlod(sScene, float4(uh, 0, 0)).rgb;
                cover = EdgeFade(uh) * match * saturate(1.5 - hi / 60);
                tHit = hi;
                block = EdgeFade(uh) * match;
                break;
            }
            // diff >= thick: a thin object in front of the ray (post, leaf); keep marching behind it
            tPrev = t;
            t = t * 1.15 + 0.3;
        }
    }
    // No scene depth (Depth Blur off, or the game's antialiasing on): no screen reflection at all. A fixed-distance guess
    // samples the wrong part of the screen and paints dark patches; the game's own sky reflection stays.
    float alpha = saturate(fres * reflParams.x) * cover * fogKeep;

    float3 spec = 0, glow = 0;
    [unroll] for (int k = 0; k < 16; k++) {
        float3 l = lampPos[k].xyz - i.pos.xyz;
        float d2 = dot(l, l);
        float rr = lampPos[k].w * lampPos[k].w + 1e-3;
        float3 h = normalize(l * rsqrt(d2 + 1e-4) + v);
        float sp = pow(saturate(dot(n, h)), exponent) * (2 * peakScale) / (1 + d2 / (rr * 16));
        // a lamp behind what the reflected ray met (a tent, a wall between the water and the lamp) shows no reflection there;
        // the glow on the water around the lamp stays
        sp *= 1 - block * saturate(sqrt(d2) - tHit - 0.5);
        float g = saturate(1 - d2 / rr);
        spec += lampCol[k].rgb * sp;
        glow += lampCol[k].rgb * (g * g);
    }
    float3 lamps = (spec * fres + glow * 0.08) * params.x * fogKeep;
    if (params.w > 0.5) {
        // Identity below the knee, bounded smoothly above it. One common RGB scale preserves hue ratios.
        float peak = max(lamps.r, max(lamps.g, lamps.b));
        float excess = max(peak - 0.7, 0.0);
        float compressedPeak = 0.7 + 0.1 * excess / (0.1 + excess);
        lamps *= peak > 0.7 ? compressedPeak / max(peak, 1e-5) : 1.0;
    } else {
        lamps = min(lamps, 0.8); // original A/B reference
    }
    return float4(refl * alpha + lamps, alpha);
}
)RAW";
