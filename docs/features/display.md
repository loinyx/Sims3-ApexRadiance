# Display

Window mode, resolution and a frame-rate limiter, in System > Display. The game can run in a window, borderless over
the whole monitor, or in exclusive fullscreen at the monitor's highest refresh rate, at the game's resolution or a
chosen one. The limiter caps the frame rate evenly, with a separate cap while another window is in front, and shows
the measured rate and how much the frame time varies; an optional counter shows the frame rate in a corner of the screen. Both take precedence over Sims3SettingsSetter's borderless
window and frame limiter when that mod is installed.

## Status

| | |
|---|---|
| Availability | Released in 3.1.0 |
| Default | Game setting, no limit (nothing changes) |
| Menu | System > Display |
| Configuration | `[screen]` in `ApexRadiance.toml` (machine settings: not part of profiles) |
| Source | [`features/screen_control.cpp`](../../features/screen_control.cpp), [`features/frame_limiter_math.h`](../../features/frame_limiter_math.h) |

## The problem

- The game offers windowed and fullscreen only. Its fullscreen asks Direct3D for the adapter's default refresh rate,
  and switching mode needs its Options screen and often a restart.
- The game has no frame limiter: it draws as fast as it can in the foreground, sleeps 10 ms per frame when another
  program has the focus and 30 ms while its device is lost
  ([timers and sleeps](../engine/timers-and-sleeps.md#render-thread-waits-in-a-frame)).
- Sims3SettingsSetter's borderless window keeps the game's resolution stretched to the monitor and, under the
  [RTX Remix bridge](rtx-remix-bridge.md), was reported not to reach the bridge's server. Its frame limiter waits
  with millisecond sleeps on its own timer resolution.

## How Apex Radiance solves it

**Window mode and resolution** are applied to the game's own device requests:

1. Every `CreateDevice` and `Reset` passes through Apex's hook, which runs inside Sims3SettingsSetter's when Apex loads
   first (the usual order: Apex sees the parameters last). Apex rewrites the request: `Windowed`, the back-buffer size,
   and in exclusive fullscreen the highest refresh rate the adapter lists for that size (`EnumAdapterModes`). A game
   that runs in its own exclusive fullscreen at a size chosen in the menu gets that rate too, so the device never gets
   a rate the size does not have.
2. If the device refuses the rewritten request, the call is made again with the game's own parameters and the menu
   says so.
3. A change while the game runs uses the game's own device-lost path: Apex's Present reports `D3DERR_DEVICELOST` once;
   the game releases its resources (`0x00610590`) and resets with its stored parameters (`0x00610AB0`, below), which
   the Reset hook rewrites. This is the path the game takes after Alt+Tab from exclusive fullscreen. Only a change of
   what Apex sets (`Windowed`, the back-buffer size, the swap effect, the fullscreen refresh rate) resets the device;
   the runtime fills in sizes and formats on return, so the whole structure is never compared.
4. The window's style and position are set on the window's thread, through a message to Apex's window procedure: a
   title-bar window sized to the back buffer (at most the monitor's work area), centred when it becomes a window and
   then left where the player moves it; or a borderless window covering the monitor. *Game setting* puts back the
   style and the rectangle the window had before Apex first changed it. If another program changes the style or the
   size, Apex puts it back (at most 5 times a second, then logs).

**Frame-rate limiter** paces the start of each frame, right after Present on the render thread:

1. The next frame may start no earlier than one interval after the previous frame's planned start. A frame late by
   less than an interval keeps the schedule; later than that, the schedule restarts, so a hitch is never followed by a
   burst of short frames ([`frame_limiter_math.h`](../../features/frame_limiter_math.h)).
2. The wait is a high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, no change of the system timer
   resolution) that ends a little before the deadline, then a `_mm_pause` spin. The margin follows the timer's measured
   lateness (0.25 to 2 ms). The wait is alertable, like the game's own sleeps.
3. The wait happens before the next frame reads input, so it adds no latency beyond the cap itself.
4. Sims3SettingsSetter's Smooth Patch limiter call in the render frame is redirected to an Apex function that keeps
   only the game's 30 ms sleep while the device is lost, while an Apex limit is in force at that moment: a limit set
   only for the background gives the call back whenever the game is in front. The call is looked for only while an
   Apex limit is in force, every 5 s and 6 times at most (Sims3SettingsSetter patches from its own thread after the
   game starts), and read again at every frame: if Sims3SettingsSetter rewrites it, Apex follows; if it holds neither
   target, Apex leaves it alone for the session.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Window mode | `window_mode` | string | `game` | `game`, `windowed`, `borderless`, `fullscreen` | Game setting leaves the game's and other mods' choice. Live (a device reset when `Windowed` or the size changes) |
| Resolution | `width`, `height` | integer | 0, 0 | 0 (game's) or a mode of the device's adapter | Back-buffer size in every mode. Live |
| Frame-rate limit | `fps_limit` | integer | 0 | 0 (off) or 20 to 1000; menu: 30, 60, 120, 144, 165, 240, the monitor's rate minus 3, or Custom (20 to 500) | Live |
| In the background | `background_fps_limit` | integer | 0 | 0 (same limit) or 5 to 1000; menu: 10, 15, 30, 60 | Applies while another window has the focus. Live |
| Show FPS | `show_fps` | boolean | false | | A small white number (the average of the last 240 frames, updated every 30) in the top-left corner, menu open or closed; left out of Apex screenshots. Live |

Settings > Menu > Reset all puts these back to their defaults. The row *Measured* shows the frames per second and the
standard deviation of the frame time over the last 240 frames. The Overview's frame-rate card counts as a hitch a
frame longer than 50 ms and than 1.5 times the frame time of the limit in force (so a 10 fps background limit is not
one), and leaves out frames whose Present failed. While a limit is set, the log has the same once a minute
(`[Screen] Limit ...`). The log also has the settings at start (`[Screen] At start`), every change, each device request
Apex rewrote, the window it applied and the Sims3SettingsSetter overrides.

## Compatibility and interactions

- **Sims3SettingsSetter.** Its borderless window (`[qol.borderless_window]`) is overridden while Apex's window mode is
  not *Game setting* (logged; the menu note shows only while Sims3SettingsSetter is loaded). Its Smooth Patch Precise
  frame limiter is paused while an Apex limit is in force (logged); its tick rate (TPS) and the rest of Smooth Patch
  stay. With its *Tick at most once per frame* on, the simulation waits for its limiter, so Apex leaves it in place
  and says so in the log only. No Sims3SettingsSetter file is written. The menu names Sims3SettingsSetter in one note,
  only while Apex overrides it.
- **RTX Remix bridge and exclusive fullscreen.** Under the bridge, *Fullscreen* runs as *Borderless*
  (`EffectiveMode`), with a menu note: an exclusive-fullscreen Reset hung the game (the bridge logged a Reset unanswered
  after 30 s; a deadlock between the server restyling the game's window and the window thread waiting for the Reset is
  likely, unverified).
- **DXVK and the RTX Remix bridge.** The same hooks apply: the bridge's client is the game's `d3d9.dll`. The bridge's
  `dxvk.conf` needs `d3d9.countLosableResources = False` for a Reset to succeed (exclusive fullscreen switches froze
  the game before it). DXVK's `d3d9.maxFrameRate` and `d3d9.presentInterval`, driver limiters and RTSS cannot be seen
  by Apex: use one limiter.
- **V-Sync / variable refresh.** Apex does not change the presentation interval. With G-SYNC or FreeSync, the menu's
  "monitor rate minus 3" entry keeps the frame rate inside the variable-refresh range.
- **Apex's effects.** They release and recreate their screen resources on every Reset as before.
- A borderless window with a resolution below the monitor's is stretched by the swap chain; the game's mouse
  coordinates follow the window (unverified in game for every resolution).

## Limitations

- Exclusive fullscreen needs a display mode of the chosen size; without one the request stays windowed (logged).
- In windowed mode at the monitor's own resolution the window is kept inside the work area, so the client area is
  smaller than the back buffer and the picture is scaled down; use Borderless.
- The limiter runs on the render thread after Present; frames the game renders without Present (thumbnails, photos)
  are not limited.
- In the background the game's own 10 ms sleep per frame also applies, so a background cap above about 90 fps has
  no effect.

## Technical reference

| Address (Steam 1.67.2) | Role | Use |
|---|---|---|
| `0x0061172C` | Present result check in `0x00611680`: `D3DERR_DEVICELOST` sets `renderer+0x8D`, calls `0x00610590`, then `0x00610AB0(0)` | Live mode change (Apex's Present reports `D3DERR_DEVICELOST` once) |
| `0x00610590` | releases the device listeners' resources (list at `renderer+0xE4`) | game's own |
| `0x00610AB0` | device recovery: `TestCooperativeLevel`, listeners, gamma ramp (fullscreen only), `Reset([renderer+0xE0]+0x14C)`, copies the result to `+0x184` (`+0x1A4` = current `Windowed`) | game's own |
| `0x00EC9FBA` | 9 bytes: Sims3SettingsSetter's `3E 56 E8 rel32 5E EB` (push renderer, `__stdcall` call, the render frame's own `pop esi`) | Apex rewrites only the rel32, on the render thread inside Present, checked against the expected bytes |

The Sims3SettingsSetter site is found by the pattern `3E 56 E8 ?? ?? ?? ?? 5E EB` over the game image and accepted
only when the call lands in Sims3SettingsSetter's module, so the Retail and EA builds are covered without addresses.
Apex's replacement must be `__stdcall`: a `__cdecl` one left the argument on the stack and the render frame returned
to it (crash at the first Present, 2026-11-09).

The schedule and its measurements are tested offline by [`tools/frame_limiter_test`](../../tools/frame_limiter_test).

## Rejected approaches

- Waiting before Present (the 2026-10-02 candidate): even present intervals but up to one interval of extra input
  latency. Replaced by start-of-frame pacing after Present.
- Calling `IDirect3DDevice9::Reset` from Apex: the game's default-pool resources would still exist. The game's own
  device-lost path releases them.
- Editing `S3SS.toml` to turn its borderless window or limiter off: needs a restart and changes another mod's
  settings. Apex overrides them at run time instead.

## See also

- [Validation](../validation/display.md)
- [History](../history/display-fluency.md): the removed 2026-10-02 candidates and this revival
- [Clock, sleeps and frame limiter](../engine/timers-and-sleeps.md)
