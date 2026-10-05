# Presentation and Frame Pacing: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The feature page is
[features/presentation.md](../features/presentation.md).

### 2026-10-02: release candidate `2.5.4-rc-presentation-test`

**Context:** a presentation policy and optional frame-rate limiter in the unified binary, not a renderer replacement or a
driver VRR activation API. Never published; gameplay and monitor validation never ran.

**Settings** (Display > Window, below Borderless):

| UI | TOML `[display]` key | Default | Range | Application |
|---|---|---|---|---|
| V-Sync | `vsync_policy` | 0: Keep current | 0 preserve, 1 On, 2 Off | Next successful device creation or reset; restart recommended |
| Limit FPS with Apex | `frame_pacing` | false | boolean | Live, subject to the Sims3SettingsSetter conflict gate |
| Target frame rate | `target_fps` | 120 | 30 to 240, integer | Live |

The Window profile part included these fields. Legacy profiles without them preserved the current values. Page reset and
Restore synchronization restored policy 0, pacing off, target 120. No other mod's configuration was edited.

**Design** (`features/presentation.cpp`):

- The requested D3D9 `PresentationInterval` was applied independently of borderless ownership. The bootstrap fallback
  kept the game's parameters when device creation or reset failed. The UI compared its request with the successful
  device parameters and showed "restart pending" when they disagreed. These were D3D9 requests only; DXVK and driver
  overrides could supersede them.
- The device Present hook called the optional scheduler after the callback chain and before the original Present,
  outside the registry lock. Disabled pacing returned at once. Enabled pacing used QPC deadlines and a render-thread
  high-resolution waitable timer, with plain waitable timer or `Sleep` fallback; no spin loop, timer-resolution change,
  worker thread, per-frame disk writes or catch-up bursts. Wait chunks were bounded to 4 ms and failures stopped the
  current schedule. Turning it off, focus loss, minimization, configuration changes, successful device creation or reset
  and a failed Present restarted scheduling; long stalls resynchronized instead of replaying deadlines. The target was a
  ceiling. The limiter also affected foreground loading frames; background limiting stayed external.
- The displayed Apex wait and Present values were last-call CPU durations, not GPU scanout, VRR status or percentiles.

**Compatibility gate:** at device creation or reset and at the first installed window procedure, Apex cached whether the
loaded Sims3SettingsSetter had SmoothPatchPrecise enabled with a positive `frameRateLimit` in its configuration. When
detected, Apex's pacing was disabled, including when a profile enabled it. The gate read configuration, not runtime
state; a restart was needed after changing that setting. Its inactive FPS limit and TPS / tick controls were kept.
Driver, RTSS and DXVK limiters could not be detected.

**Local investigation:** Sims3SettingsSetter active FPS 237, inactive FPS 60, TPS 960, DXVK `d3d9.presentInterval=1`;
none of those files was changed by this feature (an earlier, separately authorized window handoff changed only the two
borderless mode keys). DXVK can force V-Sync regardless of the Apex request. The candidate did not claim to remove OLED
flicker.

**Planned verification (never run):** in-game A/B of the previous candidate against this one with defaults, then one
limiter at a sustainable target; original D3D9 and DXVK; focus and Alt+Tab; load screens; reset and device loss; menu
open and closed; profile import and reset; V-Sync requests with and without renderer overrides; frame-time percentiles
and visible flicker rather than average FPS.

### 2026-10-02: folded into Display and Fluency

**Outcome:** the settings moved into [Display and Fluency](display-fluency.md) (`2.5.4-rc-display-fluency`).

### 2026-10-02: removed

**Outcome:** removed together with Display and Fluency by maintainer decision before any release. Apex forwards the
game's original CreateDevice, Reset and Present requests without changing synchronization or window state.
