# 17. Decision memo: which GI back end is primary

Research 12 §7.5 ranks the server bake third with the note "decide before building §4 twice". This memo decides it,
from the packages finished since: R-01, R-02, R-08, S0-05, S2-03, XR-04, XR-05, research 14, 15 and 16, the
asynchronous read-back patch 0017 and the DXVK patch series.

Marks: [V] verified in the named package or source, [I] inferred, [O] open. Nothing was built or measured for this
memo. Proposed edits to [plan.md](../plan.md) and [design.md](../design.md) are listed in §6 and §7 and are **not
applied**.

## 1. Recommendation

1. **The server solve is primary.** Direct visibility and the indirect layers are solved by one compute kernel in
   the bridge server. Ray queries are a capability level of that kernel, not a separate back end: R-02 builds both
   from one source and they agree within 2/255 on 100 % of texels [V, wp/R-02].
2. **Two back ends, not three.** `server` (level `rays` or `compute`) and `d3d9`. The switch values `serverRays` and
   `serverCompute` remain as developer overrides of the level for parity pictures.
3. **The Direct3D 9 gather is not built.** S2-02, the Direct3D 9 half of S2-04, the wiring of S2-03 and S5-04 are
   parked. The `d3d9` back end shrinks to the minimal fallback of §5: Apex's direct light (S1) and a per-room
   ambient term, no bounce solve.
4. **Devices: keep both, each for one kind of work.** The texel solve stays on the second Vulkan device of
   server-export §4 (R-03, in progress). Passes that need this frame's depth (per-pixel delta shadows of research
   16, the G-buffer consumers of research 14 WP11) run on DXVK's device through the `ExecuteExternal` patch. The
   texel solve moves to DXVK's device only if R-03 fails one of the triggers in §3.3.
5. Gate **G-server is decided: pass.** Gate G-dxvk (research 16 §8) decides only the per-pixel tier.

Why, in one paragraph: the server kernel exists and passes its leak, parity and energy checks [V]; the Direct3D 9
gather does not exist, needs looped `ps_3_0` shaders written around compiler faults [V, wp/S2-03], lacks furniture
occlusion and a stairwell walk, and can at best approach what the `compute` level already gives on any Vulkan 1.1
GPU, including an integrated one [V, wp/R-02]. Its only audience is a player without the bridge, and that player
keeps the game's own light.

## 2. The options against the criteria

