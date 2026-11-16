# Real-time GI: work plan

Stages S0 to S6 of [design.md](design.md), split into work packages. Each package is sized for one focused worker
in about 1-2 hours of wall clock, has one deliverable and one acceptance check, and reports into
[handoff.md](handoff.md). Game runs are separate packages (ids with `G`), because they need the game, the test
machine and a quiet system.

## How to read the tables

- **Inputs:** read these first, after design.md. `NN §x` is a page of [research/](research/).
- **Run:** `desk` = reading and static analysis; `offline` = builds or runs a tool on the GPU or CPU, no game;
  `bridge` = builds the bridge and runs its self-test, no game; `game` = an in-game job (named).
- **Tier:** the capability the package needs. **A** = hardest design and shader work; **B** = reverse engineering
  and C++; **C** = mechanical.
- **Now:** can start at once, in parallel with every other `Now` package.
- Rules for every package: no game launch outside a `game` package; code behind `[patches.RealtimeGI]`, default off;
  offline harnesses read-only; facts marked verified, inferred or open; public text checked with
  `python tools/public_text_check.py --files <paths>`.

Harness jobs (`tools/ingame_runner/jobs/`, created by S0-13): `realtime-gi-smoke`, `realtime-gi-pan` (ABBA, 40 m and
150 m, from `bench-apex-off-on` and `night-lot-switch-pan`), `realtime-gi-look` (paused bookmark pairs),
`realtime-gi-rooms` (lamp toggle, move, wall edit, door; three rooms), `realtime-gi-profile` (frame profiler arm).
The camera sweep runs as a two-arm queue (baseline build against the GI build).

## S0: desk proofs, tools, measurements

