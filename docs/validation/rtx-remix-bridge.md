# RTX Remix bridge: validation

The bridge must run the game for an hour without a crash or a stall, keep `Options.ini`, show the same picture as
DXVK in-process, and leave the game more address space than DXVK in-process. Apex Radiance must see it and run every
feature under it. The feature is described in [features/rtx-remix-bridge.md](../features/rtx-remix-bridge.md).

Run ids (alNNN) and issue #40 are those of the sims3-modernization harness, where the port was made and measured
before its hand-over to Apex Radiance (rodneypantonial/sims3-modernization#316).

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| sims3-modernization game harness (`game-exe.ts`) | An old save ("1111 kit5", seed 12351) for 30 or 60 minutes, Apex off, unfocused, with memory samples (`diag: mem`, memcensus) | `TS3_BIN=<a copy of Game\Bin>`: `Game\BinBridge` (bridge) or `Game\BinBase` (in-process DXVK 3.1.1 x86) | The game, a GPU |
| `render-still` | Fixed scene: 90 s still at the save's camera, then 90 s at a town overview, ABBA | the same harness | The game, a GPU |
| `dxvkx` (scratch test app: an x86 window process and an x64 Desktop Duplication probe) | Present-to-desktop and key-to-desktop latency, 20 s runs, ABBA x2, V-Sync at 240 Hz | | A GPU |
| `tools/memory/nt-heap.py` (read-only heap walk) | The client's process heap by bucket | every 10 min in al325 | |

## Latest results

All memory runs: same ASI (main b1fc929), Apex off. "big" = the game's largest free block, MB.

### Without the copy fixes (30 min)

| diag: mem | al282 in-process | al281 bridge | bridge minus in-process |
|---|---|---|---|
| load: free / big | 1764 / 1645 | 2094 / 1997 | +330 / +352 |
| +30 min: free / big | 1341 / 1180 | 1527 / 1334 | +186 / +154 |
| priv | 2034 | 2050 | +16 |
| img | 358-408 | 146-196 | -212 |

By pool at the end of a 30-minute run (al285): MANAGED 585.5 MB, SYSTEMMEM 13.6 MB, DEFAULT 0.2 MB, buffers 50 MB; per
600 frames the MANAGED surfaces took 531 read-only locks and 11,021 relocks.

### With the copy fixes (30 min)

| Run | Options | Surface copies, MB | Buffer copies, MB | big at end | priv at end |
|---|---|---|---|---|---|
| al292 | none | 500-550 | about 50 | 1375 | 2113 |
| al300 | managed 30 s | 50-150 | about 50 | 1798 (+423) | 1728 (-385) |
| al313 | managed 2 s (+ cache shift 2) | 7.4 | 46.7 | 1816 | |
| al315 | managed 2 s + buffers 2 s (+ cache shift 2) | | 1.8-5.3 | 1937 (+121 vs al313) | |
| al295 | in-process (`BinBase`) | | | 1203 | 2047 |

Fetches are cheap: al312 (managed 2 s) 37,604 fetches, 435 ms in all; al315 18,584 buffer fetches, 247 ms in all,
0 CRC mismatches.

### 60 minutes (steady state from 3 min after the world load)

| Run | Options | big min / last | Client priv last | save-end priv / big / eaRes | Fetches (surface; buffer) |
|---|---|---|---|---|---|
| al318 | managed 2 s | about 1793, flat | | | |
| al317 | managed 2 s + buffers 2 s + keep-alive 10 s | 1895 drifting to 1741 | | eaRes +36 vs al318 | |
| al321 | managed 2 s + buffers 2 s | 1872 / 1877 | 1550 | 1578 / 1845 / 842 | 114,078 in 0.90 s; 40,149 in 0.54 s |
| al324 | as al321 + `freeIdleManagedCopiesMs = 500` | 1944 / 1944 | 1521 | 1521 / 1944 / 822 | 144,510 in 1.50 s; 39,929 in 0.52 s |
| al325 | as al321 (all patches) | 1832 / 1832 | 1595 | 1593 / 1832 / 838 | 120,797 in 0.86 s; 44,925 in 0.57 s |
| al326 | in-process DXVK, same profile | 1074 at save-end (census 1122) | no bridge client; game priv 2135 | 2135 / 1074 / 842 | none |
| al327 | as al325, diagnostics off (`perfLogIntervalFrames = 0`) | census 1897 at the end | not logged | 1611 / 1817 / 833 | not logged |

- What the bridge takes out of the game (census at about +55 min, al325 vs al326): committed -649 MiB (1873 vs 2522),
  largest free block +774 MiB (1896 vs 1122). By owner: image -216 (DXVK and both Vulkan drivers not loaded),
  unlabeled -336, process heap -155, pagefile mappings +78 (the bridge's shared memory); `ea:main` the same (634).
- The client's process heap with diagnostics on (al325): committed 46.5 MiB after the load, 77.8-80.5 MiB to the end,
  live blocks flat at about 30 MiB. The growth was fragmentation of the 32-byte bucket (29% in use), 97% of it patch
  0004's `unordered_map` nodes. With diagnostics off (al327) it ends at 63.9 MiB: the rest is the client's 296-byte
  objects (16,039 of 31,759 blocks in use) and about 30k empty maps.
- In-process, 116 MiB of the process heap is DXVK's pipeline manager: 6,464 blocks of 12,808 bytes
  (`m_shaderLibraries` nodes) and 6,594 of 5,896 bytes, about 18 KB per game shader.
- The server process (x64) stays at about 3.0 GB (al321 3048 MB, al324 3043 MB), flat after the load.

### Texture churn (30 min)

| Run | Option | Re-uploads after destroy, vs al309 | big at end | Cost |
|---|---|---|---|---|
| al310 | `ts3TextureCacheShift = 2` | -30% (<1 s: -50%) | 1805 (al309 1829) | none seen |
| al311 | shift 2 + `CacheOverflowBudget` 128 MB | -65% | 1285 | 454 MB of copies outlive the free |
| al313 | shift 2 + managed 2 s | -30% (<1 s: -54%), upload MB -23% | 1816 | none seen |
| al314 | `ts3TextureKeepAliveSec = 10` + managed 2 s | -58% (<1 s: -86%) | 1887 | eaRes +28 vs al313, server +0.9 GB |

Over 60 minutes the keep-alive costs more: al317 vs al318 halves re-creations (4,113 vs 8,142 per interval), but
`eaRes` ends +36 MB and big drifts down 154 MB.

### Latency (`dxvkx`)

| | in-process | bridge |
|---|---|---|
| Present to desktop, median (GPU-bound, 230 fps) | 8.69-8.74 ms | 12.86-12.90 ms (first run 16.97) |
| Key press to desktop, median, GPU-bound | 11.46-11.72 ms (3 valid runs) | 16.18-16.37 ms |
| Key press to desktop, median, CPU-bound (15 ms of work, 66 fps) | 26.62-28.03 ms | 27.24-28.66 ms |
| Key press to desktop p95, CPU-bound | 33.4-34.4 ms | 33.4-34.9 ms |

Throughput is the same (231-236 vs 232-238 fps). The client may run `presentSemaphoreMaxFrames` (1) frames ahead, which
a GPU-bound program always fills.

### Frame time (`render-still`, al296-al299, ABBA, every window at the 60 fps cap)

| Window | bridge (al296, al299) | in-process (al297, al298) |
|---|---|---|
| save camera, busy ms per frame | 6.67, 6.22 | 6.08, 6.09 |
| town overview, busy ms per frame | 6.10, 6.09 | 6.07, 6.06 |

In live play (al324, per frame): `GetRenderTargetData` 1.17 calls, 1.88 ms (mostly the CAS compositor's readback);
`CreateTexture` 2.23 calls, 0.10 ms; the rest under 0.03 ms. Frame times of live runs do not compare between runs (the
scene differs: 800-1,040 vs 360-440 draws a frame in al284 vs al285).

### Options tried

| Option [default] | Result |
|---|---|
| `clientChannelMemSize` [96 MB] | 32 MB: al305 passed, no gain in big (1806). al301 failed on a frozen heartbeat with a clean bridge (the main thread in a script wait loop) |
| `CacheBudget` (GraphicsRules.sgr) [200 MB] | 476 MB (al304): creates -13%, re-uploads -15%, big -47 MB |
| `sendCreateFunctionServerResponses` [True] | False (al286): no async create failure in 38 min; frame effect not measured in a fixed scene |
| `useSharedHeap`, `commandBatchingEnabled` [False] | not tried (upstream: a server crash is then not recoverable) |
| `alwaysCopyEntireStaticBuffer`, `client.optimizedDynamicLock` [False] | left off; upstream calls the second unsafe |

### Apex Radiance under the bridge

| Date | Build | Check | Result |
|---|---|---|---|
| | Apex 2.18.3 with patch 0010 (al361, 14.2 min) | Apex's picture gates | every gate open until the save (`applied at the scene end yes`, `RT0 back buffer`), no warning or error; the window showed the picture |
| 2026-11-09 | 3.0.0 release tree, Steam 1.67.2 | Day/night switch, a neighbouring lot's 6:00 lamp switch | without the earlier pauses |
| 2026-11-09 | 3.0.0 with the new `dxvk.conf` | A switch to exclusive fullscreen | no freeze (before: Reset refused, "alive losable resources: 1") |
| 2026-11-13 | 3.2.1 (published) | Turbo Simulation Engine V3.4 (`Turbo.asi` and its split packages), no Sims3SettingsSetter, in-game runner on a town save | hang at start: the client logged `no response from server` right after Turbo's second device, then `Still waiting on the Present semaphore`; the save never loaded |
| 2026-11-13 | release/3.2.2 c2697794, standard flavour | The same with Sims3SettingsSetter, the whole route (orbits, a pan over 8 lots, 7 min) | pass, no error in the log; `Second device asked with Turbo.asi loaded` once; 107 shaders from the ASI, 0 to compile |
| 2026-11-13 | release/3.2.2 c2697794, standard flavour | Without Turbo, same route | pass, no error, one device as before |

## In-game test plan

1. Install the zip into `Game\Bin`, start the game. **Expected:** `Options.ini` kept; Apex's log says `d3d9: RTX Remix
   bridge (DXVK in another process)`; Settings > Compatibility shows *RTX Remix bridge: Installed*.
2. Play an old save for an hour. **Expected:** no crash or stall; Apex's memory card keeps a large free block.
3. Switch Apex's window mode between Window, Borderless and Fullscreen, and change the resolution. **Expected:** each
   applies, no freeze.
4. EA app: the same with `client.targetProcess = TS3.exe`.
5. Start a recording, start a new save or add a lot so a loading screen shows, then stop. **Expected:** the recording
   folder holds `bridge32.log` and `bridge64.log` (and the DXVK log when present), listed in `About this capture.txt`;
   `Recording.txt` ends with a `No-scene frame trace` section with `[no-scene]` frames and a line per Apex pass that
   saved the vertex input.
6. With the 3.1.1 bridge (patch 0012), record from the main menu through a save's loading screen. **Expected:** no
   black quarter on the loading screen; in `Recording.txt`, `scene_target.cpp Resolve: N save(s), 0 STALE` (every
   other pass 0 STALE too). With an older bridge and Apex 3.1.1 STALE can still be counted (it is the client's answer),
   but the quarter stays drawn: the pass restores the FVF set last.

## Confirmed in game

- 3.0.0 under the bridge on Steam 1.67.2, with every Apex feature on (2026-11-09).
- The `dxvk.conf` setting ends the freeze on a switch to exclusive fullscreen (2026-11-09).

## Open checks

- Release 3.1.0 (commit `release: 3.1.0`): the release candidate ran under the bridge in game with Display set to Fullscreen as Borderless; the update card, the bridge guide and the uncompressed-textures card were not each exercised under the bridge on this machine.
- A report of walls flickering under the bridge with *Street lamps light lots* on (Steam 1.67.2, 3.0.0, session 2026-11-10 10-49-24); not reproduced or explained yet.

- The simulation clock stopped 60 s at 11 minutes once (al322, with `freeIdleManagedCopiesMs = 500`; both bridge logs
  stopped 86 s before, with no error). Not seen in other runs.
- `ts3TextureCacheShift = 2` for 60 minutes.
- The EA app build under the bridge on this machine.
- Sims3SettingsSetter's uncompressed lot and Sim textures under the bridge (reported to stutter).
- Which losable resource stays alive at a Reset.
- The black top-right quarter on loading screens (3840 x 2160, Borderless, S3SS loaded): the no-scene frame trace
  (session 2026-11-10 14-39-28) found the High-precision scene's resolve restoring a stale declaration, 248 STALE of
  249 no-scene frames. Bridge patch 0012 and Apex's vertex input restore are built; not yet run in game (test plan
  step 6).
- Patch 0011 (stale viewport after `SetRenderTarget(0)` or `Reset`): unverified in game that the sea no longer loses
  its reflection for a frame.
- Patch 0013 (idle copy fetches), as of 2026-11-11: unverified in game. Still to measure on a still lot: no blinking
  parts of the picture, and the game's free address space and largest block against the 3.1.0 bridge.
- `ApexRadiance.asi` 3.1.1 with the outdoor maps' room bases and UV maps in video memory (commit `94596c40`): the
  address space it frees under the bridge is not measured.
