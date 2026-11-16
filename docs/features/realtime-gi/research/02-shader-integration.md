# Real-time GI: how the game's shaders would read the new light

Research for [the idea](../idea.md), lane R2 (shading integration). Static analysis only: no game run, no code changed.
It answers four questions: which shader families read lamp light today and how much room each has left, what a patched
copy would have to add, where the limits are, and how Sims and objects are lit.

**Short answer:** do not patch a lamp loop into every lit shader family. Render the new light into Apex-owned copies of
the textures the game already samples for lamp light (room maps and the four directional maps) and bind them with the
`SetTexture` swap Apex already has. That covers every room-lit family with no game-shader change. Per-family patches are
then needed only for surfaces that read no light map: outdoor rig-lit objects, Sims, foliage, roofs and water.

## 0. Sources and marks

| Mark | Meaning |
|---|---|
| **V-precomp** | Measured for this report by parsing `<Game>\Bin\Shaders_Win32.precomp` (Steam 1.67.2, 92,614,595 bytes), read-only |
| **V-doc** | Stated as verified in Apex's own docs or source (`docs/engine/shaders.md`, `room-light-maps.md`, `light-objects-and-rigs.md`, `features/shader_patches.h`, the night-lighting pages) |
| **Inferred** | Reasoned from the above, not measured. Needs a prototype or a capture |

How the precomp numbers were made (so they can be reproduced):

- Blob and technique walk as in `tools/ao_receiver_test/generate_ids.cjs` (`VSHD`/`PSHD` blobs, `TECH`/`PASS`, 1-based
  shader indices at pass +12 / +16, `KNM ` names).
- New finding (**V-precomp**): after a pass's render states comes a **sampler bit mask**, then one `(dword, name hash)`
  pair per set bit, in ascending sampler order. Check: 100,837 bindings of cube light probes all land on a sampler the
  pixel shader declares as a cube; the indices match the docs (`LightMap` = `s2` in ExteriorFloors, `s1` in the lot
  light pass, `terrainLightMap` = `s8` in the 5-layer terrain shader). The first dword of each pair is not the sampler
  index (meaning unknown).
- 156,415 of 196,645 passes parsed this way. The other 40,230 have no list in that shape (assumed: no named textures;
  not checked). Counts of "passes that bind a light map" are therefore lower bounds.
- Instruction slots are counted from the token stream with the usual macro costs (`nrm` 3, `pow` 3, `lrp` 2, `sincos`
  8, matrix ops 3-4). `def`, `dcl` and comments are not counted. Treat slot numbers as ±5%.

## 1. Who reads lamp light, and how much room is left

### 1.1 Whole package (V-precomp)

| | Value |
|---|---|
| Techniques | 296 |
| Pixel shaders used by techniques | 7,859: 4,782 `ps_3_0`, 3,077 `ps_2_0` |
| Vertex shaders used by techniques | 7,523: 3,245 `vs_3_0`, 4,278 `vs_2_0` |
| Largest `ps_3_0` | 343 slots (Roof, with loops). Limit on native D3D9: 512. DXVK reports 32768 (**V-doc**) |
| Largest `ps_2_0` | 74 slots in all, up to 64 arithmetic: **at the `ps_2_0` limit** (64 arithmetic + 32 texture) |
| Highest pixel constant the game sets | `c24` of 224 |
| Samplers per pixel shader | ≤8: 6,057; 9-12: 1,441; 13: 190; 14: 115; 15: 44; **16: 12** |
| `ps_3_0` inputs | 10 of 10 used in 54 shaders; 9 in 772 |
| Vertex shaders | Largest 192 slots; highest constant `c227` of 256; `vs_3_0` outputs: at most 11 of 12 (139 shaders) |

Pixel shaders that bind a light map: 2,318. Of those, 1,201 bind a **room** map (`LightMap`, `RoomLightMap`,
`LightBasisMap0..3`): 721 `ps_3_0` and 480 `ps_2_0`. The other 1,117 bind only the world terrain stamp
(`terrainLightMap`).

