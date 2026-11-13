# Code map: Night Lighting, draw side

This page covers the D3D9 side of Night Lighting. One draw hook classifies every draw by the shaders bound to it, then
draws it again with a patched or replacement shader, or leaves it to the game. The page also covers the game-code
modules that feed those draws: object rigs, lamp colour, trees, Rooms at Night, room map dither and Light detail.
[Night Lighting](../features/night-lighting/README.md) explains the features, and
[night-game-side.md](night-game-side.md) maps the orchestrator and its settings. Back to the [Code map](../code-map.md).

## Surface finder

`features/lot_light_bridge.cpp:OnDrawInnerCore` tries the rows below from top to bottom, and the first handler that
claims the draw wins. Rows 1-9 run before the `g_enabled` gate. The rows after it need *Street lamps light lots*. VS
classes come from `features/lot_light_bridge.cpp:ClassifyVs`: 1 roof, 2 lake, 3 snowy lot, 4 road, 5 floor, 6 foliage,
7 instanced fence/stairs, 8 snow on object, 9 snow relief, 10 object with rig, 11 snowy floor, 12 sea, 13 lake weather.
PS classes are `PsClass`, set in `features/lot_light_bridge.cpp:Classify`. Handlers live in
`features/lot_light_bridge.cpp` and patchers in `features/shader_patches.cpp` unless the row names another file.

