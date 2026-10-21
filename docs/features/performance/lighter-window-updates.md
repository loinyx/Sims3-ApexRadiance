# Lighter Window Updates

Every frame the game asked Windows to repaint its own window, although the picture comes from the graphics card. Each
request sent a paint message through every window handler for nothing. Lighter Window Updates lets Windows repaint the
window only when it needs to, when the window is uncovered or resized. Nothing on screen changes.

## Status

| | |
|---|---|
| Availability | Released in 2.7.0 (Experimental until 2.15.0) |
| Default | On |
| Menu | System > Performance > Game and scripts > *Lighter window updates* |
| Configuration | `[patches.WindowRepaint]` in `ApexRadiance.toml` |
| Source | [`features/window_repaint.{h,cpp}`](../../../features/window_repaint.cpp), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) (`WindowRepaintPatch`) |

## The problem

The game's window message pump (service 0x00588A00, window vfunc +0x8C = 0x00410890) starts every frame with
`if (flags & 8 && !paintSuspended) { [+0x1C] = 1; InvalidateRect(hwnd, 0, 0); }` (0x004108A0..0x004108B6; the window is
created with flags 0x2A). The invalidation makes Windows send a `WM_PAINT` every frame. It goes through every window
procedure in the chain (overlays and other mods included), the game's paint handler (0x004109A0: `BeginPaint`,
`MonitorFromWindow`, `EnumDisplayMonitors` with a `FillRect` of the other monitors, `EndPaint`) and a paint event
0x1EE100A that no code listens for. The picture itself comes from the Direct3D swap chain, not from `WM_PAINT`.

## How Apex Radiance solves it

The short jump over the `InvalidateRect` call at 0x004108AE (`75 0C`, `jne`) becomes unconditional (`EB 0C`). The
paints Windows sends by itself (uncovering, resizing) are handled exactly as before, and `[+0x1C]` is still set.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Lighter window updates | `[patches.WindowRepaint] enabled` | bool | on | | Skips the per-frame repaint request. A missing key reads as on |

The row is part of the Overview's Performance group switch. It is not in the Performance part of profiles: applying a
profile leaves it as it is.

## Compatibility and interactions

- Tools that draw over the game window through `WM_PAINT` instead of the swap chain would no longer be called every
  frame (none known).
- **Game build:** found by signature (`WindowRepaintJump`), so it applies on any build where the pattern matches; the
  bytes are checked before the write.

## Limitations

- The gain is small: one message per frame through the window procedures; it has not been measured in frames.

## Technical reference

- Signature `F6 46 08 08 57 74 ?? 39 5E 20 C6 46 1C 01 75 ?? 8B 46 70 53 53 50 FF 15`, the jump at +14 (0x004108AE on
  Steam 1.67.2). Start checks `75 0C` followed by `8B 46 70 53 53 50 FF 15` (`mov eax,[esi+70h]; push ebx; push ebx; push
  eax; call [InvalidateRect]`); otherwise "the game code differs" and nothing is written.
- Stop writes `75` back. Address group `WindowRepaint`.
- Log: `[WindowRepaint] On: the game no longer invalidates its window every frame (0x004108ae)`.

## Rejected approaches

None recorded.

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-lighter-window-updates.md)
- [History](../../history/performance-lighter-window-updates.md)
