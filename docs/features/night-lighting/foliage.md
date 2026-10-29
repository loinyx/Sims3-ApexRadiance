# Foliage: bushes, trees and plants

At night, bushes, hedges, trees and small plants near lamps are lit on every side the lamp can reach. Each leaf takes
the light of its own distance, height and side towards each lamp, so a small lamp at the foot of a tree lights the trunk
and the lower leaves while the top of the crown stays dim, and tree crowns shade the lamp light on the lot's ground. The side of a
planter in moon shadow keeps its lamp light, and the back of a bush facing away from a lamp is dim rather than black. The
same fixes apply to the snowy winter variants and to the fences that share the bush shader. Part of
[Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released in 1.0.0. The daytime curve on foliage lamp colours and garden lamps blocked by buildings: Released in 2.13.0. Per-vertex lamps and crown shade on lot ground: Released in 2.14.0. Trees kept on the normal light rig instead of the underground one: Released in 2.15.3. Lamp brightness and saturation aligned with dusk and dawn: Released in 2.15.6. |
| Default | On |
| Menu | Lighting > Outside > Objects > *Objects* card; the reach law also needs *Realistic lamp reach* (Lighting > Overview) and *Outdoor light between floors* (Lighting > Inside & floors > Stories) |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) (`DrawObjectRig`, `FoliageVsFor`, `FoliageVertexVsFor`, `DrawLeafShadow`, `NoteCrown`, `CrownsIn`), [`features/shader_patches.cpp`](../../../features/shader_patches.cpp) (`PatchFoliageVs`, `PatchLeafShadow`), [`features/foliage_vertex_lamps.h`](../../../features/foliage_vertex_lamps.h), [`features/foliage_canopy.h`](../../../features/foliage_canopy.h) |

## The problem

Foliage is lit per vertex by a per-instance rig: the sun and the three strongest lamps. Three defects make it dark at
night:

- **Moon shadow on lamp light.** The pixel shader multiplies the whole light, lamps included, by the sun or moon shadow.
  Wherever the moon shadow falls (the side of a hedge or a planter wall), lamp light disappears.
- **Black back sides.** The vertex shader clamps each lamp's `N.L` at 0, so the half of a bush facing away from a lamp gets
  nothing.
- **Missing lot lamps.** The rig gather barely reached lot lamp classes, so plants on lots had almost no lamp colour.

See [engine/light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md).

## How Apex Radiance solves it

Three shader fixes, plus the CPU rig boost described in [objects-and-rigs.md](objects-and-rigs.md):

1. **Moon shadow on summer instanced objects.** The game's 600-byte object-rig pixel shader is replaced by an HLSL copy
   that lifts the shadow towards 1 by the night level: `shadow = lerp(shadow, 1, night)`.
2. **Wrap lighting for lamps.** Foliage vertex shaders are patched so each lamp weight becomes `max(N.L/1.5 + 1/3, 0)`:
   light passing through leaves. The back of a bush gets a third of the facing side. The sun keeps its plain clamp.
3. **Moon shadow in other foliage pixel shaders** (winter bushes and other variants): the shadow fade is lifted towards
   1 by the night level with a two-instruction bytecode patch.

Fixes 1 and 3 act only at night (night level above 0.01). Fix 2 is applied whenever the option is on; its wrap
constants also carry the shared daytime curve (`LampDay(night)`: 3% by day, exactly the earlier constants at night), so the
per-instance lamp colours no longer light foliage at night strength by day.

Lamps behind buildings: the rig gather scales an outdoor lamp's per-instance colour by the share that reaches the
instance's centre past the outside walls (and the slabs above the lamp), with *Buildings block lamp light on trees and objects*; trees
behind a house no longer take a garden post's light through it ([objects-and-rigs.md](objects-and-rigs.md)).

Trees on the underground rig: a SpeedTree cluster whose level the game sets below 0 (on a lot with a basement, a tree
whose root stands under floor 0) takes the fixed `[UndergroundTree]` light rig of `CustomLightRigging.ini`, four white
lights at 0.5 that never gather, so the tree was drawn flat and bright white all night, and again whenever its level was
set. `TreeLightRig` hooks the cluster's level setter (vfunc `+0x50`, `0x0070D370`) and custom-rig setter (vfunc `+0x6C`,
`0x0070D800` on Steam 1.67.2): after each, a cluster left on that rig's key (`0x08D11E8C`, FNV-1 of "undergroundtree", at
`cluster+0x1F0`) with no script rig (`+0x53C`) gets key 0, the normal rig, applied by the cluster's own next update. It
installs with Night Lighting when both sites are known for the game build ([`features/tree_light_rig.cpp`](../../../features/tree_light_rig.cpp));
developer status: tree levels set and clusters given the normal rig.

