# Room lamp probe

A developer measurement taken before indoor lamps are drawn on the GPU: each solve of a room of the active lot other than
room 0 is captured as [Quick outdoor lamp edits](outdoor-lamp-delta.md) captures room 0, and each lot lamp's part of every
texel is compared with the law [Real-time outdoor lamps](rt-outdoor-lamps.md) draws that lamp with. Nothing is drawn, kept
or written.

## Status

| | |
|---|---|
| Availability | Developer build only, not released |
| Default | Off |
| Menu | Developer > Compare lighting paths > Compare indoor lamps with GPU (with Lamp lighting: Instant) |
| Configuration | `roomLampProbe` under `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/outdoor_delta.cpp`](../../../features/outdoor_delta.cpp) (`ProbeStart`, `ProbeEnd`), [`features/outdoor_delta_model.h`](../../../features/outdoor_delta_model.h) (`ProbeClassify`), [`features/rt_outdoor_lamps.cpp`](../../../features/rt_outdoor_lamps.cpp) (`WorldLaw`) |

## The problem

Indoor lamp edits wait for the CPU room solves. An earlier developer build drew every lamp of the active lot into every
room's maps on the GPU; in its first game test, room 0 matched the game's solve (98 to 100 % of texels within 2/255) and
roofed rooms did not (21 to 85 %), with the cause unknown. Its offline harness drew the game's side with the GPU's own law and
no occluder, so it could not show the gap. Drawing indoor lamps on the GPU must first be shown to give the light the solves
give.

## How Apex Radiance solves it

1. Turned on, every room of the active lot is sent to its solve once.
2. A solve of a room other than room 0 of the active lot, on the light tree thread, is captured with the room solve hooks of
   Quick outdoor lamp edits: each texel's sum before the roofed rooms' tone and each light's part of it (the running sum
   before and after each light).
3. At the solve's end, for each lamp of the room's list that the GPU path draws (lamp classes 3, 4, 5 and 11 of the lot,
   alive, a shade its law describes), the law is evaluated at every texel with no shadow, times the list's count, with the
   soft light cut and realistic reach as Real-time outdoor lamps applies them (reach only for rooms without a roof and
   lamps registered outdoors). Each texel the solve or the law lights falls in one class: within 2/255, dark in the
   solve (a test the law has no shadow for), lit only by the solve, or another strength (the median ratio solve / law is
   logged).
4. The same texels are tested against the occluders the GPU path would use (`LevelLightShare::ExportLotProxy`, built with
   the ceilings left open over the rooms, as the earlier GPU room lamps build had them): one ray from the lamp's centre to
   the texel, 2 cm off its surface. The log gives the share within 2/255 with that hard shadow, the texels dark in both,
   dark in the solve only (the GPU would light them) and shaded by the occluders only (the GPU would darken them). At most
   about 200,000 rays a solve: on a larger one every n-th texel is tested.
5. The capture is dropped.
6. A room 0 solve of the active lot (with Lamp lighting: Instant) records the lot lamps of its list Real-time outdoor lamps
   does not draw (indoor lamps carried outdoors) as steps 2 and 3 do, with their floor mask parts and the exit ground
   texels, and is dumped with its whole light list (once a session per room 0 and class). Nothing is kept for it unless
   Keep indoor lamps for GPU is on too.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Compare indoor lamps with GPU | `roomLampProbe` | bool | off | | Developer only, with Lamp lighting: Instant. Compares each room solve of the active lot with the GPU's lamp law and logs the result |

## Limitations

- The occluder test is a hard shadow from the lamp's centre; the GPU path takes the share of the lamp's 0.15 m ball, so
  texels at a shadow's rim can differ. The occluders are read again only when the lot's geometry key changes.
- Only solves on the light tree thread with Lamp lighting: Instant (the hooks of Quick outdoor lamp edits run only then);
  quick passes and rooms with a light whose evaluation is not wrapped are not probed (logged).
