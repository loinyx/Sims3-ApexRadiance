# Code map: Instant lamp lighting and kept parts

This area covers *Lamp lighting: Instant (GPU)*: Quick outdoor lamp edits keep each room-0 solve's per-lamp parts, and
Real-time outdoor lamps relights copies of the room-0 light maps on the GPU from those parts. It also covers the lamp law
shared by the GPU and the room solve. Players and maintainers read
[Real-time outdoor lamps](../features/night-lighting/rt-outdoor-lamps.md) and
[Quick outdoor lamp edits](../features/night-lighting/outdoor-lamp-delta.md); the map format is in
[room light maps](../engine/room-light-maps.md). Back to the [Code map](../code-map.md).

"Light tree thread" is the game thread that `features/level_light_share.cpp:g_gatherThread` records. Kept parts and
deltas only run there.

## Night pipeline: kept parts, GPU relight, compose

1. **Game solve.** The budgeted room solve (FUN_006a3c90) runs state 0, the map lock step
   (`features/outdoor_delta.cpp@0x0069FA40`), per-batch `FUN_006a31d0` (`features/outdoor_delta.cpp@0x01158AC8` is its
   sample vector) and the finalize (`features/outdoor_delta.cpp@0x006A0E00`). Apex wraps them with
   `features/level_light_share.cpp:RoomSolveStartHook`, `features/level_light_share.cpp:LockStepHook`,
   `features/level_light_share.cpp:SolvePointBatch`, `features/level_light_share.cpp:LightEvalHook` and
   `features/level_light_share.cpp:FinalizeHook`.
2. **Capture.** `features/outdoor_delta.cpp:SolveStart` opens a capture for an outdoor room 0 of the active lot, or of a
   neighbour lot (base only). Per sample, `features/outdoor_delta.cpp:Tap` and `features/outdoor_delta.cpp:OnEval` split
   the running sum into each light's part. `features/outdoor_delta.cpp:SampleEnd` stores the texel, its 0x30-byte sample
   and one `Contrib` per light. The packed texel is read back at the next sample (`features/outdoor_delta.cpp:FlushPending`).
3. **Keep.** `features/outdoor_delta.cpp:SolveEnd` validates the capture. A solve whose sum exceeds its parts by more than
   2/255 is not kept. A kept solve is stored as a `Record` in `features/outdoor_delta.cpp:g_records`, keyed by
   `features/outdoor_delta.cpp:KeyOf` (room pointer and class). The whole sum is `TexelRec::t`, the per-lamp parts are
   `LampRec::parts`, and the drawn lamps of pinned rooms may be folded into `drawnSum` (`features/outdoor_delta.cpp:CollapseDrawn`).
4. **Unlock into the room's own maps.** The game unlock `features/rt_outdoor_lamps.cpp@0x0069E950` copies the class's
   working maps (`features/outdoor_delta.cpp@0x011D0460`) into the room's own texture set (`[room+4] + class*64 + 0xC`) via
   `features/rt_outdoor_lamps.cpp@0x006A8FD0`.
5. **Base export.** On a worker, `features/rt_outdoor_lamps.cpp:PrepareJob` calls
   `features/outdoor_delta.cpp:OutdoorDelta::ExportRoom0`: base = sum − drawn sum − parts of GPU-drawn lamps
   (`features/rt_lamp_model.h:Drawn`). The render thread uploads it as texel points (`features/rt_outdoor_lamps.cpp:UploadSlot`).
   `features/outdoor_delta.cpp:OutdoorDelta::RecordVersion` (base version) tells the render thread when to rebuild.
6. **Edit taken by the GPU.** The lamp mark call `features/lamp_mark_filter.cpp@0x006C7CD6` reaches
   `features/lamp_mark_filter.cpp:MarkDecideImpl`. When `features/rt_outdoor_lamps.cpp:RtOutdoorLamps::HandlesLamp` holds
   (every story covered), the mark is dropped and only `features/rt_outdoor_lamps.cpp:RtOutdoorLamps::NoteLampEdit` runs.
