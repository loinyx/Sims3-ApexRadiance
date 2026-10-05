# Lot Map Probe: validation

The technique is described in [features/dev-tools/lot-map-probe.md](../features/dev-tools/lot-map-probe.md). The tool
is not part of Apex Radiance, so nothing here is run against current builds. If it is revived, it must:

- Redraw every qualifying scene draw into the map without changing what the player sees.
- Recognise the scene camera in every shader family's constant block.
- Produce heights that match known objects.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| None | | | |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-09-27 | Combined build | In-game captures MapaLote_1 to MapaLote_3 | Redraw worked; 75k to 134k draws skipped as "other camera" | Not recorded |
| 2026-09-27 | Combined build | In-game capture MapaLote_4 | Failed: all-zero block won the camera vote, every value `nan` | Not recorded |

## In-game test plan

Combined build only:

1. Developer build, Live mode. Apex tab > Performance > Developer tools > Lot Map Probe > Enabled.
2. Capture lot map, close the menu within 2 s, orbit the camera slowly for about 4 s. **Expected:** the log line
   `[LotMapProbe] Saved to MapaLote_N (R draws redrawn, P% covered)`.
3. Open `info.txt` first. **Expected:** no `nan` and a small "other camera" count; otherwise the camera vote failed.

## Confirmed in game

- Re-rendering the game's draws from an ortho camera works with the game's own shaders, textures and alpha test.

## Open checks

- No capture was made after the `LooksLikeCamera` fix.
- `upSign` and the camera position decode are not validated against a known height.
