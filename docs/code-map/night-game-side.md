# Code map: Night Lighting, game side, terrain and room queue

This page covers the game-code side of Night Lighting: the `NightTerrainRelight` patch and its Present driver, the
terrain light bake and its rebuilds, the smoothed ground light maps and world atlas, the lot light pass and the ground
shade, world lamp response, the room light queue with lamp edits and drags, and Every-Story Ground Light. Players and
maintainers read the [Night Lighting overview](../features/night-lighting/README.md) and
[Performance](../features/performance/README.md). Back to the [Code map](../code-map.md).

Below, "the patch" is `patches/night_terrain_relight_patch.cpp`. Settings of a feature live in `[patches.<Feature>]`.

## Night Lighting patch and Present driver

Owns every Night Lighting setting, writes the terrain and lot game patches, and drives all Night Lighting parts from one
Present callback. Docs: [feature](../features/night-lighting/README.md), [validation](../validation/night-lighting-overview.md), [history](../history/night-lighting-overview.md).

- **Files:** `patches/night_terrain_relight_patch.cpp` (class, settings, game patches, frame driver, menu cards),
  `patches/night_lighting.h` (menu and hotkey API), `features/night_level.h` (night level, Build-mode sun-time gates),
  `features/world_session.h` (world loaded / drawn gates), `features/lighting_restart_policy.h` ("Restart lighting"
  completion), `features/terrain_lighting_policy.h` (day curve `LampDay`, dusk/daylight `Cycle`).
- **Entry points:** `patches/night_terrain_relight_patch.cpp:NightTerrainRelightPatch` Install registers
  Present "NightTerrainRelight" at `Priority::Last`, whose lambda runs `patches/night_terrain_relight_patch.cpp:OnPresent`
  under `HookGuard::Run`, then every other Night Lighting part's frame update in a fixed order;
  `patches/night_terrain_relight_patch.cpp:PumpLightingRestart` (end of OnPresent);
  `patches/night_terrain_relight_patch.cpp:DeferredReinstall` (EndScene callback) runs `ReinstallNow` after a
  byte-changing option; `NightLighting::RefreshAll` (hotkey, Refresh card, automatic refresh via
  `patches/night_terrain_relight_patch.cpp:RequestAutoRefresh` and `RequestRefreshAfterLoad`); `NightLighting::Restart`.
- **Game hooks and addresses:** root getter `patches/night_terrain_relight_patch.cpp@0x006E97B0` (root+0x1C0 = light
  manager; +0xF0 night level, +0x104 light cells, cells+0x38/+0x3C terrain countdowns), read in
  `patches/night_terrain_relight_patch.cpp:ReadLightState` and `ReadCounters`; sky sun-times call found by pattern and
  sent to `patches/night_terrain_relight_patch.cpp:SunTimesHook`; moonlight scale
  `patches/night_terrain_relight_patch.cpp@0x011D0918` written by `ApplyMoonlight`. All addresses come from `GameAddr`
  in `patches/night_terrain_relight_patch.cpp:LoadAddresses` and are byte-checked before writing.
- **Threads:** render: Present lambda, OnPresent, menu, reinstall, and the game's terrain update and bake (so every
  bake stub); message loop: the class's `Update` only schedules a reinstall.
- **State:** `patches/night_terrain_relight_patch.cpp:g_lastCells` (new world when it changes);
  `patches/night_terrain_relight_patch.cpp:g_menuLevel` (night level for menu and other parts, -1 without a world);
  `patches/night_terrain_relight_patch.cpp:g_live` / `g_loadSettled` (world drawn / load over, read by Depth Blur);
  `patches/night_terrain_relight_patch.cpp:g_restart`; `patches/night_terrain_relight_patch.cpp:g_autoRefreshAt`.
- **Settings:** see the table at the end of this page.
- **Tests:** `tools/lighting_restart_test/` slices `PumpLightingRestart` out of the patch, run
  `tools/lighting_restart_test/run.ps1` (needs: none); `tools/loading_gate_test/` covers `features/world_session.h`.
- **Invariants:** each part of the Present lambda is wrapped in its own `HookGuard::Run`; values that cost a relight
  apply only once a menu slider is released (`patches/night_terrain_relight_patch.cpp:MenuSliderHeld`); a reinstall
  keeps chunk maps, smoothed maps and atlas, a real uninstall frees them.
