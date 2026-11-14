# ReShade/GShade looks

Runs a ReShade or GShade preset (.ini) inside Apex, with the same shader maths, on the game's picture. The presets are
read from the game's `Bin\1. Shader Presets\` folder (subfolders too); the default is `[Amelie] Serene 2.0.ini`.

## Status

| | |
|---|---|
| Availability | Unreleased (development build), Experimental |
| Default | Off |
| Menu | Image > Color, tab *ReShade/GShade* (Lucide `swatch-book`) |
| Configuration | `[patches.GShadeLooks]` in `ApexRadiance.toml` |
| Presets | Part of the Color part |
| Shortcut | *ReShade/GShade looks on or off*, `[ui] looks_key` (Ctrl+Shift+L) |
| Source | [`features/gshade_looks.cpp`](../../features/gshade_looks.cpp) |
| Validation | [validation/gshade-looks.md](../validation/gshade-looks.md) |

## Menu

One card: the preset picker (Rescan, search), *Look options* (Runs, Game UI, Mix and a note about LUTs that copy the
look), *Shaders in my preset (run order)* with per-shader switches, values and run order, the summary of changes from
the .ini (Reset all to .ini, Save as .ini…, Back to the .ini), and *All built-in shaders* with search and the Kind and
Show filters. In developer mode a *Live counters* disclosure shows the status, Last frame and Skipped lines, a typed
preset path with Reload, and the GPU memory in use. Players reload a preset by picking it again in the picker.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| ReShade/GShade (card switch) | `enabled` | bool | off | | Turns the looks on |
| Preset | `presetPath` | string | empty (`[Amelie] Serene 2.0.ini`) | | The .ini that is run |
| Look options > Runs | `beforeColor` | bool | off (After Apex color) | | Runs the look before Apex's color instead of after it |
| Look options > Game UI | `onUi` | bool | on (Shaded too) | | Also shades menus and loading screens |
| Look options > Mix | `mix` | float | 100% | 0 to 100% | How much of the look is blended over the game |
| (none) | `protectUi` | bool | on | | Never runs blur, anti-aliasing or distortion over the UI; always on, an older saved off is ignored |
| (none) | `useTweaks` | bool | on | | Uses the edited values; always on, tweaks saved while it was off are dropped |
| Shader switches | `off` | array | empty | | Shaders of the preset switched off |
| All built-in shaders | `extra` | array | empty | | Shaders added to the preset |
| Run earlier / Run later | `runOrder` | array | empty (the .ini's order) | | The run order |
| Shader values | `tweaks` | table | empty | | Edited values per shader |
