# Banding Fix: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/banding-fix.md](../features/banding-fix.md).

### 2026-09-30: report of rings around lamp light

**Context:** observed in game: colours were uneven, mainly in lit areas; an interior wall showed
rings around a lamp's pool of light, on an OLED display (AW3225QF, DCI-P3).

**Finding:** lossless captures (`cor_1/2/6.bmp`) showed black at 0 and white at 255, so the range is correct. In smooth
dark areas 64 to 89% of neighbour steps were 1 level and 11 to 36% were 2 or more: the rings are the 8-bit rounding of
dark gradients. Wide gamut is not the cause; reaching P3 would need an HDR / scRGB output.

**Outcome:** a dither in the scene's pixel shaders, before the rounding, was chosen (commit f578f43, ps_3_0 only).

### 2026-09-30: uniform grain replaced by a triangular grain

**Context:** the first build (57a0a09d) used a uniform grain of plus or minus half a step. Observed in game: a partial
improvement only.

**Finding:** a uniform dither leaves noise modulation. The inverse CDF of the triangular distribution applied to the IGN
value (TPDF) removes it. GPU check `tpdftest.cpp`: the mean is preserved and the level weights follow the triangle.

**Outcome:** triangular grain since commit 7d5615d, with a Strength slider (then 0.5 to 3 steps) and a magenta view of
uncovered surfaces. The AO composite gained the same grain (AO settings revision 3).

### 2026-09-30: Color > Banding tab

**Context:** everything against colour steps was grouped in one place (commit cfd2442).

**Finding:** Picture's deband ("Smooth gradients") is the only help for ps_2_x surfaces such as the sky.

**Outcome:** the Banding tab holds the Banding Fix card and Smooth gradients. Smooth gradients follows the Banding Fix
switch; with Picture off, the Picture pass runs with only the deband, never in a frame without a scene copy.

### 2026-09-30 evening: most of the scene was not covered

**Context:** observed in game: on many walls the fix had almost no effect. The magenta view turned the whole screen magenta: a
full-screen depth-on ps_2_x pass drawn over the scene with blending became opaque magenta. The log showed only 109 to
139 copies made in the session.

**Finding:** the magenta view was replaced by "Show covered surfaces" (a coarse 24-step grain on covered draws) and the
developer build logged coverage every 20 s. The log showed 123 to 130 scene draws per frame dithered and 166 to 172
ps_2_x (0 ps_3_0 refused): more than half of the scene, walls included, draws with ps_2_x shaders.

**Outcome:** the ps_2_x path (commit 53d1091): position from a vertex-shader copy writing the clip position to a free
TEXCOORD, ps_2_x pixel copies (519 of 2308 failed as ps_2_0 because of the 64 arithmetic slots).

### 2026-09-30 night: Strength 0 to 100% and Moving grain

**Context:** at 223% Strength the grain showed as fine specks over every surface.

**Finding:** two causes: a strength above one step, and the AO composite adding the same pattern a second time, which
doubled the grain.

**Outcome:** Strength became 0 to 100% (100% = plus or minus one step), Moving grain was added (commit a337f82), and
the AO composite grain follows the Banding Fix strength and phase, only where the shade changed the pixel, with a
shifted pattern (AO settings revision 5).

### 2026-09-30: flagged experimental

**Context:** version 2.2.1 (commit 9890316).

**Finding:** the in-game coverage of the ps_2_x path was not yet confirmed.

**Outcome:** the Banding Fix is flagged experimental and the Banding tab shows a "Still being tested" note.

### 2026-10-26: grain under the UI, specks and a deband that did nothing

**Context:** players reported a grainy picture and areas the fix missed.

**Finding:** five causes, checked in code, light captures and a new offline harness (`tools/banding_fix_check`):

- The draw gate tested only the depth test. The UI's glass passes and back-buffer redraws draw with the depth test on,
  `ZFUNC ALWAYS` and no depth write (all 30 light captures show such draws, 59 in all, every one the same ps_2_0 computing `x (1 + 0.4) / (x + 0.4)`
  from a 2048x1024 copy), so they got the grain on top of the scene's; PostScene and Picture already skipped them. The
  feature page wrongly said the UI draws with the depth test off.
- The grain crossed the clamp: on native D3D9 and DXVK, 9.7% of pure black pixels came out at level 1 and 12.5% of pure
  white at 254.
- Every blended draw got a full grain on top of the target's, also additive and multiply blends (decals
  `DESTCOLOR` / `SRCCOLOR` in the captures).
- The Picture dither used the scene's IGN pattern (correlated) and changed every pixel by up to half a step.
- *Smooth gradients* at the default 8% had a threshold of 0.48 level: no 8-bit neighbour passed it, so the pass ran
  every frame and changed nothing (0 pixels on a stepped gradient, offline).

