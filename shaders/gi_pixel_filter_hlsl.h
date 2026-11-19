// Filter and history of the quarter-resolution ray-traced shadow mask (real-time GI research 16 §4, work package XR-05).
// Three ps_3_0 passes: TemporalPS (history), AtrousFirstPS / AtrousPS (edge-aware 5x5 a-trous), UpsamplePS (depth-aware,
// full resolution). Checked offline by tools/gi_pixel_filter_test; in the game: features/gi_pixel_shadows.cpp (XR-07).
#pragma once
static const char* kGiPixelFilterHlsl = R"RAW(
sampler2D sRaw : register(s0);       // quarter: r = this frame's visibility sample (0 or 1), g = a caster moved within reach
sampler2D sGuide : register(s1);     // quarter: rgb = world normal, a = linear depth / far
sampler2D sMotion : register(s2);    // quarter: rg = previous uv - uv, b = motion class / 3
sampler2D sHist : register(s3);      // quarter: (fast sum, fast count, settle sum, settle count)
sampler2D sPrevGuide : register(s4); // sGuide of the previous frame
sampler2D sIn : register(s5);        // quarter: r = visibility, g = count / 16, a = depth
sampler2D sDepth : register(s6);     // full: r = linear depth / far
float4 kSize : register(c0);     // quarter width, height, 1 / width, 1 / height
float4 kTemporal : register(c1); // x = camera still, y = reset (cut, lamp change), z / w = clip sigma: moved caster / moving camera (0 = off)
float4 kTol : register(c2);      // x = relative depth, y = 1.5 x angle of a quarter pixel, z = normal cosine, w = history cap near a moved caster
float4 kViewZ : register(c3);    // xyz = camera forward (world)
float4 kAtrous : register(c4);   // x = step, y = strength, z = extra strength at count 0 (pass 3 only)
#ifndef GI_FULL_DEPTH // the game's INTZ is device depth: features/gi_pixel_shadows.cpp linearises it through this macro
#define GI_FULL_DEPTH(uv) tex2D(sDepth, uv).r
#endif

// Depth difference a surface may show over `pixels` quarter pixels: a floor seen at a grazing angle changes depth fast.
float DepthTol(float z, float3 n, float pixels) {
    float c = abs(dot(n, kViewZ.xyz));
    return z * (kTol.x + kTol.y * pixels * sqrt(saturate(1 - c * c)) / max(c, 0.05));
}

// Two accumulators a pixel. Settle: exact sum of the samples taken while nothing moved, same pixel only, stops at 16
// (the fixed sample set is complete, the value never changes again). Fast: reprojected running mean that follows motion.
float4 TemporalPS(float2 uv : TEXCOORD0) : COLOR0 {
    float4 raw = tex2D(sRaw, uv);
    float4 g = tex2D(sGuide, uv);
    float3 mc = tex2D(sMotion, uv).xyz;
    float x = raw.r;
    bool flag = raw.g > 0.5;
    bool mover = mc.z > 0.83;
    bool still = kTemporal.x > 0.5;
    bool reset = kTemporal.y > 0.5;
    float tol = DepthTol(g.a, g.rgb, 1);

    float4 hc = tex2D(sHist, uv);
    float4 pc = tex2D(sPrevGuide, uv);
    bool keep = still && !reset && !flag && !mover && abs(pc.a - g.a) <= tol && dot(pc.rgb, g.rgb) >= kTol.z;
    float2 s = keep ? hc.ba : 0;
    // A sample taken while the camera or a caster in reach moves belongs to another picture: it never enters the settle sum
    if (still && !flag && !mover && s.y < 15.5) s += float2(x, 1);

    float2 p = (uv + mc.xy) * kSize.xy - 0.5;
    float2 fr = frac(p);
    float2 base = (floor(p) + 0.5) * kSize.zw;
    static const float2 kTap[4] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1)};
    float2 acc = 0;
    float wsum = 0;
    for (int i = 0; i < 4; i++) {
        float2 t = base + kTap[i] * kSize.zw;
        float4 h = tex2Dlod(sHist, float4(t, 0, 0));
        float4 pg = tex2Dlod(sPrevGuide, float4(t, 0, 0));
        float2 bw = lerp(1 - fr, fr, kTap[i]);
        float w = bw.x * bw.y;
        if (abs(pg.a - g.a) > tol || dot(pg.rgb, g.rgb) < kTol.z || any(abs(t - 0.5) > 0.5)) w = 0;
        acc += h.rg * w;
        wsum += w;
    }
    float2 f = float2(x, 1); // rejected: this frame's sample alone, the spatial filter widens on the low count
    if (wsum > 1e-3 && !mover && !reset) {
        float2 h = acc / wsum;
        float n0 = min(h.y, (flag ? kTol.w : 16) - 1);
        float m0 = h.x / max(h.y, 1e-3);
        // The clip stays on for the frames after the caster left, while the capped history still holds its shadow
        float sigma = flag || (still && h.y < 8.5) ? kTemporal.z : (still ? 0 : kTemporal.w);
        if (sigma > 0) {
            float m1 = 0, m2 = 0;
            for (int y = -1; y <= 1; y++)
                for (int k = -1; k <= 1; k++) {
                    float r = tex2Dlod(sRaw, float4(uv + float2(k, y) * kSize.zw, 0, 0)).r;
                    m1 += r;
                    m2 += r * r;
                }
            m1 /= 9;
            float sd = sqrt(max(m2 / 9 - m1 * m1, 0)) * sigma;
            m0 = clamp(m0, m1 - sd, m1 + sd);
        }
        f = float2(m0 * n0 + x, n0 + 1);
    }
    return float4(f, s);
}

