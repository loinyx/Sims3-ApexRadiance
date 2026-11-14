// Generated from Vignette.fx by tools/gen_gshade_shaders.py
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
 * Vignette version 1.3
 * by Christian Cann Schuldt Jensen ~ CeeJay.dk
 * License: MIT
 *
 * Darkens the edges of the image to make it look more like it was shot with a camera lens.
 * May cause banding artifacts.
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

float Type : register(c8);
float Ratio : register(c9);
float Radius : register(c10);
float Amount : register(c11);
float Slope : register(c12);
float2 Center : register(c13);


#if GSHADE_DITHER

#endif

float4 VignettePass(float4 vpos : SV_Position, float2 tex : TexCoord) : SV_Target
{
	float4 color = tex2D(BackBuffer, tex);

	if (Type == 0)
	{
		// Set the center
		float2 distance_xy = tex - Center;

		// Adjust the ratio
		distance_xy *= float2((BUFFER_RCP_HEIGHT / BUFFER_RCP_WIDTH), Ratio);

		// Calculate the distance
		distance_xy /= Radius;
		const float distance = dot(distance_xy, distance_xy);

		// Apply the vignette
		color.rgb *= (1.0 + pow(distance, Slope * 0.5) * Amount); //pow - multiply
	}

	if (Type == 1) // New round (-x*x+x) + (-y*y+y) method.
	{
		tex = -tex * tex + tex;
		color.rgb = saturate(((BUFFER_RCP_HEIGHT / BUFFER_RCP_WIDTH)*(BUFFER_RCP_HEIGHT / BUFFER_RCP_WIDTH) * Ratio * tex.x + tex.y) * 4.0) * color.rgb;
	}

	if (Type == 2) // New (-x*x+x) * (-y*y+y) TV style method.
	{
		tex = -tex * tex + tex;
		color.rgb = saturate(tex.x * tex.y * 100.0) * color.rgb;
	}
		
	if (Type == 3)
	{
		tex = abs(tex - 0.5);
		float tc = dot(float4(-tex.x, -tex.x, tex.x, tex.y), float4(tex.y, tex.y, 1.0, 1.0)); //XOR

		tc = saturate(tc - 0.495);
		color.rgb *= (pow((1.0 - tc * 200), 4) + 0.25); //or maybe abs(tc*100-1) (-(tc*100)-1)
	}
  
	if (Type == 4)
	{
		tex = abs(tex - 0.5);
		float tc = dot(float4(-tex.x, -tex.x, tex.x, tex.y), float4(tex.y, tex.y, 1.0, 1.0)); //XOR

		tc = saturate(tc - 0.495) - 0.0002;
		color.rgb *= (pow((1.0 - tc * 200), 4) + 0.0); //or maybe abs(tc*100-1) (-(tc*100)-1)
	}

	if (Type == 5) // MAD version of 2
	{
		tex = abs(tex - 0.5);
		float tc = tex.x * (-2.0 * tex.y + 1.0) + tex.y; //XOR

		tc = saturate(tc - 0.495);
		color.rgb *= (pow((-tc * 200 + 1.0), 4) + 0.25); //or maybe abs(tc*100-1) (-(tc*100)-1)
		//color.rgb *= (pow(((tc*200.0)-1.0),4)); //or maybe abs(tc*100-1) (-(tc*100)-1)
	}

	if (Type == 6) // New round (-x*x+x) * (-y*y+y) method.
	{
		//tex.y /= float2((BUFFER_RCP_HEIGHT / BUFFER_RCP_WIDTH), Ratio);
		const float tex_xy = dot(float4(tex, tex), float4(-tex, 1.0, 1.0)); //dot is actually slower
		color.rgb = saturate(tex_xy * 4.0) * color.rgb;
	}
#if GSHADE_DITHER
	return color += TriDither(color.rgb, tex, BUFFER_COLOR_BIT_DEPTH);
#else
	return color;
#endif
}

// ---- ps_3_0 entry points (GShade looks) ----
float4 gs_VignettePass(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { float4 c = VignettePass(float4(vp + 0.5, 0.0, 1.0), uv); return float4(c.rgb, 1.0); }
