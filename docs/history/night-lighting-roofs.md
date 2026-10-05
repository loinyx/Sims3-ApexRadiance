# Night Lighting, roofs: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/roofs.md](../features/night-lighting/roofs.md).

### 2026-09-24: roof shader analysis

**Context:** roofs had no reaction to light at night: black next to a sconce that lit the wall.

**Finding:** `LightProbe-telhado` (draw #652, DIP, 2776 triangles): `PS_278ED708` (`ps_3_0`) + `VS_278F5B10`. Light =
sun/moon `c0` with a 4-tap shadow (`s5`), specular (pow 90), ambient cube `s1 x c6.x`, a 64x64 L8 mask (`s6`, UV `v5`)
multiplying sun and sky, sky reflection cube `s0`. No lamp term, no lot or terrain light map, no rig lights. A second roof
(`LightProbe-telhado2`) used the same PS (hash 42e2c20d).

**Outcome:** an HLSL replacement that reproduces the game's lighting and adds lamps.

### 2026-09-24: first test too bright

**Context:** first in-game test with strength 1.

**Finding:** roofs were almost white.

**Outcome:** a dedicated roof strength was added (first default 0.35, range 0.05 to 2).

### 2026-09-24: radius from the light bounds

**Context:** roofs and the lake blew out to pinkish white.

**Finding:** the radius came from the light bounds (`+0x134`, about 50 m), and each street lamp has two lights at the
same place (`+0x130` = 97 and 40).

**Outcome:** visual radius from `clamp(1.2 x sqrt(range), 2, 25)`; the roof default then rose to 0.6.

### 2026-09-24: flicker when zooming

**Context:** the 16 lamps were chosen nearest to the camera every 20 frames.

**Finding:** roofs flickered when zooming because the chosen set changed with the camera.

**Outcome:** selection per draw from the roof piece's position, limit 80 m. Summer roofs confirmed in game after this
fix and the strength fix.

### 2026-09-25: snowy roofs

**Context:** snowy roofs stayed "waiting" (capture m23, `LightProbe-telhado-neve`).

**Finding:** `PS_2793C3E8` (`ps_3_0`, 4992 bytes, 4 `rep` loops of noise for the snow normal) + `VS_27944BD8` (1344
bytes). Light = `sat(N.L) x c0 x shadow(s5, 4 taps, lrp v2.x) + texCUBE(s0, N) x c5.x`, times the mask `r6.w` (`s7`) and
the snowy albedo `r6`; fog with `v4`. No lamps. The shader is too large to rewrite.

**Outcome:** an additive pass on the same geometry (`roof_snow_lamps_ps.hlsl`).

### 2026-09-25: snowy roof position (m29)

**Context:** the first snowy-roof pass used TEXCOORD4.zw (world xz x VS `c19.x`).

**Finding:** on some roofs `c19 = (0,0,0,0)`, so every pixel sat at the world origin while the lamps were near the roof.

**Outcome:** position from COLOR0 x VS `c15.x` (copied to PS `c53.x`).

### 2026-09-25 (10:43): stair snow sent to the roof pass

**Context:** first test of the snow-on-stairs fix (m50/m51, `LightProbe-neve-escada`: `VS_2E036438` / `PS_2E036820`, 500
triangles, 4 `rep` loops of 3D noise with `s1` permutation 256x256 and `s2` gradient 256x1).

**Finding:** the stair snow pixel shader is byte-identical to the snowy roof one. The PS class was tested before the VS
classes, so stairs went to `DrawRoofSnow`, which read lamp positions from roof VS constants and was wrong on stairs.

**Outcome:** `IsSnowReliefVs` (VS class 9; offline only `VS_2E036438` matched among 161 VS) and the routing rule
`RoofSnow && !g_curVsIsSnowRelief`.

### 2026-09-25: plan for one lamp law and roof occlusion

**Context:** the roadmap (phase 3) plans one lamp model for all surfaces ("Telhados: passam para a mesma lei") and lamp
occlusion on roofs.

**Finding:** roofs still use the visual-radius law.

**Outcome:** not started.

### 2026-09-28: background compilation

**Context:** each replacement was compiled with `D3DCompile` in the first draw that needed it, a one-time hitch on the
render thread. The combined build also multiplied the roof strength by the HDR lamp gain on `c52.x`.

**Finding:** compilation can run at start-up; only object creation needs the device. HDR was removed.

**Outcome:** both replacements are compiled at start-up by `framework/shader_cache` and created at the first draw. No
HDR gain in Apex Radiance (see [removed-features.md](../removed-features.md)). The exact-identity check replaced the old
`roof_ref.h` and `roof_snow_ref.h` byte arrays, so the game's bytecode is no longer embedded.

### 2026-09-29: lamp list in one pass and memoised selection

**Context:** the lamp list (`UpdateLampList`) and the lot lamp tracking each walked the light enumeration.

**Finding:** one walk can feed both; most roof pieces are drawn at the same place frame after frame.

**Outcome:** `ReadEnumeratedLamps` and the 512-entry `SelectLamps` memo. Developer status line "lamp choice memo".

### 2026-10-02: disabled lot lamps leave the lamp list (2.5.4)

**Context:** a lot lamp switched off stayed in the roof, water and object lamp list until the game's fade reached zero.

**Finding:** lot lamps carry an enabled flag (`+0x100 & 0x40`).

**Outcome:** `ReadLamp` drops lot-owned lamps that are not enabled. Released in 2.5.4.
