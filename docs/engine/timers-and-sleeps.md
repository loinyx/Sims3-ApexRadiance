# Clock, sleeps, frame limiter and Smooth Patch sites (TS3W.exe)

This page documents how the game waits: the stopwatch behind every budget, the sleep wrapper `0x004E1320`, the simulation-thread idle, the render thread's inactive-window sleep and the background idle in the main loop. These are the sites frame limiters and tick patches touch. Apex Radiance ships no timer, sleep or tick patch; the Frame Profiler reports the limiter time.

## Scope

| | |
|---|---|
| Game build | Steam 1.67.2 (`TS3W.exe`, PE timestamp `0x52DEC247`, image base 0x00400000, relocations stripped, no ASLR) |
| Used by | [Frame Profiler](../features/frame-profiler.md) (timing only); reference for S3SS Smooth Patch coexistence |
| Evidence | Disassembly (`S3SS-dev\research\engine_map\full.asm`), byte checks in shipped code; *Verified* = read in the disassembly or checked byte for byte |

## Overview

Engine reference for TS3W.exe 1.67.2 Steam (PE timestamp `0x52DEC247`, image base 0x00400000, relocations stripped,
so no ASLR). "Verified" = read in the disassembly (`S3SS-dev\research\engine_map\full.asm`, the permanent copy of the
session scratchpad `engine_map\`) or checked byte for byte by shipped code. *(unverified)* / *(inferred)* = not
confirmed.

How the game waits: the stopwatch it uses for every budget, the one sleep wrapper (`0x004E1320`) that almost every
wait goes through, the simulation-thread idle (`IdleSimulationCycle`), the render thread's inactive-window sleep, and
the background idle in the main loop. These are the sites every frame limiter / tick patch touches, including
LazyDuchess's Smooth Patch and S3SS's Smooth Patch Classic / Precise.

**Apex ships no timer, sleep or tick patch.** The improved Smooth Patch Precise of the combined build collides byte for
byte with official S3SS's Smooth Patch (`0xD81FDE`, `0xEC9FBA`) and stays out of the standalone
([../architecture.md](../architecture.md#114-game-code-sites-and-the-conflict-guard), [../removed-features.md](../removed-features.md)). Apex code
that meets these sites: the [Frame Profiler](../features/frame-profiler.md) (developer mode) detours the prologue of `0xEC9F00`
(the limiter at `+0xBA` is inside the body, not in the relocated bytes) and reports "Frame limiter" time.

## Details

### The simulation idle: `IdleSimulationCycle` (`0x007694B0`)

Disassembly, in order:
1. If `host+0xA60 == 0` (idling disabled), return.
2. First call only: load the float at `0x0100772C` (33,333,333) and convert it (`0xF7EC76`, a float-to-int64 helper)
   into the globals `0x011D6708/0x011D670C` (guard bit `0x011D6710 & 1`).
3. Read the stopwatch at `host+0xA40` (`0x4F32C0`), restart it (RDTSC or QPC depending on its mode word `+0x10`).
4. `debt += target - elapsed` in `host+0xA58/+0xA5C`, clamped to [-33,000,000, +33,000,000].
5. If the debt is positive, divide by 1,000,000 (`0xF4240`, ns -> ms, helper `0xF7EF00`) and sleep that many ms through
   `0x004E1320` (`0x7695CA`). *(The ns unit is inferred from the 33,333,333 target and the 1e6 divisor.)*

So the stock game ticks the simulation at about **30 Hz**, sleeping with millisecond granularity (and with the system
timer resolution on top: 15.6 ms unless a process asked for a finer one).

### Stopwatches and budgets

- Every engine budget (services, lot load stages, lot lighting, GC slice) uses the EA stopwatch above. Units seen:
  **4 = milliseconds** (scale `[0x011CB8FC]`) and **3 = microseconds** (scale `[0x011CB900]`), both from QPC. The value
  is read as an unsigned 32-bit integer; `0xFFFFFFFF` means unlimited.
- The deadline is always checked **after** a work item, so one item can overshoot any budget (the root cause of most
  hitches; [main-loop-and-services.md](main-loop-and-services.md)).
- Examples: the lot load stage budget `C7 44 24 14 imm32` at `0x00AEA6AC` (20 ms), `0x00AEA6D0` (35 ms),
  `0x00AEA6E9` (2000 ms) ([lot-loading-and-streaming.md](lot-loading-and-streaming.md)); the GC slice timer
  `0x011EE530` in unit 3 ([mono-gc.md](mono-gc.md)).

### Render-thread waits in a frame

Per main-loop iteration (`0xECA960`, [main-loop-and-services.md](main-loop-and-services.md)):
1. Optional background idle `0x5887A0` (10 ms) when another process owns the foreground window and `NoInactiveIdle` was
   not given.
2. The services, scene jobs and the render frame.
3. Inside the render frame `0xEC9F00`: end frame + Present (`0x611760` -> `0x611680`), then the inactive-window sleep of
   30 ms at `0xEC9FBA` when `device+0x8D` is set.
4. The clock tick `0x5943F0`.

The render thread does not wait for the simulation thread each frame (see main-loop-and-services.md, thread model).

### What the known patches change (for reference; none of them is in Apex)

| Patch | Site | Change |
|---|---|---|
| LazyDuchess Smooth Patch (TS3FrameratePatch), and S3SS **Smooth Patch Classic** | every match of `8B 44 24 04 8B 08 6A 01 51 FF` (the start of `0x4E1320`) | `mov ecx, <1000/TPS>; nop` so every call sleeps a fixed time; `ret` for "uncapped"; `mov ecx,0; nop; push 0` for "system". Changes all 48 callers at once |
| S3SS **Smooth Patch Precise** (Just Harry) | CALL at `0xD81FDE` | redirected to its own IdleSimulationCycle: QPC deadline at the chosen tick rate, optional "tick at most once per frame" hand-off from the render thread |
| same | 9 bytes at `0xEC9FBA` | `3E 56` (`ds: push esi`, padding so the call ends where the original CMP ended), `E8 rel32` (call `DelayAfterFramePresentation(device)`), `5E` (pop esi), `EB` (jmp rel8, reusing the original displacement byte). Frame limiter for active and inactive windows, on the render thread after Present |
| Apex's improved Smooth Patch Precise (combined build only) | same two sites | WaitOnAddress instead of spinning for "tick once per frame"; sleep until one timer period before the deadline (`NtDelayExecution`, relative) then `_mm_pause` spin; its own 0.5 ms timer request (`timeBeginPeriod(1)` then `NtSetTimerResolution(5000)`; per process since Windows 10 2004) plus `PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION`; optional render/simulation thread boost (Above normal or MMCSS "Games"). **Not in the standalone** |
| S3SS Timer Optimization | process | `NtSetTimerResolution` to 1 ms |
| S3SS AdaptiveWait | `WaitForSingleObject(Ex)` hooks | short spin before INFINITE waits; answers `WaitForSingleObjectEx(GetCurrentThread(), 0, FALSE)` (the APC pump at `0xEBB2B0+0x14 = 0xEBB2C4`) without a syscall; guards the dead Mono domain-unload path at `0xE661AE` |

### Which Apex features depend on what

| Item | Apex user |
|---|---|
| `0xEC9F00` prologue, and the split "after `0x611680` returns = Frame limiter" | Frame Profiler (developer mode) |
| Stopwatch units (3 = us, 4 = ms) | only as knowledge (the removed performance patches rewrote budget units: [../removed-features.md](../removed-features.md)) |
| Everything else in this file | nothing (knowledge for diagnosis) |

### Measurements

- Frame time median 4.4 - 7 ms at 4K on the test machine; "Present (driver)" about 1.0 ms of it on average; 7-9 % of
  frames are hitches, which come from single large work items, not from sleeps
  (`S3SS-dev\SIMS3-PERFORMANCE-KNOWLEDGE.md`, Frame Profiler sessions of 28/09).
- Pacing advice: one limiter only (237 fps with G-Sync) instead of vsync at 240 plus a cap of 240.
  Effect not isolated.

### Pitfalls

- `0x4E1320` takes a **pointer** to the millisecond count, not the count. A replacement that reads the argument as a
  value sleeps for an address.
- Its sleep is alertable (`SleepEx(..., TRUE)`): APCs run inside it.
- Patching `0x4E1320` changes all 48 callers (sim idle, render inactive sleep, worker waits...), not just the tick.
- The 9-byte replacement at `0xEC9FBA` must end exactly where the original instruction boundary was, so the patch can be
  removed at any time (that is why Smooth Patch pads with a DS prefix).
- Timer resolution is per process since Windows 10 2004: another process's `timeBeginPeriod` no longer speeds up the
  game's sleeps.
- Do not ship anything on `0xD81FDE` / `0xEC9FBA` / `0x4E1320` in the standalone while official S3SS's Smooth Patch can
  be installed: same bytes.

### Additional details (static analysis)

**Stopwatch units.** The unit is stored at stopwatch+0x10 and its scale at +0x14 (`0x004F30E0`). The jump table
`0x004F31FC`, read from the exe bytes:

| Unit | Handler | Scale | Meaning |
|---|---|---|---|
| 0 | `0x4F31A2` | default float `[0x0107A538]` | raw *(inferred)* |
| 1 | `0x4F3103` | ratio of `[0x0114C9F0]` / `[0x0114C9F8]` | RDTSC-based: `0x4F3231` reads RDTSC when +0x10 == 1 |
| 2 | `0x4F319A` | `[0x011CB904]` = 1e9 / QPF | nanoseconds |
| 3 | `0x4F31AD` | `[0x011CB900]` = 1e6 / QPF | microseconds |
| 4 | `0x4F31C0` | `[0x011CB8FC]` = 1000 / QPF | milliseconds |
| 5 | `0x4F31D3` | `[0x011CB8F8]` = 1 / QPF | seconds |
| 6 | `0x4F31E6` | `[0x011CB8F4]` = (1/60) / QPF | minutes |

The constants come from `0x4F2FC0`: `0x00F98968` = 1e9, `0x00F9896C` = 1e6, `0x00F98970` = 1000.0, `0x00FC25C4` =
1/60.

- **`host+0xA40` is created with unit 2 (ns)**: `push 2; lea ecx,[esi+0A40h]` at `0x0076ACAE`. This confirms the ns
  reading of IdleSimulationCycle's 33.3 ms target.
- **Simulation ticks inside Simulate** (`f_D81840.asm`):
  - tick count = trunc(`[host+0xC14]` x 29.999998) (`0x010049E4`), at most 10 per pass;
  - at 20 or more, the accumulator is reset to 0.6667 (`0x00FAD570`);
  - tick dt = n x 0.0333333 s (`0x0108B1A0`);
  - on the paused path Simulate sleeps 1 ms (`0xD81AF7`) or the remaining time clamped to 10 ms (`0xD81B16`) through
    `0x4E1320`.

  The meaning of the accumulator is *(inferred)*. More in [mono-gc.md](mono-gc.md).
- **Clock tick `0x5943F0`** (ECX = clock `0x011CDD48`; its first member is a mutex, see `0x594300`):
  - reads QPC; dt = now - `[+0x38]` (a negative dt becomes 0); stores now at `+0x38`;
  - scales dt with `0x4F3480(5)` (seconds);
  - `[+0x4C]` += 1; `[+0x54]` = dt; `[+0x50]` = time since `[+0x30]`;
  - `[+0x58]` = dt, or `[+0x48]` once when `[+0x44]` is set;
  - then calls `0x594230(dt)`.

  The main loop passes `[0x011CDD9C]` (clock+0x54) and `[0x011CDDAC]` (clock+0x64, forced to 0 while `device+0x8D` is
  set) to `ServiceManager::Update` *(field roles inferred)*.
- **The game raises the timer resolution itself.** `timeBeginPeriod(1)` at `0x00EFFE81`, the first call of the function
  `0x00EFFE70`, followed by `timeGetTime`. `timeEndPeriod` at `0x00F00146` in `0x00F00010`. These are the only two
  references to those imports. The owning subsystem and thread were not identified *(unverified)*.
- **Import references in `full.asm`.** Sleep (`0xF95110`): 53 references, calls or register loads (e.g. `0x5887C3`
  background idle, `0x4E1727` in `Mutex::Lock`). SleepEx (`0xF951F0`): 4 (`0x404B66`, `0x4E1329` the wrapper,
  `0x4F2E9D`, `0xEBAE74`).

**S3SS patch settings and behaviour** (upstream files in the combined tree; they stay in S3SS):

| Patch | Setting keys (default) | Notes |
|---|---|---|
| Smooth Patch Precise (`SmoothPatchPrecise`) | `Tick at most once per frame` (false), `tickRateLimit` (480, 0-10000), `frameRateLimit` (60), `frameRateLimitInactive` (30; -1 = match); Apex additions in the combined build: `Request high timer resolution` (true), `threadPriorityBoost` (2 = MMCSS "Games") | Values are computed at install (debounced 2 s reinstall on change). `ReassertTimerResolution` runs every 256 `Update()` calls. `ReleaseTimerResolution` re-applies Timer Optimization's 1 ms if that patch is on. The test session of 2026-09-28 14:20 logged tickOnce true, 960 TPS, 237 FPS, 60 inactive, 0.5 ms held, MMCSS on both threads |
| Smooth Patch Classic (`SmoothPatchClassic`) | `customTPS` (500 → 2 ms; 0 = sleep 0; -1 = `ret`) | Its description says "Original game runs at 50 'TPS' (20ms sleep)". The static reading of IdleSimulationCycle gives a 33.3 ms (30 Hz) target instead. Discrepancy not resolved |
| Timer Optimization (`TimerOptimization`) | none | `NtSetTimerResolution(10000, TRUE)` + `timeBeginPeriod(1)`. Uninstall calls `timeEndPeriod(1)` and then `NtSetTimerResolution(<resolution queried at install>, TRUE)`, so it leaves a request in place instead of releasing it |
| Adaptive Thread Waiting (`AdaptiveWait`) | none (spin budget 50-500 µs, starts at 100, adapts every 64 calls) | IAT hooks on `WaitForSingleObject(Ex)`. **`Uninstall` only clears `isEnabled`**: the IAT hooks stay installed, and `TrySmartWait` does not check the flag, so the spinning continues until the game restarts (static reading of `adaptive_wait_patch.cpp`) |

### Open items

- Meaning of BL in the main loop (object `0x72F1C0`, vfunc `+0x58`), which switches rendering to one frame per 100 ms.
- Exact meaning of `device+0x8D` (inactive window vs device lost) and of the clock calls `0x594300` / `0x594330`.
- ~~The stopwatch unit of `host+0xA40` (inferred ns).~~ Resolved: unit 2 = ns ([Additional details](#additional-details-static-analysis)).
- Which subsystem owns the game's own `timeBeginPeriod(1)` (`0x00EFFE70`).

## Address reference

| Address | What | Convention | Thread | How verified |
|---|---|---|---|---|
| `0x004E1320` | Sleep wrapper: `mov eax,[esp+4]; mov ecx,[eax]; push 1; push ecx; call [SleepEx]; ret` = `SleepEx(*ms, TRUE)` (alertable). The argument is a **pointer** to the millisecond count | `__cdecl(const DWORD* ms)` | any | disassembly; 48 call sites in `calls.tsv` (e.g. `0x7695CA` IdleSimulationCycle, `0xD81AF7` / `0xD81B16` in Simulate, `0xEC9FD1` render frame) |
| `0x004E1330` | `jmp [GetTickCount]` | - | any | disassembly |
| `0x005887A0` | Background idle: `GetForegroundWindow` -> `GetWindowThreadProcessId`; if the foreground window belongs to another process, `Sleep(10)` | `__cdecl()` | render | disassembly (IAT `0xF958F8`, `0xF95840`, `0xF95170` GetCurrentProcessId, `0xF95110` Sleep); 2 callers |
| `0x00ECA9E8` | Its call in the main loop `0xECA960`: only when the command line has no `NoInactiveIdle` (string at `0x010F5B14`, test `0x0058A8D0`) and BL = 0 | - | render | `f_ECA960.asm` |
| `0x00ECAA5C..0x00ECAAB5` | While BL is set, the main loop renders only when a stopwatch (unit 4 = ms) reached **100.0** ms (`[0x010F5B10]` = `0x42C80000`), then restarts it; otherwise it skips straight to the clock tick. BL = vfunc `+0x58` of the object returned by `0x0072F1C0` *(meaning unverified; plausibly a loading state)* | - | render | `f_ECA960.asm` |
| `0x00EC9F00` | Render frame (scene + UI, end frame, Present, limiter) | `__stdcall(1)`, ret 4 (`this` in ECX) | render | profiler table, pattern `83 EC 20 56 E8 ...` |
| `0x00EC9FBA` | **Inactive-window limiter**: `cmp byte [esi+8Dh],0; pop esi; jz 0xEC9FD9; lea ecx,[esp+24h]; push ecx; mov dword [esp+28h],1Eh; call 0x4E1320` = sleep **30 ms** per frame while `device+0x8D` is set | inside `0xEC9F00`, after `0x611760` (end frame + Present) | render | disassembly; Smooth Patch pattern `80 BE 8D 00 00 00 00 5E 74 15 8D 4C 24 24 51 C7 44 24 28 1E 00 00 00` (Retail `0xECA64A`, EA `0xEC9F9A`) |
| `device+0x8D` | Byte on the game's graphics device object (ESI in `0xEC9F00`, also returned by `0x0060E2C0` in the main loop): "window not in the foreground" per the Smooth Patch comment, possibly "device lost" *(unverified)* | - | - | code comment in `smooth_patch_precise.cpp` |
| `0x00D81840` | `MonoScriptHost::Simulate`, the simulation thread's loop | `__thiscall(host, 1 arg)`, ret 4 | sim | `f_D81840.asm` |
| `0x00D81FDE` | `call 0x007694B0` (IdleSimulationCycle) at the end of each Simulate pass | - | sim | pattern `8B 86 C0 0A 00 00 3B C3 74 0F 8B 4C 24 5C 8B 11 6A 01 53 50 8B 42 24 FF D0 8B CE` (Retail `0xD8246E`, EA `0xD8239E`) |
| `0x007694B0` | `ScriptHostBase::IdleSimulationCycle` (below) | `__thiscall(host)` | sim | disassembly |
| `host+0xA60` | `mbIdlingEnabled`: IdleSimulationCycle returns at once when 0 | byte | sim | disassembly (`0x7694B6`) |
| `host+0xA40` | stopwatch used by IdleSimulationCycle | - | sim | disassembly |
| `host+0xA58/+0xA5C` | 64-bit sleep debt (ns), clamped to +/-33,000,000 | - | sim | disassembly (`0x76953C..0x769594`) |
| `host+0xC08` | simulation running flag: `xchg [esi+0C08h],1` at `0xD8185B`, loop test at `0xD81FF6` | dword | sim | disassembly |
| `0x0100772C` | float `0x4BFE502C` = 33,333,333: the target simulation cycle in ns (**30 Hz**) | - | - | `dwords.txt`, read at `0x7694CC` |
| `0x004F35B0` | Stopwatch constructor `(this, unit, start)` | `__thiscall` | any | 81 callers; units: see [Stopwatches and budgets](#stopwatches-and-budgets) |
| `0x00408700` | Stopwatch start | - | any | smooth_streaming_patch.cpp notes |
| `0x004F32C0` | Stopwatch read / restart (used before a time-boxed call) | - | any | disassembly of Simulate and IdleSimulationCycle |
| `0x004F33C0` | Stopwatch elapsed (returns on the FPU stack) | - | any | main loop `0xECAA60`; smooth_streaming notes |
| `0x004F34F0` | Stopwatch arm: deadline = now + value / scale | `__thiscall(this, value, flag)` | any | frame_budget_patch.cpp notes |
| `0x004F30E0` | Maps a unit to its scale; jump table `0x004F31FC` | - | any | notes |
| `0x004F2FC0` / `0x004F2FF7` | Scale init: `[0x011CB8FC]` = 1000.0 (`0x00F98970`) / QPF (unit 4, ms); `[0x011CB900]` = 1e6 / QPF (unit 3, us) | - | init | notes (smooth_streaming / frame_budget / gc_scheduler headers) |
| `0x005943F0` | Game clock tick (on the clock object `0x011CDD48`), last step of every main-loop iteration | `__thiscall(0)` | render | `f_ECA960.asm`, profiler |
| `0x00594300` / `0x00594330` | Clock calls made with argument 4 before the services, chosen by `device+0x8D`: `0x594300` when set (and the second float passed to ServiceManager::Update becomes 0), `0x594330` otherwise *(semantics unverified; likely pause / resume of a clock channel)* | `__thiscall(4)` | render | `f_ECA960.asm` |
| IAT `0x00F95110` Sleep, `0x00F951F0` SleepEx, `0x00F95120` GetTickCount, `0x00F953B8` QueryPerformanceCounter, `0x00F953C0` QueryPerformanceFrequency | imports | - | - | `iat.map` (also imported: timeGetTime, WaitForSingleObject(Ex), WaitForMultipleObjects(Ex)) |
| `0x004E16F0` / `0x004E2760` | `EA::Thread::Mutex::Lock` / `Semaphore::Wait` (WaitForSingleObject), both `__thiscall(timeout*)`, ret 4 | - | any | profiler table |

## See also

- [Main loop, services and threads](main-loop-and-services.md).
- [Mono / Boehm GC and the simulation thread](mono-gc.md).
- [Architecture: game-code sites and the conflict guard](../architecture.md#114-game-code-sites-and-the-conflict-guard).
