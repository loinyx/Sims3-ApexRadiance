# Lamp index check

`run.ps1 [-Precomp <Game\Bin\Shaders_Win32.precomp>]` builds `check.cpp` with the x86 MSVC toolchain and runs it.
Read-only: it prints to stdout and writes only the compiler outputs to `-OutDir` (default under `%TEMP%`). With the
precomp the run takes about four minutes.

It checks the lamp index cells of real-time GI ([S3-02](../../docs/features/realtime-gi/wp/S3-02.md),
[S3-02b](../../docs/features/realtime-gi/wp/S3-02b.md): `features/lamp_index.h`, `ShaderPatches::PatchObjectLampPs`,
`PatchInstancedLamps` and `PatchWindowOutdoor` with `index`):

1. **Transport on the CPU.** A model of the per-draw fix of outdoor rig-lit objects (the pick at the object, 8 lamps,
   the shader's sum with cones and visibility, the ground share and both height gates) against the same pixel lit
   from the index texture that `LampIndex::Build` fills with that pick. Four arms must show 0 pixels over 1/255, under
   the first law and the realistic reach law: open ground with objects anywhere, and walls with objects at cell
   centres (loose, set in a wall, gate in a fence gap).
   Three arms are reported only, because the cells decide per pixel what the fix decides per object: walls with
   objects anywhere, pixels up to 3 m over the ground, and a dense lamp cluster.
2. **Slices.** A rebuild of only the cells that changed lamps can reach, a few cells at a time, and a moved window
   must each equal a whole new build cell for cell. The CPU time of a whole build with the model pick is printed.
3. **The HLSL reference** of the block, `lamp_index.hlsl`: compiled as `ps_3_0` with the compiler `fxc` uses, priced
   (slots, temporaries), accepted by native D3D9, and its pixels on a hidden device against the CPU model (2 LSB).
4. **The game's shaders** (with `-Precomp`): every pixel shader the per-draw patches take (objects, fences and stairs
   with and without the floor-tile shadows, window frames; each under both laws) is patched with the index block.
   A refused object shader is printed with its price. Slots, the host's temporaries + 16 and the copy's own temporaries are printed; native D3D9 must accept every
   copy; where the fixture can feed the host's inputs, the index copy must draw the per-draw copy's pixels within
   1 LSB with the same lamps, on open ground and with walls and flags.

The last line is `OK` or `PROBLEMS`. Without a D3D9 device the device checks are skipped. These checks do not replace
an in-game run.
