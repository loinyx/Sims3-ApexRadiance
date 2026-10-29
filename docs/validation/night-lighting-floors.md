# Floors: validation

The feature is described in [features/night-lighting/floors.md](../features/night-lighting/floors.md). It must:

- Light recognised outdoor floors with `max(floor map, ground atlas)`, never darker than the game.
- Leave interior floors and unrecognised floor shaders exactly as the game draws them.
- Restore every shader, sampler and constant after each patched draw.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| Offline table generators (private research folder: `snowcover/test13.cpp`, not in the repository) | Which ExteriorFloors pixel shaders `PatchBakedAtlasPs` accepts and which pass `D3DDisassemble` after patching | Not maintained in the repository | Game `Shaders_Win32.precomp`, d3dcompiler |
| Offline VS pattern test (combined build, notes m66) | `IsFloorVs` accepts only `kFloorVs` and the pool-edge VS | Not maintained in the repository | Captured shaders |

[`tools/floor_shade_check`](../../tools/floor_shade_check/README.md) (`run.ps1 -Precomp <Shaders_Win32.precomp>`, native
D3D9) covers the floors' ground lamp shade patches, the instanced structure (fence) copies and the snowy lot passes over
the game's precomp; the table generators above are not maintained in the repository. Since `91af563` it fails unless
every winter floor pixel shader of the precomp takes `PatchFloor` (with the atlas, or gain only keeping the game's bytes
and naming its lamp scale), and it draws the weather floors' map-into-scale layout on the device:
`max(map, atlas x weight) x k` within 2 levels of 255.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-27 | `47ec493` (release 2.16.0 candidate; the outdoor scale) | `tools/floor_shade_check` (`run.ps1 -Precomp <Shaders_Win32.precomp>`) | 9,909 checks, 0 failures: 285 copies patched, 252 with the outdoor scale (none past s15), its sampler and constant after every other register | Native D3D9 (offline device) |
| 2026-10-23 | `40f351e` | `tools/floor_shade_check` (`run.ps1 -Precomp <Shaders_Win32.precomp>`) | 7,103 checks, 0 failures: the precomp floor shaders patched with and without the indoor light copies (the copies are not drawn at this commit, `kFloorIndoorLight` off) | Offline, native D3D9 |
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

- Combined build: winter floors confirmed patched (m62); summer floors added in v5.4 (2026-09-25).

## Open checks

- 2.15.3: a porch deck on a foundation beside a pillar at night, *Indoor light adds to the ground* on. **Expected:** no square steps of light and no dark square beside the pillar; a patio or path beside a lit window as bright as in 2.15.2.
- Ground brightness on floors (the `scaleConst` gain) has no recorded in-game check.
- ps_2_0 ExteriorFloors coverage (235 variants).
- Weather variants of the winter floors (`91af563`): a patio or deck near a lamp at night, dry and in rain. **Expected:**
  the same lamp light in both (before, rain drew the patio at the game's lamp scale, several times brighter). No run of
  the updated `floor_shade_check` is recorded, nor how many weather variants lack a sampler for the ground lamp shade.
