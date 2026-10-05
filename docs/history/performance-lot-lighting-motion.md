# Lot Lighting While Moving: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/lot-lighting-motion.md](../features/performance/lot-lighting-motion.md).

### 2026-09-29: budget scaling while moving (plan candidate C7)

**Context:** "Lot room solve" dominated 50 to 77% of the 16 to 50 ms moving hitches at about 15 to 17 ms, the priority
lot's 15 ms budget. The earlier Smooth Streaming "current lot while moving" cap was never tested with non-default values
([removed features](../removed-features.md)).

**Finding:** the budget function has a single caller (0x00ADB95D) and the room solve is resumable, so a smaller budget
spreads the work without skipping any. The Frame Profiler's "Lot room solve" calls are lot levels, not rooms (its label
was corrected the same day).

**Outcome:** shipped on by default at 3 ms.

### 2026-09-29: per-frame camera sample (round 3, section 2.3)

**Context:** the eye was sampled only inside the hook, so after a quiet spell a stale eye could read as "moving".

**Outcome:** the eye is also sampled once per frame from a Present callback, registered while this feature or Wall
Shading While Moving is on.

### 2026-09-30: lot lighting budget with the camera still (rejected)

**Context:** commit `25b3ca4`: the steady budgets (15 / 5 ms) scaled to 8 ms with the camera still too; the lamp boost,
lots in their first 10 s after loading and the tool mode were left alone.

**Finding:** in game (7 minutes, much of it in Build mode) the 14 to 16 ms "Lot room solve" hitches fell from 12% to 1%
of them, but the 8 to 12 ms ones stayed (the solve overshoots its budget by one step). Lamps moved in Build mode updated
their light visibly more slowly: moving a lamp is not a switch, so no boost applies, and the budget function's `+0x4D`
flag is the lot thumbnail's forced quality, not a Build mode flag.

**Outcome:** reverted the same day (`ba41dd4`). Small gain, visible cost. Do not bring it back without a real
Build-mode or lamp-moved signal.
