# Stairs on the directional maps

Stairs, railings and objects standing against a wall no longer take the colour of a lit room on the other side of that
wall. A stair made of the same material as the floor beside it has the floor's colour under the same light, and the
upper steps of a stair take the light of the floor they lead to. Part of [Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released in 2.14.0 |
| Default | On |
| Menu | None. Follows Night Lights and Lighting > Objects > *Objects* card > Game fixes > *Smooth indoor light* |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml`, read through `bordasDosMapasDeLuz` |
| Source | [`features/shader_patches.cpp`](../../../features/shader_patches.cpp) (`PatchBasisSmooth`, `BasisSmoothOptions`: `wallMask`, `floorWall`, `stairRoomFill`, `floorTone`, `storyBlend`), [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) (`DrawBasisSmooth`), [`features/level_light_share.cpp`](../../../features/level_light_share.cpp) (wall masks and ambient maps) |

## The problem

Stairs and other objects drawn with the room's directional maps read four 64 x 64 maps per story, one texel per metre
([room-light-maps.md](../../engine/room-light-maps.md)):

- **Reads across walls.** The game reads those maps bilinearly, so a step or an object against a wall blends in the
  texel behind the wall: the last step beside a red room turns red.
- **The floor-map read.** Faces turned up also read the floor's light map (4 texels per metre) bilinearly and bleed the
  same way.
- **A different rule from the floor.** The object rule is `max(directional, ambient cube x 0.25, floor map)`. The
  ambient cube is the world's grey or blue, not the room's tint, and the object shader has no tone curve, while the
  floor applies one ([shaders.md](../../engine/shaders.md)). The same texture looks like another colour on the stair.
- **Dark top steps.** The upper steps lie under the slab of the story above, where the lower story's maps are 0.

## How Apex Radiance solves it

The smoothed copy of the stair's pixel shader that *Smooth indoor light* already draws
([README](README.md#smooth-indoor-light-bordasdosmapasdeluz)) takes five more parts. The directional light itself is
unchanged.

1. **Wall mask.** Each story has a 64 x 64 mask built from Apex's wall grid: r = wall on the tile's -x side, g = +x,
   b = -z, a = +z. Only full walls without an opening block. Before every read, the uv is moved so the filter never
   crosses a masked side. Within 6 cm of a masked side (the wall's thickness), each read keeps the smaller of the two
   texels, so the lip of a step that enters the wall takes no light from behind it.
2. **Floor-map read.** On a masked side the floor-map read stays at least half a texel inside its own tile.
3. **Room fill.** A 64 x 64 ambient map per story holds each tile's room ambient (`room+0x110`, the value floors and walls
   use). The fill becomes that ambient x the floor map's alpha, the floor's own rule, instead of the world's cube.
4. **Floor tone.** The copy ends with the floor's tone curve, f(l) = (2 / (1 + 2^(-4.2 l)) - 1) / l with
   l = (r + g + b) / 3: about x1.46 in the dark.
5. **Story blend.** The copy also reads the story above's directional and ambient maps and blends by the pixel's height:
   a smoothstep from 0.5 m over its own floor to the next floor. The top step takes the landing's light.

If a story, a mask or an ambient map cannot be found for a draw, the draw uses the previous copy.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| No setting | | | | | On whenever Night Lights and *Smooth indoor light* (`bordasDosMapasDeLuz`) are on |

## Compatibility and interactions

- Of the game's 56 stair and object shader variants, all take the wall mask, the room fill and the tone. Only 10 accept
  the story blend; 28 have no free sampler and 18 do not pass the normal.
- Outdoor and yard stairs use the exterior technique and are not affected.
- [Light through stairwells](stairwells.md) brightens the per-tile ambient that the room fill reads.
- Doors, windows, arches and half walls do not block in the mask, so light through a door still reaches a stair.

## Limitations

- Not confirmed in game.
- Variants that refuse the story blend keep a dark top step under the slab.
- The lip of the top step within 6 cm of a wall takes the smaller of the two texels and can be slightly darker.

## Technical reference

- **Story of a draw.** `DrawBasisSmooth` finds the story from the bound +X basis map in the manager's basis set
  (`+0x220`, the wrapper's `+0x30`); `StoryAboveFor` gives the story above.
- **Mask texture.** Bound with `D3DSAMP_SRGBTEXTURE` off; a read is one texture fetch and about 15 instructions.
- **Height.** The stair's vertex shader already writes the world y in `o5.w`; without it, the blend constant is 0.
- **Options.** `BasisSmoothOptions` selects the parts (`wallMask`, `floorWall`, `stairRoomFill`, `floorTone`,
  `storyBlend`).
- **Diagnostics.** The F8 snapshot shows "walls in the directional maps: matched / not matched" per story.

## Rejected approaches

- A 3 cm wall band: too narrow for the lip of a step 4 to 5 cm past the wall's line.
- The floor-map read as the cause of the red sliver: the capture used was taken in another view.
- Keeping the stair's tone as the game's own rule: the missing tone curve and the world cube were the colour difference.

Details in [history](../../history/night-lighting-stairs.md).

## See also

- [Validation](../../validation/night-lighting-stairs.md)
- [History](../../history/night-lighting-stairs.md)
- [Night Lighting: smooth indoor light](README.md#smooth-indoor-light-bordasdosmapasdeluz), [Objects and rigs](objects-and-rigs.md), [Light through stairwells](stairwells.md)
- [Engine: room light maps](../../engine/room-light-maps.md), [Engine: shaders](../../engine/shaders.md)
