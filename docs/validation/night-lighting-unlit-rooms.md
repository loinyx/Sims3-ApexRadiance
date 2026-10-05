# Rooms at Night: validation

The feature is described in [features/night-lighting/unlit-rooms.md](../features/night-lighting/unlit-rooms.md). It must:

- At Brightness 100% and Blue tint 100%, and with the switch off, look exactly like the game.
- Scale only the background light: lamps keep their colour and contribution on walls, floors and furniture.
- Give unlit rooms the new colour without a solve, and bring lit, busy, rebuilt or streamed rooms to the same result
  once the change rests, without a manual refresh.
- Keep the first colour family's legitimate grey and never write the game's native colour globals.
- Never edit Sims3SettingsSetter's configuration except through the explicit *Back up and correct* action, and then
  remove only the saved `BradyBunchBlue RGB` entry after a verified backup.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/room_ambient_test`](../../tools/room_ambient_test/README.md) `room_ambient_test.cpp` | Production scheduling and ambient policy (`room_ambient_policy.h`): linear background factors, finite RGB matching, unknown and busy-room solve decisions, manager and lot cache invalidation | `powershell -NoProfile -ExecutionPolicy Bypass -File tools/room_ambient_test/run.ps1` | MSVC x86 Build Tools |
| same, `unlit_rooms_recovery_test.cpp` | Includes `features/unlit_rooms.cpp` with fixtures for Windows time, the native room queue, the group updater, address lookup and patch writes, on x86 room buffers: native top-up, both families, 0 to 100% sweeps, busy-to-idle recovery, history eviction, repeated targets, identity reuse, world resets, late base initialisation, partial patch rollback, disabled recovery, clock wrap, day / night / dark-room furniture factors, neutral and blue cube colour, lamp RGB preservation, no extra rig refresh on unchanged reconciliation, queue acknowledgement against a fresher base; S3SS override preparation (invalid, inline and CRLF TOML), preservation of unrelated settings, that enabling Rooms at Night never invokes the correction, the explicit correction path and the already-applied RGB baseline, 10 / 35 / 80% brightness with zero blue | same | same |
| same, `-CompareTag v2.5.5` | Compiles the updater from a Git tag and runs the brightness-reference checks against it and the current source: the inherited (.01, .01, .01) grey, its fourth components, the standard blue (.15, .15, .30) | `run.ps1 -CompareTag v2.5.5` | same, Git |

The runner builds into `%TEMP%/ApexRoomAmbientTests` (`-OutDir`, `-VsDevCmd` override). The executables do not attach to
the game, write config files or produce test data. They do not run the game's top-up, lot enumeration, group solver,
installed hooks or D3D9 draws; the check count includes many slider values rather than distinct gameplay scenarios.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-04 | PR #2 (`f0b8d54`) | Policy and recovery suites | 1,438 passed (89 policy, 1,349 updater) | None |
| 2026-10-03 | PR #2 (`b4f9da2`) | Policy and recovery suites | 1,421 passed (89 policy, 1,332 updater) | None |
| 2026-10-03 | same | Brightness reference against v2.5.5 and current source | 63 passed on both, identical numerical responses | None |
| 2026-10-03 | reliability audit | Recovery suite after the three updater fixes | 1,269 updater checks plus 89 policy (1,358) passed; the original updater fails the rejected-queue restoration, small first-family step and high-uptime deadline cases | None |
| 2026-10-01 | `2.5.2-test005` | Production pending loop, stale-lamp visitor, Rooms at Night visitor and acknowledgement, SEH `MoveBase` | Passed | None |
| 2026-09-30 | combined build | Cube tint patches over 349 captured shaders (scratch harness like `tools/basis_test`) | 4 `PatchIndoorBasis` and 31 `PatchCubeTint` results, all disassemble, the `mul` right after the `lrp` | d3dcompiler |
| 2026-09-30 | combined build | Rig chain census | 60 of 628 game shaders recognised, all real chains | d3dcompiler |
| 2026-09-30 | combined build | Unlit-light classification over every room-mode draw of captures 075-094 | Fill found by w in 18 slots, [NoLight] by direction in 46; the colour test never needed | None |
| 2026-09-30 | combined build | `PatchCubeTint` on captured shaders with an ambient cube weight (scratch test) | 6 of 6 patched and valid | d3dcompiler |

## In-game test plan

1. A room at night, every lamp off, card on with defaults. **Expected:** a faint neutral light, no blue glow. Brightness
   100% and Blue tint 100% look like the game; the switch off looks like the game.
2. Sweep Brightness 80% -> 10% -> 80%, then Blue tint 100% -> 0% -> 100%, in rooms with and without lamps. **Expected:**
   walls, floors and furniture follow; lamps keep their colour and contribution.
3. Repeat while dragging quickly, switching lamps, changing floors and moving between lots. **Expected:** rooms that were
   being solved catch up after the last change without a manual refresh.
4. Enable and disable the card repeatedly; re-enter the world and load a second save without changing settings.
   **Expected:** lit and unlit rooms return to the game's ambient when disabled and to Apex's when enabled.
5. Dawn or daytime, and a dark enclosed room by day. **Expected:** furniture in daylight keeps the game's look; furniture
   in the dark room follows the controls.
6. Close a room with a new wall, or add or remove a roof, with the card on. **Expected:** the room takes the controlled
   colour within about a second.
7. With official S3SS loaded and a saved `[settings.'BradyBunchBlue RGB']`, press *Back up and correct*. **Expected:**
   status *Correction complete*; a `S3SS.toml.before-room-ambient-fix.<hash>.bak` in the Apex Radiance folder; only that
   entry removed; the room blue visible this session with the card on, and after a restart.
8. Developer mode: status *Rooms at night: on | ... | relights: N*; log `[UnlitRooms] Ready: unlit-room colours at
   0x11d0b60 (...)`. Record with F6 if a response differs (control values, room ambient and solver state, furniture paths).

## Confirmed in game

- 2.5.3 builds (test005 to test008): Rooms at Night reported working well in maintainer tests (scenario-specific).
- 2026-10-03, installed recovery binary (SHA-256 verified), one Apex ASI beside the official S3SS, Brightness and Blue
  tint saved at 1.0, read-only inspection: both families' original and patched colours were (.01, .01, .01), matching
  S3SS's saved `BradyBunchBlue RGB`. The standard second family (.15, .15, .30) has luma .16083, about 16.083 times that
  grey's .01 (an ambient-component ratio, not a screen-brightness ratio). This explains why the controls could not
  brighten past the darkened base or restore blue in that session.
- 2.5.6: an intermittent response was reported, then both controls were confirmed working after a restart with a new
  build. `unlit_rooms.cpp`, `room_ambient_policy.h`, `level_light_share.cpp` and `object_light_bridge.cpp` are identical
  between the 2.5.5 and 2.5.6 tags; the cause of the earlier symptom is not established.

## Open checks

- Binary hooks, actual lot enumeration, material response, other mods and DXVK: running-game tests of the PR #2 build.
- That native blue is restored on the next start after the S3SS correction and visibly follows the controls (the
  2026-10-03 local correction, backup folder 336, was not confirmed after restart).
- Where the game writes the two colours, and whether they change with the time of day.
- Whether room-mode rigs gather again on a relight of their room (the furniture fill after a slider change).
- Floor switches, initial night loads, open stairwells, lots without lamps, rapid drags.
