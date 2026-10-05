# Lot light pass: validation

The feature is described in [features/night-lighting/lot-light-pass.md](../features/night-lighting/lot-light-pass.md).
It must:

- Show no straight cut of street-lamp light at a lot border at night.
- Leave lot edges without a nearby lamp unchanged, and never lower the lamp term below the terrain term.
- Produce, at the lot edge, the same terrain term the world grass shows on the other side.
- Reproduce the previous night output exactly when the daylight term is zero.
- Leave the draw to the game when the c14 mapping, the terrain source or the original c28..c31 read is unavailable.
- Align the lot map only for the exact recognised lot VS.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/terrain_lighting_test`](../../tools/terrain_lighting_test/README.md) (`check.cpp` with `-ReferenceSource`) | Production lot HLSL at night against the previous source; day continuity across forced lot-edge weights with native window/lot scaling at zero; extracted regular lot draw path: atlas/chunk fallback, failed original-constant reads, texture reference balance, restoration of all touched state | `run.ps1 -Captures ... -OutDir ... -ReferenceSource 'previous lot_light_bridge.cpp'` | Native D3D9 GPU |
| `tools/terrain_lighting_test/lot_uv_fixture.cpp` | Lot UV inversion for scales 1/16 to 1/256 over local -1 to 256 m; captured outside point moves from the indoor row to the outside row; shader creation by the device | `run_lot_uv_checks.ps1` | Native D3D9 GPU |
| `tools/terrain_lighting_test/resource_paths_fixture.cpp` | UV fallback and resource restoration | `run_resource_checks.ps1` | Native D3D9 GPU |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-04 | `2584e01` | Lot UV inversion and shader compilation/device acceptance | 128,512 passed, 0 failed (RTX 4070 Ti SUPER) | Native D3D9 |
| 2026-10-04 | `2584e01` | Resource restoration and UV fallback | 13,682 passed, 0 failed | Native D3D9 |
| 2026-10-04 | `92582b6` | 60 GPU scenarios of the production lot HLSL at night vs the previous HLSL | Within one 8-bit LSB, including soft-edge weights | Native D3D9 |
| 2026-10-04 | `92582b6` | Failure injection of the c28..c31 read | Native draw kept | Native D3D9 |

The lot UV fixture compiles the replacement as ps_2_0, while production compiles it as ps_3_0
(`AddLotShader(..., "ps_3_0", 0)`); the mapping check is profile-independent, but the device-acceptance part tests a
different profile from the one shipped.

## In-game test plan

1. Night, stand at a lot border next to a street lamp. **Expected:** no straight cut between lot and world grass.
2. Developer > Lighting > Surface and provider state, "Street lamps in lots". **Expected:** "Active | terrain chunks
   seen: N | lot light fixed: N draws ... | without terrain texture: M", with M near 0 once the atlas is ready.
3. F7 on lot grass. **Expected:** the covering light pass is the replacement (ps_3_0) with s2 bound to the 2D atlas
   render target (2560x2560 or 4096x4096 in the captures) or a 1024x1024 A8R8G8B8 smoothed map.
4. Toggle *Street lamps light lots* live. **Expected:** the cut returns when off.
5. Soft lot edges: F7 on lot grass 0.2 to 0.5 m inside the edge next to a lamp, then F7 on the world grass just outside.
   **Expected:** the lot pass line reads `mod draw: lot light pass | soft edges: lot <id>, W x D m, origin (x, z), band
   3.0 m (PS c28..c30)` with the real lot size (for example 30 x 30); the lamp term inside matches the terrain term
   outside within about 2 to 3%. Developer > "Soft lot edges" A/B; the status "Soft lot edges: on, band 3.0 m | lots
   known: N | lot passes feathered: M | without a lot rectangle: K" keeps K near 0 once lots are loaded.
6. A closed wall at the lot's outer edge with a lamp inside. **Expected:** no indoor light row on the grass just outside
   the wall (lot UV alignment).
7. Daytime in Build mode with lamps lit. **Expected:** lot and world grass show the same subdued lamp term across the
   border.

## Confirmed in game

- Combined build (notes section 1): the lot pass with `max(lot, terrain)` removes the straight cut.
- 2026-10-02, terrain-variant build `C1257AF49C613DF227B5EBAD639D795665D77E378A0FB3744DB948AFA0883B39`: the reported lot/world
  cutoff resolved in the tested scene (maintainer feedback, no new GPU comparison). The earlier lamp-off package
  `D593AECA5885682FEA5D6EDCDAB3F038512CC6B08D8CDC1371A4F456041DD88D` predated the multi-pass correction.

## Open checks

- The runtime value of room 0's +0xC0 / +0xC4 has not been printed in a log.
- PR #2: lot UV alignment, daylight terrain term and the c28..c31 read guard in gameplay and under DXVK; whether the
  failed constant read ever occurs in play.
- Winter variants and other untested scenes for the continuity fix.
- Per-pixel lamp term on the lot-map part (`max(lampTerm(lotMap), terrain)`, PASSO3 increment 4): not started.
- Terrain stamp re-rendered at 4 texels/m (increment 5) would sharpen world and lot grass.
