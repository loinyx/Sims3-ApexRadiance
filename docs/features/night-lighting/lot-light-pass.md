# Lot light pass (street lamps inside lots)

Street-lamp light continues from the world grass onto lot grass instead of stopping in a straight line at the lot
border. Near the border the two sides blend over 3 m, so a lamp standing next to a lot edge leaves no step. Part of
[Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released (part of Night Lighting since 0.1.0). Lot UV alignment, daylight terrain term and the constant-read guard: Released in 2.6.0. Rain and other weather variants recognised (exact rain variant and by structure): Released in 2.7.0. Lamp shade on lot ground (*Walls shade lamp light near the ground*) and lamps through the outside walls' openings: Released in 2.13.0. Crown shade from trees and bushes: Released in 2.14.0. A shade that takes only the light of the lamps a wall blocks, closed gates and narrow fence gaps, the room mask and lamp shade on snowy lots, the lot map aligned on each axis, *Indoor light adds to the ground*, and a lot's shade shown with its chunks' texels: Released in 2.15.3. Verified winter terrain layouts and exact fence-gap length reuse: Released in 2.15.6. Light past 1 with the walls' shade on it: Released in 2.16.0. Fence grid and half-float reads without x87 control-word calls: Released in 2.16.2. Released in 2.18.0: mesh fences pass their open share, fence gaps and gates only for real gates, shade updates within a quarter second. Released in 2.18.1: a fence type removed whole drops its shadow with the camera still. Released in 2.18.3: a wall's shade on the world grass continuous past the lot's edge. |
| Default | On |
| Menu | Lighting > Game fixes > Ground & Lots > *Street lamps light lots* |
| Configuration | `[patches.NightTerrainRelight]` `luzDoPosteNaGramaDoLote` in `ApexRadiance.toml` |
| Source | [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) (`kReplacementHlsl`, lot branch of `OnDrawInnerCore`), [`patches/night_terrain_relight_patch.cpp`](../../../patches/night_terrain_relight_patch.cpp) (game patches in Individual options) |

## The problem

World grass and lot grass are lit by two different formulas (Light Probe captures `LightProbe-lote` and
`LightProbe-mundo`):

- **World grass** is drawn by the terrain chunk shader, which adds `tex2D(s8 terrainLightMap, uv) * c7.x` to sun and
  sky. The map is 256x256 per 256 m chunk: the pre-baked "stamp" of the lamps, wide soft circles.
  uv = `(position - chunk centre) / 256 + 0.5`; the chunk centre is in VS c8.w / c10.w.
- **Lot grass** is drawn by several lot terrain passes. Its light pass is a 568-byte pixel shader, blended modulate2x
  (DESTCOLOR / SRCCOLOR), that adds `tex2D(s1 lotLightMap) * c3.x`. The lot light map is solved on the CPU by
  `FUN_006be020`: colour x light+0x130 x intensity x N.L / d^2, with d measured from the lamp head. Street lamps arrive
  faint.

The two formulas meet at the lot border: a straight cut. The lot pass vertex shader already outputs the terrain-map uv in
TEXCOORD1 (mapping in VS c14, chunk centre in c15.xz), so the fix only needs the pixel shader. See
[engine/terrain-and-light-bake.md](../../engine/terrain-and-light-bake.md).

## How Apex Radiance solves it

The lot light pass is redrawn with a replacement pixel shader that takes `max(lot light map, terrain light)`, where the
terrain light is the [world light atlas](world-atlas-and-smoothed-maps.md) (or, before the atlas exists, the light map
of the lot's home chunk). The terrain map contains lot lamps too when [terrain relight](terrain-relight.md) bakes them.

Per draw, when the bound PS is `PsClass::LotLight` (exact `kLotLightPs`, 568 bytes, FNV-1a `0xFDAD274B`, or one of its
weather variants, below):

1. The replacement `kReplacementHlsl` (ps_3_0) is created from the start-up bytecode (`EnsureReplacement`, `CompilePs`);
   status "Active" or "Failed: <error>".
2. VS c14..c15 are read; c14.xy must be 1/256 (tolerance 1e-5), otherwise the game draws.
3. Terrain source: the world atlas when ready (c14 rewritten for this draw), else the home chunk's map.
4. Soft lot edges: the lot rectangle is matched and PS c28..c30 built.
5. Lot map uv alignment and gains go into PS c31.
6. The original PS c28..c31 are read; if that read fails, the game draws unchanged.
7. Terrain texture bound to s2, replacement drawn, everything restored.

**Weather variants.** In rain the game draws lots and world terrain with variants of the same passes: the dry shader with
its defined constants moved to other registers and a different tail that darkens the colour with wetness constants
(`c5.x`, `c6.x`). Exact identities miss them, which left a cut at every lot edge when it rained. The rain lot pass
(`kLotLightRainPs`, 608 bytes, `0xA8E5F9F8`) is drawn with a second build of the replacement that keeps the game's wet
darkening (`col *= 1 + c5.x x (c6.x - 1)`); the rain world pass (`kWorldMultiLightRainPs`, 796 bytes) reads its lamp map
the same way as the dry one. Any other variant is recognised by structure ([`shaders/shader_structure.h`](../../../shaders/shader_structure.h)):
the body up to the first colour-output write, with defined constants renumbered, must have the dry body's token count
and hash (`kLotLightStruct`, `kWorldMultiLightStruct`), and the tail must be the dry final instruction written to a
temp, followed only by arithmetic on temps and constants the body never reads, writing `oC0`. A lot variant with the
rain tail (`c5`, `c6`) keeps it; one with another tail is drawn as the dry pass (logged). Only counts and hashes are
kept, never the game's bytecode. The snowy and melting-snow lot passes are separate shaders ([snow.md](snow.md)).

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Street lamps light lots | `luzDoPosteNaGramaDoLote` | bool | on | | This pass; off also disables roads, floors, fences, snow and per-pixel objects (everything after the wall handler in the dispatch) |
| Smooth ground light | `mapaDeLuzSuavizado` | bool | on | | Needed for the atlas; without it the pass uses the home chunk's game map |
| Ground light | `luzNoChao` | float | 100% | 25 to 300% | Night-weighted scale of c3.x (60% of it at 100%); the lot's own map (c31.x) at its full colour |
| Rooms at Night > Indoor light adds to the ground | `luzInternaSomaNoChao` | bool | on | | The lot map's indoor light added after the max and the shade (c34, *Indoor light on the ground* below) |
| Developer: Soft lot edges (A/B) | `bordaSuaveLote` | bool | on | | Registered only in developer mode (always on otherwise); pushed every frame (`SetSoftLotEdges`) |
| Individual options | `gramaDoLoteUsaLuzDoLote` | bool | on | | See *Game patches in Individual options* |
| Individual options | `qualidadeAltaEmTodosOsLotes` | bool | off | | Experimental |
| Individual options | `postesAcesosNoCalculo` | bool | on | | See *Game patches in Individual options* |

The full table is in the [Night Lighting overview](README.md#settings).

## Compatibility and interactions

- [World atlas and smoothed maps](world-atlas-and-smoothed-maps.md): source of the terrain term. A chunk map that
  changes is never read stale: `Find` and the atlas return the game's current map or a plain copy until the smoothed
  map is ready.
- [Terrain relight](terrain-relight.md): the terrain map must contain the lamps (dusk rebuild, lot lamps in the bake)
  for the max to help. The verified lot pass also reports which lots are visible, which drives the arrival relight.
- Roads, floors, snow, fences and objects all require *Street lamps light lots* (dispatch order).
- The snowy lot pass is a different shader, bytecode-patched rather than replaced: [snow.md](snow.md) "Snowy lot ground".

## Limitations

- The terrain stamp has no wall occlusion. A per-lot room mask (4 texels a metre from the rooms of the two
  lowest stories, `LevelLightShare::LotGroundMask`, s3 + PS c32/c33; the snowy passes s13/s14/s15 + PS c200/c201) keeps it
  out of rooms, roofless walled yards included; fences still let it through. The mask's alpha carries the lamp
  shade (below): outside walls shade the lamp term; fences, plants and objects do not. The world grass around the lot takes
  the same shade, and so do roads and sidewalks (2.13.1, "World grass around lots" below).
- The walls' doors, windows, arches and half-wall cells let the lamps through (`OutsideWallsIn` with the
  walls' patches, [level-light-share.md](level-light-share.md#openings-in-the-outside-walls-copies)): each texel keeps
  each lamp's share past every wall (an opening's rim gives part of it) instead of a blocked bit; a wall with no known
  opening is whole, as before.
- The lamp shade is one factor for all lamps together (the share of the bake lamps' stamp the walls leave, the lamps
  not tested counted as unblocked): on the grass, the story-0 floors and the instanced groups near the ground it
  multiplies the whole lamp term, which is exact only where that term follows the stamp's weights; the lot map's own
  light near a wall takes the same factor.
- A gap in a fence line is closed only between two fence pieces in line 0.4 to 2.5 m apart, by an outdoor object drawn
  in it, and gates are seen only while the per-pixel object lamps draw (*Lamps light objects* and *Doors and windows
  stay lit*); any outdoor object standing there (a plant, a sign) closes it too, and an open gate shades as a closed one.
  Narrower gaps are always closed. Unverified in game.
- A lamp post standing just past its lot's edge (one tile between an outside wall and the edge) can leave the grass around
  it dark: the shade shown there held about 0.2 where the shade worked out from the same lamps and walls gave about 0.7
  (the post unblocked). Whether the shade shown was stale or the build differs at the lot's edge is not established.
- The winter lot pass with VS 436BB272 (m58, "prefeitura") is not handled ([snow.md](snow.md)).
- The snowy lot pass has no soft edge.
- A porch lamp within 3 m of the edge fades towards the atlas near the edge (with *Lot lamps light the street* the atlas
  contains it, so the change is small).
- A lot the tree walk does not list (no lighting manager, or room 0 not rebuilt: +0xC0 == 0) draws the plain max
  (counted "without a lot rectangle").
- Lots on chunks drawn only by unrecognised world terrain variants never get a chunk registered; without the atlas they
  count as "without terrain texture".
- Lot UV alignment applies only to the exact recognised lot VS; other layouts keep native coordinates.

## Technical reference

Winter terrain identification follows the actual texture read rather than a fixed sampler slot. The compact terrain
path accepts its validated weather vertex layouts; the bound map still needs the expected dimensions and mapping.
Known weather masks are excluded. Unrecognised layouts keep the existing fallback. Fence reconstruction computes each
piece length once; each derived gap retains its exact length until the geometry is rebuilt, including gate tests.


### The replacement shader (`kReplacementHlsl`, ps_3_0)

| Register | Content |
|---|---|
| c0 | sun colour (game) |
| c1.xyz | sun direction (game) |
| c2 | shadow-map offsets (game) |
| c3.x | lamp scale (game; times the ground brightness for this draw) |
| c4.x | sky scale (game) |
| c28 | soft edges: `(dLx/du, dLx/dv, Lx0, W)`: lot-local x in metres = `dot(float3(terrainUv, 1), c28.xyz)`; W = lot width |
| c29 | soft edges: `(dLz/du, dLz/dv, Lz0, D)`: lot-local z; D = lot depth |
| c30 | soft edges and lot uv offset: `(1 / band, bias, offset x, offset z)`: xy `(1/3, 0)` on, `(0, 1)` off (w = 1 everywhere); zw = `-c12.x x 16/63`, `-c12.y x 16/63` for the recognised VS, else 0 (*Lot map uv alignment* below) |
| c31 | `(lot map gain, daylight terrain term, lot uv scale, 0)`: x = `LotMapGain()` at full night, the lot map's night gain before it; y = 0 (since 2.12.3: before full night c3.x itself is set to ground gain x `LampDay(night)` for one draw, `TerrainLightingPolicy::LotPass`, so the terrain and the lot map follow the shared daytime curve; until 2.12.2 y = `(1 - night) x ground gain`, the full gain by day), z = `1/63` for the recognised VS, else 0 |
| c34 | `(gate, 0, 0, 0)`: 1 while *Indoor light adds to the ground* is active, else 0 (*Indoor light on the ground* below) |
| c35 | `(log2 range, 0, 0, 0)`: 3 while s4 holds the lot map's outdoor scale ([rt-outdoor-lamps](rt-outdoor-lamps.md)), else 0; restored after the draw |
| s0 | sky cube (game) |
| s1 | lot light map (game) |
| s3 | room mask (rgb) and lamp shade (alpha), A8R8G8B8, 4 texels a metre; c32 / c33 its uv rows |
| s2 | terrain light (atlas or chunk map) |
| s4 | outdoor scale of the relit lot map (`RtOutdoorLamps::ScaleOf`), or nothing |
| s5 | sun/moon shadow map (game) |
| TEXCOORD1.xy | terrain uv |
| TEXCOORD2 | shadow projection |
| TEXCOORD4 | normal |
| TEXCOORD5 | lot map uv |

Body, equal to the game's pass except the lamp lines:

```
sun     = lerp(avg of 4 tex2Dproj shadow taps, 1, edge fade) * saturate(dot(n, c1.xyz))
terrain = tex2D(sTerrain, terrainUv.xy).rgb * (c3.x + c31.y)
lp      = (dot(float3(terrainUv.xy, 1), c28.xyz), dot(float3(terrainUv.xy, 1), c29.xyz))   // lot-local metres
e       = min(lp, (c28.w, c29.w) - lp);  w = smoothstep01(saturate(min(e.x, e.y) * c30.x + c30.y))
lotUv   = lotUv * (1 + c31.z) + c30.zw
lot     = tex2D(sLot, lotUv);  lm = lot.rgb * c31.x * c3.x                                // game: tex2D(sLot).rgb * c3.x
k       = exp2(tex2D(sScale, lotUv).r * c35.x)                                           // the light past 1 (1 without it)
a       = lot.a * c34.x;  share = sat(a / max(k lum(lot.rgb), 1/255))                     // indoor light on the ground
ex      = k lm share;  own = lm (1 - share)
lamps   = lerp(terrain, max(own, terrain * sat((1 - a) 255)), w) * shade + ex * w + (k - 1) own w shade^4
col     = sun * c0.rgb + lamps
col     = texCUBE(sSky, n).rgb * c4.x + col
return float4(col * 0.5, 0)                                                          // modulate2x
```

The light past 1 takes the walls' shade to the 4th power: the shade is soft over a texel at a wall's foot, and behind a
screen wall its sconce lit the texel straddling the foot at 0.3 to 0.5, a strip of light behind the wall once the
light past 1 showed. With k = 1 (no scale) the pass is the one of before.

At night c31.y = 0 and the result equals the earlier night shader. By day the terrain term gains
`(1 - night) x ground gain`, matching the world terrain's daylight term ([world-atlas](world-atlas-and-smoothed-maps.md));
the native lot/window map scale is unchanged. fxc check: `fxc /T ps_3_0 /E main` on the string's contents.

### Indoor light on the ground

(*Indoor light adds to the ground*, `luzInternaSomaNoChao`.) The lot map's alpha a holds the luminance of the light
lit rooms send onto the texel through their openings ([level-light-share](level-light-share.md#indoor-light-on-the-ground)).
Its share of the map, `ex = lm s`, `s = sat(a / max(lum(map), 1/255))`, leaves `max(lot, terrain)` and the walls' shade and is
added after them, so a window's light adds to a street lamp's pool and stays whole behind a wall that shades the outdoor
lamps. The terrain weight `sat((1 - a) 255)` is 0 only at a = 1 (a roofless room's own floor: the map alone); E is at
most 254/255. `ex` follows the lamp scale c3.x (the daylight curve) and the soft edge w. With c34.x = 0 every term is the
pass of before (0 LSB against the previous shader over the terrain lighting check's 60 scenarios); with c34.x = 1 the
pixels match the CPU formula within 2 LSB. The snowy passes take the same terms ([snow](snow.md)).

### Terrain source

- **World atlas** when `LightmapSmooth::Atlas(a)` is ready. The summer lot VS computes the map uv as
  `(world.xz - c15.xz) * c14.xy + c14.zw`; for the atlas (`uv = world.xz * a.xy + a.zw`) c14 becomes
  `(a.x, a.y, a.z + c15.x * a.x, a.w + c15.z * a.y)` for this draw. c15 also feeds another uv (c13), so only c14 changes.
  The atlas coordinates are zero-initialised, also when smoothing is off or the atlas is not ready.
- **No atlas**: the chunk texture recorded from the world chunk draw with key `Key(c15.x, c15.z)` (the home chunk), or
  its smoothed version (`ChunkTexture` -> `LightmapSmooth::Find`). With no recorded chunk, `g_lotMissing++` ("without
  terrain texture") and the game draws.
- s2 is set to CLAMP/CLAMP, LINEAR min/mag/mip, sRGB off through `SamplerBind`; texture, sampler states, c14, PS
  c28..c31 and the PS are restored after the draw. Counter `g_lotDrawn` ("lot light fixed: N draws").

### Lamp shade on lot ground (`GroundShadow`)

The atlas is the terrain stamp, baked with no occluder, so a free-standing wall next to a garden post left the grass
behind it fully lit. Per lot, on the room mask's grid, `features/ground_lamp_shadow.h` computes for each texel
`M = 1 - sum w_i (1 - v_i) / max(sum w_i, 0.05)`: `w_i` the stamp weight of each bake lamp
(`sat(0.4 x lum(colour x intensity x range) x cos / d^2)`, 0 outside the lamp's stamp rectangle, where the bake writes
nothing), `v_i` 0 when an outside wall (the copy *Buildings block lamp light on trees and objects* tests) stands between
the head and the ground texel. The sum runs over two lists (`ShadeLamps`): the lamps tested against the walls (rect over
the lot, the 32 strongest by their weight at the lot's nearest point) and the others (`ShadeJob::Init`'s `others`: past
those 32, or with a rect over the shade grid's 24 m margin only; at most 96, strongest first), which enter the total with
`v = 1`. So a wall's shadow takes away only the light of the lamps it blocks, and it ends where its lamp's light on the
ground ends: a lamp's reach stops at its rect's farthest corner (`GroundShadow::Reach`). Both lists are part of the lot's
input key. The build computes each lamp's weight once over the box its light is summed over and skips the wall and
crown tests of texels outside it (the same bytes; `tools/ground_shade_test`). A wall within 1 m of the lot's
ground is tested with the point at its own base; a crossing within 0.15 m of the head is the lamp's own face (a sconce).
Two `[1 2 1]` blurs soften the edge. Inside rooms the alpha is 1 (their own lamps light them). The pass multiplies the
whole lamp term (lot map, atlas and the soft edge's target) by it; the snowy passes get `mul r0.xyz, r0, rM.w` after their
max, rM a temp the game's shader does not use (its highest + 1, at least r8; it was r6, which the game's snowy lot shader
uses, so the masked copy was always refused and snowy lots drew with no mask or shade beside shaded world grass). The
build runs on the shade worker (*Worker thread* below) and again only when the lot's place, its lamps (place, about 20%
of light, in or out of the bake) or the outside walls, fences and crowns in its box changed. The inputs are checked every
100 ms while a fence group changed since the lot's last check or a lamp is dragged (`kShadeCheckMs`), else every 500 ms
(the key reads every bake lamp and the walls' copy), and a lot is built at most every 250 ms
(`kShadeMinEveryMs`; a build takes 1 to 30 ms on the worker), so a fence edit or a dragged lamp shows its shadow without
half-second steps.
It follows *Walls shade lamp light near the ground* (Lighting > Outside > Ground & Lots, `paredesSombreiamOChao`). Status (developer page): *Walls shade lamps on lot ground*. The lot's
ground height is taken from room 0's matrix (`m13`, unverified as the ground under every texel).

Outdoor floors on the ground (story 0) read the same texture: their shaders multiply `max(floor map, atlas)` by the
alpha within about 1 m of the lot's ground (`FloorShadeBind`, `LotMaskTexture`; [floors.md](floors.md)). A floor draw
finds its lot by its world rows (`FindFloorLot`: `FindLotRect`, else the same rotation with the rows' origin inside the
lot rectangle). Fences, curbs, railings and stairs (instanced groups) take it too, on their per-pixel lamp sum and within
the same 1 m: their lot is the one under the group's centre (`FindLotAt`; [fences.md](fences.md)). Both lookups keep
their last answer while the inputs and the lot list are unchanged ([per draw](README.md#per-draw-lot_light_bridgecpp)). The alpha is
one factor for all lamps, so a surface it shades is dimmed for every lamp, also one that no wall hides from it.

**World grass around lots** (2.13.1; F8 and F7 2026-10-14 23-27: a wall's shadow stopped at the lot edge and the lamp
light that reaches past the lot lay over it). The world grass takes the bake lamps from the terrain chunk's light map,
with no wall. The shade is now built on the lot's grid grown on each side as far as its lamps reach past the lot
(`GroundShadow::Reach`, at most 24 m; `ShadeGrid`), at the same texel centres: the lot's part feeds the mask's alpha as
before, and the whole grid, when it shades any texel, goes to the world pass. One texture per terrain chunk (`ChunkShade`,
L8, 1024 x 1024 over the chunk's 256 m, CPU mips) holds `1 - max(lost) / max(W)` over the lots there, with
`W = max(stamp weight, kOtherLight)` and `lost = (1 - M) W` the light that lot's walls take (255 where no lot is):
over one lot it is that lot's shade, the lot pass's value on its side of the edge. Each lot tests only its own walls, so
weighing the shades (`sum W M / sum W`) let a lot next door that sees the same lamp but not the wall lighten the shadow
past the edge, a straight step there (a garden post 1.5 m inside a lot beside a low wall: 0.12 inside, 0.55 outside); a
wall two lots both know is not counted twice. It shares the chunk light
map's uv (`(world xz - centre) / 256 + 0.5`), so the world terrain shader only reads it at the light map's uv and multiplies
the lamp RGB (`ShaderPatches::PatchTerrainShade`, after the native-alpha patch and the daylight-range patch; the solar alpha
is untouched). When a lot's shade over a chunk changes, only the texels over the changed box and the mip texels over them
are made again (`GroundShadow::ChunkUpdate`, from the texture's own texels around that box; `ChunkFull` for a new chunk;
the same bytes either way, `tools/ground_shade_test`), and only each level's changed rectangle is locked and written
(about 160 KB for a 40 m lot instead of 1.4 MB). A chunk build carries a generation: a result older than the chunk's last
build sent is dropped. A chunk no lot shades draws as before. Without a smoothed map the game's own map is drawn through the same patched shader. Roads and sidewalks read
the chunk map at the same uv: their patched copy (`PatchRoad(..., shade = true)`, `g_roadShadePs`) multiplies
`max(road map, terrain)` by the same texture before the lamp scale. Developer status: *world
grass around lots: N chunks shaded*. Checked offline (`tools/terrain_lighting_test`): RGB x shade exact on SM2 and SM3, solar
alpha bit-exact, every truncation refused, the drawn multi-pass shader of the F7 accepted by D3D; a road fixture's light x
shade exact.

**Garden walls and fences** (2.13.1). A brick garden wall let a post's light over the grass behind it: garden walls are
fences for the game, not in the outside-wall copy. `DrawInstanced` notes each instanced group it draws (`NoteFenceGroup`,
read again every 60 s, dropped after 20 s unseen): the mesh's box over the draw's vertex range (POSITION0 / w) and, per
instance, its place (POSITION1) and turn (POSITION2 = cos, slope, sin; world = (px c - pz s, py + px k, px s + pz c) + place,
VS_21C41B90 of F7 2026-10-13 11-50-47). Each piece becomes a wall along its longer side from its base to its top, so the
shade follows each wall's own height (the lamp is tested over every wall's top). Posts (under 0.3 m both ways) and thick
pieces (over 0.6 m deep: stairs, decks) are left out. `ShadeWalls` gives the job both lists. A fence group's buffers are
read without waiting for the GPU: a busy buffer is retried on a later draw and waited for after three busy answers. The
game draws a fence again as a new group when a piece is added or removed; 300 ms after a new group's first draw
(`kFenceSwapMs`), the groups over the same place that are no longer drawn (nor in the last two frames) are dropped, so a removed piece loses its
shadow at once. A fence type removed whole leaves no new group: with the camera eye unmoved since a group's last draw (`LotLightingMotion::CameraStillSince`) and the world drawn, every group on screen is drawn each frame, so a group missing for 500 ms and two frames is dropped (`FenceGroupsGone`, once a frame; developer builds log each one). While the camera moves a group may only have left the screen: it goes with the 20 s sweep.

**Mesh fences** (chain-link, lattices, railings). A fence whose pixel shader cuts its mesh out with the texture's alpha
lets lamp light through its holes. `ShaderPatches::FindCutout` traces the shader's `texkill` back to its texture read
(`mad rK, rA.w, rS, -cR` from `texld rA, vT, sN`, optionally tinted), and `FenceOpenShare` reads that texture (the
first level at most 512 texels wide; DXT1, DXT3, DXT5 or A8R8G8B8, managed pool), keeps its share while the texture's
size, format and a few sampled bytes are the same (an address freed with a lot can go to another texture), and takes
the share of its
texels under half alpha (`FenceCutout::OpenShare`). The fence's walls pass that share (`GroundShadow::Wall::pass`): a
ray through the wall's body keeps `pass` of its light, the same in the hard and soft tests. The share is fixed at half
alpha because the game draws one fence with more than one alpha reference (150 and 50 in the log), which made the share
depend on the draw that read it. The share covers the whole texture, so a texture with unused transparent texels around
the fence's own area gives a share that is too high (*Open checks*).

**Gates and fence gaps** (2.15.3). A fence gate is an object, not an instanced piece, so the fence line had a gap where
it stands and a post's light passed through the closed gate. With the fence grid (`FenceGapsBuild`), every gap between
two pieces in line (ends 0.4 to 2.5 m apart, facing each other) is found from either end and kept once by its key. An end
covered by another piece in line joins it: brick fence meshes overlap by their posts (0.12 m), and each overlapped end
listed a gap over the next piece. An outdoor object drawn with its origin in a gap, at most 0.6 m off its line (a gate's
origin is its tile's centre, half a tile off; `NoteGate`, from `DrawObjectLamp`, rig modes 1 and 2), at least 0.2 m from
the gap's ends (the fence's posts stand there) and no more than 0.5 m over the gap's base, fills it with a wall from the
higher base to the lower top of the two pieces; no new gate is noted while a lamp is dragged, so a lamp carried through
or over a gap does not close it. A gap between two mesh fences passes the lower of their open shares. A gate stays while
its gap does; it goes only when it has not been drawn for 30 s while the fence on both sides was drawn in the last 2 s, so
off screen it stays closed. Gaps narrower than 0.4 m (a post, pieces a few cm short) are always closed. Developer builds log the gaps, the gates and the objects near a gap that were not taken.
The fence grid is built again whenever a fence group changes, which happens often while the camera pans; its piece
lengths and end distances use `ApexMath::Hypot` ([`features/fast_math.h`](../../../features/fast_math.h)), a double
`sqrtsd` of the exact squares, instead of `std::hypot`, whose x87 CRT path sets and restores the FPU control word on
every call (`__ctrlfp` and `__hypothlp` were the hottest Apex samples of the "Render frame (game)" hitches). The results
are the same bits. The half-float vertex and texture reads (`ReadElement`, the light probe, the outdoor delta) scale by
`Pow2` ([`features/half_float.h`](../../../features/half_float.h)) instead of `std::ldexp` for the same reason, with the
same bits.

**Tree and bush crowns** (2.14.0). The crowns seen in the night foliage draws are porous occluders of the same mask: a
lamp's ray through a crown keeps exp(-k u s) of its light, so the shade under a tree is soft and partial
([foliage.md](foliage.md#per-vertex-lamps-and-crown-shade)). The crown set is part of the lot's input key, so a lot
is built again when its crowns change.

**Worker thread** (2.13.1). The lot builds (`GroundShadow::ShadeJob`) and the chunk textures' texels and mips run on one
worker thread (`ShadeWorker`, below-normal priority); the render thread makes the inputs, uploads and applies. Before, a
build ran 1 ms a frame on the render thread and a shade could take seconds to show. At most two lots in flight, the lot
nearest the camera first; a lot's inputs are checked every 100 ms while they change, else every 500 ms. Results carry an epoch; `ReleaseLotMasks` starts
a new one and also releases the chunks' shades. A chunk's build goes before the lots' builds waiting. A lot's finished
shade is held, its old one still shown, while the chunks under its old and new shade are made with it; then its mask and
their texels are applied in the same frame, at the first draw that reads either (`ShadeApply`). One lot is held at a
time, and a held shade is shown after 3 s even with a chunk missing (`kHoldMaxMs`). Otherwise at most one chunk's texels
are uploaded a frame. The developer status adds the last chunk upload's size and whether a shade is held; developer
builds log, per lot whose inputs changed, the time to its new mask at a draw, by stage (at most 400 lines).

### Soft lot edges

Within `kEdgeBand` = 3 m of the lot rectangle, the lamp term blends from `max(lot, atlas)` to the atlas term:
`w = smoothstep(0, 3, distance to the nearest edge)`, `lamps = lerp(atlas, max(lot, atlas), w)`. At the edge w = 0, so
the lot grass shows exactly the terrain term the world grass shows on the other side; the smoothstep has zero slope at
the edge, so the lot side continues the terrain's own gradient; 3 m inside and beyond nothing changes. It can only
lower the lamp term (never below the atlas), so lot edges without a lamp are unchanged. Two lots that share an edge both
fade to the same atlas at it. Only this pass (lot ground) is touched.

- **Lot-local position.** The PS receives the terrain uv `(world.xz - c15.xz) * k.xy + k.zw` (k = the c14 the draw runs
  with). The lot pass VS has the lot matrix in c8 / c10 (`world.x = c8.x lx + c8.z lz + c8.w`,
  `world.z = c10.x lx + c10.z lz + c10.w`). `LotEdgeConstants` inverts both on the CPU (double precision) into
  `lotLocal = A * uv + b`, so the shader does two dot products and works for any lot rotation.
- **Lot size**: room 0 of the lot, +0xC0 / +0xC4 (tiles along lot-local x / z; 1 tile = 1 m). Decompile evidence:
  `FUN_006a2740` (room rebuild) sets room 0's +0xC0 / +0xC4 from the manager's tile grid size +0x264 / +0x268 (and
  +0x20 / +0x28 = size - 1, +0x1C / +0x24 = 0: the tile bounds); `FUN_0069efc0` walks tiles `[0, C0) x [0, C4)`; the
  manager's room-id grid +0x260 is bounds-checked with +0x264 / +0x268 in `FUN_006a4300`, `006a4890`, `006a4a00`;
  `FUN_006a4c10` publishes "LotSizeParameters" = (+0x264 / 64, +0x268 / 64); `FUN_006c6ab0` gathers world lights at the
  lot centre `(C0 / 2, 0, C4 / 2)` through +0xF8.
- **Matching the draw to its lot**: `RefreshLotRects` (Present, every 20 frames, or 5 frames after a lot pass found no
  rectangle) walks the light update tree like LightDiag (tracker + 0x6A0 + level x 0x1A4, levels `kLotLevels` 0..7 then
  -1..-4, manager room hash +0x234 / +0x238, room 0) and keeps `{origin m12/m14, m0, m8, W, D, lot id}` from room 0's
  +0xF8 matrix and +0xC0 / +0xC4. `FindLotRect` matches VS c8.w / c10.w within 5 cm and c8.x / c8.z within 2e-3.
  Room 0's +0xF8 matrix equals the lot pass VS c8 / c10 (verified on lot 09080020A1D28860).
- The matched lot id is also recorded as visible (`VisibleLot`: seen within 500 ms; first sight or reappearance after
  2 s counts as an arrival), even with soft edges off. [Terrain relight](terrain-relight.md) uses it.

### Lot map uv alignment (`kLotLightVs`)

The recognised regular lot VS (680 bytes, FNV-1a `0x0C8CC5E8`, `VsInfo::contractedLotUv`) maps lot-local xz to the lot
map as `(xz x 63/64 + 1/4) x c12`. That contraction made points just outside a closed wall sample the indoor row of the
CPU-solved lot map. When VS c12.x and c12.y are finite and in (0, 1], c31.z = 1/63 and c30.zw = -c12.xy x 16/63, so
`lotUv x 64/63 - c12 x 16/63 = local xz x c12` on each axis. A lot map need not be square: a 50 x 50 m lot's map is
512 x 256 (c12 = 1/128, 1/64), and with one offset for both axes its light lay up to 0.5 m off (a ground lamp's pool
0.25 m along x from the lamp, egg-shaped where it met the centred atlas stamp). Light intensity and texture read count are unchanged; the world atlas and
the CPU light map are untouched. Other VS layouts get c31.zw = 0 (native coordinates).

### Game patches in Individual options

Each started as one of the attempts recorded in the history and is kept as a switch. `postesAcesosNoCalculo` and
`gramaDoLoteUsaLuzDoLote` are on by default; `qualidadeAltaEmTodosOsLotes` is off and stays experimental. A change of
any of them reinstalls the patch.

| Key | Site | What it does |
|---|---|---|
| `postesAcesosNoCalculo` | 0x006BE18C `movaps xmm0,[esi+0E0h]` (`0F 28 86 E0 00 00 00`) in `FUN_006be020` (street-lamp class light evaluation, vfunc+0x4C, reads effective colour +0xE0) | `StreetLampColourStub`: for a light with the lit flag (+0x100 & 0x20) clear, type +0xB0 == 0xB and lot id (+0xC0 or +0xC4) == 0, returns colour +0xF0 x intensity +0x10 instead of +0xE0 (0 while off), so a lot solved by day still gets street lamps. Counter "Street lamps counted as lit". Any future lamp packer must copy this rule |
| `gramaDoLoteUsaLuzDoLote` | 0x00C7F87D `mov eax,[edi+0D8h]; test eax,eax` (`8B 87 D8 00 00 00 85 C0`); context `F3 0F 10 05 38 A5 07 01 F3 0F 11 44 24 18 74 13`; null-bind path at 0x00C7F8B7 (`A1 80 CE 1E 01 6A 00 6A 00`) | `LotPassStub` keeps the lot pass of `FUN_00c7f750` (per-chunk light/fog pass; lot pass when `[ebp+0Ch]` = 1) on "no terrain lightmap" after a full rebuild, because the rebuilt chunk+0xD8 has no street-lamp light inside lot footprints. Float at 0x0107A538 stored at `[esp+18h]`. The world pass is untouched |
| `qualidadeAltaEmTodosOsLotes` | 0x00ADB66B, 0x00ADB884 `mov byte [esp+0Ch],0` (`C6 44 24 0C 00`) in `FUN_00adb5a0` / `FUN_00adb850` | The quality flag passed to `FUN_006a5ef0` (active lot or Build mode) becomes 1: every lot solved at the active lot's quality. Lots loaded afterwards |

### Files and functions

| File | Function | Role |
|---|---|---|
| lot_light_bridge.cpp | `kReplacementHlsl`, `kReplacementPsId`, `EnsureReplacement`, `CompilePs` | Replacement shader |
| | `OnDrawInnerCore` (last block) | The lot pass redraw |
| | `LotRect`, `ReadLotRects` (SEH walk), `RefreshLotRects`, `FindLotRect`, `LotEdgeConstants`, `g_softEdges`, `kEdgeBand`; `SetSoftLotEdges`, `LotEdgeStatus` | Soft lot edges |
| | `VisibleLot`, `LotVisible`, `TakeLotArrivals` | Visible lots for the arrival relight |
| | `LotMapGain`, `GroundGain`, `ConstGain` | Gains |
| | `LotMask`, `LotMaskFor`, `LotMaskTexture`, `ShadeTick`; `FindFloorLot`, `FindLotAt`, `FloorShadeBind` | Room mask and lamp shade; the shade on story-0 floors and instanced groups |
| | `LotLightBridge::SetEnabled`, `Status`, `OnWorldChanged` (`ClearChunks`), `Shutdown(keepChunkMaps)`, `ChunkCount` | Lifecycle |
| shader_ids.h | `kLotLightPs` {568, 0xFDAD274B}, `kLotLightVs` {680, 0x0C8CC5E8} | Exact gates |
| lightmap_smooth.cpp | `LightmapSmooth::Atlas`, `Find`, `Get` | Terrain light source |
| night_terrain_relight_patch.cpp | `LotPassStub`, `StreetLampColourStub`, `kQualitySites` | Game patches in Individual options |

## Rejected approaches

- Changing the world light collection radius, re-solving room 0, rebuilding the type-5 terrain textures: no effect.
- High quality on every lot, the null terrain bind in the lot layer, street lamps counted as lit: no fix alone (kept as
  switches in Individual options).
- Home-chunk sampling with CLAMP: stretched the chunk's last row at a chunk border (m76); the atlas is used instead.
- Lot size from the lot map uv rect, or a coverage signal in the lot map, or vertex-buffer bounds: not reliable.

Details in [history](../../history/night-lighting-lot-light-pass.md).

## See also

- [Validation](../../validation/night-lighting-lot-light-pass.md)
- [History](../../history/night-lighting-lot-light-pass.md)
- [Terrain relight: lamp change tracking](terrain-relight.md)
- [World atlas and smoothed maps: chunk registration](world-atlas-and-smoothed-maps.md)
