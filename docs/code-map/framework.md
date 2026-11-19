# Code map: Framework, configuration and menu

The shell of the mod: start-up and the feature registry, the D3D9 device hooks, game-code patching and the address table,
`ApexRadiance.toml` with presets and profiles, the ImGui menu and translations, the log and crash report, the
loaded-world gate, the shader precompile cache and coexistence with Sims3SettingsSetter (S3SS) and Sims3Performance
(S3PP). The narrative explanation is [architecture.md](../architecture.md) (threads: section 10; adding a feature:
section 14). Back to the [Code map](../code-map.md).

Build facts used everywhere: `build_flavor.h:kPublicBuild` is an atomic whose legacy name means "developer mode off"; it is
written once by `apex_config.cpp:LoadDeveloperMode` on the init thread before features are created. `build_flavor.h:kDevToolsBuild`
is false in the players' flavour (`APEX_NO_DEV_TOOLS`). A new source file is one `ClCompile` line in `ApexRadiance.vcxproj`
(no `.filters` file; framework, features, patches and shaders are include directories).

## Start-up and feature registry

Loads only into TS3W.exe / TS3.exe, waits until the game has presented a frame plus one second, installs the enabled
features, then pumps every 10 ms. Docs: [architecture](../architecture.md), [developer mode](../features/developer-mode.md).

- **Files:** `apex_main.cpp` (DllMain, init thread, pump), `framework/patch_base.h` / `framework/patch_base.cpp` (`ApexPatch`,
  setting kinds, `PatchManager`, `APEX_REGISTER_FEATURE`), `framework/module_lifetime.h` (pin, no live unload),
  `framework/hook_guard.h` (exception nets).
- **Entry points:** `apex_main.cpp:DllMain` (process check, `S3SSDetect::AcquireInstanceMutex`, `ApexD3D::InstallFromDllMain`,
  init thread); `apex_main.cpp:InitBody` (log, game version, `ShaderCache::Start`, `PatchManager::CreateAll`,
  `ApexConfig::EnsureMigrated`, `ApexConfig::LoadSettings`, `apex_main.cpp:WaitForSettle`, `GameAddr::Resolve`,
  `ApexConfig::LoadFeatures`, `CrashReport::Install`, then the pump loop); `framework/patch_base.cpp:PatchRegistration`
  (static initialisers); `framework/patch_base.cpp:CreateAll`, `framework/patch_base.cpp:EnableDefaults`,
  `framework/patch_base.cpp:UpdateAll` (pump); `framework/patch_base.cpp:Update` (default: reinstall 2 s after a setting
  change); `framework/patch_base.cpp:ApplyTableLive` (presets, undo; render thread); `framework/patch_base.cpp:Fail`
  (menu error + ERROR log); `apex_gui.cpp:SetPatch` (menu switch -> Install / Uninstall).
- **Threads:** loader thread: DllMain; init / pump thread: startup `Install()`, `Update()`, autosave; render thread: menu
  switches, `ApplyTableLive`.
- **State:** `framework/patch_base.h:PatchManager` owns `patches_` (created on init, read everywhere); `ApexPatch::isEnabled`
  (written by Install / Uninstall on init, render or pump); `pendingReinstall` written by `NotifySettingChanged` (render),
  read by `Update` (pump); `apex_gui.cpp:g_startup` (Loading / Running / RefusedOldBuild) gates the pump and the menu.
- **Settings:** none of its own.
- **Tests:** none offline (UNVERIFIED that none exists).
- **Invariants:** the module is pinned and `DLL_PROCESS_DETACH` does nothing; game-code features install only after the
  settle wait so official S3SS has loaded its patches (`apex_main.cpp:WaitForSettle`); the name passed to `ApexPatch` is the
  table `[patches.<Name>]` and never changes; with the old combined build loaded no feature is installed
  (`apex_main.cpp:InitBody`); every pump step runs inside `HookGuard::Try`.
- **Fragile:** the default `framework/patch_base.cpp:Update` reinstalls on the pump thread, so a feature that frees D3D
  objects or rewrites render-thread code overrides it ([architecture.md](../architecture.md) 10.4); the `"FrameCapture"`
  literal is special-cased in `framework/patch_base.cpp:LoadFromToml` and `apex_config.cpp:ApplyDeveloperPreferences`;
  `framework/patch_base.cpp:UninstallAll` and the other FreeLibrary-only shutdown paths have no caller.

