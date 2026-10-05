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
| `perl .claude/skills/apex-menu/audit.pl .` | Menu texts, translations, documented keys, ResetDefaults coverage | From the repository root | Perl |

The harnesses only read captures and the repository; they never write into the game folder.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
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

- PR #2 daylight composition, day/night phase rebuilds and squared ground scale: same-lot gameplay and DXVK.
- Smooth indoor light with the `IndoorBasisScale` correction (furniture after floor switches): not tested in game.
- Draw-handler CPU cost changes (`CallOriginal*` wrappers, `SamplerBind`, `VsInfo`, memoized `SelectLamps`): written
  2026-09-29, never measured in game.
- Brightness controls (1.5.0): ground, roads, street and lot lamps and moonlight were not separately confirmed in game.
- Roadmap items not implemented: plants with ground light (m79), OutdoorProp shaders (C0C6E0FF, 2BE88B48),
  4-light-matrix foliage (VS 4375A3EE), instanced SingleObject, 34 object shaders with no free input; per-pixel lamps on
  walls and floors (`PASSO3-PLANO.md`, with its 8 MUST-FIX items); terrain stamp re-rendered at 4 texels/m; one lamp
  model for every surface (today: rig bounds radius, sqrt(range) radius for roofs and water, W = 0.4 x range for
  per-pixel objects, wall gain).
