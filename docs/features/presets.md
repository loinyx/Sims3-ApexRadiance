# Presets

Presets save a chosen set of Apex settings so it can be applied again or shared. The sidebar page lists personal presets first, followed by the built-in Apex presets: Performance, Balanced (a balance of looks and speed; not the code defaults, which each row's Reset restores) and Quality. The Performance part carries every switch of the Performance page and the lamp lighting mode. Save, import and export use the same modal controls as the rest of the menu.

## Status

| | |
|---|---|
| Availability | Dedicated page and portable ZIP packages: 2.12.0; shortcuts: Released in 2.12.2 |
| Default | Current settings; importing does not automatically apply a preset |
| Menu | Presets, below Overview |
| Configuration | TOML files in `Apex Radiance\Profiles\`; LUTs in `Apex Radiance\LUTs\`; shortcuts in `[ui.profile_shortcuts]` |
| Source | `apex_config.cpp`, `apex_gui.cpp`, `hotkeys.cpp`, `key_combo.h`, `profile_package.h` |

## The problem

A preset referencing a local LUT cannot reproduce its look on another computer unless that LUT is also available. Separate files make sharing harder and can leave missing dependencies.

## How Apex Radiance solves it

Export packages the selected preset and its LUT in one ZIP when Color is included and a LUT is selected. Otherwise, the preset can be shared as TOML. Import reads the original Apex ZIP directly; manual extraction is unnecessary. A conflicting LUT filename is resolved without overwriting a different existing LUT, and the imported preset references the chosen local filename.

## Settings

The modals select the source preset, name, icon and setting categories. Shortcuts are optional and start unchecked. Applying a preset changes only the selected categories. Personal preset rows display their name without a category summary; their action menu sits before Apply.

## Shortcuts

Each preset, personal or built-in, can have its own key combination. Right-clicking the row, or its ellipsis button,
offers Assign/Change and Remove shortcut; a tag left of the ellipsis shows the keys, shortened at a `+` with the full
combination on hover. The editor shows the keys as violet chips while they are held and records keys held together, such as Ctrl+A+S or F6, with optional
Ctrl/Shift/Alt. Letters, digits, Space and punctuation need Ctrl or Alt, because without them they also type into the
game's Sim, lot and save name fields; such combinations are refused in the editor and ignored when loaded. Save commits; Cancel, the close icon or Esc keep the previous keys. Keys used by another preset, the
menu, search, peek, hold-to-compare, an Apex action or the game's F10 are refused. None are assigned by default.

Pressing the keys applies the preset's parts, the same as Apply with every category it has, except Shortcuts and
Developer. It does not toggle: pressing again applies the same preset, and another preset's keys switch to it. The
usual Undo toast follows; with the menu closed, a pill at the top center shows the preset's icon and name, like the start note. Key-down events are
queued for the render thread, repeats do not apply again, and Apex text fields and the cheat console keep their
keys. The first key of a multiple-key combination may reach the game before the combination is
recognised.

The keys belong to the player, not the preset: they are saved in `ApexRadiance.toml` under `[ui.profile_shortcuts]`
(the preset's file name, or `#builtin<n>` for Apex presets, mapped to its Windows virtual-key codes) and never in
preset files, so exporting a preset does not share them. Deleting a preset removes its shortcut; Reset all clears
them with the other menu preferences, and its Undo restores them.

## Compatibility and interactions

Existing TOML presets, category bits and the `Profiles` and `LUTs` folders remain supported. Keeping the disk folder named `Profiles` avoids moving or losing older presets; visible menu labels use Presets. Public builds hide and ignore Development, including when the imported file contains it. Developer builds retain the explicit activation confirmation.

## Limitations

Import accepts TOML or the stored ZIP format exported by Apex, not arbitrary compressed archives. File and LUT size limits and archive validation reject malformed packages. Export fails if the selected LUT cannot be read. The native save dialog confirms the final filename; Apex does not silently append another extension afterwards.

## Technical reference

Packages contain `profile.toml` and at most one `LUTs/<filename>` entry. Import checks entry names, sizes, duplicate entries and CRC before writing. Preset writes use exclusive temporary files followed by replacement. Imported LUT creation is exclusive, and a failed import rolls back its newly created LUT.

## Rejected approaches

- Multiple LUT stages and separate dependency-sharing prompts are outside this flow; see [history](../history/2026-10-08-presets-sharing.md).

## See also

- [Validation](../validation/presets.md)
- [History](../history/2026-10-08-presets-sharing.md)
- [LUTs](picture-filters.md#luts)
