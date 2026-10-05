# Main loop, services and threads (TS3W.exe)

This page documents how one frame of The Sims 3 is driven: the main loop `0x00ECA960`, the ServiceManager and its per-frame service updates, the time-sliced services behind most hitches, the job system, the simulation (script) thread and where package I/O happens. It is the map behind the Frame Profiler and the thread model every Apex hook must respect.

## Scope

| | |
|---|---|
| Game build | Steam 1.67.2 (`TS3W.exe`, image base 0x00400000) unless stated |
| Used by | [Frame Profiler](../features/frame-profiler.md), [Performance](../features/performance/README.md), every render-thread hook ([architecture](../architecture.md#10-threads-and-synchronisation)) |
| Evidence | Disassembly (`engine_map` `f_ECA960.asm`, `f_5F0E50.asm`, `f_D81840.asm` and tables), combined-build patch headers, `S3SS_Hitches.txt` of 2026-09-28; `re\out` has no decompile of these functions |

## Overview

How one frame of The Sims 3 (Steam 1.67.2, `TS3W.exe`, image base 0x00400000, no ASLR) is driven: the main loop
0x00ECA960, the ServiceManager and its per-frame service updates, the time-sliced services that cause most hitches,
the job system, the simulation (script) thread, and where package I/O happens. It is the map behind the
[Frame Profiler](../features/frame-profiler.md) (developer mode) and of the combined build's Service
Frame Budget (removed from the standalone, see [../removed-features.md](../removed-features.md)), and the thread model
every Apex hook must respect.

- **Render thread = main thread.** The process entry thread runs `0x00ECBD00` ("Services/Sims3App") which calls the main
  loop 0x00ECA960 (call at 0x00ECBD6C). Sampled render-thread stacks contain 0x00ECBD71 / 0x00ECBE4C / 0x00F7F201 /
  0x00F7F827 (CRT entry area; entry point 0x00F7F371), confirming it.
- Each loop iteration: app state, (inactive idle), ServiceManager::Update (all main-loop services), scene capture,
  Scene::BeginFrame, render frame (scene + UI + Present + limiter), Scene::EndFrame, clock tick.
- **Simulation thread** ("Scripting/Service/Thread", created by the Scripting service) runs `0x00D81840` ("Simulator
  Frame"): Mono script simulation, explicit GC at 0x00D819AA, the ServiceManager *simulation* loop 0x0059ED70, its
  own job pump. **The UI runs on the simulation thread.** The render thread does **not** wait for the simulation every
  frame.
- **Job workers** ("JobThread%i") run jobs; some jobs are pinned to the main thread and run inside the JobManager
  service update (e.g. async resource finalize 0x007297C0).
- Several render-thread services have **their own 5 ms time slice each**, checked only between work items, so several
  busy services in one frame add up to 10-20 ms: the typical hitch.
- Package I/O is synchronous on the thread that asks for it; RefPack decompression at 0x004EC010.

## Details

### Sources

Sources: static study of 2026-09-28 (`NOTAS-ILUMINACAO.md`, last section "Desempenho: mapa do motor"; files in
`scratchpad\engine_map\`: `f_ECA960.asm`, `f_5F0E50.asm`, `f_D81840.asm`, `profiler_targets.tsv`, `addsvc.tsv`,
`addsvc_sites.txt`, `svc_vt.txt`, `regfns.txt`, `calls.tsv`, `strings.tsv`, `fnstrings.tsv`, `cc.tsv`, grep of
`full.asm`), the code of `frame_profiler.cpp` and `patches/frame_budget_patch.cpp` (their header comments record
byte-level verification against `re\TS3W.exe`), and a measured `S3SS_Hitches.txt` (report 2026-09-28 13:19,
51 600 frames). The Ghidra output in `re\out` has **no** decompile of these functions (grep for ECA960 / 59ED20 /
5F0E50 / 599A10 finds nothing); everything below is from disassembly. Items marked (unverified) or (inferred) were not
confirmed at runtime.

### Main loop 0x00ECA960, step by step (from `f_ECA960.asm`)

Prologue: `idle = !0x0058A8D0([0x0114FAB0], L"NoInactiveIdle")` (stored at [esp+0xF]); `ebp = [0x011FF7AC]`;
0x00EC51C0(1) on it; a stopwatch at [esp+0x28] is built with 0x004F35B0(4, 1) (unit 4 = ms). Loop while
`[ebp+0x28] != 0x028D869F`:

1. `0x00ECA9B4 call 0x00EC6C30` (app state), `0x00ECA9BC call 0x00C0FD60` (app+0x20).
2. `bl = ([0x011D3E28] && vfunc+0x58())`. If `idle && !bl`: `0x00ECA9E8 call 0x005887A0` = **Sleep(10) unless the game
   window is in the foreground**. (Profiler evidence: background-window hitches of ~10 ms "Unattributed" with samples
   returning to 0x005887C9 from a call at 0x00ECA9ED.)
3. `dt0 = [0x011CDD9C]`, `dt1 = [0x011CDDAC]`; if renderer+0x8D: `0x00594300(4)` on the clock and `dt1 = 0`, else
   `0x00594330(4)`.
4. `0x00ECAA50 call 0x00588E00(dt0, dt1)` = **ServiceManager::Update**: every main-loop service (below).
5. If `bl`: `elapsed = 0x004F33C0(stopwatch)`; if `elapsed < 100.0` (0x010F5B10) skip steps 6-9 (jump to the clock
   tick); else reset the stopwatch (RDTSC or QPC by its mode). So while `bl` is set the scene is rendered at most every
   100 ms (inferred ~10 fps; what sets `bl` is unverified).
6. `0x009E1B70` -> if a capture manager exists and `0x009DC550` is true: `0x00ECAAD7 call 0x009DE140(0x00ECA030, app)`
   (scene capture; the callback renders off-screen frames without Present).
7. `scene = 0x006E8330()`; if scene: `0x00ECAAE9 call 0x006EBB70` **Scene::BeginFrame**.
8. If `[0x011D1530] != 0`: `0x00ECAAFC call 0x00EC9F00(1)` **render frame with Present**.
9. If scene: `0x00ECAB07 call 0x006E8810` **Scene::EndFrame** (waits for the scene jobs); `0x00ECAB0C call 0x00588E20`.
10. `0x00ECAB1A call 0x005943F0` **clock tick** (every iteration, also when rendering was throttled).

#### Render frame 0x00EC9F00(bool present)
`renderer = 0x0060E2C0()`; if `0x00611620` (BeginFrame / BeginScene) succeeds: viewport / camera setup (0x00533300,
0x0060FFE0), 0x0050AB70 object vfunc +0x30, 0x00C0FD60; then `0x009E19E0` object vfunc +0x48 (roles not studied: the
scene and UI submission happen in this span); `if (present) 0x00611760` -> 0x00611680 (EndScene, Present); then at
**0x00EC9FBA**: `cmp byte [esi+0x8D],0` -> if set, `0x004E1320(&30)` = **sleep 30 ms while the window is inactive**.
Smooth Patch (S3SS) replaces the 9 bytes at 0x00EC9FBA with its frame limiter call (`3E 56 E8 rel32 5E EB`); original
bytes `80 BE 8D 00 00 00 00 5E 74 15` (profiler `LimiterText`).

Consequences for hooks: D3D9 `Present` (and every registry Present hook) runs inside 0x00611680, inside 0x00EC9F00,
inside the main loop, on the main thread. Everything between two Presents (clock tick, app state, services, capture,
BeginFrame, scene/UI, EndScene) belongs to one "frame interval"; that is the Frame Profiler's frame definition.

### ServiceManager

Object `[0x011CE3F4]`. The service list is an intrusive circular list whose head is the manager itself:

| Offset | Field | Evidence |
|---|---|---|
| mgr+0x00 | first node (`[list]`); the walk ends when `node == list` | 0x0059ED2B..0x0059ED5D |
| mgr+0x0C | "running" flag: the loops do nothing while 0; AddService refuses once non-zero | 0x0059ED23, 0x0059F503 |
| node+0x00 | next node | 0x0059ED59 |
| node+0x08 | service object | 0x0059ED3D |
| node+0x14 | byte flags: bit 0 = main loop (vtable +0x1C), bit 1 = simulation loop (+0x20) | 0x0059ED37, 0x0059ED87 |
| node+0x1A | byte enabled | 0x0059ED31 |

Service vtable: +0x00 called by AddService first (init, inferred), +0x04 release on failure, +0x1C main-loop
update `thiscall(float, float)` `ret 8`, +0x20 simulation-loop update, +0x28 id / type (0x009F0DE0 in almost all).
Updates run **in registration (list) order**; for the four budgeted services that order is JobManager,
ResourceSystem, TextureCompositor, CAS SimService (`frame_budget_patch.cpp`, `enum Svc`).

Registration: `AddService(svc, flags)` 0x0059F500. `regfns.txt` lists the 58 functions that register services;
`addsvc.tsv` / `addsvc_sites.txt` list the 105 call sites found (for each registration two rows: the service ctor call
and the AddService call; `flags` = the pushed node flag, 1 main, 2 simulation, 3 both; **0 also appears when the flag
is pushed from a register** (e.g. 0x00607CAD / 0x00607CBC push `esi`), so 0 is not reliable; `vt` resolved for
some). Examples: 0x00599AA6 (JobManager, ctor 0x00599970, flags 1), 0x005F173F (CAS SimService, ctor 0x005F1450,
flags 1), 0x00607CAD (TextureCompositor, ctor 0x00607BD0, flag in a register), 0x00739220 (ResourceSystem, ctor
0x00739120, flags 1), 0x00733A51 (ResourceChangeMonitor, flags 3), 0x00B3A21F (ObjectDesigner, ctor 0x00B38A90, flags 1),
0x00C7E3A1 (WorldManager, vtable 0x0107A358), 0x0076B5A3 / 0x0076B5F6 (Scripting, flags 1 / 2).

#### Service vtables with a non-empty update (`svc_vt.txt`; names from the study / `kServiceNames`)

| Vtable | +0x1C main update | +0x20 sim update | Name |
|---|---|---|---|
| 0x00FCBCCC | 0x00588890 | - | MessageServer |
| 0x00FCBD34 | 0x00588A00 | - | unnamed (calls 0x00588770(1) when its object's vfunc +0x8C is false); ~0.2-0.3 ms per frame measured |
| 0x00FD9690 | 0x00598660 | - | Input (message pump, 0x00410890, PeekMessageW / Dispatch) |
| 0x00FD9810 | **0x00599A10** | - | **JobManager service (main-thread jobs)** |
| 0x00FE514C | **0x005F0E50** | - | **CAS SimService** |
| 0x00FE6B08 | **0x00608630** | - | **CAS TextureCompositor** |
| 0x00FE6F08 | 0x0060C5A0 | - | unnamed |
| 0x00FF7D84 | 0x006E3620 | - | Scene service (SceneObjectManager 0x00701270, 0x00693780) |
| 0x00FF7D7C | 0x006E3A20 | 0x006E3A40 | unnamed (its +0x28 is 0x00861C50: possibly not a service vtable) |
| 0x00FFC420 | 0x0071E640 | - | Swarm (VFX) |
| 0x00FFCF8C | 0x00723EE0 | - | unnamed |
| 0x00FFDF58 | 0x00733D20 | 0x00733D30 | ResourceChangeMonitor (sync reload per pending key, no budget) |
| 0x00FFEB10 | **0x007377F0** | - | **ResourceSystem** |
| 0x01000798 | - | 0x00740A70 | unnamed (sim) |
| 0x01007BEC | 0x0076B330 | - | Scripting service, main side (calls 0x005887A0 background idle, 0x00594300 / 0x00594330) |
| 0x01007B74 | - | 0x0076B1A0 | Scripting service, sim side (2.4 s over 51 600 frames measured) |
| 0x01015DF4 | 0x007A08C0 | - | ShaderSystem |
| 0x0101C158 | 0x007E6840 | - | unnamed |
| 0x0101E758 | - | 0x007F1530 | unnamed (sim; 11 s total measured) |
| 0x01029060 | 0x00868FF0 | 0x00869000 | unnamed (sim side 3 s total measured) |
| 0x0102CCE0 | 0x00883080 | - | unnamed |
| 0x010450DC | 0x0096DBD0 | 0x0096C2F0 | unnamed (sim side 0.43 s) |
| 0x0105373C | - | 0x009E52C0 | unnamed (sim) |
| 0x01059CA8 | - | 0x00A23B50 | unnamed (sim) |
| 0x0105DEE4 | 0x00A37E60 | - | Crossroads (SIGS AccountManager) |
| 0x0106BE6C | **0x00B3A960** | - | **ObjectDesigner** ("ObjectDesignerService") |
| 0x0107A358 | 0x00C7E3C0 | 0x00C7E300 | WorldManager (main: WorldManager::Update 0x00C6D570, lot pass 0x00C7CEA0, terrain...; sim: 0x00C636D0) |
| 0x010F3D00 | - | 0x00EC5340 | unnamed (sim; **16 s** self over 51 600 frames: the heaviest sim-loop service measured) |

`svc_vt.txt` also holds vtables whose updates are both 0x00861C50 (no per-frame work) and one garbage row
(0x0102267C, string data). It is **incomplete**: the profiler met services with vtables 0x01088F2C (main 0x00D61670,
sim 0x00D4D6D0), 0x01051C78 (0x009D96D0) and 0x00FF0070 (0x00687C80) that are not in it.

### The time-sliced services on the render thread

All use the EA stopwatch (0x004F35B0 ctor with unit 4 = ms, 3 = us; 0x004F34F0 arms `now + value`; value read as
unsigned 32-bit, 0xFFFFFFFF = unlimited). **Every deadline is checked only after a work item**, so one item always runs
and can overshoot. The patch sites of the removed Service Frame Budget are recorded in
[../removed-features.md](../removed-features.md).

| Service | Update | Budget | Work |
|---|---|---|---|
| JobManager (vt 0x00FD9810) | 0x00599A10 | `[svc+0xCC]` ms (default 5, "JobManager" timeslice variable), only if main-thread job context `[svc+0xC8] != 0`; `mov edi,[esi+0xCC]` at 0x00599A20, unit push at 0x00599A28 | ProcessJobs (vfunc +0x24 of `[svc+0x18]`) for main-thread jobs; includes the async resource loader's finalize job 0x007297C0 (resource construction: factory parse, decompression, GPU object creation, one resource per job) whose read job 0x0072A4F0 runs on JobThread workers |
| ResourceSystem (vt 0x00FFEB10) | 0x007377F0 = `push [svc+0x14]; call 0x00737560` on svc+0x18 | `[svc+0x14]` ms (ctor 0x00739120 sets 5); at least 250 ms while `+0x1F0` (0x00737572) | 0x00737560 -> 0x0072A730(0x7FFFFFFF, budget): deferred callbacks / async-load completions (0x0072A730 also called from 0x00737060) |
| CAS SimService (vt 0x00FE514C) | 0x005F0E50 | `[svc+0x139] ? [svc+0x2C] : -1` (0x005F0EBF); ctor 0x005F1450 sets +0x2C = 5 and **+0x139 = 1 (slicing on)**; script toggles +0x139 through 0x005A50F0 (CASUtils_SimServiceEnableTimeSlicing) | phase A walks requests (+0x58), deadline after each step (0x005F1262); when exhausted, phase B 0x005F129D **always** runs one step of the current build `[svc+0x68]` (ModelBuilder 0x005DC800, texture composite 0x005CED00...). Frames of 9-15 ms are "slice + one step"; 40-147 ms frames were single build steps |
| CAS TextureCompositor (vt 0x00FE6B08) | 0x00608630 | `[svc+0x15] ? -1 : [svc+0x18]` ms (ctor 0x00607BD0 sets 5; +0x15 only by `-generateThumbnails`) | work loop 0x00608270 (only caller 0x00608630), unit at 0x0060829D |
| ObjectDesigner (vt 0x0106BE6C) | 0x00B3A960 | **none seen** | whole update under mutex 0x0106B874 (`Mutex::Lock` at 0x00B3A97F) |

Other main-loop services without a budget can also spike: ResourceChangeMonitor 0x00733D20 -> 0x00733AA0 (synchronous
reload per pending key), WorldManager 0x00C7E3C0 (runs only while `[[0x011ECBC4]+0x41]`; contains WorldManager::Update
0x00C6D570, the lot renderer pass 0x00C7CEA0, terrain 0x00C845C0 via 0x00C6D68F and more; see
[lot loading and streaming](lot-loading-and-streaming.md)).

### Thread model

| Thread | What runs there | Evidence |
|---|---|---|
| **Main = render** | main loop 0x00ECA960: app state, main-loop services (flag 1) including JobManager's main-thread jobs, scene capture, Scene::Begin/EndFrame, render frame, D3D9 calls, Present, all D3D9 registry hooks, the S3SS / Apex ImGui overlay (EndScene) | f_ECA960.asm; the profiler's `g_renderTid` = thread calling Present |
| **Simulation** ("Scripting/Service/Thread") | 0x00D81840 "Simulator Frame": Mono script host steps (0x00D74800, 0x00D7F8B0, 0x00D7FE80), explicit GC at 0x00D819AA, animation ticks (0x0083CED0, 0x00571B90), the ServiceManager simulation loop 0x0059ED70 (4 call sites), sim-side job pump at 0x00D81FDA, WorldManager sim update 0x00C636D0. **The game UI (scripted) runs here.** | f_D81840.asm, profiler_targets.tsv, NOTAS engine map; the profiler identifies it as the thread calling GC_try_to_collect |
| **Job workers** ("JobThread%i", created in 0x00599CF0) | jobs with worker affinity (e.g. resource read job 0x0072A4F0, mask 2), scene jobs | fnstrings.tsv, frame_budget_patch.cpp header |
| Others | SIGS / Crossroads network threads ("NamedPipeListenerThread", "SIGS/UploadThread/..."), Mono / OS threads | strings.tsv |

Cross-thread rules:
- The render thread does **not** wait for the simulation each frame (NOTAS engine map). Simulation-to-render work goes
  through posted remote calls (0x007D98C0 / job function 0x007D9840) and PostRemoteMethodCall (0x00ABE9C0), which run as
  jobs on the target thread (e.g. `Lot::AddLotObjectsToScene` 0x00AC1130 posted by 0x00AC20E0).
- The render thread does wait for its own scene jobs: Scene::BeginFrame may wait for the previous frame's job, Scene::EndFrame
  waits for the current ones (Job::Wait 0x0059A500 -> WaitForJob 0x0059A220).
- **Package I/O is synchronous on the requesting thread** (FileStream::Read 0x004DB850 -> ReadFile; RefPack 0x004EC010
  decompresses inline). On the render thread this is typically inside the finalize job 0x007297C0 or a service.
- EA mutexes (0x004E16F0) are taken on all threads (864 call sites); render-thread blocking on them was measured small
  (< 0.1 ms per hitch on average, worst 3.3 ms).

### Measurements

From the notes (static study + profiler, 2026-09-28): about **5 ms per frame unattributed** (render-thread time outside
the timed functions); **spikes > 50 ms create ~16 textures**; limiting lot / light work (Smooth Streaming) did **not**
reduce the spikes.

From the report `S3SS_Hitches.txt`, 2026-09-28 13:19 (51 600 frames, Smooth Patch limiter, Script GC Scheduler
redirect active; p50 4.38 ms, p95 14.28, p99 32.38, 6.86% hitches; last 200 hitches averaged 32.75 ms vs a median of
9.92 before them):

| Per hitch (render thread, self ms) | avg | worst |
|---|---|---|
| Services (self) | 13.64 | 134.51 |
| Unattributed | 5.08 | - |
| Render frame (game) | 3.51 | 23.53 |
| Present (driver) | 3.51 | 19.13 |
| Jobs (self) | 2.41 (+4.38 on other threads) | 67.46 |
| Scene::BeginFrame | 2.10 | 45.65 |
| Terrain update | 1.55 | 52.45 |
| Script GC (simulation thread) | 1.42 | 3.94 |

Services per hitch (incl / self ms): WorldManager 5.09 / 0.35, JobManager 2.76 / 0.00 (its jobs, mainly 0x007297C0 at
2.19), CAS SimService 2.72 / 2.71, CAS TextureCompositor 0.91, Scene service 0.66, 0x00588A00 0.33, Swarm 0.27,
ResourceSystem 0.21. Typical single hitch: CAS SimService 9.3 + JobManager jobs 5.2 + Swarm 5.1 + TextureCompositor 4.6
ms in one frame, i.e. the 5 ms slices adding up.

### Which Apex features depend on what

Status in Apex Radiance: Frame Budget, Smooth Streaming and the Script GC Scheduler are **removed** (combined
build only; [../removed-features.md](../removed-features.md)); the Frame Profiler runs only in developer mode. The rows
naming them record where the combined build touched the engine.

| Address / item | Feature (file) | Use |
|---|---|---|
| 0x0059ED20 / 0x0059ED70 | Frame Profiler (`frame_profiler.cpp`) | loop bodies replaced by a timed C++ walk (hand-made hook) |
| 0x00588E00, list order | Frame Budget (`patches/frame_budget_patch.cpp`) | pass detection (a service seen twice starts a new pass; impostor pump passes count) |
| 0x00599A10, 0x00599A20 | Frame Budget; Profiler (name) | Detours; budget `mov edi,[esi+0xCC]; push 0; push 4` -> global in us, unit 3 |
| 0x005F0E50, 0x005F0EBF, 0x005F127D, +0x139 / +0x2C | Frame Budget; Profiler (name) | Detours; budget load; phase A gate stub (defer phase B) |
| 0x00608630, 0x00608270, 0x0060829D, +0x15 / +0x18 | Frame Budget; Profiler (name) | Detours; unit byte 4 -> 3; field in us |
| 0x007377F0, 0x00737560, +0x14 | Frame Budget; Profiler (name) | Detours; field in ms |
| 0x00C7E3CA, `[0x011ECBC4]+0x41` | Frame Budget; also Smooth Streaming and GC Scheduler read `[0x011ECBC4]` | world-loaded test |
| 0x00EC9F00, 0x00611680 | Frame Profiler | frame split: render / EndScene / Present / limiter |
| 0x00EC9FBA | Smooth Patch (S3SS, `smooth_patch_precise.cpp`); Profiler `LimiterText` | limiter call site; must not be touched by Apex |
| 0x006EBB70, 0x006E8810, 0x009DE140, 0x00EC6C30, 0x005943F0, 0x00AD97E0 | Frame Profiler | timed (Detours) |
| 0x00599720, 0x0059A220, 0x004E16F0, 0x004E2760, 0x004DB850, 0x004DB8E0, 0x004EC010 | Frame Profiler | timed (hand-made hooks, all threads suspended) |
| 0x007D9840, 0x010650C4 | Frame Profiler | intended job keys for remote calls (the globals holding them are never set in the combined code; see the profiler doc) |
| 0x00D819AA -> 0x00E4A050 | Script GC Scheduler (`patches/gc_scheduler_patch.cpp`, redirects the CALL); Chunky "Disable GC_try_to_collect" (S3SS, NOPs it); Profiler (times the entry, identifies the simulation thread) | see [mono-gc](mono-gc.md) |
| D3D9 Present inside 0x00611680 on the main thread | every Apex post effect, Night Lighting frame-boundary logic, Profiler | frame boundary = registry Present hook |
| 0x004AFFC0 FindProvider (slots 0x00FB2DE0 / 0x00FFE290), 0x004B2D00 / 0x00736A70 RegisterDatabase, 0x004B2EC0 SetDatabasePriority, 0x004B0960 DatabaseChanged (their +0x34 / +0x3C / +0x4C slots) | Faster Game File Lookups (`features/resource_cache.cpp`); Profiler (FindProvider counter) | vtable-slot layers (`framework/slot_chain.h`); the package list, the database classes and the file watcher are described in [../features/performance/README.md](../features/performance/README.md) |
| Background idle 0x005887A0 / NoInactiveIdle | none (explains 10 ms "Unattributed" background frames) | |
| Renderer +0x8D, sleep at 0x00EC9FBA | none directly; explains ~30 ms frames while inactive (profiler "Frame limiter" row) | |

### Pitfalls

- `profiler_targets.tsv` gives the loop body as 0x42 bytes; it is **0x44** (0x0059ED20..0x0059ED63, `ret 8` at
  0x0059ED61). The profiler's pattern is the full 0x44 bytes.
- The service loops, ExecuteJob, Mutex::Lock etc. are called from many threads: patch them only with all other threads
  suspended and an atomic 8-byte write (see the profiler's `AttachSafe`), never with plain Detours from one thread.
- `WorldManager::Update` 0x00C6D570 is owned by LotStreamingOptimizations (S3SS); 0x00AEA680 / 0x00C845C0 entries by
  Smooth Streaming; 0x00EC9FBA by Smooth Patch; 0x00D819AA by the GC patches. Use call sites / callers.
- Budgets are checked after each item: lowering a slice never bounds a single item (one CAS build step, one resource
  finalize). Frame Budget therefore defers the Sim build step instead.
- Nested ServiceManager passes happen inside the lot impostor pump 0x00AD97E0 (no Present in between): per-frame logic
  keyed on "one ServiceManager pass = one frame" must tolerate them.
- Scene::EndFrame runs about twice per frame interval in practice (93 494 calls / 51 600 frames in the report); do not
  use it as a frame counter.

### Open items

- What sets `bl` (object `[0x011D3E28]` vfunc +0x58) and therefore the 100 ms render throttle; meaning of the two clock
  calls 0x00594300 / 0x00594330 and of `[0x011D1530]`.
- Which of the two service args (clock+0x54, clock+0x64) is "dt" and which "real dt" (named dt / realDt in code by
  convention only).
- Identity of the heavy unnamed simulation-loop services (0x00EC5340, 0x007F1530, 0x00869000) and of 0x00588A00.
- The thread that runs the lot impostor pump: the profiler labels it render, but its time was booked in the simulation
  column of the 13:19 report.
- Which caller of Scene::EndFrame (0x007EB6D0 / 0x00ECA2A0 / 0x00AD97E0) accounts for the second call per frame.
- `svc_vt.txt` / `addsvc.tsv` miss some services (vtables 0x01088F2C, 0x01051C78, 0x00FF0070); regenerate from
  `full.asm` by scanning all 0x0059F500 call sites if a complete list is needed.

## Address reference

| Address | Name / role | Evidence |
|---|---|---|
| 0x00ECBD00 | app run function ("Services/Sims3App", "Services/Errors"); calls the main loop at 0x00ECBD6C | fnstrings.tsv, calls.tsv |
| **0x00ECA960** | **main loop** (thiscall on the app object; `ret`) | f_ECA960.asm |
| 0x00EC6C30 | app state machine update (`this` = app+0x10) | f_ECA960.asm (0x00ECA9B4); timed by the profiler |
| 0x00C0FD60 | called on app+0x20 each iteration (also from 0x00EC9F95) | f_ECA960.asm; role not studied |
| 0x0058A8D0 | option lookup, called with `[0x0114FAB0]` and L"NoInactiveIdle" (0x010F5B14) | f_ECA960.asm, strings.tsv |
| 0x005887A0 | background idle: `GetForegroundWindow`; if none or its process is not ours, `Sleep(10)` | full.asm; iat.map |
| 0x0072F1C0 | getter `[0x011D3E28]`; its vfunc +0x58 decides the 100 ms render throttle (below) | full.asm; meaning unverified |
| 0x0060E2C0 | getter of the renderer / device wrapper `[0x011CECC8]` | full.asm |
| 0x00594300 / 0x00594330 | clock object (0x011CDD48) calls with arg 4, chosen by renderer+0x8D | f_ECA960.asm; semantics unverified |
| **0x00588E00** | **ServiceManager::Update wrapper**, cdecl(float, float): `ecx = [0x011CE3F4]`, calls 0x0059ED20 | full.asm; callers 0x00ECAA50 (main loop), 0x00AD985C (impostor pump) |
| **0x0059ED20** | **ServiceManager main loop**: each node with flag & 1 -> service vtable **+0x1C**(dt, realDt); indirect call **0x0059ED57** (`FF D0`); 0x44 bytes, `ret 8` | full.asm; calls.tsv `IND:eax`; also called directly from 0x00B36DB5 |
| **0x0059ED70** | **ServiceManager simulation loop**: flag & 2 -> vtable **+0x20**; indirect call 0x0059EDA7 | full.asm; callers 0x00D81AA2, 0x00D81C8E, 0x00D81E73, 0x00D81F55 (all in 0x00D81840) |
| 0x0059F500 | ServiceManager::AddService(service, flags), thiscall, `ret 8`; refuses once `[mgr+0xC] != 0` | full.asm |
| 0x0059EFE0 | ServiceManager lookup by id (registration functions skip creation when it returns true) | full.asm (0x00599A6E) (inferred) |
| 0x00861C50 | `ret 8`: default (empty) service update in vtables | full.asm, svc_vt.txt |
| 0x009F0DE0 | common vtable +0x28 entry of services (id / type query used by AddService) | svc_vt.txt, 0x0059F525 (inferred) |
| 0x009E1B70 | SceneCaptureManager getter: `[[0x011E9AF0]+0x20]` | full.asm |
| 0x009DC550 | "capture pending?" test before 0x009DE140 | f_ECA960.asm (inferred) |
| **0x009DE140** | **SceneCaptureManager::Process**(callback 0x00ECA030, app): thumbnails / photos / video captures | f_ECA960.asm; profiler_targets.tsv |
| Apex player screenshot | Reads the presented D3D9 back buffer in the registered Present hook; this is the path that includes Apex's in-frame post-processing. It does not call SceneCaptureManager. | `features/captures.cpp`; in-game visual validation pending |
| 0x00ECA030 | capture render callback: BeginFrame, render frame **with arg 0 (no Present)**, EndFrame | full.asm |
| 0x006E8330 | Scene getter `[0x011D1860]` | full.asm |
| **0x006EBB70** | **Scene::BeginFrame**: submits scene jobs, hands the render queue over; returns early if scene+0x290 set or renderer+0x8D set; may wait on the previous frame's job | full.asm; profiler_targets.tsv |
| **0x00EC9F00** | **render frame**(bool present), `ret 4`: BeginFrame 0x00611620, scene + UI, `if (arg) 0x00611760` (end frame + Present), then at **0x00EC9FBA** `if (renderer+0x8D) sleep 30 ms` | full.asm |
| 0x00611620 | BeginFrame (device BeginScene, vtable +0xA4 per profiler); fails when renderer+0x8D set | full.asm; frame_profiler.cpp header |
| 0x00611760 -> 0x00611680 | end frame + Present: EndScene (device vtable +0xA8), Present (+0x44); 0x00611680 only caller 0x00611766 | full.asm; frame_profiler.cpp header |
| 0x004E1320 | EA thread sleep: `SleepEx([arg], TRUE)` (arg points to ms) | full.asm, iat.map |
| **0x006E8810** | **Scene::EndFrame**: waits for the scene jobs (several `Job::Wait` 0x0059A500 calls on scene+0x310 / +0x318 / +0x31C ...), pops the render queue | full.asm |
| 0x00588E20 | after EndFrame: returns a bool from renderer+0x8E / 0x006115C0; result unused by the loop | full.asm; role not studied |
| **0x005943F0** | **game clock tick** on 0x011CDD48 (QPC delta vs clock+0x38/+0x3C) | full.asm |
| 0x00ECA2A0 | another loop / frame variant (capture, BeginFrame, render, EndFrame); no direct caller in calls.tsv | calls.tsv; not studied |
| 0x007EB6D0 | also calls Scene::BeginFrame / EndFrame | calls.tsv; not studied |
| 0x00AD97E0 | lot impostor wait loop: pumps ServiceManager::Update 0x00588E00 and Scene::EndFrame until its job is done, no Present | full.asm; profiler_targets.tsv |
| 0x00D81840 | simulation thread frame ("Simulator Frame") | fnstrings.tsv, f_D81840.asm |
| 0x00D819AA | `call 0x00E4A050` GC_try_to_collect (explicit script GC) | f_D81840.asm; see [mono-gc](mono-gc.md) |
| 0x00D81FDA | simulation-thread job pump: JobManager vfunc +0x24 ProcessJobs(ctx `[esi+0xAC0]`, 0, 1) | f_D81840.asm; profiler_targets.tsv |
| 0x0076B1B0 | ctor of the Scripting service (references "Scripting/Service/Thread", 0x01007BC8); registered at 0x0076B5A3 (flags 1) | fnstrings.tsv, addsvc.tsv (thread creation inferred) |
| 0x00599CF0 | creates the job worker threads ("Services/JobManager/JobThread", "JobThread%i") | fnstrings.tsv |
| 0x00599A60 | registers the JobManager service: lookup id 0x02A8D18B, `new(0xD8)`, ctor 0x00599970, AddService(svc, 1) | full.asm |
| 0x00599720 | JobManager::ExecuteJob(job): `[job+0x24]=4`, `call [job+0x10](job+8, [job+0x14], 4)` at 0x0059974F | full.asm |
| 0x0059A430 | JobManager::ProcessJobs(ctx, timer, flag), vtable 0x00FD97A4 +0x24 | profiler_targets.tsv |
| 0x00599E60 | job loop that checks the deadline after each job | frame_budget_patch.cpp header |
| 0x0059A220 | JobManager::WaitForJob(job) (semaphore, infinite); reached via Job::Wait 0x0059A500 | full.asm, calls.tsv |
| 0x007D98C0 / 0x007D9840 | RemoteCall::Post(toSim, async) / RemoteCall job function (phase 4 runs the call) | profiler_targets.tsv, full.asm |
| 0x00ABE9C0 | PostRemoteMethodCall; objects get vtable 0x010650C4 at 0x00ABEA0A (native method at +0x10) | full.asm |
| 0x004F35B0 / 0x004F34F0 / 0x004F33C0 | EA stopwatch: ctor(unit, flag), arm(value, flag), elapsed (float) | frame_budget_patch.cpp header; f_ECA960.asm |
| 0x004E16F0 / 0x004E17B0 | EA::Thread::Mutex::Lock(timeout*) / Unlock | full.asm; profiler_targets.tsv |
| 0x004E2760 | EA::Thread::Semaphore::Wait(timeout*) | profiler_targets.tsv |
| 0x004DB850 / 0x004DB8E0 | FileStream::Read (ReadFile) / Flush (FlushFileBuffers) | profiler_targets.tsv |
| 0x004EC010 | RefPack stream read, stdcall(5) `ret 0x14`: header magic `& 0x1FFF == 0x10FB`, calls 0x004EB2F0 and 0x004EB3B0 (decompress); referenced only from data 0x00FB9020 | full.asm, calls.tsv, datarefs.tsv |

Global objects and constants:

| Address | Content | Evidence |
|---|---|---|
| `[0x011CE3F4]` | ServiceManager | 0x00588E04, 0x00599A61, 0x00AD97F0 |
| `[0x011CECC8]` | renderer / device wrapper; +0x8C, **+0x8D** (window inactive / minimised: skips BeginScene and BeginFrame, zeroes the second service dt, 30 ms sleep in the render frame), +0x8E | 0x0060E2C0, 0x00611620, 0x006EBB86, 0x00ECAA12, 0x00EC9FBA (meaning inferred from use) |
| `[0x011D1860]` | Scene | 0x006E8330 |
| `[0x011E9AF0]` +0x20 | SceneCaptureManager | 0x009E1B70 |
| `[0x011D1530]` | render gate: the main loop calls the render frame only while non-null; also used by the remote-call job 0x007D9840 | 0x00ECAAEE, 0x007D9855 (identity unverified) |
| `[0x011D3E28]` | object whose vfunc +0x58 enables the 100 ms render throttle | 0x0072F1C0 (identity unverified) |
| 0x011CDD48 | game clock object; floats at +0x54 (0x011CDD9C) and +0x64 (0x011CDDAC) are the two service update args | f_ECA960.asm |
| `[0x011FF7AC]` | app object; loop runs while `[+0x28] != 0x028D869F` | f_ECA960.asm (exit meaning inferred) |
| `[0x011ECBC4]` | WorldManager; +0x41 = world loaded (WorldManager service runs only while set) | frame_budget_patch.cpp (0x00C7E3CA) |
| 0x010F5B10 | float 100.0 (render throttle interval) | dwords.txt `42C80000` |
| 0x010F5B14 | L"NoInactiveIdle" | strings.tsv |

## See also

- [Lot loading and streaming](lot-loading-and-streaming.md).
- [Mono / Boehm GC and the simulation thread](mono-gc.md).
- [Clock, sleeps and frame limiter](timers-and-sleeps.md).
- [Removed features](../removed-features.md#service-frame-budget): Service Frame Budget.
