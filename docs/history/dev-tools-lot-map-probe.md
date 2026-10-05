# Lot Map Probe: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The technique is described
in [features/dev-tools/lot-map-probe.md](../features/dev-tools/lot-map-probe.md).

Evidence: the four folders `MapaLote_1` to `MapaLote_4` in the combined build's S3SS folder (2026-09-27 22:57 to 23:06,
written by the Portuguese-label version of the code) and NOTAS 27/09.

### 2026-09-27: world-space AO study

**Context:** NOTAS "AO novo: deterministico, estudo no lab (27/09)": a feasibility test for a deterministic world-space
ambient occlusion built from a lot height map.

**Finding:** the game's draws can be re-rendered from an ortho camera with "0 failures, 100% covered" once the camera
blocks are handled, but shader families read the camera from different constant blocks (c0, c4, c40, c180...), roofs
cover interiors from above, and Sims move, so a static map cannot shade them. The captures redrew 107k to 304k draws
over 240 frames.

**Outcome:** abandoned as the AO path in favour of the screen-space HBAO in `patches/ambient_occlusion_patch.cpp`. AO was
later removed and then reintroduced as standalone GTAO ([ambient-occlusion.md](../features/ambient-occlusion.md)). The
tool was never carried into Apex Radiance.

### 2026-09-27: only c40..c43 replaced

**Context:** the first test (MapaLote_1 to MapaLote_3) wrote the ortho matrix only into VS c40..c43.

**Finding:** 75k to 134k draws were skipped as "other camera": those families read the camera from c0, c4, c180, c192
or c216 and were drawn with the real perspective camera.

**Outcome:** replace every block whose x, y and w rows match the camera (`kCamBlocks` voting plus a register scan). The
file header comment still says "c40..c43 replaced", which is outdated: the code replaces all matching blocks.

### 2026-09-27: a block of zeros won the camera vote

**Context:** the second version (MapaLote_4, 23:06).

**Finding:** it elected an all-zero block that many draws have: every value in `info.txt` is `nan` (centre, heights,
camera) and the "camera found" list covers almost every register.

**Outcome:** only blocks shaped like a camera may vote (`LooksLikeCamera`). No capture was made after this fix.

### 2026-09-27: height calibration

**Context:** reading the heights of MapaLote_1 to MapaLote_3.

**Finding:** they report `upSign -1`, camera y -49.7 / -62.8 and heights around -230 to -216 m, while lamps in the same
town are at y about 60 to 90 (lighting snapshot). Depth spans 0.9949 to 0.9987.

**Outcome:** the world-up sign and position decode in `SetupMap` were never validated against a known height.

### 2026-09-28: standalone plan

**Context:** PLANO-SEPARACAO.md section 4 listed the renames for the standalone.

**Finding:** `MapaLote_N` was not listed; it would have landed in `...\Apex Radiance\MapaLote_N` with
`[patches.LotMapProbe]` in `ApexRadiance.toml`.

**Outcome:** the tool was not ported; Apex Radiance has no Lot Map Probe.