| Id | Goal | Inputs | Deliverable | Acceptance | Deps | Run | Tier | Now |
|---|---|---|---|---|---|---|---|---|
| S0-01 | CPU reference of the indirect data model: per-tile solve, one layer a lamp, compose, leak test | 04 §1.5, §3 S0; 12 §4.1; `features/rt_lamp_proxy.h` (`Visibility`); `tools/rt_lamps_test/check.cpp` | New cases in `tools/rt_lamps_test`: layers and compose on a captured lot and on synthetic stories | Closed room B = exactly 0; compose of layers equals a full solve within 1/255; toggle needs no solve | - | offline | B | yes |
| S0-02 | Offline path-traced reference of proxy, furniture boxes and lamp table, several bounces | 08 §5; 12 §4.5, §5 (hemicube row) | Reference mode in `tools/rt_lamps_test` that writes maps in the buffer layout | Converges (two seeds within 1/255 mean); closed room = 0; documented run time | - | offline | B | yes |
| S0-03 | Texel position source with the solve at class 0: wall atlas layout and vertex uv before and after the first solve | 02 §3.2 point 1, §6; 05 §2; `docs/engine/room-light-maps.md`; commit path `0x0069E950`, room `+0x680` | Engine note in `docs/engine/room-light-maps.md` and a verdict in handoff.md | States, with addresses, whether layout and uv exist at class 0 and are stable across classes; names the fallback | - | desk | B | yes |
| S0-04 | Shader census for the carrier: `LightmapSizeParameters` readers, how readers weight the four directional maps, highest constant each family reads | 02 §0, §3.2 point 2, §6; 12 §4.4a, §8; `research/tools/precomp.cjs` | Table in `research/` (new page) listing families by: uses texel offsets, directional weights, highest PS and VS register | Every room-map family classified; a list of families that must stay at scale 1 | - | desk | C | yes |
| S0-05 | Compile cost of the shader snippets | 02 §2; 12 §2.1; `fxc` | `tools/realtime_gi_shader_cost/` with the snippets and a result table: lamp, cone, PCF, index-cell read with 2 `texldl` a lamp, gather loop, plan sphere trace | Slot, register and sampler counts from real `ps_3_0` compiles; a verdict per snippet against 512 slots and 32 temporaries | - | offline | A | yes |
| S0-06 | Wall-plan visibility offline: plan texture, jump-flood distance field, sphere trace, and its CPU twin | 12 §4.2a, §6 P3; `rt_lamp_proxy.h` (`WallSides`) | Cases in `tools/rt_lamps_test`; sill-height rule for windows and half walls written down | Closed wall = 0; doorway spill inside the door's cone; diagonal walls tight; CPU twin agrees with the field | - | offline | A | yes |
| S0-07 | Room id a tile and wall-aware upsample weights from the proxy | 04 §3 open point 3; `rt_lamp_proxy.h` (`InsideOf`, `InsideAt`, `IntoRoom`); `features/level_light_share.h` | Pure header `features/gi_rooms.h` with a self-test | Room ids match the game's rooms on the captured lots; upsample never blends across a wall face | - | offline | B | yes |
| S0-08 | Window light laws (types 7, 8) | 01 §2, §6; evaluations `0x006BEFD0`, `0x006BF880`, rect setter `0x006BE780` | Section in `docs/engine/light-objects-and-rigs.md`: emitter shape, sky link, curtain dimmer | A law precise enough to code as an area emitter; unverified parts marked | - | desk | B | yes |
| S0-09 | Rig facts for the rig write | 02 §4, §6; 08 §11 #5; gather `0x006BBDE0`, `0x006BB2F0`; binder `0x006B8B30`; cap `0x006B92A0` | Section in `docs/engine/light-objects-and-rigs.md` | Answers: does the room-mode gather read directional maps on the CPU; can slots 1-3 and the vertex-light slots take arbitrary values; where a cached write must happen | - | desk | B | yes |
| S0-10 | Cap site details and the thumbnail path | 05 §4-§7; `features/room_light_queue.cpp` (`PriorityHook`, `FinalizeHook`) | Notes in `docs/engine/room-light-maps.md`; the exact condition for the cap and for the exemption | Offsets and call sites confirmed; says whether the thumbnail capture times out or forces a level-of-detail switch | - | desk | B | yes |
| S0-11 | Frame profiler targets that split `0x006A8BA0` into rebuild, the two AO slots and the room pass | 05 §7 Q1; `features/frame_profiler.cpp` | Profiler hooks behind the developer build | Builds; four new lines in the profiler report | - | offline | B | yes |
| S0-12 | Reflectance source for bounce light | 05 §2 (surface colour pass `0x006A0F00`); 12 §4.3 (albedo) | Note in `docs/engine/room-light-maps.md` | Names a per-room colour that can be read safely, or confirms the constant | - | desk | B | yes |
| S0-13 | Harness jobs, smoke job and the start-line pattern | 06 §3; `tools/ingame_runner/README.md`, `jobs/bench-apex-off-on.json`, `jobs/night-lot-switch-pan.json` | The five job files above; the pattern for the `[RealtimeGI] On:` line with matching and non-matching test lines | `selftest.ps1` passes; the pattern matches the real line format and rejects near misses | - | offline | C | yes |
| S0-14 | Developer tool: register-range histogram of the game's shader constant writes | 12 §2.2, §6 P1; `framework` D3D9 hooks | Counter in the developer build, printed with the profiler report | Builds; reports the highest pixel and vertex register written a frame | - | offline | B | yes |
| S0-15 | Developer tools: read-back benchmark and the 24-cube arm | 09 §0 (Step 0); 06 §5.3 | Two developer-only test arms | Build; each prints its table once and removes its resources | - | offline | B | yes |
| S0-16 | Error of a multiplicative apply behind the game's tone curve | 08 §2, §4, §11 #2 | Offline test on captured frames with a known added light | Error bound stated for small and large terms | - | offline | A | yes |
| S0-17 | Look fit tool: gain, exponent, floor and saturation per map class from the game's solved maps | 12 §4.5; design §9 | Tool in `tools/rt_lamps_test` that prints the constants and the residual a room | Residual reported for the captured lots; constants reproducible | S0-01 | offline | C | after S0-01 |
| S0-G1 | Measure the solve split | S0-11 | Profile of the far pan with the new lines; numbers in handoff.md | The room pass share of 2.81 ms is known; S1's claim restated | S0-11 | game: `realtime-gi-profile` | B | no |
| S0-G2 | Night frame capture and constant histogram | S0-14; 03 §10; 12 §8 | Answers in handoff.md: free constant range, sun shadow pass at night, Sim caster draws and bone registers, how SimSkin weights `DiffuseLightProbe` | Each question answered or marked still open with the reason | S0-14 | game: `capture` | B | no |
| S0-G3 | Bridge measurements | S0-15; 12 §2.4 | Read-back table, address-space delta of 24 cubes, cost of a re-issued draw, `commandBatchingEnabled` on and off over one pan | Tables in handoff.md; a verdict on command batching | S0-15 | game: `realtime-gi-pan` | C | no |

