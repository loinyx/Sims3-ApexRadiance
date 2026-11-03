# Object shadows (physical indoor lamps)

With the physical indoor lamp model, furniture and objects can cast the shadows of their real meshes instead of the game's
simple 3D-test shapes: a plant's leaves instead of a large disc, a table's top and legs instead of one quad. Part of
[GPU room lamps](gpu-room-lamps.md).

## Status

| | |
|---|---|
| Availability | Developer builds, in development. Not tested in game yet |
| Default | Game shapes |
| Menu | Lighting > Night (developer) > *Indoor lamp model* Physical > *Object shadows*: Game shapes / Real shapes |
| Configuration | `roomLampObjectShadows = "game"` or `"real"` in `[patches.NightTerrainRelight]` |
| Source | [`features/object_casters.cpp`](../../../features/object_casters.cpp), [`features/object_casters_math.h`](../../../features/object_casters_math.h), `Engine::RenderCasters` in [`features/rt_lamp_gpu.cpp`](../../../features/rt_lamp_gpu.cpp), `CasterVS` / `CasterAlphaPS` in [`shaders/rt_lamps_hlsl.h`](../../../shaders/rt_lamps_hlsl.h) |

## The problem

The game's room solve shades a lamp's light with its 3D test: each object is one or a few discs and quads
(`RoomLightTests`, [light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md)). The physical model reproduces that
test on the GPU, so a plant casts a large blocky shadow on the wall and a side table one flat disc.

## How it works

1. **Capture.** LotLightBridge's early draw hook hands every rig-lit object draw of a lot (not skinned) to
   `ObjectCasters::OnObjectDraw`. The object probe of 2026-11-03 found every object vertex shader reading a short4
   position at offset 0, decoded as xyz / w, its world rows at `c[worldK .. worldK + 2]`, and every vertex buffer MANAGED
   (read-only locks work). Each mesh (vertex range and indices) is copied once into a 32 MB least-recently-drawn cache;
   alpha-tested meshes (plants: alpha test on, no blending) keep their uv and hold their diffuse texture (at most 96).
2. **Instances.** Each mesh at its world rows is an instance. A moved object is a new instance; the old one expires when it
   lies in the camera's view (`PostScene::RecentCameraViewProj`) and has not been drawn for 30 frames. A lot change clears
   everything.
3. **Tiles.** The instances become one triangle list in the lot's space (flat normals, at most 120 000 triangles, DEFAULT
   pool). After the walls of a lamp's cube shadow tile, `Engine::RenderCasters` draws them with the same depth and the same
   soft lookup, leaving out the instance whose box holds the lamp (its own object). Foliage keeps the texels whose diffuse
   alpha is at least 0.5. A tile is drawn again only when an object appeared, moved or went within 15 m of its lamp.
4. **The 3D test.** A blocker record that holds a known instance's centre has its blockers left out (`ObjectCasters::Filter`),
   so the object is not shaded twice; objects never drawn keep the game's shapes.

A `[ObjectCasters]` line every 10 s gives the objects, meshes, copied bytes, triangles, tiles drawn and what was left out.

## Limits

- An object is known only once it has been drawn; until then it keeps the game's shape.
- Past 120 000 triangles the remaining objects keep the game's shape.
- Without the camera's view-projection, instances expire only at a lot change.
- The alpha cut reads the diffuse texture's alpha, not the game's shader output (the game's own alpha test reads that output).
- Rejected: copying the game's model resources (the draws already carry the meshes the game shows).

## Validation

`tools/outdoor_delta_test` checks the position decode, the world rows and lot transform, half floats, the camera's view test,
a record holding a caster and a box holding its own lamp. `tools/rt_lamps_test` compiles `CasterVS` and `CasterAlphaPS`.
In game: not yet.
