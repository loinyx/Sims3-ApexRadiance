# Night Lighting, foliage: validation

The feature is described in [features/night-lighting/foliage.md](../features/night-lighting/foliage.md). It must:

- Keep lamp light on foliage and `vs_2_0` fences inside the moon shadow at night, and change nothing by day.
- Give the side of a bush facing away from a lamp a dim lamp term instead of black.
- Recognise the known summer and winter foliage vertex and pixel shaders and refuse others (the road vertex shader in
  particular).
- Produce patched bytecode that is valid on native D3D9 as well as DXVK.
- With per-vertex lamps, light each vertex by its own distance, height and side towards each lamp, never above the rig's
  cap and the lamp's hue, with the instance's own lamps (the per-pixel lamps' choice: reach, walls, cones, indoor lamps
  shining out), never the rig's slots.
- Shade lot ground under crowns partially and softly, with the crown test fast enough for a worker thread.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `test7.cpp` (offline, combined build, not kept in `tools/`) | `PatchFoliageVs` and `PatchLeafShadow` over the captured shaders | Not kept as a repository harness | Captured shader set |
| `tools/foliage_vertex_test/` | `FoliageVertexLamps::Patch` on the captured foliage VS `9A10A3A8` and the trunk VS `89E801E0`: the patched copy against a CPU reference, a neutral instance against the old shader, the cap, transmission, crown and bounce terms | `run.ps1 -Shader <foliage_VS_9A10A3A8.bin> -Trunk <trunk_VS_89E801E0.bin>` | Native D3D9, two captured vertex shaders |
| `tools/canopy_test/` | `foliage_canopy.h`: chord length, Beer-Lambert transmission, soft crown edge, the worst-case crown test time | `run.ps1` | MSVC x86 |

No repository harness covers `PatchFoliageVs` and `PatchLeafShadow`.

## Latest results

Release validation for `v2.15.6`: Foliage phase and saturation policy checks; runtime dusk comparison remains open. The reviewed implementation is `f66e777`;
final release build and gameplay confirmation are tracked with the release tag and PR. Offline results do not establish
an FPS gain, complete climate coverage or resolution of texture allocation stalls.

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| — | `v2.15.6` (implementation `f66e777`) | `foliage_vertex_test` | 132 checks passed; canopy and trunk fixtures, phase and saturation policy, tree shader package coverage | CPU fixtures; shader package |
| 2026-10-23 | `40f351e` | `tools/canopy_test/` | 16 checks, 0 failures. No harness covers the tree light rig hooks (`TreeLightRig`) | CPU harness |
| 2026-10-22 | `b863bce` | None: `tools/foliage_vertex_test/` was not re-run; no harness covers the area lights' cones or the world change clear | Not run | not run in game yet |
| 2026-10-20 | Working tree on `c22e8d7`, foliage code as in `3e0b6ad` | `tools/foliage_vertex_test/` | 28 checks passed; the trees' own lamps by the inverse square: a street lamp 5 m from a crown gives 0.425, 9 m 0.042, 11 m 0; the red lantern's crown stays 0 | Native D3D9 |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/foliage_vertex_test/` with `-Precomp` | 26 checks passed; `PatchTreeSunShadow` over the 8,011 pixel shaders of `Shaders_Win32.precomp`: 16 tree shaders, the 8 without a shadow patched and loading in D3D9, the 8 with one left alone, nothing else touched | Native D3D9 |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/foliage_vertex_test/` | 24 checks passed; adds `EyeAnchors`: a drift of (0.8, -0.1, -0.6) m between the read and the game's eye comes back within 1 mm; another mesh, a place 9 m away and instances that disagree give no offset | Native D3D9 |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/foliage_vertex_test/` | 19 checks passed; own lamps (`SlotConstants`, `KeepBrightest`, `BoxDistance2`): a red lantern of 6.3 m reach 13.7 m from a bare crown leaves it unlit (brightest 0) and equals the per-pixel lamps' law; a warm post (intensity 3.4) next to a leaf gives (1, 0.81, 0.56), hue kept; the three brightest kept in order; per-vertex max relative error 2.75e-07 | Native D3D9 |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/foliage_vertex_test/` | 14 checks passed; trunk variant patched, neutral instance worst difference 1.80e-07 of the largest output | Native D3D9 |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/canopy_test/` | 16 checks passed; worst case 2,029,260 crown tests in 30.5 ms on a worker thread | None |
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

