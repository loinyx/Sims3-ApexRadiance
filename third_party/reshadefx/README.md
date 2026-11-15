# ReShade FX compiler

ReShade's effect compiler by crosire (Patrick Mours), from [crosire/reshade](https://github.com/crosire/reshade) tag
`v6.7.3` (`source/`), BSD 3-Clause ([LICENSE.md](LICENSE.md)). Looks uses it to compile the players' own .fx files for
Direct3D 9 (`features/looks_fx.cpp`).

Only the lexer, preprocessor, parser, symbol table, expressions, the code generator interface and its HLSL back-end are
here. Changes from the tag:

- `effect_preprocessor.hpp` / `.cpp`: `include_filter` (a check of every file an `#include` resolves to before it is
  read) and the `max_output_size` / `max_tokens` caps, marked "Apex Radiance" in the code.

The files build as C++17 does with `/Zc:char8_t-` and `_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING` (set per file in
`ApexRadiance.vcxproj`). `reshadefx_license.h` is the licence text for Settings > About. Details:
[docs/history/looks-fx-loading.md](../../docs/history/looks-fx-loading.md).
