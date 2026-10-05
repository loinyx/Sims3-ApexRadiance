# Foliage: bushes, trees and plants

At night, bushes, hedges, trees and small plants near lamps are lit on every side the lamp can reach. The side of a
planter in moon shadow keeps its lamp light, and the back of a bush facing away from a lamp is dim rather than black. The
same fixes apply to the snowy winter variants and to the fences that share the bush shader. Part of
[Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released in 1.0.0 |
| Default | On |
| Menu | Lighting > Objects > *Objects* card |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) (`DrawObjectRig`, `FoliageVsFor`, `DrawLeafShadow`), [`features/shader_patches.cpp`](../../../features/shader_patches.cpp) (`PatchFoliageVs`, `PatchLeafShadow`) |

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

Fixes 1 and 3 act only at night (night level above 0.01). Fix 2 is applied whenever the option is on.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Lamps light objects | `postesNosObjetos` | bool | on | | Turns on the rig boost, the moon-shadow replacement, the wrap-light vertex shaders and the winter leaf-shadow patch. Live |
| Brightness (Objects card) | `forcaNosObjetos` | float | 100% | 25 to 300% | Scales the rig boost that fills the per-instance lamp colours |

There is no foliage-specific strength. The Lighting balance styles set `forcaNosObjetos` to 67.5% (Subtle), 75% (Soft) or
100% (Natural). The night level is the game's `lightMgr+0xF0`, passed by `LotLightBridge::SetNightLevel`.

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
- The summer 600-byte object-rig shader is replaced only for the moon shadow: no ground light and no per-pixel lamps.
- A plant whose pixel shader multiplies all lamp light by a texture (`texld s2` on TEXCOORD3, a 128x128 DXT1 texture,
  capture m65) stays dark even with the patched shaders.
- Foliage with a 4-light matrix (`VS_4375A3EE` / `EAB58655`, `PS_936D7C02`; `max r0, r0, c31.w`, directions `c8..c10`,
  colours `c11..c13`) is not recognised (no `c27[a0]` read); which of its lights is the sun is unknown.
- Large SpeedTree trees use another path (`TreeLightColors` / `TreeLightDirections`); the captured trees did not use it.
- The wrap-light patch inserts `def c255` unconditionally. Whether any relative-addressed per-instance array can reach
  `c255` is not checked in code (`vs_2_0` guarantees 256 constants).

## Technical reference

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
def c255, 1/1.5, 0.5/1.5, 0, 0          ; inserted at the top
max r0.x, r0, cK.s                      ; the sun keeps the plain clamp (original instruction, mask .x)
mad r0.yzw, r0, c255.x, c255.y          ; lamps: N.L / 1.5 + 1/3
max r0.yzw, r0, cK.s                    ; ... clamped at 0
```

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

### Files

| File | Symbols | Role |
|---|---|---|
| `features/lot_light_bridge.cpp` | `kObjectRigHlsl`, `EnsureObjectReplacement`, `DrawObjectRig` | Fix 1 |
| | `ClassifyVs` (class 6), `FoliageVsFor`, `OnDrawTracked` | Fix 2 dispatch |
| | `DrawLeafShadow`, `g_leafPs` | Fix 3 |
| | `ObjectStatus` | Status line |
| `features/shader_patches.cpp/.h` | `PatchFoliageVs(t)`, `PatchLeafShadow(t, nightConst)` | Bytecode patches |
| `shaders/shader_ids.h` | `kObjectRigPs` {600, `0x0A2D0BE4`} | Exact identity of the summer PS |

## Rejected approaches

- Blaming the room-mode rig gather for the dark planters: measurements showed lamps arriving; the moon shadow was the
  cause. Details in [history](../../history/night-lighting-foliage.md).
- `lrp` with two constant registers in `PatchLeafShadow`: invalid on native D3D9.
- Accepting only a runtime `cK.w` clamp and `t5.x` / runtime `cK.y` shadow sites: missed the winter and m78 variants.

## See also

- [Validation](../../validation/night-lighting-foliage.md)
- [History](../../history/night-lighting-foliage.md)
- [Objects and rigs](objects-and-rigs.md), [Fences](fences.md)
- [Engine: light objects and rigs](../../engine/light-objects-and-rigs.md), [Engine: shaders](../../engine/shaders.md)
