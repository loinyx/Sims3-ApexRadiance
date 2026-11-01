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
  their blocker pointers at +0x30; each blocker's vtable, vfunc+0x14 and first 0x80 bytes) and up to 1,800 sample texels
  (position, normal, lamp, colour before the tests, after the wall test, final), for an offline replay of the 3D test.
- **Sample layout**: the batch sample's position at +0x00 and normal at +0x10, world space, as `ExportRoom0` reads them.
- **Test**: `tools/outdoor_delta_test` checks `ProbeClassify`'s five classes.

## See also

- [Quick outdoor lamp edits](outdoor-lamp-delta.md), [Real-time outdoor lamps](rt-outdoor-lamps.md)
- [engine/room-light-maps.md](../../engine/room-light-maps.md)
