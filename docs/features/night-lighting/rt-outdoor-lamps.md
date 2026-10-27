# Real-time outdoor lamps

The outdoor lamps of the active lot are drawn into its stories' outdoor light maps on the graphics card, with the
shadows of the lot's walls, roofs and floor slabs. Moving, recolouring, dimming, switching, adding or removing such a lamp
changes its light on the outside walls, outdoor floors and lot grass of every story in the next frame, instead of after
room 0 of each story is solved again. The light keeps the bake's law: the same falloff, cones, reach and brightness.

It is the *Instant (GPU)* mode of Performance > Lamp lighting, the default. With Lamp lighting on Classic, or on a device
that cannot run it, none of it runs.

## Status

| | |
|---|---|
| Availability | Released in 2.13.0 (Performance > Lamp lighting > Instant (GPU)). Repeated game writes relit at most every 0.5 s and lamps on the ground drawn from 40 cm: Released in 2.15.1. Baked lamp records without padding (the frame rate of 2.15.1 while moving the camera): Released in 2.15.2. Shaders from the shader cache: Released in 2.15.3. Light past 1 on the lot ground, outdoor floors and outside walls (the outdoor scale): Released in 2.16.0. Kept solves that account for every light: Released in 2.16.1. |
| Default | On (Lamp lighting: Instant (GPU); a saved `lampLightingMode` keeps its choice) |
| Menu | System > Performance > Lamp lighting > Lamp updates: Instant (GPU) |
| Configuration | `lampLightingMode = "instant"` under `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/rt_outdoor_lamps.cpp`](../../../features/rt_outdoor_lamps.cpp) (the maps, the swap, the edits), [`features/rt_lamp_gpu.cpp`](../../../features/rt_lamp_gpu.cpp) and [`shaders/rt_lamps_hlsl.h`](../../../shaders/rt_lamps_hlsl.h) (shadow maps, relight), [`features/rt_lamp_proxy.h`](../../../features/rt_lamp_proxy.h) (occluders), [`features/rt_lamp_model.h`](../../../features/rt_lamp_model.h) (law and bake split), [`features/outdoor_delta.cpp`](../../../features/outdoor_delta.cpp) (`ExportRoom0`) |

## The problem

Outdoor lamp light on walls, floors and lot grass comes only from room 0's light maps, which the CPU solves texel by
texel, one story at a time ([engine/room-light-maps.md](../../engine/room-light-maps.md)). A lamp edit waits for each
story's room 0 to reach its solve: on a house with several stories the upper floors follow the lamp late.

## How Apex Radiance solves it

The bake is kept, and the lamps the graphics card draws are taken out of it exactly:

1. **The base.** [Quick outdoor lamp edits](outdoor-lamp-delta.md) keep each room 0's sum and each light's part of it per
   texel. The parts of the lamps the GPU draws leave the sum (`OutdoorDelta::ExportRoom0`): what remains is the texel
   without them. Window lights, the world's street lamps and other lots' lamps stay in it. The GPU draws a lot's bare
   bulbs, spots, shaded lamps and lot street lamps (types 3, 4, 5 and 11) listed in room 0 of their own lot.
   A solve is kept only when its lights' parts account for its sum: past 2 levels of light at a texel that no part holds,
   it is not kept and the room's edits take the game's solve. Light no part holds would stay in the base whatever the GPU
   draws: a courtyard kept a recoloured, then switched-off sconce's old colour until *Restart lighting*.