| # | Surface | Classification | Handler | Patch or HLSL | Harness |
|---|---|---|---|---|---|
| 1 | Indoor walls/floors with basis maps | PS reads LightBasisMap0..3 (`features/room_map_padding.cpp:IsBasisPs`) | `DrawBasisSmooth` | `PatchBasisSmooth` (+`PatchRoomMapSmooth`) | `tools/basis_shadow_check/`, `tools/room_surface_check/` |
| 2 | Room-lit floors/walls (blocks, bands) | `features/shader_patches.cpp:IsRoomMapPs` | `DrawRoomMapSmooth`, `DrawRoomMapWalls` | `PatchRoomMapSmooth`, `RoomMapUvScale` | `tools/room_map_smooth_check/` |
| 3 | Instanced outdoor objects in moon shadow | `PsClass::ObjectRig`, not a terrain pair | `DrawObjectRig` | HLSL `kObjectRigHlsl` | none |
| 4 | Roofs | `kRoofPs` + VS 1 | `DrawRoof` | `shaders/roof_ps.hlsl` | none |
| 5 | Snowy roofs | `PsClass::RoofSnow`, VS not 9 | `DrawRoofSnow` | `shaders/roof_snow_lamps_ps.hlsl` | none |
| 6 | Lakes, sea, lake in rain/snow | `PsClass::Lake` / `Sea` / `LakeWeather` | `DrawLake` | `shaders/water_lamps_ps.hlsl` (macros SEA, WEATHER) | `tools/water_lamp_check/` |
| 7 | Leaves and bushes | VS 6 (`PatchFoliageVs` matches) | VS swap in `OnDrawTracked`, then `DrawLeafShadow` / `DrawTreeSunShadow` | `PatchFoliageVs`, `PatchLeafShadow`, `features/foliage_vertex_lamps.cpp:Patch` | `tools/foliage_vertex_test/`, `tools/canopy_test/` |
| 8 | Outside walls | `PsClass::WallGain` (`WallLampConst` over `shaders/wall_lamp_table.h`) | `DrawWallGain` | constant rescale; `PatchWallScale` for the outdoor scale | `tools/terrain_lighting_test/` (extracted) |
| 9 | Cinema marquee day bloom | `g_cinemaMarqueeDayPs` tag | `DrawCinemaMarqueeDayBloomGuard` | `BloomThresholdConst` | none |
| 10 | Roads, sidewalks | VS 4 (`IsRoadVs`) | `DrawRoad` | `PatchRoad` | `tools/terrain_lighting_test/` (ground scale) |
| 11 | Winter floor tiles, pool edges | VS 5 (`kFloorVs` or `IsFloorVs`) | `DrawFloor` | `PatchFloor`, `WorldYOutput` | `tools/floor_shade_check/` |
| 12 | Summer outdoor floors | `PsClass::FloorAtlas` (`IsFloorAtlasPs` over `shaders/floor_atlas_table.h`) | `DrawFloorAtlas` | `PatchBakedAtlasPs` + VS `PatchObjectLampVs` | `tools/floor_shade_check/` |
| 13 | Fences, railings, posts, stairs | VS 7 (`IsInstancedStructureVs`) | `DrawInstanced` | `PatchInstancedLamps` | `tools/terrain_lighting_test/` (extracted) |
| 14 | Snow on fence tops and props | VS 8 (`IsSnowCoverVs`) | `DrawSnowCover` via `DrawSnowOnObject` | `PatchSnowCover` | none |
| 15 | Snow on stair tops | VS 9 (`IsSnowReliefVs`) | `DrawSnowRelief` via `DrawSnowOnObject` | `PatchSnowRelief` | none |
| 16 | Lot ground while snow melts | `PsClass::LotLightMelt` | `DrawLotMelt` | `PatchSnowPass` = `PatchLotTerrainMax` + `PatchLotTerrainEdges` | `tools/floor_shade_check/` |
| 17 | Window outer faces (room-lit) | VS 10 + basis PS, rig mode 0 | `DrawWindowOutdoor` | `PatchWindowOutdoor`, `PatchRoomMapReadWalls`, `PatchCubeTint` | none |
| 18 | Indoor objects (rig mode 0) | VS 10 | `DrawIndoorObject` | `PatchIndoorBasis` | UNVERIFIED |
| 19 | Outdoor doors, counters, modular pieces | VS 10, rig mode 1 or 2 | `DrawObjectLamp` (VS copy `ObjectVsFor`) | `PatchObjectLampVs`, `PatchObjectLampPs` | `tools/terrain_lighting_test/` (extracted) |
| 20 | Snowy lot ground (also under rugs) | `PsClass::LotLightSnow` / `LotLightSnowCut` + VS 3 | `DrawLotSnow` | `PatchSnowPass` | `tools/floor_shade_check/` |
| 21 | World grass (terrain chunks) | `WorldCandidate` / `WorldMultiLight` / `WorldCompact` or a terrain pair | terrain branch of `OnDrawInnerCore`; map from `RecordWorldChunk` | `PatchTerrainNativeAlpha`, `PatchTerrainDaylightRange`, `PatchTerrainShade`; constant from `TerrainLampConst` | `tools/terrain_lighting_test/`, `tools/world_terrain_layout_test/` |
| 22 | Snow lying on floor tiles | VS 11, only when no PS class claimed the draw | `DrawSnowFloor` | `PatchSnowFloor` | `tools/floor_shade_check/` |
| 23 | Lot grass, soft lot edges | `PsClass::LotLight` (`kLotLightPs`, `kLotLightRainPs`, `kLotLightStruct`) | lot branch of `OnDrawInnerCore` (`EnsureReplacement`) | HLSL `kReplacementHlsl` (WET macro for rain) | `tools/terrain_lighting_test/` (extracted lot branch) |
| 24 | Furniture at night | VS 10, rig mode 0 (step before `OnDrawInnerCore`) | `OnDrawInner` -> `OnDrawFurniture`, `HoldRig` | `PatchCubeTint`, `PatchRoomMapReadWalls`, `AnalyzeRigPs` | `tools/room_ambient_test/` (unlit_rooms side only) |

## Draw interception and classification

`features/lot_light_bridge.cpp` registers SetPixelShader, SetVertexShader, DrawIndexedPrimitive and DrawPrimitive
hooks, classifies the shaders and dispatches each draw. Docs: [lot light pass](../features/night-lighting/lot-light-pass.md),
[engine shaders](../engine/shaders.md), [history](../history/night-lighting-overview.md).

- **Files:** `features/lot_light_bridge.cpp` (hooks, classes, every handler, lamp lists, lot shade, census),
  `features/lot_light_bridge.h` (API), `shaders/shader_ids.h` (exact size + FNV-1a ids, `IsShader`),
  `shaders/shader_structure.h` (weather variants by structure), `features/shader_lookup_cache.h` (32-slot front cache),
  `features/shader_constant_pair.h` (saves and restores the rows between two constants).
