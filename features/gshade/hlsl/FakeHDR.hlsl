// Generated from FakeHDR.fx by tools/gen_gshade_shaders.py
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
 * HDR
 * by Christian Cann Schuldt Jensen ~ CeeJay.dk
 * License: MIT
 *
 * Not actual HDR - It just tries to mimic an HDR look (relatively high performance cost)
 * Lightly optimized by Marot Satil for the GShade project.
 *
 *
 * The MIT License (MIT)
 * 
 * Copyright (c) 2014 CeeJayDK
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

float fHDRPower : register(c8);
float fradius1 : register(c9);
float fradius2 : register(c10);



#if GSHADE_DITHER

#endif

// !!! modified - Craig - Jul 6th, 2020
float3 fHDRPass(float4 vpos : SV_Position, float2 texcoord : TexCoord) : SV_Target
{
	const float3 color = tex2D(BackBuffer, texcoord).rgb;
	
	// !!! pre-calc radius * BPS values
	const float2 rad1 = fradius1 * BUFFER_PIXEL_SIZE;
	const float2 rad2 = fradius2 * BUFFER_PIXEL_SIZE;

	// !!! updated to use new pre-calc'ed rad value
	const float3 bloom_sum1  = (
		tex2D(BackBuffer, texcoord + float2( 1.5, -1.5) * rad1).rgb +
		tex2D(BackBuffer, texcoord + float2(-1.5, -1.5) * rad1).rgb +
		tex2D(BackBuffer, texcoord + float2( 1.5,  1.5) * rad1).rgb +
		tex2D(BackBuffer, texcoord + float2(-1.5,  1.5) * rad1).rgb +
		tex2D(BackBuffer, texcoord + float2( 0.0, -2.5) * rad1).rgb +
		tex2D(BackBuffer, texcoord + float2( 0.0,  2.5) * rad1).rgb +
		tex2D(BackBuffer, texcoord + float2(-2.5,  0.0) * rad1).rgb +
		tex2D(BackBuffer, texcoord + float2( 2.5,  0.0) * rad1).rgb
		) * 0.005;

	// !!! updated to use new pre-calc'ed rad value
	const float3 bloom_sum2  = (
		tex2D(BackBuffer, texcoord + float2( 1.5, -1.5) * rad2).rgb +
		tex2D(BackBuffer, texcoord + float2(-1.5, -1.5) * rad2).rgb +
		tex2D(BackBuffer, texcoord + float2( 1.5,  1.5) * rad2).rgb +
		tex2D(BackBuffer, texcoord + float2(-1.5,  1.5) * rad2).rgb +
		tex2D(BackBuffer, texcoord + float2( 0.0, -2.5) * rad2).rgb +
		tex2D(BackBuffer, texcoord + float2( 0.0,  2.5) * rad2).rgb +
		tex2D(BackBuffer, texcoord + float2(-2.5,  0.0) * rad2).rgb +
		tex2D(BackBuffer, texcoord + float2( 2.5,  0.0) * rad2).rgb
		) * 0.01;

	const float3 HDR = (color + (bloom_sum2 - bloom_sum1)) * (fradius2 - fradius1);

#if GSHADE_DITHER
	const float3 outcolor = saturate(pow(abs(HDR + color), abs(fHDRPower)) + HDR); // pow - don't use fractions for HDRpower
	return outcolor + TriDither(outcolor, texcoord, BUFFER_COLOR_BIT_DEPTH);
#else
	return saturate(pow(abs(HDR + color), abs(fHDRPower)) + HDR); // pow - don't use fractions for HDRpower
#endif
}

// ---- ps_3_0 entry points (GShade looks) ----
float4 gs_fHDRPass(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { return float4(fHDRPass(float4(vp + 0.5, 0.0, 1.0), uv), 1.0); }
