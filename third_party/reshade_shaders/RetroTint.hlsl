// Ported to ps_3_0 for Apex Radiance (Looks) from https://github.com/Daodan317081/reshade-shaders/blob/f01ddb6f3dce6a8fb75ffb9fee878a1489edfc16/Shaders/RetroTint.fx
// Licence of the repository, which covers this file:
// BSD 3-Clause License
//
// Copyright (c) 2018-2019, Alexander Federwisch
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

float3 fUIColor : register(c8);

float fUIStrength : register(c9);

float3 RetroTintPS(float4 vpos : SV_Position, float2 texcoord : TexCoord) : SV_Target {
    float3 color = tex2D(BackBuffer, texcoord).rgb;
    //Blend mode: Screen
    return lerp(color, 1.0 - (1.0 - color) * (1.0 - fUIColor), fUIStrength);
}

// ---- ps_3_0 entry points (Looks) ----
float4 gs_RetroTintPS(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { return float4(RetroTintPS(float4(vp + 0.5, 0.0, 1.0), uv), 1.0); }
