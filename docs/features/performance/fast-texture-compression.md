# Faster Texture Compression

Fewer hitches when the game builds terrain, Sim, lot-view and thumbnail textures while you play. The game compresses
those textures on the CPU with slow DXT1 / DXT5 encoders. Faster Texture Compression runs the same algorithm on four
blocks at once, optionally on several processor cores, and produces exactly the same bytes as the game.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier; on by default since 2.5.5 |
| Default | On; *Use several cores* on |
| Menu | System > Performance > Textures and Sims > *Faster texture compression* |
| Configuration | `[patches.FastTextureCompression]` in `ApexRadiance.toml` |
| Source | [`features/dxt_codec.{h,cpp}`](../../../features/dxt_codec.cpp), [`features/fast_dxt.{h,cpp}`](../../../features/fast_dxt.cpp), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

The game's CPU encoders spend roughly 1,200 to 1,500 cycles per 4x4 block: two MMX / SSE passes with store-forwarding,
a divide, x87 code and a per-pixel horizontal dot product. Large textures built while playing (lot impostors, packed
terrain normal maps, CAS composited textures, thumbnails, mip chains) take tens of milliseconds on the calling thread.
In the Frame Profiler, "DXT encode" is a dominant counter of the largest hitches (50 ms and more).

## How Apex Radiance solves it

1. **Same algorithm, four blocks at once.** The encoder entries are replaced by a structure-of-arrays SSE version: pixel
   k of blocks 0 to 3 in one register per channel. Every float operation is the same IEEE operation on the same values
   in the same order as the game's, so every block gets the game's bytes.
2. **Fallback for odd blocks.** A block whose axis, endpoints, 1/range or errors are not finite is encoded by the game's
   own function.
3. **Several cores** (optional). Images of 256 x 256 and more are cut into rows of blocks shared between the calling
   thread and up to six worker threads, each running with the caller's floating-point state. The call still returns the
   finished texture.
4. **Checked.** The first 16 textures of every session are also encoded by the game and compared.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Faster texture compression | `[patches.FastTextureCompression] enabled` | bool | on | | Replaces the encoders. A missing key reads as on |
| Use several cores (shown while on) | `[patches.FastTextureCompression] useSeveralCores` | bool | on | | Large textures are shared out over several cores, with the same result. Off = every texture on the calling thread. Applied to the next texture (the hook reads it on every call; `Update` clears the reinstall request). The key is never renamed |
| Developer > *Check 1 texture in N against the game* | `[developer.controls.fast_dxt] verify_every` | int | 64 in developer mode, 0 otherwise | 0 to 64 (1,024 when loaded) | After the first 16 textures, also encode 1 in N with the game and compare. Each check runs the game encoder too, so a low N makes rebuild frames slower |
| Developer > *Check every texture for 30 s* | (not saved) | action | | | |
| Developer > *Worker threads per texture (0 = one core)* | `[developer.controls.fast_dxt] workers` | int | the maximum | 0 to min(logical processors - 2, 6) | |
| Developer > *Split textures from (side, pixels)* | `[developer.controls.fast_dxt] min_side` | int | 256 | 32 to 2048 | |

## Compatibility and interactions

- **Frame Profiler:** its "DXT encode" counter is the outer layer of the entry chain and times whichever encoder runs.
  A texture checked against the game is timed with both encoders inside the counter. Its Hooks table says "the fast
  encoder is inside".
- **Official Sims3SettingsSetter:** nothing in it touches the DXT encoders.
- Requires SSE2.

## Limitations

- The approximate instructions `rcpss` / `rsqrtss` have CPU-specific exact bits. On a CPU with two core types (for
  example Intel P / E cores) they are assumed to give the same bits on both. The serial path already assumed this
  (Windows moves the calling thread between cores); workers make it more frequent. Unverified. A difference would show
  as a "Verification mismatch" and the feature turns itself off. *Use several cores* off, or the worker slider at 0,
  removes the extra exposure.
- The encoder constants are assumed equal to Steam 1.67.2's on other builds; the per-session checks catch a difference
  on the first textures.
