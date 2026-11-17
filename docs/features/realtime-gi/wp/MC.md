# MC: mesh casters in the bridge server (MC-1 to MC-3 of X-MESHCAST)

Status: **MC-1 done** (offline); **MC-2 done offline** ([MC-2.md](MC-2.md)); MC-3 not started, designed below. Branch `gi/mc` (off `gi/x-meshcast`, with
`gi/r-03` merged for the GI server patch 0018).

Marks: [V] verified (by the offline check on the game's packages, or in the source named), [I] inferred, [O] open.

## MC-1: package reader for the bridge server

| File | What |
|---|---|
| `native/remix-bridge/server/ts3_meshes.h` | Header only, standard library only, for the 64-bit server: DBPF 2.0 index, RefPack, RCOL, the VPXY index (LITE instance to MODL key), a MODL's shadow LOD, else its LOD 0, as model-space triangles. Every read is bounds-checked; a broken resource is a miss, never a fault |
| `tools/gi_server_test/meshes_check.cpp` | The reader against `casters.py`: `--self-check` (RefPack, a synthetic MLOD, the drop-shadow flag, a cut MLOD), `<game folder> [--mods <folder>] [--casters <file>] [--survey]` |
| `tools/gi_server_test/run.ps1` | `-Mesh -Game <folder>` also builds and runs `meshes_check` on `casters.bin`; `-Mods <folder>` indexes a Mods folder too |

Findings:

- [V] Same meshes as `casters.py`: the chair (LOD 0 792 triangles, shadow 198) and the table (500, 128) equal vertex for
  vertex and index for index (worst difference 0 m).
- [V] Same counts as the Python readers over the Steam install's 66 FullBuild/DeltaBuild packages: 29,160 VPXYs, 6,145
  without readable links (in both), 8,054 link a LITE and a MODL (8,051 LITEs); 8,264 of 8,336 models read with LOD 0
  (median 319 triangles), shadow LOD on 5,923 (median 200, 90 % 848, 99 % 5,259), as X-MESHCAST states.
- [V] A chunk may name a size past the resource's end; Python's slices cut it, so the reader cuts it too (without this
  66 of the first 3,000 VPXYs were lost).
- [V] Time, warm file cache, one core: game only, index 0.26 s and links 0.65 s; with a Mods folder of 171 packages
  (custom content), index 1.2 s and links 3.3 s (12.6 s cold). The links grow to 10,840 (2,786 from custom content).
  The server builds them once on the GI worker, so objects attached before then wait for it [I].
- [I] Custom content meshes read by the same rules; only their link count was checked, no mesh of theirs compared.
- Reading every model (the survey) takes 8.7 s: the server reads a model only when an object names it.

## MC-2: the object command (next)

Design, from the hooks `features/object_legs.cpp` already installs (`LightObjectAttach`: object id and LITE key;
`LightObjectMove`: the object's matrix, the layout `ObjectLegs::Place` applies):

- R-04's `RemixBridge_GiSubmit` is not on any branch yet, so patch 0019 adds a small export:
  `RemixBridge_GiObjects(const GiObject* objects, uint32_t count)`, `GiObject = {uint64 id; uint64 lite; float m[12];
  uint32 flags}` (flags: removed), sent as one command per batch (`Bridge_GiObjects`, no answer). The server keeps
  `id -> (MODL key, matrix)` and, on the GI worker, resolves each new LITE through `ts3mesh::Library` (game folder:
  three levels up from `.trex\NvRemixBridge.exe`'s folder; Mods: `Documents\Electronic Arts\The Sims 3\Mods`).
  `build.cmd` copies `server\ts3_meshes.h` into the source tree before building.
- Counters appended to the Gi block's reserved words or a layout-2 tail (coordinate with R-04): objects, models
  resolved, models missing (no link or no mesh), index ready.
- Apex side: one Developer-only option (with its i18n entries) that sends the batch from the two hooks for every LITE,
  not only the baked tables; the 3D record's `ObjectLegs` path stays as it is.
- Smoke job `mesh-casters-smoke` (ApexTest-Night, bridge Bin state): load a lot, read the counters; pass when resolved > 0,
  missing small against objects, faults 0. Not written yet: it needs the counters' names.

## MC-3: SM3 mesh tiles (next)

The d3d9 fallback's cube tiles rasterize the same triangles (X-MESHCAST: 512² 98.0-100 % equal to rays). The meshes
live in the server, so the 32-bit side needs its own copy: either the same header built for x86 (it has no 64-bit
dependence [I]) reading the packages in Apex, or a mesh read-back export. Decide with MC-2's counters in hand.

## Unblocks

MC-2 (the reader it resolves through), then MC-3; R-07's package source.
