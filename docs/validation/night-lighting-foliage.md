# Night Lighting, foliage: validation

The feature is described in [features/night-lighting/foliage.md](../features/night-lighting/foliage.md). It must:

- Keep lamp light on foliage and `vs_2_0` fences inside the moon shadow at night, and change nothing by day.
- Give the side of a bush facing away from a lamp a dim lamp term instead of black.
- Recognise the known summer and winter foliage vertex and pixel shaders and refuse others (the road vertex shader in
  particular).
- Produce patched bytecode that is valid on native D3D9 as well as DXVK.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `test7.cpp` (offline, combined build, not kept in `tools/`) | `PatchFoliageVs` and `PatchLeafShadow` over the captured shaders | Not kept as a repository harness | Captured shader set |

No repository harness covers the foliage patches.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-09-25 | combined build | `test7.cpp`, 77 VS / 99 PS | `PatchFoliageVs` accepted `2BC7F188`, `2BEA1650`, `2C55DC50`, `2A839190`, then `32779B38` and `2BC6D848` (6 to 8 VS after accepting `def`-zero clamps); road VS refused | Offline |

## In-game test plan

1. Night, walk around a planter with hedges and a street lamp. **Expected:** both sides of the planter lit; the back of a
   bush facing away from the lamp dim, not black.
2. Winter night, the same with snowy bushes and trees. **Expected:** the same.
3. Day. **Expected:** foliage looks like the game (the moon-shadow fixes are off by day).
4. Developer status. **Expected:** `Shadow: moon shadow on objects: fixed | draws fixed: N | foliage (wrap light): N |
   winter foliage without shadow: N`.
5. F7 on a bush (developer mode). **Expected:** VS `c54..c62` per-instance colours well above 0.1 near lamps; the patched
   copies have different addresses and sizes (+60 / +36 bytes).
6. Log. **Expected:** `[LotLightBridge] Object shadow fix: active`, `[LotLightBridge] Foliage: vertex shader XXXXXXXX
   patched`, `[LotLightBridge] Foliage (moon shadow): shader XXXXXXXX patched`.

## Confirmed in game

- Moon-shadow fix on planter hedges (combined build, 2026-09-24).
- Wrap lighting and the winter variants installed (combined build, 2026-09-25).

## Open checks

- Summer plants and flowers remain darker than the lit ground next to them (m79: rig 0.62 / 0.55 / 0.49).
- Plant 2 (m65): a new F7 on that plant is needed to study its texture multiply (F7 now decodes DXT1/3/5).
