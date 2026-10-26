# Glow sprite smoothing check

Run `tools/sprite_smooth_test/run.ps1 [-OutDir <scratch-folder>]` with the x86 MSVC toolchain installed.
`tools/development/check.ps1 -Suite Smoke` runs it too.

`check.cpp` includes `features/sprite_smooth_logic.h`, the code the ASI uses, and the game's DXT5 encoder
(`features/dxt_codec.cpp`, `DxtCodec::Ref`):

- The pixel shader family: hand-assembled texture x vertex colour shaders (ps_2_0, ps_2_x, ps_3_0 with a COLOR input)
  are accepted; a texture alone, a constant tint, a swizzled colour, a partial write, extra arithmetic, sampler 1 and a
  ps_3_0 multiplying by a texture coordinate are refused.
- A synthetic radial glow (128 x 128, white, a cone in alpha that saturates at its centre) encoded with the game's
  encoder and decoded: the DXT5 glow must show ring steps (the slope changing from texel to texel), and after the
  smoothing the mean must be kept, the slope changes at most half as large and under 2 levels, the glow within 8 levels
  of the source and the peak kept. Sigma 1.0, 1.5 and 2.0 are printed for comparison.
- The detail gate refuses a checker and a hard-edged disc and keeps a flat texture unchanged; the 2x2 mip keeps the
  mean; a DXT1 three-colour block decodes its midpoint and transparent black.

It writes only its executable and objects into the output folder. It does not test the hooks, the texture copies on a
device, DXVK or the look in game.
