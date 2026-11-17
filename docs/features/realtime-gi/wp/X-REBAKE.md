# X-REBAKE: re-bake cost of the per-lamp layers against lamp count

- Status: done offline (CPU reference and both server back ends measured on synthetic lots of 60, 120 and 200 lamps;
  Direct3D 9 pass cost estimated from the S0-05 slot table). Clean timing on a quiet machine is open (see Timing).
- Branch: `gi/x-rebake` (from `feature/realtime-gi`)
- Deliverable: `tools/gi_server_test/rebake_check.cpp`, `run.ps1 -Rebake`; `Solve` in `tools/gi_server_test/check.cpp`
  takes the lot size from the scene and can time every lamp's layer batched (additive, default off); this page.
- Closes the S1 measured line of [research 08 §10](../research/08-salvage-rejected-techniques.md) and open 3a of
  [research 12 §1](../research/12-prior-art-lighting-gi.md) ("re-bake cost"); covers plan S2-05.

Marks: [V] verified by the harness, [I] inferred, [O] open.

## The lots

Two stories of 5 m x 5 m rooms, walls on tile lines, 3 m stories (the S0-03 and R-02 layout facts), a ceiling lamp
(2.75 m, range 60) and a table lamp (1.2 m, range 40) in every room (research 01: range 40 to 100), a table box a
room, a door to the next room along x, a door along z in every other column of rooms, a window in every outer wall.
No stair hole. Layer resolution: 1 texel a tile and story (design §3.3).

| Lot | Rooms | House a story | Lot | CPU patches / texels / links | Server texels (1 a metre, floors + ceilings + walls) |
|---|---|---|---|---|---|
| 60 lamps | 30 | 25 x 15 m | 33 x 33 | 3 572 / 750 / 283 k | 3 124 |
| 120 lamps | 60 | 30 x 25 m | 38 x 38 | 6 876 / 1 500 / 571 k | 6 260 |
| 200 lamps | 100 | 50 x 25 m | 58 x 58 | 11 260 / 2 500 / 958 k | 10 420 |

Checks, all lots, both tiers, 0 failures [V]: every layer finite; every lamp lights its own room; **no texel of the
other story gets any light** (0 values, direct and indirect, CPU and both server back ends); the layers solved on
16 threads equal the serial ones; on the 60-lamp lot the compose of the 60 layers equals one full solve of all lamps
within 4.8e-7 (linearity holds at scale).

## Timing

The machine ran other build and test lanes during both runs: the CPU stood at 99-100% and the GPU idled at its lowest
clock between the harness's small submissions. Run 1 (quieter, 60 lamps only) and run 2 differ by 1.5 to 3 times. All
numbers below are **upper bounds** [V for the bound, O for the clean value]; a clean run belongs to R-G1 or a quiet
night. Per-dispatch numbers include a submit and a wait (floor 0.08-0.09 ms); the batched numbers (8 lamps a
submission, direct + 16 bounces a lamp) are close to GPU time.

### Full re-bake, a lamp layer and a whole lot

| Tier | 60 lamps | 120 lamps | 200 lamps |
|---|---|---|---|
| CPU reference, a layer, median (p90) | 125 (144) run 1; 190 (301) run 2 ms | 358 (554) ms | 605 (1 126) ms |
| CPU reference, all layers, 16 threads | 0.93 s run 1; 3.0 s run 2 | 4.8 s | 9.1 s |
| CPU reference, links (build change, 1 thread) | 4.5-6.6 s | 15.1 s | 37.5 s |
| Server, ray queries: a layer, batched | 6.6 ms | 7.6 ms | 6.5 ms |
| Server, ray queries: accel + links | 6.0 + 3.5 ms | 152 + 22 ms | 25 + 4.3 ms |
| Server, ray queries: whole lot from nothing, batched | 0.41 s | 1.1 s | 1.3 s |
| Server, compute: a layer, batched | 14.2 ms | 16.0 ms | 26.8 ms |
| Server, compute: links | 105 ms | 199 ms | 181 ms |
| Server, compute: whole lot from nothing, batched | 0.96 s | 2.1 s | 5.5 s |
| Bounces to settle (0.1/255), mean a lamp | 14.7 | 14.9 | 14.8 |

Per-dispatch (submit + wait each pass, settle test read back each bounce): 60 ms a layer in run 1, 167-406 ms in run 2
(ray queries); that path is the R-02 harness shape, not a server shape: the server records a layer's passes in one
command buffer. The acceleration structure build swung from 6 to 152 ms between lots for the same kind of scene: load,
not size.

Findings:

