# Glow sprite smoothing check

Run `tools/sprite_smooth_test/run.ps1 [-OutDir <scratch-folder>] [-Glow <png>] [-Beam <png>] [-Scan <captures folder>]` with
the x86 MSVC toolchain installed.
`tools/development/check.ps1 -Suite Smoke` runs it too.

`check.cpp` includes `features/sprite_smooth_logic.h`, the code the ASI uses, and the game's DXT5 encoder
(`features/dxt_codec.cpp`, `DxtCodec::Ref`):

- The pixel shader family: hand-assembled texture x vertex colour shaders (ps_2_0, ps_2_x, ps_3_0 with a COLOR input)
  are accepted; a texture alone, a constant tint, a swizzled colour, a partial write, extra arithmetic, sampler 1 and a
  ps_3_0 multiplying by a texture coordinate are refused.
- A synthetic radial glow (128 x 128) and a synthetic beam (64 x 256, its sides a quarter of its half-width), white with
  the shape in alpha, encoded with the game's encoder and decoded, then smoothed (`SpriteSmoothLogic::Smooth`): the gate
  accepts them, the mean alpha is kept within 0.05 level, the slope changes on the ramps (texels whose steps are at most
  12 levels) drop to half (glow) or 60% (beam, whose ramps include the feet of its sides) and the beam keeps 85% of its
  real edges (steps over 12 levels).
- The gate refuses a checker, fine noise and a hard-edged disc and keeps a flat texture unchanged; the 2x2 mip keeps the
  mean; a DXT1 three-colour block decodes its midpoint and transparent black.
- `-Glow` / `-Beam`: the same measures on a light capture's texture (`T<n>_<w>x<h>_DXT5.png`, its `_alpha.png` beside
  it: the decoded texture, lossless); half the slope changes for both, 85% of the edges for the beam. `-Scan`: every
  distinct captured DXT texture of 16 to 512 texels a side under a folder, with its edge share, change and the gate's
  answer (printed, not checked).

It writes only its executable and objects into the output folder; captures are only read. It does not test the hooks,
the texture copies on a device, DXVK or the look in game.
