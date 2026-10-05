# Night Lighting, water: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/water.md](../features/night-lighting/water.md).

### 2026-09-24: lake and ocean shader analysis

**Context:** water was very dark at night and did not reflect the lights.

**Finding:**

- Lake (`LightProbe-lago`, draw #70): `PS_253F0B20` (`ps_3_0`) + `VS_253F0CB0`. Samplers: `s0`, `s1` wave normal maps
  (256x256, format 63), `s2`, `s3` sky cubes for the reflection, `s4` colour ramp by depth (512x4 DXT1), `s5` sun shadow,
  `s6` scene copy for refraction (2048x1024, the image before the water). No lamp light map and no rig: brightness comes
  only from the sun (specular and shadow) and the fixed sky reflection. The lot terrain passes under it (#2, #7, #14,
  #16) were drawn with the lot bridge active.
- Ocean (`LightProbe-oceano` #176): `PS_1AC2FBF0` / `VS_1B0FB6F0`, stencil on; `s0`/`s1` waves, `s2` foam (256x256
  DXT1), `s3` ramp (512x4), `s4` refraction (2048x1024), `s6` a real-time planar reflection (1024x1024 render target).
  The ocean reflects the actual scene every frame, the lake only the sky cube. Neither gets lamp light on the surface.

**Outcome:** a second pass after the lake draw, with lamp glow first.

### 2026-09-24: whole lake pinkish white

**Context:** first in-game test of the lamp glow.

**Finding:** the radius came from the light bounds (`+0x134`, about 50 m) and each street lamp has two lights at the same
place (`+0x130` = 97 and 40), so 16 lamps covered the lake. F8 data: intensity 1.0, colour (1, 0.75, 0.79), `+0x130` =
40, 70, 97 or 100.

**Outcome:** visual radius `clamp(sqrt(range) x 1.2, 2, 25)` (7 to 12 m), glow x 0.08, specular
`x 2 / (1 + d^2 / (16 R^2))`, sum clamped to 0.8.

### 2026-09-24: ocean reflection rejected for lakes

**Context:** a report of ocean reflections appearing on the lake suggested reusing the ocean's planar reflection.

**Finding:** it is a fixed mirror at sea level, rendered only while the ocean is visible. Making lakes use it would be
expensive and touch the game's reflection pipeline.

**Outcome:** rejected.

### 2026-09-25: first reflection, fixed distances

**Context:** first shore reflection.

**Finding:** the reflected ray (smoothed wave normal x 0.5) was sampled at fixed distances 2, 6 and 14 m, projected with
the water VS WVP (VS `c4..c7` to PS `c53..c56`, translation `c57`), read from the scene copy `s6`, about 500
instructions. Positions were wrong at times because three fixed distances cannot find the surface.

**Outcome:** replaced by a ray march against Depth Blur's INTZ depth.

### 2026-09-25: black pond with white streaks (m25)

**Context:** plaza ponds at night in snow (`LightProbe-lago-preto`, screenshot 42) showed black patches with white
streaks.

**Finding:** the first ray march used a fallback "guess at 40 m with weight 0.6" when nothing was hit. With the game's
MSAA on there is no depth, so every pixel used the guess and sampled the wrong part of the scene copy. Captures from m16
on showed "samples=8".

**Outcome:** no depth means no screen reflection at all; a ray with no hit keeps the game's sky reflection.

### 2026-09-25: rotated lots

**Context:** reflections and glints were misplaced on a rotated lake.

**Finding:** projecting `(world - translation)` with the local-to-clip matrix is wrong when the world matrix is rotated.

**Outcome:** world-to-clip is computed on the CPU as `WVP x inverse(World)`.

### 2026-09-25: Depth Blur blurred mid-frame

**Context:** review item 1 (about 03:30).

**Finding:** the pass sets `ZENABLE` off; Depth Blur treated it as the first interface draw and blurred mid-frame.

**Outcome:** `DepthShare::SetInternalPass(true)` around the pass.

### 2026-09-25: plans not implemented

**Context:** open items after m25.

**Finding:** depth for water with MSAA on would need resolving the MSAA depth or drawing depth a second time. A lamp
term for the ocean surface and pools would need their own shaders.

**Outcome:** not started.

### 2026-09-28: reflection independent of the glow switch

**Context:** the shore reflection was drawn only when the lamp glow was on. The combined build also multiplied `lamps` by
`params.z` (HDR lamp gain) after the 0.8 clamp, and compiled the pass at the first lake draw.

**Finding:** the pass can run with lamp strength 0. HDR was removed from Apex Radiance.

**Outcome:** the pass runs whenever the glow is on, or the reflection is above 0 and the scene depth exists. The pass is
compiled at start-up by `framework/shader_cache`. No HDR factor (see [removed-features.md](../removed-features.md)).

### 2026-10-01: second lake shader (2.5.2)

**Context:** player report on 2.5.1: glow and shore reflection vanished in cloudy or rainy weather.

**Finding:** the game has two lake pixel shaders: the sunny one compares the sun shadow with `texldp`; the other weather
one compares it by hand (`texld` + `cmp` against `v5.z`). Only the first was recognised.

**Outcome:** `kLakePs2` added. When a game pass stops being patched, first search the shader package for a sibling shader
with the same constants.

### 2026-10-02: lamp highlight filter (2.5.4 release candidate)

**Context:** lamp sparkles shimmered on moving waves and very bright highlights clipped per channel.

**Finding:** version `2.5.4-rc-water-lamp-filter` changed only the lamp contribution. `waterSpecularFilter` widens
undersampled lamp highlights from screen-space wave-normal derivatives (variance capped at 0.016, exponent from 250 to
about 81.6 at the cap, peak scaling compensates). `waterPreserveLampColors` replaces per-channel clipping with a shared
RGB scale (identity up to 0.7, smooth approach to 0.8). Instruction slots rose from about 670 to 700. The offline harness
passed 12 flat-normal A/B cases.

**Outcome:** kept, both on by default, with A/B switches in Developer > Lighting. Released in 2.5.5.

### 2026-10-03: glow intensity range (2.5.5)

**Context:** the glow slider allowed up to 300%.

**Finding:** no measurement was recorded beyond the new range itself.

**Outcome:** `brilhoNaAgua` limited to 10 to 40%, default 40%; the run-time clamp also limits older saved values. Shore
reflection strength unchanged. At that time the light styles set the glow (Soft 30%, Natural and Bright 40%); the current
Lighting balance styles do not change water settings.
