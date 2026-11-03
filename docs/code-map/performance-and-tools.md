# Code map: Performance, memory and developer tools

The Performance page's switches (caches, faster codecs, memory handling, lot streaming), the memory observers of the
development builds, and the developer tools: Frame Profiler, Light Probe (F7), Light Diag (F8), Recorder (F6), captures,
developer mode and the Tester tab. Player-facing behaviour is in [Performance](../features/performance.md) and
[Bug reports](../features/bug-reports.md). Back to the [Code map](../code-map.md).

Conventions for the whole area:
- A switch is an `ApexPatch` class in `patches/performance_patches.cpp` registered with `APEX_REGISTER_FEATURE`; its TOML
  table is `[patches.<Name>]` (`enabled` plus its own keys), with `<Name>` from `patches/performance.h` (never renamed).
- `Update()` of each switch (the `Tick` functions below) runs on the pump thread through `PatchManager::UpdateAll`
  (`apex_main.cpp`). Install runs on the init thread at startup and on the render thread when toggled in the menu.
- Hook layers are shared: `framework/slot_chain.cpp` (vtable slots), `framework/entry_chain.cpp` (entry JMP after a
  prologue byte check), `framework/call_chain.cpp` (CALL sites). Steam 1.67.2 addresses, signatures and groups are in
  `framework/game_addresses.cpp` (`GameAddr::GroupAvailable` per `.gameCodeGroup`).