### 1.2 Families that read the room light maps (V-precomp unless marked)

"Light map @" is the sampler the name is bound to, with the number of passes in brackets where it varies. Slots,
samplers, constants and inputs are for the pixel shaders of the passes that bind a light map. `s5` never appears in a
name list: it is the sun shadow map, bound by the engine (**V-doc**).

| Family (technique) | Light map @ sampler | uv source | PS (v2 / v3) | v3 slots med / max | v3 samplers med / max | PS const max | v3 inputs max | Apex today (V-doc) |
|---|---|---|---|---|---|---|---|---|
| **InteriorWall** | `LightMap` s0 (36), s1 (20), s2 (4) | wall atlas, vertex `TEXCOORD1 × 1/4096` | 13 / 8 | 26 / 32 | 3 / 4 | c11 | 5 | Smooth read (`PatchRoomMapSmooth`); kept rooms drawn into the map copy |
| InteriorWallWith Blacklight / Strobelight / both | `LightMap` s0-s2 | same | 33 / 0 (**ps_2_0 only**) | - | 7 max | c11 | - | Same pattern |
| **ExteriorWall** | `LightMap` s1 (96), s2 (66) | same atlas | 31 / 27 | 53 / 77 | 7 / 10 | c10 | **10** (2 shaders) | Lamp scale `cK.x` around the draw (58-entry table); room-0 texels relit in the copy |
| ExteriorWallAOSI | `LightMap` s0 | same | 2 / 1 | 8 | 3 | - | 2 | - |
| **InteriorFloor**, FloorWith*, Hideable | `LightMap` s0 / s1; `LightBasisMap0..3` s3-s11 in normal-mapped variants | floor map, `TEXCOORD1 × 1/32767` | 16-18 / 16-32 each | 39-60 / 78-87 | 5-7 / 11 | c13 | 8 | Smooth read; arch ambient; wall mask |
| FloorTileCeiling | `LightMap` s0-s2 | floor / ceiling map | 36 / 4 | 21 / 23 | 3 / 4 | c6 | 5 | Smooth read |
| Ceiling | `LightMap` s0, `ceilingLightmapScale` | ceiling map | 2 / 0 | - | 3 | c0 | - | - |
| **ExteriorFloors** | `LightMap` s1-s3; basis s4-s13 | floor map 256² over 64 m | 232 / 336 | 66 / 170 | 10 / **16** | c22 | 9 (76) | `max` with the world atlas (261 of 571 accepted); room-0 relight in the copy |
| SnowFloorEdges | `LightMap` s1-s7; basis s7-s11 | floor map | 8 / 6 | 73 / 75 | 11 / 12 | c5 | 6 | Snow atlas patch |
| **TerrainLight**, TerrainLightAndFog, TerrainLightNoSnowElev (lot ground pass) | `LightMap` s0-s2; basis s4-s13; `terrainLightMap` s1-s9 | lot map, `TEXCOORD5` in the 568-byte pass | 39 / 90 | 85 / 124 | 10 / 15 | c9 | 7 | Replacement pass (`max` of lot and terrain map); snow variant patched |
| TerrainLight_Indoor, _IndoorWithStrobeLight | `LightMap` s0 | lot map | 3 / 2 | 7-10 | 2 | c2 | 1 | - |
| TerrainLight_InsideGreenhouse | `LightMap` s0-s1 + `terrainLightMap` | lot map | 39 / 9 | 29 / 40 | 3 / 4 | c5 | 3 | - |
| **Counters** (interior passes: 304 of 2,736) | `LightMap` s1-s2; basis s3-s13 | `WorldToLotTransform` (`LightMapXform`) in the VS | 24 / 32 | 70 / 94 | 10 / 13 | c10 | 9 | Smooth / wall-aware basis read (`PatchIndoorBasis`, `PatchBasisSmooth`) |
| **SingleObject / InstancedObject** (interior: 768 of 8,000 each) | `LightMap` s1; basis s2-s8 | same | 8 / 24 | 44 / 58 | 8 / 9 | c6 | 9 | Same basis patches |
| **Phong** (interior: 544 of 8,991), PhongAlpha (320 of 816) | `LightMap` s0-s2; **no basis maps** | `LightMapXform` | 32 / 84; 14 / 20 | 44 / 84; 64 / 79 | 5 / 8 | c17 | **10** (2) | Floor-map read kept off walls (`PatchRoomMapReadWalls`) |
| RoadGrateDrain, Rug, Painting, Foliage (indoor plants), BrushedMetal | `LightMap` s0-s2 | `LightMapXform` | **ps_2_0 only** (28, 6, 2, 2), BrushedMetal 1 v3 | 78 | 4-8 | c15 | 8 | - |
| PondWater (12 of 84 passes) | `LightMap` s1 | lot map | 3 / 6 | 154 / 164 | 13 / 14 | c10 | 9 | Extra additive lamp pass on the lake shader |
| FullBright (emissive, 1 pass) | `RoomLightMap` s1, `LightMapScale` | - | 1 / 0 | - | 2 | c0 | - | - |
| BurntTile, LeavesTile, AOandSI, terrainAOSI, TerrainHighNoSnowElev, TerrainLow (64 passes) | `LightMap` s0-s1 | - | mostly ps_2_0 | - | ≤7 | - | - | - |

