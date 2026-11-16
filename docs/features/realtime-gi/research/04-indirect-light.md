# Indirect light in D3D9 shader model 3: technique comparison (lane R4)

Desk research for `../idea.md` section 2 ("Indirect light"). No game run, no measurement: every cost below is an
estimate unless marked **verified** (read in a source or in the Apex source of this worktree). Tags: **verified**,
**community/paper-reported**, **inferred**.

## 0. Constraints that decide the answer

| Constraint | Consequence | Tag |
|---|---|---|
| D3D9 SM3: no compute, no UAV, no geometry shader, no atomics | Every scatter is a point draw (one vertex per sample) with additive blending; every gather is a full-target pixel pass | verified (API) |
| **No render-to-volume in D3D9** | A grid that the GPU writes must be a 2D atlas of slices ("unwrapped volume"), sampled with a manual trilinear (2 bilinear taps + lerp). Kaplanyan: rendering into a volume is "not possible with the Microsoft DirectX 9.0c API", and the PC DX9 path used an "unwrapped volume texture (a 2D texture which emulates 3D texture)" | verified [1] |
| Volume textures still exist for read-only data | `CreateVolumeTexture` works and the Remix bridge client has `d3d9_volumetexture.cpp` / `d3d9_volume.cpp`, but filling one means `LockBox` from the CPU: the bridge client keeps a CPU copy of locked resources and ships it across processes | verified [8][9], Apex `docs/features/rtx-remix-bridge.md` |
| Vertex texture fetch, MRT x4, fp16/fp32 RTs, fp16 blending | RSM read in the vertex shader of a point draw (the CryEngine 3 injection), 3 MRTs = 3 colour channels x 4 SH coefficients in one pass | verified [1] (Xbox 360 used VTF, PS3 render-to-vertex-buffer) |
| The CPU and each D3D call are the limit, not the GPU (profile `00`: render thread CPU-bound; every call crosses the bridge) | Prefer **few draws with many pixels/points** over many draws. A technique that needs per-slice or per-object scene redraws (voxelization, per-VPL shadow maps) is ruled out by CPU cost, not GPU cost | verified (profile `00` §10.1) |
| TS3 renderer is forward, no G-buffer: no albedo, no world normals on screen | Any screen-space or deferred "apply indirect" pass lacks albedo. Indirect must be added either **in the game's own light inputs** (room light maps, rig-light constants) or in patched shaders | inferred (no G-buffer found in Apex; AO reconstructs normals from depth, profile `00` §9) |
| TS3 walls are thin (zero-thickness double-sided quads in Apex's proxy), axis-aligned or diagonal **on 1 m tile edges**; rooms are known per tile | Generic grid methods leak; but TS3 hands us the exact wall layout, so leaks can be stopped by construction (room-scoped gathers, wall masks on cell faces) rather than by heuristics | verified: `features/rt_lamp_proxy.h` header (walls on tile sides/diagonals, openings, ceilings, slabs) |
| VRAM is bridge-side (64-bit server); the 32-bit game keeps only proxy objects and CPU copies of **locked** resources | DEFAULT-pool render targets that are never locked cost almost no address space; anything uploaded by Lock (volume textures, managed textures) costs a client CPU copy until `freeIdleManagedCopiesSec` frees it | verified (bridge doc), per-object size inferred |

### What Apex already has (this worktree)

- `features/rt_lamp_gpu.*` (Lamp lighting > Instant): up to **16 lamps' cube shadow maps**, 6 faces of 256x256 in one
  G16R16F atlas (distance + moment), rendered from a lot **occluder proxy** (`rt_lamp_proxy.h`: walls with openings,
  ceilings/roofs, floor slabs from the lot story grid). **verified**
- It already **relights the game's room light maps on the GPU, texel by texel**: one point per light-map texel
  (`TexelVertex` at its texel), lamps accumulated into an A32B32G32R32F target, composed back into the game's map
  format. **verified** (`rt_lamp_gpu.h` comments). Floors run at 4 texels/m (`night-lighting/floors.md`); game room maps
  1/2/4 texels per tile, Apex "High" doubles them (`night-lighting/README.md`). **verified**
- So the "texel point + lamp shadow lookup + additive accumulation" machinery that any texel-space GI needs exists.
  This is the strongest single input to the recommendation.

## 1. Techniques

Cost basis: 3840x2160 = 8.3 M pixels; RTX 4070 Ti SUPER class (the profile machine), RTX 30 laptop about 2-2.5x
slower. Lot = up to 64x64 tiles x 4-5 stories. Numbers are **inferred** from paper timings scaled by hardware and by
resolution unless marked.

### 1.1 Reflective shadow maps (RSM), gathered on screen — Dachsbacher & Stamminger 2005 [2]

- **How:** each lamp's shadow map also stores world position (or depth), normal and reflected flux. Each shaded pixel
  gathers ~100-400 RSM texels as virtual point lights (VPLs), **without visibility** between VPL and receiver.
- **SM3 fit:** native; it was designed for SM3. But TS3 is forward, so the gather lives in patched lit shaders or a
  screen pass without albedo.
- **GPU at 4K:** 8.3 M px x 16 lamps x ~200 samples is 26 G taps: far too slow at full rate. The original used
  low-res gather + interpolation; at quarter res with 64 samples per lamp and 4 lamps per pixel, roughly 2-4 ms.
- **Memory:** per lamp cube 6 x 64^2 x (flux RGBA8 + normal RGBA8) = 196 KB; 16 lamps 3 MB (bridge VRAM).
- **Leaks:** **bad.** No VPL-to-receiver visibility: a lamp lighting one side of a thin wall puts VPLs on that wall,
  which light the next room through it. The paper accepts this; it is fatal for TS3 night interiors.
- **Stability:** good under camera moves (light-space sampling), flicker under lamp moves if the RSM is sparse.
- **Size:** small (RSM MRT + gather shader): a few hours of agent work for the RSM, much more for shader coverage.
- **Verdict:** use **RSM as the VPL source** (every other method here needs one), not as the gather.

### 1.2 Light Propagation Volumes (LPV) — Kaplanyan 2009, Kaplanyan & Dachsbacher 2010 [1][3]

- **How:** RSM texels (downsampled, "smart" clustered) are injected as L1 SH (4 coefficients, 2 bands) into a 32^3
  grid; 4-8 propagation iterations of a 6-neighbour stencil; the grid is sampled per pixel. **verified** [1]
- **DX9 path:** 3 MRT (R, G, B x 4 SH coefficients each); injection = one point draw per RSM texel, position read by
  vertex texture fetch (Xbox 360) or render-to-vertex-buffer (PS3); the volume is an unwrapped 2D texture on DX9 PC
  because DX9 cannot render into a volume. The paper's PC timings are "DirectX 9.0c API ... GTX 280". **verified** [1]
- **Paper cost:** console 720p, one 32^3 LPV, 128^2 RSM: RSM 0.5-0.8 ms, injection 0.2-0.4, propagation 0.7-0.8,
  **LPV rendering 1.5-2.0 ms**; GTX 280 Sponza: 0.72 ms for 1 cascade, 1.3 for 2. **verified** [1]
- **GPU at 4K on RTX 40:** injection + propagation are resolution-independent and tiny on modern GPUs (32^3 x 8
  iterations x 3 targets ~ 0.05 ms; 16 lamps x 64^2 points injected ~ 0.1 ms). The apply pass scales with pixels:
  6 bilinear fp16 taps per pixel (manual trilinear x 3 targets) at 8.3 M px ~ 0.5-1.0 ms. Total **~0.7-1.2 ms**,
  inferred. Only if the apply is per pixel: in TS3 it would need either shader patching or the light-map route below.
- **Memory:** 32^3 x 3 RT x fp16x4 = 786 KB per buffer, ping-pong + accumulate ~2.4 MB. Negligible.
- **Leaks:** **the known weakness.** Kaplanyan names "light bleeding through a thin double-sided object" and through a
  thin roof as the main artifact; fixes are half-cell injection shift along normal/light, and directional-derivative
  "anisotropic upsampling" (he rejects antiradiance as doubling RSM cost). "Shifting ... is not sufficient to
  completely avoid self-illumination and undesired radiance bleeding." **verified** [1] The I3D 2010 paper adds a
  geometry volume for fuzzy occlusion during propagation [3]: it reduces, does not remove, thin-wall leaks.
  **TS3 twist (inferred, new):** walls lie exactly on tile edges; with a grid aligned to the 1 m tile grid, a per-story
  **wall mask** (2 bits per tile edge + diagonals) can zero the flux across exactly the faces that hold walls, and the
  apply pass can refuse to interpolate across them. That makes LPV leak-tight against walls/floors, which no generic
  engine could do. Diagonal walls cut cells; treat a diagonal cell as split (two half-cells) or accept a small leak.
- **Stability:** paper notes "temporal flickering ... during camera movements, because we use camera-attached
  approach"; fixes are RSM texel snapping and one-cell grid snapping. **verified** [1] A **lot-attached** grid in TS3
  avoids the camera case entirely; lamp toggles change the result in one frame (no history).
- **Size:** medium: RSM MRT, injection VS/PS, propagation PS, apply (shader patch per lit family) = the patching is
  the big part.
- **Verdict:** viable and historically proven on DX9; for TS3 the propagation buys little (lots are small; one bounce
  gathered directly is affordable), and its apply step hits the "shader coverage" hard part of `idea.md`.

### 1.3 Irradiance volume with SH probes, updated incrementally — Greger et al. 1998 [4]; Tatarchuk 2005; DDGI [5][6]

- **How:** a grid of probes (L1 or L2 SH, or octahedral irradiance) over the lot; each probe is (re)computed by
  gathering from VPLs or rendering a tiny cube of the proxy; shading interpolates 8 probes. DDGI adds per-probe depth
  moments and a Chebyshev visibility weight to stop leaks [5].
- **SM3 fit:** good. Probe update = one pixel per probe-coefficient in a 2D atlas (gather pass), no scatter needed.
  Incremental: update the K probes nearest a changed lamp/object per frame.
- **GPU:** update of 64x64x5 probes (20 k) x 256 VPLs ~ 5 M evaluations: ~0.05-0.2 ms, spread over frames. Apply
  like LPV apply: ~0.5-1 ms at 4K per pixel, or near zero when baked into existing light inputs.
- **Memory:** 20 k probes x L1 RGB fp16 = 480 KB; with DDGI 8x8 depth octahedra ~2.6 MB.
- **Leaks:** plain probe grids leak through thin walls exactly like LPV (trilinear across the wall). DDGI's visibility
  test is "robust to light-leaking" but limited by probe depth resolution; follow-up work says it cannot "completely
  remove the light leaking effect, especially the leaking caused by thin objects" [7]. **paper-reported**. Same TS3
  twist applies: tile-aligned probes + wall mask, or a **room id per probe** and room-matched sampling (a shaded point
  takes only probes of its own room; the point's room from a per-story tile room-id map at world xz offset by half a
  tile along the normal, so wall pixels pick the room they face).
- **Stability:** very good: probes are world-fixed and change only when inputs change.
- **Size:** small-medium. The update is like the existing texel relight; apply has the shader-coverage problem.
- **Verdict:** the right carrier for **Sims and moving objects** (they cannot read room light maps; they use rig
  lights, `idea.md` "Hard parts"). Feed the rig-light ambient/SH constants from the probe at the Sim's position: a
  CPU-side read of one probe per Sim needs a GPU read-back (2 ms per `GetRenderTargetData` under the bridge, profile
  `00` §6), so compute those few values with the CPU reference (`rt_lamp_proxy.h` already has one) or read back
  asynchronously once per lamp change, never per frame.

### 1.4 Screen-space GI (SSGI) / screen-space directional occlusion (SSDO) — Ritschel et al. 2009 [10]

- **How:** sample depth/colour around each pixel; SSDO adds one bounce from nearby on-screen pixels on top of AO.
- **SM3 fit:** works (Apex AO is already a depth-based slice pass).
- **GPU at 4K:** AO is measured at 1.3-4.5 ms by quality on this GPU (**verified**, profile `00` §7); SSGI with colour
  taps and a denoise is at least the same: **~2-5 ms**. Half-res variants in Apex AO produced "micro dots", temporal
  smoothing produced "breathing and trails" (**verified**, profile `00` §9): the same failure modes would hit SSGI.
- **Memory:** a few 4K targets, ~100-200 MB VRAM bridge-side; trivial address space.
- **Leaks:** none through walls (only visible surfaces contribute) but **missing light**: off-screen lamps and
  surfaces behind the camera contribute nothing; indirect changes as the camera pans. Light also bleeds across
  depth discontinuities by the kernel radius.
- **Stability:** poor under camera moves without temporal accumulation, which Apex already had to remove for AO.
- **Albedo problem:** no G-buffer: the bounce colour would be taken from the lit scene colour, which double-counts
  the game's existing bake.
- **Verdict:** detail layer at most (Kaplanyan used it only for "micro-bleeding" below grid resolution [1]). Not a base.

### 1.5 Per-lot precomputed bake on change (texel-space gather / lightweight radiosity) — cf. Enlighten [11]

- **How:** on lamp/object change, recompute indirect light **in light-map texel space** for the active lot: each
  room-map texel (a point at its world position and normal) gathers from the lamps' VPLs. Spread over frames;
  between changes the result is just a texture the game already samples. This is the "lightmap radiosity" family that
  Enlighten ran on PS3/360-era hardware for Frostbite 2 [11].
- **SM3 fit:** best of all: it is exactly what `rt_lamp_gpu` does for **direct** light today (one point per texel,
  additive accumulation into fp32, compose into the game map format). Indirect = the same pass with a VPL loop
  instead of a lamp loop.
- **Rasterized vs path-traced bake:** SM3 cannot trace rays against the scene. A rasterized bake can: (a) VPL gather
  from RSMs without visibility but **room-scoped** (texel and VPL must share a room, or the VPL sits in an opening
  between the two rooms); (b) optional VPL visibility via a handful of small "imperfect shadow maps" [12] of the proxy
  for the brightest clustered VPLs (64 VPLs x 6 x 32^2 paraboloid/cube faces of a ~10 k-triangle proxy = 1 draw each
  if the proxy is one vertex buffer; 64 extra draws per change is fine, not per frame). A second bounce = run the
  gather again using the first-bounce texels as VPLs (radiosity-style).
- **GPU:** a lot of 64x64 m at 4 texels/m on floors, plus walls and ceilings across 4 stories: ~0.2-0.6 M texels.
  x 256 clustered VPLs (16 lamps x 16 each) = 50-150 M VPL evaluations ~ **1-3 ms total per full re-bake**, spread
  over 4-16 frames as **0.2-0.5 ms a frame while dirty, 0 when clean**. Inferred from ALU throughput; texel counts
  need confirming per lot.
- **Memory:** an fp32 accumulation target sized like the existing one (`rt_lamp_gpu` already holds it); RSM
  atlases 3 MB. VRAM bridge-side; DEFAULT-pool, never locked: near-zero address space. (verified pattern, size inferred)
- **Leaks:** **controllable by construction.** Room scoping removes wall leaks completely for the common case (lamp
  and receiver in different rooms, no opening). Through doors/windows: VPLs inside an opening's frustum pass, others
  do not. Floors/ceilings: per-story scoping. Residual: furniture self-occlusion (handled by Apex AO, as Kaplanyan
  multiplies SSAO onto LPV [1]).
- **Stability:** perfect under camera moves (world-space, no view dependence). Lamp toggle/edit: re-bake visible in
  1-16 frames; blend old->new over ~0.2 s to hide the step. Moving casters (Sims, doors) are not in the bake; their
  indirect contribution is small and they get AO.
- **Coverage:** **no new shader patching for static surfaces**: walls, floors, ceilings and furniture ("objects"
  class map) already read these maps. This removes the biggest "hard part" for indirect light.
- **Size:** smallest of the full-GI options here, because it extends existing code: RSM output on the existing cube
  atlas pass (add 2 MRTs), a downsample/cluster pass, a gather PS on the existing texel points, room/opening tables
  from the existing proxy.
- **Limits:** only what the light maps cover (the active lot, map resolution 0.25-1 m: fine for low-frequency
  indirect). Sims and moving objects need 1.3. The game's room-map semantics (one map set per room class, per story)
  constrain what can be stored; must follow how Apex's Instant mode composes them.

### 1.6 Voxel-based GI (voxel cone tracing, VXGI) — Crassin et al. 2011 [13]

Not impossible in SM3, but impractical, for these reasons:
- **Voxelization** needs a GS (dominant-axis projection, conservative raster) and UAV writes with atomics. SM3 has
  neither. Workaround: render the scene once per Z-slice with clip planes into a 2D slice atlas, i.e. 64-128 scene
  redraws per update. Each draw crosses the bridge (per-call cost already the bottleneck, profile `00` §10.1). Ruled
  out for game geometry.
- The TS3 proxy (walls/floors on a 1 m grid) could be voxelized **on the CPU** cheaply and uploaded with `LockBox`
  to a volume texture: possible, but then it is just a coarse occupancy grid, which the wall-mask variants of 1.2/1.3
  already exploit more cheaply.
- **Cone tracing** needs a mipped, filtered 3D radiance texture; a 2D-unwrapped atlas has no hardware 3D mips, so
  every cone step costs 2 bilinear taps + lerp + manual mip. At 4K with ~5 cones x 10-20 steps, **5-10 ms**: worse
  than every option above for a scene with mostly static, axis-aligned geometry.
- Thin walls (zero thickness) voxelize to nothing or to one-voxel shells that cone tracing leaks through at coarse mips
  (a classic VCT artifact) — the TS3-critical failure case again.

## 2. Comparison

| Technique | GPU 4K (RTX 40, inferred) | Per frame when static | Memory (bridge VRAM) | 32-bit address space | Thin-wall leaks in TS3 | Camera / lamp stability | New shader coverage needed | Implementation size |
|---|---|---|---|---|---|---|---|---|
| RSM screen gather | 2-4 ms | same | ~3 MB | ~0 | severe | good / fair | all lit families | small + patching |
| LPV (generic) | 0.7-1.2 ms | apply only, ~0.5-1 ms | ~3 MB | ~0 | bad (paper's own main artifact) | fair (camera-attached) / good | all lit families | medium + patching |
| LPV, tile-aligned + wall mask | same | same | +wall mask KB | ~0 | good (diagonals residual) | good / good | all lit families | medium + patching |
| SH irradiance volume (+room-matched sampling) | update 0.05-0.2 ms (spread) + apply 0.5-1 ms | apply only | 0.5-2.6 MB | ~0 | generic bad; room-matched good | very good | all lit families, or rig constants for Sims | small-medium |
| SSGI / SSDO | 2-5 ms | same | 100-200 MB | ~0 | none, but missing off-screen light | poor (needs temporal; Apex removed it for AO) | one screen pass, no albedo | medium |
| **Texel-space bake on change** | 1-3 ms per full re-bake, spread | **0** | ~3 MB + existing targets | ~0 | **room-scoped: none for walls**, openings explicit | **perfect / 1-16 frame step, blendable** | **none for static surfaces** | **small (extends `rt_lamp_gpu`)** |
| Voxel cone tracing | 5-10 ms + voxelization redraws | cone trace every frame | 2-10 MB | ~0 | bad at coarse mips | good | all lit families | large |

"32-bit address space ~0" means: DEFAULT-pool render targets never locked, so only the client proxy objects
(inferred: hundreds of bytes each) live in the game process. Any CPU upload (volume texture via `LockBox`, managed
textures) adds a client copy until freed after 2 s idle (bridge doc).

## 3. Recommendation for TS3

**Texel-space indirect bake on change (1.5), room-scoped, written into the room light maps that Apex's Instant mode
already relights on the GPU; plus a small room-matched SH probe grid (1.3) for Sims and moving objects. No LPV
propagation, no SSGI base, no voxels.**

Why it fits TS3 specifically: lots are small (one bounce gathered directly is cheap, so propagation buys nothing);
geometry is mostly static (bake on change, zero per-frame cost, which matters because the profile shows the frame is
CPU/bridge bound and AO already spends 1.3-4.5 ms of GPU); walls are thin but **known exactly per tile edge**, so
room scoping stops leaks by construction instead of by heuristics; night interiors are the target, and the game's
own room maps already carry interior light to every static lit shader, removing the shader-coverage problem for
indirect light.

Note on `idea.md`'s "throw away the room light maps": this recommendation keeps the maps as the **carrier** only. The
game's room solve (2.8 ms/frame, 60 ms hitches) still goes; Apex writes the maps instead, as Instant mode does today.
If the direct-light lane replaces the maps with per-pixel shadow-mapped lamps in patched shaders, the bake output
moves into a separate "indirect" map set read by those same patched shaders (one extra sampler) — the bake itself is
unchanged.

### Staged plan (estimates in this repo's unit; no game runs counted)

| Stage | What | Proof / gate | Estimate |
|---|---|---|---|
| S0 | Offline in `tools/rt_lamps_test` (no game): RSM MRTs (flux, normal) added to the cube atlas pass at 32^2-64^2 per face; CPU reference of a room-scoped one-bounce gather on a captured lot's `World.txt` | GPU vs CPU reference per texel within tolerance; leak test: lamp in room A, receiver texels in room B with no opening = exactly 0 | 2-4 h agent work |
| S1 | GPU gather pass on the existing texel points: VPL cluster (16 per lamp, 256 total), room/opening tables from the proxy, result added into the fp32 accumulation before compose. Dirty flag per lot/story, spread over N frames, 0.2 s crossfade | Offline harness images; per-frame GPU time while dirty <= 0.5 ms, 0 when clean; leak test as S0 | 3-6 h |
| S2 | In game, one lot, night interior (Apex Instant mode on): compare vs Instant without indirect on the camera pan harness (p50/p99/hitches at 40 m and 150 m) + screenshots of 3 rooms (lamp-lit room, adjacent room with closed wall, adjacent room via doorway) | No new hitches; adjacent closed room unchanged; doorway spill visible | 1-2 game runs |
| S3 | Openings + second bounce: VPLs through door/window frusta; optional 2nd bounce (first-bounce texels as VPLs, one more gather) | Doorway spill looks right; cost of 2nd bounce <= first | 2-4 h + 1 game run |
| S4 | Sims/moving objects: room-matched L1 SH probes per tile centre per story, filled by the same gather; rig-light ambient fed from the probe at the Sim's tile (CPU reference value or async read-back once per change, never per frame) | Sim in a lamp-lit room vs dark neighbour matches surroundings; no `GetRenderTargetData` per frame | 4-8 h + 1-2 game runs |
| S5 (optional) | Visibility for the brightest VPLs via small imperfect shadow maps of the proxy (furniture/partial-wall occlusion); SSDO-style short-range detail only if S2 screenshots show missing contact bounce | Visual A/B; per-change cost only | 4-8 h |
| S6 | Style pass with the direct-light lane: indirect gain, colour saturation cap, minimum ambient so night rooms keep the TS3 look | Rodney / Apex owners sign-off | 1-2 h + screenshots |

Dependencies on other lanes: direct-light lane (shadow atlas and lamp list are shared; keep one RSM render per lamp,
not two); shader-coverage lane only if the maps are dropped as the carrier.

### Open questions to check before S1 (inferred, not verified)

1. Exact texel counts per room-map class on a big lot (sets the per-re-bake cost).
2. How the "objects" class map lights furniture (2D per story, or per object?): decides whether furniture gets the
   bake or needs probes.
3. Whether the room id per tile is readable from the lot story grid Apex already gathers (`LevelLightShare`), or must
   be derived from the wall graph (flood fill per story).
4. ps_3_0 static instruction-slot limit (fxc) for a 256-VPL loop: use a dynamic loop or split the gather into
   several additive passes.

## References

1. A. Kaplanyan, "Light Propagation Volumes in CryEngine 3", SIGGRAPH 2009 Advances in Real-Time Rendering course
   (read: §5.2-7, DX9 limits p.17 and p.33, console paths p.27-28, timings p.28-29).
   https://advances.realtimerendering.com/s2009/Light_Propagation_Volumes.pdf
2. C. Dachsbacher, M. Stamminger, "Reflective Shadow Maps", I3D 2005, pp. 203-213. https://doi.org/10.1145/1053427.1053460
3. A. Kaplanyan, C. Dachsbacher, "Cascaded Light Propagation Volumes for Real-Time Indirect Illumination", I3D 2010
   (geometry volume / fuzzy occlusion). Mentioned on https://www.realtimerendering.com/blog/ ; ACM DL I3D 2010 proceedings.
4. G. Greger, P. Shirley, P. Hubbard, D. Greenberg, "The Irradiance Volume", IEEE CG&A 18(2), 1998.
   https://doi.org/10.1109/38.656788
5. Z. Majercik, J.-P. Guertin, D. Nowrouzezahrai, M. McGuire, "Dynamic Diffuse Global Illumination with Ray-Traced
   Irradiance Fields", JCGT 8(2), 2019.
   https://research.nvidia.com/publication/2019-05_dynamic-diffuse-global-illumination-ray-traced-irradiance-fields
   (PDF: https://cim.mcgill.ca/~derek/files/DDGI-lowres.pdf)
6. M. McGuire et al., "Real-Time Global Illumination using Precomputed Light Field Probes", I3D 2017.
   https://research.nvidia.com/sites/default/files/pubs/2017-02_Real-Time-Global-Illumination/light-field-probes-final.pdf
7. "Signed Distance Fields Dynamic Diffuse Global Illumination" (thin-object leaking limit of DDGI).
   https://arxiv.org/pdf/2007.14394
8. Microsoft, Volume Texture Resources (Direct3D 9).
   https://learn.microsoft.com/en-us/windows/win32/direct3d9/volume-texture-resources
9. NVIDIA bridge-remix client sources (`d3d9_volumetexture.cpp`, `d3d9_volume.cpp`).
   https://github.com/NVIDIAGameWorks/bridge-remix/tree/main/src/client
10. T. Ritschel, T. Grosch, H.-P. Seidel, "Approximating Dynamic Global Illumination in Image Space" (SSDO), I3D 2009.
    https://doi.org/10.1145/1507149.1507161
11. S. Martin, P. Einarsson, "A Real-Time Radiosity Architecture for Video Games", SIGGRAPH 2010 Advances course.
    https://advances.realtimerendering.com/s2010/ ;
    https://www.advances.realtimerendering.com/s2010/Martin-Einarsson-RadiosityArchitecture(SIGGRAPH%202010%20Advanced%20RealTime%20Rendering%20Course).pdf
12. T. Ritschel et al., "Imperfect Shadow Maps for Efficient Computation of Indirect Illumination", SIGGRAPH Asia 2008.
    https://resources.mpi-inf.mpg.de/ImperfectShadowMaps/ISM.pdf
13. C. Crassin, F. Neyret, M. Sainz, S. Green, E. Eisemann, "Interactive Indirect Illumination Using Voxel Cone
    Tracing", Pacific Graphics 2011. https://research.nvidia.com/publication/2011-09_interactive-indirect-illumination-using-voxel-cone-tracing
14. G. Papaioannou, "Real-Time Diffuse Global Illumination Using Radiance Hints", HPG 2011 (grid radiance caching from
    RSMs without propagation; same leak profile as 1.3). https://diglib.eg.org/items/d76ad5a8-4d26-4603-9a15-902d25e99e35

Links read in full: [1]. Links confirmed by search only: [5][6][7][8][11][12][14], [9] via the GitHub tree. DOIs
[2][4][10] and URL [13] are from memory and unchecked: verify before citing in the Apex PR.
