# Frame Profiler

A developer tool that measures every frame and records what each stutter (hitch) is made of. It times about 40 game
functions (render and Present path, frame limiter, main-loop steps, every ServiceManager service, jobs, waits, file and
RefPack I/O, lot streaming, lot lighting, terrain, script GC, the performance counters), counts D3D9 calls and the time
Apex Radiance's own D3D9 hooks take. An optional statistical sampler covers the time no timed function explains. Results
appear in live tables in the menu and in `ApexRadiance_Hitches.txt`. Nothing is hooked while it is off.

## Status

| | |
|---|---|
| Availability | Developer mode only (included in the unified ASI; the normal runtime mode never loads or starts it) |
| Default | Off; never started automatically, also when the saved file says `enabled = true` |
| Menu | Developer > Performance > *Frame times and stutters* |
| Configuration | `[qol.frame_profiler]` in `ApexRadiance.toml`; profiles with the Development part store a copy under `[developer.frame_profiler]` |
| Source | [`features/frame_profiler.{h,cpp}`](../../features/frame_profiler.cpp), [`features/address_space.{h,cpp}`](../../features/address_space.cpp), mod-time instrumentation in [`framework/d3d9_hooks.cpp`](../../framework/d3d9_hooks.cpp) |

## The problem

Hitches (single long frames) in The Sims 3 come from many places: lot streaming, lot light solves, terrain, Create a Sim
builds, resource finalisation on the main thread, script GC, the GPU or vsync, the frame limiter and the mod's own hooks.
A debugger cannot show which of these caused one particular frame on a player's machine. The engine's main loop and
thread model are described in [engine main loop and services](../engine/main-loop-and-services.md).

## How Apex Radiance solves it

The profiler wraps the relevant game functions with timers, attributes time per thread and per category, and stores a
breakdown for every frame that exceeds the hitch threshold.

1. **Enable.** The clock is set up once (RDTSC calibrated against QPC when the CPU reports an invariant TSC, otherwise
   QPC), the writer thread starts and the D3D9 registry hooks are registered.
2. **Attach at a frame boundary.** The game functions are attached inside the first Present hook, on the render thread,
   so call-site writes never race the render thread and the startup patches have installed first.
3. **Attribute.** Each timed call pushes a frame on a per-thread stack. Self time excludes timed children; inclusive time
   is counted only for the outermost call of a category. At every Present the per-thread deltas are bucketed as render,
   simulation or other threads.
4. **Detect hitches.** A frame is a hitch when it is longer than the multiplier times the median of the last 120 frames
   and longer than the floor.
5. **Report.** Each hitch is queued to a writer thread that appends it to `ApexRadiance_Hitches.txt`; *Save report now*
   appends a full session report.

## Settings

All settings are in the card's *Measurement setup* section and its *Advanced* node.

| Menu label | TOML key (`[qol.frame_profiler]`) | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Card switch *Frame times and stutters* | `enabled` | bool | off | | Turns the profiler on. Saved, but the loader always forces it off: a measurement starts only from the menu |
| Hitch multiplier | `hitch_multiplier` | float | 2.0 | 1.2 to 5.0 | Hitch = frame longer than this times the median of the last 120 frames |
| Hitch floor | `hitch_floor_ms` | float | 8 ms | 1 to 100 ms | Frames shorter than this are never hitches |
| Count state calls | `count_state_calls` | bool | on | | Shows SetTexture, Set*Shader, Set*ShaderConstantF and SetRenderTarget per frame |
| Write ApexRadiance_Hitches.txt | `write_file` | bool | on | | Hitches are queued to the writer only while this is on |
| Sample the render thread | `sample_render` | bool | off | | Starts or stops the sampler for the render thread |
| Sample the simulation thread | `sample_simulation` | bool | off | | Needs the simulation thread id (known after the first script GC call) |
| Sampling rate | `sample_hz` | int | 2000 | 250 to 4000 Hz | Per sampled thread |
| Time lot object building (this session) | `time_objects` | bool | off | | Attaches `Lot::UpdateObjectSceneNode` 0x00ABFAC0 live. Its tooltip says "Not saved", but the value is written and read back |
| Time the Mutex::Lock hook | `time_mutex_lock` | bool | off | | Attaches the hand-made hook on `Mutex::Lock` 0x004E16F0 live; off leaves "Mutex wait" empty |
| Per-hook registry timing | `registry_timing` | bool | off | | Times each module's draw and state callbacks by name. The toggle itself does not request a save; the value is written with the next save |

Buttons: **Clear** (forgets frames, hitches and session tables; the file keeps what was written) and **Save report now**
(appends a full report), shown while on or once data exists.

**Measurement presets** (*Measurement setup*), for the 60-second measurement protocol:

- **Timing run** (run type A): hitch multiplier 2.0, floor 8 ms, count state calls on, write the file on, sampling off,
  Mutex::Lock not timed, per-hook registry timing and lot object building off; turns the profiler on.
- **Sampling run** (run type B): the same plus sampling of the render and simulation threads at 2000 Hz.

Both save the settings and log `[FrameProfiler] Measurement preset: ...`. Neither presses Clear: the protocol presses
Clear right before the run and Save report now right after it.

Developer-mode persistence and profile import rules are in [developer mode](developer-mode.md): an imported profile
restores these preferences but never starts a measurement.

## Compatibility and interactions

- **Performance switches.** The profiler shares hook sites with the
  [Performance](performance/README.md) features through the layered chains: `SlotChain` (FindProvider, RefPack write,
  wall AO step, key lists), `EntryChain` (DXT encoders, object lookup by ID) and `CallChain` (scene pending-node drain).
  The profiler is the outer layer, except at the wall AO step, where the wall shading gate must stay outermost. The
  performance features' status lines are added to the report.
- **D3D9 registry.** The profiler's hooks use priorities -1000 and +1000 so they bracket every other module's hooks
  (`Priority` is an int enum sorted by value). Any new frame-boundary hook must stay inside that range.
- **Sims3SettingsSetter beside Apex Radiance.** Its hooks are separate from Apex's registry, so per-hook registry timing
  sees only Apex hooks; its code shows as "other ASI" in samples. Its Smooth Patch frame limiter (site 0x00EC9FBA, inside
  the render-frame body) is measured as "Frame limiter". Its Chunky Patch "Disable GC_try_to_collect" NOPs the GC call:
  "Script GC" then stays empty and the simulation thread is never identified, so simulation time lands in "other threads"
  and simulation sampling cannot start.
