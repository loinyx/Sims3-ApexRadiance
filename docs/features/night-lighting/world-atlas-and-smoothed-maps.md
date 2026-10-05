# World light atlas and smoothed terrain light maps

Lamp light on the ground is smooth and evenly coloured: the blocky 1 m steps and the purple and green specks of the
game's compressed terrain light maps disappear, most visibly on snow. The same smoothed light is collected into one
world map that lots, floors, fences, snow and outdoor objects read, so they all match the ground around them, across
chunk borders. By day, lamp light that the game drops from the terrain is restored at a subdued strength, with the
game's own sunlight and shadows untouched. Part of [Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released (part of Night Lighting since 0.1.0; GPU path default). Native solar alpha, daylight range and squared terrain lamp scales: in development (PR #2) |
| Default | On |
| Menu | Lighting > Ground > Ground & Lots > *Smooth ground light* |
| Configuration | `[patches.NightTerrainRelight]` `mapaDeLuzSuavizado` in `ApexRadiance.toml` |
| Source | [`features/lightmap_smooth.cpp`](../../../features/lightmap_smooth.cpp), [`shaders/lightmap_smooth_ps.hlsl`](../../../shaders/lightmap_smooth_ps.hlsl), world terrain draw in [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) |

## The problem

Two visible defects come from the chunk light map itself:

- Blocky "low resolution" lamp circles: 1 texel per metre stretched with bilinear filtering, plus the 4x4 DXT blocks.
- Purple / green specks at night: DXT5 stores colour as RGB565 endpoints (green 6 bits, red and blue 5), so dim light
  is rounded to tinted values.

And one structural need: many surfaces (lot grass beyond its home chunk, snowy floors, fences, doors, snow on objects)
know only their world position, not a chunk. They need the terrain light anywhere, across chunk borders.

By day the world terrain shaders multiply the lamp RGB of the map by zero (`c7.x` single pass, `c3.x` squared in the
multi-pass), so lamps the game keeps lit in Build mode preview add nothing to the ground while the lot grass shows
them. The map's alpha is sun visibility and must not be smoothed with the lamp RGB.

## How Apex Radiance solves it

`lightmap_smooth.cpp` takes every terrain chunk light map the world draws (256x256 DXT5), removes the RGB565 colour
noise, enlarges it 4x with a cubic B-spline that reads across neighbouring chunks, and keeps the result as a 1024x1024
A8R8G8B8 texture with a full mip chain. Mip 1 of every smoothed chunk is also written into one **world light atlas**
(2 texels per metre). The default path does this on the GPU in the frame the game's map changes; a CPU worker path is
the automatic fallback.

1. **Register**: each world terrain draw identifies its chunk light map (`RecordWorldChunk`).
2. **Smooth**: a changed chunk is rebuilt (GPU passes or CPU worker); until then the game's current map is shown.
3. **Draw the world terrain**: the smoothed RGB is swapped into the draw's sampler, the native map stays bound on a
   spare sampler for its alpha, and the lamp scale constant is adjusted for the ground brightness and daylight.
4. **Serve consumers**: lots, floors, fences, snow and objects sample the atlas with their world xz.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Smooth ground light | `mapaDeLuzSuavizado` | bool | on | | Pushed every frame (`LightmapSmooth::SetEnabled`); off calls `Clear()` (everything released). The Doors, counters and fences options are disabled without it (no atlas) |
| Ground brightness | `brilhoNoChao` | float | 100% | 25 to 300% | Terrain lamp scale constant (see *World terrain draw*) |
| Developer: Smooth the ground light maps on the GPU (A/B) | `mapaDeLuzSuavizadoNaGpu` | bool | on | | Registered only in developer mode; pushed every frame (`SetGpuPreferred`), applied at the next Present: a switch calls `Clear()` and the new path rebuilds every map. Normal mode always prefers the GPU and falls back to the CPU by itself |

Registration happens after the *Street lamps light lots* gate of the dispatch, so with that switch off no chunk is
registered and neither smoothed maps nor the atlas exist. This is why the atlas consumers need both switches.

## Compatibility and interactions