What the room map holds (**V-doc**): `rgb = min(Σ lamps × room normalisation, 1)`, `alpha = ambient weight`, 8-bit
A8R8G8B8, 1/2/4 texels per metre by lighting class. Interior shaders compute `rgb + alpha × InteriorBuildingAmbientColor`;
interior floors then apply the `LightingTweaks` tone curve. Room-0 (outdoor) texels have alpha 0 in the game. The four
directional maps are 64 × 64 at 1 texel per metre, sampled 0.5 m above the floor, one per horizontal direction
(+X, −X, +Z, −Z); Counters indoors take
`max(LightMap × sat(2 N.y), Σ basis × sat(N·b), sky)`.

### 1.3 Lit families that read **no** room map

| Family | Lamp light today | PS (v2 / v3) | v3 slots med / max | v3 samplers max | Notes |
|---|---|---|---|---|---|
| World terrain: TerrainHigh, Road, TerrainLow, billboard | `terrainLightMap` (GPU stamp per 256 m chunk, no occlusion) | 72 / 420; 160 / 264 | 106 / 185; 60 / 148 | **16** (8 shaders); 15 | Not the room solve. Out of scope for a per-lot GI; keeps the stamp |
| **Phong / Counters exterior**, OutdoorProp, Sculpture, Landmark | Rig: sun + 3 lamps in PS `c1..c3` / `c5..c7`, 4 vertex lights in COLOR0, sky cube | 255 / 444 (Phong) | 61 / 159 | 13-14 | Apex: 8 per-pixel lamps + ground atlas through `PatchObjectLampVs/Ps` (world position in TEXCOORD8) |
| **SimSkin** | Rig constants + `DiffuseLightProbe` / `SpecularLightProbe` cubes + `SimShadow` | 97 / 654 | 99 / 158 | 12 | 72 shaders use 9 inputs, none 10. Apex: no lighting patch (AO receiver ids only) |
| SimHair | same, `BlurrySpecularLightProbe` | 30 / 18 | 53 / 65 | 8 | 2 shaders use all 10 inputs |
| SimEyes, SimRobot, CASSkinSinglePass | rig + probes | 46, 70, 26 (**ps_2_0 only**) | - | 4-8 | Arithmetic at the ps_2_0 limit |
| Instanced foliage, SpeedTree (`branchMain`, `frond`, `leafCard`, `leafMesh`) | Per-instance lamps in the VS (vs_2_0); `ShadowMap` | **ps_2_0 only** (44, 32, 108, 108) | - | 6 | Apex: wrap lighting, leaf shadow fade |
| Roof | None (sun + sky) | 108 / 120 | **297 / 343** | 11 | Apex: replacement PS with 16 lamps in `c20..c52` |
| GlassForObjects / GlassForPortals | Rig lamp colour in the specular lobe; probes | 132 / 156; 5 / 0 | 65 / 113 | 9 | No patch |
| PoolWater, sea, StandingWater | None | ps_2_0 mostly | - | 5 | - |
| Night (HD objects) | 6 per-pixel lamps in constants | 45 / 116 | 117 / 161 | 11 | Needs nothing today |

