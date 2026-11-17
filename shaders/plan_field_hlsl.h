// Real-time GI (docs/features/realtime-gi, work package S2-03): the wall-plan visibility field of features/plan_field.h as
// ps_3_0 passes, function by function with the same float math. The seeds of a band (PlanField::SeedBand, CPU) are uploaded;
// the flood and the distance run on the GPU when a build edit changes a story's walls; the trace is a function for the gather.
//
// FloodPS  one jump flooding pass of one band: the nearest of the 9 seeds at +- step (PlanField::BuildField's pass)
// DistPS   the distance to the pixel's seed in pixels; written to the band's channel of the field (colour write mask)
// TracePS  the harness's pass: PlanTrace for the point pair (sA[x], sB[y]); r = clear, g = steps used / 255
//
// PlanTrace(a, b, used) is PlanField::TraceStory: 1 when b is seen from a inside one story. The story walk
// (PlanField::Walk, SlabOpen) stays with the caller. Defines: PLAN_STEPS the step budget (default 32); PLAN_FILTERED reads the
// field with the sampler's bilinear filter (1 read a step) in place of 4 point reads and PlanField::Field::At's own weights.
//
// The seed texture holds (x, z) in pixels, no seed = kPlanNoSeed in both (finite: a pixel shader's infinities are not relied on).
// The field holds up to 4 bands in r, g, b, a; a band without walls reads g_plan.w everywhere.
#pragma once
static const char* kPlanFieldHlsl = R"RAW(
#ifndef PLAN_STEPS
#define PLAN_STEPS 32
#endif
static const float kPlanNoSeed = -1e15;
static const float kHit = 0.75;
static const float kMinStep = 0.25;

float4 g_plan : register(c0);   // 1 / w, 1 / h of the seed texture; the pass's step; the distance of "no wall" (px)
float4 g_size : register(c1);   // w, h of the seed texture
float4 g_field : register(c2);  // px a tile, the pad (px), w, h of the field
float4 g_field2 : register(c3); // 1 / w, 1 / h of the field, the story's base (m), the last band's index
float4 g_cuts : register(c4);   // the bands' cut heights over the base (unused: 1e9)
float4 g_pairs : register(c5);  // TracePS: 1 / the count of points in sA, in sB

sampler2D sSeed : register(s0);  // FloodPS, DistPS: point, clamp
sampler2D sField : register(s1); // PlanTrace: point (PLAN_FILTERED: linear), clamp
sampler2D sA : register(s2);     // TracePS: n x 1 points (x, y, z) in lot metres
sampler2D sB : register(s3);

float4 FloodPS(float2 vpos : VPOS) : COLOR {
    const float2 p = floor(vpos), c = p + 0.5;
    float2 best = kPlanNoSeed;
    float bd = 3e38;
    for (int oz = -1; oz <= 1; oz++)
        for (int ox = -1; ox <= 1; ox++) {
            const float2 j = p + float2(ox, oz) * g_plan.z;
            const float2 s = tex2Dlod(sSeed, float4((j + 0.5) * g_plan.xy, 0, 0)).xy;
            const float2 d = s - c;
            const float cd = d.x * d.x + d.y * d.y;
            if (j.x >= 0 && j.y >= 0 && j.x < g_size.x && j.y < g_size.y && cd < bd) bd = cd, best = s;
        }
    return float4(best, 0, 1);
}

float4 DistPS(float2 vpos : VPOS) : COLOR {
    const float2 c = floor(vpos) + 0.5;
    const float2 s = tex2Dlod(sSeed, float4(c * g_plan.xy, 0, 0)).xy;
    const float2 d = s - c;
    return s.x < -1e14 ? g_plan.w : sqrt(d.x * d.x + d.y * d.y);
}

// PlanField::Field::At: the band's distance (px) at (x, z) in lot metres
float PlanAt(float4 band, float2 xz) {
    const float2 f = clamp(xz * g_field.x + g_field.y - 0.5, 0, g_field.zw - 1);
    const float2 i = min(floor(f), g_field.zw - 2), t = f - i;
#ifdef PLAN_FILTERED
    return dot(tex2Dlod(sField, float4((f + 0.5) * g_field2.xy, 0, 0)), band);
#else
    const float2 uv = (i + 0.5) * g_field2.xy;
    const float p00 = dot(tex2Dlod(sField, float4(uv, 0, 0)), band);
    const float p10 = dot(tex2Dlod(sField, float4(uv + float2(g_field2.x, 0), 0, 0)), band);
    const float p01 = dot(tex2Dlod(sField, float4(uv + float2(0, g_field2.y), 0, 0)), band);
    const float p11 = dot(tex2Dlod(sField, float4(uv + g_field2.xy, 0, 0)), band);
    return (p00 * (1 - t.x) + p10 * t.x) * (1 - t.y) + (p01 * (1 - t.x) + p11 * t.x) * t.y;
#endif
}

// PlanField::TraceStory: 1 = b is seen from a (lot metres, both in the story of the bound field). A budget that runs out is 0.
float PlanTrace(float3 a, float3 b, out float used) {
    const float3 d = b - a;
    const float len = length(d.xz) * g_field.x;
    used = 0;
    if (!(len > 1e-4)) return 1;
    float t = 0, clear = 0, done = 0; // (the end is the loop's own test: this compiler's break inside an if did not leave the rep)
    for (int n = 0; n < PLAN_STEPS && done == 0; n++) {
        const float r = t / len, y = a.y + d.y * r;
        const float3 over = step(g_cuts.xyz, y - g_field2.z);
        const float bi = over.x + over.y + over.z;
        const float4 band = float4(bi == 0, bi == 1, bi == 2, bi == 3);
        const float dist = PlanAt(band, a.xz + d.xz * r);
        float st = max(dist - kHit, kMinStep);
        const bool up = d.y > 1e-6 && bi < g_field2.w, down = d.y < -1e-6 && bi > 0;
        if (up || down) { // not past the band's end: the next band's walls differ
            const float cut = up ? dot(g_cuts, band) : dot(g_cuts.xyz, band.yzw);
            const float tEnd = (g_field2.z + cut - a.y) / d.y * len;
            if (tEnd > t) st = min(st, tEnd - t + 1e-3);
        }
        t += st;
        clear = dist >= kHit && t >= len;
        done = dist < kHit || t >= len;
        used += done == 0;
    }
    return clear;
}

float4 TracePS(float2 vpos : VPOS) : COLOR {
    const float2 p = floor(vpos) + 0.5;
    const float3 a = tex2Dlod(sA, float4(p.x * g_pairs.x, 0.5, 0, 0)).xyz, b = tex2Dlod(sB, float4(p.y * g_pairs.y, 0.5, 0, 0)).xyz;
    float used;
    const float clear = PlanTrace(a, b, used);
    return float4(clear, used / 255.0, 0, 1);
}
)RAW";
