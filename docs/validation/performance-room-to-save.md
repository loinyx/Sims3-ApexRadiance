# Room to Save: validation

The feature is described in [features/performance/room-to-save.md](../features/performance/room-to-save.md). It must:

- Hold only reserved address space (`MEM_RESERVE`, no commit) and release it right before every world save.
- Shrink the resource caches only through the game's own trim, on the thread of their update, idle entries only.
- Restore both redirected calls on Stop and leave no reserve behind.

## Automated tests

The production [memory-budget harness](memory-budget.md) checks reserve exclusion during native saves, failure-path
scope cleanup and pre-save buffer ownership without modifying the game.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-05 | `3328a4b`, `733a7c5` | Offline: the five sites in Steam TS3W.exe; signatures unique on Steam | Steam addresses and bytes checked; on the EA app build (code encrypted on disk) the signatures are confirmed only by the startup scan's log | None |

## In-game test plan

1. Start a world. **Expected log:** `[MemoryGuard] On: a reserve of 128 MB of address space for world saves`.
2. Save. **Expected:** `[MemoryGuard] World save done: largest free block N MB, M MB for the save (reserve let go, ...)`
   with M about 128 MB larger than N; the reserve is taken again later.
3. A long session that used to end with Error 12. **Expected:** the save succeeds, or the log shows how much room the
   save had.
4. EA app build. **Expected:** the startup scan resolves the `MemoryGuard` group and the same log lines appear.

## Confirmed in game

- Nothing recorded at publication.

## Open checks

- A save in a session that failed with Error 12 before.
- The EA app build in game.
- The idle cache size read for the log (`+0x98` of each cache) against the game's own statistics.
