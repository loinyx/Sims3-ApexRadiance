# Water Reflections: validation

The feature is described in [features/reflections.md](../features/reflections.md). It must:

- Add the shore reflection on recognised ponds only when the scene depth is readable and bound as the scene's
  depth-stencil.
- Keep the game's water and sky reflection underneath; add lamp glow only near lamps at night.
- Run the reflection independently of the lamp-glow switch.
- Never fire the post-scene effects from its own depth-off draw.
- Restore the last brightness when the card switch is turned back on.

## Automated tests

No harness covers the water pass or the Water Reflections card. Shader recognition uses exact IDs from the game's
shader package (see [night-lighting/water.md](../features/night-lighting/water.md)).

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| | | | No automated runs recorded | |

## In-game test plan

1. Night, by a pond, Night Lights on, Depth Blur on, the game's Edge Smoothing off. **Expected:** trees, houses and lamps
   on the shore appear in the water; lamps add glow and glints.
2. Turn *Lamps glow on ponds* off. **Expected:** the glow goes, the shore reflection stays.
3. Turn the Water Reflections card off, then on. **Expected:** the reflection disappears, then returns at the previous
   brightness.
4. Depth Blur off (and Edge Smoothing *Edges from depth* off, Ambient Occlusion off). **Expected:** only the lamp glow;
   the card shows *Needs Depth Blur*.
5. Turn the game's Edge Smoothing on. **Expected:** amber *Turn off the game's Edge Smoothing* card listing Water
   Reflections; the Overview row shows *Waiting for game settings*.
6. Ocean and swimming pool. **Expected:** unchanged.
7. Developer status line (Night Lighting developer status). **Expected:** `Water: water: active | draws with
   reflections: N`. Log: `[LotLightBridge] Agua: ativo`.

## Confirmed in game

- Lamp glow and shore reflection were confirmed in the combined build (24/09 to 25/09, rotated-lake and no-depth fixes
  25/09). See [night-lighting/water.md](../features/night-lighting/water.md).

## Open checks

- A depth path while the game's MSAA is on (resolve or redraw depth).
- Whether the S3SS Mirror Reflection Settings affect the ocean's planar reflection (unverified; it is not expected to
  touch ponds).
