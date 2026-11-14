# ReShade shaders ported to Direct3D 9 (shader model 3.0)

The `.hlsl` files here are ReShade effect files ported to ps_3_0 for Apex Radiance's Looks: the maths stay as their
authors wrote them; the uniforms became constant registers and a short entry wrapper was added at the end of each file.
`reshade_shaders_hlsl.h` is generated from them by `tools/looks_shaders_gen.py` and embedded in the ASI. Each file keeps
its authors' notice; the licence each file states is listed below.

| File | Authors | Licence |
|---|---|---|
| AdaptiveSharpen.hlsl | bacondither | BSD 2-Clause (in the file) |
| CAS.hlsl | Advanced Micro Devices, Inc. (FidelityFX CAS) | MIT (in the file) |
| Colourfulness.hlsl | bacondither | BSD 2-Clause (in the file) |
| Curves.hlsl | Christian Cann Schuldt Jensen (CeeJay.dk) | MIT (in the file) |
| DPX.hlsl | Loadus | None stated in the file |
| FakeHDR.hlsl | Christian Cann Schuldt Jensen (CeeJay.dk) | MIT (in the file) |
| Levels.hlsl | Christian Cann Schuldt Jensen (CeeJay.dk) | MIT (in the file) |
| LiftGammaGain.hlsl | 3an, CeeJay.dk | MIT (in the file) |
| LumaSharpen.hlsl | Christian Cann Schuldt Jensen (CeeJay.dk) | MIT (stated in the file) |
| PD80SelectiveColor.hlsl | prod80 (Bas Veth) | MIT (in the file) |
| RetroTint.hlsl | Alexander Federwisch (Daodan317081) | BSD 3-Clause (in the file) |
| Technicolor.hlsl | DKT70, CeeJay.dk | None stated in the file |
| Technicolor2.hlsl | prod80, CeeJay.dk | MIT (in the file) |
| Tint.hlsl (Sepia.fx) | CeeJay.dk | MIT (in the file) |
| Tonemap.hlsl | Christian Cann Schuldt Jensen (CeeJay.dk) | MIT (in the file) |
| Vibrance.hlsl | Christian Cann Schuldt Jensen (CeeJay.dk) | MIT (stated in the file) |
| Vignette.hlsl | Christian Cann Schuldt Jensen (CeeJay.dk) | MIT (in the file) |

Every file was taken from the GShade shader collection, where Marot Satil lightly optimised most of them. They are to
be replaced by the copies in their authors' own repositories.