- **Fragile:** OnPresent runs before `LotLightBridge::OnPresent` in the same lambda, so lamp-edit counts lag one frame;
  developer-only settings are registered depending on the runtime `kPublicBuild` value; the light manager chain is read
  in `ReadLightState`, `LiftObjLive` and `features/night_level.h`; the camera eye is resolved twice
  (`patches/night_terrain_relight_patch.cpp:ResolveCamera`, `features/lot_lighting_motion.cpp:EnsureCamera`).

## Terrain bake and relight

Lamp light on world grass and roads: the dusk and daylight rebuilds, outdoor lot lamps in the world bake, lamp edits
relit chunk by chunk (local relight) or by a paced sweep nearest the camera first. Docs: [feature](../features/night-lighting/terrain-relight.md), [validation](../validation/night-lighting-terrain-relight.md), [history](../history/night-lighting-terrain-relight.md), [engine](../engine/terrain-and-light-bake.md).

- **Files:** the patch (kicks, edit decisions, bake stubs, chunk render thunk), `features/terrain_chunk_relight.cpp`
  (`ChunkRelight`: local queue, paced sweep, disabled paced rebuild), `features/terrain_bake_rule.h` (which lot lights the
  bake takes, area share, low-lamp lift), `features/terrain_bake_cone.h` and `shaders/terrain_bake_ps.hlsl` (lamp cones in
  the bake shader; embedded copy `shaders/terrain_bake_hlsl.h`), `features/terrain_stamps.cpp` (dragged lamps on the GPU).
- **Entry points:** `patches/night_terrain_relight_patch.cpp:Kick` -> `ArmCounters` (load, button, fallback);
  `patches/night_terrain_relight_patch.cpp:RebuildAll` (dusk/daylight from `Cycle::Consume`, edits) ->
  `ChunkRelight::QueueSweep` + `StartSweep`, else Kick; `patches/night_terrain_relight_patch.cpp:NoteEdit`, `EditReady`,
  `DecideEdit`, `TryLocal` -> `ChunkRelight::QueueLocal`; `patches/night_terrain_relight_patch.cpp:RefreshArrivingLots`;
  `patches/night_terrain_relight_patch.cpp:QueueAllLotOutdoorRooms` (lot relight after the dusk rebuild);
  `features/terrain_chunk_relight.cpp:OnPresent` (per frame from the patch); `features/terrain_stamps.cpp:Update`
  (from `features/rt_outdoor_lamps.cpp`).
- **Game hooks and addresses:** bake visitor test `patches/night_terrain_relight_patch.cpp@0x00C29626` ->
  `TerrainLightTest` / `BakeTest`; arm tests `patches/night_terrain_relight_patch.cpp@0x006B6516`, `@0x006B60D3`,
  `@0x006B6618` -> `ArmTestPlaced` / `ArmTestRemoved` / `ArmTestMoved`; bake position
  `patches/night_terrain_relight_patch.cpp@0x00C294F7` -> `BakePosStub`; bake colour
  `patches/night_terrain_relight_patch.cpp@0x00C2950F` -> `BakeColourStub`; chunk render call
  `patches/night_terrain_relight_patch.cpp@0x00C8504C` (to 0x00C7E7A0) -> `ChunkRenderThunk`; bake pixel shader
  replaced in a SetPixelShader hook (`Priority::Last`) by `BindConeShader` after a size + FNV check
  (`features/terrain_bake_cone.h:kGamePsFnv`); terrain update `features/terrain_chunk_relight.cpp@0x00C845C0` consumes
  the countdown at `patches/night_terrain_relight_patch.cpp@0x00C84C1B`.
- **Threads:** render for all of it; the arm tests run on the game's light registration thread (UNVERIFIED which) and
  touch atomics plus `LevelLightShare::CrossLotLightChanged`.
- **State:** `patches/night_terrain_relight_patch.cpp:g_pendingReason` / `g_prevCounter` (armed rebuild, consume seen
  when cells+0x38 becomes -1); `patches/night_terrain_relight_patch.cpp:g_baked` / `g_haveBaked` (lamps of the last
  rebuild); `patches/night_terrain_relight_patch.cpp:g_editKickPending` and friends; `g_localBatches`, `g_sweepId`;
  `patches/night_terrain_relight_patch.cpp:g_bakeExcluded` (dragged lamps the bake gives no colour).
