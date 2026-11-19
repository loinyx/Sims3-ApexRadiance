# Fewer Mouse Picks When Still

The game works out which object is under the mouse every other frame, even when neither the camera nor the mouse moves.
Under the RTX Remix bridge each of those checks costs about 2 ms of the render thread. With this switch on, the check
waits while nothing has changed (at most 0.25 s), and runs every 4th frame while the camera moves. A click made while
the check waits is held until a new check is done (about 3 frames), so it never hits what was under the mouse earlier.
Key presses are never delayed. A highlight on an object that moves under a still cursor (a walking Sim) may follow up
to 0.25 s late.

## Status

| | |
|---|---|
| Availability | Released in 4.0.7 |
| Default | On |
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
   and the eye): skip the two updates after a read-back, then pick, mouse-move requests included. That is every 4th
   update when the read-back comes one update after the pick; a deferred read-back that waits for the GPU (under the
   RTX Remix bridge often 2 to 3 updates) adds its waits. Counting from the pick instead let those waits use up the
   interval, so nothing was skipped while the camera moved.
4. **A mouse-move request with the camera unchanged:** pick now, as the game does.
5. **A re-pick with nothing changed:** skip it until the last pick is 250 ms old.

A skipped update returns before the game's code runs. Nothing else happens in the update (no timers or listeners), the
request slot keeps the latest request until a render takes it, and ResolvePick (0x006E01B0, called separately just
before the update) keeps storing the last result, so every reader (hover, clicks, drag and drop, Build/Buy, Create a
Sim) sees the last pick. A pending read-back is never skipped.

### Clicks wait for a fresh pick

Clicks reach the game as window messages (`Canvas_WndProc` -> `UIInput_ToWindowMessage` 0x005989C0, which sets the pick
request through 0x006DEFF0 and posts a UI message), and the click's handler reads the stored result. With the skip, that
result can be up to 250 ms old (the game's is about 2 frames old), so a click on a walking Sim could hit what was under
the cursor then. So a button press that would reach the game while a pick was skipped since the stored one, and the
stored one is over 50 ms old, is held in Apex's window procedure:

1. The press counts as an input event, so the next update renders a pick (rule 1 above).
2. The update after it reads that pick back at once, waiting for the GPU as the game does (no deferral).
3. ResolvePick stores the result at the start of the next frame's pick work; the update that follows posts the held
   mouse messages back in their order, each as a private registered message carrying its number, and Apex's window
   procedure hands each to the game's as it was held, without Apex's input handling. A mouse message another program
   posts meanwhile is new input, never taken for a held one.

Mouse messages after a held press (moves, the release) are held too, so their order stays; key messages are not held.
A hold ends after 200 ms at most, when no pick can come (no resolved cursor position, picking off), and when the switch
is turned off. Held presses are counted (`pick_click_holds`, the developer log line).

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Fewer mouse picks when still | `[patches.PickSkipWhenStill] enabled` | bool | on | | Turns the skip and the motion cap on. Off: the game's own cadence (with the read-back deferral) |

## Compatibility and interactions

- Uses the hook of the always-on pick read-back deferral; turning the switch off leaves only the deferral.
- Needs Apex's window procedure to tell a click from a mouse move and to hold clicks. Before it is installed, every
  request picks.
- Not part of the Overview > Performance group switch.

## Limitations

- A click after a still moment reaches the game about 3 frames later than without the switch (its pick is then newer
  than the game's own). Unverified in game: the frame count, and that a reposted message behaves like the original in
  every screen (the message time and position Windows reports for it are those of the repost).
- A key pressed during a held click can reach the game before that click.
- Keys are not held: a key that acts on the object under the cursor (a Build/Buy shortcut) uses the stored pick, up to
  0.25 s old with the camera and mouse still.
- While the camera moves, hover highlights follow every 4th frame instead of every other frame (later when the
  read-back waits for the GPU).
- Whether any byte of the compared camera fields changes every frame with a still camera is unverified; if one does,
  the skip never fires (the frame profiler's `pick_skip_still` stays 0) and the game's cadence remains.

## Technical reference

- Developer builds log every 10 s while the switch is on: `[PickReadback] Last 10 s: N picks, S skipped with nothing
  changed, M skipped while the camera moved, R read-backs X ms, H clicks held for a new pick (T ms, O timed out), C picked for a mode or viewport change`.
- Frame timeline columns `pick_runs`, `pick_skip_still`, `pick_skip_moving`, `pick_readbacks`, `pick_readback_ms`,
  `pick_click_holds` ([Frame Profiler](../frame-profiler.md)).
- The hold: `HoldClick` and `ReleaseHold` in `features/pick_readback.cpp`; `Overlay::SetMouseHoldGate` and
  `Overlay::ReleaseHeldMouse` in `framework/overlay.cpp`.

## See also

- [Performance](README.md)
- [Frame Profiler](../frame-profiler.md)
