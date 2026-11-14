/**
 * Tonemap version 1.1
 * by Christian Cann Schuldt Jensen ~ CeeJay.dk
 */

// Ported to ps_3_0 for Apex Radiance (Looks) from https://github.com/CeeJayDK/SweetFX/blob/93ddf39b357f5da534ed6d34ba4ec8cc7dcfa361/Shaders/SweetFX/Tonemap.fx
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

float Gamma : register(c8);
float Exposure : register(c9);
float Saturation : register(c10);

float Bleach : register(c11);

float Defog : register(c12);
float3 FogColor : register(c13);



float3 TonemapPass(float4 position : SV_Position, float2 texcoord : TexCoord) : SV_Target
{
	float3 color = tex2D(BackBuffer, texcoord).rgb;
	color = saturate(color - Defog * FogColor * 2.55); // Defog
	color *= pow(2.0f, Exposure); // Exposure
	color = pow(color, Gamma); // Gamma

	const float3 coefLuma = float3(0.2126, 0.7152, 0.0722);
	float lum = dot(coefLuma, color);
	
	float L = saturate(10.0 * (lum - 0.45));
	float3 A2 = Bleach * color;

	float3 result1 = 2.0f * color * lum;
	float3 result2 = 1.0f - 2.0f * (1.0f - lum) * (1.0f - color);
	
	float3 newColor = lerp(result1, result2, L);
	float3 mixRGB = A2 * newColor;
	color += ((1.0f - A2) * mixRGB);
	
	float3 middlegray = dot(color, (1.0 / 3.0));
	float3 diffcolor = color - middlegray;
	color = (color + diffcolor * Saturation) / (1 + (diffcolor * Saturation)); // Saturation
	
	return color;
}

// ---- ps_3_0 entry points (Looks) ----
float4 gs_TonemapPass(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { return float4(TonemapPass(float4(vp + 0.5, 0.0, 1.0), uv), 1.0); }
