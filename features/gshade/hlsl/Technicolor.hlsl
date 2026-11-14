// Generated from Technicolor.fx by tools/gen_gshade_shaders.py
// GShade looks: ReShade -> ps_3_0 compatibility layer. Prepended to every ported shader.
// Only declarations live here; the shaders' maths stay as written in the GShade .fx files.
#define GSHADE_DITHER 0 // GShade.ini UseDithering=0
#define __RENDERER__ 0x9000 // D3D9, as GShade sees TS3: shaders take their D3D9 branches
#define __RESHADE__ 60703   // GShade 5.2.4 is based on ReShade 6.7.3
float4 gs_buf : register(c0);       // 1/W, 1/H, W, H of the back buffer
float4 gs_aux : register(c1);       // pass-specific
float4 gs_depth : register(c2);     // x = far plane (ReShade RESHADE_DEPTH_LINEARIZATION_FAR_PLANE), y = reversed
#define BUFFER_RCP_WIDTH   gs_buf.x
#define BUFFER_RCP_HEIGHT  gs_buf.y
#define BUFFER_WIDTH       gs_buf.z
#define BUFFER_HEIGHT      gs_buf.w
#define BUFFER_PIXEL_SIZE  gs_buf.xy
#define BUFFER_SCREEN_SIZE gs_buf.zw
#define BUFFER_COLOR_BIT_DEPTH 8
#define rcp(x) (1.0 / (x))
// ReShade's D3D9 code generator emulates texel offsets this way (ps_3_0 has no offset overload)
#define tex2Doffset(s, c, o) tex2D(s, (c) + (o) * BUFFER_PIXEL_SIZE)
#define tex2Dlodoffset(s, c, o) tex2Dlod(s, (c) + float4((o) * BUFFER_PIXEL_SIZE, 0.0, 0.0))
sampler2D BackBuffer : register(s0);
/**
 * Technicolor version 1.1
 * Original by DKT70
 * Optimized by CeeJay.dk
 */
 // Lightly optimized by Marot Satil for the GShade project.

float Power : register(c8);
float3 RGBNegativeAmount : register(c9);

float Strength : register(c10);



#if GSHADE_DITHER

#endif

float3 TechnicolorPass(float4 vpos : SV_Position, float2 texcoord : TexCoord) : SV_Target
{
	const float3 cyanfilter = float3(0.0, 1.30, 1.0);
	const float3 magentafilter = float3(1.0, 0.0, 1.05);
	const float3 yellowfilter = float3(1.6, 1.6, 0.05);
	const float2 redorangefilter = float2(1.05, 0.620); // RG_
	const float2 greenfilter = float2(0.30, 1.0);       // RG_
	const float2 magentafilter2 = magentafilter.rb;     // R_B

	const float3 tcol = tex2D(BackBuffer, texcoord).rgb;
	
	const float2 negative_mul_r = tcol.rg * (1.0 / (RGBNegativeAmount.r * Power));
	const float2 negative_mul_g = tcol.rg * (1.0 / (RGBNegativeAmount.g * Power));
	const float2 negative_mul_b = tcol.rb * (1.0 / (RGBNegativeAmount.b * Power));
	const float3 output_r = dot(redorangefilter, negative_mul_r).xxx + cyanfilter;
	const float3 output_g = dot(greenfilter, negative_mul_g).xxx + magentafilter;
	const float3 output_b = dot(magentafilter2, negative_mul_b).xxx + yellowfilter;

#if GSHADE_DITHER
	const float3 outcolor = lerp(tcol, output_r * output_g * output_b, Strength);
	return outcolor + TriDither(outcolor, texcoord, BUFFER_COLOR_BIT_DEPTH);
#else
	return lerp(tcol, output_r * output_g * output_b, Strength);
#endif
}

// ---- ps_3_0 entry points (GShade looks) ----
float4 gs_TechnicolorPass(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { return float4(TechnicolorPass(float4(vp + 0.5, 0.0, 1.0), uv), 1.0); }
