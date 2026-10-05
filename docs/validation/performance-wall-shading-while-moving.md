# Wall Shading While Moving: validation

The feature is described in
[features/performance/wall-shading-while-moving.md](../features/performance/wall-shading-while-moving.md). It must:

- Defer only driver calls with `0 < budget < 100 ms` in state 0 or 1, while the camera moves.
- Bound each deferral to one wait per state and camera motion (2 s by default), then allow one pass per frame.
- Never leave a lot unloaded or unshaded once the camera has been still for a second.

## Automated tests

None offline.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| | | | No recorded measurement | |

## In-game test plan

1. **Expected log:** `[SlotChain] Wall AO solver step: layer 0 installed ...` and `[WallShading] On: wall shading step
   0x0068b810 (slot 0x00ff05b0) gated for calls from the solver driver 0x00688920 (return 0x0068894d) ...`.
2. Developer card while panning over a neighbourhood that loads. **Expected:** "held while moving" grows, "passed
   through" stays small (only the synchronous solve at LOD switches), "first passes run after the wait" only on long
   pans.
3. Frame Profiler, camera test. **Expected:** no moving hitch with "Lot room solve" over about 3.5 ms; the "Wall AO pass"
   counter shows passes only in still frames (or after 2 s), at most one per frame; the 54 to 108 ms lot-load hitches
   become one pass per frame after the camera stops.
4. Visual: pan quickly to a new part of town and stop. **Expected:** the new lots leave their impostor at most about 2 s
   later than without the gate; outdoor walls may look flat for a moment and get their shading within a few frames of
   stopping. Nothing stays unshaded or unloaded after the camera has been still for a second.
5. Build mode, Edit Town (tool mode) and lot switches. **Expected:** unchanged (those paths are passed through).

## Confirmed in game

- Nothing recorded beyond its release.

## Open checks

- If the 2 s first-pass wait looks bad, make the pass resumable per wall or lower the wait.
