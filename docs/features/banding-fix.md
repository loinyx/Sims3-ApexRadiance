# Banding Fix

The Banding Fix removes the visible colour steps (banding) in smooth gradients of the 3D world: the pool of light a lamp
throws on a wall or floor, the fall-off of room light and soft shadows. Instead of rings, light fades smoothly. It does
this with a fixed, invisible grain added to the scene before the game rounds its colours to 8 bits. Menus, the HUD and
the Apex overlay are not touched. Its *High-precision scene* (on by default) draws the 3D scene in 16-bit colour and
rounds it once, with one grain, so blended passes no longer add their own roundings.

## Status

| | |
|---|---|
| Availability | Released in 2.2.0 (Experimental, with a "Still being tested" note on its page, until 2.15.0). Its own page, Smooth gradients off by default and copies made at a shader's first scene draw: Released in 2.7.0. Smooth gradients and Moving grain removed, High-precision scene on by default: after 2.15.6, not yet released |
| Default | On for new configurations |
| Menu | Image > Banding Fix (card *Banding Fix*: a before / after picture of a stepped and a smoothed gradient that follows the current state, then *Grain* with Strength and *Precision* with High-precision scene and its status; Live counters in the development build); Overview > Image > Banding Fix |
| Configuration | `[patches.SceneDither]` in `ApexRadiance.toml` |
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
is fixed per pixel, so nothing flickers. The noise never reaches more than half a step past pure black or pure white,
so those stay clean, and above mid-grey, where steps are hardest to see, it narrows to half a step. The alpha channel,
which the game uses as its bloom mask, is never changed.

1. **Copies on first use.** A shader gets its dithered copy at its first draw in the 3D scene, so shadow, reflection,
   UI and bloom shaders never get one (DXVK keeps every shader object for the whole session). When the game creates a
   shader at an address that held an older one, the older copy is dropped.
2. **ps_3_0 shaders** read the pixel position from `vPos`.
3. **ps_2_0 / ps_2_x shaders** have no pixel position. A copy of the paired vertex shader writes the clip position to a
   free texture coordinate, and the pixel copy (compiled as ps_2_x) converts it to a pixel position.
4. **Draw.** For each draw into the back buffer that uses the depth test (the 3D scene), the copies are bound, the draw
   is issued, and the game's shaders and constant are put back. Additive and multiply blends, which add to a picture
   that already has its grain, get half of it with an uncorrelated pattern.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Banding Fix (card switch) | `enabled` | bool | on | | Turns the grain on |
