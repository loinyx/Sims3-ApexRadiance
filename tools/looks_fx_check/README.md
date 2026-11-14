# Looks .fx checks

Offline checks of the players' own ReShade FX loading in Looks (`features/looks_fx.cpp`,
`features/looks_fx_catalog.cpp`). Run from the repository root in an x86 VS developer prompt:

```bat
cl /nologo /c /std:c++20 /Zc:char8_t- /D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING /EHsc /utf-8 /O2 third_party\reshadefx\*.cpp /Fo:%TEMP%\
cl /nologo /std:c++20 /EHsc /utf-8 /O2 /DNOMINMAX /I. /Iframework /Ifeatures tools\looks_fx_check\check.cpp features\looks_fx.cpp features\looks_fx_catalog.cpp features\looks_runner.cpp features\looks_preset.cpp framework\apex_util.cpp %TEMP%\effect_*.obj /Fe:%TEMP%\looks_fx_check.exe /Fo:%TEMP%\
%TEMP%\looks_fx_check.exe <Shaders folder> [--run] [WxH] [--catalog <Apex Radiance folder>] [--compare] [--hashes] [--state] [--cache]
%TEMP%\looks_fx_check.exe <any folder> --packs <Apex Radiance folder>
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
- `--compare` (with `[--image <file>]...`): `<Shaders folder>` holds the authors' original `.fx` of the 17 built-in
  ports (and `ReShade.fxh`, `ReShadeUI.fxh`); each port and its original run on the same Direct3D 9 device on a
  gradient, per-pixel noise and the given images, with the defaults, every parameter at a quarter and at three quarters
  of its range, and the two in turn. One table row per run gives the largest and the mean channel difference in 8-bit
  levels; a port more than 1 level from its original is a problem.
- `--packs <Apex Radiance folder>`: the scan of `Shaders\Packs\<pack>\`: no pack file compiles before it is wanted;
  browsing checks the files the check cache does not know in the background and keeps none of them compiled, then every
  pack is counted ("<x> of <y> run here") and its techniques listed; a wanted file compiles; `FindFiles` names the pack
  of a file. Prints the process's private bytes and largest free block after the scan and after browsing, and the
  compile cache's hits. Writes `Shaders\.cache\` in that folder: give it a scratch copy (a second run reads the caches).
- `--state` (with a Direct3D 9 device): runs the 17 ports, then techniques of the folder's .fx after them, from a
  game-like device state, and compares every device state before and after with what `patches/looks_patch.cpp` puts back
  by hand (`Looks::kSavedRenderStates`, `PsConstsUsed`): a state changed and not put back is a problem. Also times a
  `D3DSBT_ALL` state block's Capture and Apply and the chain that Strength 0 skips.
- `--cache`: every .fx compiled without the compile cache, then with it twice (`<Shaders folder>\.cache-check\`, written):
  the bytecode, notes and reasons must be the same; a test effect is compiled again after its `.fxh`, its `.fx` and the
  picture size change, and read from the cache otherwise. Prints the times.
- `--hashes` (with `--compare` or `--run`): one line per picture with a hash of its pixels. Built against the sources
  before a change (`/DLOOKS_FX_CHECK_BASELINE`, which leaves out the checks of newer parts), the two builds' hash lines
  are the same when the pictures are the same to the level.
- Always: the licence text shown in Settings > About (`third_party/reshadefx/reshadefx_license.h`) is
  `third_party/reshadefx/LICENSE.md`.

The output ends with `OK` or `PROBLEMS`. Nothing is written, except by `--packs`, `--catalog` (the compile and check
caches in `Shaders\.cache\` of the given folder) and `--cache`. These checks do not show how an effect looks in the game.
