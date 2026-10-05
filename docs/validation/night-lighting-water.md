# Night Lighting, water: validation

The feature is described in [features/night-lighting/water.md](../features/night-lighting/water.md). It must:

- Add lamp glints and glow on lake water at night, from lamps chosen by the water mesh's position.
- Reflect the shore only where the scene depth confirms a hit; without depth or without a hit, keep the game's sky
  reflection.
- Project correctly on rotated lots.
- With both highlight switches off, reproduce the original lamp math.
- Restore every constant, sampler, texture, depth-stencil and render state it changes.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/water_lamp_check/check.cpp`](../../tools/water_lamp_check/README.md) | Compilation and creation of the old and new pass shader; legacy equivalence within 1 LSB with both switches off; flat-normal filter identity; zero lights; alpha preservation; analytical highlight compression and RGB ratios. Hidden D3D9 window, 64x64 readback | Build with x86 MSVC, the `shaders` include directory, `d3d9.lib`, `d3dcompiler.lib`, `user32.lib`; run with the path of the previous `water_lamps_ps.hlsl` | D3D9 GPU |

The harness does not validate moving-camera shimmer, shore geometry, walls or in-game performance.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-02 | `2.5.4-rc-water-lamp-filter` | `water_lamp_check` | Both shaders compiled and created; 12 flat-normal A/B cases passed | Native D3D9 |

## In-game test plan

1. Night by a pond with lamps around. **Expected:** glints and a soft glow near lamps.
2. Same scene with Depth Blur on and the game's Edge Smoothing off. **Expected:** the shore (trees, houses, lamps) is
   reflected; nothing black or streaky; moving the camera causes no jumps.
3. Same scene with the game's Edge Smoothing on. **Expected:** glow only, no reflection, no black patches.
4. A pond on a rotated lot. **Expected:** glints and reflection line up with the lamps and shore.
5. Cloudy or rainy weather. **Expected:** glow and reflection still present (second lake shader).
6. Developer > Water highlights: toggle both switches with the camera moving. **Expected:** less sparkle shimmer with the
   filter on; bright highlights keep their hue with the colour preservation on.
7. Developer status. **Expected:** `water: active | draws with reflection: N` ("waiting" before the first lake draw).
8. Log. **Expected:** `[LotLightBridge] Water: active` or the compile error.
9. F7 on the water (developer mode). **Expected:** an extra draw after the game's lake draw; `s7` holds the INTZ when
   depth is available.

## Confirmed in game

- Lamp glow and the shore reflection (combined build, 2026-09-24 and 2026-09-25), including the rotated-lake and no-depth
  fixes.
- Glow and reflection in cloudy and rainy weather after the second lake shader was recognised (2.5.2).

## Open checks

- Highlight filter and colour preservation (2.5.5): moving-camera shimmer, shore geometry and GPU cost have not been
  measured in game.
- Background compilation of the pass shader at start-up: not specifically retested in game.
