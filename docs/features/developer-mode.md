# Developer mode

Apex Radiance ships as one ASI. Developer mode is an optional setting that, after a restart, adds the Developer page,
the developer-only tools (Frame Capture, the frame profiler, census and false colour, light probe texture replacement,
debug views) and extra checks and log detail. It is off by default; normal play never runs developer instruments. The
Report a problem tools stay available to every player in both modes.

## Status

| | |
|---|---|
| Availability | Released in 2.5.5 |
| Default | Off (`[ui] developer_mode = false`) |
| Menu | Settings > Menu > Settings and maintenance > Enable developer mode. Developer page in the sidebar (System group) while active |
| Configuration | `[ui] developer_mode` and the `[developer]` table in `ApexRadiance.toml`; optional Developer part in profiles |
| Source | [`build_flavor.h`](../../build_flavor.h), [`apex_config.cpp`](../../apex_config.cpp) (`LoadDeveloperMode`, `ApplyDeveloperPreferences`), [`apex_gui.cpp`](../../apex_gui.cpp) (`DeveloperModeRow`, `DeveloperConfirmation`, `DeveloperPage`) |

## The problem

Investigating lighting and performance needs instruments that cost frame time, change the image temporarily or write
local paths into files. Players should never pay for them or meet them by accident, and maintaining a separate developer
build doubled the release work and let the two drift apart.

## How Apex Radiance solves it

All code ships in `Release\ApexRadiance.asi`; the legacy `ApexPublic` build property has no effect. At startup
`ApexConfig::LoadDeveloperMode()` reads `[ui] developer_mode` before `PatchManager::CreateAll()` constructs any feature,
and stores the result in the atomic `kPublicBuild` (legacy name: true means developer mode is off). Every developer-only
path checks that flag. Changing the setting takes effect only after restarting the game, in both directions, so no
feature changes mode while running. The log records `[Main] Unified build: normal mode` or
`[Main] Unified build: developer mode`, and crash reports name the mode.

## Settings

| Control | TOML | Default | Notes |
|---|---|---|---|
| Enable developer mode | `[ui] developer_mode` | false | Turning it on opens a confirmation; turning it off saves at once. Restart required both ways |
| Developer part of a profile | `[developer]` in the profile | Unchecked | See Profiles below |

