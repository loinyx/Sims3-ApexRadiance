# Sim Occlusion

Sim Occlusion controls how much ambient occlusion falls on Sims, separately from the rest of the scene. Body, face and
clothes have one intensity and hair has another, and a cap limits how dark a Sim can get. This keeps faces and hair
from looking dirty under the scene shade while furniture and rooms keep their full contact shadows.

## Status

| | |
|---|---|
| Availability | In development (PR #2) |
| Default | Off |
| Menu | Image > Ambient Occlusion > *Sim Occlusion* card |
| Configuration | `[patches.AmbientOcclusion]` in `ApexRadiance.toml` (shared with Ambient Occlusion) |
| Requires | [Ambient Occlusion](ambient-occlusion.md) on |
| Source | [`patches/ambient_occlusion_patch.cpp`](../../patches/ambient_occlusion_patch.cpp), [`shaders/sim_receiver_ids.h`](../../shaders/sim_receiver_ids.h) |

## The problem

Ambient Occlusion is computed from the screen depth only, so it cannot tell a Sim from the room around it. The shade
that looks right in a corner of a room also lands on cheeks, lips, the inside of the mouth and between hair strands,
where it reads as dirt. Lowering the global strength to fix Sims also weakens the shade everywhere else.

## How Apex Radiance solves it

Apex Radiance identifies Sims by their game material and builds a per-pixel mask of where a Sim is the visible surface.
The Ambient Occlusion composite then uses the Sim settings on those pixels and the scene settings everywhere else.

1. **Recognise Sim materials.** 916 pixel-shader fingerprints belong exclusively to the game's Sim techniques (SimSkin,
   SimHair, SimEyes, SimEyelashes, SimpleSim, SimRobot); 48 of them are hair. Shaders shared with non-Sim techniques
   are excluded. No skin colour or animation heuristic is used.
2. **Replay Sim draws into a mask.** Before the game draws a recognised Sim material, Apex Radiance replays the same
   draw into a small target that stores the nearest body depth and the nearest hair depth for each pixel.
3. **Handle transparency.** Blended hair and blended body layers replay into a second target that keeps the material's
   alpha as coverage, so semi-transparent strands are adjusted in proportion to how much they cover.
4. **Composite.** A pixel uses the Sim or hair settings only when the mask depth matches the final image depth. If it
   does not match, or matches both classes, the scene shade is kept.

Custom content that uses the standard Sim materials is covered automatically. Custom shaders keep the scene shade.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Sim Occlusion (card switch) | `simControls` | bool | off | | Enables the Sim controls. Off hides them and releases the mask resources |
| Sim intensity | `simStrength` | float | 47% | 0 to 100% | Shade on body, face and clothes. 0% removes it, 100% keeps the scene shade |
| Hair intensity | `hairStrength` | float | 38% | 0 to 100% | Shade on recognised hair, independent of the body |
| Maximum darkening | `simMaxShade` | float | 47% | 0 to 100% | Caps the shade added to Sims and hair, before the light protection of the composite |
| Advanced > Transparent hair | `transparentHair` | bool | on | | Also adjusts recognised transparent hair strands. Blended body layers always follow Sim intensity |
| Advanced > Show Sim coverage | (not saved) | bool | off | | Preview: blue = Sim controls, green = hair controls, black = scene shade |

Settings apply immediately and are included in profiles. The preview is never saved.

## Compatibility and interactions

- **Ambient Occlusion** must be on; Sim Occlusion only changes how its shade is applied.
- **Hardware:** needs G32R32F render targets with blending (`D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING`). Without it, or
  if a target cannot be created, Sims keep the scene shade and the card shows a warning.
- **Custom content:** standard Sim materials are covered without a file list. Custom shaders are not guessed.
- **F7 Light Probe:** its visibility-query copies of a draw are colour-disabled and never enter the mask.

## Limitations

- Overlapping transparent Sim layers use the last accepted layer rather than an exact sum of coverage.
- The colour of a transparent Sim layer already blended with the background cannot be separated. The result is an
  approximation.
- Where a Sim surface and a non-Sim surface have the same depth, the scene shade is kept.
- Opaque Sim draws with a depth comparison other than LESS or LESSEQUAL keep the scene shade.
- Transparency equations other than SRCALPHA/INVSRCALPHA with ADD are not adjusted.

## Technical reference

**Opaque mask.** Opaque and alpha-tested receivers replay into a G32R32F target: R = nearest body depth, G = nearest
hair depth, both cleared to 1. MIN blending keeps the nearest depth within a draw and across draws, regardless of
triangle order. The replay keeps the original geometry, alpha test, source alpha, depth comparison and scissor
rectangle, and never writes the shared scene depth/stencil. Only LESS and LESSEQUAL are accepted, because MIN cannot
represent other comparisons.

**Blended mask.** Recognised blended body and hair materials (SRCALPHA/INVSRCALPHA, ADD) replay into a second G32R32F
target with source alpha as coverage. The composite rejects receivers behind the final foreground depth and interpolates
the adjustment by coverage, on top of the opaque receiver underneath. The second target is allocated whenever the card
is active, because blended body layers use it even with Transparent hair off.

**Composite inputs.** The composite accepts a class when its mask depth matches the final depth within 2.4e-7. Sim
intensity, hair intensity and maximum darkening are passed as constants; the cap applies to the added visibility loss
before the multi-bounce and light-protection terms described in [Ambient Occlusion](ambient-occlusion.md).

**State handling.** Replays save and restore SRCBLEND, DESTBLEND, BLENDOP, the scissor enable flag and the scissor
rectangle. `SetRenderTarget` resets the rectangle, so it is reapplied after binding the mask and again after restoring
the original target. Colour-disabled (alpha-only or query) draws are skipped.

**Resources.** Two G32R32F screen targets, 8 bytes per pixel each (126.6 MiB at 4K for both). They are created on
demand and released when the card is off, or when every Sim value is 100% with the preview off. The blending capability
check runs once per resource lifetime. Shader references are retained against pointer reuse, and caches have fixed
limits.

**Cost.** No additional full-screen pass or GTAO march is added. The replays add CPU and GPU work that the Ambient
Occlusion timer does not include; it has not been measured in game.

**Fingerprints.** [`shaders/sim_receiver_ids.h`](../../shaders/sim_receiver_ids.h) stores size and hash identifiers
only, generated from technique ownership in `Shaders_Win32.precomp` by `tools/ao_receiver_test/generate_ids.cjs`. No
game bytecode is shipped. Unrecognised variants fail closed.

## Rejected approaches

- Skin-colour or skinning heuristics: unreliable and would catch non-Sim objects.
- Widening LESS to LESSEQUAL in replays: marks coplanar Sim fragments that the game rejects.
- Single-depth mask without MIN blending: kept the farther triangle when a draw overlapped itself.
- Screen-space mask dilation or blur: spreads Sim settings onto the background.

Details in [history](../history/sim-occlusion.md).

## See also

- [Ambient Occlusion](ambient-occlusion.md)
- [Validation](../validation/sim-occlusion.md)
- [History](../history/sim-occlusion.md)
