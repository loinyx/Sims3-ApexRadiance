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
| `resource_paths_fixture.cpp` | Resource restoration around the extracted draw paths | `run_resource_checks.ps1` | Native D3D9 GPU |

These fixtures execute extracted production logic, not the TS3 engine or driver. They do not establish gameplay latency,
FPS or absence of visible flicker.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
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

- Combined build, 2026-09-24: visitor patch, arm sites and dusk rebuild put lot lamps on the world grass.
- test007 build (before 2.5.3): the paced sweep, adopted as the default in 2.5.3.
- 2026-10-02, private response build `76B4C32DD5749D6FB6516971D3FDB30092259D805CE97D0B20B8E3363558C300` (released as the
  2.5.4 hotfix with the same lighting logic): lighting response in the tested scene accepted, perceived as more
  responsive (maintainer feedback, no instrumented input-to-screen or FPS measurement).

## Open checks

- PR #2: endpoint rebuilds in Build mode preview, near-camera priority, final state after rapid reversals, fallback
  frequency and hitches; priority edits merged with an armed countdown; the edit dispatch fixture run.
- Gameplay input-to-screen latency and FPS for every route; universal latency is not guaranteed (render gates, caps and
  timeouts remain).
- Roads after a local relight (`+0x54` does not set the road partition mark).
- Whether the game rebuilds by itself after Build-mode lamp edits.
- Whether the street-lamp class test (vfunc+0x20) needs the lit flag.
- Interaction with the official Split-Level Lighting Fix: inferred from the decompile, not tested in game
  ([level light share](../features/night-lighting/level-light-share.md)).
- Winter variants, unverified lot shader variants and DXVK.
