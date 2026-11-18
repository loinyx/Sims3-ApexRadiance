# Looks checks

Offline checks of the preset loader and the in-game licence texts. Run from the repository root in an x86 VS
developer prompt:

```bat
cl /nologo /std:c++20 /EHsc /utf-8 /I. /Iframework /Ifeatures tools\looks_test\check.cpp features\looks_preset.cpp framework\apex_util.cpp /Fe:%TEMP%\looks_test.exe /Fo:%TEMP%\
%TEMP%\looks_test.exe
```

An old `apex:` reference (a built-in look of earlier builds) is no source at all. Each licence text shown in Settings > About (`reshade_licenses.h`) must appear in
`LICENSES.md`. Every value kind (float, int, bool, combo, float3 and float4 colours, vectors, a hidden value) is written
as ReShade writes it (`FormatValue`) and read back the same (`PresetValues`, bools also as `true` / `false`), with the
menu control each kind gets; `PreprocessorDefinitions=` is parsed (bounded, names checked), written back the same, and
an effect section's definitions follow the global ones. The output ends with `OK` or `PROBLEMS`. Nothing is written.

These checks do not show how a look renders in the game.
