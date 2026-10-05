# Faster Cache Compression

Fewer hitches when the game stores Sims, objects and terrain in its caches and saves. The game compresses those records
with a RefPack compressor that tries every match candidate. Faster Cache Compression answers with a much faster
compressor that writes the game's own stream format, so the game reads the data back unchanged. The same switch also
computes the texture compositor cache's record checksums eight bytes per step, with the game's exact result.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier; record checksums since 2.4.0; on by default since 2.5.5 |
| Default | On |
| Menu | System > Performance > Textures and Sims > *Faster cache compression* |
| Configuration | `[patches.FastCacheCompression]` in `ApexRadiance.toml` |
| Source | [`features/refpack_codec.{h,cpp}`](../../../features/refpack_codec.cpp), [`features/fast_refpack.{h,cpp}`](../../../features/fast_refpack.cpp), [`features/fast_crc.{h,cpp}`](../../../features/fast_crc.cpp), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

The game's RefPack stream write clears a 256 KB hash table and allocates a 512 KB chain array on every call, then tries
every candidate in the chain at every position. On repetitive data (the CAS SimService and TextureCompositor caches)
the chains hold thousands of candidates per position; one 5.5 MB cache write takes about 330 ms, and the package writer
compresses each resource twice (a counting run to size the buffer, then the write). In the Frame Profiler "RefPack
compress" is a dominant counter of large hitches. The record checksum of the compositor cache is a byte-at-a-time
CRC-32 that shows up in cache eviction cascades.

## How Apex Radiance solves it

1. **Same format, bounded search.** A hash-chain compressor with reusable memory tries at most 32 candidates per
   position and writes the same header, window, opcodes and end rules as the game. Any RefPack decoder, the game's
   included, reads it. The bytes differ from the game's.
2. **Counting run kept for its write.** The stream produced by a counting run is kept per thread with a checksum of the
   source; the write that follows copies it instead of compressing again.
3. **Large streams on several cores.** Streams over 128 KB are compressed in pieces on worker threads; the output depends
   only on the input, flags and search depth.
4. **Checked.** The first 16 streams of each session are decompressed with the game's decoder and compared with the
   source.
5. **Record checksums.** The CRC-32 is computed with slicing-by-8 tables derived from the game's own table.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Faster cache compression | `[patches.FastCacheCompression] enabled` | bool | on | | Replaces the stream write and the record checksum. A missing key reads as on |
| Developer > *Check 1 stream in N by decompressing* | `[developer.controls.fast_refpack] verify_every` | int | 8 in developer mode, 0 otherwise | 0 to 64 | After the first 16 streams, also decompress 1 in N and compare |
| Developer > *Also run the game's compressor on 1 stream in N (0 = never; adds its time)* | `[developer.controls.fast_refpack] compare_every` | int | 0 | 0 to 64 | Counting-only comparison of size and time |
| Developer > *Search depth (candidates per position)* | `[developer.controls.fast_refpack] chain_depth` | int | 32 | 4 to 256 | |

## Compatibility and interactions

- **Official Sims3SettingsSetter:** its "RefPack decompressor" replaces the decoder 0x004EB3B0 (it only reads streams).
  The fast compressor's streams use the same format and are checked in game through whatever decoder is installed.
  Nothing in S3SS touches the compressor, the stream vtable or the checksum.
- **Frame Profiler:** its "RefPack compress" counter is the outer layer of the slot chain and times whichever compressor
  runs (a checked stream includes the check). Hitch lines add ", in X KB, out Y KB". The report adds "Faster cache
  compression, record checksums: ...".
- Saved games and caches written with the feature on load normally with it off: the streams are standard RefPack.
- The record checksum part is started and stopped by this switch; a failure to start it only logs `[FastCrc] Not used`.

## Limitations

- Streams are a few percent larger than the game's (bounded search). The package writer stores streams above its ratio
  threshold uncompressed and the memory caches are size-capped: slightly more memory or disk per cached item, slightly
  earlier evictions.
- Unlike the game, the write never goes past `capacity` (when non-zero): a stream that does not fit returns -1 and the
  callers store the data uncompressed.
- The search depth (32) and "nice length" (96) were chosen without data from the game.
- After the switch is turned off, its vtable layer stays for up to 2 s (until the last counting run is 2 s old).
  `FreeLibrary` of the ASI in that window would leave the slot pointing into unloaded code (ASI loaders never unload).

## Technical reference

### The game side (Steam 1.67.2)

- **Stream vtable** 0x00FB9018 (constructor 0x004EBFD0, `[stream+4]` = allocator): +0 destructor, +4 write 0x004EC200,
  +8 read 0x004EC010, +C 0x004EC080 (magic test). Slot +4 (0x00FB901C) is the write's only reference.
- **0x004EC200** thiscall(src, size, dst, capacity, flags), ret 14h. mode = 1 when flags & 2, 2 when flags & 0x10000,
  else 0. dst == 0 and flags & 1: returns ((size x 20) >> 4) + 32 (no work). Otherwise 0x004EC0A0(dst, capacity, src,
  size, allocator, mode); with dst == 0 it compresses without writing and returns the size (a "counting run").
  `capacity` is never read; the call never fails.