## D3D9 hook registry and dispatch

Gets onto the game's device next to S3SS and dispatches 15 device methods to named callbacks in priority order.
Docs: [architecture](../architecture.md) sections 2-4, [graphics recovery](../features/graphics-recovery.md).

- **Files:** `framework/d3d9_bootstrap.cpp` (Direct3DCreate9 -> CreateDevice -> EndScene / Reset), `framework/d3d9_hooks.cpp`
  (registry, lock-free draw and state chains, `CallOriginal*`, bound shader and texture tracking),
  `framework/d3d9_extra_hooks.cpp` (Clear, StretchRect, depth-stencil, *UP draws; render-target hooks on demand),
  `framework/render_callbacks.h` (end-of-scene and Reset lists), `framework/hook_chain.cpp` (who hooked a target first),
  `framework/scene_target.cpp` (High-precision scene, the one render-target substitution owner), `framework/scene_copy.cpp`
  (one shared back-buffer copy), `framework/platform.cpp` (Wine, Steam Deck, DXVK).
- **Entry points:** `framework/d3d9_bootstrap.cpp:Hooked_Direct3DCreate9`, `framework/d3d9_bootstrap.cpp:Hooked_CreateDevice`
  (first HAL device only), `framework/d3d9_bootstrap.cpp:Hooked_EndScene` (first call: `framework/d3d9_bootstrap.cpp:FrameInit`),
  `framework/d3d9_bootstrap.cpp:Hooked_Reset`, `framework/d3d9_bootstrap.cpp:OnPresent` (first Present: window subclass,
  S3SS rescan); `framework/d3d9_hooks.cpp:Install` (records the render thread); `D3D9Hooks::Register*` ->
  `framework/d3d9_hooks.cpp:Add`; `framework/d3d9_hooks.cpp:UnregisterAll`; dispatch `framework/d3d9_hooks.cpp:Run` /
  `framework/d3d9_hooks.cpp:RunList`; `framework/d3d9_hooks.cpp:ReclaimRetired` at Present.
- **Game hooks and addresses:** IDirect3D9 vtable slot 16; device slots 42 (EndScene) and 16 (Reset) in
  `framework/d3d9_bootstrap.cpp`; the 15 registry slots in the `Slot` enum of `framework/d3d9_hooks.cpp` (install refuses
  when two slots share one function).
- **Registrations:** Present "SceneTarget" at -3000 and 100000 (`framework/scene_target.cpp:Init`); Present "ApexCore" at
  -2000 (`framework/d3d9_bootstrap.cpp:FrameInit`); Present "TransitionWatch" Last (diagnostic builds). -1000 belongs to the
  profiler. Full table: [architecture.md](../architecture.md) 3.5.
- **Threads:** draw and state chains run lock-free only on the render thread (`framework/d3d9_hooks.cpp:g_renderTid`);
  Present, BeginScene, Create* and any off-thread dispatch run under `framework/d3d9_hooks.cpp:g_lock`.
- **State:** `framework/d3d9_hooks.cpp:g_rtSubstitute` (SceneTarget); `framework/d3d9_bootstrap.cpp:g_presentSeen` and
  `g_firstPresentTick` (read by the init thread's settle wait); `framework/memory_patch.cpp:Lock` (one Detours transaction
  per process).
- **Settings:** *High-precision scene* `[patches.SceneDither] cenaAltaPrecisao` = true (registered in `features/scene_dither.cpp`).
- **Tests:** `tools/scene_target_test/` covers `framework/scene_target_logic.h` and the resolve shader, run
  `tools/scene_target_test/run.ps1` (GPU optional, SKIP without a HAL device); dispatch itself is in-game only.
- **Invariants:** Skip / Block stop the chain and the device call; a callback that throws is switched off and the device
  call still happens once (`framework/d3d9_hooks.cpp:RunList`); never hold a lock a draw callback takes while calling
  `UnregisterAll` from another thread (`framework/d3d9_hooks.h`); per-frame render callbacks that throw are skipped for good,
  Reset lists keep running (`framework/render_callbacks.h:Fire`).
- **Rejected:** std::function callbacks and one exception frame per callback (dispatch cost); four fixed render-callback
  slots that dropped a fifth preReset user -> [history](../history/architecture.md).
