# Performance

Performance reduces the stutters (single long frames, also called hitches) that The Sims 3 produces while lots stream
in, the camera pans over a neighbourhood, Sims are built in Create a Sim, textures and caches are written, and rooms
relight. It is a group of fifteen independent switches, plus the separate [Lot Streaming](lot-streaming.md) page. Each
one replaces or reschedules one specific piece of game work; none of them changes what the game draws once its work is
done. Most of them produce exactly the game's own result faster; the scheduling switches spread the same work over more
frames while the camera moves.

## Status

| | |
|---|---|
| Availability | Released. The individual switches were introduced between 2.1.0 and 2.7.0 (see the table below); the first twelve are on by default since 2.5.5. The single Overview switch for the group: Released in 2.6.0, covering fifteen switches since 2.7.0. Room to save, Lighter window updates and Faster scripts: Released in 2.7.0 (Experimental until 2.15.0). Lamp lighting card: Released in 2.13.0 |
| Default | All fifteen on when no saved choice exists. An explicit saved off choice stays off. The Lot Streaming switches are off by default |
| Menu | System > Performance (cards *Lamp lighting*, *Camera and lighting*, *Files and objects*, *Textures and Sims*, *Memory handling*, *Game and scripts*); System > Lot Streaming; Overview > Performance (one switch for the fifteen) |
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
  a quadratic test, and serialises every allocation of every thread on one barely spinning lock;
- its 32-bit address space fragments in long sessions until a save fails (Error 12);
- it repaints its own window every frame and its scripts call a library for every decimal comparison.

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
4. **Refuse when unsure.** Every hook site is compared with the studied Steam 1.67.2 bytes (or found by signature on
   other builds) before it is written. A difference leaves the switch off with a status message.

### The switches

| Switch (menu label) | Card | Introduced | Page |
|---|---|---|---|
| Faster room lighting (with *Lamp edits all at once*, 2.7.0 as *Lamp switches all at once*) | Camera and lighting | 2.1.0 or earlier | [room-light-queue.md](room-light-queue.md) |
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
| Room to save | Memory handling | 2.7.0 | [room-to-save.md](room-to-save.md) |
| Lighter window updates | Game and scripts | 2.7.0 | [lighter-window-updates.md](lighter-window-updates.md) |
| Faster scripts | Game and scripts | 2.7.0 | [faster-scripts.md](faster-scripts.md) |

The System > Lot Streaming page holds five more switches, off by default (Extended lot detail, Smooth lot streaming,
Keep lot visibility stable, Pause lot streaming in map view, Spread lot objects while loading; Experimental, 2.7.0,
research and code by idavidveiga): [lot-streaming.md](lot-streaming.md). They are not part of the Overview group.

One related change has no switch: the [Vulkan driver guard](vulkan-driver-guard.md) keeps an unused AMD Vulkan driver
out of the game's address space on hybrid-GPU PCs.

Several 2.14.0 behaviours also have no switch of their own; they follow the switch or feature named:

- the step budget of the extra solves, with *Faster room lighting*, and automatic lamps first in the room queue, with
  Night Lights
  ([room-light-queue.md](room-light-queue.md#automatic-lamps-first-and-the-step-budget));
- the game allocator's own trim and top-down large blocks, always on Steam 1.67.2
  ([fast-memory.md](fast-memory.md#allocator-trim-and-top-down-large-blocks));
- the early save preparation in front of sims3-modernization's save detour, with *Room to save*
  ([room-to-save.md](room-to-save.md#compatibility-and-interactions));
- lamp-visibility memo, liveness pruning and buffer reads without waiting for the GPU, with Night Lights
  ([level-light-share.md](../night-lighting/level-light-share.md#cost));
- memory given back once only a lot that is no longer loaded could use it, with Night Lights: the kept parts of
  [Quick outdoor lamp edits](../night-lighting/outdoor-lamp-delta.md) for rooms whose story manager no loaded lot holds
  (dropped at the second 10 s sweep in a row that misses it, never while a world loads), the CPU copy of
  [Real-time outdoor lamps](../night-lighting/rt-outdoor-lamps.md)' occluders once they are uploaded, and the lot mask
  texture of a lot not drawn for 2 minutes, made again from its kept masks at its next draw with the same texels
  (`SweepLeftLots` in [`outdoor_delta.cpp`](../../../features/outdoor_delta.cpp), `ReleaseIdleMaskTextures` in
  [`lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp)).

Their maintainer-side switches are listed in [workflow.md](../../workflow.md#development-switches-dev_build).

## Bounded gameplay work

The gameplay paths avoid compression and bookkeeping work without changing simulation timing or shader
quality. Complete repeated 16x4 texture strips reuse their preceding encoded bytes only after exact pixel comparison;
partial blocks and native fallbacks retain their normal path. Already ordered CAS triangle counts skip sorting and
temporary buffers. RefPack compares long prefixes in two bounded SSE2 blocks per loop. The script type cache retains
one generation-checked result per thread, while shared entry fields use atomic loads/stores. Object-index lookups omit
unused public-mode timing reads. Mesh-occluder discovery pauses for 100 ms after a scan finds all visible groups cached.

Numbered public test builds also include an automatic frame-interval summary using a fixed 8 KB histogram, not the developer profiler. It records foreground,
loaded-world play with the Apex panel closed after a three-second warm-up, then logs mean, p50/p95/p99, worst interval
and counts above 33.3/50 ms once per minute. Percentiles round upward in half-millisecond bins; overflow percentiles
use the observed maximum. Intervals include presentation and frame limiting, so this does not identify CPU/GPU causes.
Menu, focus and world changes discard the current window. Source: `features/frame_pacing.h`, `framework/overlay.cpp`.

The previous GC scheduler and global service budgets remain removed. Historical GC totals aggregate simulation-thread
calls and do not establish a render stall; shifting those calls cannot be justified as an FPS fix by that evidence.
The implementation retains existing bounded workers and synchronous ownership of texture/cache data. Gameplay FPS gains
and long-session stability remain unverified; isolated codec timings are not a frame-rate prediction.

## Lamp lighting card

The first card of System > Performance chooses how lot lamps update after one is switched, recoloured or moved. Its
one row, *Lamp updates*, has two modes:

| Mode | Row description | What runs |
|---|---|---|
| *Instant (GPU)* (default) | "Your graphics card updates outdoor lamp light instantly; moving lamps may dip FPS a little" | [Real-time outdoor lamps](../night-lighting/rt-outdoor-lamps.md) draws the active lot's outdoor lamps and their shadows on the graphics card, on the kept parts of [Quick outdoor lamp edits](../night-lighting/outdoor-lamp-delta.md); indoor lamps use the game's calculation |
| *Classic* | "Easier on the graphics card; light updates once you let go of a lamp" | The game's room solves with every correction; a dragged lamp's rooms are solved once after the release ([lamp drags](../night-lighting/lamp-drags.md#classic-relight-only-after-release)) and an edit's stories change together ([Faster room lighting](room-light-queue.md#classic-show-all-floors-together)) |

A warning note under the row says why Instant cannot run on the device (another game version than Steam 1.67.2, a
graphics card that cannot draw the shadows, or shaders that could not start); the lamps then work as Classic, and once
Classic is picked Instant cannot be chosen again. With Instant chosen but doing nothing, a note names the cause (*Outdoor
light between floors* off, or its lamp edits could not start). Instant runs inside Night Lights: with Night Lights off
the card shows *Needs Night Lights (Lighting page)* and a button to turn it on. Switching modes relights the active lot
once. The GPU time of the last lamp edit is on Developer > [Tester](../dev-tools/tester.md).

Setting: `lampLightingMode` (`"classic"` or `"instant"`, default `"instant"`) under `[patches.NightTerrainRelight]`; a
saved value is kept, and a file without the key starts on Instant. Presets carry it in the Performance part (the key is
taken out of Night Lights' table for a Lighting-only preset). Source: `LampLightingCard` in
[`apex_gui.cpp`](../../../apex_gui.cpp).

## Settings

Each switch's settings are documented once, on its own page. The group-level controls are:

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Overview > Performance | (none: sets the fifteen `enabled` keys) | bool | on when all fifteen are on | | Turns all fifteen on or off; a switch the game version does not have counts neither as on nor as off. Turning on enables *Faster game file lookups* before *Remember missing files*; turning off stops them in the reverse order. Not switchable while the game is loading |
| Overview > All effects | (none) | bool | | | Also turns the Performance group on or off; turning everything off leaves Room to save on (it protects saves and changes nothing on screen) |
| (row switch on each page) | `[patches.<Name>] enabled` | bool | on | | A missing key reads as on; an explicit `false` stays off |

No row of this page shows an Experimental badge: Room to save, Lighter window updates, Faster scripts and the two
sub-rows of *Faster room lighting*, on by default since 2.7.0, lost it in 2.15.0. Each row's hover text is the
switch's description, ending with the credit.

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
- **Profiles:** the Performance profile part carries nine of the fifteen switches (*Faster game file lookups*,
  *Remember missing files*, *Faster file lists*, *Spread lot lighting while moving*, *Wall shading waits while moving*,
  *Faster texture compression*, *Faster cache compression*, *Spread new objects over frames*, *Faster object lookups*).
  *Faster room lighting*, *Faster Sim building*, *Faster memory handling*, *Room to save*, *Lighter window updates*,
  *Faster scripts* and the Lot Streaming switches are not in any profile part (`FeaturePart` in
  [`apex_config.cpp`](../../../apex_config.cpp)): applying a profile, built-in ones included, leaves them as they are.
  Undo covers every `[patches.*]` table.
- **Configurations from 2.5.6:** a `[ui] performance_mode` key (the removed *Optimize rendering* switch) still loads;
  it is ignored and dropped at the next save. The rendering paths it selected always use their original form.
- **Game build:** every switch is written for Steam 1.67.2 (`supportedVersions = VERSION_STEAM`; Faster scripts also
  `VERSION_EA`). On other builds a switch is available when its address group resolves by signature
  ([`framework/game_addresses.cpp`](../../../framework/game_addresses.cpp); the full signature table is in
  [game-versions.md](../../engine/game-versions.md) section 6); otherwise it stays off with a status message.

## Limitations

- The scheduling switches make lighting, wall shading and newly placed objects finish a little later while the camera
  moves. Each page states the bound.
- A switch whose verification finds a difference turns itself off for the rest of the session. The status line and
  `ApexRadiance_LOG.txt` say why.
- The switches outside the Performance profile part keep their current state when a profile is applied.

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
| [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) | The Performance and Lot Streaming `ApexPatch` features, their registration (`APEX_REGISTER_FEATURE`, category `Performance`) and the menu accessors |
| [`patches/performance.h`](../../../patches/performance.h) | TOML section names (`kResourceCacheName` .. `kRoomLightQueueName`, never renamed), `kLotLightingBudgetDefault`, `LotLightingBudgetMs` / `SetLotLightingBudgetMs`, `FastTextureSeveralCores` / `SetFastTextureSeveralCores`, the `*Status` one-line states |
| [`apex_gui.cpp`](../../../apex_gui.cpp) | `PerformancePage`, `PerformanceCard`, `LotStreamingPage`, `LotStreamingCards`, `FeatureSwitchRow`, the Overview group (`kOverviewPerformancePatches`, `SetPerformanceGroup`), Developer > Performance cards (`DevProfilerTab`) |
| [`framework/game_addresses.{h,cpp}`](../../../framework/game_addresses.h) | Address ids and groups (fixed on Steam, signatures elsewhere) |
| [`features/developer_settings.h`](../../../features/developer_settings.h) | Developer tuning saved under `[developer.controls.<module>]` while developer mode is on |

### Developer page

Developer > Performance shows a card per module with its counters (under *Live counters*), verification controls
(*Check 1 answer in N against the game*, *Check every answer for 10 s*, and similar) and tuning sliders. Cards exist
for the file caches, object lookups, lot lighting, wall shading, scene objects, texture compression and cache
compression. *Faster room lighting*, *Faster Sim building*, *Faster memory handling*, *Room to save*, *Lighter window updates*,
*Faster scripts* and the Lot Streaming switches have no card; the Frame
Profiler report carries the memory and record-checksum status lines. Tuning values are saved with the developer
preferences ([developer-mode.md](../developer-mode.md)), never with profiles unless the Development part is chosen.

## Rejected approaches

- A per-frame "Optimize rendering" mode (`[ui] performance_mode`) that cached lamp blocks, indexed light maps and
  reused texture metadata: removed in 2.6.0. Details in [history](../../history/performance.md).
- A lot lighting budget while the camera is still: reverted, lamps moved in Build mode updated visibly slower. See
  [lot-lighting-motion.md](lot-lighting-motion.md).

## See also

- [Validation](../../validation/performance.md)
- [History](../../history/performance.md)
- [Frame Profiler](../frame-profiler.md)
- [UI reference](../../ui.md)
