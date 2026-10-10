# Presets

Presets save a chosen set of Apex settings so it can be applied again or shared. The sidebar page lists personal presets first, followed by the built-in Apex presets. Save, import and export use the same modal controls as the rest of the menu.

## Status

| | |
|---|---|
| Availability | Dedicated page and portable ZIP packages: 2.12.0 |
| Default | Current settings; importing does not automatically apply a preset |
| Menu | Presets, below Overview |
| Configuration | TOML files in `Apex Radiance\Profiles\`; LUTs in `Apex Radiance\LUTs\` |
| Source | `apex_config.cpp`, `apex_gui.cpp`, `profile_package.h` |

## The problem

A preset referencing a local LUT cannot reproduce its look on another computer unless that LUT is also available. Separate files make sharing harder and can leave missing dependencies.

## How Apex Radiance solves it

Export packages the selected preset and its LUT in one ZIP when Color is included and a LUT is selected. Otherwise, the preset can be shared as TOML. Import reads the original Apex ZIP directly; manual extraction is unnecessary. A conflicting LUT filename is resolved without overwriting a different existing LUT, and the imported preset references the chosen local filename.

## Settings

The modals select the source preset, name, icon and setting categories. Shortcuts are optional and start unchecked. Applying a preset changes only the selected categories. Personal preset rows display their name without a category summary; their action menu sits before Apply.

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
