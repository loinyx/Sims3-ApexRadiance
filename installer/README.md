# Apex Radiance Setup

A Windows installer (WinUI 3, one self-contained exe) for Apex Radiance and the mods it works with. It finds The Sims 3
(Steam 1.67.2 `TS3W.exe`, EA app 1.69.47 `TS3.exe`, or a folder the player picks), shows what is installed, and applies
only what differs from the player's choice.

## What it does

| Part | Source | Where |
|---|---|---|
| Apex Radiance (required) | `loinyx/Sims3-ApexRadiance` latest release | `Game\Bin\ApexRadiance.asi` |
| Restart host (experimental, off by default; Setup 1.1.5) | `ts3wrap.exe` of the same Apex release (its own asset in 3.1.3 and 3.1.4, inside the manual-install zip from 3.1.5) | `Game\Bin\ts3wrap.exe`, and `[ui] restart_host = true` in `Documents\...\Apex Radiance\ApexRadiance.toml` |
| Renderer: DXVK, RTX Remix bridge (recommended and chosen by default on a first install, DXVK when the card cannot run it; an installed renderer is kept; DXVK and the bridge only with a Vulkan 1.3 graphics card whose driver has the extensions DXVK requires, asked from `vulkan-1.dll`), or the game's own Direct3D 9 | `doitsujin/dxvk` (x32 `d3d9.dll`); the bridge zip of the Apex release | `Game\Bin` (`client.targetProcess` set to the game's exe) |
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
- The restart host's switch (`Engine.SetRestartHost`) edits one line of `ApexRadiance.toml`: the key's value in place,
  else a line added at the end of `[ui]`, else `[ui]` added at the end; every other line stays as it is. Off changes a key
  that is there to `false` and adds nothing. When the game has not created the file yet, Setup writes `[ui]` with
  `restart_host = true` and `start_profile_done = false` (Apex's welcome page still shows), but not while Apex still has
  older settings to copy (`S3SS\Apex\Apex.toml`, or Apex's tables in `S3SS.toml`), which it copies only while the file
  is missing: the switch then waits, with *Start the game once with Apex Radiance*. Its restore point keeps the key's old
  value: *Restore* puts back only that key (or deletes the file Setup created when the game never wrote to it), so
  settings the game saved since stay. Status: installed when `ts3wrap.exe` is next to the ASI and the key is `true`.
  Removing Apex Radiance removes the restart host first.
- NVIDIA Smooth Motion (Setup 1.2.0, `SmoothMotion.cs`): a switch that writes the NVIDIA driver at once, not with
  Install. NVAPI (`nvapi64.dll`, functions from `nvapi_QueryInterface`, `NVDRS_PROFILE_V1`, `NVDRS_APPLICATION_V1`,
  `NVDRS_SETTING_V1`): on finds the profile holding `NvRemixBridge.exe` (NVIDIA's predefined *NvRemixBridge*, else creates
  `Apex Radiance - The Sims 3` with it), sets `0xB0D384C0 = 1` and `0xB0CC0875 = 7` and saves; off, only when
  `smooth-motion-on.txt` records that Setup turned it on, calls `DRS_RestoreProfileDefaultSetting` (`0x53F0381E`) for both
  on NVIDIA's profile, or deletes Setup's. The game's exes are not added. On also sets `[screen] fps_limit = 60` in `ApexRadiance.toml` when there is no limit (absent or 0), through
  `Fs.WriteFpsKey`: its restore point puts back only that key (`FpsLimitKey`, `FpsLimitWas`). Off leaves the limit.
  `Engine.Remove` of the bridge or Apex (also by a renderer change and `RemoveAll`) calls `Engine.FrameGenOff`, which
  deletes Setup's profile when it is on and returns, never throws, a failure (`TakeFrameGenOffError`, shown on the row).
  Repair, Setup's update and a later run only read the driver.
- AMD Fluid Motion Frames (Setup 1.2.0, `Afmf.cs`): the same row pattern through ADLX (`amdadlx64.dll`, C interface:
  `ADLXQueryFullVersion`, `ADLXInitialize`, `IADLXSystem::Get3DSettingsServices`, `QueryInterface` to
  `IADLX3DSettingsServices1`, `GetAMDFluidMotionFrames`, then `IsSupported`, `IsEnabled`, `SetEnabled`; vtable slots from the
  ADLX 2.0 SDK headers). ADLX has AFMF as one setting for the whole system (no GPU, no application), so the row says it
  applies to every game. Shown when ADLX starts (an AMD driver); enabled where ADLX reports it supported; the bridge is not
  required. On writes `%LOCALAPPDATA%\Apex Radiance Setup\afmf-on.txt`; a removal of the bridge or Apex turns AFMF off only
  when that file is there, so a setting the player turned on stays. The frame limit rule is Smooth Motion's.
- Apex frame limit: a list beside the frame generation rows (`Engine.FpsChoices`: 0, 30, 60, 75, 90, 120, 144, 165, 240, and
  Custom clamped by `Engine.ClampFps` to 0 or 20 to 1000) that calls `Engine.SetFpsLimit`: an atomic write of
  `[screen] fps_limit`, a restore point that records the value written (`FpsLimitSet`) and undoes only while it is still there.
- Both editions: Steam 1.67.2 (`TS3W.exe`) and EA app 1.69 (`TS3.exe`) use the game's own exe and Documents folder; AFMF
  needs nothing per edition. Shown with an NVIDIA card; enabled with an RTX 40 or 50 series card, driver 571.86 or newer
  and the bridge installed (DXVK is untested with it); an exe in a profile of the player's leaves the switch off and names
  that profile. Any NVAPI failure is shown on the row, never thrown. The driver write is unverified on a real card.
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
`APEX_SETUP_FAKE_NV=<card>|<driver>|<on>|<owner profile>` stands in for NVAPI (for example
`NVIDIA GeForce RTX 4070|57186|1|`, driver 571.86 written as 57186, on as 1) and makes the Smooth Motion switch write
nothing. `APEX_SETUP_FAKE_AMD=<supported>|<on>` (for example `1|0`, or `none` for no AMD card) does the same for AFMF.

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
