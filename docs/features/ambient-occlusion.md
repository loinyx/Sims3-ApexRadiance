# Ambient Occlusion

Ambient Occlusion adds soft shade where surfaces meet: under furniture, in room corners, where walls touch the floor,
and around houses and trees. It is computed from the scene depth after the game draws the 3D world and before bloom and
the interface, so the game's own lighting, colours and UI are untouched. [Sim Occlusion](sim-occlusion.md) can adjust
the shade on Sims and their hair separately.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 |
| Default | On for new configurations |
| Menu | Image > Ambient Occlusion |
| Configuration | `[patches.AmbientOcclusion]` in `ApexRadiance.toml` |
| Source | [`patches/ambient_occlusion_patch.cpp`](../../patches/ambient_occlusion_patch.cpp) |

## The problem

The Sims 3 has no screen-space ambient occlusion. Objects resting on the ground and furniture standing against walls
receive the same ambient light as open surfaces, so scenes look flat and objects seem to float. The game's own bloom
and lamp lighting do not add contact shading.

## How Apex Radiance solves it

Apex Radiance runs a full-resolution GTAO (ground-truth ambient occlusion) pass on the game's depth buffer and blends the
result into the finished 3D image. The result is deterministic: a still camera always produces the same image, with no
temporal noise. The composite keeps lamp-lit and bright surfaces bright and preserves their colour, so the shade does
not leave a grey film.

Each frame:

1. **Copy** the back buffer.
2. **Linearise depth** into a 9-level pyramid of inverse view depth (1/z).
3. **March** the GTAO horizons for every pixel, with a contact radius (0.6 m) and a large radius (2.0 to 2.5 m),
   weighted by the surface normal. A fixed 4x4 pattern varies the directions per pixel, so there is no per-frame
   noise.
4. **Blur** with a depth-aware filter that keeps edges sharp.
5. **Composite** over the copy. Faint shade is dropped, a multi-bounce term keeps bright and coloured surfaces from
   turning grey, and lamp-lit pixels keep a configurable share of their light.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Ambient Occlusion (card switch) | `enabled` | bool | on | | Turns the effect on |
| Strength | `forca` | float | 168% | 0 to 200% | How dark the shade gets; 0 skips every pass |
| Distance | `distance` | float | 351 m | 25 to 1000 m | View distance where the shade fades out (fade starts at 37.5% of it). Not used in map view |
| Quality | `qualidade` | enum | High | Very Low, Low, Medium, High, Ultra | Directions per pixel: 2, 4, 6, 8, 12. Stored as 4, 0, 1, 2, 3 for compatibility with 2.1.0 |
| Also in map view | `noMapa` | bool | on | | Shades the map view with radii sized for houses and trees (contact 4 m, large 15 m, no distance fade) |
| Advanced > Reach | `alcance` | float | 130% | 50 to 200% | Scales all radii |
| Advanced > Keep lamp light | `protegerLuz` | float | 38% | 0 to 100% | Share of light that bright and lamp-lit pixels keep |
| Advanced > Show the shade alone | (not saved) | bool | off | | Shows only the shade, in grey |
| (none) | `revisao` | int | 9 | | Settings revision. Older files keep every saved value and only gain missing keys |

All settings apply immediately. Keys from the old combined build (`intensidade`, `raioM`, `visualizar`) are not read.

## Compatibility and interactions

- **Post-scene order:** Ambient Occlusion runs first (order 10), before Edge Smoothing (20) and Depth Blur (30). See
  [architecture.md](../architecture.md).
- **Shared depth:** Depth Blur and Reflections read the same INTZ depth copy; the depth swap stays on while any of them
  needs it.
- **Picture filters** are applied after the shade, so colour filters also affect it.
- **Compare with the game** (shortcut) turns Ambient Occlusion off together with Night Lighting, Depth Blur and Edge
  Smoothing.
- **Hidden game UI:** when the game UI is hidden, the scene boundary is detected after 20 depth-tested draws, so the
  effect keeps working.
