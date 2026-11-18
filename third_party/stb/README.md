# stb_image_resize2

`stb_image_resize2.h` v2.17 by Sean Barrett and Jeff Roberts (nothings/stb, commit
[f1c79c0](https://github.com/nothings/stb/blob/f1c79c02822848a9bed4315b12c8c8f3761e1296/stb_image_resize2.h), the one
ReShade 6.7.3 pins as `deps/stb`), unchanged. Public domain, or MIT, at the user's choice (the end of the file).

Looks resizes a player's effect texture image to its texture's declared size with it (`features/looks_fx.cpp`
`DecodeImage`), with ReShade's own call (`stbir_resize`, `STBIR_RGBA`, `STBIR_TYPE_UINT8`, `STBIR_EDGE_CLAMP`,
`STBIR_FILTER_DEFAULT`), so the pixels match ReShade's.
