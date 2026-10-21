# Stairs on the directional maps: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/stairs.md](../features/night-lighting/stairs.md). All entries were made during the
2.14.0 development builds, numbered here as dev.N.

### 2026-10-20: wall mask in the directional maps (dev.17)

**Context:** the last step of a stair turned red beside a red-lit room, also with both options for light between rooms
off.

**Finding:** the stair reads the story's four directional maps (64 x 64, 1 texel per metre). The game's bilinear read
blends in the texel of room 8 behind the wall. Apex's B-spline was already bounded by that read (`min`), so it could not
help.

**Outcome:** a wall mask per story (r = -x, g = +x, b = -z, a = +z; full walls without openings only) moves the uv before
every read so the filter stops at the texel's centre. The story is found from the bound basis map in the manager's set
(`+0x220`, the wrapper's `+0x30`).

### 2026-10-20: a 3 cm band, wrapper offset confirmed (dev.18)

**Context:** a thin red sliver remained on the step's edge.

**Finding:** the mask matched its story 8,277 times and never missed, which confirms the wrapper offset. The four maps
were 0 on the stair's tiles. The step's edge lies on the wall's line (lot z = 25.0), so millimetres of it fall in the
red tile.

**Outcome:** within 3 cm of a masked side, each read keeps the smaller of the two texels, per channel.

### 2026-10-20: the floor-map read (dev.22)

**Context:** the sliver was still visible.

**Finding:** up faces also read the floor's light map (`texld r0, v1.zwzw, s1`, 4 texels per metre) bilinearly. The
capture used for this finding was later found to show another view; see the next entry.

**Outcome:** on a masked side, the floor-map read stays half a texel inside its own tile and takes the smaller texel in
the wall band. Kept as correct, although it was not the sliver's cause. The stair's darker tone was also attributed to
the game's own object rule and left unchanged; reversed by dev.28 and dev.32.

### 2026-10-20: a 6 cm band (dev.23)

**Context:** an F7 capture with the cursor on the red line (colour 0.169 0.039 0.012).

**Finding:** projected with the capture's camera, the pixel is the lip of the top step at about y 27.2, z 24.95 to 24.96:
4 to 5 cm past the wall's line at z = 25, inside the red tile. The 3 cm band did not reach it; a pixel 1 cm away was
already clean.

**Outcome:** the band is 6 cm (directional maps and floor read). Wall faces stand 5 cm or more from the line, so only
geometry that enters the wall falls in it.

### 2026-10-20: the room's colour on the stair (dev.28)

**Context:** a stair with the floor's texture looked a different colour: floor beside it (2, 19, 1), steps (18, 20, 7).

**Finding:** the floor adds `map.a x c4`, the room's green ambient. The stair's object rule takes
`max(directional, ambient cube x 0.25, floor map)`, and the cube is the world's neutral grey.

**Outcome:** a 64 x 64 ambient map per story (`room+0x110` per tile). The stair's fill becomes that ambient x the floor
map's alpha. A native fixture reproduced the game's grey (64, 115, 64) and gave (5, 77, 5) with the room's ambient.

### 2026-10-20: the floor's tone curve (dev.32)

**Context:** the same texture still looked different on the stair and the floor.

**Finding:** the floor shader (PS `2D8D0100`) ends with the game's tone curve
`c x (2 / (1 + 2^(-4.2 l)) - 1) / l`; the stair (PS `2D8D5628`) has none. In the dark the floor is about x1.46 brighter
under the same light. The curve was first evaluated with e^x by mistake; D3D's `exp` is 2^x.

**Outcome:** the stair copy applies the same curve: (5, 77, 5) becomes (7, 110, 7), x1.44.

### 2026-10-20: the story above at the top steps (dev.33)

**Context:** a stair down from the second story took the lamp's light from the middle down but not at the top.

**Finding:** the top steps stand on tiles under the upper slab, where the lower story's maps are 0, although the upper
floor beside them is lit.

**Outcome:** the copy also reads the upper story's maps and blends by the pixel's height (`o5.w`):
`t = smoothstep((y - (lower floor + 0.5)) / (upper floor - lower floor))`. Of the 56 variants, 10 accept it; 28 lack a
free sampler and 18 the normal.

### 2026-10-20: `BasisSmoothOptions` (dev.44)

**Context:** a review of the 2.14 series found the patch's flags hard to follow.

**Outcome:** the parts are selected through one `BasisSmoothOptions` structure. The generated shaders are identical byte
for byte to those generated before the change. The mask texture is bound with `D3DSAMP_SRGBTEXTURE` off.
