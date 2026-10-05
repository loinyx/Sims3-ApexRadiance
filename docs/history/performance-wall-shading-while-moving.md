# Wall Shading While Moving: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/wall-shading-while-moving.md](../features/performance/wall-shading-while-moving.md).

### 2026-09-29: wall AO gate (round 3, section 2)

**Context:** with Lot Lighting While Moving on, every moving "Lot room solve" hitch of 5 ms or more was one wall AO
pass: 10 to 17 ms each, 54 to 108 ms when lots load (the AO ray loop 0x0068AF31 in 85% of the samples of the 108 ms
case). The pass never looks at the budget.

**Finding:** returning the current state is the engine's own "try again next frame"; lot load stage 20 and the
thumbnail capture wait for the passes, so any deferral needs a bound.

**Outcome:** shipped on by default with a 2 s wait per state and camera motion and one pass per frame. Rejected:
capping WallMillisecondsBudget while moving (round 3 option L2: it only shortens the refinement and would change the
detail chosen for good) and a resumable per-wall pass (L3: a re-implementation, not needed unless the deferral looks
bad).

### 2026-09-29: wait not restarted by passes

**Context:** the first version restarted the wait after a pass that ran out its wait.

**Finding:** that made every other lot start a fresh wait.

**Outcome:** one wait per state and camera motion; everything resets only when the camera is seen still.
