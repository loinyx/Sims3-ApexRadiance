# Performance

Performance reduces the stutters (single long frames, also called hitches) that The Sims 3 produces while lots stream
in, the camera pans over a neighbourhood, Sims are built in Create a Sim, textures and caches are written, and rooms
relight. It is a group of twelve independent switches. Each one replaces or reschedules one specific piece of game work;
none of them changes what the game draws once its work is done. Most of them produce exactly the game's own result
faster; the scheduling switches spread the same work over more frames while the camera moves.

## Status

| | |
|---|---|
| Availability | Released. The individual switches were introduced between 2.1.0 and 2.4.0 (see the table below); all twelve are on by default since 2.5.5. The single Overview switch for the group: in development (PR #2) |
| Default | All twelve on when no saved choice exists. An explicit saved off choice stays off |
| Menu | System > Performance (cards *Camera and lighting*, *Files and objects*, *Textures and Sims*, *Memory handling*); Overview > Performance (one switch for the group) |
| Configuration | One `[patches.<Name>]` table per switch in `ApexRadiance.toml` |
| Developer mode | Switches: no. Counters, verification controls and tuning: Developer > Performance |
| Source | [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp), [`patches/performance.h`](../../../patches/performance.h), the feature modules linked from each page |

## The problem

The game does several expensive things on the render thread or while the render thread waits, with no time limit:

- it searches every one of its roughly 290 packages for each resource it loads, and walks every package index to list
  files of one type;
- it solves room lighting one room per frame for the whole world, and gives the current lot a 15 ms lighting budget per
  frame even while the camera moves;
- it shades every outdoor wall of a newly loaded lot in one pass with no time check;
- it places every newly queued scene node in one frame;
- it walks the whole world object tree to find a lot by its ID;
- it compresses textures (DXT) and cache records (RefPack) with slow single-threaded encoders, sorts CAS triangles with
  a quadratic test, and serialises every allocation of every thread on one barely spinning lock.

Each of these shows up as a dominant cause of hitches in the [Frame Profiler](../frame-profiler.md). The engine side of
each problem is described on the switch's own page and in the engine references:
[lot loading and streaming](../../engine/lot-loading-and-streaming.md), [room light maps](../../engine/room-light-maps.md)
and [main loop and services](../../engine/main-loop-and-services.md).

## How Apex Radiance solves it

Every switch works at the exact game function behind one measured cost, and follows the same rules:

1. **Same result.** Replacements are bit-identical to the game's output (texture compression, Sim building, record
   checksums), or produce the game's own format checked by the game's own decoder (cache compression), or answer from a
   cache that is re-validated against the live game state on every answer (file lookups, file lists, object lookups).
2. **Same work, later.** Scheduling switches never skip work. They shrink a per-frame budget or return the engine's own
   "try again next frame" state while the camera moves, and give the game back its full pace when the camera stops.
3. **Checked against the game.** The first calls of every session (16 for the encoders, 64 for object lookups) also run
   the game's own function and compare. Any difference is logged, the game's answer is used, and that switch turns
   itself off for the session.
4. **Refuse when unsure.** Every hook site is compared with the studied Steam 1.67.2 bytes before it is written. A
   difference leaves the switch off with a status message.

### The switches

| Switch (menu label) | Card | Introduced | Page |
|---|---|---|---|
| Faster room lighting | Camera and lighting | 2.1.0 or earlier | [room-light-queue.md](room-light-queue.md) |
| Spread lot lighting while moving | Camera and lighting | 2.1.0 or earlier | [lot-lighting-motion.md](lot-lighting-motion.md) |
| Wall shading waits while moving | Camera and lighting | 2.1.0 or earlier | [wall-shading-while-moving.md](wall-shading-while-moving.md) |
| Spread new objects over frames | Camera and lighting | 2.1.0 or earlier | [scene-node-budget.md](scene-node-budget.md) |
| Faster game file lookups | Files and objects | 2.1.0 or earlier | [resource-lookup-cache.md](resource-lookup-cache.md) |
| Remember missing files (shown under the row above) | Files and objects | 2.1.0 or earlier | [remember-missing-files.md](remember-missing-files.md) |
| Faster file lists | Files and objects | 2.1.0 or earlier | [file-list-cache.md](file-list-cache.md) |
| Faster object lookups | Files and objects | 2.1.0 or earlier | [object-lookup-index.md](object-lookup-index.md) |
| Faster texture compression (with *Use several cores*) | Textures and Sims | 2.1.0 or earlier | [fast-texture-compression.md](fast-texture-compression.md) |
| Faster cache compression (includes record checksums) | Textures and Sims | 2.1.0 or earlier; checksums 2.4.0 | [fast-cache-compression.md](fast-cache-compression.md) |
| Faster Sim building | Textures and Sims | 2.3.0 | [fast-cas-sort.md](fast-cas-sort.md) |
| Faster memory handling | Memory handling | 2.4.0 | [fast-memory.md](fast-memory.md) |