- **0x004EC0A0.** Header word 0x10FB (0x90FB and a 4-byte size when size ≥ 0x1000000), | 0x4000 and window 0x3FFF unless
  mode & 1 (window 0x1FFFF, no 0x4000); then the size, big-endian, 3 or 4 bytes. size ≤ 0x4000: 0x004EB750 with a
  256-entry table (1 KB `_alloca`); larger: 0x004EBB90 with a 64K-entry table (256 KB from the allocator). Both also
  allocate the chain array (window + 1) x 4 bytes (512 KB for the 128 KB window) and free both at the end. Returns
  payload + 2 + size bytes.
- **0x004EB750 / 0x004EBB90** (identical but for the hash): `memset(table, -1, ...)` every call (256 KB); hash of 3
  bytes (`b0 ^ b1 ^ b2`, or `(b0 << 8 | b2) ^ (b1 << 4)`); chain[pos & window] = previous head. For every position every
  candidate of the chain inside the window is tried (quick reject on the byte at the current best length, then a byte
  loop), best = largest (length − opcode bytes), stop only at a 1028-byte match. Opcode cost 2 when offset ≤ 1024 and
  length ≤ 10, 3 when offset ≤ 16384 and length ≤ 67, else 4. Matches never reach the last 4 bytes. Mode 2 inserts only
  the first position of each match, otherwise every position. Literal runs of 4..112, stop opcode 0xFC + 0..3.
- **Callers.** MemoryDB commit 0x0072B0A0 (the Sim / object compositor caches, terrain world caches): size query
  (flags 1), buffer of that size, write with flags 2 (128 KB window); -1 -> store uncompressed. Package writer 0x004A7030
  ("ResourceLoad/PackedFile/CompressionRefpack", saves and writable caches, entries 0x32..16 MB, called from 0x004A82C0):
  a size query, or counting runs (flags 2, no destination) of each registered compressor to choose one and size the
  buffer, then the write with flags 1, 0x10001 or 2 (by the package's mode) into that buffer; -1 -> the resource is
  written uncompressed (0x004A82C0 also drops streams whose ratio is above its threshold).
- **Decompressor 0x004EB3B0** cdecl(dst, capacity, src, srcSize): header flags 0x8000 (4-byte sizes) and 0x100 (a
  compressed-size field to skip), ignores 0x4000; checks every literal and copy against the capacity and the source
  length, every match offset against the start of dst; needs the stop opcode; returns the header's size or 0. It is the
  only RefPack decoder in the exe (the stream read 0x004EC010, magic `(word & 0x1FFF) == 0x10FB`). Apex never patches
  it; it only calls it to check its own streams.

### The replacement

- **Stream format = the game's:** same header for the same size and flags, same window per flags (offsets ≤ 0x3FFF or
  0x1FFFF), the same four opcode forms, the same "matches stop 4 bytes before the end" rule, same literal runs and stop
  opcode.
- **Search:** 4-byte multiplicative hash, 64K heads + 64K chain links (512 KB per context, reused: positions are stored
  as base + index and older entries fall below the base, so the table is never cleared per call); up to 32 chain
  candidates per position (developer slider 4 to 256), stop at 96 bytes; the game's cost / gain rule; one step of lazy
  matching; all positions of short matches inserted, a sample of long ones. Match lengths compared 16 bytes at a time.
- **Memory:** a pool of 4 contexts (allocated on first use, never freed: 512 KB each plus their token buffers, about
  3.5 MB); a 5th simultaneous call gets a temporary context. Check buffers up to 1 MB are kept per context.
