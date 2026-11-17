# Frame Deadline While Moving

While the camera moves, work that can wait (new objects, lots being built, loaded files being set up) only gets the
time left before a 12 ms point in the frame. A frame that is already slow because of one long item no longer gets that
work on top; the work runs in the next frames.

## Status

| | |
|---|---|
| Availability | In development (experimental) |
| Default | Off |
| Menu | System > Performance > Camera and lighting > *Frame deadline while moving* |
| Configuration | `[patches.DeadlineSlicing]` in `ApexRadiance.toml` |
| Needs | *Spread lot lighting while moving* (its camera motion detector) |
| Source | [`features/deadline_slicing.{h,cpp}`](../../../features/deadline_slicing.cpp), [`features/scene_budget.cpp`](../../../features/scene_budget.cpp), [`features/lot_build_share.cpp`](../../../features/lot_build_share.cpp), [`features/lot_build_share_policy.h`](../../../features/lot_build_share_policy.h) |

## The problem

Moving hitches are led by single items that a budget cannot split: a DXT encode, a large lot's first load stage, an
impostor build, a Sim build step, a terrain update (see [../../history/performance.md](../../history/performance.md)
and the Frame Profiler's combined-build report). The deferrable work of the same frame still runs beside them: the
main-thread finalize jobs of the resource loader (`0x007297C0`, up to the JobManager's 5 ms slice plus one job), the scene
pending-node drain (*Spread new objects over frames*, 2 ms) and the lot load stages (*Shared lot build time*, 4 ms).
That turns a 30 ms frame into a 35 ms one. The removed Service Frame Budget lowered fixed slices for every frame and
showed no perceptible gain ([../../removed-features.md](../../removed-features.md#service-frame-budget)); this feature
changes only the frames that already ran long, and only while the camera moves.

## How Apex Radiance solves it

1. **Frame start.** The JobManager service's update (vtable `0x00FD9810` + `0x1C` -> `0x00599A10`, Steam 1.67.2;
   bytes checked at start) is redirected to a wrapper. It is the first time-sliced service of the main loop
   ([../../engine/main-loop-and-services.md](../../engine/main-loop-and-services.md)), so its entry marks the frame's
   start (QPC). The camera state (*Spread lot lighting while moving*'s detector, held 300 ms) is read once there; loading
   screens (WorldManager `+0x1B4` = 0) count as still.
2. **Finalize slice while moving.** The finalize jobs run before the long items of the frame, so no deadline can see
   them coming. While moving, the wrapper writes 2 ms into the slice `[svc+0xCC]` (read at `0x00599A20`, game default 5)
   and writes the game's value back after the call. One job always runs, as in the game.
3. **Deadline for the later work.** *Spread new objects over frames* takes `min(its budget, 12 ms - time since the
   frame start)`; at or past the deadline it processes only its 8 minimum nodes and the nodes whose draw needs their
   update. The shared lot build time gives a lot `min(what is left of its share, time to the deadline)`; with less than
   1 ms left the lot waits, as it does when the share is spent.
4. **Longest waits unchanged.** A scene node that waited 500 ms grows the budget as before (the deadline is not applied
   then) and the game's full drain runs at 1.5 s; a lot that waited 6 frames is still forced; the lot being played keeps
   the game's slice. When the camera stops, every feature returns to its own budget.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Frame deadline while moving | `[patches.DeadlineSlicing] enabled` | bool | off | | Turns the deadline and the shorter finalize slice on |
| Developer > *Frame deadline* | (not saved) | float | 12 ms | 4 to 33 ms | Time after the frame start past which deferrable work waits |
| Developer > *Finalize slice while moving* | (not saved) | int | 2 ms | 0 to 5 ms | Never above the game's own slice |

## Measurement

- **Counters** (developer panel, a `[DeadlineSlicing]` log line every 10 s while it is on and one when it is turned
  off): frames and moving frames seen, finalize slices lowered, scene drains shortened / past the deadline, lot build
  calls shortened / held by the deadline.
- **Desk pre-test:** `python tools/deadline_slicing_pretest.py ApexRadiance_FrameTimeline.csv` reads a camera sweep's
  timeline and counts moving frames >= 25 ms where an item > 15 ms ran beside more than 2 ms of deferrable work (main-thread
  jobs, scene pending nodes, lot load stages), and sums that time: the ceiling of the gain. Worth an in-game A/B when that
  is >= 2 ms on >= 20 % of moving hitch frames. Not yet run: no timeline of the current build was recorded.
- **In game:** Frame Profiler timing preset on a camera sweep (pan at street zoom plus far jumps), feature off / on / off
  / on in one load. Pass: count of moving frames >= 25 ms down outside noise, the scene and lot backlogs drained within
  1.5 s after the camera stops, no new pop-in.

## Compatibility and interactions

- **Frame Profiler:** its service loop walk reads the vtable each frame, so the JobManager row shows under the wrapper's
  address while the feature is on.
- **Another mod** that writes the same vtable slot or patches `0x00599A10` makes the start check fail; the feature then
  stays off with a message.
- **Scene node budget off:** no scene deadline. **Shared lot build time off:** no lot deadline. The finalize slice still
  applies.

## Rejected approaches

- **Skipping the JobManager update on a late frame:** it runs first, so the frame is never late when it runs, and a
  skipped update would also hold other main-thread jobs a waiting thread may need.
