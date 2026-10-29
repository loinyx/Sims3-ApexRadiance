// GPU smoothing of the terrain chunk light maps (features/lightmap_smooth.cpp, GPU path). Every entry point reproduces
// one step of the CPU path (Process in lightmap_smooth.cpp) with the same float math, in the same order:
//
//   GatherPS     P grid (264x264 = source texels -4..259 of the chunk, P = source + 4) from the chunk's own map and its
//                8 neighbours' maps (one quad per neighbour region; a missing neighbour = the centre map with CLAMP, i.e.
//                "clamp to the centre chunk" like the CPU). Float RGBA (the CPU decodes DXT5 to float).
//   HBlurPS      horizontal 7-tap Gaussian (sigma 1.5) of (R, G, B, Y), Y = 0.299 R + 0.587 G + 0.114 B
//   VBlurPS      vertical 7-tap Gaussian, then the colour ratio: blurred RGB / blurred Y clamped 0..4, faded to 1 when
//                blurred Y < 0.03 (very dim: neutral), 1 when blurred Y <= 1e-4
//   HUpYAPS      4x enlargement, horizontal: cubic B-spline of Y and A      (output 1024 x 264)
//   HUpChromaPS  4x enlargement, horizontal: linear of the colour ratio     (output 1024 x 264)
//   VUpPS        4x enlargement, vertical (same weights), value = Y x ratio x 255 + 4x4 Bayer dither, rounded to 8 bits
//                (output = the chunk map's level 0, 1024x1024 A8R8G8B8)
//   DownPS       one mip level: 2x2 box of 8-bit values, (sum + 2) / 4 rounded down (the CPU's integer mip)
//   CopyPS       exact copy of 8-bit values (scratch mip -> chunk mip)
//
// Output texel j of the enlargement sits at source coordinate s = (j + 0.5) / 4 - 0.5: B-spline taps floor(s) - 1 ..
// floor(s) + 2 (weights of t = s - floor(s)), linear taps floor(s), floor(s) + 1.
//
// Every fetch is a point sample at a texel centre with tex2Dlod (level 0): samplers POINT / POINT / mip NONE, CLAMP.
// Texture coordinates arrive in PIXEL units of the target (u = x + 0.5 at pixel x), except GatherPS (normalized).
//
// Validate / compile each entry point (no macros): fxc /T ps_3_0 /E <entry> /O3 lightmap_smooth_ps.hlsl
// The mod compiles the copy of this file in lightmap_smooth_hlsl.h at run time with D3DCompile (ps_3_0, O3): keep them
// identical.

sampler2D sIn : register(s0);
sampler2D sIn2 : register(s1);
float4 inSize : register(c0);  // s0: x = 1 / width, y = 1 / height, z = width, w = height
float4 inSize2 : register(c1); // s1: same

struct PSIn {
    float2 uv : TEXCOORD0;
};

static const float kG[7] = {0.0366, 0.1112, 0.2167, 0.2710, 0.2167, 0.1112, 0.0366}; // sigma 1.5 (CPU table)
static const float3 kLuma = float3(0.299, 0.587, 0.114);

// Point sample of texel (integer index) of s0 / s1
float4 Fetch0(float2 texel) { return tex2Dlod(sIn, float4((texel + 0.5) * inSize.xy, 0, 0)); }
float4 Fetch1(float2 texel) { return tex2Dlod(sIn2, float4((texel + 0.5) * inSize2.xy, 0, 0)); }

// CPU BSplineWeights
float4 BSpline(float t) {
    const float t2 = t * t, t3 = t2 * t, s = 1 - t;
    return float4(s * s * s / 6.0, (3 * t3 - 6 * t2 + 4) / 6.0, (-3 * t3 + 3 * t2 + 3 * t + 1) / 6.0, t3 / 6.0);
}

float Luma(float3 c) { return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b; }

float4 GatherPS(PSIn i) : COLOR0 { return tex2Dlod(sIn, float4(i.uv, 0, 0)); }

float4 HBlurPS(PSIn i) : COLOR0 {
    const float2 p = floor(i.uv);
    float4 s = 0;
    [unroll] for (int k = -3; k <= 3; k++) {
        const float4 c = Fetch0(p + float2(k, 0));
        s += kG[k + 3] * float4(c.rgb, Luma(c.rgb));
    }
    return s;
}

float4 VBlurPS(PSIn i) : COLOR0 {
    const float2 p = floor(i.uv);
    float4 s = 0;
    [unroll] for (int k = -3; k <= 3; k++) s += kG[k + 3] * Fetch0(p + float2(0, k));
    const float yb = s.a;
    const float w = saturate(yb / 0.03);
    const float3 ratio = yb > 1e-4 ? clamp(s.rgb / yb, 0.0, 4.0) : float3(1, 1, 1);
    return float4(1.0 + (ratio - 1.0) * w, 1);
}