- **Tests:** `tools/terrain_lighting_test/` (`run_dispatch_checks.ps1` DecideEdit guards, `run_lamp_state_checks.ps1`,
  `run_stamped_checks.ps1` AllStamped, `run_queue_checks.ps1` ChunkRelight queue; each `-Repo <repo>`, needs: none;
  the full `run.ps1 -Captures <folder>` needs captures + GPU); `tools/terrain_stamp_test/` runs `BakeConePS`, `StampPS`
  and `features/terrain_stamps.cpp`, run `tools/terrain_stamp_test/run.ps1 -Precomp <Shaders_Win32.precomp>` (needs:
  precomp + GPU).
- **Invariants:** `kArmFrames` = 3 still lets the game's decrement at `patches/night_terrain_relight_patch.cpp@0x006B5DA0`
  run before the consume; no load rebuild during the loading screen (world live + steady level first); lots are relit
  only after the dusk rebuild, never after the load rebuild (re-arm loop); a consumed full rebuild drops pending local
  batches and sweeps; the paced full rebuild stays off (`kPacedRebuildEnabled`) until the chunk loop is traced.
- **Rejected:** fixed-delay load rebuild; rebuilding on any armed +0x38; once-a-second +0x55 reconciliation; relighting
  lots after every rebuild; fixed 30 s automatic rate; CPU-only relight of a dragged lamp; giving cone rows back at every
  render target change -> [history](../history/night-lighting-terrain-relight.md).
- **Fragile:** light offsets (+0xB0 type, +0xC0/+0xC4 lot, +0x100 flags, +0x08 room, +0x134 rect) are read in the
  stubs, `IsOutdoorLotLamp`, `BakeTest`, `ReadBakeFields`, `features/lot_light_bridge.cpp:ReadLotLamp` and
  `features/level_light_share.cpp:InTerrainBake`; `BakeTest` and `NoteBaked` read light memory without SEH inside the
  bake; the "chunk touches rect + 1 m" test exists three times (`features/terrain_chunk_relight.cpp:kMargin`,
  `features/terrain_stamps.cpp:Touches`, `features/lightmap_smooth.cpp:ChunkTouches`); the light tree lot walk
  (tracker+0x6A0 + level x 0x1A4) is repeated in `QueueAllLotOutdoorRooms`, `features/level_light_share.cpp:TreeLevel`
  and `features/light_diag.cpp`; `shaders/terrain_bake_hlsl.h` is a hand-kept copy of the `.hlsl` (no generator in the
  repo); the harnesses cut functions out of the source by name (`DecideEdit` up to `std::string diffText =`,
  `kStampedNear` right before `AllStamped`, `PumpLightingRestart` up to `// ---- menu helpers ----`).

## World atlas and smoothed maps

Each 256 m chunk's DXT5 ground light map gets a 1024x1024 smoothed copy and a cell in a world atlas (2 texels/m) that
lot, road, object and fence draws read. Docs: [feature](../features/night-lighting/world-atlas-and-smoothed-maps.md), [validation](../validation/night-lighting-world-atlas-and-smoothed-maps.md), [history](../history/night-lighting-world-atlas-and-smoothed-maps.md).

- **Files:** `features/lightmap_smooth.cpp` (entries, CPU worker, GPU path, atlas, holds), `shaders/lightmap_smooth_ps.hlsl`
  (copy embedded in `shaders/lightmap_smooth_hlsl.h`), `features/ground_profile.cpp` (developer ground profile in the
  light capture).
- **Entry points:** `LightmapSmooth::OnPresent` (patch Present lambda, last); `LightmapSmooth::Get`, `Find`, `Atlas` from
  `features/lot_light_bridge.cpp` draw hooks (`OnDrawInnerCore`); `LightmapSmooth::NoteChunkRendered` from
  `ChunkRenderThunk`; `NoteKick`, `OnTerrainRebuilt`, `ExpectRebuild`, `HoldChunks`, `ReleaseHold` from the patch;
  `BumpStamps` from `features/terrain_stamps.cpp`.
