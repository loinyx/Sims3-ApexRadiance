// Real-time GI (docs/features/realtime-gi, work package X-05): the radiance cascades of features/plan_cascades.h as ps_3_0
// passes, function by function with the same float math. The field is the story's of shaders/plan_field_hlsl.h (read with
// the sampler's bilinear filter, one band by g_band); the emitter image holds wall, floor and ceiling radiosity a field pixel.
//
// CascadePS  one additive pass of level i: a quarter of child direction g_pass.x. Without CC_FIX one trace, continued by the
//            next level's 4 probes around (4 passes a level; g_tex.w = 0 on the last level: nothing beyond). With CC_FIX
//            one trace to the next-level probe at corner g_pass.yz and that probe's share (16 passes a level). The level's
//            target is cleared first and the passes are added (blend one, one), so a pass stays one trace loop.
// GatherPS   level 0 to a probe's irradiance: faces +x, -x, +z, -z; with CC_GATHER_B floor and ceiling in x, y.
// Defines: CC_STEPS the trace's budget (32), CC_D0 level 0's directions (16).
#pragma once
static const char* kPlanCascadesHlsl = R"RAW(
#ifndef CC_STEPS
#define CC_STEPS 32
#endif
#ifndef CC_D0
#define CC_D0 16
#endif
static const float kHit = 0.75;
static const float kMinStep = 0.25;
static const float kBack = 1.5;
static const float kCapPx = 8.0;
static const float kInv2Pi = 0.15915494;

float4 g_field : register(c0);  // px a tile, the pad (px), w, h of the field
float4 g_field2 : register(c1); // 1 / w, 1 / h of the field
float4 g_band : register(c2);   // 1 in the band's channel
float4 g_lvl : register(c3);    // this level: probe spacing (m), block side, t0, t1 (m)
float4 g_up : register(c4);     // the next level: probe spacing, block side, probes in x, z
float4 g_tex : register(c5);    // 1 / w, 1 / h of a level's image, the traced directions (4 x this level's), 1 = a next level is bound
float4 g_pass : register(c6);   // the child direction 0..3; CC_FIX: the next-level probe's corner (0 or 1, 0 or 1); a step's longest share of the distance (0.1)

sampler2D sField : register(s0); // linear, clamp
sampler2D sEmit : register(s1);  // point, clamp
sampler2D sUp : register(s2);    // point: the next level (GatherPS: level 0)

float SlabT(float a, float t) { return 0.63661977 * (atan2(a, t) + a * t / (a * a + t * t)); }
float SlabU(float t) { return 9.0 / (t * t + 9.0); }
float FieldAt(float2 xz) {
    const float2 f = clamp(xz * g_field.x + g_field.y - 0.5, 0, g_field.zw - 1);
    return dot(tex2Dlod(sField, float4((f + 0.5) * g_field2.xy, 0, 0)), g_band);
}
float4 EmitAt(float2 xz) {
    const float2 i = clamp(floor(xz * g_field.x + g_field.y), 0, g_field.zw - 1);
    return tex2Dlod(sEmit, float4((i + 0.5) * g_field2.xy, 0, 0));
}

