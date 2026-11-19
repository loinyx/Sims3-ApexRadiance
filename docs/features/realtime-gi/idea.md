# Replacing TS3's lamp lighting with real-time global illumination

A proposal for Apex Radiance (The Sims 3, Steam 1.67.2, D3D9). It is an idea, not a plan. Nothing here is
implemented; the numbers come from the camera pan profile (`ts3-camera-pan-profile.md`).

## The idea

Throw away the game's lamp lighting entirely: the baked room light maps, the room solve, and every fix Apex stacks on
top of them. Put a modern lighting system in their place. The game only tells us where each lamp is, its colour, its
cone and whether it's on. Apex computes everything else on the GPU:

- **Direct light** from every lamp, with real shadows shaped like the objects: chairs, plants, Sims.
- **Indirect light:** what bounces off walls, floors and furniture and fills the room, coloured by what it hit.
- **Updates** as soon as a lamp or an object changes.

No more fixes, because nothing of the old system is left to fix: no leaks through walls, no wrong-room light, no flat
furniture. And the game's room solve no longer runs at all: today it costs 2.8 ms a frame and causes hitches of up to
60 ms.

## How it could work within D3D9 (shader model 3, no compute shaders)

### 1. Direct light: shadow-mapped lamps

- A cube shadow map per lamp (a single face for spots), drawn from the game's own draws, which Apex already sees.
- Static geometry is cached per lamp and only redrawn when something near that lamp changes.
- Moving casters (Sims, doors) are redrawn every frame, and only for the lamps nearest the camera.
- Foliage is drawn with its alpha test, so leaves cast leaf-shaped shadows.
- Soft edges that widen with distance from the caster (PCSS): sharp at a chair leg's foot, softer further away.

### 2. Indirect light: a per-lot irradiance volume

- A 3D grid of light probes over the active lot and its neighbours, kept as volume textures.
- It is filled on the GPU from the lamps' shadow maps. Each map also stores the colour of what the lamp hits (reflective
  shadow maps); that light is injected into the grid and propagated a few steps. This is the light propagation volumes
  technique, which Crysis used on DX9-class hardware.
- The scene is mostly static, so the grid is computed when a lamp or an object changes, spread over frames, and then
  just sampled.
- Screen-space GI on top adds small-scale bounce and contact detail.

### 3. Shading

- The game's shaders read the room light map for lamp light. Apex would patch each lit shader family to read the new
  direct light and the probe grid instead; it already patches many of these families today.
- The sun, the sky and the look of the materials stay the game's.

## What it costs, and why it can still be faster

| | Today | With real-time GI |
|---|---|---|
| Game room solve | 2.8 ms a frame on average, hitches up to 60 ms | Off |
| Apex per-draw lighting fixes | 1.8-3.4 ms a frame | Mostly gone |
| Apex relights, wall tests, lamp picks | Running | Gone |
| Shadow-map draws | None | Cheap once cached; Sims are the costly part |
| GPU per-pixel lighting and probe grid | Little | More GPU work (the GPU is underused today) |

The CPU and the bridge's cost per D3D call are the limits today, not the GPU. Fewer hitches are very likely. Whether
FPS goes up depends on keeping the shadow-map redraws small.

## Hard parts

- **Shader coverage:** every lit shader family needs a patched copy that reads the new light, and some families
  already use all 16 samplers.
- **Sims and objects:** the game lights them with its rig lights, a path separate from the room maps. They would have to
  read the probe grid too, or they'll look out of place.
- **Address space:** the game is 32-bit, and the resources it creates count against that. With the bridge, the VRAM
  side is fine.
- **The look:** real GI is darker and more contrasted than TS3's bake. It needs a style pass so it still feels like
  The Sims.
- **Correctness:** the new light has to follow the game's lamp data exactly: cones, colours, on/off states and edits.
  Apex already tracks most of it.

## Alternative

RTX Remix path tracing gives real GI out of the box. But it replaces the whole renderer, and TS3's shader-based draws
capture poorly in Remix. This idea keeps TS3's renderer and only replaces its lamp lighting.

## A first proof

One lot, direct light only:

1. Shadow-mapped lamps patched into the floor and wall shaders.
2. The room solve switched off for that lot.
3. Measured against today with the camera pan harness: p50, p99 and hitches per minute, at 40 m and 150 m.

If the hitches drop and the shadows look right, the irradiance volume comes next.
