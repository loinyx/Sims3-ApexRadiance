# G-buffer and motion vectors for a forward D3D9 game: what it takes

Research for [the idea](../idea.md). Static analysis only: no game run, no code changed. It answers the question
"what stops us from having normals, albedo, roughness and motion vectors per pixel, so that ReSTIR, NRD, DLSS,
temporal accumulation and per-pixel ray tracing become possible?"

**Short answer:** nothing in D3D9, shader model 3, DXVK or the bridge forbids it. The cost is breadth (7,859 pixel
shaders, of which the generic part of the patch fits all and the "which value is the albedo" part fits about two
thirds without a family rule), one hard corner (motion of skinned Sims) and the work on the 64-bit side for every
consumer that is not plain HLSL (DLSS, NRD, ray queries). Camera motion vectors need no patch at all. A first
G-buffer (albedo, normal, material id) is an extension of two things Apex already ships: the Banding Fix's
per-shader copies and the scene-draw binder.

Earlier pages said "TS3 has no G-buffer" and stopped there (`12` §7.1, §7.4 R5 / R6; `02` §3.4 "deferred shading with
an albedo target: not possible without rewriting every shader's output"). This page keeps that verdict for *deferred
shading* (the tone curve and bloom mask stay in the forward shader) and withdraws it for a *side* G-buffer written
next to the unchanged forward colour.

## 0. Sources and marks

| Mark | Meaning |
|---|---|
| **V-precomp** | Measured for this page from `<Game>\Bin\Shaders_Win32.precomp` (Steam 1.67.2), read-only, with the walk of `tools/precomp.cjs` extended as described below |
| **V-doc** | Stated as verified in Apex's docs or source (file named) |
| **Inferred** | Reasoned, not measured. Needs the smoke test of its work package |
| **Memory** | From general knowledge of D3D9, DXVK or an SDK; not re-read for this page |

How the new precomp numbers were made:

- Per pass, after the `PARM` blocks comes a **render-state list**: a count, then `(D3DRS state, value)` pairs, then the
  sampler mask and names of `02` §0 (new finding, **V-precomp**: 147,471 of 163,598 pass records parse this way; the
  states seen are 168 `COLORWRITEENABLE`, 27 `ALPHABLENDENABLE`, 19 / 20 blend factors, 15 `ALPHATESTENABLE`,
  14 `ZWRITEENABLE`, 23 `ZFUNC`...). "Blend" below means the pass sets `ALPHABLENDENABLE = 1`; "unset" means the pass
  does not name the state (it inherits the default, which is off for the opaque scene: **inferred**).
- Outputs, texture reads and their coordinate registers are counted from the token stream.
- The numbers count **shaders**, not pixels on screen. Screen coverage needs one run (§6, WP3).

## 1. What exists today

| Piece | State | Source |
|---|---|---|
| Scene depth, readable | INTZ texture in place of the game's depth-stencil (`DepthShare`), `d = A - near·A / z`, A = 1.00008 | **V-doc** `features/depth_share.h`, `post_scene.h` |
| Camera view-projection of the frame | `PostScene::CameraViewProj` (vertex constants `c40..c43`, voted over the first scene draws), near plane per frame | **V-doc** `features/post_scene.cpp` |
| Normal from depth | 3 taps per axis, smaller depth step wins (in the AO shader) | **V-doc** `patches/ambient_occlusion_patch.cpp:164`, `10` §1 |
| A copy of **every** scene pixel shader, bound per scene draw | Banding Fix: `AddDither` (ps_3_0) and `AddDither2` (ps_2_0 as a ps_2_x copy), bound by the shared `SceneBinder` only for back-buffer draws with the depth test on. Offline: ps_3_0 4,907 of 4,907, ps_2_0 2,996 of 3,104 accepted. Measured cost: 0.086 ms a frame over 2,002 draws | **V-doc** `features/scene_dither.cpp`, `00` §7 |
| A copy of every scene vertex shader that rewrites the final position | `AddJitterVs` (constant `c252`), made for the removed temporal SMAA; coverage counters per frame | **V-doc** `features/shader_patches.h:458`, `scene_dither.h` |
| A replay of selected draws with paired shader copies into an Apex target | AO Sim receiver mask (`MakeAoReceiverMask`, replay at priority Early) | **V-doc** `features/shader_patches.h` |
| Two render targets at once | `rt_outdoor_lamps.cpp` checks `NumSimultaneousRTs >= 2` and writes `COLOR1` in its compose pass, also under the bridge | **V-doc** |
| Shader identity at run time | Bytecode hash tables generated offline from the precomp (`shaders/shader_ids.h`, `sim_receiver_ids.h`, `tools/ao_receiver_test/generate_ids.cjs`) | **V-doc** |
| Previous-frame anything | None. Temporal AO and temporal SMAA were removed | **V-doc** `docs/removed-features.md`, `docs/history/edge-smoothing.md` |

Two removals matter as evidence:

- **Temporal AO** (removed-features, items 5-7): camera-only reprojection from depth with `prevVP · inv(VP)` was
  exact (0.05 px with a still camera). What failed was **leaves in the wind**: movers without motion vectors lost
  their history at every edge and the raw frame showed through.
- **Temporal SMAA (T2x)**: the jitter itself worked; removed for pool-water artefacts (one vertex family) and the
  cost of history targets.

Both failed on the part this page adds: per-pixel knowledge of *which pixels moved on their own*.

## 2. Motion vectors

Convention: `mv = current NDC xy - previous NDC xy` of the same surface point, without jitter, in a `G16R16F` target.
All matrix products are made on the CPU in double precision as one clip-to-previous-clip matrix
`D = prevViewProj · inverse(viewProj)`, so world coordinates near (927, 70, 1200) never reach a float shader.

### 2a. Camera only (static world): no patch

One full-screen pass at the scene boundary (a `PostScene` effect before Ambient Occlusion): read INTZ depth, rebuild
the clip position from the pixel and `d`, multiply by `D`, divide, subtract.

| | |
|---|---|
| Inputs | INTZ depth, this frame's and the last frame's `CameraViewProj` (keep one 4×4 per frame; `WantCamera(true)`) |
| Covers | Everything that does not move by itself: terrain, lots, walls, furniture at rest. In live mode that is nearly every pixel except Sims, pets, cars, doors in motion, swaying foliage, water, particles |
| GPU | 1 depth read + ~12 instructions per pixel into 16.6 M bytes at 4K: about 0.1 ms (**inferred** from the AO pass sizes) |
| CPU | One matrix inverse a frame, 4 state calls |
| Camera cut | Screen centre moved > 0.3 NDC (the test T2x used) or no camera last frame: write "invalid" for the whole frame |
| Known exact | The removed temporal AO measured this reprojection at 0.05 px error (**V-doc**) |

### 2b. Movers

Three tiers. Each is complete without the next one, because 2c catches what a tier does not cover.

**Rigid movers (objects, doors, cars, held items): one generic vertex patch, no pattern per family.** For a rigid
draw `clip = VP · W · p`, so `prevClip = (prevVP · prevW) · inverse(VP · W) · clip`. The vertex copy that `AddJitterVs`
already makes has the final position in a temp `rP`; the patch appends `oT_k = D_draw · rP` (4 `dp4`, constants
`c248..c251`, next to the jitter constant `c252`). `D_draw` is the camera `D` by default (set once a frame) and a
per-draw matrix only for draws whose world rows changed.

- World rows: `c8..c10` on "nearly all world geometry" (**V-doc** `docs/engine/shaders.md`). Apex's
  `SetVertexShaderConstantF` hook already sees them.
- Draw key for "the same object last frame": (vertex buffer, index buffer, start index, vertex shader). Several
  instances of one mesh (two equal chairs) share a key: match by nearest previous translation `(c8.w, c9.w, c10.w)`.
  A key seen for the first time has no previous matrix: flag (2c).
- Free registers: vertex shaders stop at `c227` of 256; `vs_3_0` uses at most 11 of 12 outputs; 4,728 of 4,782
  `ps_3_0` have a free input (**V-precomp**).
- Per-draw CPU: a hash lookup for draws whose `c8..c10` were set since the last draw (~0.05-0.1 µs), and one
  4-register upload (0.4 µs, `03` §0) only for the few draws that moved. Estimate 0.05-0.2 ms a frame at 2,000 draws
  (**inferred**).

**How the value reaches the target: a mover pass, not a third MRT in the main pass.** Re-draw only the movers after
the scene into the motion target with depth test `EQUAL`, a trivial Apex pixel shader and the vertex copy above
(the AO Sim receiver replay is the same mechanism). Tens of draws a frame, works for `ps_2_0` materials, and the
main pass keeps two extra targets instead of three. Writing `oC3` from every material copy is the later
optimisation if the replay count grows.

**Skinned movers (Sims, pets: 1,232 vertex shaders declare `BLENDINDICES`, **V-precomp**).** The bone palette reaches
`c227`; a second palette for the previous pose does not fit beside it in a patched copy. Options, cheapest first:

| Option | What | Gives | Cost |
|---|---|---|---|
| S0 | Flag only (2c) | Honest "no history" on Sims | 0 |
| S1 | Rigid approximation: the model's root transform delta as `D_draw` | The walk across the screen, which is most of a Sim's screen motion; limbs wrong, still flagged "approximate" | Same code as rigid movers. Whether Sim draws carry a model matrix in `c8..c10` is **not verified** |
| S2 | Dedicated replay vertex shader (own HLSL, position only): constants = view-projection + both palettes. Fits when a draw uploads ≤ ~40 bones (4 + 2 × 120 registers ≤ 256) | Exact per-vertex motion | Needs the bone count per draw (count the `SetVertexShaderConstantF` range at a Sim draw: one log line) and the skinning layout of the vertex declaration. 2-4 h |
| S3 | Previous palettes in a float texture, read with `texldl` in a `vs_3_0` replay shader, one row per skinned draw, one upload a frame | Exact, any bone count | 4-8 h; `vs_2_0` Sim variants stay on S1 |

**Vertex animation without a matrix (foliage sway, SpeedTree, flags, water):** no previous state exists outside the
shader. Flag by family (2c). Exact motion would need the wind time constant of the previous frame fed to a second
evaluation of the sway code: a per-family patch, not planned.

### 2c. Mask for unknown movers

A 2-bit **motion class** per pixel, written with the G-buffer (§3) into the 2-bit alpha of the normal target and
overwritten by the mover pass:

| Class | Meaning | Consumers do |
|---|---|---|
| 0 | Static: camera vector is exact | Full history |
| 1 | Exact object vector written (rigid, S2, S3) | Full history |
| 2 | Approximate (S1) | History with tight clamp |
| 3 | Unknown (S0, foliage, water, particles behind, first-seen key, refused shader) | Reject history, use this frame only (spatial filter) |

The class comes for free from the shader's **technique** (Sim*, leaf*, frond, branchMain, Foliage, water families =
3 by default), baked into the copy as a `def`. This is the piece the removed temporal AO lacked. For DLSS it maps to
the "bias current colour" mask, for FSR to the reactive mask (**Memory**).