### 1.4 Where the limits bite (V-precomp)

- **Instruction slots:** not a limit for `ps_3_0`. Room-map readers top out at 170 of 512; everything except Roof is
  under 190. **`ps_2_0` is full**: 28 room-map shaders already use 56-64 of the 64 arithmetic slots.
- **Constants:** not a limit. Pixel shaders stop at `c24`; 199 registers are free. Vertex shaders reach `c227`
  (skinning), which leaves 28.
- **Samplers:** of the 721 room-map `ps_3_0` shaders, 4 use all 16 (ExteriorFloors), 24 use 15 or more (20
  ExteriorFloors, 4 lot ground pass), 63 use 14 or more. Every one of the 15-and-over shaders binds `LightMap` plus the
  four directional maps, so **a copy that no longer reads the game's lamp maps gets those samplers back**: after that,
  none is above 15 and only 2 are at 15.
- **Inputs:** 4 room-map shaders use all 10 inputs (2 ExteriorWall, 2 Phong); 107 use 9. A copy that drops the room map
  read also frees its uv input (TEXCOORD1).
- **Vertex outputs:** at most 11 of 12 in `vs_3_0`.

## 2. What a patched copy would have to add (per-family route)

Costs below are **inferred** from hand-counted `ps_3_0` sequences; compile prototypes with `fxc` before relying on them.

| Addition | Samplers | Constants | Slots | Inputs |
|---|---|---|---|---|
| World position (and a usable normal) in the PS | 0 | 0 | VS +1-3 | 1 (`PatchObjectLampVs` already does this for 588 of 588 object VS) |
| One lamp: vector, distance, falloff, N·L | 0 | 2 | ~9 | 0 |
| Its cone (spot, shaded lamp) | 0 | 1 | ~4 | 0 |
| Its shadow: cube face pick, atlas rectangle, 1 compare | 1 shared atlas | 1-2 | ~18 | 0 |
| 4-tap PCF instead of 1 compare | 0 | 0 | +~12 | 0 |
| PCSS (blocker search + wide filter) | 0 | 1 | ~120-160 | 0 |
| Probe grid sample, volume packed in a 2D atlas, 2 slices blended by hand, 4 coefficients | 1 (or the shadow atlas's, if both share one float texture) | 3 | ~25-30 | 0 |

How many shadowed lamps fit on native D3D9 (512 slots):

| Shader size | Hard shadow (~31 a lamp) | 4-tap PCF (~43) | PCSS (~150) |
|---|---|---|---|
| Typical room-lit shader (59 slots) + probe sample | 13 | 9 | 2 |
| Largest room-lit shader (170) + probe sample | 10 | 7 | 2 |
| SimSkin, largest (158) | 10 | 7 | 2 |
| Roof (343) | 4 | 3 | 0-1 |

Other facts that shape a patched copy:

- `ps_3_0` cannot index constants in a loop, so each lamp is unrolled; N is fixed per copy.
- Lamp shadow maps must share **one atlas** (Apex's real-time outdoor lamps already use one: 1536 × 4096 G16R16F, 16
  lamps of 6 × 256² faces, **V-doc**). A cube texture per lamp would cost a sampler each.
- D3D9 cannot render into a volume texture, so the probe grid goes into a **2D atlas** of slices and the shader blends
  two slices itself.
- With one float atlas holding both the shadow faces and the probe slices, the whole feature costs **1 sampler**, which
  fits even the two shaders left with a single free one.
- The game's look survives if the copy only swaps the lamp term: the sun, sky cubes, material maps, the interior
  floors' tone curve and the bloom mask in alpha stay in the shader and act on the new light.
- **`ps_2_0` shaders cannot take any of this.** They need a `ps_2_x` copy (Apex's smooth room read already does that
  and checks `PS20Caps`, **V-doc**) or a rewrite to `ps_3_0`, which on native D3D9 also needs a `vs_3_0` partner. Whole
  families exist only as `ps_2_0`: the strobe / blacklight walls, Rug, Painting, RoadGrateDrain, SimEyes, SimRobot and
  all foliage.

The real cost of this route is not a hardware limit. It is:

1. **Breadth.** About 1,200 room-map pixel shaders in some 40 techniques, plus the rig-lit families (Phong 699,
   SimSkin 751, Counters 328, Landmark 558, glass 288...). Each family needs its own pattern matcher and an offline run
   over the precomp. Apex's history shows 60-96% acceptance per family, never 100% (**V-doc**: Counters 136 of 184,
   ExteriorFloors 261 of 571).