## Track R: server-side tier

| Id | Goal | Inputs | Deliverable | Acceptance | Deps | Run | Tier | Now |
|---|---|---|---|---|---|---|---|---|
| R-01 | Read the bridge at its pin: command loop, handle maps, shared blocks, threads, build; write the export | design §3.9; 11 §1, §5.1; 07 §1; `native/remix-bridge/` | `docs/features/realtime-gi/server-export.md`: commands, shared block layout, ticket and reset rules, how a client texture is named on the server, where a worker thread and a second Vulkan device attach | Every statement cites a file and line of the pinned source; open points listed | - | desk | B | yes |
| R-02 | Standalone solver, no bridge and no game: structure from a captured proxy, ray-query compute for direct and multi-bounce indirect a texel, and the same kernel without rays | design §3.4, §3.5, §3.9; 12 §7; `tools/rt_lamps_test` captures | `tools/gi_server_test/` (Vulkan, SPIR-V built at build time) | Closed room = 0; matches S0-02 within the look band; rays a second and time to converge printed; runs without ray query on the compute path | - (compares with S0-01, S0-02 when present) | offline | A | yes |
| R-03 | **Bridge patch prototype:** export, shared block, worker with its own Vulkan device, one compute pass that fills a client-named texture, capability report | R-01; 11 §5.1; patches 0001-0014 as style | Next free patch in `native/remix-bridge/patches/` and a bridge self-test client | Self-test reads the pattern back; stock DXVK untouched; no stall of the command loop over 1 ms; a stock server makes the client report "none" | R-01 | bridge | B | after R-01 |
| R-04 | Solver in the server: R-02's kernel in R-03's worker; scene submit, dirty tickets, uploads into layers and direct maps | R-02, R-03; design §3.3 | Bridge patch update | Self-test solves a synthetic lot and matches R-02's output | R-02, R-03 | bridge | A | no |
| R-05 | Apex client: detection, scene and lamp submit, poll, watchdog, fallback, counters | R-03; design §5, §6 | `features/gi_server.{h,cpp}` | With no export: silent `d3d9`; with it: tickets complete; fallback within 1 s when the server stops answering | R-03, S2-01 | offline | B | no |
| R-06 | Point queries for the rig write | R-04; design §3.6 | Export and client call | Values equal the layer contents at the points | R-04 | bridge | B | no |
| R-07 | Furniture from the game's vertex buffers in the ray scene | 12 §7.3 tier B; S0-G2 | Bridge patch update and the Apex command a placed object | Vertex formats handled or refused cleanly; a chair's shadow is mesh-shaped in the self-test | R-04, S0-G2 | bridge | B | no |
| R-08 | Decision note: a same-device tier for per-pixel rays | design §3.9; 12 §7.4 R4-R6; DXVK's interop interface at the pinned release | Section in `server-export.md` | States what a patched DXVK would need, its cost and a go or no-go | - | desk | B | yes |
| R-G1 | Server solve on one lot in game | R-04, R-05 | Numbers in handoff.md: frames from edit to picture, GPU ms a slice, render-thread ms, memory; parity pictures on all back ends | P12 and Q12 of design.md | R-05, S2-01 | game: `realtime-gi-rooms`, `realtime-gi-pan` | B | no |

## S1: direct light owned by Apex, solve capped, one lot

