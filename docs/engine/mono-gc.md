# Mono / Boehm GC and the simulation thread

This page documents the Mono runtime embedded in `TS3W.exe`, its Boehm-Demers-Weiser collector in incremental mode, and the simulation thread on which `MonoScriptHost` runs sim ticks, script tasks, a time-boxed GC slice and a finalizer batch. The Frame Profiler's "Script GC" category depends on it; it is also the background for the removed Script GC Scheduler and for S3SS's GC patches.

## Scope

| | |
|---|---|
| Game build | Steam 1.67.2 (`TS3W.exe`, image base 0x00400000) unless stated |
| Used by | [Frame Profiler](../features/frame-profiler.md); historically the Script GC Scheduler ([removed](../removed-features.md#script-gc-scheduler)) |
| Evidence | Combined-build `gc_scheduler_patch.cpp` header (from `dumpbin /disasm`), S3SS GC patch files, `engine_map` `f_D81840.asm`, `full.asm`, `dwords.txt`, `iat.map`, `fnstrings.tsv`; items marked *(inferred)* are static only |

## Overview

TS3W.exe embeds Mono with the Boehm-Demers-Weiser conservative collector, compiled in incremental mode. The game's script
host (`MonoScriptHost`) runs on its own **simulation thread**. That thread executes the sim ticks, the script tasks
and, on every pass, an explicit time-boxed GC slice and a finalizer batch.

This page maps those functions and globals. It is the background for:
- the Script GC Scheduler of the combined build (removed from the standalone on 2026-09-28; see
  [../removed-features.md](../removed-features.md#script-gc-scheduler));
- the Frame Profiler's "Script GC" category;
- S3SS's three GC patches (GCTryToCollect, GCFinalizeThrottle, GCStopWorld).

TS3W.exe 1.67.2 Steam, image base 0x00400000. Items marked *(inferred)* come from static reading only.

Threads, per NOTAS-ILUMINACAO.md "Desempenho: mapa do motor (28/09)":
- The render thread is the main thread.
- The render thread does **not** wait for the simulation thread every frame.
- The game UI runs on the simulation thread.

So GC work on the simulation thread shows up in the frame only indirectly: through sim tick rate, UI responsiveness,
and locks shared with the render thread. See [main-loop-and-services.md](main-loop-and-services.md).

## Details

### Sources

Sources:
- `patches/gc_scheduler_patch.cpp` header (read from the raw exe with `dumpbin /disasm`);
- the S3SS GC patch files;
- engine_map `f_D81840.asm`, `full.asm`, `dwords.txt`, `iat.map`, `fnstrings.tsv`;
- `frame_profiler.cpp`;
- `patches/smooth_patch_precise.cpp`.

### How a Simulate pass collects

1. Get the start time from the µs stopwatch 0x011EE530 and store it at 0x011EE524.
2. Call `GC_try_to_collect(0x00D717B0)`. Every incremental step polls the stop callback, which aborts once more than
   [0x011922A4] µs (500-1500, initially 1000) have passed since step 1. Effect: **the game collects continuously**, one
   time-boxed slice per Simulate pass. With Smooth Patch Precise, passes run up to the configured tick rate (the test
   log of 2026-09-28 14:20 shows 960 TPS).
3. Not time-boxed, so these are the long spikes:
   - `GC_finish_collection`;
   - the forced finish after 40 aborted attempts;
   - any blocking collection that an allocation starts (`GC_collect_or_expand` → 0xD70800 → 0xE4A320). That path is
     taken when no free heap block is left, which is why the GC Scheduler keeps a free-heap watermark.
4. Adapt the budget by ±100 µs based on the counter [0x011F2F98] (see the table).
5. Run the finalizer batch (0xE697F0). Sustained pressure (at least 50 finalizers per call for 200 passes in a row)
   triggers a blocking "run until none left" loop. S3SS's GCFinalizeThrottle targets this.

**Measured** (Frame Profiler "Script GC" = the time inside 0xE4A050, simulation thread; `S3SS_Hitches.txt` reports of
2026-09-28, GC Scheduler on but, per the 14:20 log, not postponing):

| Report | Average per call | Worst per hitch |
|---|---|---|
| 11:34 | 1.54 ms | 3.15 ms |
| 12:00 | 1.65 ms | 3.47 ms |
| 13:19 | 1.26 ms | 3.94 ms |

The 14:20 session's longest call was 3.83 ms. The averages exceed the nominal 1 ms budget, because the budget is only
checked between steps.

### S3SS's GC patches (upstream S3SS; they stay in S3SS after the split)

| Patch (name, display name) | Bytes | Effect and notes |
|---|---|---|
| `GCTryToCollect`, "Chunky Patch - Disable GC_try_to_collect()" | NOPs the 5-byte `call` at 0xD819AA (pattern `68 ?? ?? ?? ?? A3 ?? ?? ?? ?? E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 83 C4 04`, +10; `expectedBytes {E8}`) | No explicit collections at all. Collection then happens only through allocation pressure, which means **blocking full collections** on the allocating thread (0xD70800 → 0xE4A320). In the combined build it refuses while GcScheduler is on, and vice versa (see [../removed-features.md](../removed-features.md#script-gc-scheduler)). Upstream only checks `E8`, so it would NOP over Apex's redirect (PLANO-SEPARACAO.md §3) |
| `GCFinalizeThrottle`, "GC Finalizer Throttle" | `cmp eax,0C8h` → `cmp eax,7FFFh` at 0xD81A1D (pattern `03 C5 3D C8 00 00 00 A3 ?? ?? ?? ?? 75`, +2); `jnz` (75 F7) → `90 90` at 0xD81A37 (pattern `E8 ?? ?? ?? ?? 85 C0 75 F7 89 1D`, +7). PLANO §3 lists the pattern starts 0xD81A1B / 0xD81A30 | The blocking loop triggers after 32767 pressured passes instead of 200, and then runs one extra batch instead of looping until none are left. Finalizers can pile up in long sessions (upstream note: "May slightly increase memory usage") |
| `GCStopWorld`, "GC_stop_world() Optimization" | At 0xE511F5 (Retail 0xE514E5, EA 0xE51245), `3D 00 01 00 00 7C 05` → `85 C0 74 7D 90 90 90` (`test eax,eax; jz 0xE51276; nop x3`) | Upstream intent: skip the thread loop when the count is 0. **Static finding:** the patch also removes the `jl` that skipped `mov eax,0FFh` (0xE511FC), so whenever the count is non-zero the loop bound becomes 255 on every iteration. The loop then visits all 256 slots instead of count+1. Empty slots are skipped cheaply, so the effect is small either way *(not measured)*. Upstream calls it "very minor" |

The combined build's GC Scheduler redirected the same `call` as GCTryToCollect. It combined with the other two because
the bytes differ. The standalone ships no GC patch, so these three stay S3SS-only.

### The simulation thread and Smooth Patch

- The simulation thread is the one that calls `GC_try_to_collect`. The Frame Profiler identifies it that way
  (`Hook_ScriptGC` stores `g_simTid`).
- IdleSimulationCycle (0x7694B0), the job pump (0xD81FDA) and the finalizer loop run on it. Smooth Patch Precise's
  thread boost labels it "simulation thread" when its hook first runs there. See
  [timers-and-sleeps.md](timers-and-sleeps.md).
- A pass is: sim services (0x59ED70) → ticks → script tasks (0xD7FE80, 0xD74800, 0xD7F8B0) → GC slice → finalizers →
  stats → job pump → IdleSimulationCycle. This order is from `f_D81840.asm`; the GC slice sits near the top of the loop
  body at 0xD819AA, after `0xD7FC60` and the stopwatch reads.

### Which Apex features depend on what

| Item | Used by |
|---|---|
| Call 0xD819AA, target 0xE4A050 and its prologue, budget 0x011922A4 (display), 0xE69670 and the heap globals 0x012225A0/B4 | Script GC Scheduler (combined build only; removed, see [../removed-features.md](../removed-features.md#script-gc-scheduler)) |
| 0xE4A050 (entry Detour, "Script GC"), the call-site text for 0xD819AA | [Frame Profiler](../features/frame-profiler.md) |
| 0xD81FDE, host+0xC08, host+0xA60 | S3SS Smooth Patch Precise (not Apex in the standalone) |

### Open questions

- What increments [0x011F2F98] (the budget adaptation counter)?
- Whether [0x011EE528] can be 0 in normal play. If it were, the stop callback would never abort, and every slice would
  run to completion.
- How often allocator-triggered blocking collections happen in practice (not instrumented; the profiler times only
  0xE4A050).

## Address reference

### MonoScriptHost::Simulate (simulation thread)

| Address | Name / role | Evidence |
|---|---|---|
| 0x00D81840 | `MonoScriptHost::Simulate`, thiscall(1 arg), RET 4. Loops while [host+0xC08] == 1. Called through a vtable (no direct caller in `calls.tsv`) | `f_D81840.asm`; Smooth Patch comment |
| 0x00D8185B | `xchg [host+0xC08],1`: the "simulation running" flag is set at entry | `f_D81840.asm`, Smooth Patch comment |
| 0x00D81FF6 | Loop test `cmp [host+0xC08],1; je 0xD818C0` | `f_D81840.asm` |
| 0x00D8199B | `call 0x004F32C0` on timer 0x011EE530: the start time | GC Scheduler header |
| 0x00D819A0 | `push 0x00D717B0` (stop callback) | same |
| 0x00D819A5 | `mov [0x011EE524],eax` (start time) | same |
| **0x00D819AA** | `call 0x00E4A050` = `GC_try_to_collect(stop_func)`. The only caller | same; `calls.tsv` |
| 0x00D819AF..0x00D81A03 | Budget adaptation: when the counter [0x011F2F98] equals [0x011947E8] = 20, budget += 100; when it equals [0x011947EC] = -20, budget -= 100. The counter resets to 0 in both cases, and the budget is clamped to [0x011923A4] = 500 .. [0x01192324] = 1500 | disassembly, `dwords.txt` |
| 0x011922A4 | GC time budget per call, µs, initial 1000 (0x3E8) | `dwords.txt` |
| 0x011F2F98 | Counter compared at 0xD819AF. What increments it is not identified *(unverified)* | disassembly |
| 0x00D81A09..0x00D81A39 | Finalizer batch: `call 0x00E697F0` (mono_gc_invoke_finalizers). If the result is >= [0x01194848] = 50, increment [0x011EE52C]. When that counter reaches 200 (`cmp eax,0C8h` at 0xD81A1D), loop `call 0xE697F0; test eax,eax; jnz` (0xD81A30..0xD81A37) until 0, then reset the counter. A result below 50 resets the counter | disassembly; S3SS GCFinalizeThrottle |
| 0x011EE52C | "Frames since the finalizers finished" counter (S3SS's name: sFramesSinceFinalizeFinished) | GCFinalizeThrottle comment |
| 0x00D81AA2 / 0xD81C8E / 0xD81E73 / 0xD81F55 | `ServiceManager::UpdateSim` 0x0059ED70 (services with flag & 2) | disassembly |
| 0x00D81AF7 / 0x00D81B16 | Sleeps through 0x004E1320 on the paused path: 1 ms, or the remaining time clamped to 10 ms | disassembly; see [timers-and-sleeps.md](timers-and-sleeps.md) |
| 0x00D81B49..0x00D81B74 | Tick count = trunc([host+0xC14] x 29.999998), at most 10 per pass. At 20 or more, the accumulator resets to 0.6667 | disassembly *(inferred meaning)* |
| 0x00D81C27 | Tick dt = ticks x 0.0333333 (1/30 s) | `dwords.txt` 0x0108B1A0 |
| 0x00D81F9D / 0x00D81FAA | `mono_gc_get_used_size` 0xE69670 / heap size 0xE69690 → host stats setters 0x76A470 / 0x76A490 | disassembly |
| 0x00D81FC3..0x00D81FDA | `[host+0xAC0]` job context → JobManager `ProcessJobs(ctx, 0, 1)` (vtable +0x24) on the simulation thread | disassembly, `profiler_targets.tsv` |
| 0x00D81FDE | `call 0x007694B0` IdleSimulationCycle: **the Smooth Patch Precise site** (rel32 redirected) | Smooth Patch pattern |

`MonoScriptHost` fields:

| Offset | Meaning |
|---|---|
| +0xA40 | Idle stopwatch (unit 2 = ns) |
| +0xA58 | Idle "debt", 64-bit |
| +0xA60 | `mbIdlingEnabled` |
| +0xAC0 | Simulation-thread job context |
| +0xB78 | Optional per-pass callback |
| +0xC08 | Running flag |
| +0xC10 / +0xC14 | Sim time accumulators *(inferred)* |

### Boehm collector

| Address | Name / role | Evidence |
|---|---|---|
| 0x00E4A050 | `GC_try_to_collect(stop_func)`, cdecl. Checks GC_debugging_started, calls `GC_notify_or_invoke_finalizers` 0xE4B480, calls `GC_init` 0xE4E160 if needed, a lock stub (0xC0FD60 = `ret`), then `GC_try_to_collect_inner` 0xE49FB0 | disassembly; GC Scheduler prologue check `83 3D ?? ?? ?? ?? 00 74 06 FF 15 ?? ?? ?? ?? E8` |
| 0x00E4E160 | `GC_init` (reads the GC_* environment variables: GC_DONT_GC, GC_PRINT_STATS, GC_FIND_LEAK, GC_ALL_INTERIOR_POINTERS, ...). Its tail jumps to 0x00D70810 at 0xE4E43A | `fnstrings.tsv`, `full.asm` |
| 0x00D70810 | `mov [0x011EE528],1`: enables the stop callback | disassembly |
| 0x00D717B0 | Stop callback. Lazily creates stopwatch 0x011EE530 (unit 3 = µs, guard bit [0x011EE548] & 1), returns [0x011EE528] when (now - [0x011EE524]) > [0x011922A4], else 0 | disassembly |
| 0x00E49FB0 | `GC_try_to_collect_inner`: finishes a collection in progress in incremental slices, and when nothing is pending starts the next one at once | GC Scheduler header |
| 0x00E49F10 → 0x00E49DA0 | Incremental slice. Polls the stop callback every [0x0119481C] = 25 mark steps | same; `dwords.txt` |
| 0x00E49980 | `GC_stopped_mark`. Stops the world through 0xE511A0 and polls the stop callback on every mark step | same |
| 0x00E49E25..0x00E49E32 | After more than [0x01194820] = 40 aborted attempts: `GC_stopped_mark(GC_never_stop_func 0x005BFB90)`, a forced, non-interruptible finish | `full.asm`, `dwords.txt` |
| 0x00E49A50 | `GC_finish_collection` (not time-boxed) | GC Scheduler header |
| 0x005BFB90 | `GC_never_stop_func` (`xor eax,eax; ret`) | `full.asm` |
| 0x00E4A120 | `GC_collect_or_expand`, called from `GC_allocobj` at 0xE4A2F9. In incremental mode it calls 0x00D70800 (at 0xE4A1B4) | `full.asm`, `calls.tsv` |
| 0x00D70800 | `jmp 0x00E4A320`. Starts a collection if none is running and finishes it with `GC_never_stop_func`: a **blocking full collection on the allocating thread** | `full.asm`, GC Scheduler header |
| 0x00E511A0 | `GC_stop_world`. `EnterCriticalSection(0x01222580)`, [0x011F4120] = 1, then walks the thread table at 0x0122100C (stride 0x14) for slots 0..min([0x011F4124], 255). For every slot that is not the current thread: `GetExitCodeThread`; still active → `SuspendThread` (a failure reports the string at 0x0108E284); dead → clear the slot and `CloseHandle`. Then `LeaveCriticalSection` | disassembly (IAT names from `iat.map`) |
| 0x00E511F5 | `cmp eax,100h; jl 0xE51201; mov eax,0FFh`: the loop bound (the **GCStopWorld site**) | S3SS pattern `3D 00 01 00 00 7C ?? B8 FF 00 00 00 3B F8 7F` |
| 0x00E4B480 | `GC_notify_or_invoke_finalizers` | GC Scheduler header |
| 0x00E4DD30 / 0x00E4DD40 | `GC_get_heap_size` (`mov eax,[0x012225A0]`) / `GC_get_free_bytes` (`mov eax,[0x012225B4]`) | `full.asm` |
| 0x012225A0 / 0x012225B4 | Heap size / free bytes (GC_large_free_bytes: free heap blocks) | same |

### Mono wrappers

| Address | Name / role | Evidence |
|---|---|---|
| 0x00E69670 | `mono_gc_get_used_size` = heap size - free bytes, 64-bit (EDX = 0) | `full.asm`, GC Scheduler pattern |
| 0x00E69690 | Heap size, 64-bit (`call 0xE4DD30; xor edx,edx`), probably `mono_gc_get_heap_size` *(name inferred)* | `full.asm` |
| 0x00E697F0 | `mono_gc_invoke_finalizers`: `call 0xE4B3E0`; if non-zero, `jmp 0xE4B3F0`; else return 0. Probably GC_should_invoke_finalizers / GC_invoke_finalizers *(names inferred)* | `full.asm`; GC Scheduler header names the loop |

## See also

- [Main loop, services and threads](main-loop-and-services.md).
- [Clock, sleeps and frame limiter](timers-and-sleeps.md): the Smooth Patch sites.
- [Removed features: Script GC Scheduler](../removed-features.md#script-gc-scheduler).
