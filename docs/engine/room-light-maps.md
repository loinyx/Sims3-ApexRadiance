# Room light maps and the lot light solve

This page documents the CPU light solve of lots: every lot story has a lighting solver whose rooms own a light list and bake light maps (walls, floors, lot grass, ceilings, object basis maps) texel by texel with `LightPointWithAllLights` (`0x0069FD60`). Night Lighting's lot light pass, walls, floors, level light share, unlit rooms and the Light Diag depend on it.

## Scope

| | |
|---|---|
| Game build | Steam 1.67.2 (`TS3W.exe`, image base 0x00400000) unless stated |
| Used by | [Rooms at Night](../features/night-lighting/unlit-rooms.md), [Faster Room Lighting](../features/performance/room-light-queue.md), [Lot light pass](../features/night-lighting/lot-light-pass.md), [Level light share](../features/night-lighting/level-light-share.md), [Walls](../features/night-lighting/walls.md), [Floors](../features/night-lighting/floors.md), [Light Diag](../features/dev-tools/light-diag.md) |
| Evidence | Ghidra `re\out` (`fn_006a31d0.c`, `fn_0069fd60.c` and others), `engine_map` `full.asm` and `dwords.txt`, F8 diagnostics, PASSO3-PLANO.md and NOTAS-ILUMINACAO.md (see Sources) |

## Overview

Engine reference for TS3W.exe 1.67.2 (Steam, image base 0x00400000). Lots are not lit by the terrain stamp: every lot
story has a CPU "lighting solver" whose rooms each own a light list and bake light maps (walls, floors, lot grass,
ceilings, object basis maps) texel by texel with `LightPointWithAllLights` (`0x0069FD60`). Room 0 of each story is the
outside of the lot. This page covers the lot/story/room hierarchy, how room light lists are gathered (lot lights, the
ground-story double count, world lights around the lot), when rooms are invalidated and re-solved, the per-texel solve
(per-light evaluation, threshold, wall occlusion, `min(sum, 1)` in `FUN_006A31D0`), the constants behind the lamp
formula, the per-floor map layouts, window lights, the night level, and the difference between the lot map and the
world terrain map / Apex atlas.
Feature docs: [lot light pass](../features/night-lighting/lot-light-pass.md),
[level light share](../features/night-lighting/level-light-share.md), [walls](../features/night-lighting/walls.md),
[floors](../features/night-lighting/floors.md). Related engine docs: [terrain-and-light-bake.md](terrain-and-light-bake.md),
[light-objects-and-rigs.md](light-objects-and-rigs.md), [lot-loading-and-streaming.md](lot-loading-and-streaming.md).

Every lit surface of a lot (outdoor walls, floor tiles, lot grass, interior walls and floors, ceilings, and interior
objects through basis maps) samples a light map solved on the CPU per room. Street lamps and lot lamps reach those maps
only through the room's light list. The "lamp cut at the floor line", "walls darker than objects", "lot grass without
street-lamp light" and "fenced yard without street lamps" bugs all live here.

## Details

### Sources

Sources: `level_light_share.cpp`, `light_diag.cpp`, `patches/night_terrain_relight_patch.cpp`,
`patches/lot_edge_lighting_patch.cpp`, `patches/lighting_quality_patch.cpp` (inherited S3SS),
`patches/split_level_lighting_fix_patch.cpp` (inherited S3SS), `patches/smooth_streaming_patch.cpp` (note 3/5),
`hdr_native.cpp` (header; used here only for its engine findings: Native HDR and its interior lamp gain existed only in the
combined build, see [../removed-features.md](../removed-features.md)), `S3SS-dev\re\out` (`fn_006a31d0.c`, `fn_0069fd60.c`, `fn_006c6ab0.c`, `fn_006c6990.c`,
`fn_006a4700.c`, `fn_006a7700.c`, `fn_006a20d0.c`), `scratchpad\engine_map\full.asm` and `dwords.txt`,
PASSO3-PLANO.md (facts F-J1..F-J6, section 3.2, section 6), NOTAS-ILUMINACAO.md sections 1, 4c, 6, "Andares" (both rounds),
"Revisao adversarial", "Passo 3", "Paredes mais escuras", "HDR nativo" (28/09).

### Data structures

#### Hierarchy

```
*(0x011D1860) + 0x1C0 = lightMgr ("lighting system")
  +0xD4  light update tree            (lot hash: buckets +0x58, count +0x5C; node: +8 = tracker, +0x10 = next)
  +0xF0  night level (float 0..1)
  +0x104 light cells
tracker (one per lot)
  +0x6A0 + L*0x1A4, L = -4..7   tree level ("treeLevel") of story L
treeLevel
  +0x00  manager (per-story lighting solver) or 0
  +0x04  tracker
  +0x28  set "rooms pending restart"
  +0x4C  set of dirty rooms (MarkRoomDirty)
  +0x90  light registry hash: buckets +0x98, count +0x9C; node +8 -> {begin, end} of entries, next +0x10;
         entry +0x1C = room id, +0x24 = light
  +0x1A0 level (int)
manager (per story)
  +0x00  lightMgr
  +0x88  real level of the story
  +0x90 / +0x94  lot id (lo / hi)
  +0x230 room hash: buckets +0x234, count +0x238; node: +0 = room id, +0x10 = room, +0x24 = "no_hash24", +0x80 = next
  +0x280 byte flag (unverified meaning; printed by F8 as flag280)
  +0x288 byte: high lighting quality (the active lot / Build mode)
room ("LightingRoom")
```