| Id | Goal | Inputs | Deliverable | Acceptance | Deps | Run | Tier | Now |
|---|---|---|---|---|---|---|---|---|
| S1-01 | Module skeleton: patch class, switches, start line, profiler hook line, counters | design §5, §6; `patches/performance_patches.cpp` (`RoomLightQueuePatch`) as model | `features/realtime_gi.{h,cpp}` and its patch registration | Off: no hook, no resource, byte-equal pictures. On: the start line prints | - | offline | B | yes |
| S1-02 | Lamp snapshot of a GI lot and the lamp table dump | 01; `RtOutdoorLamps::ReadLamp`, `WorldLaw`; `LampMarkFilter` | Snapshot with edit notes; `debugLampTable` | Dump equals the game's fields on the test lot, 0 differences | S1-01 | offline | B | no |
| S1-03 | Solve cap, off set by lot id, thumbnail exemption, night gate | 05 §5; S0-10; S0-G1 | Condition in `PriorityHook`; exemption hook | Counters: no class 1-2 solve on the GI lot; lot loads; a lot saves to the library | S1-01, S0-10, S0-G1 | offline | B | no |
| S1-04 | Direct ownership: every lamp of the lot through the accumulation, no kept base; stand-downs for the GI lot | 03 §1, §9; `features/rt_lamp_gpu.*`, `rt_outdoor_lamps.cpp`, `gpu_room_lamps.cpp` | Ownership mode in the existing relight path | Offline: against the CPU reference within the shipped standard; stand-downs logged | S1-02 | offline | A | no |
| S1-05 | Float and scaled copies with their texel position source | 02 §3.2; S0-03, S0-04 | `bufferScale`, `floatBuffers` | Offline render of a captured atlas: no seam at strip edges; families from S0-04 stay at scale 1 | S1-04, S0-03, S0-04 | offline | A | no |
| S1-06 | Map content under the copies: rgb cleared, ambient alpha kept | 05 §5.2 item 5; `FinalizeHook` | Clear at the end-of-solve hook for GI lots | A surface the swap misses shows no lamp light and keeps its ambient | S1-03 | offline | B | no |
| S1-07 | Offline validation and the cache check | 06 §4 R5 | `debugRedrawAll`; harness cases for ownership | 0 differences between cached and redrawn buffers on the replayed scenes | S1-04 | offline | B | no |
| S1-G1 | Smoke and look pairs | S0-13 | Pictures and the smoke verdict | Start line read back; Q1, Q2, Q4, Q5 on six bookmarks | S1-03, S1-04 | game: `realtime-gi-smoke`, `realtime-gi-look` | C | no |
| S1-G2 | Camera pan ABBA and the memory arm | 06 §3 | Result table | P1-P11 of design.md | S1-G1 | game: `realtime-gi-pan` | C | no |
| S1-G3 | Camera sweep, two arms | 06 §3.1 leg B | Result table a segment | P3-P5 a segment; `debugRedrawAll` = 0 | S1-G1 | game: camera sweep | C | no |

## S2: indirect light

| Id | Goal | Inputs | Deliverable | Acceptance | Deps | Run | Tier | Now |
|---|---|---|---|---|---|---|---|---|
| S2-01 | Data model in Apex: layer atlas a lamp, compose with wall-aware upsample, dirty queue with a cap a frame, crossfade | design §3.3; S0-01, S0-07 | Layer store and compose pass | Toggle = 0 solves and one compose; compose equals the CPU reference | S1-01, S0-01, S0-07 | offline | A | no |
| S2-02 | Direct3D 9 gather into the layers: flux from the shadow tiles, clustered virtual lights, room scoping | 04 §1.5; S0-05 | Gather pass | Leak test = 0; GPU ms a layer within budget | S2-01 | offline | A | no |
| S2-03 | Plan field in the gather | S0-06 | Plan texture, jump-flood passes on build edits, sphere trace | Doorway and window cases of S0-06 pass on the GPU | S2-02, S0-06 | offline | A | no |
| S2-04 | Feedback bounces and the room ambient term | 12 §4.3 | `indirect = multiBounce`, `roomAmbient` | Second pass costs no more than the first; energy bounded | S2-01 | offline | B | no |
| S2-05 | Solve cost against lamp count on a lot with 60 lamps or more | 08 §1, §10 | Table in handoff.md | Cost a layer and a full lot, both back ends where available | S0-01 | offline | C | no |
| S2-G1 | Three rooms in game | 04 §3 S2 | Pictures, toggle and move timings | Q6, Q7, P13; no new hitch | S2-02 or R-05 | game: `realtime-gi-rooms`, `realtime-gi-pan` | C | no |

