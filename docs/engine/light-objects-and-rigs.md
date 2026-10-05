# Light objects, light classes and object light rigs

This page documents the game's light objects (the 9 `BaseLight` classes made by the factory `0x006AC590`), the light manager and its cells, and the per-object light rigs that light every SceneModel. Night Lighting's object, fence, foliage, lamp colour, roof and water parts, Every-Story Ground Light and the Light Diag and Light Probe tools depend on it.

## Scope

| | |
|---|---|
| Game build | Steam 1.67.2 (`TS3W.exe`, image base 0x00400000) unless stated |
| Used by | [Objects and rigs](../features/night-lighting/objects-and-rigs.md), [Fences](../features/night-lighting/fences.md), [Foliage](../features/night-lighting/foliage.md), [Lamp colour](../features/night-lighting/lamp-colour.md), [Roofs](../features/night-lighting/roofs.md), [Water](../features/night-lighting/water.md), [Level light share](../features/night-lighting/level-light-share.md), [Light Diag](../features/dev-tools/light-diag.md), [Light Probe](../features/dev-tools/light-probe.md) |
| Evidence | Disassembly (`engine_map` `full.asm`, `strings.tsv`, `dwords.txt`), the factory jump table in `re\TS3W.exe`, F8 diagnostics, F7 captures, the Apex sources listed under Sources |

## Overview

Engine reference for TS3W.exe 1.67.2 (Steam, image base 0x00400000). Every lamp, street lamp and window in the game is a
"BaseLight" object of one of 9 classes made by the factory `0x006AC590`. The same objects feed three consumers: the
terrain stamp bake ([terrain-and-light-bake.md](terrain-and-light-bake.md)), the room light solve
([room-light-maps.md](room-light-maps.md)) and the per-object light **rigs** that light every SceneModel (fences, plants,
doors, counters, Sims...). This page gives the light object layout, the class/type table with vtables, the light
manager and cells, the rig structure, how a rig gathers its sun + 3 lamps + 4 vertex lights, the brightness cap
`0x006B92A0`, how the binder hands the rig to the shaders, and how Apex reads and patches all of it.
Feature docs: [objects and rigs](../features/night-lighting/objects-and-rigs.md), [fences](../features/night-lighting/fences.md),
[foliage](../features/night-lighting/foliage.md), [lamp colour](../features/night-lighting/lamp-colour.md),
[roofs](../features/night-lighting/roofs.md), [water](../features/night-lighting/water.md),
[light diag](../features/dev-tools/light-diag.md), [light probe](../features/dev-tools/light-probe.md).

