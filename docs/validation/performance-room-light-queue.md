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
| 2026-10-27 | `0b99a6a`, `94c616e` | In game, Steam 1.67.2, development and standard builds | `lamp_edit_lot_only`: a lamp switched off and on near a stair opening updated the story below; lamp edits on the release build settled in 234 to 3187 ms; Smoke and Lighting suites passed | DXVK |
| 2026-10-13 | Release preparation tree for 2.12.3; final SHA in the release PR | Developer and public Release x86 builds: 0 errors, 0 warnings; `tools/room_ambient_test`; `run_lamp_state_checks.ps1` | Builds passed; room ambient policy 102 checks, 0 failures (hold decision, urgency, safety-net gap, 200 ms move delay); lamp edit state 12 assertions, 0 failures. The map hold itself has no offline harness | None (mocks) / x86 build |
| 2026-10-01 | Test005 build | Floor priority policy checks | All-floor x4000 with the policy on, camera-floor / below rule with it off; other lots and zero priority unchanged; drain limits and lot lighting budgets unchanged. No FPS improvement inferred | None |

## In-game test plan

1. Enter a lot with several stories at night. **Expected:** the viewed story's rooms light first, then the others.
2. Change floors. **Expected:** rooms of the new floor reach their final look without an intermediate step.
3. Switch a lamp. **Expected:** its room relights within a few frames.
4. Pan while a lot loads. **Expected:** no extra hitches from the drain (1 ms limit while moving).

## Confirmed in game

- Lamp switches all at once (2.7.0): tested in game by the author on Steam 1.67.2 before publication.

## Open checks

- Lot entry and floor change on a large lot (36 rooms, 6 stories). **Expected:** every room of the camera's story shows
  its final light well within 10 s of `[LevelLightShare] Lot <tracker> entered`, fewer `ended: urgency expired` lines and
  AtriumHold time-outs than before, and the status line's "edit priorities of other lots kept below the played lot's"
  above 0 when a neighbour's lamps change.

- 2.14.0: an automatic switch-on at dusk puts the lamp's own room first, without a pause; the step budget removes
  15 to 21 ms frames during the drain after a load (frame times not yet measured).
- Room 0 of the ground story of a lot with many lights is restarted repeatedly by the game (caller `0x006C7451`); how
  often its solve finishes is not counted yet.
- 2.12.3 public build: the *Lamp edits all at once* scenarios below remain to be run in game.
- Its status line has no menu location in the current menu (no Developer card calls `RenderDeveloperUI`).
- Lamp switches all at once on other game builds (the solve end hook is Steam 1.67.2 only; elsewhere switches change
  room by room).
- Lamp edits all at once (12/10, not yet tested in game): a switch on story 2 with the camera on story 1, an intensity
  change, a 3 m drag, a lamp moved to another story, a lamp added and deleted, an edit on a neighbouring lot while
  panning, and "all the lights" of a big lot. **Expected:** the lamp's own story within about 100 ms; the other stories
  in one release within 1.5 s (switch, value) or 2 s after a drag is let go; `[AtriumHold] Lamp edit:` lines end in
  `complete` (a timeout only on the big lot); no frame over 50 ms added (Frame Profiler).
- `room_ambient_test` covers the hold's decision (`EditHoldOutcome`), the urgency rule (`UrgencyKept`), the safety-net
  gap (`LampLotGap`) and the 200 ms move delay; the map hold itself has no offline harness.
- Quick update for lamp switches (with *Lamp switches all at once* off): rooms show a quick light within a few frames
  and refine without a frame-rate drop.
