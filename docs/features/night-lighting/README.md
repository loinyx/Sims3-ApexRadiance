# Night Lighting

Night Lighting (menu name **Night Lights**) rebuilds The Sims 3's lamp light while the game draws. Street-lamp light no
longer stops in a straight line at lot borders, lamps on lots light the world grass and roads around them, and walls,
floors, every story of a house, objects, fences, foliage, roofs, ponds and snow all receive the light of nearby lamps.
The light on the ground is smooth instead of blocky, and it follows lamps that are placed, moved, recoloured or switched
in Build mode and at dusk. By day, lamps that the game keeps lit add a subdued glow instead of their full night strength.

## Status

| | |
|---|---|
| Availability | Released (standalone since 0.1.0; current published version 2.5.6). Daylight composition, day/night phase updates, lot UV alignment and squared terrain lamp scales: in development (PR #2) |
| Default | On (`enabledByDefault = true`; the feature metadata keeps `experimental = true`) |
| Menu | Lighting page (tabs Overview, Ground, Objects, Buildings, Stories); World > Water & Snow page (Lamp Glow, Water Reflections, Snow); developer options under Developer > Lighting |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Game build | Steam 1.67.2 (`supportedVersions = VERSION_STEAM`); every patch site is byte-checked. Other builds resolve addresses by signature, see [engine/game-versions.md](../../engine/game-versions.md) |
| Source | [`patches/night_terrain_relight_patch.cpp`](../../../patches/night_terrain_relight_patch.cpp), [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) and the modules listed under *Module map* |

Older names in notes and code: "Night Remake", "Iluminacao melhorada", "Lot Edge Lighting" (a dev-only predecessor,
`patches/lot_edge_lighting_patch.cpp`, never shipped).

## The problem

The game lights the night through several unrelated paths, measured with Light Probe captures (Ctrl+Shift+F7):

| Surface | Where its lamp light comes from in the stock game | Defect |
|---|---|---|
| World grass (terrain chunks) | Per-chunk 256x256 DXT5 light map ("StaticTerrainLightmap"), baked by `FUN_00C292B0` from world lights only | Blocky 1 texel/m circles, RGB565 colour specks, cut at 256 m chunk borders (bake defect), lot lamps absent |
| Lot grass | CPU room solve of room 0 (`FUN_006be020`), 1/d^2 from the lamp head, drawn by a modulate2x light pass | Street lamps arrive very faint: straight cut at the lot border |
| Roads, sidewalks | Their own copy of the chunk light map, without the lamps | Dark roads next to lit grass |
| Walls, floors | Per-story room light maps (atlas per level) | Only the lamps of that story: cut at the floor line; walls much dimmer than objects |
| Objects, fences, foliage | Per-object "rig": sun + 3 strongest lamps at the object centre | Many objects get nothing (flag, cut-off 0.1, vertex-light slots empty); moon shadow removes lamp light; modular pieces differ |
| Roofs, lake water | No lamp term at all | Black roofs and ponds at night |
| Snow variants | Separate shaders (snow lot pass, snow on floors, fence tops, stair tops) | Each misses lamp light in its own way |

The game also never rebuilds the terrain light when lamps switch on at dusk, so a save loaded by day keeps a
"lamps off" ground until something else forces a rebuild. Background: [engine/terrain-and-light-bake.md](../../engine/terrain-and-light-bake.md),
[engine/room-light-maps.md](../../engine/room-light-maps.md), [engine/light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md).

## How Apex Radiance solves it

Night Lighting fixes each path where the game computes it, with two kinds of change:

- **Game-code patches** (byte-checked) for the terrain bake, light gathering and rebuild triggers: lot lamps enter the
  world terrain bake, the terrain is rebuilt at dusk and after lamp edits, and lamps are shared between stories.
- **D3D9 draw interception** that swaps in patched copies of the game's own shaders (pattern-patched bytecode or small
  HLSL replacements) for one draw, binds extra textures and constants, draws, and restores the device state.

The common data source is the terrain light: the game's chunk light maps are smoothed and copied into one **world light
atlas** that any surface knowing its world position can sample. Each sub-part below uses it or its own fix.

### Sub-parts

| Part | One line | Page |
|---|---|---|
| Lot light pass | Lot grass draws `max(lot map, terrain light)`, with a 3 m soft edge, so street-lamp light has no cut at lot borders | [lot-light-pass.md](lot-light-pass.md) |
| World atlas and smoothed maps | Chunk light maps upscaled 4x and cleaned of DXT noise; one world atlas for every consumer; daytime terrain composition | [world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md) |
| Terrain relight | Lot lamps in the terrain bake, dusk and daylight rebuilds, local per-lamp relights and paced sweeps | [terrain-relight.md](terrain-relight.md) |
| World lamp response | Edits of world-owned street lamps reach the terrain and native object rigs | [world-lamp-response.md](world-lamp-response.md) |
| Level light share | Outdoor and indoor lamps light every story, with the game's wall test | [level-light-share.md](level-light-share.md) |
| Walls | Exterior walls get a configurable lamp gain, also by day | [walls.md](walls.md) |
| Floors | Outdoor floors read the world atlas | [floors.md](floors.md) |
| Roads | Roads and sidewalks read the smoothed terrain light | [roads.md](roads.md) |
| Snow | Snowy lot pass, floors, sills, stair tops and fence tops get lamp light | [snow.md](snow.md) |
| Objects and rigs | Per-pixel lamps on outdoor objects, bake-matched falloff, rig boosts | [objects-and-rigs.md](objects-and-rigs.md) |
| Fences | Fences, railings and stairs read the ground light | [fences.md](fences.md) |
| Foliage | Bushes and trees keep lamp light in moon shadow | [foliage.md](foliage.md) |
| Roofs | Roofs and roof snow receive lamp light | [roofs.md](roofs.md) |
| Water | Ponds glow and reflect lamps | [water.md](water.md), [../reflections.md](../reflections.md) |
| Lamp colour | Stock pink lamps re-coloured towards warm white | [lamp-colour.md](lamp-colour.md) |
| Unlit rooms | Rooms at Night: the background light of rooms with lamps off | [unlit-rooms.md](unlit-rooms.md) |
| Every-Story Ground Light | Separate patch `SplitLevelGroundLight`: lamps on any story light the ground | [level-light-share.md](level-light-share.md) |