- Build flags: `build_flavor.h:kPublicBuild` (true = developer mode off), `build_flavor.h:kDevToolsBuild` (false in the
  players' build), `build_flavor.h:kMemoryOptimizationsEnabled`; `features/dev_switches.h:kCompiled` only in numbered
  development builds.

## Performance switches

Menu rows are drawn by `apex_gui.cpp:PerformanceCard` (`FeatureSwitchRow`); developer A/B switches are
`features/perf_toggles.h` (15 atomics, all default on). Docs: [feature](../features/performance.md),
[validation](../validation/performance.md), [history](../history/performance.md).

| Menu label | TOML `[patches.X]` keys | Default | Game address (Steam) | Start function | Harness |
|---|---|---|---|---|---|
| Faster game file lookups | ResourceLookupCache | on | FindProvider 0x004AFFC0 (vtable slots) | `features/resource_cache.cpp:Start` | none (in-game verify) |
| Remember missing files | ResourceLookupMisses | on | same + DPF write 0x004A7FC0 | `features/resource_cache.cpp:StartMisses` | none |
| Faster file lists | FileListCache | on | GetKeyList 0x004B1AE0 / 0x00736660 (slots) | `features/resource_cache.cpp:StartKeyLists` | none |
| Faster object lookups | ObjectLookupIndex | on | 0x00C62D40; maps 0x00939100, 0x006FDFB0 | `features/object_index.cpp:Start` (+ `object_id_map.cpp`, `render_object_map.cpp`) | `tools/gameplay_stability_test/` |
| Spread new objects over frames | SceneNodeBudget | on | CALL 0x006EBC49 -> 0x006E4130 | `features/scene_budget.cpp:Start` | `tools/scene_budget_test/` |
| Faster texture compression | FastTextureCompression: useSeveralCores = true | on | DXT1 0x006152F0, DXT5 0x006154B0 | `features/fast_dxt.cpp:Start` | `tools/dxt_test/` |
| Faster cache compression | FastCacheCompression | on | RefPack 0x004EC200 (slot), CRC 0x004FA4C0 | `features/fast_refpack.cpp:Start`, `features/fast_crc.cpp:Start` | `tools/refpack_test/`, `tools/memory_budget_test/` |
| Faster Sim building | FastCasSort | on | 0x005D1960 | `features/fast_cas.cpp:Start` | `tools/cas_sort_test/` |
| Faster memory handling | FastMemory | on | allocator 0x011CB864, VirtualFree call 0x004E5306 | `features/fast_memory.cpp:Start` | `tools/memory_budget_test/` |
| Room to save | MemoryGuard | on | CALL 0x007377F7, CALL 0x00AAC320 | `features/memory_guard.cpp:Start` | `tools/memory_budget_test/` |
| Lighter window updates | WindowRepaint | on | JNE 0x004108AE | `features/window_repaint.cpp:Start` | none |
| Faster scripts | ScriptMath | on (Steam, EA) | Mono handlers 0x00E54D1B..0x00E58304, 0x00EA8A00, 0x00E75340 | `features/script_math.cpp:Start` | `tools/gameplay_stability_test/` |
| Faster room lighting | RoomLightQueue: quickPass = true, switchAllAtOnce = true | on | see lighting area | `features/room_light_queue.cpp:Start` | lighting area |
| Spread lot lighting while moving | LotLightingMotion: budgetWhileMovingMs = 3 (1..15) | on | CALL 0x00ADB95D | `features/lot_lighting_motion.cpp:Start` | lighting area |
| Wall shading waits while moving | WallShadingWhileMoving | on | 0x0068B810 (slot) | `features/lot_lighting_motion.cpp:StartWallAo` | lighting area |
| Extended lot detail | LotDetailRange: distance = 300 (70..300), maxActiveLots = 16 (8..16) | off | WorldManager+0xDC, +0xE4 | `features/lot_detail_range.cpp:Start` | none |
| Smooth lot streaming | LotLodStreaming | off | test 0x00C6C695, flag 0x011ECBC0 | `features/lot_lod_streaming.cpp:Start` | `tools/lot_streaming_test/` (policy) |
| Pause lot streaming in map view | MapViewStreamingBlocker | off | WorldManager+0x258 | `features/lot_lod_streaming.cpp:StartMapViewBlocker` | none |
| Spread lot objects while loading | LotObjectThrottle: objectsPerLot = 2 (1..64), delayMs = 16 (0..500) | off | AddLotObjectsToScene 0x00AC1130 | `features/lot_object_throttle.cpp:Start` | none |
| Keep lot visibility stable | LotVisibilityOverride | off | JZ 0x00C63015 (+8) | `features/lot_visibility_override.cpp:Start` | none |
| (no row, EA only) | LotActiveThreshold | off | live setting by name | `features/lot_active_threshold.cpp:Start` | none |

- **Invariants:** a saved off is respected, a missing value takes `.enabledByDefault`; lot streaming switches step aside
  when Sims3SettingsSetter is loaded and owns the setting ("Handled by Sims3SettingsSetter").
- **Fragile:** defaults live in three places: `.enabledByDefault`, `Register*Setting` defaults, and
  `features/lot_detail_range.h:kDefaultDistance` / `features/lot_object_throttle.h:kDefaultObjectsPerWindow`; the menu
  slider ranges are written again in `apex_gui.cpp:PerformanceCard`. Only WindowRepaint and ScriptMath use
  `patches/performance_patches.cpp:SimpleSwitchPatch`; the other classes repeat the same Install/Uninstall body.

## Resource and file caches

Answers FindProvider lookups, missing-file lookups and per-package key lists from memory. Docs:
[lookup cache](../features/performance/resource-lookup-cache.md), [missing files](../features/performance/remember-missing-files.md),
[file lists](../features/performance/file-list-cache.md), [history](../history/performance-resource-lookup-cache.md).

- **Files:** `features/resource_cache.cpp` (all three; its header comment documents every game function and class).
- **Entry points:** `features/resource_cache.cpp:Hook_FindProvider` (FindProvider slots, every thread);
  `Hook_RegisterDb`, `Hook_RegisterDbDerived`, `Hook_SetDbPriority`, `Hook_DbChanged` (list watchers, `AcquireWatchers`);
  `Hook_KeyListBase` / `Hook_KeyListDerived` -> `EmulateKeyListOrGame` -> `EmulateKeyList`; write epochs through
  `InstallClassHooks` and `features/resource_cache.cpp:Hook_DpfWriteDirect`.
- **Game hooks and addresses:** `features/resource_cache.cpp@0x004AFFC0` FindProvider, RegisterDatabase 0x004B2D00 /
  0x00736A70, SetDatabasePriority 0x004B2EC0, DatabaseChanged 0x004B0960, read-only package vtable
  `features/resource_cache.cpp@0x00FFE078`, GetKeyList `features/resource_cache.cpp@0x004B1AE0` (byte patterns
  `kKeyListStart`, `kKeyListLoop`, `kRoKeyList`); package list at mgr+0x30 / mgr+0x34 (8-byte {db, priority}).
- **Threads:** any game thread that resolves resources (render, async loader, directory watcher); Start/Stop under `g_ctrl`.
- **State:** `features/resource_cache.cpp:g_table` under `g_lock` (SRW); `g_gen` / `g_mutating` moved by
  `BeginChange` / `EndChange` around every list change; `g_epoch`, `g_writeSeq`, `g_writeBusy` (write epochs, read by
  `StableEpoch`); `g_selfDisabled`, `g_klSelfDisabled` (set on a verification mismatch).
- **Settings:** see the table; `[developer.controls.resource_cache] verify_every` = 64 in developer mode, 0 otherwise;
  Tester `perf_key_list_counted_packages` = on (`PerfToggles::KeyListCountedPackages`).
- **Tests:** none offline; developer mode checks 1 answer in `verify_every` against the game (`Verify`, `KlMismatch`)
  and turns the cache off for the session on a difference.
- **Invariants:** watchers go in before the lookup hook (`Start`); epoch sums are trusted only 10 s after the hooks
  (`EpochsUsable`); a C++ exception inside the key-list emulation truncates the vector and calls the game
  (`EmulateKeyListOrGame`).
- **Rejected:** detouring FindProvider's entry (the game-address self-check would see changed bytes) ->
  [history](../history/performance-resource-lookup-cache.md).
- **Fragile:** the package list mgr+0x30/0x34 is read in five places with different checks:
  `features/resource_cache.cpp:ReadList` (SEH, rejects a null begin, cap 8192 entries; used by `BuildSnapshot`,
  `Fingerprint`, `FindInList`, `ReadOnlyAboveReliable`, `Remember`), `features/resource_cache.cpp:ReadListRaw` (SEH,
  allows an empty list, no cap; used by the key-list hooks, after which `EmulateKeyList` walks every package without
  SEH), the inline readers `ListEntryIs` / `ListIs` (used by `Recheck`), and `features/frame_profiler.cpp:PackagesProbed`
  (cap 65536). A fix to one reader does not reach the others.
- **Fragile:** the list fingerprint is computed twice (`BuildSnapshot`, `Fingerprint`); if they differ,
  `MaybeFingerprint` reports an unhooked change every `kFingerprintEvery` hits and the cache churns.
- **Fragile:** the per-class write-hook spec table hand-counts arguments per vtable slot; a wrong count corrupts the
  caller's stack (the comment above the table records one such crash).

## Codecs and workers (DXT, RefPack, CAS)

Byte-identical replacements of the game's CPU DXT1/DXT5 encoder, RefPack compressor, record CRC-32 and CAS triangle
sort, optionally over worker threads. Docs: [textures](../features/performance/fast-texture-compression.md),
[cache](../features/performance/fast-cache-compression.md), [CAS](../features/performance/fast-cas-sort.md).

- **Files:** `features/fast_dxt.cpp` (hook, checks, cores policy), `features/dxt_codec.cpp` (pure codec, `Parallel`
  pool), `features/fast_refpack.cpp` (slot hook, per-thread buffers, `ParPool`), `features/refpack_codec.cpp` (pure
  compressor), `features/fast_crc.cpp`, `features/fast_cas.cpp` (hook, pool), `features/cas_tri_sort.cpp` (pure sort).
- **Entry points:** `features/fast_dxt.cpp:Hook_Dxt1` / `Hook_Dxt5` (EntryChain), `features/fast_refpack.cpp:Hook_StreamWrite`
  (SlotChain), `features/fast_crc.cpp:Hook_Crc`, `features/fast_cas.cpp:Hook_TriSort`; `features/fast_refpack.cpp:Tick`
  (pump: removes the slot layer after a grace time); `features/fast_refpack.cpp:PrepareForSave` (Room to save).
- **Threads:** DXT is called from many game threads; workers copy the caller's x87 control word and MXCSR; pools are
  never freed (their threads sleep); worker stacks are 256 KB reservations.
- **State:** `g_on` / `g_selfDisabled` per file; `features/fast_dxt.cpp:g_severalCores`, `g_parSelfOff`;
  `features/fast_refpack.cpp:t_held` (per-thread buffers, caps `kKeepHeld` 8 MB and `kKeepHeldTotal` 12 MB, retired
  after 120 s idle by `RetireIdleStorage`).
- **Settings:** *Use several cores* `[patches.FastTextureCompression] useSeveralCores` = true; developer controls
  `[developer.controls.fast_dxt] verify_every / workers / min_side`, `fast_refpack verify_every / compare_every / chain_depth`
  (8 / 0 / 32), `fast_cas verify_every` (16).
- **Tests:** `tools/dxt_test/`, `tools/refpack_test/`, `tools/cas_sort_test/`, all run by
  `tools/development/check.ps1` (`-Suite Smoke` or `Full`; needs: none, MSVC x86).
- **Invariants:** the first 16 calls of every session are compared with the game in every build (`kStartupChecks`), and
  any difference turns the feature off for the session; a worker with a different FP state sends the session back to one
  core (`features/fast_dxt.cpp:RunFast`).
- **Rejected:** DXT sub-image split -> [history](../history/performance-fast-texture-compression.md); deferred
  compositor readback -> [history](../history/performance-fast-cas-sort.md).
- **Fragile:** three hand-written pools with the same FP hand-off: `features/fast_cas.cpp:ThePool`,
  `features/dxt_codec.cpp:ThePool`, `features/fast_refpack.cpp:ThePool`.

## Memory

Less lock waiting in the game allocator, a 128 MB address-space reserve and idle-cache shrink for world saves (Error 12),
plus development-only observers. Docs: [fast memory](../features/performance/fast-memory.md),
[room to save](../features/performance/room-to-save.md), [memory analysis](../features/memory-analysis.md),
[memory budget](../features/memory-budget.md), [engine](../engine/native-allocation-and-script-memory.md),
[save result](../engine/world-save-result.md).

- **Files:** `features/fast_memory.cpp` (spin count, release queue), `features/memory_guard.cpp` (Room to save),
  `features/memory_policy.h` (pure pressure and trim policy), `features/memory_telemetry.h` (event ring, `SaveScope`),
  `features/allocator_tuning.cpp` (dev builds), `features/apex_memory.cpp` (Apex's own heap by call site; global
  `operator new` only in development builds), `features/address_space.cpp`, `features/va_ledger.cpp`,
  `features/game_alloc_categories.cpp` (dev observers writing CSVs to the Apex folder).
- **Entry points:** `features/fast_memory.cpp:Stub_Free` / `Stub_Alloc` (allocator call sites), `WorkerProc`;
  `features/memory_guard.cpp:UpdateHook` (ResourceSystem update CALL), `WorldSaveHook`, `SaveDispatchHook` and
  `SaveStageHook` (early save, Steam timestamp only), `Watch` (own thread, every 4 s); `AllocatorTuning::Install` and
  `Pump`, `AddressSpace::Start`, `VaLedger::InstallEarly`, `GameAllocCategories::Install` (all from `apex_main.cpp`).
- **Game hooks and addresses:** `features/memory_guard.cpp@0x007377F7` -> 0x00737560 (update),
  `features/memory_guard.cpp@0x00733E70` (shrink both caches), `features/memory_guard.cpp@0x00AAC320` -> 0x00C6D460
  (world save), early save `features/memory_guard.cpp@0x00AB12AF`, cache trim `features/memory_guard.cpp@0x00732E90`;
  allocator `features/allocator_tuning.cpp@0x011CB864`, trim `features/allocator_tuning.cpp@0x004E4F90`; game heap
  `features/game_alloc_categories.cpp@0x004E3F90`. ResourceSystem+0x1E0 / +0x1E4 caches, cache+0x98 idle bytes (comment:
  unverified reading).
- **Threads:** FastMemory worker; MemoryGuard `Watch` thread; cache shrinks run only on the ResourceSystem update thread
  (`g_rsThread`); AddressSpace thread every 10 s; va_ledger hooks on every thread.
- **State:** `features/memory_guard.cpp:g_reserve`, `g_rs` / `g_rsThread` (set by `UpdateHook`, read by
  `WorldSaveHook`), `g_shrinkWanted` / `g_autoTrimWanted` (Watch -> update thread);
  `MemoryTelemetry::saveDepth` (no trim while saving); `features/fast_memory.cpp:g_queued` under `g_qLock`.
- **Settings:** see the table; `[dev_build] allocator_trim`, `allocator_top_down`, `measurements`, `memory_owners`,
  `release_debug_buffers`, `left_lot_release` = true each (`features/dev_switches.cpp:Load`).
- **Tests:** `tools/memory_budget_test/` (`run.ps1 [-Standard]`, needs none), `tools/memory_owners_test/` (needs a D3D9
  device; `-Dxvk <d3d9.dll>` optional), `tools/memory_analysis_test/`, `tools/game_alloc_categories_test/`; these
  `#include` the production `.cpp` files.
- **Rejected:** private memory release -> [history](../history/performance-fast-memory.md); idle files dropped early ->
  [history](../history/performance-room-to-save.md).
- **Fragile:** the allocator global 0x011CB864 is in `framework/game_addresses.cpp@0x011CB864`,
  `features/allocator_tuning.cpp:kAllocatorGlobal` and `features/address_space.cpp:kSteamGlobals`; the exe timestamp
  0x52DEC247 gates code in both `features/memory_guard.cpp:Start` and `features/address_space.cpp:Take`.
- **Fragile:** early-save and cache-trim addresses are hard-coded, not GameAddr; ResourceSystem+0x1E0 and cache+0x98 are
  read separately in `TrimIdleStepRaw` and `IdleBytes`; renaming internal functions can break the tests that
  `#include` these files.

## Lot streaming and LOD

Optional lot-detail tuning (distance, detailed-lot count, native throttle, visibility hysteresis, object windows, map
view pause) and the shared lot build time used by lot lighting while moving. Docs:
[feature](../features/performance/lot-streaming.md), [engine](../engine/lot-loading-and-streaming.md),
[map view](../engine/camera-and-map-view.md), [history](../history/performance-lot-streaming.md).

- **Files:** `features/lot_detail_range.cpp`, `features/lot_lod_streaming.cpp` (throttle, camera threshold, active
  threshold follow, map view blocker), `features/lot_visibility_override.cpp`, `features/lot_object_throttle.cpp`,
  `features/lot_active_threshold.cpp` (EA only), `features/lot_build_share.cpp` + `features/lot_build_share_policy.h`,
  `features/map_view.cpp`.
- **Entry points:** each switch's `Tick` (pump); `features/lot_object_throttle.cpp:Hook_AddLotObjectsToScene`
  (EntryChain); `features/lot_build_share.cpp:Hook` and `PassHook` (CallChain, render thread), started from
  `features/lot_lighting_motion.cpp`; `MapView::IsOpen` (also used by AO and depth blur).
- **Game hooks and addresses:** WorldManager pointer 0x011ECBC4 with fields +0xDC distance, +0xE4 max active lots,
  +0xEC camera threshold, +0x258 skip streaming; throttle test 0x00C6C695 / flag 0x011ECBC0; active threshold
  0x00C6C6B7 / 0x01184328; load stages CALL 0x00AEB306; map view getter `features/map_view.cpp@0x0073E060`.
- **State:** `features/lot_lod_streaming.cpp:g_activeWritten`, `g_activeAbandoned`; `features/lot_detail_range.cpp:g_targetMaxActive`;
  `features/lot_build_share.cpp:t_slice` (per thread).
- **Settings:** see the table; policy constants `LotBuildSharePolicy::kStillMs` 6, `kMovingMs` 4, `kMaxSkips` 6;
  `[dev_build] lot_build_share`.
- **Tests:** `tools/lot_streaming_test/` (`run.ps1 [-CsvDir <LotLoadAnalysis CSVs>] [-Exe <TS3W.exe>]`; needs none,
  captures and the exe optional); `tools/lot_load_analysis_test/`.
- **Invariants:** every write is compare-and-write (`WriteExpected`); a field changed by another owner is no longer
  maintained; Sims3SettingsSetter wins only when its module is loaded.
- **Rejected:** objects spread by default (lamps arriving one by one relit rooms for 7 s; comment on the
  LotObjectThrottle registration).
- **Fragile:** `S3SSOwns*` is copied five times (`features/lot_lod_streaming.cpp:S3SSOwnsStreamingSettings`,
  `S3SSOwnsMapViewBlocker`, `features/lot_active_threshold.cpp:S3SSOwnsStreamingSettings`,
  `features/lot_visibility_override.cpp:S3SSOwnsVisibilityOverride`, `features/lot_object_throttle.cpp:S3SSOwnsObjectThrottle`);
  `Read` / `WriteExpected` are copied in `lot_lod_streaming.cpp` and `lot_detail_range.cpp`.
- **Fragile:** WorldManager+0xE4 is written by `features/lot_detail_range.cpp:ApplyMaxActive` and read by
  `features/lot_lod_streaming.cpp:MaintainActiveThreshold` (one pump tick later); the active threshold has two writers
  (`MaintainActiveThreshold` and `features/lot_active_threshold.cpp:ResolveLiveSettingAddress`'s target on EA).

## Frame Profiler

Developer tool (Developer > Performance, "Frame times and stutters") that times frames, game functions, services, jobs,
waits, I/O and D3D hooks and writes "ApexRadiance_Hitches.txt". Docs: [feature](../features/frame-profiler.md),
[validation](../validation/frame-profiler.md), [history](../history/frame-profiler.md),
[engine](../engine/main-loop-and-services.md).

- **Files:** `features/frame_profiler.cpp` (its header comment lists every timed function and the attribution model; a
  stub API at the end for builds without dev tools), `features/lot_load_analysis.cpp` (CSV fed by the profiler).
- **Entry points:** `FrameProfiler::SetEnabled` (menu), `features/frame_profiler.cpp:RegisterD3DHooks` ->
  `OnPresentStart` -> `FrameBoundary`; `AttachTarget` / `DetachTarget` on the render thread; `BeginModTime` /
  `EndModTime` from `framework/d3d9_hooks.cpp`; `SaveReport`.
- **D3D9:** "FrameProfiler" Present at -1000 and +1000 (`kPrioStart`, `kPrioEnd`), draws at -1000, only while on.
- **Threads:** render (boundary, attach); `SamplerProc` (optional sampling); `WriterThreadProc` (file, once a second).
- **Settings:** `[qol.frame_profiler]` (written only in developer mode; `enabled` forced off at load): hitch_multiplier
  2.0, hitch_floor_ms 8.0, count_state_calls true, write_file true, sample_render / sample_simulation false, sample_hz
  2000, time_mutex_lock false, time_objects false, registry_timing false (`FrameProfiler::LoadFromToml`).
- **Tests:** none offline for the profiler.
- **Invariants:** the profiler is the outer layer of every shared site; hooks attach only after the game-address scan;
  nothing is written on the render thread; Mutex::Lock timing stays off by default.
- **Fragile:** some sites are not hooked on purpose because other mods check their bytes (header comment "Not hooked on
  purpose"); the stub section must keep every public signature of `features/frame_profiler.h`.

## Light Probe (F7) and Light Diag (F8)

Read-mostly lighting diagnostics for bug reports, in both builds. Docs: [light probe](../features/dev-tools/light-probe.md),
[light diag](../features/dev-tools/light-diag.md), [history](../history/dev-tools-light-probe.md),
[engine](../engine/room-light-maps.md).

- **Files:** `features/light_probe.cpp`, `features/light_diag.cpp`, `features/daylight_probe.cpp` (dev-only daylight
  section), `hotkeys.cpp` (`kKeys`).
- **Entry points:** `LightProbe::OnPresent` and `ScreenWatch::OnPresent` from the NightTerrainRelight Present hook;
  `LightDiag::OnPresent` and `Recorder::OnPresent` from Night Lighting's `OnPresent`
  (`patches/night_terrain_relight_patch.cpp`); `LightDiag::RequestDump` from `apex_gui.cpp:ReportCaptureCard`. F6, F7
  and F8 therefore work only while Night Lighting is installed.
- **Game hooks and addresses:** light root getter 0x006E97B0 and light enumerator 0x006ACF70 (byte checked in
  `LightDiag::Init`); light tree walk lightMgr+0xD4 -> tracker+0x6A0+level*0x1A4 -> room hash mgr+0x234 (SEH reads `Rd`).
- **D3D9:** "LightProbe" draw hooks at Priority::Last, registered only while a capture is armed (`RegisterHooks`).
- **Invariants:** draw records are reserved before recording so a draw never throws; F8 only reads.
- **Rejected:** shared "latest" file, capture pruning and automatic re-captures after floor changes ->
  [history](../history/dev-tools-light-probe.md).
- **Fragile:** the light tree walk is written twice in `features/light_diag.cpp` (`PixelLampDiag`, `WriteDiag`) and its
  offsets recur in `features/level_light_share.cpp`, `features/lot_light_bridge.cpp`; the sky offsets are in both
  `features/light_diag.cpp:DayNightText` and `features/daylight_probe.cpp`. Dead code: `LightProbe::Aim` has no caller and
  the automatic-capture `g_watch` path is never armed.

### After a capture

Hotkeys (F-key preset, the default; letters V/B/X and numbers 4/5/6 with Ctrl+Shift; overrides `[ui] probe_key`,
`diagnostics_key`, `recorder_key`): F7 light capture, F8 lighting snapshot, F6 recording (again to stop, 20 s max).
Every capture folder is `Captures\<date time> <kind>\` under the Apex folder in Documents (inside the open session
folder if one exists) and also holds "ApexRadiance_LOG.txt", `ApexRadiance.toml`, `About this capture.txt`, optional
`User notes.txt` and `Screenshot.png`.

| Question | File | Section to read |
|---|---|---|
| Which draws paint this pixel, with which shader and pass? | F7 `Light capture.txt` | `== DESENHO #n` blocks: `mod:` line (Apex pass), `estados:`, `s<n>:` textures |
| What constants did that draw get? | F7 `Light capture.txt` | `PS c0..c223` / `VS c0..c255` lines of the block |
| Final colour on screen / change since last capture | F7 `Light capture.txt` | `== COR NA TELA NO PIXEL`, `== COMPARACAO COM A CAPTURA ANTERIOR` |
| Which wall is under the pixel | F7 `Light capture.txt` | `== PAREDE`, `== PAREDES DA CENA` |
| What do the textures look like | F7 folder | `T<n>.png`, `T<n>_alpha.png`; shader text `PS_<ptr>.txt` / `VS_<ptr>.txt` |
| Is a lamp on, enabled, where, how strong | F8 `Lighting snapshot.txt` | `==== TODAS AS LUZES`: flags `ACESA` / `habilitada`, pos, raio, cor |
| Room solve state, class, ambient | F8 `Lighting snapshot.txt` | `==== LOTES`: `-- LOTE` story lines, then one line per room (state, class) and its `ambient now:` line |
| Day / night level disagreement | F8 `Lighting snapshot.txt` | header `day and night:` and `day and night WARNING:` |
| Per-pixel lamp constants and outdoor light lists | F8 `Lighting snapshot.txt` | `==== LUZ POR PIXEL` |
| What happened over time after a lamp edit | F6 "Recording.txt" | "Light updates" summary at the top, then time-sorted log and notes; `Screen pixels.csv`, `Ground timeline.csv` |

## Recorder, captures and Report a problem

Up to 20 s of lighting activity, capture folder management and the Report a problem page. Docs:
[feature](../features/bug-reports.md), [recorder](../features/dev-tools/recorder.md), [validation](../validation/bug-reports.md).

- **Files:** `features/recorder.cpp`, `features/screen_watch.cpp` (screen pixel grid), `features/captures.cpp`
  (`Finish`, `NewFolder`, `WriteText`, `WritePngAsync`, `Prune`, sessions), `apex_gui.cpp:ReportPage`.
- **Entry points:** `Recorder::RequestToggle` (Report page), `Recorder::Note` / `NoteLampEdit` (lighting modules),
  `Captures::SaveReport`.
- **D3D9:** "CapturesScreenshot" Present at Priority::First, registered once on the first screenshot.
- **Threads:** render (recorder, screenshots); PNG worker `features/captures.cpp:DumpWorker`; pruning in background.
- **Settings:** `[ui] capture_screenshot` = true, `capture_max_mb` = 2048, `capture_max_folders` = 30.
- **Tests:** `tools/report_check/` (storage, recorder request/cancel/deadline, Report page UI; manual `cl` build per its
  README; needs the x86 vcpkg ImGui).
- **Invariants:** folders are never reused; the open session and folders still being written are never pruned; cancel
  wins over the shortcut, Stop and the deadline and writes nothing.
- **Fragile:** `tools/report_check/extract_menu.py` cuts code out of `apex_gui.cpp` and `features/recorder.cpp` by
  text; renaming the Report functions breaks it. The screenshot hook registration is written twice
  (`QueueShot`, `QueuePlayerPhoto`).

## Developer mode and Tester tab

Developer mode turns on the Developer page and tools (developer build only); the Tester tab holds runtime A/B switches;
numbered development builds read `[dev_build]`. Docs: [feature](../features/developer-mode.md),
[tester](../features/dev-tools/tester.md), [history](../history/developer-mode.md).

- **Files:** `build_flavor.h`, `apex_config.cpp` (`LoadDeveloperMode`, `ApplyDeveloperPreferences`),
  `features/developer_settings.h` (`[developer.controls]` tables), `features/tester_toggles.cpp` (`kItems`),
  `features/tester_toggles.h`, `features/dev_switches.cpp`, `apex_gui.cpp:DevTesterTab`, `apex_gui.cpp:DeveloperModeRow`.
- **Settings:** *Enable developer mode* `[ui] developer_mode` = false (restart needed); Tester switches
  `[developer.controls.tester] <key>` = true each; `[dev_build] <switch>` = true each except `restart_settle`.
- **Invariants:** developer preferences never record measurements or one-shot actions; importing never starts the
  Frame Profiler; the players' build never enables developer mode.
- **Fragile:** `kPublicBuild` is computed in both `apex_config.cpp:LoadDeveloperMode` and `LoadSettings`; Tester keys in
  `kItems` and the rows in `DevTesterTab` are separate lists; `DevSwitches::kCompiled` and `kDevToolsBuild` are
  independent flags.
