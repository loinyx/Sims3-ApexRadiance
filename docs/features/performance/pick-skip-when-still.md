# Fewer Mouse Picks When Still

The game works out which object is under the mouse every other frame, even when neither the camera nor the mouse moves.
Under the RTX Remix bridge each of those checks costs about 2 ms of the render thread. With this switch on, the check
waits while nothing has changed (at most 0.25 s), and runs every 4th frame while the camera moves. Clicks and key
presses are never delayed. A highlight on an object that moves under a still cursor (a walking Sim) may follow up to
0.25 s late.

## Status

| | |
|---|---|
| Availability | In development (Experimental) |
| Default | Off |
| Menu | System > Performance > Camera and lighting > *Fewer mouse picks when still* |
| Configuration | `[patches.PickSkipWhenStill]` in `ApexRadiance.toml` |
| Source | [`features/pick_readback.{h,cpp}`](../../../features/pick_readback.cpp), [`framework/overlay.cpp`](../../../framework/overlay.cpp) (`InputEvents`), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

The pick manager (`[scene+0x38]`) renders a small pick target and reads it back with `GetRenderTargetData` (see the
[Frame Profiler](../frame-profiler.md) GetRenderTargetData section). Its update 0x006DFEC0 (Steam 1.67.2) renders a new
pick when a request waits (`[mgr+0x38] >= 0`, set by the input handler 0x005989C0 on mouse moves and buttons) or, with
no request, when the camera's byte `+0x6FD` is set and a cursor position was resolved (`[mgr+0x48] > 0`): a re-pick at
the last position. The camera controller writes `+0x6FD` every frame from 0x00972510, which always returns 1
(0x0096BE55). So the game picks every other frame (render, then read back) whatever the camera and mouse do. In a
10 s camera pan under the bridge that was 544 to 562 read-backs, 1.35 to 1.40 s of render-thread time; the always-on
read-back deferral of 4.0.2 halves the count.

## How Apex Radiance solves it

The hook on 0x006DFEC0 that defers the read-back also decides, before a new pick would be rendered (`[mgr+0x14]`
clear, `[mgr+0x15]` clear and one of the two conditions above true):

1. **Something the player did:** a button, wheel or key press or release, or a focus change since the last pick
   (counted in Apex's window procedure; key auto-repeat and mouse moves are not counted): pick now.
2. **The pick modes or the viewport changed** (`[mgr+0x18..0x37]`, `[mgr+0x101..0x104]`): pick now.
3. **The camera changed** since the last pick (camera `+0x0C..+0x1B`, `+0x30..+0x6F`: the projection scalars, the axes
   and the eye): pick at most every 4th update, mouse-move requests included.
4. **A mouse-move request with the camera unchanged:** pick now, as the game does.
5. **A re-pick with nothing changed:** skip it until the last pick is 250 ms old.

A skipped update returns before the game's code runs. Nothing else happens in the update (no timers or listeners), the
request slot keeps the latest request until a render takes it, and ResolvePick (0x006E01B0, called separately just
before the update) keeps storing the last result, so every reader (hover, clicks, drag and drop, Build/Buy, Create a
Sim) sees the last pick. A pending read-back is never skipped.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Fewer mouse picks when still (Experimental) | `[patches.PickSkipWhenStill] enabled` | bool | off | | Turns the skip and the motion cap on. Off: the game's own cadence (with the read-back deferral) |

## Compatibility and interactions

- Uses the hook of the always-on pick read-back deferral; turning the switch off leaves only the deferral.
- Needs Apex's window procedure to tell a click from a mouse move. Before it is installed, every request picks.
- Not part of the Overview > Performance group switch.

## Limitations

- A click reads the last stored pick, as in the game. With the mouse held still over a moving object, that pick can be
  up to 250 ms old (the game's is about 2 frames old), so a click on a walking Sim can hit what was under the cursor
  then. Unverified in game.
- While the camera moves, hover highlights follow every 4th frame instead of every other frame.
- Whether any byte of the compared camera fields changes every frame with a still camera is unverified; if one does,
  the skip never fires (the frame profiler's `pick_skip_still` stays 0) and the game's cadence remains.

## Technical reference

- Developer builds log every 10 s while the switch is on: `[PickReadback] Last 10 s: N picks, S skipped with nothing
  changed, M skipped while the camera moved, R read-backs X ms`.
- Frame timeline columns `pick_runs`, `pick_skip_still`, `pick_skip_moving`, `pick_readbacks`, `pick_readback_ms`
  ([Frame Profiler](../frame-profiler.md)).

## See also

- [Performance](README.md)
- [Frame Profiler](../frame-profiler.md)