- **Entries other modules verify** are not hooked: the room lightmap solve 0x006A3EC0 (timed through its only caller
  0x006A80E0), `Lot::AddLotObjectsToScene` 0x00AC1130 (the Lot Streaming throttle writes a JMP there),
  `WorldManager::Update` 0x00C6D570 (LotStreamingOptimizations' map-view blocker), `Lot::UpdateObjectSceneNode`
  0x00ABFAC0 by default (LotStreamingOptimizations finds it by an entry pattern; while it is timed, that module cannot
  re-install and its object throttle stays off), and the entries of 0x00AEA680 / 0x00C845C0 (timed at their call sites
  instead).
- **Camera motion** comes from the lot LOD scoring point (0x00C6C290) and, when that function did not run, from
  PostScene's view-projection, which is captured only while an effect using PostScene is on.

## Limitations

- Steam 1.67.2 is the verified build; other builds rely on unique pattern scans, and service names are Steam-only.
- The profiler's own overhead is part of what it measures: "Present hooks (mod)" includes its bookkeeping, every draw pays
  one extra registry hook and every outermost registry dispatch a push and pop; every Mutex::Lock and Semaphore::Wait on
  every thread pays two clock reads.
- Other threads' time is attributed to the frame in which the call returns (a 100 ms job on a worker lands in one frame).
- Threads beyond 128 that run timed code are not timed; call stacks deeper than 32 timed levels drop the deeper calls.
- Keyed tables are fixed-size open addressing (per frame: 64 services, 256 jobs, 128 waits; session: 256 / 1024 / 512)
  with a 90% fill limit; overflow is counted but not displayed.
- The duration of CreateTexture, shader creation and anything the registry does not expose cannot be measured.
- Sampling: the stack walk is heuristic and `fn~` keys are guesses; a sampled thread is paused about 5 to 30 us per
  sample under WOW64, roughly 1 to 6% of that thread at 2000 Hz, plus similar CPU on the sampler's core.
- Services on other threads are session totals only.
- The writer drops hitches when more than 256 are queued within one writer period.
- `ApexRadiance_Hitches.txt` grows without bound.
- Counters: "packages per lookup" reads the provider list without the manager's lock (a statistic); the two DXT encoders
  share one category; the scene pending nodes and the room solves are counted only from their per-frame CALLs; the
  longest call of the other threads mixes simulation and other threads. The async-load request counter (slot
  0x00FFD3BC) and cache-eviction counting are not implemented.
- Per-name timing covers the draw and the listed state chains (with the option) and Present (always); Create* and
  BeginScene callbacks have no per-name timing. These durations include nested callback work and device calls made by
  callbacks; they are not disjoint costs. Turn detailed timing off for paired performance runs.
- Reading the output: "Services (self)" in the "incl. nested" line is the inclusive value of the service category
  (label reuse), not self time.

## Technical reference

### Enabling and disabling

`FrameProfiler::SetEnabled(true)`, under `g_ctrlMutex`:

1. `InitClock` once (`std::call_once`): when CPUID 0x80000007 EDX bit 8 (invariant TSC) is set, RDTSC is calibrated
   against QPC by a 20 ms busy wait and used as the clock (`g_useTsc`, never changed later); else QPC.
   `g_blockTicks` = 0.1 ms. `RefineClock` (every 128 frames on the render thread) recomputes ms per tick from the long
   QPC / TSC baseline.
2. `g_needBaseline = true`, `StartWriter()`, `g_attachPending = true`.
3. `RegisterD3DHooks()` registers the registry hooks under the name `"FrameProfiler"`: Present at -1000 (start) and
   +1000 (end); DrawIndexedPrimitive / DrawPrimitive counting hooks at -1000; CreateTexture / CreateRenderTarget /
   CreateVertexShader / CreatePixelShader counters at -1000.
4. `UpdateSamplerLocked()` starts the sampler thread when a sampling option is on.
5. The game functions are attached at the next frame boundary in `OnPresentStart` -> `AttachAllLocked`, with `try_lock`
   on `g_ctrlMutex` (SetEnabled holds it while it may wait for the registry mutex the Present hook runs under).
   `g_summary` becomes "Timing N of M game functions", logged `[FrameProfiler] Timing N of M game functions`.

Disabling (`SetEnabled(false)`, also `Shutdown()`): stop the sampler, `UnregisterAll("FrameProfiler")`, `DetachTarget`
for every target, `StopWriter()`. The collected statistics stay visible until Clear.

`FrameProfiler::Shutdown()` runs in `DLL_PROCESS_DETACH` only on FreeLibrary (`lpReserved == NULL`), after the shader
cache shutdown and before `AddressSpace::Stop`, `PatchManager::UninstallAll` and `ApexD3D::Shutdown`. On process exit
nothing is stopped; hitches queued in the last writer period can be lost (inferred).

### Target resolution and attach

`kTargets[]` has 40 entries (table below). On Steam the pattern must match exactly at the Steam address; on other builds
it must match exactly once in TS3W's `.text` (`ScanUnique`, first result cached). A target whose bytes do not match is
skipped with a status string, listed in Advanced > Hooks and logged as a warning.

| Method | Used for | How |
|---|---|---|
| Detours on the entry | render / present path, lot LOD and lighting, GC, scene, app state, clock, impostor pump | `DetourAttach` with `DetourUpdateThread(GetCurrentThread())` only; entry bytes re-checked by `MatchAt` right before |
| Call site | 0x00AEB306 (-> 0x00AEA680), 0x00C6D68F (-> 0x00C845C0) | 5-byte `E8 rel32` rewrite (tracked, restored byte for byte); on Steam the CALL must reach the expected callee, else skipped "redirected by another module?" |
| Hand-made hook (`safeLen > 0`) | functions many threads call: service loops, ExecuteJob, WaitForJob, Mutex::Lock, Semaphore::Wait, FileStream::Read / Flush, RefPack read | `AttachSafe`: builds a trampoline (verified relocation-free prologue of `safeLen` bytes plus `JMP` back; 4 KB RWX pool, 32-byte slots, never freed), suspends all other threads, checks no thread's EIP is strictly inside the replaced prologue, writes the 8-byte patch with one `_InterlockedCompareExchange64` (entry 8-byte aligned), resumes; up to 100 retries with `Sleep(1)` |
| Vtable slots | FindProvider, RefPack write, wall AO step, key lists | `SlotChain::Install / Remove` (layer FrameProfiler) for the shared slots (`kSharedSlots`); `AttachSlots` otherwise requires every slot to hold the function and swaps it with `_InterlockedCompareExchange`; `DetachSlots` restores only a slot that still holds the hook |
| Entry chain | DXT1 / DXT5 encoders, object lookup by ID | `EntryChain::Install / Remove` (layer FrameProfiler) before the `safeLen` branch; `ResolveTarget` skips its pattern check (the entry may already hold another layer's JMP; the chain checks the prologue) |
| Call chain / suspended CALL | scene pending nodes (`CallChain`, site SceneDrain); room solve and texture loader CALLs (`WriteCallSuspended`) | CALL written with every other thread suspended (`MemPatch::WriteCodeSuspended` or the same method as `AttachSafe`) |

Detours is not used for functions many threads call: it only fixes up threads passed to `DetourUpdateThread`, and
`DetourUpdateThread` / the commit allocate from the heap, which can deadlock once other threads are suspended. `DetachSafe`
restores with one locked 8-byte write; if the entry no longer holds the profiler's JMP, it is left alone and the hook
keeps forwarding through the trampoline. Call-site detach likewise leaves a CALL that another module rewrote.

Hooks are `__fastcall(ecx, edx, stack args...)` returning `uint64_t`: ABI-identical to the originals' `__thiscall` with
callee cleanup, ECX / EDX passed through, EDX:EAX preserved, float arguments passed as raw dwords.

Targets that take their address from the game-address table ([`framework/game_addresses.h`](../../framework/game_addresses.h))
wait for the scan (first Present + 1 s, `GameAddr::Scanned()`) on every build, so the scan's self-check never sees the
profiler's hooks; they show "Waiting for the game-address scan" and are attached at the first frame boundary after it
(`AttachWaitingLocked`). The summary then reads "Timing N of M game functions (K waiting for the game-address scan)".
Mutex::Lock and lot object building are attached only with their options; while off they are not counted in M.

### Timed game functions (Steam 1.67.2)

| # | Name in UI | Address | Convention | Attach | Category | Expected thread |
|---|---|---|---|---|---|---|
| 0 | Render frame | 0x00EC9F00 | thiscall(1), ret 4 (ECX set by the caller at 0x00ECAAF6 and passed through) | Detours | Render frame (game) and Frame limiter carve | render |
| 1 | End frame + Present | 0x00611680 | thiscall(3), ret 0xC | Detours | EndScene + overlays, then Present (driver) | render |
| 2 | Lot LOD scoring | 0x00C6C290 | thiscall(4), ret 0x10 | Detours | Lot LOD scoring; argument 2 = camera point | render (WorldManager::Update) |
| 3 | Lot detail request | 0x00AC20E0 | thiscall(1), ret 4 | Detours | Lot detail request; counts promotions and demotions | render |
| 4 | Lot renderer update | 0x00AEB2E0 | thiscall(1), ret 4 | Detours | Lot renderer update | render |
| 5 | Lot load stages | CALL 0x00AEB306 -> 0x00AEA680 | thiscall(0) | call site (pattern at 0x00AEB2F8, CALL at +14) | Lot load stages | render |
| 6 | Lot LOD switch | 0x00AD9E30 | thiscall(1), ret 4 | Detours | Lot LOD switch | lot impostor builder |
| 7 | Lot lighting setup | 0x00ADBAD0 | thiscall(0) | Detours (47-byte pattern) | Lot lighting setup | lot impostor builder |
| 8 | Room lighting + solve | 0x006A80E0 | thiscall(0) | Detours | Room lighting + solve | lot impostor builder |
| 9 | Lot lighting update | 0x00ADB8F0 | thiscall(0) | Detours | Lot lighting update | render |
| 10 | Terrain update | CALL 0x00C6D68F -> 0x00C845C0 | thiscall(2), ret 8 | call site (pattern at 0x00C6D682, CALL at +13) | Terrain update | render |
| 11 | GC_try_to_collect | 0x00E4A050 | cdecl(1) | Detours | Script GC; its caller becomes the simulation thread | simulation |
| 12 | Lot::UpdateObjectSceneNode | 0x00ABFAC0 | thiscall(3), ret 0xC | Detours, optional | Lot object scene nodes | AddLotObjectsToScene's thread |
| 13 | ServiceManager main loop | 0x0059ED20 | thiscall(float, float), ret 8 | hand-made, safeLen 6, body replaced | Services (self) | render |
| 14 | ServiceManager sim loop | 0x0059ED70 | same | hand-made, safeLen 6, body replaced | Services (self) | simulation |
| 15 | JobManager::ExecuteJob | 0x00599720 | thiscall(job), ret 4 | hand-made, safeLen 7 | Jobs (self) | any (keyed on render) |
| 16 | JobManager::WaitForJob | 0x0059A220 | thiscall(job), ret 4 | hand-made, safeLen 6 | Wait for job | render (others pass through) |
| 17 | Mutex::Lock | 0x004E16F0 | thiscall(timeout*), ret 4 | hand-made, safeLen 5, optional | Mutex wait (> 0.1 ms, render) | any |
| 18 | Semaphore::Wait | 0x004E2760 | thiscall(timeout*), ret 4 | hand-made, safeLen 5 | Semaphore wait (> 0.1 ms, render) | any |
| 19 | FileStream::Read | 0x004DB850 | thiscall(buf, size), ret 8 | hand-made, safeLen 6 | File read (and bytes) | any (timed on render) |
| 20 | FileStream::Flush | 0x004DB8E0 | thiscall(0) | hand-made, safeLen 6 | File flush | any (timed on render) |
| 21 | RefPack stream read | 0x004EC010 | stdcall(5), ret 0x14 | hand-made, safeLen 5 | RefPack read | any (timed on render) |
| 22 | Scene::BeginFrame | 0x006EBB70 | thiscall(0) | Detours | Scene::BeginFrame | render |
| 23 | Scene::EndFrame | 0x006E8810 | thiscall(0) | Detours | Scene::EndFrame | render |
| 24 | SceneCaptureManager | 0x009DE140 | thiscall(2), ret 8 | Detours | Scene capture | render |
| 25 | App state | 0x00EC6C30 | thiscall(0) | Detours | App state update | render |
| 26 | Game clock tick | 0x005943F0 | thiscall(0) | Detours | Game clock tick | render |
| 27 | Lot impostor pump | 0x00AD97E0 | thiscall(job), ret 4 | Detours | Lot impostor pump | render (unverified, see validation) |
| 28 | Resource lookup | 0x004AFFC0 | thiscall(2), ret 8 | vtable slots 0x00FB2DE0 / 0x00FFE290, `SlotChain` outer layer | Resource lookup (counter) | any |
| 29 | Scene pending nodes | CALL 0x006EBC49 -> 0x006E4130 | thiscall(0) | `CallChain` layer 0 (the scene node budget is layer 1) | Scene pending nodes (counter) | render |
| 30 | RefPack compress | 0x004EC200 | thiscall(5), ret 0x14 | vtable slot 0x00FB901C, `SlotChain` outer layer | RefPack compress (counter) | any |
| 31 | DXT1 encode | 0x006152F0 | cdecl(2) | `EntryChain` layer 0 (the fast encoder is layer 1) | DXT encode (counter) | any |
| 32 | DXT5 encode | 0x006154B0 | cdecl(2) | `EntryChain` layer 0 | DXT encode (counter) | any |
| 33 | Object lookup by ID | 0x00C62D40 | thiscall(3), ret 0xC | `EntryChain` layer 0 (the object lookup index is layer 3) | Object lookup by ID (counter) | any |
| 34 | Lot room solve | CALL 0x00ADB9AD -> 0x006A8BA0 | thiscall(2), ret 8 | call site, all threads checked | Lot room solve (counter) | render |
| 35 | Wall AO pass | 0x0068B810 | thiscall(2), ret 8 | vtable slot 0x00FF05B0, `SlotChain` layer 1 inside the wall shading gate | Wall AO pass (counter) | render |
| 36 | Key list | 0x004B1AE0 | thiscall(3), ret 0xC | vtable slot 0x00FB2DC0, `SlotChain` outer layer (the file list cache is inside) | Key list (counter) | any |
| 37 | Key list, ResourceSystem | 0x00736660 | thiscall(3), ret 0xC | vtable slot 0x00FFE270, same | Key list (counter) | any |
| 38 | Texture create | CALL 0x0060E1DC -> 0x0060CEA0 | cdecl(9) | call site, all threads checked | Texture create (counter) | texture load finalize job |
| 39 | Texture fill | CALL 0x0060E1FF -> 0x0060D290 | cdecl(4) | call site, all threads checked | Texture fill (counter) | texture load finalize job |

Full byte patterns are in `kTargets` and are the ground truth; the header comment of `frame_profiler.cpp` lists the first
bytes, callers and the per-target reasoning. Every pattern was checked against `re\TS3W.exe`: unique in `.text`,
matching at the Steam address, and for Detours targets the relocated first instructions were decoded by hand with no
branch in `.text` landing inside them. The two service-loop patterns are the whole 0x44-byte bodies
(0x0059ED20..0x0059ED63, verified in `engine_map\full.asm`).

### Attribution model

- Each thread that runs timed code gets a `ThreadSlot` (at most 128, `kMaxSlots`) with a 32-deep call stack
  (`Frame{start, child, sp, cat, flags}`) and per-category `excl` / `incl` / `calls` counters written only by the owner
  (relaxed load and store, no locked operation).
- `Push` / `Pop`: on return, the call's inclusive time is added to the parent's `child`. **Self** = inclusive minus timed
  children; self times never overlap, so on the render thread they sum to at most the frame time and the rest is
  **Unattributed**. **Inclusive** is counted only for the outermost open call of a category (`kOuter` via `open[cat]`).
- Frame identity is a stack address (`volatile char marker` or `this` of a `Scope`), used to match pops and drop stale
  frames (`CleanStale`, by stack depth).
- At each frame boundary `SnapshotThreads` reads every slot's counters as deltas against `g_seen[]` and buckets them:
  **render** (the thread calling Present), **simulation** (the thread that called `GC_try_to_collect`), **other**.
  `Split` books the time open render-thread calls have spent so far and restarts them at `now`.
- Special splits: 0x00611680 is pushed with `kSwitchAtSplit`; it counts as "EndScene + overlays" until the Present
  boundary and as "Present (driver)" after it. In `Hook_RenderFrame`, the time after 0x00611680 returned is "Frame
  limiter". One frame interval = previous Present's wait + limiter + clock tick + app state + services + capture +
  BeginFrame + this frame's render + EndScene.
- Per frame: `cpuMs = frame - Present (driver) - Frame limiter`; `modMs = D3D hooks (mod) + Present hooks (mod) + Lamp
  refresh (mod)`; `unattributedMs = frame - sum(render self)`.
- 43 categories (`kCats`), of which the last 10 are counters.

### ServiceManager loop replacement

The originals of 0x0059ED20 / 0x0059ED70 never run while attached. `RunServices` walks the same list: when
`[list+0xC] != 0`, for each node from `[list]` until the head: if `byte[node+0x1A] != 0` and `byte[node+0x14] & flag`
(1 main, 2 sim), service = `[node+8]`, call `vtable[+0x1C]` (main) / `[+0x20]` (sim) as `thiscall(dt, realDt)`, ret 8;
the next node is read after the call, as the original does. Each call is a `kService` frame; on the render thread it goes
to the per-frame table keyed by the update function address (aux = vtable), on other threads to `g_otherSvc` (64 atomic
slots, session totals). Because the key is read from the vtable, services detoured by code patches keep their names.

| Update fn | Name | | Update fn | Name |
|---|---|---|---|---|
| 0x00588890 | MessageServer | | 0x00733D20 | ResourceChangeMonitor |
| 0x00598660 | Input (message pump) | | 0x00733D30 | ResourceChangeMonitor (sim) |
| 0x00599A10 | JobManager (main-thread jobs) | | 0x007377F0 | ResourceSystem |
| 0x005F0E50 | CAS SimService | | 0x007A08C0 | ShaderSystem |
| 0x00608630 | CAS TextureCompositor | | 0x00A37E60 | Crossroads (AccountManager) |
| 0x006E3620 | Scene service (SceneObjectManager) | | 0x00B3A960 | ObjectDesigner |
| 0x0071E640 | Swarm (VFX) | | 0x00C7E3C0 / 0x00C7E300 | WorldManager / WorldManager (sim) |

Other services print as `service XXXXXXXX (vtable YYYYYYYY)`.

### Jobs, waits, I/O

- `Hook_ExecuteJob` (every thread; keyed only on the render thread): key = `[job+0x10]` (the job function), read before
  the call. Remote calls (job function 0x007D9840, entry bytes `83 7C 24 0C 04 56 57 75` checked by
  `ResolveRemoteCallKeysLocked`) are keyed by the method: vtable +0x10 of `[job+0x14]`, or `[obj+0x10]` when the
  object's vtable is one of the PostRemoteMethodCall vtables 0x010650C4 (stored at 0x00ABEA0A) or 0x010650D8 (stored at
  0x00ABEA93, a method with one byte argument). They print as `remote call -> XXXXXXXX` (for example 0x00AC1130
  AddLotObjectsToScene). SEH-guarded; an unreadable or null function prints as `job 00000001`.
- `Hook_WaitForJob`: render thread only; keyed by the job waited for.
- `Hook_MutexLock` / `Hook_SemaphoreWait`: every thread pays two clock reads; render-thread calls longer than 0.1 ms are
  booked by `RecordBlocked` as a leaf under the running timed call, keyed by the caller's return address; a semaphore
  wait directly inside WaitForJob is not counted twice. The resource lookup takes the mutex about 580 times per full
  scan, which is why the Mutex::Lock hook is optional.
- File read, flush and RefPack read: render thread only; bytes read are summed.

### Counters

Ten categories at the end of `kCats` (`kFirstCounterCat` = `kResLookup`, `kNC` = 10), timed on every thread with the same
attribution machinery. For each: calls and inclusive ms per bucket per frame, the longest single call (render / other
threads; `g_cMax`, atomic maximum taken at the frame boundary) and an extra count (`ThreadSlot::extra[]`). Addresses come
from the game-address table (fixed on Steam 1.67.2, signature elsewhere); the profiler checks the bytes before hooking.
Conventions verified in `research\engine_map\full.asm`:

| Counter | Function | Convention | Attach | Extra count |
|---|---|---|---|---|
| Resource lookup | `ResourceMgr::FindProvider` 0x004AFFC0 | thiscall(key*, cookie*), ret 8; returns the provider or 0; the cookie is the provider's priority | vtable slots 0x00FB2DE0 (base vtable 0x00FB2DA0 +0x40) and 0x00FFE290 (derived 0x00FFE250 +0x40), the only references; `SlotChain` outer layer, the resource lookup cache inside; `Hook_FindProvider` calls `SlotChain::Next` and reads `ResourceCache::TakeLookupNote()` | packages probed (index of the returned provider in `[this+0x30, this+0x34)` + 1, all on a miss; for a cached answer, the packages the cache asked), misses, from cache (`kXCacheHits`), absent from cache (`kXCacheAbsent`, Remember Missing Files) |
| Scene pending nodes | 0x006E4130 | thiscall(), ret; zeroes `[this+0x18]` and adds 1 per node | call site 0x006EBC49 in `Scene::BeginFrame` (the other 5 callers are not per frame); `CallChain` layer 0, Spread New Objects Over Frames layer 1 | nodes = `[this+0x18]` after the call; deferred (`kXDeferred`, `SceneBudget::TakeDrainNote`) |
| RefPack compress | RefPack stream write 0x004EC200 | thiscall(src, size, dst, capacity, flags), ret 0x14; `dst == 0 && flags & 1` = size bound only (not timed); `dst == 0` otherwise = a counting run (timed) | vtable slot 0x00FB901C (stream vtable 0x00FB9018 +4), its only reference; `SlotChain` outer layer, Faster Cache Compression inside | bytes in, bytes out |
| DXT encode | DXT1 0x006152F0, DXT5 0x006154B0 | cdecl(dst*, src*); dst = {pixels, width +4, height +8, pitch +0xC} | `EntryChain`: prologue `55 8B EC 83 E4 F0` moved to a trampoline, JMP written with every other thread suspended; profiler layer 0, Faster Texture Compression layer 1 | pixels |
| Object lookup by ID | 0x00C62D40 | thiscall(idLo, idHi, int* visited), ret 0xC; every caller passes visited = 0 | 233 callers; `EntryChain` site ObjectById (8-byte prologue `8B 44 24 0C 8B 54 24 08`); profiler layer 0, Faster Object Lookups layer 3 | from index (`kXIndexHits`, `ObjectIndex::TakeLookupNote`) |
| Lot room solve | 0x006A8BA0 | thiscall(timer*, float budget), ret 8; x87 stack empty at the call and on return; ecx = one level object of the lot | call site 0x00ADB9AD in the lot lighting update 0x00ADB8F0, written with every other thread suspended | calls = lot levels updated; the hitch line adds the lot lighting update's inclusive ms. With Lot Lighting While Moving on, the budget argument is the scaled one |
| Wall AO pass | 0x0068B810 | thiscall(stopwatch*, float budget), ret 8; one call = one pass over every outdoor wall of a lot level | vtable slot 0x00FF05B0 (solver vtable 0x00FF0594 +0x1C), `SlotChain` site WallAoStep layer 1, inside the wall shading gate (layer 0); with the gate on only the passes that run are timed | none (calls = passes) |
| Key list | 0x004B1AE0 and 0x00736660 | thiscall(vector* out, filter*, bool unique), ret 0xC; returns the count | vtable slots 0x00FB2DC0 and 0x00FFE270, `SlotChain` sites KeyListBase / KeyListDerived, outer layer, the file list cache inside (the derived function calls the base directly: not counted twice) | keys (growth of `out`), packages from cache (`kXListCached`) |
| Texture create | 0x0060CEA0 via CALL 0x0060E1DC | cdecl(9) | call site in the DDS texture loader (texture load finalize job 0x007297C0), all threads checked | level-0 pixels |
| Texture fill | 0x0060D290 via CALL 0x0060E1FF | cdecl(4) | call site, all threads checked | none |

- No branch in `.text` lands inside a replaced prologue or CALL (checked in `full.asm`). All entries are 8-byte aligned.
- The hitch lines add "Scene pending nodes ..., nodes N[, deferred D]" and "Object lookup by ID ...[, from index H]"
  (added parts after a comma, so `agg.pl` still splits counters on "; "); the per-frame Counters table shows "x nodes,
  y deferred" and "z% from index".
- Nesting: lookups mostly run inside the pending-node drain (materials), jobs (async-load finalize 0x007297C0) and
  services (CAS); object lookups inside room solves. Self times move from those parents to the counters, so the dominant
  cause names the counter when it is the real cost.
- Overhead: two clock reads and a TLS read per call; the lookup adds the probe count (a scan of up to about 290 8-byte
  entries on a hit, about 0.1 to 0.3 us against a lookup of about 50 us; read without the manager's lock, SEH-guarded).

**Dominant cause** (`ComputeDominant`, hitch frames): the largest single item on the render thread, as `dom.pl` picks it:
every render self category except "Services (self)" / "Jobs (self)", each service's and each job's self time of the frame,
and Unattributed. Also the **top counter**: the counter with the largest render-thread self time (at least 0.05 ms), or
none.

### D3D9 counters and mod hook time

- `OnDrawStart` (DrawIndexedPrimitive / DrawPrimitive start hook): counts draws; a draw while `open[kEndScene] > 0` is an
  "end-of-frame draw" (overlays and post passes inside EndScene), else a game draw (primitives summed).
- **D3D hooks (mod):** the registry ([`framework/d3d9_hooks.cpp`](../../framework/d3d9_hooks.cpp) `Run`) books its
  outermost dispatch on a thread, for every chain except Present, with `BeginModTime(ModTime::D3DDispatch)` /
  `EndModTime` around the whole chain, also when a callback returns Skip or Block. Nested dispatches (a draw Night
  Lighting replaces is re-issued inside its own dispatch) are part of the outer one, so its driver call is counted too.
  The time includes the bookkeeping (two clock reads and a push / pop per outermost dispatch, about 20 to 40 ns, also for
  every SetPixelShader / SetVertexShader of the game), so expect a few tenths of a ms of measurement overhead per frame.
- **Present hooks (mod):** the -1000 Present hook is the frame boundary and pushes a `kPresentHooks` frame; the +1000
  hook pops it. It includes every module's Present hooks and the profiler's own per-frame bookkeeping.
- **Lamp refresh (mod):** Night Lighting's 20-frame lamp list refresh (`LotLightBridge::OnPresent`,
  `ModTimeScope(ModTime::LampRefresh)`), a category of its own inside the Present hooks.
- **State-call counts:** SetTexture, Set{Vertex,Pixel}Shader, Set{Vertex,Pixel}ShaderConstantF and SetRenderTarget are
  counted in the registry's detours with plain per-method counters (`D3D9Hooks::ReadStateCallCounts`, monotonic;
  `FrameBoundary` takes the difference). Calls Apex makes with `CallOriginal*` bypass the detours and are not counted.
