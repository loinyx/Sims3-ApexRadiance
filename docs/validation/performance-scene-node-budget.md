# Spread New Objects Over Frames: validation

The feature is described in [features/performance/scene-node-budget.md](../features/performance/scene-node-budget.md).
It must:

- Process the pending nodes in exactly the game's order, leaving the rest at the tail of the game's list.
- Drain everything when the camera is still or a node waited the longest wait.
- Never let a recorded node be freed or re-added while linked; never write a dead list or a destroyed node.
- Leave every lifetime counter that marks an unexpected case at 0.

## Automated tests

None offline.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| | | | No recorded in-game run | |

## In-game test plan

Developer mode, Developer > Performance > *Objects spread across frames*:

1. **Expected log:** three `[EntryChain] scene node destructor (0x006fd930)` / `scene AddNode (0x006e6480)` / `scene
   holder teardown (0x006e4de0): layer 4 installed` lines, then `[CallChain] Scene::BeginFrame pending-node drain (CALL
   0x006ebc49 -> 0x006e4130): layer 1 installed` and `[SceneBudget] On: ... node lifetime hooks on the destructor
   0x006fd930, AddNode 0x006e6480, holder teardown 0x006e4de0; ... at most 512 nodes / 2.0 ms per frame, longest wait
   500 ms; development checks on (destroyed-node vtable 0x00fa1b78)`. With the Frame Profiler on (either order), its
   Hooks table says "outer layer of the call chain ...; the scene node budget is inside".
2. Camera still. **Expected:** "drains ... game's (camera still)" grows, "with a budget" does not.
3. Pan quickly over a neighbourhood while lots stream in, travel, load a save and pan at once. **Expected:** "with a
   budget" grows, "frames left nodes" > 0 when a lot streams in, "largest backlog" in the hundreds or thousands,
   "game's (a node waited too long)" rare. Objects may appear a frame late (subtle). Objects flickering or missing after
   the camera stops, or any crash, are failures: turn the feature off and send the log.
4. Frame Profiler, same camera test off and on. **Expected:** "Scene pending nodes" per-hitch ms drops in the 25 to 50 ms
   moving hitches; hitch lines show ", deferred N".
5. Build / Buy: place, move and delete objects while panning; Edit Town; CAS and back. **Expected:** everything placed,
   nothing left invisible. Try the Developer sliders (for example 64 nodes, 0.5 ms) to make deferral obvious, then go
   back to the defaults.
6. Node lifetime: with the sliders low (64 nodes, 0.5 ms, longest wait 2000 ms) pan while lots stream in and, while
   moving, delete objects, let Sims leave, travel, go to CAS, load another save. **Expected:** "nodes recorded" goes up
   and down; "unlinked at destruction", "before AddNode" and "repaired" stay 0 (any other value: send the log, it has a
   `[SceneBudget]` warning naming the node); "dropped at teardown" > 0 after a world change is normal; "left nodes not
   owned by their holder" stays 0. The status never reads "Stopped: a safety check failed" (else the log has
   `[SceneBudget] Safety check failed: ...`). Turning it off logs an Off line with the same counters.

## Confirmed in game

- Nothing recorded beyond the default-on release in 2.5.5.

## Open checks

- The per-frame limits (512 nodes / 2 ms / 500 ms) were chosen without data; pick them from the profiler's "Scene
  pending nodes, nodes N, deferred D" in the moving hitches.
- Which node classes are expensive in vfunc +0x48.
- Nothing was found that expects the pending list to be empty right after BeginFrame (inferred). A crash or a missing
  object that appears only with the feature on would contradict it.
- The destructor hook may run on any thread; the "destroyed on another thread" counter should not grow together with a
  crash.