7. **GPU relight** (EndScene before the overlay, `features/rt_outdoor_lamps.cpp:Frame`). For each dirty slot, it renders shadows
   per 16-lamp batch (`features/rt_lamp_gpu.cpp:Engine::RenderShadows`), then accumulates into an A32B32G32R32F ping-pong
   (`features/rt_lamp_gpu.cpp:Engine::Accumulate`, `shaders/rt_lamps_hlsl.h:AccPS`). Blending is off
   (`features/rt_lamp_gpu.cpp:PassStates`); each batch reads the previous sum.
8. **Compose** (`features/rt_lamp_gpu.cpp:Engine::ComposeImpl`). `CopyPS` copies the game's own map into `Slot::copy`.
   Then one point per kept texel runs `shaders/rt_lamps_hlsl.h:ComposePS`: `pack(min(keephue(base + acc), 1))` with the
   floor-mask alpha and the dither. COLOR1 holds the outdoor scale. On a class-2 wall atlas the composed texels go to a
   scratch target over `preblur`, then `BlurRowPS`/`BlurColPS`/`BlockPS`. The game texture is never written.
9. **Game shaders sample.** The `SetTexture` callback `RtOutdoorLampsMapSwap`
   (`features/rt_outdoor_lamps.cpp:SwapTexture`, `features/rt_outdoor_lamps.cpp:SwapCopy`) binds the copy wherever the game
   binds its map. The lot pass also reads `features/rt_outdoor_lamps.cpp:RtOutdoorLamps::ScaleOf`. The binding site
   `features/rt_outdoor_lamps.cpp@0x006A7700` showing the room's own set is UNVERIFIED.
10. **CPU fallback branch.** Edits the GPU does not take go to `features/level_light_share.cpp:HoldLampMark`, then
    `features/outdoor_delta.cpp:OutdoorDelta::TakeEdit` and, after the room update, `features/outdoor_delta.cpp:DirectRun`.
    It runs `features/outdoor_delta.cpp:ComputeDelta` with `features/level_light_share.cpp:ReplayBatch`, locks with
    `LevelLightShare::LockRoomMaps`, writes with `features/outdoor_delta.cpp:ApplyDelta`, and finishes with
    `features/level_light_share.cpp:FinishRoomMapsTry`. The same delta can run inside a solve at
    `features/outdoor_delta.cpp:OutdoorDelta::AtLockStep`; returning true jumps the solve to its finalize. Anything refused
    becomes the game's full solve (`LevelLightShare::QueueRoom0`).

## Real-time outdoor lamps

Draws the active lot's outdoor lamps (types 3, 4, 5 and lot 11) with lot-geometry shadows into relit copies of every
story's room-0 maps, so an edit shows in the next frame without a room solve. Docs:
[feature](../features/night-lighting/rt-outdoor-lamps.md),
[validation](../validation/night-lighting-level-light-share.md),
[history](../history/night-lighting-level-light-share.md), [engine](../engine/room-light-maps.md).

- **Files:** `features/rt_outdoor_lamps.cpp` (game reads, slots, jobs, coverage, swap), `features/rt_lamp_gpu.cpp`
  (`RtLampGpu::Engine`, Direct3D 9 passes, no game access), `features/rt_lamp_model.h` (CPU model of a relit texel),
  `features/rt_lamp_proxy.h` (occluder mesh), `shaders/rt_lamps_hlsl.h` (13 shaders).
- **Entry points:** `features/rt_outdoor_lamps.cpp:RtOutdoorLamps::SetEnabled` and
  `features/rt_outdoor_lamps.cpp:RtOutdoorLamps::OnPresent` (NightTerrainRelight Present, Last;
  `patches/night_terrain_relight_patch.cpp`); `features/rt_outdoor_lamps.cpp:OnEndScene` → `Frame` (EndScene before
  overlay); `features/rt_outdoor_lamps.cpp:OnPreReset` (pre-reset); `features/rt_outdoor_lamps.cpp:RtOutdoorLamps::HandlesLamp`,
  `NoteLampEdit`, `NoteSwitchOn` (`features/lamp_mark_filter.cpp:MarkDecideImpl`);
  `features/rt_outdoor_lamps.cpp:RtOutdoorLamps::AfterRoomUpdate` (`features/level_light_share.cpp:AfterRoomUpdateInstant`);
  `features/rt_outdoor_lamps.cpp:RtOutdoorLamps::NoteRoomSolved` (`features/level_light_share.cpp:FinalizeHook`);
  `features/rt_outdoor_lamps.cpp:RtOutdoorLamps::NoteUnlock` (`features/atrium_hold.cpp:UnlockHook`).