Evidence: light_diag.cpp (walks and printed fields), level_light_share.cpp (`TreeLevel`, `FloorOutdoorLights`,
`QueueOutdoorRegather`, `ShareOutdoorLights`), night_terrain_relight `QueueAllLotOutdoorRooms`, lot_edge_lighting comment
("manager: +0x00 lighting system (its +0xD4 is the tree), +0x88 level, +0x90/+0x94 lot id").

Note: levels are relative to the lot. On a house built on a foundation, level 0 is the terrain story and the ground floor
is level 1 (F8 of the house lot C49C001BCF2DEA20, NOTAS "Andares, segunda rodada").

#### Room ("LightingRoom")

| Offset | Type | Meaning | Evidence |
|---|---|---|---|
| +0x00 | ptr | Owning manager | level_light_share `ShareOutdoorLights` |
| +0x04 | ptr | LODParams* | lighting_quality_patch.cpp |
| +0x0C | int | Room id (0 = outside of the story) | `fn_006c6ab0.c`; level_light_share |
| +0x10 | vector | 3D blockers (objects) of this story | NOTAS "Andares, segunda rodada" |
| +0x18 | byte | Roofless / outdoor flag ("externo" in F8). Fences or railings that close an area make a roofless room with +0x18 = 1. In `0x006A31D0` a room with +0x18 == 0 gets the normalisation and ambient alpha; +0x18 != 0 stores the raw sum | light_diag; NOTAS "Cerca em area cercada"; `fn_006a31d0.c` |
| +0x30 / +0x34 | vector<int> | 2D blockers: the walls of this story's room (built by `0x006A1DE0`) | level_light_share `CulledWalls` |
| +0xC0 / +0xC4 | int | Lot size (inferred: `0x006C6AB0` gathers world lights at `(C0 × 0.5, 0, C4 × 0.5)` transformed by +0xF8, i.e. the lot centre) | `fn_006c6ab0.c`; dwords `0x00F9A5AC` = 0.5 |
| +0xC8 / +0xCC | Light** | **Room light list** begin / end | `fn_0069fd60.c`; light_diag |
| +0xD8 / +0xDC | vector | LightingWall list (source of +0x30) | NOTAS "Andares, segunda rodada" |
| +0xEC | int | Resume state of a budgeted solve | smooth_streaming note 5 |
| +0xF0 | int | Solve state ("estado" in F8); 3 = synchronous solve running; 1 with +0x168 != 0 = waiting for its gather | light_diag; smooth_streaming note 3; level_light_share `QueueOutdoorRegather` |
| +0xF4 | int | LOD class / LOD index; 2 = the wall blur class | light_diag ("classe"); lighting_quality ("LOD index"); PASSO3 F-J1 |
| +0xF8 | float* | Pointer to a 4×4 lot→world matrix (row vectors); VS c8 ≈ (m0, m4, m8, m12), c10 ≈ (m2, m6, m10, m14) | PASSO3 F-J2 (runtime match unverified) |
| +0x100 | int | Printed as "x100" by F8 (unverified meaning) | light_diag |
| +0x160 | float | Normalisation factor applied to the sum before the clamp (indoor rooms) | `fn_006a31d0.c`; hdr_native header |
| +0x164 | byte/int | Budget flag of the solve | smooth_streaming note 5 |
| +0x168 | int | Gather countdown | level_light_share `QueueOutdoorRegather` |
| +0x1C4 / +0x1C8 | | Floor texture lock (data / pitch) | lighting_quality_patch.cpp |
| +0x2B4 / +0x2B8 | | Ceiling texture lock | lighting_quality_patch.cpp |
| +0x3A4 / +0x3A8 | | Object (basis) texture lock | lighting_quality_patch.cpp |
| +0x639 | byte | Wall mode read by `0x0069FC40` (wall height test / soft shadows of this pass) | level_light_share `SolveCtx::soft` |
| +0x63C | float | Per-light threshold: a light counts only if `r+g+b ≥ threshold` of its evaluated colour | `fn_0069fd60.c`; PASSO3 3.1 |
| +0x680 / +0x684 | | Wall atlas / pitch (blurred by `0x0069F650`) | PASSO3 F-J1 |

Sample (`LightSurfaceData`, 0x30 bytes): +0x00 world position float[4], +0x10 world normal float[4], +0x20 lightmap X
(ushort), +0x22 lightmap Y (ushort). Source: lighting_quality_patch.cpp comment; confirmed by `fn_006a31d0.c` (the texel is
written at `Y × pitch + X × 4`).

### Call flow

#### Gathering a room's light list

