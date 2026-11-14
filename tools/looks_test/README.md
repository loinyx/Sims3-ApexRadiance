# Looks checks

Offline checks of the built-in Apex looks and the in-game licence texts. Run from the repository root in an x86 VS
developer prompt:

```bat
cl /nologo /std:c++20 /EHsc /utf-8 /I. /Iframework /Ifeatures tools\looks_test\check.cpp features\looks_preset.cpp framework\apex_util.cpp /Fe:%TEMP%\looks_test.exe /Fo:%TEMP%\
%TEMP%\looks_test.exe
```

Each built-in look (`kBuiltInLooks` in `features/looks_preset.cpp`) is parsed with the real loader. Every enabled
technique must be a ported shader of `third_party/reshade_shaders/reshade_shaders_hlsl.h`, every `[File.fx]` section
must belong to an enabled technique, and every value must be a uniform of that shader with its number of components and
inside its `ui_min` / `ui_max`. Each licence text shown in Settings > About (`reshade_licenses.h`) must appear in
`LICENSES.md`. The output ends with `OK` or `PROBLEMS`. Nothing is written.

These checks do not show how a look renders in the game.
