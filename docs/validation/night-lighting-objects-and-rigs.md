# Objects and rigs: validation

The feature is described in [features/night-lighting/objects-and-rigs.md](../features/night-lighting/objects-and-rigs.md).
It must:

- Evaluate outdoor lamps and their cones at each surface pixel. In valid direct-pixel mode, vertical gate faces receive no directionless ground-atlas term; upward faces retain it.
- Preserve the existing native fallback when spatial data or shader support is unavailable; do not impose equal brightness on opposite faces.
- Give doors and windows at least the light of the wall around them, and the pieces of a modular counter one continuous
  light.
- Boost lamps of every light class on rigs, but leave interior lamps and indoor (mode 0) objects unchanged.
- Leave any shader it cannot patch exactly as the game draws it, and produce no invalid shader on native D3D9 or DXVK.
- By day, add only a subdued lamp term (`min(strength, 1) x 0.08`) and reach the exact configured strength at full
  night, without touching sunlight, ambient colour, lamp colour or falloff.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| Offline countertest (private research folder: perl simulators `counter_sh\`, REPORT.md; not in the repository) | `PatchObjectLampVs` / `PatchObjectLampPs` rules over the 2139 Counters/Phong variants of `Shaders_Win32.precomp`: accepted shapes, refusals, validity after patching | Scratch scripts | perl, the game's shader package |
| Scratch `test5.cpp` | Accepted and refused captured object shaders (the lists in the feature's *Shader coverage*) | Scratch | d3dcompiler, captures |
| [`tools/terrain_lighting_test`](../../tools/terrain_lighting_test/README.md) `run_resource_checks.ps1` | Extracted production `DrawObjectLamp` and `DrawInstanced` against D3D9 fixtures: the strength written, native drawing when disabled, indoor objects excluded from the daytime outdoor balance, missing atlas stays native, state restoration | `powershell -File tools/terrain_lighting_test/run_resource_checks.ps1 [-OutDir <dir>]` | MSVC x86, native D3D9 |
| same, `run.ps1 -WallCapture / -InstancedCapture` | `SurfaceLampGain` over night levels and gains; the captured instanced bench shader `PS_3019F738` (912 bytes, FNV `0xC0F5CCD7`) on the GPU: night and lamp-free sunlight pixels identical, daytime lamp term reduced, lamp attenuation separate from sunlight | `run.ps1 -Captures <session> -InstancedCapture <dir>` | same, captures |
| Scratch test008 checks | The uncapped indoor-object variant (`PatchIndoorBasis` without `kBasisCap`) over captured pixel shaders | Scratch | d3dcompiler, captures |

The harnesses only read captures and print results; they do not attach to the game or install files.

## Latest results

Release preparation: the public x86 build passes with zero warnings. Exact release commit identity is recorded in the release PR; final artifact gameplay and menu-footer validation remain open. Gameplay evidence applies to the final development candidate ASI (`26CAA132F3D4DD6921E7E3CFFA8BEFA09B3F73868AEA3A917B92B7873D1D8C0D` SHA-256), with wall updates, fence lighting and gate faces visually accepted. This does not establish general FPS gains or exhaustive scene coverage.


| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-23 | `4802ed9` (ASI SHA-256 `FC2D565C0F003F0DD9DF2BC56DFC5F047CA6FCDAC508B2DA66A83D032258ECFB`) | Standard Release x86 build; `tools/terrain_lighting_test/run_resource_checks.ps1`; `tools/terrain_lighting_test/run.ps1 -Captures <folder>` | Build: 0 warnings. Resource paths: 14,637 checks, 0 failures, including 826 object-draw checks and 388 realistic-reach checks. Terrain suite: 1,303,085 checks; 2 failures: required terrain captures are absent, and the synthetic reach-law case at `w=144.5` differs from the wall-law reference by up to 179/255; the `w=85` case differs by at most 1/255. The reach-law source is unchanged from 2.15.3 | MSBuild x86 / fake D3D9 / native D3D9 GPU |
| 2026-10-23 | `40f351e` | `tools/development/check.ps1 -Suite Smoke` and `-Suite Lighting`; developer, public and standard Release x86 builds | Passed; the builds compiled. No harness run covers the hue clamp of the reach law, the gates' lamp scale or the rig records of lamps under a room's floor | CPU harness |
| 2026-10-22 | `b863bce` | None: no harness covers the area lights' cones in `ReadLamp` | Not run | not run in game yet |
| 2026-10-14 | `release/2.13.0` (`2aefeb7`) | Developer and public Release x86 builds; `tools/roofed_openings_test`; `tools/floor_shade_check -Precomp` | Both builds: 0 errors; 173 and 5842 checks, 0 failures | native D3D9 |
| 2026-10-13 | Release preparation tree for 2.12.3; final SHA in the release PR | Developer and public Release x86 builds: 0 errors, 0 warnings; `run_resource_checks.ps1` | Builds passed; resource paths 14,154 checks, 0 failures (extracted `DrawObjectLamp` 386, realistic reach on objects and fences 386, *Buildings block light on objects* 14). `DrawWindowOutdoor` is not extracted; the captured-shader law checks (`run.ps1`) were not re-run | Fake D3D9 / x86 build |
| 2026-10-12 | Release preparation tree; final SHA in PR | Public Release x86 build; Reconstructed gate shader fixture | Zero build warnings; Passed per-pixel cones and vertical/upward/fallback ground masks; material diversity and gameplay are not exhaustive | Native D3D9 fixtures / x86 build |
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

- On the exact release ASI, compare outdoor objects and fences with adjacent walls using a high-intensity lamp. The
  synthetic `w=144.5` GPU case differs from the wall law by up to 179/255; scene impact is unverified. The ordinary
  `w=85` case differs by at most 1/255. This shader law is unchanged from 2.15.3.

- 2.15.3: warm and coloured lamps near objects and fences with *Realistic lamp reach* on. **Expected:** their hue near the lamp head, not white.
- 2.15.3: furniture in a closed room above a lit room with ceiling lamps. **Expected:** no glow from the room below.
- Area lights (`b863bce`): objects, fences and trees beside a lot's rectangle or disc area light at night (a park lot).
  **Expected:** lit only on the side the light faces, none of its light behind it; roofs and water near it unchanged
  (they take no cones). Not yet checked in game.
- 2.12.3 public build, not yet tested in game: with *Realistic lamp reach* on, a column, a balcony band and a window frame beside a lit wall, and objects standing above every lamp head around. **Expected:** they take the light of the wall behind them; objects above the lamps take no ground light; *Buildings block light on objects* off restores the previous light.
- 2.13.0 build, in game: Ambient Occlusion next to Sims beside objects that Night Lighting redraws at night, with the Tester switch *Re-issued draw filter* on and off. **Expected:** the same Sim shade in both.
- 2.13.0 build, in game: bushes and trees behind a free-standing wall with a garden post on the other side, and a lamp a player disabled next to objects. **Expected:** no lamp light on either.
- Exact release artifact: final SHA, build verification and unchanged visual behavior after cleanup
- Version footer and unread-version indicator on the final release build; all 21 changelog language tables pass offline validation
- Wider fence/gate materials, fallback groups and daytime transitions


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

The updated resource-path fixture passed 13,754 checks. Geometry producers are fixture-controlled; state restoration and cone upload are verified, without claiming scene-wide visual parity.

- Arcade column (not checked in game): a column or object inside a roofed arcade, a few metres behind an arch from a
  garden post, takes the post's dim light instead of drawing black; the F7 note says "kept" for that lamp, and the
  Night Lighting status counts "kept under the game's cut" (offline: `tools/roofed_openings_test`, the rig cut checks).
