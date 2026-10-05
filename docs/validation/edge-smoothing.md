# Edge Smoothing: validation

The feature is described in [features/edge-smoothing.md](../features/edge-smoothing.md). It must:

- Smooth only the 3D scene, before any game or Apex interface is drawn.
- Keep the back buffer's alpha and restore every render state, sampler, texture, constant, shader, stream and viewport
  it touches.
- Pause, with a clear status, while the back buffer is multisampled, and resume after the game's MSAA is turned off.
- Fall back to colour-only edges when the shared depth is not the bound depth-stencil.
- Never sharpen pixels it smoothed.
- Ignore removed settings (`temporal`, `combineMsaa`) found in old configurations or profiles.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/post_scene_test`](../../tools/post_scene_test/README.md) | Post-scene chain: order, once per frame, hidden-UI fallback, depth mismatch without consuming the effects, no retry over drawn UI, recovery, reset | See the harness README | D3D9 (hidden native device) |
| Offline SMAA checks (scratch, not in the repository) | Five spatial presets, flat and diagonal fields, alpha and state restoration, equal-brightness colour detection, rejection of native MSAA targets | Built and run by hand | D3D9 GPU |
| Offline `fxc` compiles (scratch) | Instruction slot counts per pass and level | `fxc /T ps_3_0 /O3` | Windows SDK |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-04 | PR #2 | Post-scene chain | 12 passed | None |
| 2026-10-02 | 2.5.5 candidate | Offline SMAA checks (spatial only) | Passed (five presets, colour detection, MSAA rejection) | Native D3D9 |
| 2026-10-02 | 2.5.4 temporal candidate | Pool protection and resolve | Captured pool original and grain fallback accepted, ordinary jitter retained, resolve compiled and accepted, 108 phase / inverse / reprojection cases passed (did not reproduce the artefact; feature later removed) | Native D3D9 |
| 2026-09-30 | 31ab53c8 | `fxc` | SMAA edges 35 to 43 slots (colour), weights 442 (loop, unchanged), blend 62; FXAA Extreme 694 slots (High 538) | fxc |
| 2026-09-28 | combined build | `fxc` | SMAA 35 / 442 / 62 slots for the three passes (preset not recorded) | fxc |
| 2026-09-26 | combined build | `fxc` | FXAA 265 / 382 / 538 slots for Fast / Balanced / High | fxc |

## In-game test plan

1. Game Options > Graphics > Edge Smoothing off; Edge Smoothing on in Apex. **Expected:** no error note; GPU cost chip
   appears after a few frames. Log: `[EdgeSmoothing] Installed`, `[EdgeSmoothing] Resources ready (WxH)`.
2. Developer mode > Show smoothed pixels in red. **Expected:** roof, fence and furniture edges red; the interface never
   red. *Frames smoothed* increases every frame.
3. Compare presets on fences, roof edges and power lines, FXAA and SMAA. **Expected:** Ultra and Extreme catch fainter
   and longer edges; Extreme straightens long, nearly horizontal edges at 4K.
4. Open a pie menu over the scene. **Expected:** menu text stays sharp.
5. *Edges from depth* on, at night against same-coloured walls. **Expected:** outlines smoothed; no depth note on the
   card.
6. *Sharpen textures* 0% and 100%. **Expected:** textures crisper, smoothed edges unchanged.
7. Alt-tab and change resolution. **Expected:** "Recreating after a video change..." then active again.
8. Turn the game's Edge Smoothing on. **Expected:** the effect pauses, the amber compatibility card lists Edge
   Smoothing, the Conflicts page appears and the Overview row shows *Waiting for game settings*. Turn it off: the card
   disappears and smoothing returns.
9. Hide the game UI (F10). **Expected:** smoothing still applies.
10. Frame Capture (developer mode). **Expected:** the StretchRect and DrawPrimitiveUP passes are listed through the
    ExtraHooks observers ([dev-tools/frame-capture.md](../features/dev-tools/frame-capture.md)).
11. At 1080p. **Expected:** the DSR / VSR note on the card.

## Confirmed in game

- Combined build, SMAA Ultra at 3840x2160: `[EdgeSmoothing] Resources ready (3840x2160)`; used in play.
- Game anti-aliasing notice: built and translated; first-load, device-reset and narrow-layout behaviour not yet checked
  in game.

## Open checks

- GPU cost per preset at 1080p, 1440p and 4K.
- Extreme presets (SMAA and FXAA) in game.
- Spatial-only SMAA with colour edges on High and Ultra: gameplay look and frame rate.
- The game's native screenshot key (with the Apex screenshot shortcut disabled): whether its image contains the smoothed
  scene.
- The amber notice on first load, after a device reset and in a narrow menu layout.
- Interiors where a depth-off back-buffer draw happens mid-scene.
