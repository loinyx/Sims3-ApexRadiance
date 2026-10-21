# External memory snapshot validation

## Offline and cross-process checks

The Win32 C++20 tool builds with `/LARGEADDRESSAWARE`, static CRT and psapi.lib. `--self-test <new-folder>` verifies a
64 MiB reservation with 8 MiB committed/touched, 56 MiB reserve-only, private resident-page attribution, an executable
mapped as a data file, full low/high range accounting and refusal of game-counter reads on a foreign executable.
A `--fixture` child holds the same allocations for a bounded interval. A separate observer verifies their exact
committed/reserved/resident values and mapped-file attribution through a read-only process handle. Tests write only a
few artifacts inside the explicitly selected new output directory; existing directories are refused.

2026-10-18: build has zero errors/warnings; self and external-process checks pass. Foreign-layout and missing-resource
pointer gates report unavailable data. Static executable checks confirm both allocator references, the Mono getters,
the optional heap limit and native top-down selection.

## Runtime checks pending

`session.ps1` takes a bounded series from one TS3W process identity and stops if that process exits. The default is six
snapshots at sixty-second intervals. Resident-page sampling is optional. `compare.ps1` exports Timeline.csv and
Comparison.json, preserving unavailable counters as blanks and grouping deltas by PID and process start identity.
Native allocator aliases are marked rather than summed. Raw cache accounting remains separate from process memory.
Synthetic checks cover counter deltas, allocator aliases, Mono subtraction and isolation across reused PIDs.

2026-10-19: the exact test.9 artifact adds a symbol-verified ResourceSystem pointer RVA behind its SHA-256 gate.
The capture script parses without errors and a live read validates the native ResourceSystem vtable and both cache
records. Both allocator globals are readable and point to different allocator objects in the observed session. Four
sequential snapshots complete; one includes resident-page queries with no query failures. These reads establish
counter availability, not reclamation effectiveness or Error 12 prevention.

Capture TS3W at world-ready, after a fixed lot route, after an idle interval, before save and after save. Compare matching
phases and note snapshot durations. Confirm the supported Apex cache-pointer/vtable path in a real session. Compare
process counters with Windows tools. Do not treat requested allocator bytes, heap accounting, committed address ranges
and resident pages as additive categories.
