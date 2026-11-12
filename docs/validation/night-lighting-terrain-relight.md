# Terrain relight: validation

The feature is described in [features/night-lighting/terrain-relight.md](../features/night-lighting/terrain-relight.md).
It must:

- Put the light of lit outdoor lot lamps on the world grass outside the lot, and leave disabled or unlit lamps out.
- Rebuild once after a world load, after it is drawn and at a steady night level, never during the loading screen.
- Rebuild at each settled day and night endpoint, with no stale delayed rebuild after a reversal.
- Reach the ground for every placed, moved, removed, switched, dimmed or recoloured lamp, without a second off/on cycle,
  and never rebuild twice for the same lamp state.
- Never rebuild for streaming (lots loading or unloading) or for lamps switching together at dusk and dawn.
- Avoid the ~240 ms full-rebuild frame for lamp changes and endpoint rebuilds whenever the local relight or the paced
  sweep can take them, without stalling the game's own chunk sweep.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/terrain_lighting_test`](../../tools/terrain_lighting_test/README.md) (`check.cpp`) | Day/night endpoint debounce, reversal cancellation, world-load merging, disabled scheduling, 200,000 deterministic stress frames; Build preview zero phase delay, preserved Live delay, captured rapid-switch timeline replay, at most four nearest priority chunks with a known eye; all edit-priority combinations of `DeferDayEdit` | `run.ps1 -Captures ... -OutDir ...` | Native D3D9 GPU (whole suite) |
| `queue_check_fixture.cpp` | Extracted `Attach`, `QueueSweep`, `OnPresent`, completion and refusal helpers and limits against fake native memory and timing: 3,000 randomized phase handoffs, an old completed bake awaiting consumption, nearest-first order, preserved batch ownership, non-mutating sweep refusals, native rebuild and render gates, rolling caps, terrain-owner changes | `run_queue_checks.ps1 -OutDir ...` | None |
| `edit_dispatch_fixture.cpp` | The real `DecideEdit` guards up to the snapshot compare: both former early exits, the priority route, snapshot wait, automatic serialization, load/phase merge; optional `-Baseline` reproduces the old behaviour | `run_dispatch_checks.ps1` | None |
| `lod_hold_fixture.cpp` | The real `ReadDetailSwitches`, `DetailOf`, `LodHolds`, `InBake`, `DiffBake` and `BakeTest` (fake lot renderer map and light record): the detail set (a lot leaving it recorded for 2 s, its detail time, unknown past 16 detailed lots or without a map); a demotion on a lot that is not detailed held with no snapshot difference, the same flip on a detailed lot (a sale) leaving the bake, the promotion after a hold with no difference, every release (day, edit mode, lit bit, light, place, lot, 30 s of detail) a listed switch-off, no hold for lamps never in the bake, world lamps or an unknown detail set, a held lamp gone with its lot a removal only while the lot is among the settled lots, the hold asked only for alive, lit, disabled outdoor lot lamps; the real `RefreshLotRects` and `LotGroundY` on a fake lot table: a lot that left the light tree keeps its last base with no read, a lot never read reads the table once a frame. `TrackLotLampEdits` (the kept and enabled-again routes, the arrival on a promotion) is not covered | `run_lod_hold_checks.ps1` | MSVC x86 |
| `stamped_fixture.cpp` | The real `AllStamped`: the excluded light matched by its id at any distance, other additions within 1.5 m, a lamp 3.64 m away and unpaired removals sent to the relight, nothing excluded | `run_stamped_checks.ps1` | None |
| `resource_paths_fixture.cpp` | Resource restoration around the extracted draw paths | `run_resource_checks.ps1` | Native D3D9 GPU |
| `tools/terrain_stamp_test` (`check.cpp`) | The game's bake pixel shader against the CPU law and its recognition (size, FNV-1a); `StampPS` against the game's shader on flat ground, 8 lamps in one pass against 8 additive 8-bit passes, chunk borders, the base kept, its GPU time; `BakeConePS` without cones against the game's shader and with spot and shaded-lamp cones against the CPU law times the cone factor; `StampPS` with cones against `BakeConePS` (T0 to T9) | `run.ps1 -Precomp <Shaders_Win32.precomp>` | MSVC x86, Direct3D 9 device, the game's precomp (read only) |
| `tools/terrain_stamp_test` (`state_check.cpp`) | The dragged-lamp states of `terrain_stamps.cpp` against a simulated terrain: a drag across a chunk border, pauses, a slow drag, same-map re-renders, deletion, the GPU smoothing off, refused relights, a lamp dragged again while it returns, two lamps, a lamp never baked, a lamp picked up again mid-drag, a lamp out of the list for good (S1 to S12) | same `run.ps1` | MSVC x86 |

