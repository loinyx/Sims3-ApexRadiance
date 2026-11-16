# Real-time GI: reconciled design

Status: In development (design only, no code). Game build: Steam 1.67.2. This page reconciles [idea.md](idea.md) with
the research pages [00](research/00-camera-pan-profile.md) to [12](research/12-prior-art-lighting-gi.md). The work
breakdown is in [plan.md](plan.md); progress is tracked in [handoff.md](handoff.md).

Marks: **[V]** verified in a research page (which names its source), **[I]** inferred, **[O]** open: a work package
in the plan closes it. A number without a mark is a target, not a measurement.

## 1. What the player gets

With the switch on, lamp light on a lot is computed by Apex Radiance on the GPU: every lamp casts shadows shaped by
walls, floors, furniture and Sims, light bounces off surfaces into the room, and a lamp edit shows at once. The
picture keeps the game's materials, sun, sky, tone curve and bloom. The game's room solve stops refining rooms of
that lot, which removes the largest cause of hitches while the camera moves [V, 00 §4]. On a ray-tracing GPU behind
the RTX Remix bridge the light is solved with hardware rays in the bridge's 64-bit process; other setups get the
same maps from Direct3D 9 passes. Everything is off by default.

## 2. Decisions that differ from the idea

| # | Idea | Decision | Why |
|---|---|---|---|
| D1 | Throw away the room light maps; patch every lit shader family to read the new light | **The room-map slots stay as the carrier.** Apex renders the new light into its own copies of the room maps and the four directional maps and binds them with the existing `SetTexture` swap. The game's shaders are not changed for room-lit surfaces | About 1,200 room-map pixel shaders in 40 techniques, 480 of them `ps_2_0` and full; per-family patching accepts 60-96%, never 100%, and keeps the per-draw CPU cost [V, 02 §1-2]. The swap reaches every family, both shader models and every view at 0.064 ms a frame [V, 02 §3.2] |
| D2 | The room solve no longer runs at all | **The solve is capped, not skipped:** each room of a GI lot is solved once at class 0 so lot load stage 20 passes, and never refined. The level rebuild, both wall AO solvers and the synchronous impostor path stay | A lot whose rooms never leave state 4 never finishes loading; the lot thumbnail waits for class 2 [V, 05 §4] |
| D3 | A per-lot irradiance volume filled by light propagation, sampled per pixel | **Indirect light is solved per light-map texel, on change, and stored as one linear layer a lamp.** A probe field serves only Sims and moving objects | Texel space has no shader coverage problem, no albedo problem and no per-frame cost; grids leak through zero-thickness walls unless masked [04 §1.2, §1.5; 12 §3, §4.1] |
| D4 | Shadow maps drawn from the game's own draws | **Static casters come from lot data** (the occluder proxy and the furniture blocker records). Meshes only replace a box once known; Sims are a dynamic layer | The camera's draw set is incomplete for rooms behind the camera, so shadows would appear and vanish during a pan [I, 03 §2; 08 §6] |
| D5 | Screen-space GI on top | Screen-space terms are **multiplicative detail layers after the scene** only, full resolution, no temporal filter | No G-buffer; half resolution and temporal filters are recorded failures of the AO pass [V, 00 §9; 08 §2, §4, §8] |
| D6 | Sims read the probe grid in patched shaders | **Sims and rig-lit objects are fed through their rig**, cached per model; outdoor rig-lit objects read a world-space lamp index instead of per-draw lamp picks. No SimSkin patch in the plan | Sims bind no room map and no directional map [V, 02 §4.1]; the rig write needs no shader work and covers `ps_2_0` families [12 §2.3] |
| D7 | Everything in Direct3D 9 shader model 3 | **Three back ends behind one carrier.** Shader model 3 limits only the 32-bit game side. The bridge's 64-bit server runs on Vulkan, so the solves run there as compute shaders and hardware ray queries through a bridge export; the Direct3D 9 passes are the fallback | The frame is bound by the game's render thread and the cost of each call across the bridge; the GPU is underused [V, 00 §10]. Server work costs no game-thread time, and rays give exact visibility, true penumbrae and several bounces. See §3.9 |

### Conflicts between research pages, resolved

