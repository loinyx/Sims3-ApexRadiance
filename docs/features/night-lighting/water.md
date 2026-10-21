# Water: ponds and lakes

At night, ponds and lakes near lamps show warm glints and a soft glow from each lamp. With Depth Blur on, the water also
mirrors the shore (trees, houses and lit lamps) instead of only a fixed sky. The ocean and swimming pools keep the game's
look. The water of Twinbrook, Bridgeport and Moonlight Falls, drawn with a water shader that never shows the game's
reflection, gets the same pass. Part of [Night Lighting](README.md); the reflection card is described from the
player's side in [reflections.md](../reflections.md).

## Status

| | |
|---|---|
| Availability | Released in 1.0.0. Second lake shader (cloudy and rainy weather): 2.5.2. Highlight filter and colour preservation: 2.5.5. The water shader without the game's reflection (Twinbrook, Bridgeport, Moonlight Falls): 2.7.0 |
| Default | On |
| Menu | World > Water & Snow > *Lamp Glow* and *Water Reflections* cards; Developer > Lighting > *Compare lighting paths* > Water highlights |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) (`DrawLake`), [`shaders/water_lamps_ps.hlsl`](../../../shaders/water_lamps_ps.hlsl) |

## The problem

The game's pond and lake water gets no lamp light and reflects only a fixed sky cube. Its brightness comes from the sun's
specular term and shadow alone, so at night ponds are dark and dead even beside a row of street lamps. The ocean renders a
real-time planar reflection, but lakes do not, and neither does a third water shader used by Twinbrook's sea,
Bridgeport's lake and Moonlight Falls' water. See [engine/shaders.md](../../engine/shaders.md).

## How Apex Radiance solves it

Right after the game draws its lake water, Apex Radiance draws a second pass on the same geometry. The pass adds glints
and glow from up to 16 nearby lamps and, when the scene depth is available, a screen-space reflection of the shore. It
blends premultiplied over the game's water, so the game's own shader and states stay untouched.

1. **Recognise the lake water** by the exact identity of its pixel shaders (sunny and other weather) and vertex shader,
   and the water without the game's reflection by its own shader pair.
2. **Draw the game's water** unchanged.
3. **Choose lamps** from the shared lamp list ([roofs.md](roofs.md)) around the water mesh's position.
4. **Build world-to-clip** from the water vertex shader's matrices, so rotated lots project correctly.
5. **Read the scene depth** from Depth Blur's INTZ copy when it is bound, and march the reflected ray against it.
6. **Draw the pass** with premultiplied blending: `reflection x alpha + lamps`.
7. **Hide lamps behind things** (2.15.1, with scene depth): where the reflected ray meets the scene nearer than a lamp
   (a tent or a wall between the water and the lamp), that lamp's reflection is taken out; the glow on the water around
   the lamp stays.

The lamp highlight filter widens highlights that are thinner than a pixel, so lamp sparkles do not shimmer on moving
waves. The colour preservation scales very bright highlights as a whole instead of clipping each channel, so lamp colours
keep their hue. Both are spatial: there is no frame history, jitter or temporal smoothing.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Lamps glow on ponds (Lamp Glow card) | `lagosRefletemLampadas` | bool | on | | Lamp glints and glow. With it off the pass still runs for the shore reflection, with lamp strength 0 |
| Glow brightness | `brilhoNaAgua` | float | 40% | 10 to 40% | Lamp glint and glow strength (PS `c52.x`). Older saved values are clamped to the range at run time |
| Water Reflections (card switch) | `reflexoNoLago` > 0 | | on | | Off sets the strength to 0; on restores the last value (default 156%) |
| Reflection brightness (Water Reflections card) | `reflexoNoLago` | float | 156% | 0 to 300% (slider from 5%) | Shore reflection strength (PS `c58.x`). Needs Night Lighting and Depth Blur on |
| Developer > Water highlights > Stabilize lamp sparkles on water | `waterSpecularFilter` | bool | on | | Spatial widening of undersampled lamp highlights. Off = the original lamp lobe |
| Developer > Water highlights > Preserve bright lamp colors on water | `waterPreserveLampColors` | bool | on | | Shared RGB scale for highlights above 0.7. Off = the original per-channel clamp at 0.8 |

