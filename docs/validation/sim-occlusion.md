# Sim Occlusion: validation

The feature is described in [features/sim-occlusion.md](../features/sim-occlusion.md). It must:

- Apply Sim and hair settings only where the visible pixel belongs to a recognised Sim material.
- Keep the game's depth comparison, alpha test, scissor rectangle and blend state for every replayed draw.
- Keep the scene shade when a device feature, shader layout or depth match is unsupported or ambiguous.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/ao_receiver_test/ao_receiver_test.cpp`](../../tools/ao_receiver_test/README.md) | Sim material recognition over the game's shader package, receiver shader generation (opaque and blended, SM2/SM3), composite at 0/50/100%, body/hair strengths, darkening cap, transparent coverage, foreground rejection | See the harness README | Game `Shaders_Win32.precomp`, D3D9 GPU |
| [`tools/ao_receiver_test/runtime_replay_check.cpp`](../../tools/ao_receiver_test/run_runtime_checks.ps1) | Production replay code: overlap order, depth comparisons, colour-disabled queries, scissor and blend restoration, device-failure fallback | `run_runtime_checks.ps1` | D3D9 GPU; optionally the game's DXVK DLL |
| `generate_ids.cjs` | Regenerates the Sim material fingerprints from technique ownership | `node tools\ao_receiver_test\generate_ids.cjs <precomp> shaders\sim_receiver_ids.h` | Node.js, game shader package |

The harnesses only read the shader package and the repository. They write no bytecode or images.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-04 | `892d4e4` | Shader and composite checks | 9,400 passed, 0 failed | Native D3D9 and DXVK 3.1.1 x86 |
| 2026-10-04 | `892d4e4` | Production replay fixture | 223 passed, 0 failed | Native D3D9 and DXVK 3.1.1 x86 |
| 2026-10-04 | `892d4e4` | Shader package scan | 1,912 distinct pairs: 1,796 opaque and 1,796 blended copies accepted, 116 layouts refused unchanged | D3D9 |

Regressions that these tests reproduce, and that failed before their fixes: the receiver generator rejecting blended body
materials (8 render assertions), coplanar LESS rejection, and near-then-far triangles in one draw leaving the farther
depth.

## In-game test plan

1. Sim Occlusion on, Show Sim coverage on, Sim close to the camera. **Expected:** body blue, hair green, background
   black; lips and mouth interior follow the face.
2. Sim with transparent hair over a bright background, Transparent hair on and off. **Expected:** only the hair strands
   change.
3. Sim intensity and Hair intensity at 0% and 100%. **Expected:** 0% removes the shade on that part, 100% matches the
   scene shade.
4. Maximum darkening at 0% and 100% on a Sim standing in a corner. **Expected:** 0% leaves the Sim unshaded, 100% matches
   Sim intensity.
5. Turn the card off and on, change resolution and alt-tab. **Expected:** no warning, the mask recovers on the next frame.

## Confirmed in game

- PR #2, overlap candidate (ASI SHA256 `78517EC8...BC3E`): the reported Sim scenario (lips and mouth interior on the
  coverage preview) renders correctly.

## Open checks

- CPU and GPU cost of receiver replays in game.
- Moving Sims, Create a Sim, occult and robot materials, hidden Sims, save and world transitions.
- Custom clothing and hair, including custom shaders.
- Overlapping transparent layers on custom content.