float4 HUpYAPS(PSIn i) : COLOR0 {
    const float2 p = floor(i.uv); // x = output column, y = P row
    const float s = (p.x + 0.5) / 4.0 - 0.5;
    const float f = floor(s);
    const float4 w = BSpline(s - f);
    float yy = 0, aa = 0;
    [unroll] for (int k = 0; k < 4; k++) {
        const float4 c = Fetch0(float2(f + 3 + k, p.y)); // P index of tap k = (f - 1 + k) + 4
        yy += w[k] * Luma(c.rgb);
        aa += w[k] * c.a;
    }
    return float4(yy, aa, 0, 1);
}

float4 HUpChromaPS(PSIn i) : COLOR0 {
    const float2 p = floor(i.uv);
    const float s = (p.x + 0.5) / 4.0 - 0.5;
    const float f = floor(s), lf = s - f;
    const float3 c0 = Fetch0(float2(f + 4, p.y)).rgb;
    const float3 c1 = Fetch0(float2(f + 5, p.y)).rgb;
    return float4(c0 * (1 - lf) + c1 * lf, 1);
}

float4 VUpPS(PSIn i) : COLOR0 {
    const float2 p = floor(i.uv); // output texel (x, y), level 0
    const float s = (p.y + 0.5) / 4.0 - 0.5;
    const float f = floor(s), lf = s - f;
    const float4 w = BSpline(lf);
    float yy = 0, aa = 0;
    [unroll] for (int k = 0; k < 4; k++) {
        const float2 ya = Fetch0(float2(p.x, f + 3 + k)).xy;
        yy += w[k] * ya.x;
        aa += w[k] * ya.y;
    }
    const float3 c0 = Fetch1(float2(p.x, f + 4)).rgb;
    const float3 c1 = Fetch1(float2(p.x, f + 5)).rgb;
    const float3 ch = c0 * (1 - lf) + c1 * lf;
    // CPU table bayer[y & 3][x & 3] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}}
    //  = 4 * b2(x & 1, y & 1) + b2((x >> 1) & 1, (y >> 1) & 1), b2(a, b) = 2a + 3b - 4ab
    const float2 q = p - 4 * floor(p / 4);
    const float2 lo = q - 2 * floor(q / 2), hi = floor(q / 2);
    const float bayer = 4 * (2 * lo.x + 3 * lo.y - 4 * lo.x * lo.y) + (2 * hi.x + 3 * hi.y - 4 * hi.x * hi.y);
    const float d = (bayer + 0.5) / 16.0 - 0.5;
    const float3 v = clamp(yy * ch * 255.0 + d, 0.0, 255.0);
    const float a = clamp(aa * 255.0 + d, 0.0, 255.0);
    return floor(float4(v, a) + 0.5) / 255.0; // exactly k / 255: the A8R8G8B8 target stores k
}

// 8-bit value of a texel of an A8R8G8B8 texture (exact integer)
float4 Bytes0(float2 texel) { return floor(Fetch0(texel) * 255.0 + 0.5); }

float4 DownPS(PSIn i) : COLOR0 {
    const float2 p = floor(i.uv) * 2; // top-left texel of the 2x2 block in the level above
    const float4 sum = Bytes0(p) + Bytes0(p + float2(1, 0)) + Bytes0(p + float2(0, 1)) + Bytes0(p + float2(1, 1));
    return floor((sum + 2) / 4.0) / 255.0;
}

float4 CopyPS(PSIn i) : COLOR0 { return Bytes0(floor(i.uv)) / 255.0; }

// StampPS: the ground light of lamps being dragged, added to the P grid (s0 = RAW) before the chroma blur and the
// enlargement, so it is smoothed exactly as the game's bake of the same lamps would be. The law is the game's bake
// pixel shader (technique StaticTerrainLightmap in Shaders_Win32.precomp): per light
//   colour.rgb x sat(w / d2) x (cos >= 0 ? sat(sqrt(cos)) : 0), d2 = horizontal distance squared + 1e-4 (the compiled
//   shader drops the height term), cos = N . (direction to the light), w = range x intensity x 0.2,
// one additive pass per light into an 8-bit target (the sum is clamped at 1 here). Flat ground: N = up, the texel at
// the lamp's ground height. The alpha (sun visibility) is kept.
float4 stampOrigin : register(c2);    // world x, z of the centre of P texel (0, 0); y = ground height; w = lamp count
float4 stampLamps[16] : register(c4); // per lamp k: c[4 + 2k] = (colour rgb, w), c[5 + 2k] = (position xyz, 0)
float4 StampPS(PSIn i) : COLOR0 {
    const float2 p = floor(i.uv);
    const float4 base = Fetch0(p);
    const float3 P = float3(stampOrigin.x + p.x, stampOrigin.z, stampOrigin.y + p.y);
    float3 sum = 0;
    for (int k = 0; k < 8; k++) {
        if (k >= stampOrigin.w) break;
        const float4 c = stampLamps[2 * k], pos = stampLamps[2 * k + 1];
        const float3 L = P - pos.xyz;
        const float d2 = L.x * L.x + L.z * L.z + 1e-4;
        const float cosv = dot(-normalize(L), float3(0, 1, 0));
        if (cosv >= 0) sum += c.rgb * saturate(c.w / d2) * saturate(sqrt(cosv));
    }
    return float4(min(base.rgb + sum, 1), base.a);
}
