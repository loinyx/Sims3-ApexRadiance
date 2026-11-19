# 08. Salvaging the rejected techniques

Desk work for [idea.md](../idea.md), 2026-11-17. No game run, no code change, no new web reading: every technique fact
comes from the research pages [02](02-shader-integration.md), [03](03-direct-light-pipeline.md),
[04](04-indirect-light.md), [06](06-budgets-and-proof.md), [07](07-remix-and-prior-art.md) and the profile
[00](00-camera-pan-profile.md), or from general knowledge of the papers they cite.

Rule applied here: a rejected technique is not dropped. For each one this page gives the exact reason it was rejected,
the mitigations that would make it work in The Sims 3 (with cost), and the role it fits best. Patents are not treated
as a constraint in this page (project decision); the LPV patent note in 07 §5 is therefore not a blocker.

Marks: **[V]** verified in one of the pages above (which cite source or a measurement), **[I]** inferred here, **[O]**
open, needs a probe, a capture or an offline test. All costs on this page are **[I]** unless marked. Effort is agent
work in hours; game runs are listed apart.

## 0. What all the salvages lean on

The rejections were written against generic engines. Seven TS3 facts change the answer:

| # | Fact | Mark | What it buys |
|---|---|---|---|
| F1 | Walls sit exactly on 1 m tile edges or tile diagonals; the lot proxy (`rt_lamp_proxy.h`) knows every wall, opening, slab and roof | [V] 04 §0 | Leaks can be stopped by construction (face masks, room ids), not by heuristics |
| F2 | Light reaches static surfaces through the room light maps, which Apex already rewrites on the GPU, texel by texel | [V] 03 §1, 02 §3.2 | Any technique can output into **texture space**: no shader coverage problem, no albedo problem, no per-frame cost |
| F3 | An Apex-owned full-screen pass has the INTZ depth, the camera view-projection (`c40..c43`) and no shader limits | [V] 03 §1 | A screen pass can rebuild world position and read the cube atlas, the probe atlas and the lamp table itself |
| F4 | Furniture exists as game data (rectangle and disc blocker records), not only as draws | [V] 03 §1 | Coarse furniture occlusion without redrawing the scene |
| F5 | The AO pass is full resolution, 4x4 interleaved with a box filter, no temporal filter, 1.3-4.5 ms at 4K | [V] 00 §7, §9 | A stable host for any short-range screen-space term; half-res and temporal variants are known failures |
| F6 | Never-locked DEFAULT-pool targets cost about 1 KB of game address space each | [V]/[I] 06 §1.1 | Atlases of any size are free for the 32-bit process as long as nothing is locked or read back |
| F7 | The frame is CPU and bridge bound (0.4 µs a state call, 1.5 µs a draw); the GPU is underused | [V] 00, 03 | Few draws with many pixels are cheap; anything on change only is nearly free |

The shared pattern: **move the rejected technique out of the place where it failed (the screen, the camera's draw set,
every frame) into texture space, lot data and on-change updates, and keep it to the range where its failure cannot
show.**

## 1. Light propagation volumes

**Rejected because** (04 §1.2): thin double-sided walls leak, which is the paper's own main artifact, and the paper's
fixes reduce it but do not remove it; on a small lot one bounce gathered directly is affordable, so propagation "buys
little"; and the per-pixel apply step needs a patched copy of every lit shader family.

**Mitigations**

