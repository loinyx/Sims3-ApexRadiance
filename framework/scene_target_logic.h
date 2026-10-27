#pragma once
// High-precision scene (scene_target.cpp): its pure decisions, the resolve shader and a C++ copy of the shader's math,
// shared with the offline check (tools/scene_target_test). Part of Apex Radiance. Credits: @loinyx
#include <d3d9.h>
#include <algorithm>
#include <cmath>

namespace SceneTargetLogic {

// 16 bits per channel, unsigned normalised: it clamps to 0..1 like the 8-bit back buffer, so the game's blending gives the
// same result with more precision. Not FP16 (it would not clamp) and not A2B10G10R10 (2 bits of alpha: the bloom mask).
constexpr D3DFORMAT kFormat = D3DFMT_A16B16G16R16;
constexpr unsigned kBytesPerPixel = 8;

enum class Refusal { None, Multisample, BackBufferFormat, NoSupport };

// Why the scene cannot be drawn into the 16-bit target with this back buffer. supported: the 16-bit format is a render
// target with post-pixel-shader blending and matches the game's depth format on this device
inline Refusal Check(D3DFORMAT backBuffer, D3DMULTISAMPLE_TYPE multisample, bool supported) {
    if (multisample != D3DMULTISAMPLE_NONE) return Refusal::Multisample; // the game's own Edge Smoothing: its depth is multisampled
    if (backBuffer != D3DFMT_A8R8G8B8) return Refusal::BackBufferFormat; // the bloom mask lives in the back buffer's alpha
    if (!supported) return Refusal::NoSupport;
    return Refusal::None;
}

// SetRenderTarget(index, requested) binds the 16-bit target instead (substituting = this frame's scene is not resolved yet)
inline bool Substitutes(bool substituting, DWORD index, const void* requested, const void* backBuffer) {
    return substituting && index == 0 && requested && requested == backBuffer;
}

// A copy, fill or read-back touching the back buffer needs the scene resolved into it first
inline bool NeedsResolve(bool substituting, const void* a, const void* b, const void* backBuffer) {
    return substituting && backBuffer && (a == backBuffer || b == backBuffer);
}

// A 16-bit value that is exactly an 8-bit level (k * 257 / 65535) is within 1/257 level of no other 16-bit value: such pixels
// (the UI, flat colours) pass unchanged
constexpr float kExact = 0.5f / 257.0f;

// The Banding Fix's noise (ShaderPatches::AddDither): interleaved gradient noise of the pixel, then the inverse CDF of the
// triangular distribution, in (-1, 1)
inline float Noise(float px, float py, float phase) {
    const float ign = 52.9829189f * (0.06711056f * px + 0.00583715f * py - std::floor(0.06711056f * px + 0.00583715f * py));
    const float u0 = ign - std::floor(ign) + phase;
    const float u = u0 - std::floor(u0);
    const float r = 2.0f * u - 1.0f;
    return (r < 0 ? -1.0f : 1.0f) * (1.0f - std::sqrt(1.0f - std::fabs(r)));
}

// One colour channel of the resolve, in 8-bit levels before the output's rounding. v = the 16-bit value (0..1), t = noise,
// strength = the grain's peak in levels. The amplitude never reaches past 0 or 255 (no bias at the clamp)
inline float ResolveLevel(float v, float t, float strength) {
    const float x = v * 255.0f;
    const float f = x - std::floor(x);
    if (std::min(f, 1.0f - f) < kExact) return x;
    return x + t * strength * std::min(1.0f, std::min(x, 255.0f - x));
}

// ps_3_0. c0.x = strength (levels), c0.y = grain phase. s0 = the 16-bit scene, point sampled. Alpha (the bloom mask) passes
// unchanged
inline constexpr const char* kResolveHlsl = R"HLSL(
sampler2D sceneTex : register(s0);
float4 cAmount : register(c0);
float4 main(float2 uv : TEXCOORD0, float2 pos : VPOS) : COLOR0 {
    float4 v = tex2D(sceneTex, uv);
    float u = frac(frac(52.9829189 * frac(dot(pos, float2(0.06711056, 0.00583715)))) + cAmount.y);
    float r = 2.0 * u - 1.0;
    float t = sign(r) * (1.0 - sqrt(1.0 - abs(r)));
    float3 x = v.rgb * 255.0;
    float3 f = frac(x);
    float3 amp = cAmount.x * min(1.0, min(x, 255.0 - x)) * step(0.5 / 257.0, min(f, 1.0 - f));
    return float4((x + t * amp) / 255.0, v.a);
}
)HLSL";

} // namespace SceneTargetLogic