- Images below the split size, and every level of a mip chain below it, stay on one core. A large image that arrives
  while another thread's image has the worker pool is encoded on the calling thread.

## Technical reference

### The game side (Steam 1.67.2)

**Drivers.** 0x006152F0 (DXT1) and 0x006154B0 (DXT5), `cdecl(Dst*, Src*)`, both `ret` with `eax = width & ~3` (no caller
reads it). `Dst` = {+0 first block, +4 width, +8 height, +0xC bytes per row of blocks}; `Src` = {+0 32-bit pixels
B,G,R,A, +0xC bytes per pixel row, +0x10 format}. The DXT5 driver does nothing unless `Src.format` is 0x3D or 0x3E; the
DXT1 driver never reads it. Blocks in raster order; full blocks use the fetch 0x00614000, blocks of the last column
(width not a multiple of 4) or the last row use 0x00614C50(src, pitch, cols, rows). DXT1: colour block 0x006143F0(out,
1); DXT5: alpha block 0x00614150(alpha[16], out) then 0x006143F0(out + 8, 0). The helpers have no other callers.

**Callers** (8 DXT1, 7 DXT5, several threads): 0x005FBBB0 (CAS composited textures, switch on the format), 0x00618600 /
0x00618930 ("Services/ImgData"), 0x009DC8D0 ("VideoRecording/SceneCaptureTexture"), 0x009DD250
("SceneCaptureManager/PackedImage", a mip loop), 0x00ADE030 ("LotLODCreator/LODTexture", lot impostor textures, DXT5),
0x00C25C30 ("TerrainBuilder/PackedNormalMap", a mip loop, DXT5), 0x00D523E0 ("UI/ThumbnailManager/PackedImage", mip
loop, DXT1), 0x00D52FF0, 0x00D653A0 (mip loops). Sizes are whatever those images are (mips down to 1x1).

**Colour block, step by step** (a 16-byte-aligned object: axis at +0, 16 pixels at +0x10, endpoints at +0x110 / +0x120):

