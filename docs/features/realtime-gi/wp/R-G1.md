# R-G1 (offline part): gather batches of region-listed layers

- Status: done offline. plan.md's R-G1 is the in-game server solve on one lot (numbers and parity pictures, game jobs
  `realtime-gi-rooms`, `realtime-gi-pan`); this package covers its offline part, the scoped batch sizing that R-06 and
  S2-01b left to R-G1 ("scoped slices run 6-10 ms until R-G1 sizes scoped batches from the list length"). The in-game
  numbers are still R-G1's, see "In game".
- Branch: `gi/r-g1` (from `feature/realtime-gi` 92d21db).
- Deliverable: bridge patch `native/remix-bridge/patches/0023-bridge-gi-listed-gather-batches.patch`,
  `native/remix-bridge/server/gi_list_batch.h` (copied in by `build.cmd`), `tools/gi_server_test/list_batch_check.cpp`
  (run by `test-gi.cmd`), README row.

Marks: [V] verified by the checks named, [I] inferred, [O] open.

## Cause

- [V] Before 0023 every layer of a pass shares one slice size, `m_sliceUnits[pass]`, counted in columns (64 texels) x
  gathers. A layer's batch of gathers is `units / columns`, and each submission moves `units` by at most a factor of 2.
- [I] A listed layer (0021) has few columns (300-1000 texels: 5-16 columns on the lots of `rebake_check.cpp`), fewer
  than the GPU runs at once, so its gather costs about as much as a gather of a wave of columns. Counted in columns, its
  batch comes out several times too large: up to all 16 gathers in one submission. An unscoped layer (163 columns on the
  200-lamp lot) between two listed ones moves the shared size back up, and the factor-of-2 limit keeps the next listed
  layer over the budget.
- [V] The lots test alone does not show this reliably: its lamps are all listed, and on this machine the GPU was shared
  with another lane's game, so slice times moved by 2x between runs of the same build (table below).

## Change (patch 0023)

| Piece | What it does |
|---|---|
| `runSliced` | Listed dispatches use `m_listUnits[pass]`, full ones `m_sliceUnits[pass]`: neither learns from the other |
| Batched gathers of a listed layer | `GiListBatch` (`gi_list_batch.h`) counts whole gathers that fit `server.giSliceUs`, measured at a list length: kept for a shorter list, cut in proportion for a longer one. After a submission the size drops at once to what fitted and grows back by at most a quarter. A gather alone over the budget runs in slices as before and its time is learned too |
| Full layers | Unchanged rule |
| Log | Debug level: `[Gi] Lamp N gathers: <texels> listed texels, <columns> columns, <n> submissions, up to <g> gathers and <us> us each.` |
| `test_gi_device` mode `lots` | Logs at debug level, prints the longest slice a layer (median, p90, layers over 4000 us) |

## Checks

| Check | Result | Mark |
|---|---|---|
| `list_batch_check` (no GPU): the worker's rules on a cost model (a gather costs 8 us a column, at least a 48-column wave; lamps 0.8-1.25x), 200 lamps, longest submission after the first layer | All listed: shared rule 32 submissions over 4000 us, longest 7600 us; new rule 0, longest 2868 us. An unscoped re-bake every 2nd lamp: 95 over, 7552 us; new 0, 2460 us. Every 5th: 75 over, 7536 us; new 0, 2868 us. The new rule needs 15-20 % more submissions | [V] for the rules, [I] for the cost model |
| `list_batch_check`, unit cases | Batch for 4, 8, 16, 64 columns from 32 gathers at 8: 16, 16, 16, 4. A submission at 4x the budget drops 16 to 4; the next fast one grows it to 5 | [V] |
| `test-gi.cmd` modes on, compute, off, stock, lots, lots-full (system Direct3D 9 server) | See "Test run" | [V] |
| Layers of the lots | Hashes equal before and after (dd7e9d4c777ccc83, 8bb93645312ad583, 9a9a6866d31e2183); 0 lit texels outside a box | [V] |
| `ApexRadiance.sln` Release x86, developer and public (`APEX_NO_DEV_TOOLS`) | Built, 0 warnings (Apex code is unchanged) | [V] |
| `tools/gi_indirect_test`, `tools/rt_lamps_test` | See "Test run" | [V] |

Lots mode, longest slice a layer, same machine, GPU shared with another lane's game (two runs each):

| Lot | Before: median / p90 / layers over 4 ms | After |
|---|---|---|
| 60 lamps | 1236 / 1648 / 0; 1240 / 2527 / 0 | 1240 / 2454 / 0; 1100 / 2268 / 0; 1234 / 1757 / 0; 986 / 1457 / 0 |
| 120 lamps | 1939 / 2988 / 5; 1776 / 2828 / 2 | 1845 / 2926 / 0; 1708 / 2648 / 3; 1577 / 2496 / 1; 1269 / 2329 / 0 |
| 200 lamps | 1644 / 2822 / 6; 1992 / 3421 / 9 | 1340 / 2380 / 0; 1930 / 3195 / 4; 1877 / 2628 / 0; 1903 / 4013 / 20 |

- [V] Listed layers took 1.2-1.8 gather submissions a layer of up to 16 gathers (debug lines of the lots runs).
- [I] The over-4-ms layers after the change come from the shared GPU, not the size: the same build gave 0 and 20 on the
  200-lamp lot in two runs. Clean numbers need a quiet GPU (R-G1 in game, XR-09).

## Test run

- [V] `native/remix-bridge/test-gi.cmd` (fresh bridge pair from 0001-0023): `list_batch_check` passed; `test_gi_device`
  modes on, compute, off, stock, lots and lots-full passed with timeouts on and off ("GI server tests passed"). DXVK
  modes not run.
- [V] `0023` applies with plain `git apply` on a fresh patch-only tree of 0001-0022 (`PATCH_ONLY=1 build.cmd`). After merge round 5 the series is 0001-0024 (0024 is WP11).
- [V] `tools/gi_indirect_test/run.ps1`: 36 checks, 0 failures. `tools/rt_lamps_test/run.ps1`: 680480 checks, 0 failures;
  with `-Gi`: 680326 checks, 0 failures.

## In game (the rest of R-G1) [O]

- With `logLevel = Debug` in the server's `bridge.conf`, on a lot that mixes scoped and unscoped solves (a lamp edit
  re-bakes unscoped): `[Gi] Lamp N gathers:` lines with each submission under about 2x `server.giSliceUs` after the first
  layer, and the `RemixBridge_GiReadCounters` longest slice (`kSliceUsMax`) under 4000 us on most layers.
- No change to the layers: the pictures equal 0022's on the same lot (the layer hashes above are the offline proof).
- Still R-G1's: frames from edit to picture, GPU ms a slice with DXVK busy, render-thread ms, memory, parity pictures on
  every back end (plan.md P12, Q12).

## Open issues

- [I] The cost model's wave (48 columns) is a guess for an RTX 30 laptop GPU; the rule does not depend on it, only the
  check's numbers do.
- [O] Not run with the DXVK server renderer (`DXVK_X64` not set).
- [O] The direct pass of a listed layer is one slice of a few columns in every run seen; it now adapts on its own sizes.

## Unblocks

R-G1 in game (scoped and unscoped solves on one lot within the slice budget), S2-01b's scoped re-bakes after edits.