- **Threads:** render (entries, GPU builds, uploads); one detached worker `features/lightmap_smooth.cpp:WorkerMain`
  for the CPU path (`g_mx`, `g_cv`).
- **State:** `features/lightmap_smooth.cpp:g_entries` (render only); `g_jobs` / `g_results` / `g_shared` (worker
  handoff); `features/lightmap_smooth.cpp:g_noticeKeys` (chunk re-render notices).
- **Settings:** *Smooth ground light* `mapaDeLuzSuavizado`; developer GPU/CPU switch `mapaDeLuzSuavizadoNaGpu`.
- **Tests:** `tools/terrain_stamp_test/` (StampPS); `tools/terrain_lighting_test/run_chunk_map_checks.ps1` covers the
  reader side in `features/lot_light_bridge.cpp:RecordWorldChunk`.
- **Invariants:** correct first: the smoothed copy of an old game map is never shown (Get/Find return null until the
  new one is built); at most 2 CPU results wait; the worker stays detached; the alpha (sun visibility) is never smoothed
  with the lamp RGB; Get/Find/Atlas may draw, so they are called from a draw hook before device state changes.
- **Rejected:** keeping the old smoothed map or a black atlas; re-smoothing on streaming churn; unbounded result queue;
  `D3DUSAGE_AUTOGENMIPMAP`; chunk map from the last draw -> [history](../history/night-lighting-world-atlas-and-smoothed-maps.md).
- **Fragile:** CPU `features/lightmap_smooth.cpp:Process` and the GPU shaders must stay the same math;
  `shaders/lightmap_smooth_hlsl.h` is a hand-kept copy; the key is the chunk centre (256 i + 128) while the render
  notice gives a grid index.

## Lot light pass and ground shade

Lot grass keeps the lot's own map after terrain rebuilds, and walls shade lamp light on lot ground and the grass around
lots. Docs: [feature](../features/night-lighting/lot-light-pass.md), [validation](../validation/night-lighting-lot-light-pass.md), [history](../history/night-lighting-lot-light-pass.md).

- **Files:** the patch (`LotPassStub`, `StreetLampColourStub`, quality sites), `features/ground_lamp_shadow.h` (CPU law,
  `ShadeJob`), `features/ground_shade_gpu.cpp` and `shaders/ground_shade_hlsl.h` (the same law as ps_3_0 passes),
  `features/ground_light_gate.h` (atlas share on objects), `features/native_terrain_sampler.h` (game map on a spare
  sampler); the lot pass shader `kReplacementHlsl` and `ShadeGpuFrame` live in `features/lot_light_bridge.cpp`.
- **Game hooks and addresses:** lot pass terrain bind `patches/night_terrain_relight_patch.cpp@0x00C7F87D` ->
  `LotPassStub` (jumps to the null bind `patches/night_terrain_relight_patch.cpp@0x00C7F8B7`); street lamp colour in lot
  solves `patches/night_terrain_relight_patch.cpp@0x006BE18C` -> `StreetLampColourStub`; all-lots quality
  `patches/night_terrain_relight_patch.cpp@0x00ADB66B` (off by default). All need a reinstall when switched.
- **Threads:** render for GPU builds; CPU shade and chunk builds on a worker (UNVERIFIED which).
- **Tests:** `tools/ground_shade_gpu_test/` engine + shaders vs CPU (needs: GPU); `tools/ground_shade_test/` CPU law
  (needs: none); `tools/roofed_openings_test/` covers `features/ground_light_gate.h`; `tools/terrain_lighting_test/`
  checks `NativeTerrainSampler` state restore and lot pass equivalence (captures + GPU).
- **Invariants:** `LotPassStub` changes only the lot pass, the world pass still binds chunk+0xD8; `NativeTerrainSampler`
  restores every state it touched, also on partial failure; past the bounds in `features/ground_shade_gpu.h` a lot is
  built on the CPU.
- **Rejected:** collection radius, room-0 re-solve, HQ on every lot alone, home-chunk CLAMP sampling ->
  [history](../history/night-lighting-lot-light-pass.md).
- **Fragile:** `LotPassStub` hard-codes the pass's stack layout; `StreetLampColourStub` repeats the lot-id-zero / type
  11 / lit-bit tests of `BakeTest`; CPU and GPU shade must stay identical (only `tools/ground_shade_gpu_test/` checks).

