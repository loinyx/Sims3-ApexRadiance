# 12. Prior art for the failed and open lighting / GI points

Online prior-art lane for [the real-time GI idea](../idea.md). For every attempt that failed or stayed open in
`00`-`07`, this page names published work that solved the same problem and says how to apply it in Apex Radiance
(D3D9, shader model 3, CPU-bound render thread, every D3D call crosses the RTX Remix bridge). Web and local reading
only, 2026-11-17; nothing was run in the game and no code was written.

Tags: **verified** = the link answered and the stated fact was read in the source, a search extract of it, or in
`00`-`07`; **reported** = a secondary page or a talk known only by its summary; **memory** = recalled from the talk or
paper, link checked but the detail not re-read today; **inferred** = our reading, to be proven by a prototype.
Estimates are wall-clock time for an agent build; game runs are listed separately.

It complements [`07`](07-remix-and-prior-art.md) (bridge, Remix, injector mods) and does not repeat it.

## 1. Ranking

Ordered by value for the cost. "Lane" is the research page the item upgrades. This table covers the D3D9 shader
model 3 options; §7.5 ranks them together with the options a patched 64-bit bridge server opens (compute and
hardware ray queries).

| # | Take from prior art | Solves | Lane | Build | Risk |
|---|---|---|---|---|---|
| 1 | **Frame-constant block + world-space lamp index texture** (Just Cause 2, clustered shading) | Failure 1: per-draw lamp picks and constant uploads | `02` §2, `00` §7 | 4-8 h + 2 runs | Medium: dependent reads in ps_3_0 only |
| 2 | **Per-lamp linear layers** (Activision UberBake) on **low-resolution indirect** (Enlighten) with a **fixed per-frame update budget** (Lumen surface cache) | Open 3a: re-bake cost | `04` §1.5, S1 | 3-6 h | Low |
| 3 | **Wall-plan distance field for visibility** (jump flooding, flatland ray march; DDGI-style visibility weight) | Open 3b: leaks at doors and windows; replaces opening frusta and imperfect shadow maps | `04` S3, S5 | 3-5 h | Medium: half-height walls, stairwells |
| 4 | **Feedback bounce + per-room ambient term** (Enlighten, Lumen, progressive radiosity) | Open 3c: second bounce; part of 3e | `04` S3 | 1-2 h | Low |
| 5 | **Directional maps as ambient-cube faces; rig filled from SH with dominant-light extraction, cached per model** (Source, Halo 3) | Open 3d: Sims and objects; part of failure 1 | `02` §4.2 route A, `04` S4 | 4-8 h + 1-2 runs | Medium: read-back avoidance |
| 6 | **Statistics-matched look calibration** (Reinhard colour transfer; Unity indirect intensity / albedo boost; GTAO multi-bounce) | Open 3e: the TS3 look | `04` S6, `06` §3.4 | 1-2 h + screenshots | Low |
| 7 | **Analytic capsule occlusion for Sims** (The Last of Us) | Sim grounding without per-frame Sim shadow maps | `03` §4, AO | 2-4 h + 1 run | Low |
| 8 | **Hemicube raster bake as the offline reference** (The Witness, `lightmapper.h`) | A ground truth for leak, bounce and look tests | `04` S0, `06` §3 | 2-3 h | Low |
| 9 | **2D Radiance Cascades per story** | A possible replacement for items 3 + 4 on one story | experiment | 4-8 h | High: no floor / ceiling bounce in flatland |
| 10 | **Bridge `commandBatchingEnabled`** | Failure 1, bridge side: one semaphore per frame, not per command | `07` §1 | 15 min + 1 run | Unknown: default is off for a reason not documented |
| 11 | DDGI octahedral depth probes rasterised from the proxy | Leaks where the 2D plan is not enough (stairwells, split levels) | `04` §1.3 | 4-8 h | Medium: draws per probe on build change |
| - | Not adopted: LPV, voxel / SDFGI, SSGI as a base, screen-space light index or tiled deferred, RTXGI and Enlighten SDKs | See §3 | | | |

## 2. Failure 1: per-draw lighting fixes cost 1.8-3.4 ms a frame, "no cut found"

What the profile says (**verified**, `00` §7): the cost is Apex's own work per draw (lamp picks 1-3 µs, patched
constants, then the upload across the bridge) times 1,800-2,500 draws. Lamp picks already reuse 95%. The lighting
result cannot change, so the only cut left is to stop doing the work per draw at all. Published engines did exactly
that in the D3D9 generation.

### 2.1 Lamps looked up by world position in the shader (Just Cause 2; clustered shading)

