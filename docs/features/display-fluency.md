# Display and Fluency

Window mode, monitor selection, an Apex frame-rate limiter, V-Sync requests and read-only G-SYNC / FreeSync reports. The
feature existed only in a private release candidate and was removed before any release. Apex Radiance has no window,
monitor, V-Sync or FPS controls: it forwards the game's own CreateDevice, Reset and Present requests without changing
synchronization or window state. G-SYNC and FreeSync stay under the driver's and the monitor's control.

## Status

| | |
|---|---|
| Availability | Removed (private release candidate `2.5.4-rc-display-fluency`, 2026-10-02; never published) |
| Default | Not applicable |
| Menu | None |
| Configuration | None. A legacy `[display]` table in a profile is ignored and dropped on import or export (`KeepProfileParts`); profile part index 4 (the former Window part) has no name |
| Source | None in this repository |

## Compatibility and interactions

- Window mode, borderless and frame-rate limits are left to the game, Sims3SettingsSetter, DXVK or the driver. Use one
  frame-rate limiter only.
- Active VRR in this game is not asserted by Apex Radiance.

## See also

- [History](../history/display-fluency.md): the release candidate's design, settings, VRR queries, ownership rules and
  local evidence.
- [Presentation](presentation.md): the earlier V-Sync and frame-pacing candidate it extended.