1. *Fetch.* Each pixel becomes {R/256, G/256, B/256, L} with L = 0.59f·G' + (0.11f·B' + 0.3f·R') (single precision, that
   order). The full fetch gets x/256 with float bit tricks (mask + or + add; R's bit 7 through the exponent), the partial
   fetch with `cvtsi2ss` x 1/256: both exact, identical. Partial blocks repeat each row's last pixel to 4 columns, then
   the last row to 4 rows.
2. *Moments* (0x00615100): sums over the 16 pixels in order of x, x², x·L (4 lanes); mean = sum/16; var = sum(x²)/16 −
   mean². total = (varL + varG) + (varB + varR).
3. *Axis.* total < 1e-5 -> "solid": axis {1,1,1,0}, both endpoints = mean. Else if varL > 1e-5: axis = cov(RGB, L)
   (= sum(x·L)/16 − meanL·mean), lane 3 masked, x `rsqrtss` of (covB² + covR²) + covG² (no Newton step). Else (flat
   luma, colours vary): 0x00614DD0, power iteration: covariance matrix of R,G,B (off-diagonals from the sums of G·R, B·G,
   R·B, sums/256), scaled by max(1, NR(rsqrtps(|var|²))), start = the row of the largest variance (ties go to G then B),
   16 iterations of w = M·v, stop with the luma weights {0.3, 0.59, 0.11} when |w| ≤ 2^-23 in all three lanes, else
   v = w x (rsqrtps + 2 Newton steps).
4. *Endpoints.* For each pixel t = (dB + dR) + dG with d = (x − mean)·axis; mn = `minps`(0, t), mx = `maxps`(0, t);
   ep0 = `rcpss`(Σmx) · Σ(−mn·x), ep1 = `rcpss`(−Σmn) · Σ(mx·x) (4 lanes; the two weights are equal in exact arithmetic,
   the game uses them crossed). If (ddB + ddR) + (ddL + ddG) < 0.004444 (ep1 − ep0 squared), both move apart by
   (ep1 − ep0) · `rsqrtss`(d²) · (1/31).
5. *565.* channel · 248/255 (G: 252/255) + 1.5·2^18 (G: 1.5·2^17), the float's bits minus the magic = the rounded
   integer; clamp to 0..31 / 0..63 with a byte trick. Equal colours -> {c, c, 0, 0}.
6. *Projection range.* Both colours decoded (x 1/31, 1/63) and projected: p = (x3 + x1) + (x2 + x0) with x = e·axis;
   lo = min, hi = max (lo = p0 unless p0 > p1). **x87:** `fld hi; fsub lo; fabs; fcomip 1e-5` -> if 1e-5 > |hi − lo|
   then hi += 1 (the only x87 code; its precision is the thread's x87 control word: 24-bit on the Direct3D device thread,
   53-bit elsewhere). range = hi − lo; inv = 1/range (`divss`).
7. *Ordered dither.* Strength d = clamp((0.125 − range)·16, 0, 1) by sign bits; pixel k adds dither[k]·d, dither = the
   4x4 Bayer matrix/16 − 0.5 at 0x00FE8238.
8. *Indices.* v4 = (p − lo)·(inv·3) + dither·d, code = round(v4) via + 1.5·2^23, clamped 0..3; error4 += (code − v4)² in
   pixel order. DXT1 also: v3 with inv·2, 0..2, error3. DXT1 picks 3 colours when error4 > error3·2.25 (DXT5 never).
9. *Packing.* 4 colours: rank 0..3 -> code 0, 2, 3, 1; colour0 must be > colour1, else swap and xor 0x5555. 3 colours:
   codes 1 and 2 swapped; colour0 must be ≤ colour1, else swap and codes 0 and 1 swapped. (The index assignment assumes
   p0 ≤ p1; when quantisation reverses them the game still does this, and so does Apex.)

**Alpha block** (0x00614150, MMX): extremes = 0 or 255. No extreme in the block: 8-alpha mode, a0 = max, a1 = min.
Otherwise 6-alpha mode: a0 = min over the non-zero values, a1 = max over the values that are neither 0 nor 255 (none:
a0 = 63, a1 = 192), extremes get codes 6 / 7. Code = min(K, (max(0, pmulhw((a − min)·32, round(K·4096/range))) + 1) >>
1), K = 7 or 5, then reversed in 8-alpha mode and remapped (rank 0 -> 0, K -> 1, others +1).

**Edge alpha quirk (kept on purpose).** The alpha fetch of right-edge blocks (0x00613F80) has an empty column-fill loop,
so rows are packed `cols` apart and pixels get another pixel's alpha code (only images whose width is not a multiple of
4). For a 1 to 3 pixel block in the last row (rows x cols < 4) its final fill (`rep movsd` from 16 bytes back) starts
before the array and copies the DXT5 driver's four locals that precede it (array at the driver's `esp+40h`):
`[esp+30h]` = the block's source pointer (stored at 0x0061561E just before the call), `[esp+34h]` = y, `[esp+38h]` =
height, `[esp+3Ch]` = the destination row padding. Those dwords go through the same saturations (words, then bytes) as
alpha values. `FetchAlphaPartial` models them exactly (`DriverLocals`); the fast path uses the scalar alpha translation
when such a value is not a byte, and recomputes the alpha half of blocks it hands to the game's function (whose
one-block image has other locals). A "fix" would change the output of every image whose width is not a multiple of 4.

**Constants** (all from `.rdata`, exact bit patterns in `features/dxt_codec.cpp`): 1e-5 (0x00FE82C0, 0x00F9D2C8),
0.004444 (0x00FE8288), 1/31 (0x00FE8284), 1/63 (0x00FAD528), 1/16, 1/256, 248/255, 252/255, 1.5·2^18, 1.5·2^17,
1.5·2^23, 0.125, 16, 3, 2, 2.25, FLT_MAX, −1, 0.5, 1, 2^-23, the luma weights and the dither matrix.

### The replacement

- `DxtCodec::Ref`: a literal translation (same instruction sequence with intrinsics, addresses in comments). It is the
  offline test oracle.