All values apply live every frame (`LotLightBridge::SetWaterFix`). The Lighting balance styles do not change water
settings. The two Developer switches are for A/B comparison; with both off the lamp math is the original one.

## Compatibility and interactions

- **Depth Blur** ([depth-blur.md](../depth-blur.md)) provides the INTZ scene depth. The shore reflection works only while
  Depth Blur's depth swap is active and the bound depth-stencil is Depth Blur's surface. `DepthShare::Request` lets any
  effect ask for the swap.
- **Game anti-aliasing:** with the game's own Edge Smoothing (MSAA) on, Depth Blur cannot share the depth, so only the
  lamp glints and glow remain. Apex Radiance's Edge Smoothing works with the reflection.
- **Post-scene chain:** the pass turns `ZENABLE` off; `DepthShare::SetInternalPass(true)` around the draw stops Depth Blur
  and the post-scene trigger from taking it for the first interface draw.
- **Roofs and objects** share the lamp list and the radius rule ([roofs.md](roofs.md)).
- The *Water Reflections* card is drawn in [`apex_gui.cpp`](../../../apex_gui.cpp); the card shows what it needs (Night
  Lighting, Depth Blur, the game's Edge Smoothing off) with buttons to turn them on.
- Sims3SettingsSetter's Mirror Reflection settings tune mirror objects, not water.
- No game code is patched.

## Limitations

- Only the lake shader pairs and the pair of the water without the game's reflection are handled. The reflecting ocean
  (whose planar reflection already shows lit lamps),
  swimming-pool water (`PS_3140B910`, `ps_2_0`, reflection and refraction only) and any other water shader, such as the
  plaza fountain, keep the game's look.
- Without scene depth (Depth Blur off, or the game's MSAA on) there are lamp glints and glow only, no shore reflection.
- The reflection is screen space: anything off screen or hidden is not reflected; the game's sky reflection remains
  there.
- Lamp glints use the visual-radius law, not the bake-matched law of objects.
- A lamp's reflection is hidden by what the scenery reflection's ray meets (one ray per pixel, along the flattened wave
  normal), not by a ray toward each lamp: without scene depth, or where that ray leaves the screen, lamps are never
  hidden.
- With the colour preservation on, very strong highlights look softer and slightly dimmer.

## Technical reference

### Recognition and order

- Exact identities (`shaders/shader_ids.h`): PS `kLakePs` {1344 bytes, FNV-1a `0x4F52846A`}, PS `kLakePs2` {1308,
  `0xB21E05D4`} (both `PsClass::Lake`) and VS `kLakeVs` {1088, `0x23CCB61B`} (VS class 2). `kLakePs2` is the same water
  with the sun shadow compared by hand (`texld` + `cmp` against `v5.z`, `dcl_texcoord6`) instead of the hardware
  `texldp`; the game uses it when the weather is not sunny. The shader package has only these two pixel shaders with the
  lake constants `c8 = (5, 1.25, 0.15, 0.1)`.
- Water without the game's reflection (Twinbrook's sea, Bridgeport's lake and Moonlight Falls' water): PS
  `kSeaNoReflPs` {1136, `0xC1F59F1B`} (`PsClass::Sea`; the scene copy in `s6`, the sky cube in `s3`, no planar
  reflection) with VS `kSeaNoReflVs` {1136, `0xA784C725`} (VS class 12). The reflecting ocean is another shader. The
  pass is the lake's, compiled a second time with the macro `SEA` (`kWaterSeaPsId`): its camera is in `c0`, its wave
  scales in `c4`, its world position in `TEXCOORD2` and its fog in `COLOR1.w`. `DrawLake(..., sea = true)` reads the
  world-view-projection from VS `c0..c3` and the world rows (an identity) from `c4..c6`, and picks lamps around the
  camera (VS `c7`), since the sea mesh is large and has no translation. Log: `Water: sea without the game's reflection
  seen`. In rain the same sea uses PS `kSeaNoReflRainPs` {1296, `0xAC8AF7EB`} (a ripple map in `s7`, noise in `s2`, every
  other register as above), handled the same way.
- Lake water in rain and snow (Seasons weather): PS `kLakeRainPs` {2892, `0x91A9AEF7`} and `kLakeSnowPs` {2756,
  `0x8C1D384B`} (`PsClass::LakeWeather`) with VS `kLakeWeatherVs` {1208, `0x3123FF89`} (VS class 13). Matrices and inputs
  are the lake's (world-view-projection VS `c4..c7`, world `c8..c10`); the pixel shader has the camera in `c3`, the wave
  scales in `c9`, the wave maps in `s2` / `s3` and the scene copy in `s8`. The pass is compiled a third time with the macro
  `WEATHER` (`kWaterWeatherPsId`). Before 2.7.2 these lakes kept the game's look (dark at night, F7 2026-10-06 20:20).
- If the lake pixel shader is drawn with another vertex shader, the pass is skipped (logged once); the sea pixel shader
  with another vertex shader is skipped too.
- The lake branch runs before the *Street lamps light lots* gate in `OnDrawInner`, so water works with that option off.
- The pass shader is compiled at start-up by `framework/shader_cache` and created at the first lake draw.

### `DrawLake`

1. Draw the game's water unchanged.
2. `SelectLamps(c8.w, c10.w, 150)`: up to 16 lamps with the lowest (horizontal distance - radius) within 150 m of the
   water mesh's translation.
3. Build PS `c20..c59`. The water VS's world-view-projection (VS `c4..c7`) becomes world-to-clip: `WVP x inverse(World)`
   (3x3 inverse of the world rows `c8..c10` plus translation), because the water of a rotated lot has a rotated world
   matrix (for example 0.5 / 0 / 0.866). `c57` stays 0 in that case; if the matrix is singular (|det| <= 1e-8), the raw
   WVP is used with `c57` = translation.
4. Depth: used only when Depth Blur's INTZ exists (`DepthShare::Texture()`) and the bound depth-stencil is Depth Blur's
   surface (`DepthShare::Surface()`). Then `c59 = (A, B, 1)` with device `z = A + B / w`,
   `A = dot(row2.xyz, row3.xyz) / dot(row3.xyz, row3.xyz)`, `B = row2.w - A x row3.w`. The pass unbinds the
   depth-stencil (`ExtraHooks::RawSetDepthStencilSurface(nullptr)`), sets `ZENABLE = FALSE`, binds the INTZ to `s7`
   (CLAMP, POINT, no mip, no sRGB) and does its own depth test in the shader.
5. States: blend ONE / INVSRCALPHA, BLENDOP ADD, separate alpha off, ZWRITE off, colour write RGB, alpha test off.
6. Draw with the pass shader, then restore constants, `s7` texture and sampler states, depth-stencil, render states and
   the game's pixel shader.

### Pass shader (`water_lamps_ps.hlsl`, `kWaterLampsHlsl`, `ps_3_0`)

Inputs: TEXCOORD0 (wave UV 0 in `.xy`, wave UV 1 in `.zw`), TEXCOORD1.xyz world position, TEXCOORD3.w fog. Game constants
reused: `c1` camera position, `c5` wave normal scales.

```
n = normalize(a.x c5.x + b.x c5.y, a.z + b.z, a.y c5.x + b.y c5.y)      ; a, b = wave maps s0, s1
; highlight filter (params.z), evaluated before any early return so derivatives are defined:
;   variance = min(0.25 (|ddx n|^2 + |ddy n|^2), 0.016)
;   exponent = 2 / (2/252 + variance) - 2    (250 with no variance, about 81.6 at the cap)
;   peakScale = (exponent + 2) / 252          (else exponent 250, peakScale 1)
if depth && clip.w > sceneW(uv) + 0.3 + 0.01 clip.w : return 0           ; own depth test
v = normalize(c1 - pos); fres = 0.25 + 0.75 (1 - sat(n.v))^5; fogKeep = 1 - sat(fog.w)
; reflection (depth only): nr = normalize(0.5 n.x, 1, 0.5 n.z), r = reflect(-v, nr)
;   march t = 0.4, then t = 1.15 t + 0.3, up to 48 steps; stop off screen or behind the camera (clip.w <= 0.05)
;   hit when 0 < clip.w - sceneW < 1.5 + 0.3 t; 5 bisection steps; colour = scene copy s6 at the hit
;   cover = edgeFade(uv) x match x sat(1.5 - t_hit/60); no hit keeps the game's sky reflection
;   block = edgeFade(uv) x match at the hit (0 with no hit or no depth)
alpha = sat(fres x c58.x) x cover x fogKeep
; 16 lamps: l = lampPos - pos, rr = R^2 + 1e-3
;   spec += colour x pow(sat(n.h), exponent) x 2 peakScale / (1 + d^2 / (16 rr))
;           x (1 - block x sat(d - t_hit - 0.5))     ; a lamp behind what the reflected ray met
;   glow += colour x sat(1 - d^2/rr)^2
lamps = (spec x fres + glow x 0.08) x c52.x x fogKeep
; colour preservation (params.w): peak = max(r, g, b); above 0.7 the whole RGB is scaled so that
;   peak' = 0.7 + 0.1 x excess / (0.1 + excess)   (approaches 0.8); else lamps = min(lamps, 0.8)
return (refl x alpha + lamps, alpha)
```

The filtered version costs about 800 instruction slots (700 before the lamp occlusion of 2.15.1). Wave samples run before the depth
rejection, so occluded pixels also sample the waves. No render pass, texture or ray step is added by the filter.

### Registers

| Register | Content |
|---|---|
| `c1`, `c5` | Game: camera, wave normal scales |
| `c20..c35` | Lamp position: head xyz, w = visual radius |
| `c36..c51` | Lamp colour: colour x intensity x fade |
| `c52` | x = lamp strength (`brilhoNaAgua`, 0 when the glow is off) x `LampDay(night)` (2.12.3: the shared daytime curve, 3% by day, exact at night; the shore reflection is not scaled), y = lamp count (unused by the shader), z = highlight filter on, w = colour preservation on |
| `c53..c56` | World-to-clip matrix |
| `c57` | World translation (0 when the matrix was inverted) |
| `c58` | x = reflection strength (`reflexoNoLago`) |
| `c59` | x = A, y = B, z = 1 when `s7` holds the scene depth |
| `s0`, `s1` | Game wave normal maps |
| `s6` | Game scene copy (refraction source, the image before the water) |
| `s7` | Depth Blur INTZ (only with depth) |

### Files

| File | Symbols | Role |
|---|---|---|
| `features/lot_light_bridge.cpp` | `DrawLake`, `SelectLamps`, `SetWaterFix`, `WaterStatus`, `CompilePs` | Pass |
| `shaders/water_lamps_ps.hlsl`, `shaders/water_lamps_hlsl.h` (`kWaterLampsHlsl`) | `main`, `Project`, `ClipToUv`, `SceneW`, `EdgeFade` | Shader (the header is what the build uses; same content) |
| `features/depth_share.h` | `DepthShare::Texture()`, `Surface()`, `SetInternalPass()` | Depth from Depth Blur |
| `framework/d3d9_extra_hooks.cpp` | `ExtraHooks::RawGetDepthStencilSurface`, `RawSetDepthStencilSurface` | Unhooked depth-stencil calls |
| `patches/night_terrain_relight_patch.cpp` | `DrawWaterCard`, settings, `ShoreReflection`, `SetShoreReflection` | Menu and TOML |
| `apex_gui.cpp` | `WaterReflectionsCard`, `SetShore` | Water Reflections card |
| `shaders/shader_ids.h` | `kLakePs`, `kLakePs2`, `kLakeVs` | Identities |

## Rejected approaches

- Light-bounds radius and unclamped glow: the whole lake turned pinkish white. Details in
  [history](../../history/night-lighting-water.md).
- Reusing the ocean's planar reflection: a fixed mirror at sea level, rendered only with the ocean visible.
- Sampling the reflected ray at three fixed distances: wrong positions.
- A 40 m guess when the ray finds nothing: black patches and white streaks without depth.
- Projecting `(world - translation)` with local-to-clip: wrong on rotated lots.

## See also

- [Validation](../../validation/night-lighting-water.md)
- [History](../../history/night-lighting-water.md)
- [Reflections](../reflections.md), [Depth Blur](../depth-blur.md), [Roofs](roofs.md)
- [Engine: shaders](../../engine/shaders.md)