// PlanCascades::TraceSeg: xyz what the segment meets (vertical face, floor, ceiling), w = 1 when it reached b
float4 TraceSeg(float2 a, float2 b, float tA, float tB) {
    const float2 d = b - a;
    const float len = length(d) * g_field.x;
    float3 rad = 0;
    float s = 0, t0 = tA, clear = 0, done = 0; // (the end is the loop's own test, as in PlanTrace)
    float3 k0 = float3(SlabT(1, tA), SlabT(2, tA), SlabU(tA));
    if (!(len > 1e-3)) clear = 1, done = 1;
    for (int n = 0; n < CC_STEPS && done == 0; n++) {
        const float dist = FieldAt(a + d * (s / len));
        if (dist < kHit) {
            const float w = EmitAt(a + d * (max(s - kBack, 0.0) / len)).x;
            rad += w * float3((k0.x + k0.y) * 0.25, k0.z * kInv2Pi, k0.z * kInv2Pi);
            done = 1;
        } else {
            const float st = min(min(max(dist - kHit, kMinStep), max(kCapPx, g_pass.w * t0 * g_field.x)), len - s);
            const float s1 = s + st, t1 = tA + (tB - tA) * s1 / len;
            const float3 k1 = float3(SlabT(1, t1), SlabT(2, t1), SlabU(t1));
            const float4 m = EmitAt(a + d * ((s + 0.5 * st) / len));
            rad += float3((m.y * (k0.x - k1.x) + m.z * (k0.y - k1.y)) * 0.25, m.z * (k0.z - k1.z) * kInv2Pi, m.y * (k0.z - k1.z) * kInv2Pi);
            s = s1, t0 = t1, k0 = k1;
            if (s >= len - 1e-4) clear = 1, done = 1;
        }
    }
    return float4(rad, clear);
}

float4 CascadePS(float2 vpos : VPOS) : COLOR {
    const float2 t = floor(vpos), P = floor(t / g_lvl.y), dd = t - P * g_lvl.y;
    const float2 p = (P + 0.5) * g_lvl.x;
    const float2 u = p / g_up.x - 0.5, b = floor(u), fr = u - b;
    const float jj = 4 * (dd.y * g_lvl.y + dd.x) + g_pass.x;
    float2 dir;
    sincos((jj + 0.5) * 6.2831853 / g_tex.z, dir.y, dir.x);
    const float2 start = p + g_lvl.z * dir;
    const float jz = floor(jj / g_up.y);
    const float2 jo = float2(jj - jz * g_up.y, jz) + 0.5;
#ifdef CC_FIX
    const float2 q = clamp(b + g_pass.yz, 0, g_up.zw - 1);
    const float2 w2 = lerp(1 - fr, fr, g_pass.yz);
    const float4 seg = TraceSeg(start, (q + 0.5) * g_up.x + g_lvl.w * dir, g_lvl.z, g_lvl.w);
    const float3 up = tex2Dlod(sUp, float4((q * g_up.y + jo) * g_tex.xy, 0, 0)).xyz;
    return float4(0.25 * w2.x * w2.y * (seg.xyz + seg.w * up), 0);
#else
    const float4 seg = TraceSeg(start, p + g_lvl.w * dir, g_lvl.z, g_lvl.w);
    float3 up = 0;
    for (int k = 0; k < 4; k++) {
        const float2 c = float2(k % 2, k / 2);
        const float2 q = clamp(b + c, 0, g_up.zw - 1), w2 = lerp(1 - fr, fr, c);
        up += w2.x * w2.y * tex2Dlod(sUp, float4((q * g_up.y + jo) * g_tex.xy, 0, 0)).xyz;
    }
    return float4(0.25 * (seg.xyz + seg.w * g_tex.w * up), 0);
#endif
}

float4 GatherPS(float2 vpos : VPOS) : COLOR {
    const float2 P = floor(vpos);
    const float side = sqrt(CC_D0), k = 2 * sin(3.14159265 / CC_D0);
    float4 e = 0;
    float2 fc = 0;
    for (int j = 0; j < CC_D0; j++) {
        const float jz = floor(j / side);
        const float3 r = tex2Dlod(sUp, float4((P * side + float2(j - jz * side, jz) + 0.5) * g_tex.xy, 0, 0)).xyz;
        float2 dir;
        sincos((j + 0.5) * 6.2831853 / CC_D0, dir.y, dir.x);
        e += r.x * k * max(float4(dir.x, -dir.x, dir.y, -dir.y), 0);
        fc += r.yz * (6.2831853 / CC_D0);
    }
#ifdef CC_GATHER_B
    return float4(fc, 0, 0);
#else
    return e;
#endif
}
)RAW";
