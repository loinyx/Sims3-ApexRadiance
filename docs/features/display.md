# Display

Window mode, resolution and a frame-rate limiter, in System > Display. The game can run in a window, borderless over
the whole monitor, or in exclusive fullscreen at the monitor's highest refresh rate, at the game's resolution or a
chosen one, which Apex sets through the game's own resolution change. The limiter caps the frame rate evenly, with a separate cap while another window is in front, and shows
the measured rate and how much the frame time varies; an optional counter shows the frame rate in a corner of the screen. Both take precedence over Sims3SettingsSetter's borderless
window and frame limiter when that mod is installed.

## Status

| | |
|---|---|
| Availability | Released in 3.1.0. Resolution through the game's own resolution change (every build, EA app 1.69 included) and the game's exclusive fullscreen run as Borderless under the RTX Remix bridge: Released in 3.1.1 |
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
   first (the usual order: Apex sees the parameters last). Apex rewrites the request: `Windowed` and, in exclusive
   fullscreen, the highest refresh rate the adapter lists for the game's size (`EnumAdapterModes`). The back-buffer size
   stays the game's: the game lays out its UI, picking and input at its own resolution (`renderer+0x2C/+0x30`).
2. **Resolution** changes the game's own resolution. After a Present, while the game's size differs from the chosen
   one, Apex calls the game's resolution request (`0x0077D1A0` on Steam 1.67.2, found by signature on other versions:
   group `GameResolution`), which queues a message for the game's main loop. The
   game checks the size against its mode list, stores it, saves `Options.ini` and resets its device through its own
   path. In the game's exclusive fullscreen Apex passes the highest refresh rate of the new size. The request is sent
   again every 10 s while the game still has its old size and no Reset of its own is pending (`renderer+0x8E`; a
   message sent before the game's UI listens is lost), six times at most, then logged. The Apex setting takes
   precedence: a resolution later picked in the game's own Options is asked back, as at every start. A size below
   1024x768 (the game would snap it up to another mode) is not sent: the picture is scaled to the window (logged). *Game setting*
   never touches `Options.ini`.
3. If the device refuses the rewritten request, the call is made again with the game's own parameters and the menu
   says so.
4. A change while the game runs uses the game's own device-lost path: Apex's Present reports `D3DERR_DEVICELOST` once;
   the game releases its resources (`0x00610590`) and resets with its stored parameters (`0x00610AB0`, below), which
   the Reset hook rewrites. This is the path the game takes after Alt+Tab from exclusive fullscreen. Only a change of
   what Apex sets (`Windowed`, the swap effect, the fullscreen refresh rate) resets the device;
   the runtime fills in sizes and formats on return, so the whole structure is never compared.
5. The window's style and position are set on the window's thread, through a message to Apex's window procedure: a
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
| Resolution | `width`, `height` | integer | 0, 0 | 0 (game's) or a mode of the device's adapter, at least 1024x768 | The game's own resolution (saved in its `Options.ini`); only sizes in the game's mode list. Live (the game resets its device) |
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
- **RTX Remix bridge and exclusive fullscreen.** Under the bridge, *Fullscreen*, and *Game setting* while the game
  itself asks for exclusive fullscreen, run as *Borderless* (`EffectiveMode`), with a menu note: an exclusive-fullscreen
  Reset hung the game (the bridge logged a Reset unanswered after 30 s), and so did the first device creation when the
  game asked for fullscreen with no `Options.ini` (EA 1.69: the server finished creating the device only after the
  game had exited). The likely cause is a deadlock between the server restyling the game's window and the window
  thread waiting for its answer.
- **DXVK and the RTX Remix bridge.** The same hooks apply: the bridge's client is the game's `d3d9.dll`. The bridge's
  `dxvk.conf` needs `d3d9.countLosableResources = False` for a Reset to succeed (exclusive fullscreen switches froze
  the game before it). DXVK's `d3d9.maxFrameRate` and `d3d9.presentInterval`, driver limiters and RTSS cannot be seen
  by Apex: use one limiter.
- **V-Sync / variable refresh.** Apex does not change the presentation interval. With G-SYNC or FreeSync, the menu's
  "monitor rate minus 3" entry keeps the frame rate inside the variable-refresh range.
- **Apex's effects.** They release and recreate their screen resources on every Reset as before.
- A borderless window with a resolution below the monitor's is stretched by the swap chain; the game's mouse
  coordinates follow the window (unverified in game for every resolution). Until the game has switched to the chosen
  resolution, and when it refuses it, windowed and borderless stretch its picture the same way.

## Limitations

- Exclusive fullscreen needs a display mode of the chosen size; without one the request stays windowed (logged).
- A resolution that is not in the game's mode list (an exact size at 1024x768 and above) is refused by the game: its
  own resolution stays and the picture is scaled to the window (logged).
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
| `0x0077D1A0` | `cdecl (w, h, hz, fullscreen)`: queues message `0x07983246` on the messenger (`0x0044C470`; vtable `0x00FA1AF8`, `+0x1C` = `0x0044CEE0`, inserts under its lock) | Apex's resolution request, on the render thread after Present; checked by its first 6 bytes |
| `0x007F1B70` | handler of `0x07983246`: a field of -1 takes the current value (`0x0060E6A0`), hz 0 when not fullscreen, then `0x007F19E0` | game's own; checked by its first 12 bytes |
| `0x007F19E0` | resolution change: `0x007F15C0` (UI `[0x011D8C48]`), `0x0058E510`, `0x0058E390`, `0x0060E550`, `0x0060E520(!fullscreen)`, `0x0060FFE0`, input size at `[0x011D8AB0]+0x10/+0x14` | game's own; does nothing when `0x0058E510` fails |
| `0x0058E510` | DeviceConfig (`[0x011CDD10]`, getter `0x0058E3F0`) mode check: exact size (and hz when not 0) at 1024x768 and above; below that, snaps up to the smallest mode at least that large | why the menu lists sizes from 1024x768 |
| `0x0060E550` / `0x0060E520` | set the size and hz / `renderer+0x24` (windowed); `renderer+0x8E` (reset wanted) only on a change while the device (`+0xC`) exists | game's own |
| `0x006115C0` | copies `renderer+0x2C/+0x30/+0x24/+0x34` into the stored parameters (`[renderer+0xE0]+0x14C`), then `0x00610AB0(1)` | game's own Reset, rewritten by Apex's hook |
| `0x00610BF8` | in `0x00610AB0`: a windowed request gets refresh rate 0 | game's own |
| `0x00610DFE` | after that Reset: clears `+0x8E`, posts `0x76CBB23D` (handler `0x007F1C03` resizes the window and the UI) | game's own |
| `0x0060E2C0` | returns the renderer `[0x011CECC8]` | read by Apex; checked by its 6 bytes |
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
- Overriding the back-buffer size in Apex's `CreateDevice`/`Reset` rewrite (3.1.0): the game keeps drawing at its own
  resolution, so with `Options.ini` reset to 1024x768 and Apex borderless at 3840x2160 the whole picture, 3D view and
  UI, sat in the top-left 1024x768 of the back buffer (`[Screen] CreateDevice: 1024x768 windowed -> 3840x2160
  windowed`). Replaced by the game's own resolution change.
- Editing `S3SS.toml` to turn its borderless window or limiter off: needs a restart and changes another mod's
  settings. Apex overrides them at run time instead.

## See also

- [Validation](../validation/display.md)
- [History](../history/display-fluency.md): the removed 2026-10-02 candidates and this revival
- [Clock, sleeps and frame limiter](../engine/timers-and-sleeps.md)
