# Read-backs under the RTX Remix bridge: prior art and a plan

Scope: the two `GetRenderTargetData` (GRTD) callers that cost time under the bridge.
- **Mouse pick** (`TS3W.exe+0x2DFF4A`, 16x16 target): 544-562 calls / 10 s, 1.35-1.40 s. 4.0.2 deferral: 290-306 calls,
  0.59-0.78 s (00-camera-pan-profile.md §6).
- **CAS texture compositor** (`0x5FBCE0`, 256x256 tile, only from `0x5FDBF0`): about 1.9 ms a frame in live play
  (rtx-remix-bridge.md). The deferred variant (`features/compositor_readback.cpp`) failed: one tile per frame (§9).

Labels: **verified** (read in source or measured), **inferred** (follows from source, not measured), **community**.
`research/09-salvage-apex-dropped.md` does not exist (checked 2026-11-17).

## 1. Why a read-back costs 2+ ms under the bridge, even after an event query signals

Read in the bridge source at Apex's pin `f35b86a` (`bridge/src/client/d3d9_device.cpp:886`,
`client/d3d9_surfacebuffer_helper.cpp copyServerSurfaceRawData`, `server/main.cpp:963, :228 ReturnSurfaceDataToClient`):

1. Client sends `IDirect3DDevice9Ex_GetRenderTargetData` into the FIFO command queue and blocks in
   `waitForCommand(Bridge_Response)` (**verified**).
2. Server reaches the command only after every command queued before it (**verified**: one FIFO). The client runs up to
   `presentSemaphoreMaxFrames=1` frame ahead, so this is "wait until the server has replayed the rest of the frame"
   (**inferred**).
3. Server calls GRTD on DXVK, then `LockRect(D3DLOCK_READONLY)` at once. DXVK makes that lock wait for the GPU copy and
   ignores `D3DLOCK_DONOTWAIT` on GRTD destinations: `d3d9_device.cpp` "We only ever wait for textures that were used
   with GetRenderTargetData ... Flags &= ~D3DLOCK_DONOTWAIT" (**verified**, DXVK master). So a flush plus a fence wait.
4. Rows are copied into the response blob, back into the client's surface copy (**verified**).

The 4.0.2 event query only proves step 3's GPU part is short. Steps 2 and 3's flush remain; the bytes (1 KB pick,
256 KB tile) are not the cost. Consequences:
- **Reading fewer pixels (lead 4b, "a few pixels around the cursor") saves nothing under the bridge.** The pick target
  is already 16x16 (banding-fix.md).
- **`D3DLOCK_DONOTWAIT` polling cannot work** for GRTD destinations on DXVK (above), and under the bridge the client's
  LockRect is local anyway.
- **Only an asynchronous GRTD in the bridge removes the cost**: the client must not wait for the server.

To check (cheap, no code): patch 0004's `[BridgeDiag]` already logs GRTD per caller on the client; add the server-side
time of the GRTD case to split step 2 (queue drain) from step 3 (DXVK wait).

## 2. Prior art

