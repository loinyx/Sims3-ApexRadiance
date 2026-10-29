// The terrain light bake with lamp cones (patches/night_terrain_relight_patch.cpp; features/terrain_bake_cone.h). Apex's copy
// of the game's bake pixel shader (technique StaticTerrainLightmap, ps_2_0), made where the game creates that shader. One
// additive pass per light into the chunk's 8-bit target, as the game draws it:
//   colour.rgb x sat(w / d2) x (cos >= 0 ? sat(sqrt(cos)) : 0), d2 = horizontal distance squared + 1e-4 (the game's shader
//   drops the height term), cos = N . (direction to the light), the point at the height map's height,
// times the light's cone factor S - S t + t (gpu_lamp_law.h), from c5..c7 while c7.w holds the stub's mark (else 1).
// Same registers as the game's shader: t0 = world x, z; t1 = the maps' uv; s0 normal map (x in alpha, z in green), s1 height
// map (255 g + b) x c2.x x 255; c0 = (colour rgb, w = range x intensity x 0.2), c1 = the light's position.
sampler2D bakeNormal : register(s0);
sampler2D bakeHeight : register(s1);
float4 bakeColour : register(c0);
float4 bakeLight : register(c1);
float4 bakeScale : register(c2);
float4 coneAxis : register(c5);  // axis xyz, sc1
float4 coneOff : register(c6);   // off1, sc2, off2
float4 coneSpill : register(c7); // spill rgb, the stub's mark

float4 BakeConePS(float2 world : TEXCOORD0, float2 uv : TEXCOORD1) : COLOR0 {
    const float4 nm = tex2D(bakeNormal, uv), hm = tex2D(bakeHeight, uv);
    float3 n;
    n.xz = 2 * nm.wy - 1.00392163;
    n.y = 1 / rsqrt(saturate(1 - (n.x * n.x + n.z * n.z)));
    n = normalize(n);
    const float y = (hm.y * 255 + hm.z) * bakeScale.x * 255;
    const float3 L = float3(world.x, y, world.y) - bakeLight.xyz;
    const float3 l = -normalize(L);
    const float c = dot(l, n);
    const float d2 = L.x * L.x + L.z * L.z + 1e-4;
    float3 E = bakeColour.rgb * saturate(1 / d2 * bakeColour.w) * (c >= 0 ? saturate(1 / rsqrt(c)) : 0);
    // the cones (TerrainBakeCone::Factor)
    const float al = dot(coneAxis.xyz, l);
    const float t1 = (al + coneOff.x) * -coneAxis.w, t2 = (al - coneOff.z) * coneOff.y;
    const float tsel = t1 > 0 ? t1 : t2;
    const float m = saturate((max(t1, t2) - 1) * 1e4 + 1);
    const float t = saturate(max(tsel, m));
    const float3 f = coneSpill.rgb - coneSpill.rgb * t + t;
    E *= abs(coneSpill.w - 1234.5) < 0.25 ? f : 1;
    return float4(E, 0);
}