One related change has no switch: the [Vulkan driver guard](vulkan-driver-guard.md) keeps an unused AMD Vulkan driver
out of the game's address space on hybrid-GPU PCs.

## Settings

Each switch's settings are documented once, on its own page. The group-level controls are:

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Overview > Performance | (none: sets the twelve `enabled` keys) | bool | on when all twelve are on | | Turns all twelve on or off. Turning on enables *Faster game file lookups* before *Remember missing files*; turning off stops them in the reverse order. Not switchable while the game is loading |
| Overview > All effects | (none) | bool | | | Also turns the Performance group on or off |
| (row switch on each page) | `[patches.<Name>] enabled` | bool | on | | A missing key reads as on; an explicit `false` stays off |

The rows show no Experimental badge. Each row's hover text is the switch's description, ending with the credit.

## Compatibility and interactions

- **Frame Profiler:** shares every hooked site through layered hooks (below), so the profiler times whichever
  implementation runs and adds counters such as "from cache", "deferred" and "from index". See
  [frame-profiler.md](../frame-profiler.md).
- **Night Lighting:** its room relights go through the same lot lighting update and room scheduler that *Spread lot
  lighting while moving* and *Faster room lighting* adjust. See [lot-lighting-motion.md](lot-lighting-motion.md) and
  [room-light-queue.md](room-light-queue.md).
- **Official Sims3SettingsSetter:** its patches touch none of the game sites used here. Its RefPack decompressor
  replacement only reads streams, and the fast compressor's streams are checked through whichever decoder is installed.
  Its Lot Streaming throttle limits object creation per frame and can be on together with *Spread new objects over
  frames*. Details are on each page.
- **Sims3Performance 1.0.0-beta1** hooks 0x00EA3820 (isinst cache), 0x00D7FC60 / 0x00D74800 / 0x00D7F8B0 /
  0x00D80BC0 (simulator stages), 0x0059C5B0 (localized strings), 0x00EBBDE0 / 0x00EA8720 (Mono ehash), the allocator
  arenas, the STBL layout, thread stacks and short waits; its PackageIndexWarmup only reads the Mods / DCCache files
  into the OS cache. None of these is a resource-manager or lot-lighting site used here.
- A literal scan of `Sims3SettingsSetter.asi`, `Sims3Performance.asi` and `MonoPatcher.asi` for the addresses and slots
  used by these switches finds none.
- **Old combined build / older `S3SSApex.asi`:** Apex Radiance idles when one is loaded (see [architecture](../../architecture.md)).
- **Profiles:** the Performance profile part carries nine of the twelve switches (*Faster game file lookups*,
  *Remember missing files*, *Faster file lists*, *Spread lot lighting while moving*, *Wall shading waits while moving*,
  *Faster texture compression*, *Faster cache compression*, *Spread new objects over frames*, *Faster object lookups*).
  *Faster room lighting*, *Faster Sim building* and *Faster memory handling* are not in any profile part
  (`FeaturePart` in [`apex_config.cpp`](../../../apex_config.cpp)). Undo covers every `[patches.*]` table.
- **Configurations from 2.5.6:** a `[ui] performance_mode` key (the removed *Optimize rendering* switch) still loads;
  it is ignored and dropped at the next save. The rendering paths it selected always use their original form.
- **Game build:** every switch is Steam 1.67.2 only (`supportedVersions = VERSION_STEAM`). Addresses resolve through
  [`framework/game_addresses.cpp`](../../../framework/game_addresses.cpp); the full signature table is in
  [game-versions.md](../../engine/game-versions.md) section 6.

## Limitations

- The scheduling switches make lighting, wall shading and newly placed objects finish a little later while the camera
  moves. Each page states the bound.
- A switch whose verification finds a difference turns itself off for the rest of the session. The status line and
  `ApexRadiance_LOG.txt` say why.
- The three switches outside the Performance profile part keep their current state when a profile is applied.

## Technical reference

### Layered hooks

Several Apex modules wrap the same game sites (a switch and the Frame Profiler, or the wall shading gate and the
profiler). Each site type has a chain with fixed layer positions, lower = outer, independent of install order:

