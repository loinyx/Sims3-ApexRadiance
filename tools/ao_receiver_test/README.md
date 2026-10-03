# AO receiver checks

Read-only shader-package and native D3D9 checks. Run from the repository root in an x86 VS developer prompt:

```bat
cl /nologo /O2 /EHsc /std:c++20 /Ifeatures /Ishaders tools\ao_receiver_test\ao_receiver_test.cpp features\shader_patches.cpp d3d9.lib d3dcompiler.lib user32.lib /Fe:%TEMP%\ao_receiver_test.exe
%TEMP%\ao_receiver_test.exe "C:\Games\Hydra\The Sims 3\Game\Bin\Shaders_Win32.precomp"
```

The harness reads the package and the actual AO HLSL source. It validates unique Sim material pairs against a native D3D9 HAL device, checks failure leaves input unchanged, and renders SM2/SM3 alpha-tested masks. It also renders the actual composite at 100%, 0% and 50%, separate hair/body intensities, shade caps, transparent-hair coverage and foreground-depth rejection. Shader alpha is preserved for alpha testing; mask depth and coverage use G32R32F. Transparent layered hair remains a screen-composite approximation, not per-layer colour separation. No shader bytecode or screenshots are written.

To regenerate only size/hash identifiers from technique ownership:

```bat
node tools\ao_receiver_test\generate_ids.cjs "C:\Games\Hydra\The Sims 3\Game\Bin\Shaders_Win32.precomp" shaders\sim_receiver_ids.h
```

The generator excludes PS bytecode shared with non-Sim techniques. It stores no game bytecode. Re-run native checks after generation. Exact fingerprints deliberately fail closed on unrecognized variants; they do not prove game coverage.

Regression checks also reproduce an equal-depth replay rejected by LESS and accepted by LESSEQUAL, and verify blended body coverage, zero/partial opacity, independent body/hair strength and foreground rejection. Recognized SRCALPHA/INVSRCALPHA ADD body overlays now use the same coverage target as blended hair; the hair toggle only gates hair draws. Depth-writing LESS draws replay with LESSEQUAL, then restore the original comparison.

Known limits: nonstandard transparency equations, unknown/custom shader overrides and refused interpolator layouts retain original shading. The shared blended target retains the last accepted layer, not exact multilayer colour separation. Replays add GPU/CPU cost and can affect active occlusion-query sample counts; native in-game query behavior, DXVK and gameplay cost must be evaluated before release.
