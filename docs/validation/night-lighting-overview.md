# Night Lighting: validation

The feature group is described in [features/night-lighting/README.md](../features/night-lighting/README.md). Each
sub-part has its own validation page (for example [lot light pass](night-lighting-lot-light-pass.md),
[world atlas](night-lighting-world-atlas-and-smoothed-maps.md), [terrain relight](night-lighting-terrain-relight.md),
[world lamp response](night-lighting-world-lamp-response.md)). As a whole, Night Lighting must:

- Install only when every byte-checked game site matches, and leave the game unchanged otherwise.
- Draw every unrecognised shader exactly as the game does (handlers return Continue).
- Restore every device state it changes for a replaced draw.
- Apply menu, profile and undo changes live, except the three code-byte options that reinstall on the render thread.
- Keep the default lighting balance (*Natural*) equal to the documented defaults.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/terrain_lighting_test`](../../tools/terrain_lighting_test/README.md) | Daylight policy, terrain lamp scales, native solar alpha, lot pass HLSL, edit and queue logic, world reset | `run.ps1` and the `run_*_checks.ps1` scripts | Native D3D9 GPU; F7 capture folders for some fixtures |
| [`tools/world_lamp_test/check.cpp`](../../tools/world_lamp_test/check.cpp) | World lamp eligibility and edit guard | See [world lamp response validation](night-lighting-world-lamp-response.md) | None |
| `tools/basis_test` | Room basis map patches (smooth indoor light) | See the harness | Captured shaders |
| `perl tools/menu_audit.pl .` | Menu texts, translations, documented keys, ResetDefaults coverage | From the repository root | Perl |

The harnesses only read captures and the repository; they never write into the game folder.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-23 | `40f351e` | `tools/development/check.ps1 -Suite Smoke` and `-Suite Lighting`; developer, public and standard Release x86 builds | Passed, 0 failures; the three builds compiled. Sub-part harnesses: [lot light pass](night-lighting-lot-light-pass.md#latest-results), [light between stories](night-lighting-level-light-share.md#latest-results), [floors](night-lighting-floors.md#latest-results), [foliage](night-lighting-foliage.md#latest-results) | CPU harness |
| 2026-10-23 | `40f351e` | `tools/terrain_lighting_test` (`run.ps1`) | 1,303,085 checks, 2 failures: *both captured terrain variants tested* and *GPU reach law matches the walls' law*, which also fail at commits from the 2.15.2 period (known, not from this release); the gated indoor light pass equals the previous shader at gate 0 and the CPU formula at gate 1 | Native D3D9 |
| 2026-10-22 | `d625404` | `tools/development/check.ps1 -Suite Smoke` and `-Suite Lighting`, `tools/rt_lamps_test`, `tools/roofed_openings_test`, `tools/room_structure_test` (counts in [light between stories](night-lighting-level-light-share.md#latest-results)); developer, public and standard Release x86 builds | Passed, 0 failures; the three builds compiled. The solve-side harnesses do not exercise the draw hook's memos of `99b99fb`, `c827d18`, `d625404` or the timing scope of `bb640cb` | CPU harness |
| 2026-10-22 | `99b99fb`, `c827d18`, `bb640cb` | None: no harness covers the draw hook's last answers, the lot lookups' memo or the draw cost timing scope | Not run | not run in game yet |
| 2026-10-22 | `0dba5de`, `6592518` | None: no harness covers the draw hook's front table or the draw cost log | Not run | not run in game yet |
| 2026-10-22 | `173e2bd` | 2.15.2: CPU Lighting suite, Instant lamps; developer and standard Release x86 builds. In game, the frame profiler of 2.15.1 while moving the camera at night put RtOutdoorLamps `UnderDelta` at the top of the render-thread samples (748 hitches, up to 92 ms); with the developer build of this commit the frame rate while moving the camera was much smoother in game | Checks passed, 0 failed; the in-game gain was observed, not re-measured with the profiler | Offline; in game DXVK |
| 2026-10-22 | `4e32cea` | 2.15.1 release: CPU Lighting and Smoke suites, world lamp policy (low-lamp rule, sun time gates, smooth dusk mapping), Instant lamps, floor shade, i18n check, menu audit; developer, public and standard Release x86 builds | 58,249 + 22 + 5,842 checks passed, 0 failed; builds without warnings. In game (developer builds of this branch): the moon height and the lot area lights confirmed by the maintainer; a lamp on the ground lit the snow in live mode; Build mode kept the night level at 1.00 (log) | Offline; in game DXVK |
| 2026-10-04 | `92582b6` | Terrain lighting GPU, policy and resource checks | 2,017,393 passed, 0 failed; Release x86 build without warnings | Native D3D9 |

Sub-part results are on their own validation pages.

## In-game test plan

1. Night, lot border next to a street lamp. **Expected:** no straight cut between lot and world grass.
2. Choose *Subtle*, *Soft*, *Natural* in Lighting > Overview, then *Undo choice*. **Expected:** the nine brightness
   values change together; water, colours and room light stay; undo restores the previous mix.
3. Toggle *Street lamps light lots* off. **Expected:** roads, floors, fences, snow and per-pixel objects return to the
   game's look; walls, roofs, water and the object rig fix keep working.
4. Change one of `postesAcesosNoCalculo`, `qualidadeAltaEmTodosOsLotes`, `gramaDoLoteUsaLuzDoLote` in developer mode.
   **Expected:** a reinstall on the render thread, no crash, ground maps kept.
5. Alt+Tab and change resolution. **Expected:** lighting returns on the next frames, no black ground cells.
6. F7 over lamp-lit surfaces. **Expected:** the `mod:` line names the handler for every surface listed in the problem
   table; the false-colour census shows no magenta on surfaces the feature claims to fix.

## Confirmed in game

- Lot light pass, world atlas and smoothed maps, dusk rebuild and per-surface fixes: working since the combined build
  and v0.1.0 (see the sub-part pages).
- 2.5.6: world lamp colour response in the tested scene.

## Open checks

- 2.15.3, in game: moonlight 0 by default (the night lit only by the lamps and the sky's glow) on a lot with few lamps; frame times at night against 2.15.2 on the same save with the paced room solving and the draw memos.
- Draw cost by fix (`6592518`), development build at night on a busy lot: the `[LotLightBridge] Draw cost by fix ...`
  line every 10 s, its rows plausible against the Frame Profiler's *D3D hooks (mod)* and *Re-issued draw (driver)*.
  Not yet seen in a log.
- Patched shader answers (`0dba5de`): the same lighting as before (the front table and the last room map entry give
  the caches' own answers), across a device reset and a Night Lighting reinstall (`Shutdown` moves the generation); the
  cost per draw in the draw cost line against the previous build. Not yet run in game.
- Last answers per draw (`99b99fb`, `c827d18`): furniture during a lamp switch and drag (`HoldRig`), smooth indoor light
  (`RoomMapPadding`), story-0 floors and fences with the ground lamp shade on several lots (`FindFloorLot`,
  `FindLotAt`), including after travelling and a lot change in Edit Town. **Expected:** the same light as the previous
  build, the Night Lighting status counters still counting, and a lower cost per draw in the draw cost line. Not yet
  run in game.
- 2.15.1 standard build: Build mode at night in a winter world (lawn, sidewalks and street lamps as in live mode; the
  sun time gate's log lines), smooth dusk (no steps, frame times while rigs re-gather), the F8 day and night line, and a
  lamp on the lawn in Classic lamp lighting.
- PR #2 daylight composition, day/night phase rebuilds and squared ground scale: same-lot gameplay and DXVK.
- Smooth indoor light with the `IndoorBasisScale` correction (furniture after floor switches): not tested in game.
- Draw-handler CPU cost changes (`CallOriginal*` wrappers, `SamplerBind`, `VsInfo`, memoized `SelectLamps`): written
  2026-09-29, never measured in game.
- Brightness controls (1.5.0): ground, roads, street and lot lamps and moonlight were not separately confirmed in game.
- Roadmap items not implemented: plants with ground light (m79), OutdoorProp shaders (C0C6E0FF, 2BE88B48),
  4-light-matrix foliage (VS 4375A3EE), instanced SingleObject, 34 object shaders with no free input; per-pixel lamps on
  walls and floors (the per-pixel plan (private notes), with its 8 MUST-FIX items); terrain stamp re-rendered at 4 texels/m; one lamp
  model for every surface (today: rig bounds radius, sqrt(range) radius for roofs and water, W = 0.4 x range for
  per-pixel objects, wall gain).