| Conflict | Resolution |
|---|---|
| 04 keeps the game-format maps Instant mode composes; 02 wants float copies of larger size | Both, in order. S1 proves ownership on the existing compose path. Float copies at scale 2 or 4 follow once the texel position source and the `LightmapSizeParameters` readers are checked [O: S0-03, S0-04] |
| 06 P1 asks for 0 solve calls on the GI lot; 05 shows that cannot load | P1 is restated in §8: no class 1 or class 2 room solve on a GI lot; class-0 solves are counted and bounded |
| 02 needs a texel position source with the solve off; 05 keeps a class-0 solve | The class-0 solve still yields the layout and one sample a tile. Denser copies take positions from the proxy rasterised into light-map space [O: S0-03]. Fallback: scale 1 |
| `gpu-room-lamps.md` says Sims read the directional maps; 02 finds no Sim technique that binds them | The shader bindings are the stronger evidence: Sims are rig-only here. One static check of the room-mode gather `0x006BBDE0` settles a CPU-side read [O: S0-09]. Either answer leaves the rig write as the route |
| 04 and 12 §4.4 read probe values back, per change; 06 and 07 forbid read-backs | No synchronous `GetRenderTargetData` in the GI path. Lamp visibility at a model's centre is computed on the CPU (ray reference, or a 2D walk of the wall plan). Indirect values come from the server's point queries, from the asynchronous bridge read of [11 §5.1](research/11-prior-art-readbacks.md), or, without the bridge, from an event-query ring where the wait is at the lock |
| 03 wants per-pixel PCF in patched object shaders; 08 §8 a post-scene term; 12 §2.1 a lamp index texture | Outdoor rig-lit objects keep their existing patched copies but read lamps from index cells and a frame-constant block, so nothing is picked or uploaded a draw. Shadows across Sims are the post-scene term |
| 03 puts Sim capsules in a dynamic shadow tile; 12 §4.4c puts them in the AO pass | Capsule occlusion in the AO pass first (no shadow tile, any number of lamps); capsules in the tile or in the ray scene are the higher setting |
| 04 uses opening tables and frusta; 12 §4.2 a distance field of the wall plan; rays need neither | Rays where present. In the fallback, plan visibility replaces opening frusta and imperfect shadow maps; room scoping stays as the first filter |
| 08 §1 keeps a propagation grid as transport layer; 12 §3 keeps LPV rejected and adds feedback bounces | Later bounces come from feedback (the previous indirect map as emitter) plus a per-room ambient term. The masked grid is dropped from the plan; the probe field for movers is a per-tile ambient cube, not a propagation volume |
| 05 §4.4: window lights carry daylight in the maps; 01: Apex has no GPU law for types 7 and 8 | Until the window-light law exists [O: S0-08, S4-04] the cap and ownership apply at night only; by day the lot is handed back to the game's solve |
| 04 stores reflected flux in the shadow map; the proxy has no material | A hit reads Apex's own light map at the hit point (texel irradiance), times a per-room reflectance: the constant `kIndoorReflection` first, the game's room surface colour or the bound texture's mean colour when confirmed [O: S0-12] |
| 09 and 11 overlap on read-backs | Not part of GI; 11's asynchronous read supersedes 09 items 1a and 2c. See §11 |

## 3. Architecture

```
lamp snapshot ─► visibility ─► direct light (high resolution) ───────────────┐
      │              └──────► indirect layers, one a lamp (1 texel a tile) ──┼─► compose ─► light buffer ─► SetTexture swap ─► game shaders
      │                          └─► feedback bounces, room ambient term     │        └──► directional-map copies (ambient cube)
      ├─► lamp index cells + frame constants ─► outdoor rig-lit objects      │
      └─► rig write (CPU, cached per model) ─► Sims, rig-lit objects ◄───────┘ point values
post-scene: capsule occlusion and contact shadows in the AO pass, rig shadows, bounce detail (multiplicative)
```

"Visibility" and the two solves have three implementations (§3.9): ray queries in the bridge server, compute in
the bridge server, or Direct3D 9 passes. The data model, the compose, the swap, the rig write and the cap are the
same for all three.

### 3.1 Carrier: the texel-space light buffer

- One Apex-owned copy per story of the wall atlas, floor, ceiling and object maps, plus the four directional maps, for
  each GI lot. DEFAULT pool, written by draws or by the server, read by shaders, never locked [V rule, 06 §1.1].
- Channel meaning is the game's: rgb lamp light, alpha ambient weight, 0 on outdoor texels [V, 02 §1.2]. The interior
  shaders keep computing `rgb + alpha x InteriorBuildingAmbientColor` and the floors' tone curve.
- Formats: S1 the game's 8-bit format with the existing hue target; later A2B10G10R10 or RGBA16F at scale 2 or 4
  (16-32 texels a metre on floors) [O: S0-04].
