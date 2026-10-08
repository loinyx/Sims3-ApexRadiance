# Picture filters: validation

The feature is described in [features/picture-filters.md](../features/picture-filters.md). It must:

- Grade only scene pixels when a scene copy exists; write UI pixels back unchanged.
- Add no banding: the grade is re-quantised with a fixed dither below one 8-bit step, the same every frame.
- Restore every device state it touches, and survive a device reset.
- Run the deband-only pass (Picture off, Banding Fix on) only in frames with a scene copy.
- Never save Picture as off because of the compare shortcut.
- Run each filter only while its switch is on and its amount is not 0, never over the interface, and only in a drawn
  world.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/post_scene_test`](../../tools/post_scene_test/README.md) | Post-scene chain order and the EndScene fallback before Picture's scene copy | See the harness README | Hidden native D3D9 device |

No harness renders the Picture shader. The pass check (constants and shader read back after each settings change) runs
in game and writes to the log.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-07 | 2.11.0 release work | `post_scene_test` | 49 passed | Native D3D9 |
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
6. Smooth gradients (Banding Fix page) at 100% on a sky gradient or a dim wall. **Expected:** steps smooth, edges stay sharp.
7. Alt-tab or change resolution. **Expected:** grading continues; `Resources ready` logged again.
8. Take a filtered screenshot (default F8). **Expected:** the PNG in the chosen Screenshots folder includes the grade
   and no game UI.
9. Compare shortcut, then save a setting from the menu, then compare again. **Expected:** Picture is back on and the
   saved file has `enabled = true`.
10. Game's Edge Smoothing on. **Expected:** after 2 s the menus-tinted note; menus are graded too.
11. Filters tab: turn on each filter in turn at its default. **Expected:** the look described on its card; the pie menu,
    tooltips and the Apex menu keep their colours; no box around the pie menu and no grey square at its Sim portrait.
12. Atmospheric fog and Emphasize with the game's Edge Smoothing on. **Expected:** the card note "Needs the scene
    depth" and no effect.
13. LUT: open the LUTs folder from the card, copy a 1024x32 PNG strip, pick it. **Expected:** the card shows its size
    and the look applies; a strip of another shape shows why it was refused.
14. Main menu and a load screen with filters on. **Expected:** the game's own picture until the world is drawn.

## Confirmed in game

- Combined build (2026-09-28): grading applied after the "last copy wins" fix; the earlier first-copy version graded
  almost nothing (98.7% of pixels treated as UI, HDR_Diag_1/2).

## Open checks

- F10/F8 visual parity: confirm the same paused scene with UI visible, hidden and hidden for a photo,
  including native D3D9, DXVK, loading, resets, interiors and bloom. Recorded hidden frames use complete RGB
  or RGBA write masks; both now pass the learned first-tile boundary guard. Before a visible frame has taught
  the scratch identity, the deliberately conservative EndScene fallback remains. No universal pixel-equivalence
  or performance improvement is established by these records.

- Filters tab (2.7.0): each filter in game, the GPU cost of several stacked filters, and Auto exposure, Atmospheric fog
  and Emphasize on Steam and under DXVK.
- An SDR diagnostic (dump the frame, the scene copy and the share of pixels treated as UI).
- The game's native screenshot (with the Apex shortcut disabled): whether it contains the grade.
- GPU cost at 1080p, 1440p and 4K.
- Behaviour with the game's own Edge Smoothing on, beyond the menus-tinted note.
- Whether the game ever ends more than one scene per frame with the back buffer bound.


## Current release evidence

- Six bounded GPU events from 2026-10-07 23:15 confirm the actual learned first-tile copy state. Hidden events
  with COLORWRITEENABLE=15 fell through the earlier RGB-only guard and ran the chain at EndScene; paired visible
  events had the same scene draw/write counts (2533/2189) and ran before composition. The guard now accepts 7 or 15,
  while retaining shared-depth, layout, target identity, scene readiness and UI protections.
- 49 native post-scene checks pass: ordered AO/AA/Depth Blur/Color, one execution, both recorded masks, rejection
  of partial masks and incompatible depths/copies, readiness, UI protection and reset invalidation.
- 127 extracted-production checks cover group neutralization, retained settings, filter branches, config/profile
  round trips and shortcut persistence/matching.
- 44,653 native ImGui checks cover production filter/group layout, disabled child controls, preserved preferences,
  modal state restoration and three languages at multiple widths/scales. Icon painting and GPU effects are not
  exercised by this CPU geometry fixture.
- The optional APEX_F10_STUDY diagnostic has 4,118 native checks. It is disabled in the public release.
- In-game testing of the complete release remains open for shortcut assignment/removal, typing and game-command
  overlap, stacking, existing profiles, multiple resolutions and DXVK. Recorded copy-state evidence confirms the
  boundary defect; it does not establish universal final-image equality.
