# Banding Fix

The Banding Fix removes the visible colour steps (banding) in smooth gradients of the 3D world: the pool of light a lamp
throws on a wall or floor, the fall-off of room light and soft shadows. Instead of rings, light fades smoothly. It does
this with a fixed, invisible grain added to the scene before the game rounds its colours to 8 bits. Menus, the HUD and
the Apex overlay are not touched. The Banding tab of the Color page also holds *Smooth gradients*, a filter that
softens the steps the grain cannot reach (the sky and some other surfaces).

## Status

| | |
|---|---|
| Availability | Experimental: released in 2.2.0; the Banding tab shows a "Still being tested" note |
| Default | On for new configurations |
| Menu | Image > Color > Banding (card *Banding Fix*); Overview > Image > Banding Fix |
| Configuration | `[patches.SceneDither]` in `ApexRadiance.toml` (Smooth gradients: `[qol.picture] deband`) |
| Source | [`features/scene_dither.cpp`](../../features/scene_dither.cpp), [`features/shader_patches.cpp`](../../features/shader_patches.cpp) (`AddDither`, `AddDither2`, `AddScreenPosVs`) |

## The problem

The game draws its 3D scene straight into an 8-bit back buffer, 256 levels per channel. A lamp's pool of light is a
long, smooth gradient in the dark range, where one level is a large step in brightness, so every level boundary shows
as a ring. High-contrast displays such as OLED panels show the rings sharply. Lossless captures show that the game's
range is correct (black reaches 0, white 255): the steps come from the 8-bit rounding of dark gradients, not from a
lifted black level or a clipped range. A wide-gamut display is not the cause either: the game outputs sRGB.

## How Apex Radiance solves it

Apex Radiance makes a copy of every pixel shader the game uses for its 3D scene. The copy adds triangular noise of at
most one 8-bit step (at 100% Strength) to the colour, derived from the pixel position, just before the output is
rounded. The rounding then turns a hard ring into a fine grain that the eye averages into a smooth gradient. The pattern
is fixed per pixel, so nothing flickers. The alpha channel, which the game uses as its bloom mask, is never changed.

1. **Shader creation.** When the game creates a pixel shader, Apex creates the game's shader first (to learn its
   pointer), then a dithered copy. Shaders that already existed when the feature was turned on get their copy at their
   first draw.
2. **ps_3_0 shaders** read the pixel position from `vPos`.
3. **ps_2_0 / ps_2_x shaders** have no pixel position. A copy of the paired vertex shader writes the clip position to a
   free texture coordinate, and the pixel copy (compiled as ps_2_x) converts it to a pixel position.
4. **Draw.** For each draw into the back buffer with the depth test on (the 3D scene), the copies are bound, the draw is
   issued, and the game's shaders and constant are put back.
5. **Smooth gradients** is a separate deband filter in the Picture pass ([picture-filters.md](picture-filters.md)). It
   follows the Banding Fix switch: with Picture off, the Picture pass still runs with only the deband while the Banding
   Fix is on and Smooth gradients is above 0.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Banding Fix (card switch) | `enabled` | bool | on | | Turns the grain on |
| Strength | `forca` | float | 100% | 0 to 100% | Peak of the triangular grain in 8-bit steps (100% = plus or minus one step, the full TPDF). 0% adds nothing |
| Moving grain | `graoEmMovimento` | bool | off | | A new grain pattern every frame. High frame rates average it away; at low frame rates it shows as a faint shimmer |
| Smooth gradients | `[qol.picture] deband` | float | 100% | 0 to 200% | Deband filter of the Picture pass (threshold `deband x 6/255`). Runs only while the Banding Fix is on |
| Developer > Show covered surfaces | (not saved) | bool | off | | Replaces the grain with a coarse 24-step grain on every covered surface |

Settings apply live. The Developer page also shows the shader coverage counters (under *Shader coverage*).

## Compatibility and interactions

- **Ambient Occlusion:** its composite rounds to 8 bits again, so it adds the same grain (Banding Fix Strength and
  phase) only where the shade changed the pixel, with a shifted pattern so the two grains do not add up. With the
  Banding Fix off, the composite adds no grain. See [ambient-occlusion.md](ambient-occlusion.md).
- **Night Lighting:** draws that Night Lighting replaces are re-issued through the device, so their patched shaders get
  their own dithered copies.
- **Picture filters:** the Picture pass applies its own fixed dither when it grades; Smooth gradients lives in that
  pass.
- **Compare with the game** (shortcut) turns the Banding Fix off with the other effects until it is pressed again.
- **Profiles:** the Banding Fix belongs to the Color part of a profile.
- **Other Apex passes:** any new Apex pass that draws into the back buffer with the depth test on receives the grain.

## Limitations

- Shaders that cannot be patched keep their banding: shaders without a colour write, with subroutines, with relative
  constant addressing, with no free register, or with no free texture coordinate (ps_2_0). ps_1_x shaders are not
  patched.
- Some surfaces use ps_2_0-only techniques (Sky, Sky_Reflection, Ceiling, Rug, Foliage, pool water, SimEyes,
  FloorThickness, InteriorWall with strobe or black light). They are covered only when the vertex/pixel copy pair can be
  made; Smooth gradients is the fallback for the sky.