Enabling opens the confirmation "Enable developer mode?" ("Use these tools only when you need to investigate a
problem") with three notes: diagnostic views can temporarily change the image and measurements can reduce performance;
captures and reports may contain settings, local paths and session details, and nothing is sent automatically; restart
the game after confirming, and measurements and recordings do not start when a profile is loaded. Buttons: Cancel on
the left, "Enable developer mode" (Wrench icon, primary) on the right. Cancel leaves the setting unchanged. While the
requested mode differs from the running one, the row shows "Restart the game to apply the developer mode change". The
About version line reads "Version X - Developer mode" while active.

### Profiles

Development is profile part bit 8 (`kPartDeveloper = 256`; earlier bits unchanged, bit 4 unused). Its save checkbox is
shown only when developer mode is requested or active. A profile that contains it shows it when choosing what to apply,
even in normal mode; it starts unchecked. Applying a Developer part that requests activation opens the same
confirmation before any part is applied; Cancel applies nothing. Activation never follows a profile without this part.

The `[developer]` section holds:

| Key | Content |
|---|---|
| `enabled` | The requested mode |
| `controls` | `DeveloperSettings::Capture()`: tables `fast_dxt`, `fast_refpack`, `fast_cas`, `resource_cache`, `object_index`, `scene_budget`, `lot_lighting_motion` (verification frequencies, texture workers, scene-node budgets, wall-shading wait), plus `keep_room_light`, `story_samples`, `false_color` |
| `patches` | Each feature's preferences without its `enabled` flag (Frame Capture excluded) |
| `frame_profiler` | Profiler tuning and sampling preferences, always with `enabled = false` |

Recordings, frame captures, running profilers, one-shot census or rebuild operations, temporary verify-all timers and
accumulated measurements are actions or session results; profiles never restore them. A diagnostic view may be
restored in developer mode, which the confirmation warns about.

## Compatibility and interactions

Gated by developer mode (normal mode skips or ignores them):

| Area | Normal-mode behaviour |
|---|---|
| Developer page | Hidden from the sidebar; a stored Developer page selection falls back to Overview |
| Frame Capture | `Install` fails with "Enable developer mode and restart the game first"; `[patches.FrameCapture]` is not loaded; its shortcut is skipped |
| Frame profiler | Cannot start; `[qol.frame_profiler]` is kept but not rewritten |
| Address-space monitor | Not started (`AddressSpace::Start`) |
| Translation missing-text collection | Off (`ui/i18n.cpp`) |
| Edge Smoothing, Depth Blur and other debug views | Ignored even when an old configuration keeps them on |
| Census, false colour, refused-shader dump | Pass-through ([census](dev-tools/census.md)) |
| Hook timing and per-feature counters | Not collected (`framework/d3d9_hooks.cpp` and feature files) |
| `[developer]` preferences | `ApplyDeveloperPreferences` returns at once; an imported section is kept and written back unchanged |
| Lighting modules' detailed log lines | Written only while a recording runs (`Recorder::Verbose()`) |

## Limitations

- A restart is required to change mode.
- Developer preferences imported in normal mode are stored and applied only after restarting in developer mode.

## Technical reference

### Startup order (`apex_main.cpp`)

`ApexConfig::LoadDeveloperMode()` -> `PatchManager::Get().CreateAll()` -> `EnsureMigrated()` -> `LoadSettings()` ->
`AddressSpace::Start()` (developer mode only) -> log line. `LoadFeatures` applies `[developer]` after the feature tables
and defaults.

### Persistence (`apex_config.cpp`)

- `LoadSettings` keeps `[developer]` as `g_importedDeveloper` and always loads the profiler with `enabled = false`.
- Saving in developer mode writes `[developer] controls`; in normal mode it writes back the imported table untouched.
- `CaptureFeatureState` (profiles) writes `enabled`, and in developer mode `controls`, `patches` and `frame_profiler`.
- `ApplyFeatureState` applies a `[developer]` table only when it deactivates or developer mode is already confirmed.

### Developer page

Five tabs. The Developer page shows the existing game diagnostics only, without example values.

| Tab | Cards |
|---|---|
| Lighting | Collect lighting evidence (Save light diagnostics, Record story light samples, False colour, Census); Compare lighting paths (keep room light, soft lot edges, GPU smoothing, Compare GPU vs CPU, water highlights); Refresh lighting; Inspect lighting state, with advanced groups Surface and provider state, Rebuild events and terrain tests, Light probe textures, Individual options (for tests), Technical reference from the current code |
| Performance | Frame times and stutters (frame profiler); Shader preparation; File searches and remembered answers; Find objects faster; Lighting while the camera moves; Wall shading; Objects spread across frames; Texture compression and processor cores; Compressed game data |
| Captures | Capture session; Save a capture; Capture two drawn frames ([Frame Capture](dev-tools/frame-capture.md)); Your captures |
| Visual effects | Edge smoothing, Depth blur, Ambient shadows, Gradient correction coverage, Image adjustments (debug views and readouts) |
| Translations | Review translations (language, missing count, Clear, Write the list to the log); Missing text (filter); Placeholder checks |

Developer switches are excluded from the menu's undo history (`SetChangeReporting(false)`).

## Rejected approaches

- A separate developer build (`ApexPublic`, `S3SS_PUBLIC`)
  ([history](../history/developer-mode.md#2026-10-02-one-unified-build)).

## See also

- [Validation](../validation/developer-mode.md)
- [History](../history/developer-mode.md)
- [Menu reference](../ui.md), [Report a problem](bug-reports.md), [dev tools](dev-tools/light-probe.md)
