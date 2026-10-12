// Apex Radiance test effect (tools/reshade_fx_test): depth input, two render targets in one pass, stencil and blending,
// array and matrix settings, loops and a shared texture. Written for the offline check; not a shipped effect.

uniform float FarPlane < ui_type = "drag"; ui_min = 1.0; ui_max = 10000.0; > = 1000.0;
uniform float Kernel[4] < hidden = true; > = { 0.4, 0.3, 0.2, 0.1 };
uniform float3x3 ColorMatrix < hidden = true; > = float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
uniform bool ShowDepth < ui_label = "Show depth"; > = false;
uniform bool Hold < source = "key"; keycode = 0x20; mode = "toggle"; >;
uniform float2 Mouse < source = "mousepoint"; >;

texture DepthTex : DEPTH;
sampler Depth { Texture = DepthTex; MagFilter = POINT; MinFilter = POINT; };
texture ColorTex : COLOR;
sampler Color { Texture = ColorTex; };

texture ApexSharedTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R8; };
sampler ApexShared { Texture = ApexSharedTex; };
texture NormalTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
sampler Normal { Texture = NormalTex; };

float LinearDepth(float2 uv)
{
	const float d = tex2Dlod(Depth, float4(uv, 0, 0)).x;
	const float N = 1.0;
	return d / (FarPlane - d * (FarPlane - N));
}

void FullScreenVS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD)
{
	texcoord.x = (id == 2) ? 2.0 : 0.0;
	texcoord.y = (id == 1) ? 2.0 : 0.0;
	position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

void PreparePS(float4 vpos : SV_Position, float2 uv : TEXCOORD, out float4 mask : SV_Target0, out float4 normal : SV_Target1)
{
	const float2 px = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
	const float c = LinearDepth(uv);
	const float3 dx = float3(px.x, 0, LinearDepth(uv + float2(px.x, 0)) - c);
	const float3 dy = float3(0, px.y, LinearDepth(uv + float2(0, px.y)) - c);
	normal = float4(normalize(cross(dx, dy)) * 0.5 + 0.5, 1.0);
	mask = c < 0.999 ? 1.0 : 0.0;
}

float4 BlurPS(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float2 px = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
	float3 sum = tex2D(Color, uv).rgb * Kernel[0];
	[unroll] for (int i = 1; i < 4; i++)
	{
		sum += tex2D(Color, uv + float2(px.x * i, 0)).rgb * Kernel[i];
		sum += tex2D(Color, uv - float2(px.x * i, 0)).rgb * Kernel[i];
	}
	sum = mul(ColorMatrix, sum);
	const float3 n = tex2D(Normal, uv).xyz;
	if (ShowDepth) return float4(LinearDepth(uv).xxx, 1.0);
	const float near = distance(vpos.xy, Mouse) < 8.0 || Hold ? 0.0 : 1.0;
	return float4(lerp(tex2D(Color, uv).rgb, sum * (0.999 + 0.001 * n.z), 0.5 * near * tex2D(ApexShared, uv).x), 1.0);
}

technique ApexTestDepth < ui_label = "Apex test: depth"; toggle = 0x74; toggle_ctrl = true; >
{
	pass Prepare
	{
		VertexShader = FullScreenVS; PixelShader = PreparePS;
		RenderTarget0 = ApexSharedTex; RenderTarget1 = NormalTex;
		ClearRenderTargets = true;
		StencilEnable = true; StencilPass = REPLACE; StencilRef = 1; StencilFunc = ALWAYS;
	}
	pass Blur
	{
		VertexShader = FullScreenVS; PixelShader = BlurPS;
		StencilEnable = true; StencilFunc = EQUAL; StencilRef = 1;
		BlendEnable = true; SrcBlend = SRCALPHA; DestBlend = INVSRCALPHA;
	}
}
