# RTX Remix bridge: DXVK in a 64-bit process

> Ported on 2026-11-09 from sims3-modernization (rodneypantonial/sims3-modernization#316, by alexvixell35-stack), whose
> maintainers handed it over to Apex Radiance. Build and package: `native/remix-bridge/` (`build.cmd`, then `package.ps1`).
> Run ids (alNNN) and "#40" below refer to that project's harness reports and issue. Apex Radiance 3.0.0 works under the
> bridge with or without patch 0010 (its back-buffer alias in `framework/d3d9_bootstrap.cpp`).

Status (2026-11-08): it runs, and 60-minute old-save runs pass. Two client fixes free the CPU copies that ate the
bridge's gain:
- MANAGED texture copies (patch 0003);
- static buffer copies (patch 0008).

With both at 2 s, the largest free block ends a 60-min run at 1877-1944 MB (al321, al324). The in-process run ended
30 min at 1203 MB (al295). §3 has the numbers, §7 what is left. Owner: alexvixell35-stack. Source and build:
`native/remix-bridge/`. Run reports: #40.

The bridge is NVIDIA's RTX Remix bridge (`dxvk-remix/bridge`, MIT). An x86 `d3d9.dll` client inside TS3W forwards
every D3D9 call over shared memory to `.trex\NvRemixBridge.exe`, an x64 process. With `server.useVanillaDxvk = True`,
that process renders through stock DXVK x64 (`.trex\d3d9vk_x64.dll`) and presents into TS3W's window. The renderer's
memory (the DXVK image, its heaps, the Vulkan driver) leaves the 32-bit address space.

In Apex Radiance 3.0.0 the bridge is a separate download of the release (`Apex-Radiance-RTX-Remix-bridge.zip`, built by
`package.ps1`). Settings > Compatibility shows *RTX Remix bridge: Installed* under it, and while DXVK runs inside the game it
offers the download under *Optional* (`BridgeSuggested` in `apex_gui.cpp`); there is no start note. Credits: Settings > About
and the README (NVIDIA, dalexlol and alexvixell35-stack, DXVK).

Tags: [measured] = harness or test-app run on this machine (run ids given); [verified] = checked in the source or the
binaries; [inferred] = reasoning only.

## 1. Setup

- Build: `native\remix-bridge\build.cmd`. It fetches `dxvk-remix` at f35b86a (sparse: `bridge`, `public`, `src/util`)
  and Detours at ea6c4ae, applies `patches\` in order, and builds x86 and x64 with meson. The patch list and the
  internal names the #40 reports use are in `native/remix-bridge/README.md`.
- Deploy only into a **copy** of Bin (the runs used `Game\BinBridge`):
  - client as `d3d9.dll`;
  - `.trex\NvRemixBridge.exe`, plus DXVK x64's `d3d9.dll` as `.trex\d3d9vk_x64.dll`;
  - `conf\bridge.conf` in `.trex`. The client reads its options from there too, not from Bin: its log says
    `Found user config file: ...\.trex\bridge.conf` [measured, al292: an option only in Bin's copy was ignored];
  - `conf\dxvk.conf` in Bin.
- The harness runs a copy with `TS3_BIN=<copy>` (`game-exe.ts`).
- `conf\dxvk.conf` matters. DXVK's built-in TS3W profile (vendor 10de, device 1080, "Geforce GTX 580") matches the
  exe name, and the server is `NvRemixBridge.exe`, so the profile does not apply. The game then sees a new
  `lastdevice` and resets Options.ini to defaults (1024x768, texture quality 1). The server reads `dxvk.conf` from its
  working folder, which is Bin, not `.trex` [measured, 2026-11-07].

## 2. Bugs found

| What | Cause | State |
|---|---|---|
| AV reading 0x0 at `0x60F299` after 6.4 min (al280) | `Device_Clear 0x60F270` calls `GetDepthStencilSurface` (+0xA0), tests only `hr < 0` and reads the surface. The client returned `S_OK` + NULL when no depth-stencil was set; d3d9 returns `D3DERR_NOTFOUND`. `GetRenderTarget` did the same on an empty slot. [verified] | fixed: patch 0001. al281 then passed 30 min [measured]. Root cause and diff posted on upstream rtx-remix#1028 (`TS3.exe` EA App build, crash at 0x610329), which may be the same crash [inferred]. |
| The game ends with 0xC0000409 right after a long log line (al306, 7.7 min) | upstream: the client's `Logger::emitLine` copies each line into a static 4096-byte buffer with `sprintf_s`, which fast-fails on a line of 4 KB or more [verified] | fixed: patch 0005. Not reported upstream yet. |
| Options.ini reset to defaults | the server's DXVK missed the TS3W profile (§1) | fixed by `conf\dxvk.conf` |
| Blurry, pixelated picture in a Bin copy | TS3W ran DPI-unaware (`GetProcessDpiAwareness` 0, client 1707x1067 = 2560x1600 at 150%). Windows' `HIGHDPIAWARE` compat flag for TS3W is set per exe path (the player's `Game\Bin\TS3W.exe` only), and the runs had `client.enableDpiAwareness = False`. [measured] | `conf\bridge.conf` keeps the default `True`. |
| Black window with Apex Radiance 2.18.3 | Apex's log kept `world no / draws 0 / depth writes 0 (needs 48)` all session, after `INTZ depth swapped in` and `d3d9: not DXVK`; the bridge logs showed no errors. Cause: the client's `initImplicitRenderTarget` made render target 0 a separate place-holder surface linked to the back buffer (`LinkBackBuffer`), so `GetRenderTarget(0)` and `GetBackBuffer(0, 0)` returned two objects. d3d9 and DXVK return the same one. Apex's PostScene counts scene draws only while render target 0 is the back buffer, compared by pointer, so it never saw the scene. [verified in source] | fixed: patch 0010. al361 (Apex 2.18.3 on, 14.2 min, PASS): every Apex picture gate open from 13:22:42 to the save at 13:32 (`applied at the scene end yes`, `scene on the shared depth`, `RT0 back buffer`, 60 lines, no WARN or ERROR); the window showed the picture [measured]. Posted on loinyx/Sims3-ApexRadiance#59. |
| `fetched 8 bytes expected 4` errors (582 in al300) | the server pads small replies; the client copies the smaller size, so nothing is overrun [verified] | quiet since patch 0004 |
| Sim clock stuck 60 s at 11 min (al322) | not known. Both bridge logs stopped 86 s before, with no error; frames had taken about 0.5 s each since. Memory was fine. | open. Seen once in 2 runs with `freeIdleManagedCopiesMs = 500` (al324 passed 62.6 min with a watcher that never fired), never without it. That option stays off. |
| S3SS borderless does not apply | S3SS logs "Borderless fullscreen mode applied" but no Reset reaches the server; the game stays at the Options.ini size | open; the same thing showed in the in-process run, so it may not be the bridge [unverified] |

## 3. Memory [measured]

All runs: old save "1111 kit5", seed 12351, Apex off, unfocused, `Game\BinBridge` (bridge) or `Game\BinBase`
(in-process DXVK 3.1.1 x86). Same asi (main b1fc929). "big" = largest free block, MB.

### 3.1 Without fixes, the copies use the gain

al282 = in-process, al281 = bridge, 30-min profile:

| diag: mem | al282 in-process | al281 bridge | bridge minus in-process |
|---|---|---|---|
| #1 load: free / big | 1764 / 1645 | 2094 / 1997 | +330 / +352 |
| #7 +30 min: free / big | 1341 / 1180 | 1527 / 1334 | +186 / +154 |
| #7 priv | 2034 | 2050 | +16 |
| img | 358-408 | 146-196 | -212 |

At load the bridge frees about 350 MB. The client then keeps CPU copies ("shadows") of what the game locks:
- every locked surface, freed only at release;
- every vertex and index buffer, allocated at create.

By `D3DPOOL` (al285, end of a 30-min run): MANAGED 585.5 MB, SYSTEMMEM 13.6 MB, DEFAULT 0.2 MB, SCRATCH 0, buffers
50 MB. TS3 writes a MANAGED texture level once (`DDS_UploadLevels 0x60D290`, docs/40); per 600 frames those surfaces
take 531 READONLY locks and 11,021 relocks.

### 3.2 The fixes

- **Patch 0003, `client.freeIdleManagedCopiesSec`:** each Present (at most one pass per second), the client frees the
  copy of a MANAGED level that has not been locked for N s. The next lock fetches the level back from the server
  (the server answers that `LockRect` with the whole surface). Copies of 64 KB and more come from `VirtualAlloc`, so
  a free gives the address space back.
- **Patch 0008, `client.freeIdleBufferCopiesSec`:** the same for a static (non-DYNAMIC) vertex or index buffer whose
  copy is 64 KB to 4 MB. A lock then fetches the buffer back with a READONLY lock on the server. A CRC32C taken when
  the copy was freed checks each fetch.

30-min runs:

| Run | Options | Surface copies, MB | Buffer copies, MB | big at end | priv at end |
|---|---|---|---|---|---|
| al292 | none | 500-550 | about 50 | 1375 | 2113 |
| al300 | managed 30 s | 50-150 | about 50 | 1798 (+423) | 1728 (-385) |
| al313 | managed 2 s (+ cache shift 2) | 7.4 | 46.7 | 1816 | |
| al315 | managed 2 s + buffers 2 s (+ cache shift 2) | | 1.8-5.3 | 1937 (+121 vs al313) | |
| al295 | in-process (`BinBase`) | | | 1203 | 2047 |

Fetches are cheap. al312 (managed 2 s): 37,604 fetches in the run, 435 ms in all. al315: 18,584 buffer fetches,
247 ms in all, 0 CRC mismatches.

60-min runs (steady state from 3 min after the world load):

| Run | Options | big min / last | Client priv last | ts3native save-end priv / big / eaRes | Fetches (surface; buffer) |
|---|---|---|---|---|---|
| al318 | managed 2 s | about 1793, flat | | | |
| al317 | managed 2 s + buffers 2 s + keep-alive 10 s | 1895 drifting to 1741 | | eaRes +36 vs al318 | |
| al321 | managed 2 s + buffers 2 s | 1872 / 1877 | 1550 | 1578 / 1845 / 842 | 114,078 in 0.90 s; 40,149 in 0.54 s |
| al324 | as al321 + `freeIdleManagedCopiesMs = 500` | 1944 / 1944 | 1521 | 1521 / 1944 / 822 | 144,510 in 1.50 s; 39,929 in 0.52 s |
| al325 | as al321 (build with all patches) | 1832 / 1832 | 1595 | 1593 / 1832 / 838 | 120,797 in 0.86 s; 44,925 in 0.57 s |
| al326 | in-process DXVK (`BinBase`), same profile | 1074 at save-end (census 1122) | no bridge client; TS3W priv 2135 at save-end | 2135 / 1074 / 842 | none (no bridge) |
| al327 | as al325, diagnostics off (`perfLogIntervalFrames = 0`) | census 1897 at the end | not logged | 1611 / 1817 / 833 | not logged |

- Buffer copies: 42 MB (al318) vs 1.9 MB on average (al321). `nt-heap:process` (memcensus) grew +84 MiB in al318 and
  +44 MiB in al321 over the hour.
- **Most of that remaining process-heap growth is our own diagnostics (patch 0004).** al325 walked the heap every
  10 min (read-only, `tools/memory/nt-heap.py`, PR #315):
  - Committed 46.5 MiB 2 min after the load, 70.2 at +12 min, then 77.8-80.5 MiB to the end. The busy LFH blocks
    stayed at about 30 MiB all run.
  - 05:28 to 05:38: committed +7.6 MiB (70.2 to 77.8), but the live blocks went +5.75 / -6.21 MiB (net -0.46).
  - The growth is fragmentation in the LFH 32-byte bucket. In those 10 min it took 84,623 new blocks and freed
    74,431, and its user blocks grew +2.58 MiB to 14.4 MiB. Only 29% of its blocks were in use (134,855 of 458,113).
  - 97% of its live blocks are `unordered_map<uint64, uint64>` nodes: next, prev, a 64-bit key, and a GetTickCount64
    ms value (or ms << 8) inside the tables' 60 s / 300 s windows. Those are 0004's upload, content and released
    tables.
  - So `conf\bridge.conf` ships `perfLogIntervalFrames = 0`.
  - al327 (diagnostics off, same profile and census): the process heap ends at 63.9 MiB instead of 79.4, and the
    32-byte bucket is no longer among the biggest. It still grows 41 to 64 MiB, flat from about +40 min. The rest is
    fragmentation in the client's own buckets: 296 B (`d3d9.dll` objects, 16,039 of 31,759 blocks in use) and
    72 / 40 B (about 30k empty `unordered_map`s, flat). The save-end numbers match al325 within the spread
    (1611 / 1817 / 833 vs 1593 / 1832 / 838).
- **What the bridge takes out of TS3W** (last census sample, about +55 min, al325 vs al326): committed -649 MiB
  (1873 vs 2522), largest free block +774 MiB (1896 vs 1122). By owner: image -216 (DXVK and both Vulkan drivers are
  not loaded), unlabeled -336, `nt-heap:process` -155, `mapped:pagefile` +78 (the bridge's shared memory); `ea:main`
  is the same (634). Growth over the 50 min was about the same in both (+189 / +187 MiB committed, mostly `ea:main`).
- In-process, 116 MiB of the process heap is two block sizes whose counts match the game's 6,462 shaders: 6,464
  blocks of 12808 B and 6,594 of 5896 B (with a `d3d9.dll` vtable), about 18 KB per shader. The 12808-byte blocks are
  `DxvkPipelineManager::m_shaderLibraries` nodes [verified by layout: next pointer, key {capacity 4, size 1, shader},
  then `m_device` and `m_manager` (the same in every node) and the key copy; a 41 KB bucket array holds them]. Each
  holds a `std::optional<DxvkPipelineBindings>`, about 12 KB on x86 because of 23 inline
  `small_vector<DxvkShaderDescriptor, 32>`. `native/dxvk` patch 0001 (separate PR) keeps 4 inline.
- The 500 ms threshold frees 4x as many copies for about the same MB. Its gain (+67 MB big, -29 MB client) is
  inside the spread between runs: `eaRes`, which the bridge does not touch, differs by 20 MB between al321 and al324.
  It stays off (and see the al322 stall in §2).
- The server process (x64) sits at about 3.0 GB with these options (al321 3048 MB, al324 3043 MB), flat after the
  load.

**Defaults** (`conf\bridge.conf`): `client.freeIdleManagedCopiesSec = 2`, `client.freeIdleBufferCopiesSec = 2`.

### 3.3 Texture churn: the ResourceCache trims textures it counts in full

With the copies gone, the next cost is churn. About 2,300 textures per 600 frames are destroyed by
`ResourceCache_Trim 0x732E90` (via `ResourceManager_Update`), and about 70% come back within minutes (al309, patch
0004 destroy sites) [measured]. `Texture_CacheSizeBytes 0x619DB0` counts a MANAGED texture's pixels in full against
the 200 MB `CacheBudget`, although under the bridge those pixels live in the server [verified]. Trim evicts while
held + flush > budget - reserve, and the call is forced or the flush list is at least the overflow budget (+0x90)
[verified, 0x732F39-0x732F5B]. The same mechanism showed in-process (al194, `CacheOverflowBudget` 64 MB).

Two TS3-only options test it; both are off by default.

| Run (30 min) | Option | Re-uploads after destroy, vs al309 | big at end | Cost |
|---|---|---|---|---|
| al310 | `ts3TextureCacheShift = 2` (patch 0006) | -30% (<1 s: -50%) | 1805 (al309 1829) | none seen |
| al311 | shift 2 + `CacheOverflowBudget` 128 MB | -65% | 1285 | cached textures outlive the copy free: 454 MB of copies |
| al313 | shift 2 + managed 2 s | -30% (<1 s: -54%), upload MB -23% | 1816 | none seen |
| al314 | `ts3TextureKeepAliveSec = 10` (patch 0007) + managed 2 s | -58% (<1 s: -86%) | 1887 | eaRes +28 vs al313, server +0.9 GB |

Over 60 min the keep-alive costs more: al317 vs al318 halves re-creations (4,113 vs 8,142 per interval), but `eaRes`
ends +36 MB and big drifts down 154 MB. Shift 2 has not had a 60-min run.

## 4. Latency [measured]

`dxvkx` test app (scratch tool, x86 window process plus x64 Desktop Duplication probe). Its quad matches TS3's
device setup. 20-s runs, ABBA x2, vsync at 240 Hz.

| | in-process | bridge |
|---|---|---|
| Present to desktop, median (GPU-bound, 230 fps) | 8.69-8.74 ms | 12.86-12.90 ms (first run 16.97) |
| Key press to desktop, median, GPU-bound (F24 polled at frame start) | 11.46-11.72 ms (3 valid runs) | 16.18-16.37 ms |
| Key press to desktop, median, CPU-bound (15 ms of work per frame, 66 fps) | 26.62-28.03 ms | 27.24-28.66 ms |
| Key press to desktop p95, CPU-bound | 33.4-34.4 ms | 33.4-34.9 ms |

The bridge adds one refresh (+4.2 to 4.6 ms) only when the GPU or vsync is the limit. The client may run
`presentSemaphoreMaxFrames` (default 1) frames ahead, and a GPU-bound app always fills that slot. When CPU-bound, as
TS3 is, the difference is +0.5 ms, inside the run-to-run spread. Throughput is the same (231-236 vs 232-238 fps).

## 5. Frame times [measured]

Frame times of live runs do not compare between runs. The scene differs from run to run (al284 vs al285: 800-1,040 vs
360-440 draws per frame, 3.52 ms per frame apart with only DPI awareness and counters changed). Other programs on the
machine also run beside some tests.

The fixed-scene repro `render-still` (pause, 90 s still at the save's camera, then 90 s still at a town overview) ran
ABBA in al296-al299: bridge, base, base, bridge. Every window ran at the 60 fps cap, so busy time per frame (frame
minus idle) is the comparison:

| window | bridge (al296, al299) | in-process (al297, al298) |
|---|---|---|
| save camera, busy ms per frame | 6.67, 6.22 | 6.08, 6.09 |
| town overview, busy ms per frame | 6.10, 6.09 | 6.07, 6.06 |

So the bridge costs under 0.6 ms per frame in a fixed scene.

In live play the bridge blocks the game on a few calls (al324, per frame, steady state):
- `GetRenderTargetData`: 1.17 calls, 1.88 ms. The CAS compositor's readback (`0x5FBCE0`, called only from `0x5FDBF0`,
  256x256 A8R8G8B8) is most of it: 1.25 calls and 1.96 ms per frame in al286;
- `CreateTexture` 2.23 calls, 0.10 ms; the rest under 0.03 ms.

al286 ran `sendCreateFunctionServerResponses = False` (creates do not wait for the server): no async create failure in
38 min, and no frame-time conclusion (a live run).

## 6. Settings tried

From `bridge/src/util/config/global_options.h` and `client/client_options.h` [verified, defaults in brackets]:

| Option | Result |
|---|---|
| `clientChannelMemSize` [96 MB] | 32 MB: al305 passed, no gain in big (1806). al301 failed on a frozen heartbeat with a clean bridge (the main thread sat in a script wait loop), so not the channel [measured]. |
| `CacheBudget` (GraphicsRules.sgr) [200 MB] | 476 MB (al304): creates -13%, re-uploads -15%, big -47 MB. Not the main churn cause (§3.3); not recommended [measured]. |
| `sendCreateFunctionServerResponses` [True] | False (al286): no failures logged; frame effect not measured in a fixed scene. |
| `useSharedHeap` [False], `commandBatchingEnabled` [False] | not tried. Shared heap segments are 256 MB mappings inside TS3W too; upstream warns a server crash is then not recoverable. |
| `presentSemaphoreMaxFrames` [1] | keep 1: more frames ahead means more latency (§4). |
| `alwaysCopyEntireStaticBuffer`, `client.optimizedDynamicLock` [False] | leave off; upstream calls the second unsafe. |
| `logLevel` [Info], `perfLogIntervalFrames` | for play: `Warn` and 0 (the shipped conf has 0; with 0004's diagnostics on, the process heap fragments, §3.2). |

## 7. Next

| # | What | Why |
|---|---|---|
| 1 | The client's 296-byte objects (half their LFH blocks free) and the 30k empty maps per resource | §3.2: with the diagnostics off (al327) the process heap still grows 41 to 64 MiB. A pool for the 296-byte objects, or maps created on first use, would keep it near its post-load size. |
| 2 | Run `ts3TextureCacheShift = 2` for 60 min | memory-neutral in 30 min (al313), churn -30% |
| 3 | CAS compositor readback, 1.9 ms per frame | serve it a frame late, or keep that texture in the server |
| 4 | Report the logger bug (patch 0005) upstream | it ends any game that logs a 4 KB line |
