# Salvaging the dropped performance and AO ideas

Research note, 2026-11-17. Desk work only: source and docs read in this worktree, nothing built, no game run. Inputs:
[00-camera-pan-profile.md](00-camera-pan-profile.md) §6, §9, §10; [07-remix-and-prior-art.md](07-remix-and-prior-art.md) §1
(bridge call table); `features/compositor_readback.*`, `features/pick_readback.*`, `features/lot_light_bridge.cpp`,
`features/lot_lighting_motion.h`; [ambient-occlusion.md](../../ambient-occlusion.md),
[removed-features.md](../../../removed-features.md) (AO history, Service Frame Budget); the ts3native study
"Camera stream priority" (its docs/278: size-first scene drain, smaller finalize slice while moving).

Tags: **verified** = read in source or measured in the profile (section given), **inferred** = reasoning, not checked.

## 0. One fact changes several verdicts: what a bridge read-back costs

From the bridge call table (07 §1, verified in the bridge source there):

- `GetRenderTargetData` **always waits**: the client queues the call behind every fire-and-forget command not yet
  replayed and blocks until the server has replayed them all, run the copy and sent the bytes back.
- Queries are answered asynchronously (`client.asyncQueries=true`); `LockRect` on a system-memory surface is client-side
  only; `Get*` state calls are answered from the client's own copy (can be stale, rtx-remix-bridge.md patch 0011).
- `client.dropRedundantState=true` already drops repeated `Set*` calls client-side.

So under the bridge (the only tested renderer since 2026-11-15) a read-back costs **the server's replay backlog plus
the copy**, not a GPU wait the client can hide with an event query [inferred from the table + profile §6: 2.1-2.5 ms
"even after an event query says the GPU has finished"]. The client-side event query does not know how far the
*server* has replayed. Consequences:

- Deferring the *lock* (compositor, item 1) gains nothing under the bridge: the wait is in `GetRenderTargetData` itself.
- 4.0.2's pick deferral helps by **halving how often** the pick is read (544-562 -> 290-306 per 10 s), not by making a
  read cheaper (profile §6, verified).
- Bytes matter little; a read-back of 4x4 pixels likely costs nearly the same as the full target [inferred].
- A **second** read-back right after a first should be cheap: the backlog was just drained [inferred, untested].

**Step 0 microbenchmark (prerequisite for items 1, 2 and 6; ~1 h code + one 10-min run, development build):** on a
still camera in a loaded world, from the Present hook every 60th frame, time with QPC:
(a) `GetRenderTargetData` of a 4x4 RT, (b) of a 256x256 RT, (c) of a full-screen RT, (d) (a) again immediately after (c),
(e) (a) right after Present returns vs. at Scene::BeginFrame, (f) 1000 x `GetVertexShaderConstantF` and 1000 x
`GetTexture`, (g) how often the pick's event query returns `S_FALSE` (existing `c_frames` counter, "frames waited", in
the developer log). Decision table:

| Result | Meaning | Unlocks |
|---|---|---|
| (a) ~ (c) | Cost is the round trip, not bytes | Tiny pick read-back is useless; batching is the lever |
| (d) << (a) | Second read-back is cheap | Item 1 batch, merge pick + compositor in one burst |
| (e) varies a lot | Backlog-dependent | Place read-backs where the backlog is smallest |
| (g) ~ 0 waits | Event query is answered client-side | Pick deferral = a fixed 1-frame delay; rate is the only lever |
| (f) < 0.1 µs | `Get*` local | Item 6 gains nothing from a constants shadow |

## 1. Deferred texture compositor read-back

**Failure cause (verified, `compositor_readback.cpp`).** Deferral returned 2 from the readback call (line ~179), which
the queue loop FUN_00608270 treats as "stop for this frame"; on the next frames the queue hook returned 2 again while the
query was pending (up to `kMaxWaitFrames` 8 / 250 ms, line ~132-135). The queue runs only its head builder, so the whole
compositor dropped to **at most one tile per frame** instead of several per 5 ms slice: Sims finished later. Second cause
(new, inferred from §0): under the bridge the lock was never the wait, so even a perfect deferral saves ~0 there; it was
built for DXVK in-process, where the lock waits.

**Mitigations.**

