/**
 * Vignette version 1.3
 * by Christian Cann Schuldt Jensen ~ CeeJay.dk
 *
 * Darkens the edges of the image to make it look more like it was shot with a camera lens.
 * May cause banding artifacts.
 */

// Ported to ps_3_0 for Apex Radiance (Looks) from https://github.com/CeeJayDK/SweetFX/blob/93ddf39b357f5da534ed6d34ba4ec8cc7dcfa361/Shaders/SweetFX/Vignette.fx
// Licence of the repository, which covers this file:
// The MIT License (MIT)
//
// Copyright (c) 2014 CeeJayDK
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

float Type : register(c8);
float Ratio : register(c9);
float Radius : register(c10);
float Amount : register(c11);
float Slope : register(c12);
float2 Center : register(c13);


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
		float distance = dot(distance_xy, distance_xy);

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
		float tex_xy = dot(float4(tex, tex), float4(-tex, 1.0, 1.0)); //dot is actually slower
		color.rgb = saturate(tex_xy * 4.0) * color.rgb;
	}

	return color;
}

// ---- ps_3_0 entry points (Looks) ----
float4 gs_VignettePass(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { float4 c = VignettePass(float4(vp + 0.5, 0.0, 1.0), uv); return float4(c.rgb, 1.0); }
