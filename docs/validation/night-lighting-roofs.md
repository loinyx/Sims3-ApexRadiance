# Night Lighting, roofs: validation

The feature is described in [features/night-lighting/roofs.md](../features/night-lighting/roofs.md). It must:

- Light roof tiles near lit outdoor lamps at night and leave the game's sun, sky, reflection and fog terms unchanged.
- Choose lamps from the roof piece's position only, so the result does not change with the camera.
- Add lamp light on snowy roofs without changing the game's own snowy roof draw.
- Route stair-top snow to the stair snow fix, never to the roof pass.
- Restore every constant, render state and shader it changes.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/terrain_lighting_test`](../../tools/terrain_lighting_test/README.md) (`run_world_pool_checks.ps1`, `world_pool_fixture.cpp`) | Extracted `ReadEnumeratedLamps`, `SelectLamps` memo and scan: no old-world lamp rows after a world change, memo invalidation, failed enumerations, new-world selection | `run_world_pool_checks.ps1 -OutDir <dir>` | None (CPU fixture) |

The roof shaders themselves have no repository harness.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-04 | `92582b6` | Terrain lighting suite including the world pool checks | Passed (part of 2,017,393 GPU, policy and resource checks) | Native D3D9 |

## In-game test plan

1. Night, a house with outdoor wall lamps and street lamps nearby. **Expected:** roof tiles near lamps lit; no flicker
   when zooming or rotating.
2. Winter night, the same house. **Expected:** snow on the roof lit around lamps; stair-top snow nearby lit like the
   ground, not by the roof pass.
3. Turn *Lamps light roofs* off and on. **Expected:** roofs return to the game's look and back, live.
4. Developer page > Lighting > status. **Expected:** `roofs: fixed | lamps on: N | draws fixed: N | with snow: N`
   ("waiting" until the first roof draw), plus `lamp choice memo: N reused, M computed`.
5. Log. **Expected:** `[LotLightBridge] Roofs: active` and `[LotLightBridge] Snowy roofs: active`, or the compile error.
6. F7 on a roof (developer mode). **Expected:** PS `c20+` hold lamp positions near the roof; for snowy roofs VS `c15.x`
   is not zero.

## Confirmed in game

- Summer roofs lit after the strength and flicker fixes (combined build, 2026-09-24).
- Snowy roofs lit after the position fix (combined build, 2026-09-25, capture m29).

## Open checks

- Background compilation of the two replacements at start-up (since 2026-09-28): not specifically retested in game.
- The `SelectLamps` memo (since 2026-09-29): covered by the CPU fixture; no dedicated in-game comparison.