1. `0x006C7010` → `0x006C6AB0(treeLevel, room)`:
   - `0x006C6990(room, 1)`: lights registered on this story for this room (registry entry room id == room id), filtered by
     `0x006C7820` (lit, type > 2).
   - Only if `room id == 0` and `treeLevel+0x1A0 == 0` (level 0): `0x006C6990(room, 0)` again. So the ground story's
     outdoor lamps are in its room-0 list **twice** (F8 01:39: type-3 lamps twice, type 11 three times).
   - Only for room 0: world lights around the lot centre, `0x006B66B0` (±2 cells of 32 m), added with `0x006A2060`.
     The per-cell filter keeps lights with `GetLotID() == 0` (world lights). With the Split-Level Lighting Fix
     (`GetLotID` → 0) lot lights of type 11 also enter here, on every story.
2. 3D blockers: `0x006C6A20` + `0x0069E5E0` (room+0x10), own story only.

Measured consequence (F8, NOTAS "Andares"): before Apex, upper stories never saw the ground story's type-3 lamps and the
ground story never saw upper-story lamps; only type 11 (via world lights) reached every story. The EA cascade in
`0x006C7250` (room 0 of level 0 changes → refresh room 0 of levels 0..7) suggests EA meant upper stories to see the ground
lamps; the list gather does not do it. The lot-load path `0x006C54E0` gathers every story's room 0 through level 0's tree
level, so ground lamps lit the upper stories only until the first update.

#### Invalidation and re-solve

- A light change calls `0x006C5560(tree, lot, level, roomId)` → `0x006C6D00` (room dirty). World lights have lot id 0, so
  no neighbouring lot is ever marked (lot_edge_lighting comment). `0x006C5E20` processes dirty rooms every frame.
