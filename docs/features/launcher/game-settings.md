# Apex Launcher: game settings

Game settings changes graphics and speed options that the game keeps in its own files and shows nowhere in its menus
(shadow resolution, Sim texture size, bloom, files read at once, the space between walking Sims), plus four in-game
options before the game starts (the shop, interactive loading screens, memories, online features). It checks that the
game knows the graphics card and that the Windows user folder and OneDrive will not get in the way. Only a changed value
is written, each file is copied before its first change, and *Restore my files* puts them back.

## Status

| | |
|---|---|
| Availability | Released in 4.1.1 |
| Default | Every option at the game's value; nothing is written until the player applies a change |
| Menu | Game settings; also a step of the first install (*Optional. You can change these later in the launcher.*) |
| Configuration | The game's files below; `settings.json` `PendingOptions` while `Options.ini` does not exist yet |
| Source | [`MainWindow.GameSettings.cs`](../../../installer/ApexSetup/MainWindow.GameSettings.cs), [`GameSettings.cs`](../../../installer/ApexSetup.Core/GameSettings.cs), [`DeviceCard.cs`](../../../installer/ApexSetup.Core/DeviceCard.cs), [`SetterSettings.cs`](../../../installer/ApexSetup.Core/SetterSettings.cs) |

## The problem

The game caps shadow and Sim texture sizes, the memory it reads files with and its walking values in text files under
`Game\Bin`, and keeps several options in `Options.ini`. Editing them by hand means finding the right line among
hundreds, and the game rewrites `Options.ini` with low settings when it does not recognise the graphics card.

## How Apex Radiance solves it

1. **Read.** `GameSettings` reads the current value of each option from the game's files and marks options the file
   does not have as *Not found in this game's files*.
2. **Change.** A changed value is a pending change; *Apply* writes only those values, in the files' own Latin-1
   encoding, leaving every other byte as it was. Writes to `Game\Bin` go through the [administrator copy](elevation.md)
   when the folder needs it, in a restore point named *Game settings*.
3. **Copies.** Before the first change, each file is copied: `Game\Bin\Apex Radiance backup\game settings\` for the
   `Game\Bin` files and `Documents\...\Apex Radiance\game settings\` for `Options.ini`.
4. **Restore my files.** A `Game\Bin` file unchanged since the launcher wrote it goes back whole; otherwise only the
   launcher's values are put back. `Options.ini` gets only the launcher's keys back. The [uninstall](uninstall.md) does
   the same.
5. **Graphics card** (`DeviceCard`). The card the game logged in `DeviceConfig.log` is looked up in
   `Game\Bin\GraphicsCards.sgr`; *Fix* adds a `card 0x....` line under its maker. When the driver reports less than
   512 MB of video memory, the texture-memory fallback in `GraphicsRules.sgr` is raised to 1024 MB. This has its own
   restore point, *Graphics card*.

## Settings

| Menu label | File and key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Shadow resolution | `GraphicsRules.sgr`, `LightingQuality $High` `ShadowMapSize` | choice | 2048 | 2048, 4096, 8192 | Sharper shadow edges; needs the game's lighting quality at its highest |
| Sim texture size | `GraphicsRules.sgr`, `SimQuality $VeryHigh` `RenderSimTextureSizes` | choice | 2048 | 2048, 4096 | More detail up close; needs the game's Sim detail at its highest |
| Turn off bloom | `GraphicsRules.sgr`, `AdvancedRendering $On` `RenderPostProcessEnabled false` | switch | Off | | Removes bloom, and can remove other screen effects |
| Turn off Shop mode | `Options.ini` `enableingamestore` = 0 | switch | Game's value | | The store inside the game |
| Turn off interactive loading screens | `Options.ini` `enableinteractiveloading` = 0 | switch | Game's value | | The small game played while a save loads |
| Turn off memories | `Options.ini` `enablememories` = 3 | switch | Game's value | | Sims stop recording memories |
| Turn off online and social features | `Options.ini` `receiveconnecttns`, `requireloginbeforeload` | switch | Game's value | | Stops Connect notifications and the sign-in prompt |
| Files read at once | `Sims3.ini` `[ResourceSystem] MemoryUsageLimit` | choice | 20 MB | 20, 40 MB | With 40 MB lots may load sooner (not measured) |
| Space between walking Sims | `Sims3.ini` `[Config] DynamicAvoidance_*` | choice | Default | Default, Half, Smallest | Sims pass closer to each other; *Show* lists all seven values (two for strollers), 0 to 100 each |

*Turn all off* sets the four in-game options off. *Reset all to default* sets every option to the game's default.
While the game has not made `Options.ini` yet, the in-game choices wait in `settings.json` and are applied around the
first Play.

**Checks** (read only): *Windows user folder* (characters the game may not read) and *OneDrive* (syncing the game's
Documents folder, which can damage saves).

**Recommended settings** for Sims3SettingsSetter: when it is installed, a review dialog lists what turns on and off in
its settings file, optionally with its Smooth Patch at the screen's refresh rate; its other settings stay. When it is
not installed, the row points to Components.

## Compatibility and interactions

- Close the game before applying (*Close the game before you apply*); the game reads these files at start.
- Apex Radiance's own settings are not here; they are in its in-game menu.

## Limitations

- The values written by the launcher are not yet verified in game
  ([validation](../../validation/installer.md#open-checks)).
- *Files read at once* at 40 MB has not been measured.

## Technical reference

- `GameSettings.RestoreBin` and `RestoreDocs` keep a hash of what the launcher last wrote (`.last`) to tell whether a
  file changed since.
- The first install offers the same rows (`WizardSettings`) and applies them after the components.

## Rejected approaches

None recorded.

## See also

- [Health](health.md), [Components, install and update](../installer.md)
- [Validation](../../validation/installer.md)