- **Prior art.** Just Cause 2 (forward renderer, D3D9/10-class consoles) used "world-space XZ-tiled light-indexing":
  a 128 × 128 RGBA8 **light index texture**, 4 lights per 4 m × 4 m tile, with the light data in constant registers
  on PC / Xbox 360 or in a **1D texture** on PS3 (**reported**: Persson, "Practical Clustered Shading" slides,
  [humus.name](https://www.humus.name/Articles/PracticalClusteredShading.pdf); book chapter: Persson, "Making it
  Large, Beautiful, Fast, and Consistent: Lessons Learned Developing Just Cause 2", GPU Pro, 2010, pp. 571-596;
  talk record: [SIGGRAPH history](https://history.siggraph.org/learning/creating-vast-game-worlds-experiences-from-avalanche-studios-by-persson/)).
  Clustered shading generalises it to 3D cells (Olsson, Billeter, Assarsson, HPG 2012,
  [preprint](https://www.cse.chalmers.se/~uffe/clustered_shading_preprint.pdf)); Doom 2016 ships it for lights,
  decals and probes in a forward renderer (Sousa and Geffroy, "The devil is in the details",
  [SIGGRAPH 2016](https://advances.realtimerendering.com/s2016/Siggraph2016_idTech6.pdf)). **memory** for the last two.
- **Why it fits TS3 better than it fit those games (inferred).** The lot is already a 1 m tile grid, 64 × 64 tiles by
  4-5 stories, and Apex already knows each lamp's tile and room. A cell is a tile (or 2 × 2 tiles) per story: the index
  texture is at most 64 × 320 texels. No screen depth is needed, which is what killed the screen-space light buffer
  (`02` §3.1).
- **Shape in Apex.**
  1. Index cells and the lamp table (position, colour, cone, shadow-atlas rectangle) go into rows of the **one float
     atlas** that `02` §3.4 already plans for shadow faces and probe slices. fp16 holds integers to 2048 exactly, so
     indices need no second texture: the feature still costs **one sampler**.
  2. The patched `ps_3_0` copy computes the cell from the world position it already has in TEXCOORD8
     (`PatchObjectLampVs`), reads 4 indices, then 2 texels per lamp with `texldl`. About 12-16 slots of overhead plus
     the lamp block of `02` §2, well inside the 512-slot limit.
  3. Apex rewrites the cells only when a lamp or wall changes: a handful of draws, never per frame.
  4. Per draw nothing is picked and nothing is uploaded. What remains is the `SetTexture` swap, measured at
     0.064 ms a frame for 3,728 calls (`00` §7).
- **Ceiling of the gain (inferred from `00` §7, far pan):** OutdoorObjectGroundLight 0.37-0.75 ms,
  FenceStairGroundLight 0.55-0.69 ms and most of "several fixes" 0.70-0.73 ms use per-draw lamp data: up to about
  1.5-2 ms a frame. RoomMapSmooth (0.54-0.63 ms) and RoomsAtNightFurniture (0.23-0.38 ms) go away through the
  texel-space carrier of `02` §3.2 (the smoothing and the furniture light are baked into Apex's map copies), not
  through this item.
- **Limits.** `ps_2_0` families (foliage, rugs, Sim eyes) cannot take dependent reads: they stay on the rig fill of
  §4.4. More than 4 lamps in one cell needs a second index texel or a priority cut by distance; the game's own rig
  already cuts to 3 + 4.
- **Quality.** Better than today: the lamp set changes per pixel at cell borders (blend the falloff to zero at the
  lamp's radius so the switch is invisible, as clustered shading requires), instead of per model centre.
- **License.** Technique only; slides and papers are public.

### 2.2 A frame-constant block: D3D9 shader constants are device state

- **Fact (verified, API):** `SetPixelShaderConstantF` / `SetVertexShaderConstantF` write device-wide registers that
  persist until overwritten; they are not per shader
  ([Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-setpixelshaderconstantf)).
  D3D10's constant buffers formalised the per-frame / per-object split that D3D9 engines got by reserving register
  ranges.
- **Fact (verified, `02` §1.4):** TS3's pixel shaders stop at `c24`; 199 registers are free. Vertex shaders reach
  `c227`, leaving 28.
- **Apply.** Reserve a high pixel range (for example `c200`-`c223`) as Apex's frame block: atlas scale and offsets,
  lot origin, cell size, look constants (§5.5). Write it **once per frame with one ranged call**; every patched copy
  reads it. Anything that today travels per draw but is the same for the whole frame moves here.
- **To verify before relying on it:** that no game path writes above `c24` (a `SetPixelShaderConstantF` range
  histogram from the existing D3D hooks, one run), and that the bridge's `client.dropRedundantState` does not drop the
  write after a device reset.
- **Related, same idea:** per-instance data in a vertex stream instead of constants
  ([Efficiently Drawing Multiple Instances of Geometry, D3D9](https://learn.microsoft.com/en-us/windows/win32/direct3d9/efficiently-drawing-multiple-instances-of-geometry);
  Carucci, "Inside Geometry Instancing",
  [GPU Gems 2 ch. 3](https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-3-inside-geometry-instancing)).
  It applies to Apex's own passes (texel points, probe updates), not to the game's draws.

### 2.3 Light the model once, not the draw (Source engine)

- **Prior art.** Source lights every model from an **ambient cube** plus a few picked local lights, computed on the
  CPU per entity and kept until the entity or the lighting changes
  ([Ambient cube](https://developer.valvesoftware.com/wiki/Ambient_cube); Mitchell, McTaggart, Green, "Shading in
  Valve's Source Engine", [SIGGRAPH 2006 course chapter](https://advances.realtimerendering.com/s2006/Chapter7-Shading_in_Valve%27s_Source_Engine.pdf),
  [slides](https://advances.realtimerendering.com/s2006/Mitchell-ShadingInValvesSourceEngine.pdf)). **verified** for
  the cube and the per-model lights; the caching detail is **memory**.
- **Apply.** TS3's rig is the same structure (sun + 3 lamps + 4 vertex lights, evaluated at the model centre,
  `02` §4.1). Write Apex's values **into the rig when it is gathered or when a lamp changes**, not at each draw's
  constant upload. This is route A of `02` §4.2; the prior art adds the rule "cache per model, invalidate by lamp or
  move". It covers every `ps_2_0` family that §2.1 cannot.

### 2.4 Bridge side: one semaphore per frame

`commandBatchingEnabled` in the bridge "toggles between waiting on and triggering the command queue semaphore for each
command separately when batching is off compared to waiting for it only once per frame"; its comment says "turned on
by default", the code default is `false` (**verified**,
[`global_options.h`](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/bridge/src/util/config/global_options.h)
lines 412-416 on `main`; check Apex's pinned commit). One `bridge.conf` line and one pan run tell whether the
per-call cost drops. It is not Apex code, so it ranks low, but it is the cheapest test on this page.

### 2.5 Looked at and not taken for failure 1

| Prior art | Why not |
|---|---|
| Light-indexed deferred rendering (Trebilco 2007, [code](https://github.com/dtrebilco/lightindexed-deferredrender)) | Built for DX9-class forward renderers, but the index buffer is in screen space and needs a depth pre-pass: the same wall as `02` §3.1 |
| Tiled deferred on SPUs, Battlefield 3 (Coffin, GDC 2011, [slides](https://www.slideshare.net/DICEStudio/spubased-deferred-shading-in-battlefield-3-for-playstation-3)) | Needs a G-buffer; TS3 is forward and its tone curve and bloom mask live in the forward shader (`02` §3.4) |
| Doom Eternal's fully forward clustered path | Same idea as §2.1 with compute-built clusters; the tile grid makes the build trivial here |

## 3. Failure 2: the rejected GI techniques, and what prior art changes

| Rejected | Reason in `02`/`04` | What prior art offers | Verdict |
|---|---|---|---|
| **LPV** | Leaks through thin walls | Cascaded LPV's geometry volume reduces, does not remove, the leak (`04` [3]). Godot's own guidance for its voxel and SDF GI is still "walls at least one cell thick" ([VoxelGI](https://docs.godotengine.org/en/stable/tutorials/3d/global_illumination/using_voxel_gi.html), [GI comparison](https://docs.godotengine.org/en/stable/tutorials/3d/global_illumination/introduction_to_global_illumination.html), **verified** by search extract). No engine solved zero-thickness walls in a grid by filtering | Stays rejected. The tile-edge wall mask of `04` §1.2 is the only real fix, and §4.2 below gets the same result without a volume |
| **SSGI** | No albedo, off-screen light missing, breathing | Lumen has the same three problems and solves them by using screen traces only for short range and **falling back to texel-space lighting (the surface cache)** for everything else (Wright, Narkowicz, Kelly, [SIGGRAPH 2022](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Lumen-Wright%20et%20al.pdf), **verified** by extract: "surface cache: texture space", multi-bounce "through surface cache feedback"). GTAO's multi-bounce fit brightens AO by albedo with no colour taps (Jimenez et al., [SIGGRAPH 2016](https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf), **memory**) | Same split as Lumen: texel space is the base, screen space only a contact term. Take the multi-bounce AO fit (§5.5); no SSGI pass |
| **Voxel GI** | Voxelisation costs one scene redraw per slice in SM3 | Godot VoxelGI voxelises on the CPU at bake time; SDFGI needs compute. Both leak on thin walls by their own docs | Stays rejected |
| **Screen-space light buffer** | Needs this frame's depth before the draws | Clustered and world-tiled forward shading need no depth at all | Replaced by §2.1 |
| **Patching ~1,200 shaders by family** | 60-96% acceptance per family, never 100% | 3DMigoto's `ShaderRegex` patches whole classes of shaders by one pattern over the disassembly ([repo](https://github.com/bo3b/3Dmigoto), GPL-3: approach only; **reported**, syntax not read). The engines above avoid the problem by moving light into data every shader already reads | Keep the texel-space carrier (no patch for room-lit families). For the rig-lit rest, key the matcher on **constant-table names** (`LightColors`, `LightDirections`) instead of per-family instruction patterns, so one matcher serves every family that binds them (**inferred**; check how far `PatchObjectLampPs` already does this) |

## 4. Open problems of the chosen texel-space approach

### 4.1 Incremental re-bake cost on the GPU

`04` §1.5 estimates 0.2-0.6 M texels × 256 VPLs = 1-3 ms per full re-bake. Three production systems show how to make
most changes cost far less than a re-bake.

**(a) Indirect light at low resolution, direct at high (Enlighten).** Enlighten computes radiosity on a coarse
"target" parameterisation (about a texel a metre) and separates it from direct light; the detail comes from direct
light and normal maps (Martin and Einarsson, "A Real-Time Radiosity Architecture for Video Games",
[SIGGRAPH 2010](https://www.advances.realtimerendering.com/s2010/Martin-Einarsson-RadiosityArchitecture(SIGGRAPH%202010%20Advanced%20RealTime%20Rendering%20Course).pdf);
**verified** for the direct / indirect split, the resolution figure is **memory**).
*Apply:* gather indirect at **1 texel per tile** (the game's own lowest room-map density) into a small indirect map,
and add it bilinearly when composing the 4-16 texel/m direct map. That is 16-256× fewer gather texels than `04`
assumed: a full re-bake of a 64 × 64 lot with 5 stories is about 20-40 k texels × 256 VPLs = 5-10 M evaluations,
well under 0.1 ms. Bilinear upsampling must not cross walls: use the wall mask or §4.2's visibility at the 4 corners.

**(b) Light is linear: keep one layer per lamp (UberBake).** Activision's baker precomputes "lighting changes due to
individual lights being enabled and disabled, and doors opening and closing" and applies them at run time with
"little to no overhead" (Seyb, Sloan, Silvennoinen, Iwanicki, Jarosz, "The Design and Evolution of the UberBake Light
Baking System", SIGGRAPH 2020, [abstract](https://research.activision.com/publications/2020-09/the-design-and-evolution-of-the-uberbake-light-baking-system),
**verified**).
*Apply:* store each lamp's **unit-colour indirect contribution** as its own small layer (at 1 texel per tile, 16
lamps × 64 × 320 texels of fp16 RGBA is 2.5 MB in the bridge process). The lot's indirect map is then
`sum(on_i × colour_i × layer_i)`: one compose pass. Switching a lamp, dimming it or recolouring it, which is what
happens every evening and with every Sim interaction, costs **no gather at all**. Only a moved lamp (one layer) or a
build-mode edit (all layers of that room) re-gathers. Doors: two variants of the affected layers, or accept the
1-16 frame update.

**(c) A fixed budget per frame, most visible first (Lumen, Unity, Frostbite).** Lumen's surface cache updates a fixed
share of its texels per frame and tolerates stale ones; Unity's progressive lightmapper and Frostbite's GPU baker
order work by what the camera sees (Lumen slides above;
[Unity progressive lightmapper](https://docs.unity3d.com/Manual/progressive-lightmapper.html),
[GPU backend](https://docs.unity3d.com/Manual/GPUProgressiveLightmapper.html); O'Donnell, "Precomputed Global
Illumination in Frostbite", [GDC 2018](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/gdc2018-precomputedgiobalilluminationinfrostbite.pdf);
**memory** for the scheduling details).
*Apply:* a dirty queue of (lamp layer, story) pairs, sorted by distance to the camera target, with a hard cap of
draws per frame. No temporal accumulation of noisy samples: every update is a complete gather of its texels, so
nothing can "breathe" (the AO lesson of `00` §9).

**(d) DDGI's probe states.** The production follow-up to DDGI skips probes that cannot affect the image (inside
walls, in unused cells) with a per-probe state machine (Majercik et al., "Scaling Probe-Based Real-Time Dynamic Global
Illumination for Production", [JCGT 2021](https://jcgt.org/published/0010/02/01/),
[arXiv](https://arxiv.org/abs/2009.10796), **verified** by abstract).
*Apply:* tiles with no room on a story (outside, void) get no gather texel and no probe.

Cost after (a)-(d), **inferred**: lamp toggle = 1 compose draw; lamp move = 1 layer gather (about 1-2 k texels per
story it reaches); build edit = the room's layers over a few frames. Quality: unchanged for indirect light, which
has no detail finer than a tile; direct light keeps its resolution.

### 4.2 Leaks at doorways and windows

`04` stops wall leaks by room scoping and plans "VPLs inside an opening's frustum pass". Prior art suggests one
mechanism for walls and openings together.

**(a) Visibility from a distance field of the wall plan.** TS3 walls are vertical and lie on tile edges or diagonals,
so on one story the question "does this VPL see this texel" is 2D. Draw the story's walls as lines into a small
texture (8-16 px per tile, openings left as gaps), turn it into a distance field with the **jump flooding algorithm**
(Rong and Tan, I3D 2006, [project page](https://www.comp.nus.edu.sg/~tants/jfa.html): about log2(n) full-screen
passes, pure pixel shaders, **memory**), and sphere-trace the segment VPL → texel in the gather shader (8-16 steps).
This is the visibility test every 2D "flatland" GI uses, including Radiance Cascades (§4.2c).
- Walls: exact to the plan's resolution; diagonals need no special case.
- Doors and arches: light passes through the gap and nowhere else, with the correct spread; no opening tables and
  no frusta.
- Windows and half walls: a second plan at "sill height" (walls that block at 0.5 m but not at 1.5 m), chosen by the
  height at which the segment crosses; or treat windows as open and half walls as open above 1 m (**inferred**; needs
  screenshots).
- Floors and ceilings: per-story scoping stays, as in `04`. Stairwells are the residual case (item 11).
- Cost: the field is rebuilt only on build-mode edits (about 10 passes on a 1024² target); the trace adds 8-16
  texture reads per VPL per texel. With §4.1(a) that is 5-10 M × 12 reads per full re-bake: about 1 ms once, spread.
  ps_3_0 loop limits: split VPLs over several additive passes (`04` open question 4).

**(b) The DDGI visibility weight, where it already fits.** DDGI stores mean distance and mean squared distance per
probe direction and weights each probe by a Chebyshev test, which is what stops its leaks (Majercik et al.,
[JCGT 2019](https://jcgt.org/published/0008/02/01/); McGuire et al., light field probes, `04` [6]; **verified** in
`04`). Apex's lamp atlas is already G16R16F "distance + moment" (`04` §0): the same test, from the lamp. Two uses:
- VPLs come out of that atlas, so a VPL is by construction a point its lamp sees.
- For the **probe → Sim** lookup (§4.4), weight the 4-8 probes around the Sim by plan visibility (a) instead of by
  per-probe depth maps: exact where DDGI is statistical, and no extra maps. DDGI's known residual, thin-object leaks
  (`04` [7]), does not arise because the plan has no thickness limit.
- Variance shadow maps bleed light where two occluders overlap in depth; if doorway edges show it, apply the
  standard reduction (clamp the lower tail of the Chebyshev bound: Lauritzen,
  [GPU Gems 3 ch. 8](https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-8-summed-area-variance-shadow-maps),
  **memory**).

**(c) Radiance Cascades, 2D, per story (experiment).** Sannikov's method computes a full 2D radiance field with a
fixed cost, no noise and no temporal accumulation, by merging cascades of probes whose ray count grows as their
spacing grows ([paper repo](https://github.com/Raikiri/RadianceCascadesPaper), MIT, **verified**;
[overview](https://80.lv/articles/radiance-cascades-new-approach-to-calculating-global-illumination); follow-up
"Holographic Radiance Cascades", [arXiv 2505.02041](https://arxiv.org/abs/2505.02041)). It runs in fragment shaders
only (WebGL implementations exist, **reported**), so shader model 3 can do it. Path of Exile 2 uses a screen-space
variant (**reported**).
- *Fit:* a story's plan is exactly its input (walls = occluders, lamps and lit walls = emitters). The lowest
  cascade conventionally keeps **4 directions per probe**, which is the layout of TS3's four directional maps
  (horizontal light from 4 directions at 0.5 m, `02` §3.2): the result could be written into them directly
  (**inferred**).
- *Gap:* flatland has no floor or ceiling, and those carry most indoor bounce. They would have to enter as an
  emission term per plan pixel (floor irradiance × albedo), which is a fudge to be judged by eye.
- *Verdict:* not the base. Worth one offline prototype in `tools/rt_lamps_test` after (a) exists, because it reuses
  the same plan texture and would give multi-bounce and penumbrae through doorways for a fixed, lamp-count-independent
  cost. 3D radiance cascades need ray tracing or a 3D distance field: out of reach in SM3.

**(d) Room volumes (Treyarch).** Call of Duty: Black Ops 3 replaced lightmaps by "carefully placed irradiance
volumes" per space, with fixes for geometry inside voxels and seams between volumes (Hooker, "Volumetric Global
Illumination at Treyarch", SIGGRAPH 2016, [slides](https://www.activision.com/cdn/research/Volumetric_Global_Illumination_at_Treyarch.pdf),
[abstract](https://research.activision.com/publications/archives/volumetric-global-illumination-at-treyarch);
abstract **verified**, slide details **reported**). It is production proof of `04`'s room-id-per-probe rule: a
shaded point takes light only from the volume of the space it is in. Nothing new to build; cite it when the rule is
questioned.

**(e) Texel hygiene from lightmap bakers.** Leaks at wall bases often come from the texel, not the visibility test: a
texel centre that lies under the wall or in the next room. Bakers push the sample point off the surface and into the
room it faces, and dilate valid texels into invalid ones (Unity "pushoff" and filtering,
[Lighting Settings](https://docs.unity3d.com/Manual/class-LightingSettings.html); `lmImageDilate` in
[`lightmapper.h`](https://github.com/ands/lightmapper); Iwanicki's seam stitching in The Last of Us, below).
*Apply:* offset each gather point half a texel along its normal before the visibility test (`04` already does this
for the room lookup), and dilate each room's indirect texels by one before the bilinear add of §4.1(a).

### 4.3 Second bounce

- **Feedback instead of a second gather (Enlighten, Lumen).** Both reuse the previous result as the next input, so
  bounces accumulate over updates at the cost of one bounce: Enlighten feeds its output lightmaps back as input
  lighting (**memory**), Lumen's "multi-bounce diffuse [comes] through surface cache feedback" (**verified**, slides).
  *Apply:* when a lamp layer is re-gathered, also emit VPLs from the low-resolution indirect map of the previous
  pass (the texel grid itself is the VPL set: 1 per tile). Two or three passes on change give bounces 2-4; with
  §4.1(a) each pass is the size of the first. Because each lamp layer is linear (§4.1b), the multi-bounce result
  is stored per lamp and toggling stays free.
- **`lightmapper.h` does the same thing offline:** "precompute global illumination for static scene geometry by doing
  multiple bounces", one loop over the lightmaps per bounce (**verified**, README).
- **The ambient term of progressive radiosity** estimates all bounces not yet computed as one constant per
  enclosure: `ambient = R / (1 - R) × area-weighted mean of the unshot radiosity`, with `R` the mean reflectance
  (Cohen, Chen, Wallace, Greenberg, "A Progressive Refinement Approach to Fast Radiosity Image Generation",
  SIGGRAPH 1988; **memory**, no link checked). Per TS3 room it is one downsample of the room's indirect texels to
  1 × 1 and one constant in the compose. It converges the series for free and is the physical version of the flat
  fill light that gives TS3 rooms their soft look (§4.5).
- **Albedo.** A bounce needs the surface colour. Apex has no G-buffer, but in texel space the wall and floor
  textures are known per room; if reading them is too much work, use the mean colour of the texture bound for that
  surface (one 1 × 1 mip read, cached) or a fixed 0.5-0.6 with the room's dominant hue (**inferred**).

### 4.4 Sims and objects lit from probes

**(a) The game already has the data layout; fill it the way Source and Halo 3 did.**
- *Half-Life 2's radiosity normal mapping* stores light arriving from 3 fixed tangent-space directions and blends
  them per pixel by the normal (McTaggart, [GDC 2004](https://cdn.fastly.steamstatic.com/apps/valve/2004/GDC2004_Half-Life2_Shading.pdf);
  [wiki](https://developer.valvesoftware.com/wiki/Radiosity_Normal_Mapping); self-shadowed variant: Green,
  [SIGGRAPH 2007](https://cdn.fastly.steamstatic.com/apps/valve/2007/SIGGRAPH2007_EfficientSelfShadowedRadiosityNormalMapping.pdf);
  history of such bases: [MJP's SG series, part 1](https://therealmjp.github.io/posts/sg-series-part-1-a-brief-and-incomplete-history-of-baked-lighting-representations/)).
  **verified** for the basis.
- *Source's ambient cube* stores 6 colours along ±X, ±Y, ±Z and weights them by the squared normal components
  (links in §2.3).
- *TS3's four directional maps* hold light from 4 horizontal world directions at 0.5 m above the floor, one value
  per metre (`02` §3.2). That is the **four side faces of an ambient cube per tile**, in world space; it resembles
  the Half-Life 2 basis in purpose (directional response for normal maps and objects) but not in space (world, not
  tangent). The missing up and down faces are the ceiling and floor texels of the room map itself (**inferred**).
- *Apply:* render the directional maps from the gather: each tile's 4 values are the cosine-weighted VPL sums for
  the 4 horizontal normals (the same gather shader with a fixed normal, 4 outputs = 1 MRT pass). Interior counters,
  objects and normal-mapped floors then get coloured, directional bounce light **with stock shaders**. The 4-face
  layers are per lamp like everything else.

**(b) Sims: SH at the Sim, dominant light extracted, written to the rig.** Halo 3 lit dynamic objects by sampling
its SH lightmaps at the object's position and pulling a **dominant directional light** out of the SH for the
analytic part of the shading (Chen and Liu, "Lighting and Material of Halo 3",
[SIGGRAPH 2008](https://www.advances.realtimerendering.com/s2008/SIGGRAPH-Lighting%20of%20Halo%203.pdf), **verified**
by extract). The extraction is a few lines (Sloan, "Stupid Spherical Harmonics Tricks",
[GDC 2008](https://www.ppsloan.org/publications/StupidSH36.pdf);
[worked example](https://www.gamedeveloper.com/programming/in-depth-extracting-dominant-light-from-spherical-harmonics)).
- *Apply to route A of `02` §4.2:* per visible Sim, on the CPU: direct lamps → rig slots 1-3, each scaled by
  plan visibility (§4.2a has a CPU twin: a 2D grid walk over the wall mask, a few hundred nanoseconds per lamp);
  indirect → the 4 vertex-light slots as the 4 horizontal ambient-cube faces at the Sim's tile, plus the dominant
  direction of the remainder.
- *Where the CPU gets the indirect value without a per-frame read-back:* the per-tile directional values are tiny
  (64 × 320 × 4 colours). Read the composed result back **once per change**, on the frame it changed (one
  `GetRenderTargetData` of a few KB; `00` §6 measured about 2 ms for such a call), and keep the CPU copy. Evening
  lamp toggles therefore cost one 2 ms frame each; if that shows as a hitch, do the per-lamp sum on the CPU from
  per-lamp copies read back only when a layer was re-gathered.
- *Blend over time.* Source interpolates a model's ambient lighting as it moves so it never pops (**memory**).
  Bilinear between the 4 nearest visible tiles plus a 0.2-0.3 s exponential blend on the rig values hides tile
  borders and doorway crossings.
- *Household rule (project):* this is rendering only and equal for every Sim; no gameplay state is touched.

**(c) Sim grounding: analytic occluders instead of Sim shadow maps every frame.** The Last of Us kept static
lightmaps and gave characters **ambient shadows from capsule / ellipsoid proxies**, with the lightmap's dominant
direction steering them (Iwanicki, "Lighting Technology of The Last of Us",
[SIGGRAPH 2013](https://history.siggraph.org/learning/lighting-technology-of-the-last-of-us-by-iwanicki/);
abstract **verified**, the capsule detail **reported**).
- *Apply:* in Apex's existing after-scene AO pass, add an analytic occlusion term for up to N Sim proxies (one
  sphere or capsule per Sim from `RigTracker`'s centre; the direction from the dominant lamp of §4.4b). The proxies
  travel in the frame-constant block (§2.2): one upload per frame.
- *What it replaces:* `03` redraws moving casters every frame for the nearest lamps; that is "the costly part"
  (`idea.md`). Capsule shadows give soft Sim shadows under any number of lamps for zero extra scene draws. Sharp
  Sim shadows from the nearest 1-2 lamps can still be added later on top.
- *Limit:* soft blobs, not limb-shaped shadows. Under a single nearby lamp the difference is visible; under typical
  multi-lamp rooms it is what a real penumbra looks like.

**(d) Not taken:** DDGI's octahedral irradiance probes with per-probe depth (replaced by plan visibility);
[RTXGI-DDGI](https://github.com/NVIDIAGameWorks/RTXGI-DDGI) itself needs GPU ray tracing and ships under the NVIDIA
RTX SDKs licence (**verified**, `License.txt`): not usable in an MIT D3D9 mod. Ubisoft's radiance-transfer probes
(Stefanov, [Far Cry 3, GDC 2012](https://www.gdcvault.com/play/1015326/Deferred-Radiance-Transfer-Volumes-Global);
[The Division, GDC 2016](https://www.gdcvault.com/play/1023273/Global-Illumination-in-Tom-Clancy), **reported**)
relight precomputed transfer on the GPU each frame for a moving sun; TS3's lamps are static between edits, so the
per-lamp layers of §4.1(b) are the cheaper form of the same idea.

### 4.5 Keeping the TS3 look ("real GI is darker and more contrasted")

Why the difference exists (**inferred**): the game's solve has no furniture occlusion, adds an ambient weight (the
room map's alpha) and its fill lights, and its lamps were tuned by eye against that. A physically based solve with
albedo around 0.5 loses half the energy per bounce and adds occlusion.

1. **Measure the target instead of guessing.** Reinhard et al. match one image to another by equalising mean and
   standard deviation per channel in a decorrelated log space ("Color Transfer between Images", IEEE CG&A 2001,
   [PDF](https://www.cs.tau.ac.il/~turkel/imagepapers/ColorTransfer.pdf), **memory**). Apex has both sides offline:
   the game's solved room maps (captured) and its own maps for the same lot. Fit, per map class (floor, wall,
   ceiling, objects), a **gain, a contrast exponent and an ambient floor** in log luminance, plus a saturation
   scale, so that the GI maps match the game's mean and spread. Apply them in the compose pass that already exists:
   3-4 constants, no per-frame cost. Report the residual per room as the look metric of `06` §3.4 (mean and 5th /
   95th percentile of log luminance within a tolerance, plus the screenshot pair).
2. **Use the controls production bakers expose for exactly this complaint.** Unity's lightmapper has *Indirect
   Intensity* and *Albedo Boost* ("boost the albedo so more light is bounced",
   [Lighting Settings](https://docs.unity3d.com/Manual/class-LightingSettings.html), **memory**). In Apex they are
   two user sliders that scale the indirect layers and the bounce reflectance; defaults come from step 1.
3. **Fill by physics, not by a flat add:** the per-room ambient term of §4.3. It raises dark corners the way the
   game's ambient weight did, but scales with the lamps that are on and takes the room's colour.
4. **Do not let AO darken twice.** With real indirect light, a multiplicative AO on top over-darkens creases. The
   GTAO paper's multi-bounce fit maps AO to a brighter value by albedo (cubic polynomial, a few instructions;
   Jimenez et al. 2016, link in §3). Apply it in Apex's AO compose when GI is on.
5. **No new tone mapper.** The game's interior-floor tone curve and bloom mask stay in its shaders and act on the
   new light (`02` §2). A filmic curve (Hable, [Filmic Tonemapping Operators](https://filmicworlds.com/blog/filmic-tonemapping-operators/))
   would change the whole image, which is the opposite of look preservation. Keep it out unless step 1 cannot reach
   the target because the 8-bit maps clip; then compress only inside the compose, before the write to the map.
6. **A "Classic / Realistic" blend.** One slider between the calibrated look (step 1 constants) and the raw GI
   (identity constants) serves both tastes with the same data.

## 5. Each source at a glance

| Prior art | Solves for us | SM3 / bridge fit | Run-time cost | Quality | License | Lane |
|---|---|---|---|---|---|---|
| Just Cause 2 world-tiled light index; clustered shading; Doom 2016 | Failure 1 | ps_3_0 dependent reads; 1 shared sampler; 0 per-draw calls | Rebuild cells on lamp / wall change only | Per-pixel lamp choice, better than per-model | Papers / slides | `02` §2, §3.3 |
| D3D9 device-wide constants; instancing streams | Failure 1 | Native; verify free register range | 1 ranged call a frame | Neutral | API docs | `02`, `00` §7 |
| Source ambient cube + per-model light cache | Failure 1 for ps_2_0 families; Sims | No shader change (rig write) | Per model on change | One sample per model | Papers; Source SDK code not used | `02` §4.2 A |
| Enlighten (Frostbite 2) | Re-bake cost; second bounce | Idea only: low-res indirect, feedback | 16-256× fewer gather texels | Indirect has no sub-tile detail to lose | Proprietary SDK (Silicon Studio): idea only | `04` §1.5 |
| UberBake (Call of Duty) | Lamp toggles, doors | Per-lamp fp16 layers, 1 compose draw | Toggle = 0 gathers; 2.5 MB bridge VRAM | Exact (linearity) | Paper | `04` S1 |
| Lumen surface cache | Update budget; second bounce; SSGI split | Texel-space cache is what Apex has | Fixed cap per frame | Stale texels for a few frames | UE source licence: slides only | `04` S1, S3 |
| Unity progressive / Frostbite Flux GPU bakers | View-first ordering; pushoff, dilation; look sliders | Idea only (their kernels need compute / RT) | - | - | Docs / slides | `04` S1, S6 |
| Jump flooding + plan ray march | Door / window leaks, VPL visibility | Pixel shaders only; rebuild on build edit | 8-16 reads per VPL-texel pair | Exact to plan resolution; heights approximate | Paper | `04` S3, S5 |
| DDGI (+2021 production paper) | Leak theory; probe states | Chebyshev test reuses the existing moment atlas | Small | Statistical; thin-object residual avoided by plan | Papers free; RTXGI SDK not usable | `04` §1.3 |
| Light field probes (McGuire 2017); Treyarch volumes | Confirms room-scoped probes | - | - | - | Papers | `04` §1.3 |
| Radiance Cascades (2D) | Doorway penumbra + multi-bounce at fixed cost | Fragment-shader only | Fixed per story, lamp-count independent | Noise-free; flatland only | Paper repo MIT | experiment |
| Half-Life 2 RNM / ambient cube | Meaning of TS3's 4 directional maps | Stock shaders read them | 1 MRT gather pass | Directional bounce on objects and normal maps | Papers | `02` §3.2 |
| Halo 3 SH + dominant light (Sloan) | Sims from probes | CPU math, rig write | Per Sim on move / change | Direction and colour, no shadow edge | Papers | `04` S4 |
| The Last of Us capsule occlusion | Sim shadows without per-frame shadow maps | One term in the AO pass, frame constants | N proxies per pixel in AO | Soft, not limb-shaped | Talk | `03` §4 |
| Hemicube baking (The Witness; `lightmapper.h`) | Offline ground truth with real visibility | Raster only: 5 views per texel; offline harness | Offline | Reference quality | `lightmapper.h`: public domain (**reported**, check the file header); blog posts | `04` S0, `06` |
| Reinhard colour transfer; Unity indirect intensity / albedo boost; GTAO multi-bounce | The TS3 look | Constants in existing compose / AO | 0 | Matches the game's statistics per room class | Papers / docs | `04` S6 |
| Bridge `commandBatchingEnabled` | Per-call bridge cost | Config line | - | - | dxvk-remix: Zlib / MIT | `07` §1 |
| Godot VoxelGI / SDFGI ([MIT](https://github.com/godotengine/godot/blob/master/LICENSE.txt)) | Nothing for thin walls: its docs ask for thick walls | Needs compute or CPU voxelisation | - | Leaks at zero thickness | MIT | rejected |
| ENB, TES Reloaded, MGE XE | Covered in `07` §5 | - | - | - | Closed / GPL: study only | `03` |

Hemicube sources: Castaño, [Hemicube Rendering and Integration](https://www.ludicon.com/castano/blog/articles/hemicube-rendering-and-integration/)
(render 5 views per texel, random hemicube rotation against banding, **verified** by extract);
[ands/lightmapper](https://github.com/ands/lightmapper). A full bake of even 20 k low-resolution texels is 100 k
proxy draws: fine offline in `tools/rt_lamps_test` against the occluder proxy plus furniture boxes, never in game.
MJP's [BakingLab](https://github.com/TheRealMJP/BakingLab) (MIT) is a second reference implementation for comparing
lightmap bases (SH, HL2 basis, ambient + directional).

## 6. Integration plan

Each step is one change, default off behind a switch, compared on the camera pan harness (`06` §3).

| Step | What | Depends on | Build | Game runs |
|---|---|---|---|---|
| P0 | Bridge `commandBatchingEnabled=true`: pan A/B, p50 / p99 / hitches | - | 15 min | 1 |
| P1 | Register-range histogram of the game's `SetPixelShaderConstantF` / `SetVertexShaderConstantF`; reserve the frame block | - | 30-60 min | 1 |
| P2 | Offline: per-tile indirect gather with per-lamp layers + compose (extends `04` S0 / S1); hemicube reference in the harness for one captured lot | `04` S0 | 3-6 h | 0 |
| P3 | Offline: wall-plan texture + jump-flood field + visibility in the gather; leak tests: closed wall = exactly 0, doorway = spill inside the door's cone, window per the sill rule | P2 | 3-5 h | 0 |
| P4 | Feedback bounces + per-room ambient term; look fit (gain, exponent, floor, saturation) against the game's solved maps; GTAO multi-bounce in AO | P2 | 2-4 h | 0 |
| P5 | In game, one lot: P2-P4 on, lamp toggle / move / wall edit / door; screenshots of the three rooms of `04` S2 | P2-P4 | 1-2 h | 1-2 |
| P6 | Directional maps rendered as ambient-cube faces; rig fill for Sims (visibility-scaled lamps, SH dominant light, time blend, read-back once per change) | P5 | 4-8 h | 1-2 |
| P7 | Lamp index cells + lamp table in the shared atlas; patched outdoor-object copies read them; per-draw picks and uploads off for those families | P1 | 4-8 h | 2 |
| P8 | Capsule occlusion for Sims in the AO pass | P1 | 2-4 h | 1 |
| P9 | Optional: 2D Radiance Cascades prototype offline on the P3 plan; adopt only if it beats P3 + P4 by eye at equal cost | P3 | 4-8 h | 0 |

P0, P1 and P7 attack failure 1 and do not depend on the GI work; they can run first.

## 7. Hardware ray tracing through the bridge's 64-bit Vulkan server

Premise: the bridge is ours to patch. Its server replays the game's D3D9 calls through DXVK onto Vulkan in a 64-bit
process (`07` §1). A patched server can therefore run **native Vulkan work that D3D9 cannot express**: compute
shaders, acceleration structures, ray queries, its own threads and queues. Everything in this section is
**inferred** unless tagged; none of it was built.

### 7.1 Two findings that matter more than any single RT feature

1. **The server lifts the shader model 3 limits even without ray tracing.** The texel-space bake, the probe updates
   and the plan distance field of §4 can be Vulkan compute shaders inside the server. The 32-bit client then sends
   one small custom command per change (lamp table, dirty flags, wall plan) instead of dozens of D3D9 draws, state
   calls and render-target switches, and the work leaves the game's render thread entirely. "Async compute" in this
   design means: the bake runs on the server's own thread and is submitted between the replayed frames (or on a
   second queue where the driver exposes one); the game never waits for it. This is a fallback tier of its own:
   it needs only Vulkan 1.1-class compute, which every GPU that runs the bridge has.
2. **Texel space is where rays pay off in a forward game with no G-buffer.** Per-pixel ray-traced effects (ReSTIR,
   RTGI, denoisers, DLSS) all assume depth, normals, albedo and motion vectors per pixel, which TS3 does not have
   (`04` §0). A lightmap texel has a known position, normal and room, needs no motion vector, converges once and is
   then free. That is exactly how production GPU bakers use rays.

### 7.2 Prior art for ray tracing behind a raster or translated game

| Prior art | What it shows | Use for us |
|---|---|---|
| **dxvk-remix runtime** ([repo](https://github.com/NVIDIAGameWorks/dxvk-remix); scene and acceleration-structure code in [`rtx_scene_manager.cpp`](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/src/dxvk/rtx_render/rtx_scene_manager.cpp), [`rtx_accel_manager.cpp`](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/src/dxvk/rtx_render/rtx_accel_manager.cpp)) | A DXVK fork that intercepts D3D9 draws inside the translation layer, builds BLAS / TLAS from the vertex buffers it already holds, and path-traces. Bundles RTXDI (ReSTIR DI), ReSTIR GI, NRC, NRD-style denoising, DLSS and XeSS as submodules (**verified**, `.gitmodules`; `rtx.di.*` options in `RtxOptions.md`). DXVK part zlib (**verified**, `LICENSE`); the NVIDIA SDKs keep their own licence | The model for "geometry is already in the server": take the acceleration-structure build path as a reference. Its scene capture needs fixed-function transforms, which TS3 lacks (`07` §2), so Apex must tell the server which draws are static casters and where (§7.3) |
| **Bridge Remix API passthrough** (`07` §1, verified) | A 32-bit client can already push meshes and lights over the IPC channel | The transport pattern for our custom commands |
| **vkd3d-proton** ([repo](https://github.com/HansKristian-Work/vkd3d-proton)) | Implements D3D12's DXR on `VK_KHR_ray_tracing_pipeline` / `VK_KHR_ray_query` in a translation layer (**memory**) | Proof that translated shaders and Vulkan RT coexist on all three vendors. No D3D9 relevance beyond that; DXVK itself has no ray tracing because D3D9-11 have none |
| **dxvk-nvapi** ([repo](https://github.com/jp7677/dxvk-nvapi)) | DLSS and Reflex plumbing for translated D3D11 / 12 games (**memory**) | Only if DLSS is ever wanted (§7.4 R6) |
| **Frostbite Flux GPU baker; Unity GPU progressive lightmapper; Bakery** (Hillaire, [GDC 2018](https://www.gdcvault.com/play/1024353/Real-time-Raytracing-for-Interactive); [Unity](https://docs.unity3d.com/Manual/GPUProgressiveLightmapper.html)) | Lightmaps path-traced on the GPU, progressively, view-prioritised, denoised in texel space (Unity ships Intel Open Image Denoise, **memory**) | The design of R1 below |
| **Lumen hardware ray tracing mode** ([docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine); slides in §3) | Rays hit real triangles, but the lighting at the hit is **read from the surface cache** instead of being shaded at the hit (**memory**) | A ray that hits a wall or floor reads Apex's own light map at the hit point: no material system needed in the server |
| **DDGI with ray-traced probe updates** (§4.2b papers) | Probes updated by a few hundred rays each, with visibility from the same rays | R3 below |
| **ReSTIR DI / GI** (Bitterli et al., [SIGGRAPH 2020](https://research.nvidia.com/publication/2020-07_spatiotemporal-reservoir-resampling-real-time-ray-tracing-dynamic-direct); Ouyang et al., [2021](https://research.nvidia.com/publication/2021-06_restir-gi-path-resampling-real-time-path-tracing)) | Thousands to millions of lights with 1-2 rays per pixel by reusing samples across pixels and frames; needs a G-buffer, motion vectors and a denoiser | R5 below |
| **Quake II RTX, RayTracedGL1** ([Q2RTX](https://github.com/NVIDIA/Q2RTX), GPL-2; [RayTracedGL1](https://github.com/sultim-t/RayTracedGL1), MIT) | Old raster games path-traced on Vulkan by re-submitting geometry to an RT library | Reference code for acceleration-structure management in Vulkan (RayTracedGL1 is MIT) |
| **ReShade "RTGI" and similar** (`07` §5; [coverage](https://www.pcgamer.com/this-experimental-hack-adds-ray-tracing-to-any-game-including-crysis)) | Screen-space ray marching on depth only: no geometry, no light positions, so off-screen light is missing (**reported**). A later add-on demo that captures draw geometry for path tracing was shown for D3D9+ games ([report](https://www.techspot.com/news/99628-reshade-project-may-bring-path-tracing-skyrim-all.html)); no public release or source found | Confirms the lesson: without scene geometry and lamp data an injector cannot do GI. Apex has both |

### 7.3 Where the triangles come from

- **Tier A, proxy only (first step).** Apex's occluder proxy (walls with openings, ceilings, roofs, slabs) is about
  10 k triangles and already exists (`04` §0). Send it once per build edit; the server builds one BLAS. This alone
  gives exact wall, door, window, half-wall and stairwell visibility in 3D, which removes every special case of
  §4.2.
- **Tier B, static furniture.** The server already holds every vertex and index buffer the game uploaded. Apex
  knows which draws are static objects and their world matrix (it patches their constants). One command per placed
  object ("this buffer range, this vertex layout, this matrix") lets the server build a BLAS per mesh and one TLAS
  instance per object, rebuilt only when an object is placed or moved. Position formats must be checked: compressed
  positions need a supported vertex format or a one-time conversion.
- **Tier C, Sims.** Skinned in the vertex shader, so no world-space triangles exist. Use the capsule proxies of
  §4.4(c) as procedural or low-poly instances, refit per frame (cheap), or leave Sims out of the bake and keep
  capsule occlusion.

### 7.4 The options, with cost, quality and fallback

GPU figures are order-of-magnitude for an RTX 3060-class laptop at a few hundred million simple rays a second
(**inferred**, to be measured).

| # | Option | Solves | Cost | Quality | Fallback without RT hardware |
|---|---|---|---|---|---|
| R0 | **Server-side compute bake, no rays** (§7.1 point 1): §4's gather, layers, plan field as compute | Failure 1 for Apex's own passes; re-bake cost; SM3 loop and render-to-volume limits | Bridge patch + custom commands: 1-2 days of agent work; then each §4 step is simpler than its D3D9 form | Same as §4 | Is the fallback: any Vulkan GPU. Below that, the D3D9 path of §6 |
| R1 | **Ray-query texel bake**: direct shadows and multi-bounce indirect per texel with `rayQuery` in the compute bake; hit lighting from the light maps (Lumen style); per-lamp layers kept | Leaks (all cases, 3D), second bounce, furniture occlusion and colour bleeding, chair-leg shadows at 16-32 texels a metre | Direct: about 1 M floor texels per lamp at high density = 1 M rays, a few ms once per lamp change. Indirect at 1 texel per tile: 40 k texels × 64 rays × 3 bounces = 8 M rays, tens of ms spread over frames, zero when clean | Reference quality for static light; noise-free after convergence, then frozen (no breathing). A small edge-aware filter in texel space replaces a denoiser | R0 with plan visibility (§4.2a) and shadow maps. Same maps, same shaders: the two tiers differ only in how visibility is answered |
| R2 | **Ray-traced soft shadows from lamp size** in the same bake (several rays per lamp across its bulb or shade) | PCSS (`02` §2: 120-160 slots per lamp) is no longer needed | × 4-16 rays on the direct pass, still on change only | Correct contact-hardening | Variance or PCF from the existing atlas |
| R3 | **DDGI probes updated by rays** (own implementation from the papers) for Sims and moving objects, sampled in the server into the ambient-cube faces and read back once per change (§4.4b) | Sims lit correctly in stairwells and split levels, moving furniture | 20 k probes are not needed: one probe per tile of rooms in view, 64-128 rays each on change | Better than plan-weighted probes where height matters | §4.4 as written |
| R4 | **Per-pixel ray-query shadows inside the game's translated shaders**: the patched copy reads a reserved sampler, and the server's shader translator replaces that read with a ray query toward the lamp | Sharp per-pixel lamp shadows on every surface including Sims as receivers, no shadow maps, no texel-density limit | A change inside DXVK's D3D9 shader translator; 1 ray per lamp per pixel per frame (4 lamps at 4K = 33 M rays a frame: heavy on a laptop 3060). Hard shadows only unless denoised | Sharp and exact, but hard-edged; soft needs temporal filtering, which Apex removed for AO because of breathing | The texel-space shadows of R1 / §4 |
| R5 | **ReSTIR DI / GI per pixel** | Hundreds of lamps in view (a whole neighbourhood at night) | Needs depth + reconstructed normals + motion vectors + a denoiser; several ms a frame; RTXDI and NRD are under the NVIDIA RTX SDKs licence (**verified**), so own implementations from the papers or AMD's MIT-licensed FidelityFX denoiser | No albedo and no motion vectors for Sims: ghosting and double-counted colour, the SSGI failure again with better rays | Not needed: a lot has tens of lamps, and the texel bake visits each once per change |
| R6 | **DLSS / DLAA, NRD** | Anti-aliasing or upscaling, not lighting | Needs jittered projection and motion vectors from a forward D3D9 game that has neither; NVIDIA-only | Unknown; ghosting on Sims likely | Apex's existing edge smoothing |

Fallback rule for all of them: probe `VK_KHR_acceleration_structure` + `VK_KHR_ray_query`
([extension](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_ray_query.html),
[device coverage](https://vulkan.gpuinfo.org/listdevicescoverage.php?extension=VK_KHR_ray_query)) at server start.
Ray query is not NVIDIA-only: Radeon RX 6000 and later and Intel Arc expose it (**memory**). Present → R1-R3;
absent → R0; server not patched or disabled → the D3D9 path. The light maps, layers, look constants and shader
bindings are identical in all three tiers, so screenshots can be compared tier against tier, and R1 doubles as the
ground truth that item 8 (hemicube) was meant to provide.

### 7.5 Combined ranking (SM3 items of §1 and the server options)

| Rank | Item | Why here |
|---|---|---|
| 1 | SM3 #1 frame-constant block + lamp index texture | Only item that removes the measured 1.8-3.4 ms; independent of everything else |
| 2 | SM3 #2 per-lamp layers on low-resolution indirect | Tier-independent data model; every later item writes into it |
| 3 | **R0 server-side compute bake** | Removes the bridge and SM3 from the GI passes; prerequisite of R1-R3. Decide before building §4 twice |
| 4 | **R1 ray-query texel bake** (+ R2) | Solves leaks, second bounce and furniture occlusion in one mechanism at reference quality; cost only on change |
| 5 | SM3 #3 wall-plan visibility | Becomes the non-RT fallback of R1 instead of the main path |
| 6 | SM3 #4-#6 feedback bounce, ambient cube + rig fill, look calibration | Needed in every tier |
| 7 | SM3 #7 capsule occlusion for Sims | Sims are outside the bake in every tier |
| 8 | **R3 ray-traced probes** | Only where plan-weighted probes fail |
| 9 | SM3 #8-#11 (hemicube reference, 2D Radiance Cascades, command batching, raster DDGI) | Reference and experiments; hemicube is superseded if R1 exists |
| 10 | **R4 per-pixel ray-query shadows** | Highest visual ceiling for direct light, but per-frame rays and a translator patch |
| - | **R5 ReSTIR, R6 DLSS / NRD** | Solve problems TS3 does not have, need buffers it does not have, and carry SDK licences |

Order of work if the server route is taken: P0, P1, P7 of §6 stay first (failure 1). Then R0 with the §4.1 data
model, then R1 with the proxy only (Tier A), then furniture (Tier B). P2-P4 are then written once, as compute, with
plan visibility as the no-RT branch.

Risks specific to this route: a fork of the bridge and of DXVK must follow upstream; a bake bug now crashes or
stalls the server process rather than the game (better for the 32-bit address space, worse to debug); the custom
commands must tolerate a stock server (feature probe, silent fallback).

## 8. What was not verified

- §7 as a whole: no bridge or DXVK source was read for where a compute or ray-query pass would hook in, how a second
  queue or thread interacts with DXVK's command stream, or whether TS3's vertex position formats are valid
  acceleration-structure inputs. Ray throughput figures are guesses. Vendor support for ray query beyond NVIDIA,
  Unity's use of Open Image Denoise, Lumen's hit-lighting modes, the vkd3d-proton and dxvk-nvapi descriptions and
  the FidelityFX licence are from memory.
- No prototype, shader compile or game run backs any cost on this page; the gains in §2.1 are upper bounds read off
  the `00` §7 table.
- Items tagged **memory** or **reported** were not re-read in the primary source today: Enlighten's resolution and
  feedback details, Lumen's and Unity's scheduling, Source's lighting cache and blend, the capsule details of The
  Last of Us, jump flooding pass counts, the progressive-radiosity ambient formula, Unity's slider descriptions,
  3DMigoto's `ShaderRegex` syntax, the Treyarch slide contents.
- `humus.name` refused scripted access (HTTP 403), so the Just Cause 2 figures come from a search extract of the
  slides; the GPU Pro chapter was not read.
- `lightmapper.h`'s public-domain status comes from a listing, not from the file header.
- Whether the game ever writes pixel constants above `c24` at run time (P1), whether `commandBatchingEnabled` is
  already set in Apex's shipped `bridge.conf`, and how windows and half walls should pass light (P3) are open.
- The claim that TS3's directional maps equal the side faces of an ambient cube rests on `02` §3.2's description,
  not on the shader math; check how the readers weight the 4 maps before P6.