- One point a texel carries lot position, normal and room id. The point is pushed half a texel along its normal
  before any visibility test, and valid texels are dilated by one into invalid ones [12 §4.2e].

### 3.2 Lamp data

- Source: every `BaseLight` of types 3-6 and 9-11 on the GI lot, enumerated with `0x006ACF70`; the light cells hold
  outdoor lights only [V, 01 §3]. Window lights (7, 8) join with S4-04.
- Read once a frame on the render thread into a snapshot, SEH-guarded, vtable and alive bit re-checked. Colour is
  `+0xF0 x +0x10` gated by the lit bit, times the fade `+0x20`; a lit change is debounced one frame [I, 01 §5].
- Change events come from `LampMarkFilter` and the existing object edit hooks; no new game hook. Existing readers
  are reused: `RtOutdoorLamps::ReadLamp`, `WorldLaw`, `GpuLampLaw::Lamp`.
- Not emitters: TV screens, glowing Sims, holiday lights [I, 01 §6].

### 3.3 Data model: light is linear

| Store | Resolution | Holds | Changes when |
|---|---|---|---|
| Direct accumulation | Buffer resolution | Sum of the lamps' shadowed direct light | A lamp moves, switches or changes; an occluder changes. Only affected texels |
| **Indirect layer, one a lamp** | 1 texel a tile and story (the game's lowest density) | That lamp's unit-colour indirect light, all bounces | The lamp moves, or the build changes in its reach |
| Directional layers, one a lamp | 1 texel a tile, 4 horizontal faces | The same gather with four fixed normals | With the indirect layer |
| Room ambient term | 1 value a room | `R / (1 - R)` times the room's mean unshot light | With the compose |

Compose: `direct + upsample(sum(on x colour x layer))`, one pass. Switching, dimming or recolouring a lamp, which is
what happens every evening and with strobe and stage lights, costs **no solve**: one compose draw [12 §4.1b]. The
upsample is bilinear and wall-aware (it does not blend across a wall face). Tiles with no room get no texel. Memory:
16 lamps x 64 x 320 texels of RGBA16F is 2.5 MB of server VRAM.

Updates run from a dirty queue of (lamp, story) pairs sorted by distance to the camera target, with a hard cap a
frame. Every update is a complete solve of its texels, so nothing accumulates noise on screen [12 §4.1c].

### 3.4 Direct light

- Ray back ends: shadow rays from each dirty texel to points on the lamp's emitter (bulb, shade, tube, area), 4-16 a
  lamp, against the proxy and furniture. True contact-hardening penumbra, no bias, no face seams.
- Fallback: the shipped atlas of cube faces (6 x 256 a lamp, least recently used, up to 120 tiles) with the shipped
  soft-shadow walk, verified against the CPU ray reference [V, 03 §1]. Static tile = proxy plus furniture blocker
  records, rebuilt on a lamp move, an occluder change or an object edit in reach, 1-2 tiles a frame.
- On a GI lot there is no kept base: every lamp of the lot goes through the accumulation. The game's map content is
  only the fallback under the swap.
- Same-frame edits: while a lamp is dragged, direct light stays on the shipped fast path; the ray result replaces it
  when it arrives.

### 3.5 Indirect light

| Step | Ray back end | Compute back end | Direct3D 9 back end |
|---|---|---|---|
| Emitters | The lit texels themselves: a ray that hits a surface reads the light buffer there | The same texel grid as virtual lights, 1 a tile | Flux from the shadow tiles, 16 clustered virtual lights a lamp, room-scoped |
| Visibility | The ray | Software traversal of the proxy, or the wall-plan distance field [O: R-02 picks] | Room scoping, then a sphere trace of the wall-plan distance field (jump flooding, rebuilt on build edits) |
| Bounces | 2-4 in the path | Feedback: the previous indirect map is the next emitter | Feedback, 2-3 passes on change |
| Furniture occlusion of bounce light | Yes | Proxy boxes | No |

Leaks are stopped by construction: rays cannot pass a wall quad; room scoping and the plan field do the same for the
fallback. A lamp in room A adds exactly 0 to texels of room B when no opening joins them: a test, not a tuning goal.

### 3.6 Movers: Sims and rig-lit objects

1. **Rig write, cached per model.** When a rig is gathered, or a lamp or the model changes, slots 1-3 take the three
   strongest lamps at the model's centre scaled by visibility there; the four vertex-light slots take the four
   horizontal ambient-cube faces at the model's tile plus the dominant direction of the rest (extracted as in Halo
   3). Values blend bilinearly between the nearest visible tiles and over 0.2-0.3 s. Apex applies its own ceiling,
   because the write bypasses the game's cap `0x006B92A0` [I, 02 §4.2] [O: S0-09 for writability].
