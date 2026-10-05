# Night Lighting, lamp colour: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/lamp-colour.md](../features/night-lighting/lamp-colour.md).

### 2026-09-25: pink lamp light on snow

**Context:** pools of lamp light on white snow looked pink.

**Finding:** captures `LightProbe-m12` (snow) and m14 (no snow): the lot light map is identical with and without snow,
mean (0.313 / 0.267 / 0.275); in lamp-lit areas G/R = 0.754 and B/R = 0.793, the stock lamp colour (1 / 0.75 / 0.79).
Other stock colours in the F8 dumps (G/R, B/R): 0.98/0.85 (warm), 1.01/1.18 (bluish), 0.81/0.56 (orange), 1/1 (white),
0.99/1.06 (slight lilac); these are not touched.

**Outcome:** tint exactly the stock pink towards warm white of the same luminance at the creation colour setter.

### 2026-09-25 (about 01:40): creation path not enough

**Context:** after the first fix, `S3SS_LightDiag-neve-praca.txt` and m17 still showed 56 street lamps (type 11) and 46
lot lamps (type 3) at (1 / 0.75 / 0.79).

**Finding:** the lamp object's script sets the colour after creation (`FUN_006B0B50` -> `FUN_006BC3E0`).

**Outcome:** the call at `0x006B0BDE` is redirected too (fixed about 02:10).

### 2026-09-25 (about 03:30): unaligned script buffer

**Context:** review item 2.

**Finding:** `FUN_006BC3E0` reads its argument with `MOVAPS`; an unaligned stack buffer could crash.

**Outcome:** the local buffer is `alignas(16)`.

### 2026-09-25: plan not implemented

**Context:** ROADMAP-NIGHT-REMAKE.md phase 8 lists optional per-type colours (street lamps cooler, garden lamps warmer).

**Finding:** no implementation.

**Outcome:** not started.

### 2026-09-29: live colour and a lot lamp colour (1.5.0)

**Context:** the colour applied only when a light's colour was written, so a slider change needed a save reload.

**Finding:** the lights that received the stock pink can be tracked and rewritten through the script setter.

**Outcome:** `RetintLamps`, `corPropriaNoLote` and `corDasLampadasDoLote`. Released in 1.5.0.

### 2026-10-03: menu controls removed (2.5.5)

**Context:** the menu was simplified in 2.5.5.

**Finding:** the colour rows ("Lamp color", "Own color for lot lamps", "Lot lamp color") were removed from the Lighting
page as part of the menu simplification.

**Outcome:** the TOML keys and the live re-colouring remain; colours can be set through the configuration file or a
profile.
