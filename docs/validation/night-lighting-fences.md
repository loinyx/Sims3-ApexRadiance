# Night Lighting, fences: validation

The feature is described in [features/night-lighting/fences.md](../features/night-lighting/fences.md). It must:

- Use spatial lamp light for valid instanced groups, with surface position, normal, distance and cones; shared native vertex lamp light does not impose a brightness floor.
- Limit the directionless ground atlas to upward-facing surfaces in spatial mode, retaining native vertex/atlas fallback when group data is unavailable.
- At full night, send exactly the configured strength; by day, send `0.08 x min(strength, 1)`; and leave lamp-free
  sunlit pixels identical to the game.
- Stay as the game draws when the switch is off or the atlas is missing, with no device writes.
- Restore the sampler, constants and pixel shader it changes.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/terrain_lighting_test/check.cpp`](../../tools/terrain_lighting_test/README.md) (policy) | `SurfaceLampGain` over 101 night levels and the gain range | `run.ps1 -Captures <dir> -OutDir <dir>` | None |
| `check.cpp`, `InstancedPixels` | The exact captured bench pixel shader `PS_3019F738.bin` (912 bytes, `C0F5CCD7`) on native D3D9 with controlled inputs: lower daytime lamp response, identical full-night pixels, identical lamp-free sunlight pixels, lamp attenuation separated from sunlight within 2 levels | `run.ps1 ... -ReferenceSource <old lot_light_bridge.cpp> -InstancedCapture <F7 session>` | D3D9 GPU, F7 capture |
| `run_resource_checks.ps1` (`resource_paths_fixture.cpp`) | Production `DrawInstanced` extracted from `lot_light_bridge.cpp`: shared daytime lamp scale, sunlight colour and direction kept, disabled path and missing atlas stay native with zero writes, state restoration | `run_resource_checks.ps1 -OutDir <dir>` | None |

The harnesses only read captures and sources and print to stdout.

## Latest results

Release preparation: the public x86 build passes with zero warnings. Exact release commit identity is recorded in the release PR; final artifact gameplay and menu-footer validation remain open. Gameplay evidence applies to the final development candidate ASI (`26CAA132F3D4DD6921E7E3CFFA8BEFA09B3F73868AEA3A917B92B7873D1D8C0D` SHA-256), with wall updates, fence lighting and gate faces visually accepted. This does not establish general FPS gains or exhaustive scene coverage.


| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-23 | `4802ed9` (ASI SHA-256 `FC2D565C0F003F0DD9DF2BC56DFC5F047CA6FCDAC508B2DA66A83D032258ECFB`) | Standard Release x86 build; `tools/terrain_lighting_test/run_resource_checks.ps1`; `tools/floor_shade_check`; `tools/ground_shade_test/run.ps1` | Build: 0 warnings. Resource paths: 14,637 checks, 0 failures, including `DrawInstanced` (85) and realistic reach (388). Floor shade: 7,885 checks, 0 failures, including the per-lamp cap shader. Ground shade: 240 checks, 0 failures. Release gameplay remains unverified | MSBuild x86 / fake D3D9 / native D3D9 |
| 2026-10-23 | `40f351e` | `tools/floor_shade_check` (`run.ps1 -Precomp <Shaders_Win32.precomp>`) | 7,103 checks, 0 failures (the fence copy's mask read in front of the face). No harness covers the reach scale without the walls' gain or the gates in the lot shade | Offline, native D3D9 |
| 2026-10-22 | `d625404` | None: `run_resource_checks.ps1` (which extracts `DrawInstanced`) was not re-run; no harness covers the lamp pick memo | Not run | not run in game yet |
| 2026-10-13 | Release preparation tree for 2.12.3; final SHA in the release PR | Developer and public Release x86 builds: 0 errors, 0 warnings; `run_resource_checks.ps1` | Builds passed; resource paths 14,154 checks, 0 failures (extracted `DrawInstanced` 54, realistic reach on objects and fences 386). The captured fence shader suite (`run.ps1`) was not re-run | Fake D3D9 / x86 build |
| 2026-10-12 | Release preparation tree; final SHA in PR | Public Release x86 build; Reconstructed fence shader fixture | Zero build warnings; Passed spatial/native selection, directional lamp composition and fallback checks; captured-scene gameplay is not reconstructed | Native D3D9 fixtures / x86 build |
| 2026-10-04 | `92582b6` | Terrain lighting suite (policy, bench GPU, resource fixtures) | Passed (part of 2,017,393 GPU, policy and resource checks); Release x86 built with no errors or warnings | Native D3D9 |

## In-game test plan

1. Night: a fence line and a fenced yard between street lamps and garden lamps, stairs and a porch railing outside.
   **Expected:** each face follows lamp direction and distance; distant rails do not keep the brightness of the group center. Upward surfaces can retain ground light while vertical faces remain directional.
2. Day: the same scene, including an outdoor bench. **Expected:** sunlit fences and the bench look like the game; only a
   faint lamp share remains near lamps.
3. Dusk and dawn. **Expected:** the lamp term rises and falls smoothly with the night level.
4. Turn *Fences and stairs catch light* off. **Expected:** the game's dark fences return, live.
5. Developer status. **Expected:** `fences/stairs: N` draws grow.
6. Log. **Expected:** `[LotLightBridge] Fence/stairs: shader XXXXXXXX patched` or `... does not have the expected
   pattern, left as the game draws it`.
7. F7 on a rail (developer mode). **Expected:** the patched pixel shader bound; game VS `c4..c11` usually zero.

## Confirmed in game

- Ground term on fences, railings and stairs (combined build, 2026-09-25).

## Open checks

- Ground light on stair tops (`04e5f3e`), F7 2026-10-29 17-35-27 and 17-35-29: an outdoor stair
  beside a house wall, at night. **Expected:** the steps above 1.25 m take the same wall shade as the grass beside the stair
  (no bright flat band over the shaded lower steps); the lamp at the top of the stair still lights the upper steps.
  Offline: `tools/floor_shade_check`, 10,854 checks, 0 failures (the 116 instanced pixel shaders of the precomp, both laws).

- Exact release ASI in game: compare a raised railing, elevated fence run and stairs below an emitter with nearby walls;
  verify that the floor shadow stops at open stair treads and that elevated structures keep the ground atlas pool out.

- 2.15.3, *Realistic lamp reach* on: a fence, its gate and the ground beside a garden post. **Expected:** fence, gate and ground take the same light; no dark band under 1 m on a fence face turned to the lamp.
- Fence lamp pick memo (`d625404`), development build at night: fences, stairs and railings near lamps while the camera
  moves, a lamp switch and a lamp drag beside a fence. **Expected:** the same light as with `lamp_visibility_memo` off;
  the roof status line's `fence lamp picks: N reused, M computed` with most picks reused while nothing changes; no
  `Fence lamp pick memo: a kept pick differed` warning. `run_resource_checks.ps1` (the extracted `DrawInstanced`, which
  now calls `PickFenceLamps`) to be re-run.
- 2.12.3 public build, not yet tested in game: a balcony railing and an L-shaped band with a garden post below them, with *Realistic lamp reach* on and off. **Expected:** the band follows the light of the wall behind it; railing tops above the lamp heads take no ground light; a lamp under the balcony does not light the railing on it.
- Exact release artifact: final SHA, build verification and unchanged visual behavior after cleanup
- Version footer and unread-version indicator on the final release build; all 21 changelog language tables pass offline validation
- Wider fence/gate materials, fallback groups and daytime transitions


- Daytime response 0.08 (PR #2): gameplay appearance pending; only the captured bench shader was tested offline.
- Same-lot gameplay and DXVK confirmation of the PR #2 lighting changes: pending.
- Per-pixel lamps on fences and walls blocking them (2.7.0): the patched fence shader was not validated offline;
  in-game check of a fence beside a lamp, a fence behind a wall, and a long fence running past a wall.

The updated resource-path fixture passed 13,754 checks. Geometry producers are fixture-controlled; state restoration and cone upload are verified, without claiming scene-wide visual parity.
