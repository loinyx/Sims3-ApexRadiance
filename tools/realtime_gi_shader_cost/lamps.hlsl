// Lamp snippets a patched ps_3_0 copy of a game shader would take. Cost prototypes, not shipped code.
// Defines: LAMPS (0 = host stand-in only), SOURCE (0 = constants, 1 = index cells and lamp table in the atlas),
// CONE (0/1), SHADOW (0 none, 1 one compare, 4 four taps averaged, 5 four taps with bilinear weights),
// CELLONLY (1 = only the cell and table reads, to price the lookup alone),
// LOOPED (1 = a rep loop over the lamps with one index texel a lamp; needs SOURCE 1, constants cannot be indexed).
#ifndef LAMPS
#define LAMPS 0
#endif
#ifndef SOURCE
#define SOURCE 0
#endif
#ifndef CONE
#define CONE 0
#endif
#ifndef SHADOW
#define SHADOW 0
#endif
#ifndef LOOPED
#define LOOPED 0
#endif
#ifndef CELLONLY
#define CELLONLY 0
#endif

sampler2D sAlbedo : register(s0);
sampler2D sAtlas : register(s1); // the one float atlas: shadow faces, index cells, lamp table

float4 cAmbient;
float4 cAtlas; // xy texel size, z depth bias
float4 cFace;  // xy face size in uv, zw face size in texels
float4 cCell;  // xy world xz to cell uv scale, zw offset
float4 cCellY; // x 1 / story height, y -y0 / story height, z v step a story, w u step to the second index texel
float4 cTable; // x u step a lamp index, y u origin, z v of row 0, w v step a row

#if SOURCE == 0 && LAMPS > 0
float4 cLampPos[LAMPS]; // xyz position, w 1 / radius
float4 cLampCol[LAMPS]; // rgb colour, w v origin of the lamp's six faces in the atlas
float4 cLampDir[LAMPS]; // xyz axis x cone scale, w cos(outer) x cone scale
#endif

struct PSIn
{
    float2 uv : TEXCOORD0;
    float3 n : TEXCOORD1;
    float3 wpos : TEXCOORD8; // PatchObjectLampVs already delivers it there
};

float4 Fetch(float2 uv)
{
    // texldl: the uv depends on another read, so screen derivatives are meaningless
    return tex2Dlod(sAtlas, float4(uv, 0, 0));
}

// v = lamp to point, ref = radial depth to compare. The six faces lie side by side in u; the shadow pass renders
// each face with the same st convention, so no per-face sign flips are needed here.
float Shadow(float3 v, float ref, float vOrigin)
{
    float3 a = abs(v);
    float m = max(a.x, max(a.y, a.z));
    float2 st;
    float face;
    [flatten] if (a.x >= m) { st = v.zy; face = v.x > 0 ? 0 : 1; }
    else if (a.y >= m) { st = v.xz; face = v.y > 0 ? 2 : 3; }
    else { st = v.xy; face = v.z > 0 ? 4 : 5; }
    float2 f = st / m * 0.5 + 0.5;
#if SHADOW == 1
    return step(ref, Fetch(float2((f.x + face) * cFace.x, f.y * cFace.y + vOrigin)).r);
#else
    float2 px = f * cFace.zw - 0.5;
    float2 base = clamp(floor(px), 0, cFace.zw - 2); // all four taps stay inside the face
    float2 w = saturate(px - base);
    float2 uv = (base + 0.5) * cAtlas.xy + float2(face * cFace.x, vOrigin);
    float t00 = step(ref, Fetch(uv).r);
    float t10 = step(ref, Fetch(uv + float2(cAtlas.x, 0)).r);
    float t01 = step(ref, Fetch(uv + float2(0, cAtlas.y)).r);
    float t11 = step(ref, Fetch(uv + cAtlas.xy).r);
#if SHADOW == 4
    return (t00 + t10 + t01 + t11) * 0.25;
#else
    return lerp(lerp(t00, t10, w.x), lerp(t01, t11, w.x), w.y);
#endif
#endif
}

float3 Lamp(float3 P, float3 N, float4 pos, float4 col, float4 dir)
{
    float3 d = pos.xyz - P;
    float d2 = dot(d, d);
    float inv = rsqrt(d2);
    float3 L = d * inv;
    float dist = d2 * inv;
    float win = saturate(1 - dist * pos.w); // reaches 0 at the radius, so a cell border never pops
    float k = win * win * saturate(dot(N, L)) / (d2 + 1);
#if CONE
    float c = saturate(dot(-L, dir.xyz) - dir.w);
    k *= c * c;
#endif
#if SHADOW
    k *= Shadow(-d, dist * pos.w - cAtlas.z, col.w);
#endif
    return col.rgb * k;
}

float4 main(PSIn i) : COLOR0
{
    float4 albedo = tex2D(sAlbedo, i.uv);
    float3 light = cAmbient.rgb;
#if LAMPS > 0
    float3 N = normalize(i.n);
#if SOURCE == 1
    float story = floor(i.wpos.y * cCellY.x + cCellY.y);
    float2 cell = i.wpos.xz * cCell.xy + cCell.zw + float2(0, story * cCellY.z);
    float4 idx[2];
    idx[0] = Fetch(cell);
    idx[1] = 0;
#if LAMPS > 4
    idx[1] = Fetch(cell + float2(cCellY.w, 0));
#endif
#endif
#if LOOPED
    [loop]
#else
    [unroll]
#endif
    for (int l = 0; l < LAMPS; l++)
    {
#if SOURCE == 1
        // index 0 is a black lamp row, so an empty slot needs no branch
#if LOOPED
        float u = Fetch(cell + float2(l * cCellY.w, 0)).r * cTable.x + cTable.y;
#else
        float u = idx[l / 4][l % 4] * cTable.x + cTable.y;
#endif
        float4 pos = Fetch(float2(u, cTable.z));
        float4 col = Fetch(float2(u, cTable.z + cTable.w));
#if CONE
        float4 dir = Fetch(float2(u, cTable.z + 2 * cTable.w));
#else
        float4 dir = 0;
#endif
#if CELLONLY
        light += pos.xyz + col.xyz + dir.xyz;
#else
        light += Lamp(i.wpos, N, pos, col, dir);
#endif
#else
        light += Lamp(i.wpos, N, cLampPos[l], cLampCol[l], cLampDir[l]);
#endif
    }
#endif
    return float4(albedo.rgb * light, albedo.a);
}
