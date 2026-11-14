# ReShade shaders ported to Direct3D 9 (shader model 3.0)

The `.hlsl` files here are ReShade effect files ported to ps_3_0 for Apex Radiance's Looks. Each port is the author's
`.fx` from the repository below, at the commit named in the port's "Ported to ps_3_0" line. The changes are
mechanical: the `#include` lines are dropped (prod80's `PD80_00_Base_Effects.fxh` is inlined), each uniform becomes a
constant register, `ReShade::BackBuffer` becomes `BackBuffer`, CAS's sRGB colour sampler and AdaptiveSharpen's pass
texture become the runner's samplers, and the techniques are replaced by entry wrappers at the end.
`shaders/looks_common.hlsl` supplies the `BUFFER_*` values and ReShade's D3D9 texel-offset helpers.
`reshade_shaders_hlsl.h` is generated from the ports by `tools/looks_shaders_gen.py` and embedded in the ASI.

Every port keeps its author's header as written. A port whose `.fx` carries no licence notice also carries a copy of
its repository's `LICENSE`. Settings > About > *Third-party licenses* shows the texts below in the game.

| Port | Original file | Repository | Author | Licence | Notice |
|---|---|---|---|---|---|
| AdaptiveSharpen.hlsl | AdaptiveSharpen.fx (legacy branch) | https://github.com/crosire/reshade-shaders/tree/legacy | bacondither | BSD 2-Clause style (bacondither) | In the file |
| CAS.hlsl | Shaders/SweetFX/CAS.fx | https://github.com/CeeJayDK/SweetFX | Advanced Micro Devices, Inc. (FidelityFX CAS); ReShade port by SLSNe, Marty McFly, CeeJay.dk and others | MIT (AMD) | In the file |
| Colourfulness.hlsl | Colourfulness.fx (legacy branch) | https://github.com/crosire/reshade-shaders/tree/legacy | bacondither | BSD 2-Clause style (bacondither) | In the file |
| Curves.hlsl | Shaders/SweetFX/Curves.fx | https://github.com/CeeJayDK/SweetFX | CeeJay.dk | MIT (SweetFX) | Repository LICENSE, copied into the port |
| DPX.hlsl | Shaders/SweetFX/DPX.fx | https://github.com/CeeJayDK/SweetFX | Loadus | MIT (SweetFX) | Repository LICENSE, copied into the port |
| FakeHDR.hlsl | Shaders/SweetFX/FakeHDR.fx | https://github.com/CeeJayDK/SweetFX | CeeJay.dk | MIT (SweetFX) | Repository LICENSE, copied into the port |
| Levels.hlsl | Shaders/SweetFX/Levels.fx | https://github.com/CeeJayDK/SweetFX | CeeJay.dk | MIT (SweetFX) | Repository LICENSE, copied into the port |
| LiftGammaGain.hlsl | Shaders/SweetFX/LiftGammaGain.fx | https://github.com/CeeJayDK/SweetFX | 3an, CeeJay.dk | MIT (SweetFX) | Repository LICENSE, copied into the port |
| LumaSharpen.hlsl | Shaders/SweetFX/LumaSharpen.fx | https://github.com/CeeJayDK/SweetFX | CeeJay.dk | MIT (SweetFX) | Repository LICENSE, copied into the port |
| PD80SelectiveColor.hlsl | Shaders/PD80_04_Selective_Color.fx and PD80_00_Base_Effects.fxh | https://github.com/prod80/prod80-ReShade-Repository | prod80 (Bas Veth) | MIT (prod80) | In the file |
| RetroTint.hlsl | Shaders/RetroTint.fx | https://github.com/Daodan317081/reshade-shaders | Daodan317081 (Alexander Federwisch) | BSD 3-Clause | Repository LICENSE, copied into the port |
| Technicolor.hlsl | Shaders/SweetFX/Technicolor.fx | https://github.com/CeeJayDK/SweetFX | DKT70, CeeJay.dk | MIT (SweetFX) | Repository LICENSE, copied into the port |
| Technicolor2.hlsl | Shaders/SweetFX/Technicolor2.fx | https://github.com/CeeJayDK/SweetFX | prod80, CeeJay.dk | MIT (SweetFX) | Repository LICENSE, copied into the port |
| Tint.hlsl | Shaders/SweetFX/Sepia.fx (technique Tint) | https://github.com/CeeJayDK/SweetFX | CeeJay.dk | MIT (SweetFX) | Repository LICENSE, copied into the port |
| Tonemap.hlsl | Shaders/SweetFX/Tonemap.fx | https://github.com/CeeJayDK/SweetFX | CeeJay.dk | MIT (SweetFX) | Repository LICENSE, copied into the port |
| Vibrance.hlsl | Shaders/SweetFX/Vibrance.fx | https://github.com/CeeJayDK/SweetFX | CeeJay.dk | MIT (SweetFX) | Repository LICENSE, copied into the port |
| Vignette.hlsl | Shaders/SweetFX/Vignette.fx | https://github.com/CeeJayDK/SweetFX | CeeJay.dk | MIT (SweetFX) | Repository LICENSE, copied into the port |

## Licence texts

### MIT (SweetFX), `LICENSE` of CeeJayDK/SweetFX

```text
The MIT License (MIT)

Copyright (c) 2014 CeeJayDK

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### MIT (AMD), in `CAS.fx`

```text
Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE
```

### BSD 2-Clause style (bacondither), in `AdaptiveSharpen.fx` and `Colourfulness.fx`

```text
Copyright (c) 2015-2018, bacondither (AdaptiveSharpen)
Copyright (c) 2016-2018, bacondither (Colourfulness)
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer
   in this position and unchanged.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### MIT (prod80), `LICENSE` of prod80/prod80-ReShade-Repository and `PD80_04_Selective_Color.fx`

```text
MIT License

Copyright (c) 2020 prod80

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### BSD 3-Clause, `LICENSE` of Daodan317081/reshade-shaders

```text
BSD 3-Clause License

Copyright (c) 2018-2019, Alexander Federwisch
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
