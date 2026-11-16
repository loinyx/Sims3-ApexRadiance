# Per-draw light cache

An experiment to cut the render thread's time in the per-draw lighting fixes while the camera pans, with the lighting
unchanged. Nothing changes on screen.

## Status

| | |
|---|---|
| Availability | In development, Developer builds only |
| Default | Off |
| Menu | None |
| Configuration | `[dev_build]` in `ApexRadiance.toml`: `draw_light_cache`, `lazy_shader_restore`, `draw_light_cache_check` (each off unless set) |
| Source | [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) (`ObjDrawMemoFor`, `FixLaps`), [`framework/d3d9_hooks.cpp`](../../../framework/d3d9_hooks.cpp) (`DeferShaderRestore`) |

## The problem

Zoomed out, the lighting fixes spend about 3.3-3.8 ms a frame of their own time on the render thread: 1-12 µs a draw
over hundreds of draws. The largest rows are the room maps, several fixes on one draw, fences and outdoor objects. The
lamp picks are already kept (`lamp_visibility_memo`), so the rest is spread over the work each fix repeats per draw.

## How Apex Radiance solves it

1. **Finer draw cost log.** The timed draws (one in 16) now split the outdoor object and room map fixes into parts, and
   the "several fixes" row by the set of fixes on the draw. This shows where the rest of the time goes.
2. **Outdoor object draw cache** (`draw_light_cache`). An outdoor object's lamp pick, ground share, highest lamp head,
   ground pool gate and its point on a wall line are kept by its world matrix and the reach law. Camera pans move none of
   their inputs, so the same objects find their answers frame after frame. The shaders and textures are not in the key:
   none of the kept values reads them. An entry is valid while the lamp list, the walls, slabs and switches the lamp test
   reads, and the lots are unchanged; lamp switches and edits change the lamp list or the walls and so end it.
   `NoteGate` still runs every draw, since it keeps a gate's fence gap closed.
3. **Lazy shader restore** (`lazy_shader_restore`). After an outdoor object draw the patched shader pair stays bound.
   The game's pair goes back before the next draw that reaches the device, a Present, a BeginScene or EndScene, or
   before code that reads the device's shaders to put them back later (the end-of-scene passes, state blocks, light
   captures). A game `Set*Shader` replaces it. The next outdoor object draw of the same game pair finds its own pair
   still bound and sets nothing.
4. **Check mode** (`draw_light_cache_check`). Every cached answer is computed again and compared bit for bit, and every
   reused shader pair is read back from the device. A difference falls back to the computed path and is counted; the
   first cache difference is logged.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| None | `[dev_build] draw_light_cache` | bool | false | | The outdoor object draw cache |
| None | `[dev_build] lazy_shader_restore` | bool | false | | The lazy shader restore |
| None | `[dev_build] draw_light_cache_check` | bool | false | | Both paths computed and compared (costs more than either saves) |

## Compatibility and interactions

- The cache is not used where the pick memo is not: after the pick memo turned itself off, while F8 records tests, or
  during a light capture.
- `BoundPixelShader` and `BoundVertexShader` answer the game's pair while its restore is pending.

## Limitations

- Unverified in game: the gain (estimated at 1-2 ms a frame zoomed out with both parts) and the check counters.
- Draws that bypass the hooked `DrawPrimitive` and `DrawIndexedPrimitive` (the UP draws) and state blocks applied
  without a capture point listed above are not covered by the lazy restore.
- The pick memo's development counters (lamps left out, dimmed) are not added again for cached draws.

## Technical reference

The draw cost line (`[LotLightBridge] Draw cost by fix`, every 10 s) adds:

- `of it the <part>: N a frame, X us each` for the outdoor object parts (atlas and copies, world matrix and pick, sets,
  shaders, restore) and the room map parts (copies and caps, map constants, arch rows, sets and restore); the draw's
  driver time is left out of its part;
- `several fixes A+B: N draws a frame, X us each` for the three costliest sets;
- with either switch on: `object draw cache: reused, computed, not used, checked, differed` and
  `lazy shader restore: left bound, reused, put back, checked, differed` since the last line.

## Rejected approaches

- Skipping unchanged constant uploads, and a constants shadow: the Remix bridge already drops repeated `Set*` calls on
  the client side and answers `Get*` locally.

## See also

- [Performance](README.md)