- [V] **The CPU cost of a layer grows with the lot, not only with the lamp count**: the reference gathers over every
  link of the lot each bounce (16 Jacobi steps), and the links grow with the house (283 k, 571 k, 958 k). A layer
  costs 125-190 ms (60 lamps), 358 ms (120), 605 ms (200); all layers on 16 threads 0.9-3 s, 4.8 s, 9.1 s: the lot
  cost grows about with lamps x house size. The CPU reference is a check, not a game path (as S0-01 said).
- [V] **The server's layer is milliseconds**: 6.5-7.6 ms batched with ray queries on 3-10 k texels, 14-27 ms in
  software, under load. A 200-lamp lot settles from nothing in 1.3 s (ray queries) or 5.5 s (compute).
- [I] The ray-query layer time does not grow from 3 k to 10 k texels: 17 small dependent dispatches at a low GPU clock
  are latency-bound, so the per-lamp time is a floor of the dispatch chain, not of the work. Batching several lamps'
  bounces into one dispatch (a lamp index a texel row) is the cheap cut.
- [V] **Layers are sparse.** A lamp's layer is over 0.5/255 on a median 74-75 texels (max 124) on every lot size; a
  texel is touched by a median 6-7 lamps (max 11-12). The cost a lamp is set by its room and the rooms its doors reach,
  not by the lot, once the solve is scoped to that region (the server's bounce pass still runs every texel of the
  lot; see Scheduler, region scope).
- [V] The dense compose of all layers costs 0.16 ms (60), 0.56 ms (120), 1.6 ms (200) on the CPU. [I] On the GPU a dense
  compose of 200 layers is 13 passes of the 16-layer shader. With the measured sparsity a per-tile list of at most 11-16 lamps
  makes it one pass of the S0-05 compose (68 slots plain, 408 wall-aware).

### Incremental cases

Dirty lamps come from the layers themselves: a lamp is dirty when its layer is over 0.5/255 within 2 m of the edit.
The lamp's reach R (`LampReach::Radii`) on the same story is shown for comparison: it dirties about twice as many.

| Case | What re-runs | 60 lamps | 120 lamps | 200 lamps |
|---|---|---|---|---|
| Toggle, dim, recolour | Compose only, 0 solves [V] | CPU 0.08-0.29 ms; GPU one compose draw | 0.47 ms | 1.47 ms |
| Lamp moved | Its own layer (server: direct + bounces; links kept) | 1 layer | 1 layer | 1 layer |
| Wall edited, mid-house | Re-link near the edit, then the dirty lamps | 10 lamps (23 by R) | 10 (24) | 10 (24) |
| Wall edited, corner room | as above | 6 (12) | 6 (12) | 6 (12) |
| Wall edited, upstairs | as above | 12 (24) | 13 (31) | 13 (31) |
| Furniture moved | Proxy box changes: re-link near it, the lamps lit there | as a wall edit at the box | | |

Cost of a wall edit, from the rows above (a layer a lamp, run 2 medians):

| Tier | Re-link | 10-13 dirty layers | Total |
|---|---|---|---|
| CPU reference | 11-80% of the patches lie within the 12 m link reach: 2.4-10.9 s | 1.1-7.9 s | seconds: not a game path |
| Server, ray queries | accel rebuild + all links: 10-175 ms (load) | 65-100 ms batched | 0.1-0.3 s |
| Server, compute | all links 105-199 ms | 140-350 ms | 0.3-0.55 s |
| Direct3D 9 | plan field rebuild (S2-03 flood) | see below | see below |

### Direct3D 9 back end (estimate from the S0-05 table) [I]

Passes a (lamp, story) layer, 1 texel a tile with the 4 directional faces as extra targets:

| Pass | Shader (S0-05 row) | Instructions run a texel | Draws |
|---|---|---|---|
| Virtual lights from the lamp's shadow tile (16 a lamp) | small reduction | under 100 | 1 |
| Gather, plan trace, 4 directional targets | `gather, 16 x trace 8, loops, 4 directional targets` | 2 698 | 1 |
| Feedback bounces 2-3 (S2-04) | `gather, 16 x trace 8, loops` | 2 609 each | 2-3 |

A story of these lots has 375-1 250 tiles. That is 12-40 warps: under one warp an SM on a 40-SM laptop GPU, so a pass
is latency-bound, not throughput-bound. A texel runs 128 dependent plan reads (16 lights x 8 steps) at about 400-800
cycles each: **about 35-70 µs a pass at 1.5 GHz**, so **0.15-0.3 ms GPU a layer** with 4-5 passes. CPU and bridge:
4-5 draws with a render-target switch, at 1.5-3 µs a draw (research 06 §2.2): about 0.01-0.02 ms.

Consequence [I]: because a pass is latency-bound at this size, **8 layers in one draw cost about the same as one**:
pack the dirty layers of a frame side by side in one target (a texel knows its lamp from its column), so a 0.5 ms
slice does 8-16 layers instead of 2. S2-02 measures this; the batching changes the frame counts below by up to 8
times.

