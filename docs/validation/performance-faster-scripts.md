# Faster Scripts: validation

The feature is described in [features/performance/faster-scripts.md](../features/performance/faster-scripts.md). It must:

- Give every patched compare handler the same registers, x87 stack and flags as the original, and keep the bytes from the
  old return address on.
- Answer `mono_type_get_object` only with an answer the game gave for the same domain and type while that domain lives.
- Turn the type cache off on the first difference, and restore every patched byte and hook on Stop.

## Automated tests

`tools/gameplay_stability_test/run.ps1` extracts the production type-cache lookup, store and hooks with native-call
mocks. It checks the local/shared paths, generation changes, startup verification, TypeBuilder exclusion, mismatch
fallback and 400,000 concurrent operations. The separate policy executable checks CAS ordering against an independent
stable sort, zero allocations for ordered lists, and frame histogram/session boundaries.

## Latest results

2026-10-19: 10,010 type-cache checks and 400,000 concurrent operations pass; 3,008 CAS-ordering/frame-interval checks
pass. These verify native-call contracts with mocks, not live Mono behaviour or an FPS gain.

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-05 | `a1de9ab` | Offline scan of Steam TS3W.exe | 25 compare and branch handlers matched (0x00E54D1B..0x00E58304); no branch targets the inside of a block | None |

## In-game test plan

1. Start a world. **Expected log:** `[ScriptMath] 25 interpreter compare handlers test NaN inline (...)` and the type
   cache line.
2. Play an hour with an active household. **Expected:** no `[ScriptMath] A stored type object differs ...` error; at
   exit the summary shows answers from the cache and checks, all equal.
3. EA app build. **Expected:** the same lines.

## Confirmed in game

- Nothing recorded at publication.

## Open checks

- The effect on simulation time (Frame Profiler, simulation thread).
- The EA app build in game.

The same harness also passes 10,006 object-cache checks, including repeated lookup lock avoidance, live path mutations and generation invalidation. Native calls are mocked; gameplay performance remains unmeasured.

