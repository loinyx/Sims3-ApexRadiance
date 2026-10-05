# Performance: history

Chronological record of the Performance group as a whole. Newest entries at the bottom. The current behaviour is
described in [features/performance/README.md](../features/performance/README.md); each switch has its own history page
(listed at the end).

### 2026-09-28: measured baseline

**Context:** a Frame Profiler session on the maintainer's game: a camera test of about 110 s with 1,293 hitches.

**Finding:**

- Small hitches (8 to 16 ms): "Resource lookup" dominant in about 65%: 246 to 288 lookups per hitch frame on the render
  thread, each asking about 291 packages (packages per lookup 291.1, misses 0), 4.6 to 5.3 ms per frame, about 18 us per
  lookup. Materials resolving textures (Scene::BeginFrame's pending-node drain), async-load finalize jobs, CAS and lot
  loading all go through it.
- Medium hitches (16 to 50 ms) while the camera moves: "Lot room solve" dominant in 50 to 77% at about 15 to 17 ms: the
  game's 15 ms per-frame budget for the priority lot, spent in one frame.
- Large hitches (50 ms and more), later runs: "DXT encode" dominant in 22 to 40% of them (44 to 49 ms per such hitch;
  also about 8 to 11% of the small ones), "RefPack compress" in 20 to 30% (20 to 37 ms per hitch; some small ones).

**Outcome:** the anti-stutter plan (`research\perf2\plan.md`) chose candidates C1 (lookup cache), C7 (lot lighting while
moving), C9 (texture compression), C4 (cache compression), C6 (scene node budget) and C8 (object lookups).

### 2026-09-29: first implementation

**Context:** the six candidates written from the game's disassembly (`research\engine_map\full.asm`,
`S3SS-dev\re\TS3W.exe`) and Apex's own framework, with no Sims3SettingsSetter code. Facts were marked VERIFIED (read in
the disassembly) or INFERRED (deduction, not confirmed at run time). At first written, not compiled or tested in game.

**Finding:** the menu had one card under SYSTEM > Performance, two Overview rows, and developer lines under
Developer > Profiler > "Performance". Defaults: *Lot lighting while moving* on; *Faster game file lookups*, *Faster
texture compression* (with *Use several cores* on), *Faster cache compression*, *Spread new objects over frames* and
*Faster object lookups* experimental and off by default. Round 3 (`research\perf2\round3.md`) added *Wall shading while
moving* (on), *Remember missing files* and *Faster file lists* (experimental, off); *Faster room lighting* was added the
same day (on, flagged experimental). The plan for each experimental switch was to turn its default on after several
clean in-game sessions with the developer checks.

The official Sims3SettingsSetter source was read only to list the game sites it patches; its 51 byte patterns were
matched against TS3W.exe and none overlaps a site used here. A literal scan of `Sims3SettingsSetter.asi`,
`Sims3Performance.asi` and `MonoPatcher.asi` found none of the addresses and slots (plan section 6).

**Outcome:** kept. Per-switch details are in the switch histories.

### 2026-09-30: Sim building, memory and address space

**Context:** follow-ups to loading research (`loadre\*.md`) and a sampling run.

**Outcome:** *Faster Sim building* (released in 2.3.0), *Faster memory handling*, the record checksums inside *Faster
cache compression* and the Vulkan driver guard (released in 2.4.0) were added. See their histories.

### 2026-10-01: 2.5.3 grouped Performance controls

**Context:** the single Performance card had grown to twelve rows.

**Finding:** the card was split into four cards (*Camera and lighting*, *Files and objects*, *Textures and Sims*,
*Memory handling*); dependent rows stayed under their parent. Experimental badges were removed by maintainer decision.

**Outcome:** shipped in 2.5.3. The labelling change did not add gameplay validation.

### 2026-10-02: 2.5.5 all switches on by default

**Context:** the per-switch "turn the default on after clean sessions" plan.

**Outcome:** all twelve `enabledByDefault = true` from 2.5.5. An explicit saved off choice stays off. The metadata still
carried `experimental = true` on ten of them, which the menu ignored.

### 2026-10-03: 2.5.6 Optimize rendering mode

**Context:** 2.5.6 added a live, persistent switch on the Performance page, `[ui] performance_mode` (default true;
explicit saved false stayed false; missing settings, the row reset and Reset all used true). It left visual settings and
the performance switches unchanged.

**Finding:** when enabled it cached the prepared outdoor object lamp parameter block in the exact-position lamp memo,
indexed light-map entries in map-key order, read adjacent atlas / strength pixel shader constants together, and reused
immutable metadata for validated retained world-chunk textures. Disabling restored the original map traversal, per-draw
lamp-row preparation, separate constant reads and texture metadata checks. No visible update was postponed. The offline
constant-read tests (`tools/performance_pair_test`) passed 24,577 checks, including bit preservation and failure
fallback; they compared 10,000 lamp blocks byte for byte and 10,000 map-index results with the original calculations,
including insertion, empty maps, reset and live mode switching. These checks did not establish in-game image or
performance behaviour; no FPS improvement was claimed.

**Outcome:** published in 2.5.6.

### 2026-10-03: Optimize rendering removed; Overview group switch (PR #2)

**Context:** PR #2, commit `603568a`.

**Finding:** the mode-dependent code paths (in `features/lot_light_bridge.cpp`, `features/lightmap_smooth.cpp`,
`features/performance_mode.h`) and the pair test were removed; the renderer always uses the pre-2.5.6 paths. The
`experimental` metadata flags were cleared on all twelve switches. Overview gained one Performance switch for all twelve,
included in the All effects switch.

**Outcome:** in development. Configurations with `[ui] performance_mode` still load; the key is ignored and dropped at
the next save. In-game validation of the removal is an open check.

## Switch histories

[Room lighting](performance-room-light-queue.md), [lot lighting](performance-lot-lighting-motion.md),
[wall shading](performance-wall-shading-while-moving.md), [scene nodes](performance-scene-node-budget.md),
[file lookups](performance-resource-lookup-cache.md), [missing files](performance-remember-missing-files.md),
[file lists](performance-file-list-cache.md), [object lookups](performance-object-lookup-index.md),
[texture compression](performance-fast-texture-compression.md), [cache compression](performance-fast-cache-compression.md),
[Sim building](performance-fast-cas-sort.md), [memory](performance-fast-memory.md),
[Vulkan driver guard](performance-vulkan-driver-guard.md).