## 3. G-buffer

### 3.1 Layout

`RT0` stays the game's back buffer. All extra targets are 32 bits per pixel like the back buffer, so the
independent-bit-depth capability is not needed.

| Target | Format | Content |
|---|---|---|
| `RT1` | `A8R8G8B8` | rgb = albedo (the texture colour before lighting), a = material id (8 bits) |
| `RT2` | `A2B10G10R10` | rg = world normal, octahedral, 10 + 10 bits; b = roughness (10 bits); a = motion class (§2c) |
| motion | `G16R16F` | Not bound in the main pass: full-screen pass (§2a) + mover pass (§2b) |

Reserved ids in `RT1.a`: 0 = nothing written (clear value: sky, UI), 255 = "albedo unknown" (§3.3). A flag in the
low roughness values says "normal is geometric, not written" when a shader has no usable normal.

### 3.2 The generic patch (every shader)

Facts that make one patcher fit all (**V-precomp**, all 7,859 technique pixel shaders):

- Every shader writes **only `oC0`**: no `oC1..3`, no `oDepth`, no subroutines. `ps_3_0` writes it 2 times in 4,496
  shaders (1 in 160, 3 in 126); `ps_2_0` exactly once.
- Temps: at most 12 used (`ps_3_0` has 32; `ps_2_0` has 12, hence the ps_2_x copy below).
- Slots: `ps_3_0` at most 343 of 512 (`02` §1.1); the patch adds 4-14.
- 986 `ps_3_0` use flow control. The appended writes go after the last instruction, outside any branch, like the
  Banding Fix's (4,907 of 4,907 accepted with the same shape).