2. **Per-draw CPU.** N lamps × ~5 constants must be chosen and set per draw. Today's per-draw lighting fixes are already
   Apex's largest cost (1.8-3.4 ms a frame, pan profile section 7), and the idea counts on them going away. A per-draw
   lamp loop keeps that cost. Avoiding it needs a lamp-index grid texture and dependent reads, which adds slots and a
   sampler.

## 3. Constraints and workarounds

### 3.1 Light buffer in screen space, written before the game's draws

The proposal: Apex accumulates lamp light into a screen-size target first; every lit shader then reads it at the pixel's
screen position in place of the room map.

For it:

- The patch is tiny and the same everywhere: replace the uv of the one room-map `texld` with `vPos × (1/w, 1/h)`. About
  2 slots, 1 constant, **0 new samplers** (the buffer is bound in the light map's own slot through the `SetTexture`
  swap), 0 inputs in `ps_3_0` (`vPos` is not one of the ten).
- Shadows and probes are evaluated in an Apex shader with no limits.

Against it:

- **It needs this frame's depth before anything is drawn.** The only sources are a depth pre-pass (every opaque draw
  issued twice: +1,000 to +2,500 draws a frame, on a game that is limited by its render thread and by the bridge's cost
  per D3D call) or last frame's depth reprojected, which breaks at every newly uncovered pixel during a camera pan (the
  case this project is measured on) and lags by a frame on moving Sims.
- `ps_2_0` has no `vPos`: those shaders need a VS change and an interpolator.
- One value per pixel: blended layers (glass, hair, particles) and MSAA edges get the light of whatever opaque surface
  is behind them.
- The room map's alpha (ambient weight) and the four directional maps have no place in one RGB buffer; normal-mapped
  surfaces lose their directional response.
- Every other view needs its own buffer or a fallback: mirrors, the water reflection target, thumbnails, Sim
  portraits, the pick pass.
- It still needs a patched copy of every family, so the 60-96% acceptance problem stays.

**Verdict:** not as the main path. A screen-space pass remains useful only *after* the scene, as a multiplicative
detail term next to Ambient Occlusion (contact shadows), where the INTZ depth share already exists.

### 3.2 Light buffer in texture space: Apex-owned copies of the room maps (recommended)

Same goal (minimal per-family change), without the depth problem: accumulate the light **per light-map texel** instead
of per screen pixel, and bind the result where the game binds its map.

