// Generated from Clarity.fx by tools/gen_gshade_shaders.py
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
//Clarity by Ioxa
//Version 1.5 for ReShade 3.0
// Lightly optimized by Marot Satil for the GShade project.

//>Clarity Settings<
float ClarityRadius : register(c8);

float ClarityOffset : register(c9);

float ClarityBlendMode : register(c10);

float ClarityBlendIfDark : register(c11);

float ClarityBlendIfLight : register(c12);

float ClarityViewBlendIfMask : register(c13);

float ClarityStrength : register(c14);

float ClarityDarkIntensity : register(c15);

float ClarityLightIntensity : register(c16);

float ClarityViewMask : register(c17);







sampler2D ClaritySampler : register(s1);
sampler2D ClaritySampler2 : register(s2);
sampler2D ClaritySampler3 : register(s3);

float3 ClarityFinal(in float4 pos : SV_Position, in float2 texcoord : TEXCOORD) : COLOR
{

	float color = tex2D(ClaritySampler3, texcoord).r;
	
if(ClarityRadius == 0)	
{
	const float offset[4] = { 0.0, 1.1824255238, 3.0293122308, 5.0040701377 };
	const float weight[4] = { 0.39894, 0.2959599993, 0.0045656525, 0.00000149278686458842 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 4; ++i)
	{
		color += tex2Dlod(ClaritySampler3, float4(texcoord + float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r * weight[i];
		color += tex2Dlod(ClaritySampler3, float4(texcoord - float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r * weight[i];
	}
}	

if(ClarityRadius == 1)	
{
	const float offset[6] = { 0.0, 1.4584295168, 3.40398480678, 5.3518057801, 7.302940716, 9.2581597095 };
	const float weight[6] = { 0.13298, 0.23227575, 0.1353261595, 0.0511557427, 0.01253922, 0.0019913644 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 6; ++i)
	{
		color += tex2Dlod(ClaritySampler3, float4(texcoord + float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r * weight[i];
		color += tex2Dlod(ClaritySampler3, float4(texcoord - float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r * weight[i];
	}
}	

if(ClarityRadius == 2)	
{
	const float offset[11] = { 0.0, 1.4895848401, 3.4757135714, 5.4618796741, 7.4481042327, 9.4344079746, 11.420811147, 13.4073334, 15.3939936778, 17.3808101174, 19.3677999584 };
	const float weight[11] = { 0.06649, 0.1284697563, 0.111918249, 0.0873132676, 0.0610011113, 0.0381655709, 0.0213835661, 0.0107290241, 0.0048206869, 0.0019396469, 0.0006988718 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 11; ++i)
	{
		color += tex2Dlod(ClaritySampler3, float4(texcoord + float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r * weight[i];
		color += tex2Dlod(ClaritySampler3, float4(texcoord - float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r * weight[i];
	}
}	

if(ClarityRadius == 3)	
{
	const float offset[15] = { 0.0, 1.4953705027, 3.4891992113, 5.4830312105, 7.4768683759, 9.4707125766, 11.4645656736, 13.4584295168, 15.4523059431, 17.4461967743, 19.4401038149, 21.43402885, 23.4279736431, 25.4219399344, 27.4159294386 };
	const float weight[15] = { 0.0443266667, 0.0872994708, 0.0820892038, 0.0734818355, 0.0626171681, 0.0507956191, 0.0392263968, 0.0288369812, 0.0201808877, 0.0134446557, 0.0085266392, 0.0051478359, 0.0029586248, 0.0016187257, 0.0008430913 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 15; ++i)
	{
		color += tex2Dlod(ClaritySampler3, float4(texcoord + float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r * weight[i];
		color += tex2Dlod(ClaritySampler3, float4(texcoord - float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r * weight[i];
	}
}

if(ClarityRadius == 4)	
{
	const float offset[18] = { 0.0, 1.4953705027, 3.4891992113, 5.4830312105, 7.4768683759, 9.4707125766, 11.4645656736, 13.4584295168, 15.4523059431, 17.4461967743, 19.4661974725, 21.4627427973, 23.4592916956, 25.455844494, 27.4524015179, 29.4489630909, 31.445529535, 33.4421011704 };
	const float weight[18] = { 0.033245, 0.0659162217, 0.0636705814, 0.0598194658, 0.0546642566, 0.0485871646, 0.0420045997, 0.0353207015, 0.0288880982, 0.0229808311, 0.0177815511, 0.013382297, 0.0097960001, 0.0069746748, 0.0048301008, 0.0032534598, 0.0021315311, 0.0013582974 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 18; ++i)
	{
		color += tex2Dlod(ClaritySampler3, float4(texcoord + float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r * weight[i];
		color += tex2Dlod(ClaritySampler3, float4(texcoord - float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r * weight[i];
	}
}	
	
	float3 orig = tex2D(BackBuffer, texcoord).rgb; //Original Image
	float luma = dot(orig.rgb,float3(0.32786885,0.655737705,0.0163934436));
	float3 chroma = orig.rgb/luma;
	
	float sharp = 1-color;
	sharp = (luma+sharp)*0.5;
	
	float sharpMin = lerp(0.0,1.0,smoothstep(0.0,1.0,sharp));
	float sharpMax = sharpMin;
	sharpMin = lerp(sharp,sharpMin,ClarityDarkIntensity);
	sharpMax = lerp(sharp,sharpMax,ClarityLightIntensity);
	sharp = lerp(sharpMin,sharpMax,step(0.5,sharp));

	if(ClarityViewMask)
	{
		orig.rgb = sharp;
		luma = sharp;
		chroma = 1.0;
	}
	else
	{
		if(ClarityBlendMode == 0)
		{
			//softlight
			sharp = lerp(2*luma*sharp + luma*luma*(1.0-2*sharp), 2*luma*(1.0-sharp)+pow(luma,0.5)*(2*sharp-1.0), step(0.49,sharp));
		}
		
		if(ClarityBlendMode == 1)
		{
			//overlay
			sharp = lerp(2*luma*sharp, 1.0 - 2*(1.0-luma)*(1.0-sharp), step(0.50,luma));
		}
		
		if(ClarityBlendMode == 2)
		{
			//Hardlight
			sharp = lerp(2*luma*sharp, 1.0 - 2*(1.0-luma)*(1.0-sharp), step(0.50,sharp));
		}
		
		if(ClarityBlendMode == 3)
		{
			//Multiply
			sharp = saturate(2 * luma * sharp);
		}
		
		if(ClarityBlendMode == 4)
		{
			//vivid light
			sharp = lerp(2*luma*sharp, luma/(2*(1-sharp)), step(0.5,sharp));
		}
		
		if(ClarityBlendMode == 5)
		{
			//Linear Light
			sharp = luma + 2.0*sharp-1.0;
		}
		
		if(ClarityBlendMode == 6)
		{
			//Addition
			sharp = saturate(luma + (sharp - 0.5));
		}
	}
	
	if( ClarityBlendIfDark > 0 || ClarityBlendIfLight < 255 || ClarityViewBlendIfMask)
	{
		const float ClarityBlendIfD = (ClarityBlendIfDark/255.0)+0.0001;
		const float ClarityBlendIfL = (ClarityBlendIfLight/255.0)-0.0001;
		const float mix = dot(orig.rgb, 0.333333);
		float mask = 1.0;
		
		if(ClarityBlendIfDark > 0)
		{
			mask = lerp(0.0,1.0,smoothstep(ClarityBlendIfD-(ClarityBlendIfD*0.2),ClarityBlendIfD+(ClarityBlendIfD*0.2),mix));
		}
						
		if(ClarityBlendIfLight < 255)
		{
			mask = lerp(mask,0.0,smoothstep(ClarityBlendIfL-(ClarityBlendIfL*0.2),ClarityBlendIfL+(ClarityBlendIfL*0.2),mix));
		}
			
		sharp = lerp(luma,sharp,mask);
		
		if (ClarityViewBlendIfMask)
		{
			sharp = mask;
			luma = mask;
			chroma = 1.0;
		}
	}
					
	orig.rgb = lerp(luma, sharp, ClarityStrength);
	orig.rgb *= chroma;
		
	return saturate(orig);
}	

float Clarity1(in float4 pos : SV_Position, in float2 texcoord : TEXCOORD) : COLOR
{
	float3 color = tex2D(BackBuffer, texcoord).rgb;
	
if(ClarityRadius == 0)	
{
	const float offset[4] = { 0.0, 1.1824255238, 3.0293122308, 5.0040701377 };
	const float weight[4] = { 0.39894, 0.2959599993, 0.0045656525, 0.00000149278686458842 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 4; ++i)
	{
		color += tex2Dlod(BackBuffer, float4(texcoord + float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).rgb * weight[i];
		color += tex2Dlod(BackBuffer, float4(texcoord - float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).rgb * weight[i];
	}
}	

if(ClarityRadius == 1)	
{
	const float offset[6] = { 0.0, 1.4584295168, 3.40398480678, 5.3518057801, 7.302940716, 9.2581597095 };
	const float weight[6] = { 0.13298, 0.23227575, 0.1353261595, 0.0511557427, 0.01253922, 0.0019913644 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 6; ++i)
	{
		color += tex2Dlod(BackBuffer, float4(texcoord + float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).rgb * weight[i];
		color += tex2Dlod(BackBuffer, float4(texcoord - float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).rgb * weight[i];
	}
}	

if(ClarityRadius == 2)	
{
	const float offset[11] = { 0.0, 1.4895848401, 3.4757135714, 5.4618796741, 7.4481042327, 9.4344079746, 11.420811147, 13.4073334, 15.3939936778, 17.3808101174, 19.3677999584 };
	const float weight[11] = { 0.06649, 0.1284697563, 0.111918249, 0.0873132676, 0.0610011113, 0.0381655709, 0.0213835661, 0.0107290241, 0.0048206869, 0.0019396469, 0.0006988718 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 11; ++i)
	{
		color += tex2Dlod(BackBuffer, float4(texcoord + float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).rgb * weight[i];
		color += tex2Dlod(BackBuffer, float4(texcoord - float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).rgb * weight[i];
	}
}	

if(ClarityRadius == 3)	
{
	const float offset[15] = { 0.0, 1.4953705027, 3.4891992113, 5.4830312105, 7.4768683759, 9.4707125766, 11.4645656736, 13.4584295168, 15.4523059431, 17.4461967743, 19.4401038149, 21.43402885, 23.4279736431, 25.4219399344, 27.4159294386 };
	const float weight[15] = { 0.0443266667, 0.0872994708, 0.0820892038, 0.0734818355, 0.0626171681, 0.0507956191, 0.0392263968, 0.0288369812, 0.0201808877, 0.0134446557, 0.0085266392, 0.0051478359, 0.0029586248, 0.0016187257, 0.0008430913 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 15; ++i)
	{
		color += tex2Dlod(BackBuffer, float4(texcoord + float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).rgb * weight[i];
		color += tex2Dlod(BackBuffer, float4(texcoord - float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).rgb * weight[i];
	}
}	

if(ClarityRadius == 4)	
{
	const float offset[18] = { 0.0, 1.4953705027, 3.4891992113, 5.4830312105, 7.4768683759, 9.4707125766, 11.4645656736, 13.4584295168, 15.4523059431, 17.4461967743, 19.4661974725, 21.4627427973, 23.4592916956, 25.455844494, 27.4524015179, 29.4489630909, 31.445529535, 33.4421011704 };
	const float weight[18] = { 0.033245, 0.0659162217, 0.0636705814, 0.0598194658, 0.0546642566, 0.0485871646, 0.0420045997, 0.0353207015, 0.0288880982, 0.0229808311, 0.0177815511, 0.013382297, 0.0097960001, 0.0069746748, 0.0048301008, 0.0032534598, 0.0021315311, 0.0013582974 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 18; ++i)
	{
		color += tex2Dlod(BackBuffer, float4(texcoord + float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).rgb * weight[i];
		color += tex2Dlod(BackBuffer, float4(texcoord - float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).rgb * weight[i];
	}
}	
	
	return dot(color.rgb,float3(0.32786885,0.655737705,0.0163934436));
}

float Clarity2(in float4 pos : SV_Position, in float2 texcoord : TEXCOORD) : COLOR
{
	float color = tex2D(ClaritySampler, texcoord).r;
	
if(ClarityRadius == 0)	
{
	const float offset[4] = { 0.0, 1.1824255238, 3.0293122308, 5.0040701377 };
	const float weight[4] = { 0.39894, 0.2959599993, 0.0045656525, 0.00000149278686458842 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 4; ++i)
	{
		color += tex2Dlod(ClaritySampler, float4(texcoord + float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r* weight[i];
		color += tex2Dlod(ClaritySampler, float4(texcoord - float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r* weight[i];
	}
}	

if(ClarityRadius == 1)	
{
	const float offset[6] = { 0.0, 1.4584295168, 3.40398480678, 5.3518057801, 7.302940716, 9.2581597095 };
	const float weight[6] = { 0.13298, 0.23227575, 0.1353261595, 0.0511557427, 0.01253922, 0.0019913644 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 6; ++i)
	{
		color += tex2Dlod(ClaritySampler, float4(texcoord + float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r* weight[i];
		color += tex2Dlod(ClaritySampler, float4(texcoord - float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r* weight[i];
	}
}	

if(ClarityRadius == 2)	
{
	const float offset[11] = { 0.0, 1.4895848401, 3.4757135714, 5.4618796741, 7.4481042327, 9.4344079746, 11.420811147, 13.4073334, 15.3939936778, 17.3808101174, 19.3677999584 };
	const float weight[11] = { 0.06649, 0.1284697563, 0.111918249, 0.0873132676, 0.0610011113, 0.0381655709, 0.0213835661, 0.0107290241, 0.0048206869, 0.0019396469, 0.0006988718 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 11; ++i)
	{
		color += tex2Dlod(ClaritySampler, float4(texcoord + float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r* weight[i];
		color += tex2Dlod(ClaritySampler, float4(texcoord - float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r* weight[i];
	}
}	

if(ClarityRadius == 3)	
{
	const float offset[15] = { 0.0, 1.4953705027, 3.4891992113, 5.4830312105, 7.4768683759, 9.4707125766, 11.4645656736, 13.4584295168, 15.4523059431, 17.4461967743, 19.4401038149, 21.43402885, 23.4279736431, 25.4219399344, 27.4159294386 };
	const float weight[15] = { 0.0443266667, 0.0872994708, 0.0820892038, 0.0734818355, 0.0626171681, 0.0507956191, 0.0392263968, 0.0288369812, 0.0201808877, 0.0134446557, 0.0085266392, 0.0051478359, 0.0029586248, 0.0016187257, 0.0008430913 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 15; ++i)
	{
		color += tex2Dlod(ClaritySampler, float4(texcoord + float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r* weight[i];
		color += tex2Dlod(ClaritySampler, float4(texcoord - float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r* weight[i];
	}
}

if(ClarityRadius == 4)	
{
	const float offset[18] = { 0.0, 1.4953705027, 3.4891992113, 5.4830312105, 7.4768683759, 9.4707125766, 11.4645656736, 13.4584295168, 15.4523059431, 17.4461967743, 19.4661974725, 21.4627427973, 23.4592916956, 25.455844494, 27.4524015179, 29.4489630909, 31.445529535, 33.4421011704 };
	const float weight[18] = { 0.033245, 0.0659162217, 0.0636705814, 0.0598194658, 0.0546642566, 0.0485871646, 0.0420045997, 0.0353207015, 0.0288880982, 0.0229808311, 0.0177815511, 0.013382297, 0.0097960001, 0.0069746748, 0.0048301008, 0.0032534598, 0.0021315311, 0.0013582974 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 18; ++i)
	{
		color += tex2Dlod(ClaritySampler, float4(texcoord + float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r* weight[i];
		color += tex2Dlod(ClaritySampler, float4(texcoord - float2(0.0, offset[i] * BUFFER_PIXEL_SIZE.y) * ClarityOffset, 0.0, 0.0)).r* weight[i];
	}
}	

	return color;
}

float Clarity3(in float4 pos : SV_Position, in float2 texcoord : TEXCOORD) : COLOR
{
	float color = tex2D(ClaritySampler2, texcoord).r;
	
if(ClarityRadius == 0)	
{
	const float offset[4] = { 0.0, 1.1824255238, 3.0293122308, 5.0040701377 };
	const float weight[4] = { 0.39894, 0.2959599993, 0.0045656525, 0.00000149278686458842 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 4; ++i)
	{
		color += tex2Dlod(ClaritySampler2, float4(texcoord + float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).r* weight[i];
		color += tex2Dlod(ClaritySampler2, float4(texcoord - float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).r* weight[i];
	}
}	

if(ClarityRadius == 1)	
{
	const float offset[6] = { 0.0, 1.4584295168, 3.40398480678, 5.3518057801, 7.302940716, 9.2581597095 };
	const float weight[6] = { 0.13298, 0.23227575, 0.1353261595, 0.0511557427, 0.01253922, 0.0019913644 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 6; ++i)
	{
		color += tex2Dlod(ClaritySampler2, float4(texcoord + float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).r* weight[i];
		color += tex2Dlod(ClaritySampler2, float4(texcoord - float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).r* weight[i];
	}
}	

if(ClarityRadius == 2)	
{
	const float offset[11] = { 0.0, 1.4895848401, 3.4757135714, 5.4618796741, 7.4481042327, 9.4344079746, 11.420811147, 13.4073334, 15.3939936778, 17.3808101174, 19.3677999584 };
	const float weight[11] = { 0.06649, 0.1284697563, 0.111918249, 0.0873132676, 0.0610011113, 0.0381655709, 0.0213835661, 0.0107290241, 0.0048206869, 0.0019396469, 0.0006988718 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 11; ++i)
	{
		color += tex2Dlod(ClaritySampler2, float4(texcoord + float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).r* weight[i];
		color += tex2Dlod(ClaritySampler2, float4(texcoord - float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).r* weight[i];
	}
}	

if(ClarityRadius == 3)	
{
	const float offset[15] = { 0.0, 1.4953705027, 3.4891992113, 5.4830312105, 7.4768683759, 9.4707125766, 11.4645656736, 13.4584295168, 15.4523059431, 17.4461967743, 19.4401038149, 21.43402885, 23.4279736431, 25.4219399344, 27.4159294386 };
	const float weight[15] = { 0.0443266667, 0.0872994708, 0.0820892038, 0.0734818355, 0.0626171681, 0.0507956191, 0.0392263968, 0.0288369812, 0.0201808877, 0.0134446557, 0.0085266392, 0.0051478359, 0.0029586248, 0.0016187257, 0.0008430913 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 15; ++i)
	{
		color += tex2Dlod(ClaritySampler2, float4(texcoord + float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).r* weight[i];
		color += tex2Dlod(ClaritySampler2, float4(texcoord - float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).r* weight[i];
	}
}	

if(ClarityRadius == 4)	
{
	const float offset[18] = { 0.0, 1.4953705027, 3.4891992113, 5.4830312105, 7.4768683759, 9.4707125766, 11.4645656736, 13.4584295168, 15.4523059431, 17.4461967743, 19.4661974725, 21.4627427973, 23.4592916956, 25.455844494, 27.4524015179, 29.4489630909, 31.445529535, 33.4421011704 };
	const float weight[18] = { 0.033245, 0.0659162217, 0.0636705814, 0.0598194658, 0.0546642566, 0.0485871646, 0.0420045997, 0.0353207015, 0.0288880982, 0.0229808311, 0.0177815511, 0.013382297, 0.0097960001, 0.0069746748, 0.0048301008, 0.0032534598, 0.0021315311, 0.0013582974 };
	
	color *= weight[0];
	
	[unroll]
	for(int i = 1; i < 18; ++i)
	{
		color += tex2Dlod(ClaritySampler2, float4(texcoord + float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).r* weight[i];
		color += tex2Dlod(ClaritySampler2, float4(texcoord - float2(offset[i] * BUFFER_PIXEL_SIZE.x, 0.0) * ClarityOffset, 0.0, 0.0)).r* weight[i];
	}
}	
	
	return color;
}

// ---- ps_3_0 entry points (GShade looks) ----
float4 gs_Clarity1(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { float v = Clarity1(float4(vp + 0.5, 0.0, 1.0), uv); return float4(v, v, v, 1.0); }
float4 gs_Clarity2(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { float v = Clarity2(float4(vp + 0.5, 0.0, 1.0), uv); return float4(v, v, v, 1.0); }
float4 gs_Clarity3(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { float v = Clarity3(float4(vp + 0.5, 0.0, 1.0), uv); return float4(v, v, v, 1.0); }
float4 gs_ClarityFinal(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { return float4(ClarityFinal(float4(vp + 0.5, 0.0, 1.0), uv), 1.0); }