| # | Idea | What it fixes | Risk and guard |
|---|---|---|---|
| 1a | **Batch, one round trip per slice.** At the tile's readback, `StretchRect` the tile into a slot of one atlas RT (async under the bridge) and let the builder move on; at the end of the queue's 5 ms slice do **one** `GetRenderTargetData` of the atlas and copy each slot into its builder's staging texture `[b+0x20]` (client-side lock, §0), then run the game's lock/finish (state 3) for each | N round trips -> 1 per frame | Builders must not read the staging bytes before state 3 (verified: state 3 already re-runs FUN_005fdbf0 without the copy). Atlas full -> flush early. Format must match (256x256 A8R8G8B8 per rtx-remix-bridge.md `0x5FBCE0`) |
| 1b | **Keep the queue running.** Instead of 2, make a deferred builder yield its head place (move it behind the next builder) so the loop keeps stepping other builders this frame | Restores several tiles per slice | Needs RE: is the queue order meaningful (same Sim's layers, thumbnails), and does the loop accept a rotate? If order matters, rotate only between builders of different owners |
| 1c | **Merge with the pick.** Do the atlas read-back right after the pick read-back (or vice versa) in Scene::BeginFrame | The second read-back rides on the drained backlog | Only if Step 0 (d) shows the second one cheap |
| 1d | "Placeholder" return so the builder finishes with stale bytes | | **Rejected**: wrong textures on Sims (a household-visible change); never ship |
| 1e | Ring of event queries, read-back on a later frame without ending the queue | | Gains nothing under the bridge (§0); keep only for DXVK users, low priority |

**Gain.** Profile run D: about 10 compositor read-backs per 10 s, 25-31 ms (2.5-3 ms each) in a pan; rtx-remix-bridge.md
measured ~1.9 ms a frame in live play. The win is in CAS-heavy moments (Sims arriving on a lot, Create a Sim, a new
household): N tiles in a frame -> one round trip. Estimate 1-5 ms on those frames, ~0 on a plain pan [inferred].
**Cost:** 1a ~3-4 h (atlas, slot table, end-of-slice hook at the queue loop's exit, reset handling) + RE of 1b ~1 h.
**Test:** dev counters "tiles per frame", "round trips per frame", ms in `GetRenderTargetData` per frame; repro: 8 Sims
spawned onto the active lot in one step (teleport, not waiting), ABBA 10 min each; pass = round trips per tile < 0.5 and
the frames with compositor work >= 25 ms down; correctness = a texture hash of every built Sim texture equal on/off
(the bytes read are the same; any diff = slot bug). **Apex PR: yes** (their file and hooks).

## 2. More pick deferral while the camera moves

**Failure cause (most likely, inferred; the variant is reverted, so not checkable).** The read-back count stayed at
290-306 with and without it. At ~110 frames a second (run D: 544-562 read-backs = every 2nd frame) a forced 4-frame
wait would have dropped the count to ~180 per 10 s, so the moving branch **never fired**. Candidates, in order:
(a) it used `LotLightingMotion::CameraMoving()`, which is false unless Lot lighting while moving or the wall shading gate
is running (lot_lighting_motion.h:35, `.cpp:911`) and only sees samples taken by those features; `SampleCameraMoving()`
samples itself; (b) the wait sat after the "query signalled" exit, so `GetData == S_OK` ended it at 1 frame (§0 (g): if
the query is answered client-side it signals at once); (c) the camera offsets were still unknown (`EnsureCamera` false).
Fix: use `SampleCameraMoving()` and count "frames held for motion" in `StatsText`.

**Better than a longer wait: skip the pick when nothing can have changed.**

| # | Idea | Gain | Risk and guard |
|---|---|---|---|
| 2a | **Skip when still.** Before the render step (flag `[mgr+0x14]` clear), skip the update when cursor position, camera eye/target and window size are unchanged since the last pick, and the last pick is < 250 ms old | Live play with a still camera and mouse is most of play time: removes most of 290-306 read-backs x 2.2 ms per 10 s (6-8% of render-thread time, profile §6) | Sims walking under a still cursor: highlight up to 250 ms late. Guards: never skip on a mouse button or key event this frame, after a mode change (Live/Build/Buy/CAS), or while a drag/placement is active; RE first whether the click path reads this async result or does its own pick |
| 2b | **Rate cap while moving.** With `SampleCameraMoving()` true, read back at most every 4th frame (the reverted idea, done right) | Halves again during pans | Hover lag during pans only; same input guards |
| 2c | Tiny read-back (`StretchRect` 4x4 around the cursor -> tiny RT -> `GetRenderTargetData`) | Probably ~0 under the bridge (§0) | Also needs RE of what the game reads from the target (one pixel, or a box for marquee/placement); keep only if Step 0 shows bytes matter, and only for DXVK |

**Cost:** 2a ~1-1.5 h + ~1 h static RE (pick manager readers of `[mgr+0x15]` and the result surface; who sets cursor
and camera); 2b ~20 min. **Test:** dev counters "picks skipped (still)", "(moving)"; repro A: still camera, cursor
parked, 10 min: read-backs per 10 s must drop >= 80%; repro B: scripted cursor sweep over a Sim and an object every
2 s: hover highlight must appear within the guard bound (log the frame the pick result changes vs the frame the cursor
arrived); repro C: harness pan (run D) with 2b: read-backs <= ~180 per 10 s. Clicks: 50 scripted clicks on objects must
select the same objects on/off. **Apex PR: yes**, highest value per hour in this note.

## 3. AO slices by reach on screen

**Failure cause (verified, profile §9 + ambient-occlusion.md).** Slice angles are `(s + b1) pi / SLICES` with `b1` from
the 4x4 Bayer pattern; the depth-aware 4x4 box (0.5,1,1,1,0.5) cancels the pattern only if every pixel of each 4x4 block
uses the **same** SLICES. A per-pixel count made neighbouring pixels sample different direction sets, so the box no
longer cancelled the pattern: blotches.

| # | Mitigation | Why it keeps the interleave | Risk |
|---|---|---|---|
| 3a | **One count per frame, picked by zoom** (median or 75th percentile of 1/z from a coarse pyramid level, read in the shader or chosen from the camera height on the CPU), with hysteresis | Every pixel has the same count | A count switch changes the image a little; far and mid views matched within 0.4-0.6/255 (profile §9), below visible |
| 3b | **Per 8x8 or 16x16 tile, nested counts** (2/4/8 where the 4-slice set is every other angle of the 8-slice set, `(2s + b1) pi / 8`) | Each 4x4 block is inside one tile; nested sets estimate the same integral, so tile edges differ by noise, not bias | Faint tile edges where the 5-tap box straddles two tiles; blend: in the 2 px band at a tile edge use the higher count |
| 3c | Temporal-free blend between counts per pixel | | Cost of both counts in the band; only if 3b shows edges |

**Gain:** GPU only: at 4K on a 4070 Ti SUPER, Medium (6) -> 2-4 slices far saves ~0.5-1 ms GPU. The game is CPU-bound on
its render thread (profile §10.1), so frame time gains only on GPU-bound setups (laptop GPUs at high resolution).
**Cost:** 3a ~1 h; 3b ~2-3 h. **Test:** offline AO lab on the saved depth dumps: max |difference| vs the full-count image
and a tile-edge metric (mean |a-b| across tile borders vs inside tiles, ratio must be <= 1.1); the "shift" metric of the
lab (removed-features.md AO §9) must not rise; in game, GPU timestamp of the AO pass far/mid/near and a still-camera
screenshot pair. **Apex PR: yes for 3a** (small, safe); 3b only with lab proof.

## 4. Smoother depth-reconstructed normals

**Failure cause (inferred from profile §9 + the shipped rule).** The shipped normal takes, per axis, the neighbour with
the smaller depth step. The tried variant switched to 2 px central differences on "even" surfaces. Along a mesh seam
(a counter top's triangle edge) depth has a tiny crease; the "even" test flips pixel by pixel along the seam, so the
normal alternates between the central and the one-sided form: dashes.

| # | Mitigation | Note |
|---|---|---|
| 4a | **Continuous weight instead of a switch:** per axis, extrapolation error on each side `e = |2 d1 - d2 - d0|` (5 taps, the "accurate normal reconstruction" method), weight `w = e_r / (e_l + e_r + eps)`, blend left/right differences | No binary decision, so no dashes; on a plane both errors are ~0 and it becomes the central difference |
| 4b | Game normals via MRT | **Not worth it:** the game is forward-shaded with no normal target; every material shader would need an `oC1` output (patch per shader, all captured shaders tested offline) plus a second full-screen target through the bridge. Days of work for a small visual gain |

**Gain:** visual only (cleaner shade on flat surfaces), 0 ms. **Cost:** 4a ~1 h shader + lab run. **Test:** lab on the
depth dumps with the seam scene (counter top): count of pixels whose normal differs > 5 degrees from the triangle's true
plane along seams, and the shift metric; then one in-game screenshot pair of the same counter. **Apex PR:** possible,
their call; low priority.

## 5. Half-resolution and temporal AO

**Failure causes (verified, removed-features.md AO history §6-9).** Half resolution read one of the four depth pixels of
each 2x2, so a 1-pixel camera move picked another one: micro dots (the 2x2 average still dotted). Temporal smoothing:
v1 clamped the history to an already-averaged range, so it followed noise; v2/v3 fixed most of it, but leaves in the
wind broke the depth test and the raw frame showed through (v3 never tested in game).

| # | Mitigation | Assessment |
|---|---|---|
| 5a | Half-res AO from a **conservative** reduction (min 1/z of each 2x2 for horizons; no reduction is fully shift-stable, so some dots remain) + **joint bilateral upsampling** with full-res depth and the 4a normal | Removes most dots but not all on foliage; the full-res 4x4 interleave is already the cheap form of the same thing |
| 5b | Temporal with **reprojection + disocclusion rejection + camera-motion-aware clamp** (history weight 0 while `SampleCameraMoving()`, ramp back after 300 ms still) | While moving it is plain spatial AO (no gain where frames matter); while still the deterministic AO is already stable. The only use is "fewer slices + accumulate" when still |

**Gain:** GPU only, ~0.5-1 ms at 4K, on a CPU-bound game. **Verdict:** salvageable on paper, but both bring back
artifacts that players reported and the gain does not reach frame time on this setup. Rank last; build only for a
GPU-bound target (Steam Deck, laptop at 4K) with the lab's shift metric as the gate (<= 0.3 levels, <= 0.2% of pixels
> 6 levels, removed-features.md §9). **Apex PR:** not proposed.

## 6. Cutting the per-draw lighting fixes

**Failure cause (verified, profile §7).** The obvious cut (lamp picks) is done: picks reuse >= 95%. What is left is
1-12 µs a draw over hundreds of draws, 3.3-3.8 ms a frame zoomed out, on the CPU-bound render thread; it is spread over
many fixes, and no single change kept the lighting identical.

What §0 removes from the list: **skipping unchanged constant uploads** and a **constants shadow** gain ~nothing under the
bridge: `dropRedundantState` already drops repeated `Set*` client-side and `Get*` is answered locally [verified table;
cost of `Get*` itself: Step 0 (f)]. The time is Apex's own logic per draw [inferred: "own time" excludes the forwarded
draw, `DrawCostSampled` comment in `lot_light_bridge.cpp` ~6106].

| # | Mitigation | Target (profile §7, far) | Risk and guard |
|---|---|---|---|
| 6a | **Sub-step profile first.** Extend the 1-in-16 timed draws (`FenceStepTimer`/`FolStepTimer` pattern) to RoomMapSmooth, "several fixes", OutdoorObjectGroundLight: classify / state reads / lookups / patched-shader fetch / sets / restore | All | none; dev build only |
| 6b | **Per-frame memo by draw identity.** Key = (VS, PS, world matrix rows read from VS c8.., bound textures); value = the fix's computed constants. Camera pans do not move objects, so most keys repeat frame to frame; invalidate on lamp toggle, time-of-day band change, lot relight epoch | OutdoorObjectGroundLight 112-225 x 3 µs; RoomMapSmooth `RoomMapConsts` + `ArchRowsFor` 54-64 x 8-12 µs | Wrong light when the epoch misses a change: compare memo vs computed on every 64th hit in dev builds (counter must stay 0) |
| 6c | **Lazy restore.** After a fix draw, leave its patched PS and constants bound and restore the game's state only before the next draw that the fix does not claim (or on the game's next `Set*` of the same slot) | Consecutive claimed draws of the same fix (floors, fences) | Apex already tracks the game's `Set*` through its detours; a missed path = wrong shader on the next draw: dev check compares tracked vs `Get*` each draw |
| 6d | **Move fixes into a screen pass.** If the GI work (this research folder) lands a deferred lamp pass, fixes it covers (ground light under objects, fence/stair ground light) become one full-screen pass instead of hundreds of per-draw computations | 1-2 ms of the 3.5 | Only with the GI pass shipped and judged equal; tie into the GI plan, not a standalone change |

