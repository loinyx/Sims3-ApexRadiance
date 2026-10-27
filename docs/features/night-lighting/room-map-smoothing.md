# Smooth light maps

Lamp light inside rooms changes in smooth curves on floors and walls: the square steps of the room light maps' texels,
the bands of their 8-bit levels and the ring where the game cuts a lamp's light off are gone. The shadows of walls
and openings keep their shape; only the way the baked light is stored and read changes.

## Status

| | |
|---|---|
| Availability | Released in 2.13.0. The read of the room's own texels beside walls: Released in 2.16.0. The ambient-weight floor of roofed rooms: Released in 2.16.3. |
| Default | On |
| Menu | Lighting > Buildings > Rooms at Night, group LAMP LIGHT (needs *Outdoor light between floors*) |
| Configuration | `mapasDosComodosSuaves` under `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/room_map_dither.cpp`](../../../features/room_map_dither.cpp), [`features/level_light_share.cpp`](../../../features/level_light_share.cpp) (`FadeAtThreshold`), [`features/shader_patches.cpp`](../../../features/shader_patches.cpp) (`PatchRoomMapSmooth`), [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) (`DrawRoomMapSmooth`) |

## The problem

The room solve ([engine/room-light-maps.md](../../engine/room-light-maps.md)) stores lamp light per texel: 1, 2 or 4
texels per metre by lighting class (twice that with *Light detail* High). Three things make that light look blocky:

- **Bilinear reads.** The InteriorFloor and InteriorWall shaders read the map with the bilinear filter, so the light
  changes in straight ramps between texel centres and every texel shows as a diamond or a step, most visibly at shadow
  edges.
- **8-bit levels.** Each texel is packed to 8 bits per channel (`0x0060CD10`); a lamp's slow falloff becomes rings of
  one level each.
- **The light cut.** The solve drops a lamp whose `r + g + b` at a point is under the room's threshold (`room+0x63C`,
  3/255 at class 2, 6/255 below): the lamp's circle ends in a step of that size.

## How Apex Radiance solves it

1. **B-spline reads.** Every pixel shader of the room-lit techniques (InteriorFloor, InteriorWall and their siblings that
   read the same map: FloorWith\*, InteriorWallWith\*, Hideable, FloorTileCeiling) is found by pattern and drawn with a
   copy whose map read is a cubic B-spline (four bilinear taps). The paired vertex shader tells walls from floors by its uv
   scale (1/4096 for the wall atlas, 1/32767 for the floor maps). On walls, each tap stays in the atlas block of its row
   (3n + 1 texel rows, n = atlas width / 256), so no light comes in from the wall stored above or below. The B-spline
   reaches a texel further than the game's bilinear read, and where a room's texels touch those of the room beyond a wall
   with no texel row on the wall line it took that room's light: in game a hall beside a glass door showed a warm band
   along the wall, the courtyard's light (an F7 of the hall floor 0.15 m from the wall read 0.14 of a courtyard texel at
   (1, 0.55, 0.25) beside hall texels at 0.03). Held under the brightest of the 2 x 2 texels of the bilinear read (+2/255),
   a line of it stayed within half a texel of the wall (a courtyard lit green: a green line along the wall's foot), and
   the bound cut the B-spline's tail on the dark side of every shadow edge at the texel lines: beside a glass door the
   jambs' penumbra and the sill's showed steps a texel wide; held under the brightest texel of the room's side instead,
   a room's dark first texels under a window (the wall below the sill shades them) took the courtyard's light on the
   first and left the second dark, a black line half a metre from the wall between two lit bands. The solve marks the
   sides: a roofed room's texel holds its ambient weight in alpha (0.44 to 0.84 there), room 0's 0 (or the little light
   rooms send out of their openings). When the pixel's own texel (the one it lies in) has alpha 0.125 or more, the read
   is the B-spline of the 4 x 4 texels with alpha 4/255 or more alone, its weights divided by their sum (the own
   texel's weight is at least 0.23), colour and ambient weight alike: room 0's texels beyond the wall add nothing and the
   room's light falls to the wall as the solve has it, smooth across shadow edges and a jamb's penumbra. A pixel of room
   0, a bright room texel with little ambient weight or light sent out of an opening keep the plain B-spline. A roofed
   room's texel is packed with an ambient weight of at least 5/255 (the room is the solve's own, `[room+0x18] == 0`), so
   a brightly lit texel of the room is never taken for one beyond a wall: a dimmer texel beside it (under a table, behind
   a plant) would otherwise drop its light and show as a dark square one texel wide. The added ambient is 5/255 of the
   room's ambient colour on the brightest (near white) texels only, a few 8-bit levels at most.
2. **Dither.** Before the game packs a texel, Apex adds up to half an 8-bit level of interleaved gradient noise taken from
   the texel's position: the mean light stays, the rings become a fine grain the B-spline smooths out. The same texel
   always gets the same offset, so a room lit again does not shimmer.
3. **Soft light cut.** In the solves Apex wraps, the room's threshold is lowered for the game's call and each lamp's
   colour, after Apex's own tests, takes `smoothstep(thr, 2 thr, r + g + b)`: 0 where the game cut it (still dropped
   before any test), the full colour from twice the threshold. An outdoor lamp's entry through an opening, which Apex
   scales after the game's law (the outdoor cut, the outdoor surface's factor, the colour response,
   [level light share](level-light-share.md)), meets the threshold times those scales (at most 1): measured on the
   scaled colour, the cut dropped a window's light where the law still gave it several times the threshold, a strip of
   light a metre into the room and none past it.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Rooms at Night > Smooth light maps | `mapasDosComodosSuaves` | bool | on | | B-spline reads of the room maps (per draw), the texel dither and the soft light cut (baked); relights every room when changed |

