# Code map: Light between stories (level_light_share)

`features/level_light_share.cpp` (about 18,900 lines) makes lamps light every story of a lot: it widens the room light
gather, tests other stories' walls, slabs and openings per texel, lines wall light up with the drawn walls, and keeps
render-thread copies of walls and slabs for objects, fences and the ground. The file is one anonymous namespace (hooks,
gathers, solves, caches) followed by the public `LevelLightShare` API; its header comment numbers the parts 1-5 used
below. Players' and maintainers' view: [Light between stories](../features/night-lighting/level-light-share.md).
Settings are listed on [night game side](night-game-side.md#settings). Back to the [Code map](../code-map.md).

## Lifecycle, threads and shared state

Installs every game patch of the module, runs its per-frame work and owns the cross-thread state. Docs:
[feature](../features/night-lighting/level-light-share.md), [validation](../validation/night-lighting-level-light-share.md),
[history](../history/night-lighting-level-light-share.md), [engine](../engine/room-light-maps.md).

- **Files:** `features/level_light_share.cpp` (everything), `features/level_light_share.h` (public API),
  `patches/night_terrain_relight_patch.cpp` (owner: settings, install, frame call).
- **Entry points:** `features/level_light_share.cpp:Install` and `features/level_light_share.cpp:InstallIndoor`
  (from `patches/night_terrain_relight_patch.cpp:Install` and `ApplyLive`); `features/level_light_share.cpp:Uninstall`;
  `features/level_light_share.cpp:OnPresent` (NightTerrainRelight Present hook, Priority::Last);
  `features/level_light_share.cpp:OnWorldChanged` / `OnWorldLive` (NightTerrainRelight frame update);
  `features/level_light_share.cpp:MaybeClearRooms` (start of every gather); `features/level_light_share.cpp:LoadAddresses`.
- **Game hooks and addresses:** every site comes from `framework/game_addresses.cpp` and is checked before patching with
  `features/level_light_share.cpp:CallsTarget`, `AfterCallReg` or byte compares; `features/level_light_share.cpp:Redirect`
  rewrites the call. Optional groups are all-or-none (`g_lodPatches`, `g_alignPatches`, `g_indoorPatches`, `g_cutPatches`).
- **Threads:** light tree gather (a game thread, not the render thread): every gather, point solve, room solve hook and room
  update hook. `features/level_light_share.cpp:g_gatherThread` is rewritten by each gather; hooks compare
  `features/level_light_share.cpp:ThreadId` and pass other threads straight to the game (counted in `g_otherThread`).
  Render: `OnPresent` and everything it pumps, all setters, the render-thread visibility tests, world change and live.
  Any thread: wall cutout posts, floor set/remove thunks, the shared visibility copy and rig reach queries.
- **State:**
  `features/level_light_share.cpp:g_rooms` (per-room cross-story lamps and grids) and `g_ctx` (per-point context, saved and
  restored in `SolvePoint`): light tree only, no lock.
  Options and generations are atomics (`g_indoorOn`, `g_alignOn`, `g_openingsOn`, `g_roofedOpenOn`, `g_innerOpenOn`,
  `g_floorWallsOn`, `g_indoorGen`, `g_editGen`, `g_wallSnapGen`, `g_roomLampDay`).
  Flags set anywhere and consumed in `OnPresent`: `g_refreshRequested`, `g_alignRequeue`, `g_relightAllAt`, `g_floorTick`,
  `g_structurePending`, `g_groupRigsPending`, `g_focusRelightMgr`, `g_lampEditAt`; `g_clearRooms` goes the other way
  (set on render, consumed by `MaybeClearRooms` on the light tree).
  Light-tree data read by the render thread sits behind mutexes (`g_wallSnapMx`, `g_wallMaskMx`, `g_archMx`, `g_openRigMx`,
  `g_exitMx`, `g_innerMx`, `g_roofedMx`, `g_cutMx`, `g_urgentMx`, `g_deferredMx`) or immutable shared copies (`g_visSnap`).
- **Settings:** master switch `luzExternaEntreAndares` installs or removes the whole module.
- **Invariants:** rooms are sent like the game's own update (invalidate + pending set), never as "changed", so nothing loops
  (header comment, `features/level_light_share.cpp:QueueRoom`); a room in solve state 3 is never invalidated, it is deferred
  and sent by `features/level_light_share.cpp:FlushDeferred`; basements stay as the game has them
  (`features/level_light_share.cpp:ShareOutdoorLights`).
- **Rejected:** signature cascade on every room-0 change; all-members-ready gate for stacked rooms; balcony light share;
  extra-update outdoor gather recovery; 200 ms drag relight throttle -> [history](../history/night-lighting-level-light-share.md).
- **Fragile:**
  - Ready flags written by `Install` and read by light-tree hooks are plain `bool` (`g_indoorReady`, `g_lodReady`,
    `g_ambReady`, `g_enterReady`, `g_floorMaskReady`, `g_wallBaseReady`); which thread runs `Install` is UNVERIFIED.
  - Render-thread sends (`features/level_light_share.cpp:RelightLot`, `RequeueAllRooms`, `SetShadowSoftness`) call the
    game's room-by-id, invalidate and set-insert on tree levels the light tree also mutates; only SEH guards them
    (`features/level_light_share.cpp:QueueRoomSafe`). Safety UNVERIFIED.
  - Room light list (`room+0xC8..+0xCC`) is read raw, with no shared accessor beyond `ListSize`. Readers in
    `features/level_light_share.cpp`: `ListSize`, `ListHolds`, `InList`, `ProbeAmbient`, `QuickLikeRefinement`,
    `ReadWallPeak`, `TakeCrossLotLamps`, `ListHead`, `BounceBelowImpl`, `ReadLightsAndWalls`, `UnderFloorShare`,
    `CrossStoryOnly`, `CrossOnly`, `OpenTotalFill`, `HoldsAnyLamp`, `AmbientHeld`, `StaleLamps`, `ReadNoteLamps`,
    `AllEvalWrapped`, `FirstUnwrappedRead`; elsewhere `features/light_diag.cpp`, `features/outdoor_delta.cpp`,
    `features/rt_outdoor_lamps.cpp`, `features/unlit_rooms.cpp`. Writers: `features/level_light_share.cpp:DropDisabledLotLights`
    (compacts the list end) and `features/level_light_share.cpp:ReplayCall` (swaps both pointers, restored in `__finally`).
  - Other raw offsets repeated across features: outdoor flag `room+0x18` (only named accessor
    `features/level_light_share.cpp:RoomOutdoorFlag`; also lot_load_analysis, outdoor_delta, room_map_dither,
    rt_outdoor_lamps, unlit_rooms, light_diag); LOD class `room+0xF4` (also outdoor_delta, room_light_queue, which writes
    it, rt_outdoor_lamps, light_diag); story `mgr+0x88` (only SEH accessor `features/level_light_share.cpp:SafeMgrStory`;
    also light_detail, lot_load_analysis, outdoor_delta, room_light_queue, rt_outdoor_lamps, light_diag); lot id
    `mgr+0x90/+0x94` (also lot_light_bridge, outdoor_delta, room_light_queue, rt_outdoor_lamps, light_diag; `info+0x90`
    in `GameTakesLight` and lamp_mark_filter is a different field); tree level `tracker+0x6A0+level*0x1A4`
    (`features/level_light_share.cpp:TreeLevel`, copied in `features/light_diag.cpp` and `features/lot_light_bridge.cpp`).
  - Same-name functions: `WallPass` exists twice in `features/level_light_share.cpp` (room solve and render overloads);
    `PumpLightingRestart` exists in it and in `patches/night_terrain_relight_patch.cpp`.

## Room-0 gather sharing and lamp posts of other lots (parts 1-2)

Room 0 of stories 0..7 takes the outdoor lamps of every other story; a story's room 0 regathers only when another story's
outdoor lamps or outside walls really changed.

- **Entry points:** `features/level_light_share.cpp:OutdoorGather` (both calls of
  `features/level_light_share.cpp@0x006C6AB0`, at 0x006C5816 and 0x006C7094) runs the game gather, then
  `DropDisabledLotLights`, `TakeCrossLotLamps`, `NoteRoomStructure`, `ShareOutdoorLights`, `ShareIndoorLights`,
  `ShareRooflessLights`, `ShareRoofedOpenings`, `ShareInnerLamps` in that order. `features/level_light_share.cpp:AfterChangedWalk`
  (via `ChangedClearHook`) replaces the game cascade using `RoomLampSignature` and `LampChange`.
  `features/level_light_share.cpp:SendRoom0DependentsTry` serves edits that mark no room (Instant lamp lighting).
  `features/level_light_share.cpp:RestoreOutdoorGatherTry` restores a lost pending key of room 0.
- **Game hooks and addresses:** cascade `features/level_light_share.cpp@0x006C73AA` (bytes `kCascadeBytes`): jnz -> jl in
  Install, then `90 E9` at 0x006C73B0 in InstallIndoor; root pointer `features/level_light_share.cpp@0x011D1860`.
- **Threads:** light tree; cross-lot events any thread under `g_xlotMx`.
- **Tests:** `tools/room_gather_queue_test/` extracts `OutdoorQueueMembership` and `RestoreOutdoorGatherTry` by regex,
  run `tools/room_gather_queue_test/run.ps1` (needs: none).
- **Invariants:** level-0 lamps count twice, others once; the real story is `room[0]+0x88`, the lot-load path gathers
  through level 0 (`ShareOutdoorLights`); never send the story whose pending set is walked next; a lamp moving less than
  `kMoveMin` is a wobble, not a move.
- **Fragile:** the cascade patch is two overlapping byte patches in two restore vectors (`g_patches`, `g_indoorPatches`)
  whose restore order matters.

## Cross-story point test, slabs and outdoor floor mask (part 3)

A lamp of another story is stopped per texel by the walls of its own and in-between stories and by placed slabs; outdoor
floors of stories 1+ carry the wall-blocked share of room 0's light in their map alpha. Docs:
[floors](../features/night-lighting/floors.md).

- **Files:** `features/lamp_visibility.h` (`LampVis::SlabGate`), `features/lamp_reach.h` (reach law),
  `features/penumbra.h` (soft edges), `features/mesh_occlusion.h` (instanced geometry).
- **Entry points:** `features/level_light_share.cpp:SolvePointBatch` / `SolvePointSingle` -> `SolvePoint` (calls of
  `features/level_light_share.cpp@0x0069FD60`); `features/level_light_share.cpp:LightEvalHook` (vtable +0x4C of the 9
  `kClasses`) -> `CrossFloorShadow`, `WallPassImpl`, `CulledWalls`, `OutdoorEntryImpl`, `MidPass`, `ReachShare`;
  `features/level_light_share.cpp:GameWallTest` (game wall test `features/level_light_share.cpp@0x0069FC40`);
  `features/level_light_share.cpp:WindowChannelHook` (window light vtable +0x1C); floor mask via
  `features/level_light_share.cpp:OutdoorAlphaThunk` at `features/level_light_share.cpp@0x006A333B`, `MaskedFloorRoom0`,
  `MaskWallLists`, `SolvePointBatchImpl`.
- **Threads:** light tree only; other threads get the game's solve unchanged.
- **Settings:** `alcanceRealistaDasLampadas`, `suavidadeDasSombras`, `paredesBloqueiamLuzNosPisos`.
- **Tests:** `tools/exterior_lookup_test/` compares the lookup inside `WallPassImpl` with a reference source, run
  `tools/exterior_lookup_test/run.ps1 -ReferenceSource <file>` (needs: a reference file).
- **Invariants:** the room's own lamps keep only the game's test; per-batch wall culling is mirrored for the other story.
- **Rejected:** light behind balconies -> [history](../history/night-lighting-level-light-share.md).
- **Fragile:** `LightEvalHook` recognises the game loop by `_ReturnAddress() == kLightEvalReturn`; `OutdoorAlphaThunk`
  reads `out[3]` at a fixed stack slot (UNVERIFIED off Steam 1.67.2).

## Stair openings, room update and lamp edits (part 4)

Indoor lamps light the story above or below through stairwells and removed floors; edited lamps' rooms solve first.
Docs: [stairs](../features/night-lighting/stairs.md), [lamp drags](../features/night-lighting/lamp-drags.md).

- **Files:** `features/room_ambient_policy.h` (timing rules), `features/atrium_hold.cpp` (reads edit state).
- **Entry points:** `features/level_light_share.cpp:ShareIndoorLights` (`BuildOpeningMask`, `FloorAt`, `RemovedFloorKey`,
  `RoomsNearOpenings`); per point `IndoorShadow` -> `IndoorPassImpl`, `IndoorBoundaryPass`;
  `features/level_light_share.cpp:FloorSetThunk` / `FloorRemoveThunk` / `LevelCtorHook` (floor edits, any thread);
  `features/level_light_share.cpp:RoomUpdateHook` (pointer push at `features/level_light_share.cpp@0x006C5E2A`) ->
  `FlushHeldMarks`, `BeforeRoomUpdate`, `QueueOpeningRooms`; `features/level_light_share.cpp:QueueRoom`, `MarkUrgent`,
  `NoteLampMark`, `HoldLampMark`, `LampUrgency`, `RelightLot`, `RelightLampSwitch`, `LockStepHook`, `FinalizeHook`;
  `features/level_light_share.cpp:BeginLightingRestart` / `PumpLightingRestart`.
- **State:** `g_urgent` (`g_urgentMx`), `g_lampMarks`, `g_held`, `g_deferred`; floor edits go through `g_levelsMx` and
  `g_floorTick` to `OnPresent`.
- **Settings:** `luzInternaEntreAndares`, `todosOsAndaresEmDetalhe`; `[patches.RoomLightQueue] switchAllAtOnce`.
- **Tests:** `tools/terrain_lighting_test/run_indoor_stories_checks.ps1` (extracts `IndoorBoundaryPass`..`IndoorPass`;
  needs: none); `tools/lighting_restart_test/` (extracts `RestartStory`, `BeginLightingRestart`; run.ps1; needs: none).
- **Invariants:** an opening is a removed floor key (bit 0x40000000 plus other low bits); the floor object's lighting
  manager comes from its owner lot (the copy at +0x238 goes stale); `RoomUpdateHook` restores only the missing pending
  key; `soon` only for lamps already registered in the room.

## Room ambient, LOD, normalisation and bounces

Stacked rooms share one ambient and wall ramp base, rooms seen through an opening take the camera story's LOD, rooms lit
only by other stories are not boosted; development builds add arch ambient and the stairwell bounce. Docs:
[stairwells](../features/night-lighting/stairwells.md), [arches](../features/night-lighting/arches.md),
[unlit rooms](../features/night-lighting/unlit-rooms.md).

- **Files:** `features/opening_bounce.h`, `features/arch_ambient.h`, `features/stairwell_bounce.h` (pure parts),
  `features/unlit_rooms.cpp` (stages ambient changes through this file).
- **Entry points:** `features/level_light_share.cpp:RoomSolveStartHook` (call at 0x006A3D0B) -> `MergeStackedAmbientSafe`,
  `QuickLikeRefinement`; `WallPassHook`; `LodChoiceHook` (4 calls of `features/level_light_share.cpp@0x0069E710`);
  `RoomNormHook`; render side `ApplyPendingAmbient`, `ApplyGroupAmbient`, `StageAmbientBaseChange`,
  `ApplyAmbientBaseChanges`; arches `NoteArchRoom`, `ArchAmbientConsts` (lot_light_bridge, render); bounce
  `StairwellBounceAt`, `BounceRebuild`.
- **Threads:** merges on the light tree; `g_ambToQueue` (`g_ambMx`) applied on render; `g_archRooms` under `g_archMx`.
- **Settings:** dev switches `opening_bounce`, `no_opening_band`, `arch_ambient`, `stairwell_bounce`, `room_fill`,
  `inner_norm_settle` in `[dev_build]` (missing key = on in development builds).
- **Tests:** `tools/room_ambient_test/`, `tools/opening_bounce_test/`, `tools/arch_ambient_test/`,
  `tools/stairwell_bounce_test/`, `tools/room_structure_test/` (extracts `NoteRoomStructure`): run.ps1, needs none;
  `tools/room_map_smooth_check/` needs GPU.
- **Rejected:** fresh group sources -> [history](../history/night-lighting-level-light-share.md); the opening band and
  stronger shared ambient -> [history](../history/night-lighting-stairwells.md); baked arch ambient ->
  [history](../history/night-lighting-arches.md).
- **Fragile:** `ApplyPendingAmbient` writes room ambient from the render thread while the light tree may solve that room
  (state checks only, `AmbientHeld`).

## Wall light alignment and wall heights (part 5)

Wall light rows are lit where they are drawn, walls meet without a step at the floor line, and foundation or taller
walls take light at their drawn height. Docs: [walls](../features/night-lighting/walls.md).

- **Files:** `features/wall_sample_span.h` (pure span rules), `features/wall_heights.cpp` (drawn wall feet from wall
  vertex shader draws; DrawIndexedPrimitive Last + Present Normal, only while a recording runs).
- **Entry points:** `features/level_light_share.cpp:WallSamplesHook` -> `AfterWallSamples`, `AlignWallSamples`;
  `WallSolveHook` -> `GhostSolve`; `WallBlurHook` -> `BlurWalls`; `WallUvHook`; `OutdoorWallBaseThunk` -> `OutdoorWallBase`
  and `IndoorWallBaseThunk`; `RequeueTallWalls` (render).
- **Game hooks and addresses:** wall samples `features/level_light_share.cpp@0x006AC070`, blur
  `features/level_light_share.cpp@0x0069F650` (bytes checked), batch solve `features/level_light_share.cpp@0x006A31D0`,
  base sites 0x006AB32C / 0x006AB340, UV calls 0x00C387B5 / 0x00C387CC.
- **Settings:** `paredesSemEmendaEntreAndares`, `paredesExternasNaAlturaDesenhada`.
- **Tests:** `tools/roofed_openings_test/` (`wall_native_check.cpp`, extracts `WallBlock`, `WallLightBase`, `RecutCellV`,
  `MoveWallFootSamples`); `tools/lighting_prepare_test/` (extracts `BlurWalls`, `StoryMapSlot`); needs none.
- **Invariants:** row k is moved to k*3/(N-1) only after matching the game's formula; the blur runs on class 2 only.
- **Fragile:** the base thunks assume `esi` = wall and `[ebp+10h]` (UNVERIFIED off Steam 1.67.2).

## Openings: cutouts, story grids and light through doors and windows

Doors, windows and arches per lot story feed a per-story wall grid; yards take lamps only through openings, roofed rooms
take garden lamps through them, and lit rooms shine out onto the garden and the next room.

- **Files:** `features/cut_close_hold.h` (close hold), `features/wall_openings.h` (patch profiles),
  `features/opening_lamp_balance.h` (entry scale), `features/rig_cut.h` (used by object_light_bridge for these lamps).
- **Entry points:** `features/level_light_share.cpp:CutAddThunk` / `CutRemoveThunk` -> `NoteCutoutPostImpl` (post sites
  0x00B0D866 / 0x00B0DD15, `InstallCutoutCapture`); `EnsureStoryGrid` -> `EnsureStoryGridImpl` -> `BuildStoryGrid`;
  `GridPassImpl`; yards `ShareRooflessLights`, `GatherSameStory`, `YardShadow`; roofed `ShareRoofedOpenings`,
  `RoofedDoors`, `OpenLampPass`, `OpeningLampFactor`, `OpeningLampReach` (rigs); inner `ShareInnerLamps`, `InnerDoors`,
  `InnerShadow`, `NoteExitLamps`, `ExitLampVisibility`, `SolveGroundTexel`; render retries `RequeueCutStories`,
  `RequeueGridRetries`, `SendInnerRetries`.
- **State:** `g_cuts` (`g_cutMx`, any thread), `g_grids` (light tree only, cleared above 64 entries), `g_roofedRecs`,
  `g_innerRecs`, open-rig and exit copies under their mutexes.
- **Settings:** `luzRealistaPorAberturas`, `portasBloqueiamLuz`, `luzDeForaPelasAberturas`, `luzInternaPelasAberturas`,
  `luzInternaSomaNoChao`, `suavidadeDasSombras`.
- **Tests:** `tools/roofed_openings_test/` extracts about 20 structs and functions between text anchors in its run.ps1
  and asserts that `SendRoom0DependentsTry` still sends `SameStoryRoofed` for two stories (needs: none).
- **Invariants:** cuts are keyed by the post's own lot id, never the manager's; cuts survive uninstall; a closed door
  counts only after the hold; a lamp through an opening lights no more than the same surface outside.
- **Fragile:** `CutAddThunk` reads `edi` and fixed stack slots (UNVERIFIED off Steam 1.67.2); renaming any anchor in
  `tools/roofed_openings_test/run.ps1` breaks the harness.

## Directional maps, soft light cut and daytime curve

The four basis maps get the same floor test, smooth light maps fade lamps at the game's cut, and outdoor lamps in roofed
rooms follow day and night. Docs: [room map smoothing](../features/night-lighting/room-map-smoothing.md).

- **Entry points:** `features/level_light_share.cpp:BasisLightHook` (call at 0x006A0C56, bytes before it checked),
  `NoteWallMask`, render readers `WallMaskFor`, `FloorWallMaskFor`, `AmbientMapFor`, `StoryAboveFor`; `SwapThreshold`,
  `FadeAtThreshold`, `SolveRestoring`; `RoofedDay`, `NoteDayRoom`, `SweepDayRooms` (render).
- **Settings:** `mapasDosComodosSuaves` (soft cut also needs tester toggle `smooth_cut`); dev switch `basis_wall_filter`.
- **Invariants:** no send at twilight; roofed rooms resend once after the day level settles (`kDaySettleMs`).

## Render-thread copies, lamp visibility and cache liveness

Outside walls and slabs are copied on the light tree for objects, fences, windows, trees and the ground shade; caches of
lots no longer loaded are pruned after repeated misses. Docs: [objects and rigs](../features/night-lighting/objects-and-rigs.md).

- **Files:** `features/lamp_cap_shadow.h`, `features/liveness_prune.h`, `features/wall_openings.h`.
- **Entry points:** `features/level_light_share.cpp:CaptureStoryWalls` / `CaptureStoryFloors` (from `SolvePointBatchImpl`),
  `RefreshStoryWallPatches`; render `RefreshGrid`, `LampVisibility`, `WallBlocks`, `LampFloorCap`, `OutsideWallsIn`,
  `PublishVisSnap`; any thread `LampVisibilityShared`, `CrossLampReach`; liveness `PruneDeadStoryCaches`,
  `DropDeadStoryGrids` (light tree), `SweepDeadSnaps` (render).
- **State:** `g_wallSnap` / `g_floorSnap` under `g_wallSnapMx` with `g_wallSnapGen`; render-only `g_grid`; immutable
  `g_visSnap`; `g_liveSweeps` armed by `OnWorldLive`.
- **Settings:** `paredesBloqueiamLuzNosObjetos`, `paredesSombreiamOChao`.
- **Tests:** `tools/liveness_prune_test/`, `tools/mesh_occlusion_test/` (needs none); `tools/floor_shade_check/` and
  `tools/rt_lamps_test/` (GPU; rt_lamps optionally takes World.txt captures).
- **Invariants:** an entry is erased only after `kMissesToErase` sweeps in a row, and an incomplete lot list prunes nothing;
  a world change only arms the sweep.
- **Fragile:** three wall tests over the same data: `WallPassImpl` (light tree), `WallPass` (render), `SharedWallPass` (any).

## Instant lamp lighting interface and diagnostics

What Quick outdoor lamp edits and Real-time outdoor lamps need from the room solves (inert while Lamp lighting is
Classic), plus development diagnostics. Docs: [outdoor lamp delta](../features/night-lighting/outdoor-lamp-delta.md),
[RT outdoor lamps](../features/night-lighting/rt-outdoor-lamps.md), [light diag](../features/dev-tools/light-diag.md).

- **Entry points:** `features/level_light_share.cpp:ReplayBatch` -> `ReplayCall`, `LotGeometryKey`, `PhysicsKey`,
  `QueueRoom0`, `SendRoom0Dependents`, `LockRoomMapsTry`, `ExportLotProxy`; diagnostics `DiagText`, `IndoorDiagText`,
  `OpeningsDiagText`, `TraceRoomsImpl`, `WallSurvey`, `InvalidateNoteHook`, `MeasureGatherMemory`, `MeasureRenderMemory`.
- **Threads:** solve taps run on the light tree only; diagnostic text is built on render.
- **Fragile:** `ReplayCall` swaps the room light list and the global batch sample vector; `g_replayTexels` is one global
  buffer.