2. **Directional-map copies** rendered from the directional layers: indoor counters, objects and normal-mapped
   floors get coloured directional bounce with stock shaders. How the readers weight the four maps is checked
   before relying on the ambient-cube reading [O: S0-04].
3. **Lamp index cells for outdoor rig-lit objects.** A cell is a tile a story; each holds up to 4 lamp indices. Cells
   and the lamp table live in rows of the one float atlas, so the feature costs one sampler. Atlas scale, lot origin
   and look constants sit in a reserved high pixel-constant block written once a frame [O: S0-G2 for a free range].
   The existing patched object copies read them; per-draw lamp picks and uploads stop for those families, the
   measured 1.5-2 ms of the per-draw fixes [I, 12 §2.1].
4. **Per-Sim irradiance cube** bound in place of `DiffuseLightProbe` (later, S4) [O: S0-G2].

Lighting is the only thing changed. No script binding returns a light value; the darkness buffs use lamp state,
windows and a room id [V, 05 §3]. Gameplay, saves and every Sim's behaviour are identical with the switch on or off.

### 3.7 The solve cap

- Site: the room priority call `0x006A81DF`, already redirected by Faster Room Lighting (`PriorityHook` in
  `room_light_queue.cpp`). For a room of a GI lot with class `room+0xF4 > 0` the hook returns 0 [05 §5.2]. Variant
  `hold`: also 0 for any room already solved once.
- The off set is keyed by lot id and lives outside the lighting manager, which is rebuilt on every promotion.
- Kept untouched: level rebuild `0x006A7CF0`, both AO solvers, the light gather, the synchronous path
  `0x00ADBAD0 -> 0x006A80E0`.
- **Thumbnail exemption:** while `0x00AE06B0` is polled for a lot, that lot leaves the off set until it returns true.
- Map content under the copies: the class-0 result with its rgb cleared at the end-of-solve hook, ambient alpha kept,
  so a surface the swap misses shows "no lamps" instead of class-0 light.
- Gate before building: how the 2.81 ms of `0x006A8BA0` splits between rebuild, AO slots and the room pass
  [O: S0-11, S0-G1]. If the room pass is the small part, S1's claim is restated before more is built.

### 3.8 Detail layers and stand-downs

All detail layers run after the scene, multiplicative, full resolution, on the Ambient Occlusion pass's 4x4 pattern
and box filter:

| Layer | Effect | Cost at 4K [I] |
|---|---|---|
| Capsule occlusion | Analytic soft shadow of up to N Sim capsules toward the dominant lamp; proxies travel in the frame-constant block | Under 0.3 ms |
| Contact shadows | 8-step depth march over 0.3-0.5 m toward the 1-2 dominant lamps; darkens the lamp share only; combined with AO by `min` | 1-1.5 ms |
| Rig shadows | Sim and rig-lit pixels scaled by the visibility ratio between the pixel and the rig centre | 0.2-0.5 ms |
| Bounce detail | Colour taps in the AO slices, zero-mean, range 0.5-1 m | 0.5-1.5 ms |
| AO with GI on | The multi-bounce fit brightens AO by albedo so creases are not darkened twice | 0 |

Exact furniture shadows: on the ray back end the server builds a structure from the game's own vertex buffers for
placed objects [O: R-07]; on the fallback a mesh is baked into the static tile once seen, upgraded off screen or
while the camera moves, never downgraded, crossfaded [08 §6].

Stand-downs, one owner a lot: level light share, outdoor delta, `RtOutdoorLamps` relights, room-map smoothing,
Rooms at Night base, the Faster Room Lighting drain, and each `LotLightBridge` per-draw fix once its surface is
covered. World terrain and roads keep the terrain stamp. A stand-down is logged with its reason.

### 3.9 Server-side tier: compute and ray queries in the bridge server

**Why.** The bridge server (`NvRemixBridge.exe`) is a 64-bit process that draws through stock DXVK on Vulkan
[V, `rtx-remix-bridge.md`]. Apex builds and patches the bridge (`native/remix-bridge/patches`), and
[11 §5.1](research/11-prior-art-readbacks.md) designs an export with a shared-memory block and a ticket. The same
shape carries GI: Apex submits a small scene and dirty flags, the server solves on the GPU on its own thread, and
the result lands in the light buffer textures. The game's render thread spends a poll a frame.

