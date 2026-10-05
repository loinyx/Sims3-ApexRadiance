# Night Lighting, fences: validation

The feature is described in [features/night-lighting/fences.md](../features/night-lighting/fences.md). It must:

- Light instanced fences, railings, posts and stairs at least as brightly as the ground atlas at their world position,
  scaled by the strength, and never below the game's vertex lights.
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

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-04 | `92582b6` | Terrain lighting suite (policy, bench GPU, resource fixtures) | Passed (part of 2,017,393 GPU, policy and resource checks); Release x86 built with no errors or warnings | Native D3D9 |

## In-game test plan

1. Night: a fence line and a fenced yard between street lamps and garden lamps, stairs and a porch railing outside.
   **Expected:** rails as bright as the ground next to them.
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

- Daytime response 0.08 (PR #2): gameplay appearance pending; only the captured bench shader was tested offline.
- Same-lot gameplay and DXVK confirmation of the PR #2 lighting changes: pending.
