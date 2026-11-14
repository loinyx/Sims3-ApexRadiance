# Looks: validation

The feature is described in [features/looks.md](../features/looks.md). It must:

- Run the built-in shaders of the chosen look in the look's order, with its values, and leave the picture unchanged
  when the card is off, while the eye or the picture-compare key is held, and while comparing with the game.
- Keep the interface out of the look with *World only*; never run a depth shader over the interface.
- Run once per game frame, also when the look is split around the scene boundary.
- Store the look as a `looks:`, `gshade:` or `apex:` reference, never an absolute path, and never write in GShade's
  folder; read an older `[patches.GShadeLooks]` and `presetPath` once.
- Refuse oversized or malformed preset files and bound every count read from a file or from the settings.
- Keep the edited values, switches and run order across restarts and undo; go back with *Put every value back* and
  *Back to the look's list*.
- Export a file look with a preset as `Looks/<file name>` and import it with the LUT rules (reuse, numbered name).
- Run the player's own .fx techniques from `Apex Radiance\Shaders\` like the built-in ports, compile them off the render
  thread, never crash on a file the compiler cannot handle, keep includes and textures inside that folder, list only
  what runs, and let a built-in port win over a file of the same name.
- Keep each built-in port within 1 level of its author's original compiled by ReShade's compiler.
- Go online only on Download, to GitHub hosts only; keep only .fx, .fxh and images from a pack, inside
  `Shaders\Packs\<pack>`; never change or remove a downloaded pack except on Update or Remove; fail cleanly (the rest
  of the queue goes on) for a pack gone, a rate limit or no connection.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `tools/looks_shaders_gen.py` | Every built-in port compiles for ps_3_0 with fxc; regenerates `reshade_shaders_hlsl.h` | `python -I tools/looks_shaders_gen.py` | fxc from the Windows SDK (or `FXC`) |
