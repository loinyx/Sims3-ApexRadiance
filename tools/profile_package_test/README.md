# Preset sharing checks

`extract.py` copies the production preset persistence functions into the isolated test translation unit. Build
`check.cpp`, that generated unit, `framework/apex_util.cpp` and `features/looks_preset.cpp` with the project C++20
Win32 toolchain and toml++ (include `.`, `framework`, `features`, `patches` and vcpkg's `include`).
The fixture checks stored ZIP interoperability, checksums, malformed archives, legacy TOML, LUT and look collision
handling, rollback, preservation of existing files, the Capture part, the Shaders part (bit 10) with the migration of
3.3.0 development presets whose Color part carried `[patches.Looks]`, its `packs` names, and the player's own shader files
in the ZIP (`Shaders/Shaders/`, `Shaders/Textures/`: layout, opt-in, reuse, numbered names, unsafe and orphan entries). Generated files live under the supplied test output directory.

`extract_ui.py` copies the production preset page and modals and instruments shared widgets. Build `menu.cpp` with
those generated includes, the real theme, icons, translations, ImGui, preset backend and `apex_presets.cpp`.
It exercises 840 cases across 21 languages, two widths, two scales and ten dialog states, four frames each; the imported preset needs a shader pack, so the "Download them now" box is drawn.
Native file pickers and game application are stubbed. CPU previews and layout checks do not establish gameplay,
GPU, file-picker interaction or visual parity in the game.