- **Game hooks and addresses:** none of its own. It reads the unlock copy `features/rt_outdoor_lamps.cpp@0x0069E950` (room's
  own set, `features/rt_outdoor_lamps.cpp:OwnMap`: wrapper +0x2C == 0, texture at +0x30, vtable checked against a probe
  texture). The law constants LampK1, LampK2, LampCmax, StreetLampScale and ConeScale5 are found by pattern in
  `framework/game_addresses.cpp` and read by `features/rt_outdoor_lamps.cpp:ReadConsts`. Raw room, manager and light
  offsets are read in `features/rt_outdoor_lamps.cpp:ReadRoomNow` and `features/rt_outdoor_lamps.cpp:ReadLamp`.
- **Direct3D 9:** `RenderCallbacks::endSceneBeforeOverlay`, `RenderCallbacks::preReset`, and the SetTexture callback
  `RtOutdoorLampsMapSwap` (Late), all registered on the first frame with the option on. The passes use `CallOriginal*`
  (`features/rt_outdoor_lamps.cpp:GameCalls`), so other Apex hooks never see them.
- **Threads:** render: `Frame`, the swap, reset. Light tree: `HandlesLamp`, `NoteLampEdit`, `AfterRoomUpdate`,
  `features/rt_outdoor_lamps.cpp:BuildProxy` (`features/level_light_share.cpp:LevelLightShare::ExportLotProxy`). Any thread:
  `NoteRoomSolved`, `NoteUnlock`, `CoversRoom`. Workers: `PrepareJob`, the under-floor share job, shader bytecode
  (`features/rt_outdoor_lamps.cpp:CompileAll`).
- **State:** `features/rt_outdoor_lamps.cpp:g_slots` (render only); `features/rt_outdoor_lamps.cpp:g_swap` built by
  `features/rt_outdoor_lamps.cpp:RebuildSwap`; `features/rt_outdoor_lamps.cpp:g_cover` written by `Frame` (render) and read by
  `HandlesLamp`/`CoversRoom` (light tree); `features/rt_outdoor_lamps.cpp:g_proxy` written by `BuildProxy` (light tree);
  `features/rt_outdoor_lamps.cpp:g_sendDue` written by `NoteLampEdit`; `features/rt_outdoor_lamps.cpp:g_watchDirty` written
  by `NoteUnlock` (any); `features/rt_outdoor_lamps.cpp:g_atlasKey` (skips shadow re-render).
- **Settings:** *Lamp lighting > Lamp updates: Instant (GPU)* `[patches.NightTerrainRelight] lampLightingMode` = `"instant"`
  (`patches/night_terrain_relight_patch.cpp:g_lampMode`).
- **Tests:** `tools/rt_lamps_test/` compiles `features/rt_lamp_gpu.cpp` and checks shadows against
  `features/rt_lamp_proxy.h:Visibility`, compose, mask and wall blur bit for bit. Run `tools/rt_lamps_test/run.ps1` (needs:
  GPU; `-Worlds` needs captures). `tools/lighting_restart_test/` checks the restart and fallback slices (needs: none).
- **Invariants:** off means nothing is installed and no texture is swapped (`OnPresent`). The relit texel is
  `pack(min(base + G, 1))` with every term ≥ 0 (`features/rt_lamp_model.h` header). The HLSL mirrors `rt_lamp_model.h`,
  `gpu_lamp_law.h` and `outdoor_delta_model.h` operation for operation (`shaders/rt_lamps_hlsl.h` header). The own map
  may be smaller than the kept working map, never larger, and a blurred atlas must be the exact size (`UploadSlot`).
  Switching off sends every room 0 of the lot to its solve (`Frame`). Edits mark no room only while coverage is full
  and fresh (`HandlesLamp`).