- **Entry points:** `features/lot_light_bridge.cpp:UpdateHooks` registers the hooks when any of the bridge, object, roof,
  water or wall fixes is on. It is called by the setters `LotLightBridge::SetEnabled`, `SetObjectShadowFix`,
  `SetRoofFix`, `SetWaterFix` and `SetWallGain`. Shader hooks -> `TrackPs` / `TrackVs` -> `Classify` / `ClassifyVs`.
  Draw hooks -> `OnDraw` -> `DrawCostSampled` (development builds time 1 draw in 16) -> `OnDrawTracked` ->
  `OnDrawInner` -> `OnDrawInnerCore`. `LotLightBridge::OnPresent` runs from the NightTerrainRelight Present hook.
  `LotLightBridge::Shutdown` and `OnWorldChanged` are called by the orchestrator.
- **Game hooks and addresses:** light enumeration `features/lot_light_bridge.cpp@0x006ACF70` (`EnumerateLights`, byte
  check on Steam). Lamp fields are read under SEH in `ReadLamp`, `ReadExitLamp`, `ReadLotLamp`, `LampLit` and
  `LampPlaceNow` (+0x100 flags, +0xB0 type, +0x120 head, +0x130 range, +0xF0 colour, +0x10 intensity).
- **D3D9 registrations:** all four hooks are named `LotLightBridge` at the default `Priority::Normal`. State changes
  call `D3D9Hooks::CallOriginal*` through `SetPs` / `SetVs` / `SetTex` / `SetPsConst`. The re-issued draw goes through the
  device inside `D3D9Hooks::ReissueScope`, so observers see it.
- **Threads:** render thread for every hook and for `OnPresent`. Gather threads call
  `LotLightBridge::ObjectLampColourAt` and `ObjectReachLawScale`, which read atomics and SEH-guarded lamps only.
  `ShadeWorker` builds the lot and chunk wall shade on a worker thread or on the GPU (`ShadeGpuFrame`), and `ShadeApply`
  applies the result on the render thread.
- **State:** `g_curPs` / `g_curClass` / `g_curVsInfo` (written by `TrackPs` / `TrackVs`, read by every handler);
  `g_classCache`, `g_psFacts`, `g_vsInfo` with `g_pinned` (AddRef until `Shutdown`); `g_inOwnCall` (set around the
  bridge's own calls); `g_stateUnknown`; `g_hookFailed` (set by `HookFailed`); `g_allLamps` (written by
  `ReadEnumeratedLamps` from `OnPresent`, read by `SelectLamps`, with memo `g_lampMemo` / `g_lampMemoGen`); `g_chunks`
  (written by `RecordWorldChunk`). All in `features/lot_light_bridge.cpp`.
