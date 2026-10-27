# Apex Radiance documentation

Apex Radiance is a native mod for The Sims 3 (Steam 1.67.2, `TS3W.exe`, 32-bit), loaded by Ultimate ASI Loader as
`ApexRadiance.asi`. It hooks Direct3D 9 and patches game code in memory to rebuild night lighting, add image effects
(ambient occlusion, edge smoothing, depth blur, colour grading and stackable filters) and remove stutters. It can run beside an unmodified
Sims3SettingsSetter.

Settings, logs and captures live in `Documents\Electronic Arts\The Sims 3\Apex Radiance\`.

## How these documents are organised

Every feature has up to three pages, following the [documentation standard](DOCUMENTATION-STANDARD.md):

| Folder | Content |
|---|---|
| [`features/`](features/) | What the feature does, the problem in the game, how the mod solves it, settings, limitations, technical reference |
| [`validation/`](validation/) | Test harnesses, latest results, in-game test plan, open checks |
| [`history/`](history/) | Dated investigations, rejected approaches, superseded designs |
| [`engine/`](engine/) | Reverse engineering of `TS3W.exe` |
| [`releases/`](releases/README.md) | Release notes per version |

When a document and the code disagree, the code is correct and the document must be fixed. Game addresses are for
Steam 1.67.2 (image base 0x00400000) unless stated otherwise.

## Getting started

Developer-mode memory captures (Released in 2.14.0, off by default) are described in [Memory analysis](features/memory-analysis.md).
Per-lot loading events (Released in 2.14.0, developer mode only) are described in [Lot loading analysis](features/lot-load-analysis.md).

| Document | Covers |
|---|---|
| [architecture.md](architecture.md) | Loading, D3D9 hooks, hook registry, post-scene chain, INTZ depth share, patch system, configuration, logger, threads |
| [workflow.md](workflow.md) | Build, install, testing harnesses, diagnosis with captures, release process, repository skills |
| [ui.md](ui.md) | The Violet menu: pages, cards, widgets, shortcuts, profiles, notices, languages |
| [DOCUMENTATION-STANDARD.md](DOCUMENTATION-STANDARD.md) | How to write and update these documents ([templates](templates/)) |

## Features

### Lighting

| Feature | Status |
|---|---|
| [Night Lighting](features/night-lighting/README.md): lamps that really light the world at night | Released; Released in 2.15.6: winter terrain layout recognition, foliage dusk synchronisation and less repeated lighting work; Released in 2.16.0: lamp light past 1 outdoors, outdoor light through openings balanced, room light map edges at walls, wall lamps lighting their wall; Released in 2.16.1: kept solves that account for every light |
| [Lot light pass](features/night-lighting/lot-light-pass.md): lot and street lamps on lot grass | Released; Released in 2.13.0: lamp shade on lot ground (walls shade lamp light near the ground); Released in 2.13.1: the shade past the lot (world grass, roads), garden walls and fences, smooth ground shade with Instant; Released in 2.15.3: a shade that takes only the light of the lamps a wall blocks, closed gates and fence gaps, snowy lots shaded, indoor light added to the ground |
| [World atlas and smoothed maps](features/night-lighting/world-atlas-and-smoothed-maps.md): smooth terrain light maps | Released |
| [Terrain relight](features/night-lighting/terrain-relight.md): rebuilding terrain lighting at dusk and after edits | Released |
| [World lamp response](features/night-lighting/world-lamp-response.md): terrain and rigs follow lamp edits | Released in 2.5.6 |
| [Every-story light](features/night-lighting/level-light-share.md): lamps on any floor light the ground and the rooms they reach; walls and balcony floors block lamp light outdoors | Released (walls blocking light outdoors: 2.7.0); Released in 2.12.1: update corrections; Released in 2.12.3: balcony and upper floors block lamps below them; Released in 2.13.0: light through openings, doors block light, ground lamp shade, shadow softness; Released in 2.14.0: the lamp's own walls through floor openings, hard wall tops, floor-level sills, steady indoor light; Released in 2.15.3: *Indoor light adds to the ground*, real window sizes with the wall's depth and glass, ceiling lamps of the story below kept out of the closed rooms above; Released in 2.15.5: missing outdoor lighting queue entries restored |
| [Light through stairwells](features/night-lighting/stairwells.md): lamps light the walls above a stairwell with a soft edge; bounce light and partial adaptation upstairs | Released in 2.14.0 |
| [Stairs on the directional maps](features/night-lighting/stairs.md): stairs take no light through walls, the floor's colour and the light of the floor above | Released in 2.14.0 |
| [Light through arches and doorways](features/night-lighting/arches.md): no colour step at arches, soft cone edges, indoor light independent of the camera | Released in 2.14.0; Released in 2.15.3: cheaper floor draws |
| [Real-time outdoor lamps](features/night-lighting/rt-outdoor-lamps.md) (Lamp lighting: Instant (GPU), the default): the active lot's outdoor lamps drawn on the graphics card with shadows | Released in 2.13.0; Released in 2.14.0: lamps not yet listed on a floor still reach it; Released in 2.15.3: shaders from the shader cache |
| [Quick outdoor lamp edits](features/night-lighting/outdoor-lamp-delta.md): every story's outside updated from each lamp's kept part, with Instant | Released in 2.13.0 |
| [Lamp drags](features/night-lighting/lamp-drags.md): room solves of a dragged lamp kept to its rooms; in Classic, relit after the release | Released in 2.13.0 |
| [Smooth light maps](features/night-lighting/room-map-smoothing.md): room lamp light without blocks, bands or rings | Released in 2.13.0; Released in 2.16.3: no dark squares beside bright texels |
| [Rooms at Night](features/night-lighting/unlit-rooms.md): ambient light and tint of unlit rooms | Released; Released in 2.13.0: *Background light* and *Lamp light* groups |
| [Walls](features/night-lighting/walls.md), [Floors](features/night-lighting/floors.md), [Roofs](features/night-lighting/roofs.md), [Roads](features/night-lighting/roads.md) | Released; Realistic lamp reach on walls: Released in 2.12.3; Released in 2.13.0: ground lamp shade on story-0 floors, soft edges by the lamp's size; Released in 2.15.3: gables and eaves lit from their drawn foot |
| [Objects and rigs](features/night-lighting/objects-and-rigs.md), [Fences](features/night-lighting/fences.md), [Foliage](features/night-lighting/foliage.md) | Released; Objects and Fences corrections: Released in 2.12.1; the walls' lamp law and the height gate on objects, fences and windows: Released in 2.12.3; Released in 2.13.0: buildings block lamp light on trees and objects, disabled lamps, foliage by day, colonnades on foundations, arcade columns and upper-story railing tops fixed; Released in 2.13.1: realistic lamp reach on trees and bushes; Released in 2.14.0: foliage lit per vertex and crown shade on lot ground; Released in 2.15.3: trees off the underground light rig, lamp hue kept, fences and gates at the ground's lamp scale, area lights lit on the side they face |
| [Daytime bloom and wall bloom fixes](features/night-lighting/day-bloom-fixes.md): the game's daylight look and bloom kept (idavidveiga) | Released in 2.7.0 |
| Light detail (Lighting option, in [Night Lighting](features/night-lighting/README.md#settings)): sharper lamp light on walls and floors | Experimental (released in 2.7.0) |

### Image

| Feature | Status |
|---|---|
| [Ambient Occlusion](features/ambient-occlusion.md): contact shade where surfaces meet | Released in 2.1.0; simplified controls in 2.11.1; one colour copy shared with the other screen passes in 2.15.3 |
| [Sim Occlusion](features/sim-occlusion.md): separate shade controls for Sims and hair | Released in 2.6.0 |
| [Reflections](features/reflections.md): water reflections and lamp glow | Released |
| [Picture filters](features/picture-filters.md): colour and image controls, 26 stackable filters, LUT files | Released (Filters tab: 2.7.0) |
| [Edge Smoothing](features/edge-smoothing.md): SMAA and FXAA | Released |
| [Depth Blur](features/depth-blur.md): depth of field | Released |
| [Banding Fix](features/banding-fix.md): dither against colour banding | Experimental (released in 2.2.0); Released in 2.16.0: High-precision scene on by default, redesigned card |

### Performance

| Feature | Status |
|---|---|
| [Performance](features/performance/README.md): fifteen switches against stutters (file lookups, lot lighting, texture and cache compression, object lookups, scene scheduling, memory, scripts) | Released; Released in 2.13.0: the Lamp lighting card (Instant (GPU) or Classic) and cheaper D3D9 dispatch; Released in 2.15.3: paced room solving, faster object lookups and file lists, an earlier start, cheaper Night Lighting draws |
| [Faster room lighting](features/performance/room-light-queue.md) with *Lamp edits all at once* (was *Lamp switches all at once*): the lamp's own story at once, the other stories together in a single frame | Released (lamp switches all at once: Experimental, 2.7.0); Released in 2.12.3: every lamp edit, own story first; Released in 2.13.0: Classic shows all floors together; Released in 2.14.0: automatic lamps first, step budget after loads; Released in 2.17.0: a lamp edit relights only its own lot |
| [Room to save](features/performance/room-to-save.md), [Lighter window updates](features/performance/lighter-window-updates.md), [Faster scripts](features/performance/faster-scripts.md) | Experimental (released in 2.7.0); Room to save: Released in 2.14.0: earlier buffer release and compatibility with sims3-modernization; Released in 2.17.0: one-time question card |
| [Faster memory handling](features/performance/fast-memory.md): allocator spin and background release | Released in 2.4.0; Released in 2.14.0: allocator trim and top-down large blocks |
| [Lot Streaming](features/performance/lot-streaming.md): lot detail farther away and smoother lot streaming (idavidveiga), off by default | Experimental (released in 2.7.0); Released in 2.16.2: shared lot build time, detailed-lot hysteresis, throttle threshold follow |

### Reports and diagnostics

| Feature | Status |
|---|---|
| [Report a problem](features/bug-reports.md): sessions, captures and notes for bug reports | Released in 2.5.0 |
| [Developer mode](features/developer-mode.md): optional diagnostic tools in the unified build | Released in 2.5.5 |
| [Frame Profiler](features/frame-profiler.md): per-service and per-hook timing, hitches | Developer mode |
| [Light Probe](features/dev-tools/light-probe.md), [Light Diag](features/dev-tools/light-diag.md), [Recorder](features/dev-tools/recorder.md) | Released (player captures) |
| [Frame Capture](features/dev-tools/frame-capture.md), [Census](features/dev-tools/census.md) (with the exterior surface capture, 2.12.3), [Tester tab](features/dev-tools/tester.md) (since 2.13.0) | Developer mode |

### Removed

| Feature | Notes |
|---|---|
| [Display fluency](features/display-fluency.md), [Presentation](features/presentation.md), [Lot Map Probe](features/dev-tools/lot-map-probe.md) | Never published or combined-build only |
| HDR output, Native HDR, Smooth Streaming, Script GC Scheduler, Service Frame Budget, earlier AO | See [removed-features.md](removed-features.md) |

## Engine reference (TS3W.exe)

| Document | Covers |
|---|---|
| [main-loop-and-services.md](engine/main-loop-and-services.md) | Main loop, services, jobs, resources, thread model |
| [lot-loading-and-streaming.md](engine/lot-loading-and-streaming.md) | Lot loading, LOD, world streaming, loaded-world gate |
| [terrain-and-light-bake.md](engine/terrain-and-light-bake.md) | Terrain chunks and the light bake |
| [room-light-maps.md](engine/room-light-maps.md) | Room light maps and the lot light solve |
| [light-objects-and-rigs.md](engine/light-objects-and-rigs.md) | Light objects, light manager, object rigs |
| [shaders.md](engine/shaders.md) | Shader packages, families, constants, shader identification |
| [camera-and-map-view.md](engine/camera-and-map-view.md) | Camera, projection, map view |
| [mono-gc.md](engine/mono-gc.md) | Mono and Boehm GC, simulation thread |
| [timers-and-sleeps.md](engine/timers-and-sleeps.md) | Clock, sleeps, frame limiter |
| [game-versions.md](engine/game-versions.md) | Game builds and the runtime signature table |

## History

Project-wide history: [architecture](history/architecture.md), [workflow](history/workflow.md), [UI](history/ui.md),
[lighting changes since 0.1.0](history/changes-since-0.1.0.md). Feature history is linked from each feature page.

## Primary sources

- The code in this repository; the frozen combined build (`combined-final`, commit 45e36e2) for explicitly historical
  sections.
- Private research notes (not published): a chronological lighting notebook (later entries supersede earlier ones),
  the per-pixel lighting plan ("PASSO3"), the night-lighting roadmap and the standalone split plan. Pages cite them
  only as background; every fact used by the code is restated in these docs.
- Static reverse engineering of `TS3W.exe`: a Ghidra decompile and disassembly kept with the private research notes.

## Presets and recovery

- [Presets](features/presets.md): saved settings, portable ZIP packages with a LUT and keyboard shortcuts (Released in 2.12.2).
- [Graphics recovery](features/graphics-recovery.md): restart image-effect resources without resetting settings.
- [Steam Deck and Proton](features/steam-deck.md): Proton, Steam Deck and DXVK detection, built-in shaders, installing on Linux.
- [2.14.0](releases/2.14.0.md): Released in 2.14.0 (pending publication); light through stairwells, stairs and arches, trees lit leaf by leaf, steady moonlight.
- [2.13.0](releases/2.13.0.md): Released in 2.13.0; Instant lamp lighting and lamp light through doors, windows and arches.
- [2.12.3](releases/2.12.3.md): Released in 2.12.3; realistic lamp reach, balconies blocking lamps below and faster lamp edits.
- [2.12.2](releases/2.12.2.md): Released in 2.12.2; keyboard shortcuts for presets.
- [2.12.1](releases/2.12.1.md): Released in 2.12.1; outdoor lighting corrections.
- [2.12.0](releases/2.12.0.md): LUTs, presets and graphics improvements.

- [Public memory budget experiment](features/memory-budget.md)
