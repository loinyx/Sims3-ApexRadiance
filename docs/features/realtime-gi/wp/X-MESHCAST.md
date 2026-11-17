# X-MESHCAST: mesh-exact furniture shadow casters

Status: **done** (design and offline harness; nothing in the game yet). Branch `gi/x-meshcast` (off `feature/realtime-gi`,
with `gi/xr-04` merged for the kernel).

Marks: [V] verified (by the harness on an RTX 3070 Laptop GPU, by reading the game's packages, or in the source named),
[I] inferred, [O] open.

## Deliverable

| File | What |
|---|---|
| `tools/gi_server_test/casters.py` | Reads a chair and a table out of the game's packages (LOD 0 and shadow LOD), or `--survey` of every model's triangle counts; `--self-check` on a synthetic MLOD. Reads only; writes one file into the harness's output folder, never into the repository |
| `tools/gi_server_test/mesh_check.cpp` | The two forms of the same picture (below) on XR-04's kernel and reference, the composed floor light against the truth, the SM3 cube maps, bottom-level bytes |
| `tools/gi_server_test/pixel_check.cpp` | The kernel's top level takes any number of instances: static mesh casters as one bottom level a model and an instance an object (both sides). XR-04's own run is unchanged (same counts, all checks pass) |
| `tools/gi_server_test/run.ps1` | `-Mesh` (synthetic chair and table), `-Mesh -Game <the game's folder>` (the game's own) |

Forms: **boxes** = today (XR-04): each object a box in the static set and in the texel layer, its mesh a delta caster.
**meshes** = this package: each object its model's shadow mesh in the static set and in the texel layer, no furniture
delta caster. Both draw the same depth (checked equal): the objects' LOD 0 meshes. The truth traces LOD 0, the cabinet
box and the Sim.

## Where the meshes come from: the packages, not the draws

| Source | Complete off screen | Cost | Verdict |
|---|---|---|---|
| Draw capture (research 03 §2 B/C, research 08 §6 bake-in) | No: only what the camera drew; needs the box-to-mesh crossfade and the anti-popping queue | VB/IB reference a frame, a replay a face-draw | Fallback only |
| The sun shadow pass (research 03 §10 #1) | Unknown, one capture | Same as above | Not needed for furniture [I]; stays open for Sims and foliage |
| **The model's shadow LOD, read from the packages by the object's key** | **Yes: at lot load, from lot data** | One package read a model; no game buffer held | **Primary** |

- [V] Apex already knows each placed object's model and matrix: `features/object_legs.cpp` hooks `LightObjectAttach`
  (object id and the LITE resource key) and `LightObjectMove` (the object's matrix, the layout `ObjectLegs::Place`
  applies).
- [V] LITE to MODL goes through the object's VPXY: 8,056 VPXYs of the base game's packages link a LITE and a MODL,
  never with the same instance, so the server needs a lookup (an index of the VPXYs at load, or a baked table of
  instance pairs, 16 bytes a pair; no mesh data).
- [V] 5,923 of 8,264 models name a shadow LOD (LOD id 0x10000 or 0x10001, as `tools/object_legs_table` reads it):
  median 200 triangles, 90 % 848, 99 % 5,259; LOD 0 median 319, 90 % 1,846. Shadow / LOD 0 median 0.56. EA's shadow
  LOD is the decimated caster mesh: no own decimation is needed.
- [V] LOD 0 positions are four int16 divided by the fourth: every mesh of every model lands within 0.11 mm of the
  bounds its MLOD stores. The shadow LOD's bounds equal LOD 0's within 4 mm (chair and table).
- [I] Package meshes need no AddRef of game buffers, no skinning or alpha-test shader copies, no camera sweep, and no
  anti-popping rules: the cache is complete when the lot loads. The game's data is read from the player's install
  by the bridge server (a 64-bit process); nothing of it ships.

## The cache

| Part | Design |
|---|---|
| Key | MODL instance and group (from LITE through the VPXY index) |
| Value | Shadow LOD triangles in model space; LOD 0 when the model has none [I: LOD 1 would be cheaper, not read yet]; a box when neither reads (today's form) |
| Budget | Shadow meshes over 2,000 triangles keep their box until measured otherwise (the 99th percentile is 5,259) [I] |
| RT path (server) | One bottom level a model, an instance an object (custom index 1, mask static, culling off: game meshes are open). The two chairs share one bottom level [V] |
| Moves | A dragged object moves its instance: the top level is rebuilt, the bottom level kept [I] |
| Texel layers | The server's texel solve traces the same instances; the SM3 tier rasterizes the same triangles into the lamp's cube tiles |
| Delta set | Sims (capsules) and nothing of the furniture. A box stays in the static set only for a model without a mesh |

## Findings

With the game's chair (LOD 0 792 triangles, shadow 198) and table (500, 128), two chairs and the table at XR-04's place:

- [V] Double darkening gone. Floor pixels within 0.4 m of the objects (12,717), composed lamp light against the truth:
  boxes mean -60.3/255, **56.3 % darker by more than 8/255**, worst -185/255; meshes mean +2.3/255, **1.3 % darker**,
  10.2 % lighter (the shadow LOD leaves out some of LOD 0's detail). Synthetic meshes (shadow mesh = LOD 0): boxes
  47.9 % darker, meshes 0.0 %, both ways.
- [V] The kernel matches the CPU reference in both forms, as in XR-04: point emitters 100 % of the delta and static
  counts, emitter balls 99.996 %, multiplier within 2/255 on 99.999 % (worst 5.7/255).
- [V] Rays a frame: boxes 190,711 from 48,681 guide pixels, meshes **57,680** from 14,428: furniture out of the delta
  set leaves only the Sim's reach.
- [V] Texel occlusion against the truth (point lamps, floor texels at 8 a metre within 0.6 m of an object, 636 of them):
  boxes 56.4-87.7 % equal, shadow meshes 97.3-98.9 %, over the four lamps of the room.
- [V] SM3 tier: a cube shadow map rasterized from walls, cabinet and the shadow meshes (1,768 triangles), against rays
  through the same set: 512² 98.0-100 %, 1024² 99.1-100 % equal (normal offset of 1.5 texels, 5 mm bias). Rasterizing
  on one CPU core takes 0.2-1 s for 6 faces; the GPU tier draws them as 6 face-draws [I].
- [V] Bottom levels (uncompacted): 73-76 bytes a triangle; the chair's shadow mesh 14.8 KiB, LOD 0 56.4 KiB. 100
  objects with no model shared: 1.4 MiB at the median 200 triangles, 6.1 MiB at the 90th percentile 848, plus 6.2 KiB
  of instances. A CPU copy (36 bytes a triangle, the SM3 tier's vertex buffer) 0.7 / 2.9 MiB.
- [O] Build time: 55-371 ms a model upload and build, submit to idle, with buffer allocation on a shared GPU: not a
  build-cost figure. A batched build of all a lot's models in one submit is the number to take (XR-09's quiet machine).

## Open issues

- Not built in the game: the VPXY index (or baked pair table), the package reader in the bridge server, the command
  that names an object's MODL key and matrix, the SM3 tier's shadow-mesh vertex buffers.
- Custom content: its MODL and VPXY sit in the Mods packages. The same reader can index them [I]; the draw capture
  is the fallback for anything not found.
- 2,341 of 8,264 models have no shadow LOD: LOD 0 (or LOD 1) at a higher triangle count, or the box.
- The shadow LOD is coarser than LOD 0: 10.2 % of the floor pixels around the objects end lighter by more than 8/255.
  A quality setting can take LOD 0 on the RT path (4x the triangles, 56 KiB a chair).
- Compaction of bottom levels, the texel layer's own resolution under furniture (the harness evaluates the texel at
  the drawn point) and a lot with more than one story were not tested.
- Objects without a LITE get no `LightObjectAttach` and no caster: today they have no box either [I].

## Unblocks

R-07 (its source becomes the packages; the game's vertex buffers stay the fallback), XR-06 and XR-10 (no rule for
origins inside a furniture box is needed where a mesh replaced the box), S5's exact furniture on the SM3 tier.
Follow-ups: MC-1 the VPXY index and package reader in the server (1-2 h, offline), MC-2 the object command from the
two hooks (1 h + 1 game run), MC-3 the SM3 tier's mesh tiles (2 h + 1 game run).