**Outcome:** depth-`ALWAYS` draws without depth write are left alone; the grain stays within half a step of black and
white and tapers to uniform above mid-grey; additive and multiply blends get half of it with a decorrelated phase; the
pattern is R2 (lower 2x2 and 4x4 block variance than IGN); the AO composite uses R2 with swapped coefficients and the
Picture dither IGN, triangular, only where the pass changed the pixel; the deband threshold is `1 + 6.25 x setting`
levels with bilinear taps. Shader acceptance is unchanged (ps_3_0 4907 / 4907, ps_2_0 2996 / 3104). Subtracting half a
pixel from the ps_2_x position was rejected: the two grains already match pixel for pixel. Not yet confirmed in game.

### 2026-10-26: High-precision scene (experimental)

**Context:** the grain hides the 8-bit rounding of each scene draw, but every blend still rounds to 8 bits, so a pixel
lit by several passes collects one rounding and one grain per pass.

**Finding:** from a read-only study of the frame: only render target 0 set to the back buffer needs a substitute; the
pick pass, shadows, reflections and bloom use their own targets. Apex callbacks save render target 0 with
`GetRenderTarget` and put it back with `CallOriginalSetRenderTarget` (Ambient Occlusion's receiver mask, the GPU lamps),
so the substitution has to apply there too and `GetRenderTarget` has to report the back buffer. A 16-bit unsigned
normalised target clamps like the back buffer; FP16 does not, and 10-bit formats keep 2 bits of alpha (the bloom
mask). The offline check measured the GPU's own float to 8-bit conversion (RTX 4070 Ti SUPER): values up to 0.56
level above an integer round down, the same for the game's 8-bit path.

**Outcome:** a switch on the Banding Fix card, off by default, with the target, the substitution and one resolve with
one grain (exact 8-bit values pass unchanged). 10-bit output was left out. Not yet tested in game: open checks on the
[validation page](../validation/banding-fix.md#open-checks).

### 2026-10-26: High-precision scene on by default; Smooth gradients and Moving grain removed

**Context:** the in-game test of the grain changes and the High-precision scene (development build of `banding/all`).

**Finding:** the grain and the High-precision scene looked right; the card showed the target active (63 MB of video
memory, most resolves at a back-buffer copy). *Smooth gradients* made no visible difference; *Moving grain* was not
wanted.

**Outcome:** the High-precision scene is on by default (also in the built-in profiles) and no longer marked
Experimental. *Smooth gradients* is removed with its deband in the Picture pass (the pass no longer runs for it alone)
and *Moving grain* with it; old `deband` and `graoEmMovimento` keys are ignored. The card was redesigned: a before /
after picture of the gradient that follows the current state, then *Grain* (Strength) and *Precision* (the switch and a
Status pill), with the developer resolves under *Live counters*.

### 2026-10-26: rings in car headlights' glow

**Context:** observed in game with the High-precision scene build: car headlights' glow still showed rings. Light
captures of the glow (draw #229) showed `texld r0, t1, s0; mul r0, r0, v0; mov oC0, r0` (ps_2_0), additive blending,
no depth write, and a 128x128 DXT5 texture, white with a radial glow in alpha, sampled without a mip filter.

**Finding:** the rings are in the texture. DXT5 keeps 8 alpha levels per 4x4 block: along a row of the captured glow the
values go 22, 26, 28, 31, 33, 37, 41, 43, 49 and the slope changes by up to 13 levels between neighbouring texels.
Magnified, each change is a crease of the bilinear filter. The 16-bit scene and the grain act after the texture is read.
In the game's shader package the texture x vertex colour family is 2 ps_2_x and 1 ps_3_0 shaders; the other captures
hold no other draw of it. A blur of 1.5 texels changes the glow by 0.87 levels RMS and textured content by 2.8 or more.

**Outcome:** the Banding Fix draws such sprites with a smoothed 16-bit copy of their DXT texture, made once per texture
when the blur changes it no more than DXT noise, and kept by the game texture as private data. Offline: the slope
changes drop from 13 to 1.3 levels on the captured glow, the mean is kept. Not yet tested in game: open checks on the
[validation page](../validation/banding-fix.md#open-checks).

### 2026-10-26: the headlight beam was refused

**Context:** observed in game with the glow-sprite build: the car light was still banded. A light capture of the beam
(draw #1840) showed the same shader, additive blending, no depth write and a 64x256 DXT5 texture with a narrow cone in
alpha; the coverage log line read "1 as drawn, textures smoothed 0".

**Finding:** the detail gate refused the beam: the Gaussian changed it by 2.80 levels RMS, 4.7% of texels by more than 4
levels, at its steep sides. A range-limited bilateral filter (3 to 12 levels) left the glow's slope changes almost as
they were (2.66 -> 2.29 rms at 4 levels; 0.93 at 12 levels, against 0.16 with the Gaussian), since a ramp of 4 levels a
texel already exceeds such a range. Bounding each texel's
move to half its DXT step kept too much of the beam's noise, which is larger than the quantisation. Detecting real
edges as the places where the smoothed image still bends, and smoothing those only along the edge, kept 92% of the
beam's edge sharpness (69% with the Gaussian) with the same ramp smoothing (0.79 against 0.78 rms).

**Outcome:** the edge-aware smoothing replaces the Gaussian, the gate tests its result and the share of edge texels, and
the coverage log line lists the refusals by reason. The offline check measures the captured glow and beam and scans the
captured textures. Not yet tested in game.