## Compatibility and interactions

- The wall, opening and floor tests of [level light share](level-light-share.md) are unchanged: the soft cut is applied
  after them, the dither after the game's clamp to 1.
- [Real-time outdoor lamps](rt-outdoor-lamps.md) and quick outdoor lamp edits ([outdoor-lamp-delta](outdoor-lamp-delta.md))
  relight room 0's maps with the same soft cut and dither, so a relit texel matches the solve.
- Developer > Tester switches each part off on its own (*Fade at the light cut*, *Dither the light maps*, *Smooth light
  map reads*) to compare frame time and look. Players have the one switch; the parts are sub-parts of it, on by
  default (each part runs while the option AND its switch are on).
- Banding Fix makes its own dithered copy of the B-spline copy, as of any other drawn shader.
- A ps_2_0 shader's copy is ps_2_x (about 45 more instructions, 11 more temporaries, arbitrary swizzles): drawn only when
  the device's `PS20Caps` allow it, else the game's shader draws.
- Steam 1.67.2 only for the dither (the pack call is checked byte by byte); the reads and the soft cut follow the
  shaders and the wrapped solves.

## Limitations

- Where the alpha does not tell the sides apart (two roofed rooms, room 0 beside room 0, light sent out of an opening, a bright room texel) the B-spline still blends the texels beyond a wall within 1.5 texels of it, as before the bound.
- A room solved on the render thread (the room queue solving at once) keeps the game's hard cut.
- A room whose light list holds a light of a class whose evaluation Apex does not wrap keeps the hard cut.
- Walls are drawn with the bilinear read again for the session if a wall block is seen off the 3n + 1 grid.

## Technical reference

The public gameplay experiment admits packed interior-floor vertex shaders even when the generic object detector
also recognises them. `IsPackedRoomFloorVs` requires static 1/32767 room-map UVs, the native packing constants and
the packed height reconstruction. The pixel shader must independently match the room-map read. This changes only
selection of the existing smoothing path; it does not change room-to-room light gathering or smooth across separate
room maps. A brightness discontinuity between those maps can remain.

The public memory/gameplay experiment also guards directional basis-map smoothing used by instanced structures.
Each smoothed RGB sample is bounded by its alpha-normalised original bilinear sample. Flat regions retain their
values while wider filtering cannot add light beyond that original sample. The guard adds four ALU instructions per
map, with no extra texture fetch or temporary register. It applies only to `PatchBasisSmooth`, including its combined
room-map variant; room wall-atlas filtering and `PatchIndoorBasis` keep their existing paths. This is a filter bound,
not geometry reconstruction: light already present in the native sample remains. Native stair occlusion remains
unverified in game.

| Site | What | Source |
|---|---|---|
| `0x006A34DB` (Steam 1.67.2) | CALL of the texel pack `0x0060CD10` in the batch solve `0x006A31D0`; redirected to `PackThunk`, which reads the texel's (x, y) from `edi` (its address) and the map's `{base, pitch}` at `[ebp+0Ch]`, adds the offset, raises a roofed room's alpha to 5/255 (the room is `ebx`, set by `mov ebx,ecx` at `0x006A31E8`, checked at start) and calls the game's pack | `room_map_dither.cpp` |
| `0x0069FE40` | `movss xmm0,[ebx+63Ch]; comiss; ja`: the light cut; `room+0x63C` is set to 1e-6 for the wrapped call, its real value restored after | `level_light_share.cpp` `SolvePoint`, `FadeAtThreshold` |
| Pixel shaders | `texld rL, <TEXCOORD1>, sM` then (after at most one `mul rL, rS.c, rL`) `mad rD.xyz, rL.w, cA, rL`, `cA` not defined: replaced by the B-spline; constants size `(w, h, 1/w, 1/h)` and band `(1/S, S, 0.5, S - 1)` per draw | `shader_patches.cpp` `PatchRoomMapSmooth` |
| Vertex shaders | `mul o<TEXCOORD1>.xy, v#, cK.c`: 1/4096 wall atlas, 1/32767 floor maps | `shader_patches.cpp` `RoomMapUvScale` |
| Wall blocks | `wall + cls*0x20 + 0x58` `{x0, y0, x1, y1}`; `y0 % rows` checked for every piece laid out | `level_light_share.cpp` `NoteWallBand` |

**Arch variant.** Floor copies also come in a variant that adds the neighbour's ambient near arches
([arches.md](arches.md)): up to 6 segments (`kArchCells`), each two constant rows (a.x, a.z, e.x, e.z) and
(D.rgb, 1 / |e|^2), placed after the smoothing constants. When a floor also reads the directional maps and the variant
with the wall mask does not fit the registers, the combined variant without the mask is used, never one that drops the
room map's smoothing.

Cost: the reads add about 200 ALU and 20 texture instructions per room-lit pixel (the B-spline, the own texel and the 4 x 4 point reads with their weights); the copy takes 14 temporaries more than the game's shader; the dither one call per texel solved; the
soft cut a list walk per solved point and a multiply per lamp kept.

## Rejected approaches

- Raising the map density to 16 texels per metre: four times the memory and the solve time of High, and past the game's
  2048 size cap on large lots.
- Screen-space shadows: the per-frame relighting this needs was removed earlier for its cost and flicker.
  Details in [history](../../history/night-lighting-room-map-smoothing.md).

## See also

- [Validation](../../validation/night-lighting-room-map-smoothing.md)
- [History](../../history/night-lighting-room-map-smoothing.md)
- [Room light maps](../../engine/room-light-maps.md)
- [Light through arches and doorways](arches.md), [Stairs on the directional maps](stairs.md)