- **Settings:** *Street lamps light lots*, *Ground light*, *Soft lot edges* (developer). See
  [settings](night-game-side.md#settings).
- **Tests:** `tools/terrain_lighting_test/` (`tools/terrain_lighting_test/run.ps1 -Captures <F7 folders>`, needs
  captures) extracts source text from `features/lot_light_bridge.cpp`: `RecordWorldChunk`, `ChunkTex`, `SamplerBind`,
  `ConstGain`, `ConstX`, `DrawWallGain`, `DrawObjectLamp`, `DrawInstanced`, `PickObjectLampsImpl`, `PickFenceLampsImpl`,
  `LampConeAt`, `ShadeLamps`, `MovedApart`, `RefusalHolds`, `NoteLotDraw`, `SnowEdgeBind`, the lot branch of
  `OnDrawInnerCore` and a block of `OnPresent`. `tools/mesh_occlusion_test/` extracts the code from `ReadElement` on
  (needs captured shader binaries). `tools/shader_structure_test/` checks the structure ids (needs the precomp).
  `tools/render_cost_test/` covers `features/shader_constant_pair.h` (needs none).
- **Invariants:** classified shaders stay pinned because the caches are keyed by pointer (`g_pinned`). Every handler
  restores the constants, samplers and shaders it changes, and the lot branch backs out when it cannot save them. An
  exception in a hook only sets `g_hookFailed`, and the fixes stay off until restart (`HookFailed`,
  `ReportHookFailure`). `Shutdown` unregisters the hooks before it releases shaders.
- **Rejected:** caches keyed by pointer without AddRef; a size-only weather-variant guess ->
  [README rejected approaches](../features/night-lighting/README.md), [history](../history/night-lighting-lot-light-pass.md).
- **Fragile:** the extraction-based tests match exact text, including the anchor comments "// Foliage vertex shaders
  are swapped", "// The colour an outdoor object" and "// POD only (SEH): one wall per piece of the group", so renaming
  any extracted symbol breaks them. The FNV-1a hash is reimplemented in `WallLampConst`, `IsFloorAtlasPs`,
  `SaveRefused` and `shaders/shader_ids.h:ShaderHash`. The setter names are crossed: `SetObjectPixelLamps` writes
  `g_objPixel`, and `SetObjectPixelLights` writes `g_objPixelLamps`.

### F7 `mod:` line

`features/light_probe.cpp` records draws at `Priority::Last`, also observes re-issued draws, and stores
`LotLightBridge::DescribeDraw` as the `mod:` line of each draw. Inside a bridge re-issue (`g_inOwnCall`), it returns
`g_objDrawInfo`, which handlers fill only while `LightProbe::Capturing` ("mod draw: world terrain", "mod draw: lot
light pass", "FENCE/STAIRS", "OBJECT fixed by the mod" from `DescribeObjectDraw`, "WINDOW", floors, foliage). Otherwise
it returns "game (the mod did not replace this draw) | VS <class> | PS <class> | rig mode N", plus `g_terrainWhy`. Its VS
label table stops at class 11, so the sea and lake-weather classes print "not classified".

### Log lines

- `[LotLightBridge] Active` / `Failed: ...`: `EnsureReplacement`. `Object shadow fix:`, `Roofs:`, `Snowy roofs:`,
  `Water:`, `Snow`, `Melting snow`: the matching `Ensure*` and `Draw*` functions.
- `[LotLightBridge] <what>: shader XXXXXXXX patched` / `refused by D3D` / `does not have the expected pattern`: one line
  per shader at its first draw, from `features/lot_light_bridge.cpp:PatchedFor`. In development builds `SaveRefused`
  keeps refused bytecode in `ShadersRecusados`.
- `Lot light pass: variant recognised by its structure`, `Ground brightness: terrain shader ...`, `Terrain chunk (x, z):
  ...`, and `Exception inside the draw hook` (error).

## Shader pattern patchers

`features/shader_patches.cpp` holds pure bytecode functions: a recogniser finds a game shader shape and a patcher
splices in Apex lighting. They use no device and no globals. Docs: [README](../features/night-lighting/README.md),
[engine shaders](../engine/shaders.md).

- **Files:** `features/shader_patches.cpp` (parser `Parse` / `Scan` / `Apply`, helpers `AppendLampLaw`,
  `AppendGroundShade`, `BicubicTaps`), `features/shader_patches.h` (result structs), `features/foliage_vertex_lamps.cpp`
  (per-vertex foliage lamps, `PatchTreeSunShadow`).
- **Entry points:** called through `features/lot_light_bridge.cpp:PatchedFor`, which builds one copy per game shader and
  cache, or from `ClassifyVs` / `PatchedVsCode`. The surface finder lists which patcher serves which surface.
- **Game hooks and addresses:** none.
- **Threads:** render thread at run time. The offline harnesses call the same functions.
- **State:** none in `features/shader_patches.cpp`. `features/foliage_vertex_lamps.cpp:LastRefusal` keeps the last
  refusal text.
- **Settings:** none. Each copy follows its handler's settings.
- **Tests:** `tools/floor_shade_check/`, `tools/room_map_smooth_check/`, `tools/basis_shadow_check/`,
  `tools/room_surface_check/` (each runs as `run.ps1 -Precomp <Shaders_Win32.precomp> [-Captures ...]` and needs the
  precomp and a GPU), `tools/world_terrain_layout_test/` (precomp + captured shaders),
  `tools/foliage_vertex_test/` (captured VS or precomp; D3D9 software vertex processing), `tools/basis_test/`.
- **Invariants:** a patch returns false and leaves the game's shader when the pattern is absent. Copies follow native
  D3D9 validation rules, not only DXVK's. ps_2_0 copies are checked against the device caps (`RoomMapCapsOk`).
- **Rejected:** `lrp` with two constant sources (refused by native D3D9) ->
  [foliage](../features/night-lighting/foliage.md); exact-byte VS classes for floors and roads ->
  [floors](../features/night-lighting/floors.md), [roads](../features/night-lighting/roads.md).
- **Fragile:** the token helpers `Num` / `Type` / `Reg` are duplicated in `features/foliage_vertex_lamps.cpp`, and
  `Classify` decodes sampler declarations by hand. `ClassifyVs` runs `PatchFoliageVs` / `PatchObjectLampVs` to classify
  and runs them again in `PatchedVsCode`. The order in `ClassifyVs` matters: snow floors must stay last or they take
  terrain and water draws ([snow](../features/night-lighting/snow.md)).

## Outdoor surface fixes

The handlers in rows 4-6, 8, 10-16 and 20-23 make lamp light match on walls, floors, roads, snow, roofs, water, fences
and the ground. Docs: [walls](../features/night-lighting/walls.md), [floors](../features/night-lighting/floors.md),
[roads](../features/night-lighting/roads.md), [snow](../features/night-lighting/snow.md),
[roofs](../features/night-lighting/roofs.md), [water](../features/night-lighting/water.md),
[fences](../features/night-lighting/fences.md), [stairs](../features/night-lighting/stairs.md),
[validation](../validation/night-lighting-overview.md).

- **Files:** handlers in `features/lot_light_bridge.cpp`; replacements `shaders/roof_ps.hlsl`,
  `shaders/roof_snow_lamps_ps.hlsl`, `shaders/water_lamps_ps.hlsl` with their generated `*_hlsl.h` headers; tables
  `shaders/wall_lamp_table.h`, `shaders/floor_atlas_table.h`.
- **Entry points:** only `OnDrawInnerCore`. Shared helpers are `PatchedFor`, `SelectLamps` (up to 16 lamps from the draw's
  world position), `RecordWorldChunk`, `LotMaskFor`, `FindLotRect`, `LotEdgeConstants` and `ChunkShadeTexture`.
- **Gates:** `g_roofFix` / `g_roofStrength` (`SetRoofFix`); `g_waterFix` and its strength, reflection, filter and
  colour flags (`SetWaterFix`); `g_wallEnabled` / `g_wallGain` (`SetWallGain`); `g_fenceFix` / `g_fenceStrength`
  (`SetFenceGroundLight`); `g_sidewalkClear`; `g_groundGain` (`SetGroundBrightness`); `g_softEdges`.
- **Threads:** render thread. The lot wall shade is built on `ShadeWorker` or the GPU.
- **State:** `g_wallConst` (from `Classify`); `g_nativeWallScale` (written by `DrawWallGain`, read on any thread through
  `NativeWallScale`); `g_lotMasks`, `g_shadeQueue`, `g_chunkShades`; `g_lotRects` (from `RefreshLotRects`).
- **Settings:** *Lamps light walls*, *Lamps light roofs*, *Lamp Glow*, *Glow brightness*, water reflection strength,
  *Fences and stairs catch light*, *Sidewalk visibility*, *Walls shade lamp light near the ground*. See
  [settings](night-game-side.md#settings).
- **Tests:** see the surface finder. `tools/water_lamp_check/` compiles `shaders/water_lamps_ps.hlsl` in a hidden D3D9
  window (GPU). Roofs, snowy roofs and snow on objects have no harness.
- **Invariants:** walls keep the game's alpha (the bloom mask), and the gain goes to RGB only (`DrawWallGain`). Lamps are
  picked from the draw's world position, never from the camera (the sea is the exception). The lamp radius comes from
  range +0x130, not from the light bounds +0x134 (`ReadLamp`). Snowy roofs and stair snow share PS bytes, and the VS
  decides between them.
- **Rejected:** the light-bounds radius (roofs and water blew out to white) -> [roofs](../features/night-lighting/roofs.md);
  the ocean's planar reflection for lakes -> [history](../history/night-lighting-water.md); one cache for both
  snow-floor variants -> [snow](../features/night-lighting/snow.md).
- **Fragile:** each `.hlsl` / `*_hlsl.h` pair must be edited together (generator UNVERIFIED). The wall and floor tables
  are generated offline from the precomp. `DrawWallGain` keeps a static memo of the last PS, invalidated by
  `g_patchedGen`.

## Objects, rigs and lamp colour

Outdoor objects, fences and Sims take lamp light the way the ground does. Each draw knows its rig mode. Stock pink lamps
turn warm white, trees stay off the underground rig, and lamp colour leaves the night sky probes. Docs:
[objects and rigs](../features/night-lighting/objects-and-rigs.md), [lamp colour](../features/night-lighting/lamp-colour.md),
[engine](../engine/light-objects-and-rigs.md), [validation](../validation/night-lighting-objects-and-rigs.md),
[history](../history/night-lighting-objects-and-rigs.md).

- **Files:** `features/object_light_bridge.cpp` (colour vfunc wrappers at the rig gather, cap raise, `RigCtorForce`,
  room-rig cell gather, lamp colour redirect), `features/rig_tracker.cpp` (rig bound for the current part draw),
  `features/tree_light_rig.cpp` (cluster rig key), `features/probe_neutral.cpp` (probe cubes towards luma at night).
  Draw side: `DrawObjectLamp`, `DrawObjectRig`, `DrawIndoorObject`, `DrawWindowOutdoor`, `PickObjectLamps` and
  `ObjectLampColourAtImpl` in `features/lot_light_bridge.cpp`.
- **Entry points:** `ObjectLightBridge::Install` / `InstallLampColour` / `OnPresent`, `RigTracker::Install`,
  `TreeLightRig::Install` and `ProbeNeutral::Install` / `Set`, all from the orchestrator.
  `features/lot_light_bridge.cpp:RigMode` reads `RigTracker::CurrentMode` once per draw (`DrawRigScope`).
- **Game hooks and addresses:** street lamp colour `features/object_light_bridge.cpp@0x006C02A0` (vtable slot, original
  checked), rig gather return `features/object_light_bridge.cpp@0x006BB2B3` (`AfterCallReg`), cap operand
  `features/object_light_bridge.cpp@0x006B9418` (bytes validated), room gather call
  `features/object_light_bridge.cpp@0x006BBE70`; model draw `features/rig_tracker.cpp@0x006F6250` and instance flush
  `features/rig_tracker.cpp@0x006CF920` (Detours; prologue bytes checked on Steam), binder call
  `features/rig_tracker.cpp@0x006F68C5`; tree setters `features/tree_light_rig.h@0x0070D370` and
  `features/tree_light_rig.h@0x0070D800` (EntryChain); probe writers `features/probe_neutral.h@0x006B4720` and
  `features/probe_neutral.h@0x006B4B10` (EntryChain).
- **Threads:** colour wrappers and room gathers run on the game's gather threads (`t_openShade` is thread_local).
  `DirtyAllRigs` and the regathers run on the render thread only. RigTracker compares the TEB thread id with the draw
  thread. TreeLightRig and ProbeNeutral run on whatever thread calls the game setters or writers (UNVERIFIED).
- **State:** `features/object_light_bridge.cpp:g_capScaled` (written by `OnPresent`, read through the patched operand on
  gather threads); `g_refreshRequested` -> `DirtyAllRigs`; `g_roomRigSet` under `g_roomRigMx`;
  `features/rig_tracker.cpp:g_rig` / `g_depth`.
- **Settings:** *Lamps light objects*, *Light stairs, railings, columns*, *Doors and windows stay lit*, *Seamless
  light on pieces*, *Windows take outdoor light*, *Buildings block lamp light on trees and objects*, *Lamp colors stay
  near lamps*; the lamp tint keys have no menu row. See [settings](night-game-side.md#settings).
- **Tests:** `tools/terrain_lighting_test/` (extracted `DrawObjectLamp` and pickers). No harness covers
  object_light_bridge, rig_tracker, tree_light_rig or probe_neutral.
- **Invariants:** the colour wrapper changes the result only for calls from the rig gather. The cap is the game's cap
  x max(1, strength). The light system is touched only on the render thread. RigTracker clears the rig on every model
  draw and instance flush, so nested draws do not leak a rig.
- **Rejected:** boosting only the street-lamp class; a 3x cap; `RigCtorForce` as the fence fix ->
  [objects and rigs](../features/night-lighting/objects-and-rigs.md); hooking only lamp creation for colour ->
  [lamp colour](../features/night-lighting/lamp-colour.md).
- **Fragile:** rig+0x1D4 (mode) is read in `features/rig_tracker.cpp:ReadMode` and in
  `features/object_light_bridge.cpp:AfterRoomGather`. The lamp head +0x120 is read here and in the bridge's `ReadLamp`
  family. `CellGatherForRoomRig` zeroes rig+0x1E0 for the duration of one call, under `__finally`.

## Foliage

Leaves take lamp light inside the moon shadow through wrap light and the moon-shadow PS. Development builds add
per-vertex lamps, crowns as porous occluders of ground light, and the sun shadow on trees. Docs:
[foliage](../features/night-lighting/foliage.md), [validation](../validation/night-lighting-foliage.md),
[history](../history/night-lighting-foliage.md).

- **Files:** `features/foliage_vertex_lamps.cpp` (`Patch`, `PatchTreeSunShadow`), `features/foliage_vertex_lamps.h`
  (`SlotConstants`, `FoliageLaw`), `features/foliage_canopy.h` (`Transmission`, `ShadowBox`), and in
  `features/lot_light_bridge.cpp`: `FoliageVsFor`, `FoliageVertexVsFor`, `FoliageVertexConstants`, `PickFoliageLamps`,
  `VoteFoliageOrigin`, `CorrectFoliageEye`, `CrownsIn`, `ReadFoliageLamps`.
- **Entry points:** `OnDrawTracked` swaps the VS and sets the wrap weights (c255), plus c[base..base+81] and c54..c80
  for per-vertex lamps. `OnDrawInnerCore` then runs `DrawLeafShadow` / `DrawTreeSunShadow`. `ReadFoliageLamps` runs
  from `OnPresent`.
- **State:** `g_foliageLamps` / `g_foliageLampGen`, `g_foliageInstances`, `g_crowns`, `g_foliageOrigin`.
- **Settings:** *Lamps light objects* gates the wrap copy. The development switches are `DevSwitches::FoliageVertexLamps`,
  `CanopyGroundShadow` and `FoliageSunShadow` in `features/dev_switches.h`.
- **Tests:** `tools/foliage_vertex_test/` (captured VS or precomp), `tools/canopy_test/` (needs none).
- **Invariants:** c255 and the replaced constants are restored after the draw. A draw whose lamps cannot all be matched
  keeps the plain wrap copy.
- **Fragile:** `s_pvSaved` / `s_colSaved` in `OnDrawTracked` are sized to the constant layout documented in
  `features/foliage_vertex_lamps.h`. The instance position space is learned by vote, and per-vertex lamps wait until
  the vote decides.

## Smooth room light maps and directional basis

Indoor lamp light without blocks, rings or dark sides. Room maps are read as a B-spline and basis maps smoothly,
indoor objects read the room maps per pixel, and texels get a dither before the 8-bit pack. Docs:
[room map smoothing](../features/night-lighting/room-map-smoothing.md), [engine](../engine/room-light-maps.md),
[validation](../validation/night-lighting-room-map-smoothing.md),
[history](../history/night-lighting-room-map-smoothing.md).

- **Files:** `features/room_map_padding.cpp` (`IsBasisPs`, `NoteDraw`, `BasisFor`), `features/room_map_dither.cpp`
  (`Start`, `DitherPack`; `Offset` in the header). Draw side in `features/lot_light_bridge.cpp`: `DrawBasisSmooth`,
  `DrawRoomMapSmooth`, `DrawRoomMapWalls`, `DrawIndoorObject`, `IndoorBasisScale`, `RoomMapConsts`, `RoomMapCapsOk`,
  `MapSizeThisFrame`.
- **Entry points:** `RoomMapPadding::NoteDraw` from `OnDrawInnerCore`. `RoomMapPadding::OnPresent`,
  `RoomMapDither::Start` / `Stop` / `SetEnabled`, `LotLightBridge::SetSmoothRoomMaps` and `SetIndoorSmooth` from the
  orchestrator.
- **Game hooks and addresses:** texel pack call `features/room_map_dither.cpp@0x006A34DB` -> 0x0060CD10 inside
  `features/room_map_dither.cpp@0x006A31D0` (byte checks with `CodeIs`; Steam only).
- **Threads:** the draw side runs on the render thread. `DitherPack` runs on the light tree thread or the render thread
  and reads only the caller's registers.
- **State:** `g_basisPs`, `g_roomMapPs` (from `Classify`); `g_indoorSmooth`, `g_roomMapSmooth`; `g_roomMapFrame`
  (incremented in `OnPresent`).
- **Settings:** *Smooth light maps* (its parts sit behind `TesterToggles::smoothBSpline` and its siblings in
  `features/tester_toggles.h`), *Smooth indoor light*. See [settings](night-game-side.md#settings).
- **Tests:** `tools/room_map_smooth_check/` (`IsRoomMapPs`, `PatchRoomMapSmooth`, `RoomMapUvScale`,
  `RoomMapDither::Offset`; precomp + GPU), `tools/basis_shadow_check/`, `tools/room_surface_check/`.
- **Invariants:** the dither offset depends only on texel position, so a re-solve gives the same texels. Basis copies
  cap their bicubic reads by the native read (`kBasisPreserveShadows`). `DrawIndoorObject` acts only on rig mode 0 and
  ps_3_0.
- **Rejected:** 16 texels per metre, and screen-space shadows ->
  [history](../history/night-lighting-room-map-smoothing.md).
- **Fragile:** `features/room_map_padding.cpp:NoteDraw` and `features/lot_light_bridge.cpp:Classify` both decode
  sampler declarations by hand. `g_roomMapFrame` is declared `extern` several times before it is defined.

## Rooms at Night and Light detail

Rooms with every lamp off show a controlled background light instead of the game's blue glow, and furniture follows it
on each draw. Light detail doubles the lighting texels per tile, once per session, before any lot is lit. Docs:
[unlit rooms](../features/night-lighting/unlit-rooms.md), [validation](../validation/night-lighting-unlit-rooms.md),
[history](../history/night-lighting-unlit-rooms.md), [engine](../engine/room-light-maps.md).

- **Files:** `features/unlit_rooms.cpp` (six imm32 redirects, `BaseHook`, `Retint`, furniture queries),
  `features/light_detail.cpp` (`ApplyAtStartup`, `CheckLayout`). Furniture draw side in `features/lot_light_bridge.cpp`:
  `OnDrawInner`, `OnDrawFurniture`, `RoomAmbientChroma`, `HoldRig`, `ReleaseRig`.
- **Entry points:** `UnlitRooms::Install`, `Set`, `OnPresent` and `SetNightLevel` from the orchestrator.
  `UnlitRooms::SetDrawDark` and `FurnitureActive` from `OnDrawInner`. `LightDetail::ApplyAtStartup` at install.
- **Game hooks and addresses:** unlit colours `features/unlit_rooms.cpp@0x011D0B60` / `features/unlit_rooms.cpp@0x011D0B40`
  (opcode bytes checked, all six sites or none), fill gate 0x01158D5C, top-up base
  0x006A00A0; per-class tables `features/light_detail.cpp@0x00FF36AC`,
  `features/light_detail.cpp@0x00FF36DC`, `features/light_detail.cpp@0x00FF36E8` and the class-2 layout push at
  `features/light_detail.cpp@0x006A7E62` (byte checks).
- **Threads:** `BaseHook` runs on the light tree thread. Retint and the per-draw furniture constants run on the render
  thread.
- **Settings:** *Adjust the background light*, *Brightness*, *Blue tint*; *Light detail > Detail*. See
  [settings](night-game-side.md#settings).
- **Tests:** `tools/room_ambient_test/` compiles `features/unlit_rooms.cpp` directly (`tools/room_ambient_test/run.ps1`,
  needs none). Light detail and the furniture draw path have no harness.
- **Invariants:** exact `IsUnlitLight` tests with no colour fallback. A held rig drawn by the game is drawn inside the
  hold. Light detail writes nothing when another mod has already changed the tables, and takes effect only after a
  restart once a lot is lit.
- **Rejected:** relighting every room after each change; matching rooms by colour ->
  [unlit rooms](../features/night-lighting/unlit-rooms.md).
- **Fragile:** `tools/room_ambient_test/` includes `features/unlit_rooms.cpp` directly, so a new include or global there
  must also build in the test.
