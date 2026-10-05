# Night Lighting, snow: validation

The feature is described in [features/night-lighting/snow.md](../features/night-lighting/snow.md). It must:

- Remove the cut at lot borders on snowy lots: the lot term is never below the terrain term at the same point.
- Light snow on floors, door sills, stair tops and fence tops like the ground next to them.
- Route stair-top snow to the stair fix, never to the snowy roof pass.
- Keep terrain, lot and water draws away from the snow-floor fix.
- Apply *Ground brightness* equally to the linear (`c7.x`) and squared (`c4.x x c4.x`) world terrain variants, and leave
  shared constants and unknown layouts as the game draws them.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/terrain_lighting_test/ground_scale_check.cpp`](../../tools/terrain_lighting_test/README.md) | Reads the captured `s7`/`s11` terrain shaders; linear versus squared classification; refusal of shared, mixed or saturated multipliers; native D3D9 readback comparing both multiplier equations at five day/night weights and eight brightness gains | `run_ground_scale_checks.ps1 -Captures <2026-10-04 15-33-55 session>` | D3D9 GPU, F7 capture |
| `tools/terrain_lighting_test/check.cpp` | `LampScale` for single-pass and squared multi-pass factors: 101 night levels, 120 gains, 21 native scales, previous night rounding, invalid values; `SurfaceLampGain` (snow on objects) | `run.ps1 -Captures <dir> -OutDir <dir>` | D3D9 GPU for the shader parts |
| Offline captured-shader scans (combined build, not kept in `tools/`) | `IsSnowReliefVs`, `IsSnowCoverVs`, `PatchSnowBytecode` over all captured shaders | Not kept as a repository harness | Captured shader set |

The ground scale harness only reads captures and prints results; it does not install files.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-09-25 | combined build | `PatchSnowBytecode` | Output validated with `D3DDisassemble`; patched identity C61F8B55/1940 | Offline |
| 2026-09-25 | combined build | `IsSnowReliefVs` over 161 VS | Only `VS_2E036438` matches | Offline |
| 2026-09-25 | combined build | `IsSnowCoverVs` over all captured shaders | Only `VS_2F27C9C0` / `PS_2F27C510` match | Offline |
| 2026-10-04 | `92582b6` | Terrain lighting suite (`LampScale`, `SurfaceLampGain`) | Passed (part of 2,017,393 GPU, policy and resource checks) | Native D3D9 |
| 2026-10-04 | `622a93c` | `ground_scale_check` | Gain equivalence verified across the day/night weights | Native D3D9 |

## In-game test plan

1. Winter night: lot borders next to street lamps. **Expected:** no cut on snow.
2. Winter night: snow on fence tops, stair tops, door sills and around pools near lamps. **Expected:** lit like the
   ground next to it.
3. Winter day: snow on fence tops near lamps. **Expected:** only a faint lamp share (daytime response).
4. Set *Ground brightness* to 50% and 200% on snow and on grass, crossing chunks drawn by different terrain variants.
   **Expected:** no boundary between variants.
5. Developer status, *Street lamps on lots* line. **Expected:** counters `snow`, `snow on floors`, `snow on objects`,
   `snow with relief` grow.
6. Log. **Expected:** `[LotLightBridge] Snow: active`; `Snow on floors`, `Snow on objects`, `Snow with relief (stairs)`:
   `shader XXXXXXXX patched` or `does not have the expected pattern, left as the game draws it`.
7. F7 on snow (developer mode). **Expected:** the lot pass pixel shader of 1940 bytes (patched) with `s12` bound.

## Confirmed in game

- Snowy lot ground, snow on fence tops and stair tops, snow on floors (combined build, 2026-09-25).
- Winter seam at the lot border (m73/m74) fixed after the atlas change (m76).

## Open checks

- *Ground brightness* on the squared terrain variant (PR #2): visual validation on snow and grass pending.
- Daytime response on snow on objects (PR #2): gameplay appearance pending.