- `DxtCodec::Fast`: four blocks per SSE register. Blocks are gathered 4 at a time in raster order (a group can span
  rows); edge blocks get the game's replicated pixels; the pixel bytes are transposed and converted exactly. All sums,
  moments, projections and the index loop run on the four blocks at once; the DXT5 alpha block is SSE2 on all 16 pixels.
- **Why the bytes are identical:** add, subtract, multiply and divide are correctly rounded, so for finite values the
  result does not depend on the SSE lane or on scalar vs packed; commutative operand swaps change nothing for finite
  values; `minps` / `maxps` keep the game's operand order (signed zeros); `rcpss` / `rsqrtss` run per block as the same
  scalar instruction on the same input (the power iteration uses `rsqrtps` like the game); the x87 range test runs the
  game's exact x87 sequence (inline assembly) under the calling thread's control word; MXCSR (rounding, FTZ/DAZ) is the
  calling thread's in both. The one thing that could differ is NaN payloads, so a block whose axis, endpoints, 1/range
  or errors are not finite is encoded by the game's own function (`GameBlock`: a one-block image at the block's pixels,
  which the game's drivers fetch exactly like the same block inside the whole image).
- **Hook:** the entries through `EntryChain` (sites `DxtEncode1/5`, prologue `55 8B EC 83 E4 F0` to a trampoline, JMP
  written with every other thread suspended); layer 0 = Frame Profiler (times every call), layer 1 = `FastDxt`; the
  trampoline is the game's function.
- **Checks:** the first 16 textures of every session, and in developer mode 1 in N afterwards, are also encoded by the
  game into a scratch buffer and compared. A difference logs `[FastDxt] Verification mismatch: ...` (block, both byte
  strings, the pixels), the game's bytes are used, and the feature turns itself off for the session.
- **Expected speed (inferred):** the fast path costs about a quarter of the game's cycles per block (the per-pixel work
  is vertical over four blocks; the scalar parts are 4 rcp/rsqrt, 1 x87 test and 1 divide per block), i.e. 3 to 5x on
  the encoder.

### Several cores (`DxtCodec::Parallel`)

- An image with width x height ≥ 256 x 256 is cut into chunks of whole rows of blocks (about 4 chunks per thread, at
  least 128 blocks each). The calling thread and up to N pool threads take chunks from an atomic counter until none is
  left; the hook returns only when every chunk is written. N = min(logical processors of the process's affinity mask −
  2, 6); 0 on 1 to 2 processor machines. Smaller images, and a large image arriving while another thread's image has
  the pool, are encoded on the calling thread (the "busy" counter); nothing ever waits for the pool.
- **Why the bytes match the serial fast path:**
  1. *Per-block inputs are unchanged.* A chunk is encoded by `FastEncodeRows(dst, src, rowBegin, rowEnd)` with the whole
     image's descriptors and absolute rows of blocks, so each block's job is the one the serial loop builds: same source
     pointer (`src + pitch * 4 * row`), cols / rows, output pointer, and the DXT5 driver locals that the edge-alpha quirk
     copies (the block's source pointer, y = absolute pixel row, height = the whole image's, padding = `dst.pitch −
     roundup4(width) * 4`). Serial `Fast::EncodeDxt1/5` is `FastEncodeRows(0, all rows)`, one code path.
  2. *Grouping does not matter.* Chunk ends change which blocks share a group of four (and add padding lanes); the fast
     path has no horizontal arithmetic between lanes, so a block's bytes do not depend on its lane or its neighbours.
     The flat-luma / solid counters count padding lanes, so those two statistics can differ slightly; blocks and
     "encoded by the game's function" do not.
  3. *Same FP state.* The caller reads its x87 control word (`fnstcw`) and MXCSR; each worker saves its own, clears
     pending x87 exceptions, loads the caller's (`fldcw`, `ldmxcsr`, status flags cleared), reads them back (a mismatch
     is counted), encodes, and restores its own.
  4. *No shared mutable state:* each chunk writes disjoint output bytes (row padding is never written); the source is
     only read; the fallback keeps everything on its stack and is already called by the game from several threads.
