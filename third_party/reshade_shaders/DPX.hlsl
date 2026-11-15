/**
 * DPX/Cineon shader by Loadus
 */

// Ported to ps_3_0 for Apex Radiance (Looks) from https://github.com/CeeJayDK/SweetFX/blob/93ddf39b357f5da534ed6d34ba4ec8cc7dcfa361/Shaders/SweetFX/DPX.fx
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

float3 RGB_Curve : register(c8);
float3 RGB_C : register(c9);

float Contrast : register(c10);
float Saturation : register(c11);
float Colorfulness : register(c12);

float Strength : register(c13);


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
	float3 Btemp = (1.0 / (1.0 + exp(RGB_Curve / 2.0)));
	B = ((1.0 / (1.0 + exp(-RGB_Curve * (B - RGB_C)))) / (-2.0 * Btemp + 1.0)) + (-Btemp / (-2.0 * Btemp + 1.0));

	float value = max(max(B.r, B.g), B.b);
	float3 color = B / value;
	color = pow(abs(color), 1.0 / Colorfulness);

	float3 c0 = color * value;
	c0 = mul(XYZ, c0);
	float luma = dot(c0, float3(0.30, 0.59, 0.11));
	c0 = (1.0 - Saturation) * luma + Saturation * c0;
	c0 = mul(RGB, c0);

	return lerp(input, c0, Strength);
}

// ---- ps_3_0 entry points (Looks) ----
float4 gs_DPXPass(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { return float4(DPXPass(float4(vp + 0.5, 0.0, 1.0), uv), 1.0); }
