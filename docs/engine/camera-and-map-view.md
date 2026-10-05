# Camera, projection and map view

This page documents how `TS3W.exe` exposes the camera: the map view flag read through the script binding `Camera_IsMapViewModeEnabled`, the camera position sources used for motion detection, and the camera projection as it reaches the shaders, including how scene depth is turned back into distance. Depth Blur, Ambient Occlusion, Edge Smoothing, Reflections, the camera-aware Performance switches and the developer tools depend on it.

## Scope

| | |
|---|---|
| Game build | Steam 1.67.2 (`TS3W.exe`, image base 0x00400000) unless stated |
| Used by | [Depth Blur](../features/depth-blur.md), [Ambient Occlusion](../features/ambient-occlusion.md), [Edge Smoothing](../features/edge-smoothing.md), [Reflections](../features/reflections.md), [Performance](../features/performance/README.md) (camera ids), [Frame Profiler](../features/frame-profiler.md), Lot Map Probe |
| Evidence | Disassembly (`engine_map` `full.asm`, `dwords.txt`, `strings.tsv`), Light Probe captures (LightProbe-m70..m80), `features/map_view.cpp`, `features/post_scene.cpp` |

## Overview

All addresses are `TS3W.exe` 1.67.2 Steam, image base 0x00400000.

Consumers in Apex Radiance:

- **Depth Blur**: map view fade; INTZ depth, with A = 1.00008 in its Auto focus.
- **Ambient Occlusion**: the post-scene camera vote (`PostScene::WantCamera`), the near plane and depth constant A, and
  the map view flag for its own map view radii ("Also in map view", TOML `noMapa`, default on).
- **Edge Smoothing**: "Edges from depth" reads `PostScene::CameraNear()` and `CameraDepthA()` (fallback near 0.25) to
  linearise depth; it does not request the camera vote itself.
