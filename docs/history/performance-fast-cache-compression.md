# Faster Cache Compression: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/fast-cache-compression.md](../features/performance/fast-cache-compression.md).

### 2026-09-29: bounded RefPack compressor (plan candidate C4)

**Context:** the game's RefPack compressor (0x004EC0A0, through the stream write 0x004EC200) allocates a 256 KB hash
table and clears it on every call, and follows whole hash chains; on repetitive data the chains hold thousands of
candidates per position.

**Outcome:** a compressor with a bounded search (default depth 32, "nice length" 96, chosen without game data) behind
the stream vtable slot. Every stream is checked by decompressing it; in the development build every stream at first.
Streams are a few percent larger than the game's (expected, not yet measured in game).

### 2026-09-30: counting run's stream kept for its write

**Context:** measured in game over 13 sessions: "RefPack compress" was the dominant cause of 81 hitches (10.5 s over
the median, maximum 368 ms). Each was a counting run and its write of the same stream (the CAS SimService and
TextureCompositor caches: 4 calls of about 5.5 MB, about 330 ms, about 65 MB/s).

**Outcome:** commit `bfec2d3`. The counting run compresses into a per-thread buffer with the source's CRC-32C; the
write of the same source, size, flags and depth copies it when the CRC still matches. Large streams are compressed in
128 KB pieces on worker threads.

**In game (build 0d1408ad, the same cache writes before and after, hitch frames only):** the about 21 MB writes (4
calls: 2 counting runs plus their writes) 299 ms (49 cases) to 101 ms (3 cases); about 11 MB 121 to 43 ms; about 5.5 MB
78 to 24 ms (44 / 6 cases); worst compression-dominated hitch 295 to 395 ms down to 135 ms. In game the pieces ran about
2x faster than one thread (3.8x offline), since the game's own threads are busy at those moments. Maintainer feedback:
the result appeared better in play.

### 2026-09-30: development check rate and kept buffers

**Context:** checking every stream in the development build decoded each one through the S3SS decoder, and that decode
was 19% of the compression hitches.

**Outcome:** commit `9eb87e2`: the development build checks 1 stream in 8 after the first 16; per-thread buffers are
kept up to 8 MB. The same day (`ccf1bee`, see the
[Vulkan driver guard history](performance-vulkan-driver-guard.md)) the total kept for all threads was capped at 12 MB.

### 2026-09-30: record checksums

**Context:** the loading research (session notes `loadre\compositor.md`) found the texture compositor cache's record
CRC (FUN_004fa4c0, one table lookup per byte) at 6 to 10% of a 490 ms cache eviction cascade.

**Outcome:** commit `6e34709`: a slicing-by-8 CRC built from the game's own table, started and stopped with this
switch. Offline: 4850 cases, all equal to the byte-wise loop.
