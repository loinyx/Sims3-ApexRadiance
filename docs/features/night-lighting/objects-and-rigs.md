# Objects and rigs

Lamps light outdoor objects the way they light the ground: fences, bushes, props, stairs and railings next to a street
lamp or a lot lamp are no longer left dark, doors and windows are never darker than the wall around them, and the pieces
of a modular counter show one continuous light instead of colour steps. Objects keep the game's materials, sun and
shadows. Part of [Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released (present since 2.1.0, the first version in this repository). Daytime response of the per-pixel terms: in development (PR #2) |
| Default | On (all switches), Brightness 100%, Seamless light brightness 100% |
| Menu | Lighting > Objects (cards *Objects* and *Doors, counters and fences*) |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/object_light_bridge.cpp`](../../../features/object_light_bridge.cpp), [`features/rig_tracker.cpp`](../../../features/rig_tracker.cpp), [`features/shader_patches.cpp`](../../../features/shader_patches.cpp) (`PatchObjectLampVs`, `PatchObjectLampPs`), [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) (`DrawObjectLamp`), [`features/terrain_lighting_policy.h`](../../../features/terrain_lighting_policy.h) |

## The problem

Every outdoor object in The Sims 3 is lit by its own light rig: the sun plus the 3 strongest point lights measured at the
object's centre, plus up to 4 overflow vertex lights ([light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md)).

- A street lamp gives a rig about 30% of the light the ground under it gets, and dimmer lamps fall under the 0.1
  luminance cut. Lot lamps of other light classes contributed about 0.1 or nothing (captures m32-m41).
- Stairs and railings outside had all rig lamp constants at zero (LightProbe-escada, m03).
- Fences closing an area received no street lamp (m15).
- Doors and windows were darker than the lit wall beside them (m52, m56): the rig lights them with 3 lamps from above at
  a grazing angle.
- Modular pieces outside (counters, shelves, railings) each have their own rig evaluated at their own centre, so
  neighbours get different light and show colour steps. Indoors, the interior Counters technique reads the room light
  map, so it is continuous.

## How Apex Radiance solves it

Three layers:

1. **CPU rig boost** (`ObjectLightBridge`). When a rig gathers lamps, each lamp's colour record for the rig is raised to
   at least its ground-footprint brightness at that point, for all 9 light classes. The rig cap rises with *Brightness*
   above 100%. Objects the game or their script closed to lamps are opened, and rigs in fenced yards also gather street
   lamps.
2. **Rig tracking** (`RigTracker`). Apex records which rig is bound for the current draw, so the draw code knows the
   rig's mode (outdoor, fenced yard, indoor) and centre.
3. **Per-pixel patch of outdoor object shaders.** Outdoor Counters and Phong object shaders are patched by pattern. The
   patched shader reads the ground light atlas below the pixel and up to 8 world lamps evaluated at the pixel, the same
   lamps for every piece near each other: `max(sun + rig or per-pixel lamps, ground)`, never darker than the game where
   the rig is weak.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Objects > Lamps light objects | `postesNosObjetos` | bool | on | | Installs `ObjectLightBridge` (rig boost, cap, rig opening, fenced-yard gather) and turns on the moon-shadow and foliage fixes (`LotLightBridge::SetObjectShadowFix`) |
| Objects > Brightness | `forcaNosObjetos` | float | 100% | 25 to 300% | Scales the rig boost, raises the rig cap to `cap x max(1, s)`, and sets the ground-light strength on patched objects. A change re-gathers all rigs (`FUN_006b58f0`) |
| Objects > Light stairs, railings, columns | `lampadasEmTodosObjetos` | bool | on | | Opens rigs the game created closed to lamps; applies to rigs created afterwards (world load; the row carries a reload badge). Also gates the fenced-yard gather |
| Doors, counters and fences > Doors and windows stay lit | `objetosDeForaComLuzDoChao` | bool | on | | Installs `RigTracker` and enables the per-pixel object patch (`DrawObjectLamp`). Disabled unless *Street lamps light lots* and *Smooth ground light* are on (the card offers a button to turn them on) |
| Doors, counters and fences > Seamless light on pieces | `luzPorPixelNosObjetos` | bool | on | | Per-pixel world lamps on objects (and on fences). Off: the lamp blocks are zero and only the ground term remains |
| Doors, counters and fences > Seamless light brightness | `forcaLuzPorPixelNosObjetos` | float | 100% | 25 to 300% | Strength of the per-pixel lamps (`cS.y`) |

All apply live. Every frame the Present hook calls `ObjectLightBridge::SetStrength(forcaNosObjetos)`,
`SetAllObjects(lampadasEmTodosObjetos)`, `LotLightBridge::SetObjectPixelLamps(objetosDeForaComLuzDoChao &&
RigTracker::IsInstalled(), forcaNosObjetos)` and `SetObjectPixelLights(luzPorPixelNosObjetos,
forcaLuzPorPixelNosObjetos)`. The *Fences and stairs catch light* rows of the same card are documented in
[fences.md](fences.md); *Smooth indoor light* (Indoor objects card) in
[world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md).

## Compatibility and interactions

- **Ground light atlas** ([world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md)): required by the
  per-pixel patch; it exists only with *Street lamps light lots* and *Smooth ground light*.
- **Fences** ([fences.md](fences.md)) share the per-pixel lamp code and use the rig centre from `RigTracker`, so their
  per-pixel lamps also need *Doors and windows stay lit*.
- **Foliage** ([foliage.md](foliage.md)): bushes and trees get the CPU rig boost, not the per-pixel patch.
- **Lamp colour** ([lamp-colour.md](lamp-colour.md)): the colour `F0` read here is the tinted colour.
- **Light between stories** ([level-light-share.md](level-light-share.md)): walls and windows now agree on which lamps
  light a story. Its directional-map guard lets Apex's indoor-object shader drop its floor-map cap (see *Indoor objects*).
- **Rooms at Night** ([unlit-rooms.md](unlit-rooms.md)) adjusts the unlit-room lights of indoor (room-mode) furniture rigs.
- **Floors**: the summer floor VS also classifies as an object VS; `DrawObjectLamp` declines it (no outdoor rig) and the
  floor branch handles it ([floors.md](floors.md)).

## Limitations

- Rigs and per-pixel lamps have no wall shadow: objects inside a U of walls can get lamp light through a wall.
- The ground term is the light of the ground below, without height: an upper-story window can pick up ground light.
- Specular from rig lamps (c5 read twice in 84 Counters and 300 Phong pixel shaders) is not replaced, so highlights can
  still differ between pieces.
- Shape B: whether c0/c4 is the sun or a fourth rig lamp is not verified; c4 is not treated as a lamp.
- Only 8 lamps per draw, chosen at the object's translation; a very large object far from its origin can miss one.
- Summer instanced shrubs and fences drawn with the 600-byte PS `kObjectRigPs` go to `DrawObjectRig` (moon-shadow HLSL)
  and get neither the ground light nor per-pixel lamps.
- Not covered: OutdoorProp `C0C6E0FF` (and `2BE88B48`): 1 lamp (c1 -> c3) and the sun (c5 -> c4), `mad rA, sky x c8.x,
  AO, rD` with if/else before, VS already using TEXCOORD7; instanced SingleObject/InstancedObject `PS_D4FD3CB3` /
  `VS_9E59FCE3` and `8F40BA1C` (POSITION1 instancing is refused); Sims; indoor (mode 0) objects by design.
- Fenced-yard rigs re-gather only when the night level moves by 0.1, not on single lamp changes.
- Steam 1.67.2 only. `ObjectLightBridge` fails to install with *Street lamp light function differs at 0xFF4308* when
  the street-lamp slot differs; other slots that differ are skipped.

## Technical reference

### The game's rig

- Every SceneModel has its own rig (`FUN_006f7880`); only `SceneModelArray` (fences) shares one rig per group. Rig
  vtable `0x00FF4218` (constructor `FUN_006bb8f0`).
- Layout: `+0x08` HDLight*, `+0x10` LightDirections[4], `+0x50` LightColors[4] (slot 0 sun, 1..3 lamps), `+0x90` /
  `+0xD0` VertexLightDirections/Colors[4], `+0x140` centre (transformed bounding-box centre, `FUN_006b76b0`), `+0x1B4`
  light manager, `+0x1D4` mode, `+0x1E0` room id, `+0x224` bit `0x10` accepts dynamic lights (bit `0x20` gets the fill
  light), `+0x225` bit 1.
- Mode (`FUN_006c7cf0`): room != 0 with a roof -> 0 (interior technique), roofless room -> 1, room 0 or outside a lot -> 2.
  Only mode 0 uses the interior technique (`FUN_006f4800` -> ctx+0x34, technique table `0x00FF20F0`).
- Outdoor gather: `FUN_006bbba0` (only if `rig+0x225 & 1` and `rig+0x224 & 0x10`) -> `FUN_006b5af0` walks the 3x3 cells of
  32 m around `rig+0x140` -> `FUN_006bb270` per light: requires `light+8 == rig+0x1E0`, calls light `vfunc+0x10` (colour
  record at the point), keeps it if luminance >= 0.1 (`0x01158D60`; HD objects 0.01 at `0x01158D64`). Room gather (modes
  0/1): `FUN_006bbde0` -> `FUN_006bb2f0` (room light list; rejects world-class lights).
- Sort and distribution: `FUN_006bb1f0` sorts, `FUN_006ba340` copies the 3 strongest to slots 1..3 and only the excess to
  the vertex-light slots (`vcount = min(4, n - 4)`; the off-by-one at `0x006BA386` `add eax, -4` also drops `list[3]` when
  n = 4).
- Cap: `FUN_006b92a0` scales the rig down above a cap read at `0x006B9418` (`B9 A8 0B 1D 01` = `mov ecx, 0x011D0BA8`); the
  cap includes the sun.
- Binder: `FUN_006b8b30(rig)` stores pointers into the rig in the shader parameter table (`*0x011D7530`); the effect pass
  uploads the constants and draws in the same call tree on the Present thread. If `rig+0x224 & 0x10` is clear it sends
  zeros.
- Street-lamp colour for the rig (`vfunc+0x10` of class `0xFF42F8` = `FUN_006c02a0`, record format `FUN_006bdb00`):
  `F0 x (+0x130) x I x fade(+0x20) x k2 x g² x k3 / (k1 x d²)`, d = 3D distance from the lamp head, without the x3.333 the
  ground gets.
- Updates: a rig re-gathers when a lamp turns on or off, through the night level (`FUN_006b58f0` dirties all rigs in the
  cells), or when the object moves. Room-mode rigs are removed from the cells (`FUN_006baa70`) and are not re-gathered at
  dusk.

### CPU fixes (`ObjectLightBridge`)

**Rig boost.** Each class's `vfunc+0x10` slot is replaced by `ClassColour<I>`:

| Slot | Original | Class |
|---|---|---|
| `0x00FF42B0` | `0x006C02A0` | vtable `0xFF42A0` |
| `0x00FF4308` | `0x006C02A0` | vtable `0xFF42F8`, street lamps (`kStreetClass`, type 11) |
| `0x00FF4360` | `0x006C0690` | vtable `0xFF4350` |
| `0x00FF43B8` | `0x006C0AF0` | vtable `0xFF43A8` |
| `0x00FF44D0` | `0x006C16D0` | vtable `0xFF44C0` |
| `0x00FF4528` | `0x006C1980` | vtable `0xFF4518` |
| `0x00FF4580` | `0x006C1BC0` | vtable `0xFF4570` |
| `0x00FF4418` | `0x006C0FE0` | vtable `0xFF4408` CircleWindowLight |
| `0x00FF4478` | `0x006C1320` | vtable `0xFF4468` TubeLight |

`ClassColour<I>` calls the original, then, only when the return address is `0x006BB2B3` (after `call edx` in
`FUN_006bb270`; bytes `FF D2` at `0x006BB2B1` checked), runs `BoostRec`:

- the lamp must be lit (`light+0x100 & 0x20`); non-street classes must also be outdoor (`flags & 0x04` and room `+0x08 ==
  0`) and of type (`+0xB0`) 3..6, so interiors are unchanged;
- radius R = half the width of the light bounds `+0x134` {minX, minZ, maxX, maxZ}, accepted if 0.5 < R < 100;
- horizontal distance h from the head (`+0x120`), `w = (1 - h²/R²)²` (the terrain-stamp footprint);
- `s = forcaNosObjetos x intensity(+0x10) x fade(+0x20) x w`; record colour `rec[4..6] = max(rec, F0(+0xF0) x s)`; the
  luminance `rec[10]` is recomputed with the game's weights at `0x011D1140`.

A slot that does not hold the expected function is left alone; status *light classes: N/9*.

**Cap.** The operand at `0x006B9418` is redirected to `g_capScaled`, updated every frame to `*(0x011D0BA8) x max(1,
forcaNosObjetos)`.

**Opening closed rigs** (`RigCtorForce`). The 3 constructor calls in `FUN_006f7880` (`0x006F7905`, `0x006F795C`,
`0x006F799D`; target `0x006BB8F0`, thiscall `(rig, flag, model, kind)`) set bit 0 of `flag` when
`lampadasEmTodosObjetos` is on; the constructor turns it into `rig+0x224 & 0x10` (inferred from the RE). The SceneModel
constructor `FUN_006f5b40` already sets the model bit (`model+0x29C` bit 4); the game clears it with `FUN_006f4840(0)` only
for terrain, roads, roofs, ceiling, sea, lot skirt, and objects whose script asks (message `0x827917ca` -> `FUN_006ffcb0`
-> `FUN_006f4840`). So this opens only deliberately closed objects; fences and stairs are dark for another reason (their
vertex-light slots, see [fences.md](fences.md)). Status: *objects opened to lamps*.

**Fenced yards (rig mode 1).** The call at `0x006BBE70` (`FUN_006bb2f0` inside `FUN_006bbde0`) goes through
`RoomGatherThunk`: after the room gather, for a mode-1 rig with `+0x224 & 0x10`, the world-cell gather `FUN_006b5af0(cells =
*(rig+0x1B4)+0x104, rig)` runs with `rig+0x1E0` temporarily 0 (restored in `__finally`). The game keeps the 3 strongest of
both lists. Validation: the call's target and `ret 4` (`C2 04 00`) at `0x006B5AF0 + 0x145`. Served rigs are remembered (at
most 8192) and re-gathered on the render thread with `FUN_006bbf90` (unconditional rig update) whenever the night level
(`lightMgr+0xF0`) moves by 0.1, after validating vtable `0xFF4218` and mode 1 under SEH; cleared on a world change (cells
pointer changed).

**Refresh.** Install, strength change and uninstall request `DirtyAllRigs` = `FUN_006b58f0(cells)` (13-byte prologue `83
EC 10 55 8B E9 33 C9 33 C0 39 4D 30`), always on the render thread (`OnPresent`); an uninstall off the render thread only
sets a flag.

### Rig tracking (`RigTracker`)

- The call at `0x006F68C5` (`CALL FUN_006b8b30`, ECX = rig) goes to `BinderThunk`, which stores the rig.
- Detours on `FUN_006f6250` (SceneModel part draw, thiscall `(model, part, ctx)`, `ret 8`; vtable `0xFF97F8 +0x18`, also
  called from `0x006F83B0`, `0x006FA0B0`; prologue `55 8B EC 83 E4 F0 81 EC 94 01 00 00`) and `FUN_006cf920` (instanced
  batch flush, thiscall + 5 args, `ret 0x14`; prologue `55 8B EC 83 E4 F0 81 EC A4 0B 00 00`). The model-draw hook clears
  the rig on entry and restores it on exit (binds without a draw, nested draws and pass flag 8 never leak); the flush hook
  clears it while it runs.
- `CurrentMode()` = `rig+0x1D4` when installed, depth > 0, rig set, same thread as the draw and vtable `0xFF4218`; else
  -1. `CurrentCentre()` = `rig+0x140`.
- The Swarm effects path (`FUN_0071cfb0`, binder at `0x0071D314`) is not tracked.
- Installed only while `objetosDeForaComLuzDoChao` is on.

### Per-pixel patch

**Dispatch** (`lot_light_bridge.cpp`). A vertex shader is class 10 when `PatchObjectLampVs(t, needColor0 = true)` accepts
it and no earlier class matched (order: roof, lake, snow lot, floor by exact id; road; instanced structure; snow cover;
snow relief; floor pattern; foliage; object; snow floor). In `OnDrawInner`, after the *Street lamps light lots* gate and
the road, floor, fence and snow branches, `DrawObjectLamp` runs; when it declines, the draw falls through to the remaining
branches (roof and snow VS can also be class 10). It requires `objetosDeForaComLuzDoChao` with RigTracker installed, rig
mode 2 or 1, the ground atlas (`LightmapSmooth::Atlas`), the PS accepted by `PatchObjectLampPs` (cached per PS pointer,
`g_objLampPs`) and the patched VS. It swaps VS and PS, sets the constants, draws and restores everything. Every
classified shader is AddRef-pinned until shutdown so a freed address cannot inherit a stale class.

**Vertex shader** (`PatchObjectLampVs`, vs_3_0 only):

- Flow control: only `if`, `ifc`, `else`, `endif` (opcodes 0x28..0x2B) and `ret` (0x1C); `call`, `loop`, `rep`, `break`,
  `label` refuse the shader. Skinned doors with `mova` and `if b0` (m57) pass because the world triple (c199..c201 there)
  is outside the `if`.
- POSITIONn inputs only together with NORMALn (morph targets: 176 Phong VS blend POSITION1..3/NORMAL1..3 with c26);
  POSITION1/2 without NORMAL = instancing, refused.
- Needs a COLOR0 output (object family), TEXCOORD8 free, max output register < 11.
- World position = a triple `dp4 rW.x/.y/.z, rP, cK/cK+1/cK+2` outside branches with fixed constants (no `c[a0]`). A triple
  stays open until something writes rP or a component of rW it already has (split triples, Phong_VS_3875). With several
  full triples: the one whose source's last writer reads POSITION0, otherwise the unique root triple (not computed from
  another triple's result, Phong_VS_3810).
- Inserts `dcl_texcoord8 oN.xyz` (N = max output + 1) and `mov oN.xyz, rW.xzy` after the last `dp4` of the triple.
- Reports `worldK` (`c[K..K+2].w` = object translation; in practice c12/c15/c16/c19 unskinned, c192/c195/c196/c199
  skinned) and `vertexLight` (base of the rig's 4 vertex-light colours: 4 consecutive `dp3_sat rS.s, cD, rN` + `mul/mad
  rX.xyz, rS.s, cD+4` whose last step writes or feeds COLOR0; c4, c8, c184 or c188 in all 588 SM3 VS).

**Pixel shader** (`PatchObjectLampPs`, ps_3_0 only; everything read or inserted comes before the first flow-control
instruction; TEXCOORD8 must not be read already; max input register < 9). Three shapes:

| Shape | Recognised by | Where the code goes |
|---|---|---|
| A | rig lamp chain ending `mad rD.xyz, rS.s, c7, rP`, then sky `mad rA.xyz, rCube, cK.s, rD` (rCube from a cube `texld` at the normal), optionally `add rX.xyz, rA, vC` (COLOR0 vertex lights; destination may differ, for example `add r4.xyz, r0, v0`); also the no-COLOR0 variant (PS_298DF5B8) and the 4-light specular variant (c0..c3 / c4..c7, PS_2D041628) | ground and lamp code before the cube `texld`; combine before the sky `mad` |
| B | 4-light chain c0..c3 / c4..c7, no cube, no COLOR0 (8 Counters, Counters_PS_440) | ground and lamp code before the chain end (which may overwrite the normal); combine right after it. Normal = the unique register used in `dp3` with c1 and with c0/c9/c13 |
| C | no lamps in the PS: light = `max(vC, per-object light map, sky)` (32 Phong, for example Phong_PS_3095): the single `max rX.xyz, vC, rY` and the single cube `texld` | at the cube `texld` if it comes first (Phong_PS_3113 overwrites the normal before the max), else at the max; the max reads the new register instead of vC |

For A and B the normal register must also be used with c1 (first lamp) and with the sun (c9, c0 or c13), and hold the same
value at the insertion point as at its reference use.

Resources added (refused if `maxConst + 4 + 16 >= 224`, `maxTemp + 5 >= 32` or sampler 15 taken):

| Register | Content |
|---|---|
| `v(maxIn+1)` TEXCOORD8.xyz | world (x, z, y) from the VS |
| `s(maxSampler+1)` | ground light atlas (CLAMP, LINEAR min/mag, no mip) |
| `cA` | atlas mapping: uv = world.xz x cA.xy + cA.zw |
| `cB` | `.x` = ground strength |
| `cH` (def) | (0.5, 0.5, 1, 0) |
| `cS` | (0, per-pixel lamp strength, 0, 1e-4) |
| `cL .. cL+15` | 8 lamps x {(head x, y, z, 1/R²), (colour r, g, b, 0)} |
| temps | T, Fr, A, B, Q (T+0..T+4) |

Per pixel:

1. Ground term `G = atlas(world.xz) x (0.5 + 0.5 N.y) x cB.x`. Shape A with COLOR0: `G -= vC` (vC is added after the
   combine).
2. Per-pixel lamps `Q = sum_k colour_k x sat(N.l_k) x sat(1 - d²/R_k²)²`, d = 3D distance from the pixel to the lamp
   head, `d² >= 1e-4`. The `1 - d²/R²` term is computed in two instructions because native D3D9 accepts one constant
   register per instruction.
3. Shapes A and B: `rD = max(rD + Q x cS.y, G)`. Shape C: the game's `max rX.xyz, vC, rY` reads `max(vC + Q x cS.y, G)`
   instead of vC (`rigLamps = false`).
4. While per-pixel lamps are on and lamps were selected, the draw zeroes the rig's lamps (PS c5..c7 for shapes A/B and
   the VS vertex-light colours at `vertexLight`), so Q replaces them; the Phong ambient term in COLOR0 stays. They are
   restored after the draw.

**Strengths.** `cB.x = SurfaceLampGain(night, forcaNosObjetos)` and `cS.y = SurfaceLampGain(night,
forcaLuzPorPixelNosObjetos)`, where `SurfaceLampGain(n, g) = d + (g - d) x n` with `d = min(g, 1) x 0.08`: the configured
value at full night, a subdued 8% response by day. Lower strengths are never raised. The same policy applies to fences,
instanced structures and snow on objects. The CPU rig boost has no day factor.

**Lamp list and selection.** `LotLightBridge::OnPresent` every 20 frames, when roofs, water or per-pixel lamps are on,
runs `ReadEnumeratedLamps`: `FUN_006acf70(visitor)` (stdcall, visitor vtable[0] = thiscall `(visitor, Light*)`; prologue
`E8 2B 36 00 00 8B 4C 24 04 51 68 40 CF 6A 00`) enumerates every light, and `g_allLamps` keeps those alive (`0x01`) and lit
(`0x20`) that are street lamps (type `0xB`) or outdoor lamps (`flags & 0x04` and room 0), with head `+0x120`, radius
`clamp(1.2 sqrt(range), 2, 25)` (range `+0x130`) and colour `F0 x I x fade`. `SelectLamps(x, z, 40)` at the object's
translation (`c[worldK..+2].w`) scores each lamp by horizontal distance minus radius, drops scores above 40 m, keeps the
best 16 (memoised per position and lamp-list generation), and the draw uses the first 8. Unused blocks: position
(1e6, 0, 1e6), colour 0.

### Indoor objects (directional maps)

Apex's indoor-object shader (`PatchIndoorBasis`) normally caps the basis light at `min(basis, 2 x room light map)` per
channel (`kBasisCap`). While the light-between-stories directional-map guard is installed and ready
(`LevelLightShare::BasisFloorGuardReady`), a separate cached variant without the cap is used (`g_indoorPs` keys sampler
0..7, +8 for the uncapped variant), because the cap transferred floor-height shadows onto wall objects (the cap removes 12
tokens, three instructions). Without the guard the capped variant stays.

### Address reference

| Address | What | Verification |
|---|---|---|
| `0x00FF4308` | street-lamp class `vfunc+0x10` slot = `0x006C02A0` | install fails otherwise |
| other 8 `+0x10` slots | see table | each compared, skipped if different |
| `0x006BB2B3` | return address in the rig gather `FUN_006bb270` | `FF D2` at -2 |
| `0x006B9418` | `B9 A8 0B 1D 01` (cap operand) | `ValidateBytes`, rewritten to `B9 <&g_capScaled>` |
| `0x011D0BA8` | cap global | read every frame |
| `0x011D1140` | luminance weights | read |
| `0x006B58F0` | `FUN_006b58f0(cells)` dirty all rigs | 13-byte prologue |
| `0x011D1860` | root; `+0x1C0` light manager; `lightMgr+0xF0` night level, `+0x104` cells | read under SEH |
| `0x006F7905`, `0x006F795C`, `0x006F799D` | rig constructor calls (target `0x006BB8F0`) | `E8` + target |
| `0x006BBE70` | `CALL FUN_006bb2f0` in `FUN_006bbde0` | target, and `C2 04 00` at `0x006B5AF0+0x145` |
| `0x006B5AF0`, `0x006BBF90`, `0x00FF4218` | cell gather, rig update, rig vtable | used |
| `0x006F68C5` | binder call (target `0x006B8B30`) | `E8` + target |
| `0x006F6250`, `0x006CF920` | model part draw, instanced flush | 12-byte prologues, Detours |
| `0x006ACF70` | light enumeration | 15-byte check |
| `0x01158D60` / `0x01158D64` | gather luminance cut 0.1 / 0.01 | from RE (not patched) |

### Shader coverage

Captured object shaders (LightProbe names are session pointers): door/window `PS_2CE14418` (= `PS_2C72FF40`), sofa
`PS_2CE16998` (normal-mapped), outdoor counter `PS_2947EBF8` / `VS_29586D20`, generic object (mailbox, fountain, some
windows) `PS_26656138` = `PS_26AF02A0` with `VS_2665D348` / `VS_26AF66A0`, animated door `VS_2C744058`, Phong outdoor
`C249A5C0` and `BC7DE009` (census, rig 2), `VS_E3A718D3` (world c19..c21, view c12..c14). Game constants: sun direction c9
/ colour c8 (or c0/c4, or c13/c12 in Phong), lamps c1..c3 / c5..c7, vertex lights in VS COLOR0.

Offline coverage over the 2139 Counters/Phong variants of `Shaders_Win32.precomp`:

| | Accepted |
|---|---|
| VS (SM3 Counters + Phong) | 588 / 588 |
| PS Counters | 136 / 184 (A 128, B 8) |
| PS Phong | 426 / 444 (A 394, C 32) |
| Invalid after patch | 0 |

Not covered: 34 PS that already use all of v0..v9 (no free input; packing into COLOR1.yzw was rejected for precision
risk) and 32 Counters "group D" (directional baked maps, no lamps; vanilla lamps never affect them either). TEXCOORD8+ is
used by none of the 2139 shaders (Counters uses TEXCOORD7 for the sink/stove cut-out UV, Phong for a projective
coordinate), hence the fixed index 8 in both VS and PS, which are patched independently. "HD" objects with their own
per-pixel lamps (6 lights: `2D0547A0`, `2D0555B0`, `2D0558D0`, `2D055998`, `2D055BF0`; 4 lights: `2D068BD8`; position in
TEXCOORD2, lamps in c0..cN, falloff `saturate(w/d²)`) need nothing. `2BE88B48` (2 lights, no c7), `2E279A20` (7-tap soft
shadow, inverted chain, COLOR0 in a mad) and `2E255A58` (308 instructions, flow control) stay vanilla. `PS_2A74E378` is
the wall family ([walls.md](walls.md)).

### Diagnostics (developer mode)

- Status: `Objects: Active | light classes: 9/9 | lights boosted on objects: N | objects opened to lamps (stairs,
  railings...): N | in fenced areas: N`; `Street lamps on lots: ... outdoor objects: N` (draws).
- Log: `[ObjectLightBridge] Installed`, `[RigTracker] Installed`, and the batched `Shaders at their first draw` lines.
- F7: `LotLightBridge::DescribeDraw()` adds a `mod:` line per draw (shape, rig mode, per-pixel lamps and strengths, the
  object position, the lamps used of N in reach with position, 1/R², colour and distance, the game's rig PS c0..c13 and
  the VS vertex lights; for a class-10 draw not replaced, the reason). F7 on two pieces lists the registers that changed.
- Refused shaders are saved to `Apex Radiance\ShadersRecusados\<fix>_PS_<FNV32>.bin` and `<fix>_PS_<FNV32>_VS_<FNV32>.bin`
  (named by content hash, written once, at most 300 per session). Census false colour: magenta = lamp-lit candidate no
  fix claimed.

## Rejected approaches

- Boosting only the street-lamp class: other lot lamps stayed near 0.1. Details in
  [history](../../history/night-lighting-objects-and-rigs.md).
- A 3x cap times strength: blew out objects touching a lamp head.
- `RigCtorForce` as the fence and stair fix: the bit is already set; the cause is elsewhere.
- Subtracting vC in the PS to remove vertex lights: also removed the Phong ambient term.
- TEXCOORD7 for objects, and treating POSITION1..3 as instancing.
- A class-10 dispatch that returned early and starved roofs and snow.
- Two-constant instructions: valid on DXVK, refused by native D3D9.
- The combined build's bake-matched per-pixel law (`W = 0.4 x range`, rig kept, `sat(N.y + 1)`, ground strength
  `max(1, s)`) has not been ported; see history.

## See also

- [Validation](../../validation/night-lighting-objects-and-rigs.md)
- [History](../../history/night-lighting-objects-and-rigs.md)
- [Light objects and rigs](../../engine/light-objects-and-rigs.md), [Shaders](../../engine/shaders.md),
  [Light probe](../dev-tools/light-probe.md), [Census](../dev-tools/census.md)
- [Fences](fences.md), [Foliage](foliage.md), [Lamp colour](lamp-colour.md), [Roofs](roofs.md)