Objects never sample the room maps or the terrain stamp outdoors: each gets a rig (sun + at most 3 lamps + 4 "vertex
lights") evaluated once at its centre. Most "object too dark next to a lamp", "fence without lamp light", "modular pieces
with different light" and "pink lamps" problems come from the light objects' values and from the rig gather.

## Details

### Sources

Sources: `object_light_bridge.cpp`, `rig_tracker.cpp`, `level_light_share.cpp`, `lot_light_bridge.cpp` (`ReadLamp`,
`SelectLamps`, `ReadOutdoorLotLamp`, `DrawObjectLamp`), `light_diag.cpp`, `patches/night_terrain_relight_patch.cpp`,
`shader_patches.cpp/.h`, `scratchpad\engine_map\full.asm`, `strings.tsv`, `dwords.txt`, the factory jump table read from
`S3SS-dev\re\TS3W.exe` at file offset `0x2AC7A0`, NOTAS-ILUMINACAO.md sections 3 ("Engenharia reversa da luz por objeto"),
4-5b, "Grade / corrimao", "Cerca em area cercada", "CERCAS: causa real", "Reforco ... TODAS as classes", "Porta escura",
"Counters/Phong: relatorio", "Cor rosada", "LISTA DE CORRECOES PENDENTES", `scratchpad\counter_sh\REPORT.md`,
`scratchpad\survey_lamp_paths.txt`.

### Light classes and types

Factory `0x006AC590(type)` (jump table at `0x006AC7A0`, entries `006AC5B8 006AC5F0 006AC628 006AC660 006AC698 006AC6CB
006AC6FE 006AC732 006AC766` for types 3..11). Class names are the resource strings the factory passes to the allocator
(`0x004E3F90`); vtables are what each constructor writes at +0x00. That the factory argument equals light+0xB0 is inferred
(consistent with F8 dumps: type 11 = street lamp class, type 4 = vtable `0x00FF4570`).

| Type (+0xB0) | Class (string) | Constructor | Vtable | vfunc+0x10 (rig colour) | vfunc+0x4C (room eval) | Colour set at creation |
|---|---|---|---|---|---|---|
| 3 | Lighting/BareBulb | `0x006C0450` | `0x00FF42A0` | `0x006C02A0` | `0x006BDE90` | `0x006C047D` |
| 4 | Lighting/Spot | `0x006C1AE0` | `0x00FF4570` | `0x006C1BC0` | `0x006BFFB0` | `0x006C1B11` |
| 5 | Lighting/ShadedLamp | `0x006C0590` | `0x00FF4350` | `0x006C0690` | `0x006BE1C0` | `0x006C05C1` |
| 6 | Lighting/TubeLight | `0x006C1220` | `0x00FF4468` | `0x006C1320` | `0x006BFA70` | `0x006C1251` |
| 7 | Lighting/RectangleWindowLight | `0x006C0870` | `0x00FF43A8` | `0x006C0AF0` | `0x006BEFD0` | none |
| 8 | Lighting/CircleWindowLight | `0x006C0E60` | `0x00FF4408` | `0x006C0FE0` | `0x006BF880` | none |
| 9 | Lighting/RectangleAreaLight | `0x006C15A0` | `0x00FF44C0` | `0x006C16D0` | `0x006BFBA0` | `0x006C15D1` |
| 10 | Lighting/DiscAreaLight | `0x006C1860` | `0x00FF4518` | `0x006C1980` | `0x006BFDC0` | `0x006C1891` |
| 11 (0xB) | Lighting/BareBulb (street lamp / "world light" class) | `0x006C04F0` | `0x00FF42F8` | `0x006C02A0` | `0x006BE020` | `0x006C051D` |

Vfunc slot addresses (vtable + 0x10) as patched by Apex: `0xFF42B0, 0xFF4308, 0xFF4360, 0xFF43B8, 0xFF44D0, 0xFF4528,
0xFF4580, 0xFF4418, 0xFF4478` (object_light_bridge `kClasses`). Vfunc +0x4C entries: level_light_share `kClasses`.

Type usage in practice (NOTAS, F8 dumps): lot lamps placed in Build mode are types 3..6 (garden lamps 3, spots/floods 4,
wall sconces 5); street lamps are type 11 with lot id 0 (world lights); type 11 can also be placed on a lot; window lights
are 7/8 and take part in room solves. "Lamp types" for Apex = 3..6 plus 11.

Light vtable functions used by the engine and Apex:

| vfunc | Role | Evidence |
|---|---|---|
| +0x10 | Colour of the light at a point for rigs, record format `0x006BDB00` | NOTAS 3, "Reforco" |
| +0x20 | "World light" test: true for the street-lamp class `0x00FF42F8`; used by the terrain visitor, the arm sites, the room gather (drops world lights) and the cell gather | night_terrain_relight header; NOTAS |
| +0x24 | Position (`0x009691E0`, all classes) | level_light_share |
| +0x28 | Range (returns +0x130) | lot_light_bridge comment near `ReadOutdoorLotLamp`; bake `0x00C29523` |
| +0x2C | Rect pointer (+0x134) | smooth_streaming comment; bake `0x00C29488` |
| +0x4C | Room-solve evaluation thiscall(light, sample, normal, float colour[4]) ret 0xC | `fn_0069fd60.c`; level_light_share `LightEval_t` |

### Light object layout

| Offset | Type | Meaning | Evidence |
|---|---|---|---|
| +0x00 | ptr | Vtable (class, table above) | light_diag |
| +0x08 | int | Room id (0 = outdoors / world); valid only with flag 0x04 | night_terrain_relight `IsOutdoorLotLamp`; NOTAS 3 (`light+8 == rig+0x1E0`) |
| +0x10 | float4 | Intensity (x used as the scalar intensity) | ReadLamp, BoostRec, `0x00C2951F` |
| +0x20 | float | Fade (0..1; animated on/off) | BoostRec, ReadLamp; NOTAS 3 "fade(+0x20)" |
| +0xB0 | int | Type (3..11) | everywhere |
| +0xC0 / +0xC4 | uint32 ×2 | Lot id (0 = world light) | `0x006BC020` |
| +0xD0 | int | Story (lot level) of the light; the terrain bake keeps lot lights only if it is 0 | `0x00C294D9`; light_diag "d0" |
| +0xE0 | float4 | Effective colour = intensity × colour while lit, 0 while off | `0x006BDA90`; `0x006BE18C` |
| +0xF0 | float4 | Base colour (what the terrain bake draws; what the tint fix changes) | `0x00C2950F`; NOTAS "Cor rosada" |
| +0x100 | byte | Flags: 0x01 alive, 0x02 registered in the light cells, 0x04 room known, 0x20 lit, 0x40 enabled | light_diag `LightLine` ("viva", "celulas", "comodo-conhecido", "ACESA", "habilitada"); lot_light_bridge.h |
| +0x120 | float4 | Position of the light (the lamp head); falloffs measure from here | ReadLamp; NOTAS 1, 3 |
| +0x130 | float | Range (typical 40, 70, 97, 100; a street lamp is two lights at one post, range 97 at 1.7 m and 40 at 3.7 m) | NOTAS 4d; shader_patches `AppendPixelLamps` comment |
| +0x134..+0x140 | float[4] | Rect {minX, minZ, maxX, maxZ} = pos ± sqrt(range / (k1 × `[0x011D1180]`.x)); ~50 m wide in practice | `0x006BDDF0`; NOTAS 4d |
| +0x150..+0x1A0 | | Cone data: type 4 axis +0x170, offset +0x158, scale +0x154; type 5 a1 +0x1A0, o1 +0x174, a2 +0x190, o2 +0x170, S +0x150 | light_diag `PixelLampDiag`; PASSO3 3.2 |

Typical values (F8 dumps): intensity 1.0; stock colour (1, 0.75, 0.79) ("pink"); other stock colours seen: G/R 0.98 B/R
0.85 (warm), 1.01/1.18 (bluish), 0.81/0.56 (orange), 1/1 (white), 0.99/1.06 (light lilac).

#### Light manager and light cells

| Offset | Meaning | Evidence |
|---|---|---|
| lightMgr+0xD4 | Light update tree (per-lot solvers, see [room-light-maps.md](room-light-maps.md)) | light_diag |
| lightMgr+0xF0 | Night level 0..1 | `0x006AC560` |
| lightMgr+0x104 | Light cells: world-space cells of 32 m holding the registered lights and the rigs in mode 2; countdowns +0x38/+0x3C | NOTAS 3; lot_edge_lighting ("±2 cells of 32 m") |

The cells are the only place the game re-gathers rigs from when lamps switch on (`0x006B58F0` walks the cells). Rigs in
room modes 0/1 are taken out of the cells (`0x006BAA70`) and are **not** refreshed at dusk (NOTAS "CERCAS: causa real",
"Revisao adversarial" item 4).

### The rig

One rig per SceneModel (`0x006F7880`); the fence SceneModelArray shares one rig per group. Layout (NOTAS 3,
"Grade / corrimao", "Cerca em area cercada", "Counters/Phong"; rig_tracker.cpp; object_light_bridge.cpp):

| Offset | Type | Meaning |
|---|---|---|
| +0x00 | ptr | Vtable `0x00FF4218` (Apex validates it before touching a rig) |
| +0x08 | ptr | HDLight* (HD objects with per-pixel lamps) |
| +0x10 | float4[4] | LightDirections: slot 0 = sun, 1..3 = lamps |
| +0x50 | float4[4] | LightColors: slot 0 = sun, 1..3 = lamps |
| +0x90 | float4[4] | VertexLightDirections (overflow lamps) |
| +0xD0 | float4[4] | VertexLightColors (overflow lamps) |
| +0x140 | float3 | Centre (transformed bounding-box centre, `0x006B76B0`): every lamp is evaluated here |
| +0x1B4 | ptr | Light manager (cells = +0x104 of it) |
| +0x1D4 | int | Mode: 0 roofed room, 1 roofless room (fenced area), 2 outdoor / off-lot |
| +0x1E0 | int | Room id (0 outdoors) |
| +0x224 | byte | Flags; bit 0x10 = accepts point lights (copied from model+0x29C bit 4 by the constructor) |
| +0x225 | byte | Bit 0x01 also required by the outdoor gather |

Model: `model+0x29C` bit 4 (0x10), set by the SceneModel constructor `0x006F5B40`, cleared on purpose (`0x006F4840(0)`)
for terrain, roads, roofs, ceilings, the sea, the lot skirt and objects whose script sends message `0x827917CA`.

#### Gather

1. Mode (`0x006C7CF0`) from the object's room.
2. Mode 2: `0x006BBBA0` → `0x006B5AF0(cells, rig)`: 3×3 cells of 32 m around rig+0x140; per light `0x006BB270`:
   same room id (`light+0x08 == rig+0x1E0`, and room 0 has id 0 on **every** story, so an outdoor rig takes outdoor lamps of
   any story), colour at the centre by vfunc+0x10, keep if luminance ≥ 0.1 (0.01 for HD objects).
   Modes 0/1: `0x006BBDE0` → `0x006BB2F0`: only the room's list, world-class lights dropped, same room required. Street
   lamps belong to room 0, so a fenced yard (mode 1) never got them.
3. `0x006BB1F0` sorts by luminance; `0x006BA340` fills slot 0 with the sun (ExteriorLightData, global), slots 1..3 with
   the 3 strongest (modes 1 and 2 only: `0x006BBDE0` passes the start slot = (mode == 1), so a room-mode rig, mode 0, has
   no sun and slot 0 holds its strongest room light; in an unlit room `0x006BB3E0` adds the three [NoLight] lights of
   CustomLightRigging.ini (resource 0xE23C85D8, each x sqrt(1 - sum / [0x11D0BB8]); x0.21 in the captured rooms:
   (0.0945 0.104 0.170), (0.042 0.042 0.105), (0.021 0.042 0.063), directions PS c0 / c2 / c3) and `0x006B7E70` the fill
   light (0.8 0.8 1.0) in slot 1 with w = 0.8 x its strength; the ambient cube of room-mode objects is CASDiffuseProbe,
   flat grey; second multi-agent study, 30/09), and the vertex-light slots only with lights beyond the first four (so with ≤ 4 lights they stay 0;
   with exactly 4 the 4th is dropped by the off-by-one at `0x006BA386`).
4. `0x006B92A0` caps the rig: the whole rig (sun included) is evaluated along ±X/±Y/±Z and scaled down when the maximum
   exceeds the cap `[0x011D0BA8]`.
5. Rigs re-gather when a lamp in their cells switches on/off, when the night level changes (`0x006B58F0` "redo all"), or
   when the object moves (NOTAS 3). Room-mode rigs keep the lamps of the moment they were placed.

Street lamp colour at the centre (`0x006C02A0` + `0x006BDB00`, NOTAS 3):
`F0 × range(+0x130) × I × fade(+0x20) × k2 × g² × k3 / (k1 × d²)`, d in 3D from the lamp head, **without** the ×3.333 the
ground gets: about 30 % of the ground's light next to a lamp, and dim lamps fall under the 0.1 cut.

#### Binding and draw

`0x006F6250` draws a SceneModel part and binds its rig through `0x006B8B30` at `0x006F68C5`. The binder only writes
pointers to rig memory into the shader parameter table `*0x011D7530`; the effect pass uploads the constants and issues the
DrawIndexedPrimitive in the same call tree, on the Present thread. Exceptions: instanced batches (`0x006CF920`, flushed
with the last rig) and Swarm effects (`0x0071CFB0`). The technique (interior vs exterior) follows the rig mode
(`0x006F4800`, table `0x00FF20F0`): only mode 0 uses the interior technique, which does **not** read the rig at all
(Counters interior reads the lot LightMap and LightBasisMap0..3). Modes 1 and 2 use the exterior technique.

Fences, rails, posts and railings (FenceRenderManager): each piece is an instance (0x60-byte record) of a SceneModelArray
grouped by {room, rail/post, model, product, compositor}; one rig at the centre of the group box and one instanced DIP per
group. The room is also looked up at the group centre, so a group whose centre falls in a roofed room gets mode 0 (only the
room's lights plus 4 grey fill lights; matches capture m32 with one grey light 0.354) (probable).

#### Where the rig lands in the shaders

| Family | Lamps | Sun | Other | Evidence |
|---|---|---|---|---|
| Object PS, exterior Phong / Counters, group A1 | dirs PS c1..c3, colours c5..c7 | dir c9, colour c8 | sky cube × cK.x; `add rX, rA, vC` adds COLOR0 (vertex lights) | counter_sh REPORT; NOTAS "Porta", "Sofa", "Balcao" |
| Group A2 (Phong) | c1..c3 / c5..c7, specular with `if_ne` | c13 / c12 | | counter_sh REPORT |
| Group B | 4-light chain c0..c3 / c4..c7 (c0/c4 = sun or a 4th lamp, unverified) | | | counter_sh REPORT |
| Group C (Phong) | none in the PS; light = max(COLOR0, object light map, sky) | | | counter_sh REPORT |
| Generic object PS (`PS_26AF02A0`, mailbox, fountain, windows) | c1..c3 / c5..c7 | c0 / c8 | | NOTAS 4c, m36 |
| Object VS vertex lights (COLOR0), 4 layouts | dirs c0..c3 / colours c4..c7; c4..c7 / c8..c11; skinned c180..c183 / c184..c187; c184..c187 / c188..c191 | | Phong adds an ambient `mad oC0.xyz, vNormal.w, cAmb, r` (cAmb = c15, c199, ...) | counter_sh REPORT section 4 |
| Instanced structures (fences, railings, stairs; VS with POSITION1/2) | VS c4..c7 / c8..c11 = VertexLightDirections/Colors only (overflow lights, usually 0) | | | NOTAS "CERCAS: causa real" |
| Instanced foliage, shrubs, trees (vs_2_0) | per instance dirs c27/c28/c29[a0], colours c54/c55/c56[a0] | c137/c138 (also c124/c125, c147 seen) | N·L clamp `max r0, r0, cK.w`; PS multiplies lamps by the moon shadow | NOTAS 3, 4, 4b, "Arbusto" |
| Stairs `VS_216C8910`, railings | VS c4..c7 / c8..c11 → COLOR0 | | | NOTAS 5, m03 |
| SpeedTree trees | TreeLightColors / TreeLightDirections (other path, not used by the measured trees) | | | NOTAS 4 |
| HD objects ("Night" technique) | up to 6 per-pixel lamps: positions c0..cN, colour cK, falloff `sat(w/d²)`, world position in TEXCOORD2 | | via rig+0x08 | NOTAS "Objetos de fora recusados"; censo |

### How Apex reads and patches lights and rigs

Readers (no game code changed):

| Reader | What it reads | Code |
|---|---|---|
| `LightDiag` (F8, dev) | Enumerates every light (`0x006ACF70`) and prints vtable, type, lot id, room, story, flags, position, range, intensity, base and effective colour; walks lots/stories/rooms; cone data | light_diag.cpp |
| `ReadLamp` / `UpdateLampList` (every 20 frames) | Alive (0x01) and lit (0x20) lights that are type 0xB or outdoor (0x04 and room 0); position +0x120; visual radius `clamp(sqrt(+0x130) × 1.2, 2, 25)` (the +0x134 rect, ~50 m, made roofs and water white); colour `F0 × I × fade`; per-pixel weight `W = 0.4 × range` | lot_light_bridge.cpp |
| `SelectLamps(x, z, maxScore)` | Up to 16 lamps with the smallest `horizontal distance − radius` from the drawn piece (≤ 80 m for roofs, 150 m for ponds), never from the camera | lot_light_bridge.cpp |
| `SelectPixelLamps` | 8 lamps that bring most light (W/d²) to an object's position (VS world triple translation) | lot_light_bridge.cpp |
| `ForEachOutdoorLotLamp` | Every lot light (lot id != 0) of type 3..6 or 11: type, story +0xD0, room, flags, rect, position, base colour, intensity, range (for the terrain reconciliation) | lot_light_bridge.cpp |
| `RigTracker::CurrentMode/CurrentCentre` | Rig bound for the current draw: mode +0x1D4, centre +0x140 (vtable checked) | rig_tracker.cpp |

Patches:

| Site | Change | Code | Option (TOML key, default) |
|---|---|---|---|
| Vtable slots +0x10 of the 9 classes | `ClassColour<I>`: call the original; only when the return address is `0x006BB2B3` (rig gather) and the lamp is lit: for the street class always, for the others only outdoor (flag 0x04, room 0) types 3..6; colour = max(original, `F0 × strength × I × fade × w`), `w = (1 − h²/R²)²`, h = horizontal distance from the head, R = half the rect width (0.5..100 m); luminance rec[10] recomputed with the weights at `0x011D1140` | object_light_bridge.cpp `BoostRec` | `postesNosObjetos` (true); strength `forcaNosObjetos` (1.0, 0.25..3) |
| `0x006B9418` operand | `mov ecx, &g_capScaled` with `g_capScaled = [0x011D0BA8] × max(1, strength)` updated every frame | object_light_bridge.cpp | same |
| Calls `0x006F7905/0x006F795C/0x006F799D` | `RigCtorForce`: sets bit 0 of the constructor flag (→ rig+0x224 bit 0x10) for every rig built there; affects rigs created after install (world load) | object_light_bridge.cpp | `lampadasEmTodosObjetos` (true) |
| Call `0x006BBE70` | `RoomGatherThunk`: after the room gather, a mode-1 rig also runs `0x006B5AF0(cells, rig)` with rig+0x1E0 = 0 temporarily; such rigs are remembered and re-gathered via `0x006BBF90` whenever the night level moves by 0.1 (vtable and mode re-checked under `__try`) | object_light_bridge.cpp | `lampadasEmTodosObjetos` |
| `0x006B58F0` | Called on the render thread after install/uninstall and strength changes (never from another thread: review 25/09) | object_light_bridge.cpp | |
| Creation colour calls (7) → `LampColourSet`; script call `0x006B0BDE` → `LampColourSetScript` (16-byte aligned buffer) | `TintStockColour`: if G/R within ±0.03 of 0.75 and B/R of 0.79, move toward warm white (1, 0.80, 0.62) with the same luminance, by `luzDasLampadasNatural` (0..1) | object_light_bridge.cpp | `luzDasLampadasNatural` (1.0), applies when a save loads |
| Binder call `0x006F68C5` → `BinderThunk`; Detours on `0x006F6250` and `0x006CF920` | Remember the rig for exactly the draw it belongs to (cleared on entry, restored on exit; instanced flushes clear it) | rig_tracker.cpp | installed with `objetosDeForaComLuzDoChao` (true) |
| vfunc+0x4C of the 9 classes | Cross-story wall test (see [room-light-maps.md](room-light-maps.md)) | level_light_share.cpp | `luzExternaEntreAndares` (true) |

The shader side (moon-shadow fade, fence ground light, object per-pixel lamps, foliage wrap) is in [shaders.md](shaders.md).
The HDR lamp gain that the combined build applied to rig colours and shaders existed only there; see
[../removed-features.md](../removed-features.md).

### Which Apex features depend on what

| Engine item | Apex features |
|---|---|
| Light layout (+0x08, +0x10, +0x20, +0xB0, +0xC0, +0xD0, +0xF0, +0x100, +0x120, +0x130, +0x134) | Terrain reconciliation, roofs, water, object per-pixel lamps, object boost, F8 |
| vfunc+0x10, `0x006BB2B3`, cap `0x006B9418` | Object boost ("Lamps light nearby objects") |
| Rig constructor calls, `0x006BBE70` | Stairs/railings/columns, fenced areas |
| `0x006F6250`, `0x006F68C5`, rig+0x1D4/+0x140 | Doors/windows/counters ground light and per-pixel lamps (only mode 2 and 1) |
| `0x006BDA90` callers, `0x006B0BDE` | Lamp colour |
| vfunc+0x4C, `0x0069FE19` | Level light share |

### Pitfalls and failed approaches

- "Forcing the bit 0x10 fixes fences and stairs": wrong premise. The SceneModel constructor already sets it for them; their
  missing lamp light comes from reading only the overflow vertex-light slots (`0x006BA340`) and from one shared rig per
  fence group (NOTAS "CERCAS: causa real"). `RigCtorForce` only helps objects the game deliberately closed.
- The first object boost only patched the street-lamp class slot `0xFF4308`; lot lamps (other classes) kept the 1/d²
  falloff (~0.1 at a few metres, under the 0.1 cut). Every lamp class goes through the boost now (NOTAS m41).
- `light_vtables.txt` listed 7 classes: CircleWindowLight (`0xFF4408`) and TubeLight (`0xFF4468`) were missing. Always use
  the 9 of the factory.
- The pink colour: patching only the creation setter was not enough; the lamp script sets the colour again through
  `0x006B0B50 → 0x006BC3E0` (NOTAS pending list item 1). `0x006BC3E0` uses MOVAPS: an unaligned buffer crashes (review item 2).
- The first object boost used 3× the cap; objects touching the lamp head, which already had a high 1/d², were limited by
  it. The cap is now the original × max(1, strength) (NOTAS 1b).
- Lamp radius from the rect (+0x134, ~50 m) turned roofs and ponds white; the visual radius uses `sqrt(range) × 1.2`
  instead (NOTAS 4d). Two lights per street lamp at the same place double any per-lamp glow.
- Choosing the 16 lamps nearest the camera made roofs flicker when zooming; select by the drawn piece's position.
- Zeroing the game's rig when per-pixel lamps are on left a gate 9..26 m from its lamps at ~13 % of the nearest one
  (kernel `sat(1 − d²/R²)²`, R ≈ 11.8 m); the patched shaders now take `max(rig + vertex lights, per-pixel, ground)` and the
  kernel follows the terrain bake's law (probe4_portao, 28/09).
- `DirtyAllRigs` from the message-loop thread (on uninstall) was unsafe: rig work only on the render thread.
- Room-mode rigs are not refreshed at dusk by the game; the extra cell gather would keep daytime lamps unless re-gathered.

### Open items / unverified

- The meaning of the constructor flag bits and of rig+0x225; k3 and g in the street-lamp rig formula.
- Whether vfunc+0x20 depends on the lit state.
- Group B objects: is PS c0/c4 the sun or a 4th lamp.
- `[0x011D1180].x` (rect divisor) and the runtime cap value `[0x011D0BA8]`.

## Address reference

| Address | Name / role | Evidence |
|---|---|---|
| `0x011D1860` | Root; `lightMgr = *(root+0x1C0)`; `cells = lightMgr+0x104` | object_light_bridge `kRootPtr`; `0x006E97B0`, `0x006E97D0` |
| `0x006AC590` | Light factory (a lightMgr method, reads `ecx+0x104`): `switch (type − 3)` over types 3..11, jump table `0x006AC7A0` | full.asm `006AC590..5B1`; exe bytes at `0x2AC7A0` |
| `0x006ACF70` | Enumerate every light: stdcall(visitor*), visitor vtable[0] = thiscall(visitor, Light*) ret 4 | light_diag `kEnumLightsBytes` |
| `0x006BC020` | `BaseLight::GetLotID()` (+0xC0/+0xC4) | split_level pattern `8B 81 C0 00 00 00 8B 91 C4 00 00 00 C3` |
| `0x009691E0` | vfunc+0x24 of all 9 classes: light position, thiscall(light, float out[4]) ret 4 | level_light_share `kLightPos` (checked per class) |
| `0x006BDDF0` | Sets the light rect +0x134..+0x140 from position and range | full.asm |
| `0x006BDA90` | Set light colour at creation: writes +0xF0 (base) and +0xE0 (intensity × colour when lit); callers `0x006C047D`, `0x006C051D`, `0x006C05C1`, `0x006C1251`, `0x006C15D1`, `0x006C1891`, `0x006C1B11` (one per lamp class constructor) | object_light_bridge `kSetColourCalls`; NOTAS "Cor rosada" |
| `0x006B0B50` | Script colour: (objId, r, g, b) → `0x006BC3E0(light, rgba)` for every light of the object (call at `0x006B0BDE`); stock and Build-mode colours both pass here | object_light_bridge `kScriptSetColourCall`; NOTAS pending list item 1 |
| `0x006BC3E0` | Set colour from script; reads its argument with MOVAPS (needs 16-byte alignment); caller passes (r, g, b, r) | object_light_bridge `LampColourSetScript` |
| `0x006B1850` | Per-object light dispatcher (calls `0x006B0B50` path and `0x006B08A0`) (inferred) | NOTAS; calls.tsv |
| `0x006B64B0` / `0x006B6090` / `0x006B6590` | Light register / remove / move-toggle in the cells (arm cells+0x38) | [terrain-and-light-bake.md](terrain-and-light-bake.md) |
| `0x006B58F0` | `DirtyAllRigs(cells)` __fastcall: every rig in the cells re-gathers ("refaz tudo"); runs on the render thread only | object_light_bridge `kDirtyAllRigs`, prologue `83 EC 10 55 8B E9 33 C9 33 C0 39 4D 30` |
| `0x006F7880` | Creates the rig of a SceneModel; constructor calls at `0x006F7905`, `0x006F795C`, `0x006F799D` | object_light_bridge `kRigCtorCalls` |
| `0x006BB8F0` | Rig constructor thiscall(rig, flag, model, kind); sets vtable `0x00FF4218` | object_light_bridge `kRigCtor`; rig_tracker `kRigVtable` |
| `0x006F5B40` | SceneModel constructor: sets model+0x29C bit 4 (0x10) | NOTAS "CERCAS: causa real" |
| `0x006F4840` | Sets/clears model+0x29C bit 4; called with 0 for terrain, roads, roofs, ceilings, sea, lot skirt, and from script message `0x827917CA` (`0x006FFCB0`) | NOTAS "Grade / corrimao", "CERCAS" |
| `0x006CEB20` | CAS compositor (once misread as the model constructor) | NOTAS "CERCAS: causa real" |
| `0x006B76B0` | Rig centre: transformed bounding-box centre → rig+0x140 | NOTAS "Counters/Phong: relatorio" |
| `0x006C7CF0` | Rig mode from the object's room: room != 0 → 0 (roofed) or 1 (roofless); room 0 or off-lot → 2 | NOTAS "Counters/Phong" |
| `0x006BAA70` | Puts a rig in mode 1 and takes it out of the world light cells | NOTAS "Cerca em area cercada"; object_light_bridge comment |
| `0x006BBBA0` | Outdoor gather (mode 2): needs rig+0x225 bit 1 and rig+0x224 bit 0x10; → `0x006B5AF0` | NOTAS 3, 5 |
| `0x006B5AF0` | Cell gather: the 3×3 light cells (32 m) around rig+0x140, per light `0x006BB270`; thiscall(cells, rig), `ret 4` at +0x145 | NOTAS 3; object_light_bridge (checks `C2 04 00` at `0x006B5C35`) |
| `0x006BBDE0` | Room gather (modes 0/1) → `0x006BB2F0` (call at `0x006BBE70`) | object_light_bridge `kRoomGatherCall` |
| `0x006BB2F0` | Room-list gather: drops world-class lights (vfunc+0x20 != 0), needs the same room | NOTAS "Resultado do teste" |
| `0x006BB270` | Per-light test: `light+0x08 == rig+0x1E0` (same room id), calls light vfunc+0x10 (return address `0x006BB2B3`), keeps it if luminance ≥ `[0x01158D60]` (0.1; HD objects `[0x01158D64]` = 0.01) | NOTAS 3; object_light_bridge `kRigGatherReturn` (`FF D2` before it checked); dwords.txt |
| `0x006BDB00` | Common record format of vfunc+0x10 results: rec[4..6] = colour, rec[10] = luminance (weights at `0x011D1140`) | NOTAS "Reforco"; object_light_bridge `BoostRec` |
| `0x006BB1F0` | Sorts the gathered lights by luminance | NOTAS 3 |
| `0x006BA340` | Distributes: slot 0 sun, slots 1..3 the 3 strongest, overflow to the 4 vertex lights (`vcount = min(4, n − 4)` from `list[count..]`); off-by-one at `0x006BA386` (`add eax,-4`) drops `list[3]` when n = 4 | NOTAS 3, "CERCAS: causa real" |
| `0x006BBF90` | Unconditional rig update __fastcall(rig) | object_light_bridge `kRigUpdate` |
| `0x006B92A0` | **Rig brightness cap** thiscall(rig, x) ret 4: evaluates the rig's light in 6 axis directions (`0x006B7D20` ×6), takes the component max, gets a scale from `0x006B74D0(max, cap)`, rescales with `0x006B81E0` if the scale < 1. The cap is read at `0x006B9418` (`B9 A8 0B 1D 01` = `mov ecx,0x011D0BA8; call 0x00F626F0`) | full.asm `006B92A0..9462`; object_light_bridge `kCapOrig` |
| `0x011D0BA8` | Cap tweakable (runtime value, .bss) | object_light_bridge `kCapGlobal` |
| `0x006B8B30` | Binder __fastcall(rig): stores pointers to rig arrays in the shader parameter table `*0x011D7530`; sends zeros for lamps unless rig+0x224 bit 0x10 | NOTAS 3, 5, "Porta escura" |
| `0x01158C74`, `0x01158C78` | Parameter handles of VertexLightDirections / VertexLightColors | NOTAS "CERCAS: causa real" |
| `0x006F6250` | SceneModel part draw thiscall(model, part, ctx) ret 8 (vtable `0x00FF97F8` +0x18; also called from `0x006F83B0`, `0x006FA0B0`); binds the rig at `0x006F68C5` (ECX = rig, EBX = model); prologue `55 8B EC 83 E4 F0 81 EC 94 01 00 00` | rig_tracker `kModelDraw`, `kBinderCall` |
| `0x006F4800` | Chooses interior/exterior technique from the rig mode (ctx+0x34, table `0x00FF20F0`) | NOTAS "Counters/Phong", "Porta escura" |
| `0x006CF920` | Instanced batch flush thiscall(self, 5 args) ret 0x14 (can run inside `0x006F6250`); prologue `55 8B EC 83 E4 F0 81 EC A4 0B 00 00` | rig_tracker `kInstanceFlush` |
| `0x0071CFB0` | Swarm (effects) draw, binds the owner's rig at `0x0071D314` | rig_tracker comment |
| `0x00A6C6F0` / `0x00A6CBE0` | FenceRenderManager `AddRailSceneModel` / `AddPostSceneModel` | NOTAS "CERCAS: causa real" |
| `0x006F9A10` | SceneModelArray constructor (0x340 bytes, vtable `0x00FF99D8`); group centre `0x006F8930` / `0x006F8C70` | NOTAS "CERCAS: causa real" |

## See also

- [Terrain and light bake](terrain-and-light-bake.md): the terrain stamp consumer of the same lights.
- [Room light maps](room-light-maps.md): the room solve consumer.
- [Shaders](shaders.md): where the rig lands in the shader constants.
- [Night Lighting](../features/night-lighting/README.md).
