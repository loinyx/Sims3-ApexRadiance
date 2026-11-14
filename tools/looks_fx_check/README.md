# Looks .fx checks

Offline checks of the players' own ReShade FX loading in Looks (`features/looks_fx.cpp`,
`features/looks_fx_catalog.cpp`). Run from the repository root in an x86 VS developer prompt:

```bat
cl /nologo /c /std:c++20 /Zc:char8_t- /D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING /EHsc /utf-8 /O2 third_party\reshadefx\*.cpp /Fo:%TEMP%\
cl /nologo /std:c++20 /EHsc /utf-8 /O2 /DNOMINMAX /I. /Iframework /Ifeatures tools\looks_fx_check\check.cpp features\looks_fx.cpp features\looks_fx_catalog.cpp features\looks_runner.cpp features\looks_preset.cpp framework\apex_util.cpp %TEMP%\effect_*.obj /Fe:%TEMP%\looks_fx_check.exe /Fo:%TEMP%\
%TEMP%\looks_fx_check.exe <Shaders folder> [--run] [WxH] [--catalog <Apex Radiance folder>]
```

`<Shaders folder>` is laid out like `Apex Radiance\Shaders\`: `.fx` files in it or in `Shaders\` (and subfolders),
textures in `Textures\`. For every `.fx` the real compile path runs (ReShade's preprocessor and parser, its shader model
3.0 HLSL generator, D3DCompile `vs_3_0` / `ps_3_0` with the Direct3D 9 fallbacks), at 1920x1080 or `WxH`, and one
table row per technique says `OK` (with the fallbacks used) or why it cannot run on Direct3D 9; a summary counts the
reasons. The compile time and texture memory of each file go to stderr.

- `--run` also creates each runnable effect on a real Direct3D 9 device (a hidden window) and runs every runnable
  technique once on a test gradient (default values, no depth); a failure to create or run is a problem.
- `--catalog <folder>` runs the background scan of `features/looks_fx_catalog.cpp` on `<folder>\Shaders\` and checks a
  preset that names a user technique (its `.ini` value reaches the parameter), a built-in one that is also a user file
  (the built-in port wins), a missing file and a file that cannot run; with `--run` the user technique also runs
  through the catalog and another picture size asks for a new compile.
- Always: the licence text shown in Settings > About (`third_party/reshadefx/reshadefx_license.h`) is
  `third_party/reshadefx/LICENSE.md`.

The output ends with `OK` or `PROBLEMS`. Nothing is written. These checks do not show how an effect looks in the game.
