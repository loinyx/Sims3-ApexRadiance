# Preset sharing checks

`extract.py` copies the production preset persistence functions into the isolated test translation unit. Build
`check.cpp`, that generated unit and `framework/apex_util.cpp` with the project C++20 Win32 toolchain and toml++.
The fixture checks stored ZIP interoperability, checksums, malformed archives, legacy TOML, LUT collision handling,
rollback and preservation of existing files. Generated files live under the supplied test output directory.

`extract_ui.py` copies the production preset page and modals and instruments shared widgets. Build `menu.cpp` with
those generated includes, the real theme, icons, translations, ImGui, preset backend and `apex_presets.cpp`.
It exercises 588 cases across 21 languages, two widths, two scales and seven dialog states, four frames each.
Native file pickers and game application are stubbed. CPU previews and layout checks do not establish gameplay,
GPU, file-picker interaction or visual parity in the game.
