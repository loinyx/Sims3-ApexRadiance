// Apex's own off-screen passes of the Direct3D 9 back end. Cost prototypes, not shipped code.
// Defines: PASS (1 compose, 2 gather, 3 plan sphere trace alone), LAMPS, WALL (compose: wall-aware upsample),
// VPLS, VIS (0 none, 1 room table, 2 room table and plan trace), STEPS, LOOP (0 unrolled, 1 rep loops),
// DIRS (1 = the four directional layers as four render targets).
#ifndef LAMPS
#define LAMPS 16
#endif
#ifndef WALL
#define WALL 0
#endif
#ifndef VPLS
#define VPLS 16
#endif
#ifndef VIS
#define VIS 0
#endif
#ifndef STEPS
#define STEPS 8
#endif
#ifndef LOOP
#define LOOP 0
#endif
#ifndef DIRS
#define DIRS 0
#endif
#if LOOP
#define FOR [loop] for
#else
#define FOR [unroll] for
#endif

sampler2D sPoint0 : register(s0); // texel point: xyz lot position, w room id
sampler2D sPoint1 : register(s1); // texel point: xyz normal
sampler2D sAtlas : register(s2);  // layers, virtual light rows, room table, plan distance field
sampler2D sDirect : register(s3);

float4 cPlan;  // xy lot xz to plan field uv scale, zw offset (one story a pass)
float4 cTrace; // x start offset, y penumbra sharpness, z smallest step, w room table u step

float4 Fetch(float2 uv)
{
    return tex2Dlod(sAtlas, float4(uv, 0, 0));
}

// Sphere trace of the wall-plan distance field from a to b (lot xz). 1 = open, 0 = a wall between.
float PlanTrace(float2 a, float2 b)
{
    float2 dir = b - a;
    float len = length(dir);
    dir /= max(len, 1e-4);
    float t = cTrace.x;
    float vis = 1;
    FOR (int s = 0; s < STEPS; s++)
    {
        float d = Fetch((a + dir * min(t, len)) * cPlan.xy + cPlan.zw).r;
        vis = min(vis, saturate(d * cTrace.y + step(len, t))); // past the end nothing can block
        t += max(d, cTrace.z);
    }
    return vis * step(len, t); // not arrived in STEPS steps counts as blocked
}

#if PASS == 1
float4 cLamp[LAMPS]; // rgb on x colour, w u offset of the lamp's layer in the atlas
float4 cLayer;       // xy tile to layer uv scale, zw one layer texel
float4 cRoom;        // xy uv offset of the room id layer, z u step and w v of the room ambient row

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float3 sum = 0;
    float4 pt = tex2D(sPoint0, uv);
    float2 tile = pt.xz * cLayer.xy;
#if WALL
    // bilinear over the four tile texels, a tap of another room weighs 0
    float2 px = tile / cLayer.zw - 0.5;
    float2 base = floor(px);
    float2 f = px - base;
    float2 uv0 = (base + 0.5) * cLayer.zw;
    float4 w = float4((1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y);
    float2 o[4] = { float2(0, 0), float2(cLayer.z, 0), float2(0, cLayer.w), cLayer.zw };
    [unroll] for (int k = 0; k < 4; k++)
        w[k] *= step(abs(Fetch(uv0 + o[k] + cRoom.xy).r - pt.w), 0.5);
    w /= max(dot(w, 1), 1e-4);
    [unroll] for (int l = 0; l < LAMPS; l++)
    {
        float2 lu = uv0 + float2(cLamp[l].w, 0);
        sum += cLamp[l].rgb * (Fetch(lu).rgb * w[0] + Fetch(lu + o[1]).rgb * w[1] + Fetch(lu + o[2]).rgb * w[2]
            + Fetch(lu + o[3]).rgb * w[3]);
    }
#else
    [unroll] for (int l = 0; l < LAMPS; l++)
        sum += cLamp[l].rgb * tex2D(sAtlas, tile + float2(cLamp[l].w, 0)).rgb;
#endif
    sum += Fetch(float2(pt.w * cRoom.z, cRoom.w)).rgb; // room ambient term
    float4 direct = tex2D(sDirect, uv);
    return float4(direct.rgb + sum, direct.a);
}
#endif

#if PASS == 2
float4 cVpl; // x u step a virtual light, y u origin, z v of row 0 of this lamp, w v step a row

struct Out
{
    float4 c0 : COLOR0;
#if DIRS
    float4 c1 : COLOR1;
    float4 c2 : COLOR2;
    float4 c3 : COLOR3;
#endif
};

Out main(float2 uv : TEXCOORD0)
{
    float4 pt = tex2D(sPoint0, uv);
    float3 n = tex2D(sPoint1, uv).xyz;
    float3 P = pt.xyz + n * cTrace.x; // pushed off its surface before any visibility test
    float3 s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    FOR (int v = 0; v < VPLS; v++)
    {
        float u = (v + 0.5) * cVpl.x + cVpl.y;
        float4 vp = Fetch(float2(u, cVpl.z));                  // xyz position, w room id
        float3 vn = Fetch(float2(u, cVpl.z + cVpl.w)).xyz;     // normal
        float3 fl = Fetch(float2(u, cVpl.z + 2 * cVpl.w)).rgb; // flux
        float3 d = vp.xyz - P;
        float d2 = dot(d, d);
        float3 w = d * rsqrt(d2);
        float k = saturate(dot(vn, -w)) / (d2 + 0.25);
#if VIS >= 1
        k *= Fetch(float2(pt.w, vp.w) * cTrace.w).r; // room joins room, 0 or 1
#endif
#if VIS >= 2
        k *= PlanTrace(P.xz, vp.xz);
#endif
#if DIRS
        s0 += fl * (k * saturate(w.x));
        s1 += fl * (k * saturate(-w.x));
        s2 += fl * (k * saturate(w.z));
        s3 += fl * (k * saturate(-w.z));
#else
        s0 += fl * (k * saturate(dot(n, w)));
#endif
    }
    Out o;
    o.c0 = float4(s0, 1);
#if DIRS
    o.c1 = float4(s1, 1);
    o.c2 = float4(s2, 1);
    o.c3 = float4(s3, 1);
#endif
    return o;
}
#endif

#if PASS == 3
float4 main(float2 uv : TEXCOORD0, float4 ab : TEXCOORD1) : COLOR0
{
    return PlanTrace(ab.xy, ab.zw);
}
#endif
