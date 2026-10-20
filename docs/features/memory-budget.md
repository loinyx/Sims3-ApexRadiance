# Bounded memory handling

Bounded memory handling reduces idle native cache accounting under sustained address-space pressure and retires
unused Apex compressor buffers. It is an experiment, not a measured reduction in total game memory or an Error 12 fix.

## Status

Released in 2.14.0 with the standard build. Developer mode is off by default; normal mode leaves the Frame Profiler,
process-memory CSVs and lot-analysis pumping disabled. The controller uses the existing MemoryGuard worker,
with a four-second address scan and process counter query, and action-only log messages. No UI setting is added.

## The problem

Native cache trimming and Apex work-buffer retention have different ownership rules. The game owns resource entries;
Apex owns its compression contexts, token arrays, verification buffers and count/write copies. Automatic work must
respect those boundaries and avoid repeatedly evicting reusable data.

## How Apex Radiance solves it

MemoryGuard observes free totals and largest contiguous blocks, including the lower 2 GiB, with 64-bit walk arithmetic.
The pressure policy requires three stressed observations. A small lower-region block alone does not trigger it:
that condition also requires private memory of at least 2 GiB and lower-region free bytes below 192 MiB. Separate
conditions cover a largest overall block below 320 MiB, total free space below 512 MiB or a recent allocation failure
at an existing FastMemory hook. Recovery thresholds provide hysteresis. These are experimental thresholds.

On the verified Steam executable, its original ResourceSystem update thread can request a native Trim step of up to
16 MiB per cache, retaining a target of 64 MiB of idle accounting per cache. Entries are indivisible, so a native call
can exceed the requested step. No call is made if the native accounting is already over its own budget; the original
update handles that case. Requests are at least 120 seconds apart. Less than 1 MiB of accounting reduction or more than
8 ms elapsed causes a ten-minute backoff. Accounting reduction is not a measurement of physical RAM returned.
Unsupported or changed native code disables partial steps. The existing pre-save cache/reserve behaviour remains.

FastCacheCompression allocates its private work buffers top-down. Inactive caller slots and parallel token
arrays are released after 120 seconds, using the same exclusive ownership gates as compression. Under sustained
pressure, idle worker contexts are also released while the pool is closed and unused; the owner restores each needed
context before opening another round. Threads and their wake events remain alive. Count/write streams remain
protected in thread-local storage; after a write consumes the copy, sustained
pressure prevents keeping that completed copy. Active buffers and game-owned objects are never freed by this path.

FastTextureCompression places large verification scratch in the existing high allocator. A 64 MiB aggregate lease
limits concurrent verification scratch. The ordinary encode still runs when an extra comparison cannot obtain space;
this follows the existing allocation-failure fallback. Public builds keep initial compatibility comparisons and do not
honour development-only periodic RefPack comparison frequency from an old configuration.

## Settings

The bounded policies ship in the standard build independently of developer mode. Native cache control requires the existing MemoryGuard
switch. Buffer cleanup requires FastCacheCompression. Allocation-failure hints require FastMemory. Texture scratch
handling requires FastTextureCompression. Existing configuration keys and menu layout remain unchanged.

## Limitations

Runtime benefit and overhead still require comparison on identical gameplay paths. The experiment does not unload
active lots, reduce texture quality, modify Sims or script references, force garbage collection or change game save
serialization. Lot/resource retention is addressed only through native idle cache entries. A full resource-owner map
and a dedicated lot-unload policy remain research work. Top-down allocation prioritises high addresses but cannot
create new address space. Releasing work buffers can make the next compression allocate again.

## Technical reference

See [native cache evidence](../engine/native-memory-cache.md) and [validation](../validation/memory-budget.md).
The implementation uses `memory_guard.cpp`, `memory_policy.h`, `fast_refpack.cpp` and `scratch_budget.h`.

## Compression storage

Compression pool creation is bounded to useful work. Numbered test builds retain separate first-run instructions;
the stable build opens the ordinary changelog. Runtime FPS improvement and RAM savings remain unmeasured.

Active counting-run copies are bounded: admission is capped at 64 MiB in total, or 32 MiB
under pressure. Existing in-flight copies remain protected when pressure changes. A denied copy uses counting-only
compression and recompresses on write, preserving the stream contract at the cost of additional CPU work. Small
copies start at 64 KiB instead of 1 MiB. These bounds cover Apex buffers, not the game's total memory.

A valid copied write acquires no compressor context unless a verification check needs one. Small-stream compression
and verification allocate no segmented token array. Startup decode checks, source checksums, destination capacity
checks and identical compression output remain in place. This reduces avoidable setup and allocation work; it does
not move game-owned compression jobs to another thread or establish an FPS gain.

## Save preparation

The native world-save hook marks a save scope before releasing the address-space reserve. The watcher cannot retake
the reserve during that scope, and resource-update callbacks postpone additional asynchronous cache trims until it
ends. The existing synchronous native trim still requires the resource system's own update thread.

Preparation also drains FastMemory's queue of blocks already released by the game. New releases
use the original Windows call directly during the save. This returns eligible address ranges promptly without touching
live allocations. Inactive Apex compression slots, closed worker contexts and the current thread's completed copy are
released before the native save, without waiting for the ordinary idle timeout. Busy contexts and pending paired writes
remain protected. Count/write copy admission falls to 8 MiB in total during this phase; completed copies larger than
64 KiB are dropped. Denied copies use the existing counting and recompression path.

This hook covers the native world-save phase, not all earlier serialization. Action logs report released Apex capacity,
queued release count, cache-trim availability and private-memory endpoints. Endpoints do not measure the save peak.

On the verified Steam executable, an additional native dispatch wrapper starts preparation before the existing
world-save phase. It releases the reserve and inactive Apex compression storage, drains deferred releases and keeps
the smaller copy budget until the dispatch returns, including early error paths. Pending paired writes remain live.
The existing inner preparation still runs because the earlier phase can create more temporary storage.
Three inner stage wrappers preserve their results and report private-memory endpoints and elapsed time per stage.
The new sites require the Steam timestamp, argument/entry bytes and all four direct call targets; a mismatch retains
the previous world-save preparation. Other executables retain the existing signature-resolved paths.

Failed requests at the five existing FastMemory allocation hooks enter a fixed 64-event buffer, including requested
size, return site, Windows error and flags. The watcher drains it on its ordinary four-second tick. Successful requests
do not enter the buffer. A recovered retry can still have an observed failed request; this is not proof of a failed
save. Events can be dropped under contention, overflow or process termination, and allocations outside those hooks
remain unobserved.
