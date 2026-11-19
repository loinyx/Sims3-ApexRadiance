# Ambient Occlusion options check

`run.ps1 [-OutDir <folder>]` builds `check.cpp` with the x86 MSVC toolchain and runs it (the built
`ao_options_check.exe --no-timing` skips the GPU times). Read-only: it prints to stdout
and writes only the compiler outputs to `-OutDir` (default under `%TEMP%`). It needs no game files.

It checks the Ambient Occlusion options of [ambient-occlusion.md](../../docs/features/ambient-occlusion.md):

- every GtaoPS variant (5 qualities x the 4 variants), compiled from the HLSL in
  `patches/ambient_occlusion_patch.cpp`, stays within ps_3_0's 512 instruction slots and is accepted by a native D3D9
  HAL device; the options' other passes compile within the slots too;
- a synthetic depth buffer (floor, a wall, a counter whose top has a 0.03 degree crease, house-sized boxes; 24-bit
  device depth, near 0.25 m) seen from 40, 80 and 150 m goes through the real GtaoPS and the four BlurPS passes at
  1920 x 1080, High:
  - *Fewer directions when far*: equal to the default pass up close; at mid and far views at most 1 level of 255 apart
    on average, and the open floor no rougher than with the default (the 4x4 interleave is still cancelled);
    deterministic;
  - *Smooth flat surfaces*: no more stray shade on the open floor and the counter top than the default, and at least
    75% of the contact shade where the boxes meet the floor;
- *Edge-aware smoothing* (`edgeBlur`), on 4-channel targets with two box passes: at least 90% of the contact shade,
  an open floor within 0.25 of a level of the default's roughness (no interleave left), and a shade step at silhouettes
  no smaller than the default's (a blur that carries shade across an outline lowers it);
- *Fewer directions in the open* (`blockSlices`): at least a fifth of the 4x4 blocks at half the count, at most
  1 level from the default on average, contact shade within 5%, the open floor no rougher, and the shade step across
  block edges where the count changes at most 1.1 times the same pixels' step in the default image (blotchiness);
- *Refine while the camera is still* (`temporalStill`), Low at 1080p over 32 frames of a camera that never moves: the
  first still frame is the fixed pattern; the refined image is nearer to Ultra than plain Low; flicker (the largest
  change between two of the last 16 frames) at most 0.05 of a level on average and 0.1% of the pixels by more than
  1 level; with the counter removed between two frames, at most 25% of its shade is left after 1 frame and 10% after 4;
- *Steady direction count* (`zoomHysteresis`): over walls 2% either side of each threshold in turn the count never
  switches (without it, every frame), and it still follows the view past the band;
- the GPU time of the AO pass alone at 3840 x 2160 for each variant (12 interleaved rounds, the median), of the AO pass
  with its blur, and of the blur passes alone.

The synthetic scene is not a game frame: confirm the look in game (Ambient Occlusion picture pair,
`tools/ingame_runner/jobs/ao-adaptive-ab.json`) and the GPU time with the in-game AO timer.
