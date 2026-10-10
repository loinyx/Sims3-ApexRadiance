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
| [Night Lighting](features/night-lighting/README.md): lamps that really light the world at night | Released |
| [Lot light pass](features/night-lighting/lot-light-pass.md): lot and street lamps on lot grass | Released |
| [World atlas and smoothed maps](features/night-lighting/world-atlas-and-smoothed-maps.md): smooth terrain light maps | Released |
| [Terrain relight](features/night-lighting/terrain-relight.md): rebuilding terrain lighting at dusk and after edits | Released |
| [World lamp response](features/night-lighting/world-lamp-response.md): terrain and rigs follow lamp edits | Released in 2.5.6 |
| [Every-story light](features/night-lighting/level-light-share.md): lamps on any floor light the ground and the rooms they reach; walls block lamp light outdoors | Released (walls blocking light outdoors: 2.7.0) |
| [Rooms at Night](features/night-lighting/unlit-rooms.md): ambient light and tint of unlit rooms | Released |
| [Walls](features/night-lighting/walls.md), [Floors](features/night-lighting/floors.md), [Roofs](features/night-lighting/roofs.md), [Roads](features/night-lighting/roads.md) | Released |
| [Objects and rigs](features/night-lighting/objects-and-rigs.md), [Fences](features/night-lighting/fences.md), [Foliage](features/night-lighting/foliage.md) | Released |
| [Daytime bloom and wall bloom fixes](features/night-lighting/day-bloom-fixes.md): the game's daylight look and bloom kept (idavidveiga) | Released in 2.7.0 |
| Light detail (Lighting option, in [Night Lighting](features/night-lighting/README.md#settings)): sharper lamp light on walls and floors | Experimental (released in 2.7.0) |

### Image

| Feature | Status |
|---|---|
| [Ambient Occlusion](features/ambient-occlusion.md): contact shade where surfaces meet | Released in 2.1.0; simplified controls in 2.11.1 |
| [Sim Occlusion](features/sim-occlusion.md): separate shade controls for Sims and hair | Released in 2.6.0 |
| [Reflections](features/reflections.md): water reflections and lamp glow | Released |
| [Picture filters](features/picture-filters.md): colour and image controls, 26 stackable filters, LUT files | Released (Filters tab: 2.7.0) |
| [Edge Smoothing](features/edge-smoothing.md): SMAA and FXAA | Released |
| [Depth Blur](features/depth-blur.md): depth of field | Released |
| [Banding Fix](features/banding-fix.md): dither against colour banding | Experimental (released in 2.2.0) |

### Performance

| Feature | Status |
|---|---|
| [Performance](features/performance/README.md): fifteen switches against stutters (file lookups, lot lighting, texture and cache compression, object lookups, scene scheduling, memory, scripts) | Released |
| [Faster room lighting](features/performance/room-light-queue.md) with *Lamp switches all at once*: one lamp or a whole house changes in a single frame | Released (lamp switches all at once: Experimental, 2.7.0) |
| [Room to save](features/performance/room-to-save.md), [Lighter window updates](features/performance/lighter-window-updates.md), [Faster scripts](features/performance/faster-scripts.md) | Experimental (released in 2.7.0) |
| [Lot Streaming](features/performance/lot-streaming.md): lot detail farther away and smoother lot streaming (idavidveiga), off by default | Experimental (released in 2.7.0) |

### Reports and diagnostics

| Feature | Status |
|---|---|
| [Report a problem](features/bug-reports.md): sessions, captures and notes for bug reports | Released in 2.5.0 |
| [Developer mode](features/developer-mode.md): optional diagnostic tools in the unified build | Released in 2.5.5 |
| [Frame Profiler](features/frame-profiler.md): per-service and per-hook timing, hitches | Developer mode |
| [Light Probe](features/dev-tools/light-probe.md), [Light Diag](features/dev-tools/light-diag.md), [Recorder](features/dev-tools/recorder.md) | Released (player captures) |
| [Frame Capture](features/dev-tools/frame-capture.md), [Census](features/dev-tools/census.md) | Developer mode |

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
- `%USERPROFILE%\Desktop\S3SS-dev\NOTAS-ILUMINACAO.md`: chronological lighting notebook (Portuguese); later entries
  supersede earlier ones. Also `PASSO3-PLANO.md`, `ROADMAP-NIGHT-REMAKE.md` and `PLANO-SEPARACAO.md`.
- Static reverse engineering of `TS3W.exe`: `S3SS-dev\re\out` (Ghidra decompile).

## Presets and recovery

- [Presets](features/presets.md): saved settings and portable ZIP packages with a LUT.
- [Graphics recovery](features/graphics-recovery.md): restart image-effect resources without resetting settings.
- [2.12.0](releases/2.12.0.md): LUTs, presets and graphics improvements.