**Gain:** 6b+6c ~1-2 ms a frame zoomed out [inferred; 6a decides]. **Cost:** 6a ~1 h; 6b ~3-4 h per fix family;
6c ~2-3 h. **Test:** run C (pan at 150 m) with 6a counters, then ABBA per change on the same pan; correctness: still-camera
screenshots at night on 3 lots, byte-equal on/off (the lighting must not change at all), plus the dev memo check = 0.
**Apex PR: yes** (their code; they own lighting).

## 7. Size-first drain and smaller load slice while moving (ts3native docs/278)

**Why judged low-gain (verified there).** Moving hitches are led by single indivisible items (DXT encode, lot load
stage 0, impostor build, a CAS step, terrain update); a per-frame budget cannot split them and big-first ordering moves
the same work inside the same budget. Apex also removed a "Service Frame Budget" in 2026-09 for no perceptible gain
(removed-features.md: services in hitch frames < 1.5 ms each then).

**Salvage: stop stacking, not reordering.** The loss is when deferrable work (finalize jobs, scene drain, lot build
slice) runs **in the same frame** as an indivisible item, adding its 2-6 ms to a 15-68 ms frame.

| # | Mitigation | Note |
|---|---|---|
| 7a | **Deadline slicing.** While moving, each deferrable service gets `max(0, target - elapsed_this_frame)` (target e.g. 12 ms) instead of a fixed slice; if the frame is already past the target when it runs, it skips this frame. Longest-wait bounds of the existing features stay (scene drain 500 ms then growing; finalize: skip at most 3 frames in a row) | Turns "hitch + 5 ms" into "hitch + 0" without touching the indivisible item |
| 7b | Size-first scene drain (docs/278 §5 A) | Visual only (walls before cups); keep as a pop-in fix, not a frame-time one |
| 7c | Finalize slice 5 -> 2 ms while moving (docs/278 §5 B), a data write | Subsumed by 7a; cheaper first step |

