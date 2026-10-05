# World lamp response: validation

The feature is described in
[features/night-lighting/world-lamp-response.md](../features/night-lighting/world-lamp-response.md). It must:

- Track live world-owned type-11 lamps, including the captured flags 0x73 / 0xF3, and leave lot-owned rules unchanged.
- Treat observed value edits of world lamps as priority edits, and never streaming additions or removals.
- Request one native rig refresh per coalesced world-lamp edit, even when the terrain completes before the debounce.
- Drop every lamp row of the previous world on a world change.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/world_lamp_test/check.cpp`](../../tools/world_lamp_test/check.cpp) | `Track`, `Eligible` over all flags, types 0..31 and rooms, `AcceptEdit` combinations, captured flags 0x73 / 0xF3 and unchanged lot rules | Build and run the single file | None |
| `tools/terrain_lighting_test/lamp_state_fixture.cpp` | Extracted `NoteEdit`, `RefreshWorldRigs`, `FinishEdit`, `EditReady`: native terrain completion before the debounce, one independent rig request after debounce or completion, coalesced edits, later separate edits, exclusion of ordinary lot and automatic requests; optional `-Baseline` reproduces the old lost request | `run_lamp_state_checks.ps1` | None |
| `tools/terrain_lighting_test/world_pool_fixture.cpp` | Extracted world reset, memo/scan selection, enumeration/tracking and refresh scheduling: no old-world lamp rows, memo invalidation, forced first read, failed enumerations, 512 world changes without false edit events, new-world selection; optional `-Baseline` reproduces the old cache | `run_world_pool_checks.ps1` | None |

The fixtures substitute engine memory and timing. They do not validate native bake timing, GPU output, visual latency
or the global rig refresh cost.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-03 | 2.5.6 (`98a4149`) | `world_lamp_test` | 58,221 passed, 0 failed | None |
| 2026-10-03 | 2.5.6 (`98a4149`) | Release x86 build and translation checks | Passed | None |
| 2026-10-04 | `92582b6` | Lamp state and world pool fixtures | Passed | None |

## In-game test plan

1. Recolour the same world post by day and at night. **Expected:** the ground, roads and nearby objects take the new
   colour; F7 shows the native rig and the direct lamp constants agree.
2. Disable and enable the post; pan while editing. **Expected:** the final state reaches the ground and objects.
3. Dusk and dawn, lot streaming. **Expected:** no priority world edit from the native lit-bit switch or from streaming.
4. Record F7/F8 and a session covering the edit and the ground response; measure the global rig refresh cost over
   repeated edits.
5. Indoor lights, separately. **Expected:** unchanged.

## Confirmed in game

- 2.5.5-world-lamp-test2 build: the colour response of the edited world post was correct in the test scene
  (maintainer feedback before the 2.5.6 publication; qualitative, no latency or FPS measurement, not every world,
  weather or streaming case).
- The latest terrain test build before the 2026-10-04 review was reported as excellent; that feedback predates the
  review changes and does not validate them.

## Open checks

- PR #2: rig request on early terrain completion and world-change clearing, in gameplay.
- Global rig refresh cost; repeated edits, streaming, dusk/dawn and indoor lights.
