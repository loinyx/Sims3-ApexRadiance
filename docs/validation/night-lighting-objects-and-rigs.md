# Objects and rigs: validation

The feature is described in [features/night-lighting/objects-and-rigs.md](../features/night-lighting/objects-and-rigs.md).
It must:

- Never make an outdoor object darker than the game: the result is at least the ground light below it and, outside the
  reach of the selected lamps, the game's own rig light.
- Give doors and windows at least the light of the wall around them, and the pieces of a modular counter one continuous
  light.
- Boost lamps of every light class on rigs, but leave interior lamps and indoor (mode 0) objects unchanged.
- Leave any shader it cannot patch exactly as the game draws it, and produce no invalid shader on native D3D9 or DXVK.
- By day, add only a subdued lamp term (`min(strength, 1) x 0.08`) and reach the exact configured strength at full
  night, without touching sunlight, ambient colour, lamp colour or falloff.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| Offline countertest (scratchpad perl simulators `counter_sh\`, REPORT.md; not in the repository) | `PatchObjectLampVs` / `PatchObjectLampPs` rules over the 2139 Counters/Phong variants of `Shaders_Win32.precomp`: accepted shapes, refusals, validity after patching | Scratch scripts | perl, the game's shader package |
| Scratch `test5.cpp` | Accepted and refused captured object shaders (the lists in the feature's *Shader coverage*) | Scratch | d3dcompiler, captures |
| [`tools/terrain_lighting_test`](../../tools/terrain_lighting_test/README.md) `run_resource_checks.ps1` | Extracted production `DrawObjectLamp` and `DrawInstanced` against D3D9 fixtures: the strength written, native drawing when disabled, indoor objects excluded from the daytime outdoor balance, missing atlas stays native, state restoration | `powershell -File tools/terrain_lighting_test/run_resource_checks.ps1 [-OutDir <dir>]` | MSVC x86, native D3D9 |
| same, `run.ps1 -WallCapture / -InstancedCapture` | `SurfaceLampGain` over night levels and gains; the captured instanced bench shader `PS_3019F738` (912 bytes, FNV `0xC0F5CCD7`) on the GPU: night and lamp-free sunlight pixels identical, daytime lamp term reduced, lamp attenuation separate from sunlight | `run.ps1 -Captures <session> -InstancedCapture <dir>` | same, captures |
| Scratch test008 checks | The uncapped indoor-object variant (`PatchIndoorBasis` without `kBasisCap`) over captured pixel shaders | Scratch | d3dcompiler, captures |

The harnesses only read captures and print results; they do not attach to the game or install files.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-04 | PR #2 | Resource and GPU fixtures (object, wall, instanced paths) with the 8% daytime endpoint | Passed under controlled inputs; not a visual check of a scene | Native D3D9 |
| 2026-10-01 | `2.5.3` test008 | Indoor-object cap variant | 84 captured PS examined, 3 matching shader/sampler combinations; the fallback is byte-identical to the previous build and both variants assemble; the uncapped variant removes exactly 12 tokens (three instructions) | d3dcompiler |
| 2026-09-25 | combined build, v5.6 rules | Countertest over 2139 variants | VS 588/588; PS Counters 136/184 (A 128, B 8); PS Phong 426/444 (A 394, C 32); 0 invalid after patch. Before v5.6: VS Counters 118/236, Phong 64/352; PS Counters 72/184, Phong 292/444 | perl simulators |
| 2026-09-25 | combined build | `test5.cpp` accepted/refused lists | `PS_298DF5B8`, `PS_298DFD88`, `PS_2D03FF80`, `PS_2D041628` accepted after generalising; HD, OutdoorProp and flow-control shaders refused as listed | d3dcompiler |

## In-game test plan

1. Night scene: a front door and a gate next to street lamps and lot lamps, an outdoor modular counter, and a gate 10 to
   25 m from lamps. **Expected:** the door is not darker than its wall; counter pieces show no colour steps; nothing is
   darker than with the option off.
2. Stairs, railings and a fenced yard near a street lamp. **Expected:** they receive lamp light; developer status counts
   *objects opened to lamps* and *in fenced areas*.
3. Brightness and Seamless light brightness at 25%, 100% and 300%. **Expected:** the rig and per-pixel terms follow;
   rigs re-gather on a Brightness change.
4. Dawn and daytime near lamps. **Expected:** a faint lamp term only; full strength back at night.
5. Developer mode, F7 on an object: the `mod:` line shows the shape (A, B or C), the rig mode, the per-pixel lamps and
   strengths, the object position, the lamps used of N in reach, the game's rig PS c0..c13 and the VS vertex lights. For
   a class-10 draw not replaced, the reason (refused PS, option off, indoor rig, no ground atlas). F7 on piece A, then
   piece B, lists the registers that changed.
6. Developer status: `Objects: Active | light classes: 9/9 | ...`; log `[ObjectLightBridge] Installed`, `[RigTracker]
   Installed` and `Shaders at their first draw` lines. Refused shaders appear in `Apex Radiance\ShadersRecusados`; the
   census false colour shows lamp-lit candidates no fix claimed in magenta.
7. Indoors with the light-between-stories directional maps active: place a chair below a wall painting. **Expected:** the
   painting does not darken; check walls, floors and stairs nearby for directional-map artefacts.

## Confirmed in game

- Combined build, 2026-09-25: doors, windows and counters lit correctly; upper-story windows picking up ground light
  judged acceptable.
- 2.5.3 (test008): included after its offline checks; no dedicated in-game result for the painting correction was
  recorded.

## Open checks

- Calibrate *Seamless light brightness* in game against walls and ground.
- Shape B: whether c0/c4 is the sun or a fourth rig lamp; check with F7 if a seam appears on a counter without sky
  reflection (8 variants).
- Visual balance of the 8% daytime response in a real scene.
- Test008 painting correction: walls, floors, stairs, closed rooms, cross-story floor blocking, other objects, night
  controls and performance; the maps remain 2D.
- Coverage not yet attempted: ground light on foliage (m79 flower), an OutdoorProp recogniser (TEXCOORD8), the 4-light
  foliage VS `4375A3EE` / `EAB58655` (PS `936D7C02`), instanced objects, ground light for the summer `kObjectRigHlsl`
  objects, optional COLOR1 packing for the 34 full-input PS.
- One lamp model for all surfaces (roofs, water and objects use `clamp(1.2 sqrt(range), 2, 25)` and `(1 - d²/R²)²`).
- Sims under street lamps.
- Whether the combined build's bake-matched law should be ported; it was fixed for a gate darker than vanilla
  ([history](../history/night-lighting-objects-and-rigs.md)).
