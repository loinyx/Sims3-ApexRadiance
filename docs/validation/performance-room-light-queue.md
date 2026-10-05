# Faster Room Lighting: validation

The feature is described in [features/performance/room-light-queue.md](../features/performance/room-light-queue.md).
It must:

- Raise the priority only of rooms of the priority lot, with native priority above zero (plus stranded rooms to 1).
- Apply x4000 on every floor of the priority lot while the full-detail-all-floors policy is on, and the camera-floor
  x4000 / below x2000 / above x1 rule otherwise.
- Keep the extra solve drain at 4 ms with the camera still and 1 ms while it moves, on the render thread only.
- Restore every patched byte on Stop.

## Automated tests

None. `RoomAmbientPolicy::FloorPriorityFactor` is a pure function in
[`features/room_ambient_policy.h`](../../features/room_ambient_policy.h), but no harness in `tools/` asserts it
(`tools/room_ambient_test` includes the header for other policies). The harness behind the 2026-10-01 checks below is
not identified.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-01 | Test005 build | Floor priority policy checks | All-floor x4000 with the policy on, camera-floor / below rule with it off; other lots and zero priority unchanged; drain limits and lot lighting budgets unchanged. No FPS improvement inferred | None |

## In-game test plan

1. Enter a lot with several stories at night. **Expected:** the viewed story's rooms light first, then the others.
2. Change floors. **Expected:** rooms of the new floor reach their final look without an intermediate step.
3. Switch a lamp. **Expected:** its room relights within a few frames.
4. Pan while a lot loads. **Expected:** no extra hitches from the drain (1 ms limit while moving).

## Confirmed in game

- Nothing recorded beyond its release.

## Open checks

- Its status line has no menu location in the current menu (no Developer card calls `RenderDeveloperUI`).