| Criterion | A: `ps_3_0` passes issued by the 32-bit client | B: compute in the server | C: three back ends, parity rule |
|---|---|---|---|
| Client CPU | Every pass is state, constants and a draw from the render thread. Budget 0.3 ms a frame, 0.9 worst (design §7) [I]. A plan field edit of a 64 x 64 story is 45 passes [V, wp/S2-03] | A submit on change and a poll of shared memory a frame; budget 0.1 ms [I] | The worse of A and B on each machine |
| Bridge calls | Each pass crosses the bridge as several commands; setters are packed by `client.batchState`, draws and target changes are not; `commandBatchingEnabled` does nothing [V, research 15] | One command a submit and a solve, none for a poll [V, server-export §6] | Both paths kept working |
| GPU time | [O] no timing exists. Looped gather runs 2 609 to 18 065 instructions a texel [V, wp/S0-05] | [O] R-02 timings varied over 10 times on a shared machine; links of 2.3 M rays took 1-60 ms with rays, 130-210 ms without [V, noisy]. Zero when settled [V] | - |
| 32-bit address space | Under the bridge the targets live in the server, but uploads cross: plan seeds up to 33.7 MB a band in the client while filled [I, wp/S2-03]. In-process DXVK or native: everything in the game process | Shared block about 1 MB; links, layers and structures in the 64-bit server [I, server-export §7] | As A on the fallback |
| Bounces | One bounce plus feedback; bounded by frame budget | Settles in 13-17 ray-free gathers, energy balance 1.008-1.010 [V, wp/R-02] | Tiers differ; Q12 band must absorb it |
| Visibility | Shadow tiles, room scoping, plan field of one story: 0.0017 % of pairs differ from the CPU with the half-float field [V, wp/S2-03]; no furniture occlusion in the indirect; no walk between stories [V, open issue] | Exact against the proxy and object boxes, both levels; closed room is bit-exact 0 [V, wp/R-02] | - |
| Precision | Half-float targets; 512 slots native guaranteed, 4 096 reported, DXVK 32 768 [V, wp/S2-03] | 32-bit float buffers on the device; result written as half float | - |
| Development speed | Gather not started. Known faults: `break` in a loop never leaves, a tri-state is miscompiled [V, wp/S2-03]; hand slot counts were low by 1.5-2.5 times [V, wp/S0-05] | Kernel done offline (R-02); bridge prototype in progress (R-03); R-04 moves the kernel into the worker | Everything of A plus B plus parity work for each feature |
| Maintenance | Apex only | One more bridge patch on a series of 15; the per-pixel tier adds two DXVK patches on a series that already builds from source [V, `native/dxvk`, branch `feature/dxvk-patch-series`] | All of it |
| GPUs without ray query | Works | Level `compute`: same kernel, same result, slower links [V, also on an integrated GPU] | - |
| Players without the bridge (in-process DXVK, native Direct3D 9; both stay supported, README "DXVK without the bridge remains available") | The only option that gives them bounce light | Nothing from the server; §5 applies | A |
| Failure modes | A long pass stalls the render thread; address space; shader refused on a native device | Server export absent or level `none`; a stalled worker; device reset; second device on an odd driver [O, R-03] | Tiers drift apart |

Reading the table: A wins one row, players without the bridge. B wins or ties every other. C pays for A to serve
that one row, and the parity rule turns every later feature (directional layers, window emitters, exact furniture)
into two implementations.

## 3. Which device in the server

### 3.1 What changed since server-export §4 was written

- The package no longer has to ship a stock DXVK. DXVK is built from source at v3.1.1 with a patch series, and
  `package.ps1` takes that build when present [V, `native/dxvk/README.md` on `feature/dxvk-patch-series`]. A patch
  that makes the ray-tracing extensions optional is being added there. R-08's cost line "the package stops shipping
  a stock DXVK" is already paid.
- Research 16 found that a post-scene pass needs no shader-translator change, only an entry that records foreign
  Vulkan commands in DXVK's stream (about 100 lines, [I]). R-08's no-go stays for rays inside game shaders.
- R-03 is building the second-device worker with a host copy and `UpdateSurface` [V, branch `gi/r-03`, in progress].

### 3.2 Second device against DXVK's device

| | Second device (server-export §4, R-03) | DXVK's device (`ExecuteExternal`, research 16 route D2) |
|---|---|---|
| Needs from DXVK | Nothing; works with any DXVK, including a player's own | A patched build with the new interface; stock DXVK gives no GI at all |
| Long dispatches (links, structure builds) | On an own queue and thread; the driver time-slices [I] | In DXVK's command stream, inside the frame; must be cut to about 1 ms pieces across frames |
| Result into the light buffer | Host read, `memcpy`, `UpdateSurface` under a 1 MB budget a pass [I, R-03 measures] | Image copy in stream order, same frame |
| This frame's depth, G-buffer | Not reachable without shared images or host copies, 2-5 frames late [I, research 16 §6] | Bound directly, 0 frames |
| State | Being built | Entry not built (XR-03 open) |

A texel solve is rare, long and asynchronous by design; a layer is 320 KB and is frozen once settled. That is what
the second device is good at. A per-pixel pass is short, runs every frame and needs the frame's depth. That is what
DXVK's device is good at. Neither replaces the other at lower cost than keeping both: the proxy scene is about 10 k
triangles, so a second acceleration structure is a few MB of server memory.

The per-pixel pass can still read the lamp layers: they are Direct3D 9 textures on DXVK's device once uploaded.

### 3.3 Triggers to move the texel solve to DXVK's device

