# G-buffer check

`run.ps1 -Precomp <Game\Bin\Shaders_Win32.precomp> [-D3d9 <a DXVK d3d9.dll>]` builds `check.cpp` with the x86 MSVC
toolchain and runs it. Read-only: it prints to stdout and writes only the compiler outputs to `-OutDir` (default under
`%TEMP%`).

It checks the G-buffer shader copies (`ShaderPatches::AddGBufferPs`, `shaders/gbuffer_ids.h`;
[work package](../../docs/features/realtime-gi/wp/GB-02.md)):

- every unique `ps_3_0` and `ps_2_0` of the package through `AddDither` / `AddDither2` then `AddGBufferPs`, with its
  table row or the unknown values; each copy created on a native D3D9 HAL device; per version and per technique family
  (16 shaders or more; the family list comes from `generate.cjs`, written to `-OutDir`): accepted, created, albedo and
  normal reads found, refusals by reason. Accepted at least 99 % of `ps_3_0` and 95 % of `ps_2_0`;
- `AddGBufferPs` alone only inserts: with its own instructions taken out, the shader is the game's token for token (a
  `ps_2_0` becomes `ps_2_x`);
- on three 64 x 64 render targets, a synthetic `ps_3_0` and `ps_2_0` through the patch: target 0 bit for bit the unpatched
  shader's, target 1 the texture colour with the material id, target 2 decoding to the normal for 26 directions with
  the roughness and the motion class; without samplers, the unknown values;
- with `-D3d9`, every check again on that D3D9 implementation, loaded from the given path.

The table is made by `node tools/gbuffer_ids/generate.cjs <precomp> shaders/gbuffer_ids.h [families-file]`.
