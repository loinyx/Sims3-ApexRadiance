// Generated from DPX.fx by tools/gen_gshade_shaders.py
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
 * DPX/Cineon shader by Loadus
 * Lightly optimized by Marot Satil for the GShade project.
 */

float3 RGB_Curve : register(c8);
float3 RGB_C : register(c9);

float Contrast : register(c10);
float Saturation : register(c11);
float Colorfulness : register(c12);

float Strength : register(c13);



#if GSHADE_DITHER

#endif

static const float3x3 RGB = float3x3(
	 2.6714711726599600, -1.2672360578624100, -0.4109956021722270,
	-1.0251070293466400,  1.9840911624108900,  0.0439502493584124,
	 0.0610009456429445, -0.2236707508128630,  1.1590210416706100
);
static const float3x3 XYZ = float3x3(
	 0.5003033835433160,  0.3380975732227390,  0.1645897795458570,
	 0.2579688942747580,  0.6761952591447060,  0.0658358459823868,
	 0.0234517888692628,  0.1126992737203000,  0.8668396731242010
);

float3 DPXPass(float4 vois : SV_Position, float2 texcoord : TexCoord) : SV_Target
{
	float3 input = tex2D(BackBuffer, texcoord).rgb;

	float3 B = input;
	B = B * (1.0 - Contrast) + (0.5 * Contrast);
	const float3 Btemp = (1.0 / (1.0 + exp(RGB_Curve / 2.0)));
	B = ((1.0 / (1.0 + exp(-RGB_Curve * (B - RGB_C)))) / (-2.0 * Btemp + 1.0)) + (-Btemp / (-2.0 * Btemp + 1.0));

	const float value = max(max(B.r, B.g), B.b);
	float3 color = B / value;
	color = pow(abs(color), 1.0 / Colorfulness);

	float3 c0 = color * value;
	c0 = mul(XYZ, c0);
	const float luma = dot(c0, float3(0.30, 0.59, 0.11));
	c0 = (1.0 - Saturation) * luma + Saturation * c0;
	c0 = mul(RGB, c0);

#if GSHADE_DITHER
	c0 = lerp(input, c0, Strength);
	return c0 + TriDither(c0, texcoord, BUFFER_COLOR_BIT_DEPTH);
#else
	return lerp(input, c0, Strength);
#endif
}

// ---- ps_3_0 entry points (GShade looks) ----
float4 gs_DPXPass(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { return float4(DPXPass(float4(vp + 0.5, 0.0, 1.0), uv), 1.0); }
