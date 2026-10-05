# Terrain chunks and the terrain light bake

This page documents how world terrain is split into 256 m chunks, how each chunk's lamp stamp light map is baked on the GPU by `FUN_00C292B0`, which lights the bake accepts, how a rebuild is armed and consumed, how the result reaches the screen, and how Apex re-triggers and patches it. It also describes the Apex-made world light atlas, because several engine-facing shaders read it. Night Lighting's terrain relight, world atlas and smoothed maps, lot light pass and world lamp response depend on it.

## Scope

| | |
|---|---|
| Game build | Steam 1.67.2 (`TS3W.exe`, image base 0x00400000) unless stated |
| Used by | [Terrain relight](../features/night-lighting/terrain-relight.md), [World atlas and smoothed maps](../features/night-lighting/world-atlas-and-smoothed-maps.md), [Lot light pass](../features/night-lighting/lot-light-pass.md), [World lamp response](../features/night-lighting/world-lamp-response.md) |
| Evidence | Ghidra `re\out` (`fn_00c292b0.c`, `fn_00c296e0.c` and others), `engine_map` `full.asm`, `patches/night_terrain_relight_patch.cpp`, `features/lightmap_smooth.cpp`, `features/lot_light_bridge.cpp`, NOTAS-ILUMINACAO.md (see Sources) |

## Overview

Current Apex policy and captured world-owned lamp evidence are documented in [world lamp response](../features/night-lighting/world-lamp-response.md). Release 2.5.6 also reports native LOD chunk completion to the existing lightmap reconciliation; validated retained textures reuse immutable metadata, not baked contents.

