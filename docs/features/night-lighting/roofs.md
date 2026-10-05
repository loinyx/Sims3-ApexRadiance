# Roofs and roof snow

At night, roof tiles near street lamps and outdoor lot lamps catch the lamps' warm light instead of staying black next
to a lit wall. Roof shadows from the sun and moon are softer. In winter, the snow lying on roofs is lit around lamps in
the same way. Part of [Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released in 1.0.0 |
| Default | On |
| Menu | Lighting > Buildings > *Buildings* card, ROOFS group |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) (`DrawRoof`, `DrawRoofSnow`, `SelectLamps`), [`shaders/roof_ps.hlsl`](../../../shaders/roof_ps.hlsl), [`shaders/roof_snow_lamps_ps.hlsl`](../../../shaders/roof_snow_lamps_ps.hlsl) |

## The problem

The game's roof pixel shader has no lamp term at all. It lights roofs with the sun or moon (with a 4-tap shadow), an
ambient cube, a sky reflection cube and a mask; there is no lot or terrain light map and no rig light. At night a roof
stays black even when a wall sconce lights the wall right below it. The snowy roof shader has the same gap. See
[engine/shaders.md](../../engine/shaders.md).

## How Apex Radiance solves it

Summer roofs are drawn with a replacement pixel shader that reproduces the game's lighting and adds up to 16 nearby lamps
and a 16-tap shadow. Snowy roofs are drawn by the game unchanged, then a second additive pass on the same geometry adds
the lamps on the game's snowy albedo.

1. **Lamp list.** Every 20 frames the lit street lamps and outdoor lot lamps are read from the game's light list, with a
   visual radius and the lamp's current colour.
2. **Lamp choice per roof piece.** Up to 16 lamps are chosen from the roof piece's world position, never from the
   camera, so zooming and rotating do not change which lamps light a roof.
3. **Summer roofs:** the replacement shader adds the lamp term to the diffuse light before the mask.
4. **Snowy roofs:** an additive pass recomputes the snowy albedo and adds `mask x albedo x lamps`.
5. **Stair snow:** the snowy-roof pixel shader is also used for snow on stair tops; those draws are recognised by their
   vertex shader and handled by the stair snow fix in [snow.md](snow.md) instead.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Lamps light roofs | `telhadosComLuz` | bool | on | | Lamp light on summer and snowy roofs, and the softer 16-tap roof shadow |
| Brightness (ROOFS) | `forcaNosTelhados` | float | 60% | 5 to 200% | Lamp strength on roofs (PS `c52.x`); shown when the switch is on |

Both apply live every frame (`LotLightBridge::SetRoofFix`). The Lighting balance styles set `forcaNosTelhados` to 40.5%
(Subtle), 45% (Soft) or 60% (Natural). Roof lamps work without *Street lamps light lots*: the roof branches run before
that gate in the draw dispatch.

## Compatibility and interactions

- [water.md](water.md) and outdoor objects ([objects-and-rigs.md](objects-and-rigs.md)) share the lamp list and the lamp
  choice.
- The roof lamp law (`(1 - d^2/R^2)^2` x wrap) is not the bake-matched law used for per-pixel lamps on objects
  (`W = 0.4 x range`, `min(1, W cos / d^2)`).
- Lamp colour: roofs read the tinted base colour of each lamp ([lamp-colour.md](lamp-colour.md)).
- No game code is patched; recognition is by exact shader identity and, for stair snow, by vertex shader pattern.

## Limitations

- At most 16 lamps per roof piece, chosen within 80 m of the piece's origin.
- Lamps are not occluded: a lamp behind a chimney still lights the roof.
- Only the exact roof vertex and pixel shaders are handled; any other roof variant keeps the game's lighting (none has
  been reported).
- The snowy roof pass is additive, so it cannot darken, and it relies on a re-implementation of the game's snowy albedo.
  A change to the game's winter roof shader would need a check.

## Technical reference

### Lamp list (shared with water and outdoor objects)

- Every 20 frames `LotLightBridge::OnPresent` enumerates all lights (`FUN_006ACF70` with a visitor; 15-byte prologue
  check) and `ReadEnumeratedLamps` rebuilds `g_allLamps` with `ReadLamp`:
  - alive (`+0x100 & 0x01`) and lit (`& 0x20`);
  - a lot-owned lamp (lot id `+0xC0`/`+0xC4` non-zero) must also be enabled (`& 0x40`), so a switched-off lot lamp leaves
    the list at once rather than when its fade reaches zero;
  - a street lamp (type `+0xB0 == 0xB`) or an outdoor lamp (`& 0x04` and room `+0x08 == 0`);
  - range `+0x130` must be above 0.01.
