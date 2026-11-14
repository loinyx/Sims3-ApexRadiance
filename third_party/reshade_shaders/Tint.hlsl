// Ported to ps_3_0 for Apex Radiance (Looks) from https://github.com/CeeJayDK/SweetFX/blob/93ddf39b357f5da534ed6d34ba4ec8cc7dcfa361/Shaders/SweetFX/Sepia.fx
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

float3 Tint : register(c8);

float Strength : register(c9);


float3 TintPass(float4 vois : SV_Position, float2 texcoord : TexCoord) : SV_Target
{
	float3 col = tex2D(BackBuffer, texcoord).rgb;

	return lerp(col, col * Tint * 2.55, Strength);
}

// ---- ps_3_0 entry points (Looks) ----
float4 gs_TintPass(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { return float4(TintPass(float4(vp + 0.5, 0.0, 1.0), uv), 1.0); }