| Back end | Needs | Visibility | Role |
|---|---|---|---|
| `serverRays` | Bridge with the GI export; `VK_KHR_acceleration_structure` and `VK_KHR_ray_query` (RTX 20 and newer, RX 6000 and newer, Arc; the target is RTX 30 and newer) | Hardware ray queries in a compute shader | Reference quality; first choice |
| `serverCompute` | Bridge with the GI export; any Vulkan 1.1 GPU | The same kernel without rays | No shader model 3 limits, no bridge calls for the passes |
| `d3d9` | Nothing: native Direct3D 9, in-process DXVK, a bridge without the export | Shadow tiles, room scoping, plan field | Fallback |

**How.**

| Piece | Design | Mark |
|---|---|---|
| Device | A **second Vulkan device** for GI on a worker thread of the server. Stock DXVK stays unchanged: it does not enable the ray-tracing extensions, and the package ships a stock DXVK release | [I]; coexistence [O: R-01, R-03] |
| Scene | Tier A: the lot proxy (about 10 k triangles) and furniture blocker boxes. Tier B: placed objects from vertex buffers the server already holds, named by Apex with buffer range, layout and world matrix. Tier C: Sim capsules, refitted a frame. Plus lamp table, room reflectance and the texel points. Sent on change through a shared block | Tier A data [V, 03 §1]; vertex formats for tier B [O: R-07] |
| Solve | §3.4 and §3.5 per dirty (lamp, story): shadow rays to the emitter, then paths whose hits read the light buffer. Converges once and is then frozen; a small edge-aware filter in texel space replaces a denoiser | [I] |
| Result | Written into Apex's textures on the server's Direct3D 9 device from the command loop; a texture is named by its client pointer, as patch 0015 names surfaces | [O: R-03] |
| Point queries | Irradiance, ambient-cube faces and dominant lamps at given points, returned through the shared block: the rig write's values with no read-back | [I] |
| Detection | `GetProcAddress` on the loaded `d3d9.dll`; the server reports its level; a stock or lost server means `d3d9`, silently | As 11 §5.1 |

**What moves to the server.**

| Piece | Direct3D 9 | Server | Verdict |
|---|---|---|---|
| Direct shadow visibility | Cube tiles, walk of up to 280 taps, furniture as analytic boxes; 7.5-10.9 ms a full round [V, 03 §6] | Exact rays, emitter-sized penumbra; about 1 M rays a lamp at high density [I] | Server when the edit shows within 2 frames [O: R-G1] |
| Indirect layers | One bounce plus feedback, plan visibility, no furniture occlusion | 2-4 bounces with real visibility and colour bleeding; 40 k texels x 64 rays x 3 bounces is 8 M rays a lot [I] | **Server first**: the largest quality gain |
| Probe field for movers | Directional layers per tile | The same, or probes updated by rays where height matters (stairwells, split levels) | Server; DDGI-style probes only where the per-tile values fail |
| Scheduling | Spread over render-thread frames inside a CPU budget | Asynchronous on the worker | Server |
| Sim shadows | Capsule occlusion in the AO pass | Capsules in the ray scene | Either; measured in S3 |
| AO, contact shadows, per-pixel lamp shadows | The AO pass and post-scene layers | Need this frame's depth on the ray-tracing device, or a ray query inside DXVK's translated shaders: a patched DXVK | Not in this tier; a decision note only [O: R-08] |

**Considered and ranked low.** Per-pixel ReSTIR, ray-traced GI with denoisers (NRD) and DLSS all need per-pixel
normals, albedo and motion vectors, which a forward Direct3D 9 game does not have [V, 10 §0; 12 §7.4]; a lot has
tens of lamps, and the texel solve visits each once a change. The RTXGI and RTXDI SDKs carry licences that do not
fit an MIT mod; anything used is implemented from the papers. Mesh shaders have no use for a 10 k-triangle scene.
The Remix runtime's path tracer replaces the raster frame and has no hybrid output [V, 07 §2]; it stays the
photo-mode option of §11. The ray back end doubles as the ground truth for the look checks.

**Cost.** Game thread: a scene submit on change and one poll a frame. GPU: tens of milliseconds in total for a
converged lot, sliced so the rendering device keeps its frame time, zero when clean [I; R-02, R-G1 measure].
Memory: in the 64-bit server; the shared block is about 1 MB of game address space.

**Parity rule.** All back ends fill the same maps, layers and look constants, so pictures compare tier against
tier. The fallbacks pass the same leak, lamp-table and stability checks and stay within the look band (Q12).

