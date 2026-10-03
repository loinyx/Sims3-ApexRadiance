# Ambient Occlusion

> Local development, not released: Sim Occlusion card, independent hair strength and Distance controls. Native D3D9 shader and synthetic rendering checks passed; gameplay coverage and cost remain unverified. Published 2.5.6 does not include these controls.

> Soft shade where things meet (under furniture, in corners, where walls meet the floor, around houses and trees),
> computed from the scene depth right after the game finishes the 3D scene and before bloom and the UI. GTAO at full
> resolution, deterministic (a still camera never changes it), with a composite that keeps lamp-lit and bright surfaces
> and their colour. Brought back on the user's request on 30/09/2026 after the earlier AO lines were removed (history in
> [../removed-features.md](../removed-features.md)).
> Status: released in 2.1.0 (user in game: "ficou incrível"); off by default. Since 30/09 afternoon no longer flagged
> experimental: the page shows a performance note instead. Patch name `AmbientOcclusion`, menu page Image > Ambient
> Occlusion, settings in `[patches.AmbientOcclusion]`.

## Purpose

The user's verdict on the last AO (28/09): dirty / too dark, weak / barely visible, and dots or bands, both indoors and
outdoors. The study of 30/09 (lab `gtaolab.cpp`, session scratchpad `ao\`) compared the shipped HBAO with a new GTAO on
six saved frames of the game (depth dumps `Documents\...\S3SS\Profundidade\profundidade_N_3840x2160.f32` + colour):

- **Clean:** bit-identical with a still camera; the shift metric (image moved 1-4 px) as good as the old HBAO.
- **Stronger at contact, less spread:** a flat floor or wall gets exactly no shade (the visibility is a ratio to the
  unoccluded arc), so the shaded share of an indoor frame fell from 64% to 46%, all of it where things meet.
- **Composite (d):** a dead zone drops faint shade, a multi-bounce term keeps bright surfaces bright and coloured (no
  grey film), lamp-lit pixels keep part of their light. Bright pixels lose 3.6x less light than with the old composite.

## User-facing settings

| UI label | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| (card switch) | `enabled` | bool | true | | main effect; enabled by default for new configurations |
| Strength | `forca` | float | 1.68 | 0 - 2 | 168%; multiplies contact and large strengths; 0 = no AO passes |
| Sim intensity (Sim Occlusion card) | `simStrength` | float | 0.47 | 0 - 1 | default 47%; 0 removes received AO on supported Sim pixels, 1 keeps original shading and skips mask work |
| Sim Occlusion (card switch) | `simControls` | bool | false | | experimental and off by default; depends on Ambient Occlusion; off hides the card controls and releases receiver resources |
| Hair intensity | `hairStrength` | float | 0.38 | 0 - 1 | exclusive SimHair shaders; default 38%, independent from body strength |
| Maximum darkening | `simMaxShade` | float | 0.47 | 0 - 1 | default 47%; caps added visibility loss before multi-bounce/light protection; not final pixel luminance |
| Advanced > Transparent hair | `transparentHair` | bool | true | | only recognized non-depth-writing source-alpha hair with standard additive blend equation |
| Advanced > Show Sim coverage | (not saved) | bool | false | | blue: body, green: hair, black: unknown; requires AO active |
| Distance (visible) | `distance` | float | 351 | 25 - 1000 m | view-depth cutoff, fades from 37.5% of this distance; not mesh LOD; map-view behavior is unchanged |
| Advanced > Reach | `alcance` | float | 1.3 | 0.5 - 2 | default 130%; scales the three radii |
| Advanced > Keep lamp light | `protegerLuz` | float | 0.38 | 0 - 1 | default 38%; share of light kept by bright pixels (luma 0.35 -> 0.75) |
| Quality (visible) | `qualidade` | enum | 2 (High) | stored 0 Low, 1 Medium, 2 High, 3 Ultra, 4 Very Low | 4 / 6 / 8 / 12 / 2 slices; the menu shows Very Low .. Ultra (`kQualityShown`), stored indices kept from 2.1.0 |
| Advanced > Show the shade alone | (not saved) | bool | false | | the shade in grey (also on the Developer page) |
| Also in map view (visible) | `noMapa` | bool | true | | with the map view open: contact 4 m, large 15 m, no fade (lab on 3 map captures 30/09: view 950-1800 m, one depth step 5-20 cm as near is ~1 m there; AO shift 0.35-0.48 levels) |
| (none) | `revisao` | int | 9 | | revision marker: older configs/profiles keep every stored value and fill only missing keys from the current defaults |

All live (read every frame). The old combined build's keys (`intensidade`, `raioM`, `visualizar`) are not read.

### Sim receivers and distance

The main controls stay in Image > Ambient Occlusion, directly below Strength; Quality and Also in map view follow them. Reach and lamp-light protection remain Advanced. There is no new page or independent Sim quality preset.

The mask uses 916 exclusive material PS identifiers extracted from Steam shader-package TECH/PASS ownership: SimSkin, SimHair, SimEyes, SimEyelashes, SimpleSim and SimRobot. Shaders shared with non-Sim techniques are excluded. No skin-colour or generic skinning heuristic is used, and no game bytecode is shipped.

The dedicated **Sim Occlusion** card follows the scene AO card and uses Lucide's User Round icon, distinct from the scene AO card's Contrast icon. Its Experimental badge sits beside the card switch. The scene AO feature starts enabled for new configurations, using Strength 168%, Distance 351 m, High quality, Reach 130% and Keep lamp light 38%. The Sim card itself starts off; when the user enables it, Sim intensity and maximum darkening start at 47%, hair intensity at 38%, and transparent hair is on. Turning the Sim switch off hides its controls and releases receiver resources. Page and card reset buttons were removed at the user's request; individual control defaults and Settings' global reset remain.

Hair is identified by 48 fingerprints owned exclusively by SimHair within the 916 exclusive Sim material fingerprints. Shared or custom shaders are not guessed from skin colour or animation. CC using these standard game materials participates without a filename list; this does not guarantee every CC hair is covered.

Opaque and alpha-tested depth-writing receivers replay into G32R32F: signed device depth (negative for hair), coverage 1. Alpha rejection, geometry, depth testing and source alpha are preserved; depth/stencil writes are disabled. The composite requires final-depth agreement within 2.4e-7. No mask blur/dilation or extra GTAO march is used. With the Sim card enabled, hair strength defaults to 38% and body intensity to 47%. Maximum darkening caps added AO visibility loss before the existing colour/light protection, affecting only recognized Sims.

Recognized non-depth-writing hair using SRCALPHA/INVSRCALPHA and ADD also replays into a separate G32R32F target, retaining source alpha as coverage. The composite rejects hair behind final foreground depth and interpolates suppression by coverage. It preserves the underlying opaque receiver adjustment. Other transparency equations/materials remain unchanged. This is a screen-composite approximation: it cannot separate the already blended hair/background colours, and overlapping translucent layers use the last accepted replay rather than exact accumulated coverage. Validate these cases on real CC before release.

Both targets are created on demand, reset safely and released when customization is off or all strengths/caps are 100% with preview off. Transparent hair uses a second target only when enabled. Each target costs 8 bytes per screen pixel; replays add GPU/CPU work. The AO-pass timer excludes replay costs. Device/allocation failure retains original shading with an inline warning. Configs/profiles include all controls; preview is not saved. Revision 9 preserves every explicitly saved AO value and fills only settings missing from older files with the current defaults. Shader references are retained against pointer reuse; caches have fixed limits.

Distance changes the existing view-depth fade, not pyramid sampling LOD or Sim mesh detail. The default retains 150-400 m. Map view keeps its dedicated radii and fade. A shorter cutoff can avoid distant GTAO marching, but still runs copy, pyramid, blur and composite passes; no overall FPS gain is established.

Validation: tools/ao_receiver_test reads the shader package without writing bytecode. Of 1912 distinct pairs, native D3D9 accepted 1796 opaque copies; 116 were safely refused unchanged. The extended harness also checks classified hair copies and coverage-weighted transparent-hair compositing, independent body/hair strengths, maximum darkening, disabled customization and foreground rejection (4231 checks, zero failures). The harness checks SM2/SM3 alpha rejection and the actual AO composite at 100%, 0%, 50%, and foreground-depth rejection. Gameplay checks remain: moving Sims, indoor/outdoor lighting, CAS, distance, custom clothing/hair, occult/robot materials, hidden Sims, save/world transitions, resolution changes and DXVK. Shader acceptance is not complete gameplay coverage.

## How it works (`patches/ambient_occlusion_patch.cpp`)

Runs first in the post-scene chain (`PostScene::kAmbientOcclusion = 10`, before Edge Smoothing 20 and Depth Blur 30),
only when the bound depth-stencil is `DepthShare::Surface()` (the main scene). `Install` asks for the INTZ depth swap
(`DepthShare::Request`) and for the camera (`PostScene::WantCamera`).

Passes per frame (W x H = the back buffer):
1. `StretchRect` of the back buffer into a colour copy.
2. `LinearizePS`: device depth -> 1/z in 1/m (`(A - d) / (near A)`, sky 0) into level 0 of a 9-level R32F pyramid padded
   to a multiple of 256.
3. `DownPS` x 8: each level = the 2x2 average of 1/z (sky left out), rendered into a one-level target and copied in.
4. `GtaoPS` (full resolution, G16R16F out: R = visibility, G = 1/z for the blur). Per pixel: normal from the neighbour with
   the smaller depth step per axis; SLICES slices at angle `(s + b1) pi / SLICES`, 4 geometric steps per side from 2 px
   (at 4K, scaled with the height) to the large radius (at most 30% of the height), each step reads the pyramid
   bilinearly within the nearest level to its spacing minus 2 (sampler s2 MIPFILTER POINT; trilinear until 30/09); step offset `b2` plus a golden-ratio phase per half-slice; `b1`, `b2`
   from a 4x4 Bayer. Two horizons per side from the same samples: contact (0.6 m, strength 1.2) and large (2.0 m near /
   2.5 m far, strength 0.5 near / 0.8 far, only what the contact one does not cover; near -> far between 20 and 40 m).
   XeGTAO falloff (full to 38.5% of R, 0 at R) on a distance whose depth part counts 1.3x. Cosine-weighted arcs with the
   projected normal, exact `acos` (the fast fit biased the result by 0.5%, see Verified). Isolated pixels (both
   neighbours on an axis more than 1% away in depth: leaf edges, thin rails) fade out; the shade fades out 150-400 m.
   Samples off the screen count as sky.
5. `BlurPS` x 4: depth-aware (3% of z) box 0.5 1 1 1 0.5 H and V (contains each Bayer offset once), then tent 1 2 3 2 1.
6. `CompositePS` over the copy, colour write RGB: `v = 1 - sat((1 - ao - 0.05) / 0.95)`, per channel
   `m = MultiBounce(v, min(0.9, colour^2.2))` (Jimenez 2016), `m = lerp(m, 1, sat((luma - 0.35) * 2.5) * keep)`.

Camera: near, A and tanX / tanY from `PostScene` (vertex-constant votes over the first 24 scene draws of each frame, only
while an effect asked for them: the combined build's code, see engine/camera-and-map-view.md); fallback near 0.25,
A 1.00008, `tanY = 1/4.293`.

Resources: pyramid R32F (4K: 3840x2304, 9 levels) + 8 one-level targets, two G16R16F screen targets, a colour copy.
Needs R32F filtering and G16R16F render targets (checked at start), and the game's own Edge Smoothing off (paused while
the back buffer is multisampled). Released at `preReset`, rebuilt at the next frame; rebuilt when the back buffer size
changes without a Reset. GPU cost from timestamp queries (card chip, Developer line). fxc slots: GtaoPS ~505, BlurPS 73,
CompositePS 35, DownPS 25, LinearizePS 8.

## Verified

The Sim receiver mask also accepts recognized alpha-blended body materials using SRCALPHA/INVSRCALPHA ADD, with signed depth and original alpha coverage. The transparent-hair option only gates hair draws; body coverage remains active with Sim controls. Depth-writing LESS materials replay with LESSEQUAL to accept the depth they just wrote, restoring the original state afterwards. Native regression checks cover equal-depth rejection, blended body strength/opacity and foreground rejection. This addresses two coverage gaps; the reported mouth artifact still needs an in-game before/after comparison. No screen-space mask dilation or global AO smoothing is added. Blended coverage remains a last-layer approximation.

- **The GPU shader against the CPU lab** (`scratchpad\aonew\gpucheck.cpp`: the patch's HLSL on a D3D9 HAL device, the
  same saved depth and colour, the lab's Gtao + Denoise(1) + Composite(3)): scenes 1, 2, 3, 6: raw AO mean difference
  0.08-0.11 of 255 levels, filtered 0.26-0.28 (G16R16F rounding), composite 0.06-0.09 levels (max 2-4); a second run is
  bit-identical. With the fast acos fit the raw AO was 1.3 levels off (68% of pixels > 1 level): exact `acos` costs 7
  slots.
- Not verified yet: the look in game, the GPU cost at 4K, foliage in the wind, alpha-blended surfaces (glass, particles
  do not write depth: the shade of what is behind shows through), water, thin railings, daytime scenes (the lab frames
  are night scenes, where most pixels are dark or lamp-lit and the composite protects them: the effect is subtle there).

## Plan (stages)

1. **This build:** GTAO + composite (d), experimental, Strength / Reach / Keep lamp light / Quality.
2. **Ambient share:** the Apex-patched shaders write how much of each pixel's light is ambient (a second render target),
   so the AO darkens only ambient light and can be stronger without dirtying lamp-lit surfaces (interior walls, floors,
   furniture first, then terrain, exterior walls, Sims, foliage). The back-buffer alpha is not free (bloom uses it).

## Interactions

- Compare with the game (shortcut) turns it off with Night Lighting, Depth Blur and Edge Smoothing.
- Depth Blur and Reflections read the same INTZ depth; the swap runs while any of them needs it.
- Picture's scene copy is taken after the post-scene effects, so Color filters apply on top of the shade.
- If hiding the game's UI removes the usual depth-off draw that marks the scene boundary, `PostScene` runs the effects
  at `endSceneBeforeOverlay` after at least 20 depth-tested backbuffer draws. This fallback keeps the scene effects
  available with the game UI hidden; frames with an earlier depth-off draw retain the existing trigger behavior.

## Pitfalls and failed approaches

See [../removed-features.md](../removed-features.md) (Ambient Occlusion): half resolution (the root of every "micro
dots" report), per-frame noise with temporal accumulation (twinkling on leaves), radii in near units (shade breathing with
zoom), HBAO with a plain multiply (dirty and grey, weak once toned down). Do not bring any of those back.

## Cost and quality levels (30/09, `scratchpad\aonew\aotime.cpp`)

The in-game shaders on this machine's GPU (RTX 4070 Ti SUPER, native D3D9, 3840x2160, scenes 1 / 2 / 6 of the lab; in game
DXVK: compare the levels, not the absolute ms). "Image" = mean difference of the final picture from the 2.1.0 High, in
levels of 255; "shift" = AO change when the image moves 1 px (the stability metric of the lab).

| Level | Slices | Total ms | Image vs 2.1.0 High | Shift |
|---|---|---|---|---|
| 2.1.0 High (trilinear) | 8 | 3.9 - 4.3 | 0 | 0.28 - 0.51 |
| Very Low | 2 | 1.3 | 0.10 - 0.22 | 0.57 - 1.07 |
| Low | 4 | 1.8 - 2.0 | 0.08 - 0.17 | 0.39 - 0.73 |
| Medium | 6 | 2.5 - 2.65 | 0.07 - 0.13 | 0.33 - 0.59 |
| High (default; 2.1.0 benchmark baseline) | 8 | 3.0 - 3.2 | 0.03 - 0.05 | 0.28 - 0.50 |
| Ultra | 12 | 4.1 - 4.5 | 0.06 - 0.11 | 0.23 - 0.41 |

Fixed part about 0.55 ms (scene copy + pyramid 0.26, blur 0.2, composite 0.09). Tried and dropped: fewer steps (3: the
look moves 3-4 levels), box-only blur (saves 0.1 ms, less stable), a coarser (mip 1) or finer (mip 3) read level (no
gain), the fast acos (no gain), an R16F copy of the pyramid for the march (no gain). The cost was the trilinear R32F
filtering (two bilinear lookups per tap, 64 taps per pixel), not the arithmetic.
