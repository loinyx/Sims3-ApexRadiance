# Water Reflections

Ponds and lakes in The Sims 3 reflect only a fixed sky picture, so at night they stay dark and never show the scenery
around them. With Water Reflections on, ponds mirror the trees, houses and lamps along their shore on top of the game's
sky reflection. The companion *Lamp Glow* card makes ponds glow and sparkle near lamps at night. Both are drawn by Night
Lighting's water pass; this page covers what the player controls. The pass itself is documented in
[night-lighting/water.md](night-lighting/water.md).

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier (present in the first version in this repository) |
| Default | On (shore reflection 100%, lamp glow on) for new configurations |
| Menu | World > Water & Snow (cards *Lamp Glow* and *Water Reflections*); Overview > Lighting > Water Reflections |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` (keys owned by Night Lighting) |
| Source | [`apex_gui.cpp`](../../apex_gui.cpp) (`WaterReflectionsCard`, `SetShore`), [`patches/night_terrain_relight_patch.cpp`](../../patches/night_terrain_relight_patch.cpp) (`DrawWaterCard`, settings), [`features/lot_light_bridge.cpp`](../../features/lot_light_bridge.cpp) (`DrawLake`, `SetWaterFix`) |

## The problem

The game's pond and lake water shader samples two fixed sky cube maps for its reflection and receives no lamp light.
At night ponds are flat and dark. The ocean has a real planar reflection (a 1024x1024 render target rendered each frame),
but lakes do not, and the ocean's reflection is only rendered when the ocean is visible and mirrors at sea level, so it
cannot be reused for ponds.

## How Apex Radiance solves it

Night Lighting recognises the lake shaders by their exact bytecode and, after the game draws a pond, draws a second
additive pass on the same geometry. That pass adds lamp glints and glow and, when the scene depth is readable, a
screen-space reflection of the shore.

1. **Recognise the water.** The lake pixel and vertex shaders are matched by exact ID (`kLakePs`, `kLakePs2`,
   `kLakeVs` in `shader_ids.h`).
2. **Draw the game's water unchanged**, then the extra pass with `water_lamps_ps.hlsl`, premultiplied
   (`ONE / INVSRCALPHA`).
3. **Lamp glow:** up to 16 lamps within 150 m add glints and glow, scaled by *Glow brightness*.
4. **Shore reflection:** a screen-space ray march against the scene depth; the hit colour comes from the scene copy the
   game already binds for refraction. It is weighted by Fresnel, *Reflection brightness*, hit confidence and fog.
5. Without readable depth the pass adds the lamp glow only, and the game's sky reflection stays.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Water Reflections (card switch) | `reflexoNoLago` | float | on (100%) | | Off stores 0; on restores the last brightness (100% when there is none). Not a separate key |
| Reflection brightness | `reflexoNoLago` | float | 100% | 5 to 300% (stored 0 to 3) | Strength of the shore reflection (PS `c58.x`) |
| Lamp Glow > Lamps glow on ponds | `lagosRefletemLampadas` | bool | on | | Lamp glints and glow on ponds |
| Lamp Glow > Glow brightness | `brilhoNaAgua` | float | 40% | 10 to 40% | Lamp glow strength (PS `c52.x`); 0 is sent while the glow is off |
| Developer > Water highlights > Stabilize lamp sparkles on water | `waterSpecularFilter` | bool | on | | Filters tiny highlights (no temporal smoothing) |
| Developer > Water highlights > Preserve bright lamp colors on water | `waterPreserveLampColors` | bool | on | | Softens excessive lamp brightness while keeping its colour |

Values apply live every frame through Night Lighting's Present hook; nothing is reinstalled. The keys are registered by
Night Lighting and must not be renamed, because saved configurations and profiles use them. The *All effects* switch on
the Overview turns the shore reflection off first and back on last, after Night Lights and Depth Blur.

## Compatibility and interactions

- **Night Lighting** must be on: the pass lives in Night Lighting's draw hooks. Without it, both cards show *Turn on
  Night Lights*. Water does not require *Street lamps light inside lots*; the lake branch runs before that gate.
- **Depth Blur** provides the readable scene depth (the INTZ depth share). The card shows *Needs Depth Blur* with a button
  that turns it on. The depth also exists while Edge Smoothing's *Edges from depth* or Ambient Occlusion holds a depth
  request, so the reflection can appear with Depth Blur off in that case. See [depth-blur.md](depth-blur.md).
- **The game's Edge Smoothing (MSAA)** must be off. A multisampled back buffer has no shareable depth. While it is on
  and Water Reflections is enabled, the Overview row shows *Waiting for game settings* and an amber card *Turn off the
  game's Edge Smoothing* appears on the Overview, the Conflicts page and the Water & Snow page; see
  [edge-smoothing.md](edge-smoothing.md#compatibility-and-interactions). Lamp glow alone does not trigger the notice.
- **Depth Blur trigger:** the pond pass marks itself as an internal pass (`DepthShare::SetInternalPass`), so its
  depth-off draw does not fire the post-scene effects mid-frame.
- **Compare with the game** turns Night Lighting off, which removes both effects until the shortcut is pressed again.
- **Sims3SettingsSetter "Mirror Reflection Settings"** is a different system: it changes the fade distances of mirror
  objects (`sfMirrorBaseDistance`, `sfMirrorDistanceScale`). It shares no code or addresses with Water Reflections and
  does not affect ponds.

## Limitations

- Only ponds and lakes drawn with the recognised lake shaders. The ocean and swimming pools are not affected.
- Screen space: scenery that is off screen or hidden behind other objects is not reflected.
- No shore reflection while the game's Edge Smoothing is on or no effect holds the depth share.

## Technical reference

**Live apply** (Night Lighting Present hook):

```
shoreOnly = !lagosRefletemLampadas && reflexoNoLago > 0 && DepthShare::Texture() != null
LotLightBridge::SetWaterFix(lagosRefletemLampadas || shoreOnly,
                            lagosRefletemLampadas ? clamp(brilhoNaAgua, 0.1, 0.4) : 0,
                            reflexoNoLago, waterSpecularFilter, waterPreserveLampColors)