- Per lamp: head `+0x120`; visual radius `R = clamp(1.2 x sqrt(range), 2, 25)` (range values 97, 40, 100 give about 7 to
  12 m); colour = base colour `+0xF0` x intensity `+0x10` x fade `+0x20`.
- `SelectLamps(x, z, maxScore)`: score = horizontal distance to the lamp - `R`; lamps with score > `maxScore` are
  ignored; the 64 best candidates are kept, then the 16 lowest scores go to `g_lampData`: rows 0 to 15 = (head, `R`),
  rows 16 to 31 = (colour, 0), row 32 = parameters.
- `SelectLamps` is memoised: 512 direct-mapped entries keyed on the exact float bits of (x, z, maxScore), holding the 32
  rows, the count and the candidate number. A rebuild that changes `g_allLamps` by even one bit starts a new generation,
  so a hit is exactly the scan's result.

### Summer roofs (`DrawRoof`)

- Recognition by exact identity (`shaders/shader_ids.h`): PS `kRoofPs` {1136 bytes, FNV-1a `0x6EC87E3B`}
  (`PsClass::Roof`) and VS `kRoofVs` {1192, `0x1F851ECB`} (VS class 1).
- The game's VS provides: world normal in TEXCOORD0, shadow position TEXCOORD1, fade COLOR1, fog TEXCOORD3, texture UV and
  world xz x 0.5 in TEXCOORD4 (`.xy`, `.zw`), mask UV and world y in TEXCOORD5 (`.xy`, `.w`), view vector TEXCOORD6,
  top-texture UV in COLOR0, camera in VS `c11`.
- Lamp choice: `SelectLamps(c8.w, c10.w, 80)` from the roof piece's world translation.
- Replacement `roof_ps.hlsl` (`kRoofHlsl` in `roof_ps_hlsl.h`, `ps_3_0`, about 353 instructions) = the game's lighting
  plus:
  - 16-tap shadow (4x4 at offsets `(x - 1.5) x c2.y`) instead of 4 taps, faded to 1 by COLOR1;
  - lamps at `p = (uv.z x 2, maskUv.w, uv.w x 2)`: per lamp `w = sat(1 - d^2/(R^2 + 1e-3))`,
    `wrap = sat((N.l/|l| + 0.5) / 1.5)`, `lamps += colour x w^2 x wrap`; then `lamps x= c52.x`;
  - `diffuse = (ambient cube x c6.x + sat(N.L) x c0 x shadow + lamps) x mask(s6)`; the rest (albedo `s4 top x s2 x c3`,
    reflection with Fresnel `sat((1 - N.V)^3 + c7.x)`, specular mask `s3 x c4`, fog lerp and `x c8.x`, alpha
    `sat(lum - c5.x)`) as the game.
- `DrawRoof` saves PS `c20..c52` (33 registers), sets the replacement and constants, draws, and restores.

### Snowy roofs (`DrawRoofSnow`)

- Recognition: PS `kRoofSnowPs` {4992 bytes, `0x3CEB025E`} (`PsClass::RoofSnow`), no VS identity check, but never when the
  VS is the snow-relief class 9 (stair snow).
- The game's roof is drawn unchanged. Then, if at least one lamp was selected and `|VS c15.x| > 1e-6`, a second pass on
  the same geometry with `roof_snow_lamps_ps.hlsl` (`kRoofSnowLampsHlsl`, `ps_3_0`): blend ONE/ONE, BLENDOP ADD,
  `ZWRITEENABLE` off, alpha test off, colour write RGB (`0x7`), separate alpha off. PS `c20..c53` (34 registers) and the 8
  render states are saved and restored.
