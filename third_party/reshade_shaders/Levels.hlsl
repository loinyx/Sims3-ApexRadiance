// Generated from Levels.fx by tools/gen_gshade_shaders.py
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
 * Levels version 1.2
 * by Christian Cann Schuldt Jensen ~ CeeJay.dk
 * License: MIT
 *
 * Allows you to set a new black and a white level.
 * This increases contrast, but clips any colors outside the new range to either black or white
 * and so some details in the shadows or highlights can be lost.
 *
 * The shader is very useful for expanding the 16-235 TV range to 0-255 PC range.
 * You might need it if you're playing a game meant to display on a TV with an emulator that does not do this.
 * But it's also a quick and easy way to uniformly increase the contrast of an image.
 *
 * -- Version 1.0 --
 * First release
 * -- Version 1.1 --
 * Optimized to only use 1 instruction (down from 2 - a 100% performance increase :) )
 * -- Version 1.2 --
 * Added the ability to highlight clipping regions of the image with #define HighlightClipping 1
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
 // Lightly optimized by Marot Satil for the GShade project.

float BlackPoint : register(c8);

float WhitePoint : register(c9);

float HighlightClipping : register(c10);



#if GSHADE_DITHER

#endif

float3 LevelsPass(float4 vpos : SV_Position, float2 texcoord : TexCoord) : SV_Target
{
	const float black_point_float = BlackPoint / 255.0;

	float white_point_float;
	// Avoid division by zero if the white and black point are the same
	if (WhitePoint == BlackPoint)
		white_point_float = (255.0 / 0.00025);
	else
		white_point_float = 255.0 / (WhitePoint - BlackPoint);

	float3 color = tex2D(BackBuffer, texcoord).rgb;
	color = color * white_point_float - (black_point_float *  white_point_float);

	if (HighlightClipping)
	{
		float3 clipped_colors;

		// any colors whiter than white?
		if (any(color > saturate(color)))
			clipped_colors = float3(1.0, 0.0, 0.0);
		else
			clipped_colors = color;

		// all colors whiter than white?
		if (all(color > saturate(color)))
			clipped_colors = float3(1.0, 1.0, 0.0);

		// any colors blacker than black?
		if (any(color < saturate(color)))
			clipped_colors = float3(0.0, 0.0, 1.0);

		// all colors blacker than black?
		if (all(color < saturate(color)))
			clipped_colors = float3(0.0, 1.0, 1.0);

		color = clipped_colors;
	}

#if GSHADE_DITHER
	return color + TriDither(color, texcoord, BUFFER_COLOR_BIT_DEPTH);
#else
	return color;
#endif
}

// ---- ps_3_0 entry points (GShade looks) ----
float4 gs_LevelsPass(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { return float4(LevelsPass(float4(vp + 0.5, 0.0, 1.0), uv), 1.0); }
