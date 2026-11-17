# Salvage: per-pixel ray-traced lamp shadows without a shader-translator fork

R-08 ([server-export.md](../server-export.md) §11, on branch `gi/r-08`) rejected per-pixel ray-traced lamp shadows
"now". This page takes the rejection apart, keeps what was right in it and finds the routes that avoid each cost.

**Result in one paragraph.** The rejection assumed the ray query must live inside the game's translated pixel
shaders. It does not. A post-scene compute pass issued by Apex can trace from the depth buffer, and it needs only
(a) ray extensions on a Vulkan device and (b) this frame's depth on that device. Two routes give both: **D**, a
DXVK built from source with a two-patch series (ray extensions on DXVK's own device, plus one entry that lets the
bridge server record Vulkan work in DXVK's command stream), and **B**, the GI worker's second device with the
picture crossing between devices. D is ranked first: no cross-device problem, no copy, same frame, about 150 lines
of patch in two areas of DXVK. Neither route touches `dxbc-spirv`. The noise and shimmer problem is solved by
tracing only what texel shadows cannot show (Sims and furniture detail, "delta shadows"), by accumulating a fixed
sample set and then freezing, and by rejecting history with the motion class of research 14.

Marks: **[V]** verified in the named file (DXVK `v3.1.1`, commit `b1a1c99`, paths relative to the DXVK tree; Apex
paths relative to the repository), **[I]** inferred, **[O]** open with the package that closes it.

Inputs: [server-export.md](../server-export.md) §4, §5, §11 (R-01, R-08), `wp/R-02.md`, `wp/GB-01.md`,
[14](14-gbuffer-motion-vectors.md), [10](10-prior-art-ao.md) §2 and §5, [12](12-prior-art-lighting-gi.md) §7.4.

## 1. The rejection, cost by cost

| R-08's cost | Still true? | Salvage |
|---|---|---|
| Fork DXVK and `dxbc-spirv`, a marker in every game shader, new pipeline layouts | Only for rays *inside game shaders* | Trace in an Apex post-scene compute pass. No game shader changes, no converter change (§2) |
| World position and normal per pixel do not exist | No longer | INTZ depth plus `PostScene::CameraViewProj` gives the position; the normal comes from depth (the AO pass does this) and from `RT2` once research 14 WP3 lands; GB-01 gives camera motion [V: `wp/GB-01.md`] |
| A scene on DXVK's device every frame | Partly | The static part (proxy, furniture) is built on change, as in R-02. Only Sim capsules change a frame: one small bottom-level build (§5) |
| 8-33 M rays a frame | No | Quarter resolution, rays only for pixels a delta caster can reach, zero rays when nothing moved (§5): 0-0.4 M rays in a normal frame at 4K |
| Hard shadows unless filtered in time; Apex removed temporal AO for shimmer | The risk is real | Emitter-sized soft shadows from a fixed sample set, accumulate-then-freeze, history rejection by depth and motion class, spatial fallback that is never the raw frame (§4) |
| The cost lands in the frame Apex keeps light | True for route D | A GPU budget governor, and the pass costs nothing while the picture is still (§5). Route B keeps it off the frame at the price of latency |
| Each DXVK update means rebasing forks | True for route D, but two small patches, not two forks | §3.4 |

What stays rejected: rays inside the game's translated shaders (12 §7.4 R4 as written). It would add only
transparent receivers and per-lamp exactness inside the shading; reopen it only if the compose of §4.1 fails the look
check.

## 2. The technique

One pass a frame between the end of the scene and the post-scene effects, before Ambient Occlusion:

1. **Guide.** For each pixel of a half- or quarter-resolution grid pick one full-resolution pixel (the one nearest
   the camera of its 2x2 or 4x4 block, so thin objects keep their shadow), rebuild its world position from INTZ
   depth and the inverse view-projection, take its normal.
2. **Trace.** For each of the up to 4 strongest lamps at that point (lamp table of research 01, strength =
   intensity x falloff x N·L), one ray query toward a point on the lamp's emitter ball. The ray stops on a static
   occluder (the lamp is hidden: its light is already absent from the picture) and records a hit on a *delta
   caster* (a Sim capsule, a furniture mesh the texel tier treats as a box, a small object).
3. **Filter.** Per-lamp visibility is filtered in space and time (§4), then combined into one multiplier.
4. **Compose.** A Direct3D 9 post-scene effect upsamples the multiplier with depth weights and multiplies the back
   buffer (§4.1).

Why delta shadows and not all shadows: on a GI lot the texel layers already hold the wall and furniture-box shadows
with emitter-sized penumbrae and no noise (R-02 [V]). Tracing them again per pixel would have to divide the texel
shadow out of the picture first, and the two would disagree along every shadow edge. Tracing only what the layers do
not contain adds exactly the missing part: contact shadows of Sims and of furniture detail on floors, walls, objects
and Sims. The instance mask of the top-level structure separates the two sets in one traversal.

Self-shadow guard: the ray starts one normal-offset from the surface and culls back faces, so a pixel that lies
inside its own proxy (a chair inside its box, a Sim inside its capsules) leaves the proxy without a hit [I; XR-04
checks it against a mesh reference].

The pass changes pixels only. Nothing a script reads changes, so gameplay, saves and every Sim's behaviour are
identical with the switch on or off (design §3.6).

## 3. Route D: rays on DXVK's own device, DXVK built from source

### 3.1 What is already there

| Fact | Source | Mark |
|---|---|---|
| The bridge is built from source with a patch series; DXVK is fetched as a release and checked by hash | `native/remix-bridge/build.cmd`, `package.ps1:7-24` | [V] |
| The server loads DXVK's `d3d9.dll` into its own 64-bit process, so server code can call DXVK's COM interfaces directly | server-export §1 | [V] |
| DXVK's extension list is one table; an optional extension is one line `ENABLE_EXT(name, false)` plus a member and a `HANDLE_EXT` line | `src/dxvk/dxvk_device_info.cpp:19-77, :1024-1025`, `dxvk_device_info.h:92-93, :168-169` | [V] |
| `bufferDeviceAddress`, which acceleration structures need, is already a required feature | `dxvk_device_info.cpp:877` | [V] |
| `ID3D9VkInteropDevice` gives the `VkDevice`, the queue, a queue lock, a flush that drains the command thread, and `WaitForResource` | `src/d3d9/d3d9_interfaces.h:138-240`, `d3d9_interop.cpp:181-273` | [V] |
| An interop method already records work in DXVK's command stream from the calling thread: `TransitionTextureLayout` calls `EmitCs` with a lambda that gets the `DxvkContext` | `d3d9_interop.cpp:216-233` | [V] |
| The context hands out its command list for foreign Vulkan calls (`beginExternalRendering`, which invalidates the context's cached state) and tracks images used outside it (`acquireExternalResource`, `releaseExternalResource` with a layout) | `src/dxvk/dxvk_context.h:113-123, :816-834` | [V] |
| The Direct3D 9 front end itself uses that entry inside `EmitCs` for its format converter | `src/d3d9/d3d9_device.cpp:5421-5429` | [V] |
| `ID3D9VkInteropDevice::CreateImage` creates a Direct3D 9 texture with extra `VkImageUsageFlags` | `d3d9_interfaces.h:106-121, :251`, `d3d9_interop.cpp:276-330` | [V] |
| DXVK has an entry that runs on a caller's Vulkan device, but only for Direct3D 11 | `src/d3d11/d3d11_main.cpp:370-426`; no import in `src/d3d9` | [V] |

### 3.2 The patch series (`native/dxvk/patches`, same mechanism as the bridge)

| Patch | Change | Size | Risk |
|---|---|---|---|
| 0001 ray extensions | `khrAccelerationStructure`, `khrRayQuery`, `khrDeferredHostOperations` as **optional** extensions with their two feature bits, in `dxvk_device_info.h` and `.cpp` | 30-60 lines in one file pair [I] | Low: optional, so a GPU or driver without them creates the same device as stock. More enabled extensions do not change DXVK's own pipelines [I] |
| 0002 external pass | A new interface `ID3D9VkInteropDevice2` (own IID, so stock DXVK answers `E_NOINTERFACE`): `ExecuteExternal(callback, user, resources[], layouts[])`. Body: `EmitCs` with a lambda that acquires each resource in its layout, calls `beginExternalRendering`, calls `callback(VkCommandBuffer, user)`, releases the resources. Plus `SupportsRayQuery()` | 80-120 lines in `d3d9_interfaces.h`, `d3d9_interop.{h,cpp}` [I] | Medium: the callback runs on DXVK's command thread, so everything it reads is copied at the call (DXVK's own rule for lambdas). `beginExternalRendering` exists for this use [V], so no state tracking is patched |
| 0003 version tag | The version string says `3.1.1-apex` (the zlib licence asks altered builds to be marked) | 1 line | None |

No change to shader translation, pipeline layouts, the submission thread or memory allocation.

**Without patch 0002** the same work is possible with the stock interface: `FlushRenderingCommands`,
`LockSubmissionQueue`, submit an own command buffer on DXVK's queue, `ReleaseSubmissionQueue` [V: the methods].
It costs a drain of DXVK's command thread on the play thread in the middle of the frame and splits DXVK's
submission, so it is the proof step (XR-02), not the shipped path [I].

### 3.3 How a frame runs

1. Apex ends the scene, then sends one bridge command `Bridge_GiPixelPass` (ids of the depth, normal, motion and
   mask textures; a block with the matrices, lamp table and capsules of this frame, copied into the command).
2. The server's play thread resolves the ids (as patch 0015 does) and calls `ExecuteExternal`.
3. On DXVK's command thread, in stream order, the callback records: capsule structure build, the trace dispatch,
   the filter dispatches, a copy of the result into the Direct3D 9 mask texture's image.
4. Apex's next draw, the compose effect, samples the mask. Stream order makes it **this frame's** mask: no fence, no
   latency, no reprojection of the result.

Reading and writing:

- Depth, `RT2` and the motion target are DXVK images: the callback binds views of them directly. No pack pass, no
  copy, full precision.
- The mask: the compute pass writes an own `R8G8B8A8` storage image and copies it into the Apex render target's image
  (transfer destination is an ordinary usage). Creating the target through `CreateImage` with storage usage would
  save the copy but needs a GI-specific create in the bridge; not worth it for a quarter-resolution copy [I].
- The acceleration structures, history images and pipelines are raw Vulkan objects made by the server on DXVK's
  `VkDevice` (handles from `GetVulkanHandles`), outside DXVK's allocator: a few MB.
- Device reset: the server drops its objects when the client releases the mask target before `Reset`, and builds them
  again on the next pass (ticket rule of server-export §8).

Open: whether depth read in a compute shader needs the read-only depth layout DXVK already uses for sampled depth
[O: XR-03]; whether the INTZ image carries the sampled usage in every case (it is sampled by AO today, so [I] yes).

### 3.4 Upkeep and packaging

| Item | Cost |
|---|---|
| One-off: `native/dxvk/build.cmd` (clone at the pin with submodules unmodified, apply patches, meson x64 release, the same toolchain the bridge build uses), `package.ps1` takes the built `d3d9.dll` | XR-01, about 1.5 h; a DXVK build is 5-10 min [I] |
| Each DXVK release | Rebase 0001 (the extension table moved between major versions, so expect an edit) and 0002 (interop files change rarely); build; one smoke run. 20-40 min [I] |
| Trust | The package no longer ships upstream's binary checked by hash. The pin is the source commit; the build is reproducible from the patch series |
| A player's own DXVK | `ID3D9VkInteropDevice2` missing: the feature reports "off: stock renderer" once and stays off, or falls to route B if built |
| In-process DXVK (no bridge) | Out: the game process is 32-bit and this would need a patched x32 DXVK inside the game. The feature is bridge-only, as design §3.9's server tier is |

### 3.5 What route D changes for the rest of the design

- **Research 14 WP11** (bridge command that hands the G-buffer to a server compute pass) becomes XR-03 + XR-06 and
  loses its surface-sharing problem. DLAA, NRD-style filters and frame generation would use the same entry.
- **The texel solver could move to DXVK's device too** (server-export §5 option C without its restrictions): no
  second device, no host copy, no `UpdateSurface`, and the per-pixel pass could read the lamp layers. Against it:
  long dispatches would share DXVK's queue. Not decided here; R-03 stays as planned and gate G-dxvk (§8) reopens the
  question with numbers.

## 4. No shimmer: filter and history

The failures to avoid are on record: temporal AO "breathed and trailed" because it clamped to an averaged range and
showed the raw frame where the depth test failed (10 §5); half-resolution AO showed "micro dots" (10 §4).

| Step | Design | Answers |
|---|---|---|
| Sample set | Each pixel owns a fixed sequence of N = 16 emitter points (R2 sequence, offset by a 4x4 Bayer index). Frame k traces point k | Noise statistics never change between frames (10 §5, XeGTAO schedule) |
| Accumulate, then freeze | While camera, casters and lamps are still: running mean over the N frames, then **stop tracing**. The settled mask is the exact 16-sample mean and byte-equal from then on | Breathing is impossible on a still picture, and the cost is zero (the texel tier's rule, R-02) |
| Spatial filter | Edge-aware 5x5 a-trous, 2-3 passes, weights from depth and normal; radius scaled by the penumbra width from the recorded hit distance (wide penumbra, wide kernel; contact, no blur) | A single frame is already smooth; contact edges stay sharp |
| Reprojection | History fetched through the motion target (camera vectors exact for static receivers [V: GB-01, 0.0006 px]) | Pans keep their history |
| Rejection | History dropped when the reprojected depth differs (relative threshold), the normal turns, or the motion class is 3 (unknown mover: Sims until research 14 WP8 / WP13, foliage, water) | No trails behind movers |
| Fallback | A rejected pixel takes the **spatially filtered** current frame with a wider kernel, never the raw sample | The temporal-AO v3 failure |
| Clamp | History clipped to mean ± 1.5 sigma of the current raw 3x3 neighbourhood (variance clip on raw values, 10 §5) | Ghosting, and v1's mistake |
| Moving casters | The shadow moves over a still receiver, which reprojection cannot see. Per lamp, a pixel within reach of a capsule that moved this frame caps its history at 4 frames | Shadow lag behind a walking Sim stays under about 70 ms and soft |
| Upsample | Joint bilateral, 4 taps weighted by depth against INTZ, in the compose effect; guide pixel chosen nearest the camera | "Micro dots" came from depth-blind half resolution |

Many lamps (a street or party lot at night): replace "4 strongest" by one reservoir a pixel that picks 1 lamp in
proportion to its unshadowed strength and reuses the neighbour's and last frame's pick (ReSTIR DI, own
implementation from the paper). One ray a pixel at any lamp count. Only after the 4-lamp form passes; inside a lot
it is not needed (12 §7.4 R5).

### 4.1 Compose

The forward colour is final (14 §4), so the pass can only multiply. Multiplier of a pixel:

`m = 1 - sum_i s_i (1 - v_i)`, with `s_i = w_i / (A + sum_j w_j)`

`w_i` is lamp i's unshadowed strength at the pixel times its colour (zero when a static occluder hides it), `v_i` its
filtered delta visibility, `A` the light no lamp ray accounts for (ambient, bounce, daylight). `m` is an RGB value in
one `A8R8G8B8` mask, its alpha the guide depth for the upsample.

- `A` decides how dark a shadow can get. First version: a look constant a room class with a floor, so a shadow never
  goes black [O: look pair in XR-10]. Later, on route D, read from the indirect layers.
- The same compose works before S1 lands, on the game's own lamp light, as an approximation: the feature does not
  wait for the texel tier.
- Blended surfaces (glass, particles) have one depth layer: the mask belongs to the opaque surface behind, as with AO.

## 5. Cost on an RTX 30 laptop

Ray throughput: R-02 traced 2.3 M rays in 1 to 60 ms on an RTX 3070 laptop GPU with the machine shared [V, noisy].
Planning figure: 0.3-1 G simple rays a second [I]. All numbers below are estimates until XR-09 measures them.

| Item | 4K, quarter resolution (960 x 540) | 1080p, half resolution (960 x 540) |
|---|---|---|
| Guide pixels | 0.52 M | 0.52 M |
| Worst case: every pixel, 4 lamps | 2.1 M rays, 2-7 ms | the same |
| Normal case: pixels within shadow reach of a delta caster (10-30 % of the screen), about 2 lamps facing | 0.1-0.3 M rays, 0.1-1 ms | the same |
| Still picture after 16 frames | 0 rays | 0 rays |
| Filter, history | 3 a-trous passes + 1 history pass at 0.52 M pixels: 0.1-0.3 ms | the same |
| Upsample and multiply (Direct3D 9, full resolution) | 0.15-0.3 ms | under 0.1 ms |
| Capsule structure | 20 Sims x 17 capsules as one bottom-level build of boxes with an analytic capsule test: 0.1-0.3 ms; skipped when no Sim moved | the same |
| Memory | 4 lamp visibilities + history + moments at 960 x 540: about 10 MB; structures a few MB; all in the server | the same |
| Game thread | One bridge command and a block of about 8 KB a frame; 2 draws for the compose | the same |

Reach culling: a 16 x 16 pixel tile is traced only if a ray from it toward a lamp can touch a delta caster's bounds
(tile list made on the CPU of the server from the capsule and furniture boxes, a few hundred boxes).

Budget governor: a timestamp query a frame; over 1.5 ms the pass steps down (4 lamps to 2, then quarter to eighth
resolution with a wider filter) and steps up again after 60 frames under 1 ms. The step is logged. This keeps route
D inside design §7's "GPU, steady 2 ms at 4K".

## 6. Route B: the second device (no DXVK build)

Kept as the fallback if gate G-dxvk fails, and as the answer for a player who keeps a stock DXVK.

### 6.1 Getting the picture across

| Path | How | Latency | Verdict |
|---|---|---|---|
| **B-P: no input at all** | The worker traces *primary* rays from the camera matrix against its own scene (proxy, furniture, capsules) and shadows the hit points. The mask stores the traced depth; the compose rejects pixels whose INTZ depth disagrees (real geometry the proxy lacks) and leaves them unchanged | Output only | **First on this route.** Receivers are floors, walls and proxied furniture: where Sim shadows land. Sims and small objects do not receive |
| B-G: shared images | Apex packs depth, normal and class into a quarter-resolution `A16B16G16R16F` target created on the server with a shared handle; the worker imports it. DXVK exports `OPAQUE_WIN32_KMT` handles for `D3DPOOL_DEFAULT` [V: `d3d9_common_texture.cpp:411-417`] and enables `VK_KHR_external_memory_win32` when present [V: `dxvk_device_info.cpp:1024`]. "Direct3D 9 has no shared fence" is answered by the stock interface: the play thread polls `WaitForResource(target, D3DLOCK_READONLY or D3DLOCK_DONOTWAIT)` and signals the worker when DXVK's writes are done [V: the method; I: its use]. A ring of 3 targets keeps writer and reader apart | 1-2 frames in, 1 frame out | Works on paper. Needs a GI-specific create in the bridge, and layouts across two devices rest on driver tolerance [O]. Only if B-P's receivers are not enough and D is out |
| B-H: host copies | Pack target read back through patch 0015 (4 MB a frame), mask returned by `UpdateSurface` (2 MB, twice R-01's 1 MB budget a pass) | 3-5 frames | Last. All parts exist, but the traffic and latency are the worst |

Output on every B path: the worker reads its mask to host memory and the play thread writes it with `UpdateSurface`
(server-export §5 option A); a quarter-resolution `A8R8G8B8` mask is 2 MB at 4K, 0.5 MB at 1080p.

### 6.2 Latency made harmless

A mask that arrives k frames late is not shown at the same screen position. Apex keeps the view-projection of the
last 8 frames; the compose effect rebuilds the pixel's world position, projects it with **the mask's own camera**
(`D_k = VP_mask · inverse(VP_now)`, the math of `features/gbuffer_motion.h` with another previous matrix) and samples
there, with the depth test of B-P. For static receivers this is exact at any k. Pixels the mask's camera did not see
stay unchanged until the next mask. What remains is the caster's own lag: a Sim walking 1.5 m/s moves 7 cm in 3
frames at 60 frames a second, inside a soft contact shadow's penumbra [I]. So: 1 frame is fine, 3 are acceptable
for soft shadows, 5 show on fast movers.

## 7. Ranking

| Rank | Variant | Quality | Latency | New risk | Work to first picture |
|---|---|---|---|---|---|
| 1 | **D2**: patched DXVK (0001 + 0002), compute in DXVK's stream | Every opaque surface receives, full-precision depth, lamp layers readable | 0 frames | Own DXVK build; GPU time inside the frame | XR-01..04, XR-06, XR-07: about 10 h, 2 game runs |
| 2 | D1: patch 0001 only, stock interop with flush and queue lock | As D2 | 0 frames | A mid-frame drain of DXVK's command thread | The proof step of D2 (XR-02); ship only if 0002 fails |
| 3 | B-P: second device, primary rays from the proxy, reprojected compose | Proxy surfaces receive only | 2-4 frames, hidden by §6.2 | None in DXVK; needs R-03 | After R-03: about 6 h, 1 game run |
| 4 | B-G: second device, shared images | As D2 at quarter-resolution guide precision | 2-3 frames | Cross-device layouts, a bridge create path | About 10 h after R-03 |
| 5 | B-H: second device, host copies | As B-G | 3-5 frames | Play-thread bytes | About 8 h after R-03 and patch 0015 |
| 6 | Analytic capsule shadows in the AO pass (design §3.9 "Sim shadows", S3) | Sims only, no furniture detail, no noise at all | 0 | None | Already planned; the fallback without ray hardware or without the bridge |
| - | Rays inside game shaders (R-08's subject) | Adds transparent receivers | 0 | Converter fork | Stays out (§1) |

Against texel shadows:

| | Texel layers (R-02) | Per-pixel delta shadows |
|---|---|---|
| Walls, room shape, furniture boxes | Yes, noise-free, frozen | Not traced (left to the layers) |
| Sims as casters | No (capsule occlusion planned in S3) | Yes, soft, lamp-directed |
| Sims and objects as receivers | No (rig light only) | Yes on route D and B-G |
| Chair legs, table edges | Only at 16-32 texels a metre | Yes, once furniture meshes are in the scene (R-07) |
| Cost when still | 0 | 0 after 16 frames |
| Cost when moving | 0 for the camera | Rays for the pixels near casters |
| Without ray hardware | Compute back end | Off; rank 6 takes over |

Fallback chain at run time: D2 when `ID3D9VkInteropDevice2` reports ray query; else B-P when the GI worker reports
level `rays`; else off, and the S3 capsule occlusion is the Sim shadow. One log line names the level.

## 8. Work packages

Each is at most 2 h of work with a smoke test; game runs are listed separately and are short (one load, one lot).
Timing numbers come in XR-09, after the proofs. XR-04 and XR-05 need neither DXVK nor the bridge and can start now.

| Id | What | Time | Depends | Smoke test | Game run |
|---|---|---|---|---|---|
| XR-01 | `native/dxvk/build.cmd`: DXVK at the pin from source, no patch, x64 `d3d9.dll`; `package.ps1` switch to take it | 1.5 h | - | A 64-bit harness (the start of `tools/dxvk_rays_check`; the existing `-D3d9` harnesses are 32-bit) creates a Direct3D 9 device on the built library, draws one triangle and reads it back; the log shows the version | 1, existing smoke job, picture and log as stock |
| XR-02 | Patch 0001 + 0003. Harness `tools/dxvk_rays_check`: Direct3D 9 device on the built library, `GetVulkanHandles`, one-triangle acceleration structure and one ray query in a compute shader, submitted under the stock queue lock | 1.5 h | XR-01 | The ray hits at the expected distance; on the stock library the harness prints "rays: no" and exits clean | none |
| XR-03 | Patch 0002 `ExecuteExternal`. Harness: draw a scene into a target with INTZ depth, callback reads depth in compute and copies a function of it into a Direct3D 9 target; draws before and after | 2 h | XR-02 | `GetRenderTargetData` equals the CPU expectation; the scene picture is byte-equal with an empty callback; 1 000 passes without a validation-layer message | none |
| XR-04 | The kernel `pixel_shadow.comp` in `tools/gi_server_test` (R-02's scene and device code): guide from a synthetic depth, delta rays with instance masks, capsule boxes with analytic test, multiplier output | 2 h | R-02 | Point emitter: 100 % of pixels within 2/255 of a CPU ray reference; a chair mesh inside its box casts no self-shadow; 16 accumulated frames equal the 16-sample reference | none |
| XR-05 | Filter and history in the same harness: a-trous, accumulate-then-freeze, reprojection with `features/gbuffer_motion.h`, rejection, clip, caster cap | 2 h | XR-04 | Still: frames 17-80 byte-equal. Synthetic pan: error against the 16-sample reference under 4/255 on 99 % of pixels. Moving capsule: no pixel keeps shadow more than 4 frames after the caster left | none |
| XR-06 | Bridge patch: `Bridge_GiPixelPass`, id resolution, the server side that owns the Vulkan objects and calls `ExecuteExternal` with XR-04/05's shaders; reset handling | 2 h | XR-03, XR-04 | Bridge self-test on a synthetic device: mask non-empty, device reset and second pass succeed; stock DXVK gives "off: stock renderer" | none |
| XR-07 | Apex: `features/gi_pixel_shadows.{h,cpp}`, switch off by default, lamp and box block, compose effect with depth-aware upsample, mask view, counters and the level log line | 2 h | XR-06, GB-01 smoke | Offline: shaders compile to `ps_3_0`; switch off = no bridge command. Game: mask view shows furniture shadows that follow a lamp move; picture byte-equal with the switch off | 1 |
| XR-08 | Sim capsules into the block (source: the capsule set S3 defines for AO; until then a bone-box fit from the AO receiver replay) | 1.5 h | XR-07, S3 capsule source | Mask view: a walking Sim's shadow follows it without a trail; household and other Sims drawn alike (pixels only) | 1 |
| XR-09 | Reach tiles, timestamp counters, budget governor | 1.5 h | XR-07 | Log: rays a frame drop to 0 on a still picture; forced budget of 0.2 ms steps down and the picture stays stable | 1, also the first timing numbers |
| XR-10 | Look pairs against texel shadows and against the switch off; `A` constants; decision on the default | 1 h + pictures | XR-08 | Picture pairs on 3 lots, day and night; no shimmer in a 60 s still capture (frame difference 0) and a slow pan | 1 |
| XR-11 | Only if G-dxvk fails: B-P on the GI worker (primary rays, mask upload, reprojected compose of §6.2) | 2 x 2 h | R-03, XR-04 | Harness: reprojected mask equals a same-frame mask on static receivers at k = 3 | 1 |
| XR-12 | Optional: one-reservoir lamp pick for more than 4 lamps | 2 h | XR-05 | Harness with 32 lamps: mean equals the all-lamps reference within the look band | none |

**Gate G-dxvk** (after XR-03 and XR-01's game run): the built DXVK shows the stock picture and log in game, both
harnesses pass, and the patches are within the sizes of §3.2. Pass: D2 leads and research 14 WP11 is replaced by
XR-03 + XR-06. Fail: XR-11.

## 9. Not verified

- The patches are not written; their sizes are estimates from reading the files named in §3.1.
- DXVK `v3.1.1` was not built here. That it builds with the toolchain of the bridge build is [I].
- No ray, filter or upload timing was measured; §5 is arithmetic on R-02's noisy figure.
- Whether enabling the ray extensions changes DXVK's memory or pipeline behaviour on any driver [I: no].
- Import of DXVK's shared handle on a second Vulkan device (B-G) was not tried.
- The compose constant `A` and the shadow depth it gives are a look decision without pictures yet.
- Sim capsules do not exist in Apex yet (S3); the motion class for Sims stays 3 until research 14 WP8 or WP13.