- [Terrain relight](terrain-relight.md): every game rebuild changes chunk maps, one chunk per frame. Each re-rendered
  chunk is reported by the call-site notice (or found within a few frames by the boosted hash checks). GPU path: a chunk
  reported by the notice is smoothed before its next draw, in the frame the game re-rendered it (one build per frame
  during a rebuild); one found by the Present hash check is smoothed the next frame; neighbour borders follow after the
  sweep within the 1 ms budget. CPU path: the game's map and a plain atlas copy at once, the smoothed map about one job
  later. A local relight's chunks are reported by the thunk or, when the native LOD path rendered them, by
  `FinishFlight` ([world lamp response](world-lamp-response.md)).
- [Lot light pass](lot-light-pass.md): uses the atlas, else `ChunkTexture` -> `Find`.
- Removed HDR build: the atlas and smoothed maps are 8-bit and clamp at 1.0 ([removed-features.md](../../removed-features.md)).

## Limitations

- The atlas is the light of the ground: no height and no occlusion. Balcony railings get the ground light under them;
  stair rails indoors near a street lamp can glow.
- Worlds larger than 16x16 chunks get no atlas: every atlas consumer falls back to the game (lot passes to the home
  chunk map).
- World terrain variants other than the recognised ones keep the blocky DXT5 look (and the game's daytime lamp factor).
  The daylight range adjustment applies only to the exact captured single-pass shader.
- TerrainLow (distant terrain, DXT5 map in s3, census FF6760A8) is not smoothed.
- A shader whose light-map read cannot be patched for native alpha keeps the game's unsmoothed map.

## Technical reference

### Registration (`RecordWorldChunk`, render thread, per draw)

For every draw whose PS is `WorldCandidate` (declares s6+), `WorldCompact` (with `kWorldCompactVs`) or `WorldMultiLight`
(with `kWorldMultiLightVs`), the terrain uv mapping constant must be (1/256, 1/256, 0.5, 0.5): VS c15, or c13 for the
multi-pass. The light map is found by scanning s15 down to s1, only among the samplers the PS declares
(`g_worldSamplers`, recorded by `Classify`; unknown shader = every sampler), for a 2D texture of 256x256 with at most
5 levels and format != Q8W8V8U8 (the normal map is also 256x256 but has 9 mips and Q8W8V8U8; paint layers are
1024x1024). A texture already registered for another chunk (`g_chunkOfTexture`) is skipped and counted. The key is
`Key(c8.w, c10.w)` rounded (chunk centre, 256 m steps, centre = 256 i + 128); the texture is kept AddRef'd in
`g_chunks`, then `LightmapSmooth::Get(key, gameTexture)`:

- creates or updates the `Entry` for the key; stores the game texture AddRef'd; a new texture pointer resets `hash = 0`
  (CPU path) or bumps the version `gver` (GPU path);
- sets `lastUse = g_frame`; a new entry or changed `lastUse` marks the entry for `ByUse()`, the kept order (lastUse
  descending, key ascending) that the GPU service and both Present steps use;
- returns the smoothed texture only while it was built from the game's current map (CPU: `hash == doneHash`; GPU:
  `gBuiltVer == gver`), else nullptr. `Find` (roads, lot passes without the atlas) follows the same rule.

Recognised world terrain variants:

| Variant | PS | VS | Lamp sampler | Lamp scale |
|---|---|---|---|---|
| Single pass, 2 to 4 paint layers | `WorldCandidate` (declares s6+) | any with c15 mapping | s6, s7, s8; winter s10 to s12 | c7.x (linear) or a squared c4.x (winter multi-pass layout), found by `LightMapScaleConst` |
| Compact, single diffuse layer | `kWorldCompactPs` 1296 B / `73376C6A` | `kWorldCompactVs` 744 B / `34E1F1B7` | s3 only | c7.x |
| Summer multi-pass light | `kWorldMultiLightPs` 756 B / `EC3141AB` | `kWorldMultiLightVs` 656 B / `5882F972` (uv from c13, chunk matrix c8/c10) | s2 only | c3.x, squared by the shader |

Both shaders of a pair must match. The compact pair's rig-mode field does not make it an object; its bytecode computes
chunk terrain geometry and world lamp-map uvs.

### World terrain draw

1. `LightmapSmooth::Get` gives the smoothed map, or nullptr.
2. **Native solar alpha.** With a smoothed map, the PS is replaced by a copy from `ShaderPatches::PatchTerrainNativeAlpha`
   (cached per pinned game shader in `g_terrainAlphaPs`): a second `texld` of the same uv from a spare sampler (highest
   declared + 1, same partial precision) and `mov rLight.w, rTemp.w`. Requires ps_2_0 or ps_3_0, one direct `texld` of
   the lamp sampler with full-mask temp destination, 2D declaration, no relative addressing, no flow control, free
   temp and sampler. `NativeTerrainSampler` binds the game's map on the spare sampler, cloning its 11 sampler states
   (filter, LOD, addressing, sRGB) and restoring them, and any partial change, afterwards. An unsupported layout or a
   failed creation keeps the game's unsmoothed map. Cost: one texture read and one move per smoothed draw.