2. **The maps.** Each story's own room-0 maps of the class on screen (wall atlas, floor map; ceiling and object maps when
   they hold room-0 texels) get a copy in video memory. Every room-0 texel of the kept solve is relit over the game's map:
   `pack(base + Σ mult × fade(law × shadow))`, packed as the solve packs it (its hue kept past 1,
   [level-light-share](level-light-share.md#outdoor-texels-keep-their-hue)), with the floor mask's alpha and the class-2 wall blur. The law,
   cones and realistic reach are those of the room bake (`rt_lamp_model.h`, `lamp_reach.h`): the game's own cones for
   shaded lamps, the game's hard light cut and no map dither.
3. **Shadows.** The lot's occluders are read from its story grids on the light tree thread
   (`LevelLightShare::ExportLotProxy`): walls from 0.1 m below their foot to their top with their door and window holes,
   roofed quadrants at the ceiling (stopped at the floor of the story above), placed floors of upper stories as 0.2 m
   slabs. Each lamp gets a cube shadow map (6 × 256² faces). The shadow is the share of the lamp's 0.15 m ball the texel
   sees, from a blocker search and a walk along the rays to a disc of points on the ball.
4. **The swap.** Wherever the game binds one of these maps, the copy is bound instead (a `SetTexture` callback registered
   the first time Instant is chosen), so every draw that reads room 0's light gets the lamps without a shader patch.
5. **Edits.** The game's lamp entry update (`0x006C7CD6`, LampMarkFilter) asks whether the GPU draws the lamp on every
   story of a lot whose maps are all relit. If so, the edit marks no room and room 0 is not solved again; the rooms that
   take room 0's lamps (yards, rooms near openings) are sent once, 1.5 s after the story's last move (0.3 s after a value
   or a switch), with no urgency. The roofed rooms of that story and of the story above that saw a door of the edited lamp
   at their last gather (outdoor light through openings, `SameStoryRoofed`) go with them at a lamp edit's priority (tier
   1), as the changed-room walk sends them on Classic: such an edit marks no room, and before this the rooms behind a
   courtyard sconce's glass doors kept its old colour until something else solved them (a story change, for instance). The
   lamp's own object, which the game marks as an occluder at every drag step (`0x006C7939`), marks no room while the GPU
   draws the lamp. Every 2 s a GPU lamp that is on is checked against the stories it was drawn on: a story whose room 0 no
   longer lists it is sent to gather again, once. A lamp switched on takes the usual path.
6. **Drags.** While a lamp keeps changing (within 300 ms of its last change) its maps are relit with a cheaper shadow
   estimate (`AccFastPS`), at most one relight every 200 ms, no map is uploaded, and the floor mask's under-floor share is
   computed on a worker thread (the map keeps its last copy until it arrives); once the lamp stays put every map is relit
   with the full estimate. Each frame starts maps only while its relight work stays under about 2 ms of CPU.
   A map the game writes again and again (its room solves, for instance while a lot stays loading) is relit for those
   writes at most every 0.5 s; a single write after a rest is relit at once (since 2.15.1; an F8 session: 105 writes in 10 s,
   each relight 45 ms of floor mask with 28 lamps, logged as "relights for repeated game writes moved later").
   On story 0's floor map, a lamp whose light stands less than 40 cm over the lot's ground (its base `+0xD4` taken into
   lot space) is drawn from 40 cm (since 2.15.1): a lamp lying on the lawn, its light 5 cm up, lit a tree above it and
   left the lawn dark, the law's light falling with its angle of incidence (F8 capture). Walls, ceilings, objects
   and trees keep its real place. The terrain bake stamps such a lamp from the same 40 cm (`BakePosStub`,
   [terrain relight](terrain-relight.md)), so the lot pass's max(lot map, atlas), the soft lot edge, snow (atlas x 4),
   Classic and the moments right after an edit, when the game's own map is drawn, light the lawn too. One rule:
   `TerrainBakeRule::StampHeight` (`features/terrain_bake_rule.h`), also in the stamp mirrors (`BakeStamp`,
   `UnderFloorShare`, `GroundShadow::Weight`), for lot lamps of the lamp classes only (`TerrainBakeRule::Liftable`; a
   lot's street-lamp class and the world's street lamps keep their height, as in the bake). Developer A/B: Lighting > Developer > *Lamps on the ground light the lawn
   from 40 cm* (both places; the terrain is rebuilt once).
   Under DXVK the floor mask buffer is written in two copies, alternately: each write fills the one the last draws did not
   read and then becomes the drawn one, since a lock of a buffer the GPU may still read waits there (native D3D9 copies
   a managed buffer at its lock, with one copy as before).
7. **Light past 1 (the outdoor scale).** A room-0 texel is packed with the sum's hue kept past 1: divided by its largest
   channel, so within a strong lamp's reach the ground and the walls were flat at the cut, and a room lit through a door
   or a window looked as bright as the yard beside it. Each relit copy gets a second A8R8G8B8 texture of its size, written
   by the compose draw as a second render target: `log2(largest channel) / 3` in 8 bits (1 to 8 times, 0.8 % a level),
   0 within 1 and on texels no sample writes. A wall atlas's texels go through a scratch map, their scale copied after them
   (the scale is not blurred). `RtOutdoorLamps::ScaleOf` gives the scale of a bound copy. The lot pass
   ([lot-light-pass](lot-light-pass.md)), the summer outdoor floors ([floors](floors.md)) and the ps_3_0 outside walls
   ([walls](walls.md)) multiply the copy's colour by `2^(3 r)`; every other draw reads the copy as before.
8. **Rebuilds.** A story's maps are built again only when its base changes (`OutdoorDelta` `BaseHash`). The export and
   the texel streams are prepared on a worker thread; the render thread uploads one map a frame, and the old copy stays
   bound meanwhile. An upload that fails waits 1 s, 2 s ... 32 s; an export that fails waits 0.25 s ... 8 s.
9. **Kept parts.** The memory budget of Quick outdoor lamp edits never drops the active lot's room 0s
   (`OutdoorDelta::PinRooms`, 160 MB ceiling; their drawn lamps' parts kept as one sum per texel, the base this part reads).
   Past the ceiling a new solve of one is not kept and that story is no longer sent to its solve, so the stories do not
   evict one another in a loop ([outdoor-lamp-delta](outdoor-lamp-delta.md#limitations)). A story with no kept solve is sent
   to its solve at most three times in a row.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Lamp lighting > Lamp updates: Instant (GPU) | `lampLightingMode` | string | `"instant"` | `"classic"`, `"instant"` | Instant draws the active lot's outdoor lamps on the GPU with real shadows and keeps the parts of Quick outdoor lamp edits on. Not selectable (a warning note; the lamps then work as Classic) on a device without shader model 3 or when the shaders cannot be made. A change relights the active lot once ([Performance](../performance/README.md#lamp-lighting-card)) |

The status line *Real-time outdoor lamps* (Developer > Lighting > Inspect lighting state) shows the maps relit, the lamps
handled, the shadow tiles rendered and the last relight times, or why the feature is inactive.

## Compatibility and interactions

- Needs Night Lighting's Outdoor light between floors (the room solve hooks) and the parts of
  [Quick outdoor lamp edits](outdoor-lamp-delta.md), which this option keeps on. Steam 1.67.2 only.
- Shader model 3 and floating-point render targets (A32B32G32R32F, G16R16F) are required. The light past 1 also needs two
  simultaneous render targets (`NumSimultaneousRTs` 2 or more); without them the maps are relit as before, cut at 1.
- Classic (or Instant unable to run): nothing of this or of Quick outdoor lamp edits runs, no texture is swapped and no SetTexture
  callback is registered; the solve hooks only test that the option is off.
- Turned off, the copies are dropped, the game's maps are bound again and the active lot's room 0s are solved again.

## Limitations

- The cause of the courtyard's stale light (2.16.0) is not confirmed, and the accounting check has not been seen to refuse
  a solve in game ([validation](../../validation/night-lighting-level-light-share.md)). If old light stays, *Restart
  lighting* clears it.
- Only the active lot. Other lots, the world grass and roads (terrain bake) and the world's street lamps stay baked.
- The light past 1 shows on the lot ground, the summer outdoor floors and the ps_3_0 outside walls of the active lot. Classic,
  other lots, winter and snowy floors, the ps_2_x walls (lower shader settings) and objects keep the cut at 1.
- Rooms that take room 0's lamps (yards, rooms near openings) and their objects follow at the game's pace, through their
  own solve after the send above.
- Objects, Sims and terrain do not cast shadows; roofs are flat at the wall top, as in the solve's own test.
- A lamp added or removed reaches the GPU set once the game gathers the room's light list again.
- A map whose walls were blurred by the game's own blur is not relit; the log says so.
- The lot geometry key counts wall tops that rose and floor edits of every lot together: such an edit on another lot
  rebuilds the active lot's occluders and solves its room 0s in full once.

## Technical reference

- **Map identity.** The unlock `0x0069E950(commit)` copies the class's working maps (`0x011D0460 + class × 16`) into the
  room's own set `[room+4] + class × 64 + 0xC` (index 0 floor, 1 ceiling, 2 objects, 3 walls); a wrapper's texture is
  `[wrapper+0x30]` while `[wrapper+0x2C] == 0` ([engine/room-light-maps.md](../../engine/room-light-maps.md)).
- **Self-measure.** Every 10 s while the option is on: the average and largest cost a frame on the render thread, the
  relights and why the maps were relit.
- **Coverage.** Lamp edits mark no room only while every story's maps are relit; the log says when that starts and
  stops (`Every story covered` / `Not every story covered (...)`). A lamp drawn on the GPU that is still missing from a
  story's light list where it can be drawn does not count as covered: its edit also goes to the game's solve, and the
  consistency check asks those stories to gather their lights again.
- **GPU passes**, in the `EndScene` pass before the overlay, with the device state saved: shadow atlas 1536 × 4096
  G16R16F with D24X8 depth, 16 lamps an atlas, near 0.005 m, far 64 m; the relight accumulates 16 lamps a draw into an
  A32B32G32R32F target at each texel, then composes and packs into the copy; the wall blur runs on the GPU with the
  game's integer rounding.
- **Shaders.** The 13 vertex and pixel shaders (`shaders/rt_lamps_hlsl.h`, `RtLampGpu::ShaderId`) are registered with
  Apex's shader cache with the same source, entries, targets and flags they always used, so their bytecode is unchanged.
  They are precompiled with every other Apex shader at start-up, off the render thread, and come from the disk cache or
  the copy built into the ASI ([shader precompile](../../architecture.md#44-shader-precompile)). The first frame the
  option runs, a worker takes the 13 bytecodes with `ShaderCache::Bytecode`; until they are ready the status says the
  shaders are being compiled, and a shader that failed to compile leaves the option inactive with the compiler's message.
- **Outdoor scale**: `Slot::scale` (DEFAULT render target, made with the copy, released with it and at a device reset),
  `Slot::scaled` (the last relight wrote it); `Engine::Compose(..., scale)` clears it to 0, binds it as target 1 for the
  texels' draw (`ComposePS` COLOR1) and, for a blurred atlas, binds scratch 3 there and copies it into `scale` before
  the blur; `kScaleLog2 = 3`.
- **Logs**: `[RtOutdoorLamps]` lines (inactive reasons once each, each story's map identity, each relight and each edit).
- **Offline check**: `tools/rt_lamps_test/run.ps1` (shadows against a CPU reference on a Direct3D 9 device, the bake split
  and the wall blur).

## See also

- [Quick outdoor lamp edits](outdoor-lamp-delta.md), [Level light share](level-light-share.md)
- [engine/room-light-maps.md](../../engine/room-light-maps.md)