## Settings

All keys live under `[patches.NightTerrainRelight]` together with `enabled`. They are registered in the
`NightTerrainRelightPatch` constructor with `RegisterBoolSetting` / `RegisterFloatSetting`; floats are clamped to their
range on load. The keys are the TOML names of saved configurations and are never renamed. Settings marked *developer*
are registered only when developer mode is on at startup (`kPublicBuild` false); otherwise they keep the default shown.
Menu labels without a page prefix are on the Lighting page; the water and snow rows are on the World > Water & Snow page.
*Individual options* means the setting has no card in the normal menu and is reachable only through Developer >
Lighting > "Individual options (for tests)" (developer mode) or the TOML file.

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Ground > Street lamps light lots | `luzDoPosteNaGramaDoLote` | bool | on | | Lot light pass and every draw handler after it ([lot-light-pass](lot-light-pass.md)) |
| Ground > Lot lamps light the street | `luzDoLoteNaGrama` | bool | on | | Outdoor lot lamps in the terrain bake ([terrain-relight](terrain-relight.md)) |
| Ground > Smooth ground light | `mapaDeLuzSuavizado` | bool | on | | Smoothed chunk maps and the world atlas ([world-atlas](world-atlas-and-smoothed-maps.md)) |
| Ground > Updates > Update at dusk | `automaticoAoAnoitecer` | bool | on | | Rebuild the terrain light at the settled day and night endpoints |
| Ground > Updates > Delay after dusk | `atrasoSegundos` | float | 2.0 s | 0.5 to 10 s | Wait before the endpoint rebuild (none while Build mode editing) |
| Ground intensity > Ground brightness | `brilhoNoChao` | float | 100% | 25 to 300% | Lamp scale on terrain, lot grass, snowy lot grass and floors; needs *Street lamps light lots* |
| Ground intensity > Roads and sidewalks | `brilhoNasRuas` | float | 100% | 25 to 300% | Road lamp scale, multiplied by the ground brightness ([roads](roads.md)) |
| Ground intensity > Street lamp brightness | `forcaDosPostes` | float | 100% | 25 to 300% | Street-lamp colour in the terrain bake; one terrain rebuild on slider release |
| Ground intensity > Lot lamp brightness | `forcaDasLampadasDoLote` | float | 100% | 25 to 300% | Lot-lamp colour in the terrain bake (rebuild on release) and the lot's own light map on its grass (live) |
| Objects > Lamps light objects | `postesNosObjetos` | bool | on | | Rig lamp boost and moon-shadow fix ([objects-and-rigs](objects-and-rigs.md), [foliage](foliage.md)) |
| Objects > Brightness | `forcaNosObjetos` | float | 100% | 25 to 300% | Strength of lamps on objects |
| Objects > Light stairs, railings, columns | `lampadasEmTodosObjetos` | bool | on | | Objects the game leaves without lamp light; applies when a world loads ("Reload save" badge) |
| Doors, counters and fences > Doors and windows stay lit | `objetosDeForaComLuzDoChao` | bool | on | | Outdoor rig objects get at least the ground light (`RigTracker`); needs the ground light |
| Doors, counters and fences > Seamless light on pieces | `luzPorPixelNosObjetos` | bool | on | | Per-pixel world lamps on outdoor rig objects; needs the ground light |
| Doors, counters and fences > Seamless light brightness | `forcaLuzPorPixelNosObjetos` | float | 100% | 25 to 300% | Strength of those per-pixel lamps |
| Doors, counters and fences > Fences and stairs catch light | `cercasComLuzDoChao` | bool | on | | Fences, railings, stairs and their snow read the atlas ([fences](fences.md), [snow](snow.md)); needs the ground light |
| Doors, counters and fences > Fence brightness | `forcaNasCercas` | float | 100% | 25 to 200% | Strength on fences, railings, stairs and their snow |
| Indoor objects > Smooth indoor light (Experimental) | `bordasDosMapasDeLuz` | bool | on | | Indoor objects and stairs read the room's directional light maps smoothly (see *Smooth indoor light*) |
| Buildings > Lamps light walls | `paredesComLuz` | bool | on | | Exterior wall lamp gain by day and night; off keeps the native draw ([walls](walls.md)) |
| Buildings > Brightness (walls) | `forcaNasParedes` | float | 200% | 25 to 400% | Wall lamp RGB multiplier (`SetWallGain` clamps 0.25 to 8) |
| Buildings > Lamps light roofs | `telhadosComLuz` | bool | on | | Roof lamp pass ([roofs](roofs.md)) |
| Buildings > Brightness (roofs) | `forcaNosTelhados` | float | 60% | 5 to 200% | Roof lamp strength |
| Rooms at Night > Adjust the background light | `comodosEscurosSemLuz` | bool | on | | Rooms with lamps off keep the light set below ([unlit-rooms](unlit-rooms.md)) |
| Rooms at Night > Brightness | `luzQueSobraNosComodos` | float | 35% | 10 to 80% | How much of the game's unlit-room light stays |
| Rooms at Night > Blue tint | `azulNosComodos` | float | 0% | 0 to 100% | 0% neutral grey, 100% the game's blue |
| Stories > Outdoor light between floors | `luzExternaEntreAndares` | bool | on | | Outdoor lamps light every story ([level-light-share](level-light-share.md)) |
| Stories > Indoor light between floors | `luzInternaEntreAndares` | bool | on | | Indoor lamps through stairwells and open floors; needs the switch above |
| Stories > Floor detail > Seamless walls between floors | `paredesSemEmendaEntreAndares` | bool | on | | Walls lit at the heights the game draws their light; needs outdoor light between floors |
| Stories > Floor detail > Every floor in full detail | `todosOsAndaresEmDetalhe` | bool | on | | Every floor of the active lot solved in full detail |
| Water & Snow > Lamp Glow > Lamps glow on ponds | `lagosRefletemLampadas` | bool | on | | Pond lamp pass ([water](water.md)) |
| Water & Snow > Lamp Glow > Glow brightness | `brilhoNaAgua` | float | 40% | 10 to 40% | Lamp glow and sparkles on ponds |
| Water & Snow > Water Reflections > Reflection brightness | `reflexoNoLago` | float | 100% | 0 to 300% (slider from 5%) | Shore reflection; needs Depth Blur ([../reflections.md](../reflections.md)) |
| Water & Snow > Snow > Sidewalk visibility | `calcadaComNevePisada` | float | 50% | 0 to 100% | Sidewalk concrete showing through trodden snow; needs *Street lamps light lots* ([roads](roads.md)) |
| Individual options | `luzDasLampadasNatural` | float | 1.0 | 0 to 1 | Stock lamp colour: 0 pink (game), 1 warm white; applied on release ([lamp-colour](lamp-colour.md)) |
| Individual options | `corPropriaNoLote` | bool | off | | Lot lamps get their own colour |
| Individual options | `corDasLampadasDoLote` | float | 1.0 | 0 to 1 | That lot lamp colour |
| Individual options | `luar` | float | 1.0 | 0 to 2 | Moonlight at night (see *Brightness controls*) |
| Individual options | `postesAcesosNoCalculo` | bool | off | | Experimental: street lamps count as lit in lot light solves (0x6BE18C); reinstall |
| Individual options | `qualidadeAltaEmTodosOsLotes` | bool | off | | Experimental: every lot at the active lot's quality; reinstall; lots loaded afterwards |
| Individual options | `gramaDoLoteUsaLuzDoLote` | bool | off | | Experimental: lot pass keeps "no terrain lightmap" (0xC7F87D); reinstall |
| Individual options | `recalcularLotesAoAnoitecer` | bool | off | | Experimental: re-solve room 0 of every lot after the dusk rebuild |
| Developer > Water highlights | `waterSpecularFilter` | bool | on | | Stabilise lamp sparkles on water (A/B) |
| Developer > Water highlights | `waterPreserveLampColors` | bool | on | | Preserve bright lamp colours on water (A/B) |
| Developer: Soft lot edges (A/B) | `bordaSuaveLote` | bool | on | | Developer only; off = plain max of lot and ground light |
| Developer: Smooth the ground light maps on the GPU (A/B) | `mapaDeLuzSuavizadoNaGpu` | bool | on | | Developer only; off = CPU worker path |
| Developer: Relight only nearby terrain | `relightNearbyChunks` | bool | off | | Developer only; automatic lamp changes also take the local relight |
| Developer: Paced terrain sweep | `relightPacedSweep` | bool | on | | Developer only (registration); the default is active in normal mode too |

