# The Sims 3: frame time and hitches during camera movement, a detailed profile

This report brings together several profiling runs from 2026-11-16. It covers where the game spends its frame, what makes
it hitch while the camera moves, how much Apex Radiance adds on top, and what has already been fixed, tried or dropped.
Section 10 lists open leads for anyone who wants to dig further.

## 0. Setup

| Item | Value |
|---|---|
| Game | The Sims 3, Steam 1.67.2.024037 (`TS3W.exe`, 32-bit). Every address below is this build's |
| Renderer | RTX Remix bridge: the 32-bit D3D9 calls are forwarded to a 64-bit process (`NvRemixBridge`) |
| Mods | Apex Radiance 4.0.1 to 4.0.2 (branch `release/4.0.2`), Sims3SettingsSetter (Smooth Patch frame limiter), WinterRoadsFix, Tiny UI Fix, ts3native.asi |
| Machine | 4.2 GHz CPU (RDTSC invariant), RTX 4070 Ti SUPER, 3840x2160 |
| Save | Sunset Valley, winter, snowing, night (in-game evening) |
| Driver | Apex's in-game test harness: loads the save, moves the camera by script, closes the game and restores every file |
| Profiler | Apex Radiance's frame profiler (`features/frame_profiler.cpp`): hooks on about 40 game functions, per-hook timing of the D3D9 callbacks and a hitch report per window |

"Self" times leave out nested hooked functions. "Hitch" means a frame longer than max(2 x the median of the last 120
frames, 8 ms).

### Runs

| Run | Camera | Length | Build | What it measured |
|---|---|---|---|---|
| A. Mod off and on | Pan over 8 lots at 40 m, 12 passes of 60 s | 4 loads x 8.5 min (off, on, on, off) | Public 4.0.1+ | Frame times with and without the ASI (`-AB`), both frame limiters lifted |
| B. Mod costs, near | Same pan at 40 m | about 9 min | Development | The mod's dispatch cost and per-fix draw cost, logged every 10 s |
| C. Mod costs, far | Same pan at 150 m | about 7 min | Development | Same as B, zoomed out |
| D. Full profile, far | Pan at 150 m, 4 passes of 60 s | 4417 frames | Development, profiler on, per-hook timing | Everything in sections 2 to 6 |
| E. Pick read-back deferred | Same as D | about 7 min | Development + the pick fix | Section 6 |

## 1. The whole game with and without Apex Radiance (run A)

| Load | FPS | p50 ms | p95 ms | p99 ms | max ms | Frames >50 ms per min | Sim hitches per min | Freezes >=300 ms per 10 min |
|---|---|---|---|---|---|---|---|---|
| 1, off | 249.1 | 3.09 | 10.69 | 21.91 | 181.4 | 5.90 | 2.71 | 0 |
| 2, on | 116.0 | 7.35 | 17.07 | 25.37 | 219.6 | 3.07 | 2.00 | 2.4 |
| 3, on | 103.2 | 8.56 | 17.51 | 26.65 | 221.3 | 3.41 | 1.18 | 0 |
| 4, off | 241.3 | 3.25 | 11.52 | 22.08 | 204.9 | 5.45 | 1.65 | 0 |