- `0x006C7250` restarts rooms; room 0 of level 0 cascades to all levels 0..7.
- `0x006C7160(treeLevel, roomId)` queues a room the way a Build-mode lamp change does (used by Apex's lot relight).
- The solve itself runs in the budgeted path (normal play) or synchronously (impostor builder); see
  [lot-loading-and-streaming.md](lot-loading-and-streaming.md).
- A lot solved by day keeps the street lamps as "off": `0x006BE020` uses the effective colour +0xE0, which is 0 while a
  lamp is off, and nothing re-solves lots when lamps switch on at dusk (NOTAS 1, lot_edge_lighting comment). Loading the
  save at night makes the cut mostly disappear.

#### The per-texel solve

`0x006A31D0(room, samples, {base, pitch}, flags, ..., addAmbient)`:

1. Builds per-light cull lists for the batch (`0x0069F1E0` centre; `0x006A30B0` → `0x0069DFF0` walls whose culling edge
   crosses centre→light; `0x006A3140` for 3D occluders), only when the batch has more than 3 samples (level_light_share
   comment).
2. Per sample, `LightPointWithAllLights(room, out, list2D, list3D, flags, sample)`:
   - for each light of room+0xC8..+0xCC: `colour = light->vfunc4C(sample, sample+0x10 normal, &colour)`;
   - if `colour.r + colour.g + colour.b ≥ room+0x63C` (weights `[0x0107A538]` = 1.0):
     - if `flags[0]`: 2D wall test `0x0069FC40` with the light position (vfunc+0x24) → transmission `t` or skip;
     - if `flags[1]`: 3D test `0x0069FCE0`;
     - `out += t × colour`.
3. For indoor rooms (+0x18 == 0): `out *= room+0x160`, transformed by `0x0069EC40` (unverified), then
   `alpha = (1 − sat(lum × k)) × 0x006AB210(sample)` (k from `0x00F626F0`, unverified), plus an optional term
   (`0x00963DD0`) when the last argument is set.
4. `texel = min(out, (1,1,1,1))` (`minps` with `[0x0107A538]` = 1.0), packed by `0x0060CD10`, stored at `Y × pitch + X × 4`.

So a room map texel is `rgb = min(Σ lights × normalisation, 1)` and `alpha = an ambient weight`. Interior shaders add
`alpha × InteriorBuildingAmbientColor + rgb` (hdr_native header, rule A). The map is 8-bit A8R8G8B8.

#### The per-light lamp formula (street-lamp class, `0x006BE020`)

From the disassembly and PASSO3-PLANO.md 3.2 (the other classes follow the same shape with their own cone terms; only the
street lamp was read in full):

- `W = +0x130 (range) × intensity.x (+0x10) × k2 [0x011D0A68] / k1 [0x011D0A60]`;
- per-lamp term `min(Cmax [0x011D1160].x, W × max(0, N·l) / d²)` with d measured from the lamp head (+0x120) and a wrapped
  N·L (ground_report 5, unverified detail);
- colour = effective colour +0xE0 (`0x006BE18C`), i.e. zero while the lamp is off; this street-lamp evaluation also
  reads `[0x01158DA8]` = 3.333 right after (`0x006BE1A1`). PASSO3 3.2 models it as `col = E0 × 3.333` for type 11, and
  NOTAS 3 calls it "the ×3.333 of the ground" that the rig formula lacks. Whether it applies to every sample or only to
  ground samples is unverified.

Measured runtime values (F8 16:09, "LUZ POR PIXEL"): k1 = 1, k2 = 0.075, Cmax = 2, street factor 3.333, type-5 s = 5,
blur passes 2, blur mode 0; spot (type 4) axis ≈ (0, −1, 0), offset −0.32, scale 4.9. k2 = 0.075 is why walls look much
dimmer than objects lit by rigs (NOTAS "Paredes mais escuras").

Cone data read by F8 (light_diag `PixelLampDiag`): type 4 (vtable `0x00FF4570`): axis +0x170, offset +0x158, scale +0x154;
type 5 (vtable `0x00FF4350`): a1 +0x1A0, o1 +0x174, a2 +0x190, o2 +0x170, S +0x150, s `[0x011D11A0]`. The type-5 blend
(`FUN_006BC940`) is unverified (PASSO3 section 6, question 4).

#### The level solvers and the wall ambient occlusion (round 3, 2026-09-29; VERIFIED in full.asm)

- Each lot level embeds two solvers: `lvl+0x290` (wall AO, vtable 0x00FF0594; "Renderer/AOSolver") and `lvl+0x2E8`
  (vtable 0x00FF0714). The room solve `0x006A8BA0` calls slot +0xC of both (when `[lvl+0x88] >= 0`) before the rooms.
- Slot +0xC = the driver `0x00688920`: `if ([s+0x14] != 2 && [s+8]->vfunc+0xC()) [s+0x14] = s->vfunc+0x1C(stopwatch,
  budget)`. The state (0 first pass due, 1 refinement due, 2 done) is whatever the step returns.
- The wall AO step `0x0068B810` (slot 0x00FF05B0, its only reference): outdoor room = RoomById(level, 0); state 1 =
  refinement sized by AO.ini WallMillisecondsBudget (`[0x011CF4A0+0x24]`, 10 ms; returns 1 when its estimate is
  negative, i.e. "try later"); otherwise one pass at detail 0 over **every** outdoor wall (`0x0068B2B0` per wall between
  the AO image lock 0x00618DF0 / unlock 0x00619160), returns 1. No time check inside a pass; the budget argument is unused.
  10-17 ms per pass, 54-108 ms when several lots load (round3.md section 2). The second solver (step 0x0068A400) checks
  the stopwatch per row and resumes.
- Readers: 0x00688DB0 binds the AO image only in states 1 / 2; lot load stage 20 (`0x00ADBBA0` -> `0x006A5B50`) waits for
  state != 0 of both solvers of every level ([lot-loading-and-streaming.md](lot-loading-and-streaming.md)); `0x006A5BF0`
  (both == 2) is waited for by the ThumbnailManager's lot capture (0x00AE06B0 -> 0x00ADBC30, CALL 0x00D5BE2F). Resets: slot +0x10 0x006895C0 (message 0x0486519D, 0x006A4240, 0x006A4180) and level
  creation. The synchronous level solve 0x006A4180 (lot LOD switch setup 0x00ADBAD0) drives both once with 60,000 ms.
- AO config singleton 0x011CF4A0 (ctor 0x006886C0, loaded from "AO.ini" by 0x006883A0): +0x0C DetailZeroTexelsPerSample
  8, +0x10 DetailZeroBlurWidth 19, +0x14 MinimumBlurWidth 7, +0x18 DetailZeroRaysPerQuadrant 5, +0x1C MaximumDetailLevel
  10, +0x20 NumHeightBands 4, +0x24 WallMillisecondsBudget 10.
- Apex: Wall Shading While Moving gates the step's slot ([../features/performance/wall-shading-while-moving.md](../features/performance/wall-shading-while-moving.md)).

### Which lights end up in a room map

- Lot lamps (types 3..6, see [light-objects-and-rigs.md](light-objects-and-rigs.md)) of the room, and for room 0 world
  lights (street lamps, type 11, lot id 0) within ±2 cells.
- **Window lights**: RectangleWindowLight (vtable `0x00FF43A8`, factory type 7) and CircleWindowLight (vtable `0x00FF4408`)
  appear in room lists like lamps (F8 dumps; PASSO3 F-J5 saw index #5494 become a "type-7 window light" in a later dump).
  They carry daylight, so a room map is lamp-only only at night (hdr_native header, 28/09).
- A roofless room (+0x18 = 1) made by fences/railings gets 0 lights on lots that are not active (NOTAS 4c, 6: park plaza
  floor dark).

### The maps and their layouts

| Surface | Map | uv | Evidence |
|---|---|---|---|
| Outdoor and indoor walls | One wall atlas per story (e.g. 256×128 A8R8G8B8 T9 upper / T3 ground in m44/m45; 1024×512 for the ExteriorWall family of m59); small strips, one per wall piece | vertex `TEXCOORD1 × 1/4096` → `o2.xy`; PS `texld r1, v1, s2; mad r5.xyz, r1, c3.x(or c2.x), r2` | NOTAS "Andares", "Passo 3: capturas"; wall_lamp_table.h |
| Outdoor floor tiles | Floor map, e.g. 256×256 covering 64 m (`uv = pos × 0.5 / 64`), nearly black in summer (mean 0.002, max 0.18, m61) | TEXCOORD2 = local xz × c12 (VS 898DEAF3) | NOTAS "Piso de fora no verao"; PASSO3 F-J3 |
| Lot grass (light pass) | Lot `LightMap` of room 0 | TEXCOORD5 (`s1`, stock 568-byte pass) | NOTAS 1; lot_light_bridge `kReplacementHlsl` |
| Snowy lot / floors | Same room maps × a direction factor × 0.25 (def c9.x) | | NOTAS "Neve: analise completa" |
| Interior objects in room mode | Lot LightMap + LightBasisMap0..3 via `WorldToLotTransform` (LightMapXform) | | NOTAS "Counters/Phong"; `fn_006a7700.c` |
| Doors/thresholds (Counter-like pieces) | Own 32×32 A8R8G8B8 map | | NOTAS "Porta" (m52) |

Room-0 texels always have alpha 0 (PASSO3 critique F2: an atlas alpha > 0 comes from indoor texels); Apex's planned
per-pixel wall lamps used "alpha < 1/255" as the outdoor guard.

**Lot map vs world terrain map**: lot surfaces bind "LightMap" (per room, CPU solve, occluded by walls, lamps × k2 or
× 3.333 for street lamps on the ground); world terrain binds "terrainLightMap" (per 256 m chunk, GPU stamp, no occlusion,
see [terrain-and-light-bake.md](terrain-and-light-bake.md)). The two meet at the lot border, which is the original "straight
cut" (NOTAS 1). Apex's lot light bridge draws the lot pass with `max(lot map, terrain map or world atlas)`, so lot ground
now shows terrain light through walls (ground_report 5: "the leak already exists").

### Night level (lightMgr+0xF0)

A float from 0 (day) to 1 (night) set by `0x006ADD60`. `IsNight` (`0x006AC560`) uses `> 0.99`. The game switches lamps
on at dusk through the same setter but re-solves neither lots nor terrain. Apex reads it every frame
(night_terrain_relight `ReadLightState`, object_light_bridge `ReadNight`) and passes it to shaders as the moon-shadow fade
(see [shaders.md](shaders.md)) and to the room-rig re-gather (every 0.1 of change).

### How Apex reads and patches this

| Site / hook | What | Code | Option (TOML key) |
|---|---|---|---|
| Calls `0x006C5816`, `0x006C7094` → `OutdoorGather` | After the game's gather, room 0 of story L (0..7) also runs `0x006C6990(otherStory, room, 0)` for every other story 0..7, twice for story 0, so each outdoor lamp lights every story with its home weight. Real story from `room[0]+0x88`; lot-load path relative to level 0; basements untouched | level_light_share.cpp | `luzExternaEntreAndares` (default true) |
| `0x006C73B1` JNZ → JL (`0x85` → `0x8C`) | Room 0 of any level 0..7 cascades the refresh, not only level 0 | level_light_share.cpp | same |
| Calls `0x006A1187`, `0x006A126F`, `0x006A3336` → `SolvePointSingle/Batch` | Remember which room is solved and the batch centre | level_light_share.cpp | same |
| vfunc+0x4C of the 9 light classes → `LightEvalHook<I>` | Only when returning to `0x0069FE19` for a light from another story: test `0x0069FC40` against room 0 of the lamp's story and the stories in between (the game's own per-light cull lists when the batch has them), with the solving room's +0x639 swapped in; scale the colour by the product of transmissions | level_light_share.cpp `CrossFloorShadow`, `WallPass` | same |
| `0x0069FE93` → `GameWallTest` | Records the game's own wall result for F8 diagnostics | level_light_share.cpp | same |
| `0x006BE18C` → `StreetLampColourStub` | In the solve, a street lamp (type 0xB, lot id 0) that is off uses `+0xF0 × +0x10` instead of `+0xE0` | night_terrain_relight | `postesAcesosNoCalculo` (experimental, default false) |
| `0x00ADB66B`, `0x00ADB884`: `C6 44 24 0C 00` → `... 01` | Every lot solved at the active lot's high quality (lots loaded afterwards) | night_terrain_relight | `qualidadeAltaEmTodosOsLotes` (experimental, default false) |
| `QueueAllLotOutdoorRooms` | `0x006C7160(treeLevel, 0)` for levels −4..7 of every lot | night_terrain_relight | button / `recalcularLotesAoAnoitecer` (experimental) |
| F8 `LightDiag` | Dumps every light, every lot story and room (state, class, x100, outdoor flag, light list with street/lit counts), the story-share section and "LUZ POR PIXEL" (k1, k2, Cmax, street factor, blur, per-lot room-0 class/threshold/matrix, cone data) | light_diag.cpp | developer mode, Ctrl+Shift+F8 |
| Unlit-room colour `0x011D0B60` / `0x011D0B40` (imm32 at `0x006A0F95`, `0x006A0F9C` in the room ambient `0x006A0F50` and at `0x006A00C2`, `0x006A00C9` in the dim-room top-up `0x006A00A0`), the rig fill light gate `0x01158D5C` (imm32 at `0x006BA57E`) and colour `0x011D0E10` (imm32 at `0x006B816D`); optional `BaseHook` on the two calls of `0x006A00A0` | Rooms at Night: the colour of rooms with no lamp and of dim rooms at night, and the base under the lamps | `features/unlit_rooms.cpp`; [../features/night-lighting/unlit-rooms.md](../features/night-lighting/unlit-rooms.md) | Rooms at Night switch |
| Room priority call `0x006A81DF` → `0x0069E770`, LOD step `0x0069EAA2`, class keeps `0x0069EF58` / `0x0069F1C5`, pick jump `0x006C5E39` → `0x006C5C20`, budgeted step `0x006A3F80` with the stopwatch `0x004F35B0` / `0x00408700` / `0x004F33C0` | Faster Room Lighting: room order, class 0 to target in one step, requeued rooms keep their class, more than one room per frame | `features/room_light_queue.cpp`; [../features/performance/room-light-queue.md](../features/performance/room-light-queue.md) | Performance switch |

The share and the cross-story wall test run on the light-tree thread (`g_gatherThread`); the status shows "on another
thread" if a solve is seen elsewhere. Refreshes (`RefreshAllLots`) run on the render thread; rooms already waiting for
their gather (state 1 with +0x168 != 0) are skipped.

Other Apex paths that use room maps without changing the solve: the lot light bridge (lot grass), `DrawWallGain` (outdoor
walls: `cK.x × forcaNasParedes`, default 2.0, 58 ExteriorWall variants), the floor patches (`max(room map, atlas)`), snow
on floors. See [shaders.md](shaders.md) and the feature docs.

### Pitfalls and history

- The first story fix only shared lamp lists; a wall sconce near a corner then lit the side wall of the story below
  around the corner, because the lower story's wall list does not contain the upper walls and `0x0069AA90` only blocks
  below the wall top. Fixed by the cross-story wall test (NOTAS "Andares, segunda rodada").
- Review 25/09 fixes (keep them): the real story comes from the manager (`room[0]+0x88`), basements excluded, no cascade
  by signature, the refresh skips rooms already pending, reinstalling other options never touches the stories;
  `0x0069FC40` reads the wall mode +0x639 of the room it is given, so the solving room's value is swapped in and restored
  (also on a fault).
- Two light classes were missing from the first class table (`light_vtables.txt` merged `0xFF4408` with `0xFF43A8` and
  omitted `0xFF4468`): the factory `0x006AC590` makes 9 classes. Always hook all 9.
- Porch lamps under an overhang must not be blocked by the upper story's walls: the room's own lights are tested only as
  the game does.
- A slab of an upper balcony does not block light going down (blockers are per story) — same limitation as the
  Split-Level fix for type 11.
- Street lamps counted as lit (`0x006BE18C`) and high quality on every lot were tried for the lot-border cut and did not
  fix it; both remain experimental options (NOTAS 1).
- Room lists are written on the light-tree thread; reading them on the render thread needs `__try` and sanity checks
  (PASSO3 critique F5). F8 light indices are not stable between dumps: identify lights by pointer (PASSO3 F-J5).
- The planned per-pixel wall/floor lamp term (PASSO3-PLANO.md, "Design A") was never implemented; its facts (threshold,
  blur, matrix, constants) are recorded above.

### Open items / unverified

- `0x0069EC40`, `0x006AB210`, `0x00963DD0` in the normalisation/ambient step; room+0x100, +0x280.
- The exact N·L wrap in `0x006BE020`; the evaluation of the other 8 classes; the type-5 blend.
- Whether room+0xF8 equals the lot VS c8/c10 at runtime (PASSO3 question 3).
- The code path that fills the lot-grass light map (ground_report 6).

## Address reference

| Address | Name / role | Evidence |
|---|---|---|
| `0x006C6AB0` | `AddWorldLightsToLot`: thiscall(treeLevel, room) ret 4. Room light list = own-floor lot lights (`0x006C6990(room, 1)`); for room 0 of level 0 once more (`0x006C6990(room, 0)`); for room 0 also world lights around the lot (`0x006B66B0`) | `fn_006c6ab0.c`; level_light_share.cpp header; lot_edge_lighting pattern `55 8B EC 83 E4 F0 83 EC 34 53 56 33 DB F6 05 ?? ?? ?? ?? 01 57 8B F9 75 30 83 0D` |
| `0x006C6B08`, `0x006C6B2D` | The two calls of `0x006C6990` inside it; the level-0 test is `cmp [edi+0x1A0],0 / jnz` at `0x006C6B16` | level_light_share `kLevelGatherCalls`; NOTAS "Andares" |
| `0x006C5816`, `0x006C7094` | Callers of `0x006C6AB0`: room creation / room update | level_light_share `kGatherCalls` |
| `0x006C7010` | Room gather entry: `0x006C6AB0` + 3D blockers (`0x006C6A20` + `0x0069E5E0`, room+0x10) | NOTAS "Andares, segunda rodada" |
| `0x006C6990` | Walks a tree level's light registry (+0x90: buckets +0x98, count +0x9C) and offers each light to `0x006C7820` | `fn_006c6990.c` |
| `0x006C7820` | Per-light filter for the room list: lit, type > 2, entry room id == room (entry+0x1C) | NOTAS "Andares" |
| `0x006B66B0` | `GatherWorldLightsNearPoint`: world lights in ±2 light cells (32 m) around a point; lights added with `0x006A2060` | lot_edge_lighting pattern `8B 54 24 04 83 EC 10 53 55 56 8B E9 8D 44 24 14 50 8D 4C 24 1C 51 52 8B CD E8`, `kRadiusOffsets` |
| `0x006B6320` | `CellGatherWorldLights`: per-cell filter, `vfunc+0x20` and `GetLotID() == 0` | lot_edge_lighting pattern `51 53 57 8B 79 14 2B 79 10 ...`; re/out report.txt |
| `0x006BC020` | `BaseLight::GetLotID` (+0xC0/+0xC4). S3SS's Split-Level Lighting Fix makes it return 0, so lot lights of type 11 also come in as "world lights" on every story | split_level_lighting_fix_patch.cpp |
| `0x006C5560` | `LightUpdateTree_RoomLightChanged` thiscall(tree, lotLo, lotHi, level, roomId): marks that room dirty | lot_edge_lighting pattern `8B 44 24 08 8B 54 24 04 50 52 E8 ?? ?? ?? ?? 85 C0 74 31 8B 4C 24 0C 83 F9 FC` |
| `0x006C6D00` | `LightTreeLevel_MarkRoomDirty` thiscall(treeLevel, roomId) (inserts into the set at treeLevel+0x4C) | lot_edge_lighting pattern `8B 44 24 04 50 68 ?? ?? ?? ?? 8D 51 4C 52 E8 ?? ?? ?? ?? C2 04 00` |
| `0x006C5E20` | `LightUpdateTree_Update`: processes dirty rooms every frame | lot_edge_lighting pattern `56 6A 00 8B F1 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? 8B CE E8` |
| `0x006C7250` | Dirty rooms per floor. When room 0 of **level 0** changes, invalidates room 0 of levels 0..7 (`0x006C73B6..0x006C7426`: `0x006A6550(mgr,0)`, `0x0069EED0(room,1,0)`, insert 0 into treeLevel+0x28). Level test `cmp [esi+0x1A0],eax; jnz` at `0x006C73AA` (bytes `39 86 A0 01 00 00 0F 85 7C 00 00 00`), Jcc at `0x006C73B1` | level_light_share `kCascadeTest`, `kCascadeJcc`; NOTAS "Andares" |
| `0x006C54E0` | Lot load: gathers room 0 of EVERY floor through level 0's tree level (`0x006C5525`) | level_light_share header |
| `0x006C7160` | Queue a room: thiscall(treeLevel, roomId) ret 4 (the Build-mode lamp-change path) | night_terrain_relight `kQueueRoom`, bytes `83 EC 2C 53 55 56 33 DB 8B F1` |
| `0x006A6550` | Room by id: thiscall(manager, id) ret 4 | level_light_share `kRoomById` |
| `0x0069EED0` | Invalidate room: thiscall(room, char full, char keep) ret 8 | level_light_share `kInvalidateRoom` |
| `0x00B7AAD0` | Set insert thiscall(set, out, const int* key, char) ret 0xC | level_light_share `kSetInsert` |
| `0x0069FD60` | `LightPointWithAllLights` thiscall(room, float out[4], list2D, list3D, char flags[2], sample) ret 0x14 | `fn_0069fd60.c`; re/out report.txt |
| `0x0069FE19` | Return address after the per-light `call edx` (light vfunc+0x4C) inside it | level_light_share `kLightEvalReturn` (`FF D2` before it checked) |
| `0x0069FE93` | Its call of the 2D wall test `0x0069FC40` | level_light_share `kWallTestCall` |
| `0x0069FC40` | 2D wall test thiscall(room, int* indexVec, lightPos, sample, float* t) ret 0x10 → `0x0069D4C0` over room+0x30; reads the room's wall mode byte +0x639 | level_light_share; NOTAS "Andares, segunda rodada", "Revisao ... 2." |
| `0x0069FCE0` | 3D occluder test (bubble-shaped objects) | `fn_0069fd60.c` |
| `0x0069AA90` | Wall/ray test: blocks only if the ray passes **below the wall top**; no base test | NOTAS "Andares, segunda rodada" |
| `0x0069DFF0` | Per-light wall cull thiscall(walls, int-vector* out, from, lightPos) ret 0xC, called at `0x006A311F` in `0x006A30B0` | level_light_share `kWallCull` |
| `0x006A3140` | Per-light 3D occluder cull (batch) | `fn_006a31d0.c` |
| `0x0069F1E0` | Batch centre (mean of the samples) | level_light_share comment; `fn_006a31d0.c` |
| `0x006A31D0` | **Batch solve of wall/floor samples**: per sample calls `0x0069FD60` (call at `0x006A3336`), normalises, computes the ambient alpha, `min(result, 1)`, packs and stores the texel | `fn_006a31d0.c` |
| `0x006A3B03`, `0x006A3687`, `0x006A37CD`, `0x006A3956` | `push 0x01158AC8` (the batch sample vector) in the 4 callers of `0x006A31D0` | level_light_share `kBatchPushes` (`68 C8 8A 15 01`) |
| `0x01158AC8` | Global vector {begin, end} of the current batch, 0x30 bytes per sample | level_light_share `kBatchSamples` |
| `0x006A0F50` | Other solve path; calls `0x0069FD60` at `0x006A1187` and `0x006A126F` | level_light_share `kSolvePointCalls` |
| `0x011D0368` / `0x011D0358` | Static vectors "Lighting/LightingOccluder/m2DOccluders" / "m3DOccluders" (per-light cull lists of a batch) | `fn_006a31d0.c` |
| `0x0069F650` | Wall atlas blur when room+0xF4 == 2: `[0x01158B1C]` passes (static value 2) of [1 2 1]/4 (flag `[0x011D02E4] == 0`) or a 2×2 box with half-texel shift | PASSO3-PLANO.md F-J1 (`passo3\out\fn_0069f650.c`); dwords.txt |
| `0x006A1DE0` | Builds room+0x30 (2D blockers) from room+0xD8..+0xDC (LightingWall+4); called by the room rebuild `0x006A2740` | NOTAS "Andares, segunda rodada" |
| `0x006A0E00` | `FinalizePrime`: commits locked floor/ceiling/object textures (called from `0x006A3C90`) | re/out report.txt; lighting_quality pattern `56 8B F1 57 8B BE F4 00 00 00 C1 E7 06 03 7E 04 8B CF E8` |
| `0x006A4480` | `CacheLightingParams` (called from `0x006A6D10`) | re/out report.txt |
| `0x006A4700` | Registers lot surface shader parameters: "LightMap", "SpilloverLightMap", "WorldToLotTransform" (4×float4), "LightmapSizeParameters", "LotSizeParameters", "LightBasisMap0..3", "CounterLightingConfig"; handles at `0x011D04A4..0x011D04C8` | `fn_006a4700.c` |
| `0x006A7700` | Binds a lot's LightMap / LightBasisMap0..3 and chooses CounterLightingConfig `(2, 0, 0, 5)` or, when manager+0x288 is set, `(2, 0.85, 1, 1)` | `fn_006a7700.c`; dwords `0x00F98A58`=2.0, `0x00FB6258`=0.85, `0x00FBD498`=5.0 |
| `0x006A20D0` | Room init; registers "InteriorBuildingAmbientColor"; allocs "Lighting/LightingRoom/mLightList", "mTileStorage", "mWallStorage" | `fn_006a20d0.c` |
| `0x006AABE0` | Floor sampler: `*(float**)(room+0xF8)` is a 4×4 lot→world matrix, row-vector convention (world = x·m[0..3] + y·m[4..7] + z·m[8..11] + m[12..15]) | PASSO3-PLANO.md F-J2 |
| `0x006BE020` | Street-lamp class evaluation (vfunc+0x4C of vtable `0x00FF42F8`) for the room solve | level_light_share `kClasses`; full.asm |
| `0x006BE18C` | Inside it: `movaps xmm0,[esi+0E0h]` (effective colour; 0 while the lamp is off) | night_terrain_relight `kLampColourSite` `0F 28 86 E0 00 00 00` |
| `0x011D0A60`, `0x011D0A68` | k1, k2 (tweakables read through `0x00F626F0`); runtime 1.0 and 0.075 | full.asm `006BE064`, `006BE13C`; F8 16:09 (NOTAS "Lote escuro") |
| `0x011D1160` | Cmax float4; `.x` = 2.0 at runtime | full.asm `006BE167`; F8 16:09 |
| `0x01158DA8` | Street-lamp factor 3.3333 (`405554CA`) | full.asm `006BE1A1`; dwords.txt |
| `0x011D11A0` | Type-5 (ShadedLamp) cone scale s (runtime 5) | light_diag `PixelLampDiag`; F8 16:09 |
| `0x00ADB5A0`, `0x00ADB850` | Pass `(lot is active || Build mode)` as the quality flag to `0x006A5EF0`; the `mov byte [esp+0Ch],0` sites are `0x00ADB66B`, `0x00ADB884` | night_terrain_relight `kQualitySites`, bytes `C6 44 24 0C 00` |
| `0x00ADB120` | Lot lighting time budget per frame (5 / 10 / 15 / 30 ms; 1000 in tool mode); only caller the CALL at `0x00ADB95D` | smooth_streaming note 5; Lot Lighting While Moving scales it while the camera moves ([../features/performance/README.md](../features/performance/README.md)) |
| `0x00AE4CB0 → 0x00ADB8F0 → 0x006A8BA0 → 0x006A88B0 → 0x006A3F80 → 0x006A3C90` | Budgeted room solve during normal lot loading; `0x00ADB8F0` calls `0x006A8BA0` once per level object of the lot (deque at manager+0x24..0x40) with one shared stopwatch and stops when elapsed > budget; `0x006A3C90` returns when the budget is spent while room+0x164 is set and resumes from room+0xEC (state machine, 9 = done) | smooth_streaming note 5; verified 2026-09-29 ([../features/performance/README.md](../features/performance/README.md)) |
| `0x00ADBAD0 → 0x006A80E0 → 0x006A3EC0` | Synchronous solve (while room+0xF0 == 3, 60000 ms budget) used only by the lot impostor builder | smooth_streaming note 3 |
| `0x006ADD60` | Night-level setter (lightMgr+0xF0) | night_terrain_relight header |
| `0x006AC560` | `IsNight`: lightMgr+0xF0 > 0.99 and byte `[0x011D08C0] == 0` | full.asm |

## See also

- [Terrain and light bake](terrain-and-light-bake.md).
- [Light objects and rigs](light-objects-and-rigs.md).
- [Lot loading and streaming](lot-loading-and-streaming.md).
- [Shaders](shaders.md).