Notes:

- **Lighting balance** (Lighting > Overview): *Subtle*, *Soft*, *Natural* and *Custom* set nine brightness values at
  once (order: ground, roads, street lamps, lot lamps, objects, pieces, fences, walls, roofs). Water, moonlight, lamp
  colours and room background light are not part of a style. *Undo choice* restores the previous values.

  | Style | Ground | Roads | Street | Lot | Objects | Pieces | Fences | Walls | Roofs |
  |---|---|---|---|---|---|---|---|---|---|
  | Subtle | 0.675 | 1.0 | 0.72 | 0.72 | 0.675 | 0.675 | 0.675 | 1.35 | 0.405 |
  | Soft | 0.75 | 1.0 | 0.8 | 0.8 | 0.75 | 0.75 | 0.75 | 1.5 | 0.45 |
  | Natural (defaults) | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 2.0 | 0.6 |

- **Dependencies** (`BeginDisabled`): `groundLight = g_bridge && g_smoothMaps` gates the three Doors, counters and
  fences options (they read the world atlas, which exists only with both on; a button turns both on). Ground and road
  brightness and *Sidewalk visibility* need *Street lamps light lots*. Street and lot lamp brightness need the bake stub
  (`g_bakeGainInstalled`). Strength sliders are hidden while their switch is off.
- **Live vs reinstall.** Every menu edit goes through `Edit()`: `ApplyLive` installs or removes the parts that follow
  their switch (`LotLightBridge::SetEnabled`, `ObjectLightBridge::Install/Uninstall`, `LevelLightShare::Install/Uninstall`,
  `RigTracker::Install/Uninstall`), the configuration is saved and an automatic lighting refresh runs 1 s after the last
  change (`RequestAutoRefresh`). `Update()` schedules a reinstall only when `postesAcesosNoCalculo`,
  `qualidadeAltaEmTodosOsLotes` or `gramaDoLoteUsaLuzDoLote` changed (they change code bytes), or when `luzDoLoteNaGrama`
  is on but its code could not be installed. `ReinstallNow` runs on the render thread through `DeferredReinstall`
  (`RenderCallbacks::endSceneBeforeOverlay`) and keeps the world state and ground light maps
  (`LotLightBridge::Shutdown(true)`). Profiles, looks and undo apply through `ApplyTableLive` the same way.
- `ResetDefaults` in `night_terrain_relight_patch.cpp` still lists the defaults above but has no caller: there is no
  reset button for Night Lighting, its pages or its cards (including water).

