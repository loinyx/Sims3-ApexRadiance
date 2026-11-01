# GPU room lamps

A developer capture, the first stage of drawing indoor lamps on the GPU: each solve of a room of the active lot other than
room 0 is kept, split into the light the GPU will not draw and, per texel and lot lamp, the share of the lamp's law that
Apex's tests and the game's tests let through. With [the room lamp probe](gpu-room-lamps-probe.md) on too, each kept solve is
recomposed from what was kept and compared with the solve. Nothing is drawn yet.

## Status

| | |
|---|---|
| Availability | Developer build only, not released |
| Default | Off |
| Menu | Developer > Compare lighting paths > Keep indoor lamps for GPU (with Lamp lighting: Instant) |
| Configuration | `gpuRoomLampsCapture` under `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/outdoor_delta.cpp`](../../../features/outdoor_delta.cpp) (`KeepEnd`, `KeptSelfCheck`), [`features/rt_lamp_model.h`](../../../features/rt_lamp_model.h) (`KeptAdd`), [`features/level_light_share.cpp`](../../../features/level_light_share.cpp) (`LightEvalHook`) |

## The problem

Indoor lamp edits wait for the CPU room solves. To draw a lot lamp's light in a room on the GPU instantly, the GPU needs, per
texel, what the solve's tests did to that lamp: Apex's own tests (the floor under the story, light through openings, light
between stories, the soft light cut) and the game's wall and furniture tests. Those depend on the room's geometry and the
lamp's position, not on its colour or intensity.

## How Apex Radiance solves it

1. Turned on, every room of the active lot is sent to its solve once.
2. A solve of a room other than room 0 of the active lot, on the light tree thread, is captured with the room solve hooks the
   probe uses: per texel the batch sample (position, normal), the map and place, the solve's sum before the room's tone and
   each light's part of it; per evaluation the colour Apex's tests leave before the soft light cut, the threshold scale an
   outdoor lamp through an opening takes, and whether Apex's threshold early-out returned.
3. At the solve's end, for each lamp the GPU draws (a lot lamp of classes 3, 4, 5 or 11, alive, a shade its law describes:
   `RtOutdoorLamps::WorldLaw`), per texel where its evaluation was lit before the soft light cut down to a quarter of the
   threshold:
   - A = the colour before the soft light cut over the lamp's law there (r + g + b);
   - B = the lamp's part over its colour after the soft light cut: the game's 2D wall and 3D test share (0 when dropped, or
     never tested because the evaluation was dark);
   - the lamp's entries in the list, the threshold scale and the early-out.
   A, B and the scale are kept as half floats, per lamp by texel. The texel's base is its sum less the parts of those lamps.
4. The kept rooms stay above 2 GB (`ApexMem::HighAllocator`) within 192 MB for the active lot. Past it a new solve is not
   kept (logged); no kept room is dropped for another. A solve of another lot's room drops the kept rooms of the lot before.
   Turning the option off drops them all.
5. With the room lamp probe on, each kept room is recomposed at once with the lamps' laws as they are then:
   base + the sum of mult x fade(law x A) x B (`RtLampModel::KeptAdd`), and compared with the solve's sum.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Keep indoor lamps for GPU | `gpuRoomLampsCapture` | bool | off | | Developer only, with Lamp lighting: Instant. Keeps each room solve of the active lot for the GPU; with the probe on, logs a self-check |

## Limitations

- A is a share of the law's r + g + b: where the law's hue differs from the game's evaluation, the recomposed channels
  differ slightly.
- B is unknown where the game never tested the lamp (its colour faded or was dropped before the wall test); it is kept as 0,
  so a lamp made brighter would stay dark there. An evaluation darker than a quarter of the threshold is not kept.
- After Apex's threshold early-out the lamp's tests for light through openings and between stories never ran; a brighter
  law than the solve's is recomposed without them.
- Only solves on the light tree thread with Lamp lighting: Instant; quick passes and rooms with a light whose evaluation is
  not wrapped are not kept (as the probe).

## Technical reference

- **Logs** (`[GpuRoomLamps]`): `Story S room R class C: kept N texels, K drawn lamps, P parts (U never tested by the game),
  X MB; M rooms kept, Y MB of 192 MB`; `not kept (the 192 MB budget: ...)`, `not kept (no memory)`, `not kept (why)`; with
  the probe on, `Story S room R class C self-check: N texels lit, P% within 2/255 of the solve's sum recomposed from the kept
  parts (L lamps without a law now)`, then up to three of the worst texels: position, map, the solve's sum and the
  recomposed one, and the texel's largest kept part (lamp, entries, A, B, the law there, bits: 1 early-out, 2 never tested,
  4 soft light cut).
- **Kept part** (12 bytes): texel index, A, B and threshold scale as halves, entries, bits. Per texel 92 bytes: the 0x30
  sample, the capture's texel record and the base.
- **Test**: `tools/outdoor_delta_test` checks `KeptAdd` (the soft light cut and its scaled threshold, the early-out, the
  game's drop, half-float A and B).

## See also

- [Room lamp probe](gpu-room-lamps-probe.md), [Real-time outdoor lamps](rt-outdoor-lamps.md), [Quick outdoor lamp edits](outdoor-lamp-delta.md)
- [engine/room-light-maps.md](../../engine/room-light-maps.md)