- Each probe costs one model evaluation per texel and lamp, and the rays, on the light tree thread (the log gives its time).
- At most 2000 log lines a session.
- Room 0: CrossFloorShadow has no per-evaluation record (its lines are counted apart, not replayed); the floor mask replay
  tests walls with room 0's whole list where the copy used Apex's culled list (UNVERIFIED that the cull never changes the
  answer). Neither room 0's records nor the fills have been seen in a game dump yet (UNVERIFIED).

## Technical reference

- **Logs** (`[RoomLampProbe]`): `Story S room R class C (roofed|roofless): N texels, K lot lamps of L lights listed;
  their light within 2/255 of the GPU law: P% unshadowed (M texels lit by either), Q% with the occluders (K texels tested);
  light no part holds at U texels; T ms`,
  then per lamp `lamp A type T xM (registered in room R): N texels lit, a% within 2/255, b% dark in the solve, c% lit only
  by the solve, d% other strength (median solve/law r); darkest miss: ...; largest other: ...; with the occluders (K texels
  tested): e% within 2/255, f% dark in both, g% dark in the solve only, h% shaded by the occluders only; the law alone (the colour before the game's wall and object tests): i% of N texels within
  2/255 (j% other strength, median k); the game's wall test at N texels: o% open, p% blocked, q% partly; its 3D
  (object) test at N texels past the walls: ...` (the wall test's answer comes from `GameWallTest`; the 3D share is what is left) with the texel's
  position, normal and distance to the lamp. `not probed (...)` and `not compared (...)` give why a solve was left out.
- **Dump**: one file a session, `RoomLampProbe-<time>.txt` in the Apex Radiance folder (at most 64 MB): per probed room its
  3D blockers as the game's 3D test `0x0069D910` reads them (room+0x40: 64-byte records with a centre and a squared radius,
  their blocker pointers at +0x30; each blocker's vtable, vfunc+0x14 and first 0x100 bytes; once a file, the 19 globals the 3D test and the blockers' functions read) and up to 1,800 sample texels
  (position, normal, lamp, colour before the tests, after the wall test, final), for an offline replay of the 3D test.
