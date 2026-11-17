# dxvk

DXVK (zlib, https://github.com/doitsujin/dxvk) at v3.1.1 with our patches, built as the x64 `d3d9.dll` that the RTX
Remix bridge server renders through (`.trex\d3d9vk_x64.dll`, see `../remix-bridge`).

- `build.cmd` fetches the pinned commit with its submodules into `src\`, applies `patches\` in order and builds
  `out\d3d9.dll` (release, MSVC, static C runtime, Direct3D 9 only). Needs git, VS 2022 C++ build tools (x64) and
  meson + ninja on PATH. It fetches glslang 16.6.0 (DXVK's shader compiler, checked by SHA-256) into `tools\` once.
  A clean build takes about 6 minutes, a rebuild after a patch change about 3.
- `../remix-bridge/package.ps1` packs `out\d3d9.dll` when it exists, otherwise the stock DXVK 3.1.1 release (checked by
  SHA-256).
- `test\dxvk_smoke.cpp` loads a given x64 `d3d9.dll`, creates a device on a hidden window, presents one cleared frame
  and prints the adapter, a CRC32 of the device caps and the results of each call. DXVK writes its own log next to it.

DXVK's releases are built with MinGW gcc, this build with MSVC. With no patch, the built library and the 3.1.1 release
print the same smoke lines on an RTX 3070 laptop GPU (caps CRC32 `98a84800`), export the same functions, and write the
same 341-line log apart from the `Build:` line. A patched tree reports itself as `v3.1.1+` (DXVK's own mark for a
modified source, as the zlib licence asks).

Patches: none yet; `patches\*.patch` are applied in name order.

To move to a newer DXVK: change `TAG` and `COMMIT` in `build.cmd`, delete `src\`, and run it. Refresh any patch that
no longer applies with `git diff`, from a checkout of the new commit that has the earlier patches applied.