## 4. Stages

| Stage | Result | Proof |
|---|---|---|
| S0 | Desk proofs, tools and measurement runs that close the open points; the standalone ray-query bake and the bridge export prototype | Offline harness results, RE notes, profiles |
| S1 | One lot at night: Apex owns direct light in the light buffer, solve capped at class 0 | Camera pan ABBA at 40 m and 150 m, look pairs |
| S2 | Indirect light: per-lamp layers, compose, feedback bounces, room ambient term; server back end first when its prototype passes, Direct3D 9 gather as fallback | Three-room pictures, leak test, toggle cost, back-end parity |
| S3 | Movers: rig write, directional-map copies, lamp index cells and frame constants, capsule occlusion | Sim-in-room pictures, per-draw fix time removed |
| S4 | Window lights and daylight, per-Sim cube, furniture in the ray scene, every covered lot | Day and night runs, 60 min memory arm |
| S5 | Detail layers: contact shadows, rig shadows, bounce detail, exact furniture on the fallback | Look pairs and GPU time per layer |
| S6 | Look calibration, menu and translations, pages, compatibility | Full gate of §8 and §9 |

S1, S2 and the server track R are independent lanes after S0. The frame-constant block and the lamp index cells
(S3) do not depend on the GI solves and can be built early.

## 5. Switches

TOML section `[patches.RealtimeGI]`. Every key defaults to off or neutral; with `enabled = false` no hook is
registered and no resource is created.

| Key | Type, default | Values | Stage |
|---|---|---|---|
| `enabled` | bool, false | Master switch | S1 |
| `backend` | enum, `auto` | `auto`, `serverRays`, `serverCompute`, `d3d9` | S2 |
| `lots` | enum, `active` | `active`, `activeAndNeighbours`, `all` | S1, S4 |
| `solveCap` | enum, `off` | `off`, `class0`, `hold` | S1 |
| `nightOnly` | bool, true | Hand the lot back to the game's solve by day; false needs `windowLights` | S1, S4 |
| `bufferScale` | enum, `1` | `1`, `2`, `4` | S1 |
| `floatBuffers` | bool, false | Float copies instead of the 8-bit compose | S1 |
| `indirect` | enum, `off` | `off`, `bounce1`, `multiBounce` | S2 |
| `roomAmbient` | bool, false | Per-room ambient term | S2 |
| `rigFill` | bool, false | Rig write: lamps with visibility and ambient-cube faces | S3 |
| `lampIndex` | bool, false | Lamp index cells and frame constants for outdoor rig-lit objects | S3 |
| `simShadows` | enum, `off` | `off`, `capsule`, `mesh` | S3, S5 |
| `simCube` | bool, false | Per-Sim irradiance cube | S4 |
| `windowLights` | bool, false | GPU law for types 7 and 8 | S4 |
| `exactFurniture` | enum, `off` | `off`, `on` | S4, S5 |
| `contactShadows`, `rigShadows`, `bounceDetail` | bool, false | Post-scene layers; `bounceDetail` needs Ambient Occlusion | S5 |
| `look` | float, 0.0 | 0 = fitted to the game's look, 1 = raw solve | S6 |
| `indirectIntensity` | float, 1.0 | 0-2 | S6 |
| `albedoBoost` | float, 1.0 | 1-2, bounce reflectance | S6 |
| `maxTiles` | int, 24 | 8-120, fallback shadow tiles | S1 |
| `debugRedrawAll`, `debugLayer`, `debugLampTable` | developer builds | Cache check, single-layer view, lamp table dump | S1 |

Menu labels, tooltips and the 21 translations are added in S6 with the menu rules; until then the keys are
developer-mode only.

## 6. Counters and logs

- Start line, read back by the test harness:
  `[RealtimeGI] On: lots=<n> cap=<mode> scale=<n> indirect=<mode> backend=<name>`.
- Frame profiler hook line `RealtimeGI`: ms a frame, draws a frame, device calls a frame.
- Every 10 s, developer log:

| Group | Counters |
|---|---|
| Cap | priorities zeroed, class-0 solves, **class 1-2 solves on GI lots (must be 0)**, thumbnail exemptions, day hand-backs |
| Direct | texels relit, tiles rebuilt, cache hits and misses, evictions |
| Indirect | layers solved, compose-only updates (toggle, dim, colour), dirty queue length, bounces, solve ms |
| Buffer | maps composed, crossfades running, swap binds |
| Movers | rigs written, cache hits, ceiling clamps, capsules, index cell rebuilds, frame-constant writes, per-draw picks left |
| Server | back end in use, scene submits and bytes, solves issued, ready, timed out, fallbacks, rays a second, GPU ms a slice, frames from edit to upload, point queries |
| GPU | ms per pass: shadow, direct, indirect, compose, post-scene |
| Safety | **synchronous `GetRenderTargetData` from GI code (must be 0)**, **locks of GI resources (must be 0)**, device resets, stand-downs by reason, SEH faults in the lamp read |
| Check | `debugRedrawAll` differences (must be 0) |

