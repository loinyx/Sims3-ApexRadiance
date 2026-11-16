# RTX Remix and prior art for real-time lamp lighting in a D3D9 game

Research lane R7 for [the real-time GI idea](../idea.md): what the RTX Remix bridge and runtime can and cannot do for
The Sims 3, and what other D3D9-era injection mods did to add dynamic lights, shadows and GI. Web and source reading
only, 2026-11-17; nothing here was run in the game.

Tags: **verified** = read in the source or official docs (file and line given), **reported** = a community page says
so, **inferred** = our reading, not checked.

## 1. The RTX Remix bridge (what Apex already ships)

Apex's bridge is NVIDIA's bridge built against stock DXVK, not the path tracer
([`docs/features/rtx-remix-bridge.md`](../../rtx-remix-bridge.md)). The bridge code now lives in
[`dxvk-remix/bridge`](https://github.com/NVIDIAGameWorks/dxvk-remix/tree/main/bridge) (the old
[`bridge-remix`](https://github.com/NVIDIAGameWorks/bridge-remix) repo is deprecated). Read from `main`, not from
Apex's pinned commit `f35b86a`; the macros have been stable but check the pin before relying on a detail.

### How it works (verified, bridge README)

- A 32-bit `d3d9.dll` client takes every D3D9 call; `NvRemixBridge.exe` (64-bit) replays it on a real D3D9 device
  (system d3d9, DXVK, or the Remix runtime if `.trex/d3d9.dll` exists) and presents straight into the game's window.
- Transport: two `IpcChannel`s (commands, responses), each a command queue plus a data queue in shared memory. Sizes
  come from `bridge.conf` (`clientChannelMemSize`, `serverChannelMemSize`, ...).

### Which calls wait for the server (verified, `bridge/src/util/util_bridgecommand.h`, `client/d3d9_device.cpp`)

| Kind | Default behaviour | Option |
|---|---|---|
| Draws, `Set*` state, `SetRenderTarget`, `Clear`, `StretchRect`, `ColorFill`, `UpdateSurface/Texture`, `Begin/EndScene` | Fire and forget (`WAIT_FOR_OPTIONAL_SERVER_RESPONSE` only waits when `sendAllServerResponses=true`, default false) | `sendAllServerResponses` |
| `Create*` (textures, volume textures, cube textures, VB/IB, RTs, depth, state blocks) | Waits (`sendCreateFunctionServerResponses=true` by default) | same |
| `GetRenderTargetData`, `GetFrontBufferData` | Always waits: `copyServerSurfaceRawData` pulls the bytes back | none |
| `GetDeviceCaps`, `GetDisplayMode`, `GetAvailableTextureMem`, `ShowCursor` | Always waits (`WAIT_FOR_SERVER_RESPONSE`) | none |
| `LockRect` on non-back-buffer surfaces | Client-side only: a CPU copy (`m_shadow` or shared heap) is allocated on first lock, sent on `UnlockRect` (`client/d3d9_surface.cpp:215-233`) | `useSharedHeap` |
| Queries | Async by default | `client.asyncQueries=true` |
| Present | Paced by a semaphore, `presentSemaphoreMaxFrames=1` | `presentSemaphoreEnabled` |

Other defaults: `commandBatchingEnabled=false`, `client.batchState=true`, `client.dropRedundantState=true`,
`ipcSpinMicroseconds=100`, `disableTimeouts=true`.

**What this means for GI (inferred from the table):**

- Shadow-map and probe-grid passes are plain draws into render targets: async, so their cost on the game thread is the
  client-side serialisation per call, not GPU time. Keep the call count low (cache static casters, instancing is not in
  SM3/D3D9 without `SetStreamSourceFreq`, which the bridge forwards like any other state call).
- Never read GPU results back on the game thread. Apex measured the CAS compositor's 256x256 `GetRenderTargetData` at
  about 1.9 ms a frame. Any "which lamps are visible" test must stay on the GPU (occlusion queries are async, but their
  `GetData` still crosses the bridge; budget them).
- Create every GI resource once at load (creates are synchronous) as `D3DPOOL_DEFAULT` render targets that are never
  locked. A never-locked surface gets no client-side CPU copy (the copy is made on first `LockRect`), so the shadow
  maps and volume textures cost address space only in the 64-bit server. This is the main reason the GI idea fits the
  32-bit limit at all.
- `CreateVolumeTexture` is forwarded (`d3d9_device.cpp:651`), so SM3 volume textures for an irradiance grid work
  across the bridge.

### The bridge exposes the Remix API to the 32-bit game (verified, `client/remix_api.cpp`)

With `exposeRemixApi=true` (default false) the client implements `remixapi_CreateLight`, `DrawLightInstance`,
`CreateMesh`, `DrawInstance`, `CreateMaterial` and `SetConfigVariable` over the IPC channel. A 32-bit ASI can therefore
push lights and meshes into the 64-bit Remix runtime. This only matters if the server loads the Remix runtime instead
of stock DXVK.

## 2. The RTX Remix runtime

Docs: [Game compatibility](https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/latest/docs/introduction/intro-compatibility.html),
[Game setup tab](https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/1.7.0-dev/docs/runtimeinterface/remix-runtimeinterface-gamesetup.html),
[RtxOptions.md](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/RtxOptions.md) (all `rtx.conf` options),
[FAQ: why shaders are hard](https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/1.4.0-0/docs/remix-faq.html).
License: runtime `dxvk-remix` Zlib (DXVK base), `rtx-remix` MIT.

### Shader-based games (verified, docs)

- Remix rebuilds the scene from fixed-function data (FVF/declarations, `SetTransform`, `SetLight`, `SetMaterial`,
  texture stages). Shader pipelines "make it very difficult ... leading to incompatibility"; in mixed games only the
  fixed-function parts are enhanced.
- `rtx.useVertexCapture`: injects code into the game's vertex shader to capture post-transform positions. Docs: "useful
  for games using simple vertex shaders, that still also set the fixed function transform matrices". Default is
  `True` in current `RtxOptions.md`, shown unchecked in the 1.7.0-dev UI page. Related: `useVertexCapturedNormals`,
  `useVertexCapturedTexcoords`.
- Pixel shaders are not run for the path-traced image; materials come from the bound albedo texture
  (`rtx.legacyMaterial.*`) or USD replacements. `terrainBaker` replacement support in pixel shaders is limited to fixed
  function and SM1.0.
- Game lights are translated only from D3D9 `SetLight` (`rtx.lightConversion*`, `rtx.suppressLightKeeping`).
  `rtx.lightConverter` spawns sphere lights at draws tagged by texture hash. `fallbackLightMode` is a debug light.

**For TS3 (inferred):** TS3 lights through shader constants and room light maps, not `SetLight`, and does not set
fixed-function transforms, so neither light translation nor vertex capture applies as-is. Sims are GPU-skinned in the
vertex shader.

### "Hybrid" (RT lights over the game's own raster shading)

Not a Remix mode (verified by absence in `RtxOptions.md`): `rtx.enableRaytracing=false` gives plain DXVK raster, true
gives the path-traced frame with the game's pixel shaders discarded, UI and sky handled separately. There is no option
to keep the game's raster shading and add path-traced lamp light on top. A hybrid would have to be our own: read back
nothing, but e.g. use Remix's raw AOV outputs (`rtx.enableRawAovOutputs`) in our own composite, which would need a fork
of the runtime (inferred).

## 3. The Sims 3 and Remix: existing attempts

- **ModDB RTX compatibility entry** ([the-sims-3](https://www.moddb.com/rtx/view/the-sims-3), 403 to scripts; content
  via search snippets): "experimental wrapper prototypes exist, but no publicly available configuration currently
  produces a playable result"; "rendering is severely broken"; "gameplay rendering does not utilize a traditional
  fixed-function pipeline". **Reported.**
- **The Sims 2** ([the-sims-2](https://www.moddb.com/rtx/view/the-sims-2)): partially playable, depends on an unreleased
  compatibility layer by its author; Patreon blog only. **Reported.**
- **`coderwg/dxvk-remix-sims3`**: a search result described it as "an unofficial fork of NVIDIA's RTX Remix bridge with
  a hook that lets RTX Remix ray-trace The Sims 3". The repo returns 404 on 2026-11-17 (deleted or private). No code
  could be read. **Reported, unverifiable.**
- [Nexus news: RTX Remix Q&A](https://www.nexusmods.com/thesims3/news/14758) is generic.

Nothing public path-traces TS3 today. Everyone hit the same wall: shader-only draws, no fixed-function transforms.

## 4. How shader-based D3D9 games were made Remix-compatible

| Project | What they did | License |
|---|---|---|
| [xoxor4d/gta4-rtx](https://github.com/xoxor4d/gta4-rtx) (GTA IV, shader-only D3D9) | ASI hooks the engine; "99% of meshes rendered via the RemixApi" (`CreateMesh`/`DrawInstance`) with a full fixed-function fallback; all game lights incl. sun translated to Remix lights with per-light overrides; modified vertex shaders so Remix captures normals; custom Remix runtime branch; anti-culling cascades. Bridge CPU cost was the bottleneck until meshes moved to the API; ships affinity scripts to split cores between game and Remix | Custom, all rights reserved: approach only |
| [xoxor4d/l4d2-rtx](https://github.com/xoxor4d/l4d2-rtx), p2-rtx, t4-rtx (Source, CoD WaW) | "By manually reimplementing fixed function rendering": per engine draw path, set FF transforms/textures and draw without shaders | l4d2-rtx MIT |
| [xoxor4d/remix-comp-base](https://github.com/xoxor4d/remix-comp-base) | Starter: every D3D9 function detoured, draw-call modification helpers, ImGui | MIT |
| Half-Life 2 RTX ([wiki](https://developer.valvesoftware.com/wiki/Half-Life_2_RTX)) | Modified Source 2013 engine (source access), all VRAD baked lights replaced with real-time path-traced lights | n/a |

**For TS3 (inferred):** a Remix path would mean, per TS3 shader family (walls, floors, objects, skinned Sims, terrain,
water, foliage), re-issuing each draw as fixed-function or Remix API meshes with CPU-side skinning or captured bones,
plus a lamp-to-`remixapi_CreateLight` bridge from Apex's lamp tracking. GTA IV shows it is doable for one game by one
experienced author over years, and that the 32-bit bridge's per-call cost becomes the limit. It also throws away TS3's
look (pixel shaders) and Apex's existing work, which the idea explicitly wants to keep.

## 5. Prior art: dynamic lights, shadows and GI injected into D3D9-era games

| Project | Technique | D3D9 tricks relevant to us | License / reuse |
|---|---|---|---|
| **MGE XE** (Morrowind) [repo](https://github.com/Hrnchamd/MGE-XE) | Sun shadows: 2 cascades, exponential shadow maps (ESM) with a blur; distant land renderer | Records the game's draws per frame (`recordMW`: VB, IB, stride, FVF, alpha test, blend, bone palette) and **replays them** into the shadow map and again as an extra shadow-receive pass (`src/mge/rendershadow.cpp`, verified). Additive blends skipped; alpha-tested casters keep their texture and `alphaRef`; cull-none casters drawn CW to avoid self-shadow on two-sided polys | GPL-2.0: study only, no copying into MIT Apex |
| **TES Reloaded / Oblivion & New Vegas Reloaded** [repo](https://github.com/mchaptel/TESReloaded10) | Exterior sun shadows (post-process and forward), interior shadows for up to 4 point lights; changelog mentions static maps reused when possible | Engine hooks via the script extenders; shaders replaced per game shader | GPL with extra binary-redistribution terms: study only |
| **GTA RenderHook** [repo](https://github.com/petrgeorgievsky/gtaRenderHook) | Replaces GTA 3/VC/SA renderer with D3D11/Vulkan: deferred lighting, shadows, later RT | Full renderer replacement through RenderWare hooks, not D3D9 capture | MIT: code reusable with notice (different API, so ideas more than code) |
| **ENBSeries** (Skyrim LE, Oblivion, GTA IV, FNV) | SSAO/SSIL, sun shadow tweaks, particle and point-light additions | Closed; uses engine light data where available | Proprietary: nothing reusable |
| **ReShade** [repo](https://github.com/crosire/reshade) + add-on API | Post effects with depth; DX9 depth selected by heuristics (copy before clears, reversed toggle). TS3 users report depth access trouble ([forum](https://www.reshade.me/forum/shader-troubleshooting/6965-no-access-to-depth-buffer-sims-3)) | Depth-buffer pick heuristics, add-on event hooks | BSD-3-Clause: reusable with notice |
| **RTGI / iMMERSE MXAO** (P. Gilcher) [repo](https://github.com/martymcmodding/iMMERSE) | Screen-space ray-marched GI (RTGI, paid), MXAO = GTAO + visibility-bitmask SSIL; Launchpad pass makes normals and motion vectors from depth | Normals from depth, temporal reprojection, all in pixel shaders | All rights reserved: technique only (papers below are public) |
| **Skyrim Community Shaders** [repo](https://github.com/community-shaders/skyrim-community-shaders) | Clustered light lists, many shadowed lights, SSGI | D3D11 (SE/AE), not D3D9 | GPL-3.0: study only |
| **GTA IV FusionFix** [repo](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix) | Shader fixes incl. shadow quality, cascades | Replaces game shaders by hash at load | GPL-3.0: study only |
| **SpecialK**, **dgVoodoo2** | Frame pacing, texture management / API wrapping | No lighting work | GPL-3.0 / closed |
| **TS3 ReShade presets** ([ExtraReal](https://modthesims.info/d/600416), [All-Round Alright](https://modthesims.info/d/584854)) | Screen-space AO, DoF | Users report AO bleeding through UI, cost | Presets only |

### Techniques and papers (public, free to implement)

- Reflective shadow maps: Dachsbacher & Stamminger, I3D 2005 (depth + normal + flux from the light).
- Light propagation volumes: Kaplanyan, [SIGGRAPH 2009 CryEngine 3](https://advances.realtimerendering.com/s2009/Light_Propagation_Volumes.pdf);
  cascaded LPV with occlusion, Kaplanyan & Dachsbacher, I3D 2010 ([slides](https://cgg.mff.cuni.cz/~jaroslav/gicourse2010/giai2010-07-anton_kaplanyan-slides.pdf)).
  LPV injection and propagation use render-to-volume-slice passes; on SM3 (no geometry shader, no scatter) injection
  is done with point sprites or vertex-texture fetch into a 2D-unwrapped volume (inferred; the 2009 talk targeted
  consoles of that generation, the DX9 detail is unverified). Crytek's LPV is covered by
  [US 2011/0012901](https://uspto.report/patent/app/20110012901): check its status before shipping a straight LPV.
- GTAO: Jimenez et al. 2016; visibility-bitmask SSIL: Therrien et al. 2023. Both pixel-shader friendly.
- Exponential shadow maps (MGE XE uses them): filterable, cheap soft edges on SM3; an alternative to PCSS.

## 6. What we can leverage, ranked by value

1. **Bridge rules for GI resources (verified, free).** All GI passes are async draws; create targets once as
   never-locked `D3DPOOL_DEFAULT` RTs and volume textures so they live only in the 64-bit server; no
   `GetRenderTargetData` or blocking `GetData` on the game thread. This decides whether the idea fits the 32-bit budget
   and the per-call cost that is today's limit.
2. **MGE XE's record-and-replay pattern** (study, GPL-2: re-implement, don't copy). Apex already sees every draw:
   record caster draws per lamp (VB/IB/decl/VS constants incl. bone palette for Sims, alpha-test texture), replay into
   cached per-lamp shadow maps, skip additive blends, force a cull mode for two-sided casters, ESM-style filtering for
   soft edges. Closest existing proof that "shadow maps from captured draws" works in a D3D9 injector.
3. **GTA IV RTX's game-light translation and draw-call split** (approach only). Shows reading lamp data in the ASI and
   driving an external light system per light, with per-light overrides; and that per-call bridge cost, not the GPU,
   is the bottleneck. Supports the idea's plan to key everything off Apex's existing lamp tracking.
4. **Public GI papers** (RSM, LPV, cascaded LPV, GTAO, visibility-bitmask SSIL, ESM): implementable in SM3 pixel
   shaders. Check the LPV patent first; an irradiance-probe grid filled from RSM samples avoids the patented
   propagation step if needed.
5. **ReShade (BSD-3) depth heuristics and normal-from-depth code** for the screen-space GI layer, if Apex's own depth
   access needs help. Reusable with notice.
6. **`remix-comp-base` / `l4d2-rtx` (MIT)** as a reference for a full Remix route: per-draw fixed-function re-issue.
   Low value now: costs every TS3 shader family, discards TS3's pixel shaders and Apex's work, and no public TS3
   attempt got past broken rendering.
7. **Bridge Remix API passthrough (`exposeRemixApi`)**: lets a 32-bit ASI create Remix lights and meshes. Only useful
   if Apex ever loads the Remix runtime in the server; keep as a long-term option, not for the first proof.

Not leverageable: ENB, RTGI/iMMERSE, gta4-rtx code (closed or all rights reserved); GPL-3 projects (Community Shaders,
FusionFix, SpecialK) and GPL TES Reloaded for code.
