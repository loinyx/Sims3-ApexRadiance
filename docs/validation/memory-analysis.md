# Memory analysis validation

## Offline checks

Run `tools/memory_analysis_test/run.ps1` in the x86 MSVC environment. The test compiles the production collector with
stubbed logging, paths and game-address resolution. It verifies complete scans, process/system/I/O query success,
the low/high partition sum, a 64 MiB reservation, an 8 MiB commit and release. Game address reads are disabled.

## In-game checks pending

Compare the process counters with Windows tools. Record a clean world load, several minutes of gameplay, a save,
travel and return, and a large-world session. Confirm that the CSV remains readable, scan durations stay modest,
version strings agree and lighting/rendering behavior remains stable. No performance improvement is claimed.

## Interpretation

Compare matching milestones across repeated sessions with one change at a time. A rising private-byte total is a
research signal, not a leak diagnosis. Attribute memory only after validating allocation call sites or retention
paths against the executable and relevant mods.

## Gameplay instrumentation checks

The production-header harness checks size buckets, successful and failed attempt accounting, failure metadata,
last-error preservation, bounded queue overflow and draining, and writes the gameplay CSV. Runtime coverage,
thread overhead, save peaks between markers, and allocation ownership remain open checks. No automatic cache
policy is enabled by this instrumentation.