| Chain | File | Hooks | Layers (outer to inner) |
|---|---|---|---|
| `SlotChain` | [`framework/slot_chain.{h,cpp}`](../../../framework/slot_chain.h) | Vtable slots | `Gate` (wall shading gate, site `WallAoStep` only), `FrameProfiler`, `ResourceCache`, `FastCompress` |
| `EntryChain` | [`framework/entry_chain.{h,cpp}`](../../../framework/entry_chain.h) | Function entries: the prologue moves to a trampoline, a 5-byte JMP is written with every other thread suspended | `FrameProfiler`, `FastDxt`, `ResourceCache`, `ObjectIndex`, `SceneBudget`, `LevelLightShare`, `FastCas`, `FastCrc` |
| `CallChain` | [`framework/call_chain.{h,cpp}`](../../../framework/call_chain.h) | One `E8 rel32` CALL, rewritten with every other thread suspended | `FrameProfiler`, `SceneBudget` |

Protocol (`SlotChain`; the other two are the same idea):

- The slots hold the outermost installed layer's hook; each hook calls `Next(site, layer)`, which returns the next
  inner installed layer's hook or the game function.
- Install writes the new layer's next pointer first, then either swaps the slots to it (one interlocked
  compare-exchange per slot while the page is writable; every slot must hold the expected pointer or nothing is written)
  or re-points the outer layer's next pointer (one atomic store).
- Remove does the reverse. A removed hook keeps its next pointer, so threads still inside it finish normally.
- Sites: `FindProvider`, `RegisterDb`, `RegisterDbDerived`, `SetDbPriority`, `DbChanged`, `RefPackCompress`,
  `WallAoStep`, `KeyListBase`, `KeyListDerived` (slots); `DxtEncode1/5`, `DpfWriteDirect`, `ObjectById`,
  `SceneNodeDtor`, `SceneAddNode`, `SceneHolderTeardown`, `CasTriSort`, `RecordCrc` (entries); `SceneDrain` (call).

Code writes outside the chains use `MemPatch::WriteCodeSuspended` ([`framework/memory_patch.{h,cpp}`](../../../framework/memory_patch.h)):
every other thread is suspended and none may be stopped inside the bytes being replaced.

### Shared camera motion signal

*Spread lot lighting while moving*, *Wall shading waits while moving*, *Spread new objects over frames* and *Faster
room lighting* use one camera sampler, `LotLightingMotion::SampleCameraMoving`. It works whether or not *Spread lot
lighting while moving* is on. Its definition of "moving" is on [lot-lighting-motion.md](lot-lighting-motion.md).

### Files

| File | Role |
|---|---|
| [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) | The twelve `ApexPatch` features, their registration (`APEX_REGISTER_FEATURE`, category `Performance`) and the menu accessors |
| [`patches/performance.h`](../../../patches/performance.h) | TOML section names (`kResourceCacheName` .. `kRoomLightQueueName`, never renamed), `kLotLightingBudgetDefault`, `LotLightingBudgetMs` / `SetLotLightingBudgetMs`, `FastTextureSeveralCores` / `SetFastTextureSeveralCores`, the `*Status` one-line states |
| [`apex_gui.cpp`](../../../apex_gui.cpp) | `PerformancePage`, `PerformanceCard`, `FeatureSwitchRow`, the Overview group (`kOverviewPerformancePatches`, `SetPerformanceGroup`), Developer > Performance cards (`DevProfilerTab`) |
| [`framework/game_addresses.{h,cpp}`](../../../framework/game_addresses.h) | Address ids and groups (fixed on Steam, signatures elsewhere) |
| [`features/developer_settings.h`](../../../features/developer_settings.h) | Developer tuning saved under `[developer.controls.<module>]` while developer mode is on |

### Developer page

Developer > Performance shows a card per module with its counters (under *Live counters*), verification controls
(*Check 1 answer in N against the game*, *Check every answer for 10 s*, and similar) and tuning sliders. Cards exist
for the file caches, object lookups, lot lighting, wall shading, scene objects, texture compression and cache
compression. *Faster room lighting*, *Faster Sim building* and *Faster memory handling* have no card; the Frame
Profiler report carries the memory and record-checksum status lines. Tuning values are saved with the developer
preferences ([developer-mode.md](../developer-mode.md)), never with profiles unless the Development part is chosen.

## Rejected approaches

- A per-frame "Optimize rendering" mode (`[ui] performance_mode`) that cached lamp blocks, indexed light maps and
  reused texture metadata: removed in PR #2. Details in [history](../../history/performance.md).
- A lot lighting budget while the camera is still: reverted, lamps moved in Build mode updated visibly slower. See
  [lot-lighting-motion.md](lot-lighting-motion.md).

## See also

- [Validation](../../validation/performance.md)
- [History](../../history/performance.md)
- [Frame Profiler](../frame-profiler.md)
- [UI reference](../../ui.md)
