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

## Development flavour measurements

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-20 | Working tree on `c22e8d7` | `tools/memory_owners_test/run.ps1` | 41 checks passed: exact `operator new` counts (`new[]`, `nothrow`, ImGui), separate calling sites, container estimates within 35 bytes of the real count, report cadence, textures and render targets counted at creation and subtracted at destruction on a real device | Native D3D9 and DXVK |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/game_alloc_categories_test/run.ps1` | 21 checks passed under concurrency (103 bucket overflows counted) | None |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/memory_analysis_test/run.ps1` | Passed | None |
| 2026-10-27 | Working tree on `7f06996` | `tools/memory_analysis_test/run.ps1` | Passed: the game's domains read from two core rings, a second instance equal to the first, a ring broken by a concurrent change and an inconsistent script heap read; 39-column CSV | None |

The game's domain counters are not yet read in game from the Apex process; the layouts were read from outside the
running Steam 1.67.2 game ([engine page](../engine/native-allocation-and-script-memory.md#domains-measured-in-game)).

The public binary contains none of the accounting. In game, the owner table, the largest calling sites, the Direct3D
memory by pool and the amounts given back by `release_debug_buffers` are not yet measured.