- **Reflections**: water depth linearisation from the water shader's own projection.
- **Performance**: Lot Lighting While Moving, Wall Shading While Moving and Spread New Objects Over Frames read the camera
  eye through the camera ids (`CameraRootCall`, `CameraGetterCall`, `CameraRootGetter`, `CameraGetter`;
  [game-versions.md](game-versions.md#features-and-the-addresses-they-need)).
- **Frame Profiler** (developer mode): camera motion from the lot LOD scoring call.
- **Lot Map Probe** (developer mode).

The combined build's Script GC Scheduler also read the camera motion sources below; it is removed
([../removed-features.md](../removed-features.md#script-gc-scheduler)). S3SS's own Lot Streaming Optimizations (not
Apex) calls the same map view getter.

## Details

### Camera_IsMapViewModeEnabled (0x0073E060)

```
0073E060  call 0096B390          ; eax = [0x011E9400]
          test eax,eax / je 0073E098
          mov ecx,eax / call 00F20C20   ; eax = [ecx+0x14]
          test / je
          mov ecx,eax / call 0096B6D0   ; eax = [ecx+0x8C]
          test / je
          mov edx,[eax] / mov ecx,eax / mov eax,[edx+0Ch]
          push 110FDD89h / call eax     ; cast to the camera interface
          test / je
          mov al, byte ptr [eax+8B9h]   ; 8A 80 B9 08 00 00
          ret
0073E098  xor al,al / ret               ; 32 C0 C3
```
It only reads, so calling it from the render thread is safe (map_view.cpp comment). Map view = the view opened with M or
by zooming all the way out (UI hint in Depth Blur).

#### How Apex finds it (map_view.cpp, `Resolve`, once via `std::call_once`)

1. Find `.text`, `.rdata`, `.data` of the exe from the PE section headers.
2. Find the NUL-terminated name "ScriptCore.CameraController::Camera_IsMapViewModeEnabled" in `.rdata` (byte scan).
3. Find the 4-byte name VA in `.data` (4-aligned scan; fallback `.rdata`): the binding table entry is `{function, name}`,
   so the function pointer is the dword before it.
4. Validate: inside `.text` with 0x60 bytes of room, first byte `E8`, and within the first 0x50 bytes the cast push
   `68 89 DD 0F 11` followed later by `8A 80 xx xx xx xx C3 32 C0 C3` (`LooksLikeGetter`). The displacement (0x8B9) is
   not checked, so a version with another offset would still pass.
5. `MapView::IsOpen()` calls it under SEH (`CallGuarded`); a fault logs `[MapView] Call faulted, map view detection off`
   and disables it for the session.

Log lines: `[MapView] Camera_IsMapViewModeEnabled at 0x0073e060`, or one of `Game sections not found`,
`Script binding name not found`, `Script binding table entry not found`,
`Unexpected function at 0x..., map view detection off`.

S3SS's LSO resolves the same function differently: Steam address `0x0073E060` plus the pattern
`E8 ?? ?? ?? ?? 85 C0 74 2F 8B C8 E8 ?? ?? ?? ?? 85 C0 74 24 8B C8 E8 ?? ?? ?? ?? 85 C0 74 19 8B 10 8B C8 8B 42 0C 68 89 DD 0F 11 FF D0 85 C0 74 ?? 8A 80 B9 08 00 00`.
Its **map view blocker** detours WorldManager::Update (`0x00C6D570`) and, while map view is on and for 1000 ms after it
closes (`kMapViewGraceMs`), sets WorldManager+0x258 = 1 around the original call (restoring the old value after).
Apex only calls the getter; PLANO-SEPARACAO.md section 3 lists it as "cooperate (read or call only)". Consequence for
Apex: while that blocker is active, 0x00C6C290 is not called, so the Frame Profiler's camera point (and the removed GC
Scheduler's) stops updating; both have / had a second source (below).

#### Users

| Feature | Use |
|---|---|
| Depth Blur (`StepMapFade`) | `offInMapView` (default on): blur amount x (1 - fade), fade eased over 0.3 s (`kMapFadeSeconds`); no GPU work when fully faded; the frame the map closes, the Auto focus snaps to the new view |
| Ambient Occlusion | `inMapView` (TOML `noMapa`, default on): while the map view is open the shade uses its map radii and fade instead of fading out at the `distance` setting |
| LSO (S3SS) | map view blocker (above) |

### Camera position / motion sources

| Source | Read by | Notes |
|---|---|---|
| Camera point, arg 2 of `0x00C6C290` (copied to WorldManager+0x3A0) | Frame Profiler (`Hook_LotLodScoring`, render thread, moved if any component changes by > 1e-3); GC Scheduler (combined build only, removed: read WorldManager+0x3A0 from `[0x011ECBC4]`, SEH-guarded, every 50 ms) | Not updated while WorldManager+0x258 is set (map view blocker, loading) |
| Camera eye `[[0x011D1860]+0x24]+0x60` | Performance: Lot Lighting While Moving, Wall Shading While Moving and Spread New Objects Over Frames (camera ids parsed from the `0x00C6D5BD` call sequence); GC Scheduler (combined build only, removed; its second source) | On Steam the resolved chain must equal `[0x011D1860]+0x24+0x60` or it is not used |
| View-projection `c40..c43` of the scene draws | Ambient Occlusion (`PostScene::CameraViewProj`, through the camera vote below); combined build only: Frame Profiler fallback (element change > 1e-5 relative) | Apex Radiance's frame_profiler.cpp does not use the vote and counts frames without the lot LOD call as "unknown" |

GC Scheduler (removed): moving when either point exceeded `cameraSpeedThreshold` (default 0.25 m/s); see
[../removed-features.md](../removed-features.md) and [mono-gc.md](mono-gc.md). Frame Profiler: see
[../features/frame-profiler.md](../features/frame-profiler.md).

### Projection as the shaders see it

#### Where it is

Every scene draw carries the camera in several vertex-constant blocks (LightProbe-m80, the first 9-digit capture):

| VS constants | Content |
|---|---|
| `c40..c43` | camera **view-projection** (world -> clip), identical in all scene draws; its translation encodes the camera position; a mirrored copy is used in the water reflection pass (notes 28/09 "acumulo temporal + projecao exata") |
| `c0`, `c4`, `c180`, `c192`, `c216` | blocks that also give near 0.250 in every draw of the frame (c4 is in the code's list and the notes; the post_scene.cpp comment lists c0/c40/c180/c192/c216) |
| `c132`, `c171` | other projections (not the camera) |
| water VS `c4..c7` | world-view-projection of the water shader (used by the water pass, below) |

Different shader families read the camera from different blocks (c0, c4, c40, c180...), found by Lot Map Probe
(notes 27/09 "AO novo").

#### Shape and measured values

A 4-row block is a camera projection when `row2 = A * row3 + (0, 0, 0, B)` and rows 0 and 1 are orthogonal to row 3.
Then:
- `near = -B`
- device depth `d = A - near * A / z` (z = view distance along the camera axis, metres)
- `z = near * A / (A - d)`, `1/z = (A - d) / (near * A)`
- `tan(fovX/2) = 1 / |row0.xyz|`, `tan(fovY/2) = 1 / |row1.xyz|`

| Quantity | Value | Source |
|---|---|---|
| A | 1.00008 | LightProbe-m80 (notes 28/09). The earlier model A = 1 ("infinite far plane", 26/09, m70..m79 with 3-digit captures) was a rounding artefact. |
| far plane | about 3 km: `far = near * A / (A - 1)` = 3125 m at near 0.25 (computed) | post_scene.cpp comment "far plane near 3 km" |
| near | 0.2 .. 0.3, changes with zoom and camera height, same value in every block of a frame | m70..m80; "near 1.0 on the street" and "10 in c40" in the 26/09 notes were 3-digit rounding (w ~ 243), corrected 27/09 |
| y scale Py | 4.293 (`tan(fovY/2)` = 0.2330, fovY about 26.2 deg computed) | notes 26/09, `kTanHalfFovY = 1/4.293` in AO |
| x scale Px | 2.415 = Py / (16/9) at 16:9; exact per-frame tanX / tanY 0.41421 / 0.23300 | notes 28/09 (previously measured 0.41411 / 0.23294) |

`tanX = 0.41421 = tan(22.5 deg)`, i.e. a 45 deg horizontal field of view at 16:9 (computed; whether the game fixes the
horizontal or the vertical FOV across aspect ratios is *unverified*: the 26/09 note says the vertical one is fixed, but only
16:9 was measured).

Depth precision (lab, notes 27/09): on the road at z/near about 100 - 120, d changes about 1.05e-5 per pixel; the float
ULP near 1 is 6e-8, so reconstructed depth carries about +/-0.8% noise; far away with the camera looking down, the
ground changes by a few 2^-24 steps per pixel and turns into stairs (hypothesis for the lines of the first SSAO).

#### Camera vote (`post_scene.cpp`, back since 30/09 for Ambient Occlusion)

Restored from the combined build (tag `combined-final`) as `VoteCamera`; it runs only while an effect asked for it with
`PostScene::WantCamera(true)` (reference counted; today only Ambient Occlusion). Depth Blur's Auto focus does not need
it (depth ratios, the near plane cancels):
- For each of the first `kNearDraws` = 24 scene draws of a frame (backbuffer RT0, ZENABLE on, not an internal pass),
  `VoteNear` reads the VS constants of blocks `{0, 4, 40, 180, 192, 216}` with `GetVertexShaderConstantF` and runs
  `NearFromBlock`: `A = (row2.row3)/|row3|^2` must be in [0.999, 1.001]; the residual of `row2 - A*row3` must be tiny;
  rows 0/1 orthogonal to row 3 (1e-3 relative); `B = row2.w - A*row3.w` in [-5, -0.01]. Valid blocks vote for
  `near = -B` (values within 1e-3 relative are the same vote). The shape test is what stopped a block of zeros from
  winning (Lot Map Probe's second capture, 27/09).
- `c40..c43` is also collected as a view-projection candidate (exact `memcmp` groups, at most 8 distinct); the most voted
  one whose own near matches the frame's near (1e-3 relative) becomes `CameraViewProj`.
- Present (`Priority::First`) clears the votes and `g_vpValid`. `CameraNear()` keeps the last winner across frames (0
  until the first vote ever); `CameraViewProj()` is false until this frame's first vote; `CameraDepthA()` = A of the
  current view-projection (double precision), 1.00008 when unknown (the combined build returned 1). The Frame Profiler's own Present hook runs at priority
  -1000, before this reset.

### How scene depth is linearised, per consumer

All read the INTZ copy of the game's depth ([../features/depth-blur.md](../features/depth-blur.md), `DepthShare`),
point-sampled; all treat `d >= 0.99999` as sky.

| Consumer | Formula | Notes |
|---|---|---|
| Depth Blur, Auto focus (default) | `r = (A - d) / (A - d_f) = z_f / z` with A = 1.00008 and `d_f` the eased 25th percentile of 16 central depths | ratio of distances, near cancels; stored on the GPU as `A - d_f` (see depth-blur.md) |
| Depth Blur, Fixed focus | `lin = d / (F - d(F-1))`, F = `farPlane` (1000) | heuristic curve, not metres; the blur start therefore scales with near (see depth-blur.md for the metre table) |
| Reflections (water pass, lot_light_bridge.cpp) | from the water VS WVP rows: `A = row2.row3 / |row3|^2`, `B = row2.w - A*row3.w`, device `z = A + B/w`; passed in PS c59 (x = A, y = B, z = 1) | INTZ bound to s7 (POINT, CLAMP) with the depth-stencil unbound and ZENABLE off during the pass |
| Ambient Occlusion | `1/z = max(A - d, 1e-7) / (near*A)` with `CameraNear()` (fallback 0.25) and `CameraDepthA()`; tanX/tanY from `CameraViewProj` rows 0/1; sky at `d >= 0.99999` (beyond about 2.8 km); outside map view the shade fades out towards the `distance` setting (default 351 m, 25..1000; fade start = distance × 150/400); with "Also in map view" (`noMapa`, default on) the open map view uses its own radii (contact 4, large 15) and fades between 5000 and 6000 m | see [../features/ambient-occlusion.md](../features/ambient-occlusion.md) |
| Edge Smoothing, "Edges from depth" | a log-depth pass (`psDepthLog`) with PS `c2 = (A, 1/(near*A))` from `CameraNear()` (fallback 0.25) and `CameraDepthA()` | Edge Smoothing does not request the camera vote; without Ambient Occlusion on, the fallbacks (near 0.25, A 1.00008) are used. See [../features/edge-smoothing.md](../features/edge-smoothing.md) |
| HDR sky boost (combined build only) | `d >= 0.99999` test only | removed in the standalone |

### Which Apex features depend on what

| Item | Features |
|---|---|
| `0x0073E060` map view getter | Depth Blur, Ambient Occlusion (map view radii) |
| WorldManager `+0x3A0` / `0x00C6C290` arg, `[0x011ECBC4]` | Frame Profiler (developer mode); GC Scheduler (combined build only, removed) |
| `[0x011D1860]+0x24+0x60` | Performance camera ids (Lot Lighting While Moving, Wall Shading While Moving, Spread New Objects Over Frames); GC Scheduler (combined build only, removed) |
| VS `c40..c43` and the near vote | Ambient Occlusion (`PostScene::WantCamera`); Edge Smoothing reads the last near and A ("Edges from depth"); the combined build's Frame Profiler fallback |
| Projection shape (A, near) | Reflections (own WVP), Depth Blur (A in Auto focus; implicitly through the heuristic in Fixed), Ambient Occlusion, Edge Smoothing (depth edges) |
| WorldManager `+0x258` | LSO (S3SS); read indirectly by the profiler's behaviour (and the removed GC Scheduler's) |

### Pitfalls

- Do not assume A = 1 or a fixed near: both early assumptions were wrong (3-digit captures). Use 9-digit captures
  (F7 records VS c0..c255 with 9 digits since 27/09).
- Do not vote on arbitrary 4x4 blocks: require the camera shape.
- WorldManager::Update is detoured by S3SS's LSO; do not detour it from Apex (frame_profiler.cpp "Not hooked on purpose").
- The camera point from 0x00C6C290 disappears in map view and loading; always have a second source.

## Address reference

| Address | Name / role | Evidence |
|---|---|---|
| `0x0073E060` | `ScriptCore.CameraController::Camera_IsMapViewModeEnabled`, `__cdecl() -> bool/char` | Disassembly (engine_map full.asm), below. map_view.cpp resolves it at run time; lot_streaming_optimizations_patch.cpp has it as `cameraIsMapViewModeEnabled` (Steam address + pattern). |
| `0x0115DD40` / `0x0115DD44` | Its entry in the script binding table in `.data`: `{0x0073E060, 0x010000A4}` | engine_map dwords.txt / datarefs.tsv (`0115DD40 DATA 0073E060`) |
| `0x010000A4` | String "ScriptCore.CameraController::Camera_IsMapViewModeEnabled" (`.rdata`) | engine_map strings.tsv |
| `0x0115DD20` | Entry `{0x0073D620, 0x00FFFFD4}` = `Camera_SetMotion` | dwords.txt / strings.tsv. **Discrepancy:** map_view.cpp's comment and the notes (28/09, "Mapa (M)") give 0x0115DD20 as the table location of the map view entry; the actual entry is 0x0115DD40. Harmless: the code searches for the name pointer, it does not use the constant. |
| `0x0073DFB0` | `Camera_EnableMapViewMode` (entry 0x0115DD30, name 0x01000034) | same walk as below, then `fld [esp+4]; push 0; ... call 0x009750C0` (a float argument) |
| `0x0073E000` | `Camera_DisableMapViewMode` (entry 0x0115DD38, name 0x0100006C) | dwords / strings |
| `0x0073E0A0` | `Camera_EnableNHoodPlopMode` (entry 0x0115DD48, name 0x010000E0) | dwords / strings |
| `0x0073DE60`, `0x0073DEB0`, `0x0073DEF0`, `0x0073DF70`, `0x0073D660` | `Camera_TetherToObject`, `Camera_Untether`, `Camera_Lock`, `Camera_Unlock`, `Camera_SetRotating` (entries 0x0115DD00..0x0115DD28) | dwords / strings |
| `0x0096B390` | getter: `mov eax, [0x011E9400]; ret` (root object; "app" in map_view.cpp) | full.asm |
| `0x00F20C20` | getter: `mov eax, [ecx+0x14]; ret` ("world") | full.asm |
| `0x0096B6D0` | getter: `mov eax, [ecx+0x8C]; ret` ("camera manager") | full.asm |
| cast id `0x110FDD89` | interface id passed to vtable slot +0x0C (QueryInterface-like cast) to get the camera interface | full.asm (`push 110FDD89h; call [edx+0Ch]`) |
| camera `+0x8B9` | byte: map view mode on | full.asm (`mov al, byte ptr [eax+8B9h]`) |
| `0x00C6D570` | `Sims3::World::WorldManager::Update` (`__thiscall(wm, unsigned, float)`, ret 8) | LSO `worldManagerUpdate` (Steam; also Retail 0x00C6D3B0, EA 0x00C6C8F0), pattern `55 8B EC 83 E4 F0 83 EC 64 53 56 8B F1 83 BE B4 01 00 00 00 57 75` |
| WorldManager `+0x258` | "skip lot streaming" gate: Update skips the lot LOD call while set | LSO map view blocker (`kWorldMgrSkipOffset`); gc_scheduler_patch.cpp comment |
| `[0x011ECBC4]` | WorldManager singleton (stored by the constructor at `0x00C67883`, `mov [imm32], esi`) | gc_scheduler_patch.cpp `worldManagerSingletonStore`, pattern `68 ?? ?? ?? ?? FF D0 89 35 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C3 74 07 8B C8 E8` (+7) |
| `0x00C6C290` | lot LOD scoring (`__thiscall`, 4 args, ret 0x10), called from WorldManager::Update at `0x00C6D6E4`; arg 2 = pointer to the camera point (16 bytes) | frame_profiler.cpp header table; `Hook_LotLodScoring` |
| `0x00C6C2B3` | `movaps [ebx+3A0h], xmm0`: stores that point at WorldManager `+0x3A0` | gc_scheduler_patch.cpp `worldManagerCameraPointStore` (pattern ending `0F 29 83`, offset 35) |
| WorldManager `+0xEC`, `+0xE8` | "Camera speed threshold", "Terrain Height Thresh" | gc_scheduler_patch.cpp comment (0xC6C290 averages |delta|/dt of the point over 10 samples as "Camera speed") |
| `0x00961440` | produces the camera point on the camera object | gc_scheduler_patch.cpp comment *(not used by code)* |
| `[0x011D1860]` | application root (getter `0x006E8330`, `mov eax,[0x011D1860]`) | gc_scheduler_patch.cpp `kSteamAppRoot`, read at `0x00C6D5BD` (WorldManager::Update + 0x4D) |
| root `+0x24` | camera object (getter `0x006E8400`, `mov eax,[ecx+24h]`) | `kSteamCameraOffset`, call at `0x00C6D5C4` |
| camera `+0x60` | camera eye position (vec3, `movaps xmm0,[eax+60h]` at `0x00C6D5C9`); its y is compared with WorldManager+0xE8 | `kSteamCameraEyeOffset` |

The camera object reached through `[0x011D1860]+0x24` (GC Scheduler) and the camera interface reached through
`[0x011E9400]+0x14+0x8C` + cast (map view) have not been shown to be the same object *(unverified)*.

## See also

- [Lot loading and streaming](lot-loading-and-streaming.md): WorldManager and the lot LOD pass.
- [Main loop, services and threads](main-loop-and-services.md).
- [Shaders](shaders.md): vertex constant conventions.
- [Architecture: post-scene chain and the INTZ depth share](../architecture.md#43-post-scene-chain-and-the-intz-depth-share).
- [Removed features](../removed-features.md#script-gc-scheduler): the Script GC Scheduler's camera sources.
