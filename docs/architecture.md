# Architecture: how Apex Radiance hooks the game and Direct3D 9

Apex Radiance is one Win32 DLL renamed `ApexRadiance.asi`, loaded into `TS3W.exe` by an ASI loader. It gets onto the
game's Direct3D 9 device through Microsoft Detours, dispatches the device calls it needs to its features through a
priority-ordered hook registry, patches game code in memory at addresses resolved per game build, and draws its own
ImGui menu. It runs alone or next to an unmodified official Sims3SettingsSetter (S3SS).

This page describes the framework as it is in the code. The frozen combined build (Apex inside a fork of S3SS, tag
`combined-final`) and the plan that produced the standalone are kept in [history/architecture.md](history/architecture.md).

## Overview

| Part | Source | Section |
|---|---|---|
| Start-up, settle wait, update pump, shutdown | [apex_main.cpp](../apex_main.cpp) | [1](#1-start-up-and-shutdown) |
| Device bootstrap (Direct3DCreate9, CreateDevice, EndScene, Reset) | [framework/d3d9_bootstrap.cpp](../framework/d3d9_bootstrap.cpp) | [2](#2-getting-onto-the-device) |
| Hook registry (15 device methods, priorities, Skip / Block) | [framework/d3d9_hooks.cpp](../framework/d3d9_hooks.cpp) | [3](#3-the-hook-registry-d3d9hooks) |
| Extra hooks, render callbacks, post-scene chain, depth share, shader precompile, game-code chains | `framework/`, `features/post_scene.cpp` | [4](#4-other-hook-plumbing) |
| Features, settings, config file | [framework/patch_base.h](../framework/patch_base.h), [apex_config.cpp](../apex_config.cpp) | [5](#5-features-and-settings) |
| Logger, crash report | [framework/apex_log.cpp](../framework/apex_log.cpp), [framework/crash_report.cpp](../framework/crash_report.cpp) | [6](#6-logger-and-crash-report) |
| Developer mode | [build_flavor.h](../build_flavor.h) | [7](#7-developer-mode) |
| Per-frame and per-event flows | | [8](#8-one-frame), [9](#9-per-event-flows) |
| Threads and synchronisation | | [10](#10-threads-and-synchronisation) |
| Coexistence with official S3SS | [framework/s3ss_detect.cpp](../framework/s3ss_detect.cpp) | [11](#11-coexisting-with-official-s3ss) |
| Loaded-world gate (menu, start note, screen effects) | [features/world_session.h](../features/world_session.h) | [12](#12-loaded-world-gate-and-start-note) |
| Report storage | [features/captures.cpp](../features/captures.cpp) | [13](#13-report-storage) |
| Adding a feature | | [14](#14-how-to-add-a-new-feature) |

Game addresses are for `TS3W.exe` Steam 1.67.2.024037 (PE `TimeDateStamp` `0x52DEC247`, image base `0x00400000`, no
ASLR) unless stated. Other builds are resolved by signature ([engine/game-versions.md](engine/game-versions.md)).
Anything marked *Unverified* was not confirmed at run time.

Related pages: [workflow.md](workflow.md) (build, test, install, release),
[engine/main-loop-and-services.md](engine/main-loop-and-services.md) (the game's main loop and threads),
[ui.md](ui.md) (the menu), [removed-features.md](removed-features.md) (removed features).

---

## 1. Start-up and shutdown

### 1.1 How the ASI gets loaded

- The DLL is built as `ApexRadiance.asi` (solution `ApexRadiance.sln`, project `ApexRadiance.vcxproj`). An ASI loader
  in `Game\Bin` loads every `*.asi` in that folder. The test machine uses Ultimate ASI Loader as `wininet.dll`
  (inferred from the loader's README, not checked by hash).
- The loader loads ASIs in folder enumeration order. `ApexRadiance.asi` sorts before `S3SSApex.asi` and
  `Sims3SettingsSetter.asi` (*Unverified* for every loader version).
- `TS3W.exe` imports `d3d9.dll` statically (IAT slot `0x00F95A58` = `d3d9.dll!Direct3DCreate9`,
  `research\engine_map\iat.map`), so `d3d9.dll` is loaded before any ASI. On the test machine `d3d9.dll` is the official
  DXVK 3.1.1.

### 1.2 `DllMain` (`DLL_PROCESS_ATTACH`)

Under the loader lock, in order:

1. `DisableThreadLibraryCalls`.
2. **Process check.** If the executable is neither `TS3W.exe` nor `TS3.exe`, return `FALSE` (the launcher and other
   tools do not load the mod at all).
3. **Instance mutexes** (`S3SSDetect::AcquireInstanceMutex`):
   - `Local\ApexRadiance.<pid>` already held: another copy of Apex Radiance runs. This copy stays idle (returns `TRUE`,
     no hooks, no menu) and writes only to `OutputDebugString`.
   - `Local\S3SSApex.<pid>` already held: an older `S3SSApex.asi` loaded first and runs. This copy stays idle; a short
     thread opens the log and writes one error line asking to delete the old file.
   - Otherwise this copy owns both names, so an `S3SSApex.asi` that loads later finds its name taken and idles by its
     own duplicate check.
4. `Overlay::SetClient(&ApexGui::Client())`.
5. `ApexD3D::InstallFromDllMain()` detours the `Direct3DCreate9` export of the loaded `d3d9.dll`
   ([section 2](#2-getting-onto-the-device)). If `d3d9.dll` is not loaded yet, the init thread retries.
6. Creates the stop event and the **init thread**.

Lines logged before the log file opens are queued and written first.

### 1.3 Init thread

1. Opens `ApexRadiance_LOG.txt` in the Apex Radiance folder (fallback: next to the game executable, never
   `S3SS_LOG.txt`). Logs the product, version, executable and folder.
2. `DetectGameVersion()` reads the PE `TimeDateStamp` (table in [engine/game-versions.md](engine/game-versions.md)). An
   unknown build is logged; game-code features then start only where their code is found by signature.
3. `LogEnvironment()`: the ASIs and wrapper DLLs in `Game\Bin` (`*.asi`, `d3d9.dll`, `dxgi.dll`, `dinput8.dll`,
   `wininet.dll`, `version.dll`, `dsound.dll`, `winmm.dll`, `d3d11.dll`, `dxvk.conf`, `*.ini`) with size, date and file
   version, and the game's `Options.ini`.
4. `ShaderCache::Start()` precompiles Apex's HLSL on a background thread ([4.4](#44-shader-precompile)).
5. `S3SSDetect::Scan()`, then `ApexD3D::EnsureInstalled()` (only acts when the DllMain install could not happen).
6. `ApexConfig::LoadDeveloperMode()` (developer mode is fixed before any feature exists), `PatchManager::CreateAll()`
   (constructs every registered feature), `ApexConfig::EnsureMigrated()` (one-time migration,
   [5.3](#53-config-file-apexradiancetoml)) and `ApexConfig::LoadSettings()` (menu, Picture, profiler preferences).
7. `AddressSpace::Start()` (developer mode only).
8. **Settle wait** (`WaitForSettle`): until the first Present has passed through Apex's hooks and 1 s has elapsed
   (`kSettleAfterPresentMs`), so official S3SS has loaded its own patches from its hook thread first. If no Present
   arrives within 20 s (`kSettleTimeoutMs`), the features are installed anyway (logged).
9. `S3SSDetect::Rescan()`. An older standalone sets a menu notice. **The old combined build loaded too**: features stay
   off, the startup state becomes `RefusedOldBuild` and a banner asks to delete it.
10. Otherwise `GameAddr::Resolve()` resolves every game-code address (fixed on Steam 1.67.2, signature scan elsewhere;
    on the EA app build `.text` is decrypted only in memory, which is long done by now), then `ApexConfig::LoadFeatures()`
    applies `[patches.*]` and installs the enabled features. Startup state `Running`.
11. `CrashReport::Install()`, after the game's own start-up (which may set its own filter).
12. **Pump loop** every 10 ms (`kPumpIntervalMs`) until the stop event:
    - `PatchManager::UpdateAll()` while `Running` (each feature's `Update()`: debounced reinstalls, work scheduled off the
      render thread);
    - `ApexConfig::PumpAutosave()`;
    - once a second: `ConflictGuard::Tick()` (interface only, [11.4](#114-game-code-sites-and-the-conflict-guard)),
      `CrashReport::Refresh()` and the crash report's list of enabled features.

Startup feature installs therefore run on the init thread, after the device exists and after the first Present.

### 1.4 Shutdown

Before starting hooks or workers, Apex pins its module with `GetModuleHandleExW` and
`GET_MODULE_HANDLE_EX_FLAG_PIN`. The module remains mapped until the game process exits, including if a loader
calls `FreeLibrary`. If pinning fails, attachment fails before starting hooks or threads.

The detach handler performs no shutdown work under the loader lock. Windows releases the process resources on
exit. Removing or replacing the ASI therefore requires closing the game. The in-menu graphics restart keeps the
module and hooks loaded and recreates only the supported effect resources.

---

## 2. Getting onto the device

All device hooks are Microsoft Detours inline hooks on **function bodies inside `d3d9.dll`** (DXVK). Vtables are never
written. Each attach logs the target and what its prologue already holds (`HookChain::DescribePrologue`: "clean (...)",
"E9 -> Sims3SettingsSetter.asi+0x1234", ...), so the order of Detours chains with other mods is visible in the log.
One mutex serialises this module's Detours transactions.

| Hook | Installed | Slot | Detour |
|---|---|---|---|
| `d3d9!Direct3DCreate9` | DllMain (or init thread, late path) | export | `Hooked_Direct3DCreate9` |
| `IDirect3D9::CreateDevice` | inside the first `Direct3DCreate9` call (`call_once`, on the caller's thread) | IDirect3D9[16] | `Hooked_CreateDevice` |
| `IDirect3DDevice9::EndScene` | after the first HAL device is created | device[42] | `Hooked_EndScene` |
| `IDirect3DDevice9::Reset` | same | device[16] | `Hooked_Reset` |
| 15 device methods | first EndScene | [3.1](#31-hooked-device-methods) | `framework/d3d9_hooks.cpp` |
| 6 device methods | first use (`ExtraHooks::EnsureInstalled`) | [4.1](#41-extra-device-hooks) | `framework/d3d9_extra_hooks.cpp` |

### 2.1 `Direct3DCreate9` and `CreateDevice`

- `Hooked_Direct3DCreate9` first calls `VulkanDriverGuard::BeforeDirect3DCreate()` (before DXVK loads the Vulkan
  loader), then the original. The first caller that gets an `IDirect3D9` installs the `CreateDevice` detour, once, on
  its own thread, before returning. Every later hook on `CreateDevice` (S3SS's, its Resolution Spoofer's) is therefore
  installed after Apex's, in sequence: two Detours transactions never touch the same prologue at once.
- **Late path** (`EnsureInstalled`, `d3d9.dll` not loaded at DllMain): wait up to 30 s for `d3d9.dll`. If S3SS is
  loaded, probe the `CreateDevice` prologue for up to about 3 s until it is an `E9` into S3SS, then hook
  `Direct3DCreate9` (logged as a warning: a device created before this is not seen).
- `Hooked_CreateDevice` calls the original with the game's **unchanged** present parameters. Apex never edits
  `D3DPRESENT_PARAMETERS`. On a successful HAL device it logs the size, windowed or exclusive mode, back-buffer format,
  multisample type, behaviour flags, the adapter description, vendor and device IDs, the driver version, the adapter
  count and whether `amdvlk32.dll` is loaded; then it detours EndScene and Reset of that device and records the window
  (`hDeviceWindow`, else the focus window).
- A second HAL device is logged and ignored: Apex stays on the first. Non-HAL devices pass through untouched.

### 2.2 `Hooked_EndScene`

Per call, with a thread-local re-entry guard:

1. **First call only** (`FrameInit`, render thread): store the device (`ApexD3D::Device()`), find the window
   (creation parameters' focus window, else the swap chain's `hDeviceWindow`), `D3D9Hooks::Install(device)` (the
   registry detours), register the `ApexCore` Present hook at priority -2000 (first of all), `Overlay::Init`.
2. If `TestCooperativeLevel` succeeds:
   1. `RenderCallbacks::endSceneBeforeOverlay` (post-scene fallback, Frame Capture, Night Lighting deferred reinstall);
   2. `Picture::BeforeOverlay` (scene copy when the frame ended on the scene);
   3. if a Report screenshot is pending with the menu open, `Picture::OnEndScene` now so the photo is graded without
      the menu (Picture then does not grade twice);
   4. `RenderCallbacks::filteredSceneBeforeOverlay` (Report screenshots of the graded frame);
   5. `Overlay::Frame` once per frame (the Present hook clears the flag; every EndScene if the registry is missing);
   6. `Picture::OnEndScene` (the SDR Picture pass).
3. The original `EndScene`.

### 2.3 `Hooked_Reset`

1. `Overlay::BeforeReset()`.
2. `RenderCallbacks::preReset`: features release their `D3DPOOL_DEFAULT` resources.
3. `Picture::BeforeReset()`.
4. The original `Reset` with the game's unchanged parameters.
5. On success: log size and mode, `RenderCallbacks::postReset`, `Overlay::AfterReset()`. On failure: log the HRESULT.

### 2.4 First Present (`ApexCore`, priority -2000)

Clears the overlay-drawn flag every frame. On the first Present only: records the tick (used by the settle wait),
`Overlay::InstallWndProc()` (after S3SS subclassed the window in its first EndScene, so Apex sees messages first) and
`S3SSDetect::Rescan()` (every ASI is loaded by now).

### 2.5 Overlay and input

The overlay ([framework/overlay.cpp](../framework/overlay.cpp)) has its own ImGui context (separate from S3SS's, which
lives in its own DLL), its own layout file `apex_radiance_imgui.ini` and its own window-procedure subclass.

- **Input policy (capture only).** While the Apex menu is open, messages are fed to Apex's ImGui and eaten only when
  that ImGui wants them (mouse over an Apex window, a text field being edited) or when they are Apex's toggle chord.
  Everything else goes on to S3SS's menu and the game. With the menu closed nothing is fed or eaten, except Apex's
  other shortcuts (`Client::HotkeyDown`), which are eaten with their key-up and character.
- The client may claim single keys while the mouse is over the menu (`Client::CaptureKey`: Alt to peek, B to compare);
  their key-up and character are eaten too. The game's cheat console owns all keys while it is open. Alt+Tab is handled
  by Windows before any window sees it.
- Mouse coordinates and the display size are scaled whenever the back buffer differs from the window's client area
  (borderless fullscreen, resolution spoofing).
- The toggle chord defaults to Ctrl+Shift+F11 (`[ui] toggle_key`); S3SS toggles on a bare Insert and ignores modifiers.
- A monotonic frame clock (`framework/overlay_clock.h`) is sampled every game frame, even when nothing is drawn, so
  reopening the menu does not feed the closed time into ImGui's animations; real long frames are preserved. A
  "Slow panel" warning (more than 50 ms in the panel, or a frame delta over 50 ms, with the menu visible) is logged at
  most once every 10 s; it is CPU timing, not GPU timing.
- The menu can only open while the loaded-world gate is open ([section 12](#12-loaded-world-gate-and-start-note)).

---

## 3. The hook registry (`D3D9Hooks`)

### 3.1 Hooked device methods

Installed by `D3D9Hooks::Install` at the first EndScene, in one Detours transaction, whether or not anything is
registered:

| Method | Vtable index | Dispatch |
|---|---|---|
| Present | 17 | locked |
| CreateTexture | 23 | locked |
| CreateRenderTarget | 28 | locked |
| SetRenderTarget | 37 | lock-free on the render thread |
| BeginScene | 41 | locked |
| SetViewport | 47 | lock-free on the render thread |
| SetTexture | 65 | lock-free on the render thread |
| DrawPrimitive | 81 | lock-free on the render thread |
| DrawIndexedPrimitive | 82 | lock-free on the render thread |
| CreateVertexShader | 91 | locked |
| SetVertexShader | 92 | lock-free on the render thread |
| SetVertexShaderConstantF | 94 | lock-free on the render thread |
| CreatePixelShader | 106 | locked |
| SetPixelShader | 107 | lock-free on the render thread |
| SetPixelShaderConstantF | 109 | lock-free on the render thread |

### 3.2 Registration, order and results

- **Register:** `D3D9Hooks::Register<Method>(name, std::function, Priority = Normal)`. `UnregisterAll(name)` removes
  every callback with that name from all chains. Convention: one name per module (`kHookName`, for example
  `"LotLightBridge"`, `"PostScene"`, `"NightTerrainRelight"`). A module may register several callbacks of one type under
  one name.
- **Priority:** `enum class Priority { First = 0, Early = 25, Normal = 50, Late = 75, Last = 100 }`; any int may be
  cast in. Lower runs first. **Equal priorities run in registration order** (stable).
- **Results:** `HookAction::Continue` goes on; `Skip` stops the chain, the device call is not made and `S_OK` is
  returned to the game; `Block` stops the chain and returns `E_FAIL`.
- `DeviceContext { device }` is passed to every callback. `ZEnable()` and `ZWriteEnable()` read `D3DRS_ZENABLE` /
  `D3DRS_ZWRITEENABLE` once per dispatch and share the value between the draw callbacks (PostScene, Picture, the Banding
  Fix), instead of one device query per callback for each of the roughly 2,000 back-buffer draws of a frame. The values
  hold for that dispatch only; a callback that changes those states through `CallOriginal*` restores them before it
  returns.
- **Nesting:** a callback may call the device (a nested dispatch on the same thread) or register and unregister
  callbacks; a chain being run keeps the list it started with until it returns.
- **`CallOriginal*`** calls the next hook in the Detours chain (or the driver) without any Apex callback. It exists for
  CreateRenderTarget, SetRenderTarget, SetViewport, DrawIndexedPrimitive, DrawPrimitive, SetVertexShaderConstantF,
  SetPixelShaderConstantF, SetPixelShader, CreatePixelShader, CreateVertexShader, SetVertexShader and SetTexture. Before
  `Install` they call through the device's vtable. A module that detoured the same DXVK function **before** Apex
  (inner) still sees these calls; one that detoured it **after** Apex (outer) does not. Official S3SS registers nothing
  on these chains, so either order is harmless; a proxy `d3d9.dll` (ReShade) sees every call, being below the detours.

### 3.3 Dispatch and locking

- **Lock-free chains.** DrawIndexedPrimitive, DrawPrimitive, SetRenderTarget, SetViewport, SetPixelShader,
  SetVertexShader, SetTexture and Set{Pixel,Vertex}ShaderConstantF publish an immutable list through an atomic pointer
  (`Chain::list`). On the render thread (the thread of the first EndScene) they are dispatched with no lock: one relaxed
  count test, one acquire load, the callbacks. A chain with no callback ends at the count test.
- **Locked chains.** Present, BeginScene and the four Create* chains, and any chain called from **another thread**, run
  under one recursive registration lock. Off-thread dispatches of the lock-free chains are counted
  (`D3D9Hooks::OffThreadDispatches()`, shown by the Frame Profiler); the first one per method is logged
  `[D3D9Hooks] <method> called from thread N (render thread M): dispatched under the lock`. Callbacks of the lock-free
  chains are not serialised against callbacks running on other threads; that matters only if that counter grows.
- **Publishing.** Register and UnregisterAll build a new list under the lock and publish it. Older lists are kept until
  a safe point: Present on the render thread when it is outside every lock-free dispatch and no locked dispatch is
  running (the lock is only tried, never waited for), or `Uninstall` (skipped when its wait for the render thread timed
  out). A dispatch still reading an older list reads valid memory.
- **Unregistering from another thread.** `UnregisterAll` from the render thread returns at once. From another thread it
  publishes, drops the lock, calls `FlushProcessWriteBuffers` and waits until the render thread has left any lock-free
  dispatch that may still run the removed callback (`g_renderInside` / `g_renderExits`, written only by the render
  thread). The wait is bounded at 1 s and logged when it expires
  (`[D3D9Hooks] UnregisterAll("X") from thread N: the render thread did not leave its draw hook within 1 s; continuing`):
  a caller holding a lock that a draw or state callback takes (for example `PostScene::Remove` holding its effect
  mutex) would otherwise deadlock. **Rule:** do not free what a draw or state callback uses from another thread without
  unregistering first, and prefer the render thread.
- **Profiler counters (developer mode).** The detours of SetTexture, Set{Vertex,Pixel}Shader,
  Set{Vertex,Pixel}ShaderConstantF and SetRenderTarget count their calls with plain per-method counters
  (`ReadStateCallCounts`). The outermost dispatch on a thread (all chains but Present) is booked as the profiler's
  "D3D hooks (mod)" (`FrameProfiler::BeginModTime` / `EndModTime`, keyed by the dispatch's stack address), also when a
  callback returns Skip or Block; a nested dispatch is part of the outer one. Every Present callback is timed by name
  while the profiler is on (`"<name> (Present)"`); draw and state callbacks are timed per hook only with Advanced >
  "Per-hook registry timing", separated by method ([features/frame-profiler.md](features/frame-profiler.md)).

### 3.4 Skip in practice: the draw-replacement pattern

Night Lighting's `features/lot_light_bridge.cpp` registers DrawIndexedPrimitive and DrawPrimitive at `Normal`. For a
draw it handles:

1. It changes state for its own shader and constants through `CallOriginal*`, so its state changes never re-enter
   Apex's chains.
2. It re-issues the draw through the device (`ctx.device->DrawIndexedPrimitive(...)`), a nested dispatch on the same
   thread. In the nested dispatch its own callback returns `Continue` at once (own-call flag), and the call reaches the
   driver.
3. It restores state and returns `Skip` for the outer call. The game's original draw is never issued and the outer chain
   stops at `Normal`.

Consequences: hooks before `Normal` (PostScene, Picture, Ambient Occlusion's receiver replay) see a replaced draw twice,
once as the original and once as the re-issue. Hooks after `Normal` (Light Probe, Frame Capture, Banding Fix) see only
the re-issued draw, with the mod's shader bound. Light Probe uses the same re-issue-and-Skip pattern. The profiler
recognises a dispatch cut short by Skip or Block by its stack address.

**Rule:** a callback may register or unregister callbacks of any type; the running chain keeps its list. Still, prefer
registering in `Install` and unregistering in `Uninstall`.

### 3.5 Current registrations

| Method | -2000 | -1000 | First (0) | 10 | Early (25) | Normal (50) | Last (100) | +1000 |
|---|---|---|---|---|---|---|---|---|
| Present | ApexCore | Frame Profiler start (dev) | PostScene, Picture, Depth Blur (while its depth swap runs), Ambient Occlusion, Edge Smoothing, Banding Fix, CapturesScreenshot | | | LotLightingMotion (camera sample), RoomLightQueue (render-thread id) | NightTerrainRelight (Night Lighting per-frame dispatcher), Frame Capture (dev) | Frame Profiler end (dev) |
| DrawIndexedPrimitive / DrawPrimitive | | Frame Profiler counter (dev) | PostScene | Picture | Ambient Occlusion (Sim receiver replay) | **LotLightBridge (Skip)** | Light Probe (Skip when it re-draws), Frame Capture (dev) | Banding Fix (binds its copies after every observer) |
| SetRenderTarget | | | PostScene, Picture | | | | Frame Capture (dev) | |
| SetPixelShader | | | | | Ambient Occlusion (Sim receiver test) | LotLightBridge | | |
| SetVertexShader | | | | | | LotLightBridge | | |
| CreatePixelShader / CreateVertexShader | | Frame Profiler counter (dev) | | | | | | Banding Fix (creates the game shader, then its copy; Skip) |
| BeginScene | | | | | | | Frame Capture (dev) | |
| CreateTexture / CreateRenderTarget | | Frame Profiler counter (dev) | | | | | | |
| SetTexture, Set*ShaderConstantF, SetViewport | | | | | | | | |

Picture registers its draw hooks at 10 so its scene copy already contains the post-scene effects that fire inside
PostScene's `First` callback, and before any feature that may Skip a draw (`features/picture.cpp`). The Banding Fix
registers at 1000 so every observer has seen the draw before it binds its shader copies (`features/scene_dither.cpp`).
Priorities -1000 and +1000 are reserved for the Frame Profiler, -2000 for the framework.

---

## 4. Other hook plumbing

### 4.1 Extra device hooks

`ExtraHooks` ([framework/d3d9_extra_hooks.cpp](../framework/d3d9_extra_hooks.cpp)) detours methods the registry does not
cover:

| Method | Vtable index |
|---|---|
| StretchRect | 34 |
| SetDepthStencilSurface | 39 |
| GetDepthStencilSurface | 40 |
| Clear | 43 |
| DrawPrimitiveUP | 83 |
| DrawIndexedPrimitiveUP | 84 |

- Installed once, on first use (`EnsureInstalled(dev)`: Depth Blur's resource setup, Frame Capture), and kept for the
  life of the process. With no callback set they only cost an atomic load.
- **Guard:** if any of those slots shares a code address with a slot already owned by the bootstrap or the registry
  (16, 17, 23, 28, 37, 41, 42, 47, 65, 81, 82, 91, 92, 94, 106, 107, 109), it logs an error and does not install. This
  protects against DXVK folding identical functions together.
- **Observer slots** (one each, atomic function pointers, used by Frame Capture): Clear, SetDepthStencil, StretchRect,
  DrawUP.
- **Depth substitution** (single owner, Depth Blur), `SetDepthSubstitution(substitute, report)`: `substitute` maps the
  surface the game binds to the surface actually bound; `report` maps back for `GetDepthStencilSurface`, so the game never
  sees the swap. `RawSet/GetDepthStencilSurface` bypass it; before `EnsureInstalled` they call the device methods.
- Post-scene effects draw with DrawPrimitiveUP. The registry does not hook it, so the effects never re-trigger the
  post-scene trigger; the DrawUP observer only reports them.

### 4.2 Render callbacks

`RenderCallbacks` ([framework/render_callbacks.h](../framework/render_callbacks.h)) are lists of plain
`void(*)(IDirect3DDevice9*)` fired by the bootstrap at points the registry does not cover. The lists grow as needed;
`Add` ignores a duplicate; `Fire` copies the list first, so a callback may add or remove; callbacks run in the order
added.

| List | Fired | Users |
|---|---|---|
| `endSceneBeforeOverlay` | EndScene, before Picture and the overlay | PostScene fallback trigger, Night Lighting `DeferredReinstall`, Frame Capture (dev) |
| `filteredSceneBeforeOverlay` | EndScene, before the overlay; after Picture's grade when a screenshot is pending with the menu open | Report screenshots with the menu open (`Captures`); with the menu closed the screenshot is taken in a `First` Present hook |
| `preReset` | before `Reset` | PostScene, Depth Blur, Ambient Occlusion, Edge Smoothing, Banding Fix, Night Lighting (`LightmapSmooth::OnPreReset`) |
| `postReset` | after a successful `Reset` | Depth Blur, Ambient Occlusion, Edge Smoothing |

### 4.3 Post-scene chain and the INTZ depth share

One shared trigger runs effects on the finished 3D scene, before any game UI
([features/post_scene.cpp](../features/post_scene.cpp)).

- **Order:** `PostScene::Add(order, fn)` keeps effects sorted: Ambient Occlusion 10 (`kAmbientOcclusion`), Edge
  Smoothing 20 (`kEdgeSmoothing`), Depth Blur 30 (`kDepthBlur`). The first `Add` registers the hooks under
  `"PostScene"` (Present, SetRenderTarget, DIP, DP, all `First`) and the `endSceneBeforeOverlay` / `preReset` callbacks;
  `Remove` of the last effect unregisters them.
- **Trigger:**
  1. Count back-buffer draws (RT0 is the back buffer) with `ZENABLE != FALSE`.
  2. The first back-buffer draw with `ZENABLE == FALSE` after at least `kMinSceneDraws = 20` depth-tested draws fires
     every effect once, inside that draw's DIP/DP callback, before the game's draw executes. If a shared scene depth
     exists, an incompatible bound surface rejects the boundary without consuming the effects; only resumed depth-tested
     scene draws unlock another boundary.
  3. If no qualifying depth-off draw happened (game UI hidden), `endSceneBeforeOverlay` checks that at least 20 scene
     draws occurred and RT0 is still the back buffer, then runs the same chain once, before Picture's scene copy and the
     overlay. EndScene never retries over a rejected UI boundary.
  4. Draws marked with `DepthShare::SetInternalPass(true)` are ignored. The lake-lamp pass in `lot_light_bridge.cpp`
     turns Z off and unbinds the depth-stencil, which would otherwise look like the first UI draw.
  5. The Present hook resets the counters at the frame boundary.
- **Known limitation:** interiors can have depth-off back-buffer draws in the middle of the scene, so the first such
  draw can precede the finished scene (Picture re-copies at every depth-on to depth-off transition for that reason). The
  EndScene fallback does not change that.
- **Camera:** while an effect asks for it (`PostScene::WantCamera`, reference counted; Ambient Occlusion), the scene
  draws' vertex constants vote for the near plane (`CameraNear`, 0.2 to 0.3 m with zoom and height), the view-projection
  (`CameraViewProj`, rows `c40..c43`) and A of the projection (`CameraDepthA`; device depth `d = A - near * A / z`,
  A = 1.00008 measured, far plane about 3 km). Recipe: [engine/camera-and-map-view.md](engine/camera-and-map-view.md).
- **INTZ swap (`DepthShare`, owned by `patches/depth_blur_patch.cpp`):** the game's auto depth-stencil (D24S8 or D24X8,
  back-buffer size, not multisampled) is swapped for an INTZ texture with `D3DUSAGE_DEPTHSTENCIL` through the ExtraHooks
  substitution. `DepthShare::Texture()` / `Surface()` are null unless the swap is ready; `Status()` says why.
  `Request(bool)` is reference counted; the swap runs while Depth Blur is on or any request is held. Requesters: Ambient
  Occlusion (while installed) and Edge Smoothing (while its depth edges are on and the game's own edge smoothing is
  off). Night Lighting's pond reflection reads the texture when it exists and does not request it. A multisampled back
  buffer means the game's Edge Smoothing (MSAA) is on: the swap then refuses, with the status "turn it off in Options >
  Graphics".

### 4.4 Shader precompile

<a id="shader-precompile"></a>
Apex's own HLSL pixel shaders are never compiled on the render thread
([framework/shader_cache.cpp](../framework/shader_cache.cpp)).

- **Registration:** each feature adds every variant it can use (all qualities and modes) with `ShaderCache::Add` from
  namespace-scope initialisers in its own `.cpp`, so the list is complete before the init thread runs. A `Desc` keeps
  the exact `D3DCompile` inputs (source, source name, entry, target, flags, macros), so the bytecode is what the feature
  would compile; `priority` 0 is the feature's default quality or mode, compiled first.
- **Compile:** `ShaderCache::Start()` (init thread, right after the log opens, before the device exists) starts one
  below-normal-priority worker that compiles every variant and keeps the bytecode for the session (the source string is
  freed after its compile). Log: `[ShaderCache] Precompiling N Apex shaders on a background thread`, then
  `[ShaderCache] Precompiled N Apex shaders in X ms on a background thread (F failed; slowest: ...)`; a failure logs
  `[ShaderCache] <tag> did not compile: <message>`.
- **Disk cache:** every compiled variant's bytecode is saved to `ApexRadiance_ShaderCache.bin` in the Apex Radiance
  folder (magic `APXS`; written to a temporary file and renamed), keyed by the variant's identity (source, entry,
  target, flags, macros). At the next start the variants found there are created without `D3DCompile`; a variant whose
  identity changed misses and compiles as before, and the file is written again with the current set only. A damaged
  file is read up to the damage. `d3dcompiler_47.dll` is linked normally (2.7.0 delay-loaded it so a
  complete cache never mapped it; antivirus heuristics flagged that build, so 2.7.1 links it as 2.6.0 did). Before the
  first compile the DLL is checked (`CompilerAvailable`); without it, shaders missing from the cache stay off (`[ShaderCache] d3dcompiler_47.dll not found: ...`). Log:
  `[ShaderCache] N Apex shaders: X from ApexRadiance_ShaderCache.bin, Y to compile` and `[ShaderCache] Saved N compiled
  Apex shaders ...`.
- **Use:** `ShaderCache::CreatePixelShader(dev, id, &ps, &msg)` only creates the D3D9 object from the bytecode. If the
  worker has not reached that variant yet, it becomes the worker's next job and the caller waits (logged as a warning
  `waited for the precompile`, counted; the worker is raised to normal priority). With no worker left (thread creation
  failed, or FreeLibrary) the variant is compiled on the calling thread as a last resort (logged).
  `PrecompileComplete()` is a non-blocking readiness probe.
- **Reset and release:** D3D9 shader objects survive `Reset`. Features that release them (Uninstall, Shutdown) recreate
  them from the kept bytecode, never compiling again.
- **Status:** `ShaderCache::StatusText()` on the Developer page and in the profiler report (`Apex shaders: ...`).
- **Not covered:** game-shader copies patched in bytecode (`features/shader_patches.cpp`: roads, floors, snow, fences,
  foliage, object vertex shaders, Banding Fix and Ambient Occlusion receiver copies) are created from the game's own
  bytecode (no HLSL, no `D3DCompile`) when the game shader is created or first drawn. `light_probe.cpp` uses
  `D3DDisassemble` on demand.

### 4.5 Game-code patching helpers

[framework/memory_patch.h](../framework/memory_patch.h):

| Helper | What it does |
|---|---|
| `MemPatch::ReadBytes` / `ValidateBytes` | Guarded read; compare with expected bytes |
| `MemPatch::WriteBytes` / `WriteDWORD` | Write code or data (protection lifted, instruction cache flushed); with `expected`, nothing is written unless the current bytes match; `undo` receives the old bytes |
| `MemPatch::RestoreAll(undo)` | Restores every recorded write, newest first; keeps what could not be restored |
| `MemPatch::WriteCodeSuspended(address, bytes, n)` | Writes 1 to 16 code bytes with every other thread suspended and none stopped inside them (retried for about 100 ms), for CALL rewrites that several threads may run. Nothing is allocated and no lock is taken while threads are suspended |
| `MemPatch::CalculateRelativeOffset(from, to, length)` | rel32 for a JMP or CALL |
| `MemPatch::ScanPattern(base, size, "8B 4E ?? E8")` | First match of an IDA-style pattern; unreadable memory skipped |
| `DetourBatch::InstallHooks` / `RemoveHooks` | Several Detours hooks in one all-or-nothing transaction |
| `GameAddress::Resolve()` | The verified address for this build, else a pattern scan of `TS3W.exe`; checked against `expectedBytes` either way |

Game-code addresses shared by Night Lighting, Every-Story Ground Light, the Performance features and the Frame Profiler
are resolved once by `GameAddr::Resolve()` ([framework/game_addresses.h](../framework/game_addresses.h)): fixed on Steam
1.67.2, found by masked signature elsewhere. An address counts only when its signature matches once (or every match gives
the same value). Every signature is logged (`[Addr] name: N matches at ... (Steam 0x...)`, with 16 bytes around each
match on non-Steam builds). A feature whose required addresses are missing stays off with "Not available on <version>:
<missing>" and writes nothing ([engine/game-versions.md](engine/game-versions.md)).

### 4.6 Layered game-function hooks

Several Apex modules may wrap one game function. Each wrapper is a layer with a fixed position (lower = outer); each
hook calls `Next(site, layer)` to reach the next inner layer or the original. Removing a layer re-points its outer
neighbour; removing the last restores the original.

**`SlotChain`** ([framework/slot_chain.h](../framework/slot_chain.h)): functions reached only through vtable slots.
Install and Remove swap the slots with an interlocked compare-exchange (expected value checked); a removed hook keeps its
next pointer. Layers: 0 Gate (the wall shading gate, which must be outermost because it recognises its caller by its own
return address), 1 Frame Profiler, 2 Resource cache, 3 FastCompress.

| Site | Function | Slots | Layers used |
|---|---|---|---|
| FindProvider | `ResourceMgr::FindProvider` `0x004AFFC0` | +0x40 of both resource-manager vtables | Frame Profiler, Resource cache |
| RegisterDb | `ResourceMgr::RegisterDatabase` `0x004B2D00` | +0x34 of base vtable `0x00FB2DA0` | Resource cache |
| RegisterDbDerived | ResourceSystem override `0x00736A70` | +0x34 of derived vtable `0x00FFE250` | Resource cache |
| SetDbPriority | `0x004B2EC0` | +0x3C of both vtables | Resource cache |
| DbChanged | `0x004B0960` | +0x4C of both vtables | Resource cache |
| RefPackCompress | RefPack stream write `0x004EC200` | +4 of stream vtable `0x00FB9018` (`0x00FB901C`) | Frame Profiler, FastCompress |
| WallAoStep | wall AO solver step `0x0068B810` | +0x1C of solver vtable `0x00FF0594` (`0x00FF05B0`) | Gate, Frame Profiler |
| KeyListBase | `ResourceMgr::GetKeyList` `0x004B1AE0` | +0x20 of base vtable (`0x00FB2DC0`) | Frame Profiler, Resource cache |
| KeyListDerived | `ResourceSystem::GetKeyList` `0x00736660` | +0x20 of derived vtable (`0x00FFE270`) | Frame Profiler, Resource cache |

The write epochs of "Remember missing files" swap their database-class slots directly (the only Apex module on them;
`InstallClassHooks` in `features/resource_cache.cpp`, same compare-exchange).

**`EntryChain`** ([framework/entry_chain.h](../framework/entry_chain.h)): functions reached by direct CALLs from several
threads. The entry's relocation-free prologue is copied to a trampoline (plus a JMP back) and a 5-byte JMP to the
outermost layer's hook is written with `MemPatch::WriteCodeSuspended`. Layers: 0 Frame Profiler, 1 FastDxt,
2 ResourceCache, 3 ObjectIndex, 4 SceneBudget, 5 LevelLightShare, 6 FastCas, 7 FastCrc; each module layer is used on its
own sites only.

| Site | Address | Prologue | Users |
|---|---|---|---|
| DxtEncode1 / DxtEncode5 | `0x006152F0` / `0x006154B0` cdecl(Dst*, Src*) | `55 8B EC 83 E4 F0` | Frame Profiler, fast DXT encoder |
| DpfWriteDirect | `0x004A7FC0` thiscall(5 args), ret 0x14 | `83 EC 28 53 56` | Resource cache write epochs |
| ObjectById | `0x00C62D40` thiscall(idLo, idHi, int* visited), ret 0xC | `8B 44 24 0C 8B 54 24 08` | Frame Profiler, Faster Object Lookups |
| SceneNodeDtor | `0x006FD930` | `55 8B EC 83 E4 F0` | Spread New Objects Over Frames (node lifetime guard) |
| SceneAddNode | `0x006E6480` thiscall(node, group), ret 8 | `56 8B 74 24 08` | same |
| SceneHolderTeardown | `0x006E4DE0` | `53 55 56 57 8B F9` | same |
| RoomInvalidate | `0x0069EED0` thiscall(room, char full, char keep), ret 8 | `56 8B F1 8B 0E` | Night Lighting level light share |
| RoomInvalidateFlag | `0x0069F160` thiscall(room, char flag), ret 4 | `8A 44 24 04 56` | same |
| CasTriSort | `0x005D1960` cdecl(6 args) | `55 8B EC 83 E4 F0` | Fast CAS sort |
| RecordCrc | `0x004FA4C0` cdecl(bytes, length, crc, bool invert) | `8B 4C 24 04 8B 44 24 08` | Fast CRC |

**`CallChain`** ([framework/call_chain.h](../framework/call_chain.h)): one CALL instruction (E8 rel32). The CALL targets
the outermost layer's hook, written with `WriteCodeSuspended`; removing the last layer writes the original CALL back.
Site: `Scene::BeginFrame` (`0x006EBB70`)'s CALL at `0x006EBC49` of the pending-node drain `0x006E4130`; layers 0 Frame
Profiler ("Scene pending nodes"), 1 SceneBudget (Spread New Objects Over Frames).

Lot Lighting While Moving rewrites its CALL at `0x00ADB95D` with `WriteCodeSuspended`; the Frame Profiler keeps its own
copy of that routine (`WriteCallSuspended`). Details of the performance sites: [features/performance/README.md](features/performance/README.md).

---

## 5. Features and settings

### 5.1 Feature classes and registration

Every feature ("patch") derives from `ApexPatch` ([framework/patch_base.h](../framework/patch_base.h)):

| Member | Purpose |
|---|---|
| `Install()` / `Uninstall()` | Do the work; `Fail(msg)` records the reason shown in the menu and logs it |
| `Update()` | Pump thread, every ~10 ms; by default reinstalls 2 s (`SETTING_CHANGE_DEBOUNCE`) after a setting changed |
| `RenderCustomUI()` | The feature's controls while it is on (default: its registered settings) |
| `RenderDeveloperUI()` | Developer-page lines (developer mode only) |
| `SaveToToml` / `LoadFromToml` | `[patches.<Name>]` serialisation |
| `ApplyTableLive(table)` | Applies a table while the game runs (profiles, looks, Undo; render thread): settings that differ, then `enabled` (Install / Uninstall when it differs); missing keys stay as they are |
| `DefaultsToToml` | Every setting at its default |
| `GpuCostMs()` | GPU time per frame from timestamp queries (< 0 = not measured) |
| `OverviewSummary()` | Cheap read-only summary for the Overview page |
| `NotifySettingChanged()` | Schedules the debounced reinstall and marks the config dirty |

- **Registration:** `APEX_REGISTER_FEATURE(Class, {.displayName, .description, .category, .experimental,
  .enabledByDefault, .supportedVersions, .technicalDetails, .gameCodeGroup})` creates a static registration.
  `PatchManager::CreateAll()` instantiates every registration once on the init thread.
- **`FeatureInfo.description`** is the hover text of the feature; it ends with "Part of " `APEX_PRODUCT_NAME` ".
  Credits: @loinyx".
- **Version gating:** `IsCompatibleWithCurrentVersion()` checks `supportedVersions` (`VERSION_STEAM`, `VERSION_EA`,
  `VERSION_RETAIL`, `VERSION_ALL`) against the detected build. An unknown build is incompatible unless the feature names
  a `gameCodeGroup` (for example `"NightLights"`, `"SplitLevel"`) whose addresses the signature scan found.
  `UnavailableReason()` gives "Not available on <version>[: missing <addresses>]".
- **Registered features:** NightTerrainRelight (Night Lighting), SplitLevelGroundLight (Every-Story Ground Light),
  EdgeSmoothing, DepthBlur, AmbientOcclusion, SceneDither (Banding Fix), the Performance switches in
  `patches/performance_patches.cpp` (resource lookup cache, lookup misses, file list cache, wall shading while moving,
  lot lighting while moving, fast texture compression, fast cache compression, fast CAS sort, fast memory, scene node
  budget, object lookup index, room light queue) and FrameCapture (developer). Picture and the Frame Profiler are
  modules with their own settings tables.

### 5.2 Declaring and using settings

- **Declare in the constructor:** `RegisterFloatSetting(ptr, key, SettingWidget, default, min, max, desc, presets)`,
  `RegisterIntSetting`, `RegisterBoolSetting`, `RegisterEnumSetting`. The setting writes its default into `*ptr` at
  construction; `Load` clamps to the range.
- **Load order:** settings first, then `enabled`, so `Install()` sees the loaded values.
- **Reinstall policy:** the base `Update()` uninstalls and reinstalls on the pump thread 2 s after a change. Features
  avoid this where possible:
  - **Read live:** settings read every frame or call; `Update()` clears `pendingReinstall`.
  - **Reinstall on the render thread:** Night Lighting does not reinstall itself from the pump. It sets a flag and its
    `DeferredReinstall` (`endSceneBeforeOverlay`) reinstalls on the render thread, because Uninstall frees textures and
    shaders the draw hooks use. Options that can change live are applied at once (`ApplyLive`).
- **Never rename a TOML key.** Saved configs and profiles use them. Some keys are Portuguese identifiers kept from the
  combined build (Night Lighting: `luzDoPosteNaGramaDoLote`, `postesNosObjetos`, `cercasComLuzDoChao`, ...; Depth Blur:
  `distancia`, `transicao`, `forca`, `tamanho` (unused), `qualidade`, `focoAuto`, `quantidade`, `areaNitida`,
  `velocidadeFoco`, `realceLuzes`).

### 5.3 Config file (`ApexRadiance.toml`)

Location: `Documents\Electronic Arts\<localized game folder>\Apex Radiance\` ([framework/apex_paths.h](../framework/apex_paths.h)).
The localized folder name is resolved from the game executable's string table, with `"The Sims 3"` as fallback.

| Table | Contents |
|---|---|
| `[meta]` | `version`, `written_by` (product and version), migration record |
| `[ui]` | `toggle_key`, `developer_mode`, `font_scale` (0.5 to 3), `recommend_s3ss`, `start_note`, `capture_screenshot`, `hotkey_preset`, `mine_base`, `compare_key`, `refresh_key`, `probe_key`, `diagnostics_key`, `recorder_key`, `frame_capture_key`, `search_key`, `peek_key`, `picture_compare_key`, `screenshot_folder`, `screenshot_key`, `screenshot_hide_game_ui`, `sidebar_collapsed`, `language`; legacy `welcome_done`, `key_chosen` kept for compatibility |
| `[qol.picture]` | Picture filters |
| `[qol.frame_profiler]` | Frame Profiler preferences (developer mode; never saved as running) |
| `[developer]` | Developer preferences imported with a profile |
| `[patches.<Name>]` | One table per feature: `enabled` plus one key per registered setting |

- **Writes** are atomic (temporary file, then replace) and debounced: `RequestSave` marks the file dirty and the pump
  thread writes it about a second later (`PumpAutosave`). Menu changes are therefore saved automatically.
- **Profiles:** `Profiles\<name>.toml` in the same folder, with the same feature tables ([ui.md](ui.md)).
- **One-time migration** (`ApexConfig::EnsureMigrated`, only while `ApexRadiance.toml` is missing; the log line
  `[Config] Migration path: ...` and Settings > Status say which path ran):
  1. The previous standalone's `...\S3SS\Apex\Apex.toml` exists: copy it byte for byte. Its `apex_imgui.ini` is not
     copied (the menu's scale changed). The old folder is left in place. If it cannot be read or written, nothing is
     written and the next start tries again.
  2. Else, `S3SS.toml` exists: back up its raw bytes to `Apex Radiance\S3SS.toml.pre-split.bak` and copy only the Apex
     tables (`[qol.picture]`, or Picture keys from `[qol.hdr]` as a fallback; `[qol.frame_profiler]`; the Apex
     `[patches.*]` tables, keeping only registered keys). NightTerrainRelight's object and fence strengths
     (`forcaNosObjetos`, `forcaLuzPorPixelNosObjetos`, `forcaNasCercas`) are reset to 0.57 / 1.0 / 1.0.
  3. Else (or S3SS already dropped the tables): defaults, logged.
- Tables no longer read (features removed): `[qol.hdr]` (except the Picture fallback), `[patches.SmoothStreaming]`,
  `[patches.GcScheduler]`, `[patches.FrameBudget]`. Ambient Occlusion uses `[patches.AmbientOcclusion]` again.
- **S3SS.toml** is read-only, with one exception: the explicit Rooms at Night compatibility action
  ([section 11.3](#113-s3sstoml)).

### 5.4 Menu

The menu is one window titled "Apex Radiance" (`APEX_PRODUCT_NAME`) in the Violet layout: a sidebar of pages, feature
cards, Advanced areas for tuning, a Developer page in developer mode. Pages, widgets, banners, profiles and shortcuts
are documented in [ui.md](ui.md). Menu changes follow the [apex-menu](../.agents/skills/apex-menu/SKILL.md) skill.

---

## 6. Logger and crash report

**Log** ([framework/apex_log.cpp](../framework/apex_log.cpp)): `ApexRadiance_LOG.txt`, truncated at each start, header
"Apex Radiance Log". Levels `Debug < Info < Warning < Error < Critical` (`LOG_DEBUG` ... `LOG_CRITICAL`).

- Lines are queued in memory and written by a writer thread at most every 0.5 s (`kWriteIntervalMs = 500`). Warnings
  and errors are written at once, together with everything queued before them, so a crash report ends right after the
  lines that led to it.
- Lines logged before `Open()` (DllMain, early init) are kept and written first.
- Debug lines go only to `OutputDebugString` (use DebugView) unless verbose logging is on (`ApexLog::SetVerbose`).
- `Close()` (FreeLibrary only) writes what is queued and stops the writer; process exit flushes by itself.
- **Rule for render-thread code:** do not log per draw or per frame. `lot_light_bridge.cpp` counts notes per kind and
  writes one line at most about once a second.

**Crash report** ([framework/crash_report.h](../framework/crash_report.h)): an unhandled exception writes
`ApexRadiance_Crash.txt` (exception, registers, module and offset of the faulting address and of every stack value that
points into a module's code, the enabled Apex features) and a small minidump `ApexRadiance_Crash.dmp`, then passes the
exception on (to the game's filter, else Windows Error Reporting). Both files are overwritten by the next crash. The
filter uses no heap, only stack buffers and Win32 calls. The pump thread takes the filter back once a second if another
module replaced it (that one then runs after the report).

**Other outputs** in the same folder: `ApexRadiance_Hitches.txt` (profiler writer thread), `ApexRadiance_FrameCapture.txt`,
`ApexRadiance_LightDiag.txt`, `ApexRadiance_LightProbe.txt` and `LightProbe\`, `ApexRadiance_Censo.txt` and `Censo\`,
`ShadersRecusados\`, `Captures\` (Report a problem), `Profiles\`.

---

## 7. Developer mode

There is one binary ([features/developer-mode.md](features/developer-mode.md)). `build_flavor.h` keeps the legacy name
`kPublicBuild`, now an `std::atomic<bool>` meaning "developer mode is off". `ApexConfig::LoadDeveloperMode()` sets it
from `[ui] developer_mode` on the init thread before any feature is constructed; it is atomic because D3D callbacks
may already run. Developer instruments, threads and checks (Frame Profiler, Address Space monitor, Light Probe, Light
Diag, Frame Capture, census, false colour, Developer page) are compiled in and gated at run time by
`if (!kPublicBuild)`. Changing the mode requires a confirmation and a game restart. `S3SS_TR(pt, en)` expands to `en`;
it remains so old call sites compile.

---

## 8. One frame

Game addresses are Steam. Sources: `engine_map\f_ECA960.asm`, the `frame_profiler.cpp` header, `profiler_targets.tsv`.

```
Game main loop FUN_00ECA960 (render thread = main thread), one iteration:
  0x00EC6C30  app state update
  0x00588E00  ServiceManager::Update -> 0x0059ED20: each service's vfunc+0x1C (indirect call at 0x0059ED57)
     |- Input service 0x00598660 -> message pump 0x00410890 -> Apex window procedure (overlay input, shortcuts)
     |- JobManager 0x00599A10, ResourceSystem 0x007377F0, CAS SimService 0x005F0E50, TextureCompositor 0x00608630
     |- WorldManager service 0x00C7E3C0 -> WorldManager::Update 0x00C6D570
     |     -> terrain update 0x00C845C0 (called at 0x00C6D68F)                    [Night Lighting terrain state]
     |     -> lot renderer pass 0x00C7CEA0 -> 0x00AEB2E0 -> lot load stages 0x00AEA680
     |        (room lighting 0x006A80E0 -> solve 0x006A3EC0)                      [level light share, room queue]
  0x009DE140  SceneCaptureManager
  0x006EBB70  Scene::BeginFrame (CALL 0x006EBC49 -> pending-node drain 0x006E4130) [Spread New Objects]
  0x00EC9F00  render frame (only if [0x011D1530] != 0):
     0x00611620 BeginFrame -> device BeginScene -> registry BeginScene (Frame Capture)
     scene draws: every DIP/DP -> registry chain
         -1000 profiler counter
         First:  PostScene (count depth-tested back-buffer draws; camera vote)
         10:     Picture (scene copy at depth-on -> depth-off transitions)
         Early:  Ambient Occlusion Sim receiver replay
         Normal: LotLightBridge -> may bind a patched shader, re-issue the draw (nested chain) and Skip
         Last:   Light Probe (dev), Frame Capture (dev)
         +1000:  Banding Fix (bind dithered copies for the draw) -> DXVK
     first back-buffer draw with ZENABLE = FALSE after >= 20 scene draws (bloom composite, then UI):
         PostScene fires inside that callback, before the draw:
         Ambient Occlusion (10) -> Edge Smoothing (20) -> Depth Blur (30), each drawing with DrawPrimitiveUP
     game UI draws (Z off)
     0x00611760 -> 0x00611680 end frame:
         device EndScene -> Hooked_EndScene:
             [first call: registry detours, ApexCore Present hook, overlay]
             endSceneBeforeOverlay: PostScene fallback, Night Lighting DeferredReinstall, Frame Capture
             Picture::BeforeOverlay (scene copy if the frame ended on the scene)
             filteredSceneBeforeOverlay (Report screenshot)
             Overlay::Frame (menu, notices) -> Picture::OnEndScene (SDR grade) -> original EndScene
         device Present -> registry Present chain:
             -2000 ApexCore (first-Present work, overlay flag)
             -1000 profiler frame boundary
             First: PostScene / Picture / Depth Blur / AO / Edge Smoothing / Banding Fix resets, Report screenshot
             Normal: LotLightingMotion camera sample, RoomLightQueue
             Last:  NightTerrainRelight: Light Diag (dev), world change, dusk, reconcile and relight, Light Probe (dev),
                    object light bridge, level light share, LotLightBridge setters, LightmapSmooth; Frame Capture (dev)
             +1000 profiler end
             DXVK Present (GPU back-pressure, vsync)
     +0xBA: frame limiter (Smooth Patch's call at 0x00EC9FBA, or the game's ~30 ms sleep when inactive)
  0x006E8810  Scene::EndFrame (waits for the scene jobs)
  0x005943F0  game clock tick
```

The Frame Profiler's categories follow the same boundaries: `0x611680` counts as "EndScene + overlays" until the Present
boundary and "Present (driver)" after it; the rest of `0xEC9F00` is "Frame limiter"
([features/frame-profiler.md](features/frame-profiler.md)).

---

## 9. Per-event flows

### 9.1 Game start

1. DllMain hooks `Direct3DCreate9`; the init thread opens the log, precompiles shaders, creates the features and loads
   settings.
2. The game calls `Direct3DCreate9`: Apex hooks `CreateDevice` on that thread.
3. The game creates its HAL device: Apex detours EndScene and Reset.
4. First EndScene: registry detours, the `ApexCore` Present hook, the overlay.
5. First Present: window procedure subclass, S3SS rescan; one second later the init thread resolves game addresses and
   installs the enabled features.
6. Features create their D3D resources lazily on the render thread at the first frame that needs them (log lines such as
   `[DepthBlur] Resources ready (WxH, INTZ depth swapped in)`).

### 9.2 Device reset (alt-tab from exclusive mode, resolution change)

1. `Overlay::BeforeReset`.
2. `preReset`: every `D3DPOOL_DEFAULT` object is released (Depth Blur also unbinds the INTZ surface and puts back the
   game's depth-stencil); `Picture::BeforeReset`.
3. The real `Reset`.
4. On success: `postReset` (Depth Blur, Ambient Occlusion and Edge Smoothing reset their state and back-buffer identity);
   `Overlay::AfterReset`.
5. Resources are rebuilt lazily on the next frame (status "Recreating after a video change...").

**Rule:** any module that owns a `D3DPOOL_DEFAULT` resource must register `preReset`, or `Reset` fails with
`D3DERR_INVALIDCALL` (standard D3D9 behaviour).

### 9.3 Lot and world load

Engine details: [engine/lot-loading-and-streaming.md](engine/lot-loading-and-streaming.md).

- **Shader creation** during a load reaches the registry's `CreatePixelShader` / `CreateVertexShader` chains. The Banding
  Fix creates its copies there, so DXVK translates them during the load and not in the frame where a room first appears.
- **Night Lighting** code runs inside the game's light calls where they are detoured
  ([features/night-lighting/level-light-share.md](features/night-lighting/level-light-share.md)).
- **World change** is detected in Night Lighting's Present hook when the light-cells pointer changes. Then: log
  `[NightTerrainRelight] World loaded (...)`; `LotLightBridge::OnWorldChanged()` drops chunk maps, smoothed maps and the
  atlas; `LevelLightShare::OnWorldChanged()`; `Settle` waits 5 to 60 s for the lot lamps, then arms one full terrain
  rebuild; "Terrain rebuilt" is logged when the countdown at `cells+0x38` reaches -1. After that, reconciliation relights
  only the chunks whose bake rectangle overlaps a changed lamp
  ([features/night-lighting/terrain-relight.md](features/night-lighting/terrain-relight.md)).
- The menu, the start note and the screen effects wait for the loaded-world gate ([section 12](#12-loaded-world-gate-and-start-note)).

### 9.4 A setting change in the menu

- Menu code runs on the render thread inside EndScene. It changes the feature's values and calls
  `NotifySettingChanged()`; the config is written by the pump about a second later.
- Read-live features pick the value up on the next frame or call. Base features reinstall on the pump thread after 2 s.
  Night Lighting schedules, and the next EndScene reinstalls on the render thread.
- A feature switch calls `Install()` / `Uninstall()` immediately on the render thread. Profiles, looks and Undo apply
  whole tables with `ApplyTableLive`.

---

## 10. Threads and synchronisation

### 10.1 The game's threads

Full map in [engine/main-loop-and-services.md](engine/main-loop-and-services.md).

| Thread | What runs there | Evidence |
|---|---|---|
| **Render thread = main thread** | Main loop `0x00ECA960`: ServiceManager main services, WorldManager and lot streaming, terrain update `0x00C845C0`, the whole D3D9 frame (BeginScene to Present), the window message pump | NOTAS-ILUMINACAO "Desempenho: mapa do motor"; `profiler_targets.tsv`; `frame_profiler.cpp` header |
| **Simulation (Mono) thread** | `MonoScriptHost::Simulate` (GC call at `0x00D819AA`, IdleSimulationCycle call at `0x00D81FDE`, sim job pump `ProcessJobs` called at `0x00D81FDA`), ServiceManager sim services `0x0059ED70`, script tasks `0x00D7FE80`, animation ticks. **The game UI runs here**; the render thread does not wait for it each frame | Notes; `profiler_targets.tsv` |
| Job workers ("JobThread", affinity mask 2) | `ExecuteJob 0x00599720`, for example resource read jobs `0x0072A4F0`; their mask-1 follow-ups run on the render thread | `profiler_targets.tsv` |
| Other game threads | Light tree gather (`level_light_share.cpp` counts calls "on another thread") | `level_light_share.cpp` |

### 10.2 The mod's own threads

| Thread | Created by | Runs |
|---|---|---|
| Loader thread (DllMain) | the ASI loader | Process check, instance mutexes, `Direct3DCreate9` detour, `CreateThread` |
| **Init / pump thread** | DllMain | Start-up (section 1.3), including startup `Install()` of every enabled feature; then `Update()` of every feature every 10 ms, autosave, crash-report refresh |
| Log writer | `ApexLog::Open` | Writes queued lines at most every 0.5 s |
| Shader precompile worker | `ShaderCache::Start` | `D3DCompile` of every registered variant, below normal priority |
| Frame Profiler sampler (developer) | Profiler, when sampling is on | Suspends the render and/or sim thread at `g_sampleHz` (default 2000) and copies 4 KB of stack |
| Frame Profiler writer (developer) | Profiler | Appends hitches to `ApexRadiance_Hitches.txt` at most once a second |
| Address Space monitor (developer) | `AddressSpace::Start` | Walks the address space with `VirtualQuery` every 10 s |
| DXT, RefPack and CAS workers | `features/dxt_codec.cpp`, `fast_refpack.cpp`, `fast_cas.cpp` | Split one large job across cores while the game's calling thread also works and waits; normal priority, 256 KB stack reservation, never destroyed ([features/performance/README.md](features/performance/README.md)) |
| Fast memory worker | `features/fast_memory.cpp` | Returns big blocks to Windows in the background |
| Light map smoothing worker | `features/lightmap_smooth.cpp` | Detached worker that decodes and prepares terrain light maps for the world light smoothing |
| Short-lived helper threads | `features/captures.cpp`, `apex_gui.cpp`, `features/frame_profiler.cpp` | WIC PNG encoding of capture screenshots and folder work; opening folders in Explorer (COM) and URLs; profiler report file appends |

### 10.3 Which Apex code runs on which thread

| Code | Thread |
|---|---|
| Registry callbacks, `Hooked_EndScene` / `Hooked_Reset`, render callbacks, post-scene effects, Picture, Night Lighting draw hooks and Present dispatcher, overlay drawing | Render |
| Menu code (`RenderCustomUI`, switches calling `Install` / `Uninstall`, `ApplyTableLive`) | Render (inside EndScene) |
| Window procedure (input, shortcuts) | The thread that pumps the game window, the render thread (inferred: the Input service pumps messages inside ServiceManager) |
| Startup `Install()`, feature constructors, `Update()`, autosave | Init / pump thread |
| Night Lighting terrain and light logic, object light bridge and level light share refreshes | Render (the game calls them there, or Night Lighting's Present hook drives them) |
| Game-function detours in general | Whatever thread the game calls them on. Check `GetCurrentThreadId()` against a recorded thread when it matters, as `level_light_share.cpp`, `object_light_bridge.cpp`, `rig_tracker.cpp` (`g_drawThread`) and Ambient Occlusion (`simRenderThread`) do |
| `CreatePixelShader` / `CreateVertexShader` hooks | Usually render (*Unverified* which thread calls them during loads) |

### 10.4 Synchronisation rules

1. **Game state is touched only on the render thread.** Requests from elsewhere set a flag that the next Present handles
   (`object_light_bridge.cpp` `g_refreshRequested`, `level_light_share.cpp`).
2. **Freeing D3D resources used by draw hooks, or rewriting code the render thread runs, happens on the render thread**
   (Night Lighting's `DeferredReinstall`), or uses `WriteCodeSuspended` / the layered chains (section 4.6).
3. **Registry state:** see section 3.3. Do not wait, inside a registry callback, on a lock that another thread holds
   while it registers; the profiler uses `try_lock` in its Present hook for this reason.
4. **Settings read by hooks** are plain globals or `std::atomic` with relaxed loads. A torn read of a float slider is
   accepted. A module-level mutex is used only where Install and hooks can overlap (for example `lot_light_bridge.cpp`
   `UpdateHooks`, `PostScene`'s effect mutex).
5. **Callback slots** (`ExtraHooks` observers, depth substitution) are lock-free atomics; render-callback lists copy
   under a short mutex.
6. **No file I/O on the render thread** in hot paths. The logger and the profiler use writer threads; config saves run on
   the pump; Report PNG encoding runs on its own threads.
7. **Detours transactions** call `DetourUpdateThread(GetCurrentThread())` only; other threads are not suspended, so
   detouring a function another thread is executing is a race. Hot multi-thread game functions are hooked with
   `WriteCodeSuspended` or the profiler's `AttachSafe` (all other threads suspended and checked).

---

## 11. Coexisting with official S3SS

### 11.1 Detection

`S3SSDetect` ([framework/s3ss_detect.h](../framework/s3ss_detect.h)) recognises modules by strings in their read-only
data (none exports anything). The needles are stored encoded so that Apex itself never matches them.

| Module | Recognised by | Result |
|---|---|---|
| Official S3SS | its log header `"S3SS Log - Started at "` and ImGui window ID `"###S3SSWindow"` | `s3ssLoaded`; module range recorded (`IsInS3SS`) |
| Old combined build | the same two strings plus `"Sims3 Settings Setter Apex Edition"` | `oldCombinedBuild`: Apex Radiance keeps its features off and shows a banner |
| Previous standalone | the old product name without S3SS's strings, or the file name `S3SSApex.asi` | `oldStandalone`: banner "An older S3SSApex.asi is also installed; delete it from Game\Bin" |

`Scan()` runs on the init thread; `Rescan()` runs at the first Present and before installing features. `Summary()` gives
one line for the log and the compatibility page.

### 11.2 Two Detours chains and two overlays

- Each DLL links its own Detours and patches the same DXVK function bodies. Detours relocates an existing `E9` into the
  new trampoline, so chains compose in either order, and **the module installed last runs first** (outermost).
- `CreateDevice` is installed from inside the first `Direct3DCreate9` call, so Apex's and S3SS's transactions never race
  on its prologue (section 2.1). Apex never detaches at process exit.
- S3SS's registry registers nothing; Apex's `CallOriginal*` paths may pass through S3SS's empty dispatch when S3SS is
  inner. This is harmless.
- Apex makes no present-parameter changes, so S3SS's borderless window and Resolution Spoofer have no conflict.
  Borderless stays in S3SS.
- The Picture pass runs at Apex's EndScene. For SDR either order is correct: with Apex outer, S3SS's menu draws after
  the filters; with Apex inner, the menu counts as UI through the scene copy. Calling `EndScene` from a pass would
  re-enter S3SS's hook and draw its menu twice: never do it.
- Each DLL has its own ImGui context. Apex uses `apex_radiance_imgui.ini`, window ID `###ApexWindow` and
  `NoMouseCursorChange`. S3SS's window procedure eats all input while its menu is open; Apex subclasses at its first
  Present so it is outer, and passes on everything its ImGui does not want (section 2.5).
- S3SS does not hook Set/GetDepthStencilSurface, so the Depth Blur swap has no competitor. ImGui's DX9 state blocks do
  not include the depth-stencil binding.

### 11.3 S3SS.toml

Apex reads `S3SS.toml` read-only for S3SS's intent (`[patches.<name>].enabled`, overlay disabled) and for the
migration.

`S3SSDetect::SplitLevelFixActive()` reports S3SS's Split-Level Lighting Fix (enabled in `S3SS.toml`, or `GetLotID`
`0x6BC020` no longer holds its original bytes); Apex's Every-Story Ground Light then stays out of the way.

### 11.4 Game-code sites and the conflict guard

| Apex feature | Overlap with S3SS | Policy |
|---|---|---|
| Frame Profiler (developer; many entry detours and hand hooks) | Avoids S3SS LotStreamingOptimizations' sites; skips any target whose bytes differ | Cooperate; label "owned by S3SS" in its hook table |
| Night Lighting level light share (CALL redirects to LPWAL at `0x6A1187` / `0x6A126F` / `0x6A3336`, return-address test `0x69FE19`) | S3SS LightingQuality detours LPWAL's entry `0x69FD60` and calls it N times: no byte overlap, but Apex's tests run N times | Cooperate |
| Night Lighting terrain bake sites (visitor `0xC29626`, story gate `0xC294D9`, arm sites `0x6B6516` / `0x6B60D3` / `0x6B6618`, lot pass `0xC7F87D`, street lamp colour `0x6BE18C`) and the localized relight (writes `chunk+0x55`) | None found. S3SS's LotStreamingOptimizations detours `WorldManager::Update` (`0xC6D570`), which calls the terrain update; Apex never patches that function | Cooperate |
| Other Night Lighting sites (rig tracker, object light bridge, lamp colour), map view (calls `0x73E060`, the getter S3SS also uses) | None found | Cooperate |
| Every-Story Ground Light (`GetLotID` `0x6BC020`) | S3SS's Split-Level Lighting Fix patches the same function | Apex stays off while S3SS's fix is active |

`ConflictGuard` ([framework/conflict_guard.h](../framework/conflict_guard.h)) declares the intended four-stage guard:
defer game-code installs until S3SS has loaded its patches (implemented by the settle wait, section 1.3), refuse known
same-byte pairs from `S3SS.toml`, byte-compare every site at install and name the module a foreign E8/E9 lands in, and
re-verify every second without restoring over foreign bytes. Only the interface exists: `MayInstall` always allows,
`Query` reports no conflict and `Tick` does nothing. Features rely on their own byte checks at install.

---

## 12. Loaded-world gate and start note

The screen effects wait until a world is really drawn, and the menu and the start note until the startup loading is
over, so nothing runs over a loading screen.

- **World active** (`WorldSession::IsActive`, [features/world_session.h](../features/world_session.h), read-only): the
  WorldManager global (`GameAddr` `WorldManagerPtr`, `0x011ECBC4` on Steam) is non-null, its active byte `+0x41` is set,
  its mode `+0x1B4` is 1 to 3, and the native startup/loading window is absent. Fields:
  [engine/lot-loading-and-streaming.md](engine/lot-loading-and-streaming.md).
- **Loading window:** the window with ID `0x95947678` is created by `0x00EC7DB9` and removed by `0x00EC7A60`.
  `LoaderDismissed()` resolves the UI-service getter (`GameAddr` `UiServiceGetter`, `0x0050AB70` on Steam), checks that
  its code is `A1 <global> ... C3`, reads the service, calls its root getter (vtable +4) and the root's child lookup
  (vtable +0xF4) for that ID. A missing getter, service or root, or an exception, fails closed (the world counts as not
  active). A world-loaded flag alone is not enough, because the world can be active behind the loading screen.
- **World drawn** (`WorldSession::InWorld`): the world is active, the last complete frame had at least 48
  depth-writing scene draws (counted by PostScene) for 500 ms in a row, and Night Lighting's world-live signal (terrain
  drawn; true with Night Lighting off) is set. It stays open while the world is active (the save screen and in-game
  menus keep the same look) and closes with the world. Color (Picture), Ambient Occlusion, Edge Smoothing and Depth
  Blur do nothing while it is closed, so the main menu and load screens keep the game's picture.
- **Menu availability** (`apex_gui.cpp` `UpdateMenuAvailability`, render thread, at most every 200 ms): the startup state
  is `Running` (or `RefusedOldBuild`, so the banner can show) and either the world is drawn or the startup loading window
  is gone (from the world selector on); then 3 continuous seconds must pass. The result is cached in an atomic
  (`g_menuAvailable`); the window thread never reads game memory (`Client::CanOpen`). While it is false the menu is
  closed and cannot open, and shortcuts do not run.
- **Start note:** the "Apex Radiance is ready, press <key>" note starts once per process, only when the menu first
  becomes available (first Present alone does not start it), if `[ui] start_note` is on and the menu is not already
  open. It lasts 8 s (`kHintMs`). A later load hides and pauses a running note; returning does not restart its lifetime.
  Opening the menu ends it.
- **Depth Blur** also waits for Night Lighting's load-settled signal and keeps its own settling state
  (`WorldSession::Settled`: 3 s of continuous activity, reset whenever the world is not playable), then holds 2 s and fades
  in over 1 s; it skips frames with fewer than 48 depth-writing draws (a frozen save screen). It is updated at the
  Present frame boundary and checked again before blurring. While it is not ready,
  every blur and debug GPU pass is skipped, the autofocus snaps on return, the map-view fade resets, and the shared depth
  is not released ([features/depth-blur.md](features/depth-blur.md)).
- No game writes or hooks are involved. Validation: [tools/loading_gate_test](../tools/loading_gate_test/) and the
  Depth Blur validation page.

---

## 13. Report storage

Report a problem ([features/bug-reports.md](features/bug-reports.md)) keeps its controls on the overlay (render)
thread and adds no game-memory hook or light polling.

- `Captures::WriteText` serialises completed diagnostics; failed writes are kept for an explicit retry.
- Capture receipt, description, session and PNG job state share one lock (`g_lock`). WIC threads only write the supplied
  image and mark their own folder complete under the lock; translation and completion notices are handled on the render
  thread. `Saving()` includes queued and encoding PNG work; `ScreenshotPending()` only suppresses overlays before the
  picture is read at the next Present.
- Last-save polling performs no file-system scan. The post-save description card reads its target folder once when
  opened. Saves use atomic replacement (`ApexUtil::WriteFileAtomic`), so a failed replacement keeps the previous note.
  `ReadDescription` rejects reparse files and reads at most 4 KB.
- Library scanning is limited to the visible page; file contents are read on request; reparse directories are not
  followed. Menu removal renames capture folders into `.Removed` (collision-safe), with an explicit Undo. Explorer runs
  on a separate COM thread.

---

## 14. How to add a new feature

1. **Study first.** Read the relevant [engine pages](engine/) and the feature pages' *Rejected approaches* and history.
   Get evidence: Light Probe and Light Diag captures, the decompile in `S3SS-dev\re\out`, engine_map. Do not guess
   addresses or constants.
2. **Files.** Put the feature in `patches/<name>_patch.cpp` (or a `features/<name>.cpp/.h` module plus a thin patch) and
   add it to `ApexRadiance.vcxproj`'s `ClCompile` list.
3. **Class and registration:** `class XPatch : public ApexPatch` with `XPatch() : ApexPatch("XName")`. `"XName"` is the
   TOML table `[patches.XName]`: choose it once, never rename it. Register with `APEX_REGISTER_FEATURE(XPatch,
   {.displayName, .description = "... Part of " APEX_PRODUCT_NAME ". Credits: @loinyx", .category, .experimental,
   .enabledByDefault, .supportedVersions = VERSION_STEAM (unless verified elsewhere) or a .gameCodeGroup,
   .technicalDetails})`.
4. **Install / Uninstall:** idempotent (`if (isEnabled) return true;`), `lastError.clear()`, `Fail(msg)` on errors,
   `isEnabled` set at the end. Resolve game addresses through `GameAddr` (add the address and its signature to
   `framework/game_addresses.*`) or `GameAddress`. Write through `MemPatch::Write*` with `expected` bytes and an undo
   list; restore with `RestoreAll`. Use `DetourBatch`, or the layered chains when another module may hook the same
   function. Startup `Install()` runs on the init thread, after the first Present.
5. **Settings:** register in the constructor with English descriptions and defaults. Choose the reinstall policy: read
   live; or defer to the render thread when Uninstall frees D3D objects or rewrites code the render thread runs. Add the
   setting to the menu's reset and profile handling ([ui.md](ui.md)).
6. **D3D hooks:** register in `Install`, `UnregisterAll(kHookName)` in `Uninstall`. Priority: `First` for observers and
   frame-boundary resets that must see every original draw; `Early` for work that must precede draw replacement;
   `Normal` for draw replacement; `Last` for probes and captures; never -2000, -1000 or +1000. Return `Skip` only when
   you issued the draw yourself, guarded by an own-call flag. Use `CallOriginal*` for state changes no module may see.
   Post-scene effects: `PostScene::Add(order, fn)` with a new order value, drawing with DrawPrimitiveUP. Readable depth:
   `DepthShare::Request(true/false)`, handling `Texture() == nullptr` with `DepthShare::Status()`. `D3DPOOL_DEFAULT`
   resources: `RenderCallbacks::Add(preReset / postReset)`. HLSL: register every variant with `ShaderCache::Add`.
7. **Threads:** touch game objects only on the render thread; queue from elsewhere. No blocking I/O or per-draw logging
   on the render thread. Atomics for values the UI writes and hooks read.
8. **UI:** follow the [apex-menu](../.agents/skills/apex-menu/SKILL.md) skill: main controls on the card with hints,
   tuning under Advanced, developer diagnostics in `RenderDeveloperUI`, English source text with translations in
   `i18n/tr_*.cpp`, no visible credit lines. Run the audit.
9. **Logging:** a `[XName]` prefix. Log install success with the resolved addresses. Warning for "pattern not found /
   bytes differ", Info for state changes. `LOG_DEBUG` does not reach the file unless verbose logging is on.
10. **Test:** offline harness in `tools/` where possible ([workflow.md, Testing](workflow.md#4-testing)), then in game.
11. **Docs:** write `docs/features/<name>.md`, `docs/validation/<name>.md` and, when there is history,
    `docs/history/<name>.md` from [templates/](templates/), following [DOCUMENTATION-STANDARD.md](DOCUMENTATION-STANDARD.md).
    Update this page's registration table (3.5) and thread tables (10.2, 10.3) when the feature adds hooks or threads.

---

## 15. Known gaps

- Whether the game ever creates a non-HAL device (for example NULLREF) was not checked; logs show one HAL device.
- Which thread calls `CreatePixelShader` / `CreateVertexShader` during loads was not measured.
- The ASI loader DLL name (`wininet.dll` = Ultimate ASI Loader) is inferred from its README, not checked by hash.
- The conflict guard is an interface only (section 11.4).
- The loaded-world gate's behaviour across every loading transition (travel, save load, main menu return) needs in-game
  validation; the mock checks and compilation do not show loading screens.

## See also

- [history/architecture.md](history/architecture.md): the combined build, the standalone split plan and decisions,
  release-candidate notes.
- [workflow.md](workflow.md), [ui.md](ui.md), [engine/](engine/), [removed-features.md](removed-features.md).