| `tools/looks_fx_check` | The player's .fx: real compile path on a folder of packs, per technique OK or the reason; with `--run` each runnable technique created and run on a Direct3D 9 device; `--catalog` the background scan, preset lookup, built-in priority, missing / refused files; the About licence text | see its README | MSVC, a Direct3D 9 device for `--run`, a folder of packs |
| `tools/looks_fx_check --compare` | Each built-in port against its author's original compiled by the real path, on four pictures and four value sets: max and mean channel difference | see its README | MSVC, a Direct3D 9 device, the originals |
| `tools/looks_fx_check --packs` | Downloaded packs: not compiled before they are wanted, browsing keeps none compiled, counted per pack from the check cache; memory | see its README | MSVC, a scratch folder with `Shaders\Packs\` |
| `tools/looks_fx_check --state` | Device state the passes change is all put back by hand; state block and Strength 0 costs | see its README | MSVC, a Direct3D 9 device |
| `tools/looks_fx_check --cache` | Compile cache: same bytecode as without it, compiled again after a `.fx`, `.fxh` or size change | see its README | MSVC, a scratch folder of packs |
| `tools/looks_fx_check --hashes` | Pictures of two builds (`--compare`, `--run`) the same to the level | see its README | MSVC, a Direct3D 9 device |
| `tools/looks_packs_test` | Pack downloads: allowed hosts, HTTP failures, unpacking (layout, caps, unsafe paths), the list joined with the index and the downloaded folders, the queue with simulated answers (gone, busy, offline, a failed update, Stop the rest), which pack has a missing file; `--real` one real download | see its README | MSVC; network only with `--real` |
| `tools/looks_packs_index.py` | Regenerates `features/looks_packs_index.h` from ReShade's list and GitHub | `python -I tools/looks_packs_index.py` | `gh` signed in |
| `tools/looks_test` | Built-in looks against the shader table; licence texts | see its README | MSVC |
| `tools/profile_package_test` | Look ZIP export and import: exact bytes, reuse, numbered name, original kept, mismatched reference refused, built-in looks need no file; Capture part kept or dropped | see its README | MSVC, toml++ |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-11-14 | PR branch | `looks_shaders_gen.py` | 17 shaders, every entry point compiles | None |
| 2026-11-14 | PR branch | `profile_package_test` (check) | 3880 checks, 0 failures | None |
| 2026-11-14 | PR branch | `looks_fx_check --run` on six packs (131 .fx) | 146 of 148 techniques compile and run on a D3D9 device, 0 failures | Direct3D 9 HAL |
| 2026-11-14 | PR branch | `looks_fx_check --run --catalog` on safety files and the packs | 9 unsafe files refused; 14 catalog checks, 0 failures | Direct3D 9 HAL |
| 2026-11-14 | PR branch | `looks_test` | 394 checks, 0 failures | None |
| 2026-11-14 | PR branch | `looks_fx_check --compare` (17 ports, 16 runs each, 1920x1080) | every port within 1 level (max 1, mean at most 0.003); Levels with black = white point reported, not counted | Direct3D 9 HAL |
| 2026-11-14 | PR branch | `looks_fx_check --run` on the 17 originals | 17 of 17 run | Direct3D 9 HAL |
| 2026-11-14 | PR branch | `looks_fx_check --run --catalog` (dh_anime, dh_ahoh, Vibrance) | 15 catalog checks, 0 failures | Direct3D 9 HAL |
| 2026-11-14 | PR branch | `looks_fx_check --packs` (Ptho-FX, standard effects, a test pack) | 8 checks, 0 failures | None |
| 2026-11-14 | PR branch | `looks_fx_check --state` (17 ports, 24 user techniques) | 0 changed states not put back by hand | Direct3D 9 HAL |
| 2026-11-14 | PR branch | `looks_fx_check --cache` (131 .fx) | 131 the same with and without the cache; invalidation 6 of 6 | None |
| 2026-11-14 | PR branch | `looks_fx_check --compare/--run --hashes`, before and after the performance pass | 272 + 146 pictures, the same hashes | Direct3D 9 HAL |
| 2026-11-14 | PR branch | `looks_fx_check --packs` (six packs, three runs) | 17 checks, 0 failures; no pack effect kept | None |
| 2026-11-14 | PR branch | `looks_packs_test --real` | 54 checks, 0 failures; Ptho-FX downloaded through WinHTTP | Network (one pack) |

## In-game test plan

The in-game runner covers the start-up part without a person: `looks-off-on` (Shaders off against on with
`apex:Cinematic`, frame times and the largest free block, ABBA) and `looks-builtin` (each built-in look loads with every
shader built in and draws, no `[Looks]` warning or error). Run them with the 4.0.0 queue
([tools/ingame_runner/README.md](../../tools/ingame_runner/README.md#shaders-and-capture-jobs)); how a look looks stays a
manual check below.

1. Image > Color > Shaders, switch on, pick a look from GShade's folder. **Expected:** the status shows its name and
   shader count; the look shows on the scene and on menus.
2. *Applies to* World only. **Expected:** the HUD and menus keep their own look.
3. Hold the card's eye, then B over the menu. **Expected:** the game without the look while held.
4. Compare with the game (its shortcut). **Expected:** the look is off with the other effects and comes back.
5. Open a shader, move a slider, restart the game. **Expected:** the changed dot and value are kept; *Put back the
   look's values* restores the .ini value.
6. *Save as a new look…*. **Expected:** Looks\<name>.ini is written and selected under My looks.
7. *Import a preset…* twice with the same file, then with a different file of the same name. **Expected:** the same
   file is reused, then "<name>-1" is created.
8. Turn on a LUT. **Expected:** the note under the card; *Turn off the LUT* turns it off and can be undone.
9. Window at 400 units and text size 200%. **Expected:** rows wrap, nothing is cut off.
10. Ctrl+Shift+L. **Expected:** the look turns off and on, as the card switch does.
11. Put a pack (for example AlucardDH's `Shaders\` and `Textures\`) in `Apex Radiance\Shaders\`, turn Looks on.
    **Expected:** the Shader library card shows "Checking your shaders…", then the count; the game keeps its frame rate
    while it compiles.
12. *Add a shader*, group *Your shaders*, add a technique. **Expected:** it runs, its row shows the .fx file name, its
    sliders change it; the setting survives a restart.
13. Choose a look that names a technique of a file you do not have. **Expected:** "<n> missing" in the subtitle, the
    line "add this file to the Shaders folder"; *Add .fx files…* with that file, or *Check again* after copying it,
    makes it run ("All shaders found").
14. A look with `DH_AhOh@dh_ahoh.fx`. **Expected:** "DH_AhOh doesn't run in The Sims 3"; the game does not crash.
15. A depth effect from the folder (for example DisplayDepth). **Expected:** it runs on the world only, never over the
    interface, with the same depth as the built-in ports.
16. Change the game's resolution, and Alt+Tab out and back in full screen. **Expected:** user effects pause while the
    folder compiles for the new size, then run; no corruption after the device Reset.
17. Developer build, *Live counters*. **Expected:** the GPU memory includes the user effects; the reasons of the files
    that do not run are listed.
18. Get shader packs: tick two packs, *Download 2 packs*. **Expected:** ReShade's standard effects download first, then
    the two, one at a time with their percentage; the game keeps playing; each becomes "Downloaded" with "<x> of <y>
    run here", and its shaders appear in *Add a shader* under its name.
19. *Stop the rest* during a download. **Expected:** the current and the waiting packs stop; nothing half-written stays
    in `Shaders\Packs`.
20. Import a preset that uses a pack you do not have (for example one with MXAO). **Expected:** the note card with
    *Download <pack>*; after the download the shaders run without *Check again*.
21. A downloaded pack's ⋯: *Update*, then *Remove*. **Expected:** the pack is replaced, then gone from the card and from
    *Add a shader*.
22. No network (cable out), *Download*. **Expected:** "No connection to GitHub", the note "No connection; the list may
    be out of date"; the menu does not freeze.
23. Window at 400 units and text size 200% with the Get shader packs card. **Expected:** rows and the footer wrap;
    nothing is cut off without its full text on hover.

## Confirmed in game

- Nothing yet.

## Open checks

- Every step of the in-game test plan.
- The player's .fx effects have run only offline on a separate Direct3D 9 device, never in the game: the look of each
  effect, its cost per frame, depth through DepthShare, the state restore around it (and with the RTX Remix bridge),
  the menu layout at 400 units and 200% text, and the scan's effect on frame pacing are unverified.
- Shader packs: only one real download was made (Ptho-FX, from an offline tool); the menu, a large pack, the memory
  that compiled packs keep in the game, and behaviour behind a proxy are unverified.