Engine reference for TS3W.exe 1.67.2 (Steam, image base 0x00400000). It covers how world terrain is split into 256 m
chunks, how each chunk's lamp "stamp" light map is baked on the GPU by `FUN_00C292B0`, which lights that bake accepts
(visitor filter, rectangle test, lot/story gate at `0x00C294D9`), how a rebuild is armed and consumed (light-cell
countdowns `+0x38`/`+0x3C`, chunk flags `+0x55`/`+0x54`), how the result reaches the screen, and how Apex re-triggers and
patches all of this. It also describes the Apex-made world light atlas, because several engine-facing shaders read it.
Feature docs: [terrain relight](../features/night-lighting/terrain-relight.md),
[world atlas and smoothed maps](../features/night-lighting/world-atlas-and-smoothed-maps.md),
[lot light pass](../features/night-lighting/lot-light-pass.md). Smooth Streaming (combined build only) is removed from
the standalone ([../removed-features.md](../removed-features.md)); in the standalone the local terrain relight (29/09,
developer toggle) lives in the Night Lighting module and sets only `chunk+0x54`, one chunk at a time ([How Apex drives and patches the bake](#how-apex-drives-and-patches-the-bake)).

World grass, roads and every "ground light" consumer in Apex depend on one texture per terrain chunk: the chunk light map
("Terrain/LightmapTexture", the lamp stamp). The game bakes it on the GPU from the lamps whose light rectangle overlaps the
chunk, and only when something arms a rebuild. Most "straight cut" bugs of the night lighting work came from this bake:
lamps not admitted by its filters, a rebuild never armed after dusk, or a baked map from the world file that misses light
crossing a chunk border. Everything Apex does to the terrain light goes through the game's own rebuild path.

## Details

### Sources

Sources: `patches/night_terrain_relight_patch.cpp` (header comment and constants), `patches/smooth_streaming_patch.cpp`
(notes 4 and "Localized terrain relight"), `lightmap_smooth.cpp`, `lot_light_bridge.cpp`, the Ghidra output in
`S3SS-dev\re\out` (`fn_00c292b0.c`, `fn_00c296e0.c`, `fn_00c256f0.c`, `fn_00c25a90.c`, `fn_00c7e7a0.c`, `fn_00c845c0.c`,
`fn_00c7f750.c`), the disassembly `scratchpad\engine_map\full.asm`, NOTAS-ILUMINACAO.md sections 1, 1b, 1c, 2, "Mapa de luz
suavizado", "Lote escuro ao lado da calcada" and "Corte na borda do lote (28/09)", and `scratchpad\ground_report.md`.
Where a statement rests on one of these only, the table says so. "(unverified)" means nobody confirmed it at runtime or
in code.

### Data structures

#### Light manager and light cells

`lightMgr = *(*(0x011D1860) + 0x1C0)`. Offsets used here (the full layout is in
[light-objects-and-rigs.md](light-objects-and-rigs.md) and [room-light-maps.md](room-light-maps.md)):

| Offset | Type | Meaning | Evidence |
|---|---|---|---|
| lightMgr+0xD4 | ptr | Light update tree (lot hash) | night_terrain_relight `QueueAllLotOutdoorRooms`, light_diag |
| lightMgr+0xF0 | float | Night level, 0 = day, 1 = night; `> 0.99` is "night" for `IsNight` and for Apex's dusk detection | `0x006AC560`; night_terrain_relight `OnPresent` |
| lightMgr+0x104 | ptr | Light cells object ("cells") | `0x006E97D0` |

Light cells (world lights are bucketed in 32 m cells; see the rig doc):

| Offset | Type | Meaning | Evidence |
|---|---|---|---|
| cells+0x10 / +0x14 | vector | Cell list walked by `0x006B5DA0` after the decrement (unverified meaning) | full.asm `006B5DBE` |
| cells+0x30 | int | Read by `DirtyAllRigs` prologue (`cmp [ebp+30h],ecx`) (unverified meaning) | object_light_bridge `kDirtyAllBytes` |
| cells+0x38 | int | Countdown A ("lights changed"): set to 50 by the three light register/remove/move sites; decremented by `0x006B5DA0`; `-1` = consumed | full.asm |
| cells+0x3C | int | Countdown B ("full rebuild"): set to 50 only by `0x006B5730`; decremented by `0x006B5DA0`; `-1` = consumed | full.asm |

`0x006B5750` decides which countdown matters: in live play (WorldManager+0x1B4 != 0) only `+0x3C == 0`; in tool mode
(`+0x1B4 == 0`) `+0x38 == 0` and `IsNight`. Consequence: **in normal play `+0x38` alone never rebuilds the terrain**, and
there is **no night condition** on the rebuild (smooth_streaming note 4 says the same). The older header comment of
`night_terrain_relight_patch.cpp` ("when the counter reaches 0 AND it is night ... or edit mode") and its status string
"the game only rebuilds the terrain light at night (or in Build mode)" describe the tool-mode branch; the disassembly and
the later smooth_streaming analysis win. The status string is therefore misleading (see [Discrepancies between sources](#discrepancies-between-sources)).

World mode `WorldManager+0x1B4` (smooth_streaming note 1): `FUN_00C6D970` (load world) sets 1, `FUN_00C5FE40` sets 3
("saveInGameMode") or 2 ("editInGameMode"); 0 is the engine's tool mode, never seen in normal play.

#### Terrain object (`terrain = WorldManager+0x58`)

| Offset | Meaning | Evidence |
|---|---|---|
| +0x14 | Back pointer to WorldManager (the arm test reads `[+0x14]+0x1B4`; `0x00C7E7A0` too) | full.asm `00C84B03`, `00C7E7E2`; `fn_00c845c0.c` |
| +0x64 | State object; bytes +0x1C, +0x1D, +0x20 gate texture work (edit / loading) (unverified meaning) | `fn_00c7e7a0.c`, `fn_00c7f750.c` |
| +0x68 | Compositor (ECX of `0x00C25A90` at `0x00C7E96E`); `compositor+0x08` = bake cell grid (ECX of `0x00C2A470` at `0x00C2570B`) | smooth_streaming `ReadCellGrid` |
| +0xB0 / +0xB4 | `std::vector<Chunk*>` begin / end: a dense row-major grid, slot `(z0/256)·nx + x0/256` | full.asm `00C84C43`, `00C816A2..00C816C6`; smooth_streaming `kTerrainChunksBegin/End` |
| +0xC0 / +0xC4 / +0xC8 | Grid width nx, height nz (copied from TerrainData `+0xCC/+0xD0`), cell size 0x100 | full.asm `00C8161B..00C81638` |
| +0xF0 / +0xF1 | Bytes gating a pass over chunks at the top of the update (unverified) | `fn_00c845c0.c` line ~179 |
| +0x110 | Camera position copied on entry of `0x00C845C0` | smooth_streaming `kTerrainCamera` |
| +0x140 | Flags; bit 0x20 toggled around a timed branch (unverified) | `fn_00c845c0.c` |

Bake cell grid (`grid = *(*(terrain+0x68)+8)`): `grid+0xBC` = array of record pointers, `grid+0xCC` = width, `grid+0xD0` =
height (both ≤ 1024 in Apex's sanity check). Index = `(chunk+0x0C >> 8, chunk+0x10 >> 8)`, row-major `iz*nx+ix`. A record
is 4 ints `{x0, z0, w, h}`; the bake converts them as unsigned to float and uses `[x0, x0+w] × [z0, z0+h]` (world units).
Evidence: smooth_streaming `ReadCellGrid` / `ChunkBakeRect`, `fn_00c292b0.c` (the `+ _DAT_00f98940` unsigned fix-ups).

#### Terrain chunk

| Offset | Type | Meaning | Evidence |
|---|---|---|---|
| +0x00 | int | Chunk kind; kind 3 takes other paths in the update loop (`0x00C250E0`, `0x00C84090`) (unverified meaning) | `fn_00c845c0.c` |
| +0x04 / +0x08 | uint | Chunk corner x0 / z0 in world units; also the world-file map lookup (`0x00C321B0`) and the road partition (`0x00B789B0`: `partition[(z>>8)·nx + (x>>8)]`) | full.asm `00C816C9..00C816D1` (creation), `00B789B0` |
| +0x0C / +0x10 | uint | Chunk **centre** X / Z = corner + size/2; `>> 8` = bake grid index; the texture render passes them to `0x00C25A90` | full.asm `00C816E0..00C816F2`; `fn_00c7e7a0.c` |
| +0x14 / +0x18 | uint | Chunk size w / h (256; also VS c15 = 1/w) | full.asm `00C816D4..00C816DD` |
| +0x40..+0x4C | int[4] | Rect {x0, z0, x0+w, z0+h}; compared against a visible-rect in the update loop | full.asm `00C81703..00C8170E`; `fn_00c845c0.c` (`piVar14[0x10..0x13]`) |
| +0x50 | byte | Pending job for `0x00C246E0(chunk+0x74, x, z)` (unverified); `(WM+0x1B4 == 0)` at creation | `fn_00c845c0.c`; full.asm `00C8171F` |
| +0x51..+0x53 | byte | LOD states read by `0x00C853C0` (1 at creation) | full.asm |
| +0x54 | byte | "Re-render composited textures": set by `0x00C853B0` (and to `(WM+0x1B4 == 0)` at creation: in live play never until something flags it), consumed by `0x00C7E7A0` in the one-chunk-per-update branch. Apex's local terrain relight sets it on one chunk at a time ([How Apex drives and patches the bake](#how-apex-drives-and-patches-the-bake)) | `fn_00c7e7a0.c`; full.asm `00C81747`; smooth_streaming |
| +0x55 | byte | **Relight / rebuild request**: set on every chunk when the countdown fires (`0x00C84C5A`), consumed by the per-chunk loop (runs `0x00C834F0`/`0x00C80E50`, `0x00C83060`, `0x00C7FA70`, `0x00B789B0`, `0x00C853B0`) | full.asm; `fn_00c845c0.c` |
| +0x56 | byte | Second rebuild request, set on every chunk when `0x006FDE10`/`0x006FDDB0`/`0x006FDDD0` report their own countdown (another subsystem; unverified which) | `fn_00c845c0.c` lines ~321-326 |
| +0x57 | byte | Edit-path flag (calls `0x00C2A4B0(x, z)`) (unverified) | `fn_00c845c0.c` |
| +0x5C / +0x60 | int | Compared in the update loop (unverified) | `fn_00c845c0.c` |
| +0xD0 | tex | "Terrain/MixCompositedTexture" (type 0) | `fn_00c7e7a0.c` |
| +0xD4 | tex | "Terrain/ColorCompositedTexture" (type 4) | `fn_00c7e7a0.c` |
| +0xD8 | tex | **"Terrain/LightmapTexture" (type 5)**: the rebuilt lamp stamp. 0 until the first rebuild; then the lot pass and the world pass bind it | `fn_00c7e7a0.c`, `fn_00c7f750.c` |
| +0xDC | tex | "Terrain/WorldLeafTexture" (type 7) | `fn_00c7e7a0.c` |

Apex keys chunks by the chunk centre `(round(c8.w), round(c10.w))` of the world VS (lot_light_bridge `RecordWorldChunk`),
and lightmap_smooth assumes `key = 256·i + 128` (`ChunkIndex`). `chunk+0x0C/+0x10` is the centre (resolved 29/09 from the
creation `0x00C815E0`: corner + size/2), so the key and `+0x0C >> 8` agree.

#### Bake context (`this` of `0x00C296E0` / `0x00C292B0`)

Inferred from `fn_00c292b0.c`, `fn_00c296e0.c` and full.asm (unverified as a named type):

| Offset | Meaning |
|---|---|
| +0x04 / +0x08 | Output size (texels) |
| +0x14 / +0x18 | Record w / h (world units); +0x1C / +0x20 = record x0 / z0 (written through `0x00C28280`) |
| +0x24 | Texture type being rendered (0, 4, 5, 7) |
| +0xF0 | Shader parameter handle: light position |
| +0xF4 | Shader parameter handle: light colour / weight |
| +0x100 / +0x104 | Parameter handles for "Lighting/RenderLightmap/NormalMap" and ".../HeightMap" |
| +0x110 | Current light position (from vfunc+0x24) |
| +0x120 | Current light colour (rgb from light+0xF0, w = range × intensity.x × 0.2) |

### Call flow

#### Arming and consuming a rebuild (per frame, render thread)

1. Game code changes a light: register `0x006B64B0`, remove `0x006B6090`, move/toggle `0x006B6590`. Each tests
   `light->vfunc20()` and, if true, sets `cells+0x38 = 50`. A per-object light command (`0x006B08A0` from `0x006B1850`)
   instead sets `cells+0x3C = 50` for each light of the object whose vfunc+0x20 is true (`0x006B5730`). vfunc+0x20 is true
   for the street-lamp class (vtable `0x00FF42F8`, type 0xB); lot lamp classes return false (night_terrain_relight header;
   `lot_edge_lighting_patch.cpp` calls the same vfunc "on/usable", so whether it also depends on the lamp being lit is
   unverified).
2. Every frame `0x006B5DA0` counts both countdowns down to 0.
3. `0x00C845C0`: `cells = GetLightCells()`; if `CountdownDone(live)` (and, only in tool mode, `IsNight`), it consumes
   (`0x006B5770`: both = -1) and sets `+0x55` on every chunk.
4. The per-chunk loop rebuilds **every** flagged chunk in the same call (the `+0x55` branch does not set the "work done"
   flag): geometry (`0x00C834F0` / `0x00C80E50`), then `0x00C83060`, which **re-renders the chunk's textures
   synchronously** (`0x00C7E7A0` at `0x00C8307E`: the type-5 bake, type 7, render-target readback and CPU DXT of 4 mips),
   `0x00C7FA70`, the road mark `0x00B789B0`; then it sets `+0x54` and clears `+0x55`/`+0x56`. Chunks skipped because an
   earlier chunk did work in this call keep `+0x55` for a later call (`0x00C84FDC`).
5. `+0x54` then makes the loop call `0x00C7E7A0` again (one chunk per call, `0x00C85041`): every chunk is rendered a
   **second** time, one per frame (a duplicate sweep).

   So a full rebuild costs every chunk's bake and DXT in the consume frame plus the duplicate sweep. MEASURED 29/09
   (`ApexRadiance_Hitches.txt` #927): terrain update 173 ms + DXT encode 57 ms in one frame (512 DXT calls = 128 textures
   x 4 mips = 64 chunks x types 5 and 7; 229-241 ms frames), then about 3.5 ms per frame (`DXT x8`) for 64 frames.
   **Correction (29/09, research\perf2\chunkrelight.md 0):** this section used to say, after smooth_streaming note 4, that
   the `+0x55` work only rebuilt geometry and that "a full rebuild therefore costs about one chunk texture render per
   frame"; the synchronous `+0x55` work is the expensive part. `+0x54` alone (no `+0x55`) is the light path: one render
   of one chunk per terrain update, no geometry, no LOD steps, no road mark, no duplicate.

Picking up a street lamp in Build mode is the canonical trigger: a call trace showed 128 chunk rebuilds and 243 light
texture rebuilds afterwards (night_terrain_relight header). Setting `+0x55` directly on a subset of chunks is the game's
full rebuild restricted to those chunks (smooth_streaming "Localized terrain relight", combined build), but it bakes all
of them synchronously in one frame and renders them twice: the 12-60 ms hitches of the combined build's reconciliation.
The standalone's local terrain relight sets only `+0x54`, one chunk at a time ([How Apex drives and patches the bake](#how-apex-drives-and-patches-the-bake)).

#### The bake `0x00C292B0` (type 5, "staticTerrainLightmap")

1. Looks up the material "staticTerrainLightmap" (`0x0079B840`), binds the chunk's "Lighting/RenderLightmap/NormalMap"
   and ".../HeightMap" (built by `0x00C245D0` / `0x00C28140` for the record rect).
2. Builds a "Terrain/Lights" visitor (vtable `0x010768A0`) and calls the light enumerator `0x006ACF70` with it (only if
   `GetLightManager()` is non-zero). The visitor keeps a light iff `light->vfunc20()` (site `0x00C29626`).
3. For each kept light:
   - **Rect test**: `r = light->vfunc2C()` (= light+0x134 `{minX, minZ, maxX, maxZ}`); kept iff
     `r.minX ≤ x0+w`, `x0 ≤ r.maxX`, `r.minZ ≤ z0+h`, `z0 ≤ r.maxZ` (`0x00C29480..0x00C294C8`).
   - **Lot / story gate**: `lot = GetLotID()`; if `lot != 0` the light is dropped unless `light+0xD0 == 0`
     (`0x00C294D9: cmp dword [edi+0D0h],0; jnz 0x00C295BC`). World lights (lot 0) always pass.
   - Sets the light position (vfunc+0x24) and `(colour +0xF0 .rgb, range × intensity.x × 0.2)` as shader parameters,
     then draws (`0x0079A300`). One additive pass per light (inferred from the per-light draw).
4. Returns 1; `0x00C296E0` then runs `0x00C28CD0(0)` on the result (unverified what it does; type 7 calls it with 1).

Light rect: `0x006BDDF0` sets `+0x134..+0x140 = (x - s, z - s, x + s, z + s)` with
`s = sqrt(+0x130 / (k1 × [0x011D1180].x))`. k1 (`0x011D0A60`) measured 1.0 at runtime (F8 of 16:09, NOTAS "Lote escuro");
the value of `[0x011D1180].x` was never read (unverified). NOTAS 4d measured rects of about 50 m for ranges 40..100.

Stamp law: the shader itself was never disassembled (ground_report section 6). Fitted on the smoothed atlas around a
street-lamp pair, the ground follows `2 · w · cos / d²` with `w = 0.2 · range · intensity` (shader_patches.cpp comment above
`AppendPixelLamps`, 28/09), saturated at 1 next to the lamp. PASSO3-PLANO.md's Design B wrote it as
`E = c0.rgb · sat(c0.w / d²) · (N·L ≥ 0 ? sat(sqrt(N·L)) : 0)` (unverified). The bake has **no occluder input** (only the
normal and height maps are bound; ground_report section 5, inferred), so walls never block terrain light.

The map format: the world file ships a 256×256 DXT5 map with 4 mips per chunk (bound via `0x00C321B0` while `chunk+0xD8`
is 0). A rebuilt map `+0xD8` is also a managed 256×256 DXT5 with **4 mips**: `0x00C296E0` renders into the
"TerrainCompositor" render target, copies it to system memory (`0x00617DE0`, `0x00610660`), and `0x00618CD0(tex, sysmem,
0)` at `0x00C29AB6` encodes 4 mips into the same texture object (the pointer does not change, so consumers see the new
content without rebinding). Evidence: 512 DXT calls = 128 textures x (256² + 128² + 64² + 32²) px in a full-rebuild
frame (research\perf2\chunkrelight.md 1.3). The earlier "1 mip when rebuilt" (here and in lot_light_bridge
`RecordWorldChunk`) was wrong, corrected 29/09. 1 texel per metre.

#### How the map reaches the screen

| Consumer | Binding | Evidence |
|---|---|---|
| World terrain, single pass | PS samples the map from `s8` (4 paint layers), `s7` (3 layers) or another sampler depending on the variant; `uv = (world − chunk centre)/256 + 0.5`; VS `c15 = (1/256, 1/256, 0.5, 0.5)`, chunk centre in VS `c8.w`/`c10.w`; lamp term `texld rT, v1, sLM; mul rL.xyz, rT, c7.x` (c7.x = 1 in captures). The map alpha is also used for sun visibility | NOTAS 1, 1b; lot_light_bridge `RecordWorldChunk`; ground_report 1A |
| World terrain, multi-pass (albedo passes + modulate2x light pass + fog) | summer light pass `475E594D/756` samples `s2 × c3.x²`; winter `1A5B580F/1548` samples `s7 × c4.x²` | ground_report 1B |
| Lot grass light pass | `0x00C7F750` with the lot flag binds `chunk+0xD8` when non-zero (else nothing); the stock 568-byte pass samples the lot `LightMap` (s1) × c3.x, modulate2x | night_terrain_relight `kLotPassSite` comment; NOTAS 1 |
| Roads / sidewalks | Their own copy of the chunk map (s6/s4/s2 by variant), same uv via VS `c16` (winter) / `c14` (summer) | NOTAS "Ruas"; shader_patches `IsRoadVs` |

Details of these shaders are in [shaders.md](shaders.md).

### The world light atlas and the smoothed maps (Apex-made)

Not engine structures, but every Apex "ground light" consumer depends on them, and they mirror the chunk maps 1:1.
Code: `lightmap_smooth.cpp`, `lot_light_bridge.cpp` (`RecordWorldChunk`, `ChunkTexture`).

- **Registration**: every world terrain draw whose VS `c15 == (1/256, 1/256, 0.5, 0.5)` is scanned for the chunk map:
  samplers s15 down to s1, the first 2D texture of 256×256, at most 5 levels and not `D3DFMT_Q8W8V8U8` (the normal map is
  256×256 Q8W8V8U8 with 9 mips; paint layers are 1024²). Key = rounded VS `(c8.w, c10.w)`. The texture is AddRef'd in
  `g_chunks` (lot_light_bridge.cpp `RecordWorldChunk`).
- **Smoothed map** per chunk: level 0 read with `LockRect` (must be a managed 256² DXT5; otherwise counted "unreadable",
  and an older smoothed copy is dropped), decoded, colour ratio blurred (σ 1.5) with brightness kept sharp, enlarged 4× with
  a cubic B-spline reading 6 texels of each neighbour chunk, dithered, full mip chain, `1024×1024 A8R8G8B8`
  `D3DPOOL_DEFAULT`. Maps are hashed round robin (FNV-1a 64 over the 64 KB DXT5) to detect a game rebuild; chunks used by
  recent draws (`Entry::lastUse`) go first. A worker thread does the heavy work; at most 2 results wait.
- **World atlas**: one `D3DUSAGE_RENDERTARGET A8R8G8B8 D3DPOOL_DEFAULT` texture, 512 texels per 256 m chunk (2 texels/m,
  mip 1 of the smoothed maps), covering every chunk seen plus one chunk of margin, at most 16×16 chunks (8192²; larger
  worlds get no atlas). Cleared black with `ColorFill` when created, filled per chunk with `UpdateSurface` from 512² SYSTEMMEM staging
  textures (since 28/09: a plain 2x copy of a changed chunk at once, then its smoothed mip 1; on growth the old contents
  are copied into the larger atlas with `StretchRect`, no black cells). Mapping: `uv = world.xz × c.xy + c.zw`, `c.xy = 1/(chunks × 256)`,
  `c.zw = −minChunk × 256 × c.xy` (`LightmapSmooth::Atlas`).
- **Why the atlas exists**: a lot or road can extend past its "home" chunk. With CLAMP addressing the lot pass stretched
  the chunk's last row (LightProbe-m76: lot origin (1440, 1290), rotated 45°, reading the chunk centred (1408, 1152) that
  ends at z = 1280). The summer lot pass and the snowy lot pass now read the atlas: summer VS computes
  `uv = (world − c15) × c14.xy + c14.zw`, so only c14 is replaced with `(a.xy, a.zw + c15.xz × a.xy)`; winter uses c16/c15
  the same way (lot_light_bridge.cpp `OnDrawInner`, `DrawLotSnow`).
- Precision/format limits: 8-bit A8R8G8B8 everywhere, so baked lamp light is at most 1.0 before each shader's scale
  (survey_lamp_paths.txt "two facts").

Consumers of the atlas (feature docs): lot grass (summer and snow), roads, winter floors, summer outdoor floors, fences /
railings / stairs, snow on objects and stair tops, outdoor rig objects.

### How Apex drives and patches the bake

All in `patches/night_terrain_relight_patch.cpp` unless noted; every site is byte-checked before writing.

| Site | Original | Apex change | Option (TOML key) |
|---|---|---|---|
| `0x00C29626` (visitor) | `8B 07 8B 50 20 8B F1 8B CF FF D2` (`mov eax,[edi]; mov edx,[eax+20h]; mov esi,ecx; mov ecx,edi; call edx`) | `mov esi,ecx; push edi; call TerrainLightTest; nop×3`: original test, else accept an outdoor lot lamp (lot id != 0, type 3..6, flags alive 0x01 + enabled 0x40 + room known 0x04, room `+0x08 == 0`) **that is lit** (0x20) | `luzDoLoteNaGrama` (default true) |
| `0x006B6516`, `0x006B60D3`, `0x006B6618` | `8B 17 8B 42 20 8B CF FF D0` | `push edi; call ArmTest; nop×3`: original test, else the same outdoor-lot-lamp test (lit or not); only counted (`g_lotLampArms`); in live play it only arms `+0x38`, which has no effect ([Light manager and light cells](#light-manager-and-light-cells)) | `luzDoLoteNaGrama` |
| `0x00C294D9` (story gate) | `83 BF D0 00 00 00 00` | `call BakeLevelStub; nop×2` (the `jnz` kept; ZF = 1 keeps the light). `BakeLevelTest`: story 0 → keep (as the game); story > 0 → keep only an outdoor lot lamp that is lit (and the option is on); basements (< 0) refused. Optional: if the surrounding bytes differ, the rest installs without it | `luzDoLoteNaGrama` |
| `0x00C7F87D` (lot pass) | `8B 87 D8 00 00 00 85 C0` | `call LotPassStub`: world pass unchanged; lot pass jumps to the null-bind `0x00C7F8B7` so lot grass keeps the lot LightMap | `gramaDoLoteUsaLuzDoLote` (experimental, default false, dev UI only) |
| `0x00C84C43` arm loop | set `+0x55` on all chunks | Combined build only: Smooth Streaming queued the chunks and released K per call, nearest to the camera (`patches/smooth_streaming_patch.cpp`). **Removed in the standalone**: the game's loop is left alone | (Smooth Streaming, removed) |
| `0x00C8504C` (chunk texture re-render call) | `call 0x00C7E7A0` | `call ChunkRenderThunk` (`__fastcall`, 2 stack args: same contract), calls the original, then if chunk+0x54 == 0 reports `(chunk+0x0C >> 8, chunk+0x10 >> 8)` to `LightmapSmooth::NoteChunkRendered` and the terrain, chunk and QPC time of the call to `ChunkRelight::OnChunkRendered` (completion of the local relight's chunk in flight). Optional (warning if the context differs; without it the local relight is off) | always (Night Lighting installed) |
| `chunk+0x54` (data only) | 0 in live play until flagged | **Local terrain relight** (29/09, `features/terrain_chunk_relight.cpp`): set on one chunk at a time, the chunks whose bake record overlaps a changed lamp's old or new rect (+1 m), the lamp's own chunk first; at most one per frame with a free frame in between, at most 8 per second, never while any chunk has `+0x55`/`+0x56`, only while `0x00C7E7A0`'s early-exit gates are open; timeout 120 frames with nothing else pending (1200 in all) → `+0x54` put back to 0, full rebuild. **Paced sweep**: every chunk the same way, nearest to the camera first. [terrain-relight.md](../features/night-lighting/terrain-relight.md) "Local terrain relight" | `relightNearbyChunks`, `relightPacedSweep` (developer, default false) |

Runtime logic (render thread, `OnPresent` from the Present hook):

- **Kick** = write `cells+0x38 = cells+0x3C` (50 in the combined build; **3** in the standalone since 28/09, `kArmFrames`). It is consumed by the game within that many
  frames; Apex detects consumption as `+0x38` going from ≥ 0 to -1 and then treats every lamp as covered
  (`MarkAllCovered`).
- **Dusk**: when lightMgr+0xF0 crosses 0.99 upwards, a kick is scheduled after `atrasoSegundos` (default 2 s, 0.5..10)
  if `automaticoAoAnoitecer` (default true). The game's night setter `0x006ADD60` never arms the countdown, so without this
  a save loaded by day keeps the day-time terrain light.
- **World load** (`cells` pointer changed): `Settle` waits until the set of lit lot lamps has not changed for 3 s (at least
  5 s after the load, at most 60 s), then one full kick. It runs by day too, because the world-file maps also miss the part
  of a lamp's light that crosses into the neighbouring chunk (NOTAS 1b: cut at x = 1280 between chunks (1408, 1152) and
  (1152, 1152)).
- **Reconcile** (about once a second, not while a full rebuild is pending): `Bakeable` lamps (from
  `LotLightBridge::ForEachOutdoorLotLamp`, refreshed every 20 frames by the light enumeration) are diffed against what the
  bake was last given (`g_baked`, sorted by pointer). New / removed / switched lamps and moves over 5 cm are "structural";
  colour, intensity or range changes over 2 % (abs 0.002) are value changes, throttled to one relight per 5 s. The old and
  new rects go to `SmoothStreamingRelightTerrainRects(rects, n, 0.4)` (combined build: a function of
  `smooth_streaming_patch.cpp`; standalone: the same logic inside the Night Lighting module, flagging directly, no queue),
  which flags (or, in the combined build with Smooth Streaming on, queues) the chunks whose bake rect overlaps any rect
  with a 1 m margin. Returns -1 (terrain unreadable) or -2 (more than 40 % of the chunks): then a full
  kick, at most every 15 s. More than 2048 rects also means a full kick. `relightLocal` (default true) off = always full.
- **Local terrain relight** (standalone, 29/09, developer toggle `relightNearbyChunks`, default off): a lamp change the
  decisions take (terrain-relight.md "Lamp change decisions") queues the chunks under the changed lamps' old and new rects
  (`LotLightBridge::DiffBake` changes; light `+0x134`) instead of the kick: `chunk+0x54` only, paced (table above). It
  falls back to the kick when the terrain / grid / chunk layout is not as studied, the world's first rebuild has not run,
  a chunk has no rebuilt light map, or more than 16 chunks (or a quarter of the world) would be hit. Developer toggle
  `relightPacedSweep`: Apex's own dusk and lamp-change kicks become a paced `+0x54` sweep of every chunk (the load
  rebuild and the button stay kicks). The "Reconcile" bullet above is the combined build's.
- **Lot relight** (experimental `recalcularLotesAoAnoitecer`, default false): 3 s after the dusk rebuild is consumed,
  room 0 of every loaded lot story is queued through `0x006C7160` (see [room-light-maps.md](room-light-maps.md)).

Developer counters (developer mode, Night Lighting > Developer > Status): "Terrain: armed / rebuilt / local relights (chunks,
fallbacks)", "Lot lamps: armed / on the ground / off", "Terrain bake, lot lights: ground story / lot lamps / upper stories /
refused". If "on the ground" grows while "lot lamps" stays 0, lamps are lost between the visitor and the bake (their rect).

### Which Apex features depend on what

| Engine item | Apex features |
|---|---|
| Countdowns, `+0x55`, `0x006B5770` | Terrain relight (dusk, load settle, reconciliation, button "Rebuild terrain light now"); combined build only: Smooth Streaming (spread the rebuild; removed) |
| Visitor `0x00C29626`, story gate `0x00C294D9`, light rect `+0x134` | "Lot lights light the ground outside the lot" (`luzDoLoteNaGrama`), local relight rects |
| `chunk+0xD8`, world-file map, VS c15 / c8.w / c10.w | Lot light bridge, smoothed maps, world atlas, roads, snow, floors, fences, objects (everything reading ground light) |
| `0x00C7F87D` | Experimental "Lot grass keeps the lot's own light" |
| lightMgr+0xF0 | Dusk detection; night-weighted shader tweaks (moon-shadow fade uses it as `c3.x`/`cN.x`, see [shaders.md](shaders.md)) |

### Discrepancies between sources

- **Night condition**: code comments in `night_terrain_relight_patch.cpp` (header lines ~7-10 and the status string
  "Rebuild pending: the game only rebuilds the terrain light at night (or in Build mode)") contradict the disassembly of
  `0x00C84C09..0x00C84C3A` and `0x006B5750`: in live play the only test is `cells+0x3C == 0`. The status text is shown when
  `+0x38 == 0` by day, which in live play is simply the idle state.
- **Which countdown the game arms on a lamp change**: the header says `+0x38/+0x3C` are both set to 50 by
  register/unregister/move; the three sites only write `+0x38`. `+0x3C` has a single writer (`0x006B5730`). Apex's own
  kick writes both, so its behaviour is unaffected.
- The public `technicalDetails` text lists the quality sites as `0xADB66F/0xADB888`; the code patches the instructions at
  `0x00ADB66B/0x00ADB884` (the imm8 is 4 bytes later). Same patch.

### Pitfalls and failed approaches (do not retry)

- NOTAS 1, tried and failed for the lot-border cut: widening the world-light gather radius; recomputing room 0; redoing the
  type-5 terrain textures; high lighting quality on every lot (also dropped FPS to 63); disabling the terrain texture in
  the lot layer (the `0x00C7F87D` stub, kept only as an experimental option); counting street lamps as lit in the solve.
  What worked was the lot light bridge (`max(lot map, terrain map)`), see [lot-light-pass.md](../features/night-lighting/lot-light-pass.md).
- NOTAS 1c: an automatic "unlock" that reacted to any armed `+0x38` rebuilt the whole terrain every ~30 s (16 times per
  session), because lots streaming in register lamps. Fixed then by a 1/min limit, later replaced by the reconciliation.
- Re-queuing lot solves after every terrain rebuild made a loop (relighting re-registers lamps, which re-arms), so lot
  relights run only after the dusk rebuild.
- The fixed load rebuild (15 s, then 5 s after "World loaded") often ran before nearby lots had loaded and lit their
  lamps; replaced by `Settle`.
- The "Rebuild terrain light now" button did not fix the cut at houses on a foundation: their ground floor is story 1 and
  the story gate `0x00C294D9` drops every lot light with `+0xD0 != 0` (probe2: lot 6C11001B51A4D8B0, origin (900.8, 59.41,
  1185.6), type-4 lamps with d0 = 1/2, atlas = 0 there). Fixed by `BakeLevelStub`.
- DXT5 RGB565 endpoints make dim lamp light purple/green on snow; the 1 texel/m map looks blocky. Fixed texture-side
  (smoothed maps), never by changing the bake.
- The world-file maps miss light that crosses a chunk border (cut on world grass at chunk seams); only a rebuild fixes it.
- A per-pixel terrain term was designed (PASSO3-PLANO.md increment 5: re-render the stamp at 4 texels/m) but never built.

### Open items / unverified

- The stamp pixel shader and its exact falloff (never disassembled); the meaning of `0x00C28CD0`.
- `[0x011D1180].x` (light rect divisor) at runtime.
- What `chunk+0x56` / `0x006FDE10` count, and the chunk fields marked unverified above.
- Whether vfunc+0x20 of the street-lamp class also depends on the lamp being lit.
- Which script command reaches `0x006B08A0` (the `+0x3C` arming path).
- Local terrain relight (29/09): `+0x54` does not mark the road partition (`0x00B789B0`, only the `+0x55` branch does).
  The light map is updated in place, so roads that bind `+0xD8` per draw should see it; whether roads keep their own copy
  that only the road mark refreshes (NOTAS "Ruas") is unverified: check roads after a local relight in game. Also
  unverified at run time: the bake record equals the chunk rect, TerrainData `+0x1C/+0x1D/+0x20` and WM `+0x1B4` values
  in live play (the local relight checks the record, the rect and the gates before every chunk it flags).

## Address reference

| Address | Name / role | Evidence |
|---|---|---|
| `0x011D1860` | Global "scene root" pointer; light manager = `*(root + 0x1C0)` | imm32 of `mov eax,[0x011D1860]` in `0x006E97B0` (bytes `A1 <imm32> 85 C0 75 01 C3 8B 80 C0 01 00 00`, validated by night_terrain_relight `kRootGetterBytes`, light_diag `kRootGetterTail`) |
| `0x006E97B0` | `GetLightManager()`: root ? root+0x1C0 : 0 | full.asm; used by the bake (`fn_00c292b0.c`) and every Apex module |
| `0x006E97D0` | `GetLightCells()`: root+0x1C0 → +0x104 | full.asm `006E97DA..E0` |
| `0x006AC560` | `IsNight(lightMgr)`: `lightMgr+0xF0 > [0x00FC2A74]` (= 0.99, dword `3F7D70A4`) and byte `[0x011D08C0] == 0` | full.asm; dwords.txt |
| `0x006ADD60` | Night-level setter (switches lamps on at dusk, never arms a terrain rebuild) | night_terrain_relight header comment (call trace) |
| `0x006B64B0` | Light registration; at `0x006B6516` tests light vfunc+0x20 and writes `cells+0x38 = 50` | full.asm `006B6516..6523`; Apex arm site |
| `0x006B6090` | Light removal; same test at `0x006B60D3`, `cells+0x38 = 50` | full.asm `006B60D3..60E0` |
| `0x006B6590` | Light moved / toggled; same test at `0x006B6618`, `cells+0x38 = 50` | full.asm `006B6618..6625` |
| `0x006B5730` | Arms the FULL rebuild: `cells+0x3C = 50` | full.asm (only writer of `+0x3C = 0x32`) |
| `0x006B08A0` | Per-object light command: for each light of an object id whose vfunc+0x20 is true, calls `0x006B5730` (call at `0x006B0923`); only caller is `FUN_006B1850` (`0x006B18CE`) | full.asm, calls.tsv. That `FUN_006B1850` is the script light-object dispatcher is inferred from NOTAS ("FUN_006b0b50 ... chamado por FUN_006b1850") (unverified) |
| `0x006B5DA0` | Per-frame countdown decrement: `+0x38` and `+0x3C` each go down by 1 while > 0 | full.asm `006B5DA0..5DB7` |
| `0x006B5750` | `CountdownDone(cells, bool live)`: live ? `+0x3C == 0` : `+0x38 == 0` (thiscall, ret 4) | full.asm `006B5750..5767` |
| `0x006B5770` | Consume: `+0x38 = +0x3C = -1` | full.asm `006B5770..5779` |
| `0x011ECBC4` | WorldManager global; `WorldManager+0x58` = terrain, `+0x1B4` = world mode, `+0x5C` = RoadNetwork, `+0x54` = object read by `0x00C61040` | smooth_streaming_patch.cpp notes 1, 4; GameAddr `WorldManagerPtr` (the store `mov [0x011ECBC4],ebp` at `0x00C6D0CC` in `0x00C6CF80`; other writers `0x00C67883`, `0x00C6B507`, `0x00C7E281`) |
| `0x00C6D570` | `WorldManager::Update`; calls the terrain update at `0x00C6D68C` (`mov ecx,[esi+58h]; call`) | smooth_streaming `kTerrainCallSite`, pattern `8B 4E 58 E8`; GameAddr `TerrainUpdateCall` (the local relight reads its disp8) |
| `0x00C845C0` | Terrain update, thiscall(terrain, float* camera, char), ret 8, render thread. Arms and consumes the relight, runs the per-chunk rebuild loop | `fn_00c845c0.c`; smooth_streaming `kTerrainUpdate` pattern `55 8B EC 83 E4 F0 8A 45 0C 81 EC C4 00 00 00 53 56 57 8B F9 8B 4D 08 88 87 F4 00 00 00` |
| `0x00C84B03..0x00C84B15` | `bl = (WorldManager+0x1B4 != 0)` ("live"), also stored at `[esp+1Ch]` | full.asm |
| `0x00C84C09..0x00C84C3A` | Arm test: cells = `0x006E97D0()`; `0x006B5750(live)`; if not live also `IsNight` | full.asm |
| `0x00C84C3C` | `mov ecx,esi; call 0x006B5770` (consume) then the loop `0x00C84C43..0x00C84C5E` that sets `chunk+0x55 = 1` on every chunk | full.asm; smooth_streaming `kTerrainArm` pattern `8B CE E8 ?? ?? ?? ?? 8B 87 B0 00 00 00 8B 8F B4 00 00 00 3B C1 74 0D 8B 10 83 C0 04 3B C1 C6 42 55 01 75 F3 E8` |
| `0x00C84FDC`, `0x00C85041` | Per-chunk loop. Once a chunk did work in this call (byte `[esp+0Ch]`), later chunks are skipped and keep their flags. Otherwise: LOD state not ready (`0x00C853C0` on `+0x60` / `+0x5C`) or `+0x55` / `+0x56` set → the rebuild branch `0x00C85058`, which does NOT set "work done" (every such chunk in the same call); else `+0x50` → the `0x00C246E0` job (work done); else `+0x54` → the texture render `0x00C7E7A0(chunk, 0)` at `0x00C8504C` (work done): **one chunk per terrain update** | full.asm `00C84FDC..00C850B5` (re-read 29/09; research\perf2\chunkrelight.md 1.1-1.2) |
| `0x00C853C0` | LOD state ready for `(chunk, lod)`: LOD 1 → `+0x51 == 0`; LOD 3 → `+0x52 == 0 && +0x78 != 0`; LOD 4 → `+0x53 == 0`; else true | full.asm |
| `0x00C815E0` | Terrain chunk creation: vector sized by `0x00C29E80(TerrainData)`, `terrain+0xC0/+0xC4` = TerrainData `+0xCC/+0xD0`, `terrain+0xC8 = 0x100`; slot `(z0/256)·nx + x0/256`; fills the chunk fields of [Terrain chunk](#terrain-chunk) | full.asm `00C815E0..00C81768` |
| `0x00C61040` | `[ecx+0x54] ? [[ecx+0x54]+8] : 0` (ECX = WorldManager); a gate of `0x00C7E7A0` | full.asm |
| `0x00C8504C` | In the per-chunk loop: `push 0; push esi; mov ecx,edi; call 0x00C7E7A0` (the +0x54 texture re-render, one chunk per call; context bytes at `0x00C85047`: `6A 00 56 8B CF E8 4F 97 FF FF C6 44 24 0C 01`). `0x00C7E7A0` = `void __thiscall(terrain, chunk, char force)`, `RET 8`; its full path ends with `mov byte [esi+54h],0` at `0x00C7E978` (the only +0x54 write in it). Other callers: `0x00C8088E`, `0x00C8307E`. Apex (28/09) redirects only this CALL to report re-rendered chunks to the smoothed maps; since 29/09 it also times the call (QPC) and tells the local terrain relight that the chunk it released was rendered | full.asm, `re/out/dump/asm/00c7e7a0.asm`, `00c845c0.asm`, engine_map calls.tsv |
| `0x00C834F0` / `0x00C80E50` | Chunk geometry rebuild (which one depends on a local flag) | `fn_00c845c0.c` lines ~445-448 |
| `0x00C83060`, `0x00C7FA70` | Further per-chunk rebuild steps run for `+0x55`/`+0x56`. `0x00C83060(chunk, 1, flag)` first calls **`0x00C7E7A0(chunk, 0)` at `0x00C8307E`: a synchronous texture render** (type-5 bake, type 7, render-target readback, CPU DXT of 4 mips), then LOD-dependent steps | `fn_00c845c0.c`; full.asm (research\perf2\chunkrelight.md 0) |
| `0x00B789B0` | Marks the road partition of the chunk (`chunk+4`, `chunk+8`) when `+0x55` was set | `fn_00c845c0.c`; smooth_streaming |
| `0x00C853B0` | Sets `chunk+0x54` (re-render the 4 composited textures) | smooth_streaming note 4 |
| `0x00C86170` | Chunk init (the only other writer of `+0x55`) | smooth_streaming note 4 |
| `0x00C7E7A0` | Re-renders a chunk's composited textures (Mix `+0xD0`, Color `+0xD4`, Lightmap `+0xD8`, WorldLeaf `+0xDC`) through `0x00C25A90`, then clears `+0x54`. Early exits that leave `+0x54` set (`0x00C7E7B4..0x00C7E823`): all four textures exist and `+0x54 == 0` (never in live play, where Mix / Color are never created); live (WM `+0x1B4` != 0) and TerrainData (terrain+0x64) `+0x1D == 0`; `0x00C61040(WM) != 0` and TerrainData `+0x20 == 0`. With `0x00C61040(WM) == 0` it does not create `+0xD8`. In live play it renders only type 5 (`+0xD8`) and type 7 (`+0xDC`) | `fn_00c7e7a0.c` (resource names "Terrain/...CompositedTexture", "Terrain/LightmapTexture", "Terrain/WorldLeafTexture") |
| `0x00C25A90` | Renders the 4 textures: type 0 → Mix, type 4 → Color (scale `[0x00F9A5AC]`), type 5 → Lightmap, type 7 → WorldLeaf | `fn_00c25a90.c` |
| `0x00C256F0` | One texture: cell record `0x00C2A470(x, z)`, `0x00C28280(x0, z0, w, h)`, then `0x00C296E0` | `fn_00c256f0.c` |
| `0x00C2A470` | Bake cell grid lookup (ECX = grid = `*(*(terrain+0x68)+8)`) | `fn_00c256f0.c`; smooth_streaming `ReadCellGrid` |
| `0x00C296E0` | "TerrainCompositor" render of one texture: sizes, render target (format code 10 for types 5 and 7, 2 otherwise), clear colour (type 4: 0; 5/7: `0xFF000000`; else `0xFFFFFF00`); type 5 → `0x00C292B0` then `0x00C28CD0(0)`; type 7 → `0x00C28CD0(1)`; others `0x00C285E0` (+ `0x00C28A40` unless type 6) | `fn_00c296e0.c` |
| `0x00C292B0` | **Terrain light bake** ("staticTerrainLightmap"): gathers lights with the "Terrain/Lights" visitor, draws one pass per accepted light | `fn_00c292b0.c`; strings in the function |
| `0x010768A0` | Vtable of the "Terrain/Lights" collector visitor; slot 0 = `0x00C29620` | `fn_00c292b0.c` (`ppuStack_74 = &PTR_LAB_010768a0`) |
| `0x00C29620` | Visitor: `if (light->vfunc20()) push_back(light)`; the vfunc call site is `0x00C29626` | full.asm `00C29620..965E` |
| `0x006ACF70` | Enumerate every light: stdcall(visitor*), visitor vtable[0] = thiscall(visitor, Light*) ret 4 | light_diag.cpp / lot_light_bridge.cpp `kEnumLightsBytes` `E8 2B 36 00 00 8B 4C 24 04 51 68 40 CF 6A 00` |
| `0x00C29480..0x00C294C8` | Bake: per light, rect `light->vfunc2C()` (= light+0x134) tested against the chunk bake rect | full.asm |
| `0x006BC020` | `BaseLight::GetLotID()`: returns `+0xC0/+0xC4` (64-bit lot id) | re/out report.txt; split_level_lighting_fix pattern `8B 81 C0 00 00 00 8B 91 C4 00 00 00 C3` |
| `0x00C294D9` | **Story gate**: `cmp dword [edi+0D0h],0` (7 bytes) followed by `jnz 0x00C295BC` at `0x00C294E0`; reached only when lot id != 0 (`or eax,edx; je 0x00C294E6` at `0x00C294D5`) | full.asm; night_terrain_relight `kBakeLevelOrig`, `kBakeLevelBefore` (at `0xC294CE`), `kBakeLevelAfter` |
| `0x00C294F5` | Light position (vfunc+0x24) → bake+0x110 → shader parameter bake+0xF0 | full.asm |
| `0x00C2950F..0x00C29533` | Light colour `+0xF0` → bake+0x120; `.w = vfunc+0x28() (= +0x130 range) × intensity.x (+0x10) × [0x00FAA20C]` | full.asm; `0x00FAA20C` = `3E4CCCCD` = 0.2 (dwords.txt) |
| `0x0079A300` | Draw call of one light pass (after `0x00799CF0` set parameter bake+0xF4) | full.asm `00C295B7` |
| `0x006BDDF0` | Sets the light rect `+0x134..+0x140` = position `+0x120`/`+0x128` ± `sqrt(+0x130 / (k1 × [0x011D1180].x))`, k1 read from `0x011D0A60` through the tweakable getter `0x00F626F0` | full.asm `006BDDF0..DE8A` |
| `0x00C7F750` | Per-chunk light/fog pass setup, `(chunk, lot flag [ebp+0Ch], out)`: binds "terrainLightMap" (`chunk+0xD8` if non-zero, else the world-file map `0x00C321B0(..., chunk+4, chunk+8, 5)`), "bIsLotTerrain", "posScale", "normalTexMapping", "terrainLightFog" | `fn_00c7f750.c` strings and lines ~57-70 |
| `0x00C7F87D` | Inside `0x00C7F750`: `mov eax,[edi+0D8h]; test eax,eax` (lot pass binds the rebuilt map) | night_terrain_relight `kLotPassSite`, context bytes `F3 0F 10 05 38 A5 07 01 ...` |
| `0x00C7F8B7` | Null-bind branch (`A1 80 CE 1E 01 6A 00 6A 00`) | night_terrain_relight `kLotPassNullBind` |
| `0x00C24BE0` | Registers "normalMap" / "terrainLightMap" / "normalTexMapping" parameters of the world terrain material | `fn_00c24be0.c` |
| `0x00C2A290` | Called by WorldManager::Update; returns at once unless TerrainData+0x1C is set (edit only) | smooth_streaming "Localized terrain relight" |
| `0x006B58F0` | `DirtyAllRigs(cells)`: every rig in the cells re-gathers (not a terrain function; listed because the dusk path relies on it) | object_light_bridge.cpp `kDirtyAllRigs`, bytes `83 EC 10 55 8B E9 33 C9 33 C0 39 4D 30` |

## See also

- [Light objects and rigs](light-objects-and-rigs.md).
- [Room light maps](room-light-maps.md).
- [Lot loading and streaming](lot-loading-and-streaming.md).
- [Shaders](shaders.md).
- [Removed features: Smooth Streaming](../removed-features.md#smooth-streaming).
