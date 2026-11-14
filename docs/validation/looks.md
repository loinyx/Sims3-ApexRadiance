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

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `tools/looks_shaders_gen.py` | Every built-in port compiles for ps_3_0 with fxc; regenerates `reshade_shaders_hlsl.h` | `python -I tools/looks_shaders_gen.py` | fxc from the Windows SDK (or `FXC`) |
| `tools/looks_fx_check` | The player's .fx: real compile path on a folder of packs, per technique OK or the reason; with `--run` each runnable technique created and run on a Direct3D 9 device; `--catalog` the background scan, preset lookup, built-in priority, missing / refused files; the About licence text | see its README | MSVC, a Direct3D 9 device for `--run`, a folder of packs |
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

## In-game test plan

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

## Confirmed in game

- Nothing yet.

## Open checks

- Every step of the in-game test plan.
- The player's .fx effects have run only offline on a separate Direct3D 9 device, never in the game: the look of each
  effect, its cost per frame, depth through DepthShare, the state restore around it (and with the RTX Remix bridge),
  the menu layout at 400 units and 200% text, and the scan's effect on frame pacing are unverified.
