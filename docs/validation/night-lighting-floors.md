# Floors: validation

The feature is described in [features/night-lighting/floors.md](../features/night-lighting/floors.md). It must:

- Light recognised outdoor floors with `max(floor map, ground atlas)`, never darker than the game.
- Leave interior floors and unrecognised floor shaders exactly as the game draws them.
- Restore every shader, sampler and constant after each patched draw.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| Offline table generators (scratchpad `snowcover/test13.cpp`, not in the repository) | Which ExteriorFloors pixel shaders `PatchBakedAtlasPs` accepts and which pass `D3DDisassemble` after patching | Not maintained in the repository | Game `Shaders_Win32.precomp`, d3dcompiler |
| Offline VS pattern test (combined build, notes m66) | `IsFloorVs` accepts only `kFloorVs` and the pool-edge VS | Not maintained in the repository | Captured shaders |

No maintained harness in `tools/` covers the floor patches.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-09-25 | combined build | `floor_atlas_table.h` generation | 261 of 571 ExteriorFloors PS accepted and valid (235 are ps_2_0); 0 of 58 InteriorFloor PS; all 96 vs_3_0 ExteriorFloors VS export world xz | d3dcompiler |
| 2026-09-25 | combined build | Captured `.bin` hashes | m60 / m61 / fountain PS (1044 bytes) = `0x9CFC614F`, flat PS (812 bytes) = `0x802006F6`, both in the table | None |

## In-game test plan

1. Summer, night: a deck or tiled patio next to a street lamp. **Expected:** as bright as the grass beside it. Turning
   *Smooth ground light* off (no atlas) returns the game's dark floor.
2. Winter: snowy lot floor tiles and a curved pool edge near a lamp. **Expected:** lit like the snowy ground around them.
3. Developer status *Street lamps on lots*. **Expected:** `floors: N` (winter) and `outdoor floors (summer): N` grow while
   floors are on screen.
4. F7 on a summer floor. **Expected:** the VS copy has one more output, the PS copy one more input and a sampler bound to
   the atlas render target. On a winter floor the `PatchFloor` output (m62: MD5 `40D087B0`, 1788 bytes, s13 = atlas).
5. *Ground brightness* at 50% and 200% at night. **Expected:** floors follow the grass.
6. Interior floor near a lamp. **Expected:** unchanged.

## Confirmed in game

- Combined build: winter floors confirmed patched (m62); summer floors added in v5.4 (2026-09-25 around 17:25).

## Open checks

- Ground brightness on floors (the `scaleConst` gain) has no recorded in-game check.
- ps_2_0 ExteriorFloors coverage (235 variants).