Apex already ships most of the machinery (**V-doc**, `rt-outdoor-lamps.md`, `gpu-room-lamps.md`):

- a video-memory copy of each story's wall atlas, floor, ceiling and object maps of the active lot and its neighbours;
- a per-texel position source ("UV map") and a float accumulation target (A32B32G32R32F), 16 lamps a draw;
- a shadow atlas with a cube map per lamp;
- a `SetTexture` callback that binds the copy wherever the game binds the map: "every draw that reads room 0's light
  gets the lamps without a shader patch". Measured cost: 0.064 ms a frame over 3,728 `SetTexture` calls.

What changes for real-time GI:

| Today | With GI |
|---|---|
| Shadows from an analytic proxy of walls, roofs and slabs | Shadow maps drawn from the game's own draws (furniture, Sims, foliage) |
| Base = the game's kept solve minus the lamps Apex draws | No base: direct light + probe grid, the game's solve off |
| Copy packed back to 8 bits, hue kept past 1 by a second target | Copy can stay a float texture; the shaders multiply after the fetch |
| Copy has the game map's size | Copy can be larger: the uv is normalised (`× 1/4096`, `× 1/32767`), so 4× the texels gives 16-32 texels a metre |
| Directional maps patched per edited lamp on the CPU | Directional maps rendered from the probe grid: they *are* a 2D probe grid (4 horizontal directions, 0.5 m above the floor) |

What it gives: walls, floors, ceilings, the lot ground and (through the directional maps) indoor counters, objects and
normal-mapped floors read the new light **with their stock or already-patched shaders**, in every view, including
`ps_2_0` shaders and the 16-sampler ones. The game's tone curve, ambient term and bloom mask apply as before. Static
light is cached; only texels near a changed lamp or a moving caster are redrawn, by a handful of Apex draws.

Open points (all **inferred**, each needs a check):

1. **Texel → world position without the solve.** Today the position comes from the solve's samples. With the solve
   off, Apex must make it: draw the wall and floor geometry once into light-map space (clip position = light-map uv,
   world position and normal as outputs). Whether the wall atlas layout and the vertex uv's exist before the first solve
   is not verified.
2. **Does any shader use `LightmapSizeParameters` for texel offsets?** It is referenced by ExteriorFloors, Counters,
   the lot ground pass and others. A larger copy must not break a hand-made filter. Apex's own smooth-read copies take
   the size per draw, so they adapt.
3. **Shadow sharpness is bound to texel density.** 4 texels a metre cannot show a chair leg; 16-32 can. Memory: a
   2048² RGBA16F map is 32 MB of video memory per map and story; A2B10G10R10 halves it. Under the bridge this is the
   64-bit process's memory.
4. **Indoor objects stay coarse.** One light value per metre per direction, one height per story, combined by `max`.
   Good enough for bounce light; no shadow of one object on another. A denser directional-map copy helps; real
   per-pixel shadows on objects need the patch of section 3.3.
5. **Channel meaning must be kept:** rgb lamp light, alpha ambient weight (0 on room-0 texels, Apex's floor mask on
   outdoor floors).

### 3.3 What still needs a per-family patch

Only surfaces that read no light map:

| Surface | Route | Effort driver |
|---|---|---|
| Outdoor rig-lit objects (Phong / Counters exterior, fences, stairs) | Extend the existing `PatchObjectLampVs/Ps` copies: world position is already in TEXCOORD8 and a lamp block exists. Add the shared atlas sampler, a shadow term per lamp and the probe sample | Existing matchers; 426 of 444 Phong and 136 of 184 Counters already accepted |
| Sims | Section 4 | New matcher for SimSkin; ps_2_0 for eyes and robots |
| Foliage, trees | Lamps are per instance in a vs_2_0; ps_2_0 pixel shaders. Probe sample in the **vertex** shader (no texture reads in vs_2_0: so the value must come as constants per instance) or leave to the rig route of section 4 | ps_2_0 / vs_2_0 only |
| Roofs | Apex already replaces the PS; add shadow + probe terms to `roof_ps.hlsl` (own shader, no limit issue) | Small |
| Lake | Apex already adds a pass (`water_lamps_ps.hlsl`); same | Small |
| Glass, pools, sea, emissive | Leave as they are in a first version | - |
| World terrain, roads | Keep the terrain stamp; out of the lot grid's reach | - |