## 7. Budgets

| Budget | Limit | Basis |
|---|---|---|
| CPU, steady, Direct3D 9 back end | 0.3 ms a frame, 0.9 ms worst | 03 §6, 06 §2.2 |
| CPU, steady, server back ends | 0.1 ms a frame | §3.9 |
| CPU, change burst | 2 ms a frame, spread | Existing `RtOutdoorLamps` rule |
| Draws, steady | 0-100 a frame | 03 §9 |
| GPU, steady | 2 ms at 4K on the profile GPU; the laptop RTX 30 class is recorded | 06 P7 |
| GPU, while solving | 2 ms a frame slice; a lamp toggle is one compose draw | §3.3, §3.9 |
| Latency | Toggle, dim, colour: next frame. Lamp move: direct within 2 frames, indirect within 30 | §3.3 |
| VRAM | 50 MB at 16 tiles; 150 MB with layers and float buffers at scale 2 | 06 §1.2 |
| Game address space | Under 2 MB with the shared block; largest free block within 64 MB of baseline | 06 P8 |
| Removed, expected | Room pass share of 2.81 ms, 0.5-0.6 ms room-map smoothing and 0.2-0.4 ms relights in S1; 1.5-2 ms of per-draw picks in S3 [I] | 06 §2.1, 12 §2.1 |

## 8. Performance targets and pass criteria

Measured with the Apex runner (`tools/ingame_runner`, ABBA) and the camera sweep, baseline re-measured in the same
session. A smoke job proves the switch read-back and the per-arm split before any long run.

| # | Criterion | Number |
|---|---|---|
| P1 | Room solves on a GI lot | No class 1 or 2 solve; class-0 solves at most one a room a load plus one a lamp edit (`class0`) or none (`hold`) |
| P2 | Hitches a minute, moving, 150 m | At least 40% fewer than baseline |
| P3 | p99 and p99.9 at 40 m and 150 m | Not worse by more than 0.5 ms; target p99 3 ms better |
| P4 | p50 | Not worse by more than 0.3 ms; target 1 ms better in S1, 3 ms after S3 |
| P5 | Max frame, frames over 50 ms and 100 ms | Not worse |
| P6 | `RealtimeGI` hook, steady | Within the §7 budget of its back end; a frame over 3 ms only in a burst after a change |
| P7 | GPU time of the GI passes at 4K | At most 2 ms |
| P8 | Largest free block at +10 and +60 min | Within 64 MB of baseline; client surface copies within 5 MB |
| P9 | Synchronous read-backs and locks from GI code | 0 |
| P10 | Stability | No crash, script error or out-of-memory error; lot loads, saves and library thumbnails complete |
| P11 | Switch off | Frame-time distribution and pictures equal to the baseline build within run-to-run spread |
| P12 | Server back ends | No stall of the server's command loop over 1 ms from a GI upload; a lost or slow server falls back to `d3d9` within 1 s with no picture gap |
| P13 | Lamp toggle | 0 solves, one compose; no frame over the hitch rule from an evening of switches |

No clear difference is reported as such, not as a gain.

## 9. Image quality and realism targets

The aim is light that behaves physically inside a picture that still reads as The Sims 3.

