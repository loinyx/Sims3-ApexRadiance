# Night Lighting, roads and sidewalks: validation

The feature is described in [features/night-lighting/roads.md](../features/night-lighting/roads.md). It must:

- Recognise every road and sidewalk vertex shader variant (summer, winter, alpha-blended sidewalk) and refuse other
  shaders, including lot `.xyz` outputs and arbitrary full-mask TEXCOORD1 outputs.
- Light roads and sidewalks at least as brightly as the terrain chunk map beside them.
- Leave the road as the game draws it when the pattern, the UV mapping constant or the chunk map is missing.
- Apply *Sidewalk visibility* only on the bright parts of the road texture, and 0% must reproduce the game.
- Restore every sampler, texture, constant and shader it changes.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| Offline captured-shader scan (ad hoc, not in `tools/`) | `IsRoadVs` and `PatchRoad` over all captured shaders; patched output disassembles | Not kept as a repository harness | Captured shader set |

No repository harness exercises `DrawRoad` directly. The road gain path (`ConstGain`, `RoadGain`) is extracted by
[`tools/terrain_lighting_test/run_resource_checks.ps1`](../../tools/terrain_lighting_test/README.md) together with the
other ground gain paths.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-09-25 | combined build | Offline scan of 118 unique captured shaders | 3 road VS and 7 road PS (4 winter, 3 summer) recognised; all patched outputs disassemble | Offline |
| 2026-09-25 | combined build | Winter variants 1 to 4 (m06, m10, m18, m24) | Patched offline; sidewalk blend applies to variants 1 and 3 | Offline |
| Before 2.5.6 | 2.5.6 | Offline captured-VS scan (314 shaders) | The three existing road matches kept, one alpha-blended sidewalk match added | Offline |

The 2.5.6 scan recognises a shader variant; it does not certify every sidewalk or colour-correction configuration.

## In-game test plan

1. Night, summer: roads and sidewalks next to street lamps and lot lamps. **Expected:** as bright as the grass beside
   them.
2. Night, winter: road edges and sidewalk corners near lamps. **Expected:** no dark strip at the road edge, no dark
   squares at sidewalk corners.
3. Winter: move *Sidewalk visibility* from 0% to 100%. **Expected:** 0% looks like the game (fully covered); higher
   values show the concrete.
4. Move *Roads and sidewalks* while lamps are on. **Expected:** roads brighten or dim relative to the ground; by day the
   change is weighted out.
5. Developer page > Lighting > status. **Expected:** "roads: N" grows; "without terrain texture" stays stable.
6. Log. **Expected:** `[LotLightBridge] Road: shader XXXXXXXX patched` or `... does not have the expected pattern, left as
   the game draws it`. In developer mode refused shaders are saved to `Apex Radiance\ShadersRecusados\Road_PS_*.bin`.
7. F7 on a road (developer mode). **Expected:** the patched pixel shader is bound.

## Confirmed in game

- Winter variants 1 to 4 (combined build, 2026-09-25): dark road edges and sidewalk corners fixed.

## Open checks

- Summer roads: every summer road capture predates the summer fix; no capture shows the patched summer shaders.
- Alpha-blended sidewalk variant (2.5.6): recognised offline only; its in-game appearance is unverified.
