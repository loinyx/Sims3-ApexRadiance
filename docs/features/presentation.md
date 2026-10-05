# Presentation and Frame Pacing

A V-Sync request policy and an optional Apex frame-rate limiter. The feature existed only in a private release
candidate, was folded into [Display and Fluency](display-fluency.md) and removed with it before any release. Apex
Radiance does not change the game's presentation interval and has no frame-rate limiter.

## Status

| | |
|---|---|
| Availability | Removed (private release candidate `2.5.4-rc-presentation-test`, 2026-10-02; never published) |
| Default | Not applicable |
| Menu | None |
| Configuration | None. Legacy `[display]` keys (`vsync_policy`, `frame_pacing`, `target_fps`) are ignored and dropped from profiles |
| Source | None in this repository |

## See also

- [History](../history/presentation.md): the release candidate's settings, scheduler design and compatibility gate.
- [Display and Fluency](display-fluency.md)