## World lamp response

World street lamps (type 11, lot id 0) that are recoloured, dimmed or switched update the ground and object rigs like
lot lamps. Docs: [feature](../features/night-lighting/world-lamp-response.md), [validation](../validation/night-lighting-world-lamp-response.md), [history](../history/night-lighting-world-lamp-response.md).

- **Files:** `features/world_lamp_policy.h` (`Track`, `Eligible`, `AcceptEdit`), used by
  `features/lot_light_bridge.cpp:ReadLotLamp` and `TrackLotLampEdits`; the patch's
  `patches/night_terrain_relight_patch.cpp:RefreshWorldRigs` and `g_worldRigRefreshPending`.
- **Settings:** none of its own. World-lamp edits are not deferred to dusk (`TerrainLightingPolicy::DeferDayEdit`).
- **Tests:** `tools/world_lamp_test/` (built by `tools/terrain_lighting_test/run.ps1`; also covers `features/night_level.h`).
- **Rejected:** removing only the lot-id-zero exclusion; a six-second window retry -> [history](../history/night-lighting-world-lamp-response.md).
- **Fragile:** `WorldLampPolicy::Eligible` (flags bits 1 and 4) and `IsOutdoorLotLamp` (bits 0x01, 0x40, 0x04) test
  related rules with different bits.

## Room light queue, lamp edits and drags

Rooms of the played lot and story solve first, several per frame, a lamp switch gets a quick class-0 pass, lamp marks
can be filtered, drags hold room solves, and the lot budget shrinks while the camera moves. Docs: [room light queue](../features/performance/room-light-queue.md), [lot lighting while moving](../features/performance/lot-lighting-motion.md), [lamp drags](../features/night-lighting/lamp-drags.md), [validation](../validation/performance-room-light-queue.md), [history](../history/performance-room-light-queue.md), [engine](../engine/room-light-maps.md).

- **Files:** `features/room_light_queue.cpp` (Faster Room Lighting), `patches/performance_patches.cpp`
  (`RoomLightQueuePatch`, `LotLightingMotionPatch`), `features/lot_lighting_motion.cpp`, `features/lamp_mark_filter.cpp`,
  `features/drag_lite.cpp`, `features/room_solve_causes.cpp` (developer measures), `features/restart_settle.h`,
  `features/inner_retry.h`, `features/wall_update_policy.h`; *Lamp edits all at once* is `AtriumHold::SetAllAtOnce` in
  `features/atrium_hold.cpp`.
- **Entry points:** `features/room_light_queue.cpp:PriorityHook` (priority call), `features/room_light_queue.cpp:PickHook`
  -> `Drain` (scheduler tail, render thread only), `features/room_light_queue.cpp:EmptyRemovalHook`;
  `features/lot_lighting_motion.cpp:Hook_LotLightBudget`; `features/lamp_mark_filter.cpp:MarkThunk` -> `MarkDecide`;
  `LampMarkFilter::OnPresent` (patch Present lambda); `DragLite::OnPresent` (from `LevelLightShare::OnPresent`).
  Present hooks "RoomLightQueue", "AtriumHold", "LotLightingMotion" (Normal).
- **Game hooks and addresses:** priority call `features/room_light_queue.cpp@0x006A81DF`; scheduler jump
  `features/room_light_queue.cpp@0x006C5E39`; class step `features/room_light_queue.cpp@0x0069EAA2`; keep class
  `features/room_light_queue.cpp@0x0069EF58` / `@0x0069F1C5`; empty removal `features/room_light_queue.cpp@0x006C7610`;
  room mark `features/lamp_mark_filter.cpp@0x006C7CD6`; lot budget call `features/lot_lighting_motion.h@0x00ADB95D`.
  Each part is byte-checked and written with `MemPatch::WriteCodeSuspended`; a part whose bytes differ stays off.
- **Threads:** priority and pick run on the light tree update's thread, the drain only on the render thread; marks on
  the light tree thread (per-thread deferral); `LampMarkFilter::OnPresent` and `DragLite::OnPresent` on render.
- **State:** `features/lamp_mark_filter.cpp:g_sig`, `g_lotDue` under `g_mx` (light tree writes, render reads);
  `features/lamp_mark_filter.cpp:g_on` (filter on/off, default off); `features/room_light_queue.cpp:g_tree`;
  `DragLite::t_why` (thread-local send reason read by `LevelLightShare::QueueRoom`).
