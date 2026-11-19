# Read-back benchmark

A one-shot measurement of what reading GPU results back and a few device calls cost on the render thread, under the
renderer in use (the RTX Remix bridge or an in-process one). It answers whether a read-back costs its bytes or the
renderer's backlog, whether a second read-back right after a first is cheap, and whether `Get*` calls and event
queries are answered without a round trip. It changes nothing in the game.

## Status

| | |
|---|---|
| Availability | Developer build with developer mode on; the players' build compiles none of it |
| Default | Off |
| Menu | None |
| Configuration | `[readback_bench]` in `ApexRadiance.toml` |
| Source | [`features/readback_bench.cpp`](../../../features/readback_bench.cpp), [`features/readback_bench.h`](../../../features/readback_bench.h) |

## How it works

1. At start-up, `run = true` arms it: a Present callback is registered (log: `[ReadbackBench] Armed:`).
2. Once a world has been shown for `delay_s` seconds (`WorldSession::InWorld`), every `every_frames`-th Present runs
   one case, in turn, so each read-back case starts with an ordinary frame's backlog. Samples pause while no world is
   shown. Surfaces and the event query are created before the first sample; creates are not measured.
3. After `iterations` samples of each case the table goes to the log and to `Diagnostics\ReadbackBench-<pid>.csv` in
   the Apex Radiance folder, every surface is released and the callback removes itself. A device reset aborts it.

Cases (QPC, microseconds; render targets are `D3DPOOL_DEFAULT` A8R8G8B8, filled from the back buffer by `StretchRect`
just before each read-back so the copy depends on the frame's draws):

| Case | What is timed |
|---|---|
| `GetRenderTargetData` 1x1, 4x4, 64x64, 256x256, 1024x1024, back-buffer size | One read-back into a system-memory surface |
| Pair 4x4 then 1x1 | Two read-backs back to back, each timed |
| Back-buffer size then 4x4 | A full-size read-back, then a 4x4 one |
| `StretchRect` back buffer to 4x4 | The copy alone, and the copy plus the 4x4 read-back |
| `LockRect` 256x256 system memory | After a read-back and an event query: with the query just issued, and after it signalled |
| Event query | `Issue`, the first `GetData` (flush), the wait and number of polls until it signals (100 ms limit), one `GetData` once signalled |
| `GetRenderState`, `GetTexture`, `GetVertexShaderConstantF` (4 registers) | Mean of 1000 calls |
| `SetVertexShaderConstantF` 1, 16, 64 registers | Mean of 100 calls through the original method, alternating two values so a renderer cannot drop repeats; registers 0..63 are set back afterwards |

The `Done` line also counts how often the first `GetData` was already signalled, how often the query had signalled
before the "just issued" lock, query waits that timed out and failed calls.

## Settings

| TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|
| `run` | bool | false | | Arms the benchmark for this session |
| `iterations` | int | 60 | 5 .. 1000 | Samples per case |
| `every_frames` | int | 4 | 1 .. 600 | One case every N frames |
| `delay_s` | int | 20 | 0 .. 600 | Seconds a world must be shown before the first sample |

With the defaults the run takes 14 x 60 x 4 = 3360 frames after the delay. The in-game runner job
`tools/ingame_runner/jobs/readback-bench.json` sets these keys, pauses the game with a still camera and waits for the
table.

## Limitations

- Each sample adds its own cost to that frame: a back-buffer-size read-back is a hitch while it runs.
- The back-buffer-size surfaces hold one more back buffer of address space (client side) until the run ends.
- `GetRenderTargetData` and `StretchRect` pass through Apex's own detours (observers only), as the game's calls do.
- `GetVertexShaderConstantF` under the bridge answers from the client's copy; the registers set back are those values.
