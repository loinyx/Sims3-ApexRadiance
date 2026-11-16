# Real-time GI: the bridge server export

What the GI back ends `serverRays` and `serverCompute` (design §3.9) need from the RTX Remix bridge, read in the
bridge source at Apex's pin and in the DXVK release the package ships. The patch that implements this is R-03.

Sources, all read at fixed revisions:

- Bridge: `dxvk-remix` commit `f35b86a494712eba72177ddabe62a48ff4cb6e9c` (`native/remix-bridge/build.cmd`, `COMMIT`).
  Paths below are relative to `bridge/src/`. Line numbers are those of the pinned upstream file; Apex's patches
  0001-0014 shift some of them by a few lines.
- DXVK: release `v3.1.1` (commit `b1a1c99`), the server's renderer in Apex's package (`native/remix-bridge/README.md`,
  `package.ps1`). Paths are relative to the DXVK tree.
- The asynchronous read-back patch (research 11 §5.1, planned as patch 0015), from its work-in-progress source:
  `util/util_readbacks.h`, `server/async_readback.h`, `client/d3d9_async_readback.cpp`. Not yet committed; its
  statements are marked as such.

Marks: **[V]** verified in the named file and line, **[I]** inferred, **[O]** open, with the package that closes it.

## 1. What the server is

