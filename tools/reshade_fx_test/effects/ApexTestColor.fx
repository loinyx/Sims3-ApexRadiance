// Apex Radiance test effect (tools/reshade_fx_test): colour grading with every kind of menu setting, two passes through a
// mipmapped render target and the special sources. Written for the offline check; not a shipped effect.

uniform float Strength < ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Strength"; ui_tooltip = "How strong"; ui_category = "Look"; > = 0.5;
uniform int Mode < ui_type = "combo"; ui_items = "Warm\0Cool\0Neutral\0"; ui_label = "Mode"; > = 0;
uniform bool Invert < ui_label = "Invert"; > = false;
uniform float3 Tint < ui_type = "color"; ui_label = "Tint"; > = float3(1.0, 0.9, 0.8);
uniform int2 Offset < ui_type = "drag"; ui_min = -8; ui_max = 8; > = int2(1, -1);
uniform float4 Weights < ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; > = float4(1.0, 1.0, 1.0, 0.5);

uniform float FrameTime < source = "frametime"; >;
uniform int FrameCount < source = "framecount"; >;
uniform float Timer < source = "timer"; >;
uniform int Random < source = "random"; min = 0; max = 100; >;
uniform float4 Date < source = "date"; >;
uniform float2 PingPong < source = "pingpong"; min = 0.0; max = 1.0; step = float2(0.5, 1.0); smoothing = 0.1; >;
uniform bool OverlayOpen < source = "overlay_open"; >;

texture BackBufferTex : COLOR;
sampler BackBuffer { Texture = BackBufferTex; SRGBTexture = true; };
sampler BackBufferLinear { Texture = BackBufferTex; MagFilter = LINEAR; MinFilter = LINEAR; AddressU = CLAMP; AddressV = CLAMP; };

texture HalfTex { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; MipLevels = 4; };
sampler Half { Texture = HalfTex; MipFilter = LINEAR; };

void FullScreenVS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD)
{
	texcoord.x = (id == 2) ? 2.0 : 0.0;
	texcoord.y = (id == 1) ? 2.0 : 0.0;
	position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 DownPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float2 px = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
	float3 c = tex2D(BackBufferLinear, uv + px * Offset).rgb;
	c += tex2Doffset(BackBufferLinear, uv, int2(1, 0)).rgb;
	return float4(c * 0.5, 1.0);
}

float4 GradePS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float3 c = tex2D(BackBuffer, uv).rgb;
	const float3 blur = tex2Dlod(Half, float4(uv, 0.0, 2.0)).rgb;
	float3 tone = Mode == 0 ? float3(1.05, 1.0, 0.95) : Mode == 1 ? float3(0.95, 1.0, 1.05) : 1.0;
	c = lerp(c, c * Tint * tone, Strength) * Weights.rgb + blur * 0.01 * Weights.a;
	if (Invert) c = 1.0 - c;
	// keep the special sources alive in the code
	c += 1e-6 * (FrameTime + FrameCount + Timer + Random + Date.w + PingPong.x + (OverlayOpen ? 1.0 : 0.0));
	c += 1e-6 * float(tex2Dsize(Half, 0).x);
	return float4(saturate(c), 1.0);
}

technique ApexTestColor < ui_label = "Apex test: colour"; ui_tooltip = "Colour grading test"; enabled = true; >
{
	pass Down { VertexShader = FullScreenVS; PixelShader = DownPS; RenderTarget = HalfTex; }
	pass Grade { VertexShader = FullScreenVS; PixelShader = GradePS; SRGBWriteEnable = true; }
}
