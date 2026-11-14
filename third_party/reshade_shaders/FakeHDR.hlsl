/**
 * HDR
 * by Christian Cann Schuldt Jensen ~ CeeJay.dk
 *
 * Not actual HDR - It just tries to mimic an HDR look (relatively high performance cost)
 */

// Ported to ps_3_0 for Apex Radiance (Looks) from https://github.com/CeeJayDK/SweetFX/blob/93ddf39b357f5da534ed6d34ba4ec8cc7dcfa361/Shaders/SweetFX/FakeHDR.fx
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

float HDRPower : register(c8);
float radius1 : register(c9);
float radius2 : register(c10);


float3 HDRPass(float4 vpos : SV_Position, float2 texcoord : TexCoord) : SV_Target
{
	float3 color = tex2D(BackBuffer, texcoord).rgb;

	float3 bloom_sum1 = tex2D(BackBuffer, texcoord + float2(1.5, -1.5) * radius1 * BUFFER_PIXEL_SIZE).rgb;
	bloom_sum1 += tex2D(BackBuffer, texcoord + float2(-1.5, -1.5) * radius1 * BUFFER_PIXEL_SIZE).rgb;
	bloom_sum1 += tex2D(BackBuffer, texcoord + float2( 1.5,  1.5) * radius1 * BUFFER_PIXEL_SIZE).rgb;
	bloom_sum1 += tex2D(BackBuffer, texcoord + float2(-1.5,  1.5) * radius1 * BUFFER_PIXEL_SIZE).rgb;
	bloom_sum1 += tex2D(BackBuffer, texcoord + float2( 0.0, -2.5) * radius1 * BUFFER_PIXEL_SIZE).rgb;
	bloom_sum1 += tex2D(BackBuffer, texcoord + float2( 0.0,  2.5) * radius1 * BUFFER_PIXEL_SIZE).rgb;
	bloom_sum1 += tex2D(BackBuffer, texcoord + float2(-2.5,  0.0) * radius1 * BUFFER_PIXEL_SIZE).rgb;
	bloom_sum1 += tex2D(BackBuffer, texcoord + float2( 2.5,  0.0) * radius1 * BUFFER_PIXEL_SIZE).rgb;

	bloom_sum1 *= 0.005;

	float3 bloom_sum2 = tex2D(BackBuffer, texcoord + float2(1.5, -1.5) * radius2 * BUFFER_PIXEL_SIZE).rgb;
	bloom_sum2 += tex2D(BackBuffer, texcoord + float2(-1.5, -1.5) * radius2 * BUFFER_PIXEL_SIZE).rgb;
	bloom_sum2 += tex2D(BackBuffer, texcoord + float2( 1.5,  1.5) * radius2 * BUFFER_PIXEL_SIZE).rgb;
	bloom_sum2 += tex2D(BackBuffer, texcoord + float2(-1.5,  1.5) * radius2 * BUFFER_PIXEL_SIZE).rgb;
	bloom_sum2 += tex2D(BackBuffer, texcoord + float2( 0.0, -2.5) * radius2 * BUFFER_PIXEL_SIZE).rgb;	
	bloom_sum2 += tex2D(BackBuffer, texcoord + float2( 0.0,  2.5) * radius2 * BUFFER_PIXEL_SIZE).rgb;
	bloom_sum2 += tex2D(BackBuffer, texcoord + float2(-2.5,  0.0) * radius2 * BUFFER_PIXEL_SIZE).rgb;
	bloom_sum2 += tex2D(BackBuffer, texcoord + float2( 2.5,  0.0) * radius2 * BUFFER_PIXEL_SIZE).rgb;

	bloom_sum2 *= 0.010;

	float dist = radius2 - radius1;
	float3 HDR = (color + (bloom_sum2 - bloom_sum1)) * dist;
	float3 blend = HDR + color;
	color = pow(abs(blend), abs(HDRPower)) + HDR; // pow - don't use fractions for HDRpower
	
	return saturate(color);
}

// ---- ps_3_0 entry points (Looks) ----
float4 gs_HDRPass(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { return float4(HDRPass(float4(vp + 0.5, 0.0, 1.0), uv), 1.0); }