- **Fragile:** device slot numbers are written in both `framework/d3d9_bootstrap.cpp` and `framework/d3d9_hooks.cpp`; the
  texture cache behind `D3D9Hooks::BoundTexture` switches itself off after 8 mismatches and needs
  `D3D9Hooks::ShaderStateChanged` after a state block.

## Game-code patching and the address table

Finds game addresses for the running build and writes byte-checked patches and layered hooks. Docs:
[game versions](../engine/game-versions.md), [architecture](../architecture.md) 4.5-4.6, [Night Lighting module map](../features/night-lighting/README.md).

- **Files:** `framework/game_version.cpp` (PE timestamp -> build), `framework/game_addresses.h` / `framework/game_addresses.cpp`
  (`GameAddr::Id`, Steam table `kInfo`, signatures `kTable`, feature groups `kGroups`), `framework/memory_patch.cpp`
  (`MemPatch`, `DetourBatch`, `GameAddress`), `framework/slot_chain.cpp` (vtable-slot layers), `framework/entry_chain.cpp`
  (function-entry layers with trampoline), `framework/call_chain.cpp` (CALL-site layers), `framework/conflict_guard.h` (stub).
- **Entry points:** `framework/game_version.cpp:DetectGameVersion`; `framework/game_addresses.cpp:Resolve` (init thread,
  after the settle wait, once); `framework/game_addresses.cpp:Get` (Steam: fixed address even before Resolve);
  `framework/game_addresses.cpp:GroupAvailable` (via `FeatureInfo::gameCodeGroup`);
  `framework/memory_patch.cpp:WriteBytes` / `framework/memory_patch.cpp:RestoreAll`;
  `framework/memory_patch.cpp:WriteCodeSuspended`; `SlotChain::Install`, `EntryChain::Install`, `CallChain::Install`.
- **Game hooks and addresses:** Steam 1.67.2 is `framework/game_version.cpp@0x52DEC247`; every id's Steam address is in
  `framework/game_addresses.cpp:kInfo`; chain sites are listed in each `kSiteInfo` table with expected prologue bytes or
  call targets.
- **Threads:** Resolve on the init thread (waits up to 15 s while EA .text still looks encrypted); chain Install / Remove
  under one mutex, `Next()` lock-free from any game thread.
- **State:** `framework/game_version.cpp:g_gameVersion`, `g_exeTimestamp`; `framework/game_addresses.cpp:g_found`,
  `g_final`, `g_resolved`; the `g_next` tables of the three chains.
- **Settings:** none.
- **Tests:** none offline; `tools/code_map_check.py` checks addresses named in these pages.
- **Invariants:** an address counts only when its signature matches once or all matches agree, otherwise the feature stays
  off with "Not available on ..." (`framework/game_addresses.h`); `kTable` lists every dependency before its users and
  `kInfo` follows the `Id` order (static_assert); chains never overwrite a slot, prologue or CALL another module changed,
  and removed layers keep their next pointer; hot multi-thread code is written only with the other threads suspended.
- **Rejected:** matching signatures in the file on disk (EA .text is encrypted there); running the Steam self-check scan in
  normal mode (about 0.9 s of start-up) -> [game versions](../engine/game-versions.md).
- **Fragile:** the Steam timestamp is compared directly instead of `GameAddr::IsFixed` in `framework/transition_watch.cpp:Start`,
  `framework/transition_calls.cpp:Start`, `features/address_space.cpp`, `features/lot_load_analysis.cpp:Supported`,
  `features/memory_guard.cpp`; Steam addresses are repeated in the comments of `framework/slot_chain.h`,
  `framework/entry_chain.h`, `framework/call_chain.h`; `ConflictGuard` has no callers besides an empty `Tick`.

## Configuration, presets and profiles

`ApexRadiance.toml` load and save, setting registration, debounced autosave, one-time migration, built-in presets and
profile files (TOML, or ZIP with a LUT). Docs: [presets](../features/presets.md), [validation](../validation/presets.md),
[history](../history/2026-10-08-presets-sharing.md).

- **Files:** `apex_config.cpp` (all of `ApexConfig`), `framework/patch_base.cpp` (`FloatOption`, `IntOption`, `BoolOption`,
  `EnumOption`, `NamedEnumOption`), `apex_presets.cpp` (Performance / Balanced / Quality TOML), `profile_package.h`
  (stored ZIP), `hotkeys.cpp` (shortcut presets), `key_combo.h`.