- The runner flagged every load as noisy: other programs used 11.4% to 14.6% of the machine.
- Visible freezes (gaps between the game's frame publishes) per 10 min: off, 27.1 of 100 ms or more and none of 300 ms
  or more. On, 18.9 of 100 ms or more and 2.4 of 300 ms or more.
- The mod adds about 4-5 ms to the median frame. At p99 the gap is only 3-4 ms: the worst frames are the game's own.
- With the mod on, there are fewer frames over 50 ms. Some of the mod's features hold or spread game work while the
  camera moves (section 8).
- The off loads' frames over 100 ms were led by `CAS/SimService` (176 ms) and `Canvas` (129 ms): Sim and texture
  building, not drawing.

## 2. Far pan: frame distribution (run D)

- 4417 frames: p50 12.98 ms, p95 24.18, p99 33.98, max 83.7.
- 180 hitches (4.08%). The camera was moving in 3798 frames and still in 619. The hitch rate is 4.40% while moving and
  2.10% while still.
- The last 180 hitches average 28.05 ms, against a median of 11.28 ms just before them. The camera was moving in 167 of
  them.
- Page faults per frame: 2504 in hitch frames, 167 in other frames. Hitches go with memory being touched for the first
  time: streaming.
- Three repeats of the same far pan (runs D, E and a third) gave p50 11.7-13.4 ms and p99 33.6-37.8 ms.

## 3. Render thread: totals over the far pan (run D)

| Category (render thread, self) | Total ms | Per frame ms | Calls | Longest single call ms |
|---|---|---|---|---|
| Render frame (game, FUN_00EC9F00) | 16436 | 3.72 | 4417 | |
| D3D hooks (mod) | 14727 | 3.33 | 30.4 M | |
| Lot room solve (CALL 0x00ADB9AD -> 0x006A8BA0) | 12402 | 2.81 | 121648 | 60.3 |
| Scene::BeginFrame (FUN_006EBB70) | 4775 | 1.08 | 4417 | |
| Services (self) | 2749 | 0.62 | 408261 | |
| EndScene + overlays | 1915 | 0.43 | | |
| Jobs (self) | 1589 | 0.36 | 57673 | |
| Re-issued draw (driver; the mod's own draws) | 948 | 0.21 | 4.18 M | |
| Lot load stages (CALL 0x00AEB306) | 535 | 0.12 | 19727 | |
| Present hooks (mod) | 529 | 0.12 | 4417 | |
| Scene pending nodes (CALL 0x006EBC49) | 474 | 0.11 | 4417 | 3.3 |
| Lot renderer update (FUN_00AEB2E0) | 448 | 0.10 | 406364 | |
| Texture fill (CALL 0x0060E1FF) | 406 | 0.09 | 5668 | |
| Wall AO pass (FUN_0068B810) | 374 | 0.08 | 200 | 35.3 |
| Resource lookup (FUN_004AFFC0) | 312 | 0.07 | 135924 | 0.83 |
| Lamp refresh (mod) | 274 | 0.06 | 227 | |
| Wait for job | 254 | 0.06 | 53007 | |
| Present (driver) | 239 | 0.05 | 4417 | |
| Terrain update (CALL 0x00C6D68F) | 161 | 0.04 | 4417 | |
| Key list (FUN_004B1AE0) | 108 | 0.02 | 55 | 4.0 |
| Texture create (CALL 0x0060E1DC) | 94 | 0.02 | 5668 | 0.24 |
| Lot LOD scoring (FUN_00C6C290) | 64 | 0.01 | 4417 | |
| Frame limiter | 54 | 0.01 | | |
| Object lookup by ID (FUN_00C62D40) | 37 | 0.01 | 355458 | 0.07 |
| DXT encode | 32 | 0.01 | 795 | 0.31 |
| Lot lighting update (FUN_00ADB8F0) | 23 | 0.01 | 35331 | |

Simulation thread: Script GC (GC_try_to_collect, FUN_00E4A050) took 41476 ms over 25188 calls. Its other services are
in section 5.

## 4. Anatomy of a hitch (run D)

### Per hitch, average self ms by category (last 180 hitches)

| Category | Render ms per hitch | Other threads ms | Worst single hitch ms |
|---|---|---|---|
| Lot room solve | 8.97 | 0 | 60.09 |
| D3D hooks (mod) | 3.95 | 0 | 11.81 |
| Render frame (game) | 3.84 | 0 | 8.40 |
| Scene::BeginFrame | 2.42 | 0 | 12.42 |
| Services (self) | 1.56 | 1.91 | 17.80 |
| Lot load stages | 1.31 | 0 | 15.21 |
| EndScene + overlays | 0.92 | 0 | 11.34 |
| Lot renderer update | 0.72 | 0 | 13.20 |
| Wall AO pass | 0.68 | 0 | 35.32 |
| Jobs (self) | 0.61 | 1.63 | 23.45 |
| Terrain update | 0.57 | 0 | 26.57 |
| Texture fill | 0.56 | 0 | 3.86 |
| Resource lookup | 0.46 | 0.02 | 8.85 |
| Present hooks (mod) | 0.30 | 0 | 2.69 |
| Script GC | 0 | 19.60 | 62.68 |

### Dominant cause by bucket (the largest self item of each hitch)

| Bucket | N | Dominant items (% of the bucket's hitches, average ms of that item) |
|---|---|---|
| Moving, < 16 ms | 37 | Lot room solve 95% (10.7); D3D hooks (mod) 3% (5.0); EndScene + overlays 3% (6.4) |
| Moving, 16-25 ms | 31 | Lot room solve 35% (11.1); D3D hooks (mod) 23% (5.1); Lot load stages 10% (9.6); Render frame 10% (3.7); Scene::BeginFrame 10% (6.6) |
| Moving, 25-50 ms | 91 | Lot room solve 30% (14.0); D3D hooks (mod) 24% (8.3); Lot load stages 13% (10.6); Scene::BeginFrame 10% (8.3); Render frame 8% (6.5); Terrain update 3% |
| Moving, >= 50 ms | 8 | Lot room solve 88% (39.1); Terrain update 12% (26.6) |
| Moving, top counter (N=167) | 167 | Lot room solve 81% (9.86); Texture fill 8% (2.47); Wall AO pass 5% (12.81); Resource lookup 2% (7.02); Key list 2% |
| Still, 25-50 ms | 13 | Lot room solve 100% (18.2) |

### Game counters per hitch (calls x ms inclusive, self in brackets)

| Counter | Render | Simulation | Extras |
|---|---|---|---|
| Lot room solve | 26.7 x 9.68 (8.97) | 0 | |
| Resource lookup | 197.0 x 0.46 (0.46) | 2.8 x 0.02 | 21.3 packages per lookup; 75 misses, 179 from cache, 62 absent from cache |
| Scene pending nodes | 1.0 x 0.35 (0.25) | 0 | 47 nodes, 25 deferred |
| Object lookup by ID | 326.8 x 0.03 | 152.1 x 0.03 | 456 from the index |
| Texture create / fill | 8.0 x 0.11 / 0.56 | 0 | 1.34 M pixels |
| DXT encode | 1.8 x 0.05 | 0 | 1.34 M pixels |
| Wall AO pass | 0.2 x 0.68 | 0 | |
| Key list | 0.0 x 0.10 | 0 | 1 key, 7 packages from cache |
| RefPack compress | 0.0 x 0.08 | 0 | 30.4 KB in, 8.9 KB out |

**Conclusion:** the game's lot room solve causes most hitches while the camera moves, and every hitch while it is
still. It is called about 27 times per hitch frame, about 9 ms in all, and once reached 60 ms in a single call. Lot
load stages, Scene::BeginFrame and the terrain update follow.

In this run Apex Radiance asked for 2 regathers of lot outdoor rooms in all, logged as `gather their outdoor rooms
again`. The solves are the game relighting lots as they stream in or change their detail level.

## 5. Services, jobs and waits (run D)

### Render-thread services: ms per hitch (incl / self) against ms per other frame

| Service | Per hitch | Per other frame | Calls in hitches |
|---|---|---|---|
| WorldManager | 12.53 / 0.03 | 2.82 / 0.03 | 180 |
| JobManager (main-thread jobs) | 0.94 / 0.00 | 0.12 / 0.00 | 180 |
| ResourceSystem | 0.48 / 0.48 | 0.04 / 0.03 | 180 |
| Scene service (SceneObjectManager) | 0.38 / 0.38 | 0.26 / 0.26 | 180 |
| CAS SimService | 0.30 / 0.09 | 0.05 / 0.02 | 180 |
| Swarm (VFX) | 0.26 / 0.26 | 0.18 / 0.18 | 180 |
| MessageServer | 0.20 / 0.20 | 0.01 / 0.01 | 180 |
| CAS TextureCompositor | 0.11 / 0.09 | 0.03 / 0.03 | 180 |

WorldManager carries the lot streaming: its inclusive time goes from 2.8 ms on a normal frame to 12.5 ms in a hitch.

### Jobs run on the render thread

| Job | Per hitch incl / self | Per other frame | Runs in hitches |
|---|---|---|---|
| 007297C0 | 1.05 / 0.35 | 0.16 | 2999 |
| 006EEB30 | 0.09 / 0.09 | 0.11 | 92 |
| 006F3C30 | 0.07 / 0.07 | 0.10 | 92 |
| 006CF1E0 | 0.07 / 0.04 | 0.01 | 970 |

Waits on the render thread are tiny: job waits are at most 0.05 ms per hitch, and no blocked mutex or semaphore call
showed up. The hitches are work on the render thread, not threads waiting on each other.

### Simulation thread services (total over the run)

| Service | ms incl / self | Calls |
|---|---|---|
| 007F1530 (vtable 0101E758) | 2303.8 / 2301.7 | 25188 |
| 00869000 (vtable 01029060) | 961.9 / 951.2 | 25188 |
| 0076B1A0 (vtable 01007B74) | 211.4 / 209.8 | 25188 |
| WorldManager (sim) | 46.7 | 25188 |
| 0096C2F0 (vtable 010450DC) | 46.7 / 42.5 | 25188 |

Plus Script GC, 41476 ms. The simulation thread does not block the render thread here, but Sims stop while it collects.

## 6. The mouse pick read-back (runs D and E)

Scene::BeginFrame calls the pick manager's update at 0x006EBC62; the update itself is 0x006DFEC0.

`RenderPickDelayReadback` = 1 is set in GraphicsRules.sgr for every card level and stored in `[mgr+0x16]` at 0x006DF17D.
With it on, the update alternates between two steps:
- rendering the pick target and setting `[mgr+0x14]`;
- reading it back: 0x006DFF45 calls the game's render-target copy 0x00610660, which calls GetRenderTargetData; the
  result flag goes to `[mgr+0x15]` and `[mgr+0x14]` is cleared.

While `[mgr+0x14]` is set, the update only reads back and returns (from 0x006DFF64 on runs only with it clear).

GetRenderTargetData callers in 10 s, by return address:

| Caller | Run D (game as is) | Run E (4.0.2 deferral) |
|---|---|---|
| `TS3W.exe+0x2DFF4A` (pick read-back) | 544-562 calls, 1349-1396 ms | 290-306 calls, 590-783 ms |
| `TS3W.exe+0x1FBD75` (texture compositor, Sim building) | about 10 calls, 25-31 ms | about 10 calls, 25-31 ms |
| `TS3W.exe+0x829A76` | 2 calls, 0.7-0.9 ms | 2 calls, 0.7 ms |

- Under the RTX Remix bridge, GetRenderTargetData blocks inside the call. Under DXVK the call only marks the read-back,
  and the GPU wait lands at the game's later LockRect.
- Each pick read-back costs 2.1-2.5 ms. **That holds even after an event query says the GPU has finished**: the cost is
  the bridge's synchronous copy across processes.
- 4.0.2 hooks 0x006DFEC0. When the read-back is due, it issues an event query and skips the update until the query
  signals (at most 4 frames). Picks happen about half as often, and the read-back time halves.
- A variant that also waited the full 4 frames whenever the camera moved changed nothing in the harness: 290-306 calls
  either way. It was reverted.

## 7. Apex Radiance's own cost

### Dispatch cost by D3D method (development build; per frame)

| Run | Callbacks per frame | µs each | ms per frame | DrawIndexedPrimitive | DrawPrimitive | SetTexture | SetPixelShader | SetVertexShader | Present |
|---|---|---|---|---|---|---|---|---|---|
| B, near | 3900-5900 | 0.70-1.14 | 3.3-5.7 | 1010-1572, 2.2-2.6 µs | 247-359, 3.2-7.8 µs | | | | 0.16-0.43 ms |
| C, far | 7555-10238 | 0.46-0.57 | 3.5-5.7 | 2498, 1.84 µs, 4.59 ms | 349, 2.84 µs, 0.99 ms | 4130, 0.035 µs, 0.14 ms | 910, 0.08 µs | 902, 0.05 µs | 0.35 ms |
| D, far | 8230-8283 | 0.34-0.36 | 2.8-2.9 | 2094-2110, 1.17-1.18 µs, 2.47 ms | 476, 1.09-1.23 µs, 0.52-0.59 ms | 3726-3751, 0.04 µs, 0.16 ms | 943-950, 0.11 µs, 0.10 ms | 936-943, 0.06 µs, 0.06 ms | 0.09-0.16 ms |

The draws the mod issues itself add 0.13-0.37 ms a frame in the driver.

### Per hook, far pan (run D, last second)

| Hook | ms per frame | Calls per frame |
|---|---|---|
| LotLightBridge (draws) | 1.762 | 1801 |
| PostScene | 0.104 | 2570 |
| NightTerrainRelight (Present) | 0.094 | 1 |
| SceneDither | 0.086 | 2002 |
| RtOutdoorLampsMapSwap (SetTexture) | 0.064 | 3728 |
| AmbientOcclusion | 0.035 | 1801 |
| Picture | 0.025 | 2570 |
| LotLightBridge (SetVertexShader) | 0.025 | 942 |
| FrameProfiler | 0.024 | 2570 |
| LotLightBridge (SetPixelShader) | 0.022 | 949 |
| AmbientOcclusion (SetPixelShader) | 0.021 | 949 |
| SceneTarget | 0.015 | 1801 |
| Every Present hook of the other modules | under 0.002 each | 1 |

### LotLightBridge by fix (its own time; 1 draw in 16 timed)

| Fix | Near (run B), µs per draw x draws | Far (run C), µs per draw x draws | Far, ms per frame |
|---|---|---|---|
| RoomMapSmooth (before 4.0.2) | 23.8-75.6 x 15-39 | | 0.56-1.78 near |
| RoomMapSmooth (4.0.2) | 11.4-19.8 x 10-48 | 8.4-11.7 x 54-64 | 0.54-0.63 |
| several fixes | 6.6-14.2 x 30-96 | 5.2-6.0 x 120-141 | 0.70-0.73 |
| FenceStairGroundLight | 12.0-12.9 x 13-41 | 8.0-11.0 x 59-75 | 0.55-0.69 |
| OutdoorObjectGroundLight | 2.7-3.3 x 18-58 | 3.0-3.3 x 112-225 | 0.37-0.75 |
| RoomsAtNightFurniture | 2.0-2.6 x 12-63 | 1.6-2.6 x 145-239 | 0.23-0.38 |
| FoliageVertex | 66.7-94.0 x 5-7 | 12.0-14.1 x 16-19 | 0.19-0.27 |
| WorldTerrainLight | 2.0-3.0 x 39 | 2.1-2.6 x 43-51 | 0.10-0.11 |
| FloorLight | 9.6 x 3 | 6.6-7.0 x 10-16 | 0.07-0.11 |
| ExteriorWallGain | 1.0 x 43 | 0.9 x 76 | 0.07 |
| Left to the game | 0.08-0.09 x 637-897 | 0.07-0.10 x 1086-1236 | 0.09-0.11 |
| All fixes, own time | 2.4-4.4 ms per frame | 3.3-3.8 ms per frame | |

Sub-steps (run B, before 4.0.2):
- The floors' arch rows took 87-168 µs a call, 7-15 calls a frame. 4.0.2 keeps them for the frame: 22-39 µs a call.
- Foliage: the instance pick check took 5.95 µs x 123 a frame, the instances 40.9 µs x 20.
- Fences: the per-piece runs took 4.8-7.6 µs, the lamp picks 1.5-1.8 µs.
- Fence pick memo: 7391-15660 reused against 314-1109 computed.

Night lamps (RtOutdoorLamps), per 10 s:
- 0.17-0.41 ms a frame on average on the render thread, with a largest frame of 3-9.6 ms;
- 5-33 relights;
- 0-13 story maps prepared on a worker thread (0-1148 ms there).

### Apex's GPU passes (4K, native D3D9, RTX 4070 Ti SUPER)

Ambient Occlusion by quality: 1.3 ms (Very Low, 2 slices), 1.8-2.0 (Low, 4), 2.5-2.65 (Medium, 6), 3.0-3.2 (High, 8)
and 4.1-4.5 (Ultra, 12). About 0.55 ms is fixed: copy and pyramid 0.26, blur 0.2, composite 0.09. From 4.0.2 no
built-in profile uses Ultra.

## 8. What Apex Radiance already does about performance

| Feature | What it does | Counter in run D |
|---|---|---|
| Lot lighting while moving | Budgets the game's lot lighting update (the solve's caller) while the camera moves | The solve's per-call size is untouched |
| Wall shading while moving | Holds wall shading passes while moving | 1031 run, 3288 held, 370 moved to a later frame |
| Spread new objects over frames | Spreads new scene objects over frames while moving | 278 of 17349 frames spread; largest wait 2347 objects |
| Resource lookup cache | Answers resource lookups from memory | 76% from memory (4.4 packages asked instead of up to 195); 1307170 lookups, 991396 from memory |
| Remember missing files | Answers lookups of absent files | 306142 answered from memory |
| File list cache | Keeps package file lists | 331 lists, 93% from memory |
| Faster object lookups | Index for object-by-ID lookups | 98% from the index |
| Faster memory handling | Lock spin 2000; big blocks released by a helper thread outside the lock | 1952 blocks, 71.4 ms of release work moved off |
| Faster cache compression | Faster RefPack for cache records, checked against the game | 110 records (0.9 MB), 17 compared, all equal |
| Fast DXT | Faster DXT encode of composited textures | 1172 images in 33 ms (one load) |
| Built-in shader cache | Apex's 107 shaders ship compiled | 107 ready from the cache, 0 compiled, 0 render-thread waits |
| Mouse pick read-back (4.0.2) | Defers the pick read-back to an event query | Section 6 |
| Arch rows kept for the frame (4.0.2) | Room light map smoothing reuses the floors' arch rows within a frame | Section 7 |

## 9. Tried and dropped

| Idea | Result |
|---|---|
| Deferred texture compositor read-back (tiles read back without the GPU wait) | Sim textures got slower in game: each deferred tile ended the compositor queue for the frame, so one tile per frame instead of several in the time slice. The code exists but is not started |
| More pick deferral while the camera moves | No change in the harness (section 6); reverted. The camera-moving test may not catch the harness's pan: unverified |
| Ambient Occlusion slices by reach on screen (fewer slices where the shade spans few pixels, more up close) | Far and mid views matched within 0.4-0.6 of 255. In game the shade looked blotchy: the per-pixel count broke the 4x4 interleave that the box filter cancels. Reverted |
| Smoother depth-reconstructed normals for the AO (2 px central differences on even surfaces) | Dashed lines along mesh seams (a counter top's triangle edge). Reverted |
| Half-resolution AO (earlier releases) | "Micro dots", even with the 2x2 average; removed |
| Temporal AO smoothing (earlier releases) | Shade breathing and trails with camera moves; removed |
| Cutting more from the per-draw lighting fixes | Lamp picks already reuse 95% or more; what remains is 1-3 µs a draw over hundreds of draws. No cut found that leaves the lighting as is |

## 10. Observations and open leads

### General observations

1. **The game is CPU-bound on its render thread.** That thread runs the game's frame, lot streaming, lot relighting,
   resource lookups and every D3D call. Under the bridge, every D3D call also crosses into another process. The GPU only
   shows up through Apex's screen passes.
2. **Draw count rises with zoom:** about 1000-1600 draws a frame at 40 m and 2100-2500 at 150 m. Every per-draw cost,
   the game's or a mod's, grows with it, so zoomed-out views are slower even without a single hitch.
3. **Hitches come from streaming, not drawing.** They track page faults (2504 against 167 per frame), WorldManager
   (12.5 ms against 2.8) and the room solve. Threads waiting on each other play no part.
4. **The worst game frames are not the mod's.** p99 is about 22 ms without Apex Radiance and 25-27 ms with it. The mod
   adds a steady 4-5 ms in the middle of the distribution.
5. **Address space is not tight in these runs.** About 1.6 GB free, with a largest free block of 1.38-1.42 GB. Committed
   private memory is 1.83-1.95 GB. The ASI image is 19.9 MB.
6. **Texture streaming is cheap per call:** 5668 loads (mostly 256-512 px) took 94 ms to create and 406 ms to fill over
   the run. The longest fill was 0.78 ms (2048x2048).

### Open leads, by expected gain

1. **The lot room solve (0x006A8BA0, from the lot lighting update at 0x00ADB9AD).** It averages 2.8 ms a frame, runs 27
   times per hitch frame, and reached 60 ms in one call.
   - Is it solving rooms of lots at a detail level that never shows them lit (far lots, impostors)?
   - Can a lot's solves be spread over frames, or capped per frame while the camera moves?
   - Is the same room solved again by the next detail-level switch?
   - Apex's Lot lighting while moving budgets the caller. A finer budget inside the solve, per room or per story, is
     untried.
2. **WorldManager and Lot load stages (0x00AEB306) / Lot renderer update (FUN_00AEB2E0):** 1.3 and 0.7 ms per hitch,
   with worst cases of 15 and 13 ms. These are the other half of streaming.
3. **Scene::BeginFrame:** 2.4 ms per hitch against 1.08 per frame. Part of it is the pick read-back (section 6). The
   rest is the pending scene nodes (47 nodes, 25 deferred per hitch) and the scene update.
4. **The pick read-back under the bridge:** still about 2 ms a copy. Two untried ideas:
   - skip the pick while the mouse, the camera and the object under the cursor are all unchanged;
   - read back a few pixels around the cursor instead of the whole target.
5. **Terrain update (CALL 0x00C6D68F):** small on average, but one hitch spent 26.6 ms in it.
6. **Wall AO pass (FUN_0068B810):** 200 calls in the run, with a worst case of 35 ms. It is the game's own wall
   shading. Apex already holds it while the camera moves; the long calls ran while the camera was still.
7. **Simulation thread:** Script GC (41 s over the run), service 007F1530 (2.3 s) and service 00869000 (0.96 s).
   They do not show in render hitches, but Sims stop while they run.
