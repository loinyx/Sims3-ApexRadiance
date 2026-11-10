# Display: validation

Checks of [Display](../features/display.md): window mode, resolution and frame-rate limiter.

## Offline

| Check | Result |
|---|---|
| `tools/frame_limiter_test/run.ps1`: steady cap, slightly late frame, 100 ms hitch, cap raised and lowered, off, measured spread, spin margin | 13 checks pass (2026-11-09) |
| `tools/i18n_check` | OK, 2520 / 2520 keys in every language (2026-11-09) |
| `perl tools/menu_audit.pl .` | no missing entry for the new texts; one note over 90 characters (the rare *Tick at most once per frame* warning, removed from the menu since) |
| Release 3.1.0 (commit `release: 3.1.0`, 2026-11-10): Release x86 build, `tools/frame_limiter_test`, `tools/i18n_check` (2583 keys), `perl tools/menu_audit.pl .` | Build clean; all limiter checks pass; i18n OK. The release candidate (ASI SHA-256 `AA8AFADD…`) was played and approved in game under the RTX Remix bridge; the review fixes after it (the Custom limit choice, the fullscreen log line under the bridge) were not played |
| Review fixes (S3SS site looked for while a limit is in force and read every frame, the limit in force for the redirect, window position kept, *Game setting* rectangle, hitch threshold, reset compare, adapter modes): `features/screen_control.cpp` compiles, `tools/frame_limiter_test` all pass | 2026-11-09, uncommitted; not run in game |

| Resolution through the game (2026-11-10): the bytes of `0x0077D1A0`, `0x0060E2C0` and `0x007F1B70` checked against `TS3W.exe` (Steam 1.67.2); Release x86 build | Bytes match; build clean; not run in game |

## In game

Steam 1.67.2, 3840x2160 at 240 Hz, RTX Remix bridge installed, Sims3SettingsSetter 1.6.3 with Smooth Patch Precise
(165 fps) and borderless fullscreen. Game unfocused (main menu).

| Check | Result |
|---|---|
| Sims3SettingsSetter's limiter call found and redirected | found at `0x00EC9FBA`; paused at the first Present (log) |
| First build: replacement declared `__cdecl` | crash at the first Present (EIP `0x45070000`, return to `0x00ECAB01`); fixed with `__stdcall` |
| `window_mode = "windowed"` over Sims3SettingsSetter's borderless window | window style `0x14CA0000` (caption, no frame resize), client 3840x2160; Sims3SettingsSetter reapplied its borderless window once and Apex put its own back, no further changes |
| 120 fps limit, game in the background | 93.6 fps (the game's own 10 ms background sleep is the cap), frame time varies by 0.24 ms, timer late by up to 0.35 ms |

## Pending

- Focused game: 60 / 120 / 165 / 237 fps caps, measured rate and variation.
- Live changes from the menu: Window, Borderless, Fullscreen (240 Hz) and back to Game setting; a resolution below
  the monitor's (image and mouse).
- The same under in-process DXVK (bridge removed) and plain Direct3D 9.
- Alt+Tab and minimise in exclusive fullscreen.
- Window: drag the title-bar window (it stays where it is dropped, no "Another program keeps changing" warning);
  Borderless then *Game setting* (the game's own size and place come back).
- A background-only limit with Sims3SettingsSetter's limit set: its limit in front, Apex's in the background (log).
- Turning on a limit some seconds after start with Sims3SettingsSetter: its call found on a later look (log).
- Game resolution below the Apex one (`Options.ini` at `1024 768`, Apex Borderless at 3840x2160): one
  `[Screen] Game resolution` line, a game Reset at the new size, a full sharp picture, UI and picking right at the
  corners, `Options.ini` now `3840 2160`; after a restart no request line. The same in Window, Fullscreen and *Game
  setting* (game in fullscreen), under the RTX Remix bridge with Fullscreen (runs as Borderless), and with
  Sims3SettingsSetter's borderless window on.
- Sizes already matching: the log has no request line and the device requests are unchanged from 3.1.0.
- A resolution chosen live in the main menu, Live mode, CAS and during a loading screen.
- A size the game does not list: one warning, the picture stays full (scaled).
- Show FPS and the Custom slider do not reset the device (no "Display change" line in the log).
