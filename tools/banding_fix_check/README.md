# Banding Fix check

`run.ps1 -Precomp <Game\Bin\Shaders_Win32.precomp> [-D3d9 <a DXVK d3d9.dll>]` builds `check.cpp` with the x86 MSVC
toolchain and runs it. Read-only: it prints to stdout and writes only the compiler outputs to `-OutDir` (default under
`%TEMP%`).

It checks the shader copies of the [Banding Fix](../../docs/features/banding-fix.md):

- every unique `ps_3_0` of the package through `AddDither`, every `ps_2_0` through `AddDither2` and every `vs_1_1` /
  `vs_2_x` through `AddScreenPosVs` (highest free texture coordinate); each copy is created on a native D3D9 HAL device;
- on a hidden 64 x 64 target, a flat colour through a patched synthetic `ps_3_0`: the mean of each grey is kept within
  0.06 of a level (0.1 within half a level of black or white), pure black and pure white come out unchanged, alpha is
  unchanged and Strength 0 adds nothing;
- a patched `vs_2_0` + `ps_2_0` pair gives the same pixels as the `ps_3_0` copy (the two grains line up);
- the Picture dither: `PicturePS` from `features/picture.cpp` with every control neutral; an unchanged picture stays
  bit for bit;
- how many of the package's pixel shaders the glow sprite family matches (`SpriteSmoothLogic::IsTintedSprite`); at least
  one ps_2_x must;
- with `-D3d9`, every check again on that D3D9 implementation, loaded from the given path.
