# Memory analysis

Memory analysis records process RAM and virtual address-space measurements for investigating large worlds, long
sessions and large saves. It provides evidence for future allocation research; it does not reduce memory use.

## Status

Released in 2.14.0 as developer diagnostics. Developer mode is off by default. In-game attribution remains unverified.

## The problem

Physical RAM consumption alone does not explain allocation failures in a 32-bit game. Reserved address ranges and
fragmentation can leave no sufficiently large free block even while physical RAM remains available.

## How Apex Radiance solves it

The existing address-space worker collects a sample every 10 seconds at below-normal thread priority, independent
of rendering in builds containing development tools, only when developer mode is enabled at startup. No rendering hook performs the scan.
Each session writes `MemoryAnalysis-<pid>-<date>-<time>.csv` in the Apex Radiance documents folder. Files stop growing
at 16 MiB per session; log and profiler snapshots continue. Previous sessions remain available for comparison.

Samples contain process private committed bytes, physical working set and peak working set, system available
physical memory and commit usage/limit, free virtual bytes and largest free regions below/above 2 GiB, committed
image/mapped/private regions, reserved ranges, private executable pages, cumulative page faults and I/O bytes,
tracked Apex high-allocator bytes, net private growth from baseline, scan duration and API validity flags.

## Settings

There is no new menu control. Builds without development tools do not start the observer. Logs identify the full
development version; `tools/development/build-test.ps1` creates numbered artifacts and increments the development
suffix when the corresponding artifact already exists. The menu uses the same version string.

## Compatibility and interactions

Windows process and memory-query APIs require no extra runtime library installation. The build uses the existing
Windows SDK, MSVC and project dependencies. The observer preserves the existing profiler address-space report.

## Limitations

- Address queries occur sequentially while allocations continue; a scan is not an atomic snapshot. Incomplete scans
  are flagged and excluded from the session minimum.
- Reserved address space is not physical RAM. Private committed bytes need not be resident. Mapped and image pages
  may be shared. Executable private pages do not identify the Mono heap.
- Net growth does not prove a leak. Page faults include soft faults; these counters cannot establish disk paging.
- The tracked Apex high allocator is only part of the mod's allocations. Game allocator counters use existing
  resolved addresses; their availability and meaning require validation on each supported game build.
- Allocation owners, object retention paths, texture/Sim counts and automatic optimizations are not inferred.
- Session files are bounded individually; retained sessions can accumulate on disk.

## Technical reference

`features/address_space.cpp` uses `VirtualQuery`, `GetProcessMemoryInfo`, `GetPerformanceInfo` and
`GetProcessIoCounters`. The walk uses 64-bit arithmetic to avoid wrapping at the top of a 32-bit address space and
splits free regions crossing 2 GiB. No heap is walked, no thread is suspended and no working set is trimmed.

`tools/development/analyze-memory.ps1 -Csv <file>` summarizes valid samples without modifying the input.

## See also

- [Validation](../validation/memory-analysis.md)
- [Frame profiler](frame-profiler.md)

## Gameplay allocation telemetry

Developer mode adds `GameplayMemory-<pid>-<date>-<time>.csv`. Six cumulative size buckets record
VirtualAlloc attempts, requested bytes and failed attempts at the five existing FastMemory allocator call sites.
Retries count as attempts. Requested bytes are allocation traffic, not live memory or unique committed bytes.
Coverage requires FastMemory to be running; `hook_active` records its state at the worker sample, not event time.
The game allocator big-block counters accompany summaries when readable; their meaning remains unverified.

A fixed 64-event queue records allocation failures with caller return address, request size/type, Windows error,
thread and process private/working-set bytes. Producers do not wait for the queue lock, allocate memory or write
files. Contention/full queues increment a loss counter. Save entry/result events reuse the existing MemoryGuard
hook, so save coverage requires MemoryGuard. The background worker drains events every 10 seconds; a crash or exit
before draining can lose the final events. Files stop at 16 MiB independently of the process summary file.

The observer preserves the allocator result and Windows last-error value. It does not add global heap hooks or
attribute native allocations to textures, Sims or script objects. A successful allocation can span reserve/commit
operations on the same region. Cross-thread cumulative counters are approximate concurrent observations.

Normal mode omits these development collectors and uses the separate [bounded memory controller](memory-budget.md).

## Development flavour measurements

Builds of the development flavour (`APEX_DEVELOPMENT_BUILD_FLAVOR`, [workflow.md](../workflow.md#development-switches-dev_build))
add three measurements. Public builds compile none of them.

- **Address-space owners and game allocation categories** (switch `measurements`). An address-space ledger
  (`features/va_ledger.cpp`) attributes reservations to their owners, and the game's named allocation entries
  (`0x004E3F90`, `0x004E3FB0`, free `0x004E3F70`) are counted by category (`features/game_alloc_categories.cpp`,
  Steam executable only). Only part of the game's allocations pass through those named entries; the rest of the
  allocator is called directly and is not attributed. These measurements cost frame time; like every switch they are on unless `[dev_build]` sets them to false.
- **Apex memory by owner** (switch `memory_owners`, `features/apex_memory.{h,cpp}`). Every `operator new` / `delete` of
  the ASI is counted with an 8-byte header (live bytes, blocks, session peak and the 40 largest calling sites, ImGui
  apart). The large containers of the lighting code are estimated from their capacity and the MSVC layout by the thread
  that owns them, at most every 10 s and right after an F8. Direct3D textures, render targets and shaders the ASI creates
  are counted at creation and subtracted when Direct3D destroys them, by pool. Results appear in an `[ApexMemory]` log
  line every minute and in the APEX RADIANCE MEMORY section at the end of the F8 lighting snapshot. Measuring never
  changes what the lighting computes. Driver and DXVK memory is not visible.
- **Debug buffers given back** (switch `release_debug_buffers`): the Light Probe's draw records and occlusion queries
  after a capture, a recording's lines and tracer states after it is saved, and the F8 light records when their
  recording is switched off ([light-probe.md](dev-tools/light-probe.md), [recorder.md](dev-tools/recorder.md)). The F8
  section lists what each gave back.
