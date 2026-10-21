# Memory budget validation

## Offline checks

`tools/memory_budget_test/run.ps1` builds production MemoryGuard, FastMemory and FastRefPack code with external hook/UI services
stubbed, plus the real codecs, as Win32 with `APEX_NO_DEV_TOOLS` and `APEX_MEMORY_TEST`.

Checks cover lower/high boundary splitting, invalid scans, pressure persistence and hysteresis, fragmentation without
corroboration, budget arithmetic overflow and native-over-budget suppression. A fake native thiscall-compatible Trim
checks the production call ABI and 16 MiB request. Scratch leases check concurrent-cap rejection and release.
Collector gates exclude public failure events and cumulative allocation accounting.

The compressor integration checks high-region allocations, busy-slot protection, idle retirement, reinitialization,
identical encoded bytes and round-trip data. Four concurrent compressor callers race against repeated accelerated
retirement passes. Thread-local completed copies are released under pressure. The production stream hook is also
called on fixture data: a counted copy writes with every pooled slot busy and no temporary context, insufficient
capacity preserves the destination guard, and startup decode verification still executes. A small copy uses 64 KiB
and a context without token storage. Eight competing 8 MiB copies admit four at the 32 MiB pressure limit and return
the budget after release. These checks do not execute inside the game.

## Latest result

2026-10-18: production integration checks pass. RefPack quick suite passes stream, round-trip, capacity, segmented and
threaded tests. DXT tests pass 100352 random blocks per format, edge sizes and parallel stress with identical blocks.
The Public test build completes with zero errors and zero build warnings. The standalone W4 integration harness also
reports existing codec constant-condition warnings.

Static executable validation confirms Trim entry/return, shrink arguments, native target subtraction and idle
accounting access. The partial step remains disabled on other executable timestamps.

## In-game checks pending

2026-10-19: the expanded native fixture preserves dispatch results 0, 9, 10, 11, 12 and 13, nested save scopes,
reserve exclusion and LastError. Stage wrappers preserve true/false, arguments and LastError. The bounded public
failure buffer rejects successful requests, records failure metadata, caps at 64 entries and reports overflow.
The four direct calls and dispatch argument/entry bytes match the on-disk Steam executable. Early preparation and
stage reporting remain unverified in an actual save; no reduction in peak memory or Error 12 rate is established.

2026-10-19: the save fixture passes real Windows allocation, deferred-release draining and direct release within a
save scope. A fake native save observes a released reserve; reserve reacquisition is refused until the scope ends,
including a failed native save. Compression preparation preserves a pending paired write and releases the completed
copy. A 9 MiB copy is refused during the save scope. These checks validate ownership and phase transitions, not a
reduction in a real save's peak.

An EA 1.69.47 startup log resolves the FastMemory, FastCacheCompression and MemoryGuard groups and reports all three
active. It records idle Apex buffer retirement and resource-cache actions. That session ends with an allocation
exception associated with a separate graphics module; it does not demonstrate Error 12 prevention or a successful save.
The partial native Trim step remains restricted to the statically checked Steam executable.

Compare the same save and camera route with the previous Public build and this Public test. Record loading, ten minutes
of lot visits, return, save and travel. Confirm action frequency, no visual corruption, no compression self-disable,
normal reload behaviour and matching saved state after reopening. Savings and Error 12 prevention are unverified.

## Interpretation

Native idle-accounting reduction is not OS RAM freed. Top-down placement addresses fragmentation pressure, not total
allocation size. An inactive slot release is a real VirtualFree of Apex-owned storage; its next use allocates again.
The experiment makes no ownership claim about native lot textures or script objects.

## Useful-work sizing

The production integration harness asserts that a three-piece stream creates at most two workers and three token
arrays, then races four compressing threads against accelerated idle retirement. Decoded data and stream bytes remain
identical after pool recreation. This validates work sizing and concurrency, not a gain in average gameplay FPS.

Worker contexts are retired in the pressure fixture as well: pointers and codec tables become empty, a subsequent
compression restores the contexts before a round opens, and decoded data and stream bytes remain identical. Four
concurrent callers also race the context-retirement pass. Maximum context storage released is six times ContextBytes
(3 MiB with the current codec), separate from token arrays. This is a bounded Apex-owned saving.
