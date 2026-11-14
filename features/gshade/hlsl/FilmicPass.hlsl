// Generated from FilmicPass.fx by tools/gen_gshade_shaders.py
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
 * FilmicPass
 *
 * Applies some common color adjustments to mimic a more cinema-like look.
 */
 // Lightly optimized by Marot Satil for the GShade project.

float Strength : register(c8);

float Fade : register(c9);
float Contrast : register(c10);
float Linearization : register(c11);
float Bleach : register(c12);
float Saturation : register(c13);

float RedCurve : register(c14);
float GreenCurve : register(c15);
float BlueCurve : register(c16);
float BaseCurve : register(c17);

float BaseGamma : register(c18);
float EffectGamma : register(c19);
float EffectGammaR : register(c20);
float EffectGammaG : register(c21);
float EffectGammaB : register(c22);

float3 LumCoeff : register(c23);



#if GSHADE_DITHER

#endif

float3 FilmPass(float4 vpos : SV_Position, float2 texcoord : TexCoord) : SV_Target
{
	float3 B = tex2D(BackBuffer, texcoord).rgb;	

	B = saturate(B);
	B = pow(B, Linearization);
	B = lerp(0.01, B, Contrast);

	float3 D = dot(B.rgb, LumCoeff);

	B = pow(abs(B), 1.0 / BaseGamma);

	const float y = 1.0 / (1.0 + exp(RedCurve / 2.0));
	const float z = 1.0 / (1.0 + exp(GreenCurve / 2.0));
	const float w = 1.0 / (1.0 + exp(BlueCurve / 2.0));
	const float v = 1.0 / (1.0 + exp(BaseCurve / 2.0));

	float3 C = B;

	D.r = (1.0 / (1.0 + exp(-RedCurve * (D.r - 0.5))) - y) / (1.0 - 2.0 * y);
	D.g = (1.0 / (1.0 + exp(-GreenCurve * (D.g - 0.5))) - z) / (1.0 - 2.0 * z);
	D.b = (1.0 / (1.0 + exp(-BlueCurve * (D.b - 0.5))) - w) / (1.0 - 2.0 * w);

	D = pow(abs(D), 1.0 / EffectGamma);

	D = lerp(D, 1.0 - D, Bleach);

	D.r = pow(abs(D.r), 1.0 / EffectGammaR);
	D.g = pow(abs(D.g), 1.0 / EffectGammaG);
	D.b = pow(abs(D.b), 1.0 / EffectGammaB);

	if (D.r < 0.5)
		C.r = (2.0 * D.r - 1.0) * (B.r - B.r * B.r) + B.r;
	else
		C.r = (2.0 * D.r - 1.0) * (sqrt(B.r) - B.r) + B.r;

	if (D.g < 0.5)
		C.g = (2.0 * D.g - 1.0) * (B.g - B.g * B.g) + B.g;
	else
		C.g = (2.0 * D.g - 1.0) * (sqrt(B.g) - B.g) + B.g;

	if (D.b < 0.5)
		C.b = (2.0 * D.b - 1.0) * (B.b - B.b * B.b) + B.b;
	else
		C.b = (2.0 * D.b - 1.0) * (sqrt(B.b) - B.b) + B.b;

	float3 F = lerp(B, C, Strength);

	F = (1.0 / (1.0 + exp(-BaseCurve * (F - 0.5))) - v) / (1.0 - 2.0 * v);

	const float r2R = 1.0 - Saturation;
	const float g2R = 0.0 + Saturation;
	const float b2R = 0.0 + Saturation;

	const float r2G = 0.0 + Saturation;
	const float g2G = (1.0 - Fade) - Saturation;
	const float b2G = (0.0 + Fade) + Saturation;

	const float r2B = 0.0 + Saturation;
	const float g2B = (0.0 + Fade) + Saturation;
	const float b2B = (1.0 - Fade) - Saturation;

	float3 iF = F;

	F.r = (iF.r * r2R + iF.g * g2R + iF.b * b2R);
	F.g = (iF.r * r2G + iF.g * g2G + iF.b * b2G);
	F.b = (iF.r * r2B + iF.g * g2B + iF.b * b2B);

	const float N = dot(F.rgb, LumCoeff);
	float3 Cn;

	if (N < 0.5)
		Cn = (2.0 * N - 1.0) * (F - F * F) + F;
	else
		Cn = (2.0 * N - 1.0) * (sqrt(F) - F) + F;

	Cn = pow(max(Cn,0), 1.0 / Linearization);

#if GSHADE_DITHER
	Cn = lerp(B, Cn, Strength);
	return Cn + TriDither(Cn, texcoord, BUFFER_COLOR_BIT_DEPTH);
#else
	return lerp(B, Cn, Strength);
#endif
}

// ---- ps_3_0 entry points (GShade looks) ----
float4 gs_FilmPass(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { return float4(FilmPass(float4(vp + 0.5, 0.0, 1.0), uv), 1.0); }