- **Dump, the 2D wall test**: per room the mode byte +0x639, its walls (room+0x30..+0x34, 0x100 bytes each) and the default 2D
  and 3D index vectors (+0x78, +0x88); per sample texel the game's own wall test inside Apex's wrapper (the ray's start and
  its answer before Apex's overrides) and the texel's batch (size, the pass's two switches, centroid).
- **Apex's paths**: each lamp row ends with the path Apex's hook took for it (own, InnerShadow, opening, CrossFloorShadow,
  IndoorShadow), how the law matches the colour before that path, and that path's factor (unchanged, zero, other with its
  median); the dump's sample texels carry the path bits and that colour (`path P pre r g b`).
- **Dump, Apex's steps per evaluation**: each sample texel line ends with `c0 r g b evals N` (the game's evaluation summed over
  the lamp's entries) and, for a lamp of the room's record (`set` 1 inner, 2 same, 3 cross) or one the floor cut stopped, the
  last evaluation's record: `ev set S e0 r g b mount k day k reach k cut 0|1 early 0|1`, then per step taken `p6 pass kNorm
  srcNorm roomNorm` (InnerShadow), `p7 pass tOpen times law gain total cut k q thrScale` (the opening), `p8 mine slabShare
  transmission why W stage G nseg N` with `seg story room t0 t1 keep passed` per ray segment (passed 255: not tested) and
  `ent x y z` (where the game's wall test then starts) (IndoorShadow), and `fade f thr` (the soft light cut). -1: not reached.
  `Z` lines (same fields; at most 800 a room, 200 a lamp) are texels the game lit and Apex left dark. At most 65,536 records
  and 200,000 such dark evaluations are kept a solve (`PROBE evals E dropped D zero Z`).
- **Dump, the room's state**: after each `ROOM` line `APEX` (threshold, the lowered one, the soft light cut, the wall mode
  byte, the daytime factor, +0x160 at the solve's start and end, the half texel, the shadow softness, the opening sliders,
  realistic reach and the development switches the tests read), `INFO` and the record's sets (`XC` cross lamps with their
  story, room and floor object, `XS` same, `XL` lower, `XI` inner, `XU` innerUp, `XN` innerSrc with the source room's
  +0x160); the story grids the tests walk (`GRIDHASH H role R serial S story K`, then `GRID`, `GZ`/`GX` wall edges per row,
  `GT` per tile diagonals, flags and ceiling, `GE` edges, `GO` openings; floats as their bits) and per story the tiles' floor
  height and quadrant rooms (`FT`) and the floor object's quadrant keys (`FK`) under `FLOORHASH H story K`. A block already in
  the file is written as `GRIDREF` / `FLOORREF` with its hash.
- **Every light of a room**: `LIGHT <light> type T vt <vtable> xM colour r g b at x y z drawn 0|1` for each light of the probed room's
  list, and for a light the GPU does not draw up to about 600 `N <light> p .. n .. hook .. wall .. got ..` lines (its lit texels).
- **The opening's outdoor total**: a `p7` record ends with `tf I` when `OpeningOutdoorTotal` filled or reused a fill in the
  probed solve; the room's block holds `TF I serial S X x y z n x y z q a b c d lights N`, then per light of room 0's list
  that `OpenTotalFill` evaluated at X (lit there, window lights left out, at most 64) `l <light> pos x y z c r g b pass p
  day d slot s` (its place as `YardPass` read it, the game's evaluation at X, `YardPass`, the daytime factor applied, its
  slot among the record's `same` lamps, -1 none). At most 20,000 fills a solve.
- **Other rooms' walls**: after the room's own `RECORDS`/`WALLS`/`IDX` lines, for each other room a `p8` ray segment was
  tested in (the game's 2D test of that room), `OTHER story S id R room <ptr>` and that room's lines with an `O` in front
  (`ORECORDS`, `OR`, `OB`, `OWALLS`, `OW`, `OIDX2D`, `OIDX3D`; same fields).
- **The GPU law**: a `LIGHT` line of a light with a GPU law ends with `law` and `GpuLampLaw::Lamp`'s 20 floats as bits.
- **Room 0** (`ROOM ... id 0`, then the same blocks): `T`/`Z` lines of the lamps it recorded end with `r0 bits B vis v all a
  tvis V tall A exit e` (the texel's bits: 1 masked, 2 gated, 8 exit ground; the lamp's floor mask parts and the texel's
  totals as luminances; e = an exit lamp); `M <light> p .. n .. L .. law 0|1 vis v all a tvis V tall A bits B batch ..`
  per masked texel where it has a floor mask part (at most 1000 a lamp, 4000 a solve); `E <texel> p .. n .. bits B packed
  P sum r g b exitParts` then `<light> r g b` per exit lamp lit there, per exit ground texel (at most 4000 a solve, evenly
  sampled).
- **Replay**: `tools/apex_paths_replay/run.ps1 -Dump <file>` recomputes these: the other rooms' wall tests with their dumped
  walls (the recorded answer where a room is not dumped), the outdoor total from its fill, each fill light's `YardPass` and
  GPU law at X, room 0's chain per path, its floor mask copies (the law with N.l ignored, the wall test with room 0's whole
  wall list) and its exit ground texels (not under a floor tile, `ExitAlpha` of the exit parts against the map's alpha).
- **Sample layout**: the batch sample's position at +0x00 and normal at +0x10, world space, as `ExportRoom0` reads them.
- **Test**: `tools/outdoor_delta_test` checks `ProbeClassify`'s five classes.

## See also

- [Quick outdoor lamp edits](outdoor-lamp-delta.md), [Real-time outdoor lamps](rt-outdoor-lamps.md)
- [engine/room-light-maps.md](../../engine/room-light-maps.md)