// Shown value: the fast mean cross-fades into the settle mean as it fills, so settling never pops
float2 Resolve(float4 h) {
    float mf = h.x / max(h.y, 1e-3);
    float ms = h.z / max(h.w, 1e-3);
    return float2(h.w >= 15.5 ? ms : lerp(mf, ms, h.w / 16), saturate(max(h.y, h.w) / 16));
}
float2 Tap(float2 uv, bool first) {
    float4 t = first ? tex2Dlod(sHist, float4(uv, 0, 0)) : tex2Dlod(sIn, float4(uv, 0, 0));
    return first ? Resolve(t) : t.rg;
}
float4 Atrous(float2 uv, bool first) {
    float4 g = tex2D(sGuide, uv);
    float2 c = Tap(uv, first);
    static const float kKernel[3] = {0.375, 0.25, 0.0625};
    float sum = 0, wsum = 0;
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++) {
            float2 o = float2(x, y) * kAtrous.x;
            float2 t = uv + o * kSize.zw;
            float4 tg = tex2Dlod(sGuide, float4(t, 0, 0));
            float2 tv = Tap(t, first);
            float w = kKernel[abs(x)] * kKernel[abs(y)] * exp(-abs(tg.a - g.a) / DepthTol(g.a, g.rgb, max(length(o), 1)))
                * pow(saturate(dot(tg.rgb, g.rgb)), 32) * (0.25 + 0.75 * tv.y);
            sum += tv.x * w;
            wsum += w;
        }
    // The extra strength of a low count applies only where the value is uncertain: a pixel that reads fully lit or
    // fully dark after the narrow passes has nothing to smooth, and a wide kernel there drags a halo behind a moving shadow
    float low = kAtrous.z * (1 - c.y) * saturate(16 * c.x * (1 - c.x));
    return float4(lerp(c.x, sum / wsum, saturate(kAtrous.y + low)), c.y, 0, g.a);
}
float4 AtrousFirstPS(float2 uv : TEXCOORD0) : COLOR0 { return Atrous(uv, true); }
float4 AtrousPS(float2 uv : TEXCOORD0) : COLOR0 { return Atrous(uv, false); }

// Full resolution: 4 quarter taps, bilinear weight divided by the depth difference against the full-resolution depth.
// The compose effect multiplies the back buffer with the result (research 16 §4.1); here it is written as a grey.
float4 UpsamplePS(float2 uv : TEXCOORD0) : COLOR0 {
    float z = GI_FULL_DEPTH(uv);
    float2 p = uv * kSize.xy - 0.5;
    float2 fr = frac(p);
    float2 base = (floor(p) + 0.5) * kSize.zw;
    static const float2 kTap[4] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1)};
    float sum = 0, wsum = 0;
    for (int i = 0; i < 4; i++) {
        float4 t = float4(base + kTap[i] * kSize.zw, 0, 0);
        float4 q = tex2Dlod(sIn, t);
        float2 bw = lerp(1 - fr, fr, kTap[i]);
        float w = (bw.x * bw.y + 0.01) / (DepthTol(q.a, tex2Dlod(sGuide, t).rgb, 1) + abs(q.a - z));
        sum += q.r * w;
        wsum += w;
    }
    float v = sum / wsum;
    return float4(v, v, v, 1);
}
)RAW";
