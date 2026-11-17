# MC-2: mesh casters to the bridge server

Status: **done offline**; the in-game smoke (`tools/ingame_runner/jobs/mesh-casters-smoke.json`) is open. Branch `gi/mc-2`
(off `gi/r-04`, with `gi/mc` and `gi/xr-06` merged). Design: [MC.md](MC.md); package reader: MC-1
(`native/remix-bridge/server/ts3_meshes.h`).

## What it adds

| Part | Where | What |
|---|---|---|
| Bridge patch 0020 | `native/remix-bridge/patches/0020-bridge-gi-mesh-casters.patch` | Client export `RemixBridge_GiObjects(objects, count)`; record `GiObject` (object id, LITE instance, 3x4 model-to-world rows, flag 1 = removed, 72 bytes); `Bridge_GiObjects` commands of at most 256 records, no answer |
| GI worker | `gi_objects.h` (in the patch) | Index of the game's packages built once at the first batch on the GI worker thread; batches queued before then wait in the worker queue. Each LITE resolved once to its model's caster mesh (shadow LOD, else LOD 0); latest record per object kept, at most 65536 |
| Counters | words 14-21 of `RemixBridge_GiReadCounters` | objects, models resolved, models missing, index ready (1, or 2 = no game package found), LITEs linked, index milliseconds, records dropped |
| Per-pixel pass | `gi/gi_pixel_pass.h`, `gi/gi_pixel_block.h` | One bottom level per model, built in the pass that first needs it (at most 16 a pass); the 1024 objects nearest the camera as instances after the capsules and boxes (custom index 0, mask 1, no culling). `[GiPixel]` line and shared words gain `meshes` and `models` |
| Server options | `bridge.conf` | `server.giMeshGame` (default: three levels up from `.trex`), `server.giMeshMods` (default: the Documents Mods folder; `off` for none) |
| Apex | `features/object_legs.*`, `features/lamp_mark_filter.cpp`, `features/realtime_gi.*` | `[patches.RealtimeGI] meshCasters` (developer builds, off). The LightObjectAttach hook records each object's LITE, LightObjectMove queues its matrix, LightObjectRemove queues the removed flag; `OnPresent` sends the batch and logs `[GiMesh] objects= resolved= missing= missingShare= indexReady= links= indexMs= dropped=` every 10 s |
| Offline test | `tools/gi_server_test/run.ps1 -Mesh -Game <folder> -Bridge`, `native/remix-bridge/test-gi-mesh.cmd` | `meshes_check --objects` writes the chair's caster mesh; `test_gi_mesh_device` submits objects through the real export on the built bridge pair |

## Findings

| Finding | Tag |
|---|---|
| Through the real export on the built pair: 4 records (chair, unknown LITE, a second chair, its removal) give objects=2 resolved=1 missing=1 indexReady=1 links=8051 dropped=0 | verified (offline test) |
| Index of the Steam install, no Mods: 8051 LITEs linked; built in 0.6-0.8 s with a warm file cache, 5.2 s on the first, cold run | verified (offline test) |
| Chair bottom level in the per-pixel pass against CPU rays (Moller-Trumbore, the kernel's samples): 7239 pixels compared, 209 shadowed, 6 off by more than 2 of 16 rays, mean difference 0.0009; faults=0 meshes=1 models=1; after the removal meshes=0 | verified (offline test, `test-gi-mesh.cmd` mode `on`) |
| `test-gi.cmd` (R-04) and `test-gi-pixel.cmd` (XR-06) still pass with 0020 | verified |
| Patches 0018-0020 apply in order on the 0015/0016 base | verified |
| Developer and Public builds succeed; the option is ignored in Public builds (`meshCasters_ && !kPublicBuild`) | verified (build) |
| Objects placed before the sink is on are not sent until they move. The sink starts on the first presented frame, before a world loads, so a loaded world's objects are expected to arrive through the move hook | inferred: the in-game smoke checks it |

## Open issues

- In-game smoke not run (no game launch in this package). It needs the bridge pair with 0020 in `Bin` and
  `client.giServer = True`.
- The pass draws mesh casters only where a capsule's caster bounds reach (the kernel's `TRACE 1` gate); a lot without
  Sims traces none. A mesh-only bound is MC-3 or a kernel change.
- No cap on total bottom-level memory beyond 4096 models of at most 65536 triangles.

## MC-3 recommendation: server readback, not an x86 reader

The server already holds the index and every resolved mesh (links=8051 in under a second warm, one copy in the
64-bit process). An x86 reader would rebuild the same index inside the 32-bit game and spend its address space on
it. MC-3 should build its SM3 cube tiles on the server from this mesh table and read the result back (inferred; measure
the readback cost in MC-3).

## Unblocks

MC-3 (SM3 cube tiles from server meshes), and per-pixel mesh shadows in game once the smoke passes.