- **Entry points:** `apex_config.cpp:LoadDeveloperMode`, `apex_config.cpp:EnsureMigrated` -> `apex_config.cpp:DoMigrate`
  (after `CreateAll`: it needs the feature keys), `apex_config.cpp:LoadSettings`, `apex_config.cpp:LoadFeatures`,
  `apex_config.cpp:Save`, `apex_config.cpp:PumpAutosave` (pump, `kSaveDelay` after the last change),
  `apex_config.cpp:CaptureFeatureState` / `apex_config.cpp:ApplyFeatureState` (presets, undo, reset all),
  `apex_config.cpp:SaveProfile`, `apex_config.cpp:ReadProfileImport`, `apex_presets.cpp:Read`.
- **Threads:** load on init; save on pump (autosave) or render ("Save now"); file access under
  `apex_config.cpp:g_fileLock`, `apex_config.cpp:g_ui` under `g_uiLock`.
- **State:** `apex_config.cpp:g_ui`; `apex_config.cpp:g_saveRequested`; `apex_config.cpp:g_compareOff` (features the Compare
  shortcut switched off, saved as on).
- **Settings:** *Menu key* `[ui] toggle_key` = Ctrl+Shift+F11; *Text size* `[ui] font_scale` = 1.0 (0.5-3.0); *Language*
  `[ui] language` = "auto"; *Startup menu hint* `[ui] start_note` = true; *Enable developer mode* `[ui] developer_mode` =
  false (next start); *Shortcut preset* `[ui] hotkey_preset` = missing (F keys); per-action keys `[ui] compare_key`,
  `refresh_key`, `probe_key`, `diagnostics_key`, `recorder_key`, `frame_capture_key` = the preset's; *Search the settings*
  `[ui] search_key` = Ctrl+F; *Hide the menu while held* `[ui] peek_key` = Alt; *Take a filtered screenshot*
  `[ui] screenshot_key` = F8; *Save screenshots to* `[ui] screenshot_folder` = "game"; *Hide game UI in screenshots*
  `[ui] screenshot_hide_game_ui` = true; preset shortcuts `[ui.profile_shortcuts]` = none. Defaults: `apex_config.h:UiSettings`.
- **Tests:** `tools/profile_package_test/` (profile functions, ZIP, Presets page; see its README; needs none);
  `tools/developer_mode_check/` (profile parts; `prepare.py` then build `check.cpp`; needs none).
- **Invariants:** S3SS.toml is only read; saves are atomic and keep tables this build does not know
  (`framework/patch_base.cpp:SaveToToml`); a Compare in progress is never saved; the Frame Profiler never starts from a file or
  an import; profile part bit values are fixed and bit 4 is reserved (`apex_config.h:ProfilePart`); loaded values are
  clamped to their range.
- **Fragile:** `apex_config.cpp:FeaturePart` maps feature names by literal, so a renamed feature drops out of profiles;
  `kPresetKeys` order in `apex_config.cpp:Save` must match `Hotkeys::Preset`; profile and developer-mode tests copy functions
  out of `apex_config.cpp` by text anchors (`tools/profile_package_test/extract.py`, `tools/developer_mode_check/prepare.py`).

## Menu, UI and translations

The overlay menu with its own ImGui context, ini file and window-procedure subclass, pages of feature cards, shortcuts,
What's new and 21 languages. Docs: [ui](../ui.md), [validation](../validation/ui.md), [history](../history/ui.md),
[translating](../../i18n/TRANSLATING.md).

- **Files:** `framework/overlay.cpp` (ImGui, `ApexWndProc` input policy), `apex_gui.cpp` (`GuiClient`, `Page` enum, cards,
  welcome, presets, Report page), `hotkeys.cpp`, `ui/widgets.cpp` (Violet widgets; they translate their labels),
  `ui/violet_theme.cpp`, `ui/icons.cpp`, `ui/i18n.cpp`, `i18n/tr_menu.cpp` and the other `tr_*.cpp` (English, Portuguese,
  Spanish, French; the master key list), `i18n/lang_de.cpp` and the other `lang_<code>.cpp` (one language each),
  `i18n/keys.tsv` (generated), `apex_changelog.cpp`.
