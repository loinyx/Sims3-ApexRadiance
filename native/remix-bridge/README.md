# remix-bridge

NVIDIA's RTX Remix bridge (`dxvk-remix/bridge`, MIT, https://github.com/NVIDIAGameWorks/dxvk-remix) with our patches,
used to run TS3's DXVK in a 64-bit process. Ported from sims3-modernization (#316, by alexvixell35-stack), whose
maintainers handed it over to Apex Radiance. Findings, numbers and next steps: `docs/features/rtx-remix-bridge.md`.

- `build.cmd` fetches the pinned upstream commit into `src\`, applies `patches\` in order and builds `out\d3d9.dll`
  (x86 client) and `out\.trex\NvRemixBridge.exe` (x64 server). Needs git, VS 2022 C++ build tools and meson + ninja.
- `conf\`: `bridge.conf` (next to `d3d9.dll` and in `.trex`) and `dxvk.conf` (in Bin; it repeats DXVK's TS3W profile,
  without which the game resets Options.ini).

Patches. Each applies after the ones before it. "Internal" is the name the #40 run reports use.

| Patch | Internal | What | Default |
|---|---|---|---|
| 0001 | 0001 | `GetRenderTarget` / `GetDepthStencilSurface` return `D3DERR_NOTFOUND` on an empty slot (TS3 crashed on `S_OK` + NULL). Posted on rtx-remix#1028. | always |
| 0002 | 0002 | log-only counters of the client's CPU copies of locked surfaces and buffers (`BridgePerf` lines) | with `perfLogIntervalFrames` |
| 0003 | 0003-0005 | `client.freeIdleManagedCopiesSec`: free a MANAGED texture level's client copy after N s without a lock, and fetch it back on the next lock. Also adds per-pool copy counters and `GetRenderTargetData` caller lines. | conf: 2 |
| 0004 | 0006-0010 | `[BridgeDiag]` lines: memory and live objects, frame outliers, upload repeats (full-data CRC32C), re-uploads after destroy, and creation / destroy / lock sites named by the game's callers | with `perfLogIntervalFrames` |
| 0005 | 0011 | logger: a log line of 4 KB or more no longer ends the game with 0xC0000409 (upstream bug) | always |
| 0006 | 0012 | `client.ts3TextureCacheShift` (TS3W 1.67 only): the ResourceCache counts a MANAGED texture's pixels as bytes >> N | off |
| 0007 | 0013 | `client.ts3TextureKeepAliveSec` (TS3W 1.67 only): unforced ResourceCache trims keep a texture for N s | off |
| 0008 | 0014 | `client.freeIdleBufferCopiesSec`: free a static vertex / index buffer's client copy (64 KB to 4 MB) after N s without a lock. A CRC32C checks each fetch. | conf: 2 |
| 0009 | 0015 | `client.freeIdleManagedCopiesMs`: 0003's threshold in milliseconds | off |
| 0010 | 0016 | render target 0 is the implicit swapchain's back buffer 0 object, so `GetRenderTarget(0)` and `GetBackBuffer(0, 0)` return the same surface, as in d3d9 and DXVK (upstream made a separate place-holder; Apex Radiance compares the two by pointer and drew a black picture) | always |
| 0011 | | render target 0 also resets the viewport and the scissor rectangle, as d3d9 does | always |
| 0012 | | `SetFVF` and `SetVertexDeclaration` replace each other, as in d3d9 (a stale declaration stayed bound) | always |
| 0013 | | idle copy fetches: a freed MANAGED level is always fetched, a lock fails when no whole copy came back, texture fetches are CRC32C-checked, only copies of 64 KB and more are freed | with 0003 / 0008 |
| 0014 | | a surface's new client copy starts zeroed, as a new d3d9 or DXVK texture does (some lots drew their ground black: the game uploads part of a terrain overlay map it never wrote) | always |
| 0016 | | render target 0 is not counted as a reference to the device (0010 left it in the device's count of implicit references, so an AddRef and Release pair destroyed the implicit swapchain and the next call crashed). From the rewind work, without its test hunk for 0015; the GI test apps need it | always |
| 0017 | | `client.asyncReadback`: client exports `RemixBridge_ReadRenderTargetAsync`, `RemixBridge_PollReadback` and `RemixBridge_CancelReadback` read a render target (up to 256 KB) without waiting for the server: the server answers through 4 shared slots once an event query shows the copy done. The game's own `GetRenderTargetData` calls stay synchronous. Test: `test-async-readback.cmd`. Numbered after 0015-0016 of the rewind work and applies with or without them | conf: on |
| 0018 | | `client.giServer`: real-time GI server. Client exports `RemixBridge_GiOpen`, `RemixBridge_GiSubmit`, `RemixBridge_GiSolve`, `RemixBridge_GiPoll`, `RemixBridge_GiCancel` and `RemixBridge_GiReadCounters`. The first open starts a worker thread with its own Vulkan device on DXVK's GPU (ray query when present). A submit sends the scene (occluder triangles, texels, reflectance) and the lamp table; a solve computes one lamp's direct or bounce layer in GPU slices of `server.giSliceUs` (2000), and the server's command thread copies the result into the named DEFAULT-pool texture with `UpdateSurface`, at most `server.giUploadBudgetKB` (1024) a pass. Needs Vulkan-Headers, volk and glslang, which `build.cmd` fetches. Test: `test-gi.cmd`. Design: `docs/features/realtime-gi/server-export.md`, `docs/features/realtime-gi/wp/R-04.md`. Applies after 0017 (checked without 0015-0016) | conf: on |
| 0019 | | Per-pixel shadow pass. Client exports `RemixBridge_GiPixelOpen`, `RemixBridge_GiPixelPass` and `RemixBridge_GiPixelReadCounters`. When the server's renderer is the DXVK of `../dxvk` (patches 0003 and 0004) on a device with ray queries, a pass records a ray-query compute kernel into the renderer's command stream: it samples the INTZ depth texture, traces the lamps of the client's block against its capsules and boxes and fills four quarter-resolution `A16B16G16R16F` render target textures, which a draw sent after the pass samples. Any other renderer answers the open with an off state. The pass itself is `gi/gi_pixel_pass.h` with the block layout `gi/gi_pixel_block.h` and the kernel `tools/gi_server_test/pixel_shadow.comp`; `build.cmd` copies the three into the source tree. Test: `test-gi-pixel.cmd`. Applies after 0018 |
| 0020 | | Mesh casters. Client export `RemixBridge_GiObjects` sends placed objects (object id, LITE instance, 3x4 model-to-world matrix, removed flag) to the GI worker, which indexes the game's packages once (`server.giMeshGame`, `server.giMeshMods`) with `server/ts3_meshes.h`, resolves each LITE to its model's caster mesh and reports words 14-21 of `RemixBridge_GiReadCounters`. The per-pixel pass builds one bottom level per model and adds the 1024 objects nearest the camera after the capsules and boxes, as delta casters whose reach the server's grouped bounds give (up to 32 after the block's casters), so a lot without Sims traces them too. `build.cmd` copies `ts3_meshes.h` into the source tree. Test: `test-gi-mesh.cmd`. Applies after 0019 |
| 0021 | | GI region scope, touch boxes and point queries (R-06): a lamp with a region box solves only the texels in it (`server.giListDispatch`, default True); each settled solve writes the lamp's touch box, read with `RemixBridge_GiReadTouch`; `RemixBridge_GiQueryPoints` / `RemixBridge_GiReadPoints` return the irradiance at up to 256 points from the settled layers, each lamp weighted. Blobs holding a NaN or an infinity are refused. Test: `test-gi.cmd` (modes lots, lots-full). Design: `docs/features/realtime-gi/wp/R-06.md`. GI block layout version 3 (keeps 0020's counter words 14-21; 0019's GiPixel block is separate). Applies after 0020 | conf: on |
| 0022 | | Mesh read-back for the Direct3D 9 shadow tiles. Client exports `RemixBridge_GiMeshFetch` and `RemixBridge_GiMeshRead`: the GI worker resolves a LITE through 0020's package index and writes its model's caster mesh into a 256 KB mapping of its own, one fetch in flight; counter word 21 counts the answers. Apex keeps the meshes in a budgeted cache (`features/mesh_caster_set.h`). Test: `test-gi-mesh.cmd`. Applies after 0021 |
| 0023 | | Gather batches of region-listed layers. Listed dispatches (0021) keep their own slice sizes, and their batched gathers count whole gathers (`server/gi_list_batch.h`, copied in by `build.cmd`): kept for a shorter list, cut in proportion for a longer one, dropped to what fits after an overrun. Full layers keep their rule. A debug line a listed layer (`[Gi] Lamp N gathers:`). Test: `test-gi.cmd` (`list_batch_check`, mode `lots`). Design: `docs/features/realtime-gi/wp/R-G1.md`. Applies after 0022 |
| 0024 | | Server view of a client texture (WP11): client export `RemixBridge_GiPixelView(src, dst)` blits the renderer's image of a colour texture (the G-buffer's albedo + id target) into a render target texture through DXVK's external pass, in stream order; GiPixel words 11-12 count views and refused views. Runs only while the per-pixel pass (0019) is on. Test: `test-gi-pixel.cmd`. Design: `docs/features/realtime-gi/wp/WP11.md`. Applies after 0023 |
| 0025 | | Per-pixel pass diagnostics for a GPU fault, from `bridge.conf` when the pass opens: `server.giPixelOnly = record` runs only the shadow pass, `= view` only the G-buffer view (0024); `server.giPixelDump = <path>` writes the input of the first `server.giPixelDumpPasses` passes (default 1) to `<path>.<pass>` before the pass is recorded, for `dxvk_gi_pixel_pass --replay` (`tools/gi_server_test/run.ps1 -BridgePass <d3d9.dll> -Validation -TestArgs --replay,<dump>`). A `[GiPixel] diagnostics:` line names the settings. Defaults change nothing. Applies after 0024 |

The TS3-only options patch the game's code in memory only when the bytes there match TS3W 1.67. Otherwise they change
nothing.

`package.ps1` assembles the player package after `build.cmd`. For the server's renderer it takes `../dxvk/out/d3d9.dll`
when `../dxvk/build.cmd` built it, otherwise the DXVK 3.1.1 release (checked by SHA-256), and writes `out\package\` and `out\Apex-Radiance-RTX-Remix-bridge.zip`, laid out as in Bin:
- `d3d9.dll` (the client), `bridge.conf` and `dxvk.conf`;
- `.trex\NvRemixBridge.exe` (the server), `.trex\d3d9vk_x64.dll` (DXVK x64) and `.trex\bridge.conf`;
- `licenses\` (bridge and Detours MIT, DXVK zlib) and the player notes from `PLAYER-README.txt`.

The EA app build runs `TS3.exe`: its players set `client.targetProcess = TS3.exe` in both bridge.conf files.
Apex Radiance 3.0.0 detects the bridge and works under it, with or without patch 0010 (issue 59 is fixed on Apex's
side; 0010 stays because it matches what d3d9 and DXVK return).

To move to a newer upstream: change `COMMIT` in `build.cmd`, delete `src\`, and run it. Refresh any patch that no
longer applies with `git diff`, from a checkout of the new commit that has the earlier patches applied.