- **Settings:** *Faster room lighting* `[patches.RoomLightQueue] enabled` = true; *Quick update for lamp switches*
  `quickPass` = true; *Lamp edits all at once* `switchAllAtOnce` = true; *Lot lighting time while moving (ms)*
  `[patches.LotLightingMotion] budgetWhileMovingMs` = 3; developer *Rooms keep their light when their lamps did not
  change* `[developer] keep_room_light` = false (`features/developer_settings.h`); drag measures are Tester switches.
- **Tests:** `tools/room_solve_causes_test/` (also `features/restart_settle.h`), run
  `tools/room_solve_causes_test/run.ps1` (needs: none); `tools/terrain_lighting_test/run_shade_lamps_checks.ps1`
  extracts `LampMoving`; `tools/roofed_openings_test/` covers `features/inner_retry.h`. No offline test for the queue,
  the mark filter or the lot budget.
- **Invariants:** the drag throttle keeps the priority call while Faster Room Lighting is off
  (`RoomLightQueue::EnsureDragThrottle`, never blocking inside Present); a lamp edit's rooms are never held by restart
  settle; window lights (types 7, 8) and unreadable entries always mark; the Classic drag hold blocks only picks, never
  the game's list rebuilds; the tool mode's 1000 ms lot budget is untouched.
- **Rejected:** `opening_skip` input hashing; indoor light fades (`lightFade`, key ignored); holding every story until
  the first solves end -> [history](../history/performance-room-light-queue.md).
- **Fragile:** room offsets (+0xF0 state, +0xF4 class, +0x100 shown, +0xEC sub-step) read in
  `features/room_light_queue.cpp` and `features/room_solve_causes.h`; the order of checks inside `PriorityHook`; the
  drain budget mixes AtriumHold, DragLite, LotLightingMotion and LevelLightShare state; lamp edits are detected three
  ways (`features/lamp_mark_filter.cpp:Signature`, `features/lot_light_bridge.cpp:TrackLotLampEdits`, DragLite steps).

## Every-Story Ground Light

Lamps on any story light the ground outside the lot, while the outdoor-room gather keeps the real lot id.
Docs: [level light share](../features/night-lighting/level-light-share.md), [engine](../engine/light-objects-and-rigs.md).

- **Files:** `patches/split_level_ground_light_patch.cpp` (`SplitLevelGroundLightPatch`), gather filter
  `features/level_light_share.cpp:GatherLotId`.
- **Game hooks and addresses:** GetLotID `patches/split_level_ground_light_patch.cpp@0x006BC020` returns 0 (skipped when
  the Sims3SettingsSetter Split-Level fix is on, `S3SSDetect::SplitLevelFixActive`); its gather call
  `patches/split_level_ground_light_patch.cpp@0x006B635D` goes to `GatherLotId`; the bake's call at
  `patches/split_level_ground_light_patch.cpp@0x00C294D0` then sees lot id 0.
- **Settings:** *Upper floors light the ground* `[patches.SplitLevelGroundLight] enabled` = true (row drawn by
  `apex_gui.cpp`, first in the Stories card).
- **Tests:** none.
- **Fragile:** the vanilla bytes hard-code the light's lot id at +0xC0/+0xC4, the offsets the bake stubs read too.

## Night pipeline: game side

One lamp edit at night. Steps 2-7 are the rooms, 8-11 the terrain; both start from the same edit through different
detectors. *Instant* is the default lamp lighting mode; *Classic* keeps game solves for outdoor lamps.

1. The light manager's messages flag the lamp's entries (FUN_006c4cf0; developer counters `FlagThunk0`..`FlagThunk4` in
   `features/lamp_mark_filter.cpp`).
2. Register / unregister / move (FUN_006b64b0, FUN_006b6090, FUN_006b6590) arm cells+0x38/+0x3C through
   `patches/night_terrain_relight_patch.cpp:ArmTestPlaced` / `ArmTestRemoved` / `ArmTestMoved` (outdoor lot lamps too
   with *Lot lamps light the street*), each also calling `LevelLightShare::CrossLotLightChanged`.
