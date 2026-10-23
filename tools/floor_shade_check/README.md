# Floor ground lamp shade check

`run.ps1 -Precomp <Game\Bin\Shaders_Win32.precomp> [-Captures <F7 capture folders>]` builds `check.cpp` with the x86
MSVC toolchain and runs it. Read-only: it prints to stdout and writes only the compiler outputs to `-OutDir` (default
under `%TEMP%`).

It checks the ground lamp shade of story-0 outdoor floors ([floors.md](../../docs/features/night-lighting/floors.md)):

- every `floor_atlas_table.h` pixel shader, with the TEXCOORD of each paired vertex shader's copy, takes
  `PatchBakedAtlasPs` with and without the shade; the shaded copy is the copy without it plus one sampler declaration
  and the shade block after the atlas `max`; `D3DDisassemble` within 512 slots and native D3D9 accept both;
- the winter floor tiles and snow-on-floor shaders: which vertex shaders write world y (`WorldYOutput`), and their pixel
  shaders through `PatchFloor` / `PatchSnowFloor` with and without it (the shade never changes which shaders are taken);
- `GroundShadow::FloorShadeRows` against `WorldTexel` on a straight and a rotated lot, the height gate and the off switch;
- on a hidden D3D9 device, a synthetic floor-shaped shader patched with the shade over a known mask: every pixel equals
  `map x (1 + gate (M - 1))` within 2 LSB at seven heights, with the shade off and with the white mask;
- with `-Captures`, each captured summer floor copy (`PS_*.bin`) is found again as a table shader's patch;
- fences, curbs, railings and stairs ([fences.md](../../docs/features/night-lighting/fences.md)): every pixel shader
  the precomp pairs with an instanced structure vertex shader, both laws, through `PatchInstancedLamps` with and without
  the shade, with the same checks as the summer floors (the block follows the lamp `max`); on a hidden D3D9 device a
  synthetic fence shader with the shade, ground light alone and one lamp alone, equals the sum x `1 + gate (M - 1)`
  within 2 LSB at four heights; with `-Captures` (searched recursively, for example the Light Probe `Captures` and
  `ShadersRecusados` folders) every distinct `ps_3_0` `.bin` goes through the same checks, and each captured fence copy is
  found again as a precomp shader's copy.

- the indoor light on the ground (2.15.3): every summer pair and winter shader also patched with `exitLight`, which is
  either the shaded copy byte for byte (no room) or a copy within 512 slots that native D3D9 accepts, with the same
  registers; on a hidden D3D9 device the synthetic floor shader patched with it, over a map whose rgb and alpha vary,
  equals `max(L - ex, atlas tw) (1 + g (M - 1)) + ex` within 2 LSB at seven heights, with the shade, with the gate over
  the white mask, off, and with both atlas weight constants;
- the snowy, rug-cut and melting-snow lot light passes (`PatchLotTerrainMax`, [snow.md](../../docs/features/night-lighting/snow.md)):
  the precomp's three shaders with the room mask, and with the mask and the indoor light on the ground; both copies
  disassemble within 512 slots and native D3D9 accepts them; on a hidden D3D9 device the second copy draws the first one's
  pixels exactly with its gate at 0, and over a lot map of alpha 0 with the gate at 1, and changes them with the gate at 1
  over a lot map whose alpha varies.

The last line is `OK` or `PROBLEMS`. These checks do not replace an in-game capture.
