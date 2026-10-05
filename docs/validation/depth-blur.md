# Depth Blur: validation

The feature is described in [features/depth-blur.md](../features/depth-blur.md). It must:

- Blur only the 3D scene, before any interface draw, and leave pixels with zero blur bit-identical.
- Produce identical output for identical input (no temporal noise).
- Keep the game unaware of the depth swap, and restore everything it touches.
- Do no blur or debug pass outside an active loaded world or while the loading window is attached, and resume only after
  3 s of continuous activity.
- Pause, with a clear status, while the back buffer is multisampled.
- Fade out in the map view and snap the focus when it closes.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/loading_gate_test`](../../tools/loading_gate_test/run.ps1) | The production `WorldSession` reader (`world_session.h`) with mocked game memory and UI root, plus the guard of `BlurEffect` and the menu's `UpdateHint` extracted from the sources: loaded world, missing manager, loading window present or absent, unrecognised getter, settle interval, focus snap and map-fade reset | `tools\loading_gate_test\run.ps1` (writes only to `%TEMP%\ApexLoadingGateChecks`) | MSVC x86 (`compiler_env.ps1`); no game, no device |
| [`tools/post_scene_test`](../../tools/post_scene_test/README.md) | Post-scene chain: order, once per frame, hidden-UI fallback, invalid depth without consuming the effects, no retry over drawn UI, recovery, short scenes, internal draws, reset | See the harness README | Hidden native D3D9 device |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-05 | f575f0a | `loading_gate_test` | 91 mock checks passed, including a loaded world behind the loading window | None |
| 2026-10-04 | PR #2 | `post_scene_test` | 12 passed | Native D3D9 |

## In-game test plan

1. Depth Blur page with the game's Edge Smoothing off. **Expected:** no error note; the cost chip shows a value after a
   few frames. Log: `[DepthBlur] Installed`, `[ExtraHooks] Installed (...)`, `[DepthBlur] Resources ready (3840x2160,
   INTZ depth swapped in, blur targets A16B16G16R16F, focus target R32F)`, `[MapView] Camera_IsMapViewModeEnabled at
   0x0073e060`. A `Shader ... failed to compile` line is a build problem.
2. Focus Auto, Developer read-out. **Expected:** "Focus: Auto" and a plausible metre value for a Sim a few metres away;
   moving the camera eases the value; pointing at the sky keeps the last value.
3. Developer > Show blur amount. **Expected:** black around the centre object, white far away and on the sky; violet
   focus window. Sharp area Small / Medium / Large grows the black zone.
4. Blur amount 0% to 100%. **Expected:** progressive softening, no double image in the transition.
5. Pan slowly. **Expected:** silhouettes against the blurred background do not shimmer or crawl.
6. Lamps at night in the background. **Expected:** bright blobs with Glowing lights on; no banding in dark skies.
7. Pie menu over a blurred background. **Expected:** the menu stays sharp.
8. Press M (map view). **Expected:** the blur fades out within 0.3 s; closing the map refocuses at once.
9. Alt-tab or change resolution. **Expected:** "Recreating after a video change...", then active; the focus snaps.
10. Focus Fixed. **Expected:** the blur starts at the set distance.
11. Load a save, travel between worlds, return to the main menu. **Expected:** no blur over loading screens or the
    loading window; the blur returns about 3 s after the world is shown, with a fresh focus.
12. Turn the game's Edge Smoothing on. **Expected:** the effect pauses with the MSAA status; the amber compatibility card
    lists Depth Blur.
13. Hide the game UI (F10). **Expected:** the blur still applies.

## Confirmed in game

- Before the half-resolution rewrite: the INTZ swap, the post-scene insertion point, the map-view getter and fade, the
  Fixed curve's look, and the pause while the game's Edge Smoothing is on.
- From captures: `d = A - near * A / z`, A = 1.00008, near 0.2 to 0.3 (LightProbe-m80).

## Open checks

- Loaded-world and loading-window guard on real loading transitions, and on non-Steam builds.
- The look of Auto focus, the 25th-percentile choice, the Sharp area presets, the lamp weight 2, the composite's
  `smoothstep(0.1, 1.0)` and the similarity falloff.
- `A16B16G16R16F` filtering and R32F 1x1 targets under DXVK 3.1.1 (expected supported; the code checks and falls back),
  and `GetRenderTargetData` from a float 1x1 target (developer read-out only).
- Shader slot counts (estimates) and the GPU cost at 4K.
- Distant foliage speckles: compare the debug view and quality levels on the same camera before changing the composite
  fallback.
- Interiors where a depth-off back-buffer draw happens mid-scene.
