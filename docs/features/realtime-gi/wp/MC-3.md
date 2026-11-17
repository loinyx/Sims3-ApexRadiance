# MC-3: mesh casters in the Direct3D 9 shadow tiles

Status: **done offline**; the in-game smoke (`mesh-casters-smoke`, variant `fallback`) is open. Branch `gi/mc-3` (off
`gi/mc-2`). Design: [MC.md](MC.md); server side: [MC-2.md](MC-2.md).

Marks: [V] verified (offline test or build named), [I] inferred, [O] open.

## Decision: read the meshes back from the bridge server

| Path | 32-bit address space | Time | Verdict |
|---|---|---|---|
| x86 package reader in Apex (`ts3_meshes.h` built x86) | Index: 5.4 MB private (game only), 7.4 MB with a Mods folder of 171 packages; peak commit 7.3-8.4 MB. Meshes of 300 models: +1.2-1.8 MB | Index 2.7-5.8 s of package reads in the game process; 1-2 ms a model | Not built |
| **Read-back from the server** (`RemixBridge_GiMeshFetch`, patch 0022) | A 256 KB slot mapping, 512 KB of read buffers while fetching (freed when the set is done), the cache: the same 1.2-1.8 MB for 300 models (budget 16 MB) | No package read in the game; one fetch in flight, median 25-33 ms wall a model (90 % 45-55 ms), polled once a frame | **Chosen** |

- [V] x86 reader numbers: a measurement program around `ts3_meshes.h`, built x86 and x64, indexing the Steam install
  (8051 LITEs linked) and loading the shadow LOD of 300 models (56,284 triangles; 86,930 with the Mods folder).
- [V] Read-back numbers: `test_gi_mesh_device` through the built bridge pair, 50 warm fetches of the chair, two runs.
  The cost is latency, not bytes (the chair is 3.9 KB): a lot of 300 models fills in about 8-10 s in the background with
  one fetch a frame [I]. A batched fetch would cut that if it matters.
- The server already holds the index (built once for MC-2's objects) and every resolved mesh, so the read-back spends no
  second index in the game process. Saving 5-8 MB of the game's 4 GB matters under the address-space rule.
- **Without the bridge** (plain DXVK, native Direct3D 9) there is no server and no read-back: the tiles keep today's
  occluders (walls, roofs, slabs, stairs) and furniture keeps the game's own 3D test; Sims get capsule occlusion
  (research 17 §5). The x86 reader stays possible at the measured 8-13 MB, but plain DXVK is not a test target, so it is
  not built.

## What it adds

| Part | Where | What |
|---|---|---|
| Bridge patch 0022 | `native/remix-bridge/patches/0022-bridge-gi-mesh-readback.patch` | Client exports `RemixBridge_GiMeshFetch(lite, &ticket)`, `RemixBridge_GiMeshRead(...)`; worker writes the LITE's caster mesh (model space) into `GiMeshSlot`, a mapping of its own (`"GiMesh"`), so the GI block's layout is unchanged; counter word 21 = answers. Numbered 0022: R-06 holds 0021, and both append to `Commands` and the `.def` |
| Cache and set | `features/mesh_caster_set.h` | Placed objects (from the MC-2 object hooks), model cache keyed by LITE with a byte budget (16 MB) and LRU that never drops a model a placed object uses; refused models wait until an object leaves. Shadow meshes over 2,000 triangles are left out (X-MESHCAST budget). An object whose bounds hold a lamp's head is left out (a lamp inside its shade would see nothing) |
| Tile occluders | `features/rt_outdoor_lamps.cpp` | The physical indoor lamps' tile occluder upload appends every placed object's triangles in lot space (at most 32,768, 2.3 MB of vertex buffer) as object shapes; a changed set uploads again after 2 s quiet, at most every 5 s, not while a lamp is dragged; the kept tiles' generation moves so they render again. Log line gains `N mesh caster triangles` |
| Apex option | `features/realtime_gi.*` | `[patches.RealtimeGI] meshTiles` (developer builds, off; implies `meshCasters`). One fetch a frame on the render thread. `[GiMeshTiles] objects= models= bytes= fetched= missing= tooBig= evicted= overBudget= lampSkips= serverFetches= waitFrames= tileTris=` every 10 s |
| Tests | `tools/rt_lamps_test/check.cpp` (`MeshCasterSetChecks`), `test-gi-mesh.cmd` | Cache budget and LRU, fetch once per model, missing and too-big models, lamp skip, triangle cap, a table top that blocks a lamp's ray to the floor under it and not beside it; through the bridge pair: the chair read back equal to the package reader's, unknown LITE, short array, timing |
| Smoke | `tools/ingame_runner/jobs/mesh-casters-smoke.json` variant `fallback` | Expects the `[GiMeshTiles]` line with models and tile triangles over 0 and the occluder line with mesh caster triangles |

## Findings

| Finding | Tag |
|---|---|
| Chair read back through the built pair: 136 vertices, 198 triangles, equal to `meshes_check` value for value; unknown LITE `D3DERR_NOTFOUND`; second fetch `D3DERR_WASSTILLDRAWING`; short array `D3DERR_MOREDATA` with counts | verified (test-gi-mesh.cmd, counters mode) |
| Warm fetch: median 24.5 / 33.1 ms, 90 % 54.7 / 44.6 ms, max 97.5 / 82.4 ms; 2-3 of 50 needed a Present | verified (two runs) |
| `rt_lamps_test`: 680,467 checks, 0 failures, with the mesh caster checks | verified |
| Patch 0022 applies on the 0020 tree (forward and reverse check); bridge pair builds x86 and x64 | verified |
| Developer and Public builds succeed; `meshTiles` is ignored in Public builds | verified (build) |
| The tiles show the same mesh shadows as the server's rays (X-MESHCAST: 512² 98.0-100 % equal) | inferred: the same triangles, not compared in game |

## Open issues

- In-game smoke not run (no game launch in this package). Needs Bin with the bridge pair carrying 0020 and 0022,
  `client.giServer = True`, physical indoor lamps.
- Double darkening: furniture keeps the game's own 3D test (blocker table) in the physical model, so a mesh-cast object
  now darkens twice under a lamp. X-MESHCAST measured this with boxes (56 % of floor pixels darker). Next: leave out the
  blocker records of objects whose mesh is in the tiles (needs the record's shape linked to the object id) [O].
- Fetch latency is ~25-33 ms a model; a batched fetch (several LITEs a command, several slots) if lots fill too slowly.
- Objects only arrive through the move hook (MC-2's shortcut), so a loaded lot's still objects cast once they move [I].
- Patch 0022 must be rebased onto R-06's 0021 at the GI merge (both append a command and exports).

## Unblocks

S5's exact furniture on the Direct3D 9 tier; the double-darkening fix above; R-07 (a second consumer of the server's mesh table).