Any one of these, from R-03's self-test or R-G1:

- `UpdateSurface` of a settled lot's layers stalls the command loop over 1 ms a pass at the 1 MB budget, or an edit
  takes more than design §7's 30 frames to show because of that budget.
- DXVK's frame time rises by more than the 2 ms GPU budget while the worker is busy, at any slice length.
- The second device fails to open or loses the device on a supported driver.

Then R-04's kernel is recorded through `ExecuteExternal` instead (about 2-4 h on top of XR-03 [I]); the client
exports of §4 do not change.

## 4. API surface between client and server

The client side is the same for either device, which is why §3 can stay open without blocking Apex work.

Client exports on the bridge's `d3d9.dll` (unchanged from server-export §6, found with `GetProcAddress`):

| Export | Use |
|---|---|
| `RemixBridge_GiOpen(uint32_t* pLevel)` | Level 0 none, 1 compute, 2 rays. Proposed: bit 8 set when the per-pixel entry is present (DXVK reports the interface and ray query) |
| `RemixBridge_GiSubmit(data, size, kind, pTicket)` | Scene (proxy, boxes), lamp table, texel points, capsules |
| `RemixBridge_GiSolve(target, layer, sceneTicket, pTicket)` | One (lamp, story) layer or direct map into a DEFAULT-pool texture Apex never locks |
| `RemixBridge_GiPoll(ticket)` / `RemixBridge_GiCancel(ticket)` | Shared-memory read; no command |
| `RemixBridge_GiQueryPoints(ticket, out, size)` | Rig values (R-06) |
| `RemixBridge_GiPixelPass(...)` (XR-06, later) | Ids of depth, normal, motion and mask textures plus a block of about 8 KB a frame |

Contract Apex relies on:

- A missing export, `E_NOTIMPL`, level 0, or no answer to `GiOpen` within 100 ms means `d3d9`, decided once at start.
- A draw issued after `GiPoll` returns `S_OK` samples the new contents (one ordered command stream [V,
  server-export §6]).
- All tickets die at device reset; targets are released before it and named again after.
- The server holds no game state: the scene is resubmitted after a reset or a lot change.
- Read-backs of render targets are not part of this surface; they use patch 0017's three exports [V,
  handoff-async-readback.md]. The GI patch follows 0017 and shares its poll points.

Server to DXVK, per-pixel tier only: `ID3D9VkInteropDevice2::ExecuteExternal` and `SupportsRayQuery` (research 16
§3.2). A stock DXVK answers `E_NOINTERFACE` and bit 8 stays clear.

Data layouts shared by both back ends, so that pictures compare: the layer atlas, the direct maps, the look
constants and the compose pass (S2-01), which stays a Direct3D 9 pass in Apex because it feeds the `SetTexture` swap.

## 5. The minimal fallback

`backend = d3d9` after this decision:

| Piece | Fallback |
|---|---|
| Direct light | S1 as planned: Apex's accumulation with shadow tiles. It does not depend on the server |
| Indirect light | `roomAmbient` only: one value a room from the lamp table and the room's reflectance, computed on the CPU and added in the compose. No gather, no plan field, no feedback pass |
| Movers | Rig write from lamps with CPU visibility (S3-03); no ambient-cube faces |
| Sim shadows | Capsule occlusion in the AO pass (S3-06), which is also the level without ray hardware |
| By day | Lot handed back to the game's solve (`nightOnly`), as today |

If S1 does not pass G-stage1 on a renderer without the bridge, the feature is off there and the game's own solve
lights the lot: nothing is lost against today.

A worker that stops answering (watchdog, 1 s) does not switch back ends in mid-session: the layers already composed
stay, new edits fall to `roomAmbient`, and one log line says so. If the server process itself dies there is no
picture at all under the bridge, so no GI fallback applies to that case.

What a player without the bridge loses against plan C: one bounce with plan visibility. Parked, not deleted: S2-03's
field and trace are proven on the GPU and stay in the tree for X-05 and for a later decision to build the gather.

