// Generated from CAS.fx by tools/gen_gshade_shaders.py
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
// LICENSE
// =======
// Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All rights reserved.
// -------
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// -------
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software.
// -------
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE

//Initial port to ReShade: SLSNe	https://gist.github.com/SLSNe/bbaf2d77db0b2a2a0755df581b3cf00c

//Optimizations by Marty McFly:
//	vectorized math, even with scalar gcn hardware this should work
//	out the same, order of operations has not changed
//	For some reason, it went from 64 to 48 instructions, a lot of MOV gone
//	Also modified the way the final window is calculated
//	  
//	reordered min() and max() operations, from 11 down to 9 registers	
//
//	restructured final weighting, 49 -> 48 instructions
//
//     delayed RCP to replace SQRT with RSQRT
//
//	removed the saturate() from the control var as it is clamped
//	by UI manager already, 48 -> 47 instructions
//
//	replaced tex2D with tex2Doffset intrinsic (address offset by immediate integer)
//	47 -> 43 instructions
//	9 -> 8 registers

//Further modified by OopyDoopy and Lord of Lunacy:
//	Changed wording in the UI for the existing variable and added a new variable and relevant code to adjust sharpening strength.

//Fix by Lord of Lunacy:
//	Made the shader use a linear colorspace rather than sRGB, as recommended by the original AMD documentation from FidelityFX.

//Modified by CeeJay.dk:
//	Included a label and tooltip description. I followed AMDs official naming guidelines for FidelityFX.
//
//	Used gather trick to reduce the number of texture operations by one (9 -> 8). It's now 42 -> 51 instructions but still faster
//	because of the texture operation that was optimized away.

//Fix by CeeJay.dk
//	Fixed precision issues with the gather at super high resolutions
//	Also tried to refactor the samples so more work can be done while they are being sampled, but it's not so easy and the gains
//	I'm seeing are so small they might be statistical noise. So it MIGHT be faster - no promises.

//Fix by CeeJay.dk
//	Gather optimization is only faster on DX11 and up - Not DX10 and up. Correcting this so DX10 does not use the Gather codepath

float Contrast : register(c8);

float Sharpening : register(c9);


#define pixel float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT)

texture TexCASColor : COLOR;


float3 CASPass(float4 vpos : SV_Position, float2 texcoord : TexCoord) : SV_Target
{	
	// fetch a 3x3 neighborhood around the pixel 'e',
	//  a b c
	//  d(e)f
	//  g h i
	

	const float3 b = tex2Doffset(BackBuffer, texcoord, int2(0, -1)).rgb;
	const float3 d = tex2Doffset(BackBuffer, texcoord, int2(-1, 0)).rgb;
	
 
#if __RENDERER__ >= 0xb000 // If DX11 or higher
	const float4 red_efhi = tex2DgatherR(BackBuffer, texcoord + 0.5 * pixel);
	const float4 green_efhi = tex2DgatherG(BackBuffer, texcoord + 0.5 * pixel);
	const float4 blue_efhi = tex2DgatherB(BackBuffer, texcoord + 0.5 * pixel);

	const float3 e = float3( red_efhi.w, green_efhi.w, blue_efhi.w);
	const float3 f = float3( red_efhi.z, green_efhi.z, blue_efhi.z);
	const float3 h = float3( red_efhi.x, green_efhi.x, blue_efhi.x);
	const float3 i = float3( red_efhi.y, green_efhi.y, blue_efhi.y);


#else // If DX9
	const float3 e = tex2D(BackBuffer, texcoord).rgb;
	const float3 f = tex2Doffset(BackBuffer, texcoord, int2(1, 0)).rgb;

	const float3 h = tex2Doffset(BackBuffer, texcoord, int2(0, 1)).rgb;
	const float3 i = tex2Doffset(BackBuffer, texcoord, int2(1, 1)).rgb;

#endif

	const float3 g = tex2Doffset(BackBuffer, texcoord, int2(-1, 1)).rgb; 
	const float3 a = tex2Doffset(BackBuffer, texcoord, int2(-1, -1)).rgb;
	const float3 c = tex2Doffset(BackBuffer, texcoord, int2(1, -1)).rgb;
   

	// Soft min and max.
	//  a b c			 b
	//  d e f * 0.5  +  d e f * 0.5
	//  g h i			 h
	// These are 2.0x bigger (factored out the extra multiply).
	float3 mnRGB = min(min(min(d, e), min(f, b)), h);
	mnRGB += min(mnRGB, min(min(a, c), min(g, i)));

	float3 mxRGB = max(max(max(d, e), max(f, b)), h);
	mxRGB += max(mxRGB, max(max(a, c), max(g, i)));

	// Smooth minimum distance to signal limit divided by smooth max.
	// Shaping amount of sharpening.
	const float3 wRGB = -rcp(rsqrt(saturate(min(mnRGB, 2.0 - mxRGB) * rcp(mxRGB))) * (-3.0 * Contrast + 8.0));

	//						  0 w 0
	//  Filter shape:		   w 1 w
	//						  0 w 0  
	return lerp(e, saturate((((b + d) + (f + h)) * wRGB + e) * rcp(4.0 * wRGB + 1.0)), Sharpening);
}

// ---- ps_3_0 entry points (GShade looks) ----
float4 gs_CASPass(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { return float4(CASPass(float4(vp + 0.5, 0.0, 1.0), uv), 1.0); }
