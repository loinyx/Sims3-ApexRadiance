> Published 2.5.6: Optimize rendering defaults on unless explicitly saved off; world-lamp colour response reconciles terrain and native rigs. Published 2.5.5 introduced the unified ASI, optional developer mode, restored capture layout and spatial-only Edge Smoothing. Apex window/pacing controls are removed. Older RC sections below are history, not the current release.

# Apex Radiance: developer documentation

Apex Radiance is a native ASI mod for The Sims 3, loaded by Ultimate ASI Loader. It hooks Direct3D 9 and patches game code in memory. Supported executable details are in [engine/game-versions.md](engine/game-versions.md); compatibility is documented in the repository README.
Its files live in `Documents\Electronic Arts\The Sims 3\Apex Radiance\` (`ApexRadiance.toml`, `ApexRadiance_LOG.txt`).

These documents are written for future maintainers, especially Claude sessions. With them you should not need to
re-derive anything from the long Portuguese notes. Start with [../CLAUDE.md](../CLAUDE.md), then
[architecture.md](architecture.md) and [workflow.md](workflow.md).

**Code baseline.** Current behavior is defined by this repository. Older reverse-engineering sections refer to the frozen combined build (`combined-final`, commit 45e36e2); treat dated findings as history, not current feature availability. Game addresses are for Steam `TS3W.exe` 1.67.2, image base 0x00400000, unless stated otherwise. Unverified findings are not runtime confirmations.

**Scope.** HDR output and Native HDR remain removed. The earlier Ambient Occlusion implementation is historical; current standalone GTAO is documented in [features/ambient-occlusion.md](features/ambient-occlusion.md).

## Contents

### Current release (2.5.6)

The current development PR removes the optional Optimize rendering switch and restores its original rendering paths;
this does not change the already published 2.5.6 build.

- [Performance](features/performance.md): rendering paths, lookup caches, lighting budgets, compression, scene-node scheduling, object indexing and validation limits.
- [World lamp response](features/night-lighting/world-lamp-response.md): captured evidence, terrain/rig reconciliation, failed approaches and player acceptance.
- [Roads](features/night-lighting/roads.md): alpha-blended sidewalk shader recognition.
- [Frame profiler](features/frame-profiler.md): optional timing of draw and state callbacks.
- [Developer mode](features/developer-mode.md) and [reports](features/bug-reports.md): published unified build and optional post-save descriptions.

### General
| Document | What it covers |
|---|---|
| [features/bug-reports.md](features/bug-reports.md) | Published since 2.5.5: restored session/capture/list/help page, optional notes after saving, storage guards and retry. Superseded redesign notes remain historical. |
| [architecture.md](architecture.md) | Loading, D3D9 device hooks, hook registry (priorities, Skip), extra hooks, render callbacks, post-scene trigger chain, INTZ depth share, patch system and TOML settings, logger, build flavours, per-frame flow, threads, the standalone split |
| [workflow.md](workflow.md) | Build commands, install, the user's standing rules, diagnosis with F7/F8/profiler, release process |
| [removed-features.md](removed-features.md) | Removed implementations and their reverse-engineering findings; current GTAO is documented separately |

### Engine reverse engineering (TS3W.exe)
| Document | What it covers |
|---|---|
| [engine/main-loop-and-services.md](engine/main-loop-and-services.md) | Main loop 0xECA960, ServiceManager, JobManager, ResourceSystem, SimService, TextureCompositor, thread model |
| [engine/lot-loading-and-streaming.md](engine/lot-loading-and-streaming.md) | Lot loading, LOD, world streaming, package IO |
| [engine/terrain-and-light-bake.md](engine/terrain-and-light-bake.md) | Terrain chunks, the light bake FUN_00C292B0, story gate 0xC294D9, flags and countdowns, world atlas |
| [engine/room-light-maps.md](engine/room-light-maps.md) | Room light maps and the lot light solve |
| [engine/light-objects-and-rigs.md](engine/light-objects-and-rigs.md) | Light object layout and classes, light manager, object rigs |
| [engine/shaders.md](engine/shaders.md) | Shaders_Win32.precomp, shader families, constants, LightingTweaks tanh, how Apex identifies shaders |
| [engine/camera-and-map-view.md](engine/camera-and-map-view.md) | Camera, projection, map view 0x73E060 |
| [engine/mono-gc.md](engine/mono-gc.md) | Mono / Boehm GC and the simulation thread |
| [engine/timers-and-sleeps.md](engine/timers-and-sleeps.md) | Clock, sleeps, frame limiter, Smooth Patch sites |
| [engine/game-versions.md](engine/game-versions.md) | Game builds (Steam 1.67.2, EA app 1.69.47), the encrypted EA .text, the signature table of every Night Lights address and how it is resolved at run time |

### Features
| Document | Feature |
|---|---|
| [features/night-lighting/README.md](features/night-lighting/README.md) | **Night Lighting** (`[patches.NightTerrainRelight]`): overview, all settings, module map |
| [features/night-lighting/lot-light-pass.md](features/night-lighting/lot-light-pass.md) | Lot light pass: max(lot map, atlas) on the ground |
| [features/night-lighting/world-atlas-and-smoothed-maps.md](features/night-lighting/world-atlas-and-smoothed-maps.md) | World atlas and smoothed light maps |
| [features/night-lighting/terrain-relight.md](features/night-lighting/terrain-relight.md) | Story gate patch, dusk rebuild, automatic terrain relight reconciliation |
| [features/night-lighting/level-light-share.md](features/night-lighting/level-light-share.md) | Outdoor lamps on every floor, lamp-floor wall test |
| [features/night-lighting/walls.md](features/night-lighting/walls.md) | Exterior walls |
| [features/night-lighting/floors.md](features/night-lighting/floors.md) | Floors |
| [features/night-lighting/roads.md](features/night-lighting/roads.md) | Roads and sidewalks |
| [features/night-lighting/roofs.md](features/night-lighting/roofs.md) | Roofs and roof snow |
| [features/night-lighting/water.md](features/night-lighting/water.md) | Lakes, ponds, ocean, pools |
| [features/night-lighting/foliage.md](features/night-lighting/foliage.md) | Bushes, trees, plants |
| [features/night-lighting/objects-and-rigs.md](features/night-lighting/objects-and-rigs.md) | Objects and rigs: per-pixel lamps, bake-matched falloff, max rule |
| [features/night-lighting/fences.md](features/night-lighting/fences.md) | Fences and railings, per-pixel |
| [features/night-lighting/snow.md](features/night-lighting/snow.md) | Snow on ground, floors, sills, stairs, fence tops |
| [features/night-lighting/lamp-colour.md](features/night-lighting/lamp-colour.md) | Lamp colour |
| [features/reflections.md](features/reflections.md) | Reflections |
| [features/picture-filters.md](features/picture-filters.md) | Picture filters (SDR colour and image controls) |
| [features/edge-smoothing.md](features/edge-smoothing.md) | Edge Smoothing: SMAA 1x and FXAA |
| [features/ambient-occlusion.md](features/ambient-occlusion.md) | Ambient Occlusion (GTAO) |
| [features/banding-fix.md](features/banding-fix.md) | Banding Fix: dither of the scene pixel shaders |
| [features/depth-blur.md](features/depth-blur.md) | Depth Blur (off in map view) |
| [features/frame-profiler.md](features/frame-profiler.md) | Frame Profiler (developer mode only): sampling, per-service and per-hook timing, hitches |
| [features/performance.md](features/performance.md) | Rendering paths, lookup caches, lighting budgets, compression, scene-node scheduling, object indexing and offline validation |
| [changes-since-0.1.0.md](changes-since-0.1.0.md) | Lighting changes after v0.1.0 and the re-add order (fences first) |

### Developer tools (developer mode only)
| Document | Tool |
|---|---|
| [features/dev-tools/light-probe.md](features/dev-tools/light-probe.md) | Light Probe, Ctrl+Shift+F7 |
| [features/dev-tools/light-diag.md](features/dev-tools/light-diag.md) | Light Diag, Ctrl+Shift+F8 |
| [features/dev-tools/frame-capture.md](features/dev-tools/frame-capture.md) | Frame Capture, Ctrl+Shift+F9 |
| [features/dev-tools/lot-map-probe.md](features/dev-tools/lot-map-probe.md) | Lot Map Probe |
| [features/dev-tools/census.md](features/dev-tools/census.md) | Shader census, false colour, offline coverage tests |

## Primary sources behind these docs

- Code: this repository; the frozen combined tree for explicitly historical sections.
- `%USERPROFILE%\Desktop\S3SS-dev\NOTAS-ILUMINACAO.md`: chronological lab notebook in Portuguese, every capture and
  root cause. Later entries supersede earlier ones.
- `PASSO3-PLANO.md` (per-pixel lamp plan and critique), `ROADMAP-NIGHT-REMAKE.md`, `PLANO-SEPARACAO.md` (standalone split).
- Static RE of TS3W.exe: `re\out` (Ghidra decompile) and the session scratchpad `engine_map\` (call graph, strings,
  service tables, profiler targets).
- `README.md` of the combined build (user-facing feature list).

When these docs and the code disagree, the code is right; fix the doc.

## Historical notes

Release-specific investigations and superseded candidates remain in their feature guides, where their evidence and limitations are useful: [lighting](features/night-lighting/terrain-relight.md), [reports](features/bug-reports.md), and [Edge Smoothing](features/edge-smoothing.md).

- [Game anti-aliasing compatibility](ui-aa-compatibility.md): affected effects and inline help.
