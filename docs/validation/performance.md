# Performance: validation

Group-level checks for the Performance switches. Each switch has its own validation page (linked below); the group is
described in [features/performance/README.md](../features/performance/README.md). The group must:

- Load all twelve switches on when no saved choice exists, and keep an explicit saved `enabled = false`.
- Toggle all twelve from Overview > Performance, enabling *Faster game file lookups* before *Remember missing files* and
  disabling in the reverse order.
- Load a configuration that still contains `[ui] performance_mode`, ignore the key and drop it at the next save.
- Resolve every performance address on Steam 1.67.2 and refuse cleanly elsewhere.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `research\port169\sigcheck.pl` (outside the repository) | Every `GameAddr` signature matches once at the expected place, alternates included | Perl, against `TS3W.exe` | Game executable |
| `perl .claude/skills/apex-menu/audit.pl .` | Menu texts, translations, documented keys, ResetDefaults | From the repository root | Perl |

Per-switch harnesses (`tools/dxt_test`, `tools/refpack_test`, `tools/cas_sort_test`) are listed on their switch's page.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-09-29 | first performance round | `sigcheck.pl` | 140 of 140 ok, each alternate matches once at the same place | None |
| 2026-09-29 | round 3 ids | `sigcheck.pl` | 147 of 147 ok (every new primary signature and alternate) | None |
| 2026-09-29 | v1.5.0 merge with C6 / C8 | `sigcheck.pl` | 153 of 153 ok | None |
| 2026-09-29 | C6 lifetime hook ids | `sigcheck.pl` | 158 of 158 ok | None |

## In-game test plan

1. Delete the twelve `[patches.*]` tables of the switches, start the game. **Expected:** all twelve rows on.
2. Set one switch off, restart. **Expected:** it stays off; Overview > Performance shows off.
3. Overview > Performance off, then on. **Expected:** all twelve follow; no error notes under the rows.
4. Start with a 2.5.6 configuration that has `[ui] performance_mode`. **Expected:** loads; the key is gone after the
   next save.
5. Turn the Frame Profiler on and off with all switches on (either order). **Expected:** every switch keeps working; the
   profiler's Hooks table names the inner layers.

## Confirmed in game

- 2.5.3: the Performance page split into four cards; Experimental badges removed. This labelling change did not add
  gameplay validation.

## Open checks

- In-game visual and performance validation of the removal of the *Optimize rendering* mode (PR #2).
- Group switch, profile behaviour and default-on loading on PR #2 (not yet confirmed in game).

## Per-switch validation

[Room lighting](performance-room-light-queue.md), [lot lighting](performance-lot-lighting-motion.md),
[wall shading](performance-wall-shading-while-moving.md), [scene nodes](performance-scene-node-budget.md),
[file lookups](performance-resource-lookup-cache.md), [missing files](performance-remember-missing-files.md),
[file lists](performance-file-list-cache.md), [object lookups](performance-object-lookup-index.md),
[texture compression](performance-fast-texture-compression.md), [cache compression](performance-fast-cache-compression.md),
[Sim building](performance-fast-cas-sort.md), [memory](performance-fast-memory.md),
[Vulkan driver guard](performance-vulkan-driver-guard.md).