Order inside S2: when R-02 and R-03 pass, R-04 and R-05 come before S2-02 and S2-03, and the Direct3D 9 gather is
built as the fallback against the server result.

## S3: movers

| Id | Goal | Inputs | Deliverable | Acceptance | Deps | Run | Tier | Now |
|---|---|---|---|---|---|---|---|---|
| S3-01 | Frame-constant block | 12 §2.2; S0-G2 | One ranged write a frame, rewritten after reset | No game write in the range over a pan; patched copies read it | S0-G2, S1-01 | offline | B | no |
| S3-02 | Lamp index cells and lamp table in the float atlas; object copies read them; per-draw picks off for those families | 12 §2.1; S0-05; `ShaderPatches` (`PatchObjectLampVs/Ps`), `features/lot_light_bridge.cpp` | `lampIndex` | Offline over every captured object shader; pictures equal to per-draw picks within 1/255 away from cell borders | S3-01, S0-05 | offline | A | no |
| S3-03 | Rig write, cached per model: lamps with visibility at the centre | 02 §4.2 route A; 12 §2.3; S0-09; `features/rig_tracker.cpp` | `rigFill` | Sim behind a wall gets no lamp; ceiling clamp counted; cache invalidates on lamp or model change | S0-09, S1-02 | offline | B | no |
| S3-04 | Directional layers and the directional-map copies | 12 §4.4a; S0-04 | Four-face gather output and the swap for the four maps | Counters and objects indoors show directional bounce in the offline render | S2-01, S0-04 | offline | A | no |
| S3-05 | Rig indirect: ambient-cube faces, dominant light, blend over space and time | 12 §4.4b | Vertex-light slot fill | Q8; no pop at a doorway | S3-03, S3-04, R-06 or asynchronous read | offline | B | no |
| S3-06 | Capsule occlusion for Sims in the AO pass | 12 §4.4c; `patches/ambient_occlusion_patch.cpp` | `simShadows = capsule` | Soft shadow under a Sim toward the dominant lamp; AO lab shift metric not up | S3-01, S0-G2 | offline | A | no |
| S3-G1 | Movers in game | - | Pictures; `LotLightBridge` ms a frame before and after | Q8; per-draw time of the covered fixes removed | S3-02, S3-05 | game: `realtime-gi-look`, `realtime-gi-pan` | C | no |

## S4: daylight, cube, every lot

| Id | Goal | Inputs | Deliverable | Acceptance | Deps | Run | Tier | Now |
|---|---|---|---|---|---|---|---|---|
| S4-01 | Window lights as emitters; cap by day | S0-08; 05 §4.4 | `windowLights`; `nightOnly = false` allowed | Q10 offline against the game's solved day maps | S0-08, S2-01 | offline | A | no |
| S4-02 | Per-Sim irradiance cube | 02 §4.2 route B; S0-G2 | `simCube` | A Sim's shading follows its normal in the offline render | S3-04, S0-G2 | offline | A | no |
| S4-03 | Every covered lot: neighbours, promotion and demotion, device reset, memory following free VRAM | 05 §5.3; 06 §4 | `lots = all` | Reset and lot switches leave no stale buffer; resources released on demotion | S1-G2 | offline | B | no |
| S4-G1 | Day and night, 60 min memory arm, laptop GPU class | 06 §3 | Result table | P8, Q10; VRAM recorded | S4-01, S4-03 | game: `realtime-gi-pan`, `realtime-gi-look` | C | no |

## S5: detail layers

