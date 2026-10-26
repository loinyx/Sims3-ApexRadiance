# Banding Fix: validation

The feature is described in [features/banding-fix.md](../features/banding-fix.md). It must:

- Add a grain of at most one 8-bit step (at 100% Strength) to the RGB of 3D scene draws, and never change alpha.
- Keep the average colour: a grey between two levels must come out with the same mean, spread over neighbouring levels
  in the triangular proportions.
- Leave pure black and pure white unchanged (no specks).
- Produce the same pattern every frame.
- Leave every non-scene target (pick pass, shadows, reflections, bloom), every depth-off back-buffer draw and every
  depth-on draw with `ZFUNC ALWAYS` and no depth write (the UI) unchanged.
- Leave a shader unchanged when it cannot be patched, and put the game's shader and constant back after every draw.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/banding_fix_check`](../../tools/banding_fix_check/README.md) | `AddDither` over every ps_3_0, `AddDither2` over every ps_2_0 and `AddScreenPosVs` over every vs_1_1 / vs_2_x of the game's shader package, each copy created on the device; mean, level weights, black and white of the grain; ps_2_x pair against ps_3_0 pixel for pixel; the Picture dither (`PicturePS`) leaves an unchanged picture alone | `run.ps1 -Precomp <Shaders_Win32.precomp> [-D3d9 <DXVK d3d9.dll>]` | Game `Shaders_Win32.precomp`, D3D9 GPU |
| `techver.pl` (not in the repository) | ps_2_0 / ps_3_0 variant counts per technique | Perl over the shader package | Game shader package |

`tools/banding_fix_check` replaces the scratch harnesses of the investigation (`dithertest.cpp`, `dithertest2.cpp`,
`tpdftest.cpp`), which were not kept. It counts unique shaders (by size and hash): 4907 ps_3_0, 3104 ps_2_0, 5 ps_1_x
and 4348 vertex shaders.

High-precision scene: [`tools/scene_target_test`](../../tools/scene_target_test/README.md) (pure logic of
`framework/scene_target_logic.h`, and its resolve shader on a native D3D9 device; part of
`tools/development/check.ps1 -Suite Smoke`).

