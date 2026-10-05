# Lamp colour

The game's stock street lamps and lot lamps have a pink base colour. It barely shows on green grass but turns snow and
pale walls pink. Night Lighting turns exactly that stock pink into a warm white of the same brightness, everywhere the
lamp light lands: ground, roads, walls, objects, roofs and water. Colours picked in Build mode and every other stock
colour are left alone. Part of [Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released in 1.0.0. Live re-colouring and a separate lot lamp colour: 1.5.0. Menu controls removed in 2.5.5 (the TOML keys remain) |
| Default | On: warm white for street and lot lamps |
| Menu | None. Set in `ApexRadiance.toml` or through a saved profile |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/object_light_bridge.cpp`](../../../features/object_light_bridge.cpp) (`InstallLampColour`, `TintStockColour`, `RetintLamps`) |

## The problem

Stock lamps carry the base colour (1, 0.75, 0.79). In lamp-lit areas the lot light map shows exactly those ratios
(G/R = 0.754, B/R = 0.793), with or without snow, so pools of lamp light on white snow look pink. The colour reaches a
light twice: when the light is created from its definition, and again when the lamp object's script sets it. See
[engine/light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md).

## How Apex Radiance solves it

Apex Radiance redirects the two game calls that write a light's colour. Before the game stores the colour, the stock
pink is recognised by its ratios and blended towards warm white (1, 0.80, 0.62) with the same luminance. Every
lamp-lit surface then reads the corrected colour with no further change.

1. **At creation:** the seven light-type constructors' calls to the colour setter go through `LampColourSet`.
2. **From the lamp script:** the script colour setter call goes through `LampColourSetScript`. Build-mode colours also
   pass here and are left alone unless they are exactly the stock pink.
3. **Tracking:** each light that received the stock pink is remembered with its original pink, so a later change of the
   colour setting re-colours it live (`RetintLamps`), followed by one terrain rebuild and one re-solve of the lots'
   light.
4. **Street or lot:** a light counts as a street lamp when it is of the street-lamp class (`0xB`) outside any lot; other
   lamps use the lot lamp colour when *own colour for lot lamps* is on, otherwise the street lamp colour.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| (not in the menu) | `luzDasLampadasNatural` | float | 1.0 | 0 to 1 | Street lamps (and lot lamps without their own colour): 0 = the game's pink, 1 = warm white |
| (not in the menu) | `corPropriaNoLote` | bool | off | | Lot lamps use `corDasLampadasDoLote` instead of the street lamp colour |
| (not in the menu) | `corDasLampadasDoLote` | float | 1.0 | 0 to 1 | Lot lamp colour when `corPropriaNoLote` is on |

Values are clamped to 0 to 1 by `SetLampTint`. A change applied while the game runs (for example a profile import) is
taken once no menu slider is held: tracked stock lamps are re-coloured, the terrain light is rebuilt once and the lots'
light is solved again. The Lighting balance styles do not change lamp colours.

## Compatibility and interactions

- Every Night Lighting surface reads the corrected colour:
  - the terrain light bake (`FUN_00C292B0`) reads the base colour `+0xF0` at `0x00C2950F`, so world light maps, smoothed
    maps and the atlas are warm white after a rebuild ([terrain-relight.md](terrain-relight.md));
  - object rigs: `vfunc+0x10` records and `BoostRec` use `+0xF0` ([objects-and-rigs.md](objects-and-rigs.md));
  - roofs, water and per-pixel object lamps: `ReadLamp` uses `+0xF0 x intensity x fade`;
  - the room and lot light solve (walls, floors, lot grass) uses the light's own evaluation `vfunc+0x4C`. Which colour
    field it reads is not verified, but the lot map carried the stock ratios, so it follows the light's colour.
- Colours chosen in Build mode differ from the stock ratios and are left alone, unless the player picks exactly the stock
  pink.
- Sims3SettingsSetter patches none of these call sites.

## Limitations

- Only the one stock pink is recognised (tolerance 0.03 on both ratios).
- With the feature uninstalled the call sites are restored, but colours already written stay until the game writes them
  again (reloading the save).
- There are no per-type colours (for example cooler street lamps and warmer garden lamps).

## Technical reference

### Where a light's colour lives

| Offset | Meaning |
|---|---|
| `light+0xF0` | Base colour (r, g, b) |
| `light+0xE0` | Intensity x colour while lit |
| `light+0x10` | Intensity |
| `light+0x20` | Fade (night fade-in) |
| `light+0x100` | Flags (0x01 alive, 0x04 room known, 0x20 lit, 0x40 enabled) |
| `light+0xB0` | Light class (0xB = street lamp) |
| `light+0xC0`, `+0xC4` | Lot id (0 = not on a lot) |

Both colour writers fill `+0xF0` and `+0xE0`.

### The two writers

1. **Creation from the light definition:** `FUN_006BDA90(light, def+0x10)` (`thiscall`, reads only rgb), with seven call
   sites, one per light-type constructor created by the light factory `FUN_006AC590`:

   | Call site | Constructor (factory case) |
   |---|---|
   | `0x006C047D` | `FUN_006C0450`, type 3 "Lighting/BareBulb" |
   | `0x006C051D` | `FUN_006C04F0`, type 0xB (street lamp, "Lighting/BareBulb") |
   | `0x006C05C1` | `FUN_006C0590`, type 5 "Lighting/ShadedLamp" |
   | `0x006C1251` | `FUN_006C1220`, type 6 "Lighting/TubeLight" |
   | `0x006C15D1` | `FUN_006C15A0`, type 9 "Lighting/RectangleAreaLight" |
   | `0x006C1891` | `FUN_006C1860`, type 10 "Lighting/DiscAreaLight" |
   | `0x006C1B11` | `FUN_006C1AE0`, type 4 "Lighting/Spot" |

   Window lights (types 7 and 8) have no such call. The mapping is inferred from the address ranges of the constructors.
   Each call goes to `LampColourSet<Street>(light, rgb)` (`Street` = true only for site `0x006C051D`; its lot id may not be
   set yet, and the later script colour decides with the lot id), which copies rgb into a local, applies
   `TintStockColour` and calls the original `0x006BDA90`.
2. **The lamp object's script:** `FUN_006B0B50(objId, r, g, b)` (called by `FUN_006B1850`) calls
   `FUN_006BC3E0(light, rgba)` for every light of the object. The call at `0x006B0BDE` goes to
   `LampColourSetScript(light, rgba)`. The game passes (r, g, b, r); `FUN_006BC3E0` reads the buffer with `MOVAPS`, so it
   is `alignas(16)`; after tinting, w is set back to r; then the original `0x006BC3E0` is called. Street or lot is decided
   by `IsStreetLight` (class `0xB` and lot id 0).

Every redirect rewrites the rel32 of an `E8` call after checking the opcode and the current target. If any of the eight
sites does not match, all are restored and the log says `[ObjectLightBridge] Lamp colour: call site differs`. Addresses
come from `GameAddr` (`SetLightColour`, `SetColourCall0..6`, `ScriptSetColour`, `ScriptSetColourCall`); the values above
are Steam 1.67.2.

### The transform (`IsStockPink`, `TintPink`)

```
t = colour setting for this light (street or lot); stock pink only:
  r > 0.05, |g/r - 0.75| < 0.03 and |b/r - 0.79| < 0.03
lumPink = 0.2126 + 0.7152 g' + 0.0722 b'          (g' = g/r, b' = b/r)
lumWarm = 0.2126 + 0.7152 x 0.80 + 0.0722 x 0.62
k = r x lumPink / lumWarm
warm = (k, 0.80 k, 0.62 k)                         ; same luminance as the original
colour = pink + (warm - pink) x t                  ; skipped when t <= 0
```

`TintStockColour` records each stock-pink light (`g_tintedLights`: original pink, written colour, street flag) and forgets
a light whose colour is anything else. `g_tinted` counts corrected writes.

### Live re-colouring (`RetintLamps`)

When the street or effective lot colour changes and no menu slider is held, `NightTerrainRelightPatch` calls
`SetLampTint`, enumerates all lights (`LotLightBridge::EnumerateAllLights`) and calls `RetintLamps`. For each tracked
light still present whose `+0xF0` still equals the written colour, the new colour is written through the script setter
(`+0xF0`, and `+0xE0` when lit). Lights no longer enumerated, or with another colour, are dropped. When at least one light
changed, the rigs regather, a lamp refresh is requested, one terrain rebuild is queued ("lamp colour changed") and the
lots are re-solved. Every 30 s the same call with unchanged colours prunes freed lights.

### Files

| File | Symbols | Role |
|---|---|---|
| `features/object_light_bridge.cpp` | `InstallLampColour`, `UninstallLampColour`, `SetLampTint`, `RetintLamps`, `LampColourStatus` | Install, live change, status |
| | `IsStockPink`, `TintPink`, `TintStockColour`, `IsStreetLight`, `LampColourSet`, `LampColourSetScript`, `RedirectCall` | Transform and thunks |
| `patches/night_terrain_relight_patch.cpp` | Settings, `Install`, `Uninstall`, the per-frame update (`g_tintSeen`) | Lifecycle |

## Rejected approaches

- Hooking only the creation path: the lamp script writes the colour again afterwards. Details in
  [history](../../history/night-lighting-lamp-colour.md).
- Passing an unaligned buffer to the script setter: it reads with `MOVAPS` and could crash.

## See also

- [Validation](../../validation/night-lighting-lamp-colour.md)
- [History](../../history/night-lighting-lamp-colour.md)
- [Objects and rigs](objects-and-rigs.md), [Snow](snow.md), [Terrain relight](terrain-relight.md)
- [Engine: light objects and rigs](../../engine/light-objects-and-rigs.md)