3. **Lamp scale** (`TerrainConst`): constant k from `TerrainLampConst` (multi-pass: c3, squared). For night level n and
   ground gain g, `TerrainLightingPolicy::LampScale` writes `K w + (1 - n) g` (linear) or `sqrt(K^2 w + (1 - n) g)`
   (squared), `w = 1 + (g - 1) n`; at n = 1 exactly the previous night value. Skipped when n = 1 and g = 1.
   `LightMapScaleConst` accepts only an exclusive multiplier: a linear `mul` after the fetch, or a squared scalar temp
   `mul rT.c, cK.x, cK.x` used once on the light RGB, with cK read exactly twice in the shader and never `def`'d.
4. **Daylight range** (single pass, smoothed, day weight > 0): `ShaderPatches::PatchTerrainDaylightRange` accepts only
   the captured native-alpha-patched ps_3_0 (387 DWORDs, FNV-1a `3A0A3E52`) with exactly one
   `mad r1.xyz, r1, c8.x, r5` light accumulator, and appends `min rA.xyz, r1, c223.y; lrp rB.xyz, c223.x, rA, r1;
   mov r1.xyz, rB`. c223 = (day weight 1 - n, 2.0) for this draw, saved and restored. By day this caps the light
   accumulator at 2.0 before albedo, the ceiling the multi-pass composition has (it writes half the light to an 8-bit
   target and doubles it in material blending). Twilight blends smoothly; at full night the alpha-only shader is used.
   Three ALU instructions, two temps, no texture read, no extra draw.
5. Smoothed texture swapped into the lamp sampler, draw, game texture and PS restored. F7 text:
   `mod draw: world terrain | compact | multi-pass | lamp sampler | gain | smoothed | daylight range candidate`.

### GPU path (default)

The smoothed map of the game's current map is on screen in the frame the game's map changes, with the same look as the
CPU path (expected within 1/255 per channel), no CPU readback, decode, upload or worker. Shaders:
`shaders/lightmap_smooth_ps.hlsl` (the run-time copy `lightmap_smooth_hlsl.h` must stay identical); the eight entry
points are compiled at start-up on a background thread and `InitGpu` creates the objects.

**Passes per chunk** (`BuildOne`; one `DrawPrimitiveUP` quad each, `D3DFVF_XYZRHW | D3DFVF_TEX1` with ps_3_0; every
fetch a point sample at a texel centre with `tex2Dlod` level 0; samplers POINT / POINT / mip NONE, CLAMP, sRGB off,
MAXMIPLEVEL 0). P grid = source texels -4..259 (P = source + 4, 264x264): the widest reach of the math is 4 texels into
the neighbours (blur 3 + B-spline 1); the CPU's `kBorder` = 6 only pads.

| # | Entry point | Input -> target | Math (identical to `Process`) |
|---|---|---|---|
| 1 | `GatherPS` | the game's DXT5 maps -> RAW (264x264 float) | 9 quads, one per neighbour region (x: [0,4) left, [4,260) chunk, [260,264) right; same in z), each with that map on s0. A missing neighbour draws the centre map with coordinates outside [0, 1]: CLAMP = the CPU's clamp to the centre chunk (a missing diagonal clamps both axes). The GPU's DXT5 decoder replaces `DecodeDxt5` |
| 2 | `HBlurPS` | RAW -> HB (264x264 float) | 7-tap Gaussian (0.0366, 0.1112, 0.2167, 0.2710, ...) of (R, G, B, Y), Y = 0.299 R + 0.587 G + 0.114 B |
| 3 | `VBlurPS` | HB -> CH (264x264 float) | Vertical 7 taps; ratio = RGB / Y clamped 0..4 (1 when Y <= 1e-4), faded to 1 by saturate(Y / 0.03) |
| 4a | `HUpYAPS` | RAW -> HUYA (1024x264 float) | s = (x + 0.5)/4 - 0.5, f = floor(s): cubic B-spline of Y and A over P f+3..f+6 |
| 4b | `HUpChromaPS` | CH -> HUC (1024x264 float) | Linear between P f+4 and f+5 (weight s - f) |
| 5 | `VUpPS` | HUYA + HUC -> chunk level 0 (1024x1024 A8R8G8B8) | Vertical B-spline / linear, v = Y x ratio x 255 + d, d = (bayer + 0.5)/16 - 0.5 from the CPU's 4x4 table (`4 b2(x&1, y&1) + b2(x>>1&1, y>>1&1)`, b2(a,b) = 2a + 3b - 4ab: fixed per texel), clamped 0..255, `floor(v + 0.5) / 255` written |
| 6 | `DownPS` | chunk level 0 -> atlas cell (512x512 at the cell, viewport = whole atlas) | 2x2 box of the bytes, `(sum + 2) / 4` rounded down = the CPU's integer mip 1 |
| 7 | `DownPS` / `CopyPS` | level l-1 -> MIP[l] (scratch), MIP[l] -> chunk level l, l = 1..10 | Integer mip chain; written from separate scratch textures so no pass samples the texture it renders to |