| Fact | Source | Mark |
|---|---|---|
| The server is a 64-bit process that plays the client's Direct3D 9 calls on a Direct3D 9 device. It never sees Vulkan directly | `server/main.cpp:3436-3480` `InitializeD3D` | [V] |
| With `server.useVanillaDxvk = True` (Apex's `conf/bridge.conf`) it loads `d3d9vk_x64.dll`, which the package fills with stock DXVK 3.1.1 | `server/main.cpp:3437-3445`; `native/remix-bridge/package.ps1` | [V] |
| The Remix API (`exposeRemixApi`) is only loaded when the server loads the Remix runtime's `d3d9.dll`; with vanilla DXVK it is unused | `server/main.cpp:3460-3470` | [V] |
| One thread plays all device commands, in order, from one FIFO; a second thread plays module commands (`IDirect3D9` queries) | `server/main.cpp:3768-3774`, `ProcessDeviceCommandQueue` at `:304` | [V] |
| The play loop blocks in `waitForCommand` while the client sends nothing; nothing else wakes it | `server/main.cpp:307`; `util/util_bridgecommand.cpp:162-250` | [V] |
| The server's device is created with the game's own `BehaviorFlags`; DXVK makes it thread-safe only with `D3DCREATE_MULTITHREADED` | `server/main.cpp:350, :392`; DXVK `src/d3d9/d3d9_device.cpp:55` | [V] |
| TS3 does not pass `D3DCREATE_MULTITHREADED`, so the server's device must only be called from the play thread | | [I]; R-03 logs the flags |
| All handle maps (`gpD3DDevices`, `gpD3DResources`, ...) are plain `std::unordered_map` globals without a lock, owned by the play thread | `server/main.cpp:157-166` | [V] |

Consequence: **every Direct3D 9 call of the GI work, including the write of results into Apex's textures, runs on the
play thread.** A worker thread may only touch its own Vulkan objects and shared memory.

## 2. How a client texture is named on the server

| Fact | Source | Mark |
|---|---|---|
| Every client object gets an id from a process-wide counter at construction | `client/base.h:304-309`, `:359`, `:372` | [V] |
| The client sends that id, not its pointer, as the handle of a new texture; the server stores the DXVK texture under it | `client/d3d9_device.cpp:615-620`; `server/main.cpp:798-812` | [V] |
| A surface level is its own client object with its own id; `GetSurfaceLevel` maps it on the server | `server/main.cpp:2098-2110` | [V] |
| `IDirect3DTexture9_Destroy` and `Bridge_UnlinkResource` remove the handle from `gpD3DResources` | `server/main.cpp:2044-2051`, `:2924-2929` | [V] |
| An export resolves the pointer the caller holds with `bridge_cast<Direct3DSurface9_LSS*>` and sends `getId()` | patch 0015 `client/d3d9_async_readback.cpp` | [V] in the uncommitted source |

So an export takes `IDirect3DTexture9*` from Apex (Apex creates its light buffers through the game's device, so it
holds the bridge's wrapper), resolves it to `Direct3DTexture9_LSS::getId()` in the client, and the server finds the
DXVK texture with `gpD3DResources[id]` on the play thread. Design §3.9 says "named by its client pointer": the
pointer is the export's argument, the id is what crosses the bridge.

## 3. Existing patterns the export reuses

| Pattern | Source | Use for GI |
|---|---|---|
| Named shared memory, name prefixed with the session GUID, zeroed by the first process to map it | `util/util_sharedmemory.cpp:32-73` | The GI block |
| Answer slots of 32-bit fields with `std::atomic<uint32_t> answeredSeq`, written last with release order; sequence 0 means unanswered | `util/util_queryresults.h:27-64`; server write at `server/main.cpp:2854-2869` | Ticket answers |
| `FlagBits::AsyncResult`: the server answers into a shared slot instead of a `Bridge_Response`, so the client never waits | `util/util_commands.h:875-885` | GI commands send no response |
| Bridge commands outside the Direct3D 9 interface (`Bridge_SharedHeap_*`, `Bridge_UnlinkResource`) | `util/util_commands.h:32-64`; `server/main.cpp:2902-2935` | New `Bridge_Gi*` commands |
| Exports by name in the client's `.def` (`remixapi_*` are listed there) | `client/d3d9_lss.def:21-22` | `RemixBridge_Gi*` exports |
| Per-pass poll on the play thread: every 64 commands and at `Present` | patch 0015 `server/async_readback.h` `onCommand`, `poll` | GI applies finished results at the same two places |
| Reset and device destroy drop pending work | patch 0015 calls `dropAll` in Reset, ResetEx, Destroy | GI does the same |
| Client and server must be the same build; the server exits on a version mismatch | `server/main.cpp:3666-3671` | New commands need no wire versioning; the block header still carries a layout version for Apex |

## 4. Device: a second Vulkan device, not DXVK's

| Option | What it allows | Verdict |
|---|---|---|
| **Second device**: own `VkInstance` and `VkDevice` on the same physical GPU, on a worker thread | Any extension, including `VK_KHR_acceleration_structure` and `VK_KHR_ray_query`; long dispatches never hold DXVK's queue lock | **Chosen** for both server back ends |
| Shared device through DXVK's interop (`ID3D9VkInteropDevice`) | Command buffers on DXVK's own device and queue, writing the light buffers' `VkImage` directly | Not for rays: stock DXVK 3.1.1 enables neither extension. Kept as a later zero-copy path for `serverCompute` |
| A patched DXVK with ray query in its device | Rays in DXVK's own shaders | Out of scope here; R-08 |

Facts behind the choice:

| Fact | Source | Mark |
|---|---|---|
| DXVK 3.1.1 lists no ray-tracing extension in its device extension table; ray query appears only as loader entry points | DXVK `src/dxvk/dxvk_device_info.h` (60 extensions, none ray), `src/vulkan/vulkan_loader.h` | [V] |
| `ID3D9VkInteropDevice` gives the instance, physical device and device handles, the queue and its family | DXVK `src/d3d9/d3d9_interfaces.h:138-158`; `QueryInterface` at `src/d3d9/d3d9_device.cpp:195` | [V] |
| Using DXVK's queue needs `FlushRenderingCommands`, `LockSubmissionQueue` and no Direct3D 9 call from the locking thread until `ReleaseSubmissionQueue`; layouts move with `TransitionTextureLayout` | DXVK `src/d3d9/d3d9_interfaces.h:170-205` | [V] |
| A texture's `VkImage`, layout and create info come from `ID3D9VkInteropTexture::GetVulkanImageInfo`; not for SYSTEMMEM or SCRATCH | DXVK `src/d3d9/d3d9_interfaces.h:84-99`; `src/d3d9/d3d9_texture.cpp:36` | [V] |
| Two Vulkan devices on one GPU run side by side; the GPU time-slices their queues | | [I]; R-03 measures the frame time with a busy worker |
| A long dispatch on the second device can delay DXVK's frame, since preemption of compute is not guaranteed | | [I]; R-02 and R-03 slice dispatches to about 1 ms |

How the second device attaches:

1. Lazily, on the first `Bridge_GiOpen` (section 6), the play thread starts the worker. A server that never sees the
   command costs nothing.
2. The worker loads `vulkan-1.dll` itself (DXVK has already loaded it into the process) and creates its own instance,
   so its lifetime does not depend on DXVK's.
3. It picks the physical device whose `deviceUUID` equals that of DXVK's: the play thread queries
   `ID3D9VkInteropDevice::GetVulkanHandles` once on the server's device (`gpD3DDevices`) and passes the UUID to the
   worker. Without the interop interface (a non-DXVK `d3d9.dll`) it takes the first discrete GPU and says so in the
   log. [I]; R-03 tests both paths.
4. It enables `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations` and buffer device
   addresses when the GPU has them (level `rays`), else only compute (level `compute`), and writes the level into
   the block header (section 5). A failure writes level `none` and the client falls back.

## 5. Upload path: from the worker into Apex's textures

| Option | Copies | Synchronisation | Verdict |
|---|---|---|---|
| **A. Host copy**: the worker reads its results into a host-visible buffer; the play thread copies them into a server-owned SYSTEMMEM surface and calls `UpdateSurface` into the target | One GPU-to-host read on the worker, one `memcpy` and one DXVK upload on the play thread | A fence wait on the worker; the play thread only sees finished data | **Chosen** for R-03 and R-04 |
| B. External memory: the target created by DXVK with an exported handle and imported into the second device | None | DXVK 3.1.1 exports only KMT handles and Direct3D 9 has no shared fence, so the worker would write a second copy and the play thread would swap them | Later, if A's play-thread cost exceeds its budget |
| C. Shared device (section 4) | None | Queue lock, layout transitions | `serverCompute` only, later |

Facts:

| Fact | Source | Mark |
|---|---|---|
| The bridge's own upload already locks a server surface on the play thread and copies rows (`IDirect3DSurface9_UnlockRect`) | `server/main.cpp:2623-2660` | [V] |
| `CreateTexture` on the server always passes `pSharedHandle = nullptr`, so option B needs a GI-specific create | `server/main.cpp:809` | [V] |
| DXVK accepts a shared handle only for `D3DPOOL_DEFAULT` and makes it an opaque Win32 KMT handle | DXVK `src/d3d9/d3d9_device.cpp:648-650`; `src/d3d9/d3d9_common_texture.cpp:411-417` | [V] |
| `UpdateSurface` from SYSTEMMEM into DEFAULT is a buffer-to-image copy recorded on DXVK's stream; it does not wait for the GPU | | [I]; R-03 times it |
| A light buffer of 40 k texels in `A16B16G16R16F` is 320 KB; a converged lot updates a few of them | design §3.9 numbers | [I] |

Rules for the targets (Apex's side, R-05):

- GI targets are `D3DPOOL_DEFAULT` textures without `D3DUSAGE_DYNAMIC`, created by Apex through the game's device.
  Apex never locks them: a client lock would send the client's copy and overwrite the server's write. [I] from the
  upload path above.
- They are released before a device reset like every DEFAULT resource, and named again after it.
- The play thread copies at most `server.giUploadBudgetKB` (default 1024) a pass, so one pass never stalls the
  command loop by more than about 1 ms (R-03's acceptance). [I]; R-03 sets the default from a measurement.

## 6. Commands and exports

Client exports, found by Apex with `GetProcAddress(GetModuleHandleW(L"d3d9.dll"), ...)`, all `__stdcall`:

| Export | Effect | Returns |
|---|---|---|
| `RemixBridge_GiOpen(uint32_t* pLevel)` | Maps the block, sends `Bridge_GiOpen` once, reads the level the server wrote (waits up to 100 ms on the first call only) | `S_OK` with `*pLevel` = 0 none, 1 compute, 2 rays; `E_NOTIMPL` when `client.giServer = False`; `D3DERR_DEVICELOST` without a server |
| `RemixBridge_GiSubmit(const void* data, uint32_t size, uint32_t kind, uint32_t* pTicket)` | Copies a scene, lamp table or texel-point list into a free submit slot, sends `Bridge_GiSubmit` with slot and sequence | `S_OK` and a ticket; `D3DERR_WASSTILLDRAWING` when all slots are busy |
| `RemixBridge_GiSolve(IDirect3DTexture9* target, uint32_t layer, uint32_t sceneTicket, uint32_t* pTicket)` | Names a target by id (section 2) and asks for a solve into it | `S_OK` and a ticket; `D3DERR_INVALIDCALL` for a texture that is not the bridge's or not DEFAULT |
| `RemixBridge_GiPoll(uint32_t ticket)` | Reads the ticket's answer slot | `S_FALSE` pending, `S_OK` done, the server's error, `D3DERR_INVALIDCALL` for an unknown ticket |
| `RemixBridge_GiCancel(uint32_t ticket)` | Frees the ticket; the server drops it when it sees it | `S_OK` |
| `RemixBridge_GiQueryPoints(uint32_t ticket, void* out, uint32_t size)` | Copies a finished point-query answer (R-06) | as `GiPoll` |

New bridge commands, all sent with `FlagBits::AsyncResult` so none waits for a response:

| Command | Payload | Play thread | Worker |
|---|---|---|---|
| `Bridge_GiOpen` | none | Starts the worker once, queries the interop UUID | Creates the device, writes the level |
| `Bridge_GiSubmit` | slot, seq, kind | Hands the slot to the worker | Builds or refits the scene, answers the seq when the slot can be reused |
| `Bridge_GiSolve` | target id, layer, scene seq, seq | Checks `gpD3DResources[id]`, records the target's size and format, queues the job | Solves, reads back into its host buffer, marks the job ready |
| `Bridge_GiCancel` | seq | Drops the job | Stops at the next slice |

Applying results: at `Present` (`server/main.cpp:724`) and every 64 commands, as patch 0015 polls, the play thread takes
ready jobs, looks the target up again by id, copies within the budget and `UpdateSurface`s, then stores the seq into
the answer slot with release order. A draw the client issues after `GiPoll` returns `S_OK` comes later in the FIFO
than that copy, so it samples the new contents [V: one ordered FIFO, `server/main.cpp:304-307`].

## 7. Shared block

One named mapping, `"Gi"` through `SharedMemory` (GUID-prefixed, `util/util_sharedmemory.cpp:33`), mapped by the
client on the first `RemixBridge_GiOpen` only. All fields 32-bit, so the 32-bit client and the 64-bit server see one
layout (the `QueryResultSlot` rule, `util/util_queryresults.h:29`).

| Offset | Field | Writer |
|---|---|---|
| 0 | `magic` `'GI01'`, `layoutVersion` | server |
| 8 | `level` (0 none, 1 compute, 2 rays), `capBits` (ray query, AS, float16 storage) | worker |
| 16 | `heartbeat`: incremented by the play thread every pass | play thread |
| 20 | `workerHeartbeat`, `lastError`, `jobsQueued`, `jobsDone`, `gpuUsLast` (counters for design §6) | worker |
| 64 | 64 answer slots: `answeredSeq` (atomic), `hresult`, `kind`, `size` | server |
| 1 KB | 2 submit slots of 448 KB: the scene (tier A proxy about 10 k triangles is about 360 KB as float3 positions plus 32-bit ids), lamp table, texel points | client |
| 897 KB | 1 point-query answer slot, 96 KB | worker |
| | Total about 1 MB of the client's address space | |

Sizes are [I] from design §3.9 and are fixed in R-03; a scene larger than a slot goes in parts with a part index in
the submit command. The 1 MB comes on top of patch 0015's 1 MB; both are mapped only when used (memory-gains note).

## 8. Ticket and reset rules

- A ticket is a client sequence number, never 0. It is answered once: `S_OK`, a failure, or `D3DERR_DEVICELOST`.
  A finished, failed or cancelled ticket is unknown afterwards (the same contract as patch 0015).
- Answers come in any order for different targets; a later solve for the same target and layer replaces an earlier
  one that has not been applied (the earlier ticket answers `S_OK` without a copy, flagged in `kind`).
- `Reset`, `ResetEx` (`server/main.cpp:678`, `:699`) and device `Destroy` (`:535`): the play thread drops every queued
  copy and answers those tickets `D3DERR_DEVICELOST`. The worker keeps its scene; Apex re-names its new textures and
  solves again.
- `IDirect3DTexture9_Destroy` and `Bridge_UnlinkResource` (`:2044`, `:2924`): queued copies for that id are dropped. A
  copy always looks the id up again just before it writes, so a reused id never receives a stale result. [I] ids are
  not reused in practice: the counter is 64-bit in the client (`client/base.h:306`) and sent as 32 bits.
- Lost server: the client export returns `D3DERR_DEVICELOST` when `gbBridgeRunning` is false; a `heartbeat` that has not
  moved for 1 s makes Apex fall back to `d3d9` (design §3.9 detection, R-05 watchdog).
- Worker failure (device lost on the second device, out of memory): the worker sets `level = 0` and `lastError`,
  answers open tickets with `E_FAIL`, and the client falls back. The game's rendering is unaffected.

## 9. Where the patch touches the bridge

| Place | Change |
|---|---|
| `util/util_commands.h:32-64`, `toString` | `Bridge_GiOpen`, `Bridge_GiSubmit`, `Bridge_GiSolve`, `Bridge_GiCancel` |
| `util/util_gi.h` (new) | Block layout and `static_assert`s, as `util/util_queryresults.h` |
| `client/d3d9_gi.cpp` (new), `client/d3d9_lss.def` | The six exports |
| `server/gi_worker.{h,cpp}` (new) | Worker thread, second Vulkan device, ray scene, solver kernels (R-04), host read-back |
| `server/main.cpp` command switch | The four commands; apply at `Present` and every 64 commands; drop on Reset, ResetEx, Destroy, texture destroy, unlink |
| `server/meson.build` | The new files and the SPIR-V build step (R-02's kernel) |
| `bridge.conf` | `client.giServer` (default off upstream, on in Apex's package), `server.giUploadBudgetKB` |

Patch 0015 changes the same switch cases (Present, Reset, ResetEx, Destroy). The GI patch is numbered after it and is
written on top of it; landing 0015 first avoids a rebase of both.

## 10. Open points

| Point | Closes in |
|---|---|
| TS3's `BehaviorFlags` on the server device (multithreaded or not) | R-03 log line |
| Two devices side by side: frame time of DXVK with the worker busy, and the slice length that keeps it | R-03, R-G1 |
| `UpdateSurface` cost on the play thread per 320 KB, and the budget default | R-03 |
| Matching the physical device by UUID through the interop interface | R-03 |
| Point-query and scene slot sizes | R-03, R-06 |
| Vertex buffers the server already holds as tier B input: they are in `gpD3DResources` and readable only on the play thread | R-07 |
| A same-device tier in a patched DXVK | R-08 |