- **Counting runs:** answered by the fast compressor too, and the thread remembers (source, size, flags, search depth,
  which compressor). The write that follows uses the same compressor, even if the feature was switched meanwhile, so a
  buffer sized by one compressor is never filled by the other. The fast compressor writes it with the counted flags and
  depth, so the stream has exactly the counted size. (The package writer counts with flags 2 but writes with the
  package's flags 1 / 0x10001 / 2; with the game's compressor a smaller window could make the write longer than its
  buffer, inferred from 0x004A7030, not observed.) The header then says 128 KB window where the package asked for 16 KB:
  every RefPack decoder accepts it, the game's ignores that bit. When the feature is switched off, its vtable layer
  stays until the last counting run is 2 s old (`FastRefPack::Tick` from the patch's `Update`).
- **Counting-run pairing:** per thread, matched on (source pointer, size). If the game ever wrote a counted stream from
  another thread, the write would use the current compressor and could return -1 (safe: stored uncompressed), never
  overflow (the fast compressor checks the capacity; the game's is only used when the capacity can hold its bound or the
  counting run was the game's).
- **Kept stream:** the counting run compresses into a buffer of its thread (`Held`, SizeBound bytes) with the source's
  CRC-32C (SSE4.2 `crc32`, 4 interleaved lanes, about 0.3 ms per 5 MB). The write of the same source, size, flags and
  depth copies it when the CRC still matches (else compresses again; without SSE4.2 it is never copied). Buffers up to
  8 MB are kept per thread (`kKeepHeld`), at most 12 MB for all threads together (`kKeepHeldTotal`); larger ones are
  freed after the write. The write's checks (decompress and compare) are unchanged.
- **Segmented compressor:** a stream over one piece of `kSegmentBytes` (128 KB) is parsed piece by piece
  (`ParseSegment` / `SegmentEncoder`), each piece with the hash chains first filled from the window before it and no
  match past its end, then written in order (literal runs continue across pieces). The pool (`ParPool`, the texture
  encoder's hand-off protocol): `DxtCodec::Parallel::DefaultWorkers()` threads (min(processors − 2, 6)), rounds of 2
  pieces per thread; the caller parses too and writes each round in order. Memory: 512 KB per worker context and 14
  token buffers of 350 KB (on first use, never freed; about 8 MB at most on 8+ processors). Worker stack reservation
  256 KB.
- **Checks:** the first 16 streams of each session, and in developer mode 1 stream in N afterwards, are decompressed
  with the game's decoder (`RefPackDecompress`, else Apex's translation) and compared with the source. A difference logs
  `[FastRefPack] Verification mismatch: ...`, the game's compressor writes that stream (when the destination can hold
  its bound, else -1) and the feature turns itself off for the session.

### Record checksums

- **The game side:** FUN_004fa4c0 cdecl(bytes, length, crc, bool invert), plain `ret`: an MSB-first table CRC-32, one
  lookup per byte (`crc = (crc << 8) ^ T[(crc >> 24) ^ b]`), table 0x0114D330 = the standard 0x04C11DB7 table (static
  data in the file), result inverted when the low byte of the fourth argument is set; nothing read when bytes + length
  wraps. Called 4 times, all from the checksum filter of the texture compositor's cache package (vtable 0x00FE2594:
  verify 0x0072C580 on reads, write 0x0072C610; set up at 0x005BBB8B, 0x005BBF6B and by 0x0072C602).
- **The replacement** (`features/fast_crc.{h,cpp}`, entry chain site `RecordCrc` layer `FastCrc`, GameAddr `RecordCrc`
  + `RecordCrcTable`, group `FastRecordCrc`): slicing by 8 with tables derived from the game's own table read at start
  (T[k][i] = T[k−1][i] advanced by a zero byte), so the value is the same for every input.
- **Checks:** at start the table must be linear (T[0] = 0, every entry the XOR of its bits' entries) and 192 test
  buffers (0 to 70,000 bytes, 8 alignments, seeds 0 / ~0 / random, both inversions) must give the game's own function's
  values. Then the first 16 calls of each session (developer mode: 1 in 64 afterwards) are compared with the game's. A
  difference logs `[FastCrc] Result differs`, the game's value is used and the part turns itself off.

### Address reference

| Id (`GameAddr`) | Steam 1.67.2 | Kind |
|---|---|---|
| RefPackCompress / RefPackCompressSlot | 0x004EC200 / 0x00FB901C | Sig / SlotsOf(1) |
| RefPackDecompress | 0x004EB3B0 | Sig: the CALL in the stream read 0x004EC010 (not the entry, which S3SS detours); optional |
| RecordCrc / RecordCrcTable | 0x004FA4C0 / 0x0114D330 | Sig (the whole function) / Deref (dword at RecordCrc+0x25) |

Groups `FastCacheCompression`, `FastRecordCrc`. Hook: `SlotChain` site `RefPackCompress`, layer `FastCompress` (inside
the profiler). Run-time check: the RefPack slot holds the write (or the slot chain's outer hook).

### Source

| File | Symbols |
|---|---|
| `features/refpack_codec.{h,cpp}` | `Compress` (fast), `Decompress` (0x004EB3B0 translated), `GameCompress` / `GameCore` (0x004EC0A0 + 0x004EB750 / 0x004EBB90 translated), `ParamsFor`, `SizeBound`, `Context`, `kSegmentBytes`, `ParseSegment`, `SegmentEncoder` (pure; also built by `tools/refpack_test`) |
| `features/fast_refpack.{h,cpp}` | `Start`, `Stop`, `Tick`, `Hook_StreamWrite`, the context pool, the per-thread counting-run pairing, `Held`, `ParPool`, `Check`, statistics, `RenderDeveloperUI` |
| `features/fast_crc.{h,cpp}` | The slicing-by-8 CRC, its start checks and status |

Developer card *Compressed game data*: streams, MB in / out, time, counting runs, writes after a counting run, did not
fit, temporary contexts, checks (game's decoder), the comparison with the game's compressor ("size +x%, y.yx faster").

### Rules for maintainers

- Do not swap the RefPack slot outside `SlotChain` (the profiler and the fast compressor would lose each other).
- Never patch the decoder 0x004EB3B0: S3SS detours its entry; Apex reaches it through the CALL in the stream read.

## Rejected approaches

- Checking every stream by decompressing in developer mode: the decode was a large share of the compression hitches;
  the default became 1 in 8. See [history](../../history/performance-fast-cache-compression.md).

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-fast-cache-compression.md)
- [History](../../history/performance-fast-cache-compression.md)