- **Pool:** created once (the first large image, or at Start / when the option is turned on), never destroyed (threads
  sleep in `WaitForSingleObject` on their own auto-reset event and die with the process; nothing to tear down at exit or
  in `DllMain`). Hand-off: `state` = closed flag | threads checked in. The caller owns the pool (`inUse`), writes the
  task while closed, opens it, wakes k = min(N, chunks − 1) workers, takes chunks itself, closes, then waits (short spin,
  then the `done` event, re-checking the count) until no thread is checked in. A worker that wakes late sees "closed"
  and leaves without reading the task, or joins the next image legitimately. Stack reservation 256 KB per worker (1.5 MB
  of address space for 6). Thread name "Apex DXT worker" (`SetThreadDescription` when present).
- **Priority: normal, on purpose.** The calling thread is blocked on the texture, so below-normal workers could only be
  delayed by the game's other normal-priority threads. The work is bounded and two logical processors stay out of the
  pool.
- **Checks:** checked images are encoded by the game into scratch and by Apex (parallel when large) into the
  destination, then every row of blocks is compared. A worker whose FP state did not read back as the caller's is
  counted, the whole image is encoded again on the calling thread, and several cores stay off until the feature is
  started again (`[FastDxt] A DXT worker thread did not take ...`).

### Address reference

| Id (`GameAddr`) | Steam 1.67.2 | Kind |
|---|---|---|
| DxtEncode1 / DxtEncode5 | 0x006152F0 / 0x006154B0 | Sig (entry) |

Group `FastTextureCompression`. Run-time checks: the entries' prologue `55 8B EC 83 E4 F0` (or the entry chain's own
JMP), and the first textures of each session compared with the game.

### Source

| File | Symbols |
|---|---|
| `features/dxt_codec.{h,cpp}` | `Ref::EncodeDxt1/5`, `Ref::EncodeBlock`, `Fast::EncodeDxt1/5`, `FastEncodeRows`, `FetchFull` / `FetchPartial` / `FetchAlphaPartial` / `Endpoints` / `PowerAxis` / `EncodeColor` / `EncodeAlpha` (the game's helpers), `EncodeColorGroup` / `EncodeAlphaFast` / `FlushGroup` (fast), `X87RangeBelowEps`, `CpuHasSse2`, `Parallel` (pure; also built by `tools/dxt_test`) |
| `features/fast_dxt.{h,cpp}` | `Start`, `Stop`, `Hook_Dxt1/5` -> `Encode`, `RunFast` (parallel or serial), `GameBlock` (fallback), `SetSeveralCores`, `Checked`, statistics, `RenderDeveloperUI` |

Developer card *Texture compression and processor cores*: textures, blocks (flat-luma, solid, encoded by the game's
function), time, "checked textures: game X ms, Apex Y ms (Zx)", CPU features, checks and the last difference; several
cores: state, worker threads created, split textures (chunks, % taken by workers, wall ms), "about X ms saved" (summed
chunk time minus wall time; it slightly overstates when the cores slow each other down), textures on one core because
another texture had the workers, FP state mismatches (must be 0).

### Rules for maintainers

- `dxt_codec.cpp` must be built with legacy SSE (`/arch:SSE2`, the x86 default; it refuses `/arch:AVX`) and without
  `/fp:fast` (it forces `float_control(precise)` and `fp_contract(off)`). Do not reorder its arithmetic, replace
  `rcpss` / `rsqrtss` by divisions, use `rcpps` / `rsqrtps` on four blocks, or replace the x87 inline assembly.
- Never encode a chunk as a sub-image with its own pointer or height: the DXT5 edge-alpha quirk reads y, height, the
  source pointer and the padding. Always `FastEncodeRows` with the whole image's descriptors.
- Never let a worker encode without the caller's x87 control word and MXCSR. Keep the call synchronous and never wait
  for the pool.
- Do not hook the DXT entries outside `EntryChain`.

## Rejected approaches

- Splitting a texture into sub-images for the workers: changes the edge-alpha bytes. See [history](../../history/performance-fast-texture-compression.md).
- An AVX2 path with 8 blocks per register: needs proof that the VEX forms of `rcpss` / `rsqrtss` match the legacy ones;
  not done.

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-fast-texture-compression.md)
- [History](../../history/performance-fast-texture-compression.md)
