# Banding Fix: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/banding-fix.md](../features/banding-fix.md).

### 2026-09-30: report of rings around lamp light

**Context:** maintainer feedback: colours were uneven, mainly in lit areas; an interior wall showed
rings around a lamp's pool of light, on an OLED display (AW3225QF, DCI-P3).

**Finding:** lossless captures (`cor_1/2/6.bmp`) showed black at 0 and white at 255, so the range is correct. In smooth
dark areas 64 to 89% of neighbour steps were 1 level and 11 to 36% were 2 or more: the rings are the 8-bit rounding of
dark gradients. Wide gamut is not the cause; reaching P3 would need an HDR / scRGB output.

**Outcome:** a dither in the scene's pixel shaders, before the rounding, was chosen (commit f578f43, ps_3_0 only).

### 2026-09-30: uniform grain replaced by a triangular grain

**Context:** the first build (57a0a09d) used a uniform grain of plus or minus half a step. Maintainer feedback: a partial
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

**Context:** maintainer feedback: on many walls the fix had almost no effect. The magenta view turned the whole screen magenta: a
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