### 3.4 Other workarounds considered

| Workaround | Verdict |
|---|---|
| All lamp shadow maps in one atlas | Required in every design; already Apex's layout |
| Probe volume as a 2D slice atlas | Required for any per-pixel probe read; 2 reads per coefficient set |
| Shadow atlas and probe atlas in one float texture | Worth doing: makes the per-pixel route cost 1 sampler |
| Extra additive pass per surface (draw again with an Apex lamp shader) | Works without touching the game shader (Apex does it for water and snowy roofs) but doubles the draws of every lit surface: wrong direction for a CPU-bound render thread. Keep for rare surfaces only |
| Dropping the game's `LightMap` + directional maps from a patched copy | Frees 1 or 5 samplers and 1 input; solves every "full" room-lit shader |
| Deferred shading with an albedo target | Not possible without rewriting every shader's output; the tone curve and bloom mask are applied in the forward shader |

## 4. Sims and objects: the rig path

### 4.1 Today (V-doc unless marked)

- Every SceneModel (objects, fences, plants, Sims) has a **rig**, evaluated once at the model's centre: slot 0 the sun,
  slots 1-3 the three strongest lamps, 4 overflow "vertex lights". The binder `0x006B8B30` hands pointers to the rig's
  arrays to the shader parameters `LightDirections` / `LightColors` / `VertexLightDirections` / `VertexLightColors`.
- The rig's mode picks the technique: mode 0 (roofed room) uses the **interior** technique, modes 1 and 2 the exterior.
- **Interior objects** (Counters, SingleObject, InstancedObject) ignore the rig and read the lot `LightMap` and the four
  directional maps. Interior Phong passes read `LightMap` and the room's probe cubes but no directional maps
  (**V-precomp**).
- **Outdoor objects** read the rig in the pixel shader (`c1..c3` / `c5..c7`), the vertex lights through COLOR0 and the
  sky cube. No room map, no terrain stamp.