Glow sprites: [`tools/sprite_smooth_test`](../../tools/sprite_smooth_test/README.md) (the shader family, DXT decoding,
the smoothing of a glow encoded with the game's DXT5 encoder and the detail gate, from
`features/sprite_smooth_logic.h`; part of `tools/development/check.ps1 -Suite Smoke`). `tools/banding_fix_check` counts
the family in the game's shader package.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-26 | e4b6a2d | `banding_fix_check` | ps_3_0 4907 / 4907 and ps_2_0 2996 / 3104 patched and accepted (108 with no free register), as at 92c1454; vertex copies 4036 (2962 with t7), 4035 accepted (4036 on DXVK). Black and white exact (at 92c1454: 9.7% of black pixels at level 1, 12.5% of white at 254); grey 20.40 / 100.70 / 200.25 -> 20.354 / 100.657 / 200.244; 0.30 -> 0.228 (theory 0.28 with the GPU's -0.05 conversion bias seen at every grey). ps_2_x pair = ps_3_0: 24576 / 24576 pixels. Smooth gradients 8%: 1-level steps 0 pixels changed with the old threshold, column jump 1.000 -> 0.281 now, mean 41.491 (41.500); flat grey unchanged | Native D3D9 and DXVK (the game's d3d9.dll) |
| 2026-09-30 | 7d5615d | `tpdftest.cpp` | Grey 20.40 / 100.70 levels comes out as 20.355 / 100.657 on average (20 / 101 without); level weights about 1/8, 3/4, 1/8 | Native D3D9 |
| 2026-09-30 | f578f43 | `dithertest.cpp` | ps_3_0: 4907 / 4907 patched and accepted by a native HAL device (strict validator); 3104 ps_2_0 and 7 ps_1_1 left alone (no `vPos`) | Native D3D9 |
| 2026-09-30 | 53d1091 | `dithertest2.cpp` | ps_2_0: 2996 / 3104 patched and created (108 with no free texture coordinate, temp or constant); 519 of 2308 failed when the copy was ps_2_0, hence ps_2_x. vs_2_0: 4035 / 4338 (2961 with t7, 1074 with a lower k, 303 refused). A grey through a patched vs_2_0 + ps_2_0 pair gives the same means and triangle as ps_3_0 (20.355 / 63.971 / 100.657) | Native D3D9 |

| 2026-10-26 | glow sprites (this change) | `tools/sprite_smooth_test`, `banding_fix_check` | 31 / 31. Glow 128x128 through the game's DXT5 encoder: decode within 1.7 levels of the source; slope change between neighbouring texels rms / max 2.33 / 6.86 levels (source 0.75 / 2.00), smoothed (sigma 1.5) 0.15 / 0.73, mean alpha 63.409 kept, peak 255, within 3.7 levels of the source. Family in the package: 2 ps_2_x, 1 ps_3_0. The rest of `banding_fix_check` unchanged (66 / 66) | CPU; native D3D9 for `banding_fix_check` |
| 2026-10-26 | 0d9ef9d | `tools/scene_target_test` | 30 / 30. Resolve average error 0.0005 level between 1 and 254 levels (plain rounding: up to 0.498); exact levels unchanged; never past 0 / 255. On the GPU: 0 channels more than one level from the C++ copy, 2.9% one level (float precision of the noise), alpha equal to plain rounding, ramp mean error -0.031 level. The GPU's own float to 8-bit conversion rounds values up to 0.56 level down (2.4% of channels at strength 0): the game's 8-bit path goes through the same conversion | Native D3D9, RTX 4070 Ti SUPER |

## In-game test plan

1. Interior wall at night lit by one lamp, Banding Fix off then on. **Expected:** rings around the pool of light turn
   into a smooth gradient.
2. Developer > Show covered surfaces. **Expected:** a coarse grain on walls, floors and objects; menus and the HUD clean.
3. Developer page coverage counters in an interior and outdoors. **Expected:** most scene draws "with the grain"; few
   ps_2_x without a vertex copy.
4. Strength 0% and 100%. **Expected:** 0% looks like Banding Fix off; 100% shows no visible noise at normal viewing
   distance.
5. Click objects and Sims (pick pass). **Expected:** picking works normally.
6. Open a panel over the world (the pie menu, the build catalog) with Show covered surfaces on. **Expected:** no coarse
   grain on the panels' glass; the Developer counters show the depth-`ALWAYS` draws left alone.
7. Pure black and white areas (a dark night sky, a white wall in sunlight). **Expected:** no isolated lighter or darker
   pixels.
8. The card in Image > Banding Fix at two text sizes and in a narrow menu, with the Banding Fix off, on, at Strength 0%
   and with the High-precision scene on and off. **Expected:** the picture's lower bar is stepped while off or at 0%,
   grained while on, and the caption follows; the Status pill shows Active with its video memory, Off, or Unavailable
   with the reason; nothing overlaps or is cut.

## Confirmed in game

- 2026-09-30, build 57a0a09d (uniform grain of plus or minus half a step): partial improvement only.
- 2026-09-30 evening, developer log on the first triangular build: 123 to 130 scene draws per frame dithered and 166 to
  172 drawn with ps_2_x (0 ps_3_0 refused), and only 109 to 139 copies made in the session. This led to the ps_2_x path.
- 2026-09-30: at 223% Strength (old range) the grain showed as fine specks, which led to the 0 to 100% range and to
  the shifted pattern in the AO composite.
- 2026-10-26, development build of branch `banding/all`: the grain changes and the High-precision scene look right; the
  card showed "On: 63 MB of video memory" with most resolves at a back-buffer copy. Smooth gradients made no visible
  difference. This made the High-precision scene the default and removed Smooth gradients and Moving grain.

## Open checks

- In-game look and coverage of the ps_2_x path on walls (build 53d1091 or later).
- Extra stutter on the first session under DXVK while pipelines for the copies are built (expected, not measured).
- GPU cost of the extra instructions per pixel (not measured; the grain is 18 instructions per pixel, 13 before, and
  the copies now need three free temps).
- In game: the blend-class counts, the look of the half grain on additive and multiply draws, the absence of grain
  under the UI panels (built 2026-10-26, not yet run in game).

High-precision scene (on by default since the 2026-10-26 in-game test, which showed it active; the checks below were
not each run). Each of these is open:

- Rings and grain: night interior lit by one lamp, switch off then on, Banding Fix Strength 100%. Expected: no rings, a
  grain no stronger than with the switch off.
- Picking: click objects, Sims and the terrain. Expected: unchanged (the 16x16 pick target is never substituted).
- Bloom: night lamps and daylight glow. Expected: unchanged (alpha copied exactly).
- Screenshots: the game's own and Apex's (report photo, filtered capture). Expected: the resolved scene, never a black or
  stale picture. The game's screenshot path is not known.
- UI: menus, the HUD glass panels, map view, Build mode and Create a Sim. Expected: unchanged (exact 8-bit values pass
  unchanged; the UI's back-buffer copies resolve first).
- Hidden UI (Tab camera, F10): the colour-tile path resolves before the effects. Expected: effects applied once.
- Resolution change, windowed / full screen switch and alt-tab (device Reset). Expected: no failed Reset, the target made
  again at the new size.
- The game's own Edge Smoothing on. Expected: the "Unavailable" status, nothing substituted.
- ReShade and DXVK: the substitution and the `GetRenderTarget` detour under a wrapped device; DXVK's support of
  `D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING` for `A16B16G16R16`.
- Frame time at 3840x2160 with the switch on and off (the target doubles the scene's colour bandwidth; the resolve is
  one full-screen pass), and the video memory reported (about 66 MB at 4K).
- Developer page resolve causes over a minute in world: most at the boundary, few at Present.
- sRGB writes: whether any scene draw into the back buffer turns `D3DRS_SRGBWRITEENABLE` on (the 16-bit target is not
  known to support sRGB writes; such a draw would come out darker). Not checked.

Glow sprites (built 2026-10-26, not yet run in game). Each of these is open:

- Car headlights at night, Banding Fix off then on, with the High-precision scene on and off. Expected: the rings in the
  glow gone, its size, brightness and colour unchanged; the Developer counters show the sprite draws smoothed.
- Lamp halos, fire, smoke and other particles, light shafts. Expected: smooth glows smoothed, detailed particles left as
  they are (refused as "detail"); none softer than before. Which of them use this shader family is not known: only the
  headlight was captured.
- UI and the HUD. Expected: unchanged (drawn without the depth test or with `ZFUNC ALWAYS`, outside the gate).
- Video memory: the textures smoothed and their size on the Developer page over an evening in a town. Expected: a few
  MB.
- First-use hitch: a frame-time spike when a new glow texture appears (a car arriving at night). Expected: none
  visible at 128x128; not measured.
- DXVK: `LockRect` `READONLY` on a `MANAGED` DXT texture, `SetPrivateData` with `D3DSPD_IUNKNOWN`, and the filtering
  of `A16B16G16R16` (the format reported on the Developer page).
