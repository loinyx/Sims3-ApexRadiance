# dxvk

DXVK (zlib, https://github.com/doitsujin/dxvk) at v3.1.1 with our patches, built as the x64 `d3d9.dll` that the RTX
Remix bridge server renders through (`.trex\d3d9vk_x64.dll`, see `../remix-bridge`).

- `build.cmd` fetches the pinned commit with its submodules into `src\`, applies `patches\` in order and builds
  `out\d3d9.dll` (release, MSVC, static C runtime, Direct3D 9 only). Needs git, VS 2022 C++ build tools (x64) and
  meson + ninja on PATH. It fetches glslang 16.6.0 (DXVK's shader compiler, checked by SHA-256) into `tools\` once.
  A clean build takes about 6 minutes, a rebuild after a patch change about 3.
- `../remix-bridge/package.ps1` packs `out\d3d9.dll` when it exists, otherwise the stock DXVK 3.1.1 release (checked by
  SHA-256).
- `test\dxvk_smoke.cpp` loads a given x64 `d3d9.dll`, creates a device on a hidden window, presents one cleared frame
  and prints the adapter, a CRC32 of the device caps and the results of each call. DXVK writes its own log next to it.
- `test\dxvk_external_pass.cpp` (with `test\fill.comp`) checks patch 0004: it clears a 64x64 render target red through
  Direct3D 9, has a compute dispatch recorded by `ExecuteExternal` paint the left half green, and reads the target
  back, once and after 1000 more passes. Build steps are in the file. On the stock library it prints
  `external pass: no`.
- `test/dxvk_gi_pixel_pass.cpp` runs the bridge server's per-pixel shadow pass (`../remix-bridge/gi/gi_pixel_pass.h`) on a given
  library without the bridge: a floor drawn into an INTZ depth texture, one lamp, two capsules and a static box; it checks
  the four outputs, again after 100 frames and after a device reset. Build and run:
  `tools/gi_server_test/run.ps1 -BridgePass <d3d9.dll>`. On the stock library it prints `[GiPixel] off: stock renderer`.

DXVK's releases are built with MinGW gcc, this build with MSVC. With no patch, the built library and the 3.1.1 release
print the same smoke lines on an RTX 3070 laptop GPU (caps CRC32 `98a84800`), export the same functions, and write the
same 341-line log apart from the `Build:` line. A patched tree reports itself as `v3.1.1+` (DXVK's own mark for a
modified source, as the zlib licence asks).

Patches. Each applies after the ones before it. 0001, 0002 and 0005 are proposed upstream; 0003 and 0004 are for Apex
Radiance's own passes in the bridge server and are not.

| Patch | What | Why |
|---|---|---|
| 0001 | `DxvkPipelineBindings` binding lists keep 4 entries inline instead of 32 | One `DxvkPipelineBindings` holds 23 lists, about 12 KB in a 32-bit build, and DXVK keeps one in every shader pipeline library, graphics and compute pipeline. In The Sims 3 that was 6,464 library nodes of 12,808 bytes (79 MiB); with 4 inline they are 2,568 bytes. A longer list moves to the heap |
| 0002 | `DxvkShaderIo` keeps 16 variables inline instead of 32 | Every shader object holds four `DxvkShaderIo` and every fast pipeline key five more. The Sims 3's shaders use at most 16 inputs and 15 outputs, so nothing moves to the heap and each shader object is 2,304 bytes smaller in a 32-bit build |
| 0003 | `VK_KHR_deferred_host_operations`, `VK_KHR_acceleration_structure` and `VK_KHR_ray_query` are enabled when the driver has them, with the `accelerationStructure` and `rayQuery` features | DXVK does not use them; a pass recorded through 0004 can trace rays on DXVK's device. Optional, so without driver support the device is created as before |
| 0004 | `ID3D9VkExternalPass` (own IID): `ExecuteExternal(callback, data, resources, layouts)` and `SupportsRayQuery()` | Records the caller's Vulkan commands in DXVK's command stream, in stream order: no flush and no wait on the calling thread. The data is copied at the call, each resource moves to its layout for the callback and back, and DXVK re-binds its state afterwards |
| 0005 | `DxvkDevice::waitForResource` also ends when the submission queue reports `VK_ERROR_DEVICE_LOST`; a failed submission wakes the waiters; `LockImage` and `LockBuffer` then return `D3DERR_DEVICELOST` | After a GPU fault (driver reset) the dropped command lists never release their resources, so a lock that waited for the GPU hung forever: the bridge server froze in a `LockRect` read-back instead of reporting device loss |

With 0001 to 0004 on an RTX 3070 laptop GPU the log lists the three extensions as enabled, `SupportsRayQuery` returns
true and `dxvk_external_pass` reads back no wrong pixel; the smoke test prints the same caps as the release.
Not checked under the Vulkan validation layers.

0001 and 0002 come from sims3-modernization (by alexvixell35-stack), measured there in the 32-bit game process. The
bridge server is 64-bit, so they save memory there, not address space; not measured in the server.

To move to a newer DXVK: change `TAG` and `COMMIT` in `build.cmd`, delete `src\`, and run it. Refresh any patch that
no longer applies with `git diff`, from a checkout of the new commit that has the earlier patches applied.
