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

### 2026-10-05: lamp edits in Build mode

**Context:** commits `3604e77` and `9de1c74`. Lamps edited in Build mode corrected their rooms slowly; a later recording
of a dragged sconce in an atrium house showed 5.9 s of solving in 6.5 s (about 9 frames a second).

**Outcome:** while a lamp edit's rooms wait for or are in their solve, the lot being played gets 25 ms of solving a frame
with the camera still once the lamp is let go; while it is dragged the game's 15 ms stays.

### 2026-10-05: lot build slice while moving

**Context:** commit `36c66e5`. A streaming lot is built a slice per frame by 0x00AEA680 with 20 ms (35 for a priority
lot), which made hitches while the camera moved over lots streaming in.

**Outcome:** the load at 0x00AEA6D8 becomes a call that lowers the slice to 6 ms while the camera moves; the loading
screen's 2000 ms is still written after it.

### 2026-10-06: after-load settle and the refinement cap

**Context:** commits `a93bbd0` and `e286d0f`. Entering a lot took long to correct its lighting (4.4 ms of solving a
frame for 8 s); and many lamps refined after their quick pass took 35 to 44 ms of solving a frame for 2 to 3 s.

**Outcome:** for 15 s after the world goes live the lot being played keeps the game's budget while moving and gets 25 ms
still; while a quick pass is refined the budget is capped at 6 ms. All four changes released in 2.7.0.

### 2026-10-22: paced raises while moving

**Context:** commit `715a410`. In a frame-time recording, 61 of 197 hitches while the camera moved came from the two
raised budgets that kept the game's full budget while moving: a lot settling after its load and a lamp switch's boost.

**Finding:** the frame-time pacing the settle window already used with the camera still (`PaceSettle`) can dose both:
the game's budget while frames hold, down to the moving budget when they do not. Only the pace of relighting changes,
not the light.

**Outcome:** both raises are paced while moving, behind the Tester switch *Paced relight while moving*
(`perf_motion_paced_raise`, on) so the two paths can be compared in game. Gameplay measurement is open
([validation](../validation/performance-lot-lighting-motion.md)).