- **Rejected:** compiling the shaders at every start (3.6 s), replaced by the shader cache; counting a doubly listed lamp
  twice on floor maps (patios too bright); drawing unmodelled shaded lamps as bulbs (`features/rt_lamp_model.h:ShadedModelled`);
  uploads and the under-floor share on the render thread during a drag. Reasons are in code comments of `Frame`;
  [history](../history/night-lighting-level-light-share.md).
- **Fragile:** `tools/lighting_restart_test/run.ps1` slices `features/rt_outdoor_lamps.cpp` by literal markers
  (`struct Ask {`, `bool RestartSolveTerminal(`, `bool RestartIncomplete() {`, `void BuildProxy(`), so renames break it.
  The render thread reads the room-0 list (`features/rt_outdoor_lamps.cpp:ReadList`) while
  `features/level_light_share.cpp:ReplayCall` may have swapped it on the light tree thread (impact UNVERIFIED). With more
  than 16 lamps a map, `g_atlasKey` changes per batch and shadows are re-rendered per batch and map.

## Quick outdoor lamp edits (kept parts)

Keeps each room-0 solve's per-light parts so an edit becomes `S − old + new` without a full solve, and supplies the GPU
base. Docs: [feature](../features/night-lighting/outdoor-lamp-delta.md),
[level light share](../features/night-lighting/level-light-share.md).

- **Files:** `features/outdoor_delta.cpp` (capture, records, deltas, direct path, export), `features/outdoor_delta_model.h`
  (shared arithmetic: halves, `MaskAlpha`, `KeepHue`, `PackInput`, `PackModel`, wall blur, `ReachBound2`),
  `features/level_light_share.cpp` (solve hooks, replay, map lock and finish).
- **Entry points:** `features/outdoor_delta.cpp:OutdoorDelta::SetEnabled`, `SetSelfCheck` and `OnPresent`
  (NightTerrainRelight Present); the solve hooks listed in the walk; `features/outdoor_delta.cpp:OutdoorDelta::TakeEdit`
  (`HoldLampMark`); `features/outdoor_delta.cpp:OutdoorDelta::AfterRoomUpdate` (`AfterRoomUpdateInstant`);
  `features/outdoor_delta.cpp:OutdoorDelta::UpToDate` (`features/level_light_share.cpp:QueueRoom`);
  `DirectWriting`/`DeltaSolving` (`features/atrium_hold.cpp:UnlockHook`); `ExportRoom0`, `RecordVersion`, `PinRooms` and
  `SetNeighbourLots` (Real-time outdoor lamps).
- **Game hooks and addresses:** batch vector `features/outdoor_delta.cpp@0x01158AC8`, map slots
  `features/outdoor_delta.cpp@0x011D0460`, texel pack `features/outdoor_delta.cpp@0x0060CD10`
  (`features/outdoor_delta.cpp:PackGame`), all found by pattern in `framework/game_addresses.cpp`. The lock and finalize
  steps (MapLocker, MapEdges, EdgeCopy, MapUnlock, RoomLitNote) are checked by `features/level_light_share.cpp:CheckMapSteps`.
  Raw room reads are in `features/outdoor_delta.cpp:ReadRoom` and `features/outdoor_delta.cpp:ReadDesc` (lock records
  +0x680, +0x1C4, +0x2B4, +0x3A4); light reads in `features/outdoor_delta.cpp:ReadLight` (includes light vfunc +0x24).
- **Threads:** light tree: capture, delta, direct run, replay (`g_cap`, `g_s`, `g_pending`, `g_replay`). Render: `OnPresent`,
  `PinRooms`, `RecordVersion`, `SetNeighbourLots`, world change. Worker: `ExportRoom0` from `PrepareJob`.