| # | Property | Check |
|---|---|---|
| Q1 | No leak | Offline: lamp in room A, texels of a closed room B and of other stories = exactly 0, direct and indirect, on every back end. In game: three bookmarks across a wall and a floor |
| Q2 | Lamp fidelity | Lamp table dump against the game's fields: 0 differences in position, colour, cone, on and off |
| Q3 | Shadow correctness | Direct3D 9 back end against the CPU ray reference within 2/255 on at least 97% of texels a lamp (the shipped standard); ray back end against the offline path-traced reference |
| Q4 | Shadow shape | Chair, plant and Sim shadows lie on floor and wall; the edge widens with distance from the caster |
| Q5 | Stability | Buffer contents byte-equal across 600 frames of camera pan with no edit; no flicker, no popping; a converged solve is frozen |
| Q6 | Linearity | A toggled lamp's contribution equals the difference of two full solves within 1/255 |
| Q7 | Bounce | A lit room shows coloured fill on the ceiling and in corners; a doorway spills into the next room with the door's spread; a closed neighbour is unchanged |
| Q8 | Movers match | Mean luminance of a Sim against the floor under it stays in the same ratio band as the game's class-2 solve, in a lit room and in a dark neighbour; no pop when a Sim crosses a doorway |
| Q9 | TS3 look | With `look = 0`, mean and 5th and 95th percentile of log luminance per map class within tolerance of the game's solved maps for the same lot; picture pairs at the bookmarks |
| Q10 | Daylight | Day pictures unchanged while `nightOnly` is on; with `windowLights`, within the Q9 band |
| Q11 | No double darkening | AO over GI does not darken creases beyond the reference |
| Q12 | Back-end parity | The same bookmarks on each back end: Q1, Q2, Q5 pass; mean luminance of lamp-lit pixels within 10% of the ray result |

**Look calibration (S6).** Per map class (floor, wall, ceiling, objects) a gain, a contrast exponent, an ambient
floor and a saturation scale are fitted offline so Apex's maps match the mean and spread of the game's solved maps
for the same lots (statistics matching in log luminance). They are constants in the compose. `look` blends between
the fitted constants and identity; `indirectIntensity` and `albedoBoost` are the two player controls. The game's
ambient term, tone curve and bloom mask keep acting on the new light because the shaders are the game's own; no new
tone mapper is added.

Pictures are compared as pairs at fixed bookmarks, paused, fixed time of day. Q1-Q3, Q5, Q6 and Q9's statistics are
pass or fail; the others need a reviewer.

## 10. Risks

| Risk | Mitigation |
|---|---|
| The room pass is a small part of the solve's time | S0-G1 before S1; S1's claim is restated from the split |
| Texel positions above scale 1 cannot be made without the solve; a larger copy breaks a shader's own texel offsets | S0-03, S0-04; scale 1 fallback per family |
| Rig slots are overwritten or capped after the write | S0-09; fall back to the existing gather boost |
| A game path writes the reserved constant range, or the bridge drops the write after a reset | Histogram in S0-G2; rewrite after device reset |
| Thumbnail capture waits forever on a capped lot | Exemption in §3.7; test saves a lot to the library |
| A second Vulkan device beside DXVK misbehaves on some drivers, or the upload stalls the command loop | R-01 and R-03 prove it on the bridge alone before any GI code; `backend = d3d9` always works |
| The bridge patch must follow upstream; a solve bug stalls the server, not the game | Small export surface; feature probe; watchdog that drops to `d3d9` |
| Back ends drift apart | Parity rule and Q12; one data model |
| VRAM on 6-8 GB laptops | Layer and tile counts follow free VRAM; measured in S4 |
| Other game builds and renderers | Pattern scans; the feature stays off where a site is not found |

## 11. Adjacent work outside this design

From [09](research/09-salvage-apex-dropped.md), [10](research/10-prior-art-ao.md),
[11](research/11-prior-art-readbacks.md) and [12 §2.4](research/12-prior-art-lighting-gi.md). Separate changes with
their own proof; plan track X lists them.

| Item | Link to GI |
|---|---|
| Asynchronous render-target read in the bridge (11 §5.1), pick and compositor integration | Its export, shared block and ticket are the model for the GI export; optional source of CPU values on the `d3d9` back end under the bridge |
| Bridge `commandBatchingEnabled` (12 §2.4) | Lowers the per-call cost every back end pays; one configuration line and one run |
| Pick skip while still (09 §2a) | None |
| Slope-adjusted edges, quadrant-weighted normals and an edge-aware 4x4 box in Ambient Occlusion (10 §1-2) | The post-scene layers ride on the same pass; build these before S5 |
| One slice count a frame by zoom (10 §3) | Frees GPU time for S5 on GPU-bound setups |
| Per-draw fix memo and lazy restore (09 §6) | Superseded on GI lots by the stand-downs and the lamp index |
| Path-traced photo mode on the Remix runtime (08 §5) | Not planned |
| 2D radiance cascades per story (12 §4.2c) | One offline experiment after the plan field exists |

## 12. To fold in

- **Research page 13** (bridge command batching and free constant ranges) is not in the folder yet. When it
  arrives: check it against §3.6 item 3, §3.9 and §11, update the marks, and record the change in
  [handoff.md](handoff.md).
- Results of S0 replace the **[O]** marks above; each closing work package edits this page in the same commit.