3. The per-story room update FUN_006c7250 runs inside `features/level_light_share.cpp:RoomUpdateHook`; the lamp entry
   update FUN_006c7ba0 marks the room changed through `features/lamp_mark_filter.cpp:MarkDecideImpl` (Instant: a lamp the
   GPU draws marks no room except at switch-on). The room-0 cascade is disabled;
   `features/level_light_share.cpp:AfterChangedWalk` sends other floors' room 0 only when their outdoor lamps changed.
4. Apex's own sends go through `features/level_light_share.cpp:QueueRoom` (drag deferral, urgency tiers via
   `MarkUrgent`); the lot safety net is `LampMarkFilter::OnPresent` -> `LevelLightShare::RelightLampSwitch` / `RelightLot`.
5. Gather (light tree thread): the room gather calls at `features/level_light_share.cpp@0x006C5816` and
   `features/level_light_share.cpp@0x006C7094` go through `features/level_light_share.cpp:OutdoorGather`, which adds
   other floors' outdoor lamps and lamps through openings; world lights come in through the call that
   `features/level_light_share.cpp:GatherLotId` filters.
6. Scheduling: `features/room_light_queue.cpp:PickHook` wraps the scheduler; `features/room_light_queue.cpp:PriorityHook`
   applies the quick pass (`QuickPassRoom`), drag holds (`DragLite::HoldUntilRelease`, `DragLite::HoldRepick`), restart
   settle, then `features/room_light_queue.cpp:Prioritise` (lamp-edit urgency `LevelLightShare::LampUrgency`, played lot
   and camera story first).
7. Solve: the lot lighting pass with the budget from `features/lot_lighting_motion.cpp:Hook_LotLightBudget` (boosted 3 s
   after a switch by `LotLightingMotion::Boost`), plus `features/room_light_queue.cpp:Drain` on the render thread; per
   point `features/level_light_share.cpp:LightEvalHook`; street lamps in lot solves through `StreetLampColourStub`; the
   end of a solve reaches `features/level_light_share.cpp:FinalizeHook`. Instant draws the active lot's outdoor lamps on
   the GPU (`features/rt_outdoor_lamps.cpp`).
8. Terrain detection: `LotLightBridge::OnPresent` -> `features/lot_light_bridge.cpp:TrackLotLampEdits` counts edits;
   the next frame the patch's OnPresent calls `NoteEdit`, then `EditReady` (50 ms after the last step, never during a
   Classic drag hold) and `DecideEdit` (day edits wait for dusk, merges with a pending rebuild, compares with the bake
   snapshot via `LotLightBridge::DiffBake`).
9. User edits go to `patches/night_terrain_relight_patch.cpp:TryLocal` -> `ChunkRelight::QueueLocal` (chunks under the
   old and new light rects, at most 16); lamps being dragged are drawn by `TerrainStamps` and skipped
   (`patches/night_terrain_relight_patch.cpp:AllStamped`). Otherwise, after rate and camera limits, `RebuildAll`
   (paced sweep, else `Kick`).
10. Game rebuild: the terrain update consumes the countdown (+0x55 on every chunk) or renders one released chunk (+0x54).
    The bake runs `TerrainLightTest`, `BakePosStub`, `BakeColourStub` and Apex's `BakeConePS`; every chunk render passes
    `ChunkRenderThunk` -> `LightmapSmooth::NoteChunkRendered` and `ChunkRelight::OnChunkRendered`.
11. After it: OnPresent sees the consume (`LightmapSmooth::OnTerrainRebuilt`, `ChunkRelight::OnFullRebuild`, new bake
    snapshot); local batches finish in the ChunkRelight completion block (`LotLightBridge::CoverLots`,
    `LightmapSmooth::ReleaseHold`, failure -> Kick); after a dusk rebuild only, `QueueAllLotOutdoorRooms` re-queues room 0
    of every lot story through `patches/night_terrain_relight_patch.cpp@0x006C7160`.
12. Draw: changed chunks get new smoothed maps and atlas cells (`LightmapSmooth::Get`), and the lot pass keeps the lot
    map (`LotPassStub`).

## Settings

Registered in the `NightTerrainRelightPatch` constructor; labels from its menu cards. "(list)" = shown only in the
individual options list; "(dev)" = registered only in development builds with developer mode.

