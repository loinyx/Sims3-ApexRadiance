# Lot loading and streaming

This page documents how `TS3W.exe` streams lots around the camera: LOD scoring, the object build, the LotRenderer load stages and lot room lighting, the lot impostor builder, the terrain and world update that runs in the same pass, the world cache and the resource I/O underneath. The loaded-world gate, Lot Lighting While Moving, Faster Game File Lookups, Night Lighting's terrain relight and the Frame Profiler depend on it.

## Scope

| | |
|---|---|
| Game build | Steam 1.67.2 (`TS3W.exe`, image base 0x00400000) unless stated |
| Used by | [Architecture: loaded-world gate](../architecture.md#12-loaded-world-gate-and-start-note) (`features/world_session.h`), [Depth Blur](../features/depth-blur.md), [Performance](../features/performance/README.md), [Terrain relight](../features/night-lighting/terrain-relight.md), [Frame Profiler](../features/frame-profiler.md) |
| Evidence | Disassembly (`engine_map`), Ghidra `re/out/fn_00aea680.c`, combined-build patch headers, `features/world_session.h`; items marked *(inferred)* are static only |

## Overview

This page covers how TS3W.exe (1.67.2 Steam) streams lots around the camera:
- LOD scoring and promotion to "detailed view";
- the object build;
- the LotRenderer load-stage state machine and lot room lighting;
- the lot impostor builder;
- the terrain and world update that runs in the same pass;
- the world cache;
- the resource system I/O underneath all of it.

It also records exactly what S3SS's **Lot Streaming Optimizations** (LSO) patch touches, because the combined build's
Smooth Streaming was designed to sit next to it. Smooth Streaming was removed from the standalone on 2026-09-28; see
[../removed-features.md](../removed-features.md#smooth-streaming).

Addresses are image base 0x00400000, no ASLR. Anything marked *(inferred)* was read from static disassembly and not
confirmed at runtime.

## Details

### Sources

Sources:
- `patches/smooth_streaming_patch.cpp` (header notes 1-5);
- `patches/lot_streaming_optimizations_patch.cpp` (S3SS);
- `frame_profiler.cpp` (header, target table);
- engine_map (`f_C6D570.asm`, `full.asm`, `strings.tsv`, `calls.tsv`, `dwords.txt`, `profiler_targets.tsv`);
- Ghidra `re/out/fn_00aea680.c`;
- NOTAS-ILUMINACAO.md "Desempenho: mapa do motor (28/09)".

### Data structures

#### WorldManager ([0x011ECBC4])

| Offset | Meaning | Evidence |
|---|---|---|
| +0x41 | World active: the WorldManager service (lot pass) runs only while it is set | 0xC7E3D9 |
| +0x43 | Flag passed to the terrain heightmap update 0xC2A290 | `f_C6D570.asm` |
| +0x58 | Terrain (`mov ecx,[esi+58h]; call 0xC845C0` at 0xC6D68C) | Smooth Streaming |
| +0x5C | Road network (update 0x00B890C0) | Smooth Streaming comment, `f_C6D570.asm` |
| +0x60 | Sea/water (0x00C0DD60) | `profiler_targets.tsv` |
| +0x64 | Sky/lighting (0x00C15D80) | `profiler_targets.tsv` |
| +0xDA | Forces the terrain "low camera" flag | `f_C6D570.asm` 0xC6D66F |
| +0xDC | Live setting "Lot LOD dist.", float, default 70.0 | ctor 0xC672A4; registered at 0xC676E2 |
| +0xE0 | Live setting "Active Lot Bias", float, default 8.0. Subtracted from the lot metric of lots already in detailed view (0xC6301D) | ctor 0xC672B4; 0xC67718 |
| +0xE4 | Live setting "Max Active Lots", int, default 4, range 0..12 | ctor 0xC672F6; 0xC6763C |
| +0xE8 | Live setting "Terrain Height Thresh", float, default 600.0. The camera eye y is compared with it (0xC6D650) | ctor 0xC67300; 0xC676AC |
| +0xEC | Live setting "Camera speed threshold", float, 0.0 at construction, UI range 0..100. LSO writes 5.0 | ctor 0xC67308; 0xC67676 |
| +0x1B4 | World mode: 0 tool, 1 world loaded, 2 edit in game, 3 save in game | Smooth Streaming note 1 |
| +0x258 | "Skip lot streaming": skips the lot LOD scoring call (0xC6D694). LSO's map-view blocker sets it | LSO, `f_C6D570.asm` |
| +0x310 | 10-sample list of camera motion (the game's "Camera speed") | GC Scheduler / Smooth Streaming comments |
| +0x3A0 | Camera point stored by 0xC6C290 (`movaps [ebx+3A0h]` at 0xC6C2B3) | GC Scheduler |

The live settings are registered in the WorldManager constructor (0xC6763C..0xC677B0, through the registry object from
0x005A00A0) under the group "Streaming" (string 0x00FF829C). Some are globals rather than fields:

| Global | Live setting | Default |
|---|---|---|
| 0x011ECBC0 (byte) | "Throttle Lot LoD Transitions" | not in the initialized data dump, so presumably 0 (.bss) *(inferred)* |
| 0x01184328 (int) | "Throttle Lot LoD Transitions Max Active Lot Threshold" | **12**, range 0..12. LSO "sets" it to 12, which is already the default |
| 0x0118432C (int) | "Force LoD Min Priority" | 1, range 0..4 |

"World Stats" (0x01079400) is also registered.

#### Lot (`Sims3::World::Lot`)

| Offset | Meaning | Evidence |
|---|---|---|
| +0x14 / +0x18 | Object pointer vector begin / end (mObjects) | LSO `kOffObjBegin/End` |
| +0x48 / +0x4C | Lot id lo/hi (read through the lighting manager's lot) | Smooth Streaming `kLotIdLo/Hi` |
| +0xC1 | Detailed view requested (`FUN_00AB1860` returns it) | LSO, 0xAB1860 |
| +0xC9 | Bulldozing (AddLotObjectsToScene early-out) | LSO |
| +0x364, +0x408 == 1, +0x40C == 'P' | Used by `Lot::SetActiveImpl`'s one-shot apartment-shell fixups, which never retry if the shell has no scene presence yet | LSO comment |

#### LotRenderer (argument of 0xAEA680)

| Offset | Meaning |
|---|---|
| +0x10 / +0x14 | Lot id |
| +0x18 | Lot. [+0x18]+0xD0 is a critical section taken with `TryEnterCriticalSection` for the stages flagged in the byte table 0x01068B88 (busy → `return 0`, no state change) |
| +0x1C / +0x1D / +0x1E / +0x1F | Ready / (flag passed to 0x7019F0) / done / failed |
| +0x24 | Stage counter, 0..0x15 |
| +0x28 | Sub-step inside a stage |
| +0x30 | Job-done flag for stages 0x11/0x12 |
| +0x23C | LotLightingManager ("LotRenderer/LotLightingManager", allocated at stage 1) |
| +0x230, +0x238, +0x240, +0x248, +0x24C, +0x244, +0x250, +0x254, +0x258, +0x25C, +0x260 | Floor, Wall, Roof, Fence, Pool, RoofPreview, Ceiling, ModularStairs, FloorFeature, StiltedFoundation, SeaLife render managers |
| +0x3D0 | Optional per-stage timing statistics (float per stage) |

Load stages of `FUN_00AEA680` (from `re/out/fn_00aea680.c`; allocator tag strings in quotes):

| Stage | Work |
|---|---|
| 0 | Setup; scene object bindings (`FUN_007019F0`), `FUN_00AB5530` |
| 1 | "LotRenderer/LotLightingManager" (only if WorldManager+0x151): creates the manager and sets manager+0x4F = 1 (loading) |
| 2 | "LotRenderer/TriangulationLayer" |
| 3 | "LotRenderer/FloorRenderManager" (three sub-steps) |
| 4 | "LotRenderer/LotWaterRenderer" |
| 5 | "LotRenderer/RoofRenderManager" (two sub-steps) |
| 6 | "LotRenderer/WallRenderManager" (two sub-steps) |
| 7 | "LotRenderer/FenceRenderManager" |
| 8 | "LotRenderer/PoolRenderManager" |
| 9 | "LotRenderer/RoofPreviewRenderManager" |
| 10 / 11 | "LotRenderer/CeilingRenderManager" / wait for the ceilings |
| 12 | "LotRenderer/ModularStairsRenderManager" |
| 13 | "LotRenderer/FloorFeatureRenderManager" |
| 14 | "LotRenderer/StiltedFoundationRenderManager" |
| 15 | "LotRenderer/SeaLifeRenderManager" |
| 16 | "LotRenderer/InitDetailedView/..." room sets (sunnyRooms, lightableRooms, fogOfExplorationRooms, blackLitRooms, strobeLitRooms, buildableShellRooms) |
| 17 / 18 | Post a job (function 0xACBB80) / wait for it (+0x30) |
| 19 | Wait for `FUN_00C4D990` |
| 20 (0x14) | Lighting ready (`FUN_00ADBBA0` -> `0x006A5B50` per level: both level solvers' state != 0, i.e. the **wall AO first pass** of every level ran; not yet = yield and retry next frame; round 3, [room-light-maps.md](room-light-maps.md) 4.5); clears manager+0x4F |
| 21 (0x15) | Done (`FUN_00AD9CA0`) |

The budget stopwatch is unit 4 (ms), started once before the loop, and checked after each stage. A failed stage sets
+0x1F and calls `FUN_00AE1A90(1)`.

#### Lot lighting manager
- +0x14: lot.
- +0x4F: loading (set at stage 1, cleared at stage 0x14). This is the flag `FUN_00ADB120` reads.

#### SceneObjectManager ([0x011D1CF8])
- +0x10D0 / +0x10E0: the two lot ids that `FUN_006FDC80` treats as "priority". The same test gives a lot the larger
  lighting budget in `FUN_00ADB120`. Most likely the active or focused lot *(not proven)*.

#### Terrain (WorldManager+0x58)
- +0xB0/+0xB4: chunk vector. Chunk +0x0C/+0x10 are int x/z; +0x54 re-renders the four composited textures; +0x55
  relights.
- +0x110: camera copy. See [terrain-and-light-bake.md](terrain-and-light-bake.md).

### Call flow (render thread = main thread)

```
main loop 0xECA960
 └ ServiceManager::Update 0x588E00 → 0x59ED20 (vtable +0x1C per service)
    └ WorldManager service 0xC7E3C0        (only while WorldManager+0x41)
       ├ WorldManager::Update 0xC6D570     (LSO detour: map view → +0x258 = 1)
       │   ├ camera = 0x6E8330 → 0x6E8400 → eye [+0x60]; point from 0x961440
       │   ├ terrain heightmap 0xC2A290 (tool/edit paths only)
       │   ├ terrain update 0xC845C0(terrain, &point, lowCameraFlag)   ← Smooth Streaming queue
       │   ├ if !+0x258: lot LOD scoring 0xC6C290(dt, &point, ...)
       │   │     ├ metric 0xC62D80 per lot (Active Lot Bias for detailed lots; LSO JZ→JMP at 0xC63015)
       │   │     ├ throttle test [0x11ECBC0], threshold [0x1184328], 0xC69FF0
       │   │     └ detail request 0xAC20E0 → PostRemoteMethodCall(AddLotObjectsToScene 0xAC1130 / demotion)
       │   └ road network, water, sky, ... updates
       └ lot pass 0xC7CEA0([0x11ECE58], dt)                          ← Smooth Streaming frame gate
           └ per lot renderer: 0xAEB2E0
               ├ 0xAEA680 load stages (20/35 ms)                     ← Smooth Streaming slices
               └ 0xAE4CB0 → 0xADB8F0 room lighting (0xADB120 budget) ← Smooth Streaming caps
```

- **AddLotObjectsToScene.** It is posted through `PostRemoteMethodCall`, which runs inline when posted from its own
  thread. LSO's object throttle builds `objectsPerLot` objects per window. It re-posts the rest from the S3SS
  hook/pump thread, so the continuation is really enqueued (a real per-frame spread), no sooner than `delayMs` later.
  Large and flora objects (shells) are built in the first window, because `Lot::SetActiveImpl`'s fixups need them at
  once.
- **Impostors.** The impostor builder (0xAD9E30 / 0xAD97E0) builds a lot impostor synchronously. It pumps the services
  in a nested loop until done, so there is no frame to spread that work over.
- **Thread.** `WorldManager::Update`, the lot pass and the terrain update all run on the render thread (the thread that
  calls Present). Night Terrain Relight calls 0xC845C0 from its Present hook on the same thread.

### Resource system I/O

- **Package I/O is synchronous on the thread that asks for it** (NOTAS 28/09): `FileStream::Read` 0x4DB850 calls
  `ReadFile` directly. RefPack-compressed resources are read and decompressed by 0x4EC010 → 0x4EB3B0, in the requesting
  thread.
- **Async loads** use a read job 0x72A4F0 on the JobThread workers (mask 2). A finalize job 0x7297C0 then runs on the
  main thread (mask 1) inside the JobManager service 0x599A10, one resource per job: factory parse, decompression through
  the stream, GPU object creation. This is the "job 007297C0" that dominates the "jobs run on the render thread" lines of
  the Frame Profiler hitch reports (for example 5.28 ms x65 in hitch #50176 of the 2026-09-28 13:19 report).
- **Deferred completions** are delivered by the ResourceSystem service (0x7377F0 → 0x737560 → 0x72A730).
  ResourceChangeMonitor (0x733D20) reloads changed keys synchronously with no budget.
- **Frame Profiler categories:** "File read", "File flush", "RefPack read", "Jobs (self)" / per job, "Wait for job".
  See [../features/frame-profiler.md](../features/frame-profiler.md).
- **S3SS patches here:** RefPack Decompressor Optimization (replaces 0x4EB3B0 with AVX2/SSE2 code), Mimalloc (CRT
  allocator), WorldCache Size Uncap.

### World cache

Evidence is limited to strings and one S3SS patch:
- `WorldCaches\` (0x00FE2508) is used by 0x5BB4D0 (path builder; callers 0x5EB9C0 and 0x6CBF90).
- S3SS's "WorldCache Size Uncap" (`patches/worldcache_uncap_patch.cpp`) turns a `jb` into a `jmp` to remove a 512 MB
  limit on WorldCache files (Documents\EA\The Sims 3\WorldCaches):
  - pattern `0F 82 ?? ?? ?? ?? 8B 0F E8`; Retail 0x5BD114, EA 0x5BD704;
  - its Steam address entry is commented out, so on Steam it resolves by pattern. The static match is 0x005BC8B4 in
    0x005BC6D0, where the check compares a size with [ebp+38h] + 0x100000;
  - its description says it has no effect on stock EA worlds that don't use a WorldCache.
- 0x6CC570 opens a cache package for "CAS/Compositor/Cache" unless `IgnoreWorldCache` is on the command line.

How the world cache interacts with streaming (what is cached, when it is read) was **not studied**.

### What S3SS Lot Streaming Optimizations touches

(`patches/lot_streaming_optimizations_patch.cpp`, S3SS, all versions; the Steam addresses are from PLANO-SEPARACAO.md
§3 and the 2026-09-28 log.) Each sub-feature is independent. A resolve failure logs and skips it.

| Sub-feature (TOML key, default) | Sites | What it does |
|---|---|---|
| Object throttle (`objectThrottle`, true; `objectsPerLot` 2, range 1-64; `delayMs` 16, range 0-500) | JMP at `Lot::AddLotObjectsToScene` 0xAC1130. Uses 0xABFAC0, 0x7D2DB0/0x7D2DF0 (script message scope, messages 0x4C55E8C/0x4C55EFA), 0xABE9C0, 0xB088C0 | Builds shells first, then N objects per window. The rest is re-posted cross-thread by `DrainPending()` in the patch's `Update()` (every 10 ms pump) |
| Map view blocker (`mapViewBlocker`, true) | Detour on `WorldManager::Update` 0xC6D570 (Retail 0xC6D3B0, EA 0xC6C8F0); calls 0x73E060 | Sets WorldManager+0x258 = 1 around the call while in map view, plus 1 s of grace after leaving it |
| Visibility override (`visibilityOverride`, true) | `74` → `EB` at 0xC63015 (Retail 0xC62AE5, EA 0xC623A5) | Described by LSO as disabling "the camera-view distance bias". Statically, the skipped instruction subtracts WorldManager+0xE0 ("Active Lot Bias", 8.0) from the metric of lots whose +0xC1 (detailed view) is set, which is a hysteresis for already-detailed lots *(inferred: ECX of 0xC62D80 = WorldManager, from the caller 0xC6C290's `mov ebx,ecx`)* |
| Streaming settings (`streamingSettings`, true; `cameraSpeedThreshold` 5.0, range 0-50) | Live settings by name, re-asserted (`AddMaintainedWrite`) | "Throttle Lot LoD Transitions" = true, its Max Active Lot Threshold = 12, "Camera speed threshold" = 5.0 (lots load only after the camera has been slow for a while) |

LSO's install log lines, as seen on 28/09:
- `[LotStreamingOpt] Object throttle installed (entry @ 0x00ac1130)`
- `Map view blocker installed (WorldManager::Update @ 0x00c6d570)`
- `Visibility override installed (JZ->JMP @ 0x00c63015)`
- `Enabled 'Throttle Lot LoD Transitions'`
- `Set 'Max Active Lot Threshold' = 12`
- `Set 'Camera speed threshold' = 5.00`

### Which Apex features depend on what

| Item | Used by |
|---|---|
| 0xC7CEA0, 0xAEA680 (+ budget movs), 0x6FDC80, 0x6FDE10, 0xADB120, 0xC845C0 (+ arming loop 0xC84C3C), 0xC6D68C | Smooth Streaming (combined build only; removed, see [../removed-features.md](../removed-features.md#smooth-streaming)) |
| Terrain grid / chunk flags, `SmoothStreamingRelightTerrainRects` | Night Lighting terrain relight ([../features/night-lighting/terrain-relight.md](../features/night-lighting/terrain-relight.md)). In the combined build the function lived in `smooth_streaming_patch.cpp`; Apex Radiance keeps the logic in the Night Lighting module, and the local terrain relight is `features/terrain_chunk_relight.cpp` ([terrain-and-light-bake.md](terrain-and-light-bake.md#how-apex-drives-and-patches-the-bake)) |
| `[0x011ECBC4]` (WorldManagerPtr), WorldManager+0x41 (active) and +0x1B4 (mode 1..3), plus the loading window (id `0x95947678`, created at `0x00EC7DB9`, removed at `0x00EC7A60`, looked up through UiServiceGetter `0x0050AB70`, the service's vtable +4 root getter and the root's vtable +0xF4 child lookup) | Loaded-world gate `features/world_session.h` (read only, SEH-guarded, never latched across loads): the menu's start note and Depth Blur, which also waits 3 s after the world becomes active (`WorldSession::Settled`). See [../architecture.md](../architecture.md#12-loaded-world-gate-and-start-note) |
| WorldManager+0x41 | Smooth Streaming (indirectly, through the lot-pass gap) and Service Frame Budget, both removed ([../removed-features.md](../removed-features.md)) |
| WorldManager+0x3A0, camera chain 0x11D1860+0x24+0x60, WorldManager ctor store 0xC67883 | Script GC Scheduler (removed, see [../removed-features.md](../removed-features.md#script-gc-scheduler)) |
| 0xC6C290, 0xAC20E0, 0xAEB2E0, call 0xAEB306, 0xAD9E30, 0xADBAD0, 0x6A80E0, 0xADB8F0, call 0xC6D68F, 0xABFAC0 (optional), 0xAD97E0, 0x4DB850/8E0, 0x4EC010 | [Frame Profiler](../features/frame-profiler.md) (timing only) |
| 0x73E060 | `map_view.cpp` (Depth Blur off in map view), LSO |
| CALL 0xADB95D (-> 0xADB120), camera eye read 0xC6D5BD (0x6E8330 / 0x6E8400, +0x60) | Lot Lighting While Moving ([../features/performance/lot-lighting-motion.md](../features/performance/lot-lighting-motion.md)): the budget is scaled while the camera moves |
| 0x4AFFC0 FindProvider and the resource manager's list methods (0x4B2D00, 0x736A70, 0x4B2EC0, 0x4B0960) | Faster Game File Lookups ([../features/performance/resource-lookup-cache.md](../features/performance/resource-lookup-cache.md)) |

### Open questions

- The meaning of the "priority" lots (SceneObjectManager+0x10D0/+0x10E0).
- The default of "Throttle Lot LoD Transitions" (0x011ECBC0 is presumably zero-initialized) and whether the live-setting
  registry loads saved values over the constructor's defaults.
- What exactly 0xC62D80 measures (it is some distance squared, scaled), and whether LSO's JZ→JMP removes a hysteresis
  (static reading) or a view-angle bias (LSO's description).
- The world cache's role in streaming.

## Address reference

| Address | Name / role | Evidence |
|---|---|---|
| 0x011ECBC4 | WorldManager singleton pointer | stored by the WorldManager ctor at 0x00C67883; operand checks in Smooth Streaming and GC Scheduler |
| 0x00C7E3C0 | WorldManager service update (main thread; vtable 0x0107A358 +0x1C). Runs only while [WorldManager+0x41] != 0 (0xC7E3CA..0xC7E3DD) | engine_map disassembly, `svc_vt.txt`, Frame Budget pattern |
| 0x00C7E300 | WorldManager service, simulation side (vtable +0x20) | `svc_vt.txt` |
| 0x00C6D570 | `WorldManager::Update` thiscall(float, float), RET 8, called at 0xC7E403 | `f_C6D570.asm`; LSO detours it |
| 0x00C845C0 | Terrain update, thiscall(terrain, float* camera, char), RET 8, called at 0xC6D68F | Smooth Streaming site, profiler |
| 0x00C6C290 | Lot LOD scoring, thiscall(4), RET 0x10, called at 0xC6D6E4 (skipped while WorldManager+0x258 != 0) | `f_C6D570.asm`, profiler |
| 0x00C62D80 | Lot distance/visibility metric (RET 0xC), called from 0xC6C5D7 | `calls.tsv`, disassembly |
| 0x00C63015 | `je` in 0xC62D80 that skips `metric -= [WorldManager+0xE0]` when `FUN_00AB1860(lot)` (= lot+0xC1) is 0. LSO patches it to `jmp` | disassembly; LSO `lotVisibilityCameraBiasJZ` |
| 0x00C69FF0 | Blocks the next promotion while a promoted lot is still loading (LoD throttle) | Smooth Streaming note 2 |
| 0x00C6C695 | `cmp byte [0x011ECBC0],0`: the "Throttle Lot LoD Transitions" test in the scoring | `full.asm` |
| 0x00AC20E0 | Lot detail request, thiscall(1), RET 4. If lot+0xC1 != arg and lot+0xC9 == 0, stores the flag and posts AddLotObjectsToScene (arg 1) or the demotion (arg 0) through PostRemoteMethodCall. Callers 0xC6A0F9, 0xC6C7C3, 0xC6C7CE, 0xAE61A7 | profiler header |
| 0x00AC1130 | `Lot::AddLotObjectsToScene` thiscall(lot, char initialLoad, char alwaysVisibleOnly), RET 8. Called from 0xACE1E2 | LSO pattern, PLANO §3, `calls.tsv` |
| 0x00ABFAC0 | `Lot::UpdateObjectSceneNode` thiscall(lot, obj, char, char), RET 0xC, per object | LSO pattern, profiler |
| 0x00ABE9C0 | `PostRemoteMethodCall` cdecl(thread, lot, func, a4, char, char): allocates a RemoteMethodCall (vtable 0x010650C4, built at 0xABEA0A) and posts it cross-thread. Runs inline when posted from the target thread itself | LSO comment, profiler |
| 0x007D2DB0 / 0x007D2DF0 | `ScriptMessageScope` ctor(scope, beginMsg, endMsg, lot) / dtor | LSO patterns; bytes confirmed in `full.asm` |
| 0x00B088C0 | `IsObjectLargeOrFlora` cdecl(obj): reads Object+0x18 (resource key) and CTProductObject flag bits (building shells, exterior geometry) | LSO pattern; bytes confirmed |
| 0x00C7CEA0 | Lot renderer pass thiscall(worldRenderer, float dt), RET 4. ECX = [0x011ECE58], called at 0xC7E419 | Smooth Streaming site, disassembly |
| 0x00AEB2E0 | Lot renderer update thiscall(1), RET 4. Callers 0xC7CEDF, 0xAEB3F9, 0xAEB41C. Calls 0xAEA680 at 0xAEB306 while +0x1E and +0x1F are 0, then `FUN_00AE4CB0` (room lighting) | profiler, Smooth Streaming |
| 0x00AEA680 | LotRenderer load stages, thiscall(lotRenderer), RET, returns AL | Smooth Streaming site; `re/out/fn_00aea680.c` |
| 0x00AEA6AC / 0xAEA6D0 / 0xAEA6E9 | The three budget movs: 20 / 35 (priority lot) / 2000 (tool mode) ms | Smooth Streaming |
| 0x006FDC80 | "Priority lot" test thiscall(sceneObjMgr, lo, hi), RET 8: lot id == SceneObjectManager+0x10D0 or +0x10E0 | Smooth Streaming |
| 0x006FDE10 | SceneObjectManager getter `mov eax,[0x011D1CF8]; ret` | bytes checked |
| 0x00ADB120 | Lot lighting budget (ECX = lot lighting manager, ST0): 5 ms; 10 while loading (+0x4F); 15/30 for priority lots; 1000 in tool mode. Only caller: the CALL at 0x00ADB95D in 0x00ADB8F0 | Smooth Streaming; Lot Lighting While Moving redirects that CALL ([../features/performance/README.md](../features/performance/README.md)) |
| 0x00AE4CB0 → 0x00ADB8F0 → 0x006A8BA0 | Budgeted per-frame room lighting of a lot (`FUN_00ADB8F0` only caller 0xAE4D2A) | profiler, Smooth Streaming note 5 |
| 0x00ADBAD0 → 0x006A80E0 → 0x006A3EC0 | Synchronous room light solve (while room+0xF0 == 3, 60000 ms budget), used by the impostor builder | Smooth Streaming note 3 |
| 0x00AD9E30 | Lot LOD switch ("World/LotImpostor/LODOverrideHook"): calls 0xAEB3F0 (two renderer updates + 0xADBAD0). Caller 0xADAEBC | profiler, `fnstrings.tsv` |
| 0x00AD97E0 | Lot impostor wait loop: pumps the services (0x588E00 at 0xAD985C) until the job is done, with no Present | profiler, `profiler_targets.tsv` |
| 0x00ADAF40 | Jump table of the impostor builder states (state 2 → `FUN_00ADAF60`) | Smooth Streaming note 3 |
| 0x00ACB9A9 | "Error during construction of imposter for lot %I64u" | Smooth Streaming note 3, `fnstrings.tsv` (fn 0x00ACB8A0) |
| 0x00C6D970 / 0x00C5FE40 | Load world (sets WorldManager+0x1B4 = 1) / sets 3 ("saveInGameMode") or 2 ("editInGameMode") | Smooth Streaming note 1 |
| 0x00C6CF80 (0xC6D430) / 0x00C6B780 | Sets WorldManager+0x41 at the end of a world load / clears it on shutdown | Smooth Streaming note 1, Frame Budget |
| 0x007F1760 | `GameUtils_SwapLoadScreen` posts a UI callback (the loading screen is script driven) | Smooth Streaming note 1 |
| 0x00C0FD60 | Empty stub (`ret`), the body of `GameUtils_Begin/End/ResetLoadEvent` | Smooth Streaming note 1 |
| 0x0073E060 | `Camera_IsMapViewModeEnabled` (reads camera+0x8B9) | LSO pattern, NOTAS (Mapa (M)) |
| 0x004DB850 / 0x004DB8E0 | `FileStream::Read` (ReadFile) / `Flush` (FlushFileBuffers) | profiler targets |
| 0x004EC010 | RefPack stream read, stdcall(5), RET 0x14 → 0x004EB3B0 decompress | profiler targets, NOTAS |
| 0x004EB3B0 | RefPack decompressor (replaced entirely by S3SS's "RefPack Decompressor Optimization") | S3SS `refpack_decompressor_patch.cpp`, PLANO §3 |
| 0x007377F0 → 0x00737560 → 0x0072A730 | ResourceSystem service → `Update(budgetMs)` (at least 250 ms while +0x1F0) → deferred callback queue | Frame Budget, `profiler_targets.tsv` |
| 0x0072A4F0 / 0x007297C0 | Async resource read job (JobThread workers, mask 2) / finalize job (main thread, mask 1; created at 0x00729C5B) | Frame Budget header |
| 0x00733D20 → 0x00733AA0 | ResourceChangeMonitor: synchronous reload per pending key, no budget | `profiler_targets.tsv` |
| 0x005BB4D0 | Builds the `WorldCaches\` path (call sites 0x5EBA38 in 0x5EB9C0, 0x6CC005 in 0x6CBF90) | `fnstrings.tsv`, `calls.tsv` |
| 0x005BC8B4 | WorldCache size check `jb 0x005BCA49` (0F 82 8F 01 00 00) in 0x005BC6D0 | S3SS "WorldCache Size Uncap" pattern `0F 82 ?? ?? ?? ?? 8B 0F E8`, matched in `full.asm` |
| 0x00EC7DB9 / 0x00EC7A60 | Create / remove the startup and loading window (UI child id `0x95947678`) | `features/world_session.h` comment; `tools/loading_gate_test` |
| 0x0050AB70 | UI root-service getter (`mov eax,[global]; ret`, bytes `A1 imm32 ... C3`), called by `UIManager_GetMainWindowImpl`; the service's vtable +4 returns the UI root, whose vtable +0xF4 looks up a child by id | `framework/game_addresses` id `UiServiceGetter`; `features/world_session.h` |
| 0x006CC570 | Opens a cache for "CAS/Compositor/Cache" unless the command-line option `IgnoreWorldCache` is present (0x0058B100 lookup at 0x6CC58C) *(inferred)* | `fnstrings.tsv`, disassembly |

## See also

- [Main loop, services and threads](main-loop-and-services.md).
- [Terrain chunks and the light bake](terrain-and-light-bake.md).
- [Room light maps](room-light-maps.md).
- [Clock, sleeps and frame limiter](timers-and-sleeps.md).
- [Removed features: Smooth Streaming](../removed-features.md#smooth-streaming).
