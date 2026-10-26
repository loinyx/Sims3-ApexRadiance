# Banding Fix: validation

The feature is described in [features/banding-fix.md](../features/banding-fix.md). It must:

- Add a grain of at most one 8-bit step (at 100% Strength) to the RGB of 3D scene draws, and never change alpha.
- Keep the average colour: a grey between two levels must come out with the same mean, spread over neighbouring levels
  in the triangular proportions.
- Leave pure black and pure white unchanged (no specks).
- Produce the same pattern every frame unless Moving grain is on.
- Leave every non-scene target (pick pass, shadows, reflections, bloom), every depth-off back-buffer draw and every
  depth-on draw with `ZFUNC ALWAYS` and no depth write (the UI) unchanged.
- Leave a shader unchanged when it cannot be patched, and put the game's shader and constant back after every draw.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/banding_fix_check`](../../tools/banding_fix_check/README.md) | `AddDither` over every ps_3_0, `AddDither2` over every ps_2_0 and `AddScreenPosVs` over every vs_1_1 / vs_2_x of the game's shader package, each copy created on the device; mean, level weights, black and white of the grain; ps_2_x pair against ps_3_0 pixel for pixel; Smooth gradients and the Picture dither (`PicturePS`) | `run.ps1 -Precomp <Shaders_Win32.precomp> [-D3d9 <DXVK d3d9.dll>]` | Game `Shaders_Win32.precomp`, D3D9 GPU |
| `techver.pl` (not in the repository) | ps_2_0 / ps_3_0 variant counts per technique | Perl over the shader package | Game shader package |

`tools/banding_fix_check` replaces the scratch harnesses of the investigation (`dithertest.cpp`, `dithertest2.cpp`,
`tpdftest.cpp`), which were not kept. It counts unique shaders (by size and hash): 4907 ps_3_0, 3104 ps_2_0, 5 ps_1_x
and 4348 vertex shaders.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-26 | e4b6a2d | `banding_fix_check` | ps_3_0 4907 / 4907 and ps_2_0 2996 / 3104 patched and accepted (108 with no free register), as at 92c1454; vertex copies 4036 (2962 with t7), 4035 accepted (4036 on DXVK). Black and white exact (at 92c1454: 9.7% of black pixels at level 1, 12.5% of white at 254); grey 20.40 / 100.70 / 200.25 -> 20.354 / 100.657 / 200.244; 0.30 -> 0.228 (theory 0.28 with the GPU's -0.05 conversion bias seen at every grey). ps_2_x pair = ps_3_0: 24576 / 24576 pixels. Smooth gradients 8%: 1-level steps 0 pixels changed with the old threshold, column jump 1.000 -> 0.281 now, mean 41.491 (41.500); flat grey unchanged | Native D3D9 and DXVK (the game's d3d9.dll) |
| 2026-09-30 | 7d5615d | `tpdftest.cpp` | Grey 20.40 / 100.70 levels comes out as 20.355 / 100.657 on average (20 / 101 without); level weights about 1/8, 3/4, 1/8 | Native D3D9 |
| 2026-09-30 | f578f43 | `dithertest.cpp` | ps_3_0: 4907 / 4907 patched and accepted by a native HAL device (strict validator); 3104 ps_2_0 and 7 ps_1_1 left alone (no `vPos`) | Native D3D9 |
| 2026-09-30 | 53d1091 | `dithertest2.cpp` | ps_2_0: 2996 / 3104 patched and created (108 with no free texture coordinate, temp or constant); 519 of 2308 failed when the copy was ps_2_0, hence ps_2_x. vs_2_0: 4035 / 4338 (2961 with t7, 1074 with a lower k, 303 refused). A grey through a patched vs_2_0 + ps_2_0 pair gives the same means and triangle as ps_3_0 (20.355 / 63.971 / 100.657) | Native D3D9 |

## In-game test plan

1. Interior wall at night lit by one lamp, Banding Fix off then on. **Expected:** rings around the pool of light turn
   into a smooth gradient.
2. Developer > Show covered surfaces. **Expected:** a coarse grain on walls, floors and objects; menus and the HUD clean.
3. Developer page coverage counters in an interior and outdoors. **Expected:** most scene draws "with the grain"; few
   ps_2_x without a vertex copy.
4. Strength 0% and 100%. **Expected:** 0% looks like Banding Fix off; 100% shows no visible noise at normal viewing
   distance.
5. Moving grain on at a high and a low frame rate. **Expected:** invisible at high frame rates, a faint shimmer at low
   ones.
6. Smooth gradients at 0% and 100% on a sky gradient with Picture off. **Expected:** steps in the sky soften; menus are
   unchanged.
7. Click objects and Sims (pick pass). **Expected:** picking works normally.
8. Open a panel over the world (the pie menu, the build catalog) with Show covered surfaces on. **Expected:** no coarse
   grain on the panels' glass; the Developer counters show the depth-`ALWAYS` draws left alone.
9. Pure black and white areas (a dark night sky, a white wall in sunlight). **Expected:** no isolated lighter or darker
   pixels.
10. Smooth gradients at the default 8% with Picture off, on a sky gradient or a dim wall. **Expected:** steps soften;
    textures and edges keep their detail.

## Confirmed in game

- 2026-09-30, build 57a0a09d (uniform grain of plus or minus half a step): partial improvement only.
- 2026-09-30 evening, developer log on the first triangular build: 123 to 130 scene draws per frame dithered and 166 to
  172 drawn with ps_2_x (0 ps_3_0 refused), and only 109 to 139 copies made in the session. This led to the ps_2_x path.
- 2026-09-30: at 223% Strength (old range) the grain showed as fine specks, which led to the 0 to 100% range and to
  the shifted pattern in the AO composite.

## Open checks

- In-game look and coverage of the ps_2_x path on walls (build 53d1091 or later).
- Extra stutter on the first session under DXVK while pipelines for the copies are built (expected, not measured).
- GPU cost of the extra instructions per pixel (not measured; the grain is 18 instructions per pixel, 13 before, and
  the copies now need three free temps).
- In game: the blend-class counts, the look of the half grain on additive and multiply draws, the absence of grain
  under the UI panels, and Smooth gradients at 8% (now an active full-screen pass) and its GPU cost (built 2026-10-26,
  not yet run in game).