`AddGBufferPs(tokens, info)` therefore:

1. Finds the albedo read (§3.3) and copies its result to a free temp right after the `texld` (the game may reuse
   the register).
2. Finds the normal (§3.4) and copies it to a free temp at its last write before use.
3. Appends: octahedral encode (~8 slots), `mov oC1, rAlbedo` with `.a` from a `def` (material id), `mov oC2, ...`
   with roughness and class from `def`s or the specular read.
4. Always succeeds in writing *something*: with no albedo found, `oC1 = (0, 0, 0, 255/255)`; with no normal, the
   "geometric" flag. So a patched shader never leaves stale data under its pixels.

`info` comes from a table generated offline from the precomp (bytecode hash → albedo sampler, normal source,
material id, class), like `sim_receiver_ids.h`: the sampler **names** exist only in the precomp's pass records,
not in the bytecode. About 7,859 × 8 bytes. A hash that is not in the table (EA 1.69, mods with their own shaders)
gets the "unknown" outputs.

**Merge with the Banding Fix copy.** One copy per shader carries the grain and the G-buffer outputs, made at the
shader's first scene draw and bound by the existing `SceneBinder`. No second set of shader objects (the Banding
Fix history records +0.3 GB in 30 minutes when every created shader got a copy at once), no extra bind per draw.

