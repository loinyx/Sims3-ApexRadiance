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
| Availability | Released in 2.13.0 (Performance > Lamp lighting > Instant (GPU)). Repeated game writes relit at most every 0.5 s: In development (2.15.1) |
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
2. **The maps.** Each story's own room-0 maps of the class on screen (wall atlas, floor map; ceiling and object maps when
   they hold room-0 texels) get a copy in video memory. Every room-0 texel of the kept solve is relit over the game's map:
   `pack(min(base + Σ mult × fade(law × shadow), 1))`, with the floor mask's alpha and the class-2 wall blur. The law,
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
   take room 0's lamps (yards, rooms near openings) are sent once, 1.5 s after the story's last edit, with no urgency. The
   lamp's own object, which the game marks as an occluder at every drag step (`0x006C7939`), marks no room while the GPU
   draws the lamp. Every 2 s a GPU lamp that is on is checked against the stories it was drawn on: a story whose room 0 no
   longer lists it is sent to gather again, once. A lamp switched on takes the usual path.
6. **Drags.** While a lamp keeps changing (within 300 ms of its last change) its maps are relit with a cheaper shadow
   estimate (`AccFastPS`), at most one relight every 200 ms, no map is uploaded, and the floor mask's under-floor share is
   computed on a worker thread (the map keeps its last copy until it arrives); once the lamp stays put every map is relit
   with the full estimate. Each frame starts maps only while its relight work stays under about 2 ms of CPU.
   A map the game writes again and again (its room solves, for instance while a lot stays loading) is relit for those
   writes at most every 0.5 s; a single write after a rest is relit at once (since 2.15.1; 21/10: 105 writes in 10 s,
   each relight 45 ms of floor mask with 28 lamps, logged as "relights for repeated game writes moved later").
   On story 0's floor map, a lamp whose light stands less than 40 cm over the lot's ground (its base `+0xD4` taken into
   lot space) is drawn from 40 cm (since 2.15.1): a lamp lying on the lawn, its light 5 cm up, lit a tree above it and
   left the lawn dark, the law's light falling with its angle of incidence (21/10 F8 17-26-36). Walls, ceilings, objects
   and trees keep its real place; Lamp lighting Classic and the world grass keep the game's. Developer A/B: Lighting >
   Developer > *Lamps on the ground light the lawn from 40 cm*.
   Under DXVK the floor mask buffer is written in two copies, alternately: each write fills the one the last draws did not
   read and then becomes the drawn one, since a lock of a buffer the GPU may still read waits there (native D3D9 copies
   a managed buffer at its lock, with one copy as before).
7. **Rebuilds.** A story's maps are built again only when its base changes (`OutdoorDelta` `BaseHash`). The export and
   the texel streams are prepared on a worker thread; the render thread uploads one map a frame, and the old copy stays
   bound meanwhile. An upload that fails waits 1 s, 2 s ... 32 s; an export that fails waits 0.25 s ... 8 s.
8. **Kept parts.** The memory budget of Quick outdoor lamp edits never drops the active lot's room 0s
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
- Shader model 3 and floating-point render targets (A32B32G32R32F, G16R16F) are required.
- Classic (or Instant unable to run): nothing of this or of Quick outdoor lamp edits runs, no texture is swapped and no SetTexture
  callback is registered; the solve hooks only test that the option is off.
- Turned off, the copies are dropped, the game's maps are bound again and the active lot's room 0s are solved again.

## Limitations

- Only the active lot. Other lots, the world grass and roads (terrain bake) and the world's street lamps stay baked.
- Rooms that take room 0's lamps (yards, rooms near openings) and their objects follow at the game's pace.
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
- **Logs**: `[RtOutdoorLamps]` lines (inactive reasons once each, each story's map identity, each relight and each edit).
- **Offline check**: `tools/rt_lamps_test/run.ps1` (shadows against a CPU reference on a Direct3D 9 device, the bake split
  and the wall blur).

## See also

- [Quick outdoor lamp edits](outdoor-lamp-delta.md), [Level light share](level-light-share.md)
- [engine/room-light-maps.md](../../engine/room-light-maps.md)