| Id | Goal | Inputs | Deliverable | Acceptance | Deps | Run | Tier | Now |
|---|---|---|---|---|---|---|---|---|
| S5-01 | Contact shadows on the lamp share | 08 §4; S0-16 | `contactShadows` | No darkening of sun or ambient share; AO lab metrics hold | S3-01, S0-16 | offline | A | no |
| S5-02 | Rig shadows across Sims and rig-lit objects | 08 §8 | `rigShadows` | A wall's shadow edge crosses a Sim | S5-01, S3-03 | offline | A | no |
| S5-03 | Bounce detail in the AO slices and the multi-bounce AO fit | 08 §2; 12 §4.5 item 4 | `bounceDetail` | Zero-mean term; Q11 | X-03 | offline | A | no |
| S5-04 | Exact furniture on the fallback: bake-in caster cache | 08 §6 | `exactFurniture` for `d3d9` | No popping in the camera sweep | S0-G2, S1-04 | offline | B | no |
| S5-05 | Sim mesh shadows | 03 §2 row C | `simShadows = mesh` | Within the 0.3 ms budget for 4 Sims | S3-06 | offline | B | no |
| S5-G1 | Detail layers in game | - | Pictures and GPU ms a layer | Each layer within its cost | S5-01 to S5-05 | game: `realtime-gi-look` | C | no |

## S6: look, menu, pages, compatibility

| Id | Goal | Inputs | Deliverable | Acceptance | Deps | Run | Tier | Now |
|---|---|---|---|---|---|---|---|---|
| S6-01 | Look constants in the compose; `look`, `indirectIntensity`, `albedoBoost` | design §9; S0-17, S0-02 | Constants and the three controls | Q9 statistics pass on the captured lots | S0-17, S2-01 | offline | B | no |
| S6-02 | Menu, tooltips, 21 translations, presets | `AGENTS.md` menu and i18n rules | Menu entries and tables | `tools/i18n_check` and `tools/menu_audit.pl` pass | S6-01 | offline | C | no |
| S6-03 | Feature, validation and history pages; README index | `docs/DOCUMENTATION-STANDARD.md` | Pages from the templates | Review checklist of the standard | S6-01 | desk | C | no |
| S6-04 | Compatibility: EA app build sites by pattern, native Direct3D 9 and in-process DXVK, other mods, stand-down matrix | 06 §4 R15, R16 | Stand-down rules and notes | Feature stays off where a site is missing; no picture change with it off | S4-03 | offline | B | no |
| S6-G1 | Full gate | design §8, §9 | Result tables and picture pairs | P1-P13, Q1-Q12 | all | game: all jobs, camera sweep | C | no |

## Track X: adjacent work (own proofs, not gated by GI)

| Id | Item | Source | Now |
|---|---|---|---|
| X-01 | Asynchronous render-target read in the bridge, pick integration | 11 §5.1, §5.2 | in progress elsewhere |
| X-02 | Pick skip while camera and cursor are still | 09 §2a | yes |
| X-03 | AO: slope-adjusted edges, quadrant-weighted normals, edge-aware 4x4 box | 10 §1, §2 | yes |
| X-04 | AO: one slice count a frame by zoom | 10 §3 | yes |
| X-05 | 2D radiance cascades a story, offline experiment | 12 §4.2c | after S0-06 |

## What can start now

All in parallel, none needs the game:

- Desk: S0-03, S0-04, S0-08, S0-09, S0-10, S0-12, R-01, R-08.
- Offline tools and code: S0-01, S0-02, S0-05, S0-06, S0-07, S0-11, S0-13, S0-14, S0-15, S0-16, R-02, S1-01.
- Adjacent: X-02, X-03, X-04.

First followers: R-03 after R-01; S0-17 after S0-01; the three S0 game runs after S0-11, S0-14 and S0-15.

## Gates

| Gate | Decides | From |
|---|---|---|
| G-split | Whether S1's gain justifies the cap as designed | S0-G1 |
| G-server | Whether the server back end leads S2 | R-02, R-03 |
| G-scale | Which families may use scaled float copies | S0-03, S0-04 |
| G-rig | Whether the rig write is possible as designed | S0-09 |
| G-constants | Whether the frame-constant block and lamp index can ship | S0-G2 |
| G-stage1 | S1 continues to all lots | S1-G2, S1-G3 |

## To fold in

Research page 13 (bridge batching, free constant ranges): when it arrives, update S0-14, S0-G2, S0-G3, S3-01 and
R-01 with its findings and drop any package it already answers.
