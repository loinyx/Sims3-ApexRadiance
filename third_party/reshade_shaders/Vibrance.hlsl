// Generated from Vibrance.fx by tools/gen_gshade_shaders.py
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
  Vibrance
  by Christian Cann Schuldt Jensen ~ CeeJay.dk
  License: MIT
 
  Vibrance intelligently boosts the saturation of pixels so pixels that had little color get a larger boost than pixels that had a lot.
  This avoids oversaturation of pixels that were already very saturated.

  History:

  Version 1.0 by Ceejay.dk
  - Original 
  Version 1.1 by CeeJay.dk
  - Introduced RBG balance to help colorblind users
  Version 1.1.1
  - Minor UI improvements for Reshade 3.x
  Version 1.1.2
  - Modified by Marot for ReShade 4.0 compatibility and lightly optimized for GShade.

  The MIT License (MIT)

  Copyright (c) 2014 CeeJayDK

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
 */

float Vibrance : register(c8);

float3 VibranceRGBBalance : register(c9);

/*
float Vibrance_Luma : register(c10);
*/



#if GSHADE_DITHER

#endif

float3 VibrancePass(float4 position : SV_Position, float2 texcoord : TexCoord) : SV_Target
{
	const float3 color = tex2D(BackBuffer, texcoord).rgb;

	const float luma = dot(float3(0.212656, 0.715158, 0.072186), color);

	// Extrapolate between luma and original by 1 + (1-saturation) - current
	const float3 coeffVibrance = float3(VibranceRGBBalance * Vibrance);

#if GSHADE_DITHER
	const float3 outcolor = lerp(luma, color, 1.0 + (coeffVibrance * (1.0 - (sign(coeffVibrance) * (max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b)))))));
	return outcolor + TriDither(outcolor, texcoord, BUFFER_COLOR_BIT_DEPTH);
#else
	return lerp(luma, color, 1.0 + (coeffVibrance * (1.0 - (sign(coeffVibrance) * (max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b)))))));
#endif
}

// ---- ps_3_0 entry points (GShade looks) ----
float4 gs_VibrancePass(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { return float4(VibrancePass(float4(vp + 0.5, 0.0, 1.0), uv), 1.0); }
