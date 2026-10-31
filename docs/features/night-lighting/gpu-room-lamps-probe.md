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
| Menu | Developer > Compare lighting paths > Check indoor lamps against the GPU law (with Lamp lighting: Instant) |
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
4. The capture is dropped.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Check indoor lamps against the GPU law | `roomLampProbe` | bool | off | | Developer only, with Lamp lighting: Instant. Compares each room solve of the active lot with the GPU's lamp law and logs the result |

## Limitations

- No shadow: "dark in the solve" counts every texel a test of the solve shades (walls, floors, Apex's opening tests). The
  next step compares those texels with the occluders the GPU would use.
- Only solves on the light tree thread with Lamp lighting: Instant (the hooks of Quick outdoor lamp edits run only then);
  quick passes and rooms with a light whose evaluation is not wrapped are not probed (logged once each).
- Each probe costs about one model evaluation per texel and lamp on the light tree thread (the log gives its time).
- At most 600 log lines a session.

## Technical reference

- **Logs** (`[RoomLampProbe]`): `Story S room R class C (roofed|roofless): N texels, K lot lamps of L lights listed;
  their light P% within 2/255 of the GPU law unshadowed (M texels lit by either); light no part holds at U texels; T ms`,
  then per lamp `lamp A type T xM (registered in room R): N texels lit, a% within 2/255, b% dark in the solve, c% lit only
  by the solve, d% other strength (median solve/law r); darkest miss: ...; largest other: ...` with the texel's position,
  normal and distance to the lamp. `not probed (...)` and `not compared (...)` give why a solve was left out.
- **Sample layout**: the batch sample's position at +0x00 and normal at +0x10, world space, as `ExportRoom0` reads them.
- **Test**: `tools/outdoor_delta_test` checks `ProbeClassify`'s five classes.

## See also

- [Quick outdoor lamp edits](outdoor-lamp-delta.md), [Real-time outdoor lamps](rt-outdoor-lamps.md)
- [engine/room-light-maps.md](../../engine/room-light-maps.md)
