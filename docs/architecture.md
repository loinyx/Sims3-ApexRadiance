> Current standalone update (2026-10-02): one unified ASI now includes optional developer tools, off by default and selected at startup. The compile-time flavor descriptions below are historical. See [developer-mode.md](features/developer-mode.md).

# Architecture: how Apex Radiance hooks the game and D3D9

> Historical architecture baseline: the following covers the **frozen combined build**: Apex inside a fork of Sims3SettingsSetter, tag `combined-final`, commit
> 45e36e2, tree `%USERPROFILE%\Desktop\S3SS-dev\Sims3SettingsSetter\`. File paths below are relative to that tree.
> The **standalone** ASI, **Apex Radiance** (`ApexRadiance.asi`; project folder still `S3SSApex\`), is **in progress**.
> Its design is summarised in
> [section 12](#12-the-standalone-split-planned--in-progress). Where the standalone differs (HDR, Native HDR and Ambient
> Occlusion removed; Smooth Streaming, Script GC Scheduler and Service Frame Budget removed; Frame Profiler dev-only;
> Picture filters in their own SDR module; framework rewritten from scratch; Night Lighting restarted from v0.1.0), this
> document says so, and section 12.0 lists the decisions. Anything marked "(unverified)" or
> "(inferred ...)" was not confirmed at runtime.
>
> Game addresses are for TS3W.exe Steam 1.67.2.024037 (PE TimeDateStamp `0x52DEC247`, image base 0x00400000, no ASLR).

Related docs: [workflow.md](workflow.md) (build, install, test), [engine/main-loop-and-services.md](engine/main-loop-and-services.md)
(main loop, services, threads), [removed-features.md](removed-features.md) (HDR / Native HDR / AO).

---

## 1. Load and initialisation

### 1.1 How the ASI gets loaded
- The mod is a Win32 DLL renamed `.asi` (`Sims3SettingsSetter.vcxproj`: `TargetName` = `S3SSApex`, `TargetExt` = `.asi`).
  An ASI loader in `Game\Bin` loads every `*.asi` in that folder. README "Installation" lists dxwrapper or Ultimate ASI
  Loader. The user's `Game\Bin` holds `wininet.dll`, `S3SSApex.asi`, `MonoPatcher.asi` and `Sims3Performance.asi`.
  That `wininet.dll` is the UAL build: README names `wininet.dll` as the Win32 UAL file (inferred, not hashed).
- The loader loads ASIs in folder enumeration order (PLANO-SEPARACAO section 4, "Output name"). `S3SSApex.asi` sorts before
  `Sims3SettingsSetter.asi`.
- **Never keep two copies of the mod in `Bin`.** The combined build is a whole S3SS. The old name was
  `Sims3SettingsSetter.asi`.

### 1.2 `DllMain` (`dllmain.cpp`, `DLL_PROCESS_ATTACH`)
These steps run in order, under the loader lock:
1. `DisableThreadLibraryCalls`.
2. `InitializeAllocatorHooks()` (`allocator_hook.cpp`). This is S3SS's mimalloc hook of `MSVCR80.dll`. It does nothing
   unless the config enables it.
3. **Process check.** `GetModuleFileNameA(NULL)`. If the file name is neither `TS3.exe` nor `TS3W.exe`, it logs
   `LOG_CRITICAL` (this only reaches `OutputDebugString`, because the logger does not exist yet) and returns `FALSE`.
4. **CPUID topology fix**, only on Intel hybrid CPUs (`IsIntelHybridCPU`: vendor `GenuineIntel`, and CPUID.07H EDX bit 15
   or family 6 model >= 0x97). It patches two `SAR` to `SHR` in the game's topology detector (`FUN_006135e0`):
   - pattern `B8 04 00 00 00 33 C9 0F A2 89 44 24 ?? 8B 44 24 ?? C1 F8 1A`, +18: `F8` becomes `E8`;
   - pattern `51 C1 FA 18 F6 D0 22 D0`, +2: `FA` becomes `EA`.

   It writes through `PatchHelper::WriteBytes` with an expected-byte check. The messages are held in
   `g_deferredTopologyLog` and replayed once the logger exists. The comment warns: do not bring back the old
   `CALL -> MOV EAX,1` "fix".
5. `CreateThread(HookThread)`. If that fails, `DllMain` returns `FALSE`.

### 1.3 `HookThread` (`dllmain.cpp`): the init sequence and the Update pump
The step numbers match the comments in the code:
1. `ConfigPaths::EnsureDirectoryExists()`. Then `Logger::Handler::Initialize(ConfigPaths::GetLogPath())`. If that
   fails, it falls back to `Game\Bin\S3SS_LOG.txt`. `SetDebugMode(true)` is set only in `_DEBUG` builds. The deferred
   CPUID lines are replayed here.
2. **D3D9 hooks, as early as possible.** `UISettings::PeekDisableOverlayEarly()` and
   `BorderlessWindow::PeekEnabledEarly()` parse the TOML directly, before the full load. The branches:
   - Overlay off and borderless off: no D3D hooks at all (headless).
   - Otherwise: `InitializeD3D9Hook(!disableOverlay)`.

   A failure here is not fatal.
3. `DetectGameVersion()` (`patch_system.h`) reads the PE `TimeDateStamp` of the exe. Known values:
   - Retail `0x52D872DA`
   - Steam `0x52DEC247`
   - EA 1.69.47 `0x6707155C`
   - EA 1.69.43 `0x568D4BAC`

   The result goes to `g_gameVersion` and `g_exeTimeDateStamp`.
4. `CPUFeatures::Get()` (logs SSE4.1, AVX2 and FMA).
5. `Migration::CheckAndMigrate()`: the old `Game\Bin\S3SS.ini` is converted to TOML when the TOML does not exist yet.
6. `OptimizationManager::Get()`. The first call runs `PatchRegistry::InstantiateAll`, which **constructs every patch**. It
   logs "Registered N patches". Patch constructors run here, on the hook thread.
7. `ConfigStore::LoadAll()`: `[settings]`, `[config]` and `[qol.*]`, but **not** `[patches]`.
8. `g_hookManager.Initialize()`: S3SS's settings hooks. This is skipped when `[qol.ui].disable_hooks` is set. Details:
   - `VTableManager` finds a vtable through the constructor pattern `56 8B F1 C7 06 ?? ?? ?? ?? 33 C9 89 4E 0C`.
   - It detours slot `0x3C` (VariableRegistry) and slot `0x44` (CustomDebugVar).
   - `ConfigRetrievalHook` finds its function by the pattern
     `83 EC 2C 8B 44 24 ?? 53 55 56 57 33 DB 8B F1 BF ?? ?? ?? ?? 50 8D 4C 24 ?? 89 5C 24 ?? ...` and checks that the
     string at +16 is `"Services/ConfigRegistry"`.
9. `ConfigStore::LoadPatches()` calls `OptimizationManager::LoadFromToml`. For each `[patches.<Name>]` table it loads the
   settings, then `enabled`, then calls `Install()` or `Uninstall()`. **Startup installs happen here, on the hook thread.**
10. `EnsureEnabledByDefaultPatchesAreEnabled()` installs patches that have `enabledByDefault` and no `enabled` key in the
    TOML.

The thread then logs "Starting message loop" and loops forever:
- `PeekMessage(PM_REMOVE)`. On `WM_QUIT` it returns.
- When there is no message:
  - `MemoryMonitor::Get().Update()`;
  - `patch->Update()` for **every** registered patch (enabled or not);
  - `Sleep(10)`.

**The `Update()` pump is the only periodic tick on this thread.** The base `OptimizationPatch::Update()`
(`optimization.h`) does the debounced reinstall after a setting change:
- `NotifySettingChanged()` records the time and sets `pendingReinstall`.
- Once `SETTING_CHANGE_DEBOUNCE` (2 s) has passed, `Update()` calls `Uninstall()` and then `Install()`, **on the hook thread**.

Apex patches override this. See [section 5.3](#53-how-a-patch-declares-and-uses-settings).

**Evidence of the real order** comes from `Documents\...\S3SS\S3SS_LOG.txt` of a 28/09 session:
- "Starting message loop" is logged **before** "[CreateDevice] Device created (type=1)". All startup patches were installed
  before the game created its device.
- The registry initialises only at the first EndScene: "[D3D9Hooks] Hook registry initialized successfully" comes after
  "[EndScene] ImGui initialized".
- Patches that register registry callbacks at install time are therefore only storing callbacks in vectors. The callbacks
  start firing once the detours exist.
- This ordering is timing, not a guarantee (unverified that the device is always created later).

The `DetectGameVersion` step runs **after** `InitializeD3D9Hook`. The D3D hooks do not depend on the version.

### 1.4 Shutdown (`DllMain`, `DLL_PROCESS_DETACH`)
Nothing runs at process exit (`lpReserved != NULL`), apart from static destructors. The logger's `ExitFlush` writes any
pending lines with `try_lock` (`logger.cpp`).

On `FreeLibrary` (`lpReserved == NULL`) the order is:
1. `Uninstall()` of every enabled patch.
2. `PostThreadMessage(WM_QUIT)` to the hook thread, then `WaitForSingleObject(..., 5000)`.
3. `FrameProfiler::Shutdown()`. This must come before the D3D hooks are removed: it unregisters its registry hooks and
   detaches its game-function hooks.
4. `CleanupD3D9Hook()`:
   - `D3D9Hooks::Internal::Cleanup()` (detaches the 15 registry detours);
   - restores the WndProc;
   - detaches CreateDevice, EndScene and Reset.
5. `g_hookManager.Cleanup()` (the settings hooks).
6. `Logger::Handler::Close()`.

UAL never unloads, so in practice this path never runs (PLANO-SEPARACAO 2a.6, 2b hazard 2).

---

## 2. D3D9 device hooks (`d3d9_hook.cpp` / `d3d9_hook.h`)

The game renders through its own `d3d9.dll`, which is the official DXVK 3.1.1 (user setup memory, sha256 `265888c3...`).
Every detour below patches **function bodies inside DXVK's d3d9.dll** with Microsoft Detours. The vtables are never
written.

| Hook | Where installed | Vtable slot | Function |
|---|---|---|---|
| `IDirect3D9::CreateDevice` | `InitializeD3D9Hook`, hook thread, step 2 | IDirect3D9[16] | `HookedCreateDevice` |
| `IDirect3DDevice9::EndScene` | `AttachDeviceHooks`, inside `HookedCreateDevice` | device[42] | `HookedEndScene` |
| `IDirect3DDevice9::Reset` | same | device[16] | `HookedReset` |
| 15 more device methods | `D3D9Hooks::Internal::Initialize`, first EndScene | see section 3 | `Hooked_*` in `d3d9_hook_registry.cpp` |
| 6 more device methods | `ExtraHooks::EnsureInstalled`, on first use | see section 4.1 | `d3d9_extra_hooks.cpp` |

`Present` (device[17]) is hooked **only by the registry** (`Hooked_Present`). `hooks.cpp` contains an older
`Present_Hook` and a global `Original_Present`. That code is dead: nothing attaches it.

### 2.1 `InitializeD3D9Hook(enableOverlay)`
- `Direct3DCreate9(D3D_SDK_VERSION)` creates a throwaway IDirect3D9 (a DXVK object). The code reads vtable[16] from it,
  releases the object, and detours `CreateDevice` in one Detours transaction.
- No dummy device is created. The comment says this avoids loading a HAL driver just to throw it away.
- This works only if it runs before the game creates its device (see 1.3).
- `g_overlayEnabled = enableOverlay`. With `false`, the device hooks are still installed (borderless needs them), but no
  ImGui context and no WndProc are created.

### 2.2 `HookedCreateDevice`
1. For `D3DDEVTYPE_HAL` only: `EnforceBorderlessWindowedParams(pp, "CreateDevice")`. When a borderless mode is set, it
   forces `Windowed = TRUE`, `FullScreen_RefreshRateInHz = 0` and `SwapEffect = DISCARD`, and strips
   `D3DPRESENTFLAG_LOCKABLE_BACKBUFFER`.
2. **HDR back buffer (combined build only):** `HdrOutput::Get().BeforeCreateDevice(pD3D, hFocusWindow, pp)`
   (`hdr_output.cpp`).
   - If the config is not loaded yet, it parses `[qol]` itself.
   - It returns false (no change) when any of these holds: HDR is off; Windows HDR is off on the game's monitor;
     `IDirect3D9::QueryInterface(ID3D9VkExtInterface)` fails ("needs DXVK 2.3 or newer").
   - Otherwise it calls `ext->UnlockAdditionalFormats()`, saves `pp->BackBufferFormat` and sets it to
     `D3DFMT_A16B16G16R16F`.
3. It calls the original.
   - If HDR changed the format and the call failed: `HdrOutput::CreateDeviceFailed(pp)` restores the format and the call
     is **retried once**.
   - On success with HDR: `AfterCreateDevice`, then `ApplyColorSpace`, which calls
     `ID3D9VkExtSwapchain::SetColorSpace(EXTENDED_SRGB_LINEAR)` (scRGB) and logs the EDID luminance.
4. On success, for HAL only: `AttachDeviceHooks(device)` detours EndScene (vtable[42]) and Reset (vtable[16]) once
   (`g_deviceHooksAttached`).
   - A later device whose EndScene pointer differs only gets a warning ("overlay may not render").
   - If a Detours transaction fails, the flag is cleared so the next device retries.
5. If a borderless mode is set, the window is handed to `BorderlessWindow::SetWindowHandle` straight away, before the game
   shows or sizes it.

**Non-HAL devices** (`D3DDEVTYPE_REF`, `D3DDEVTYPE_NULLREF`, `D3DDEVTYPE_SW`) pass through untouched: no borderless
change, no HDR change, no hooks attached. The only device in the log is "Device created (type=1)" (HAL). Nothing shows the
game creating a NULLREF device (unverified; no evidence either way).

### 2.3 `HookedEndScene`: overlay, first-call init and fire points
Per call:
1. Re-entry guard `g_inEndScene`.
2. `TestCooperativeLevel`. If the device is lost (any failure other than `D3DERR_DEVICENOTRESET`), it passes straight
   through to the original.
3. **First call only** (`g_deviceInitDone` / `g_deviceInitializing` CAS):
   - Set `g_pd3dDevice = pDevice`. This is the global device pointer that Apex modules use (`extern` in `d3d9_hook.h`).
   - Find the window from `GetCreationParameters().hFocusWindow`, falling back to the swap chain's `hDeviceWindow`.
     Store it in `g_hookedWindow`.
   - `BorderlessWindow::SetWindowHandle`.
   - If the overlay is on:
     - `ImGui::CreateContext` with `NavEnableKeyboard`, `NavEnableGamepad` and `NoMouseCursorChange`;
     - the default font at 13 px x `FONT_OVERSAMPLE` (3);
     - `SetWindowLongPtr(GWLP_WNDPROC, HookedWndProc)`, then a check that it took;
     - `ImGui_ImplWin32_Init` and `ImGui_ImplDX9_Init`.
   - `D3D9Hooks::Internal::Initialize(pDevice)` installs the registry detours (section 3).
4. `BorderlessWindow::TickReapply()`.
5. **`RenderCallbacks::Fire(endSceneBeforeOverlay)`**. This is the end of the game's frame, before any overlay. Users:
   - `frame_capture_patch.cpp` `OnEndScene`;
   - `night_terrain_relight_patch.cpp` `DeferredReinstall` (added in the patch constructor).
6. `HdrOutput::Get().BeforeOverlay(dev)`. If the frame ended on the scene (no game UI), the scene copy is taken now.
7. ImGui frame (only when initialised):
   - `NewFrame`;
   - display size and style scale are overridden from the back buffer in borderless mode (resolution spoofing): scale =
     height / 1080, clamped to 0.75 .. 3;
   - `SettingsGui::Render()`, which draws the whole S3SS window including the Apex and Display tabs;
   - `Render` and `ImGui_ImplDX9_RenderDrawData`.
8. **`HdrOutput::Get().OnEndScene(dev)`** (combined build):
   - sets the lamp gain for the next frame;
   - `HdrNative::Update`;
   - the full-screen HDR and/or Picture pass. It runs on the finished frame, after the overlay; the UI is told apart by the
     scene copy.
9. The original `EndScene`.

### 2.4 `HookedReset`
1. `ImGui_ImplDX9_InvalidateDeviceObjects`, if ImGui is up.
2. `EnforceBorderlessWindowedParams(pp, "Reset")`.
3. **`RenderCallbacks::Fire(preReset)`**: patches release their `D3DPOOL_DEFAULT` resources.
4. `HdrOutput::BeforeReset(pp)` releases the HDR resources and forces `A16B16G16R16F` into `pp` again, because the game
   asks for its own format.
5. The original `Reset`.
6. On success:
   - `HdrOutput::AfterReset` (colour space again);
   - **`RenderCallbacks::Fire(postReset)`**;
   - borderless is re-applied;
   - `ImGui_ImplDX9_CreateDeviceObjects`.

### 2.5 `HookedWndProc`
- Mouse messages are scaled from window space to back-buffer space (`ScaleMouseCoords`, borderless / resolution spoofer
  only). They then go to `ImGui_ImplWin32_WndProcHandler`.
- The toggle key (`[qol.ui].toggle_key`, default `VK_INSERT`) flips `SettingsGui::m_visible`. It is a bare
  `wParam == key` test that ignores modifiers.
- While the menu is visible, mouse, keyboard and char messages are **eaten** (return 0). The comment says this "doesn't
  fully work".
- Everything else goes to `CallWindowProc(original_WndProc)`.
- The WndProc runs on the thread that pumps the game window. That is the render thread: the Input service `0x00598660`
  pumps messages through `0x00410890` inside ServiceManager on the render thread (engine_map `profiler_targets.tsv`;
  inferred for WndProc).
- Apex dev hotkeys (Ctrl+Shift+F7 light probe, F8 light diag, F9 frame capture; F10 is the dead call_trace) do **not**
  use the WndProc. They are polled with `GetAsyncKeyState` from Present hooks (`light_probe.cpp`, `light_diag.cpp`,
  `patches/frame_capture_patch.cpp`).

---

## 3. The hook registry (`d3d9_hook_registry.cpp` / `.h`)

This is S3SS's "D3D9Hooks" system. The Apex changes to it: a `std::recursive_mutex`, the extra `CallOriginal*`
functions, and per-hook profiler timing.

### 3.1 Hooked device methods (installed by `Internal::Initialize`, first EndScene, one Detours transaction)
| Method | Vtable index | Callback type | Per-hook timing |
|---|---|---|---|
| Present | 17 | `PresentHook` | no |
| CreateTexture | 23 | `CreateTextureHook` | no |
| CreateRenderTarget | 28 | `CreateRenderTargetHook` | no |
| SetRenderTarget | 37 | `SetRenderTargetHook` | no |
| BeginScene | 41 | `BeginSceneHook` | no |
| SetViewport | 47 | `SetViewportHook` | no |
| SetTexture | 65 | `SetTextureHook` | no |
| DrawPrimitive | 81 | `DrawPrimitiveHook` | **yes** |
| DrawIndexedPrimitive | 82 | `DrawIndexedPrimitiveHook` | **yes** |
| CreateVertexShader | 91 | `CreateVertexShaderHook` | no |
| SetVertexShader | 92 | `SetVertexShaderHook` | no |
| SetVertexShaderConstantF | 94 | `SetVertexShaderConstantFHook` | no |
| CreatePixelShader | 106 | `CreatePixelShaderHook` | no |
| SetPixelShader | 107 | `SetPixelShaderHook` | no |
| SetPixelShaderConstantF | 109 | `SetPixelShaderConstantFHook` | no |

The registry installs all 15 unconditionally, even when nothing is registered.

### 3.2 Registration, order and results
- **Register:** `D3D9Hooks::Register<Method>(name, std::function, Priority = Normal)`. The call takes the registry mutex,
  does `push_back`, then `std::sort` by `static_cast<int>(priority)`. `UnregisterAll(name)` removes every entry with that
  name from all 15 vectors.
  - Convention: one name per module (`kHookName`, e.g. `"LotLightBridge"`, `"PostScene"`, `"NightTerrainRelight"`).
  - A module can register several callbacks of the same type under one name. The profiler registers two Present hooks
    under `"FrameProfiler"`.
- **Priority** is a plain `enum class Priority { First = 0, Early = 25, Normal = 50, Late = 75, Last = 100 }`. Lower runs
  first. Any int can be cast in: the Frame Profiler uses `-1000` (start) and `+1000` (end) (`frame_profiler.cpp`
  `kPrioStart` / `kPrioEnd`).
- **Equal priorities have no defined order.** `std::sort` is not stable (`SortHooks`; PLANO-SEPARACAO 2d). Several
  modules sit at `Priority::First` on Present and on the draws. None of them may depend on running before another
  `First` hook.
- **Dispatch** (`Execute*Hooks`): hold the registry mutex, then call each callback in order.
  - `HookAction::Continue`: go on to the next callback.
  - `HookAction::Skip`: stop the chain; the original is **not** called and the method returns `S_OK`.
  - `HookAction::Block`: stop the chain; the method returns `E_FAIL`.

  The mutex is released before `Hooked_*` calls the original.
- `DeviceContext { device, skipOriginal, overrideResult }` is created per call on the stack of `Hooked_*`.
- **`CallOriginal*`** calls the saved trampoline directly and runs no callback: `CreateRenderTarget`, `SetRenderTarget`,
  `SetViewport`, and the Apex-added `DrawIndexedPrimitive`, `DrawPrimitive` and `SetVertexShaderConstantF`. Use these to
  issue a call that must not be seen by any module.

### 3.3 Skip in practice: the draw-replacement pattern
Night Lighting's `lot_light_bridge.cpp` (`UpdateHooks`, `OnDraw` / `OnDrawTracked` / `OnDrawInner`) registers DIP and DP
at the default `Normal` priority. For a draw it handles:
1. It sets its own shader and constants (`g_inOwnCall = true`).
2. It **re-issues the draw through the device** (`ctx.device->DrawIndexedPrimitive(...)`). That call goes through
   `Hooked_DrawIndexedPrimitive` again, a **nested dispatch on the same thread**. This is why the mutex is recursive.
3. In the nested dispatch, `OnDrawTracked` returns `Continue` at once because `g_inOwnCall` is set, and the nested call
   reaches the real DXVK function.
4. It restores state, then returns **`Skip`** for the outer call. The game's original draw is never issued, and the outer
   chain stops at `Normal`.

What this means for other modules (inferred from the code):
- `First` hooks (PostScene, HDR, Lot Map Probe) see a replaced draw **twice**: once as the original and once as the
  re-issue.
- `Late` and `Last` hooks (HdrNative, Light Probe, Frame Capture) and the profiler's end hook see **only the re-issued
  draw**, with the mod's shader bound.
- `hdr_native.cpp` relies on this. Its header comment says a draw the bridge replaces "ends there (Skip), and its own
  re-issued draw has its shader bound, which matches none of the rules". HdrNative (`Late`) and Light Probe (`Last`) use
  the same re-issue-and-Skip pattern themselves.
- The profiler recognises a dispatch cut short by Skip or Block by its `DeviceContext` address and drops it
  (`frame_profiler.cpp` header, "D3D9").

**Rule (inferred):** a callback may register or unregister callbacks of **another** method type, but never of the type
being dispatched. The `std::vector` is being iterated, and the recursive mutex lets the same thread in.
- Example: NTR's Present hook calls `LotLightBridge` setters, which may register or unregister DIP, DP, Set*Shader and
  Create*Shader hooks. `LotLightBridge` registers no Present hook, so `UnregisterAll` leaves `g_presentHooks` unchanged.

### 3.4 Current registrations (combined tree)
| Method | -1000 | First (0) | Normal (50) | Late (75) | Last (100) | +1000 |
|---|---|---|---|---|---|---|
| Present | Profiler start (frame boundary) | PostScene, HdrOutput, DepthBlur*, AmbientOcclusion, EdgeSmoothing, LotMapProbe | | | NightTerrainRelight (Night Lighting per-frame dispatcher), FrameCapture | Profiler end |
| DIP / DP | Profiler start | PostScene, HdrOutput, LotMapProbe | **LotLightBridge (Skip)** | **HdrNative (Skip)** | LightProbe (Skip when it re-draws), FrameCapture | Profiler end |
| SetRenderTarget | Profiler (optional) | PostScene, HdrOutput, LotMapProbe | | | FrameCapture | |
| SetPixelShader | Profiler (optional) | | LotLightBridge | | HdrNative | |
| SetVertexShader | Profiler (optional) | | LotLightBridge | | | |
| CreatePixelShader | Profiler | | | | LotLightBridge (precreate), HdrNative (precreate) | |
| CreateVertexShader | Profiler | | | | LotLightBridge (precreate) | |
| BeginScene | | | | | FrameCapture | |
| CreateTexture, CreateRenderTarget | Profiler | | | | | |
| Set*ShaderConstantF, SetTexture | Profiler (optional) | | | | | |

\* `DepthBlur` registers its Present hook while the INTZ depth swap is active (`StartDepth`), even with the blur itself
off.

Not compiled (not in the vcxproj): `call_trace_patch`, `light_diag_patch`, `lot_edge_lighting_patch`. They also register
hooks.

### 3.5 Per-hook timing for the profiler
- `ExecuteDrawIndexedPrimitiveHooks` and `ExecuteDrawPrimitiveHooks` only check `FrameProfiler::RegistryHookTimingActive()`.
  This is a relaxed atomic load, true only while the profiler is on and Advanced > "Per-hook registry timing" is checked.
- When it is true, they wrap each `entry.hook(...)` in `FrameProfiler::Ticks()` and call
  `AddRegistryHookTime(entry.name, ticks)` with the registry mutex held.
- The other 13 executors are **not** timed per hook.
- The total mod D3D-hook time per draw and per Present comes from the -1000 and +1000 bracket hooks (see
  [features/frame-profiler.md](features/frame-profiler.md)).

### 3.6 The standalone registry (`framework/d3d9_hooks.{h,cpp}`, own-cost work of 2026-09-29)
Written, not compiled or tested in game yet. Analysis: `research\perf2\apexcost\report.md` (items P1, P2, P3, M1, M3).
The standalone registry was rewritten from scratch (12.0): same 15 detours, same priorities / Skip / Block, stable order
for equal priorities. What differs from 3.1-3.5:
- **Lock-free dispatch of the draw and state chains.** Each chain publishes an immutable list through an atomic pointer
  (`Chain::list`). DrawIndexedPrimitive, DrawPrimitive, SetRenderTarget, SetViewport, SetPixelShader, SetVertexShader,
  SetTexture and Set{Pixel,Vertex}ShaderConstantF are dispatched **without any lock on the render thread** (the thread
  of the first EndScene, recorded by `Install`): one relaxed count test, one acquire load, the callbacks. Before, every
  dispatch took the `std::recursive_mutex` and copied a `shared_ptr` (two lock operations and two interlocked reference
  count changes).
  - Present, BeginScene and the four Create* chains keep the recursive lock; so does any chain called from **another
    thread** (counted: `D3D9Hooks::OffThreadDispatches()`, Frame Profiler Advanced and report; the first one per method is
    logged `[D3D9Hooks] <method> called from thread N (render thread M): dispatched under the lock`). Callbacks of the
    lock-free chains are therefore no longer serialised against callbacks running on other threads; that matters only if
    that counter grows.
  - Register / UnregisterAll build the new list under the lock and publish it; **older lists are kept until a safe point**: Present on
    the render thread when it is outside every lock-free dispatch and no locked dispatch is running (the lock is only
    tried, never waited for), or `Uninstall` (skipped when its wait for the render thread timed out). A dispatch still
    reading an older list (the render thread, or a callback that registers during its own dispatch) reads valid memory.
  - `UnregisterAll` from the render thread returns at once (as before, a chain being run keeps its list until it
    returns). From **another thread** it publishes, drops the lock, calls `FlushProcessWriteBuffers` (so a later render
    dispatch sees the new list, or its "inside" flag is visible) and waits until the render thread has left the lock-free
    dispatch that may still run the removed callback (`g_renderInside` / `g_renderExits`, written only by the render
    thread with plain stores). Bounded at 1 s (logged `[D3D9Hooks] UnregisterAll("X") from thread N: the render thread
    did not leave its draw hook within 1 s; continuing`): a caller holding a lock that a draw / state callback takes
    (e.g. `PostScene::Remove` holds its effect mutex, which the trigger takes) would otherwise deadlock. Rule: do not free
    what a draw / state callback uses from another thread without unregistering first, and prefer the render thread.
- **`CallOriginal*`** now also exists for SetPixelShader, SetVertexShader, SetTexture and SetPixelShaderConstantF. Night
  Lighting's draw handlers use them for their own state changes around a replaced draw ([night-lighting
  README](features/night-lighting/README.md) "Own cost"); the re-issued draw itself still goes through the device. A
  `CallOriginal*` call runs the trampoline: a module that detoured the same DXVK function **before** Apex (inner) still
  sees it, one that detoured it **after** Apex (outer) does not. Official S3SS's registry registers nothing on these
  chains (12.2), so either order is harmless; a proxy `d3d9.dll` (ReShade) sees every call, being below the detours.
- **Profiler instrumentation (development build only, `if constexpr (!kPublicBuild)`):**
  - the detours of SetTexture, Set{Vertex,Pixel}Shader, Set{Vertex,Pixel}ShaderConstantF and SetRenderTarget count their
    calls with plain per-method counters (`ReadStateCallCounts`), replacing six counting callbacks that made every state
    call of the game run a full dispatch (P1);
  - `Run` books its **outermost** dispatch on a thread (all chains but Present) as the profiler's "D3D hooks (mod)"
    (`FrameProfiler::BeginModTime` / `EndModTime`, keyed by the dispatch's stack address), also when a callback returns
    Skip / Block (M1). A nested dispatch (a replaced draw's re-issue) is part of the outer one;
  - every Present callback is timed by name while the profiler is on and reported as `"<name> (Present)"` (M3); the draw
    callbacks still only with Advanced > "Per-hook registry timing".
- 3.4's table for the standalone: Present -2000 ApexCore, -1000 / +1000 Frame Profiler (dev), First PostScene / Picture
  / Edge Smoothing / Depth Blur (while its depth swap is active), Normal LotLightingMotion (camera sample, while on), Last
  NightTerrainRelight / FrameCapture (dev); DIP / DP: -1000 Frame Profiler counting hook (dev; no +1000 hook since
  2026-09-29), First PostScene, 10 Picture, Normal LotLightBridge (Skip), Last Light Probe (Skip when it re-draws) /
  FrameCapture (dev); SetRenderTarget:
  PostScene, Picture, FrameCapture (dev); Set{Pixel,Vertex}Shader: LotLightBridge; BeginScene: FrameCapture (dev);
  Create*: Frame Profiler counters (dev). SetTexture, Set*ShaderConstantF and SetViewport have no callback in either build,
  so their dispatch ends at the count test.

---

## 4. Other hook plumbing

### 4.1 `d3d9_extra_hooks.*` (`ExtraHooks`, Apex-owned)
- Detours for methods the registry does not cover:

  | Method | Vtable index |
  |---|---|
  | StretchRect | 34 |
  | SetDepthStencilSurface | 39 |
  | GetDepthStencilSurface | 40 |
  | Clear | 43 |
  | DrawPrimitiveUP | 83 |
  | DrawIndexedPrimitiveUP | 84 |
- Installed **once, on first use**, by `EnsureInstalled(dev)` (Depth Blur `SetupResources`, Frame Capture `EnsureDetours`).
  They stay for the life of the process.
- **Guard:** if any of those slots shares a code address with a slot already owned by `d3d9_hook.cpp` or the registry
  (`16, 17, 23, 28, 37, 41, 42, 47, 65, 81, 82, 91, 92, 94, 106, 107, 109`), it logs an error and does not install. This
  protects against DXVK folding identical functions together.
- **Observer slots**, one each, atomic function pointers, owned by Frame Capture: Clear, SetDepthStencil, StretchRect and
  DrawUP.
- **Depth substitution** (single owner, Depth Blur), set with `SetDepthSubstitution(substitute, report)`:
  - `substitute` maps the surface the game binds to the surface actually bound;
  - `report` maps back for `GetDepthStencilSurface`, so the game never sees the swap.
- `RawSet/GetDepthStencilSurface` bypass the substitution. Before `EnsureInstalled` they fall back to the device methods.
- Post-scene effects draw with **DrawPrimitiveUP**. The registry does not hook it, so the effects never re-trigger the
  post-scene trigger. The DrawUP observer only reports these draws.

### 4.2 `render_callbacks.h` (Apex-owned)
- Three lists of `std::atomic<DeviceFn>`: `endSceneBeforeOverlay`, `preReset` and `postReset`.
- **Each list has only `kSlots = 4` entries.**
- `Add` is idempotent. It CAS-es into the first empty slot and **silently drops the callback when all four are full**.
  `Fire` calls every non-null slot.
- Users in the combined tree:
  - `preReset`: AO, Depth Blur, Edge Smoothing, Lot Map Probe, Night Lighting (`LightmapSmooth::OnPreReset`). That is
    **five possible users for four slots**. With all five enabled (Lot Map Probe is dev-only), the last one to register
    loses its preReset call (inferred from the code; not seen at runtime). The standalone drops AO, which leaves four.
  - `postReset`: AO, Depth Blur, Edge Smoothing.
  - `endSceneBeforeOverlay`: Frame Capture, Night Lighting `DeferredReinstall`.
- If a new module needs a slot, raise `kSlots` first.

### 4.3 `vtable_manager.*` and `pattern_scan.*` (S3SS framework, used only by the settings hooks)
- `VTableManager::Initialize` finds the constructor pattern `56 8B F1 C7 06 ?? ?? ?? ?? 33 C9 89 4E 0C` and reads the
  vtable immediate at +5.
  - It validates the first 8 entries: they must be executable and inside the main module.
  - `GetFunctionAddress(name, offset)` also checks, for offset 0x3C, that the function references
    `"Debug/VariableRegistry/Variable"` (or `"Debug/VarMan"`) near its prologue.
- `Pattern::Scan` / `ScanModule` is a byte-wildcard scanner (`??`) over the main module. Apex code uses
  `PatchHelper::ScanPattern` instead (next section).

### 4.4 `patch_helpers.h` (S3SS framework, used by every game-code patch)
- **Memory writes**, all through `WriteProtectedMemory` (VirtualProtect, optional original-byte tracking, read-back check):

  | Helper | What it does |
  |---|---|
  | `WriteByte` / `WriteBytes` / `WriteWORD` / `WriteDWORD` | Write; each takes an optional `expectedOld` checked before writing |
  | `WriteNOP` | Fills with 0x90 |
  | `RestoreAll(tracker)` | Restores every tracked location |
  | `BeginTransaction` / `CommitTransaction` / `RollbackTransaction` | All-or-nothing groups |
- **Call and jump stubs:**
  - `CalculateRelativeOffset(from, to, size = 5)`;
  - `WriteRelativeJump`, which writes `E9 rel32`;
  - `WriteRelativeCall`, which writes `E8 rel32`.

  Apex patches that redirect a `CALL` use these, or build the bytes themselves. `night_terrain_relight_patch.cpp` `CallPatch`
  builds prefix bytes + `E8` rel32 + NOP padding up to the site length, then calls `FlushInstructionCache`.
- **`ScanPattern(start, size, "8B ?? ?F F? ..")`** supports nibble wildcards: `??` any byte, `?F` low nibble, `F?` high
  nibble.
- **`AddressInfo`** is `{name, addresses{GameVersion, addr}, pattern, patternOffset, expectedBytes}`. `Resolve()` works
  like this:
  - known version with an address: use it, or fail if `expectedBytes` does not match;
  - otherwise: pattern scan, then validate.
  - Note: after a pattern hit it logs "Pattern matched on unknown version" **even on a known version that has no address
    entry**. The 28/09 log shows this for LotStreamingOptimizations on Steam. The wording is misleading, not an error.
- **`DetourHelper::InstallHooks` / `RemoveHooks`**: a vector of `{originalPtr, hookFunc}` in one Detours transaction.
- **Also present:** `IATHookHelper::Hook`, `LiveSetting::*` (S3SS game-variable access by name), and `D3D9Helper::*`
  (back-buffer size, present params, caps, format names, shader bytecode dump). `SAFE_IMGUI_BEGIN()` returns if there is
  no ImGui context.
- `OptimizationPatch` (`optimization.h`) also provides `AddMaintainedWrite` / `ClearMaintainedWrites` /
  `OnSettingsRefired`. These re-assert data writes after the game re-runs its variable registration. The
  VariableRegistry hook calls `OptimizationManager::OnSettingsRefired()` when it sees `"MT Time Step"`.

### 4.5 The post-scene trigger chain (`post_scene.*`) and the INTZ depth share (`depth_share.h`)
This is one shared trigger for effects that work on the finished 3D scene, before any game UI. Details are in
[features/depth-blur.md](features/depth-blur.md), [features/edge-smoothing.md](features/edge-smoothing.md) and
[features/ambient-occlusion.md](features/ambient-occlusion.md) (the chain is Ambient Occlusion 10, Edge Smoothing 20, Depth Blur 30).

- `PostScene::Add(order, fn)` sorts the effects with `stable_sort` on `order`: `kAmbientOcclusion = 10`, `kEdgeSmoothing = 20`,
  `kDepthBlur = 30`. `PostScene::WantCamera` (reference counted, AO) turns on the camera votes: near, A and the
  view-projection from the vertex constants of the first 24 scene draws of each frame (`CameraNear`, `CameraDepthA`,
  `CameraViewProj`).
  - The first `Add` registers the hooks under `"PostScene"`: Present, SetRenderTarget, DIP and DP, all at `First`.
  - `Remove` of the last effect unregisters them.
- **Trigger:**
  1. Count back-buffer draws (RT0 == back buffer) with `ZENABLE != FALSE`.
  2. Normally, the first back-buffer draw with `ZENABLE == FALSE`, after at least `kMinSceneDraws = 20` depth-tested
     draws, fires every effect **once** (`g_done`) inside that draw's DIP/DP callback, before the game's draw executes.
  3. If no qualifying depth-off draw happened (for example, when the game UI is hidden), `endSceneBeforeOverlay` checks
     that at least 20 scene draws occurred and RT0 is still the back buffer, then runs the same ordered chain once before
     Picture's scene copy and Apex's overlay.
  4. Draws marked with `DepthShare::SetInternalPass(true)` are ignored. The lake-lamp pass in `lot_light_bridge.cpp`
     turns Z off and must not look like the UI.
  5. The Present hook resets the counters at the frame boundary. The fallback does not change the existing behavior in
     interiors where a depth-off backbuffer draw occurs mid-scene; validate those scenes separately.
- **Camera for the effects (combined build only):** `CameraNear()`, `CameraViewProj()` and `CameraDepthA()` (near vote
  over VS blocks `c0`, `c4`, `c40`, `c180`, `c192`, `c216`; view-projection `c40..c43`) existed for Ambient Occlusion.
  The standalone's `post_scene.cpp` is the v0.1.0 one and has none of them; Depth Blur's Auto focus uses depth ratios
  (A = 1.00008 as a constant, the near plane cancels). Recipe: [engine/camera-and-map-view.md](engine/camera-and-map-view.md).
- **INTZ swap (`DepthShare`, implemented in `patches/depth_blur_patch.cpp`):**
  - The game's auto depth-stencil (D24S8 or D24X8, same size as the back buffer, not multisampled) is swapped for an INTZ
    texture with `D3DUSAGE_DEPTHSTENCIL`, through the ExtraHooks substitution.
  - `DepthShare::Texture()` and `Surface()` are null unless the swap is ready.
  - `Request(bool)` is reference-counted. The swap runs while Depth Blur is on **or** `requests > 0` (`UpdateDepth`).
  - Requesters in the combined tree: Ambient Occlusion (`Install`) and HDR sky boost (`HdrOutput::OnEndScene`, when
    `skyBoost > 0.001`).
  - Edge Smoothing does **not** use or request depth: `edge_smoothing_patch.cpp` has no `DepthShare` reference.
  - `lot_light_bridge.cpp`'s pond reflection reads `DepthShare::Texture()` when it happens to exist, and does not request
    it. This is why the Reflections tooltip says "with Depth Blur on, also the scenery on the shore".
  - A multisampled back buffer means the game's Edge Smoothing (MSAA) is on. The swap then refuses, with the status "turn
    it off in Options > Graphics".

### 4.6 Shader precompile (`framework/shader_cache.*`, standalone, 2026-09-28)
<a id="shader-precompile"></a>
Apex's own HLSL pixel shaders are never compiled on the render thread. Written 2026-09-28, not compiled or tested in
game yet (anti-stutter plan `research\perf2\plan.md`, items 7 and C5: `d3dcompiler_47.dll` was 8.5% of the samples of
"Render frame" hitches, one-time hitches of 10-100+ ms at the first lamp / roof / water / SMAA / Depth Blur frame).
- **Registration:** each feature adds every variant it can use (all qualities and modes) with `ShaderCache::Add`, from
  namespace-scope initialisers in its own .cpp (so the list is complete before the init thread runs). A `Desc` keeps the
  exact `D3DCompile` inputs the feature used before (source, source name, entry, target, flags, macros), so the bytecode
  is the same; `priority` 0 = the feature's default quality / mode, compiled first.
- **Compile:** `ShaderCache::Start()` (init thread, right after the log opens, before the device exists) starts one
  worker thread (below-normal priority) that runs `D3DCompile` for every variant and keeps the bytecode for the session
  (the source string is freed after its compile). Log: `[ShaderCache] Precompiling N Apex shaders on a background
  thread`, then `[ShaderCache] Precompiled N Apex shaders in X ms on a background thread (F failed; slowest: ...)`; a
  failure logs `[ShaderCache] <tag> did not compile: <message>` (the feature logs its own error text at first use too).
- **Use:** where a feature called `D3DCompile` + `CreatePixelShader`, it now calls `ShaderCache::CreatePixelShader(dev,
  id, &ps, &msg)`: the render thread only creates the D3D9 object from the bytecode (at the feature's first use, as
  before). If the worker has not reached that variant yet, it becomes the worker's next job and the caller waits
  (logged as a warning `waited for the precompile`, counted; the worker is raised to normal priority); with no worker
  left (thread creation failed, or FreeLibrary) the variant is compiled on the calling thread as a last resort (logged).
- **Device Reset / release:** D3D9 shader objects survive `Reset`; the features that release them (Uninstall, Shutdown,
  `ReleaseShaders`) recreate them from the kept bytecode, never compiling again.
- **Status:** `ShaderCache::StatusText()`: Developer > Profiler tab (under the Frame Profiler card) and the profiler
  report (`Apex shaders: ...`).
- **Registered variants (36):** Night Lighting (`lot_light_bridge.cpp`, entry `main`, flags 0 as before): lot light pass
  (ps_3_0), object rig moon-shadow fix (ps_2_0), roofs, lake water, snowy roofs (ps_3_0); world light smoothing
  (`lightmap_smooth.cpp`): GatherPS, HBlurPS, VBlurPS, HUpYAPS, HUpChromaPS, VUpPS, DownPS, CopyPS; Depth Blur: FocusPS,
  PrepPS, CompositePS, BlurPS x 4 qualities (TAPS 4 / 6 / 8 / 12); Edge Smoothing: FXAA x 3 qualities, SMAA x 4 presets
  x 3 passes (edge detection, blending weights, neighbourhood blending); Picture: PicturePS. All with
  `D3DCOMPILE_OPTIMIZATION_LEVEL3` except the Night Lighting ones (flags 0).
- **Not affected:** the game-shader copies patched in bytecode (`shader_patches.cpp`: roads, floors, snow, fences,
  foliage and object vertex shaders, ...) are created with `CreatePixelShader` / `CreateVertexShader` at their first
  draw from the game's own bytecode (no HLSL, no `D3DCompile`); they depend on which game shader is drawn, so they stay
  lazy. `light_probe.cpp` uses `D3DDisassemble` (developer tool, on demand).

### 4.7 Layered vtable-slot hooks and suspended code writes (standalone, 2026-09-29)
- `framework/slot_chain.{h,cpp}` (`SlotChain`): several Apex modules may wrap one game function that is reached only
  through vtable slots. Each wrapper is a layer with a fixed position (lower = outer; since round 3: 0 = Gate, the wall
  shading gate, which must be outermost; 1 = Frame Profiler; 2 = Resource cache; 3 = FastCompress);
  the slots hold the outermost installed layer's hook and every hook calls `SlotChain::Next(site, layer)`. Install /
  Remove swap the slots (interlocked compare-exchange, expected value checked) or re-point the outer layer's next
  pointer; a removed hook keeps its next pointer. Sites: FindProvider (profiler + cache) and the resource manager's
  RegisterDatabase (base and derived), SetDatabasePriority and DatabaseChanged (cache only); RefPackCompress (the RefPack
  stream write slot 0x00FB901C: profiler + the fast compressor); round 3: WallAoStep (the wall AO step slot 0x00FF05B0:
  gate + profiler), KeyListBase / KeyListDerived (GetKeyList slots 0x00FB2DC0 / 0x00FFE270: profiler + the file list
  cache). The write epochs of "Remember missing files" swap their database-class slots directly (only Apex module on
  them; `InstallClassHooks` in features/resource_cache.cpp, same compare-exchange). See
  [features/performance.md](features/performance.md).
- `framework/entry_chain.{h,cpp}` (`EntryChain`, 2026-09-29): the same layering for functions reached by direct CALLs from
  several threads. The entry's relocation-free prologue is copied to a trampoline (+ JMP back) and a 5-byte JMP to the
  outermost layer's hook is written with `MemPatch::WriteCodeSuspended`; each hook calls `EntryChain::Next(site, layer)`
  (the next inner layer or the trampoline); removing the last layer writes the original bytes back. Sites: the CPU DXT1 /
  DXT5 encoders 0x006152F0 / 0x006154B0 (layer 0 Frame Profiler, layer 1 the fast DXT encoder); round 3: the DPF's
  direct record write 0x004A7FC0 (layer 2, the resource cache's write epochs; prologue 83 EC 28 53 56); the object
  lookup by ID 0x00C62D40 (8-byte prologue; layer 0 Frame Profiler, layer 3 `ObjectIndex` = Faster Object Lookups);
  the scene node destructor 0x006FD930, the scene AddNode 0x006E6480 and the holder teardown 0x006E4DE0 (layer 4
  `SceneBudget`: the node lifetime guard of Spread New Objects Over Frames). Layers: 0 FrameProfiler, 1 FastDxt,
  2 ResourceCache, 3 ObjectIndex, 4 SceneBudget (each module layer is used on its own sites only).
- `framework/call_chain.{h,cpp}` (`CallChain`, 2026-09-29): the same layering on one CALL instruction (E8 rel32): the CALL
  targets the outermost layer's hook, written with `MemPatch::WriteCodeSuspended`; each hook calls
  `CallChain::Next(site, layer)` (the next inner layer or the original callee); removing the last layer writes the
  original CALL back. Site: Scene::BeginFrame's CALL 0x006EBC49 of the pending-node drain 0x006E4130 (layer 0 Frame
  Profiler "Scene pending nodes", layer 1 `SceneBudget` = Spread New Objects Over Frames).
- `MemPatch::WriteCodeSuspended(address, bytes, n)`: writes up to 16 code bytes with every other thread suspended and none
  stopped inside them (retried for ~100 ms), for CALL rewrites that several threads may run (Lot Lighting While Moving's
  CALL at 0x00ADB95D). The Frame Profiler keeps its own copy (`WriteCallSuspended`).

---

## 5. Patch system and settings

### 5.1 Classes and registration (`patch_system.h`, `optimization.*`)
- `OptimizationPatch` is the base class of every patch. Its members:

  | Member | Purpose |
  |---|---|
  | `Install()` / `Uninstall()` | Pure virtual |
  | `Update()` | Hook-thread tick; debounced reinstall by default |
  | `RenderCustomUI()` | Default: draws the registered settings |
  | `SaveToToml` / `LoadFromToml` | Serialisation |
  | `OnSettingsRefired()` | Re-asserts maintained writes |
  | `isEnabled` (atomic), `lastError`, `Fail(msg)` | `Fail` sets the error, logs it and returns false |
  | `patchName` | The TOML table name and the `IsApexPatch` key |
- **Registration macros:**
  - `APEX_REGISTER_FEATURE(Class, FeatureInfo{...})` creates a static `PatchRegistrar`, which calls `PatchRegistry::Register`
    (a factory lambda plus the metadata) during static init;
  - `REGISTER_CUSTOM_PATCH(Name, Class, ...)` is the same, with a different registrar name.
  - `PatchRegistry::InstantiateAll` (called from the first `OptimizationManager::Get()`) constructs each patch, attaches
    the metadata and calls `RegisterPatch`, which rejects duplicate names.
- **`FeatureInfo`**: `displayName`, `description` (the combined build put "Part of Sims3 Settings Setter Apex Edition.
  Credits: @loinyx" at its end; the standalone writes "Part of " `APEX_PRODUCT_NAME` = "Apex Radiance"), `category`, `experimental`, `enabledByDefault`, `supportedVersions` (`VERSION_STEAM`,
  `VERSION_EA`, `VERSION_RETAIL`, `VERSION_ALL`), `technicalDetails`, `gameCodeGroup` (standalone).
  - `IsCompatibleWithCurrentVersion()` checks `supportedVersions` against `g_gameVersion`. An unknown version means
    incompatible, unless the feature names a `gameCodeGroup` ("NightLights", "SplitLevel") whose addresses the
    signature scan found on this build (`framework/game_addresses.h`, [engine/game-versions.md](engine/game-versions.md));
    `UnavailableReason()` gives "Not available on <version>: missing <addresses>".
- `patches/_TEMPLATE.cpp` is **not compiled**. It uses `.targetVersion`, which no longer exists: use
  `.supportedVersions`. The `SaveState` / `LoadState` INI examples in `patches/README.md` are also stale, because
  persistence is TOML now.

### 5.2 Config files and TOML layout (`config/`)
- **`config_paths`:**
  - `GetS3SSDirectory()` returns `Documents\Electronic Arts\<localized game folder>\S3SS\`.
  - The folder name comes from the exe's string table (`ResolveLocalizedGameFolder`). Blocks of 100 from id 1000 to 3599:
    base+0 is the locale code, base+2 the folder name. It matches the user locale, then the language prefix, and falls
    back to `"The Sims 3"`.
  - Files: `S3SS.toml`, `S3SS_defaults.toml`, `S3SS_LOG.txt`.
  - `AtomicWriteToml` writes `<file>.tmp`, then `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`.
  - `GetLegacyINIPath` is `Game\Bin\S3SS.ini`.
- **`config_store`** (`ConfigStore`, one mutex):
  - `SaveAll` builds the **whole file from scratch**: `[meta] version = 1`, `SettingsManager` (`[settings.'<game
    variable>'] value`), `ConfigValueManager` (`[config]`), `OptimizationManager` (`[patches.<Name>]`), then one `[qol]`
    table filled by `UISettings` (`[qol.ui]`), `MemoryMonitor` (`[qol.memory_monitor]`), `BorderlessWindow`
    (`[qol.borderless_window]`), `HdrOutput` (`[qol.hdr]`, `[qol.picture]`) and `FrameProfiler` (`[qol.frame_profiler]`).
  - `LoadAll` distributes everything except patches. `LoadPatches` loads `[patches]` only (step 9).
  - `SaveDefaults` / `LoadDefaults` handle `S3SS_defaults.toml`.
- **`config_value_manager`:** GraphicsRules config values (`[config]`), with stable `wchar_t` buffers the game reads.
- **`migration`:** INI to TOML, once, with a popup (`Migration::RenderMigrationPopup`).
- **Patch table layout:** `[patches.<patchName>]` holds `enabled = bool` plus one key per registered setting, named by the
  setting's `name` argument.
  - Some Night Lighting keys are Portuguese identifiers (`luzDoPosteNaGramaDoLote`, `postesNosObjetos`,
    `cercasComLuzDoChao` and more).
  - Depth Blur keys are also Portuguese: `distancia`, `transicao`, `forca`, `tamanho` (legacy, unused), `qualidade`,
    and since 2026-09-28 `focoAuto`, `quantidade`, `areaNitida`, `velocidadeFoco`, `realceLuzes`.
  - **Never rename a key.** Saved configs use them (comment in the `NightTerrainRelightPatch` constructor).
- **When things are saved:**
  - Patch enable state and patch settings are saved **only by File > Save Settings**. The window title shows "Unsaved
    Changes".
  - `UISettings`, `BorderlessWindow`, `MemoryMonitor`, `HdrOutput::SetParams` / `SetPicture` and the Frame Profiler call
    `ConfigStore::SaveAll()` themselves on change.
- **The menu's "Load Settings" item** calls `LoadAll` only, so it does not reload `[patches]` (observed in `gui.cpp`).
- **Early readers** parse the TOML before `LoadAll`: `PeekDisableOverlayEarly`, `BorderlessWindow::PeekEnabledEarly`,
  and `HdrOutput::BeforeCreateDevice` when `!m_loaded`.

### 5.3 How a patch declares and uses settings
- **Declare in the constructor.** `RegisterFloatSetting(ptr, key, SettingWidget, default, min, max, desc, presets)`,
  `RegisterIntSetting`, `RegisterBoolSetting` and `RegisterEnumSetting` (`patch_settings.h`: `FloatSetting`,
  `IntSetting`, `BoolSetting`, `EnumSetting`).
  - The setting writes its default into `*ptr` at construction.
  - `LoadFromToml` clamps to min and max, and writes to a bound address if `BindSettingToAddress` was used.
  - Each setting's changed-callback calls `NotifySettingChanged()`.
- **Load order in `OptimizationPatch::LoadFromToml`:** settings first, then `enabled`. So `Install()` sees the loaded
  values.
- **Reinstall policy.** The base `Update()` does Uninstall + Install on the **hook thread** 2 s after a change. Apex
  patches avoid this:
  - **Read live:** AO, Depth Blur, Edge Smoothing, Frame Budget and GC Scheduler override
    `Update() { pendingReinstall = false; }`. Their settings are read every frame or call.
  - **Reinstall only when code bytes must change:** `SmoothStreamingPatch::Update` calls the base only when a part is
    switched on or off (`PartsMatchInstalled`).
  - **Reinstall on the render thread:** `NightTerrainRelightPatch::Update` does not reinstall itself. It sets
    `g_reinstallDue`, and `DeferredReinstall` (`endSceneBeforeOverlay`) runs `ReinstallNow()` on the render thread,
    because Uninstall frees textures and shaders that the draw hooks use. Options that can change live are applied
    immediately by `ApplyLive`.
- Apex feature UIs call `ImGui` widgets on their own globals, then `NotifySettingChanged()` when something changed.

### 5.4 Menu organisation (`gui.cpp`, `SettingsGui::RenderUI`)
The window is "Sims3 Settings Setter Apex Edition (v1.6.3)" (`version.h`), with window ID `###S3SSWindow`. Tabs, in order:
**Settings, Patches, Apex, Display, Config Values, Other/QoL, Debug**. The user asked for Apex and Display right after
Patches.

- **`IsApexPatch(name)`** lists `NightTerrainRelight`, `AmbientOcclusion`, `EdgeSmoothing`, `DepthBlur`, `FrameCapture`,
  `LotMapProbe`, `SmoothStreaming`, `GcScheduler` and `FrameBudget`. The Patches tab skips these. **A new Apex patch must
  be added to this list**, or it also shows in Patches.
- **`RenderApexFeature(patchName, title, open)`** draws, for each feature:
  - a collapsing header with the metadata description as a hover tooltip;
  - an "Enabled" checkbox that calls `Install()` / `Uninstall()` directly (on the render thread, because this is inside
    EndScene) and marks unsaved;
  - a disabled state with "(not supported on <version>)" when the game version is incompatible;
  - the error in red;
  - `RenderCustomUI()` while enabled.
- **Apex tab:**
  - Night Lighting.
  - Reflections (`ApexRenderReflectionsUI`, declared in `apex_ui.h` and implemented in
    `night_terrain_relight_patch.cpp`).
  - AO and Depth Blur, followed by a note about the game's Edge Smoothing.
  - "Performance": Smooth Streaming; Script GC Scheduler and Service Frame Budget (collapsed); Frame Profiler
    (`FrameProfiler::RenderUI`).
  - In non-public builds only: a "Developer tools" header with Frame Capture and Lot Map Probe.
- **Display tab:** HDR (`HdrOutput::RenderUI`), Picture (`RenderPictureUI`), Edge Smoothing, Borderless Window (moved here
  from QoL).
- **Inside each feature** (convention from the user; see memory "Apex Edition UI conventions"):
  - status line and main controls visible;
  - `ImGui::TreeNode("Advanced##<Feature>")`, collapsed, for tuning;
  - `if constexpr (!kPublicBuild) ImGui::TreeNode("Developer##<Feature>")` for diagnostics;
  - English text only; a `Hint()` tooltip on every control;
  - "Credits: @loinyx" only at the end of the metadata description.

### 5.5 `qol.*` (S3SS framework)
| Class | TOML table | Contents |
|---|---|---|
| `UISettings` | `[qol.ui]` | `toggle_key` (default `VK_INSERT`), `disable_hooks`, `disable_overlay`, `font_scale` |
| `BorderlessWindow` | `[qol.borderless_window]` | `mode`: Disabled, Decorations Only, Maximized, Fullscreen. `Apply`, `TickReapply` and `SetWindowHandle` are driven from the D3D hooks |
| `MemoryMonitor` | `[qol.memory_monitor]` | `enabled`, `warning_threshold`, `warning_style`. `Update()` runs from the hook thread's loop |

---

## 6. Logger (`logger.cpp` / `logger.h`)
- **File:** `Documents\Electronic Arts\<localized>\S3SS\S3SS_LOG.txt`, truncated at each start. The fallback is
  `Game\Bin\S3SS_LOG.txt`. The header is "S3SS Log - Started at <date time>".
- **Levels:** `Debug < Info < Warning < Error < Critical`.
  - **Debug never reaches the file**, in any build. It goes only to `OutputDebugStringA`: use DebugView.
  - `LOG_DEBUG` is compiled in Release too. `SetDebugMode` exists but `Log` does not use it.
  - Debug, Error and Critical lines carry `(file:line)`.
  - Every line also goes to `OutputDebugStringA`.
- **Buffering** (Apex rewrite):
  - `Log` appends to `g_pending` under the mutex.
  - A Win32 flush thread (`FlushThread`) writes and flushes at most every `kFlushIntervalMs = 1000`.
  - Warning and above flush at once, together with everything before them, so a crash report still follows its lead-in
    lines.
  - Above `kMaxPendingBytes = 64 KB`, the logging thread writes itself.
  - Without a flush thread, every line is written at once.
  - `Close()` signals the thread without waiting (DllMain deadlock) and writes what is left. `ExitFlush`'s static
    destructor writes at process exit with `try_lock`.
- **Rule for render-thread code:** do not log per draw or per frame. `lot_light_bridge.cpp` counts notes per kind and
  writes one line at most about once a second.
- **Other Apex output files** also go to `GetS3SSDirectory()`: `S3SS_Hitches.txt` (profiler writer thread),
  `S3SS_FrameCapture.txt`, `S3SS_LightDiag.txt`, `S3SS_LightProbe.txt` plus `LightProbe*\` folders, `Censo\`
  (S3SS_Censo.txt), and `HDR_Diag_*`.

---

## 7. Build flavours
- **`build_flavor.h`:**
  - `S3SS_PUBLIC` is defined only for the public build. `inline constexpr bool kPublicBuild` follows it.
  - `S3SS_TR(pt, en)` now expands to `en`. The code is English-only since 28/09; the macro stays so old call sites still
    compile.
- **Dev-only code** is guarded by `if constexpr (!kPublicBuild)`: Light Probe / Light Diag calls in NTR's Present hook,
  "Developer" tree nodes, the "Developer tools" header, census and false colour (`lot_light_bridge.cpp` `OnDraw`).
  - The files are still compiled in both flavours. Frame Capture and Lot Map Probe stay registered as patches in the
    public build, but they are not reachable in its UI (inferred from `gui.cpp`).
- **`Sims3SettingsSetter.vcxproj`:**
  - MSBuild property `S3SSPublic=true` sets `S3SSFlavorDefines=S3SS_PUBLIC` (added to the Release
    `PreprocessorDefinitions`), `OutDir=$(ProjectDir)Public\` and `IntDir=$(ProjectDir)Public\obj\`.
  - Outputs: **`Release\S3SSApex.asi`** (dev, the default) and **`Public\S3SSApex.asi`** (public). Both folders also hold
    an old `Sims3SettingsSetter.asi`.
- **Toolchain:** v143, Win32, C++20, static CRT (`MultiThreaded`), vcpkg triplet `x86-windows-static`. Libs: `imgui.lib`,
  `detours.lib`, `d3d9.lib`, `dxguid.lib`, `psapi.lib`, plus `d3dcompiler.lib` via `#pragma`. Always build with
  `/p:VcpkgEnableManifest=false`. Commands are in [workflow.md](workflow.md).
- **Files on disk but not in the project:** `patches/_TEMPLATE.cpp`, `call_trace_patch.cpp`, `light_diag_patch.cpp`,
  `lot_edge_lighting_patch.cpp`, `split_level_lighting_fix_patch.cpp`. **Do not add or commit** `call_trace`,
  `light_diag_patch`, `lot_edge_lighting` or `trace_targets.h` (dev leftovers).

---

## 8. Per-frame flow (combined build)

### 8.1 One frame on the render thread
Sources: `engine_map\f_ECA960.asm`, the `frame_profiler.cpp` header comment, `profiler_targets.tsv`, and the code
above. Game addresses are Steam.

```
Game main loop FUN_00ECA960 (render thread = main thread), one iteration:
  0x00EC6C30  app state update
  0x00588E00  ServiceManager::Update -> 0x0059ED20: each service's vfunc+0x1C (indirect call at 0x0059ED57)
     |- Input service 0x00598660 -> message pump 0x00410890 -> HookedWndProc -> ImGui input      [mod: WndProc]
     |- JobManager 0x00599A10, ResourceSystem 0x007377F0, CAS SimService 0x005F0E50,
     |  TextureCompositor 0x00608630 (5 ms slices each)                         [mod: Service Frame Budget]
     |- WorldManager service 0x00C7E3C0 -> WorldManager::Update 0x00C6D570
     |     -> terrain update 0x00C845C0 (called at 0x00C6D68F)                [mod: Smooth Streaming, NTR state]
     |     -> lot renderer pass 0x00C7CEA0 -> 0x00AEB2E0 -> lot load stages 0x00AEA680   [mod: Smooth Streaming]
     |        (room lighting 0x006A80E0 -> solve 0x006A3EC0 inside lot lighting)  [mod: level_light_share etc.]
  0x009DE140  SceneCaptureManager
  0x006EBB70  Scene::BeginFrame
  0x00EC9F00  render frame (only if [0x011D1530] != 0):
     0x00611620 BeginFrame -> device vtable+0xA4 BeginScene
         -> registry BeginScene hooks (Frame Capture, Last)
     scene draws: every DIP/DP -> registry chain
         -1000 profiler start
         First: PostScene.OnGameDraw (count depth-tested back-buffer draws; camera near/VP vote)
                HdrOutput.OnGameDraw (count; scene copy at depth-tested -> depth-off transitions)
                LotMapProbe
         Normal: LotLightBridge.OnDraw -> may bind a patched shader, re-issue the draw (nested chain) and return Skip
         Late:  HdrNative (lamp gain / tone rules) -> may re-issue + Skip
         Last:  LightProbe (dev), FrameCapture (dev)
         +1000 profiler end -> DXVK DrawIndexedPrimitive
     first back-buffer draw with ZENABLE=FALSE after >= 20 scene draws (bloom composite, then UI):
         PostScene fires inside that callback, before the draw: AO (10) -> Edge Smoothing (20) -> Depth Blur (30)
         (each draws with DrawPrimitiveUP and restores the state it touched)
     game UI draws (Z off)                                                (counted as UI by HdrOutput)
     0x00611760 -> 0x00611680 end frame:
         device vtable+0xA8 EndScene -> HookedEndScene:
             [first call: ImGui + WndProc + registry detours]
             BorderlessWindow::TickReapply
             RenderCallbacks endSceneBeforeOverlay: FrameCapture.OnEndScene, NTR DeferredReinstall
             HdrOutput::BeforeOverlay (scene copy if the frame ended on the scene)
             ImGui NewFrame -> SettingsGui::Render (Apex/Display tabs; Install/Uninstall from checkboxes) -> RenderDrawData
             HdrOutput::OnEndScene: lamp gain, HdrNative::Update, HDR (scRGB) and/or Picture full-screen pass
             original EndScene
         device vtable+0x44 Present -> registry Present chain:
             -1000 profiler start = frame boundary (attach pending timed functions, sampler bookkeeping)
             First: PostScene / HdrOutput / DepthBlur / AO / EdgeSmoothing / LotMapProbe frame-boundary resets
             Last:  NightTerrainRelight: LightDiag (dev, F8), OnPresent (world change, dusk, reconcile/relight),
                    LightProbe (dev, F7), ObjectLightBridge, LevelLightShare, LotLightBridge setters + OnPresent,
                    LightmapSmooth::OnPresent
                    FrameCapture (dev, F9)
             +1000 profiler end
             DXVK Present (GPU back-pressure, vsync)
     +0xBA: frame limiter (Smooth Patch's call at 0x00EC9FBA, or the game's ~30 ms sleep when inactive)
  0x006E8810  Scene::EndFrame (waits for the scene jobs)
  0x005943F0  game clock tick
```

The frame profiler's category split follows the same boundaries:
- 0x611680 counts as "EndScene + overlays" until the Present boundary and as "Present (driver)" after it.
- The rest of 0xEC9F00 after that is "Frame limiter".
- Details: [features/frame-profiler.md](features/frame-profiler.md).

### 8.2 The same frame in the standalone (planned)
HDR output, Native HDR and Ambient Occlusion are **removed** (see [removed-features.md](removed-features.md)).

| Area | Combined build | Standalone |
|---|---|---|
| Back buffer | `A16B16G16R16F` when HDR is on | Never changed. No `UnlockAdditionalFormats`, no scRGB colour space |
| CreateDevice / Reset | HDR edits `pp`, restores it and retries | No `D3DPRESENT_PARAMETERS` edits. Borderless stays in official S3SS |
| End-of-frame pass | `HdrOutput::OnEndScene` (HDR + Picture) | No HDR pass anywhere, at EndScene or Present. **Picture** is its own SDR-only module, config `[qol.picture]`. It still needs a scene/UI split (the scene copy), so the game UI and menus keep their colours. The exact hook point is up to the standalone code; PLANO 2d.2 says SDR Picture is correct at Apex's EndScene in either hook order |
| Registry draw hooks | HdrOutput `First`, HdrNative `Late` | Neither exists. LotLightBridge `Normal` and the dev tools remain |
| Post-scene chain | AO 10, Edge 20, Depth Blur 30 | **Edge Smoothing (20), then Depth Blur (30)** |
| INTZ depth swap | Depth Blur, AO, HDR sky boost | **Only Depth Blur** turns it on (owner). Edge Smoothing never uses depth. The pond reflection only reads it when present |

---

## 9. Per-event flows

### 9.1 Device creation (game start)
1. The hook thread installs the CreateDevice detour.
2. The game calls `IDirect3D9::CreateDevice`, which reaches `HookedCreateDevice`. See 2.2 for borderless, HDR format and
   retry.
3. EndScene and Reset are attached.
4. The first EndScene initialises ImGui, the WndProc and the registry detours.
5. Callbacks registered earlier by startup `Install()` calls (on the hook thread) start firing.
6. Modules create their D3D resources lazily on the render thread, on the first frame that needs them. Log lines:
   "[DepthBlur] Resources ready (WxH, INTZ depth swapped in)", "[EdgeSmoothing] Resources ready", "[HDR] Resources ready".

### 9.2 Device reset (alt-tab from exclusive mode, resolution change)
1. `HookedReset`: ImGui invalidate; borderless params.
2. **`preReset`** callbacks release every `D3DPOOL_DEFAULT` object:
   - AO, Depth Blur (this also unbinds the INTZ surface and puts back the game's DS), Edge Smoothing, Lot Map Probe,
     `LightmapSmooth`.
   - `HdrOutput::BeforeReset` releases the HDR targets and re-requests FP16.
3. The real `Reset`.
4. On success: `HdrOutput::AfterReset` (scRGB again); **`postReset`** (AO, Depth Blur, Edge Smoothing reset their state
   and back-buffer identity); borderless re-applied; ImGui objects recreated.
5. The resources are rebuilt lazily on the next frame, with the status "Recreating after a video change...".

**Rule:** any module that owns a `D3DPOOL_DEFAULT` resource **must** register `preReset`. Otherwise `Reset` fails with
`D3DERR_INVALIDCALL`, which is standard D3D9 behaviour. Mind the four-slot limit (4.2).

### 9.3 Lot and world load (Apex view; engine details in [engine/lot-loading-and-streaming.md](engine/lot-loading-and-streaming.md))
- **Shader creation** during a load reaches the registry `CreatePixelShader` / `CreateVertexShader` hooks (Last).
  `LotLightBridge::PrecreatePs` / `PrecreateVs` and HdrNative's precreate build the patched copies now, so DXVK
  translates them during the load and not in the frame where a room first appears (`hdr_native.cpp` header;
  `lot_light_bridge.cpp` `UpdateHooks`).
- **Lot streaming** (render thread, inside WorldManager and the lot renderer pass):
  - `0x00AEA680` (lot load stages, budget 20 ms) and room lighting under `0x00ADBAD0` / `0x00ADB8F0` are sliced and capped
    by Smooth Streaming in the combined build (removed from the standalone, see [removed-features.md](removed-features.md)).
  - Level-light-share and object-bridge code runs inside the game's light calls where they are detoured
    ([features/night-lighting/level-light-share.md](features/night-lighting/level-light-share.md)).
- **World change** is detected in NTR's Present hook (`OnPresent`): the light-cells pointer changes. Then:
  1. log "[NightTerrainRelight] World loaded (...)";
  2. `LotLightBridge::OnWorldChanged()` drops the chunk maps, smoothed maps and atlas;
  3. `LevelLightShare::OnWorldChanged()`;
  4. `Settle` waits 5 to 60 s for the lot lamps, then arms one full terrain rebuild;
  5. "Terrain rebuilt" is logged when the countdown at cells+0x38 reaches -1.

  After that, `Reconcile` about once a second relights only the chunks whose bake rect overlaps a changed lamp. See
  [features/night-lighting/terrain-relight.md](features/night-lighting/terrain-relight.md).
- A typical log sequence (28/09):
  - "World loaded (night level 1.00)"
  - "Rebuild armed: world loaded"
  - "[SmoothStreaming] Lot pass resumed ..."
  - "Terrain rebuilt (world loaded; ...)"
  - "Rebuild armed: dusk"
  - "Local relight: 103 lamps changed, 13 terrain chunks relit"
  - "Terrain rebuilt (dusk; ...)"
  - "[LotLightBridge] Active"

### 9.4 A setting change in the menu
- **UI code:** on the render thread inside EndScene. It changes globals and calls `NotifySettingChanged()`.
- **Read-live patches:** the next frame or call picks the value up.
- **Base patches:** the hook thread reinstalls after 2 s.
- **NTR:** the hook thread schedules; the next EndScene reinstalls on the render thread.
- **Enable checkbox:** `Install()` / `Uninstall()` run immediately on the render thread.

---

## 10. Threads and synchronisation

### 10.1 The game's threads (engine side; full map in [engine/main-loop-and-services.md](engine/main-loop-and-services.md))
| Thread | What runs there | Evidence |
|---|---|---|
| **Render thread = main thread** | Main loop `0x00ECA960`: ServiceManager main services, WorldManager and lot streaming, terrain update `0x00C845C0`, the whole D3D9 frame (BeginScene .. Present), the window message pump | NOTAS-ILUMINACAO "Desempenho: mapa do motor"; `profiler_targets.tsv` (thread = render); `frame_profiler.cpp` header ("The render thread is the main thread") |
| **Simulation (Mono) thread** | `MonoScriptHost::Simulate` (the GC call at `0x00D819AA`, IdleSimulationCycle call at `0x00D81FDE`, sim job pump `ProcessJobs` called at `0x00D81FDA`), ServiceManager sim services `0x0059ED70`, script tasks `0x00D7FE80`, animation ticks. **The game UI runs here**, and the render thread does not wait for it each frame | Notes (same section); `gc_scheduler_patch.cpp` header; `profiler_targets.tsv` |
| Job workers ("JobThread", affinity mask 2) | `ExecuteJob 0x00599720` on any thread, e.g. resource read jobs `0x0072A4F0`; their mask-1 follow-ups run on the render thread | `frame_budget_patch.cpp` header; `profiler_targets.tsv` |
| Other game threads | Light tree gather (see `level_light_share.cpp` `g_gatherThread`, which may differ from the render thread) | `level_light_share.cpp` (it counts calls "on another thread") |

The 28/09 log shows different ids for the two main threads: SmoothPatchPrecise logged "simulation thread (id 23944)" and
"render thread (id 14128)".

### 10.2 The mod's own threads
| Thread | Created by | Runs |
|---|---|---|
| Loader thread (DllMain) | UAL | Process check, CPUID fix, allocator hooks, `CreateThread` |
| **Hook thread** (`HookThread`) | `DllMain` | The whole init sequence (1.3), including **startup `Install()` of every enabled patch**; then the message loop with `MemoryMonitor::Update` and **`patch->Update()` every ~10 ms** (base debounced reinstalls, NTR scheduling, Smooth Streaming reinstalls) |
| Logger flush thread | `Logger::Handler::Initialize` | Writes pending log lines once a second |
| Frame Profiler sampler | Profiler, when sampling is on (Advanced) | Suspends the render and/or sim thread at `g_sampleHz` (default 2000) and copies 4 KB of stack |
| Frame Profiler writer | Profiler | Appends hitches to `S3SS_Hitches.txt` at most once a second |
| DXT workers ("Apex DXT worker", up to min(logical processors - 2, 6)) | `DxtCodec::Parallel` (`features/dxt_codec.cpp`), at Faster Texture Compression's start with "Use several cores" on, or at the first large texture | Sleep on an event; encode rows of blocks of a large texture while the game's calling thread (which also encodes) waits in the DXT hook; normal priority, 256 KB stack reservation, never destroyed ([features/performance.md](features/performance.md), "Several cores") |

### 10.3 Which Apex code runs on which thread
| Code | Thread |
|---|---|
| All registry callbacks, `HookedEndScene` / `HookedReset`, RenderCallbacks, PostScene effects, HDR / Picture pass, Night Lighting draw hooks and Present dispatcher, dev hotkeys (GetAsyncKeyState) | Render |
| All menu code (`SettingsGui::Render`, `RenderCustomUI`, checkboxes calling `Install` / `Uninstall`, `ConfigStore::SaveAll` from the UI) | Render (inside EndScene) |
| Startup `Install()` (`LoadPatches`, `EnsureEnabledByDefault`), patch constructors, `Update()` | Hook thread |
| NTR terrain / light logic, `ObjectLightBridge` / `LevelLightShare` refreshes, Smooth Streaming frame gate, Frame Budget service detours, profiler timed calls on the render thread | Render (the game calls them there, or NTR's Present hook drives them) |
| GC Scheduler (the redirected CALL at `0x00D819AA`) | Simulation |
| Game-function detours in general | Whatever thread the game calls them on. Check `GetCurrentThreadId()` against a recorded thread when it matters, as `level_light_share.cpp`, `object_light_bridge.cpp`, `smooth_streaming_patch.cpp` (`g_passThread`) and `rig_tracker.cpp` (`g_drawThread`) do |
| `CreatePixelShader` / `CreateVertexShader` hooks | Usually render. `lot_light_bridge.cpp` notes the setters may be reached "from another thread (Ensure*, even at shader creation)" (unverified which) |

### 10.4 Synchronisation rules (as practised in the code)
1. **Game state is touched only on the render thread.** `object_light_bridge.cpp`: "the game's light system may only be
   touched from the render thread". Requests from elsewhere set a flag (`g_refreshRequested`) that the next `OnPresent`
   handles (`object_light_bridge.cpp`, `level_light_share.cpp`).
2. **Anything that frees D3D resources used by draw hooks, or rewrites code the render thread runs, happens on the render
   thread.** NTR's `DeferredReinstall` pattern (5.3). Profiler attach runs at the frame boundary: "so the call-site writes
   never race the render thread executing them".
3. **Registry state** is guarded by one recursive mutex. It is held during each dispatch and each (un)registration, and
   released before the original call. (Standalone since 2026-09-29: the draw and state chains run without it on the render
   thread, and `UnregisterAll` from another thread waits for the render thread instead; see 3.6.)
   - Registration from the hook thread (startup, debounced reinstall) blocks while the render thread dispatches.
   - Do not wait, from inside a registry callback, on a lock that another thread holds while it registers. The profiler
     uses `try_lock` on `g_ctrlMutex` in its Present hook for this reason.
4. **Settings read by hooks** are plain globals or `std::atomic` with relaxed loads. A torn read of a float slider is
   accepted. Module-level `std::mutex` only where Install and hooks can overlap (`lot_light_bridge.cpp` `UpdateHooks`:
   "Install runs off the render thread at startup while Present may call the setters"; `PostScene` `g_mutex`).
5. **Callback slots** (`RenderCallbacks`, `ExtraHooks` observers, depth substitution) are lock-free atomics.
6. **No file I/O on the render thread** in hot paths. The logger is buffered; the profiler uses a writer thread.
   `ConfigStore::SaveAll` from a UI click is the accepted exception.
7. **Detours transactions:**
   - `DetourUpdateThread(GetCurrentThread())` only. Other threads are not suspended, so installing a detour on a function
     that another thread is executing is a race.
   - The profiler hooks hot multi-thread functions "by hand with all other threads suspended and checked" (`AttachSafe`).
   - Game-code byte patches are written from whichever thread installs.

---

## 11. Apex-owned vs inherited files (combined tree)

From PLANO-SEPARACAO section 1a / 1b. `git diff 5eb2c65` is the merge-base with upstream.

**Apex-owned (new files):**
- HDR and post: `hdr_output.*`, `hdr_native.*`, `post_scene.*`, `depth_share.h`.
- Hook plumbing: `d3d9_extra_hooks.*`, `render_callbacks.h`.
- Tools: `frame_profiler.*`, `map_view.*`, `rig_tracker.*`, `light_probe.*`, `light_diag.*`.
- Night Lighting: `lot_light_bridge.*`, `shader_patches.*`, `lightmap_smooth.*`, `level_light_share.*`,
  `object_light_bridge.*`.
- Headers and tables: `apex_ui.h`, `build_flavor.h`, `shader_ids.h`, `*_hlsl.h`, `*.hlsl`, `wall_lamp_table.h`,
  `floor_atlas_table.h`.
- Patches: `ambient_occlusion`, `edge_smoothing`, `depth_blur`, `frame_capture`, `lot_map_probe`, `night_terrain_relight`,
  `smooth_streaming`, `gc_scheduler`, `frame_budget` (written after the plan).
- Third party: `third_party/smaa` (MIT), `third_party/dxvk/d3d9_vk_ext.h` (zlib).

**Apex edits to upstream files:**

| File | Apex change |
|---|---|
| `d3d9_hook.cpp` | RenderCallbacks fire points; HdrOutput calls in CreateDevice, EndScene and Reset |
| `d3d9_hook_registry.*` | recursive mutex, `CallOriginal*` additions, DIP/DP per-hook timing |
| `config/config_store.cpp` | HdrOutput and FrameProfiler save/load |
| `gui.cpp` | `IsApexPatch`, `RenderApexFeature`, Apex and Display tabs, Borderless moved, title |
| `dllmain.cpp` | `FrameProfiler::Shutdown` |
| `logger.cpp` | Buffered rewrite |
| `patches/gc_try_to_collect_patch.cpp` | Refuses while GcScheduler is on |
| `patches/smooth_patch_precise.cpp` | Timer resolution, MMCSS, WaitOnAddress |
| vcxproj | `TargetName` S3SSApex, `S3SSPublic` |

**Inherited S3SS framework used by Apex:** `patch_system.h`, `optimization.*`, `patch_settings.h`, `patch_helpers.h`,
`logger.*`, `utils.h`, `version.h`, `config_paths` / `config_store`, the D3D bootstrap (`d3d9_hook.cpp`), the registry,
`gui.cpp` / `settings_gui.h`, and `qol.*` (font scale, toggle key).

**Not used by Apex:** `pattern_scan.*`, `vtable_manager.*`, `hooks.cpp`, `settings.*`, `config_value_manager.*`,
`migration.*`, `memory_statistics.*`, `cpu_optimization.*`, `allocator_hook.*`.

---

## 12. The standalone split (planned / in progress)

> **Status (2026-09-28):** in progress in `S3SSApex\`. Nothing in this section describes the frozen code. The source is
> `S3SS-dev\PLANO-SEPARACAO.md` (written against HEAD `f18cca8` plus the working tree), amended by the **user decisions
> of 2026-09-28** in 12.0. Where the plan and those decisions disagree, the decisions win. Check the standalone's own
> code and `CLAUDE.md` for the final names.

### 12.0 Decisions of 2026-09-28 (they override the plan)
- **Content baseline = v0.1.0.** Night Lighting, Depth Blur and Edge Smoothing in the standalone start from **v0.1.0**:
  combined-tree commit `b84d5f1` ("Night Remake alpha: rebuilt night lighting and post-processing effects", released as
  `nightremake-v0.1.0-alpha`). Three later pieces are kept on top of it:
  - **Picture** filters, SDR only (`[qol.picture]`, [features/picture-filters.md](features/picture-filters.md));
  - **SMAA 1x** inside Edge Smoothing ([features/edge-smoothing.md](features/edge-smoothing.md));
  - the **Depth Blur fade in map view** (`MapView::IsOpen`, `0x0073E060`,
    [engine/camera-and-map-view.md](engine/camera-and-map-view.md)).
- **Post-0.1.0 lighting changes come back one at a time,** each only after the user has tested it, **fences first**.
  The list, the order and the state of each one: [changes-since-0.1.0.md](changes-since-0.1.0.md) (being written).
  Until a change is re-added, the feature docs (written against `combined-final`) describe the target, not necessarily
  what the standalone does.
- **Removed** (findings in [removed-features.md](removed-features.md)): HDR output, Native HDR (lamp gain), Ambient
  Occlusion, and the three performance patches **Smooth Streaming, Script GC Scheduler and Service Frame Budget** (no
  perceptible gain for the user; the engine knowledge they produced is kept in `docs/engine/`). Smooth Patch Precise
  improvements stay out too (upstream PR candidate).
- **Frame Profiler stays, dev build only** ([features/frame-profiler.md](features/frame-profiler.md)).
- **Localized terrain relight lives in Night Lighting.** In the combined build Night Lighting asked Smooth Streaming
  (`SmoothStreamingRelightTerrainRects`) to flag the terrain chunks under a changed lamp. In the standalone the Night
  Lighting module sets `chunk+0x55` on the overlapping chunks itself, with no queue
  ([engine/terrain-and-light-bake.md](engine/terrain-and-light-bake.md)).
- **Framework rewritten from scratch.** No S3SS source is copied (upstream has no licence). The standalone keeps the
  concepts of sections 2-7 (a D3D9 hook registry with priorities and Skip, extra hooks, render callbacks, patch objects
  with TOML settings, a buffered logger), with two deliberate differences from the combined framework:
  - **stable hook order**: hooks of equal priority run in registration order (the combined registry uses `std::sort`,
    3.2);
  - **growable callback lists** instead of `RenderCallbacks::kSlots = 4` fixed slots (4.2).
  Section-level names (`D3D9Hooks`, `ExtraHooks`, `RenderCallbacks`, `PostScene`, `DepthShare`) are kept where they
  still fit; read the standalone code for the final API.
- **Snapshot of the tree (28/09, about 14:45; it changes quickly):** `framework/` had `apex_log.*` (new buffered logger,
  writer thread every 0.5 s, warnings flushed at once), `apex_util.*`, `game_version.*` (PE timestamp table, Steam value
  checked against `re\TS3W.exe`), plus `d3d9_extra_hooks.*` and `render_callbacks.h` still identical to the combined
  files. `features/*.cpp` and `patches/*.cpp` were byte-identical to `combined-final`, not yet reset to v0.1.0, and
  `patches/` still held `smooth_streaming_patch.cpp`, `gc_scheduler_patch.cpp` and `frame_budget_patch.cpp`. Check the
  tree before assuming either state.
- **UI** (standalone `CLAUDE.md`): new menu, Violet theme, sidebar plus feature cards, own hotkey Ctrl+Shift+F11.

### 12.1 Why a separate ASI
- Official S3SS's `SaveAll` rebuilds `S3SS.toml` from its own sections only, and its `LoadFromToml` skips unknown patch
  names. **Every Apex key in `S3SS.toml` is lost the first time official S3SS saves.**
- Both mods would write `S3SS_LOG.txt` (truncate) and `imgui.ini` in `Game\Bin`.
- Upstream has no LICENSE. Decision: the framework is rewritten rather than carried (12.0); ask sims3fiend before
  publishing anyway, and credit "based on Sims3SettingsSetter by sims3fiend".

### 12.2 Coexisting with official S3SS on one device
**Two Detours chains:**
- Each DLL links its own Detours and patches the same DXVK function bodies. Detours relocates an existing `E9` into the
  new trampoline, so chains compose in either order, and **the mod installed last runs first** (outermost).
- **Hazard: two threads detouring the same prologue at once.** This can happen on `CreateDevice`, when both hook threads
  race. The planned fix:
  1. In DllMain, detour the `Direct3DCreate9` export of the loaded `d3d9.dll`. `TS3W.exe` imports it statically: IAT
     slot `0x00F95A58` = `d3d9.dll!Direct3DCreate9` (`research\engine_map\iat.map`; spike 1d answered for the import,
     not yet for the moment of the call).
  2. Install Apex's CreateDevice detour lazily, inside the first `Direct3DCreate9` call (call_once).
  3. Fallback: detect S3SS and wait up to about 3 s for its `E9`.
- Never detach at process exit.
- S3SS's registry is pass-through (it registers nothing). Apex's raw "call original" paths may pass through S3SS's empty
  dispatch when S3SS is inner. This is harmless today.
- Log, at each attach, whether the prologue was clean or already an `E9`, and into which module it jumps.

**CreateDevice parameter conflicts (plan 2c): superseded.**
- The plan analysed S3SS borderless (Windowed, SwapEffect, LOCKABLE) against Apex's FP16 back buffer.
- The standalone makes **no** parameter changes at all, so there is no conflict. Borderless stays in S3SS.

**HDR pass placement (plan 2d, "HDR pass moved to Present at priority -500", and migration step 6): superseded.**
- There is no HDR pass. What remains is the Picture SDR pass, at Apex's own EndScene.
- For SDR, the plan notes either EndScene order is correct:
  - Apex outer: S3SS's menu draws after the filters.
  - Apex inner: the menu counts as UI via the scene copy.
- Calling `EndScene` from a pass would re-enter S3SS's hook and draw its menu twice: never do it.

**Two ImGui contexts (plan 2e):**
- Each DLL has its own static ImGui, so the contexts are separate.
- Set `io.IniFilename` to `...\Apex Radiance\apex_radiance_imgui.ini` (the previous standalone used
  `...\S3SS\Apex\apex_imgui.ini`) and use window ID `###ApexWindow`.
- S3SS's WndProc eats all input while its menu is open. The plan:
  - subclass at Apex's first Present, so Apex is outer;
  - eat only what Apex's ImGui wants (`WantCaptureMouse` / `WantCaptureKeyboard`) or Apex's toggle key;
  - pass everything else down.
- **Hotkey Ctrl+Shift+F11.** S3SS toggles on bare Insert and ignores modifiers. Make the Apex key rebindable, stored in
  `ApexRadiance.toml [ui]`.
- Set `NoMouseCursorChange`.

**INTZ vs S3SS (plan 2f):**
- S3SS does not hook Set/GetDepthStencilSurface, so the Depth Blur swap has no competitor.
- ImGui's DX9 state blocks do not include the depth-stencil binding.
- Keep the ExtraHooks vtable-sharing guard.

**Detecting S3SS (plan 2g):**
- Scan every other module's read-only data for `"S3SS Log - Started at "` and `"###S3SSWindow"`.
- If the module also contains `"Sims3 Settings Setter Apex Edition"`, it is **the old combined build**: refuse to start
  and tell the user to delete it.
- A module with that old product name but without S3SS's two strings (or named `S3SSApex.asi`) is **the previous
  standalone build**: `Info::oldStandalone`, banner "An older S3SSApex.asi is also installed; delete it from Game\Bin".
- Named mutexes (implemented, `S3SSDetect::AcquireInstanceMutex`, called from DllMain): `Local\ApexRadiance.<pid>`
  against a second copy of Apex Radiance, and the previous standalone's `Local\S3SSApex.<pid>`. If the second one is
  already held, an old `S3SSApex.asi` loaded first and runs: Apex Radiance stays idle and writes only an error line to
  its log. Otherwise Apex Radiance takes it too, so an `S3SSApex.asi` loading later finds it held and idles by its own
  duplicate check (UAL normally loads `ApexRadiance.asi` before `S3SSApex.asi`, alphabetical order; unverified for this
  UAL version).
- Read S3SS's intent read-only from `S3SS.toml` (`[patches.X].enabled`, `[qol.ui].disable_overlay`,
  `[qol.borderless_window].mode`).
- Record S3SS's module range.

### 12.3 Game-code patch conflicts (plan section 3, updated for the removals)
| Apex feature | Overlap with S3SS | Policy |
|---|---|---|
| ~~Script GC Scheduler~~ (CALL redirect at `0x00D819AA`) | **Removed from the standalone.** If ever revived: same 5 bytes as S3SS GCTryToCollect, which NOPs them after checking only `E8` | Revival rule: refuse both ways, 1 s watchdog on the E8 target, never restore over foreign bytes ([removed-features.md](removed-features.md)) |
| ~~Smooth Streaming~~ (`0xAEA680`, `0xAEA6AC`, `0xC7CEA0`, `0xADB120`, `0xC845C0`, `0xC84C3C`) | **Removed.** No byte overlap with LotStreamingOptimizations | - |
| ~~Service Frame Budget~~ (detours on `0x599A10`, `0x7377F0`, `0x608630`, `0x5F0E50`; bytes at `0x599A20`, `0x5F0EBF`, `0x5F127D`, `0x60829D`) | **Removed** | - |
| Frame Profiler (dev build only; many entry detours and hand hooks) | Avoids LSO's sites; skips any target whose bytes differ | Cooperate. Label "owned by S3SS" in its hook table |
| Night Lighting `level_light_share` (CALL redirects to LPWAL at `0x6A1187` / `0x6A126F` / `0x6A3336`, return-address test `0x69FE19`) | S3SS LightingQuality detours LPWAL's entry `0x69FD60` and calls it N times: no byte overlap, but Apex's tests run N times | Cooperate. Measure the cost at 16 and 32 samples; show "S3SS Lighting Quality active" |
| Night Lighting terrain bake sites (visitor `0xC29626`, story gate `0xC294D9`, arm sites `0x6B6516` / `0x6B60D3` / `0x6B6618`, lot pass `0xC7F87D`, street lamp colour `0x6BE18C`) and the localized relight (writes `chunk+0x55`) | None found. S3SS's LSO detours `WorldManager::Update` (`0xC6D570`), which calls the terrain update; Apex never patches that function | Cooperate |
| Other Night Lighting sites (rig tracker, object light bridge, lamp colour), `map_view` (calls `0x73E060`, the same getter LSO uses) | None found | Cooperate |
| Apex's improved Smooth Patch Precise (`0xD81FDE`, `0xEC9FBA`) | Head-on collision with S3SS's Smooth Patch | **Not shipped** in the standalone; upstream PR instead |

**`ApexConflictGuard`** keeps one table of `{site, length, vanillaBytes, owner}` and works in four stages:
1. **Defer:** install after S3SS's startup patch load (first Present plus about 1 s, or S3SS absent).
2. **Intent:** read `S3SS.toml`.
3. **Truth:** compare bytes at install, and follow E8/E9 targets to a module.
4. **Watchdog:** re-check every 1 s; never restore over foreign bytes.

With the performance patches gone, the guard mainly protects Night Lighting's game-code sites and the dev-only profiler.

### 12.4 Standalone layout, files and config
- **Folders:**
  - `framework/`: bootstrap, registry, extra hooks, render callbacks, patch objects and settings, byte-patch and detour
    helpers, logger, utils, config, conflict guard, S3SS detection, all **newly written** (12.0);
  - `features/` and `patches/`;
  - `shaders/`;
  - `third_party/` (smaa with its MIT `LICENSE.txt`; the dxvk header only if a DXVK extension is still used; HDR was its
    only user);
  - `apex_gui.cpp`, `apex_main.cpp`.
- **Not rebuilt** (no Apex use): S3SS's `pattern_scan`, `vtable_manager`, `hooks.cpp`, settings hooks, config values,
  INI migration, memory stats, CPU optimisation, allocator hook and mimalloc, `qol.*`, the CPUID topology fix.
- **`apex_main.cpp`:** process check, game version detection, config load and migration, patch instantiation, its **own
  `Update()` pump every 10 ms**, and the profiler shut down first.
- **Output name `ApexRadiance.asi`** (rename of 2026-09-28; before it the plan kept `S3SSApex.asi`). Solution
  `ApexRadiance.sln`, project `ApexRadiance.vcxproj`. It sorts before `Sims3SettingsSetter.asi` and `S3SSApex.asi`.
  The old `S3SSApex.asi` must be deleted from `Bin`; the official `Sims3SettingsSetter.asi` stays beside it.
  - MSBuild flavour property: `/p:ApexPublic=true`, which defines `S3SS_PUBLIC` and writes to `Public\`.
- **Files go to `Documents\Electronic Arts\<localized>\Apex Radiance\`** (`framework/apex_paths.*`; the localized
  folder is resolved as before):
  - `ApexRadiance.toml`;
  - `ApexRadiance_LOG.txt` (header "Apex Radiance Log");
  - `apex_radiance_imgui.ini`;
  - dev outputs: `ApexRadiance_Hitches.txt`, `ApexRadiance_FrameCapture.txt`, `ApexRadiance_LightDiag.txt`,
    `ApexRadiance_LightProbe.txt`, `LightProbe\`, `ApexRadiance_Censo.txt`, `Censo\`, `ShadersRecusados\`.
  - It never writes `S3SS.toml`, `S3SS_LOG.txt` or anything in the previous standalone's `...\S3SS\Apex\` folder.
- **Schema:** the table names are kept, so settings carry over:
  - `[qol.picture]`; `[qol.frame_profiler]` (dev);
  - `[patches.<Name>]` for NightTerrainRelight, EdgeSmoothing, DepthBlur, FrameCapture (dev) and LotMapProbe (dev);
  - new `[ui]` (toggle_key, font_scale, recommend_s3ss, welcome_done, sidebar_collapsed; see ui.md) and `[meta]` (version, migrated_from_s3ss).
  - Menu profiles: `Profiles\<name>.toml` in the same folder, the same feature tables (see ui.md "Profiles").
  - Not read any more (features removed): `[qol.hdr]` (except the Picture grade keys as a migration fallback, see
    picture-filters.md), `[patches.AmbientOcclusion]`, `[patches.SmoothStreaming]`, `[patches.GcScheduler]`,
    `[patches.FrameBudget]`.
  - Night Lighting keys added after v0.1.0 appear only when their change is re-added (12.0).
- **One-time migration** (`ApexConfig::EnsureMigrated`, call_once in the init thread), only while `ApexRadiance.toml`
  is missing; the log line `[Config] Migration path: ...` and Settings > Status > Settings say which path ran:
  1. If the previous standalone's `...\S3SS\Apex\Apex.toml` exists: copy it byte for byte (same schema, every key).
     Its `apex_imgui.ini` is not copied (the menu's scale changed; the new default size applies). The old folder is
     left in place. If it cannot be read or written, nothing is written and the next start tries again.
  2. Else, if `S3SS.toml` exists: back up the raw bytes to `Apex Radiance\S3SS.toml.pre-split.bak`, copy only the Apex
     tables (NightTerrainRelight strengths reset to v0.1.0's 0.57 / 1.0 / 1.0).
  3. Else (or if S3SS already dropped the tables): defaults, logged.
- **Menu:** one window, "Apex Radiance" (`APEX_PRODUCT_NAME`), in the Violet layout (sidebar: Lighting, Image,
  Display, Developer in the dev build, Settings; see [ui.md](ui.md)).
  - A Settings/About page: S3SS detected, conflict status, credits ("based on Sims3SettingsSetter by sims3fiend").

### 12.5 Migration steps and test plan (plan sections 5 and 6, condensed and updated)
**Steps:**
0. Freeze. Done: tag `combined-final`.
1. Spikes:
   - a. DXVK drawing at Present. This mattered for HDR; for the Picture pass it is only relevant if it moves to Present.
   - b. Two Detours chains in both load orders.
   - c. Two ImGui overlays and WndProc chaining.
   - d. When the game calls `Direct3DCreate9` (the static import itself is confirmed, 12.2).
2. Skeleton that loads next to S3SS.
3. **Framework rewrite** (was "carve-out" in the plan).
4. D3D bootstrap (Direct3DCreate9 export detour, lazy CreateDevice, EndScene, Reset). No device parameter changes.
5. Bring in the v0.1.0 modules plus Picture, SMAA and the map-view fade; rename the output files.
6. HDR relocation. **Superseded**: only the Picture SDR pass remains to place (Apex's EndScene).
7. Menu.
8. Coexistence layer.
9. Config migration.
10. Re-add the post-0.1.0 lighting changes one at a time, fences first, each after a user test
    ([changes-since-0.1.0.md](changes-since-0.1.0.md)).
11. Test matrix.
12. Docs and release.

**Test variants:**
- A: Apex alone.
- B: S3SS alone.
- C: both, Apex first.
- D: both, S3SS first (rename to `0_Sims3SettingsSetter.asi`).
- E: C and D with S3SS headless.
- F: S3SS borderless plus Resolution Spoofer.
- G: old combined build present (must refuse).

**Check in each variant:**
- separate logs and chain-position lines;
- both menus usable, with S3SS's menu open too;
- Edge Smoothing and Depth Blur before any UI in both orders, and INTZ surviving Reset;
- Night Lighting with LightingQuality at 16 and 32 samples;
- Night Lighting's localized terrain relight with S3SS's LSO on (map view blocker active);
- profiler skip labels (dev build);
- config migration cases;
- public vs dev flavour.

---

## 13. How to add a new feature (checklist)

1. **Study first.** Read the relevant docs in `docs/engine/` and the feature docs' "Pitfalls" sections. Get evidence:
   F7 / F8 captures, decompile in `S3SS-dev\re\out`, engine_map. Do not guess addresses or constants.
2. **New patch file.** Put it in `patches/<name>_patch.cpp` (or a module `.cpp/.h` pair plus a thin patch) and **add it
   to the vcxproj** `ClCompile` list. Files on disk that are not in the project are silently not built.
3. **Class and registration:**
   - `class XPatch : public OptimizationPatch`, with `XPatch() : OptimizationPatch("XName", nullptr)`. `"XName"` becomes
     the TOML table `[patches.XName]`, so choose it once and never rename it.
   - `APEX_REGISTER_FEATURE(XPatch, {.displayName, .description = "... Part of " APEX_PRODUCT_NAME ". Credits:
     @loinyx", .category, .experimental, .enabledByDefault, .supportedVersions = VERSION_STEAM (unless verified on
     others), .technicalDetails = {addresses, patterns}})`.
4. **Install / Uninstall:**
   - Both idempotent (`if (isEnabled) return true;`), with `lastError.clear()`, `Fail(msg)` on errors, and
     `isEnabled = true/false` at the end.
   - Resolve game addresses with `AddressInfo` (Steam address plus a pattern plus `expectedBytes`), or `ScanPattern`
     with a byte check.
   - Write through `PatchHelper::Write*` with an `expectedOld` and a tracker. Restore with `RestoreAll`.
   - Detours through `DetourHelper`.
   - Remember that startup `Install()` runs on the **hook thread**, before the D3D device exists.
5. **Settings:**
   - Register in the constructor, with English descriptions and sane defaults.
   - Choose the reinstall policy:
     - read live and override `Update() { pendingReinstall = false; }`; or
     - reinstall only when code bytes change (like Smooth Streaming); or
     - defer to the render thread (NTR `DeferredReinstall`) if Uninstall frees D3D objects or rewrites code the render
       thread runs.
6. **D3D hooks:**
   - Register in `Install`, and `UnregisterAll(kHookName)` in `Uninstall`.
   - Priority:
     - `First` for observers and frame-boundary resets that must see every original draw;
     - `Normal` for draw replacement (lot_light_bridge's slot);
     - `Late` for a second replacement layer (HdrNative in the combined build);
     - `Last` for probes and captures;
     - never ±1000, which is reserved for the profiler.
   - Return `Skip` only when you issued the draw yourself. Guard the re-issue with an own-call flag.
   - Use `CallOriginal*` when no module may see the call.
   - Never (un)register the method type you are being dispatched from.
   - Post-scene effects: `PostScene::Add(order, fn)` with a new order value, drawing with DrawPrimitiveUP.
   - Readable depth: `DepthShare::Request(true)` / `Request(false)`, and handle `Texture() == nullptr` with
     `DepthShare::Status()`.
   - `D3DPOOL_DEFAULT` resources: `RenderCallbacks::Add(preReset / postReset)`. Check the four-slot limit.
7. **Threads:**
   - Touch game objects only on the render thread; queue from elsewhere.
   - No blocking I/O or per-draw logging on the render thread.
   - Use atomics for values that the UI writes and hooks read.
8. **UI:**
   - Add the name to `IsApexPatch` (`gui.cpp`).
   - Place it with `RenderApexFeature("XName", "Title", defaultOpen)` in the Apex tab (lighting and effects, or the
     "Performance" section) or the Display tab (image and window).
   - In `RenderCustomUI`:
     - `SAFE_IMGUI_BEGIN()`, a status line, the main controls with `Hint()` tooltips;
     - `TreeNode("Advanced##XName")` for tuning;
     - `if constexpr (!kPublicBuild) TreeNode("Developer##XName")` for diagnostics;
     - English only; no visible credit lines.
9. **Logging:** use a `[XName]` prefix. Log install success with the resolved addresses (the log is the first thing read
   when diagnosing). Use Warning for "pattern not found / bytes differ" and Info for state changes. Remember that
   `LOG_DEBUG` never reaches the file.
10. **Build both flavours** (dev and public) and test in game. See [workflow.md](workflow.md).
11. **Docs:**
    - Add `docs/features/<name>.md` following the template in the brief: settings table with TOML keys, addresses table,
      pitfalls, testing.
    - Link it from `docs/README.md`.
    - Update this file's registry table (3.4) and thread table (10.3) if the feature adds hooks or threads.
    - Record what failed in the feature doc's "Pitfalls" section.

---

## 14. Known gaps in this document
- Whether the game ever creates a non-HAL device (for example NULLREF) was not checked. The log only shows one HAL device.
- Which thread calls `CreatePixelShader` / `CreateVertexShader` during loads was not measured.
- The `preReset` five-users / four-slots overflow comes from reading the code and was never seen at runtime.
- The ASI loader DLL name (`wininet.dll` = Ultimate ASI Loader) is inferred from README, not checked by hash or version.
- The standalone's file names and MSBuild property are settled (12.4); its Picture hook point must be read from the
  standalone code.

## Local Report storage/UI revision (`2.5.4-test-report-review`, unreleased)

Report controls remain on the overlay/render thread. `Captures::WriteText` serializes completed diagnostics, retaining failed writes for an explicit retry; no polling of lights or new game-memory hook was added. Capture receipt, description, session and PNG job state share `g_lock`. WIC threads only write the supplied image and mark their own folder complete under the lock; translation and completion notices are handled on the render thread. `Saving()` includes queued/encoding PNG work, whereas `ScreenshotPending()` only suppresses overlays before the picture is read. Last-save polling performs no filesystem scan. The required inline post-save description card reads its target folder once when opened; explicit saves use atomic replacement through `ApexUtil::WriteFileAtomic`, preserving the previous annotation if replacement fails. `ReadDescription` rejects reparse files and bounds reads to 4 KB; its non-empty body controls the UI send guidance. The legacy receipt-note API remains for retry compatibility; current inline edits target a guarded direct capture/collection folder. Library scanning is limited to its visible page; file contents are read on request, and reparse directories are not followed. Menu removal renames direct capture folders into `.Removed`, with collision-safe explicit Undo. Explorer remains a separate COM thread. Public builds share Report tools but continue to omit Developer UI. See `docs/features/bug-reports.md` for failure/undo limits and validation.

The local overlay revision (`framework/overlay_clock.h`, `framework/overlay.cpp`) samples a monotonic frame delta even when no menu/notice is drawn; the Win32 backend's idle-gap delta is overridden before ImGui::NewFrame. It preserves real long frames. A slow-panel phase warning is capped at one per 10 seconds and is not GPU timing. `GuiClient::CanOpen` exposes only atomic readiness; the render-thread 200 ms check requires an active loaded WorldManager and, when Night Lights is on, the world-live signal, followed by 3 seconds. The fallback offsets are documented in docs/engine/lot-loading-and-streaming.md; no additional game hook/write is introduced. The generic fallback and actual loading transitions need in-game validation.

The approved stage-based presentation adds no hooks, timers, threads or persisted setting keys. `ReportStages` reflects recording, pending receipt, description editing and successful completion; it is not navigation. `ReportCaptureTools` draws only while recording and shares the recording card. Start and comparison no longer hide the overlay; Return to game and point aiming do so explicitly. Point return behavior remains owned by the existing probe completion path. Library contents and sharing guidance are inline cards; only removal uses a confirmation dialog. Receipt notes are cached from reading/editing and contents are enumerated on first expansion, with separate cache identity from library details. Restoring comparison remains available after capture controls disappear.

Private RC temporal candidate, 2026-10-02: `2.5.4-rc-temporal-pool-test` retains the report-library RC UI and adds targeted pool jitter protection and phase-aware temporal reprojection. Gameplay validation is pending; not installed or published. See [features/edge-smoothing.md](features/edge-smoothing.md) for evidence, fallback scope, quality tradeoff and checks.
