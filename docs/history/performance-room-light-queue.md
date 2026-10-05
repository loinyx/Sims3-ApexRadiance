# Faster Room Lighting: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/room-light-queue.md](../features/performance/room-light-queue.md).

### 2026-09-29: room lighting queue study

**Context:** five parallel studies (report in the session scratchpad `plan.md`) of why rooms light up slowly.

**Finding:** from entering a lot to the last room solve took 12 to 36 s, with only 1.2 to 2.8 s of solve work in it; in
95 to 98% of the frames with solve work a single lot story was being solved. The cause is the game's queue, not the
solve itself.

**Outcome:** the five changes of the feature page; shipped on by default.

### 2026-09-29: adversarial review

**Context:** an independent review of the queue and of Apex's own requeues.

**Finding and outcome:** the drain runs only when the room current at the previous pick is done and the new one is of
that same lot and the priority lot. `QueueRoom` holds back only requeues after a setting or ambient change, never a
requeue caused by a lamp list change, and only while the room update can send it later; a whole-world relight asked
while the story share is off runs at once. In `level_light_share.cpp` the same day: no invalidation of the room being
solved, burst relights coalesced to one 250 ms after the last ask, the settle requeue armed once per lot state, early
return for a light the game is about to drop, and no relight of every lot 3 s after a world loads at night.

### 2026-10-01: all-floor priority (Test005)

**Context:** level light share's installed full-detail-all-floors policy.

**Finding:** while that policy is active, `PriorityHook` applies the x4000 boost to all floors of the priority lot,
including those above the camera. With the policy off, the camera-floor x4000 / below x2000 rule stays. Other lots and
zero native priority are unchanged; native priority differences remain within the boosted floors. The extra drain
stays at 4 ms still / 1 ms moving and the lot lighting budgets are unchanged. Ambient publication is coordinated
separately. No FPS improvement was inferred from these policy checks.

**Outcome:** kept; released in 2.5.3.