### Refresh controls

| Where | Control | Action |
|---|---|---|
| Lighting > Overview, Refresh lighting card (also Developer > Lighting) | Refresh terrain / Refresh lots / Refresh lights | Arm a terrain rebuild (`g_kickRequested`); queue room 0 of every loaded lot story; `NightLighting::RefreshAll`: terrain, lots, every room and the object rigs |
| Buildings > Rooms at Night | Refresh the lighting (with its shortcut chip) | `RefreshAll("button")` |
| Hotkey | Refresh the lighting | Same as above |

### Status lines

| Where | Source | Values |
|---|---|---|
| Developer > Lighting > Inspect lighting state | `g_status` (`OnPresent`) | "Waiting for the game to load a world", "World loading: ...", "World loaded: rebuilding ...", "Rebuild pending: the game only rebuilds the terrain at night (or in Build mode)" (stale wording, see [terrain-relight.md](terrain-relight.md)), "Rebuild in N frames", "Night: ok", "Day: ok" |
| Surface and provider state | `LightDiag::Status`, `LotLightBridge::Status`, `LotEdgeStatus`, `ObjectLightBridge::Status`, `ObjectStatus`, `WallStatus`, `GroundBrightnessStatus`, `RoomMapPadding::Status`, `IndoorSmoothStatus`, terrain bake gains and moonlight, `RoofStatus`, `WaterStatus`, `LightmapSmooth::Status` and `CompareStatus`, `LampColourStatus`, `LevelLightShare::Status`, `UnlitRooms::Status` | One line each |
| Rebuild events and terrain tests | `RenderDeveloperUI` | Last event, night level and countdowns +0x38/+0x3C, terrain armed / rebuilt, world load, lamp changes, lamp change decisions, chunk re-render notices, local relight and sweep counters, `ChunkRelight::Status`, lot relights, "Street lamps counted as lit", lot-lamp arms / baked / off |

## Compatibility and interactions

- **Every-Story Ground Light** (`SplitLevelGroundLight`, Lighting > Stories > "Upper floors light the ground") zeroes
  `GetLotID` (0x006BC020) for the terrain bake so lot lamps on any story enter it. When the official Sims3SettingsSetter
  already applies its Split-Level Lighting Fix, the row shows on and locked. Inferred from the decompile of
  `FUN_00C292B0` (`re/out/fn_00c292b0.c` line 114: `GetLotID() == 0 || light+0xD0 == 0`): with lot id 0 every lamp the
  visitor accepts passes the bake's lot/story test, including basement lot lamps, which the lamp tracking does not model.
- **Depth Blur**: the lake pass reads its INTZ depth and marks its own pass as internal; Water Reflections need Depth
  Blur ([water](water.md)).
- **Picture filters, Edge Smoothing, Ambient Occlusion**: post-scene effects, no interaction with the draw hooks.
- **D3D9 hook order**: the bridge's draw hooks return Skip after drawing, which ends the hook chain for that draw. The
  handlers' own state calls go straight to the device below Apex's detours (`D3D9Hooks::CallOriginal*`); the replaced
  draw itself is re-issued through the device, so the Frame Profiler, Light Probe, Frame Capture and post-scene trigger
  counts see it.
- **Compare with the game** (shortcut) turns Night Lighting off together with the post-scene effects.
- **Hardware**: pixel shader 3.0 for the replacements; the GPU smoothing path needs float render targets and falls back
  to the CPU path otherwise.

## Limitations

- Ground light (atlas) has no height and no wall occlusion, and the terrain stamp itself has no wall occlusion
  (inferred), so `max(lot, terrain)` can show street-lamp light through lot walls.
- Paths still drawn only by the game: multi-pass world terrain light passes other than the exact captured summer pair
  (PS 756 B / `EC3141AB`, VS 656 B / `5882F972`); the winter lot light pass with VS 436BB272/1348 (needs the exact `kSnowLotVs`); TerrainLow distant terrain;
  Sims.
- Walls get a gain, not per-pixel lamps; per-pixel lamps on walls and floors are not implemented.
- Lamp colour, lot lamp colour and moonlight have no card in the normal menu.
- There is no single control for night darkness or lamp range (see the history).
- The daytime lamp response is visual tuning, not a physical model.

## Technical reference

### Module map