**Desk pre-test (no game run):** from an existing `ApexRadiance_FrameTimeline.csv` of a camera sweep, count moving frames
>= 25 ms where finalize (`007297C0`) + scene drain + lot build > 2 ms ran beside an item > 15 ms, and sum that time: the
ceiling of 7a. Build only if it is >= 2 ms on >= 20% of moving hitch frames. **Cost:** pre-test ~30 min; 7a ~2 h in Apex
(it owns the three budgets). **Test:** docs/278 §6 queue (smoke + ABBA BP and IP sweeps); pass = count of moving frames
>= 25 ms down outside noise, backlog drained within 1.5 s after the camera stops. **Apex PR: yes** (their budgets).

## 8. Ranking by expected gain per effort

| Rank | Item | Expected gain | Effort | Gate | Apex PR |
|---|---|---|---|---|---|
| 1 | 2a/2b pick skip when still + motion cap done right | up to 6-8% of render-thread time when still; ~1 ms/frame in pans | ~2-3 h + 1 h RE | Step 0 (g), click/hover tests | Yes |
| 2 | Step 0 bridge read-back microbenchmark | decides 1, 2c, 6 | ~1 h + 1 run | none | Dev tool, yes |
| 3 | 6a sub-step profile, then 6b memo / 6c lazy restore | ~1-2 ms/frame zoomed out | 1 h + 5-7 h | byte-equal screenshots | Yes |
| 4 | 7a deadline slicing | 0-6 ms on moving hitch frames | 30 min desk + 2 h | desk pre-test | Yes |
| 5 | 1a/1b compositor atlas batch | 1-5 ms on CAS-heavy frames | 4-5 h | Step 0 (d); texture hash equal | Yes |
| 6 | 3a AO slices by zoom (one count per frame) | 0.5-1 ms GPU, ~0 on CPU-bound | ~1 h | lab metrics | Yes |
| 7 | 4a weighted normals | visual | ~1 h | lab seam metric | Maybe |
| 8 | 3b nested per-tile slices | GPU | 2-3 h | lab tile-edge metric | Only with proof |
| 9 | 5a/5b half-res / temporal AO | GPU, artifacts risk | 4-6 h | lab shift metric | No |
| - | 1d placeholder textures, 4b MRT normals, 6 constants shadow | rejected: wrong Sim textures / days of work / ~0 under the bridge | | | |

## 9. Open questions

- Q1: does the click (select, place) read the async pick result or do a synchronous pick of its own? Decides 2a's guards.
- Q2: is the texture compositor queue order meaningful (layers of one Sim), and what does the queue loop do with return
  codes other than 2/3? Decides 1b.
- Q3: Step 0 results (round trip vs bytes, second read-back cost, `Get*` cost, query waits).
- Q4: whether the GI pass (this folder) will cover the ground-light fixes (6d).