| Problem | Mitigation | Cost |
|---|---|---|
| Wall leaks | Lot-attached grid aligned to the tile grid (F1). A **face mask** atlas: per cell, one opacity for each of the 6 faces, written from the proxy. The propagation shader multiplies the flux through a face by its mask, so a wall face passes exactly 0. Three cell layers a story (0-1, 1-2, 2-3 m) let a door be open in the lower two and a window in the middle one | Mask atlas of the grid's size, 1 or 2 RGBA8 targets, drawn from the proxy's wall list as lines into the slices (draws only, no lock): ~2-6 draws on an occluder change |
| Diagonal walls | Run the grid at 0.5 m and block the staircase of faces along the diagonal: topologically tight, the shape is off by at most half a cell. (Alternative: two SH sets for a split cell; more code, not needed first) | 4x the cells of a 1 m grid in plan; still small (below) |
| Floors and ceilings | The same mask on the y faces from the slab and roof quadrants of the proxy | In the same mask draw |
| Leak at the apply (trilinear across a wall) | **Do not apply per pixel.** The grid is read by the existing texel points (F2): each light-map texel samples the cell on its own side of the wall (position offset half a cell along the texel normal). Sims and rig-lit objects read it through the rig fill of 02 §4.2 | 6 bilinear taps a texel on change; 0 a frame |
| Shader coverage | Gone with the texel-space apply | - |
| Furniture occlusion | Geometry volume from the blocker records (F4): boxes and discs drawn as quads into a second opacity atlas, used as fuzzy occlusion in the propagation, as in the 2010 paper | 1 batched draw on an object change |
| Injection in SM3 | One point draw per lamp, position read from the reflective shadow map by vertex texture fetch, additive into 3 MRTs (the paper's Xbox 360 path). Shift the point half a cell along the surface normal | 1 draw a lamp on change; vertex texture fetch from the fp16/fp32 map through the bridge's DXVK [O], one offline probe |
| Reach of the propagation | Light travels one cell an iteration: a 16 m run needs 16 iterations at 1 m, 32 at 0.5 m. Spread at 4-8 iterations a frame | ~10 calls an iteration: 32 iterations ≈ 320 calls ≈ 0.13 ms CPU in total, on change only |
| Low angular resolution (L1 SH smears light around door frames) | Keep the **first bounce on the direct, room-scoped gather** of 04 §1.5 (sharp); give the grid only what gathering does badly: second and later bounces and light that crosses more than one opening | - |

Sizes: 64 x 64 tiles x 4 stories at 1 m x 3 layers = 49 k cells; at 0.5 m in plan and 3 layers, 197 k cells; 3 fp16
targets x ping-pong + accumulation ≈ 5-14 MB of VRAM, ~10 KB of address space (F6). GPU: 197 k cells x 32 iterations x
3 targets is far under 0.5 ms in total.

**Where propagation really wins (so it is more than a "no")**

1. **Lamp-count independence.** The gather of 04 §1.5 costs texels x VPLs. A community lot has 60-130 lamps (03 §5):
   at 16 VPLs a lamp that is 1,000-2,000 VPLs x 0.2-0.6 M texels, 10-20x the family-house cost (10-30 ms a re-bake
   instead of 1-3). The grid's cost does not depend on the lamp count.
2. **Multi-room transport.** Room A to hallway to room B through two doorways is one more opening hop than the gather's
   opening frusta handle. The masked grid does it for free.
3. **The probe grid for Sims is the same grid.** Stage S4 of 04 builds a tile-aligned L1 SH grid anyway; propagation is
   one more shader on it.
4. **Daylight through windows** (out of scope here, noted): sky flux injected at window cells gives interior daylight
   bounce with the same code.

**Best role:** transport layer behind the main path. Bounce 2 and later, multi-opening spill, the Sims' probe field,
and the **whole indirect solve on lots above ~40 lamps** (automatic switch by VPL count, or a quality setting).
Not the first-bounce carrier on a family house.

**Effort:** offline in `tools/rt_lamps_test` with a CPU reference and a leak test (lamp in room A, cells of room B with
no opening = exactly 0): 4-8 h. GPU passes and texel read: 6-10 h. 1-2 game runs. Most of it is shared with S4 if S4's
grid is laid out for it from the start (tile-aligned, 3 layers a story, face mask atlas).

## 2. Screen-space GI

**Rejected because** (04 §1.4, 00 §9): there is no G-buffer, so the bounce colour comes from the lit scene and
double-counts the existing light; off-screen lamps and surfaces contribute nothing, so the indirect light changes as
the camera pans; half-resolution gave "micro dots" and temporal smoothing gave "breathing and trails" in Apex's AO;
2-5 ms at 4K; light bleeds across depth edges.

**Mitigations**

| Problem | Mitigation | Cost |
|---|---|---|
| Sender colour | For the *sender* the lit scene colour is the right quantity: it is the light leaving that surface. No albedo needed there. A colour pyramid of the scene (one `StretchRect` + a few downsample draws, GPU only) next to AO's 1/z pyramid | ~10-15 calls a frame, ~0.2 ms GPU |
| Receiver albedo | Apply **multiplicatively**: `colour *= 1 + k * dE / (E + Emin)`, where `dE` is the gathered short-range bounce and `E` the irradiance at the pixel. `colour = albedo * E`, so the albedo is in the result without ever being known. `E` comes from the world-space solution: the pass rebuilds the world position (F3) and reads the probe atlas (section 1 / S4) | 6 more taps a pixel. Error: the game's tone curve sits between `E` and `colour`; small for a small `dE` [O], check on screenshots |
| Lights-off pass, MRT albedo | Not needed with the two rows above. (A second scene pass doubles the draws; an albedo MRT needs every shader's output rewritten: both stay rejected, 02 §3.4) | - |
| Double counting | Range limit: gather only within 0.5-1 m, below the resolution of the world-space layers (0.25-1 m texels, 0.5-1 m cells), and subtract the gather's own low-pass so the term is zero-mean. It adds colour detail, not energy | 1 mip tap |
| Off-screen light | By design the world-space layers carry it. The screen term only loses a band of 0.5-1 m at the screen edge; fade it out there | - |
| Micro dots, breathing | No half resolution and no temporal filter. Ride on the AO pass itself (F5): the same slices and steps also fetch colour (the visibility-bitmask screen-space indirect lighting of 07 §5 is this), and the existing 4x4 interleave + box filter does the denoise | +30-60% of the AO cost: **+0.5-1.5 ms at 4K** on the profile GPU, 2-2.5x on the laptop class |
| Bleed across depth edges | The AO pass already weights by depth; reuse its falloff and thickness rule | - |
| Reprojection with depth (asked for) | Possible for the static scene (previous `c40..c43` + depth, reject on depth mismatch), but moving Sims have no motion vectors and this is the filter Apex removed. Keep as a later opt-in, not in the first version | - |

**What only this layer can do:** bounce from things that are in no bake: a Sim's red dress on a white wall, a moved
chair, clutter with no blocker record. The world-space layers are static by design.

**Best role:** detail layer, a quality setting inside Ambient Occlusion ("bounce detail"), off at the low AO
qualities. Never a base. **Effort:** 4-8 h + 1 game run with the AO look bookmarks. Depends on the probe atlas for `E`.

## 3. Voxel cone tracing

**Rejected because** (04 §1.6): SM3 cannot voxelize (no geometry shader, no UAV), and the workaround is 64-128 scene
redraws through the bridge; a 2D slice atlas has no hardware 3D mips, so cone steps are expensive, 5-10 ms a frame at
4K; zero-thickness walls voxelize to nothing or leak at coarse mips; a CPU-voxelized proxy "is just a coarse occupancy
grid".

**Mitigations**

| Problem | Mitigation | Cost |
|---|---|---|
| Voxelization | From lot data, never from scene draws (F1, F4): the proxy's walls, slabs and roofs and the furniture blocker records, drawn straight into the slice atlas as lines and quads. Only on an occluder or object change | 2-6 batched draws on change; no lock |
| Zero-thickness walls | Store **face opacity per direction** (the anisotropic voxels of the original paper), not cell occupancy. A wall on a tile edge is an opaque face. This is the same atlas as the face mask of section 1: build it once, use it for both | Shared with section 1 |
| Radiance in the voxels | The same point injection from the reflective shadow maps as section 1 | Shared |
| No 3D mips | Manual mip chain in the atlas (1 draw a mip, on change). Downsample opacity with **max along the axis**, not the average, so a wall stays opaque in every mip. Limit the trace to 16 m and 3 mips (lots are small) | 3 draws on change |
| 5-10 ms a frame | **Trace per light-map texel on change, not per screen pixel per frame** (F2): 5 cones x 16 steps x 2 taps = 160 taps x 0.2-0.6 M texels = 30-100 M taps, 1-2 ms a re-bake, spread over frames, 0 when clean | Same order as the gather, but independent of the lamp count |
| Shader coverage | Gone with the texel-space trace | - |

**What it adds over section 1:** visibility along the cone. Indirect light is shadowed by furniture and partial walls
(dark under tables, behind sofas), and it does not have the numerical diffusion of propagation. It is the alternative
to the "imperfect shadow maps" of stage S5 for the same job, with a cost that does not grow with lamps or VPLs.
A per-pixel specular cone (glossy floors) is possible in principle but needs a patched floor family every frame: not
proposed.

**Best role:** quality setting for furniture-aware indirect shadowing, as the S5 visibility method **instead of**
imperfect shadow maps if those show light under furniture; and a cross-check of the propagation result offline.
**Effort:** 10-16 h on top of section 1's atlases (4-6 h if section 1 is built first) + 1 game run.

## 4. Screen-space shadows

**Rejected because** (03 §8): casters off screen or behind the camera leave the receiver lit; the depth buffer has
one layer, so a wall seen edge-on or cut away lets light through, "the exact bug class the old system's fixes target";
a lamp behind the camera cannot be marched to; accumulating lamps on screen needs albedo; 3-5 ms at 4K for 3 lamps;
depth-rebuilt normals gave dashed seams.

**Mitigations**

| Problem | Mitigation | Cost |
|---|---|---|
| Walls, off-screen casters, lamps behind the camera | **Hybrid: the screen march never decides whether a lamp reaches a surface.** Walls, slabs and roofs stay on the cached proxy shadow maps; furniture on the blocker records; both are complete (03 §2 row A). The march only adds a **contact term over 0.3-0.5 m**, where a wall cannot sit between caster and receiver. A ray that leaves the screen returns "no occluder": the coarse shadow underneath is still there | - |
| Off-screen Sims | Capsule shadows from the bone constants (03 §2 row E) evaluated analytically in the same pass or in texel space: complete for every Sim the game skins, no depth needed. The march adds the fine shape (feet, fingers, chair legs) only where visible | < 0.02 ms CPU (03 §6) |
| No albedo, light lives in the maps | Multiplicative, like section 2: the pass rebuilds the world position (F3), loops the nearest lamps from the lamp table, samples the cube atlas itself, and finds the 1-2 dominant lamps and their share `w` of the pixel's light. Then `colour *= 1 - w * occlusion`. Only the lamp share is darkened; the sun and ambient parts are not | 2 atlas taps a lamp, 4-8 lamps tested a pixel |
| Depth peeling (asked for) | A second depth layer means a second scene pass: +1,000-2,500 draws a frame, rejected by F7. The fixed thickness rule (0.1-0.3 m) plus the short range makes it unnecessary | - |
| Dashed seams, acne | Start the march 1-2 pixels out, bias by the pixel's footprint, no depth-rebuilt normals in the test | - |
| Noise | Full resolution, per-pixel jitter on the AO's 4x4 pattern, the AO's box filter (F5); no temporal filter | - |
| Double darkening with AO | Combine as `min(AO, contact)` on the lamp share, not a product | - |
| GPU cost | 8 steps x 2 lamps x 8.3 M px ≈ 130 M taps: **1-1.5 ms at 4K**; less with a mask (run only where a lamp share above a threshold exists: night interiors) | - |

**What only this layer can do:** shadows from everything with no blocker record and no cached mesh (clutter, plants,
table legs, a Sim's feet), exact in shape, with zero extra draws and no caster bookkeeping. It also sharpens the box
shadows of furniture near contact before section 6 exists.

**Best role:** detail layer of the direct-light lane, right after capsule shadows (03 §9 item 3 already hints at it;
this page makes it a defined stage). **Effort:** 4-6 h + 1 game run. Needs the lamp table in constants and the atlas,
both of which the direct-light lane has.

## 5. RTX Remix path tracing as a hybrid

**Rejected because** (07 §2-4): Remix has no hybrid mode: `rtx.enableRaytracing=false` is plain raster, `true`
replaces the frame and discards the game's pixel shaders; Remix builds its scene from fixed-function data, and TS3
sets no fixed-function transforms and no `SetLight`, and skins Sims in the vertex shader; a full conversion costs
every shader family and throws away TS3's look and Apex's work; no public TS3 attempt got past broken rendering; the
bridge's per-call cost becomes the limit.

**What each proposed variant breaks, and the way round it**

| Variant | What breaks | Mitigation | Cost |
|---|---|---|---|
| A. Lamps only through `exposeRemixApi` | Remix lights only exist in the path-traced frame. With ray tracing off they do nothing; with it on, the game's geometry is not in the Remix scene (shader-only draws), so there is nothing to light | Needs geometry: variant C | - |
| B. Remix runtime in the server instead of stock DXVK | The raster path becomes the Remix fork's DXVK, older than Apex's pin: INTZ, G16R16F atlases, the bridge patches 0002/0003/0008 and frame pacing are all unverified there [O]; needs a ray-tracing GPU with enough VRAM (the 6-8 GB laptop class is tight at 4K); the bridge package is Apex's to change | Ship only as a second, opt-in server profile; never the default; a compatibility pass of every Apex feature on that server | 8-16 h + several game runs before anything new is visible |
| C. Remix geometry from Apex's proxy, lamps from the lamp table | Feasible over the API: the proxy is one mesh of ~10 k triangles (`CreateMesh` once, `DrawInstance` a frame), furniture as boxes, lamps as sphere or spot lights. But the result is a path-traced picture of a **grey proxy world**, in screen space, inside the Remix runtime. The game's raster frame cannot read it: there is no texture-space or irradiance output that our D3D9 passes can sample | A fork of the runtime that exposes a raw irradiance output (`rtx.enableRawAovOutputs` exists) as a texture on the same device, plus running raster and path tracing in one frame | Days to weeks, and a fork to maintain; GPU cost of path tracing beside the raster frame |
| D. Full conversion (the GTA IV route) | Everything in the rejection | None that changes the size | Months |

Honest reading: once the scene is reduced to the proxy and the lamp table, there is nothing left that needs a path
tracer in real time. The SM3 gather solves the same scene in 1-3 ms a change. What Remix would add is correct
multi-bounce, soft area lights and furniture-exact visibility on a scene we can also path trace **offline**.

**Best role (specific cases, outside the staged plan)**

1. **Reference renderer.** A ground truth for the look and for the leak and brightness checks of the style pass: the
   same proxy + blocker boxes + lamp table, path traced. The cheapest form is not Remix at all but a CPU path tracer
   beside the existing CPU ray reference in `tools/rt_lamps_test` (4-8 h, no runtime, no bridge risk). Variant C is the
   in-game form of the same thing, only if someone wants it interactive.
2. **A future path-traced photo mode** (variants B + C, then real meshes through `CreateMesh` with skinning from the
   bone constants). Long term, Apex-owned, opt-in, and only once the bridge's server can host the Remix runtime.
   Trigger to reopen: Apex decides to ship a Remix server profile, or a runtime release adds a hybrid or
   irradiance-output mode.

## 6. Shadow casters from the camera's draws

**Rejected because** (03 §2): the camera's draws are only what the frustum and the level of detail produced this
frame, so a lamp lighting a room behind the camera has no mesh casters; caching per lamp then makes shadows "appear
and vanish as the camera pans"; holding the game's vertex and index buffers across frames risks freed buffers or
keeps streamed meshes (and their 32-bit client copies) alive; skinned and instanced casters need patched shaders and
captured constants.

**Mitigations**

| Problem | Mitigation | Cost |
|---|---|---|
| Missing casters | **Never missing, only coarser**: the proxy and the blocker box are always in the static tile. A mesh only *replaces* a box once it has been seen. The worst case is today's box shadow | 0 |
| Popping | Three rules. (1) The upgrade is box to mesh for one object, a small change, and it is crossfaded in texel space with the 0.2 s blend stage S1 already has. (2) Upgrade a lamp's tile only in a frame where the camera moves or the lamp's lit area is off screen; otherwise queue it. (3) Never downgrade: once a mesh is in, it stays until the object or the lamp changes | 1 queue; no GPU cost |
| Cache key | Object key = world x/z from `c8.w`/`c10.w` [V] + mesh identity (vertex buffer, start index, primitive count) + level of detail; keep the most detailed level seen | A small hash map |
| Buffer lifetime | Two options. **Bake-in:** replay the mesh in the frame it is first seen (reference held for one frame, 03 §2) into the static tile of every lamp in reach; the tile keeps the depth, nothing else is kept. **Record/replay** (the MGE XE pattern, 07 §5): keep a draw record with a reference for objects in reach of kept lamps on the active lot only, capped by count and bytes, released on the object-removed hook, on lot change and on device reset | Bake-in: ~6 draws an object once (10-20 µs), spread at ≤ 0.5 ms a frame. Record/replay: the same per rebuild, plus the bounded references |
| A rebuild loses the baked meshes (an edit near the lamp resets the tile to proxy + boxes) | This is why record/replay is the complete form: the tile is rebuilt from records, not from what is on screen. With bake-in only, off-screen objects fall back to boxes until seen again | 100-200 face-draws a lamp rebuild (03 §6), spread |
| Pre-warm by a hidden sweep at lot load (asked for) | A sweep means moving the game's camera: visible, and it costs frames. Two better sources: (a) the game's own **sun shadow caster pass** (03 §2 row D) may already draw the lot's casters with skinning and alpha test from a wider frustum [O, one frame capture]; (b) read the meshes from the scene's model list instead of from draws [O, static analysis, 4-10 h]. Either makes the cache complete at load with no sweep | 1 capture; or the analysis |
| Skinned Sims | Not cached at all: dynamic layer, capsules first, mesh replay as the high setting (03 §9 item 3) | 03 §6 |
| Foliage | Instanced, alpha tested: caster copy keeps the diffuse read and the alpha test (03 §2); outdoor lamps only | A few casters |

**Best role:** quality setting "exact furniture shadows" in the direct-light lane, after the proxy + box base and after
section 4 (which already hides most of the difference near contact). Bake-in first; record/replay only if the
box-after-edit fallback shows. **Effort:** bake-in 6-10 h + 1 game run (the camera sweep, for popping); record/replay
+4-6 h + 1 run with a 60 min memory arm.

## 7. Reflective shadow maps gathered on screen

**Rejected because** (04 §1.1): no visibility between the virtual point light and the receiver, so a lamp-lit wall
lights the next room through itself: "fatal for TS3 night interiors"; 26 G taps at full rate at 4K; and a forward
renderer leaves the gather either in patched lit shaders or in a screen pass with no albedo.

**Mitigations**

| Problem | Mitigation | Cost |
|---|---|---|
| Leaks | **Room scoping** (F1): each virtual light carries its room id (in a spare channel of the map); the receiver's room comes from the per-story tile room map. A pair in different rooms contributes 0 unless the light lies inside the frustum of an opening between them | A room-id texture and an opening table from the proxy |
| Tap count, albedo, coverage | Gather on **light-map texels on change**, not on screen pixels a frame (F2): 0.2-0.6 M texels x 256 clustered lights, 1-3 ms a re-bake, 0 a frame | 04 §1.5 |
| Furniture between light and receiver | Imperfect shadow maps of the proxy for the brightest clusters (S5), or the cone trace of section 3 | S5 |

This salvage is already the recommended main path of 04 (stages S0-S3): the texel-space bake **is** a room-scoped
reflective-shadow-map gather. Two leftovers are worth keeping from the screen variant:

1. **Virtual lights as rig lights.** The rig of a Sim or object has 4 overflow "vertex light" slots (02 §4.1). Fill
   them with the 4 strongest clustered virtual lights of the model's room, scaled by visibility at the model's centre:
   coloured, directional bounce light on Sims with no shader patch and no GPU pass. It is a variant of 02's rig fill
   (route A) that needs no probe grid, so it can land before S4. 2-4 h on top of the cluster table.
2. **A masked screen gather for Sims only** (the Sim receiver mask exists, 03 §1): per-pixel bounce by normal on under
   5% of the screen, applied multiplicatively as in section 2. A high setting at most; the rig route covers the need.

**Best role:** main path (as texel-space gather), plus the rig-light variant for Sims.

## 8. Screen-space light buffer written before the draws

**Rejected because** ([02 §3.1](02-shader-integration.md), §5 item 4): it needs this frame's depth before anything is
drawn, and the only sources are a depth pre-pass (+1,000 to +2,500 draws a frame on a render thread that is the limit)
or last frame's depth, which breaks at every newly uncovered pixel during a pan and lags on moving Sims; `ps_2_0` has
no `vPos`; one value a pixel is wrong for blended layers (glass, hair, particles) and MSAA edges; the room map's alpha
(ambient weight) and the four directional maps have no place in one RGB buffer; mirrors, the water reflection,
thumbnails, portraits and the pick pass each need their own buffer; and every family still needs a patched copy.

02 recommends the **texel-space light buffer** instead: Apex-owned float copies of the room maps and the four
directional maps, bound by the existing `SetTexture` swap. For every room-map-lit surface that buffer wins on each
rejected point (all views, both shader models, alpha and directional maps kept, 0 a frame), so no salvage should compete
with it there. The salvage is for what the texel buffer cannot reach: **surfaces with no light map** (Sims, outdoor
rig-lit objects, foliage) and **detail finer than a texel**.

**Mitigations**

| Problem | Mitigation | Cost |
|---|---|---|
| Depth before the draws | **Move the buffer to after the draws** and apply it multiplicatively (sections 2 and 4): this frame's INTZ depth exists then (F3), nothing is stale, no game shader is patched. The game draws rig-lit surfaces with the rig fill of 02 §4.2 (lamps unshadowed at the model's centre); the pass multiplies each such pixel by `V(pixel) / V(centre)` for the 1-2 dominant lamps, read from the cube atlas at the pixel's world position. Result: wall and furniture shadows fall across a Sim, which 02 lists as the benefit of a SimSkin patch (654 shaders), without that patch | Same pass and lamp loop as the contact shadows of section 4: +2-4 PCF taps a lamp on masked pixels only (Sims and rig-lit objects are a small part of the screen): ~0.2-0.5 ms at 4K |
| Which pixels, which rig | The Sim receiver ids of the AO pass [V, 02 §1.3] give a mask and an id; a small constant table gives each id's rig centre and dominant lamps (`RigTracker` knows them at draw time) | A few constants a frame; rig-lit objects need an id too [O] |
| Proxy-only depth pre-pass (asked for) | Cheap (1-3 draws of the proxy), but it yields depth only for walls, floors and roofs: exactly the surfaces the texel buffer already serves better. No use | - |
| Last frame's depth + per-pixel fallback to the texel buffer (asked for) | Works on paper for room-map surfaces (store the depth the value was computed for in the buffer's alpha, compare in the shader, fall back to the map read), but it costs a patched copy of every family and an interpolator, to gain only sub-texel shadow edges. The cheaper way to that gain is a **higher map resolution on change** (4 to 8 texels a metre for floors near the camera; the GPU is idle and it is 0 a frame) | Not proposed; raise the texel density instead |
| `ps_2_0`, families that fail to patch | No game shader is touched | - |
| Blended layers, MSAA edges | Blended draws are not in the mask and keep the rig fill; edge pixels take the resolved depth (a soft error one pixel wide) | - |
| Alpha and directional-map semantics | Not involved: the pass scales the final colour of rig-lit pixels only | - |
| Mirrors, reflections, thumbnails, portraits | Main view only. Other views show the rig-fill base, which is already consistent with the room | - |
| Sun and ambient share must not be darkened | Scale only the lamp share `w` of the rig's light (known from the rig table), as in section 4 | - |

**Best role:** specific case. Per-pixel lamp shadows on Sims and rig-lit objects, after the scene, as the high setting
above the rig fill and **instead of** the per-pixel SimSkin patch (02 §4.2 route C). It is one branch in the section 4
pass, not a pass of its own. **Effort:** 2-4 h on top of section 4 + 1 game run. As a main path it stays rejected; the
texel-space buffer is the main path.

## 9. Ranking by value over effort

The carrier is fixed first: the **texel-space light buffer** of 02 §3.2 (Apex-owned float copies of the room maps and
directional maps, bound by the `SetTexture` swap) for static surfaces, and the rig fill for movers. It is rank 0: every
option below either fills it (ranks 1, 4, 8), feeds the rig (rank 2) or is a multiplicative layer on top of its
result (ranks 3, 5, 7), or sharpens its casters (rank 6). None replaces it.

| Rank | Salvaged option | Value | Effort | Role |
|---|---|---|---|---|
| 0 | Texel-space light buffer + rig fill (02 §3.2, §4.2; not a salvage) | Reaches every room-lit family, all views, 0 a frame | Extends shipped code | Carrier |
| 1 | Room-scoped reflective-shadow-map gather in texel space (§7) | Whole indirect base, leak-free, 0 a frame | Already planned (S0-S3) | Main path |
| 2 | Virtual lights into the rig's vertex-light slots (§7.1) | Sims and objects match the room, no patch | 2-4 h + 1 run | Main path for movers, before S4 |
| 3 | Screen-space contact shadows on the lamp share (§4) | Exact small shadows, no caster cost | 4-6 h + 1 run | Detail layer, direct lane |
| 4 | Wall-masked propagation grid (§1) | Big lots, multi-room spill, bounce 2+, the Sims' probe field | 10-18 h + 1-2 runs, mostly shared with S4 | Transport layer; automatic above ~40 lamps |
| 5 | Shadow modulation on rig-lit pixels after the scene (§8) | Wall and furniture shadows across Sims, no SimSkin patch | 2-4 h after rank 3 + 1 run | Specific case, high setting |
| 6 | Persistent caster cache, bake-in (§6) | Mesh-exact furniture shadows | 6-10 h + 1 run (+4-6 h record/replay) | Quality setting |
| 7 | Screen-space bounce detail inside AO (§2) | Colour bleed from movers and clutter | 4-8 h + 1 run | Detail layer, quality setting |
| 8 | Cone trace per texel on the face-opacity atlas (§3) | Furniture-aware indirect shadowing | 4-6 h after rank 4, else 10-16 h | Quality setting; S5 alternative |
| 9 | Path-traced reference, offline first (§5) | Ground truth for the style pass | 4-8 h offline; Remix forms days to months | Tool; photo mode long term |

## 10. Where they go in the staged plan

Stages S0-S6 are those of [04 §3](04-indirect-light.md); the direct-light steps are those of
[03 §9](03-direct-light-pipeline.md).

| Stage | Change from this page |
|---|---|
| Direct lane, after item 3 (capsule shadows) | **New step D-contact:** screen-space contact shadows on the lamp share (§4) |
| Direct lane, after D-contact, once the rig fill exists | **New step D-rigshadow:** shadow modulation on rig-lit pixels (§8), in the same pass |
| Direct lane, item 2 (mesh-exact furniture) | Defined as the bake-in caster cache with the three anti-popping rules (§6); record/replay as its follow-up. The frame capture of the sun shadow pass (03 §10 #1) is done first: it may make the cache complete at load |
| S1 | Add a measured line: re-bake cost against lamp count, on a lot with 60 or more lamps. It is the gate for the propagation grid |
| S3 (openings + second bounce) | If S1's line is over budget on big lots, or two-doorway spill looks wrong: the wall-masked grid (§1) takes bounce 2+ and multi-opening transport instead of a second gather |
| Between S3 and S4 | **New step S3b:** virtual lights into the rig's vertex-light slots (§7.1) |
| S4 (probe grid) | Lay the grid out for propagation from the start: tile-aligned, 3 layers a story, 0.5 m in plan if diagonals matter, with the face-opacity atlas. Costs nothing extra now and makes §1 and §3 one shader each later |
| S5 (visibility + detail) | Two options named: cone trace per texel (§3) as the alternative to imperfect shadow maps; bounce detail inside AO (§2) as the detail term |
| S6 (style pass) | Uses the offline path-traced reference (§5) for brightness and leak comparison |
| Outside the plan | Remix server profile and photo mode (§5), reopened only on the triggers named there |

## 11. To verify before building any of it

| # | Question | How | For |
|---|---|---|---|
| 1 | Vertex texture fetch from the fp16/fp32 shadow atlas through the bridge's DXVK | Offline probe on the harness device | §1, §3 injection |
| 2 | Error of the multiplicative apply behind the game's tone curve | Offline: apply to captured frames with a known added light | §2, §4 |
| 3 | Re-bake cost against lamp count on a community lot | S1 measurement | §1 gate |
| 4 | Does the sun shadow caster pass run at night, with which frustum | One frame capture (03 §10 #1) | §6 pre-warm |
| 5 | Can the rig's vertex-light slots take arbitrary positions and colours each frame | Read the rig gather (02 §6) | §7.1 |
| 6 | Diagonal walls: is the 0.5 m staircase mask tight in the CPU reference | Leak test in `tools/rt_lamps_test` | §1, §3 |
| 7 | GPU cost of colour taps in the AO slices on the laptop GPU class | AO bench with the extra fetch | §2 |
| 8 | Do rig-lit objects (not only Sims) have a per-pixel id the pass can read | Read the AO receiver-id code | §8 |
