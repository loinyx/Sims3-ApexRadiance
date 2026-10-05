# Display and Fluency: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The feature page is
[features/display-fluency.md](../features/display-fluency.md).

### 2026-10-02: release candidate `2.5.4-rc-display-fluency`

**Context:** design proposal 3, built into the unified ASI as an extension of the
[presentation candidate](presentation.md). Not published or installed; lighting, terrain and image pipelines unchanged.

**Player flow:** Display > Display and fluency had two columns: *How the game appears* (mode and monitor) on the left,
separate *Smooth movement* (Apex FPS) and *G-SYNC / FreeSync* cards on the right, stacking on narrow panels. V-Sync,
ownership details, CPU wait timings and vendor diagnostics were expandable. Overview and search led to the same page,
translated into the four menu languages.

- Four modes: Window, Borderless window, Borderless fullscreen and Game fullscreen; legacy `off` stayed as *Keep game
  mode* under details. Window restored the Windows frame. Window changes were posted to the window thread, kept the game
  backbuffer size, did not resize other displays and respected minimization; failed posts cleared the pending flag.
- The chosen and current modes were shown separately. Game fullscreen described D3D9 parameters, not proof of
  DXVK / Vulkan exclusive scanout. Crossing windowed and exclusive state needed a successful game device reset or
  restart; the bootstrap retried the game's original parameters when the new request failed. No render-thread device
  reset was forced.
- Monitor selection applied to windowed modes and did not change the game's D3D adapter. Missing saved monitors fell back
  to the current one while the preference was kept for reconnection. Monitors were identified by Windows display name;
  negative desktop positions were supported. Display changes refreshed the inventory and invalidated VRR results,
  including queries in flight.

**Settings:**

| `[display]` key | Default | Values | Application |
|---|---|---|---|
| `mode` | `borderless_fullscreen` | `off`, `windowed`, `borderless_windowed`, `borderless_fullscreen`, `exclusive_fullscreen` | Style and position live when the device is already windowed; device mode on successful creation or reset |
| `monitor` | empty | Windows GDI display name; empty follows the window | Live for windowed modes |
| `vsync_policy` | 0 | 0 preserve, 1 on, 2 off | Successful device creation or reset |
| `frame_pacing` | false | boolean | Live, guarded when Sims3SettingsSetter has a foreground FPS limit configured |
| `target_fps` | 120 | 30 to 240 | Live |

The Window profile part included these fields; old profiles without monitor or synchronization kept current values.
Page and global reset cleared the monitor preference and restored the defaults; Undo captured the same state. Imported
monitor names unavailable on the machine used the fallback.

**Read-only VRR query** (`features/display_monitor.cpp`): optional system-directory driver libraries only; no bundled
driver DLL, no driver profile written, no renderer replaced.

- NVIDIA: public `NvAPI_Disp_GetVRRInfo` from x86 `nvapi.dll`; the GDI name resolved through
  `NvAPI_DISP_GetDisplayIdByDisplayName`; the 24-byte V1 ABI and interface IDs from NVIDIA's MIT-licensed SDK. Success
  with `bIsVRREnabled` reported *Enabled on this monitor*; success with the bit clear reported *Not active when checked*
  (never "disabled in the Control Panel"). Extra raw flags were diagnostic only and did not establish per-process
  engagement.
- AMD: documented x86 ADL2 adapter, display and FreeSync queries (not a guessed ADLX ABI), in a separate ADL context. A
  unique connected and mapped logical display had to match the Windows display identity; clone and Eyefinity mappings
  stayed unknown. Capability success was required to report unsupported; failures never became unsupported. The Gaming
  bit from `FreeSyncState_Get` is a driver configuration report, not proof of game scanout. AMD runtime validation was
  pending (the development machine has NVIDIA hardware).
- Driver calls ran in one short-lived, module-pinned worker, never in DllMain, Present or the menu render thread; the
  render path read snapshots. A first visit, reopened page, changed monitor or *Check again* could request a query; no
  per-frame polling or automatic setting writes. Results were tied to the game window's monitor, stale in-flight results
  were rejected, and unknown, checking, enabled, inactive and confirmed unsupported were distinct states. "Enabled on the
  display" was never treated as evidence that the game used VRR at that moment.

**Conflicts and ownership:** a conditional Conflicts sidebar entry appeared only for the confirmed runtime MSAA
incompatibility (a successfully queried multisampled game backbuffer plus enabled dependent Apex effects), listing the
affected effects and the steps to turn native Edge Smoothing off; it disappeared when resolved. The Sims3SettingsSetter
window and FPS guards stayed: a guard that stopped Apex's equivalent from running was shown as ownership information,
not as a conflict. Unknown driver overrides, restart pending and VRR inactive were not conflicts. *Use Apex* (primary)
and *Keep S3SS FPS* (secondary) expanded the exact foreground-limit handoff steps without applying unsupported live
changes. Precise, TPS and the background FPS of Sims3SettingsSetter stayed untouched; no Sims3SettingsSetter, DXVK,
Windows or driver configuration file was edited.

**Multiple monitors:** the candidate added correct targeting and honest mode reporting, not a fix for FPS loss with a
second monitor (Windows composition, the renderer, adapters, driver settings and mismatched refresh rates can all
matter). The planned comparison: the same save, camera, resolution and one FPS controller, previous candidate against
this one, with two monitors and with only the game monitor, each window mode separately, Precise / TPS kept, frame-time
distributions and camera hitches. DXVK's `dxvk.allowFse` can affect presentation but has compatibility trade-offs and
was not enabled; `d3d9.presentInterval` and driver V-Sync settings may override the D3D9 request. Required gameplay
checks (never run): Alt+Tab, minimization, switching window mode, DPI differences, moving to a secondary display,
disconnect and reconnect, loading and reset fallback, profiles and resets, the native AA conflict and the VRR
indicator.

**Local evidence:** an x86 harness linked the production monitor, window and presentation modules with only the
configuration, log and Sims3SettingsSetter services stubbed. It tested window geometry and caption restoration,
parameter fallback status, profile preservation, FPS ownership and eight native ImGui layouts (four languages x two
widths). A real read-only NVIDIA query succeeded on an AW3225QF at 240 Hz; a second 60 Hz display at a negative desktop
origin was enumerated. This was detection and window evidence, not gameplay VRR or frame-time evidence.

Sources: [NVIDIA VRR structure](https://docs.nvidia.com/nvapi/struct___n_v___g_e_t___v_r_r___i_n_f_o___v1.html),
[NVAPI SDK](https://github.com/NVIDIA/nvapi),
[AMD ADL FreeSync APIs](https://gpuopen-librariesandsdks.github.io/adl/display_8h.html),
[AMD ADL SDK](https://github.com/GPUOpen-LibrariesAndSDKs/display-library),
[DXVK Windows notes](https://github.com/doitsujin/dxvk/wiki/Windows).

### 2026-10-02: removed

**Outcome:** window modes, monitor selection, the Apex FPS and V-Sync controls and the driver VRR detection were removed
by maintainer decision before any release. Apex forwards the game's original CreateDevice, Reset and Present requests
without changing synchronization or window state; G-SYNC and FreeSync stay controlled by the driver and monitor. The
`[display]` table is dropped from profiles.