These fixtures execute extracted production logic, not the TS3 engine or driver. They do not establish gameplay latency,
FPS or absence of visible flicker.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-11-12 | not committed (3.1.2 working tree) | LOD hold fixture (`run_lod_hold_checks.ps1`) | 42 passed, 0 failed; with the `LotGroundY` fallback removed, its check fails | None |
| 2026-10-29 | `7eca05b` | Edit dispatch fixture, with a `RebuildActive` stub and its executable renamed (Windows asked to elevate a program named `dispatch_checks.exe`, which is why it was cancelled before) | 194 passed, 0 failed | None |
| 2026-10-29 | `7eca05b` | Terrain queue fixture (`run_queue_checks.ps1`) | Does not compile: the paced full rebuild (`g_rb`, `StepRebuild`, `kSlowMs`) is not in its extracted code; with it added, 386 of 11,963 assertions fail on the release rules of chunks changed since. Not updated in this release | None |
| 2026-10-29 | `8d13e95` (a dragged lamp's ground light on the GPU) | Developer recordings of lot lamp drags, before and after | Local terrain relights during the drags: 127 (2,155 ms of CPU in 73 s) before, 7 (94 ms in 101 s) after | In game, backend not recorded |
| 2026-10-28 | `373445c` (a dragged lot lamp reaches the ground past the lot edge) | Developer recording of a drag of a lamp Real-time outdoor lamps draws | The terrain relit 23 times in 20 s of drag (before: up to 7.3 s without a relight, and the ground could keep the lamp's old light after it was put down) | In game, backend not recorded |
| 2026-10-04 | `92582b6` | Full terrain lighting suite (policy, cycle, GPU, resources) | 2,017,393 passed, 0 failed (2,003,751 GPU/policy + 13,642 resource) | Native D3D9 |
| 2026-10-04 | `92582b6` | Edit dispatch fixture | Compiled; execution cancelled by the system, not run | None |
| 2026-10-04 | after `92582b6` (priority follow-up) | Queue and resource fixtures | Passed; native D3D9 device creation failed, so GPU pixel checks were not run | None |
| 2026-10-02 | 2.5.4 candidate | Extracted tracking, snapshot and queue checks: repeated switches on two lots, zero-intensity switches, visible colour, nonzero and sub-threshold intensity edits, offscreen animation suppression, native registration and value notices, unchanged/fade/window exclusions, old/new arrival rectangles and disabled receipts, malformed rectangles, queue owner preservation, in-flight re-render, debounce bounds, measured-cost reserve | Passed | None |
| 2026-10-02 | 02:03:26 follow-up | Replay of the tracking and snapshot functions for 28 lamps on two lots: early entry, repeated switches, simultaneous unrelated streaming, intensity animation, identity reuse, zero light, excluded pending baselines | Passed | None |

## In-game test plan

Rebuild triggers:

1. Load a save by day and wait for dusk. **Expected:** `Rebuild armed: dusk` or `Terrain sweep started: dusk`, then the
   rebuild or `Terrain sweep done`; grass around street and lot lamps lights up.
2. Load a save at night. **Expected:** one `world load (night: also the dusk rebuild)` rebuild 1 to 2 s after the world is
   first drawn, no second dusk rebuild; `World live: world terrain drawn after X s` in developer mode.
3. Build mode, switch day/night repeatedly, including fast reversals. **Expected:** each settled endpoint rebuilds without
   the saved delay; the final state matches the final phase; no stale delayed target.
4. Live mode at dusk and dawn. **Expected:** the endpoint rebuild after `atrasoSegundos`.

Lamp changes (night and Build-mode day preview; stationary and moving camera):

5. Place, move, delete, recolour, dim and switch a lot lamp, inside and outside lot borders. **Expected:** log `relit
   locally ... chunks (ix,iz)` with the expected chunks; the Hitches file shows no `DXT x512` frame, only a few frames with
   `DXT x8` and terrain about 3 ms; grass, lot grass, roads, sidewalks, snow and fences show the change.
6. A lamp near a chunk border (x or z = k x 256, for example 1280). **Expected:** both chunks relit back to back, no lasting
   step once smoothing settled.
7. Enter a newly loaded lot and switch a lamp off once; then all-lamp off, all-lamp on, all-lamp recolour, small intensity
   adjustments and a continuous adjustment. **Expected:** the final state on the ground without a second off/on cycle;
   record input-to-visible time and profiler spikes.
8. A game rebuild while a batch is queued. **Expected:** the batch is dropped. A lamp change during the load rebuild takes
   the full path ("the world's first terrain rebuild has not run yet").
9. Map view, CAS, Edit Town, save / load. **Expected:** no "was not re-rendered within" warning; the game's own sweep
   still completes (`Rebuild sweep done ... 0 not re-rendered`).
10. 20 minutes of night play while moving. **Expected:** a few local relights per minute, none from streaming.
11. Developer status. **Expected:** "Lot lamps: armed A | on the ground B | off O" with B > 0 on lots with lit lamps;
    pending lamp changes return to none.
12. Paced sweep at an endpoint. **Expected:** chunks near the camera light first; no ~240 ms frame.

## Confirmed in game

- 2026-10-29, developer build of `7839649` (2.18.1): a lamp deleted right after another lamp of the same type on the
  same lot was dropped from a GPU drag lost its ground light at once (player report); the drag steps kept being skipped
  (log: `skipped: drawn on the GPU while dragged`).

- Combined build, 2026-09-24: visitor patch, arm sites and dusk rebuild put lot lamps on the world grass.
- test007 build (before 2.5.3): the paced sweep, adopted as the default in 2.5.3.
- 2026-10-02, private response build `76B4C32DD5749D6FB6516971D3FDB30092259D805CE97D0B20B8E3363558C300` (released as the
  2.5.4 hotfix with the same lighting logic): lighting response in the tested scene accepted, perceived as more
  responsive (observed in game, no instrumented input-to-screen or FPS measurement).

## Open checks

- The LOD hold, at night on a save with lit lamp posts on neighbouring lots: pan across lot borders so lots swap
  Detailed View. **Expected:** no `Lot lamp change: ... (manual-driven)` line and no `switched off (user-driven)`
  decision for a lot leaving Detailed View; `Lamp changes that do not rebuild the terrain` lists `kept in the bake
  (Detailed View demotion)` and, on the promotion, `enabled again after a Detailed View demotion`; the developer status
  counts the lamps kept; no local terrain relight on a demotion; on a promotion at most one `Entry relight queued:
  visible lot <id>` for that lot (also expected without the hold when demoted lots are not drawn). Trees, water and
  roofs beside a held lamp keep its light, never brighter than before the demotion. A timeline of the pan, before and
  after, gives the time saved per swap.
- The ground of a held lot, with a lamp less than 40 cm high on it. Capture the ground before the demotion; during the
  hold, re-render the lot's chunk (a neighbour lot's entry relight, or *Refresh lighting*) and capture again; capture a
  third time after the promotion. **Expected:** the three captures match: the low lamp keeps its lifted pool, not a dot.
  The same after a pan with several swaps, the promoted lot's ground compared with its capture before the demotion
  (a chunk re-rendered between the demotion and the read that starts the hold leaves the lamp out until the
  promotion's entry relight; the bake's lot and story gate at 0x00C294D9 is unverified).
- The objects beside a held lamp on its promotion. **Expected:** roofs, water and trees beside its pool do not go dark
  and fade in again: held rows keep the enabled fade and switch to the live `+0x20` fade at the promotion read. If the
  game fades `+0x20` down while a lamp is disabled, keep the larger of the two fades until the live one catches up, or
  keep held lamps out of the per-pixel rows.
- Lamps registered again on a promotion (new light objects at a held lamp's place were seen for one lot, cause
  unconfirmed). **Expected:** no lamp counted twice: one pool on the ground, one per-pixel row. If both are listed, end
  a hold when a lamp in the bake that is not held appears on the same lot, of the same type, within 5 cm.
- The per-pixel cost of held lamps: `RoofStatus` *lamps N*, the `SelectLamps` memo misses and the Frame Profiler's
  render-thread time, before and after a night pan across 10 or more lots. **Expected:** no material growth; otherwise
  keep held lamps out of the per-pixel rows, as before the hold.
- The same at dawn. **Expected:** the held count drops to 0 on the first day frame, before the daylight rebuild. Whether a
  demoted lamp's lit bit clears at dawn (`lit 1->0` on a held lamp) is unverified; if it does, the day release can go.
- Build mode entered with lamps held, and a demoted lot bulldozed in Edit Town. **Expected:** entering the edit mode ends
  the holds with one local relight or a paced sweep; a bulldozed lot's light leaves the ground.
- Unverified for held lamps: whether a lot out of Detailed View still draws its lamp posts (pools without posts), and the
  bake's lot and story gate at 0x00C294D9 (light +0xD0).
- A full terrain rebuild or a lot load. **Expected:** no object drawn just after it with wrong rig lamps. The cone rows
  (c5..c7) of a bake that does not run inside the chunk render call are given back only at the game's next other pixel
  shader; whether those bakes go through that call has not been checked.
- Night Lighting turned on mid-session, then a lamp dragged. **Expected:** no old light beside the GPU stamp. Lamps the
  game baked before the hook are not in `g_everBaked`, so the drag start does not relight their old place.

- PR #2: endpoint rebuilds in Build mode preview, near-camera priority, final state after rapid reversals, fallback
  frequency and hitches; priority edits merged with an armed countdown.
- The terrain queue fixture brought up to the paced rebuild and the current release rules of chunks.
- Gameplay input-to-screen latency and FPS for every route; universal latency is not guaranteed (render gates, caps and
  timeouts remain).
- Roads after a local relight (`+0x54` does not set the road partition mark).
- Whether the game rebuilds by itself after Build-mode lamp edits.
- Whether the street-lamp class test (vfunc+0x20) needs the lit flag.
- Interaction with the official Split-Level Lighting Fix: inferred from the decompile, not tested in game
  ([level light share](../features/night-lighting/level-light-share.md)).
- Winter variants, unverified lot shader variants and DXVK.
- Dragged lamps on the GPU (`8d13e95`, `91af563`), Lamp lighting Instant, at night: drag a lot lamp across the lot edge
  and across a chunk border (x or z = k x 256), pause, go on, put it down; then drag a lamp new from the catalog, and
  pick a lamp up again mid-drag. **Expected:** the grass and road past the lot follow the lamp at every move, with no
  frame where its light is doubled or missing beyond a few frames when a chunk changes from the bake to the stamp and
  back; once the lamp is still the ground keeps its light where it stands. `tools/terrain_stamp_test` runs these states
  on a simulated terrain only; sloped ground (the stamp lights flat ground) is not covered.
- Lamp cones in the bake (`91af563`): a spot and a shaded wall lamp near a lot border, after a terrain rebuild.
  **Expected:** the pool under the shade continues past the border with no straight cut; no light on the ground behind
  the spot; the log line `Terrain bake: lamps light the ground through their cones` once. Not yet checked in game.
- Lamps the game switches on the load screen (`8172a29`): load a save at dusk or night with lamps on a timer.
  **Expected:** no lot's rooms solve at the lamp edit budget during or right after the load (no run of 36 ms frames),
  while a lamp switched by the player after the world is live still relights at once.
