# Smooth light maps: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/room-map-smoothing.md](../features/night-lighting/room-map-smoothing.md).

### 2026-10-13: why interior shadows look blocky

**Context:** shadows of walls and furniture on room floors and walls showed texel steps even with *Light detail* High.

**Finding:** three separate causes. The room-lit shaders read the map with the bilinear filter (no InteriorFloor or
InteriorWall shader was patched). The batch solve `0x006A31D0` packs each texel with `0x0060CD10`: x 255 and a 1.5 x 2^23
add (round to nearest), so a slow falloff steps by whole levels. `LightPointWithAllLights` drops a light under
`room+0x63C` at `0x0069FE40` before its wall and occluder tests, a step of 3/255 or 6/255 at the edge of each lamp's reach.

**Outcome:** B-spline reads in copies of the room-lit shaders, a position-keyed dither before the pack, and a smoothstep
fade over the threshold's last step in the wrapped solves.

### 2026-10-13: the room-lit shader family and the wall atlas

**Finding:** over the Steam precomp, one plain `texld` of TEXCOORD1 followed by `mad rD.xyz, rL.w, cA, rL` (the ambient
term) matches 186 pixel shaders, all of techniques that read the same room map, none of ExteriorWall (which reads the
atlas with a lamp scale and no ambient term). Their vertex shaders scale the uv by 1/4096 (walls) or 1/32767 (floors). The
wall atlas is 256n x 128n texels with blocks of 3n + 1 rows; a block's row k is drawn at texel `y0 + N - 1 - k`.

**Outcome:** a pattern match (not shader ids); the band clamp from the atlas width; a runtime check of each block's `y0`.

### 2026-10-13: rejected

- **16 texels per metre:** four times the memory and the solve time of *Light detail* High and over the game's 2048 map
  size on large lots; the B-spline gives smooth edges at the existing density.
- **Screen-space shadows:** the per-frame relighting they need was removed earlier for its cost and flicker.

### 2026-10-14: ported to 2.12.3 + Real-time outdoor lamps

**Context:** the light through an arch into the next room and the lamp light around wall openings outdoors looked blocky.

**Outcome:** the B-spline reads, the dither and the soft light cut ported with the penumbra from the lamp's size; the GPU
relight of real-time outdoor lamps and the quick outdoor lamp edits take the same soft cut and dither. Each part has a
developer Tester switch. Offline: `tools/room_map_smooth_check` 957 checks, `tools/rt_lamps_test` 22, `tools/outdoor_delta_test`
11, 0 failures. Not yet checked in game.

### 2026-10-26: a black line half a metre from a wall under a window

**Context:** after the 4 x 4 bound of the pixel's side, an F7 of a room behind a courtyard window showed a dark line
parallel to the wall between two lit bands.

**Finding:** under a window the wall below the sill shades the room's first texels (courtyard 255 | wall | 0, 20, 47,
45 levels). The B-spline still mixed the courtyard's texels into the first one, held under the room's brightest texel
(47), and left the second at its own dark value: 46, 21, 42 across the first three texels.

**Outcome:** with the pixel's own texel a roofed room's, the read is the B-spline of that side's texels alone, its
weights over their sum, colour and ambient weight alike. `tools/room_map_smooth_check`: the window case falls 30.6 levels
after the wall with the bound and 0 now; GPU against the CPU reference 1 LSB.

### 2026-10-27: dark squares under furniture beside a lamp

**Context:** a 2.16.2 capture session: dark squares one tile wide, with hard edges, on a lit floor under a table's legs,
a plant and a floor lamp's foot.

**Finding:** the floor copy's masked read (2.16.0) excludes the 4 x 4 texels with alpha under 4/255, meant as room 0
beyond a wall. A roofed room's ambient weight falls with its light, so the brightest texels of the room itself had alpha
near 0 too. A dimmer texel beside them (alpha 0.125 or more: under the furniture) took the masked read and dropped their
light, while the pixels of the bright texels kept the plain read: a step at the texel's edge. Re-rendering the captured
atlas with the copy's arithmetic reproduced the squares (7.6% of the pixels off the plain read, up to 0.55).

**Outcome:** the texel pack raises a roofed room's alpha to 5/255 at least (`room_map_dither.cpp`; the solve's room is in
`ebx`). Room 0 keeps 0, so the wall case is unchanged; the re-render is then identical to the plain read inside the room.
The added ambient is 5/255 of the room's ambient colour on near-white texels, a few 8-bit levels at most. Maps already baked keep the squares until their rooms are solved again.

### 2026-10-27: two roofed rooms either side of a wall

**Context:** F7 19-26-24: a dark hall's floor along the wall of a lit living room showed a warm band (screen 0.090,
0.051, 0.027). The floor copy (`PS_6478B620`, the room map's B-spline) read the 4 x 4 texels around the pixel; the
living room's texels beyond the wall have alpha 5/255 (bright texels have little ambient weight, and roofed ones are
kept at 5/255 or more), so the read of the room's own texels (alpha 4/255 or more) took them in.

**Finding:** the alpha cannot tell two roofed rooms apart: it falls with the light (`(1 - sat(lum k)) x ambient`). A tag
of the room in the alpha's low bits was considered and rejected: rooms joined by an arch or a doorway would then never
mix, a hard line across every opening where the B-spline is smooth today. The story's wall mask (the stairs', one texel
per tile, a full wall with no opening in that cell) marks walls and not openings. The class-2 floor maps lay a tile's
texels from `(tile x n, tile z n)` (`light_detail.cpp`), so a texel's tile is its column and row over n.

**Outcome:** a copy of the floor read that drops the columns and rows of the 4 x 4 whose tile lies beyond a walled side
of the pixel's own tile (their B-spline weights set to 0 before the sum), used on floor maps whose story and class-2
layout are found (`FloorWallMaskFor`). Offline (`tools/room_map_smooth_check`, 1666 checks, 0 failures): all 186 copies
take the variant and are created natively; on a synthetic map (lit room alpha 5, dark room alpha 191, a wall with a
doorway) the dark room's first half metre takes +0 green instead of +63, the doorway stays within 1 LSB of the read
without walls, and the GPU matches the CPU reference within 1 LSB. Not yet checked in game.