- **Off-thread dispatches:** the draw and state chains run without a lock on the render thread; a call from another thread
  takes the registry lock and is counted (`D3D9Hooks::OffThreadDispatches()`; Advanced line "Draw / state hooks called
  from another thread than the render thread", report line "Draw / state hooks called off the render thread"). A
  non-zero value means the draw counts may race.
- The registry calls hooks only before the device method, so resource-creation duration is not measurable (counts
  only). Not exposed by the registry: CreateVertexBuffer / CreateIndexBuffer, Lock / Unlock, SetRenderState;
  DrawPrimitiveUP / DrawIndexedPrimitiveUP exist only through ExtraHooks' single observer slot, owned by Frame Capture.

### Per-hook registry timing

`RunList` in `framework/d3d9_hooks.cpp` times each callback by name when its chain's timing mode allows it:

| Chains | Timing |
|---|---|
| Present | Always while the profiler is on (`PresentHookTimingActive()`), reported as `"<name> (Present)"` |
| DrawIndexedPrimitive, DrawPrimitive, SetRenderTarget, SetPixelShader, SetVertexShader, SetTexture, SetViewport, SetPixelShaderConstantF, SetVertexShaderConstantF | With *Per-hook registry timing* (`RegistryHookTimingActive()`); state entries carry a method suffix, such as `LotLightBridge (SetPixelShader)` |
| Create*, BeginScene | Not timed per name |

`AddRegistryHookTime` keys by the name string's address (fast path) verified by content, at most 64 names and 256 pointer
cache entries, serialised by the registry's timing mutex. `RollRegistryWindow` (render thread) publishes ms per frame and
calls per frame per name once a second ("Registry hooks by name" in Advanced, shown while the option is on or any name has
data); *Save report now* adds it. The *Per-hook registry timing* tooltip still mentions only the draw hooks.

### Hitch detection

- Frame time = time between two Present boundaries. Median of the last 120 frames (`kMedianWindow`, `nth_element`);
  threshold = `max(hitch_multiplier x median, hitch_floor_ms)`, computed before the current frame enters the window.
- Warm-up: no hitch until 30 frames are in the window (`kWarmupFrames`); the window resets at every enable.
- Histogram: 2000 bins of 0.05 ms up to 100 ms, 900 bins of 1 ms up to 1000 ms, one overflow bin; percentiles are bin
  centres (the overflow bin reads 1000.0); "max" is exact.
- Camera signal (`SampleCamera`): moved when the point passed to 0x00C6C290 changed by more than 1e-3 on any axis; when
  that function did not run (map view, loading), PostScene's view-projection is compared instead; `camera -1` = unknown.
- Per hitch: foreground check (`GetForegroundWindow` pid; else "(window in background)"), top 8 services, 6 jobs, 6
  waits, file bytes, the sampler summary, page faults; stored in a 200-entry ring and pushed to the writer queue (256
  entries, single producer / single consumer; drops counted).

### Statistical sampler

`SamplerProc` thread, `THREAD_PRIORITY_HIGHEST`, waits on a high-resolution waitable timer
(`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, fallback plain timer or `Sleep(1)`), period `1/hz` relative per iteration (the
achieved rate is below target; the UI shows the measured rate). For each wanted thread: `SuspendThread`,
`GetThreadContext(CONTEXT_CONTROL)`, copy up to 4 KB from ESP (bounded by the stack region found with `VirtualQuery`,
SEH-guarded), `ResumeThread`. Nothing between suspend and resume allocates, logs or locks. After resuming, dwords of the
copy that point into TS3W's `.text` right after a CALL (`IsCallSite`) are kept as candidate return addresses (up to 12;
a frame-pointer-free heuristic, so deep entries are hints). Samples go to an 8192-entry ring with the TSC timestamp; at
each frame boundary the render thread assigns those in `[intervalStart, now)` to the frame (`ConsumeSamples`).

- For a sample whose EIP is in system code, the sampler also keeps the first return address after a CALL in any
  non-system module (`FirstOutsideSystem`); report table "system code: first caller outside system code", printed
  `TS3W XXXXXXXX` or `module+RVA`.
- The exact-EIP table names system-code EIPs by the nearest export (`ntdll.dll NtWaitForAlertByThreadId+0xC`).
- Page faults of the process per frame (`GetProcessMemoryInfo`): per hitch ("page faults N" on the lots line) and the
  session average for hitch and other frames.
- Texture loads by size class (loads, create / fill ms total, average, maximum, MB) from the profiler's CreateTexture
  callback, which notes the size on the creating thread.

Symbolisation:

- Module classes (`ClassifyModule`): **TS3W** (main module), **Apex** (this ASI), **other ASI** (`*.asi`),
  **DXVK/driver** (`d3d9.dll`, `dxgi.dll`, `d3d11.dll`, `vulkan-1.dll`, names starting `nv`, `amd`, `ati`, `igvk`, `igd`),
  **system** (`ntdll`, `kernelbase`, `kernel32`, `win32u`, `user32`, `gdi32`, `gdi32full`, `wow64*`), **other**. Module
  table: up to 384 entries, append-only, refreshed at most once a second when an unknown address appears.
- TS3W code key = `FnStartGuess(eip)`: walk back in 16-byte steps (at most 4096) to the first aligned address preceded by
  `int3` (0xCC). On the 4126 functions of the RE dump, 94% start that way, 5.5% follow a RET without padding (merged with
  the previous function) and 0.1% of aligned positions inside bodies follow a CC (split). Printed `TS3W fn~XXXXXXXX`: a
  grouping key, to be resolved with the decompile. A function that follows a jump table without padding is booked to the
  function before it; confirm with the stack's return addresses.
- Apex code is keyed the same way inside its own `.text` (`OwnFnStartGuess`) and printed as an RVA
  (`apexradiance.asi fn~+1A2B0`; `apexradiance.asi+1A2B3` in the exact-EIP list). The Release configuration writes
  `ApexRadiance.map` (`GenerateMapFile`); RVA = map address - preferred base. The module base, size and PE TimeDateStamp
  are logged when the sampler starts and printed at the top of the report's sampling section, so a report can be matched
  with the map of the same build. The int3-padding guess is unverified for this project's MSVC output.
- Per hitch: share of samples per class, top 8 code keys per thread, top 8 TS3W call sites on the render stack, top 4
  "system code called from". Session tables (other frames vs hitch frames): 4096-entry count tables, plus exact EIPs in
  hitch frames.

### Address space

[`features/address_space.{h,cpp}`](../../features/address_space.cpp), started by the init thread in developer mode
whether the profiler is on or not: a below-normal thread (64 KB stack) walks the address space with `VirtualQuery` every
10 s (about 1 to 3 ms, off the game's threads). It keeps the free total and largest free block (below and above 2 GB),
the five largest free blocks, committed and reserved-only memory by kind (images, mapped views, private; committed
private `PAGE_EXECUTE_READWRITE` = the script GC heap), private allocations by size class, the 8 largest images and the
game allocator's own big blocks (`[[0x011CB864]+0x488 / +0x48C]`), and the snapshot with the session's smallest
largest-free-block. One `[AddressSpace]` log line a minute; both snapshots are in the profiler report. The number to
watch is the largest free block: big allocations, the save's Error 12, DXVK's texture views and the script heap's 8 MB
growth steps need one contiguous piece; below about 64 MB is the danger zone.

### Output file

`ApexRadiance_Hitches.txt` in `Documents\Electronic Arts\The Sims 3\Apex Radiance\`. Writer: a raw Win32 thread (a global
joinable `std::thread` would `std::terminate` at exit), appending in binary mode, woken every 1000 ms or on
`SetEvent(g_wake)`; nothing is written or flushed on the render thread. Content, in order:

1. Session header: `==== Frame profiler session <time> | game <version> | clock <RDTSC (invariant, X GHz) |
   QueryPerformanceCounter> | hitch = frame > max(M x median of the last 120 frames, F ms) ====` and a legend line.
2. "Timed functions:" and the hook status table.
3. One block per hitch (`FormatHitch`): `#frame t=..s frame X ms (median, threshold) cpu present limiter mod D3D camera`,
   then `render thread (self ms)` sorted with `Unattributed`, `render thread (incl. nested ms)` (only categories whose
   inclusive exceeds self by more than 0.05 ms), `simulation thread (self ms)`, `other threads (self ms)`, `calls:`,
   `d3d:` counts, `lots promoted/demoted`, the counters line, the `dominant:` line, `services (render thread, ms incl /
   self)`, `jobs run on the render thread`, `render-thread waits`, `file reads`, and the per-thread sample summaries when
   sampling.
4. *Save report now* (`BuildReport`): percentiles, last-60-frame averages, camera hit rates, hook status, GC call-site
   state, limiter state, totals since Clear per category and bucket, per-hitch averages over the last 200 hitches, all
   200 hitches, keyed session tables (services / jobs / waits: ms per hitch vs per other frame), sim-loop services, the
   sampling tables, "Counters since Clear", "Counters per hitch", "Apex shaders: ..." (the
   [shader precompile](../architecture.md#44-shader-precompile) status), "Registry hooks by name (last second; ...)", "Draw /
   state hooks called off the render thread", the address-space snapshots, the performance features' status lines
   ([Performance](performance/README.md)) and "Dominant cause of the last hitches" (per camera state and frame-time
   bucket < 16 / 16-25 / 25-50 / >= 50 ms, foreground only). With the profiler off the report is written by a detached
   one-shot thread.

Counters line format (illustrative values):

```
   counters (calls x ms incl. on the render / simulation / other threads): Resource lookup 812 x 9.12 (max 0.31) / 12 x 0.20 / 400 x 3.10 (max 1.20), packages per lookup 245.3, misses 40; Scene pending nodes 1 x 14.13 (max 14.13) / 0 x 0.00 / 0 x 0.00, nodes 120; Lot room solve 3 x 12.10 (max 5.20) / 0 x 0.00 / 0 x 0.00, lot lighting update 12.40 ms incl.
   dominant: Scene pending nodes 5.01 ms (21% of the frame) | top counter: Resource lookup 9.12 ms self (9.12 ms incl.)
```

Only counters with calls in the frame are listed, separated by `; `; `(max ..)` is the longest single call after the
render group and after the other-threads group. Extras: RefPack compress `, in X KB, out Y KB`; DXT encode `, pixels X M`;
Resource lookup `, from cache N` and `, absent from cache N`; Key list `, keys N` and `, packages from cache N`. The
dominant key is a category name, `svc:<service>`, `job:<job name>` (`job:job 007297C0`, `job:remote call -> 00AC1130`)
or `Unattributed`; `top counter: none` when no counter reached 0.05 ms of render self time.

`GcCallSiteText` inspects 0x00D819AA (Steam, after `MatchAt(0x00D819A0, "68 ?? ?? ?? ?? A3")`; elsewhere a unique scan):
`90` = removed by Chunky Patch, `E8` to 0x00E4A050 = direct, `E8` elsewhere = redirected. `LimiterText` inspects
0x00EC9FBA: `80 BE 8D 00 00 00 00 5E 74 15` = game default (30 ms sleep only while inactive), `3E 56 E8 ?? ?? ?? ?? 5E EB` =
Smooth Patch limiter.

Analysis tools (`research\perf2\tools\`, outside this repository): `dom.pl [--since "YYYY-MM-DD HH:MM"] [--computed]
FILE` uses the `dominant:` line when present (else computes it; `--computed` forces that) and prints the top-counter
distribution per bucket; `agg.pl` prints per-hitch counter averages (`C:`) and dominant shares (`D:`); `cond2.pl` accepts
`dom:<dominant key>` and `job:remote call -> X`; `smp.pl` reads the sampling lines. New lines are only appended after
existing ones, so the tools read older and newer files alike.

### UI

The card *Frame times and stutters* (Developer > Performance) has the on switch in its header.

- *Collected measurement*: Clear and Save report now.
- *Measurement setup* (collapsed): the presets and the **Advanced** node: multiplier, floor, count state calls, write
  file, time lot object building, time the Mutex::Lock hook, per-hook registry timing; "Services, jobs and waits" (render
  services with a self column, jobs, waits, services on other threads); "Sampling" (checkboxes, rate, measured rate, us
  paused per sample, estimated share of a sampled thread, dropped, thread ids, per-thread class shares and three tables);
  "Hooks" (Function | Address | Calls R / S / O | Status, the summary, GC call site, frame limiter, clock, the
  not-measurable list, file written / dropped counts); "Totals since Clear"; "Registry hooks by name".
- Live line (averages of the last 60 frames: frame, FPS, CPU, Present, limiter; draws and end-of-frame draws, mod D3D
  ms; optional state-call counts); a plot of the last 300 frame times (scale top = clamp(1.5 x threshold, 33.4, 250));
  p50 / p95 / p99 / max since Clear; frames, hitches and their share; camera moving share and hitch rate moving vs still.
- *Collected timing details* (collapsed): "Last hitches" (Category | Render ms | Other threads ms | Worst ms | Calls, per
  hitch averages over the ring, Unattributed row, textures / shaders created and lots promoted per hitch; "Dominant
  causes"); "Recent hitches" (last 25 with the top 3 categories and the dominant cause); "Counters" (Counter | Render /
  frame | Simulation / frame | Other / frame | Per hitch | Longest call | Per frame; a warning line while Mutex::Lock is
  timed).

### Files and functions

| File | Function / symbol | Role |
|---|---|---|
| `features/frame_profiler.h` | public API | `SetEnabled`, `IsEnabled`, `RenderUI`, `SaveToToml`, `LoadFromToml`, `Shutdown`, `RegistryHookTimingActive`, `PresentHookTimingActive`, `Ticks`, `AddRegistryHookTime`, `BeginModTime` / `EndModTime`, `ModTimeScope` |
| `features/frame_profiler.cpp` | `kCats`, `kTargets`, `TargetId` | 43 categories with UI hints; 40 targets |
| | `InitClock`, `RefineClock`, `Now` | clock |
| | `ThreadSlot`, `GetSlot`, `Push`, `Pop`, `Split`, `CleanStale`, `Scope` | attribution |
| | `Hook_*`, `RunServices` | wrappers |
| | `ResolveTarget`, `AttachTarget`, `AttachCallSite`, `AttachSafe`, `DetachSafe`, `DetachTarget`, `BuildTrampoline`, `WriteQwordAtomic`, `OpenOtherThreads`, `AttachSlots`, `DetachSlots`, `SwapSlot`, `WriteCallSuspended`, `AttachWaitingLocked`, `UpdateSummaryLocked`, `ApplyMutexOptionLocked`, `ResolveRemoteCallKeysLocked` | attach and detach |
| | `TimeTable<N>`, `JobKey`, `ServiceName`, `RecordBlocked`, `AddOtherService` | keyed tables |
| | `SamplerProc`, `TakeSample`, `IsCallSite`, `ClassifyModule`, `RefreshModules`, `FnStartGuess`, `OwnFnStartGuess`, `FirstOutsideSystem`, `ConsumeSamples` | sampler |
| | `FrameBoundary`, `SnapshotThreads`, `SampleCamera`, `UpdateStats`, `Percentile`, `MedianOfWindow` | per-frame bookkeeping |
| | `OnPresentStart`, `OnDrawStart`, `RegisterD3DHooks` | registry hooks |
| | `CounterScope`, `NoteMax`, `AddX`, `PackagesProbed`, `CounterFrame`, `ComputeDominant`, `DomName`, `DominantText`, `FormatCounters`, `CounterExtraText`, `CounterReport`, `DominantSummaryLines`, `RenderCounters`, `ApplyPreset` | counters |
| | `StartWriter`, `StopWriter`, `WriterMain`, `FormatHitch`, `BuildReport`, `SaveReport`, `KeyedReport`, `SamplingReport` | output |
| | `RenderUI`, `RenderLive`, `RenderHitches`, `RenderKeyed`, `RenderSampling`, `RenderAdvanced` | UI |
| `features/address_space.{h,cpp}` | `Start`, `Stop`, `ReportText` | address-space monitor |
| `framework/d3d9_hooks.cpp` | `Run`, `RunList`, `CountCall`, `ReadStateCallCounts`, `OffThreadDispatches` | mod D3D time, per-name timing, state-call counts |
| `framework/game_addresses.*` | ids `ResFindProvider` .. `RemoteMethodVtable2`, `TexCreateCall`, `TexFillCall` | counter addresses |
| `apex_gui.cpp` | `DevProfilerTab` | the Developer card |
| `apex_config.cpp` | settings load / save, profile capture | `[qol.frame_profiler]`, `[developer.frame_profiler]`; `enabled` forced off on load and import |
| `apex_main.cpp` | `DLL_PROCESS_DETACH` | `FrameProfiler::Shutdown()` |

### Game addresses

All addresses Steam 1.67.2 (`TS3W.exe`, image base 0x00400000, no ASLR). Patterns: `kTargets`.

| Address | What | How found / verified |
|---|---|---|
| 0x00EC9F00 | render frame, argument = "present" flag; +0xBA = inactive limiter site 0x00EC9FBA | pattern at entry; `full.asm` (callers 0x00ECA059, 0x00ECA2E6, 0x00ECAAFC) |
| 0x00611680 / 0x00611760 | end frame + Present / its only caller wrapper | pattern; `full.asm` |
| 0x00611620 | BeginFrame (checks renderer +0x8C / +0x8D) | `full.asm` |
| 0x0059ED20 / 0x0059ED70 | ServiceManager main / sim loops, indirect calls 0x0059ED57 / 0x0059EDA7 | whole-body pattern; `full.asm` |
| 0x00599720 | ExecuteJob: `[job+0x24]=4`, `call [job+0x10](job+8, [job+0x14], 4)` at 0x0059974F | pattern; `full.asm` |
| 0x0059A220 | WaitForJob (reached via Job::Wait 0x0059A500) | pattern |
| 0x007D9840, 0x010650C4, 0x010650D8 | remote-call job function, PostRemoteMethodCall vtables (stored at 0x00ABEA0A / 0x00ABEA93) | `RemoteCallJob` (Sig), `RemoteMethodVtable`, `RemoteMethodVtable2` |
| 0x004E16F0 / 0x004E2760 | EA::Thread::Mutex::Lock / Semaphore::Wait | pattern (864 Mutex::Lock call sites) |
| 0x004DB850 / 0x004DB8E0 | FileStream::Read (ReadFile) / Flush (FlushFileBuffers) | pattern |
| 0x004EC010 | RefPack stream read; referenced only from data 0x00FB9020 | `full.asm` |
| 0x00C6C290, 0x00AC20E0, 0x00AEB2E0, 0x00AEA680, 0x00AD9E30, 0x00ADBAD0, 0x006A80E0, 0x00ADB8F0, 0x00C845C0, 0x00ABFAC0 | lot and terrain targets | [lot loading and streaming](../engine/lot-loading-and-streaming.md) |
| 0x00E4A050 | GC_try_to_collect, only caller 0x00D819AA | [Mono GC](../engine/mono-gc.md) |
| 0x006EBB70, 0x006E8810, 0x009DE140, 0x00EC6C30, 0x005943F0, 0x00AD97E0 | main-loop steps | [main loop](../engine/main-loop-and-services.md) |
| lot +0xC1 / +0xC9 | detailed-view flag / bulldozing flag (promotion counter condition) | `Hook_LotDetailRequest` |
| WorldManager +0x3A0 | camera point stored by 0x00C6C290 | `full.asm` (movaps at 0x00C6C2B3) |
| 0x004AFFC0; slots 0x00FB2DE0, 0x00FFE290 | FindProvider and its two vtable slots | `ResFindProvider` (Sig), `ResFindProviderSlot0/1` (`K::SlotsOf`, exactly 2) |
| 0x004EC200; slot 0x00FB901C | RefPack stream write | `RefPackCompress`, `RefPackCompressSlot` (SlotsOf, exactly 1) |
| 0x006EBC49 -> 0x006E4130 | BeginFrame's CALL of the pending-node drain | `SceneDrainCall` (Sig), `SceneDrain` |
| 0x006152F0 / 0x006154B0 | DXT1 / DXT5 encoders | `DxtEncode1`, `DxtEncode5` (Sig) |
| 0x00C62D40 | object lookup by ID | `ObjectById` (Sig) |
| 0x00ADB9AD -> 0x006A8BA0 | lot lighting update's CALL of the room solve | `RoomSolveCall` (Sig), `RoomSolve` |
| 0x0060E1DC -> 0x0060CEA0, 0x0060E1FF -> 0x0060D290 | DDS texture loader's create and fill CALLs | `TexCreateCall`, `TexFillCall` |

Runtime verification: status strings in Advanced > Hooks and the "Timed functions:" block of each session in the output
file ("Timed (pattern matches at the Steam 1.67.2 address[; hand-made hook, all threads checked])", "Timed at the CALL
0x00aeb306 -> 0x00aea680 ...", "outer layer of the slot chain", "outer layer of the entry chain", "outer layer of the call
chain"); log line `[FrameProfiler] Timing N of M game functions`.

## Rejected approaches

- Detours for functions many threads call: only the calling thread is fixed up, and suspending threads before the
  commit's heap allocations can deadlock. Details in [history](../history/frame-profiler.md).
- Detouring entries that other modules verify: they refuse to install or silently disable features. Call sites or
  callers are used instead.
- Hooking a site another Apex module wraps outside its chain: a direct slot swap, entry or CALL write would cut the other
  layer out or be refused by its expected-value check.
- Attaching from the UI click instead of the render thread at a frame boundary: call-site writes would race the render
  thread.
- Registered callbacks for state-call counting: every game state call then ran a full registry dispatch (0.25 to 0.35 ms
  per frame).
- Global `std::thread` objects for long-lived threads: they terminate the process at exit; raw `CreateThread` is used.

## See also

- [Validation](../validation/frame-profiler.md)
- [History](../history/frame-profiler.md)
- [Performance](performance/README.md)
- [Developer mode](developer-mode.md)
- [Engine main loop and services](../engine/main-loop-and-services.md)
- [Removed features](../removed-features.md) (Service Frame Budget, Smooth Streaming and Script GC Scheduler, measured
  with this tool)
