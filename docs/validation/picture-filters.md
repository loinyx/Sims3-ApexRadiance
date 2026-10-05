# Picture filters: validation

The feature is described in [features/picture-filters.md](../features/picture-filters.md). It must:

- Grade only scene pixels when a scene copy exists; write UI pixels back unchanged.
- Add no banding: the grade is re-quantised with a fixed dither below one 8-bit step, the same every frame.
- Restore every device state it touches, and survive a device reset.
- Run the deband-only pass (Picture off, Banding Fix on) only in frames with a scene copy.
- Never save Picture as off because of the compare shortcut.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/post_scene_test`](../../tools/post_scene_test/README.md) | Post-scene chain order and the EndScene fallback before Picture's scene copy | See the harness README | Hidden native D3D9 device |

No harness renders the Picture shader. The pass check (constants and shader read back after each settings change) runs
in game and writes to the log.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-04 | PR #2 | `post_scene_test` | 12 passed | Native D3D9 |
| 2026-09-28 | combined build | Log check | Back buffer `D3DFMT_A8R8G8B8` (21): `[HDR] Resources ready (3840x2160, format 21)` with HDR off | DXVK |

## In-game test plan

1. Color > Picture on. **Expected:** no warning note; GPU cost chip after a few frames; log `[Picture] Resources ready
   (WxH, format N)` and `[Picture] Applied to the game's picture (WxH)`.
2. Mask check: Saturation 0%. **Expected:** the 3D scene turns grey while the pie menu, HUD, tooltips and the Apex menu
   keep colour. Whole screen coloured: no grade applied (check the card's reason). UI grey too: no scene copy (the
   menus-tinted note should show).
3. Interior at night, and outdoors with bloom. **Expected:** both graded; bloom graded with the scene.
4. Before / after. **Expected:** left half original, red line in the middle.
5. Hold the eye button, or B over the menu. **Expected:** the original picture while held.
6. Smooth gradients at 100% on a sky gradient or a dim wall. **Expected:** steps smooth, edges stay sharp.
7. Alt-tab or change resolution. **Expected:** grading continues; `Resources ready` logged again.
8. Take a filtered screenshot (default C). **Expected:** the PNG in Documents `Screenshots` includes the grade and no
   game UI.
9. Compare shortcut, then save a setting from the menu, then compare again. **Expected:** Picture is back on and the
   saved file has `enabled = true`.
10. Game's Edge Smoothing on. **Expected:** after 2 s the menus-tinted note; menus are graded too.

## Confirmed in game

- Combined build (2026-09-28): grading applied after the "last copy wins" fix; the earlier first-copy version graded
  almost nothing (98.7% of pixels treated as UI, HDR_Diag_1/2).

## Open checks

- An SDR diagnostic (dump the frame, the scene copy and the share of pixels treated as UI).
- The game's native screenshot (with the Apex shortcut disabled): whether it contains the grade.
- GPU cost at 1080p, 1440p and 4K.
- Behaviour with the game's own Edge Smoothing on, beyond the menus-tinted note.
- Whether the game ever ends more than one scene per frame with the back buffer bound.
