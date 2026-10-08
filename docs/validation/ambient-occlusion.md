# Ambient Occlusion: validation

The feature is described in [features/ambient-occlusion.md](../features/ambient-occlusion.md). It must:

- Produce identical output for a still camera (no temporal noise).
- Leave flat, unoccluded surfaces unshaded and keep bright or lamp-lit pixels close to their original colour.
- Fall back to the unmodified image when a device feature is unsupported.

Sim Occlusion has its own page: [validation/sim-occlusion.md](sim-occlusion.md).

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/post_scene_test`](../../tools/post_scene_test/README.md) | Post-scene chain: order, once per frame, hidden-UI fallback, depth mismatch and recovery, reset | See the harness README | None |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-04 | PR #2 | Post-scene chain | 12 passed | None |
| 2026-09-30 | 2.1.0 | GPU shader against the CPU reference (`gpucheck.cpp`, four saved frames) | Raw AO within 0.08 to 0.11 of 255 levels, composite within 0.06 to 0.09; repeat run bit-identical | Native D3D9 |

## In-game test plan

1. Indoor room at night with furniture against walls. **Expected:** contact shade under and behind furniture, walls and
   floors away from objects unshaded, lamp-lit areas keep their colour.
2. Still camera for ten seconds. **Expected:** no flicker or crawling dots.
3. Outdoor scene with trees in wind. **Expected:** no twinkling on leaves.
4. Map view with *Also in map view* on and off. **Expected:** houses and trees shaded when on, no shade when off.
5. Change resolution and alt-tab. **Expected:** the effect recovers on the next frame.

## Confirmed in game

- 2.1.0: scene GTAO accepted in game for release.

## Open checks

- GPU cost at 4K in game (DXVK).
- Foliage in wind, water, thin railings and daytime scenes (the reference frames were night scenes).
- Scene-boundary detection: the Picture colour-difference UI heuristic and frames with an early depth-off draw still
  need controlled gameplay captures. The post-scene tests establish ordering, not visual equivalence.

### Local full-resolution-only candidate, 2026-10-07

Half resolution and the unreleased reconstruction candidate were removed at the user's request. The local native D3D9
fixture passed 65 checks: the six retained non-GTAO entrypoints and all ten GTAO quality/thickness variants compile to
bytecode identical to the pre-reconstruction full-resolution baseline and create successfully on the native device.
Existing full-resolution uniforms retain pixel size 1 and base pyramid level 0. Runtime/preset reduction keys,
upsample shader registration and extra full-size upsample resources are removed. Old files must retain unrelated values
and cannot reactivate the removed path. Gameplay and native/DXVK timing remain required; no residual F10 fix is claimed.
