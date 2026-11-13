# Apex Radiance Setup

A Windows installer (WinUI 3, one self-contained exe) for Apex Radiance and the mods it works with. It finds The Sims 3
(Steam 1.67.2 `TS3W.exe`, EA app 1.69.47 `TS3.exe`, or a folder the player picks), shows what is installed, and applies
only what differs from the player's choice.

## What it does

| Part | Source | Where |
|---|---|---|
| Apex Radiance (required) | `loinyx/Sims3-ApexRadiance` latest release | `Game\Bin\ApexRadiance.asi`, and `Game\Bin\ts3wrap.exe` (the restart host, off until `[ui] restart_host`) when the release has it |
| Renderer: DXVK (recommended), RTX Remix bridge (DXVK and the bridge only with a Vulkan 1.3 graphics card whose driver has the extensions DXVK requires, asked from `vulkan-1.dll`), or the game's own Direct3D 9 | `doitsujin/dxvk` (x32 `d3d9.dll`); the bridge zip of the Apex release | `Game\Bin` (`client.targetProcess` set to the game's exe) |
| Sims3SettingsSetter | `sims3fiend/Sims3SettingsSetter` | `Game\Bin` |
| Sims 3 Modernization | `rodneypantonial/sims3-modernization` (Steam or EA zip by edition; when the latest release lacks the edition's zip, the newest of the last 10 that has it, with its own `ts3fixes.package`, and no update is offered to a release without it) | `ts3native` in `Game\Bin`, `ts3fixes.package` in `Mods\Packages` |
| MonoPatcher (no longer offered since Setup 1.1.3: no mod Setup installs needs it; an installed copy can still be kept, updated or removed) | `LazyDuchess/MonoPatcher` (its bundled loader is left out) | `MonoPatcher.asi` in `Game\Bin`, its package in `Mods\Packages\MonoPatcher` |
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
- Before every change (install, update, repair, removal, a compatibility fix) Setup opens a restore point in
  `Game\Bin\Apex Radiance backup\restore\<yyyyMMdd-HHmmss>`: a copy of each file as it was before Setup first touched
  it, in `Game\Bin` and in Documents (`Options.ini`, `Mods\Resource.cfg`, `S3SS.toml`, the `.apex-backup` copies,
  `setup.json`), and the files and folders it created, listed in `point.json`. Every write of `ApexSetup.Core` goes
  through `Fs` (`RestorePoints.cs`), so nothing is left out. An action that changes nothing keeps no point; the newest
  5 are kept. Repair or remove lists them; *Restore* puts the files back and undoes every newer point too (the game
  must be closed; restoring is final, and a file that cannot be put back is named and its point kept to try again).
  When an install or a removal fails, its error offers *Undo these changes*.
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
| The bridge's `dxvk.conf` without the TS3 profile, `d3d9.countLosableResources = False` before its first `[exe]` section | Wrong values set, missing lines added at the top |
| `.package` files in `Mods\Packages` (up to 3 folders deep) without `Resource.cfg` | `Resource.cfg` created |
| A `Resource.cfg` that does not read `ts3fixes.package` or MonoPatcher's package | The `Packages` folder lines added, copy as `Resource.cfg.apex-backup` |
| Smooth Patch's `TS3Patch.asi` next to S3SS (same game code) | Moved to the backup folder |
| `ts3native` built for the other edition | Reported; turning Sims 3 Modernization on installs the right one |
| A Steam build of simler90 or XCAS on the EA app (or the reverse), NRaas ErrorTrap on the EA app, simler90 next to StoryProgression, MonoPatcher without the VC++ x86 runtime | Reported with a link |

## Suggestions

What Apex Radiance recommends in game (Overview, Compatibility and Attention pages), listed as *Suggested for Apex
Radiance* on the Components step and on the game's status, in Apex's own words where they fit. Unlike the fixes above,
none runs with Install or Repair: each has its own *Apply* (and *Apply all suggestions*), the game must be closed, and
each apply takes a restore point ("Apply suggestion: ..."). A value Setup cannot read is reported and its file left alone.

| Suggestion | When | Apply |
|---|---|---|
| Turn off the game's Edge Smoothing | `Options.ini` `[Options] edgesmoothing` not 0, and one of Apex's Edge Smoothing, Depth Blur, Ambient Occlusion or Water Reflections on in `Apex Radiance\ApexRadiance.toml` (all on by default) | `edgesmoothing = 0`, copy as `Options.ini.apex-backup` |
| Two copies of the mod | Another `.asi` in `Game\Bin`, `scripts` or `plugins` that is a renamed `ApexRadiance.asi` (its "Another copy of ApexRadiance.asi" line), while `ApexRadiance.asi` is in `Game\Bin` | Moved to the backup folder |
| DXVK | The game's own Direct3D 9, or a `d3d9.dll` that is neither DXVK nor the bridge | None: chosen in Components |
| Sims3SettingsSetter | Not installed (and no old combined build) | None: turned on in Components |
| RTX Remix bridge | DXVK in the game process | None: chosen in Components |

The rule for the game's Edge Smoothing reads `Options.ini`, where Apex reads the back buffer: `edgesmoothing = 0` was
seen with no MSAA; that every other value means MSAA is unverified in game.

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
`APEX_SETUP_FAKE_GPU=<version>|<card>|<missing extension>,...` stands in for the Vulkan driver's answer (for example
`1.3|Intel(R) Iris(R) Xe Graphics|VK_KHR_load_store_op_none`), to see the screens of an old driver or an old card.

The tests run the engine on a fake `Game\Bin` and Documents folder in `%TEMP%` with local files (no network, no game).
For a manual run against a fake game: start the exe with the game folder as its argument and `APEX_SETUP_DOCUMENTS` set
to a fake Documents folder. The exe asks for administrator rights (`Game\Bin` is usually under Program Files). Errors are
logged to `%TEMP%\ApexRadianceSetup.log`.

## Not covered

- S3SS features to turn off: Apex asks only for the uncompressed textures under the bridge (above); every other S3SS
  feature it shares (Split-Level Lighting Fix, Lot Streaming, the frame-rate limit) it leaves to S3SS, so Setup changes
  none of them.
- The AMD integrated-GPU Vulkan driver guard: Apex sets it for the game process only; a system-wide setting would
  affect other programs.