The split, the order of enlargement and of every sum follow the CPU code, so float results differ only by operation
order / FMA (about 1e-7 relative). Constants: c0 (s0 size: 1/w, 1/h, w, h), c1 (s1 size, `VUpPS` only).

**Formats and memory.** Chunk maps: 1024x1024 A8R8G8B8, 11 levels, `D3DUSAGE_RENDERTARGET`, `D3DPOOL_DEFAULT` (5.6 MB
of video memory each). Scratch (shared, one chunk at a time): RAW, HB, CH (264x264) and HUYA, HUC (1024x264) in
`A32B32G32R32F` (3 x 1.1 + 2 x 4.3 MB), else `A16B16G16R16F`, plus MIP[1..10] A8R8G8B8 (1.4 MB): about 13.4 MB (7.4 MB
with 16-bit floats). No system memory (the developer compare allocates about 20 MB briefly). Formats checked with
`CheckDeviceFormat(D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE)`. `D3DUSAGE_AUTOGENMIPMAP` is not used: its filter is up to
the driver (DXVK blits with linear filtering), not the integer `(sum + 2) / 4`.

**When a chunk is built** (`GpuService`, from the draw hooks, inside the game's scene):

- A change is seen by (a) the re-render notice (`DrainNotices`, next draw; a lockable map whose hash did not change is
  not rebuilt: "same map"; the pending flag is read with a plain load before the locked exchange), (b) a new texture
  pointer in `Get` (also a new chunk), (c) the fallback hash checks at Present (`GpuHashCheck`: round robin 1 per
  frame, 4 chunks in view per frame while boosted; only lockable managed DXT5 maps, `gNoHash` for others). Each bumps
  `gver`.
- `Get` builds a chunk whose version is not built yet before its draw. `Find` and `Atlas(forDraw)` run the same service
  first. The first service of a frame, and any later one after a new change, builds every chunk in view with its own
  change (cap 32), refills atlas cells from existing smoothed maps (cap 64, after growth or a failed copy), and spends
  up to 1 ms of GPU (measured per-chunk time, 0.5 ms until measured) on chunks out of view and neighbour borders. Held
  while a rebuild is imminent (`ExpectRebuild`, except chunks in view); borders wait for the end of a rebuild sweep and
  30 quiet frames.
- A failed or interrupted build sets `gBuiltVer = 0`: the game's map is shown, retried next frame.
- Present (`OnPresentGpu`): timestamp read-back, fallback hash checks, sweep end, `EnsureAtlas`. No drawing at Present.

The CPU path's `AtlasRawCopy` is not used: every consumer runs the service before reading. Exceptions show the game's map
or a slightly late cell, never black: a change seen only by the Present hash check (one frame late), a chunk out of view
beyond the 1 ms budget, more than 32 chunks in view changing at once (world load).

**Device state.** `PassState` (RAII) saves and restores through the device: render targets 0..3 (1..3 unbound), the
depth-stencil (`ExtraHooks::RawGet/RawSetDepthStencilSurface`, unbound: the atlas is larger than the screen), PS and
VS, declaration / FVF, stream 0 and the stream frequencies of streams 0 and 1, textures and 8 sampler states of s0 and
s1, 14 render states (Z, Z write, blend, separate alpha, alpha test, stencil, cull, scissor test, fog, sRGB write, clip
planes, colour write, fill mode, WRAP0), PS c0..c1, viewport and scissor rect. One save / restore and one timestamp
pair per batch. `g_gpuBusy` stops re-entry. Get / Find / Atlas are called by the handlers before they change any state.
An exception inside turns the feature off (`g_failed`, released at the next Present).

**Reset.** `OnPreReset` releases the chunk render targets, the scratch, the atlas and the queries (shaders stay); the
first draws after the reset rebuild every chunk.

**Fallback.** `InitGpu` (first Present with the feature on, once per session) needs pixel shader 3.0, A8R8G8B8
render-target textures, a float render-target format and the 8 shaders. Failure: log `[LightmapSmooth] GPU smoothing
unavailable (why): using the CPU path`, status `CPU (GPU unavailable: why)`. Later scratch failure: `GPU smoothing
failed (why): switching to the CPU path`. Success: `GPU smoothing ready (intermediates F, PS 3.0 N instruction slots)`
and `Smoothing on the GPU`. A path switch (`ResolveMode`) calls `Clear()`.

**Developer compare.** "Compare GPU vs CPU (one chunk)" (`RequestCompare`): at the next ground draw, the most recently
drawn chunk whose map and neighbours can be locked is read (`ReadMap`), rebuilt on the GPU, read back
(`GetRenderTargetData`), smoothed by `Process` on the render thread (about 30 ms once) and compared level by level. Log:
`GPU vs CPU, chunk (x, z): level 0 max diff R r G g B b A a (texels off by 1: n, by more: m of 1048576), levels 1-10 max
diff d`. Expected differences come only from the GPU's DXT5 decoder (under 1/255 per texel), float order (about 1e-7)
and, with 16-bit floats, 11-bit mantissas (about 1/8 LSB): at most 1 per channel, dim texels amplify a decode
difference through the colour ratio (at Y about 0.03 about 1 LSB).

**Cost estimate.** About 9 M point fetches (8.9 M of 128-bit texels, most in `VUpPS`: 1 M pixels x 6) and 17 MB of
render-target writes per chunk: about 0.2 to 0.5 ms on a mid-range desktop GPU, maybe 1 ms or more on integrated
graphics, plus about 40 draws and render-target switches per chunk. The status shows the measured EMA per chunk.

### CPU path (fallback and developer A/B)

| Step | What happens | Where |
|---|---|---|
| Detect | (a) the chunk re-render notice: read the same frame; (b) new texture pointers, up to 4 per frame; (c) for 60 frames after a kick and `kSweepFrames` = 300 after a consumed rebuild: 4 chunks in view per frame, round robin; (d) one round-robin chunk per frame. The hash is 64-bit words over the locked level 0 (`HashRows`); the 64 KB is copied only when it changed | `OnPresentBody` 1-4, `CheckEntry`, `ReadMap` |
| Show correct | `Get` / `Find` return nullptr until the smoothed map of the current map exists. The atlas cell gets a plain copy of the current map (`AtlasRawCopy`: integer DXT5 decode, bilinear 2x at the smoothed mip 1's sample positions), chunks in view first, within 2 ms per frame, through a ring of 3 staging textures locked with `D3DLOCK_DONOTWAIT` | `OnPresentBody` 5 |
| Queue | Only the key is queued (the dirty flag coalesces); at most `kMaxInFlight` = 2 jobs queued + processing + waiting upload. Chunks in view first; a chunk's own change before a neighbour's border change. Held while a rebuild is imminent; during a sweep only chunks already re-rendered; borders after the sweep and 30 quiet frames. Skipped when its inputs (its map and the 8 neighbours' hashes, `SigOf`) equal those of its smoothed map (`doneSig`) | `OnPresentBody` 6 |
| Process | The worker reads the latest maps of the chunk and neighbours (`g_shared`, under `g_mx`) when the job starts; times each job | `WorkerMain` |
| Upload | Dropped if the map changed again ("outdated"). Otherwise one persistent SYSTEMMEM staging (1024², 11 levels, `D3DLOCK_DONOTWAIT`: busy = one frame later, "later") and `UpdateTexture` into the chunk's own `D3DPOOL_DEFAULT` texture; a new chunk takes a texture from a pool of up to 4 or creates one. Generation must match (results from before a `Clear` are dropped). Then `AtlasUpload` of level 1 (512x512) | `Upload` |

Each frame (`OnPresentBody`): take the notices and sort by `ByUse()`; `CheckEntry` each notice; up to 4 new textures;
while boosted 4 chunks in view, else one round-robin chunk (`g_checkCursor`) when no new texture was read;
`EndSweepIfDone` (every chunk re-rendered or 300 frames), `EnsureAtlas`, plain copies; queue jobs, upload one result.
A changed map marks its entry `dirty` and the 8 neighbours `borderDirty`.

**Processing** (`Process`, detached worker `WorkerMain`):

| Step | Detail |
|---|---|
| Extended source | 268x268 (256 + 2 x `kBorder` 6). Border texels from the registered neighbour, else clamped to the centre chunk; each neighbour decoded one at a time (`DecodeDxt5`) |
| Planes | Luma `Y = 0.299 R + 0.587 G + 0.114 B`, alpha A (sun visibility; kept) |
| Chroma cleanup | Separable 7-tap Gaussian, sigma 1.5, over RGB and Y; ratio = blurred RGB / blurred Y, clamped 0..4, neutral (1) when blurred Y < 0.03 |
| Enlarge 4x | Output texel j at source (j + 0.5)/4 - 0.5; cubic B-spline (4 taps, no ringing) for Y and A; bilinear for the ratio |
| Encode | A8R8G8B8, R at bit 16; 4x4 Bayer dither (+-0.5 LSB) on RGB and A; value = Y x ratio x 255 clamped 0..255 |
| Mips | 2x2 box down to 1x1: 11 levels |

The worker waits while 2 results are pending (5.6 MB each). A worker exception gives an empty result; an exception in
`OnPresent` sets `g_failed` (log `[LightmapSmooth] Out of memory: smoothed light map off`). The worker is detached on
purpose: a joinable `std::thread` would terminate the game at exit.

**Light level.** The smoothed map keeps the luma of the game map and only blurs the colour ratio; the B-spline lowers a
smooth peak by about h^2/(6 sigma^2) (h = 1 texel, pools 10 to 25 m wide: under 1%); very dim texels lose their RGB565
tint; dithering is +-0.5 LSB. The plain copy's 8-bit decode differs from the float decode by under 1 LSB.

**Unreadable maps** (not a managed 256x256 DXT5): `hash = 1` (not retried until the pointer changes); the older smoothed
map is not shown and its texture goes to the pool. Log: `[LightmapSmooth] Map (x, z) unreadable: WxH format F pool P
levels L (old smoothed map dropped)`. The atlas cell keeps the last map read.

### Rebuild sweep (driven by the terrain relight)

`NightTerrainRelight` calls `NoteKick(reason)` when it arms a rebuild or starts a paced sweep (60 boosted frames),
`ExpectRebuild(30)` every frame while a rebuild is coming (load waiting for the world, phase delay, a user-driven or
forced lamp change not taken by the local relight, kick armed and not consumed) and `OnTerrainRebuilt()` in the frame
the game consumes it: every chunk becomes `awaiting` (except those already re-rendered in that frame), 300 boosted
frames. A consumed rebuild re-renders one chunk per frame, so jobs wait per chunk until it changed. The sweep ends when
no chunk is awaiting or after 300 frames; the developer log prints `[LightmapSmooth] Rebuild sweep done: kick -> rebuild
X ms, rebuild -> first chunk Y ms, -> last chunk Z ms (N chunks changed, M not re-rendered)`.

### Chunk re-render notice (call site 0x00C8504C)

The terrain update's per-chunk loop re-renders one chunk's composited textures per call when chunk+0x54 is set:
`0x00C85041 cmp byte [esi+54h],0; jz; 0x00C85047 push 0; push esi; mov ecx,edi; call 0x00C7E7A0`. `FUN_00C7E7A0` is
`void __thiscall(terrain, chunk, char force)`, `RET 8` (`re/out/dump/asm/00c7e7a0.asm`, `fn_00c7e7a0.c`); its full
render path ends with `mov byte [esi+54h],0` at 0x00C7E978, the only write of +0x54 in it. `NightTerrainRelight` checks
the 15 bytes at 0x00C85047 (`6A 00 56 8B CF E8 4F 97 FF FF C6 44 24 0C 01`) and redirects only this CALL to
`ChunkRenderThunk` (`__fastcall` with two stack arguments = the same stack contract). After the original returns, if
+0x54 is 0, it reports `(chunk+0x0C >> 8, chunk+0x10 >> 8)` to `LightmapSmooth::NoteChunkRendered` and passes the QPC
time to `ChunkRelight::OnChunkRendered`. The other callers (0x00C8088E, 0x00C8307E) are left alone. If the bytes differ,
a warning is logged and detection falls back to hashing. Counter: "Chunk re-render notices".

### The world atlas

| Property | Value |
|---|---|
| Texels per chunk | 512 (`kAtlasPerChunk` = kOut/2): 2 texels per metre |
| Size | (W x 512) x (H x 512), W/H = chunk span + 1 chunk margin each side; max 16 chunks per axis (8192 texels), else no atlas ("unusually large world") |
| Format | A8R8G8B8, 1 level, `D3DUSAGE_RENDERTARGET`, `D3DPOOL_DEFAULT`; a new atlas is cleared with `ColorFill` to ARGB(255,0,0,0) |
| Fill | CPU path: per chunk, 512x512 SYSTEMMEM staging (ring of 3) + `UpdateSurface` at (ix x 512, iz x 512): first a plain 2x copy, then the smoothed mip 1 (`atlasHash` / `atlasSmooth` per entry). GPU path: `DownPS` from the chunk's level 0 into the cell (`WriteCell`; `gAtlasSig` says which build the cell holds) |
| Growth | A chunk outside the rectangle recreates the atlas larger (union of old and new, +1 margin); the old contents are copied on the GPU (`StretchRect` RT -> RT, same size, `D3DTEXF_NONE`) at their new offset, so no valid cell goes black. If the copy fails, every cell is refilled (log `World map grown to WxH chunks (...)`) |
| Mapping | `LightmapSmooth::Atlas(c)`: uv = world.xz x c.xy + c.zw, c.x = 1/(W x 256), c.y = 1/(H x 256), c.z = -minX x 256 x c.x, c.w = -minZ x 256 x c.y. nullptr until at least one chunk was copied |

Consumers bind it with `SamplerBind` (CLAMP, LINEAR min/mag, mip NONE) or, for the lot passes, LINEAR mip on s2 / s12.
`SamplerBind` sets and restores only the texture and sampler states that differ.

| Consumer | Handler | Coordinate | Page |
|---|---|---|---|
| Summer lot grass | lot branch of `OnDrawInnerCore` | VS c14 rewritten to the atlas mapping | [lot-light-pass](lot-light-pass.md) |
| Snowy lot grass | `DrawLotSnow` | VS c15 rewritten | [snow](snow.md) |
| Winter floors, pool edge | `DrawFloor` | TEXCOORD0.zw = world xz | [floors](floors.md) |
| Summer outdoor floors | `DrawFloorAtlas` | VS copy writes world xz to the first free TEXCOORD >= 7 | [floors](floors.md) |
| Snow on floors, door sills | `DrawSnowFloor` | world xz / 2 (scale c.xy x 2) | [snow](snow.md) |
| Snow on fence tops | `DrawSnowCover` | TEXCOORD3.xy | [snow](snow.md) |
| Snow on stair tops | `DrawSnowRelief` | TEXCOORD4.zw = world xz / 2 (x 2) | [snow](snow.md) |
| Fences, railings, stairs | `DrawInstanced` | TEXCOORD1.zw | [fences](fences.md) |
| Outdoor rig objects | `DrawObjectLamp` | TEXCOORD8 (VS copy) | [objects-and-rigs](objects-and-rigs.md) |

Smoothed per-chunk maps (not the atlas) are used by the world terrain draw, roads (`LightmapSmooth::Find`, the road's
own copy and the extra sampler) and the lot passes when the atlas is not ready (`ChunkTexture`). `floor_atlas_table.h`
(261 ExteriorFloors PS read the atlas through `PatchBakedAtlasPs`) is the largest consumer by shader count.

### Lifecycle

- New world: `NightTerrainRelight::OnPresent` sees a new cells pointer -> `LotLightBridge::OnWorldChanged` ->
  `ClearChunks` -> `LightmapSmooth::Clear` (jobs, results, shared maps, entries, atlas, pool and staging released).
- Device reset: `LightmapSmooth::OnPreReset` (`RenderCallbacks::preReset`) releases the atlas, the pool and all DEFAULT
  textures and marks every entry dirty. SYSTEMMEM staging survives. GPU path: also chunk render targets, scratch and
  queries.
- Path switch (developer A/B or GPU failure, `ResolveMode` at Present): `Clear()`; the draws register the chunks again.
- Bridge hooks unregistered or bridge off: `ClearChunks`.
- Reinstall after a developer option change (`ReinstallNow`, render thread): `LotLightBridge::Shutdown(true)` keeps
  chunk maps, smoothed maps and atlas; a real uninstall releases them.

### Facts relied on

| Fact | Evidence |
|---|---|
| Chunk light map = 256x256 DXT5, 4 mips (world file and rebuilt: the rebuild reads the render target back and CPU-encodes 4 mips into the same texture, 0x00C29AB6 -> 0x00618CD0); MANAGED pool | `RecordWorldChunk` comment; captures; research\perf2\chunkrelight.md 1.3 |
| Chunk = 256 m, uv = (pos - centre)/256 + 0.5, centre in VS c8.w / c10.w | notes section 1 |
| The terrain map is `FUN_00C292B0` ("staticTerrainLightmap") output, chunk+0xD8 ("Terrain/LightmapTexture") | [engine/terrain-and-light-bake.md](../../engine/terrain-and-light-bake.md) |
| Light-map alpha = sun visibility, read by both captured world variants | F7 2026-10-04 captures |

### Files and functions

| File | Function | Role |
|---|---|---|
| lightmap_smooth.cpp | `DecodeDxt5`, `ReadMap`, `HashRows`, `SigOf` | Read the game map |
| | `CheckEntry`, `MarkBordersAround`, `MarkUnreadable`, `NoteChange`, `EndSweepIfDone` | Change detection, sweep |
| | `Process`, `BSplineWeights`, `WorkerMain`, `EnsureWorker` | CPU smoothing |
| | `Upload`, `EnsureAtlas`, `AtlasUpload`, `AtlasRawCopy`, `DecodeDxt5Argb`, `Blend4`, `LockAtlasStage`, `ReleaseAtlas`, `ChunkIndex`, `PoolPut` | Uploads and atlas |
| | `InitGpu`, `RtFormatOk`, `EnsureGpuRes`, `ReleaseGpuRes`, `ReleaseGpuChunks`, `ResolveMode`, `GpuRuntimeFailure` | GPU setup, fallback |
| | `GpuService`, `RunBatch`, `BuildOne`, `WriteCell`, `PassState`, `DrawRect`, `GpuSafe` | GPU passes |
| | `DrainNotices`, `BumpVersion`, `GpuHashCheck`, `HashMap`, `GpuSig`, `OnPresentGpu` | GPU change detection |
| | `BeginTiming`, `EndTiming`, `ReadTimings`, `RunCompare` | GPU timing, compare |
| | `LightmapSmooth::Get / Find / Atlas / OnPresent / OnPreReset / Clear / Status / SetEnabled / Enabled / SetGpuPreferred / GpuActive / RequestCompare / CompareStatus / NoteKick / OnTerrainRebuilt / ExpectRebuild / NoteChunkRendered` | API |
| lot_light_bridge.cpp | `RecordWorldChunk`, `ChunkTexture`, `ClearChunks`, `SamplerBind`, `ChunkCount`, `TerrainLampConst`, `TerrainConst`, `g_terrainAlphaPs`, `g_terrainDayPs` | Registration, world terrain draw |
| native_terrain_sampler.h | `NativeTerrainSampler` | Spare-sampler alias of the native map |
| shader_patches.cpp | `LightMapScaleConst`, `PatchTerrainNativeAlpha`, `PatchTerrainDaylightRange` | Terrain bytecode |
| night_terrain_relight_patch.cpp | `ChunkRenderThunk` at 0x00C8504C; `RenderCallbacks::Add(preReset, OnPreReset)`; developer A/B and compare | Driver |

## Rejected approaches

- Keeping the old smoothed map until the new one is ready, and clearing the atlas black on growth: black or stale ground
  light while panning.
- Re-smoothing on streaming churn: 12 to 60 ms hitches.
- Unbounded result queue and per-chunk float buffers: out of memory in the 32-bit process.
- `D3DUSAGE_AUTOGENMIPMAP`: driver-defined filter.
- Smoothing the light-map alpha with the lamp RGB: changes solar shading.

Details in [history](../../history/night-lighting-world-atlas-and-smoothed-maps.md).

## See also

- [Validation](../../validation/night-lighting-world-atlas-and-smoothed-maps.md)
- [History](../../history/night-lighting-world-atlas-and-smoothed-maps.md)
- [Terrain relight](terrain-relight.md), [lot light pass](lot-light-pass.md)
