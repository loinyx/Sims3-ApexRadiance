# Apex Radiance Setup

A Windows installer (WinUI 3, one self-contained exe) for Apex Radiance and the mods it works with. It finds The Sims 3
(Steam 1.67.2 `TS3W.exe`, EA app 1.69.47 `TS3.exe`, or a folder the player picks), shows what is installed, and applies
only what differs from the player's choice.

## What it does

| Part | Source | Where |
|---|---|---|
| Apex Radiance (required) | `loinyx/Sims3-ApexRadiance` latest release | `Game\Bin\ApexRadiance.asi` |
| Renderer: DXVK (recommended), RTX Remix bridge, or the game's own Direct3D 9 | `doitsujin/dxvk` (x32 `d3d9.dll`); the bridge zip of the Apex release | `Game\Bin` (`client.targetProcess` set to the game's exe) |
| Sims3SettingsSetter | `sims3fiend/Sims3SettingsSetter` | `Game\Bin` |
| Sims 3 Modernization | `rodneypantonial/sims3-modernization` (Steam or EA zip by edition) | `ts3native` in `Game\Bin`, `ts3fixes.package` in `Mods\Packages` |
| MonoPatcher | `LazyDuchess/MonoPatcher` (its bundled loader is left out) | `MonoPatcher.asi` in `Game\Bin`, its package in `Mods\Packages\MonoPatcher` |
| Sims 3 Performance Patch | Nexus Mods only (needs the player's account): Setup opens its page, watches the Downloads folder and installs the `.zip` (or a chosen `.asi`) when the download finishes | `Game\Bin` |
| Ultimate ASI Loader, only when no loader is found | `ThirteenAG/Ultimate-ASI-Loader` (`dinput8.dll` as `wininet.dll`) | `Game\Bin` |

- Every file is downloaded at install time from the author's own GitHub release (never re-hosted) and checked against
  GitHub's SHA-256 digest when the release has one. Without the GitHub API (60 calls an hour per address), the release
  page is read instead.
- Installed components show switched on; switching one off removes it. A file is copied only when it is missing or older
  than the latest release (a file without a version is compared by its SHA-256 with the release's; one Setup cannot
  date stays as it is, and Repair copies it again).
- Every file Setup replaces is moved to `Game\Bin\Apex Radiance backup` (with `.bak` appended, so no loader reads it);
  `setup.json` there records what Setup installed. Repair or remove restores them. Uninstall everything removes Apex
  Radiance and what Setup installed; mods installed by hand stay.
- When the renderer changes the graphics card the game sees (DXVK's TS3W profile and the bridge report a GeForce GTX
  580), the game resets its graphics options once: Setup says so and keeps a copy of `Options.ini`, restored when the
  player goes back to the game's own Direct3D 9.
- The welcome page lists the updates for the installed components at every start, with *Update now*.
- Setup refuses to change files while `TS3W.exe` or `TS3.exe` runs.
- Languages: the 21 of The Sims 3, as the menu (`Languages.cs`): Windows' display language, or the one picked with the
  title bar's language button (kept in `%LOCALAPPDATA%\Apex Radiance Setup\language.txt`). English text is the key;
  each language is a `Strings.<code>.cs` table, and `Strings.pt.cs` lists every key. A text without a pair shows in
  English. `ApexSetup.Tests` checks every table (keys, `{n}` placeholders, empty and duplicate entries).

## Compatibility fixes

The checks of Settings > Compatibility in the game, made before the game starts (`ApexSetup.Core/Compat.cs`). Fixes
run at the end of every install, or with *Fix all now* on the game page.

| Finding | Fix |
|---|---|
| `S3SSApex.asi`, an old build with Apex inside S3SS, or a second `ApexRadiance.asi` in `scripts\` or `plugins\` | Moved to the backup folder |
| Two ASI loaders | All but the first moved to the backup folder |
| Sims3Performance next to `ts3native` with `ShortWaitSpin` or `ThreadStackDiet` above 0 | Both set to 0, copy as `Sims3Performance.ini.apex-backup` |
| S3SS uncompressed lot or Sim textures under the bridge | `enabled = false`, copy as `S3SS.toml.apex-backup` |
| `client.targetProcess` in a `bridge.conf` not the game's exe | Set to the exe |
| The bridge's `dxvk.conf` without the TS3 profile, `d3d9.countLosableResources = False` or `d3d9.samplerAnisotropy = 16` before its first `[exe]` section | Wrong values set, missing lines added at the top |
| `.package` files in `Mods\Packages` (up to 3 folders deep) without `Resource.cfg` | `Resource.cfg` created |
| A `Resource.cfg` that does not read `ts3fixes.package` or MonoPatcher's package | The `Packages` folder lines added, copy as `Resource.cfg.apex-backup` |
| Smooth Patch's `TS3Patch.asi` next to S3SS (same game code) | Moved to the backup folder |
| `ts3native` built for the other edition | Reported; turning Sims 3 Modernization on installs the right one |
| A Steam build of simler90 or XCAS on the EA app (or the reverse), NRaas ErrorTrap on the EA app, simler90 next to StoryProgression, MonoPatcher without the VC++ x86 runtime | Reported with a link |

Other mods published outside GitHub (Smooth Patch, NRaas, simler90, and others of sims3-modernization's
community list) are recognised and listed with a link to their official page (`KnownMods.cs`), never downloaded.

The Documents folder is found the way Apex finds it (the localized name in the exe's string table, by the user locale),
in the signed-in player's profile when Setup was elevated with another administrator account.

## Build and test

```bash
dotnet run --project installer/ApexSetup.Tests
dotnet publish installer/ApexSetup -c Release -o installer/publish
```

`APEX_SETUP_DRY_RUN=1` shows what Setup finds and would change on a real game while writing nothing.

The tests run the engine on a fake `Game\Bin` and Documents folder in `%TEMP%` with local files (no network, no game).
For a manual run against a fake game: start the exe with the game folder as its argument and `APEX_SETUP_DOCUMENTS` set
to a fake Documents folder. The exe asks for administrator rights (`Game\Bin` is usually under Program Files). Errors are
logged to `%TEMP%\ApexRadianceSetup.log`.

## Not covered

- Game Edge Smoothing (MSAA): Apex asks to turn it off in game; its `Options.ini` key is unverified, so Setup leaves it.
- The AMD integrated-GPU Vulkan driver guard: Apex sets it for the game process only; a system-wide setting would
  affect other programs.