### Lamp reach on foliage (2.13.1)

Study (F7/Full captures, 2026-10-13 `16-12-45 Full capture` draw #279, VS `A0E8EBA2` / PS `0A2D0BE4`):

- Foliage takes lamp light only per vertex from its rig: the directions are `c27..c29[i]` and the colours `c54..c56[i]`, one
  set per instance and computed at the rig's centre. Lamp light is summed into `oT2` and multiplied by `c143.x`. Foliage has no
  atlas term and no per-pixel lamps.
- The CPU rig boost (`BoostRec`) raised every outdoor lamp's record to `F0 x intensity x strength x (1 - h^2/R^2)^2`, with R
  half the light's bounds (about 35 m for a garden post). A bush 2 m from a post and a tree 9 m away got almost the same colour
  (x1.14). Together with the wrap (`N.L/1.5 + 1/3`), this lit canopies almost evenly.
- The outdoor objects' per-pixel machinery (lamp list per draw, `TEXCOORD8` world position, `ps_3_0` law) does not
  transfer directly. The foliage shaders are `vs_2_0`/`ps_2_0`, instanced through `a0` (up to 9 instances per draw). In
  `A0E8EBA2` the position sent to `oPos` is relative to the tree's origin (`c142`), not a world position, and the variants
  differ in shape (tree 2 strip, winter, m63). A per-vertex lamp list would need a world position recovered per variant
  and free constants. That was not done: no night foliage draw is in the current captures to verify it.

What changed, with *Doors and windows stay lit*, *Seamless light on pieces* (per-pixel lamps), *Realistic lamp reach* and
*Outdoor light between floors* on (`LotLightBridge::ObjectReachLawScale`):

1. **Rig records by the reach law** (`ObjectLightBridge::ReachLawRec`). Each outdoor lamp's record for a rig (street lamp,
   or lit and enabled room-0 lamp of type 3..6, `BoostRec`'s rule) is set to `ObjectLampColourAt` at the rig's centre: the
   outdoor walls' law (`LampReach::PixelLaw`), their scale, the daytime curve, the cones, at most 1 per channel. This
   replaces the footprint boost. *Outdoor lamps through openings* already used the same rule. `ShadeRec` (walls and slabs)
   still follows. Outdoor objects that draw per-pixel lamps zero their rig and are unaffected. The rigs gather again when
   the law is turned on or off or its scale changes by more than 5% (`TerrainLightingPolicy::RigLawChanged`; about 67
   gathers from day to night).
2. **Lambert weight** (`TerrainLightingPolicy::FoliageLampWeights`). The foliage draw sets VS `c255 = (1, 0)`, so each
   lamp's weight is `max(N.L, 0)`, as on every per-pixel lamp. Leaves turned away from a lamp take none of it, and the game's
   sky (moon) term stays as drawn. The colours already carry the daytime curve, so no second factor is applied.

With any of these options off, the earlier boost and wrap stay. Status: Objects *outdoor lamps on rigs by the reach law: N*;
foliage *Lambert on reach-law rigs: N*. Offline: `FoliageReachChecks` in `tools/terrain_lighting_test`.

At night on reach-law rigs, the per-vertex lamps below replace the rig-centre evaluation; a slot whose lamp cannot be
confirmed keeps it.

### Per-vertex lamps and crown shade

With the rig law above on, at night, each foliage draw uses a further copy of its vertex shader
(`FoliageVertexLamps::Patch`) and per-instance constants for its three lamps:

1. **Lamps per vertex.** The real direction and distance from each vertex to the lamp's head, Lambert with the wrap
   constants, and the lamp's law at the vertex, with the instance's own lamps (below). No channel passes 1, and the hue
   is kept. The law is the plain inverse square, Gain / d^2 (the base of the game's ground law), faded smoothly near the
   lamp's reach R (`FoliageVertexLamps::FoliageLaw`: the walls' law with r0 = R). The outdoor walls' law keeps r0^2 / d^2,
   which falls as 1 / d^4 past r0: with it a street lamp lit the pavement under it and left the crowns 5 m away dark (F8
   2026-10-20 21-27-17; offline: 0.094 against 0.425 now, 0.042 at 9 m, 0 past R).
2. **Leaf transmission.** Light from behind a leaf passes through it as tau / rho x max(-N.l, 0), with tau / rho = 0.83
   for a green leaf at 550 nm, tinted by the leaf's colour in the pixel shader.
3. **The crown's own leaves.** Beer-Lambert along the vertex-to-lamp segment inside the crown's ellipsoid
   (`foliage_canopy.h`: k = G x clumping x sqrt(1 - leaf albedo), about 0.37; leaf area density 1 for trees, 2.5 for
   bushes).
4. **Ground bounce** from the strongest lamp: the ground's irradiance x rho_g / 2 x (1 - n.y), with grass reflectance 0.15.

**Each instance picks its own lamps** (`PickFoliageLamps`), by the rule of the per-pixel lamps of outdoor objects
instead of the rig's three slots: the lamps within reach of the crown's middle (`SelectLamps`, with the indoor lamps
shining out of a house), each with its cones (an area light's facing counts as its cone,
[objects-and-rigs.md](objects-and-rigs.md#per-pixel-patch)) and its share past the outside walls and slabs at that
middle (`PickedLampVisibility`), ranked by the light the reach law gives at the mesh box's point nearest the lamp (trunk
included). The three brightest are kept, brightest first (slot 0 takes the ground bounce); a lamp whose reach ends before
the box is left out. The draw replaces the per-instance colours `c54..c80` with their colours (F0 x intensity x fade x
cone x walls' share) and restores them afterwards; each vertex then takes colour x min(law x gain x scale, 1 / the
colour's brightest channel), so no channel passes 1 and the hue is kept (a warm lamp no longer turns white near a leaf).
The trees read their own lamp list (`ReadFoliageLamps`) at every lamp refresh, also while the per-pixel lamps keep theirs
for a lamp switch or a drag (`AtriumHold`, `DragLite`: 5.5 to 9 s in game, 20/10 21:18, waiting for the rooms of a house,
which trees outside never need), with the indoor lamps shining out read from their own list while those are held; the list
notes where it changed (each lamp's place before and after), by everything but the colour. An instance picks at once when
a changed lamp is within 20 m of its box, else it only takes the new generation. A lamp whose colour alone changed (a
fade at dusk, a recolour) keeps its place in each pick: the instance takes its new colour, times the cone and walls'
share of its pick, at its next draw; a lamp going to black or coming out of it counts as a change. Instances also pick
again every 1.5 s when what a pick reads changed since (the walls and floors, which arrive after a lot's first frames;
the lamps' colours, which set the ranking; or with indoor lamps shining out), at most 96 per 8 ms, keeping their older
pick meanwhile; an instance's first pick is never put off, so no draw falls back to the old light. When the instance map
passes 16,384 (grass and flowers are instances too), the ones not drawn for 3 s go, then the ones not drawn for 250 ms;
the map is cleared only past 32,768. A world change clears the mesh boxes (kept by vertex buffer address, so a
buffer of the new world made at an old one's address would take that mesh's box and instance count) and every
instance's pick of the old world's lamps. The log counts the picks made again that changed the light and the ones that
kept it.

**The camera the game drew with** (`FoliageVertexLamps::EyeAnchors`). The instance positions are relative to the eye the
game built the frame with, while the eye Apex reads from the game's camera can differ from it while the camera moves (in
game: lamp light on trees moved with the camera, and crowns blinked as their instances got new keys and fell back to the
old light). While the read eye has not changed for half a second, the instances of each draw are learned as fixed places
(2 m cells, by mesh and scale); while it moves, the offset that puts a draw's instances back on their places (median per
axis, within 0.3 m of each other, at most 2.5 m) is added to the eye, found again every frame (the game's eye keeps
moving for a moment after the read one stops; an offset kept per eye read shifted the light for half a second after
the camera stopped). The camera counts as still after 0.3 s. The log reports the largest
correction (`camera drift corrected: up to N m`).

Before 20/10 each rig slot had to be matched to an outdoor lamp by direction and confirmed by colour, and an unmatched
slot lit the whole crown with the rig's colour. In game under 2% of the lit slots matched (public 2.14 log: 9,058 by the
law against 607,905 without a lamp): a red garden lantern of 6.3 m reach lit a whole bare crown red 15 m away, and a red
cube lamp (range 70: R 13.6 m) lit a leafy crown in patches to its top. The rig's lamp directions now only find whether
the instance positions are camera-relative or in world space (once per session, by matching instances to outdoor lamps
both ways); crowns and per-vertex lamps wait until it is known. The trunk and branch variant is patched as well. A
vertex shader the patch refuses is named in the log with the reason, and its code is kept once in
`Apex Radiance\Censo\foliage_VS_<FNV>.bin` for the offline check.

### The sun's shadow on trees

Study: F7 `2026-10-20 16-42-39` draw #678 and every pixel shader of `Shaders_Win32.precomp`. The game has 16 tree pixel
shaders (`ps_2_0`, sun = `sat(t1.z) x c0`, three lamps, ambient cube). Eight read the sun shadow map (four `texldp s5`
taps at `t5`, faded by `t6.x`); the other eight never do, although the foliage vertex shader writes the shadow position in
`oT5` (the shadow matrix `c105..c108`, the same as the draws that sample `s5` in that frame) and the edge fade in `oT6.x`.
Trees drawn with those took sunlight with no shadow: no tree shaded another, no house shaded a tree, and no crown shaded
itself. Both kinds are drawn in the same session (the log patches the moon shadow of some foliage shaders and not others).

`FoliageVertexLamps::PatchTreeSunShadow` patches the eight without a shadow (pattern: `mov_sat rW.xy, t1.wzyx`, then
`mad rS.xyz, rW.y, c0, rX`): before the sun term it reads the shadow map at four taps half a texel around `t5` (as the
game's shadowed variants do; each tap its own constant, since `ps_2_0` has no free swizzle), fades it to 1 by `t6.x` and
keeps a floor of 0.25 for the light that passes a crown, then multiplies only the sun's weight: sun x (0.25 + 0.75 x
shadow). Lamps and the ambient are untouched. `DrawTreeSunShadow` binds the frame's shadow map to `s5` (the depth
texture on `s5` of the bushes, roofs, lot pass and water draws, used only in the frame it was seen in or the next, and
forgotten before a Reset), sets the five constants and restores everything. A draw whose vertex shader writes no `oT5` /
`oT6.x` (the per-vertex lamp family `VS_9A10A3A8`) keeps the game's pixel shader. Development switch `foliage_sun_shadow`
(developer only: off in released builds until trees cast into the shadow map). Offline: `tools/foliage_vertex_test` with `-Precomp`: the eight patched shaders load in D3D9; the eight
shadowed ones are left alone. Not confirmed in game: the depth bias (1/2048 of the map's depth) against leaves shading
themselves, and the cost. The first public build drew no tree with it (log `tree draws with the sun shadow: 0`, at
night); the log now gives every 30 s the draws with it and, for the others, why (no shadow map seen, none this frame, a
vertex shader without `oT5` / `oT6.x`, a pixel shader not patched), and names once each vertex shader that draws into the
shadow map (sampled one frame in 120), with whether it is an instanced caster, to see whether trees cast at all. A
daytime F8 (`2026-10-20 21-18-03`) shows a house's shadow on the lawn and none under the tree beside it: trees likely do
not draw into the shadow map, so no tree can shade another through it; receiving it gives trees the shadows of houses
and other casters only. UNVERIFIED until the shadow pass log above.

**Crown shade on lot ground.** Tree and bush crowns seen in those draws are recorded as ellipsoids (merged by place, kept
while near the camera, starting above a tree's clear trunk). With *Walls shade lamp light near the ground*, the lot
ground's lamp shade mask treats them as porous occluders: a ray through a crown keeps exp(-k u s) of its light, so the
shadow is soft and partial. The mask is built off the render thread ([lot-light-pass.md](lot-light-pass.md)).

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Lamps light objects | `postesNosObjetos` | bool | on | | Turns on the rig boost, the moon-shadow replacement, the wrap-light vertex shaders and the winter leaf-shadow patch. Live |
| Brightness (Objects card) | `forcaNosObjetos` | float | 75% | 25 to 300% | Scales the rig boost that fills the per-instance lamp colours. On the reach law (below) the rig's outdoor lamps take no boost: their colours follow the walls' brightness x *Seamless light brightness* |
| No setting | | | | | Per-vertex lamps: at night, on rigs on the reach law (`LotLightBridge::ObjectReachLawScale` >= 0), which needs *Lamps light objects*, *Doors and windows stay lit*, *Seamless light on pieces*, *Realistic lamp reach* and *Outdoor light between floors* (the reach law runs in `LevelLightShare`). Crown shade: the crowns are noted only in those draws, so it needs the same reach law, and also *Walls shade lamp light near the ground* |

There is no foliage-specific strength. The Lighting balance styles set `forcaNosObjetos` to 67.5% (Subtle), 75% (Soft,
the default) or 100% (Natural). The night level is the game's `lightMgr+0xF0`, passed by `LotLightBridge::SetNightLevel`.

## Compatibility and interactions

- Fences of the `vs_2_0` instanced family use the same shaders as bushes and get all three fixes
  ([fences.md](fences.md) covers the `vs_3_0` fence family).
- Winter foliage vertex and pixel shaders are covered by fixes 2 and 3; ground snow is in [snow.md](snow.md).
- Lamp colour: per-instance colours read the tinted lamp colour ([lamp-colour.md](lamp-colour.md)).
- The rig boost and cap change of `ObjectLightBridge` fill the per-instance lamp colours `c54..c56[i]`
  ([objects-and-rigs.md](objects-and-rigs.md)).
- No game code is patched for the shader fixes; they are D3D9-level shader swaps by class.

## Limitations

- Foliage receives no ground light: summer plants and flowers stay darker than the atlas-lit ground next to them.
- Per-vertex lamps and crown shade are not confirmed in game. A crown with a lamp at its foot may look darker than
  with the rig-centre light, because the crown's own leaves now attenuate the lamp.
- Foliage variants outside the captured family keep the rig-centre light.
- The summer 600-byte object-rig shader is replaced only for the moon shadow: no ground light and no per-pixel lamps.
- A plant whose pixel shader multiplies all lamp light by a texture (`texld s2` on TEXCOORD3, a 128x128 DXT1 texture,
  capture m65) stays dark even with the patched shaders.
- Foliage with a 4-light matrix (`VS_4375A3EE` / `EAB58655`, `PS_936D7C02`; `max r0, r0, c31.w`, directions `c8..c10`,
  colours `c11..c13`) is not recognised (no `c27[a0]` read); which of its lights is the sun is unknown.
- Large SpeedTree trees use another path (`TreeLightColors` / `TreeLightDirections`); the captured trees did not use it.
- The wrap-light patch inserts `def c255` unconditionally. Whether any relative-addressed per-instance array can reach
  `c255` is not checked in code (`vs_2_0` guarantees 256 constants).

## Technical reference

Foliage lamp colour uses the lower of the smooth daylight transition and the native lamp night factor. Its saturation
cap follows that same phase, so tree crowns do not receive full night lamp brightness ahead of the native lights.
This changes the lamp term only; sunlight and the existing foliage shadow paths remain.


### Captured shaders

| Capture | VS | PS | Notes |
|---|---|---|---|
| Bushes and fences, summer (`LightProbe-arbusto` #103, `-cerca` #106) | `VS_2BC7F188` (`vs_2_0`, instanced, `a0.z`) | `PS_2BC82F40` (`ps_2_0`) = `kObjectRigPs` {600 bytes, FNV `0x0A2D0BE4`} | Lamp directions `c27/c28/c29[i]`, colours `c54/c55/c56[i]`, sun `c137/c138` |
| Planter bushes (`-conjunto`, `-arbusto2`) | `VS_2BEA1650` | Same 600-byte PS | Lamp colours 0.8 to 0.99 per instance after the rig boost |
| Tree (`-arvore` #252) | `VS_2BC70BD8` | `PS_2BC72CA8` (`ps_2_0`, no shadow) | Sun `c124/c125` |
| Tree 2 (`-arvore2` #107) | `VS_2BC6D848` (triangle strip) | `PS_2BC6CD58` | Lamp 1 in `oD0`, lamps 2 and 3 in `oT4`; `max` with `def c117 = 0` |
| Winter bush (m21 `-arbusto-neve`, m22) | `VS_2C55DC50` | `PS_2C565BA8` (`ps_2_0`, 1016 bytes) | Lamps in `oT2` = sun x `c147` + 3 lamps; PS `lrp_pp r3.w, t5.x, c8.y, r1.w` then `mad_pp r0.xyz, t2, r3.w, r0` |
| Winter tree (m28 `-arvore-neve`) | `VS_2A839190` (1448 bytes) | `PS_2A83D588` (`ps_2_0`, 672 bytes) | No shadow map: light = ambient cube(`t0`) x `c0.w` + `t2` |
| Winter bush (m63/m64) | `VS_32779B38` (`vs_2_0`) | `PS_3277A240` (`ps_2_0`, 872 bytes) | `max r0, r0, c131.x` with `def c131 = 0`; shadow `lrp r2.w, t6.x, c6.y, r1.w` |
| Summer bush (m78) | Foliage VS (patched) | `PS_32DDB220` (`ps_2_0`) | Lamps `t1.w x v0 + t4` x shadow `lrp r1.w, t6.x, c3.z, r2.w` (`def c3 = 0.5, 0.25, 1, 0`) |

In the vertex shader `r0.x` is the sun's `N.L` and `r0.yzw` the three lamps' `N.L`; `max r0, r0, cK.w` clamps them at 0,
and each lamp weight then scales its per-instance colour `cN[a0.c]`. In the pixel shader lamp light and sun share the
moon-shadow multiply.

### Fix 1: moon shadow on summer instanced objects (`DrawObjectRig`)

The 600-byte PS (`PsClass::ObjectRig`, exact identity `kObjectRigPs`) computes `light = (t2 x shadow + sky x c1.w) x t1.z`.
It is replaced by `kObjectRigHlsl` (in `lot_light_bridge.cpp`, `ps_2_0`, compiled at start-up by `framework/shader_cache`
and created at the first draw), identical except `shadow = lerp(shadow, 1, c3.x)`. `DrawObjectRig` sets PS
`c3 = (night, 0, 0, 0)` for the draw and restores it. It is checked first in `OnDrawInner`, before any vertex shader class.

### Fix 2: wrap lighting (`ShaderPatches::PatchFoliageVs`, VS class 6)

Recognition: any vertex shader (`vs_2_0` or `vs_3_0`) that reads the per-instance lamp array `c27[a0.x]` (relative
addressing on `c27`), with a `max r0, r0, cK.s` (full mask, no relative addressing) where `cK.w` is a runtime constant or
`cK.s` is a replicated component of a shader-defined constant equal to 0 (winter bush m63 `c131.x`, tree 2 `c117`).
Patch:

```
def c255, 1/1.5, 0.5/1.5, 0, 0          ; classification copy only; the drawn copy leaves c255 to the draw
max r0.x, r0, cK.s                      ; the sun keeps the plain clamp (original instruction, mask .x)
mad r0.yzw, r0, c255.x, c255.y          ; lamps: N.L / 1.5 + 1/3
max r0.yzw, r0, cK.s                    ; ... clamped at 0
```

The drawn copy (`PatchFoliageVs(t, true)`) has no `def`: the foliage draw sets VS `c255 = (s/1.5, s 0.5/1.5, 0, 0)` with
`s = LampDay(night)` and restores the game's `c255` afterwards (status *foliage (wrap light): N (by the daytime curve: N)*).

`ClassifyVs` tests the foliage pattern after the road, instanced-structure, snow and floor classes; the road vertex shader
is refused. The patched bytecode is stored in the shader's `VsInfo`, and `FoliageVsFor` creates the D3D9 shader at the
first draw. In `OnDrawTracked`, when the VS is class 6 and the option is on, the patched copy is bound around the whole
draw handling; then the pixel side runs (`OnDrawInner`: the object-rig replacement or the leaf-shadow patch). If no pixel
branch draws, the game's pixel shader draws with the patched vertex shader. The game's vertex shader is restored
afterwards.

### Fix 3: moon shadow in other foliage pixel shaders (`ShaderPatches::PatchLeafShadow`, `DrawLeafShadow`)

For any pixel shader drawn with a class 6 vertex shader that is not the 600-byte one. Recognition:
`lrp rD.w, tN.x, cK.s, rS.w` (the shadow fade; `t5.x` in the summer bushes, `t6.x` in m63/m78), with `cK.s` the runtime
`cK.y` or a replicated component of a `def` equal to exactly 1 (m78 `c3.z`). Right after it (two instructions, because
`ps_2_0` allows one constant register per instruction):

```
add rT.w, cK.s, -rD.w
mad rD.w, rT.w, cN.x, rD.w              ; rD.w = lerp(rD.w, 1, night)
```

`cN = maxConst + 1` (refused if it would be 32 or more, or the temp 12 or more: `ps_2_0` limits). `DrawLeafShadow` sets
`cN = (night, 0, 0, 0)` for the draw.

### Patterns

| Pattern | Used by |
|---|---|
| Relative read of `c27` (`c27[a0.x]`) | Foliage VS detection |
| `max r0, r0, cK.w` (runtime) or `max r0, r0, cK.s` with `def cK.s = 0` | Wrap-light site |
| `lrp rD.w, tN.x, cK.s, rS.w` with runtime `cK.y` or `def cK.s = 1` | Leaf-shadow site |
| PS size 600 + FNV-1a `0x0A2D0BE4` | Summer object-rig PS |

### Light capture of a foliage draw

A light capture ([Light Probe](../dev-tools/light-probe.md)) describes a foliage draw in its `mod:` line, filled only
while a capture runs (`LightProbe::Capturing`; `FoliageVertexConstants` writes the instance part, the foliage branch of
`OnDrawTracked` the rest):

```
mod draw: foliage | per-vertex lamps: drawn | wrap weights A B (reach law) | instance positions relative to the camera | camera eye (x y z) offset (dx dy dz)
      instance i: box centre (x y z) half a x b, crown (x y z), picked T s ago (lamp list G / now G', walls same), N own lamp(s): [k] at (x y z) colour (r g b) share s key K ...
```

- *per-vertex lamps*: `drawn`, or `not used for this draw (the old path)` (no instance lines then).
- *wrap weights* and their source: `reach law` or `daytime curve`.
- *instance positions*: `relative to the camera`, `world` or `not known yet`.
- *camera eye*: the eye read from the game's camera (`not read` when unavailable) and the offset added to it
  (`EyeAnchors`, above).
- Per instance (up to 9 a draw): the mesh box's centre and half extents, the crown's centre (`none` without a crown
  shape), how long ago it picked its lamps, the lamp list generation of its pick and now, whether the walls and floors
  it read are the same or changed since, and each own lamp's place, colour, share (cones and walls) and key.

### Files

| File | Symbols | Role |
|---|---|---|
| `features/lot_light_bridge.cpp` | `kObjectRigHlsl`, `EnsureObjectReplacement`, `DrawObjectRig` | Fix 1 |
| | `ClassifyVs` (class 6), `FoliageVsFor`, `OnDrawTracked` | Fix 2 dispatch |
| | `DrawLeafShadow`, `g_leafPs` | Fix 3 |
| | `ObjectStatus` | Status line |
| `features/shader_patches.cpp/.h` | `PatchFoliageVs(t)`, `PatchLeafShadow(t, nightConst)` | Bytecode patches |
| `shaders/shader_ids.h` | `kObjectRigPs` {600, `0x0A2D0BE4`} | Exact identity of the summer PS |
| `features/tree_light_rig.cpp/.h` | `Hook_SetLevel`, `Hook_SetCustomRig`, `Restore` | Trees off the underground rig |

## Rejected approaches

- Blaming the room-mode rig gather for the dark planters: measurements showed lamps arriving; the moon shadow was the
  cause. Details in [history](../../history/night-lighting-foliage.md).
- `lrp` with two constant registers in `PatchLeafShadow`: invalid on native D3D9.
- Accepting only a runtime `cK.w` clamp and `t5.x` / runtime `cK.y` shadow sites: missed the winter and m78 variants.
- Sending a whole draw back to the old shader when one lamp could not be matched: almost no draw kept per-vertex light.
- Confirming a lamp by colour over the crown only: a lamp at the foot of a tree was refused and the tree stayed neutral.
- Refusing a lamp found by direction when its colour differs from the rig's (no light at the mesh box, or more than 4x
  apart): the slot fell back to one colour for the whole crown, taken at the rig box's point nearest the lamp, and 99.7%
  of instances drew that way.
- Following the rig's three lamp slots at all (matching each to a lamp by direction, keeping the slot's direction when
  it did not match): even with the colour test removed, under 2% of the lit slots found their lamp in game.

## See also

- [Validation](../../validation/night-lighting-foliage.md)
- [History](../../history/night-lighting-foliage.md)
- [Objects and rigs](objects-and-rigs.md), [Fences](fences.md)
- [Engine: light objects and rigs](../../engine/light-objects-and-rigs.md), [Engine: shaders](../../engine/shaders.md)