## 6. Proposed changes to plan.md (not applied)

| Package | Change |
|---|---|
| G-server | Decided: pass. The server leads S2 |
| G-dxvk | Add to the gate table: decides the per-pixel tier (after XR-03); not a gate for S2 |
| R-03 | Unchanged. Add to its report: the three measurements of §3.3 |
| R-04 | Unchanged in scope. Add: convergence by fixed gather count on the device, no read-back a bounce (wp/R-02 open issue); the level is a build option of one kernel |
| R-05 | Fallback target is §5; the watchdog freezes layers instead of starting a Direct3D 9 solve. `backend` values: `auto`, `server`, `d3d9`; `serverRays` / `serverCompute` developer overrides |
| R-06, R-07 | Unchanged |
| R-08 | Add a line: the verdict covers rays inside game shaders; post-scene passes are research 16 |
| XR-01 to XR-12 | Add as a track to plan.md (X-RTPP left them out). XR-01 is covered by the DXVK build that exists; XR-11 (primary rays on the second device) stays the fallback of G-dxvk |
| Research 14 WP11 | Replaced by XR-03 + XR-06 (research 16 §3.5) |
| S2-01 | Unchanged; needed by every tier |
| S2-02 | **Parked** (Direct3D 9 gather) |
| S2-03 | Closed as done offline; the Apex wiring is parked with S2-02. Its "server form" is dropped: the `compute` level walks the proxy grid in 3D and needs no plan field [V, wp/R-02] |
| S2-04 | Split: feedback bounces are R-04's gathers (already in the kernel); only `roomAmbient` remains, as the fallback term of §5 |
| S2-05 | Server only: cost against lamp count on both levels |
| S2-G1 | Depends on R-05 (drop "S2-02 or") |
| "Order inside S2" | Replace with: R-03, R-04, R-05, then S2-01's compose against server layers, then S2-G1 |
| S3-04 | Directional layers come from the server kernel (four-face output of the gather in R-04's worker); the swap stays |
| S3-05 | Depends on R-06; drop "or asynchronous read" |
| S5-04 | **Parked** (bake-in caster cache existed only for the Direct3D 9 back end; R-07 serves the server) |
| S6-04 | Stand-down matrix states the fallback of §5 for native Direct3D 9, in-process DXVK and a bridge without the export |

Saved: S2-02, the S2-03 wiring, the Direct3D 9 parts of S2-04 and S3-04, S5-04: five packages of 1-2 h each plus a
parity check for each later feature. Added: none; XR packages were already proposed by research 16.

## 7. Proposed changes to design.md (not applied)

- §2 D7: "Two back ends behind one carrier"; the server solve is primary, Direct3D 9 keeps direct light and a room
  ambient term.
- §3.9 back-end table: rows `serverRays` and `serverCompute` become one row `server` with two levels; row `d3d9`
  reads as §5 above. Device row: add that a per-pixel tier uses DXVK's own device through a patched build and that
  the package builds DXVK from source.
- §3.9 "What moves to the server", last row: "Not in this tier" becomes "research 16, gate G-dxvk".
- §5 `backend` values as in §6; §7 "CPU, steady, Direct3D 9 back end" applies to S1 only.
- §10: drop "Back ends drift apart" for the indirect solve; add "a player without the bridge gets no bounce light".

## 8. Open

- [O] Every GPU time above. R-G1 and XR-09 measure on a quiet machine; this memo does not depend on a number, only on
  the Direct3D 9 gather not being faster to build or better to look at, which no timing can change.
- [O] Two devices side by side and the upload cost (R-03). §3.3 names what happens if they fail.
- [O] `ExecuteExternal` is not built. If G-dxvk fails, the per-pixel tier falls to XR-11 or to capsule occlusion;
  the texel decision is untouched.
- [I] `roomAmbient` as the only indirect term of the fallback is judged acceptable without a picture. If look pairs
  in S1-G1 show the fallback too flat, the parked gather is the answer, at its known cost.
