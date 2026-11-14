// Generated from Colourfulness.fx by tools/gen_gshade_shaders.py
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
// Copyright (c) 2016-2018, bacondither
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer
//    in this position and unchanged.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
// OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
// IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
// NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
// THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Colourfulness - version 2019-01-22 - Modified by Marot for ReShade 4.0 compatibility and optimized slightly for the GShade project.
// EXPECTS FULL RANGE GAMMA LIGHT

float colourfulness : register(c8);

float lim_luma : register(c9);

float enable_dither : register(c10);

float col_noise : register(c11);

float backbuffer_bits : register(c12);

//-------------------------------------------------------------------------------------------------
#ifndef fast_luma
	#define fast_luma 1 // Rapid approx of sRGB gamma, small difference in quality
#endif

#ifndef temporal_dither
	#define temporal_dither 0 // Dither changes with every frame
#endif
//-------------------------------------------------------------------------------------------------



#if GSHADE_DITHER

#endif

#if (temporal_dither == 1)
	float rnd : register(c13);
#endif

// Sigmoid function, sign(v)*pow(pow(abs(v), -2) + pow(s, -2), 1.0/-2)
#define soft_lim(v,s)  ( (v*s)*rcp(sqrt(s*s + v*v)) )

// Weighted power mean, p = 0.5
#define wpmean(a,b,w)  ( ((abs(w)*sqrt(abs(a)) + abs(1-w)*sqrt(abs(b)))*2) )

// Max/Min RGB components
#define maxRGB(c)      ( max((c).r, max((c).g, (c).b)) )
#define minRGB(c)      ( min((c).r, min((c).g, (c).b)) )

// Mean of Rec. 709 & 601 luma coefficients
#define lumacoeff        float3(0.2558, 0.6511, 0.0931)

float3 Colourfulness(float4 vpos : SV_Position, float2 tex : TEXCOORD) : SV_Target
{
	#if (fast_luma == 1)
		float3 c0  = tex2D(BackBuffer, tex).rgb;
		const float luma = sqrt(dot(saturate(c0*abs(c0)), lumacoeff));
		c0 = saturate(c0);
	#else // Better approx of sRGB gamma
		const float3 c0  = saturate(tex2D(BackBuffer, tex).rgb);
		const float luma = pow(dot(pow(c0 + 0.06, 2.4), lumacoeff), 1.0/2.4) - 0.06;
	#endif

	// Calc colour saturation change
	const float3 diff_luma = c0 - luma;
	float3 c_diff = diff_luma*(colourfulness + 1) - diff_luma;

	if (colourfulness > 0.0)
	{
		// 120% of c_diff clamped to max visible range + overshoot
		const float3 rlc_diff = clamp((c_diff*1.2) + c0, -0.0001, 1.0001) - c0;

		// Calc max saturation-increase without altering RGB ratios
		const float poslim = (1.0002 - luma)/(abs(maxRGB(diff_luma)) + 0.0001);
		const float neglim = (luma + 0.0002)/(abs(minRGB(diff_luma)) + 0.0001);

		const float3 diffmax = diff_luma*min(min(poslim, neglim), 32) - diff_luma;

		// Soft limit diff
		c_diff = soft_lim( c_diff, max(wpmean(diffmax, rlc_diff, lim_luma), 1e-6) );
	}

	if (enable_dither == true)
	{
		// Interleaved gradient noise by Jorge Jimenez
		const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
		#if (temporal_dither == 1)
			const float xy_magic = (vpos.x + rnd)*magic.x + (vpos.y + rnd)*magic.y;
		#else
			const float xy_magic = vpos.x*magic.x + vpos.y*magic.y;
		#endif
		const float noise = (frac(magic.z*frac(xy_magic)) - 0.5)/(exp2(backbuffer_bits) - 1);
		if (col_noise == true)
			c_diff += float3(-noise, noise, -noise);
		else
			c_diff += noise;
	}

#if GSHADE_DITHER
    const float3 outcolor = saturate(c0 + c_diff);
	return outcolor + TriDither(outcolor, tex, BUFFER_COLOR_BIT_DEPTH);
#else
	return saturate(c0 + c_diff);
#endif
}

// ---- ps_3_0 entry points (GShade looks) ----
float4 gs_Colourfulness(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { return float4(Colourfulness(float4(vp + 0.5, 0.0, 1.0), uv), 1.0); }
