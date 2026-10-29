# Light Probe: validation

The feature is described in [features/dev-tools/light-probe.md](../features/dev-tools/light-probe.md). It must:

- Record every back-buffer-sized draw of the next frame that paints the chosen pixel, and nothing from other frames.
- Restore every render state, scissor rectangle and constant it touches, so the captured frame looks unchanged apart
  from the short query wait.
- Save its files in a new `Captures\<date> Light capture` folder and announce success or failure with a notice.
- In developer mode, blank only the ticked textures and restore them as soon as they are unticked.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/report_check`](../../tools/report_check/README.md) | Capture folder storage and the Report page control that shows the probe key; probe requests are stubbed | See the harness README | None (x86 MSVC, ImGui from vcpkg) |

The probe's GPU path (occlusion queries, constant reads, texture dumps) has no offline harness. The ground light
profile's camera math (`features/ground_profile_math.h`: the 4x4 inverse, a world point's pixel and the ground point
under a pixel, for a camera like the game's) is checked by `ground_math_check.cpp` in `tools/terrain_stamp_test`
(`run.ps1 -Precomp <Shaders_Win32.precomp>`); the profile's game reads are not.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-22 | `916df4b` | None: the foliage description of the `mod:` line has no offline harness | Not run | not run in game yet |

## In-game test plan

1. Night Lights on, a lot at night, menu closed. Point at a lamp-lit wall and press the probe shortcut (`V`, `4` or
   `F7` depending on the preset). **Expected:** a capture notice, then "Light capture saved. Open Report a problem to find your files"; the log shows
   `[LightProbe] Measured: N draws cover pixel (x, y), M textures saved.`
2. Open the new `Captures\<date> Light capture` folder. **Expected:** `Light capture.txt`, `PS_<ptr>.txt` / `VS_<ptr>.txt`
   and `.bin` files, texture DDS files, `Screenshot.png` when Include a screenshot is on.
3. Read the covering draws from the bottom. **Expected:** the last draw is the one on top unless blending is on; the
   `mod:` line names the Night Lighting decision for each draw.
4. Change the floor shown, or the lot lighting, then take a second capture at the same spot. **Expected:** the report
   has a comparison section against the first capture.
5. Developer mode: Developer > Lighting > Light probe textures, tick one texture. **Expected:** that texture turns
   black (or white with the white option) everywhere it is bound; unticking restores it.

## Confirmed in game

- Light capture was used throughout the night-lighting work for wall, roof, terrain, water and stair investigations
  (see the capture index in [history](../history/dev-tools-light-probe.md)).

## Open checks

- Final-colour reading on non-8-bit back buffers (FP16) is not implemented; such captures say `(not read)`.
- The one-click pointer selection (`Aim`) has no menu control; it is not reachable from the current Report page.
- Planned extra detail (replacement shader pointers for mod draws, constants c144..c149, a lamp block id) is not
  implemented.
- Foliage draws (`916df4b`): a capture on a lamp-lit tree at night. **Expected:** its `mod:` line reads `mod draw:
  foliage | per-vertex lamps: drawn | ...` with one instance line per instance (box, crown, pick age, own lamps), the
  lamps matching the lamps near the tree; a foliage draw without per-vertex lamps (by day) reads `not used for this
  draw (the old path)`, with no instance lines. Not yet seen in a capture.
- Ground light profile (`91af563`): a capture with the mouse on the grass just past a lot's edge, Lamp lighting Instant,
  at night. **Expected:** `Light capture.txt` has a `GROUND LIGHT PROFILE` section whose target is the lot lamp nearest
  the mouse, with screen colours on the line (camera read) and `Ground profile.csv` beside it; the screen colour at the
  mouse matches the report's own screen colour line. A sky cube bound in a covering draw lists its mean colour per face.
  Not yet seen in a capture.