- The pass recomputes the game's snowy albedo (its first ~20 instructions):
  `snow = sat(2 c6.z)`, `cover = sat(1.4 snow)`, `a = albedo(s4) x c3 x top(s6, N.y >= 0 ? COLOR0 : 0)`,
  `b = sat((a.r a.g a.b x 500 + 0.2)(snow + 1) + a)`, `s = snow texture(s3, TEXCOORD2.zw)`,
  `c = lerp(min(s, b), s, snow)`, `albedo = sat((1 - n.w)(c - a) + a)`, `mask = lerp(s7.x, 1, cover)`. Output:
  `mask x albedo x lamps x c52.x x c8.x x (1 - fog.w)`, with the same per-lamp law as summer roofs.
- **Position:** xz = `COLOR0 x c53.x`, where the VS writes `COLOR0 = world xz / VS c15.x` (`rcp r0.w, c15.x;
  mul o9.xy, r2.xzzw, r0.w`, used by the game for the top texture) and the C++ copies VS `c15.x` into PS `c53.x`;
  y = TEXCOORD5.w.

### Stair snow routing

The snow on stair tops (`VS_2E036438` / `PS_2E036820`) uses byte for byte the same pixel shader as snowy roofs
(`PS_2E036820 = PS_2793C3E8 = PS_27D4DAE0 = kRoofSnowPs`). `OnDrawInner` sends `RoofSnow` to `DrawRoofSnow` only when
`!g_curVsIsSnowRelief`; class 9 draws go to `DrawSnowRelief` ([snow.md](snow.md)).

### Registers

| Register | Summer roof (replacement) | Snowy roof pass |
|---|---|---|
| `c0..c8` | Game: sun colour, sun direction, shadow size, albedo tint, specular tint, alpha reference, ambient scale, Fresnel bias, fog mix | `c3` albedo tint, `c6.z` snow amount, `c8.x` output scale |
| `c20..c35` | Lamp head + radius | Same |
| `c36..c51` | Lamp colour | Same |
| `c52` | x strength, y count | x strength |
| `c53` | | x = VS `c15.x` |
| Samplers | `s0` environment cube, `s1` ambient cube, `s2` albedo, `s3` specular mask, `s4` top, `s5` shadow, `s6` mask | `s3` snow, `s4` albedo, `s6` top, `s7` mask |

### Shader compilation

Both replacements are compiled at start-up on a background thread by `framework/shader_cache` (see
[architecture.md](../../architecture.md), section 4.6) and created as D3D9 shader objects at their first draw. The
`*_hlsl.h` headers are what the build uses (raw string literals generated from the `.hlsl` files, which are not in the
project); keep each pair in sync by hand.

### Files

| File | Symbols | Role |
|---|---|---|
| `features/lot_light_bridge.cpp` | `DrawRoof`, `DrawRoofSnow`, `SelectLamps`, `SelectLampsScan`, `ReadLamp`, `ReadEnumeratedLamps`, `OnDrawInner`, `RoofStatus`, `SetRoofFix`, `CompilePs` | Dispatch and constants |
| `shaders/roof_ps.hlsl`, `shaders/roof_ps_hlsl.h` (`kRoofHlsl`) | `main` | Summer roof replacement |
| `shaders/roof_snow_lamps_ps.hlsl`, `shaders/roof_snow_lamps_hlsl.h` (`kRoofSnowLampsHlsl`) | `main` | Snowy roof additive pass |
| `shaders/shader_ids.h` | `kRoofPs`, `kRoofVs`, `kRoofSnowPs` | Exact identities (size + FNV-1a over DWORDs) |
| `features/shader_patches.cpp` | `IsSnowReliefVs`, `PatchSnowRelief` | Stair snow |

Lamp enumeration: `FUN_006ACF70` (`0x006ACF70`, `stdcall(visitor)`, prologue
`E8 2B 36 00 00 8B 4C 24 04 51 68 40 CF 6A 00`), Steam 1.67.2.

## Rejected approaches

- Strength 1 by default: roofs almost white. Details in [history](../../history/night-lighting-roofs.md).
- Radius from the light bounds (`+0x134`, about 50 m): roofs blew out to pinkish white.
- Choosing the 16 lamps nearest the camera: roofs flickered when zooming.
- Snowy roof position from TEXCOORD4.zw: `c19` is zero on some roofs.
- Classifying by pixel shader before vertex shader: stair snow went to the roof pass.

## See also

- [Validation](../../validation/night-lighting-roofs.md)
- [History](../../history/night-lighting-roofs.md)
- [Snow](snow.md), [Water](water.md), [Objects and rigs](objects-and-rigs.md)
- [Engine: shaders](../../engine/shaders.md)