### 3.3 Albedo

| | `ps_3_0` (4,782) | `ps_2_0` (3,077) |
|---|---|---|
| Never used with blending on (candidates for G-buffer writes) | 4,091 | 2,536 |
| ... of those, a pass names an albedo texture (`DiffuseMap`, `colorMap`, `RoadTexture`, `TwigDiffuseMap`, leaf textures) | 2,712 (66%) | 1,765 (70%) |
| `DiffuseMap` bound, all versions of blend | 2,730, of which **2,696 read it with exactly one `texld`** | 1,795, 1,767 with one |

The rule "albedo = the result of the single `texld` on the sampler the pass names `DiffuseMap`" is automatic for
those. Families it does not cover, with what each needs:

| Family (candidates without an albedo name) | Why | Route |
|---|---|---|
| SimSkin (650 `ps_3_0`, 87 `ps_2_0`), SimHair, SimEyes, SimRobot, SimHorse | Skin and clothing are composited: all 654 SimSkin shaders bind `OverlayTexture`, plus tone ramps and detail maps | One family rule (which register holds the composited colour before the rig light is applied): **inferred**, needs 2-3 disassemblies. Until then id 255 |
| TerrainHigh (352 + 96), TerrainLow, LotImposter | Pass records without a parsed name list; terrain blends up to 4 `colorMap` layers by `blendMap` | Family rule: the register after the layer blend. Until then id 255 |
| Road (264) | `RoadTexture` + `RoadDetailMap` | Covered by name (`RoadTexture`); detail multiply ignored |
| Glass, water, particles, CAS passes | Blended or special | Excluded (§3.5) |

"Albedo unknown" is a usable state: consumers that demodulate (denoisers, bounce colour) treat it as mid grey and
skip demodulation there.

Not covered by any rule: multiply maps, dirt overlays, snow blended over the diffuse (`TerrainSnowDiffuseMap` is
bound by 2,315 shaders). The first version writes the raw `DiffuseMap` colour; a snowy roof then has a roof-coloured
albedo under white light. Acceptable for bounce tint and demodulation; wrong for a path tracer.

### 3.4 Normal and roughness

No `ps_3_0` receives a `NORMAL`- or `TANGENT`-usage input: everything arrives as `TEXCOORD` (4,767 shaders) and
`COLOR` (3,972) (**V-precomp**), so "the normal register" cannot be read off the declarations. An automatic handle
exists anyway:

- **The coordinate of the diffuse light-probe read is the shading normal in world space** (**inferred**: an
  irradiance cube is looked up by the normal; to confirm on five families by disassembly). 3,323 of the 4,091
  candidate `ps_3_0` bind `ExteriorDiffuseLightProbe` / `DiffuseLightProbe`; **3,144 read it once with the
  coordinate in a temp**, which is the normal *after* normal mapping (2,346 candidates bind a normal map).
  `ps_2_0`: 1,727 of 2,536 bind the probe, 1,372 in that simple shape.
