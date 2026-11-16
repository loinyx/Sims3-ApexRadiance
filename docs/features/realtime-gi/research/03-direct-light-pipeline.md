# R3: the direct light pipeline (shadow-mapped lamps) under D3D9 SM3 and the RTX Remix bridge

Desk research for [idea.md](../idea.md) part 1 ("Direct light: shadow-mapped lamps"). No game run, no code change. Every
claim is marked **[V]** verified (in Apex source or a measured doc, linked), **[I]** inferred (reasoning or general
D3D9/DXVK knowledge, not checked here) or **[O]** open (needs a probe or a capture).

Cost inputs from [00-camera-pan-profile.md](00-camera-pan-profile.md) §7 and the bridge doc: a device call costs about
0.35-1.1 µs on the render thread, `DrawIndexedPrimitive` 1.2-2.6 µs, and Apex's own draws added 0.13-0.37 ms a frame
in the driver [V]. `GetRenderTargetData` is a ~2 ms synchronous copy across processes [V]. For planning: **0.4 µs per
state call and 1.5 µs per draw** issued through `CallOriginal*` (Apex's own hooks skip them) [I].

## 1. What Apex already has (the starting point is not zero)

| Piece | Where | What it does today | Status |
|---|---|---|---|
| Cube shadow atlas per lamp | `features/rt_lamp_gpu.{h,cpp}`, `shaders/rt_lamps_hlsl.h` | 6 faces × 256² per lamp, side by side in a G16R16F atlas (r = distance / far, g = the surface plane's distance from the lamp / far) with a D24X8 depth test; 16 lamps an atlas (1536 × 4096), near 0.005 m, far 64 m | [V] shipped (Instant outdoor lamps 2.13.0, GPU room lamps 3.0.0) |
| Kept atlas for indoor lamps | `PhysTiles`, [gpu-room-lamps.md](../../night-lighting/gpu-room-lamps.md) item 20 | Least-recently-used tiles, grown 4 at a time up to 120 (3.1 MB a tile: 75 MB at 24, 377 MB at 120), kept while the lamp stays put and the occluders are the same; a lamp past the cap cycles | [V] |
| Occluders | `features/rt_lamp_proxy.h` | A CPU-built proxy mesh from the story grids: walls with door/window holes, roofed quadrants, 0.2 m floor slabs. Rendered with **one `DrawPrimitive` per cube face** (`RenderShadows`: 6 draws a lamp) | [V] |
| Furniture shadows | `rt_lamp_blockers.h`, `T3dPS` | Not in the cube map: the game's own 3D test (rectangle and disc blocker records) evaluated analytically per texel; table legs added by `ObjectLegs` | [V] |
| Soft shadows (PCSS) | `Shadow()` in `rt_lamps_hlsl.h` | Blocker search on the cone rim (24 taps), then 16 emitter points × up to 16 walk steps through the map; 8/4/8 while dragging. Within 2/255 of a CPU ray reference at 97.4-100% per lamp | [V] |
| Where the light lands | `RelightKept*`, `Compose*` | **Light-map space**, not screen space: one point per room-map texel, recomputed only when a lamp or occluder changes, then bound in place of the game's map by a `SetTexture` swap (0.064 ms a frame for 3728 binds) | [V] |
| GPU cost of a relight round | gpu-room-lamps.md item 32 | 47 maps, 436,140 points, 3 lamps: 10.9 → 7.5 ms GPU on an RTX 4070 Ti SUPER (bound by the longest texel walk) | [V] |
| Re-issuing a game draw | `RecordSimReceiver` in `patches/ambient_occlusion_patch.cpp` | Inside the game's draw callback: swaps the render target, binds a bytecode-patched copy of the game's VS/PS (`ShaderPatches::MakeAoReceiverMask`), calls `CallOriginalDrawIndexedPrimitive` with the game's own VB/IB/constants, restores ~12 states, RT, DS, viewport, scissor | [V] shipped (Sim Occlusion), works under the bridge |
| Readable scene depth | `features/depth_share.h` | INTZ texture replacing the game's depth-stencil; AO builds an R32F 1/z pyramid from it | [V] |
| Game's own sun/moon shadow map | [engine/shaders.md](../../../engine/shaders.md) `s5` | 4096² at the tested settings, sampled with 4 taps (16 in Apex's roof copy); so the game has a shadow caster pass | [V] that the map exists; its caster pass is undocumented [O] |

**Not there [V]:** no deferred record/replay of the game's draws. Every "replay" in the source is the offline lighting
scene replay (`scene_record.h`, `tools/scene_replay`), which stores texels and lamp laws, not draws. Hooks see
`DrawIndexedPrimitive`, `DrawPrimitive`, shaders, textures, render targets, viewport and `Set*ShaderConstantF`;
`SetStreamSource`, `SetIndices`, `SetVertexDeclaration` (trace-only detour) and `SetStreamSourceFreq` are not on the
registry, so a recorder would read them with `Get*` at draw time or add detours.

So the idea's step 1 is mostly an **extension** of a shipped engine: more casters in the existing atlas, and new
receivers (objects and Sims) that sample it.

## 2. Where shadow casters can come from

| Source | Casters | Shape | Complete (off-screen, behind camera) | Per-frame cost | Verdict |
|---|---|---|---|---|---|
| A. Proxy from game data (today) | Walls, roofs, slabs; furniture as blocker boxes/discs | Boxy for furniture | Yes: built from lot data, not from draws [V] | 0 when cached; 6 draws a lamp to rebuild | Keep as the base layer |
| B. Inline re-draw of the game's draws (Sim Occlusion pattern) | Anything the camera draws this frame | Exact mesh, alpha-tested foliage, skinned Sims | **No**: only what the camera's frustum and LOD drew this frame [I] | ~14 state calls + 1 draw per lamp face per game draw | Only for dynamic casters |
| C. Record now, replay at one point (AddRef the VB/IB for one frame) | Same as B | Exact | No (same draw set) | ~5-8 calls + 1 draw per face-draw; one RT switch per tile instead of per game draw | Best for Sims |
| D. Re-target the game's sun-shadow caster pass | What the game puts in its 4096² sun map | Exact, with the game's own caster shaders (skinning, alpha test) | Wider than the camera frustum if the sun frustum is [O] | Same as C | Worth one capture to check |
| E. Analytic capsules for Sims (bone matrices seen through `SetVertexShaderConstantF`) | Sims only | 8-10 capsules a Sim | Yes for Sims the game skins this frame | ~0: a few constants a lamp batch | Cheapest Sim shadow |

Points that decide it:

- **Visibility of casters [I].** A lamp lights rooms the camera does not see (behind the camera, upstairs, through a
  wall cut-away). Anything built from the camera's draws (B, C) is incomplete, and caching it per lamp makes shadows
  appear and vanish as the camera pans. The proxy (A) is complete. So static shadows stay on A, and mesh-exact shapes for
  furniture are an *addition* only where a mesh was seen (needs an object key per draw: `c8.w`/`c10.w` = the object's
  world x/z [V] is a usable key) and a box is used otherwise.
- **Lifetime [I].** Holding the game's VB/IB pointers across frames without a reference risks a freed or reused buffer;
  holding a reference keeps lot-streamed meshes alive, and a MANAGED buffer keeps a CPU copy in the 32-bit client under
  the bridge. AddRef for one frame (record → replay → Release) is safe and costs nothing.
- **Skinned Sims [V/I].** The camera view-projection is `c40..c43` in every scene draw [V], but some families also carry
  it in `c0/c4/c180/c192/c216` [V]. So a caster copy needs a bytecode-patched VS that outputs lamp-face clip space (the
  same kind of patch `MakeAoReceiverMask` does) [I]. Bones are VS constants that the game sets before each draw, so a
  deferred replay must also copy them in the `SetVertexShaderConstantF` hook (Apex hooks it already) [I].
- **Foliage alpha test [I].** Shrubs and trees are instanced vs_2_0 (`c27..c29[a0]` per instance [V]); the caster copy
  needs `SetStreamSourceFreq` state (read with `GetStreamSourceFreq` at draw time) and a PS copy that keeps the diffuse
  `texld` + alpha test (`clip` or `ALPHATESTENABLE`). Outdoor lamps are the ones that meet foliage; few casters.
- **The game's sun shadow pass (D)** is the most interesting unknown: it already has caster shaders for skinned Sims and
  alpha-tested leaves. One Frame Capture of a night frame answers its draw count, its frustum and whether it runs at night [O].

## 3. Point-light shadow map layout

| Layout | Draws per caster | Quality | D3D9 / bridge notes | Verdict |
|---|---|---|---|---|
| Cube texture (6 faces as RT) | 1 per face it touches (avg ~1.5-2 with per-face culling [I]) | Uniform, no distortion | Colour cube RTs are fine; depth-format cube textures are not portable in D3D9, so no hardware PCF [I]; one sampler per lamp, and 16 samplers are already tight in some families (idea.md) | No |
| **2D atlas of cube faces (today)** | Same as cube | Same as cube; face seams need the face-select math (`Stored()` does it) [V] | One texture and one sampler for all lamps; a depth atlas (INTZ/DF24) would allow hardware PCF per face | **Yes** |
| Dual paraboloid | 1 per hemisphere touched (~1-1.3) | Straight edges bend between vertices: the proxy's big wall quads and low-poly furniture need CPU tessellation; worse resolution at the rim | Saves maybe 25-40% of caster draws after cube face culling [I], not 3× | No, unless draws dominate |
| Single 2D frustum (spots) | 1 | Best per texel | Only for cones ≤ ~120°; TS3's "shaded" lamps (type 5) have two opposite cones plus spill [V], so need the cube | Use the cube with faces culled by the cone instead: one code path |

Formats under D3D9 SM3 (the bridge's server is stock DXVK x64 [V]):

| Format | Use | Notes |
|---|---|---|
| G16R16F colour + D24X8 (today) | distance + receiver-plane term, manual compare | [V] works under the bridge; half float: ~1e-3 relative, ~5 mm at 5 m [I] |
| R32F colour + D24X8 | distance only | [V] used by AO; 4 B/texel, same as G16R16F |
| INTZ / DF24 (FOURCC) depth texture | sampled depth, manual compare | INTZ [V] works (DepthShare); stores non-linear depth, so a cube face needs its own projection per face [I] |
| D24X8 / D16 sampled as texture | **hardware PCF** (2×2 bilinear compare per tap) | DXVK does depth-compare sampling for these depth formats, not for INTZ/DF* [I: DXVK behaviour, probe on the bridge [O]]; makes a PCF tap ~4× cheaper |

Atlas memory: 3.1 MB a tile (6 × 256² × (4 B colour + 4 B depth)) [V]; 24 lamps 75 MB, 120 lamps 377 MB [V].

## 4. Caching and invalidation

Two layers a lamp (the static one exists [V]; the dynamic one is new):

| Layer | Contents | Rebuilt when | Cost |
|---|---|---|---|
| Static tile | Proxy + furniture (boxes, meshes where seen) | Lamp moved/added; occluder generation changed (the lot geometry key [V]); an object within the lamp's far radius placed/moved/removed (object edit hooks Apex already uses for `LampMarkFilter`/`RoomLampMove` [V]); a mesh seen for the first time near the lamp | Proxy 6 draws; meshes ~100-200 face-draws [I] |
| Dynamic tile | Static tile copied (1 `StretchRect` per lamp tile, GPU only) + Sims (+ doors mid-swing) | Every frame, **only** for lamps that have a moving caster inside their reach **and** light something on screen | See §6 |

- Colour/intensity/on-off edits change no tile (only the lighting constants) [V: today's tiles are kept "while the lamp
  stays put and the occluders are the same"].
- Spread rebuilds over frames with a CPU budget, as RtOutdoorLamps already does ("starts maps only while its relight work
  stays under about 2 ms" [V]); 1-2 static tiles a frame.
- Never read anything back: no `GetRenderTargetData` (2 ms a call [V]), no occlusion-query waits.

## 5. Lamps per lot (what K to plan for)

| Data point | Lamps | Source |
|---|---|---|
| One active-lot relight in the field | 28 lamps | rt-outdoor-lamps.md item 5 [V] |
| Probe dumps of 02/11 (several lots) | 235 lamps listed by 77 rooms | gpu-room-lamps.md item 21 [V] |
| Drag bench | 120-130 lamps (large community lot) | gpu-room-lamps.md item 20 [V] |
| Engine limits | 16 lamps a batch, 64 a map, 120 kept tiles | [V] |
| A room's list | up to 32 lamps | level-light-share.md [V] |

Planning figures [I]: a furnished family house **K = 15-30** lamps on the lot (indoor + outdoor), of which **6-10**
light something on screen at a time and **2-4** reach any one pixel. A community lot 60-130. Objects already pick at
most 3 lamps (rig) or 8 (`SelectPixelLamps`) [V].

## 6. Estimate: CPU ms and draws a frame, typical lit lot at night

Assumptions [I]: K = 20 lamps on the lot, 8 on screen; 4 Sims near lamps, each ~6 caster draws (body, head, hair, top,
bottom, shoes; eyes/lashes skipped), each Sim inside ~2 lamps' reach, touching ~1.5 cube faces per lamp; game frame 1000-1600
draws at 40 m.

| Item | Draws / frame | Other calls / frame | CPU ms / frame | When |
|---|---|---|---|---|
| Static tiles, steady | 0 | 0 | 0 | Camera still or panning, no edits |
| Static tile rebuild, proxy only | 6 a lamp | ~30 a lamp | ~0.02 a lamp | On edit |
| Static tile rebuild with meshes | 100-200 a lamp | ~500-1000 | 0.4-0.8 a lamp; spread at 1-2 lamps a frame | On edit; cold lot load: 20 lamps ≈ 8-16 ms total over 10-20 frames |
| Sims, mesh, deferred replay (C) | 4 × 6 × 2 × 1.5 ≈ 72 | ~72 × 4 (VB/IB/decl/bones) + 8 tiles × 6 (RT, viewport, copy) ≈ 340 | 72 × 1.5 µs + 340 × 0.4 µs ≈ **0.25 ms** | Every frame while Sims move |
| Sims, mesh, inline (B) | 72 | 24 game draws × ~28 (save/restore) + 72 × 2 ≈ 820 | ≈ **0.45 ms**, plus a render-pass break per game draw on the GPU | Same |
| Sims, capsules (E) | 0 | ~2-4 constant uploads a lamp batch | **< 0.02 ms** | Same |
| Receivers: lamp list in constants | 0 | 0 a draw if set once a frame in PS registers no game shader writes [O]; else +1 a draw ≈ +0.4-0.6 ms at 1000-1600 draws | 0.01 or 0.5 | Every frame |
| Room-map receivers (walls, floors) | 0 | `SetTexture` swap as today | 0.06 [V] | Every frame |
| **Total, steady, Sims as capsules** | **0** | **~20** | **~0.05** | |
| **Total, steady, Sims as meshes (C)** | **~70-100** | **~350** | **~0.3** | |

Against what goes away: the game's room solve 2.8 ms a frame on average (hitches to 60 ms) and Apex's per-draw
lighting fixes 1.8-3.4 ms [V]. Even the mesh option is a CPU win of ~4 ms a frame **if** those really go away; the
risk is not the shadow maps but receivers that still need per-draw constants.

GPU side [I, RTX 4070 Ti SUPER class; laptop RTX 30 roughly half]:

| Work | Cost |
|---|---|
| Static tiles in light-map space with full PCSS (today's `Shadow()`, up to 280 taps) | 7.5-10.9 ms a full relight round for 47 maps, 3 lamps [V]: fine because it only runs on change and is spread |
| Receivers per pixel, PCF only (4 hardware-compare taps a lamp, 3 lamps, overdraw 1.5) | ~1-2 ms at 4K, ~0.3-0.6 at 1080p |
| Receivers per pixel, PCSS-lite (16 blocker + 16 PCF taps a lamp) | ~6-12 ms at 4K, ~1.5-3 at 1080p: too heavy for every pixel at 4K |
| Dynamic tile copies (8 × 3.1 MB) | < 0.1 ms |

So **PCSS stays where it is cached (light-map texels)**; per-pixel receivers (objects, Sims, and the Sims' shadows on
floors) take plain PCF, or PCSS only on the dynamic layer's footprint.

ps_3_0 limits that matter [V from today's shaders]: 32 temporaries (the 3D test already had to move to its own pass),
loops of at most 255 iterations (nested loops used to go past it), 16 samplers. A lamp loop × PCSS loop fits with
`[loop]`; one atlas sampler for all lamps is what keeps families that already use all 16 samplers patchable.

## 7. Address space (32-bit game)

- Under the bridge a DEFAULT texture or render target lives in the 64-bit server; the client keeps CPU copies only of
  what is locked (MANAGED, SYSTEMMEM, locked buffers) [V: bridge doc §"How", patches 0002/0003]. So a 377 MB atlas
  costs the game process a handle, not 377 MB [I: the handle size; check with patch 0002's per-pool counters [O]].
- Today's code already moved its textures and VBs from MANAGED to DEFAULT for this reason ("no system-memory copy below
  the 2 GB line"; filled through a SYSTEMMEM texture released at once) [V]. The new layers must follow the same rule and
  be recreated at device reset.
- Recording game draws must not hold references past one frame (§2), or lot-streamed meshes stay alive.
- Without the bridge (in-process DXVK or native d3d9) VRAM-only resources are not mapped into the process either [I];
  the earlier measured headroom (1.6 GB free, largest block 1.38-1.42 GB [V]) is not at risk from tens of MB of handles.

## 8. The alternative: screen-space lamp accumulation with ray-marched shadows

A full-screen pass per lamp batch over the INTZ depth, normals rebuilt from depth (as AO does), each pixel marching
8-24 steps toward each lamp in the depth buffer.

| | Shadow-mapped lamps (this design) | Screen-space accumulation |
|---|---|---|
| CPU / bridge | ~0.05-0.3 ms (above) | ~20-40 calls a frame: < 0.05 ms [I] |
| GPU at 4K, 3 lamps a pixel | 1-2 ms PCF receivers (+ cached texel PCSS) | 16 steps × 3 lamps × 8.3 M px ≈ 400 M fetches ≈ 3-5 ms; half resolution gave "micro dots" in AO [V] |
| Casters off screen or behind the camera | Shadowed | **Lit**: no information |
| Walls between lamp and receiver (the next room's lamp) | Blocked by the proxy | **Leaks**: the depth buffer has one layer; a wall seen edge-on or cut away lets light through. This is the exact bug class the old system's fixes target |
| Lamp behind the camera | Fine | Marching leaves the screen: unknown → lit |
| Material response | Light-map receivers keep the game's shading; objects get the lamp through their patched shader | Needs albedo per pixel: the game's forward shaders output final colour only; an MRT output from every lit family, or the light is added in the wrong place |
| Removing the old lamp light | Same as this design (room maps swapped) | Same, plus nowhere to put the result for surfaces read from room maps |
| Seams | Shadow acne/bias only | Depth-rebuilt normals gave dashed seams in AO [V] |

Verdict [I]: not a replacement. Useful as **contact shadows** for the Sims' feet and small props on top of the shadow
maps (one short march, 8 steps, only near casters), where its failures do not show.

## 9. Recommendation

1. **Keep light-map-space shading with the cached cube atlas for every room-map receiver** (walls, floors, ceilings,
   terrain on the lot). It exists, is verified against a CPU reference, costs 0 draws when nothing changes, and is where
   full PCSS is affordable. The idea's "patch every lit shader family" is not needed for these surfaces: the texture swap
   already reaches them.
2. **Static casters: the proxy plus the blocker records first.** Mesh-exact furniture and foliage shadows only as a second
   step, recorded from camera draws and keyed by object (`c8.w`/`c10.w`), with the box kept for objects never seen.
3. **Sims: capsule shadows from bone constants first** (≈0 CPU), mesh replay (C, ~0.25 ms) as the high setting.
   Inline re-draw (B) only if replay proves impossible; it costs about twice as much and breaks the render pass per draw.
4. **Atlas of cube faces, 256² a face; try D24X8 sampled with hardware compare** for per-pixel receivers on the bridge
   (one probe). No cube textures, no dual paraboloid.
5. **Per-pixel receivers (objects, Sims) use PCF, not PCSS**, sample the one atlas through one sampler, and read the lamp
   list from constants set once a frame. Whether a high PS register range survives the game's draws is the first thing to
   check [O]; if not, the per-draw constant cost decides the whole CPU budget.
6. **Look at the game's sun-shadow caster pass in one Frame Capture** [O] before building any recorder: if it runs at
   night and covers the lot, its draws are ready-made casters with skinning and alpha test.
7. Budget rule: static rebuilds within RtOutdoorLamps' existing ~2 ms CPU spread; dynamic layer only for lamps with a
   moving caster in reach that light something on screen; no read-backs.

Expected for a K = 20 lot at night: **0-100 extra draws and 0.05-0.3 ms CPU a frame**, against 4-6 ms of room solve and
per-draw fixes that this replaces. The GPU adds 1-2 ms at 4K (PCF receivers) plus cached relights on edits.

## 10. Open questions for a probe or capture (no game run done here)

| # | Question | How |
|---|---|---|
| 1 | Does the game's sun/moon shadow pass run at night, how many draws, what frustum? | Frame Capture of a night frame |
| 2 | Is there a PS (and VS) constant range no game shader writes, so the lamp list can be set once a frame? | Scan the shader census for the highest register each family reads/writes; a capture of `Set*ShaderConstantF` ranges |
| 3 | Does D24X8 sampled as a texture give hardware PCF through the bridge's DXVK? | `rt_lamps_test`-style offline probe on the bridge |
| 4 | Address-space cost of a DEFAULT render target in the bridge client | Patch 0002's per-pool counters before/after creating 100 tiles |
| 5 | How many caster draws a Sim makes, and which VS registers hold its bones | Frame Capture with a Sim near a lamp |
| 6 | GPU cost of 72 Sim face-draws into the atlas plus the copies | Offline bench on the harness device |