- 2026-10-23, development build of this branch (`d6aecf5` and later): trees on a lot with a basement are no longer drawn flat and white at night.
- Moon-shadow fix on planter hedges (combined build, 2026-09-24).
- Wrap lighting and the winter variants installed (combined build, 2026-09-25).
- Street lamps light the side of nearby crowns facing them, and lamp edits reach the trees without the earlier 1-2 s wait (2.14.0 public test build, 2026-10-20). Log: 5,583 picks made again in 4 minutes at night, about 20 to 60 per second.
- 2.14.0 release candidate (public build after the release review, 2026-10-20): lamps on nearby trees, dusk, a window lamp beside a tree, indoor lamp edits, loading a lot with stairs and arches, and a Report a problem recording, all approved by the maintainer in game.

## Open checks

- 2.15.3: area lights of a lot (types 9 and 10) beside a tree. **Expected:** the side the light faces is lit, not the whole crown.
- Area lights (`b863bce`): the lot tree that drew bright white at load in the session whose lighting snapshot showed a
  lot rectangle area light (white, range 100, intensity 1, at ground height) as the only non-lamp light the lamp reader
  accepted ([history](../history/night-lighting-foliage.md)). **Expected:** it looks like its neighbours, at load and after
  travelling to another world and back (the mesh boxes and picks are cleared on a world change). A light capture of it
  lists the lamps each instance picked ([Light Probe](dev-tools-light-probe.md)). Not yet checked in game.
- A lamp switched on indoors next to a window, with a tree outside: the tree takes its light at once, not after the rooms of the house (the trees now read the lamps shining out themselves during a hold).
- Summer plants and flowers remain darker than the lit ground next to them (m79: rig 0.62 / 0.55 / 0.49).
- Plant 2 (m65): a new F7 on that plant is needed to study its texture multiply (F7 now decodes DXT1/3/5).
- Per-vertex lamps in game: whether a crown with a lamp at its foot looks darker than before, and the trunk near the lamp.
- The log line `[Foliage] Instance positions are ...` and the share of draws that keep per-vertex light.
- Crown shade on lot ground in game, and its cost.
- The snowy park lot of F8 `2026-10-20 19-46-11` with only the red garden lantern lit. **Expected:** the large bare tree's
  crown is no longer red; only branches within about 6 m of the lantern could take a faint red.
- The summer lot of F8 `2026-10-20 20-36-15` (red cube lamp, range 70, 4 to 5 m from a leafy tree). **Expected:** red on
  the lower crown facing the lamp, fading upwards, the top dark. Log `[Foliage] ... instances by their own lamps: N with
  none, N with 1, N with 2, N with 3 (N without a crown shape); lamps left out by walls: N`, and any `not of the captured
  family (reason)` line with its `Censo\foliage_VS_*.bin`.
- Dusk with many trees in view: no hitch while every crown picks its lamps again (96 per 8 ms). A fading lamp now changes only the colour of the picks (no new pick); expect the log's picks made again to stay near the night's rate (about 20 to 60 per second) through dusk.
- Day, sun low: trees beside a house and trees side by side. **Expected:** the house's and the neighbour's shadow on the
  crown, the far side of a crown darker than the sunny side, never black (the floor 0.25); no speckle on the leaves (if
  there is: the depth bias); FPS within noise of `foliage_sun_shadow = false`. Log `[Foliage] Sun shadow map seen: NxN`,
  `[Foliage] Tree pixel shader X: reads the sun shadow`, `[Foliage] Tree draws with the sun shadow: N`.
- Night, pan and zoom around a lit tree. **Expected:** the lamp light stays on the same leaves, no crown blinks; log
  `camera drift corrected: up to N m` (N above 0 while moving shows the drift that was corrected).