| File | Role |
|---|---|
| [`patches/night_terrain_relight_patch.cpp`](../../../patches/night_terrain_relight_patch.cpp) | The patch class, all settings, the menu pieces (`DrawGroundCard`, `DrawObjectsCard`, `DrawBuildingsCard`, `DrawRoomsCard`, `DrawStoriesCard`, `DrawWaterCard`, `DrawSnowCard`, `DrawLightingBalance`, `DrawRefreshCard`, `RenderDeveloperUI`), terrain bake patches, dusk and daylight rebuilds, lamp change decisions, Present driver, `APEX_REGISTER_FEATURE` (`displayName = "Night Lights"`, category Graphics, `gameCodeGroup = "NightLights"`) |
| [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) | D3D9 draw interception: classification, dispatch, every `Draw*` handler, HLSL replacements (`kReplacementHlsl`, `kObjectRigHlsl`), lamp enumeration and selection (`g_allLamps`, `SelectLamps`, `SelectPixelLamps`), lamp tracking and bake snapshots, census and false colour, `DescribeDraw` for F7 |
| [`features/shader_patches.cpp`](../../../features/shader_patches.cpp) | Pure functions on D3D9 bytecode: recognisers (`IsRoadVs`, `IsFloorVs`, `IsSnowFloorVs`, `IsSnowCoverVs`, `IsSnowReliefVs`, `IsInstancedStructureVs`) and patchers (`PatchRoad`, `PatchFloor`, `PatchSnowFloor`, `PatchBakedAtlasPs`, `PatchSnowCover`, `PatchSnowRelief`, `PatchInstancedLamps`, `PatchObjectLampVs/Ps`, `PatchFoliageVs`, `PatchLeafShadow`, `PatchBasisSmooth`, `PatchIndoorBasis`, `PatchTerrainNativeAlpha`, `PatchTerrainDaylightRange`), `LightMapScaleConst`, `CodeBytes`. Testable offline |
| [`features/terrain_lighting_policy.h`](../../../features/terrain_lighting_policy.h) | Day/night lamp scales (`DayLampScale`, `SurfaceLampGain`, `WallLampScale`, `LampScale`), day-edit deferral, phase cycle |
| [`features/lightmap_smooth.cpp`](../../../features/lightmap_smooth.cpp) | Smoothed 1024x1024 chunk light maps and the world light atlas |
| [`features/terrain_chunk_relight.cpp`](../../../features/terrain_chunk_relight.cpp) | Local terrain relight and paced sweep (`ChunkRelight`) |
| [`features/world_lamp_policy.h`](../../../features/world_lamp_policy.h) | World-owned lamp eligibility and priority edits |
| [`features/level_light_share.cpp`](../../../features/level_light_share.cpp) | Lamps shared across stories, cross-story wall test |
| [`features/object_light_bridge.cpp`](../../../features/object_light_bridge.cpp) | Rig lamp boost for all light classes, lamp colour, stairs/railings rig flag, fenced-area gather, rig refresh |
| [`features/rig_tracker.cpp`](../../../features/rig_tracker.cpp) | Which rig (mode 0/1/2, centre) lit the current draw |
| [`features/room_map_padding.cpp`](../../../features/room_map_padding.cpp) | Remembers the directional basis maps of each room light map (smooth indoor light) |
| [`features/unlit_rooms.cpp`](../../../features/unlit_rooms.cpp) | Rooms at Night |
| [`shaders/shader_ids.h`](../../../shaders/shader_ids.h) | Size + FNV-1a of the exact-match game shaders (never the bytecode) |
| [`shaders/wall_lamp_table.h`](../../../shaders/wall_lamp_table.h) | 58 ExteriorWall PS: size, FNV-1a, lamp constant K |
| [`shaders/floor_atlas_table.h`](../../../shaders/floor_atlas_table.h) | 261 ExteriorFloors PS accepted by `PatchBakedAtlasPs` |
| `shaders/roof_ps.hlsl`, `roof_snow_lamps_ps.hlsl`, `water_lamps_ps.hlsl`, `lightmap_smooth_ps.hlsl` (+ `_hlsl.h`) | HLSL sources embedded as generated headers |
| `features/light_probe.cpp`, `features/light_diag.cpp` | Dev tools F7 / F8 ([../dev-tools/light-probe.md](../dev-tools/light-probe.md), [../dev-tools/light-diag.md](../dev-tools/light-diag.md)) |
| `framework/render_callbacks.h` | `endSceneBeforeOverlay` (deferred reinstall) and `preReset` (`LightmapSmooth::OnPreReset`) |
| `framework/shader_cache.h` | Start-up background compile of the HLSL replacements ([architecture](../../architecture.md#44-shader-precompile)) |
| `framework/d3d9_extra_hooks.h`, `features/depth_share.h` | Raw depth-stencil get/set and the INTZ depth used by the lake pass |
| `build_flavor.h` | `kPublicBuild` (runtime: true while developer mode is off) |

### Per frame (Present, render thread = the game's main thread)

The patch registers one Present callback (`D3D9Hooks::RegisterPresent("NightTerrainRelight", ..., Priority::Last)`):

1. once: logs `[NightTerrainRelight] Shader limits: PS 3.0 N instruction slots, VS 3.0 M, PS version X` (D3DCAPS9);
2. `OnPresent()` of the patch: `LightDiag::OnPresent`, `Recorder::OnPresent`, world detection, load / dusk / daylight
   rebuilds, lamp change decisions, arriving lots, `ChunkRelight::OnPresent`, lot relight, moonlight, status
   ([terrain-relight.md](terrain-relight.md));
3. developer mode: `LightProbe::OnPresent` (Ctrl+Shift+F7);
4. `ObjectLightBridge::SetStrength/SetAllObjects/OnPresent`, `LevelLightShare::OnPresent`;
5. `LotLightBridge::SetNightLevel(lightMgr+0xF0)`, `SetRoofFix`, `SetWaterFix`, `LotLightBridge::OnPresent` (shader log
   flush; every 20 frames, or earlier on request, the light enumeration `FUN_006ACF70` that feeds the lamp lists);
6. `LightmapSmooth::SetEnabled`, the remaining setters (`SetSidewalkClear`, `SetLampTint`, `SetFenceGroundLight`,
   `SetWallGain(g_wallStrength, g_walls)`, `SetObjectPixelLamps(g_objPixel && RigTracker::IsInstalled(), ...)`,
   `SetObjectPixelLights`), `LightmapSmooth::SetGpuPreferred`, then `LightmapSmooth::OnPresent(device)`.

### Per draw (`lot_light_bridge.cpp`)

`LotLightBridge::UpdateHooks` registers `SetPixelShader`, `SetVertexShader`, `DrawIndexedPrimitive`, `DrawPrimitive`
and (Priority::Last) `CreatePixelShader` / `CreateVertexShader` hooks named `LotLightBridge` whenever the bridge, the
object fix, the roof fix, the water fix or the wall gain (`g_wallEnabled`) is on.

**Classification**, cached per shader pointer; every classified shader is AddRef'd into `g_pinned` until `Shutdown`
(pointer reuse would give a new shader an old class).

- Pixel shader (`ClassifyPsCode`, `PsClass`): exact size + FNV-1a from `shader_ids.h` (`LotLight` 568 B, `ObjectRig`
  600, `Roof` 1136, `Lake` 1344, `LotLightSnow` 1852, `RoofSnow` 4992, `WorldMultiLight` 756 / `EC3141AB`,
  `WorldCompact` 1296 / `73376C6A`), then `WallGain` (58-entry table), `FloorAtlas` (261-entry table), then
  `WorldCandidate` = any PS that declares a sampler s6 or higher, else `Other`.
- Vertex shader (`ClassifyVs`, one `VsInfo` per shader): exact `kRoofVs` = 1, `kLakeVs` = 2, `kSnowLotVs` = 3,
  `kFloorVs` = 5, plus flags `worldMultiLight` (`kWorldMultiLightVs` 656 / `5882F972`), `worldCompact`
  (`kWorldCompactVs` 744 / `34E1F1B7`) and `contractedLotUv` (`kLotLightVs` 680 / `0C8CC5E8`); then by pattern, in this
  order: `IsRoadVs` = 4, `IsInstancedStructureVs` = 7, `IsSnowCoverVs` = 8, `IsSnowReliefVs` = 9, `IsFloorVs` = 5,
  `PatchFoliageVs` = 6, `PatchObjectLampVs` = 10, `IsSnowFloorVs` = 11 (last on purpose), else 0.

**Dispatch** (`OnDrawInnerCore`, first match wins; a handler returns Skip after drawing, or Continue so the game draws
unchanged):

| Order | Condition | Handler | Page |
|---|---|---|---|
| 0 | VS class 6 and object fix on | patched foliage VS bound around everything below | [foliage](foliage.md) |
| 1 | PS reads room basis maps, not fence / snow relief / rig object | `DrawBasisSmooth` (smooth indoor light) | this page |
| 2 | PS `ObjectRig` | `DrawObjectRig` (moon-shadow-free HLSL, c3.x = night) | [foliage](foliage.md) |
| 3 | PS `Roof` | `DrawRoof` | [roofs](roofs.md) |
| 4 | PS `RoofSnow` and VS not class 9 | `DrawRoofSnow` (additive pass) | [roofs](roofs.md) |
| 5 | PS `Lake` | `DrawLake` (additive pass) | [water](water.md) |
| 6 | VS class 6 | `DrawLeafShadow` | [foliage](foliage.md) |
| 7 | PS `WallGain` | `DrawWallGain` | [walls](walls.md) |
| - | bridge off (`luzDoPosteNaGramaDoLote` false) | stop here | |
| 8 | VS class 4 | `DrawRoad` | [roads](roads.md) |
| 9 | VS class 5 | `DrawFloor` | [floors](floors.md), [snow](snow.md) |
| 10 | PS `FloorAtlas` and VS not class 11 | `DrawFloorAtlas` | [floors](floors.md) |
| 11 | VS class 7 | `DrawInstanced` | [fences](fences.md) |
| 12 | VS class 8 | `DrawSnowCover` | [snow](snow.md) |
| 13 | VS class 9 | `DrawSnowRelief` | [snow](snow.md) |
| 14 | VS class 10 | `DrawIndoorObject`, then `DrawObjectLamp` (falls through otherwise) | [objects-and-rigs](objects-and-rigs.md) |
| 15 | PS `LotLightSnow` | `DrawLotSnow` | [snow](snow.md), [lot-light-pass](lot-light-pass.md) |
| 16 | PS `WorldCandidate`, `WorldMultiLight` (with its VS) or `WorldCompact` (with its VS) | `RecordWorldChunk` + world terrain draw; if not a chunk and VS class 11: `DrawSnowFloor` | [world-atlas](world-atlas-and-smoothed-maps.md), [snow](snow.md) |
| 17 | PS not `LotLight` | VS class 11: `DrawSnowFloor`; else game | [snow](snow.md) |
| 18 | PS `LotLight` | the lot light pass replacement | [lot-light-pass](lot-light-pass.md) |

- **Own draws** set `g_inOwnCall` so the hooks ignore the mod's own calls; shaders the module creates go through
  `OwnCreatePs/OwnCreateVs` (thread-local `t_ownCreate`) so the create hooks do not classify them.
- **Pre-compilation**: the five HLSL replacements of `lot_light_bridge.cpp` (lot pass ps_3_0, object rig ps_2_0, roofs,
  lake water, snowy roofs) and the eight smoothing shaders of `lightmap_smooth.cpp` are compiled at start-up on a
  background thread (`framework/shader_cache.h`); the draw hooks only create the objects from the bytecode. Foliage VS
  copies are pooled (`g_vsPool`, max 64). Pattern patches stay lazy (made at first draw) because which patch applies
  depends on the VS a shader is drawn with.
- **Robustness**: hooks catch C++ exceptions (`HookFailed` switches everything off until restart);
  `g_stateUnknown` reads the bound VS/PS from the device at the first draw after the hooks register; `UpdateHooks` holds
  a mutex; `Shutdown` clears every fix flag before `SetEnabled(false)` so hooks are unregistered before shaders are
  released.
- **CPU cost per replaced draw**: the handlers' own `SetPixelShader` / `SetVertexShader` / `SetTexture` /
  `Set*ShaderConstantF` use the wrappers `SetPs` / `SetVs` / `SetTex` / `SetPsConst` / `SetVsConst` over
  `D3D9Hooks::CallOriginal*`; `SetSamplerState` / `SetRenderState` are plain device calls. `SamplerBind` sets and
  restores only the sampler states and texture that differ. `TrackPs` / `TrackVs` do nothing when the game sets the same
  shader again. `SelectLamps` results are memoized per (x, z, maxScore) bit pattern in a 512-entry direct-mapped table
  (`g_lampMemo`) until the lamp list changes. `RecordWorldChunk` hands back the chunk's `g_chunks` entry. The lamp
  refresh appears in the Frame Profiler as "Lamp refresh (mod)".

### Daylight policy (`terrain_lighting_policy.h`)

At night level n (lightMgr+0xF0, 0 day to 1 night) and a configured gain g:

| Function | Formula | Used by |
|---|---|---|
| `NightWeighted(g)` (lot_light_bridge.cpp) | `1 + (g - 1) n` | Ground, road and lot-map gains |
| `DayLampScale(n, g)` | `(1 - n) g` | Lot pass terrain term (c31.y), terrain lamp scale |
| `SurfaceLampGain(n, g)` | `d + (g - d) n`, `d = min(g, 1) x 0.08` (`kDaySurfaceResponse`) | Fences and stairs, snow on objects, per-pixel object lamps |
| `WallLampScale(K, n, g)` | `K g + DayLampScale(n, min(g, 1) x 0.08)` | Exterior wall cK.x |
| `LampScale(K, n, g, squared)` | linear: `K w + (1 - n) g`; squared: `sqrt(K^2 w + (1 - n) g)`; `w = 1 + (g - 1) n`; at n = 1 exactly `K w` or `K sqrt(w)` | World terrain lamp constant ([world-atlas](world-atlas-and-smoothed-maps.md)) |

Native solar and sky constants are never raised.

### Brightness controls

- **Ground brightness / Roads and sidewalks** (live): for one draw, the game's lamp scale constant of a light-map term is
  scaled: the terrain chunk PS (`TerrainLampConst` via `ShaderPatches::LightMapScaleConst`, c7 in the captured single
  pass chunks, c3 squared in the summer multi-pass, a squared c4.x in the recognised winter multi-pass layout; -1 = that
  shader keeps the game's brightness), the lot pass c3.x, the snowy lot pass c4.x, the floors' `FloorPatch.scaleConst`
  and the roads' `RoadPatch.scaleConst` (x the road factor). Only while *Street lamps light lots* is on.
- **Street lamp / Lot lamp brightness** (on the ground): `BakeColourStub` replaces `movaps xmm0,[edi+0F0h]`
  (`0F 28 87 F0 00 00 00`) at 0x00C2950F in the terrain bake: the colour copied to the bake's shader parameter is
  multiplied by `g_bakeStreetMul` (lot id +0xC0/+0xC4 = 0) or `g_bakeLotMul`. Lamp objects, rigs, room solves and the
  lamp tracking keep the real colour. A change applies on slider release as one forced terrain update (`NoteEdit` switch
  path). Lot lamps also scale the lot's own light map on its grass (lot pass c31.x, live), because lot grass is
  `max(lot map, terrain)` and the bake alone cannot dim a lot's own lamps there. Objects, walls and roofs use their own
  sliders.
- **Lamp colour, own colour for lot lamps**: the colour thunks record every stock-pink light (pink + written colour); on
  release `RetintLamps` rewrites those that still hold the written colour through `FUN_006bc3e0` (+0xF0 / +0xE0), then
  the rigs regather, one terrain update and one lot re-solve (room 0). Street lamp = class 0xB with lot id 0. Interiors
  re-solve when the game next solves them. Stock lamps are pruned every 30 s. See [lamp-colour.md](lamp-colour.md).
- **Moonlight**: the "Sunlight Scale" float at 0x011D0918 (only reader `FUN_00c11ad0` at 0x00C11B01,
  `mov ecx,011D0918h; call`, which multiplies the sun / moon colour before SetSun: ExteriorLightData, read by 88
  techniques including Sims) is written every frame as its base value x `lerp(1, luar, night level)`; restored on
  uninstall. A value written by another mod (Sims3SettingsSetter's "Sunlight brightness") becomes the new base. Rigs
  copy the sun colour when they gather, so they are refreshed on slider release. Things in moon shadow do not change:
  this is the moon, not the night ambient.

### Smooth indoor light (`bordasDosMapasDeLuz`)

Instanced objects and stairs read the four directional room light maps with a bicubic filter averaged over the house
(alpha) texels only (`ShaderPatches::PatchBasisSmooth`, `DrawBasisSmooth`). Indoor rig objects (rig mode 0,
`DrawIndoorObject` + `PatchIndoorBasis`) get those maps per pixel instead of the lamps of their per-object rig diffuse:
the diffuse becomes the rig's unlit-room lights (lamps zeroed, `diffuseConst`) + basis x strength, vertex lights zeroed.
The basis maps are read at their own scale: `IndoorBasisScale` sets the size constant's .xy to
`coverage_m x basis texels / 64`, the coverage taken from the VS constants that build the uv
(`ShaderPatches::UvRowConsts`, the light-map uv semantic recorded by `PatchIndoorBasis`), else room light map size / 4.
Only 64x64 basis maps (the only size captured) use it. The room light map covers the lot's power-of-two size at
4 texels/m; the basis maps always cover 64 x 64 m (game basis VS: uv = lot half-metres x 1/128). `RoomMapPadding` keeps
the directional maps of each room light map while the game holds both (reference count checked every 300 frames); the
30-frame look is per (+X basis map, shader). The game's textures are never written. Details of the room maps:
[engine/room-light-maps.md](../../engine/room-light-maps.md).

### Game-code side

| Module | What it changes | Page |
|---|---|---|
| night_terrain_relight_patch.cpp | Terrain bake visitor 0xC29626, arm sites 0x6B6516 / 0x6B60D3 / 0x6B6618, chunk render call 0xC8504C, bake colour 0xC2950F, sunlight scale 0x011D0918; experimental 0x6BE18C, 0xADB66B / 0xADB884, 0xC7F87D | [terrain-relight](terrain-relight.md), [lot-light-pass](lot-light-pass.md) |
| split_level_ground_light_patch.cpp | `GetLotID` 0x006BC020 zeroed; gather call 0x006B635D kept vanilla | [level-light-share](level-light-share.md) |
| level_light_share.cpp | Room-0 gather 0x6C5816 / 0x6C7094, cascade jcc 0x6C73B1, solve-point calls, light vfunc+0x4C of the 9 classes | [level-light-share](level-light-share.md) |
| object_light_bridge.cpp | Light-colour vfunc+0x10 of the light classes, rig brightness cap read 0x6B9418, rig constructor calls in `FUN_006f7880`, room gather thunk 0x6BBE70, lamp colour 0x6B0BDE + creation sites | [objects-and-rigs](objects-and-rigs.md), [lamp-colour](lamp-colour.md) |
| rig_tracker.cpp | Binder call 0x6F68C5, Detours on `FUN_006f6250` and `FUN_006cf920` | [objects-and-rigs](objects-and-rigs.md) |

### Game addresses owned by the patch file

| Address | What | Verification |
|---|---|---|
| 0x006E97B0 | Root getter `A1 <imm32> 85 C0 75 01 C3 8B 80 C0 01 00 00`; imm32 = address of the root pointer (0x011D1860); lightMgr = *(root+0x1C0) | Bytes compared with imm32 masked; Fail "Light manager code differs at 0x6E97B0" |
| lightMgr+0xF0 | Night level (0 day to 1 night); "night" = > 0.99, "day" = < 0.01 | Read every frame |
| lightMgr+0x104 | Light cells; +0x38 / +0x3C countdowns | Read every frame |
| 0x00C29626 | Terrain bake light visitor (in 0xC29620, vtable 0x010768A0) | `8B 07 8B 50 20 8B F1 8B CF FF D2` |
| 0x006B6516, 0x006B60D3, 0x006B6618 | Light register / remove / move-toggle arm tests | `8B 17 8B 42 20 8B CF FF D0` |
| 0x00C8504C | Chunk texture render call in the terrain sweep | 15 bytes at 0x00C85047 |
| 0x00C2950F | Bake colour copy (`BakeColourStub`) | `0F 28 87 F0 00 00 00` |
| 0x011D0918 | Sunlight scale float | Reader at 0x00C11B01 `B9 18 09 1D 01 E8` |
| 0x006C7160 | Room queue `FUN_006c7160` thiscall(treeLevel, roomId) | `83 EC 2C 53 55 56 33 DB 8B F1` (Fail otherwise) |
| 0x006BE18C | Street lamp colour in the lot solve (experimental) | `0F 28 86 E0 00 00 00` |
| 0x00ADB66B, 0x00ADB884 | Lot quality byte (experimental) | `C6 44 24 0C 00` -> `... 01`. The feature's technical-details text names 0xADB66F / 0xADB888 (the immediate byte); the patched instruction starts 4 bytes earlier |
| 0x00C7F87D | Lot pass terrain lightmap bind (experimental) | 8 bytes + 16 bytes context + 9 bytes at 0xC7F8B7 |
| 0x006ACF70 | Light enumeration (`EnumerateLights`) | `E8 2B 36 00 00 8B 4C 24 04 51 68 40 CF 6A 00` |

### Shader patch conventions

Common to all pattern patches (`shader_patches.cpp` header): extra sampler = highest declared sampler + 1, temporary =
highest temp + 1, new constant = highest constant + 1 (refused if >= 224 or no room); a patch fails and leaves the
shader alone when its pattern is absent; world position comes either from the VS constants c8/c10 (world matrix rows,
`.w` = translation) or from a TEXCOORD the game already writes. Every patch follows native D3D9 validation rules, not
only DXVK's.

### Diagnostics

- `ApexRadiance_LOG.txt`: `[NightTerrainRelight] Installed (...)`, `[LotLightBridge] Active` (lot pass created) or
  `Failed: ...`, and the batched line `[LotLightBridge] Shaders at their first draw: <kind>: ...` (some kinds still log
  Portuguese words).
- Developer mode: `ShadersRecusados\<fix>_PS_<hash>.bin` holds every shader a pattern patch refused (max 300 per session).
- Ctrl+Shift+F7 over a pixel: `ApexRadiance_LightProbe.txt` lists the draws covering it; the `mod:` line
  (`LotLightBridge::DescribeDraw`) says whether the mod redrew it and with which class.
- Ctrl+Shift+F8: `ApexRadiance_LightDiag.txt` (all lights, lots, stories, rooms, the "LUZ POR PIXEL" section).
- Developer > Lighting: "False colour: magenta = gets lamp light but no fix claimed it" and the census
  (`ApexRadiance_Censo.txt`, [../dev-tools/census.md](../dev-tools/census.md)).

## Rejected approaches

- Reinstalling on the message-loop thread: crashed (textures released while drawn).
- Calling `DirtyAllRigs` off the render thread from `Uninstall`.
- Shader caches keyed by pointer without AddRef: stale classes after address reuse.
- An `lrp` with two constant sources: accepted by DXVK, refused by native D3D9.
- Reading 64x64 basis maps with the room light map's uv: furniture dark after floor switches.
- `max(rig, basis)` for indoor objects: brought the per-object lamp back.
- A single night-darkness control and a lamp range control: no consistent engine value; not implemented.

Details in [history](../../history/night-lighting-overview.md).

## See also

- [Validation](../../validation/night-lighting-overview.md)
- [History](../../history/night-lighting-overview.md)
- [Architecture: hooks, registry priorities, shader precompile](../../architecture.md)
- [changes-since-0.1.0.md](../../history/changes-since-0.1.0.md), [removed-features.md](../../removed-features.md)
- Engine: [terrain-and-light-bake](../../engine/terrain-and-light-bake.md), [room-light-maps](../../engine/room-light-maps.md),
  [light-objects-and-rigs](../../engine/light-objects-and-rigs.md), [shaders](../../engine/shaders.md)
