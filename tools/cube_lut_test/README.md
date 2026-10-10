# 3D CUBE LUT checks

Compile `check.cpp` with MSVC C++20 and link `d3dcompiler.lib`, `d3d9.lib` and `user32.lib`.
Run from the repository root. An optional first argument names a Resolve 33-cell `.cube` fixture.
No game files are read or modified. The test creates a hidden native D3D9 HAL device.

Checks cover parser boundaries, malformed tables, finite RGB values, domains, ordering,
float preservation, production PicturePS/AdaptPS compilation and device acceptance.
The production cube helper is extracted from picture.cpp and executed on a float32
texture. Pixel readbacks are checked against analytic identity and asymmetric transforms
at sizes 2, 17, 33 and 65, including a custom input domain.

These checks do not establish gameplay, UI layout, DXVK/Proton compatibility, or the
suitability of a camera/log LUT for the game's gamma-encoded SDR picture.
