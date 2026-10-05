# Developer mode: validation

The feature is described in [features/developer-mode.md](../features/developer-mode.md). It must:

- Start in normal mode when `[ui] developer_mode` is missing or false, with no developer instrument running.
- Change mode only after a confirmed request and a restart.
- Keep profile part bits stable, filter the Developer part correctly and never start a measurement from a profile.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/developer_mode_check`](../../tools/developer_mode_check/check.cpp) | Default-off UI setting and runtime gate, profile part bits (`kPartDeveloper = 256`, `kPartAmbientOcclusion = 128`, bit 4 unused), developer-only / normal / empty part filtering, old profiles without the part, retired `[display]` entries ignored, atomic gate | `python tools/developer_mode_check/prepare.py` extracts the profile functions from `apex_config.cpp`, then compile `check.cpp` with the printed folder on the include path | x86 MSVC, toml++ |

Offline checks do not validate game performance or menu layout.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-02 | Unified build | `developer_mode_check` | PASS: default-off mode, legacy profile bits, developer-only/normal/empty profile filtering and atomic gate | None |
| 2026-10-02 | Unified build | Release build of `ApexRadiance.sln` | Passed; the legacy `ApexPublic` property selects the same output and definitions | MSBuild |

## In-game test plan

1. Fresh `ApexRadiance.toml`. **Expected:** log `[Main] Unified build: normal mode`; no Developer page; no Frame Capture
   row in Settings > Shortcuts.
2. Settings > Menu > Enable developer mode, then Cancel. **Expected:** the switch stays off.
3. Enable and confirm. **Expected:** the note "Restart the game to apply the developer mode change"; after a restart,
   the log says developer mode, the Developer page appears and About reads "Version X - Developer mode".
4. Save a profile with Development checked; turn developer mode off and restart; apply the profile with Development
   checked. **Expected:** the confirmation opens before anything is applied; Cancel applies nothing.
5. In developer mode, start the frame profiler, save a profile, restart and apply it. **Expected:** the profiler is not
   running.
6. Turn developer mode off with an Edge Smoothing debug view saved on. **Expected:** after a restart the normal image is
   shown.

## Confirmed in game

- Nothing recorded yet.

## Open checks

- Confirmation layout, restart behaviour and profile round trips in game.
- Performance in normal mode compared with the former public build.
- Developer page hierarchy: full visual and diagnostic validation in game (native UI checks do not establish it).