- **State:** `features/outdoor_delta.cpp:g_records` under recursive `g_mx` (written on the light tree thread, cleared or
  swept on the render thread, read by workers); `features/outdoor_delta.cpp:g_pinned` (render, read by `SolveEnd`'s budget);
  `features/outdoor_delta.cpp:g_gen` (option generation, `InvalidateAll`); `features/outdoor_delta.cpp:g_deltaRoom` and
  `features/outdoor_delta.cpp:g_directWriting` (read by AtriumHold); `features/outdoor_delta.cpp:g_pending` (taken edits).
- **Settings:** follows *Lamp lighting: Instant*. Developer-only *Check quick lamp edits*
  `[patches.NightTerrainRelight] outdoorLampDeltaCheck` = false (non-public builds;
  `patches/night_terrain_relight_patch.cpp:g_outdoorDeltaCheck`).
- **Tests:** `tools/outdoor_delta_test/` (deltas against full solves of the model; run `tools/outdoor_delta_test/run.ps1`,
  needs: none). `tools/rt_lamps_test/` checks `BlurBlock` and `KeepHue` against the GPU (needs: GPU).
  `tools/memory_owners_test/` stubs `OutdoorDelta::KeptBytes` and `OutdoorDelta::MemoryText`.
- **Invariants:** a solve whose parts do not account for its sum is not kept (`SolveEnd`). Deltas run only on the light
  tree thread (`AtLockStep`, `ReplayBatch`). The render thread never blocks on `g_mx` and uses try-lock
  (`PinRooms`, `RecordVersion`, `SweepLeftLots`). A pinned room never evicts another pinned room; past
  `kMaxPinnedBytes` the new solve is not kept (`SolveEnd`). Kept buffers stay above 2 GB (`WouldLeaveHigh`). Deltas
  are refused past `kMaxChanged` lights or `kMaxEditBatches` batches during an edit.
- **Rejected:** keeping every lot's parts (206 MB), now active lot only (`SolveStart`); cutting each channel at 1, now
  `features/outdoor_delta_model.h:KeepHue`; pinned rooms evicting each other in a loop; long single-step replays during
  an edit; budgets below 2 GB that made the game's saves fail. See the feature page limitations.
- **Fragile:** `features/level_light_share.cpp:ReplayCall` rewrites the game's batch vector and the room's light list for
  the call, restoring them in `__finally`. `features/outdoor_delta.cpp:ReadLight` hashes raw byte ranges of the light, so a
  per-frame game field inside them would mark every light changed (UNVERIFIED that none exists). Its signature differs from
  `features/lamp_mark_filter.cpp:Signature`, which decides "edited" on its own.

## GPU lamp law and timing

`features/gpu_lamp_law.h` is the CPU form of the room solve's per-lamp law (bulb, street lamp, spot, shaded-lamp cones,
reach share, wall mount). `features/gpu_timing_policy.h` samples effect GPU timings every 250 ms in public builds. Docs:
[light objects](../engine/light-objects-and-rigs.md).

- **Entry points:** `features/gpu_lamp_law.h:Eval` (`features/rt_lamp_model.h:Add`, `features/rt_lamp_model.h:AddMask`,
  `features/ground_profile.cpp`); `features/gpu_lamp_law.h:MountedNdl` (the room solve in
  `features/level_light_share.cpp`); `features/gpu_lamp_law.h:Rows` (constants in `features/rt_lamp_gpu.cpp:Engine::Accumulate`);
  cones via `features/rt_outdoor_lamps.cpp:ToLaw`. `features/gpu_timing_policy.h:Due` is used by
  `features/picture.cpp`, `patches/ambient_occlusion_patch.cpp`, `patches/depth_blur_patch.cpp` and
  `patches/edge_smoothing_patch.cpp`, not by Real-time outdoor lamps (`features/rt_outdoor_lamps.cpp:ReadGpuTime` queries
  every relight).
- **Threads / state / settings:** pure functions; reach follows *Realistic lamp reach*.
- **Tests:** `tools/rt_lamps_test/` (law against HLSL, GPU), `tools/render_cost_test/`, `tools/roofed_openings_test/`
  (`Tone`), `tools/gameplay_stability_test/` (`Due`; needs: none).
