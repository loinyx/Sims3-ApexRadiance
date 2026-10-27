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

### 2026-10-05 and 2026-10-27: texture compositor readback without the GPU wait

**Context:** in the 2.16.2 Frame Profiler reports, 5 to 6 % of the render thread's samples in ordinary frames wait in
`SleepConditionVariableSRW` under the game's texture lock `0x00618DF0` (return `0x00618E5E`). The stacks hold
`0x005FDCB7`, `0x005FDE42`, `0x005FDFF1` and `0x006082FD`: the texture compositor's queue loop `0x00608270`, its
state 2 `0x005FDDE0` and `0x005FDBF0`, which copies the rendered tile into the builder's system-memory texture
(`GetRenderTargetData` in `0x005FBCE0`) and locks it at once (`0x005FDCB2`). The room solve's locks (`0x0069FAD9` and
the next four) and the wall ambient occlusion lock are not in those stacks. In DXVK 3.1.1, `GetRenderTargetData` marks
the system-memory texture for read-back (`SetNeedsReadback`), and `LockImage` then copies and waits for that copy,
behind every command recorded before it; `D3DLOCK_DONOTWAIT` is removed by DXVK.

**Tried (2026-10-05, `features/compositor_readback.cpp`, part of Faster Sim building):** after the tile's copy, an
EVENT query was issued and the queue stopped for the frame; the lock ran in state 3 of a later frame once the query
signalled. The bytes read are the game's. In game the textures of Sims and objects built more slowly: the queue only
works on its head builder, each tile is used before the next is rendered, so every deferred tile ended the queue's
time slice and the compositor finished one tile per frame instead of several.

**Outcome:** the part stays in the code, not started (commit `e62887c`). Two tiles in flight would need a second staging
texture and the use of tile k (`0x005FD420`) after the render of tile k+1, an order the game's state machine does not
have. Moving the lock within the frame does not shorten the wait, which covers everything recorded before the copy.
