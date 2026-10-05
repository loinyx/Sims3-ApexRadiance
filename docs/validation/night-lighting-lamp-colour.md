# Night Lighting, lamp colour: validation

The feature is described in [features/night-lighting/lamp-colour.md](../features/night-lighting/lamp-colour.md). It must:

- Change only lights whose colour is the stock pink (within 0.03 on both ratios), keeping their luminance.
- Leave every other stock colour and every Build-mode colour unchanged.
- Install all eight call redirects or none.
- Re-colour tracked stock lamps when the setting changes at run time, and leave untracked or recoloured lights alone.

## Automated tests

No repository harness covers the lamp colour redirects or the transform.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| | | | No automated runs recorded | |

## In-game test plan

1. Load a snowy save at night. **Expected:** pools of lamp light on snow look warm white, not pink.
2. Set `luzDasLampadasNatural = 0` through a profile while the game runs. **Expected:** stock lamps turn pink again
   without reloading; the log shows `[ObjectLightBridge] Lamp colour changed live: N lights re-coloured (street 0.00,
   lot 0.00)`.
3. Turn `corPropriaNoLote` on with `corDasLampadasDoLote = 0`. **Expected:** lot lamps pink, street lamps warm white.
4. Place a lamp with a custom Build-mode colour. **Expected:** its colour is unchanged.
5. Developer status. **Expected:** `Lamp colour: active | lights with corrected colour: N | stock lamps tracked: M` (N
   grows while lights are created on load).
6. F8 light diagnostics (developer mode). **Expected:** stock lamps show ratios about 0.80 / 0.62.
7. Log. **Expected:** `[ObjectLightBridge] Lamp colour: installed (creation + script)`.

## Confirmed in game

- Warm white lamps on snow after the script redirect (combined build, 2026-09-25).

## Open checks

- Which colour field the room and lot light solve (`vfunc+0x4C`) reads is not verified; the lot map follows the light's
  colour in captures m12/m14.
- The constructor-to-type mapping of the seven creation sites is inferred from address ranges, not confirmed per type.