- The first session after installing may stutter slightly more while DXVK builds pipelines for the copies; its state
  cache keeps them afterwards. *Expected, not measured.*

## Technical reference

**ps_3_0 patch** (`ShaderPatches::AddDither`, a pure function tested offline). Every write to `oC0` goes to a free temp
`rO`. At the end:

```
oC0.rgb = rO.rgb + t * amount        // t = triangular noise in (-1, 1)
oC0.a   = rO.a                       // bloom mask untouched
u = IGN(vPos) = frac(52.9829189 * frac(dot(vPos, (0.06711056, 0.00583715))))   // interleaved gradient noise
r = 2u - 1;  t = sign(r) * (1 - sqrt(1 - |r|))                                  // inverse CDF of the triangular distribution
```

Two `def` constants and two temps are added above the shader's own; `dcl vPos.xy` is added when missing. The amount
constant `cA = (strength / 255, w/2, h/2, phase)` is set per draw; `phase` (Moving grain) is
`frac(frame x 0.6180339887)` and is added to the IGN input. Shaders that are not ps_3_0, have no colour write, use
subroutines or `ret`, use relative constant addressing or have no free register are refused and left alone.

**ps_2_x path** (`ShaderPatches::AddDither2`, `AddScreenPosVs`):

- The pixel copy reads the clip position from TEXCOORDk (k = the highest texture coordinate the pixel shader does not
  use) and computes the pixel as `(ndc.x * w/2 + w/2, -ndc.y * h/2 + h/2)`, `ndc = tk.xy / tk.w`, with `w`, `h` from the
  viewport (`cA = (amount, w/2, h/2, phase)`). It then applies the same triangular grain. One constant per instruction
  (ps_2_0 rule); `oC0` is written once at the end.
- The copy is ps_2_x (`0xFFFF0201`): the grain does not fit the 64 arithmetic slots of some ps_2_0 shaders.
- `AddScreenPosVs(vs, k)` redirects every `oPos` write to a free temp and writes it to both `oPos` and `oTk`
  (vs_1_1 and vs_2_x only; vs_3_0 pairs with ps_3_0).
- At vertex shader creation the copy for k = 7 (the usual value) is made; copies for other k are made at the first
  draw. Both copies are bound at the draw. A ps_2_x copy without a matching vertex copy is not used (counted as
  "vertex shader" in the coverage).

**Creation and caching.** `CreatePixelShader` and `CreateVertexShader` callbacks run after every other callback
(priority 1000). They call `D3D9Hooks::CallOriginalCreate*Shader` to create the game's shader, then the copy, and return
`Skip`. Pairs are kept by the game shader's pointer; a reused address replaces the old copy (`ForgetVs` for vertex
shaders). Everything is released when the feature turns off. Older shaders are read with `GetFunction` at their first
draw. Lookups go through a `ShaderLookupCache`.

**Draw gate** (DIP/DP callbacks, priority 1000, after every observer): the copy is bound only when render target 0 is the
back buffer and `D3DRS_ZENABLE` is on. This keeps the grain out of:

- the mouse-pick pass (its own 16x16 target with object IDs and packed depth in the colour);
- shadows, reflections and bloom (their own targets);
- every back-buffer draw with the depth test off: the UI copies the back buffer and redraws strips of it about 135
  times per frame, where a dither would feed on itself.

Never dither a data target (pick IDs, packed depth, light maps); the render-target and depth-test gate is what keeps them
out.

**Coverage of the game's shader package** (`Shaders_Win32.precomp`, 8018 unique pixel shaders): 4907 ps_3_0, 3104 ps_2_0
and 7 ps_1_1. By technique most families have both ps_2_0 and ps_3_0 variants (InteriorWall 13 / 8, InteriorFloor
26 / 32, Phong 255 / 444, SimSkin 97 / 654, TerrainLight 47 / 95). Patch acceptance figures are on the
[validation page](../validation/banding-fix.md).

**Coverage log** (developer mode, the first 12 times, every 20 s of scene with more than 50 scene draws):
`[SceneDither] Last frame, 3D scene draws: N dithered (ps_3_0), M dithered (ps_2_x), ... | copies made: pixel P, vertex V;
refused: ...`. The Developer page shows the same counters for the last frame.

**Shared binder.** The draw hook (`SceneBinder`) was written to also bind jittered vertex copies for temporal
anti-aliasing (constant `c252`). Temporal anti-aliasing is removed from Edge Smoothing and no longer calls it; the code
remains.

## Rejected approaches

- Uniform grain of plus or minus half a step: an improvement but not complete; replaced by the triangular grain.
- Magenta view of uncovered surfaces: turned whole-screen passes opaque magenta; replaced by *Show covered surfaces*.
- ps_3_0-only coverage: more than half of the scene, walls included, draws with ps_2_x shaders.
- Strength range 0.5 to 3 steps: covered every surface with visible grain; replaced by 0 to 100%.
- The same grain pattern in the scene and the AO composite: doubled the grain.

Details in [history](../history/banding-fix.md).

## See also

- [Validation](../validation/banding-fix.md)
- [History](../history/banding-fix.md)
- [Picture filters](picture-filters.md) (Smooth gradients)
- [Engine: shaders](../engine/shaders.md)
