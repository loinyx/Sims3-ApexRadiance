# Faster Sim Building: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/fast-cas-sort.md](../features/performance/fast-cas-sort.md).

### 2026-09-30: CAS triangle sort found by sampling

**Context:** the Frame Profiler's sampling run (10 minutes, Create a Sim and play): "CAS SimService" dominated 38
hitches of about 80 ms each, 3.0 s over the median. 83% of their render-thread samples were keyed
"TS3W fn~005D1010", with 0x005D38F8 on the stack in 84%.

**Finding:** the sampler guesses a function's start from the int3 padding before it, and FUN_005d1960 follows
FUN_005d1010's jump table with no padding, so its samples were booked to 005D1010. A read-only probe on 005D1010
(development build, removed again; the CAS fill probe was commit `f29f33d`) showed it cheap: 2566 calls, 84 ms in all,
writing plain private memory, not write-combined. 0x005D38F8 is the return address of `call 005D1960`, the triangle sort
"CAS/ModelBuilder/TriangleSortDataList": triangles x vertices steps of `divps` and `rsqrtps`, about 90 ms for a
2013-vertex, 3000-triangle part.

**Outcome:** commit `1720843`: the sort rewritten with the game's arithmetic (bit-identical, SSE over four vertices,
worker threads for large parts), checked against the game, with the offline harness `tools/cas_sort_test`; the CAS fill
probe removed. Offline: 1030 x 1500 21 to 1.5 ms, 2013 x 3000 88 to 2.5 ms, 3363 x 5000 242 to 5.2 ms (8 threads,
thread start included).

**Pitfall kept for the sampler:** a function key of the form "fn~ADDRESS" is a guess; confirm with the stack's return
addresses before attributing time to it.
