// Generated from HighPassSharpen.fx by tools/gen_gshade_shaders.py
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
//High Pass Sharpening by Ioxa
//Version 1.5 for ReShade 3.0
// Lightly optimized by Marot Satil for the GShade project.

//Settings
float HighPassSharpRadius : register(c8);

float HighPassSharpOffset : register(c9);

float HighPassBlendMode : register(c10);

float HighPassBlendIfDark : register(c11);

float HighPassBlendIfLight : register(c12);

float HighPassViewBlendIfMask : register(c13);

float HighPassSharpStrength : register(c14);

float HighPassDarkIntensity : register(c15);

float HighPassLightIntensity : register(c16);

float HighPassViewSharpMask : register(c17);



float3 SharpBlurFinal(in float4 pos : SV_Position, in float2 texcoord : TEXCOORD) : COLOR
{

	float3 color = tex2D(BackBuffer, texcoord).rgb;
	float3 orig = color;
	float luma = dot(color.rgb,float3(0.32786885,0.655737705,0.0163934436));
	float3 chroma = orig.rgb/luma;
	
	if (HighPassSharpRadius == 1)
	{
		const int sampleOffsetsX[25] = {  0.0, 	 1, 	  0, 	 1,     1,     2,     0,     2,     2,     1,      1,     2,     2,     3,     0,     3,     3,     1,    -1, 3, 3, 2, 2, 3, 3 };
		const int sampleOffsetsY[25] = {  0.0,     0, 	  1, 	 1,    -1,     0,     2,     1,    -1,     2,     -2,     2,    -2,     0,     3,     1,    -1,     3,     3, 2, -2, 3, -3, 3, -3};	
		const float sampleWeights[5] = { 0.225806, 0.150538, 0.150538, 0.0430108, 0.0430108 };
		
		color *= sampleWeights[0];
		
		[loop]
		for(int i = 1; i < 5; ++i) {
			color += tex2Dlod(BackBuffer, float4(texcoord + float2(sampleOffsetsX[i] * BUFFER_RCP_WIDTH, sampleOffsetsY[i] * BUFFER_RCP_HEIGHT) * HighPassSharpOffset, 0.0, 0.0)).rgb * sampleWeights[i];
			color += tex2Dlod(BackBuffer, float4(texcoord - float2(sampleOffsetsX[i] * BUFFER_RCP_WIDTH, sampleOffsetsY[i] * BUFFER_RCP_HEIGHT) * HighPassSharpOffset, 0.0, 0.0)).rgb * sampleWeights[i];
		}
	}
	
	if (HighPassSharpRadius == 2)
	{
		const int sampleOffsetsX[13] = {  0.0, 	   1, 	  0, 	 1,     1,     2,     0,     2,     2,     1,    1,     2,     2 };
		const int sampleOffsetsY[13] = {  0.0,     0, 	  1, 	 1,    -1,     0,     2,     1,    -1,     2,     -2,     2,    -2};
		float sampleWeights[13] = { 0.1509985387665926499, 0.1132489040749444874, 0.1132489040749444874, 0.0273989284225933369, 0.0273989284225933369, 0.0452995616018920668, 0.0452995616018920668, 0.0109595713409516066, 0.0109595713409516066, 0.0109595713409516066, 0.0109595713409516066, 0.0043838285270187332, 0.0043838285270187332 };
		
		color *= sampleWeights[0];
		
		[loop]
		for(int i = 1; i < 13; ++i) {
			color += tex2Dlod(BackBuffer, float4(texcoord + float2(sampleOffsetsX[i] * BUFFER_RCP_WIDTH, sampleOffsetsY[i] * BUFFER_RCP_HEIGHT) * HighPassSharpOffset, 0.0, 0.0)).rgb * sampleWeights[i];
			color += tex2Dlod(BackBuffer, float4(texcoord - float2(sampleOffsetsX[i] * BUFFER_RCP_WIDTH, sampleOffsetsY[i] * BUFFER_RCP_HEIGHT) * HighPassSharpOffset, 0.0, 0.0)).rgb * sampleWeights[i];
		}
	}

	if (HighPassSharpRadius == 3)
	{
		static const float sampleOffsetsX[13] = { 				  0.0, 			    1.3846153846, 			 			  0, 	 		  1.3846153846,     	   	 1.3846153846,     		    3.2307692308,     		  			  0,     		 3.2307692308,     		   3.2307692308,     		 1.3846153846,    		   1.3846153846,     		  3.2307692308,     		  3.2307692308 };
		static const float sampleOffsetsY[13] = {  				  0.0,   					   0, 	  		   1.3846153846, 	 		  1.3846153846,     		-1.3846153846,     					   0,     		   3.2307692308,     		 1.3846153846,    		  -1.3846153846,     		 3.2307692308,   		  -3.2307692308,     		  3.2307692308,    		     -3.2307692308 };
		float sampleWeights[13] = { 0.0957733978977875942, 0.1333986613666725565, 0.1333986613666725565, 0.0421828199486419528, 0.0421828199486419528, 0.0296441469844336464, 0.0296441469844336464, 0.0093739599979617454, 0.0093739599979617454, 0.0093739599979617454, 0.0093739599979617454, 0.0020831022264565991,  0.0020831022264565991 };
		
		color *= sampleWeights[0];
		
		[loop]
		for(int i = 1; i < 13; ++i) {
			color += tex2Dlod(BackBuffer, float4(texcoord + float2(sampleOffsetsX[i] * BUFFER_RCP_WIDTH, sampleOffsetsY[i] * BUFFER_RCP_HEIGHT) * HighPassSharpOffset, 0.0, 0.0)).rgb * sampleWeights[i];
			color += tex2Dlod(BackBuffer, float4(texcoord - float2(sampleOffsetsX[i] * BUFFER_RCP_WIDTH, sampleOffsetsY[i] * BUFFER_RCP_HEIGHT) * HighPassSharpOffset, 0.0, 0.0)).rgb * sampleWeights[i];
		}
	}	
		
	float sharp = dot(color.rgb,float3(0.32786885,0.655737705,0.0163934436));
	sharp = 1.0 - sharp;
	sharp = (luma+sharp)*0.5;

	float sharpMin = lerp(0,1,smoothstep(0,1,sharp));
	float sharpMax = sharpMin;
	sharpMin = lerp(sharp,sharpMin,HighPassDarkIntensity);
	sharpMax = lerp(sharp,sharpMax,HighPassLightIntensity);
	sharp = lerp(sharpMin,sharpMax,step(0.5,sharp));

	if(HighPassViewSharpMask)
	{
		//View sharp mask
		orig.rgb = sharp;
		luma = sharp;
		chroma = 1.0;
	}
	else 
	{	
		if(HighPassBlendMode == 0)
		{
			//softlight
			sharp = lerp(2*luma*sharp + luma*luma*(1.0-2*sharp), 2*luma*(1.0-sharp)+pow(luma,0.5)*(2*sharp-1.0), step(0.49,sharp));
		}
		
		if(HighPassBlendMode == 1)
		{
			//overlay
			sharp = lerp(2*luma*sharp, 1.0 - 2*(1.0-luma)*(1.0-sharp), step(0.50,luma));
		}
		
		if(HighPassBlendMode == 2)
		{
			//Hardlight
			sharp = lerp(2*luma*sharp, 1.0 - 2*(1.0-luma)*(1.0-sharp), step(0.50,sharp));
		}
		
		if(HighPassBlendMode == 3)
		{
			//Multiply
			sharp = saturate(2 * luma * sharp);
		}
		
		if(HighPassBlendMode == 4)
		{
			//vivid light
			sharp = lerp(2*luma*sharp, luma/(2*(1-sharp)), step(0.5,sharp));
		}
		
		if(HighPassBlendMode == 5)
		{
			//Linear Light
			sharp = luma + 2.0*sharp-1.0;
		}
		
		if(HighPassBlendMode == 6)
		{
			//Screen
			sharp = 1.0 - (2*(1.0-luma)*(1.0-sharp));
		}
		
		if(HighPassBlendMode == 7)
		{
			//Addition
			sharp = saturate(luma + (sharp - 0.5));
		}
	}
	
	if( HighPassBlendIfDark > 0 || HighPassBlendIfLight < 255 || HighPassViewBlendIfMask)
	{
		float BlendIfD = (HighPassBlendIfDark/255.0)+0.0001;
		float BlendIfL = (HighPassBlendIfLight/255.0)-0.0001;
		float mix = dot(orig.rgb, 0.333333);
		float mask = 1.0;
		
		if(HighPassBlendIfDark > 0)
		{
			mask = lerp(0.0,1.0,smoothstep(BlendIfD-(BlendIfD*0.2),BlendIfD+(BlendIfD*0.2),mix));
		}
		
		if(HighPassBlendIfLight < 255)
		{
			mask = lerp(mask,0.0,smoothstep(BlendIfL-(BlendIfL*0.2),BlendIfL+(BlendIfL*0.2),mix));
		}
		
		sharp = lerp(luma,sharp,mask);
		if (HighPassViewBlendIfMask)
		{
			sharp = mask;
			luma = mask;
			chroma = 1.0;
		}
	}
	
	luma = lerp(luma, sharp, HighPassSharpStrength);
	orig.rgb = luma*chroma;

	return saturate(orig);
}

// ---- ps_3_0 entry points (GShade looks) ----
float4 gs_SharpBlurFinal(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { return float4(SharpBlurFinal(float4(vp + 0.5, 0.0, 1.0), uv), 1.0); }