| Strength | `forca` | float | 100% | 0 to 100% | Peak of the triangular grain in 8-bit steps (100% = plus or minus one step, the full TPDF). 0% adds nothing |
| High-precision scene | `cenaAltaPrecisao` | bool | on (also in every built-in profile) | | Draws the 3D scene into a 16-bit target and resolves it once; see [High-precision scene](#high-precision-scene) |
| Developer > Show covered surfaces | (not saved) | bool | off | | Replaces the grain with a coarse 24-step grain on every covered surface |

Removed after the 2026-10-26 in-game test: *Smooth gradients* (`[qol.picture] deband`, a deband filter in the Picture
pass), which made no visible difference, and *Moving grain* (`graoEmMovimento`, a new grain pattern every frame). Older
files still load: both keys are ignored (`deband` is dropped on the next save; `graoEmMovimento` stays in the feature's
table, as unknown keys do), the grain stays fixed and the Picture pass no longer runs for the deband alone. A saved
`cenaAltaPrecisao = false` (written by earlier builds while it was off by default) keeps the switch off.

Settings apply live. The Developer page also shows the shader coverage counters (under *Shader coverage*): scene
draws with the grain by shader model and by blend class, and the draws left alone (refused, depth `ALWAYS` without
depth write, no pixel shader).

## Compatibility and interactions

- **Ambient Occlusion:** its composite rounds to 8 bits again, so it adds the same grain (Banding Fix Strength and
  phase, the same clamp-aware law) only where the shade changed the pixel, from the R2 pattern with its coefficients
  swapped, which does not correlate with the scene's. With the Banding Fix off, the composite adds no grain. See
  [ambient-occlusion.md](ambient-occlusion.md).
- **Night Lighting:** draws that Night Lighting replaces are re-issued through the device, so their patched shaders get
  their own dithered copies.
- **Picture filters:** the Picture pass applies its own fixed dither (the same law, from interleaved gradient noise,
  which does not correlate with the R2 grain) only where it changed a pixel.
- **Compare with the game** (shortcut) turns the Banding Fix off with the other effects until it is pressed again.
- **Profiles:** the Banding Fix belongs to the Color part of a profile. The three built-in profiles set it
  (`[patches.SceneDither]` on, Strength 100%, High-precision scene on) ([apex_presets.cpp](../../apex_presets.cpp)).
- **Other Apex passes:** any new Apex pass that draws into the back buffer with the depth test on (and not `ALWAYS`
  without depth write) receives the grain.

## Limitations

- Shaders that cannot be patched keep their banding: shaders without a colour write, with subroutines, with relative
  constant addressing, with no free register, or with no free texture coordinate (ps_2_0). ps_1_x shaders are not
  patched.
- Some surfaces use ps_2_0-only techniques (Sky, Sky_Reflection, Ceiling, Rug, Foliage, pool water, SimEyes,
  FloorThickness, InteriorWall with strobe or black light). They are covered only when the vertex/pixel copy pair can be
  made. With the High-precision scene active they are drawn into the 16-bit target too and get the resolve's grain.
- The first session after installing may stutter slightly more while DXVK builds pipelines for the copies; its state
  cache keeps them afterwards. *Expected, not measured.*

## Technical reference

**ps_3_0 patch** (`ShaderPatches::AddDither`, a pure function tested offline). Every write to `oC0` goes to a free temp
`rO`. At the end (`GrainTail`, shared with the ps_2_x path; x = `rO.rgb`):

```
u = frac(dot(vPos, (0.7548777, 0.5698403)) + phase)   // the R2 low-discrepancy pattern
r = 2u - 1;  t = sign(r) * (1 - sqrt(1 - |r|))        // inverse CDF of the triangular distribution, in (-1, 1)
n = lerp(t, r / 2, saturate(2x - 1))                  // above mid-grey: towards a uniform +-0.5 step
a = min(amount, min(x, 1 - x) + 0.5 / 255)            // per channel: never more than half a step past 0 or 1
oC0.rgb = x + n * a
oC0.a   = rO.a                                        // bloom mask untouched
```

Two `def` constants and three temps are added above the shader's own; `dcl vPos.xy` is added when missing. The amount
constant `cA = (strength / 255, w/2, h/2, phase)` is set per draw; `phase` is 0 (a fixed grain; Moving grain set it
to `frac(frame x 0.6180339887)` until it was removed), plus 0.19 for the half grain of additive and multiply blends (the R2 pattern shifted by
0.19 correlates 0.006 with the unshifted one, offline). The clamp bound keeps pure black and white exact and every grey's
mean (noise below -0.5 or above 255.5 would be clipped and bias it); within half a level of black or white the grain is
narrower than a full TPDF, a bias of about 0.02 level there. Shaders that are not ps_3_0, have no colour write, use
subroutines or `ret`, use relative constant addressing or have no free register are refused and left alone.

**ps_2_x path** (`ShaderPatches::AddDither2`, `AddScreenPosVs`):

- The pixel copy reads the clip position from TEXCOORDk (k = the highest texture coordinate the pixel shader does not
  use) and computes the pixel as `(ndc.x * w/2 + w/2, -ndc.y * h/2 + h/2)`, `ndc = tk.xy / tk.w`, with `w`, `h` from the
  viewport (`cA = (amount, w/2, h/2, phase)`). This is the same integer position as `vPos`: the two grains match pixel
  for pixel on native D3D9 and DXVK. It then applies the same grain. One constant per instruction (ps_2_0 rule); `oC0`
  is written once at the end.
- The copy is ps_2_x (`0xFFFF0201`): the grain does not fit the 64 arithmetic slots of some ps_2_0 shaders.
- `AddScreenPosVs(vs, k)` redirects every `oPos` write to a free temp and writes it to both `oPos` and `oTk`
  (vs_1_1 and vs_2_x only; vs_3_0 pairs with ps_3_0).
- At vertex shader creation the copy for k = 7 (the usual value) is made; copies for other k are made at the first
  draw. Both copies are bound at the draw. A ps_2_x copy without a matching vertex copy is not used (counted as
  "vertex shader" in the coverage).

**Creation and caching.** `CreatePixelShader` and `CreateVertexShader` callbacks run after every other callback
(priority 1000). They call `D3D9Hooks::CallOriginalCreate*Shader` to create the game's shader, drop the copies of a
reused address (`Forget`, `ForgetVs`) and return `Skip`. Copies are made at a shader's first draw in the 3D scene
(`CopyOf`, `VsCopyOf`), from its bytecode read with `GetFunction`. Pairs are kept by the game shader's pointer.
Everything is released when the feature turns off. Lookups go through a `ShaderLookupCache`.

**Draw gate** (DIP/DP callbacks, priority 1000, after every observer): the copy is bound only when render target 0 is the
back buffer, `D3DRS_ZENABLE` is on, and the draw is not `ZFUNC ALWAYS` without depth write (the rule PostScene and
Picture use to find the end of the scene). This keeps the grain out of:

- the mouse-pick pass (its own 16x16 target with object IDs and packed depth in the colour);
- shadows, reflections and bloom (their own targets; the bloom is added to the back buffer with the depth test off);
- the UI, which draws with the depth test off or with it on, `ZFUNC ALWAYS` and no depth write: it copies the back
  buffer into a 2048x1024 target and redraws tiles of it about 137 times per frame (a light capture shows a ps_2_0
  redraw computing `x (1 + 0.4) / (x + 0.4)` under a panel), where a dither would feed on itself. Until 2.15.6 the
  depth-on UI passes got the grain.

**Blend classes** (`BlendOf`, read with `GetRenderState` only for draws that get the grain; no hook follows the blend
states): blend off and `SRCALPHA` / `INVSRCALPHA` get the full grain; additive (`DESTBLEND ONE`), multiply (`SRCBLEND
DESTCOLOR` or `ZERO`, as the game's decals `DESTCOLOR` / `SRCCOLOR` and `DESTCOLOR` / `ONE`) and every other blend get
half of it with the phase shifted by 0.19, since they add their grain to a target that already has its own.

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

## High-precision scene

On by default since the in-game test of 2026-10-26 (the card showed the target active, with most resolves at a
back-buffer copy). The grain above hides the 8-bit rounding of each draw, but each blend still
rounds: a pixel lit by several passes collects one rounding (and one grain) per pass. This switch removes those roundings
instead of hiding them.

**How.** Source: [`framework/scene_target.cpp`](../../framework/scene_target.cpp), pure parts in
[`framework/scene_target_logic.h`](../../framework/scene_target_logic.h).

1. **Target.** A `D3DFMT_A16B16G16R16` render-target texture the size of the back buffer (8 bytes a pixel: about 66 MB
   at 3840x2160), `D3DPOOL_DEFAULT`, released before a device Reset and made again at the next frame. 16-bit unsigned
   normalised clamps to 0..1 like the 8-bit back buffer, so the game's blending gives the same result with more
   precision.
2. **Substitution.** From Present until the scene is resolved, `SetRenderTarget(0, back buffer)`, from the game or from
   any Apex callback (`D3D9Hooks::SetRenderTargetSubstitute`, which `CallOriginalSetRenderTarget` also follows), binds
   the target. Every SetRenderTarget callback still sees the back buffer, and `GetRenderTarget` reports the back buffer
   while the target is bound, so the identity checks of PostScene, Picture and the Banding Fix are unchanged. The target
   is bound again at every Present, after every other Present callback, so a Clear before the frame's first
   SetRenderTarget lands in it. Other targets (mouse pick, shadows, reflections, bloom) are never touched.
3. **Resolve.** One full-screen draw of the target into the back buffer, at the first of: the post-scene boundary (a
   PostScene effect ordered before Ambient Occlusion, Edge Smoothing, Depth Blur and Picture), a `StretchRect`,
   `ColorFill` or `GetRenderTargetData` touching the back buffer (the UI's back-buffer copies, the hidden-UI colour tile,
   Apex's own scene copies, read-backs), the game's EndScene while the target is bound, Present. After it, the frame
   continues on the real 8-bit back buffer, as without the switch.
4. **Grain once.** The resolve adds the Banding Fix's triangular grain (its Strength) once. Its
   amplitude never reaches past 0 or 255 (`min(1, x, 255 - x)` levels), and a value that is exactly an 8-bit level (the
   UI, flat colours) passes unchanged. Alpha, the bloom mask, is copied without grain. While the scene goes into the
   target, the per-draw grain is not bound (`SceneDither` skips its copies).
5. **Refused** (status on the card, nothing substituted): the game's own Edge Smoothing on (a multisampled back buffer;
   its depth-stencil is multisampled too), a back buffer other than `A8R8G8B8`, no 16-bit render target with
   post-pixel-shader blending (`CheckDeviceFormat` with `D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING`), a depth format that
   does not match it (`CheckDepthStencilMatch`), or the target not created.
6. **Hooks.** `GetRenderTargetData`, `ColorFill` and `GetRenderTarget` are detoured (`ExtraHooks::EnsureTargetHooks`)
   only when the switch first turns on. With the switch off the frame callbacks return at once.

**Status.** The card's *Status* row shows a pill (Active, Starting, Unavailable or Off) with the video memory of the
target or the reason it is not used. The development build adds the resolves by cause and the CPU time to issue one,
under *Live counters*. The texture is counted in Apex's Direct3D memory (developer
`memory_owners`).

**Not covered.** A `LockRect` of a lockable back buffer, `UpdateSurface` into it, or `GetFrontBufferData` would see
the back buffer before the resolve; the game is not known to use them (*not verified*). A scene draw with
`D3DRS_SRGBWRITEENABLE` on would not get the sRGB conversion in the 16-bit target (*not verified* whether the game
does this). A scene drawn in several
BeginScene / EndScene pairs is resolved at the first EndScene with the target bound; what follows draws on the 8-bit
back buffer as before.

**Rejected for this switch.**

- FP16 (`A16B16G16R16F`): does not clamp, so blends that rely on the 8-bit clamp would differ.
- `A2B10G10R10`: 2 bits of alpha break the bloom mask kept in the back buffer's alpha.
- 10-bit output (an `A2R10G10B10` swap chain): the same 2-bit alpha, and the UI and every Apex pass read the back
  buffer as 8-bit; not part of this switch.

## Rejected approaches

- Uniform grain of plus or minus half a step: an improvement but not complete; replaced by the triangular grain.
- Interleaved gradient noise as the scene pattern (until 2.15.6): replaced by R2, whose 2x2 and 4x4 block means vary
  less (0.017 and 0.0014 against 0.019 and 0.0034 for IGN, per-pixel variance 0.167, offline), at the same cost.
- Subtracting half a pixel from the ps_2_x position: the rebuilt position already equals `vPos` (24576 / 24576 pixels
  identical on native D3D9 and DXVK); the shift would misalign the two grains.
- Magenta view of uncovered surfaces: turned whole-screen passes opaque magenta; replaced by *Show covered surfaces*.
- ps_3_0-only coverage: more than half of the scene, walls included, draws with ps_2_x shaders.
- Strength range 0.5 to 3 steps: covered every surface with visible grain; replaced by 0 to 100%.
- The same grain pattern in the scene and the AO composite: doubled the grain.
- *Smooth gradients* (a deband filter in the Picture pass, averaging neighbours within `1 + 6.25 x setting` levels):
  no visible difference in game (2026-10-26); removed with its full-screen pass.
- *Moving grain* (a new grain pattern every frame): removed after the same in-game test; the grain is fixed.

Details in [history](../history/banding-fix.md).

## See also

- [Validation](../validation/banding-fix.md)
- [History](../history/banding-fix.md)
- [Picture filters](picture-filters.md)
- [Engine: shaders](../engine/shaders.md)