```

With the glow off the pass still runs when there is scene depth, with a lamp strength of 0, so it adds the reflection
alone.

**Depth check.** The pass uses `DepthShare::Texture()` only when `DepthShare::Surface()` is the depth-stencil bound at
that moment (read with `ExtraHooks::RawGetDepthStencilSurface`), so it never ray-marches against a reflection or UI
depth.

**Files and symbols**

| File | Symbol | Role |
|---|---|---|
| `apex_gui.cpp` | `WaterReflectionsCard`, `ShoreOn`, `SetShore`, `g_lastShore`, `kShoreDefault`, `kShoreDescription` | Card, Overview row, switch semantics |
| `patches/night_terrain_relight_patch.cpp` | `DrawWaterCard`, `ShoreReflection`, `SetShoreReflection`, `g_water`, `g_waterStrengthSetting`, `g_waterReflSetting`, `g_waterFilter`, `g_waterColorCompression` | Settings, Lamp Glow card, live apply |
| `features/lot_light_bridge.cpp` | `DrawLake`, `SetWaterFix` | The pass |
| `shaders/water_lamps_ps.hlsl` | | Pass shader |
| `features/depth_share.h` | `DepthShare::Texture`, `Surface`, `SetInternalPass` | Depth provider link |

**Game code.** None patched. Shader recognition is by exact ID; the lamp list comes from `FUN_006acf70`
(`0x006ACF70`, Steam 1.67.2).

**Ocean reflection** (not reused): the ocean shader reads a 1024x1024 planar reflection in `s6`. The camera
view-projection block `c40..c43` is mirrored in that reflection pass, which matters for code that votes on camera
constants (see [engine/camera-and-map-view.md](../engine/camera-and-map-view.md)).

Constants, rotated-lot projection and depth linearisation of the pass: [night-lighting/water.md](night-lighting/water.md).

## Rejected approaches

- Shore reflection tied to the lamp-glow switch: turning the glow off removed the reflection too.
- Guessing a 40 m depth when none is readable: painted black patches with the game's MSAA on.
- Further pass history (oversized lamp radii, fixed-distance reflection, rotated lots): in
  [night-lighting/water.md](night-lighting/water.md).

Details in [history](../history/reflections.md).

## See also

- [Validation](../validation/reflections.md)
- [History](../history/reflections.md)
- [Night Lighting: water pass](night-lighting/water.md)
- [Depth Blur and the depth share](depth-blur.md)