- Without a probe read (TerrainHigh 392, interior floors and their strobe variants ~110, some objects): normal from
  depth in the consumer, as today, flagged. Interior floors and walls are planes, where the depth normal is exact
  away from edges. A family rule for the normal-mapped interior floors (they read `LightBasisMap0..3` with the
  normal's components) is a later refinement.
- 608 candidate `ps_3_0` have neither a probe nor a normal map: flat geometric normals lose nothing there.

Roughness: `SpecularMap` / `SpecCompositeTexture` is bound by 1,676 candidate `ps_3_0` (692 `ps_2_0`). First
version: a constant per material id (wall, floor, wood object, metal, skin, hair, glass, water, foliage, terrain,
road, roof: about 16 ids from the 296 technique names; a shader shared by several techniques, 971 of them, takes
the first in a fixed priority order). Later: `1 - specular mask` where the map is read once.

### 3.5 What does not write

| Case | Handling |
|---|---|
| Draws with `ALPHABLENDENABLE = 1` (glass, hair cards, particles, decals, the lot light pass on terrain, 691 `ps_3_0` + 541 `ps_2_0` that are only ever blended) | `COLORWRITEENABLE1..2 = 0` while blending is on. Tracked in the render-state hook: 2 state calls per blend toggle. The pixel keeps the opaque surface behind it |
| Alpha-tested draws (`texkill` in 1,483 `ps_3_0`, 1,027 `ps_2_0`, or `ALPHATESTENABLE`) | Write normally: a killed or rejected pixel writes no target |
| Scene draws with a refused or unknown shader, or fixed function | Write mask off for that draw; counted per frame (`SceneBinder::Coverage` style). Their pixels keep stale values: v1 accepts that and logs the count; if it is visible, add a 16-bit depth check channel (§5 WP9) |
| Other views: shadow maps, water reflection, mirrors, thumbnails, the pick pass, impostors | Never bound: `RT1..2` are set only while `RT0` is the back buffer, and unset at every `SetRenderTarget(0, other)` (D3D9 requires equal sizes). Copies that write `oC1` into an unbound target are harmless (**Memory**; smoke WP0) |
| Game MSAA on | G-buffer off (multisampled back buffer; the same rule as Edge Smoothing, Depth Blur and AO today) |
| UI | Drawn after the `PostScene` boundary, where the extra targets are already unbound |

### 3.6 `ps_2_0`

Not optional: in game "more than half of the scene draws, the walls among them, were ps_2_x" (**V-doc**
`scene_dither.cpp`), and whole families exist only as `ps_2_0` (strobe / blacklight walls, Rug, Painting, SimEyes,
SimRobot, all foliage; `02` §2). `ps_2_0` has `oC0..oC3` (**Memory**), so no model change is needed for the outputs,
but 12 temps (12 used at most) and 64 arithmetic slots (11 shaders use ≥ 60) are tight. Same answer as `AddDither2`:
a **ps_2_x copy** (more temps and slots, `PS20Caps` checked), accepted for 2,996 of 3,104 there. The G-buffer part
is easier than the grain: it needs no pixel position, so no paired vertex copy.

### 3.7 Coverage and patch success (estimates)

| Measure | `ps_3_0` | `ps_2_0` | Basis |
|---|---|---|---|
| Generic patch accepted (writes at least id + class) | ~100% | ~96% | Banding Fix offline results with the same patch shape (**V-doc**); to be re-run for the new patcher |
| Albedo automatic by name | 66% of candidates | 70% | **V-precomp** |
| + SimSkin and terrain family rules | ~91% | ~80% | **V-precomp** counts of those families; rules **inferred** |
| Shaded normal automatic (probe coordinate) | 77% of candidates | 54% | **V-precomp**; semantics **inferred** |
| Rest of the normals | depth normal, exact on planes | same | **V-doc** (shipped AO path) |
| Screen pixels with albedo + shaded normal in a furnished interior | unknown: walls and floors (albedo by name: yes; normal: plane) + objects (both automatic) suggest > 85% | | **Inferred**; WP3 measures it with a debug view |

Patch success is not the risk; the 60-96% acceptance of Apex's lighting patches (`02` §2) came from matching each
family's *lighting code*. This patch matches one `texld` by sampler index and appends.

### 3.8 Cost

| | Estimate | Basis |
|---|---|---|
| CPU per draw | ~0 added: copies are made at shader creation / first scene draw, and the binder already binds one copy per scene draw (0.086 ms a frame for 2,002 draws, **V-doc** `00` §7). Without the Banding Fix the same binder cost appears once | |
| CPU per frame | Bind `RT1..2` when the scene starts, unbind at each target switch and at the boundary, write masks at blend toggles: 50-300 state calls × 0.4 µs = 0.02-0.12 ms | `03` §0 call costs (**V-doc**); toggle count **inferred** |
| Shader creation | One patch per shader at first scene draw: microseconds each, plus the server's compile of the copy (already paid when the Banding Fix is on) | |
| GPU memory at 4K | 8.29 M pixels × 4 bytes = 33 MB per target: `RT1` + `RT2` + motion = 100 MB; with history (previous depth, previous normal) ~170 MB | arithmetic |
| GPU bandwidth at 4K | 2 extra targets × 33 MB × overdraw ~2 = ~130 MB a frame, 8 GB/s at 60 fps: a few percent of a laptop RTX 3060's memory bandwidth. Estimate +0.3-0.8 ms at 4K, +0.1-0.2 ms at 1080p, plus 2 clears | **Inferred**; measure after the proof, not before |
| Game address space (32-bit) | Handles only. `D3DPOOL_DEFAULT` render targets that are never locked live in the 64-bit server (**V-doc** `07` §1, `03` §7). The id table is ~63 KB. No new shader copies when merged with the Banding Fix copies | |
| Without the bridge (in-process DXVK) | Works the same; the targets are video memory, not mapped into the process (**inferred**, `03` §7) | |

Capabilities to read once at start (WP0): `NumSimultaneousRTs` (DXVK: 4, **Memory**; Apex already requires ≥ 2
elsewhere), `D3DPMISCCAPS_INDEPENDENTWRITEMASKS`, `D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING`, and whether fog or
sRGB write touch the extra targets under DXVK (`ps_2_0` draws with fixed-function fog).

## 4. What each buffer unlocks

"D3D9 side" = an Apex HLSL pass in the game process, shader model 3. "Server side" = Vulkan in the 64-bit bridge
server, which needs a bridge patch that hands the server the G-buffer surfaces and the frame's matrices through a
custom command (Apex ships 13 bridge patches already; `12` §7.1).

| Technique | Needs | Where | Fit and limits |
|---|---|---|---|
| **Temporal accumulation of AO / GI / contact shadows** | depth, camera motion, class mask; better with normals | D3D9 side | The cheapest win and the direct repair of the removed temporal AO: history rejected by class 3 instead of twinkling at leaf edges. Lets AO run at fewer samples (`10` §5 item 7) |
| **AO and SSGI with real normals** | `RT2` | D3D9 side | Removes the dashed lines along mesh seams (`10` §1: failure 2) and gives normal-mapped detail |
| **Own TAA** | jitter (`AddJitterVs`), motion, class | D3D9 side | Brings back what T2x tried, with a mask for the pool family instead of artefacts. Replaces nothing until proven |
| **SSR** | depth, normal, roughness, previous colour | D3D9 side | Apex's Reflections already ray-march depth; roughness and normals let it leave water: polished floors, counters, mirrors' surroundings. Screen-space limits stay |
| **Bounce colour for the texel-space GI** | `RT1` | D3D9 or server | `12` §4.3 needs a surface colour per light-map texel: average the albedo target into the room map's texels of visible walls and floors. Free second-bounce tint |
| **Per-pixel ray-query shadows (R4 of `12`)** | depth, normal; motion + class for the denoise | Server side | Sharp contact shadows of furniture and Sims on everything, which texel density cannot give (`02` §3.2 point 3). 1 ray a lamp a pixel is noisy or hard: needs the denoiser below |
| **NRD (SIGMA for shadows, ReBLUR / ReLAX for GI)** | depth (view z), normal + roughness, motion, optionally albedo to demodulate | Server side | Makes R4 soft at 1 ray a pixel. Class-3 pixels reset its history (`disocclusion` inputs). Without Sim motion, Sims in shadow shimmer: S1 at least |
| **ReSTIR DI** | depth, normal, albedo, motion, a lamp list, rays for visibility | Server side | Many lamps per pixel at a fixed cost: a street or a party lot at night, where the rig's 3 lamps and the stamp fall short. Not needed inside one lot (`12` R5). The lit result must be *added* to the forward colour: see the limit below |
| **DLAA / DLSS Super Resolution** | colour before the UI, depth, motion, jitter, exposure; class as bias mask | Server side | DLAA (native resolution) is the realistic first step: it needs no change of render size. Super Resolution needs the scene drawn smaller than the window while the UI stays native: the game draws both into one back buffer, so the scene would have to be redirected into an Apex target with scaled viewports. A project of its own |
| **DLSS Ray Reconstruction** | albedo, specular albedo, normal, roughness, motion + a noisy ray-traced signal | Server side | Only once per-pixel ray tracing exists; replaces NRD on RTX cards |
| **Frame generation** | colour without UI, depth, motion, UI separately | Server side | DLSS Frame Generation needs an RTX 40 card: not the target machine class "RTX 30+". FSR 3 frame generation runs on any card. Sim motion (S2 / S3) matters most here: generated frames show every wrong vector |

**The limit that stays: the forward colour is final.** `RT0` already contains the game's lamp light, tone curve and
bloom mask. A per-pixel technique can (a) multiply (AO, shadows of a light Apex knows the share of), (b) add
`albedo × new light` for light the game did not draw, or (c) replace reflections. It cannot remove the game's own
lamp term per pixel. That is why this page complements the texel-space plan instead of replacing it:

| Job | Owner |
|---|---|
| Static direct and bounced light of rooms: converges once, no motion vectors, every view, `ps_2_0` included | Texel space (`02` §3.2, `12` §4, §7 R0-R3) |
| What texels cannot resolve: contact shadows, Sims and moving objects as casters and receivers, glossy reflections, anti-aliasing, noise removal over time | G-buffer + motion (this page) |
| Shared | The lamp table, the shadow atlas or the ray-tracing scene, the bridge commands |

## 5. Licences and patents

| Item | Licence (**Memory** unless marked; read the current text before shipping) | Consequence for a free MIT mod |
|---|---|---|
| DLSS SDK (Super Resolution, DLAA, Ray Reconstruction, Frame Generation), Streamline | NVIDIA RTX SDKs licence / DLSS licence: free of charge, binary redistribution with the application allowed, attribution and notice rules, NVIDIA-only at run time | Allowed. Ship the DLLs in the optional bridge download with their licence text, as the bridge download already does for its parts. Not part of the MIT source |
| NRD, RTXDI | NVIDIA RTX SDKs licence (**verified** in `12` §7.4) | Allowed for a free mod; source cannot be relicensed MIT. Keep it server-side and separate |
| FSR 2 / 3 (upscaling, frame generation), FidelityFX denoiser | MIT | No constraint; any GPU |
| XeGTAO | MIT (`10`) | No constraint |
| XeSS | Intel binary licence | Allowed; optional |
| SVGF, ReSTIR, variance clipping, octahedral normals | Published papers | Own implementations. ReSTIR and some temporal techniques are covered by patent applications of their authors' employers: noted, not treated as a constraint for this project |

## 6. Work packages

Each is 1-2 hours of agent work, with a smoke test that proves the piece works; numbers (ms, coverage on screen)
come after the proof. Game runs are listed separately. Offline harnesses are read-only over the precomp, as
`tools/looks_fx_check` and the Banding Fix checks are.

| WP | What | Time | Model | Smoke test | Game run |
|---|---|---|---|---|---|
| 0 | Capability log line (§3.8) and an empty `RT1` bound during the scene, no shader change; dev view of the target | 30 min | Sonnet | Picture unchanged, AO / Depth Blur / Edge Smoothing still run, log shows the caps | 1 short |
| 1 | **Camera motion vectors**: previous view-projection, `D` in double, full-screen pass into `G16R16F`, colour-coded dev view; unit check of `D` with synthetic matrices | 1 h | Sonnet | Unit check passes; still camera = zero everywhere; a pan = one smooth gradient; a cut = "invalid" frame | 1 short |
| 2 | **`AddGBufferPs` for `ps_3_0`** + offline table generator (hash → albedo sampler, probe sampler, id, class) + harness over the precomp. First scope: the 5 largest `ps_3_0` families: SimSkin 654, Day 498, TerrainHigh 480, Phong 444, Landmark 432 | 1.5 h | Opus | Harness: every shader of the 5 families accepted, copies assemble on native D3D9 limits, `oC0` tokens unchanged. Expected: Day, Phong, Landmark with albedo + probe normal; SimSkin and TerrainHigh with id 255 (they are the two families without a named albedo, §3.3) | none |
| 3 | Bind logic in `SceneBinder`: `RT1..2` on / off with the scene target, write masks at blend toggles, merge with the grain copy, dev views (albedo, normal, id, class), per-frame counters (patched / refused / masked draws) | 1.5 h | Opus | Screenshots of the 4 views on one furnished lot: objects coloured by texture, normals change with the surface, UI absent, no stale trails behind glass or particles | 1 |
| 4 | Widen WP2 to all `ps_3_0`; add ExteriorFloors, the wall and floor families and Counters explicitly to the harness report | 45 min | Sonnet | Harness totals per family; accept ≥ 99% | none |
| 5 | `ps_2_0` as ps_2_x copies (walls) | 1.5 h | Opus | Harness accept ≥ 95%; walls appear in the albedo view | 1 short |
| 6 | First consumer: AO takes `RT2` normals where flagged valid (switch, default off) | 1 h | Sonnet | Side-by-side picture pair: seam dashes gone on a counter top | 1 short |
| 7 | Temporal core: history targets, reprojection with the motion target, rejection by depth, id and class, variance clip; used by AO behind a switch | 2 h | Opus | Still camera: converges, no twinkle; pan: no trails on walls; trees: no twinkle (class 3 falls back to the spatial filter) | 1 |
| 8 | Rigid mover vectors: `oT_k = D_draw · rP` on the jitter vertex copy, draw-key cache of `c8..c10`, mover replay pass, class 1; S1 for Sims if they carry a model matrix (one log line decides) | 2 h | Opus | Motion view: an opening door and a driving car show their own colour; a walking Sim shows a uniform vector (S1) or class 3 | 1 |
| 9 | Only if WP3's counters show stale pixels: depth-check channel for unpatched scene draws | 1 h | Sonnet | Stale counter view empty | 1 short |
| 10 | Family rules: SimSkin albedo, terrain layer albedo | 1.5 h | Opus | Harness + albedo view: Sims and ground coloured | 1 short |
| 11 | Bridge patch: custom command that names the depth, `RT1`, `RT2`, motion and colour surfaces and sends the matrices and jitter; a server-side compute pass that only copies one of them to the screen corner | 2 × 2 h | Opus | The corner shows the server's view of the normal target | 1 |
| 12 | Jitter back on (`AddJitterVs`, pool family masked class 3) + own TAA resolve on the temporal core | 2 h | Opus | Thin rails stable in a slow pan; pool water without triangles | 1 |
| 13 | Skinned motion S2 (bone count log first; S3 if > 40 bones) | 2-4 h | Opus | Motion view: a waving arm has its own gradient | 1 |
| 14+ | Server-side consumers, each on WP11: DLAA (2-4 h + SDK), ray-query contact shadows with SIGMA, FSR 3 frame generation | each 2-4 h | Opus | per consumer | 1 each |

Order of value for the least work: WP0-3 are the proof (about 4.5 h of agent work, 3 short runs). WP5-7 turn it
into a visible gain on the D3D9 side with no bridge change. WP11 is the gate for everything named in the question
that is an NVIDIA SDK or needs rays.

## 7. What stops us: the list

| # | Obstacle | Kind | Answer |
|---|---|---|---|
| 1 | 7,859 shaders to patch | Breadth | One generic append patch + an offline table; the Banding Fix proves the shape on every shader |
| 2 | "Albedo" is not one texture in Sim and terrain shaders | Breadth | Two family rules (WP10); "unknown" id until then |
| 3 | No normal semantic in the pixel shaders | Analysis | Probe-read coordinate (to confirm) or depth normal |
| 4 | Skinned and vertex-animated motion | Real cost | Class mask first; S1; S2 / S3 for Sims; foliage stays masked |
| 5 | Blended surfaces have one layer | Inherent | Mask; same as every deferred renderer |
| 6 | The forward colour is final | Inherent | Add, multiply or replace reflections only; rooms stay in texel space |
| 7 | DLSS, NRD, rays live in Vulkan, the game speaks D3D9 | Work | Bridge patch + custom command (WP11); DLAA before Super Resolution |
| 8 | Game MSAA, other views, mods' own shaders, EA 1.69 | Scope | Off / never bound / "unknown" id / a second table |
| 9 | Past temporal features were removed | History | They lacked exactly the class mask and mover vectors of §2 |

## 8. Not verified

- That DXVK leaves an unbound `oC1` write harmless and applies fog and sRGB write to `RT0` only (WP0).
- That "unset" `ALPHABLENDENABLE` in a pass means off at draw time for the opaque scene (the run-time hook uses the
  device state, so the patch does not depend on it; only the counts of §3.3 do).
- The probe coordinate as world shading normal (5 disassemblies in WP2).
- The pass records of TerrainHigh and PhongAlpha that parse without names (same gap as `02` §6).
- Sim draws and `c8..c10`; bones uploaded per Sim draw (WP8 / WP13 log lines).
- Every GPU millisecond on this page.
- Licence texts were not re-read for this page.