- **Invariants:** the shader `shaders/rt_lamps_hlsl.h:Law` performs the same operations in the same order as `Eval`.
- **Fragile:** `MountedNdl`'s 0.4 m and 0.5 are literals in `shaders/rt_lamps_hlsl.h` rather than shared with
  `kMountReach`/`kMountFloor`. The indoor helpers (`Tone`, `IndoorTexel`, `IndoorSplit`) have no production caller.
- **Every-room API:** `features/rt_lamp_gpu.cpp:Engine::AccumulateRlg`, `features/rt_lamp_gpu.cpp:Engine::ComposeRlg`,
  `features/rt_lamp_gpu.cpp:Engine::SeedAcc` and `features/rt_lamp_gpu.cpp:Engine::StoreAcc` with shaders `AccRlgPS` and
  `ComposeRlgPS` are only called by `tools/rt_lamps_test/`. Their 3 shaders are still created with the live set
  (`features/rt_outdoor_lamps.cpp:kRtLampShaderIds`), so reordering `RtLampGpu::ShaderId` affects both.

## Duplicated game reads

Several copies read the same game offsets with their own SEH wrappers. A changed offset must be fixed in each copy.

| Read | Copies | Difference |
|---|---|---|
| Room light list +0xC8/+0xCC | `features/outdoor_delta.cpp:ReadList`, `features/rt_outdoor_lamps.cpp:ReadList` (identical); `features/level_light_share.cpp:ReadLightsAndWalls` (+ walls +0x30/+0x34), `features/level_light_share.cpp:ListHead`, `features/level_light_share.cpp:ListSize`, `features/level_light_share.cpp:InList`, `features/level_light_share.cpp:ListHolds` | writers: `features/level_light_share.cpp:DropDisabledLotLights`, `features/level_light_share.cpp:TakeCrossLotLamps`, `features/level_light_share.cpp:ReplayCall` |
| Room and story | `features/outdoor_delta.cpp:ReadRoom` (id, class, step, threshold +0x63C, wall mode +0x639); `features/rt_outdoor_lamps.cpp:ReadRoomNow` (state, shown class, lot base +0xD4, transform +0xE0); `features/rt_outdoor_lamps.cpp:TrackerOfRoom`; `features/level_light_share.cpp:RoomKey`, `features/level_light_share.cpp:ReadMgrLotStory` | same mgr +0x88 story, +0x90/+0x94 lot |
| Tree level +4 / +0x1A0 | `features/outdoor_delta.cpp:ReadTracker`, `features/rt_outdoor_lamps.cpp:TrackerOfTl` | identical |
| Light fields | `features/outdoor_delta.cpp:ReadLight` (hash), `features/outdoor_delta.cpp:DrawnLight`, `features/rt_outdoor_lamps.cpp:ReadLamp`, `features/lamp_mark_filter.cpp:Signature`; terrain bake rule in `features/rt_outdoor_lamps.cpp:InTerrainBake` and `features/level_light_share.cpp:InTerrainBake` | different field sets per purpose |
| Law constants | `features/outdoor_delta.cpp:ReadConsts`, `features/rt_outdoor_lamps.cpp:ReadConsts` | delta falls back per field; Real-time outdoor lamps falls back entirely if one address is missing |
| Map slot index {3,0,1,2} | `features/outdoor_delta.cpp:kSlotIdx` with `features/outdoor_delta.cpp:SlotTexture`; `features/rt_outdoor_lamps.cpp:kSetIdx` with `features/rt_outdoor_lamps.cpp:OwnMap` | working-map slots versus the room's own set |
| Pack and wall blur | `features/outdoor_delta_model.h:PackInput`, `features/outdoor_delta_model.h:BlurBlock` versus `shaders/rt_lamps_hlsl.h:ComposePS`, `shaders/rt_lamps_hlsl.h:BlockPS` | bit-exact, checked by `tools/rt_lamps_test/` |