- **Entry points:** `framework/overlay.cpp:Init` (first EndScene), `framework/overlay.cpp:InstallWndProc` (first Present, after
  S3SS's subclass), `framework/overlay.cpp:ApexWndProc` -> `framework/overlay.cpp:Decide`, `framework/overlay.cpp:Frame`;
  `apex_gui.cpp:MainWindow` -> `apex_gui.cpp:DrawPage`; `apex_gui.cpp:FeatureCardWith` (switch + `RenderCustomUI` body);
  `apex_gui.cpp:UpdateMenuAvailability`; `apex_gui.cpp:RunShortcuts`; `hotkeys.cpp:OnKeyDown`; `ui/i18n.cpp:Tr`.
- **Threads:** drawing on the render thread inside EndScene; the window procedure on the window-pump thread (the render
  thread, inferred in [architecture.md](../architecture.md) 10.3); `apex_gui.cpp:g_menuAvailable` is the atomic the window
  thread reads.
- **State:** `framework/overlay.cpp:g_visible`; `apex_gui.cpp:g_page`; `apex_gui.cpp:g_menuAvailable`; `apex_gui.cpp:g_oldStandalone`.
- **Settings:** the `[ui]` keys above.
- **Tests:** `tools/i18n_check/` (table conflicts, placeholders, `##id`, keys.tsv freshness; build line in its header; needs
  none); `perl tools/i18n_keys.pl` regenerates keys.tsv; `perl tools/menu_audit.pl`; `tools/report_check/` (Report page,
  notices, startup gate); `tools/public_test_notice_test/`.
- **Invariants:** English text is the key and every shown text has an entry in an `i18n/tr_*.cpp` table; translations keep
  the `{}` placeholders and `##id` tail; new languages go before `Count` in `ui/i18n.h:Lang`; logs and the Developer page
  stay English; the menu uses `apex_radiance_imgui.ini`, never the imgui.ini in Game\Bin; input is eaten only when Apex's
  ImGui wants it or it is an Apex chord.
- **Rejected:** a keyboard map with preset tiles for shortcuts -> [history](../history/ui.md); a start note shown on load
  screens (now behind `WorldSession::InWorld`).
- **Fragile:** several tests cut code out of `apex_gui.cpp` and `ui/widgets.cpp` by text anchors (list in the extras);
  changelog lines live in both `apex_changelog.cpp` and `i18n/tr_menu.cpp`; a comment in `apex_gui.cpp:StateNotes` carries
  damaged UTF-8.

## Log, crash report, paths

ApexRadiance_LOG.txt with a writer thread, ApexRadiance_Crash.txt and `.dmp` on an unhandled exception, exception
nets in every hook, and the Documents folder layout. Docs: [architecture](../architecture.md) 6, [bug reports](../features/bug-reports.md).

- **Files:** `framework/apex_log.cpp`, `framework/crash_report.cpp`, `framework/hook_guard.cpp`, `framework/apex_paths.cpp`.
- **Entry points:** `apex_main.cpp:OpenLog` -> `framework/apex_log.cpp:Open`; `framework/apex_log.cpp:Write` (`LOG_*`);
  `framework/crash_report.cpp:Install` (after the game's own start-up), `framework/crash_report.cpp:Refresh` (pump, every
  second), `framework/crash_report.cpp:ThreadStart` (first line of every Apex thread), `framework/crash_report.cpp:Filter`;
  `framework/hook_guard.cpp:ReportPending` (each Present); `framework/apex_paths.cpp:Resolve`.
- **Threads:** log writer thread flushes every 500 ms; warnings and errors flush on the caller; the crash filter runs on the
  crashing thread without heap.
- **State:** `framework/crash_report.cpp:g_features` (feature line refreshed by the pump); HookGuard slots (no heap, logged at Present).
- **Invariants:** `ApexLog::Write` never throws; `LOG_DEBUG` is not built unless debug output is wanted; `HookGuard::Run`
  switches a call site off for good, `HookGuard::Try` retries.
- **Fragile:** `ApexLog::SetVerbose` has no caller, so debug lines reach the file only with a debugger attached;
  `HookGuard::Run` keeps one switch per lambda type, so pass a lambda, not a function pointer.

## Loaded-world gate and transitions

Decides when a world is really drawn (screen effects, menu, start note) and, in diagnostic builds, logs load phases and
scene gaps. Docs: [architecture](../architecture.md) 12, [lot loading](../engine/lot-loading-and-streaming.md),
[lot load analysis](../features/lot-load-analysis.md).

- **Files:** `features/world_session.h` (the gate), `framework/load_timing.cpp`, `framework/transition_watch.cpp`,
  `framework/transition_watch_native.h`, `framework/transition_calls.cpp`.
- **Entry points:** `features/world_session.h:InWorld` (Picture, PostScene, Ambient Occlusion, Depth Blur, Edge Smoothing,
  Night Lighting, overlay, menu); `features/world_session.h:LoaderDismissed`; `features/world_session.h:g_loadSettled` (set
  by `patches/depth_blur_patch.cpp:NightLive`); `LoadTiming::Note*` and `framework/load_timing.cpp:Pump`;
  `framework/transition_watch.cpp:Start` / `framework/transition_calls.cpp:Start`.
- **Game hooks and addresses:** world manager global `framework/game_addresses.cpp@0x011ECBC4` (+0x41 active, +0x1B4 mode,
  +0x258 skip-streaming); UI service getter `framework/game_addresses.cpp@0x0050AB70` (loading window id 0x95947678 through
  root vfunc +0xF4); lot renderer map `framework/transition_watch.cpp@0x011ECE58`; ScriptCore natives
  `framework/transition_calls.cpp@0x00778CC0`, 0x0078AE40, 0x00787060 (pattern plus expected Steam address). All reads are SEH-guarded.
- **Threads:** `InWorld` render thread only (function-local statics); load timing notes from any thread, logged on the pump.
- **Settings:** none; thresholds `kMinDepthWrites` 48, `kDrawnMs` 500, `kSparseMs` 2000 in `features/world_session.h`.
- **Tests:** `tools/loading_gate_test/` (gate with a mocked UI service and extracted guards; run `tools/loading_gate_test/run.ps1`;
  needs none); `tools/transition_watch_test/` (run `tools/transition_watch_test/run.ps1`; needs none).
- **Invariants:** never latch across loads; a build without the UI getter fails open so effects are never off for good;
  transition watch and calls compile to empty stubs outside diagnostic or development builds.
- **Rejected:** 150 depth writes (never opened in close views); 1000 ms drawn time (effects came late); Night Lighting's
  "load settled" alone (3-10 s late) — reasons in the comments of `features/world_session.h`.
- **Fragile:** world manager offsets +0x41 / +0x1B4 are read in `features/world_session.h:IsActive`,
  `framework/transition_watch.cpp:ReadWorld` and `features/terrain_chunk_relight.cpp`; the gate depends on PostScene's depth
  counts and on Depth Blur setting `g_loadSettled` at static initialisation.

## Shader precompile cache

Compiles every Apex HLSL variant on a background thread at start, keeps the bytecode, saves it to disk and ships a prebuilt
copy inside the ASI for Wine / Proton. Docs: [architecture](../architecture.md) 4.4, [release process](../release-process.md)
step 0, [Steam Deck](../features/steam-deck.md).

- **Files:** `framework/shader_cache.cpp`, `ApexRadiance.rc` (resource `APEX_RC_SHADER_CACHE`).
- **Entry points:** `framework/shader_cache.cpp:Add` (namespace-scope initialisers in the features that draw);
  `framework/shader_cache.cpp:Start` (init thread) -> `framework/shader_cache.cpp:LoadEmbedded`,
  `framework/shader_cache.cpp:LoadDisk`, `framework/shader_cache.cpp:WorkerBody`, `framework/shader_cache.cpp:SaveDisk`;
  `framework/shader_cache.cpp:CreatePixelShader` / `framework/shader_cache.cpp:Bytecode` (render thread; waits if not compiled yet).
- **Invariants:** the render thread never runs D3DCompile; the cache key covers source, entry, target, flags and macros
  (`framework/shader_cache.cpp:KeyOf`), so a changed source recompiles.
- **Fragile:** under Wine the builtin compiler refuses some ps_3_0 shaders, so a variant missing from the embedded cache stays
  off there; the release must refresh `resources/ApexRadiance_ShaderCache.bin`.

## S3SS and S3PP coexistence

Runs beside official S3SS, refuses the old combined build, idles a duplicate copy, reads S3SS.toml settings, and offers to
change two Sims3Performance.ini lines next to sims3-modernization. Docs: [architecture](../architecture.md) 11.

- **Files:** `framework/s3ss_detect.cpp`, `framework/s3pp_settings.h` / `framework/s3pp_settings.cpp`,
  `framework/s3ss_ambient_policy.h`, `framework/conflict_guard.h`.
- **Entry points:** `framework/s3ss_detect.cpp:AcquireInstanceMutex` (DllMain); `framework/s3ss_detect.cpp:Scan` /
  `framework/s3ss_detect.cpp:Rescan` (init, device creation, first Present); `framework/s3ss_detect.cpp:S3SSPatchBoolSettingEnabled`
  (lot streaming features); `framework/s3ss_detect.cpp:SavedRoomAmbientOverride` (unlit rooms, Night Lighting, menu);
  `framework/s3ss_detect.cpp:SplitLevelFixActive` (`patches/split_level_ground_light_patch.cpp`);
  `framework/s3pp_settings.cpp:Apply` (Settings > Compatibility, one-time question card).
- **Settings:** `[ui] s3pp_spin_asked`, `[ui] recommend_s3ss` = true.
- **Tests:** `tools/s3pp_settings_test/` (run `tools/s3pp_settings_test/run.ps1`; needs none);
  `tools/room_ambient_test/` covers `S3SSAmbientPolicy::Prepare`.
- **Invariants:** detection needles are stored encoded so Apex never matches itself; Apex's CreateDevice detour goes in
  before S3SS's, never in parallel; Sims3Performance.ini changes only on a click, after a `.apex-backup` copy.
- **Fragile:** S3SS is recognised by strings in its read-only data, so an S3SS update can break detection;
  `S3SSAmbientPolicy::Prepare` and the `RoomAmbientCorrection` types have no production caller.

## Reading the log

ApexRadiance_LOG.txt lines are `HH:MM:SS.mmm [LEVEL] text`; errors and debug lines end with ` (file.cpp:line)`
(`framework/apex_log.cpp:WriteLine`).

| Line | Meaning | Printed by |
|---|---|---|
| `[Main] Game: <build>` / `Unknown game build` | build detection | `apex_main.cpp:InitBody` |
| `[Main] Unified build: normal mode / developer mode` | `kPublicBuild` | `apex_main.cpp:InitBody` |
| `[Main] Features started` | startup installs done | `apex_main.cpp:InitBody` |
| `[Main] The old combined build ... features stay off` | refusal | `apex_main.cpp:InitBody` |
| `[Patches] N features: ...` | registry created | `framework/patch_base.cpp:CreateAll` |
| `[Patches] X did not start: <reason>` | a saved-on feature failed | `framework/patch_base.cpp:LoadFromToml` |
| `[<Feature>] <message>` (ERROR) | `Fail()`; also the red menu note | `framework/patch_base.cpp:Fail` |
| `[<Tag>] Installed ...` | feature-specific success line | each feature's Install |
| `[D3D] <target> at <module+off>: clean / E9 -> module` | hook order with other mods | `framework/d3d9_bootstrap.cpp:Attach` |
| `[D3D] Game device created`, `Graphics card:` | device and driver | `framework/d3d9_bootstrap.cpp:Hooked_CreateDevice` |
| `[D3D9Hooks] Device hooks installed` | registry live | `framework/d3d9_hooks.cpp:Install` |
| `[D3D9Hooks] ... dispatched under the lock` | off-render-thread dispatch | `framework/d3d9_hooks.cpp:NoteOffThread` |
| `[Addr] N of M addresses found ... Group: available / missing` | address summary per feature group | `framework/game_addresses.cpp:Resolve` |
| `[Addr] Not found on <build>` | signatures to refine | `framework/game_addresses.cpp:Resolve` |
| `[MemPatch] 0x...: unexpected bytes, not patched` | foreign patch or other build | `framework/memory_patch.cpp:WriteBytes` |
| `[Config] Migration path: ...` | one-time migration | `apex_config.cpp:DoMigrate` |
| `[Config] Settings loaded` / `ApexRadiance.toml: <error>` | load / parse error | `apex_config.cpp:LoadSettings`, `apex_config.cpp:ReadRoot` |
| `[ShaderCache] ... the background precompile had not reached it` | render thread waited | `framework/shader_cache.cpp:Ready` |
| `[HookGuard] <where> threw a C++ exception` | step caught and switched off | `framework/hook_guard.cpp:ReportPending` |
| `[S3SSDetect] <summary>` | S3SS / old build presence changed | `framework/s3ss_detect.cpp:Rescan` |