- **Sims:** no Sim technique binds `LightMap` or a directional map (**V-precomp**: SimSkin, SimHair, SimEyes, SimRobot,
  SimHorse, CASSkinSinglePass). They are lit by the rig constants, the vertex lights, `DiffuseLightProbe` /
  `SpecularLightProbe` cubes (the room's probe, or the flat grey `CASDiffuseProbe` for room-mode objects), `SimShadow`
  and the `SimLightingTweaks` constants. A room-mode rig has no sun: slot 0 is the strongest room light, and an unlit
  room gets the three `[NoLight]` fill lights.
- `gpu-room-lamps.md` says the directional maps are read by "objects and Sims". The shader bindings do not show that
  for Sims. If Sims use those maps it is on the CPU (the rig gather), which is **not verified**: needs a look at the
  room-mode gather `0x006BBDE0`.
- Room-mode rigs keep the lamps of the moment they were placed; the game does not refresh them at dusk.
- Apex: boosts lamp colours in the rig gather, forces the point-light bit on closed models, adds a cell gather for
  fenced yards, and `RigTracker` knows the rig (mode, centre) of the draw in progress. Sims get no lighting patch.

### 4.2 How they would read the new light

Three routes, cheapest first. They stack.

| Route | What | Shader patches | Gives | Lacks |
|---|---|---|---|---|
| **A. Fill the rig** | At the binder (or by writing the rig), Apex sets slots 1-3 to the three strongest lamps at the model's centre, each scaled by its visibility there, and the 4 vertex lights to a 4-direction fit of the probe grid's indirect light | **None.** Works for Sims, outdoor objects, foliage rigs, every ps_2_0 shader | Sims and objects follow lamp edits, walls block light, bounce light has a direction and a colour | One sample per model: no shadow edge across a Sim, a long object is lit as a point. Needs the probe value at a point **on the CPU**: a render-target read-back costs about 2 ms under the bridge, so the CPU needs its own small copy of the grid or a batched read every few frames |
| **B. Per-object ambient cube** | For each visible Sim, render a tiny irradiance cube from the probe grid on the GPU and bind it in place of `DiffuseLightProbe` for that draw (the `SetTexture` swap + `RigTracker`) | None | Smooth per-pixel indirect light by normal, no read-back | How each Sim shader weights the cube is not captured yet (`SimLightingTweaks`); one cube render target per Sim in view |
| **C. Per-pixel patch** | New matcher for SimSkin (654 ps_3_0: at most 158 slots, 12 samplers, 9 inputs) on the model of `PatchObjectLampPs`: world position in a free texcoord, the shared atlas, shadowed lamps and the probe sample | Yes | Furniture and wall shadows falling across a Sim | SimEyes / SimRobot are ps_2_0 only; 2 SimHair shaders have no free input; skinned VS need the skinned world position, not the model matrix |

Start with **A** for Sims and for anything not otherwise covered: it makes them consistent with the rooms at once and
needs no shader work. Add **C** for SimSkin once floors and walls are proven, because a Sim without a shadow edge next
to a floor with one is the visible mismatch.

The game's rig cap (`0x006B92A0`) and the 0.1 luminance cut in the gather apply to what the game gathers; a rig Apex
fills after the gather bypasses both, so the fill must apply its own ceiling to keep Sims from blowing out (**inferred**).

## 5. Recommendation

1. **Main path: texture-space light buffers.** Extend real-time outdoor lamps / GPU room lamps into the GI's shading
   back end: Apex-owned float copies of the room maps and directional maps, filled from shadow maps and the probe grid,
   bound by the existing swap. No game-shader patch; all room-lit families, both shader models, every view.
2. **Rig fill (route A)** for Sims, outdoor objects and foliage in the same first version.
3. **Per-pixel patches only where a light map does not exist and a shadow edge matters:** outdoor objects (extend the
   existing copies), then SimSkin. Roofs and the lake are Apex's own shaders already.
4. **Reject** the screen-space buffer before the draws (needs a depth pre-pass or stale depth) and the lamp loop in
   every family (breadth, and it keeps the per-draw CPU cost the idea wants to remove).

For the first proof in the idea ("shadow-mapped lamps patched into the floor and wall shaders"): do it in texel space
instead. One lot, floor and wall map copies at 4× resolution, shadow maps from the game's draws, room solve off. It
tests the same question (hitches and the look of the shadows) and reuses shipped code instead of a new patcher.

## 6. To verify next

| Question | How |
|---|---|
| Wall atlas layout and vertex uv's with the room solve off | Static: who allocates the atlas strips (`0x0069E950` commit path, room `+0x680`); then one run |
| `LightmapSizeParameters` users | Disassemble the readers in ExteriorFloors, Counters and the lot pass from the precomp |
| A larger copy bound in place of the game map: filtering at strip edges of the wall atlas | Offline render with a captured atlas |
| Slot costs of section 2 | Compile the lamp, shadow and probe snippets with `fxc`, `ps_3_0` |
| Which of a family's ps_2_0 / ps_3_0 variants a high-detail game actually uses | The Banding Fix coverage log already counts both per frame |
| Sims and the directional maps (CPU side) | Read the room-mode rig gather `0x006BBDE0` / `0x006BB2F0` |
| How SimSkin weights `DiffuseLightProbe` | One F7 capture of a Sim indoors |
| The 40,230 passes without a parsed texture list | Dump a few; confirm they bind no named texture |
