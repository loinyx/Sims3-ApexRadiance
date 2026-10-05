# Faster Texture Compression: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/fast-texture-compression.md](../features/performance/fast-texture-compression.md).

### 2026-09-29: bit-exact DXT encoder (plan candidate C9)

**Context:** texture compression (DXT1 / DXT5, the CAS composited textures, lot impostors, terrain normal maps,
thumbnails) appeared in 44 to 49 ms hitches. The game spends roughly 1,200 to 1,500 cycles per block (inferred from the
instruction sequence, not measured).

**Finding:** the drivers 0x006152F0 / 0x006154B0 and their helpers were translated instruction by instruction. The
offline harness `tools/dxt_test` found the DXT5 edge-alpha quirk: the partial-width alpha fetch (0x00613F80) has an
empty column-fill loop, so right-edge blocks read rows packed `cols` apart. The quirk is reproduced on purpose so the
output stays bit-identical.

**Outcome:** a four-blocks-per-register SSE path that keeps the game's association order, with blocks whose
intermediate values are not finite encoded by the game's own function. Shipped with startup checks (the first 16
textures of each session) and, in the development build, periodic checks against the game.

### 2026-09-29: several cores

**Context:** large images (1024 x 1024 and 2048 x 2048) still took several milliseconds on one core.

**Finding:** encoding a chunk as a sub-image (own pointer and height) changes `y` / `height`, which the DXT5 edge-alpha
quirk reads, so those blocks' alpha bytes would differ. Chunks are therefore whole rows of blocks encoded with the whole
image's descriptors. Mutation check: with the worker's FP-state load removed, the MXCSR runs differed in about 1.2 M
blocks and the read-back counter fired in the x87 24-bit run, which confirmed that the read-back is the guard for the
x87 precision.

**Outcome:** "Use several cores" added. Offline speed on the development PC (6 workers plus the caller): 256 x 256
0.43 to 0.09 ms (DXT1); 1024 x 1024 7.3 to 1.5 ms (DXT1) and 8.6 to 1.55 ms (DXT5); 2048 x 2048 29 to 5 ms and 34.5 to
5.6 ms. Rejected: the sub-image split described above.

### Development check rate

The combined-build documentation gave the development check rate as 1 texture in 8. The first commit of this repository
(`7d5f453`, 2026-09-30) already uses 1 in 64 (`g_verifyEvery` in `features/fast_dxt.cpp`); the date of that change is
not recorded.

### Not done: AVX2 path

An AVX2 path (8 blocks per register) was considered. It needs proof that the VEX forms of `rcpss` / `rsqrtss` return the
same bits as the legacy SSE forms on every CPU; without that proof the file stays on legacy SSE encodings (it refuses
`/arch:AVX`).