| Menu label | Key | Default |
|---|---|---|
| Street lamps light lots | luzDoPosteNaGramaDoLote | true |
| Lamps light objects | postesNosObjetos | true |
| Brightness (objects) | forcaNosObjetos | 0.75 |
| Light stairs, railings, columns | lampadasEmTodosObjetos | true |
| Smooth ground light | mapaDeLuzSuavizado | true |
| (dev) Smooth the ground light maps on the GPU | mapaDeLuzSuavizadoNaGpu | true |
| Lamp lighting (Performance page) | lampLightingMode | instant |
| (dev) Check quick lamp edits | outdoorLampDeltaCheck | false |
| (dev) Soft lot edges | bordaSuaveLote | true |
| Sidewalk visibility | calcadaComNevePisada | 0.5 |
| (lamp colour slider) | luzDasLampadasNatural | 1.0 |
| Fences and stairs catch light | cercasComLuzDoChao | true |
| Fence brightness | forcaNasCercas | 0.75 |
| Lamps light walls | paredesComLuz | true |
| Brightness (walls) | forcaNasParedes | 0.30 |
| Realistic lamp reach | alcanceRealistaDasLampadas | true |
| Outdoor light between floors | luzExternaEntreAndares | true |
| Indoor light between floors | luzInternaEntreAndares | true |
| Seamless walls between floors | paredesSemEmendaEntreAndares | true |
| Lamp colors stay near lamps | reflexosNeutrosANoite | true |
| (list) | forcaReflexosNeutros | 1.0 |
| Wall lamps at the right height | paredesExternasNaAlturaDesenhada | true |
| Walls block light on floors | paredesBloqueiamLuzNosPisos | true |
| Light through doors and windows | luzRealistaPorAberturas | true |
| Doors block light | portasBloqueiamLuz | true |
| Indoor light through openings | luzInternaPelasAberturas | true |
| Indoor light adds to the ground | luzInternaSomaNoChao | true |
| Shadow softness | suavidadeDasSombras | 0.15 |
| Outdoor light through openings | luzDeForaPelasAberturas | true |
| Smooth light maps | mapasDosComodosSuaves | true |
| Buildings block lamp light on trees and objects | paredesBloqueiamLuzNosObjetos | true |
| Walls shade lamp light near the ground | paredesSombreiamOChao | true |
| (Light detail card; restart) | detalheDaLuz | 0 (Game) |
| Every floor in full detail | todosOsAndaresEmDetalhe | true |
| Doors and windows stay lit | objetosDeForaComLuzDoChao | true |
| Seamless light on pieces | luzPorPixelNosObjetos | true |
| Seamless light brightness | forcaLuzPorPixelNosObjetos | 0.75 |
| Windows take outdoor light | janelasLuzDeFora | true |
| Lamps light roofs | telhadosComLuz | true |
| Brightness (roofs) | forcaNosTelhados | 0.45 |
| Adjust the background light | comodosEscurosSemLuz | true |
| Brightness (unlit rooms) | luzQueSobraNosComodos | 0.20 |
| Blue tint (unlit rooms) | azulNosComodos | 0.0 |
| Stabilize lamp sparkles on water | waterSpecularFilter | true |
| Keep lamp colors on water | waterPreserveLampColors | true |
| (Water card glow switch) | lagosRefletemLampadas | true |
| Glow brightness | brilhoNaAgua | 0.4 |
| (Water Reflections card) | reflexoNoLago | 1.56 |
| Ground light | luzNoChao | 1.0 |
| (moonlight slider) | luar | 0.0 |
| Smooth indoor light | bordasDosMapasDeLuz | true |
| (list) | corPropriaNoLote | false |
| (list) | corDasLampadasDoLote | 1.0 |
| Update at dusk | automaticoAoAnoitecer | true |
| Lot lamps light the street | luzDoLoteNaGrama | true |
| Delay after dusk | atrasoSegundos | 2.0 |
| (list; reinstall) | postesAcesosNoCalculo | true |
| (list; reinstall) | qualidadeAltaEmTodosOsLotes | false |
| (list; reinstall) | gramaDoLoteUsaLuzDoLote | true |
| (list) | recalcularLotesAoAnoitecer | true |
| (dev) | relightNearbyChunks | false |
| (dev; public builds always on) | relightPacedSweep | true |
