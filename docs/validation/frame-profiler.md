# Frame Profiler: validation

The feature is described in [features/frame-profiler.md](../features/frame-profiler.md). It must:

- Hook nothing while off, and never start a measurement from a saved or imported setting.
- Attach every target whose bytes match, and skip and report every target whose bytes do not.
- Stay the outer layer of every shared hook site (except inside the wall shading gate) without cutting another layer out.
- Attribute render-thread self time so that it never exceeds the frame time.

## Automated tests

None for the profiler itself. The analysis tools in `research\perf2\tools\` (outside this repository) read its output:

| Tool | What it checks | How to run |
|---|---|---|
| `dom.pl` | Dominant cause per hitch and top-counter distribution per bucket | `perl dom.pl [--since "YYYY-MM-DD HH:MM"] [--computed] FILE` |
| `agg.pl` | Per-hitch category and counter averages (`C:`), dominant shares (`D:`) | `perl agg.pl FILE` |
| `cond2.pl` | Hitches matching a condition (`dom:<key>`, `job:remote call -> X`) | `perl cond2.pl FILE CONDITION` |
| `smp.pl` | Sampling lines | `perl smp.pl FILE` |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-09-28 | combined build | `dom.pl`, `cond2.pl` on the combined build's 51 MB `S3SS_Hitches.txt` | Same numbers as the anti-stutter plan's sections 2.1 and 2.2 (`cond2.pl` `Scene::BeginFrame 8`: 772 hitches) | DXVK |
| 2026-09-28 | combined build | In game, 13:19 report, 51 600 frames | p50 4.38 / p95 14.28 / p99 32.38 ms, 6.86% hitches; overhead: "Present hooks (mod)" about 0.2 ms per frame (all modules, profiler included), "D3D hooks (mod)" about 0.08 ms per frame | DXVK |

## In-game test plan

1. Turn on developer mode, open Developer > Performance and switch on *Frame times and stutters*. **Expected:** the
   status changes from "Waiting for frames..." to the live line; `ApexRadiance_LOG.txt` shows `[FrameProfiler] On` and
   `[FrameProfiler] Timing N of M game functions`. With the defaults 38 of 38 are expected (40 targets; Mutex::Lock and
   lot object building off by option), 39 of 39 with *Time the Mutex::Lock hook* on. Turned on within about a second of
   the first frame, the first line reads `(K waiting for the game-address scan)` and is followed by the full count.
2. Restart the game with the profiler on when it was closed. **Expected:** it starts off.
3. Advanced > Hooks. **Expected:** every row "Timed (...)" or a chain status ("outer layer of the slot chain", "entry
   chain", "call chain"); Calls R / S / O increase; the "GC call site" and "Frame limiter" lines show which patches are
   active. Any skipped target is logged as a warning with its reason.
4. Walk or pan the camera across the neighbourhood. **Expected:** hitches under "Last hitches"; every counter row of
   "Counters" fills in. Save report now and read `ApexRadiance_Hitches.txt` in the Apex Radiance folder.
5. Sampling run preset. **Expected:** `[FrameProfiler] Sampler on`; Advanced > Sampling shows the samples per second and
   the microseconds paused per sample; the Apex module base and PE TimeDateStamp are logged.
6. Reading results: high Present (driver) = GPU or vsync bound; high CPU = game or submission bound. A hitch flagged
   "(window in background)" with about 10 ms Unattributed and samples in ntdll called from 0x005887C9 is the game's own
   background `Sleep(10)` (0x005887A0, called from the main loop at 0x00ECA9E8), not a real hitch. Compare "ms per hitch"
   with "ms per other frame" in the keyed tables: high in both is steady load, high only in hitches is a spike source.

## Confirmed in game

- The 2026-09-28 engine study used the profiler over 11 sessions (51 MB of output); see
  [history](../history/frame-profiler.md) for what it found.
- The 2026-09-30 sampling runs located the CAS triangle sort and the RefPack compressor hitches
  ([Performance](../features/performance/README.md)).

## Open checks

- The exact count logged with the current 40 targets (`Timing 38 of 38` is expected from the code, not observed).
- Remote-call job keys (both PostRemoteMethodCall vtables): not yet seen in a report.
- `kTargets` labels the lot impostor pump 0x00AD97E0 "render", but in the 13:19 report its two calls (563 ms) were booked
  in the simulation column; which thread runs it is unverified.
- Scene::EndFrame shows about 2 calls per frame (93 494 calls / 51 600 frames); its other callers are 0x007EB6D0,
  0x00AD97E0 and 0x00ECA2A0; which one runs every frame is not established.
- Unnamed services seen in reports: render `0x00588A00` (vtable 0x00FCBD34), `0x00D61670` (0x01088F2C), `0x009D96D0`
  (0x01051C78), `0x00687C80` (0x00FF0070); simulation loop `0x00EC5340` (0x010F3D00, 16 s self in 13:19), `0x007F1530`
  (0x0101E758, 11 s), `0x00869000` (0x01029060, 3 s), `0x0076B1A0` (0x01007B74, Scripting service, 2.4 s). Add them to
  `kServiceNames` once identified.
- The int3-padding function-start guess has not been checked against `ApexRadiance.map` for Apex's own code.
- Shutdown on FreeLibrary: the writer stop (up to 5 s) and sampler stop (up to 2 s) may run to their timeouts under the
  loader lock (inferred, not observed).
- Two tooltips disagree with the code: *Time lot object building* says "Not saved" although `time_objects` is saved;
  *Per-hook registry timing* mentions only the draw hooks although the state chains are timed too. The
  *Per-hook registry timing* toggle does not request a save by itself.
- Not implemented: "owned by Sims3SettingsSetter" labels in the Hooks table and a separate sampler class for its module
  (it counts as "other ASI").
