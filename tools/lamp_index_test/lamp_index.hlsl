// The lamp index block (features/lamp_index.h) as HLSL, with a stand-in host: the reference of the token block that
// ShaderPatches::PatchObjectLampPs(index) inserts into the game's object shaders. check.cpp compiles it as ps_3_0 (the
// compiler fxc uses), prices it and draws it against the CPU model. Not shipped.
sampler2D sAlbedo : register(s0);
sampler2D sGround : register(s1); // the ground light atlas
sampler2D sIndex : register(s2);  // the lamp index texture, point sampled

float4 cHost : register(c0); // the host's own light
float4 cObj : register(c1);  // (set in a wall, gate in a fence gap, 0, 0): LampIndex::ObjectRow
float4 F0 : register(c208);  // LampIndex::FrameConstants
float4 F1 : register(c209);
float4 F2 : register(c210);
float4 F3 : register(c211);
float4 F4 : register(c212);
float4 F5 : register(c213);
float4 F6 : register(c214);

struct PSIn
{
    float2 uv : TEXCOORD0;
    float3 n : TEXCOORD1;
    float3 w : TEXCOORD8; // world x, z, y (PatchObjectLampVs)
};

float4 Fetch(float2 uv)
{
    return tex2Dlod(sIndex, float4(uv, 0, 0));
}

float4 main(PSIn i) : COLOR0
{
    float3 N = normalize(i.n);
    float3 P = i.w.xzy;
    float2 uv = clamp(floor(i.w.xy * F0.x + F0.zw), 0, F0.y) * F2.xy + F2.zw;
    float3 Q = 0;
    [loop] for (int l = 0; l < 8; l++)
    {
        float4 e = Fetch(uv); // (index, visibility, visibility set in a wall)
        float2 t = float2(e.x * F1.x + F1.y, F1.z);
        float4 pos = Fetch(t);
        t.y += F1.w;
        float4 col = Fetch(t);
        t.y += F1.w;
        float4 k1 = Fetch(t);
        t.y += F1.w;
        float4 k2 = Fetch(t);
        float3 d = pos.xyz - P;
        float d2 = max(dot(d, d), F3.w);
        float3 L = d * rsqrt(d2);
        float s = saturate(1 - d2 * pos.w);
        float law = saturate(dot(N, L)) * s * s;
        float c = saturate(max(dot(L, k1.xyz) + k1.w, dot(L, k2.xyz) + k2.w));
        law *= col.w + c * (1 - col.w);
        Q += col.rgb * (law * lerp(e.y, e.z, cObj.x));
        uv.x += F1.x;
    }
    float4 g = Fetch(uv); // (ground share, highest lamp head, share set in a wall)
    float ground = lerp(g.x, g.z, cObj.x) * saturate(1 + (g.y - i.w.z) * F6.x) * saturate(F5.z - abs(i.w.z - F5.y) * F5.w) * F5.x;
    Q *= lerp(F3.y, F3.z, cObj.y);
    float3 T = tex2D(sGround, i.w.xy * F4.xy + F4.zw).rgb * (saturate(N.y) * ground);
    return float4(tex2D(sAlbedo, i.uv).rgb * max(cHost.rgb + Q, T), 1);
}