| Direct3D 9, 0.5 ms a frame | One layer a draw (2 a frame) | 8 layers a draw (about 13 a frame) |
|---|---|---|
| Lamp moved | 1 frame | 1 frame |
| Wall edit, 10-13 dirty | 5-7 frames | 1 frame |
| 60-lamp lot from nothing | 30 frames | 5 frames |
| 200-lamp lot from nothing | 100 frames (1.7 s at 60 fps) | 16 frames |

## Scheduler

Design §3.3 already names a dirty queue of (lamp, story) sorted by camera distance with a hard cap. The numbers fix
its budget and its rules.

**Budget.**

| Back end | Render thread | GPU | Slice |
|---|---|---|---|
| `d3d9` | 0.1 ms queue + draws | **0.5 ms a frame** (research 04 S1), 2 ms in a change burst (design §7) | 2 layers a frame, or one packed draw of up to 8 layers once S2-02 confirms the batching |
| `serverCompute`, `serverRays` | one poll (0.1 ms, design §7) | the worker's own device, sliced at about 2 ms a submission so the rendering device keeps its frame | ray queries: a batched layer is 6.5-7.6 ms at the upper bound, so a slice is a lamp's direct pass plus about 4 bounces; compute: about 1-2 bounces. A moved lamp lands in 4-8 slices, inside the 30-frame target (design §7) |
| CPU reference | not a game path | | |

**Dirty sources.**

| Event | Dirty | Re-bake |
|---|---|---|
| Lamp on, off, dimmed, recoloured | nothing | none: compose next frame (P13) [V linear] |
| Lamp moved (also picked up and put down) | that lamp's layer | after the lamp rests 2 frames; direct light stays on the shipped fast path meanwhile (design §3.4) |
| Lamp added | its layer | as moved; the compose shows it with direct light only until its layer lands |
| Lamp deleted | nothing | drop the layer, compose |
| Wall, door, window, floor or ceiling edited | first the geometry (server: accel + links; d3d9: plan field), then every lamp whose touch box holds the edit | after build-mode edits stop for 0.5 s; while edits continue, nothing re-bakes |
| Furniture moved (server tiers: a proxy box) | the lamps whose touch box holds the box's old or new place | as a wall edit; `d3d9` has no furniture in the bounce and skips it |
| Lot load | every layer | camera-near first; the game's class-0 map stays shown until a story's layers are all in (design §3.7) |

**Touch box.** Each layer keeps the box of its texels over 0.5/255, written by the solve. An edit dirties the lamps
whose box holds it: 6-13 lamps on these lots, against 12-31 for the lamp-reach radius [V]. A layer never grows past its
box without a geometry edit inside it, so the box is exact for walls and furniture.

**Priority** (highest first): a lamp whose touch box is in view on the camera's story; then lamps by camera-target
distance; then age in the queue. A cause class breaks ties: moved lamp, then build edit, then load. A layer whose lamp
is off still re-bakes, last (a toggle must stay free).

**Region scope.** A layer's solve runs only on its touch box grown by one room (the doors' reach). The server's bounce
pass now gathers every texel of the lot; scoped to the box it gathers 74-124 texels in place of 3-14 k, so a layer's
cost stops growing with the lot [I: from the sparsity rows; R-04 builds it as a texel-index list a dispatch].

**Results.** A layer is replaced whole, never partly: the new one goes to a second slot and the compose crossfades
over 0.2 s (research 04 S1). A stale layer stays shown until its replacement lands. A fixed bounce count (16) replaces
the per-bounce settle read-back in the game (R-02 open issue); 13-17 bounces settle every lamp here.

**Caps.** At most 64 queued (lamp, story) pairs; over the cap the queue keeps the camera's story and drops the rest to
a "re-bake when idle" list. A lamp re-queued before its bake finishes restarts once, not per edit.

**Counters** (design §6): queued, baked a frame, frames from dirty to landed (p50, max), GPU ms a slice, dropped.

## Open issues

- Clean timing: both tiers on a quiet machine, GPU clocks held (a warm-up dispatch before each batch); R-G1 has the
  in-game form. The ratios (server ms against CPU seconds) hold at any load.
- Direct3D 9 pass time and the packed-draw claim are estimates; S2-02 measures them.
- Stair holes and split levels are not in these lots (no cross-story light at all); the dirty rule then crosses a
  story, which the touch box already handles.
- The region scope and the per-tile compose list are R-04 and S2-01 work.

## Unblocks

S2-01 (dirty queue rules, touch box, per-tile compose list), S2-02 (budget and the packed-draw check), R-04 (region
scope, fixed bounce count, batched submission), S1 gate of research 08 §10: **under budget** on every tier that
ships: the propagation grid fallback is not needed for re-bake cost.