| Source | What it does | Use for us | License |
|---|---|---|---|
| [OBS `d3d9-capture.cpp`](https://github.com/obsproject/obs-studio/blob/master/plugins/win-capture/graphics-hook/d3d9-capture.cpp) | Ring of `NUM_BUFFERS=3` SYSTEMMEM surfaces, GRTD then `IDirect3DQuery9` EVENT `Issue(END)`; each Present polls `GetData(0,0,0)` and locks only the slot that signalled, two frames later | The classic D3D9 async read-back ring. Native D3D9 drivers queue GRTD and wait at the lock, so this works there; under DXVK the wait is also at the lock; under the bridge GRTD itself blocks (§1), so the ring must live **in the bridge server** | GPLv2: pattern only, no code |
| Bridge's own async queries (`client/d3d9_query.cpp getDataAsync`, `server/main.cpp:2854`, `bridge_util::QuerySharedBlock`) | Client sends `GetData` with `FlagBits::AsyncResult` + slot + sequence and returns; server writes the result into a shared-memory slot and publishes `answeredSeq` (release) | **The template for an async GRTD**: same flag, same slot/sequence handshake, bigger slots | MIT (upstream) |
| [Unity `AsyncGPUReadback`](https://docs.unity3d.com/ScriptReference/Rendering.AsyncGPUReadback.html), [UE `FRHIGPUTextureReadback`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RHI/FRHIGPUTextureReadback) | Request now, poll `done` / `IsReady()` frames later, never block the game thread; N-frame latency accepted | API shape for the bridge export: `Begin` returns a ticket, `Poll` returns pending / ready | Concept only |
| [GameDev.net 572092](https://www.gamedev.net/forums/topic/572092-getrendertargetdata---does-it-block/) | GRTD finishes queued rendering then copies; double buffering advised | Confirms the native-D3D9 behaviour (**community**) | n/a |
| Bridge / Remix trackers | One web search found no issue or PR for async GRTD or read-back speed; remix 0.4.x notes only touch GRTD thread safety ([remix-0.4.1](https://github.com/NVIDIAGameWorks/rtx-remix/releases/tag/remix-0.4.1)) | Our patch would be the first; worth offering upstream (MIT) like 0001 | n/a |
| [van Waveren 2006, Real-Time DXT Compression](https://www.quaddicted.com/webarchive/mrelusive.com_20170131/publications/papers/Real-Time-Dxt-Compression.pdf); [van Waveren & Castaño 2007, Real-Time YCoCg-DXT](https://developer.download.nvidia.com/whitepapers/2007/Real-Time-YCoCg-DXT-Compression/Real-Time%20YCoCg-DXT%20Compression.pdf) (GPU fragment-program encoder) | BC1/BC3 encode on the GPU, one 4x4 block per output pixel | See §4: does not remove the read-back in D3D9 and is not bit-identical to the game's encoder | Paper code samples: check the paper's terms before copying |
| CPU ray-cast picking (Unity `Physics.Raycast`, UE line traces) | Pick against collision/bounding volumes on the CPU | Rejected for TS3, §3 | n/a |
| TS3 caches ([EA troubleshooting](https://help.ea.com/en/help/the-sims/the-sims-3/troubleshoot-the-sims-3-pc/), [MTS Clean Start](https://db.modthesims.info/d/623008/ts3-clean-start-for-steam-and-origin-pc-only.html)) | `compositorCache.package` (objects) and `simCompositorCache.package` (Sims) store composited textures on disk (**community**) | One reason the compositor needs CPU bytes; DXT encoding (Apex's `DxtEncode1/5` sites) is the other | n/a |

No Sims 3 mod found (S3SS, NRaas, Smooth Patch, S3PP) that changes the pick or the compositor read-back itself. S3SS and
Apex's own work touch the CPU encoders and the cache compression only (performance/README.md).

## 3. Mouse pick: options

| Option | Saves (10 s far pan) | Risk | Verdict |
|---|---|---|---|
| A. **Async GRTD in the bridge** (patch, §5) + Apex issues it on the frame the pick is drawn, completes the game's read-back when the slot is ready | 0.59-0.78 s (4.0.2) to about 0.02 s: about 60-75 ms/s of render thread (**inferred**: one enqueue, ~30-50 µs, per pick) | Pick result 1-2 frames old, as 4.0.2 already allows (up to 4). Falls back to 4.0.2 when the export is missing (DXVK, native) | **Do first** |
| B. Skip the pick while mouse, camera matrix and buttons are unchanged, refresh every 4th frame | Up to 75% of picks while idle; little during a pan | Hover over a Sim walking under a still cursor lags by ≤ 4 frames; any button change forces a pick | Apex-only fallback for non-bridge users; second |
| C. Read fewer pixels (sub-rect / StretchRect to 1x1) | ~0 under the bridge (§1) | n/a | Drop |
| D. `D3DLOCK_DONOTWAIT` polling | 0: DXVK forces the wait on GRTD targets | n/a | Drop |
| E. CPU ray-cast against bounds | All of it | Sims are skinned and objects have thin parts; the game uses an ID buffer for exact per-pixel picks. Needs the scene graph, a full re-implementation, and loses pick precision | Drop (quality) |
| F. `RenderPickDelayReadback=0` | Worse: read-back in the same frame | n/a | Drop |

## 4. Compositor: options

| Option | Saves | Risk | Verdict |
|---|---|---|---|
| G. **Async GRTD in the bridge + rotate-on-wait in the queue loop**: at `0x005FDC8A` issue the async read-back; at `0x006082F8`, while that builder waits, step the **next** builder instead of returning 2 (stop), so several tiles are in flight; complete each tile in state 3 when its slot is ready | ~1.9 ms/frame of live play and the CAS hitches; throughput = tiles in flight per frame instead of 1 | Needs RE of `FUN_00608270`'s queue (head-only stepping, builder order, thumbnail priority): **unverified**. Per-tile latency 1-2 frames | **Do second**, after a ts3-static-re pass on the queue |
| H. Async GRTD only, keep the 2-stop of the current deferred variant | Main-thread cost gone | One tile per frame again: the §9 failure | Only as a measurement arm |
| I. Batch tiles into one atlas read-back | One round trip per frame | Same sequential-builder problem as H unless combined with G; redirecting viewports into an atlas needs more RE | Later, only if G leaves round trips that matter |
| J. GPU DXT encode in ps_3_0 (Waveren/Castaño) | CPU encode time; read-back bytes /4 to /8 | D3D9 cannot alias a render target as a DXT texture, so a read-back (and an upload) stays; under the bridge the round trip, not the bytes, is the cost. Not bit-identical to the game's encoder: breaks Apex's "same result" rule (performance/README.md) | Drop |
| K. Keep composited textures on the GPU (no read-back, no DXT, no disk cache) | Read-back + encode + cache write | The game's texture objects, ResourceCache accounting and `*CompositorCache.package` expect CPU bytes; large RE; uncompressed textures would live in the 64-bit server only (address space fine) but every load recomposes | Long-term idea, not now |

## 5. Plan

### 5.1 Bridge patch `0015-bridge-async-render-target-reads` (MIT, Apex's `native/remix-bridge`)

- Client exports, found by Apex through `GetProcAddress(GetModuleHandleW(L"d3d9.dll"), ...)`:
  - `HRESULT RemixBridge_ReadRenderTargetAsync(IDirect3DSurface9* rt, IDirect3DSurface9* dst, uint32_t* ticket)`:
    sends `IDirect3DDevice9Ex_GetRenderTargetData` with `FlagBits::AsyncResult`, a slot and a sequence; returns at once.
  - `HRESULT RemixBridge_PollReadback(uint32_t ticket)`: `S_FALSE` pending; `S_OK` after copying the slot into `dst`'s
    client copy (the same `LockRect(DISCARD)` + row copy as `copyServerSurfaceRawData`); error on failure or a lost slot.
- Shared block modelled on `QuerySharedBlock`: 4 slots x 256 KB + header (1 MB in the 32-bit client; acceptable,
  memory-gains rule: say so in the notes). Larger reads use the old synchronous path.
- Server: on the async command, GRTD into the server's `dst`, `Issue(D3DISSUE_END)` on a per-slot event query, add to a
  pending list; once per command-loop pass (and at Present) poll `GetData(nullptr, 0, 0)`; on `S_OK`, `LockRect(READONLY)`
  (no wait now), copy rows into the slot, store `answeredSeq` with release. The server thread never stalls either.
- `bridge.conf`: `client.asyncReadback = True` (default on in Apex's package; off upstream). The game's own GRTD calls are
  unchanged: only callers of the export go async.
- Device reset / surface destroy: drop pending slots for that surface; `PollReadback` returns `D3DERR_INVALIDCALL`.
- Self-test: a bridge-side test client that reads back a known `ColorFill` pattern through both paths and compares bytes.

### 5.2 Apex pick (`features/pick_readback.cpp`)

1. Learn the pick pair once: the existing GRTD caller hook (`framework/d3d9_extra_hooks.cpp`, caller `TS3W+0x2DFF4A`)
   records `(rt, dst)`; re-learn after reset.
2. `Hook_Update`, read-back due, first time: if the export and the pair exist, `ReadRenderTargetAsync`, return (instead of
   the event query). Else today's 4.0.2 path.
3. Later frames: `PollReadback`; `S_FALSE` and < `kMaxWaitFrames` waits: return. `S_OK`: set a thread flag and call the
   original update; the GRTD hook sees caller `0x2DFF4A` + flag + same pair and returns `D3D_OK` without forwarding (the
   bytes are already in `dst`). Timeout or error: original path, synchronous as today.
4. Stats: async issued / ready / timed out / fallback, and GRTD ms for `0x2DFF4A` (target ≤ 0.05 s / 10 s).
5. Test: far-pan harness run D vs E vs async; correctness: hover highlight and click on a moving Sim, Build-mode drag.

### 5.3 Apex compositor (`features/compositor_readback.cpp`)

1. ts3-static-re pass on `FUN_00608270`: queue layout, whether stepping a non-head builder is safe, priorities.
2. At `0x005FDC8A`: issue the async read-back instead of GRTD + event query.
3. At `0x006082F8`: while the head waits, step the next ready builder (G); fall back to returning 2 if rotation is unsafe.
4. State 3 (`0x005FDFFF`): `PollReadback`; `S_OK` resumes `FUN_005fdbf0` with the read-back skipped, as today.
5. Arms: game as is / H / G; measure CAS hitch ms, tiles per second, time to finish a Sim's textures in CAS.
   Correctness: textures byte-identical to the synchronous path (hash each tile's locked bytes in both arms).

### 5.4 Non-bridge users

- Native D3D9 and plain DXVK: the OBS ring pattern already works because the wait is at the lock. The 4.0.2 event
  query does that for the pick; the compositor's deferred variant did it but lost throughput, so it also needs G.
- Option B (pick skip while idle) is the only pick gain that also helps there.

## 6. Order and effort (wall clock)

1. Bridge patch 0015 + self-test: about 2-3 h (one agent build ~30-60 min, plus a review).
2. Apex pick integration + far-pan harness: about 1-2 h, one harness run.
3. Queue RE for G: about 1 h static. Compositor G + arms: about 2-3 h, two harness runs.
4. Option B: about 30 min, one run.

Expected total: pick about 60-75 ms/s of render-thread time back on the far pan; compositor about 1.9 ms/frame in live
play plus fewer CAS hitches (both **inferred** until measured).
