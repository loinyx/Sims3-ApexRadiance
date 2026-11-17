# Ambient Occlusion options check

`run.ps1 [-OutDir <folder>]` builds `check.cpp` with the x86 MSVC toolchain and runs it. Read-only: it prints to stdout
and writes only the compiler outputs to `-OutDir` (default under `%TEMP%`). It needs no game files.

It checks the two Ambient Occlusion options *Fewer directions when far* (`zoomSlices`) and *Smooth flat surfaces*
(`blendNormals`) of [ambient-occlusion.md](../../docs/features/ambient-occlusion.md):

- every GtaoPS variant (5 qualities x the 4 option combinations), compiled from the HLSL in
  `patches/ambient_occlusion_patch.cpp`, stays within ps_3_0's 512 instruction slots and is accepted by a native D3D9
  HAL device;
- a synthetic depth buffer (floor, a wall, a counter whose top has a 0.03 degree crease, house-sized boxes; 24-bit
  device depth, near 0.25 m) seen from 40, 80 and 150 m goes through the real GtaoPS and the four BlurPS passes at
  1920 x 1080, High:
  - *Fewer directions when far*: equal to the default pass up close; at mid and far views at most 1 level of 255 apart
    on average, and the open floor no rougher than with the default (the 4x4 interleave is still cancelled);
    deterministic;
  - *Smooth flat surfaces*: no more stray shade on the open floor and the counter top than the default, and at least
    75% of the contact shade where the boxes meet the floor;
- the GPU time of the AO pass alone at 3840 x 2160 for each variant (12 interleaved rounds, the median).

The synthetic scene is not a game frame: confirm the look in game (Ambient Occlusion picture pair,
`tools/ingame_runner/jobs/ao-adaptive-ab.json`) and the GPU time with the in-game AO timer.
