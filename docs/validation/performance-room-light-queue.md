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
| 2026-11-12 | Uncommitted change on release/3.1.2 | `tools/room_ambient_test` | Room ambient policy 170 checks, 0 failures (now also `GroundFirstHolds`: other rooms wait while the ground room 0 is in state 1 or 2, never the ground itself, until the deadline, across tick wrap); production room updater 1346 checks, 0 failures. Ground first not yet run in game | None (mocks) |
| 2026-11-10 | Uncommitted change on `810d882b` (release/3.1.1) | Developer Release x86 build; `tools/room_ambient_test` | Build passed; room ambient policy 146 checks, 0 failures (now also `EditRoomStillWaiting`: a room blank at its first mark stops waiting when its first solve ends, even one begun before a later mark; lit rooms unchanged); the unlit-rooms recovery executable was blocked by security software and did not run; production room updater 1345 checks, 0 failures. Never-solved rooms not held: not yet run in game | None (mocks) / x86 build |
| 2026-10-27 | `e39db28`, `b6959de` | In game, Steam 1.67.2, development and standard builds | `lamp_edit_lot_only`: a lamp switched off and on near a stair opening updated the story below; lamp edits on the release build settled in 234 to 3187 ms; Smoke and Lighting suites passed | DXVK |
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

- Never-solved rooms not held (black story on lot entry, capture 2026-11-10 09-18-28, lot `6C11001D27D24720`): enter the
  lot, leave and re-enter it. **Expected:** the `[AtriumHold]` line shows "never-lit maps not held" above 0, far fewer
  "later map writes kept back" than 234 / 88 and "maps keep their light" near 0; story 1 lights within a frame or two of
  its class-0 solves, not at "settled ... (ended: urgency expired)"; its furniture is released with its floor. Then, on
  the lit lot, switch a lamp on story 2 with story 1 in view: the other stories still change together ("N maps held"
  above 0). The held map's real texels were never dumped; whether the game ever sets `+0x100` back to 4 on an existing
  room is unverified.

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
- Lot entry is no lamp edit (3.1.1, not yet tested in game): enter an atrium lot with the camera held still. **Expected:**
  no `[AtriumHold] Lamp edit` line, no furniture *held for a lamp switch* and no RtOutdoorLamps *a lamp edit* line near
  the entry; the counters *rebuilt lamps not taken as moves* (LampMarkFilter) or *first sights not taken as edits*
  (LevelLightShare) rise; the final frame and wall seams match a capture before the change within 1/255. A lamp switch,
  a drag and a lamp moved to another story on that lot still start the hold.
- `room_ambient_test` covers the hold's decision (`EditHoldOutcome`), the urgency rule (`UrgencyKept`), the ground-first hold (`GroundFirstHolds`), an edit room's wait (`EditRoomStillWaiting`), the safety-net
  gap (`LampLotGap`) and the 200 ms move delay; the map hold itself has no offline harness.
- Quick update for lamp switches (with *Lamp switches all at once* off): rooms show a quick light within a few frames
  and refine without a frame-rate drop.
- Lot entry priorities (3.1.1, not measured in game): enter a lot and pan to the next lot and back with a recording on.
  **Expected:** a neighbour's edit room no longer goes before the played lot's waiting rooms (before: quick pass for up
  to 49.6 s); the entry budget lasts while the played lot's rooms on or below the camera's story wait (up to 30 s;
  before, the lot took 18.8 s to look right after a pan back); no false lamp edit on the way back.
- Ground first after an active-lot switch (not yet tested in game): on the *Claude teste* save, pan from a neighbour
  into the played lot at least 5 times with a recording on. **Expected:** the first `[solve]` after
  `Active lot tracker X` is room 0 of story 0 of that lot; `story 0 room 0 solved N ms after the switch` with N under
  about 600 (before, the camera story ran first: story 3 at 3328, 594, 969 and 5406 ms); `Story 0 class 2 floor map` /
  `sharper copy` within 1 s of the switch (before: 3.503 s and 3.882 s); frame median and p95 in the 5 s after the switch
  no worse than 9.4 / 15.6 ms. Entries where story 0 has window-light marks (`M ... let through of lamp`) are expected
  to stay slow (a separate follow-up).