- **Game anti-aliasing:** the effect pauses while the back buffer is multisampled. Use Edge Smoothing instead.
- **Sim Occlusion** adjusts this shade on Sims; see [sim-occlusion.md](sim-occlusion.md).
- **Hardware:** needs R32F texture filtering and G16R16F render targets. Without them the effect stays off and the card
  shows a warning.

## Limitations

- Surfaces that do not write depth (glass, particles, some transparent materials) show the shade of what is behind them.
- A shorter Distance skips distant GTAO marching, but the copy, pyramid, blur and composite passes still run.

## Technical reference

**Pipeline** (W x H = back buffer):

| Pass | Shader | Output |
|---|---|---|
| Copy | `StretchRect` | Colour copy |
| Linearise | `LinearizePS` | Level 0 of an R32F pyramid of 1/z in 1/m, padded to a multiple of 256 |
| Downsample x8 | `DownPS` | 2x2 average of 1/z (sky excluded) per level |
| GTAO | `GtaoPS` (SLICES 2/4/6/8/12) | G16R16F: R = visibility, G = 1/z |
| Blur x4 | `BlurPS` | Depth-aware box (H, V) then tent |
| Composite | `CompositePS` | Over the copy, RGB write |

**GTAO details.** Normal from the neighbour with the smaller depth step per axis. Slices at angle `(s + b1) pi / SLICES`,
4 geometric steps per side from 2 px (at 4K, scaled with height) up to the large radius (at most 30% of the height). Each
step reads the pyramid bilinearly at the nearest level to its spacing minus 2 (point mip filter). `b1`, `b2` come from a
4x4 Bayer matrix plus a golden-ratio phase. Contact horizon 0.6 m at strength 1.2; large horizon 2.0 m (near) to 2.5 m
(far) at strength 0.5 to 0.8, blending between 20 and 40 m. XeGTAO falloff (full to 38.5% of the radius). Exact `acos`.
Isolated pixels (leaf edges, thin rails) fade out. Off-screen samples count as sky.

**Composite.** `v = 1 - saturate((1 - ao - 0.05) / 0.95)`, then per channel
`m = MultiBounce(v, min(0.9, colour^2.2))` (Jimenez 2016), then `m = lerp(m, 1, saturate((luma - 0.35) * 2.5) * keep)`.

**Camera.** Near plane, depth scale and field of view come from `PostScene` (vertex-constant votes over the first 24
scene draws). Fallback: near 0.25, A 1.00008, `tanY = 1/4.293`. See
[engine/camera-and-map-view.md](../engine/camera-and-map-view.md).

**Resources.** R32F pyramid (3840x2304 with 9 levels at 4K) plus 8 one-level targets, two G16R16F screen targets and a
colour copy. Everything is released on device reset and rebuilt on the next frame.

**Cost** at 3840x2160, native D3D9, RTX 4070 Ti SUPER:

| Quality | Slices | Total |
|---|---|---|
| Very Low | 2 | 1.3 ms |
| Low | 4 | 1.8 to 2.0 ms |
| Medium | 6 | 2.5 to 2.65 ms |
| High | 8 | 3.0 to 3.2 ms |
| Ultra | 12 | 4.1 to 4.5 ms |

About 0.55 ms is fixed (copy and pyramid 0.26, blur 0.2, composite 0.09).

## Rejected approaches

- Half-resolution AO: caused the "micro dots" reports.
- Per-frame noise with temporal accumulation: twinkling on foliage.
- Radii in near-plane units: shade breathing with zoom.
- HBAO with a plain multiply: dirty and grey, too weak when toned down.
- Fast `acos` approximation: 0.5% bias for no measurable gain.
- Trilinear pyramid filtering: most of the original cost, no visual gain.

Details in [history](../history/ambient-occlusion.md) and [removed-features.md](../removed-features.md).

## See also

- [Sim Occlusion](sim-occlusion.md)
- [Validation](../validation/ambient-occlusion.md)
- [History](../history/ambient-occlusion.md)
- [Architecture: post-scene chain and INTZ depth share](../architecture.md)
