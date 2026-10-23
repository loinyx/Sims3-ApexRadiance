# Tester tab

The Tester tab switches, one by one and while the game runs, the lighting parts, fixes, lamp drag measures and
per-frame caches added in 2.13.0. The same scene and the same lamp drag can then be compared with a part on
and off, for frame time and for look, without another build.

## Status

| | |
|---|---|
| Availability | Developer mode, since 2.13.0 |
| Default | Every switch on: the normal build, Lamp lighting on Instant (its default). *Restore defaults* turns every switch on again, the player options shown here included |
| Menu | Developer > Tester |
| Configuration | `[developer] controls` table `tester` (one boolean per switch, below); the player options shown keep their own keys |
| Source | [`features/tester_toggles.h`](../../../features/tester_toggles.h), [`features/tester_settings.h`](../../../features/tester_settings.h), [`features/tester_toggles.cpp`](../../../features/tester_toggles.cpp), [`features/perf_toggles.h`](../../../features/perf_toggles.h), [`apex_gui.cpp`](../../../apex_gui.cpp) (`DevTesterTab`) |

## Cards

| Card | Switch | TOML key in `tester` | What turns off |
|---|---|---|---|
| Frame time | (readout) | | Frame time now; the average and worst frame and the frame count of the last lamp drag (`DragLite`); GPU time of the last lamp edit (Instant; was the Lamp lighting card's chip); Instant (GPU) kept data: MB kept / the active lot's ceiling (rooms) (`OutdoorDelta::KeptBytes`); Restore defaults |
| Lamp light features | Walls shade ground and curbs | `ground_shade` | Sub-part of the player option *Walls shade lamp light near the ground* (`paredesSombreiamOChao`): the lamp shade mask on lot grass, story-0 outdoor floors, fences, curbs and stairs |
| | Buildings block light on trees and objects | `rig_building_block` | Sub-part of the player option *Buildings block lamp light on trees and objects* (`paredesBloqueiamLuzNosObjetos`): the outside walls and slabs test on outdoor rig objects and foliage |
| | Outdoor light through openings | (player option `luzDeForaPelasAberturas`) | |
| | Indoor light through openings | (player option `luzInternaPelasAberturas`) | |
| | Roofed rooms take lamps from the story below | `roofed_from_below` | Sub-part of *Outdoor light through openings*: a roofed room taking room 0's lamps of the story below |
| | Doors block light | (player option `portasBloqueiamLuz`) | |
| | Real-time outdoor lamps | (player option `lampLightingMode`) | Instant when on, Classic when off |
| Fixes | Disabled lamps fix | `disabled_lamp_fix` | The enabled flag test of the object rig boost |
| | Foliage by day fix | `foliage_day_fix` | The daytime curve on foliage lamp colours (the factor is 1) |
| | Colonnades on a foundation fix | `colonnade_fix` | `CapLowerWalls` in the story grids |
| | Lamps through arches on indoor objects | `opening_lamp_cut` | An object inside an arcade keeping a garden lamp under the game's cut |
| Soft light | Soft edges from the lamp's size | `penumbra` | Sub-part of *Shadow softness*: the penumbra of openings, wall ends and balcony edges |
| | Smooth light maps | (player option `mapasDosComodosSuaves`) | |
| | Fade at the light cut, Dither the light maps, Smooth light map reads | `smooth_cut`, `smooth_dither`, `smooth_bspline` | Sub-parts of *Smooth light maps* |
| | Soft ground shade | `ground_soft_edges` | Sub-part of *Walls shade lamp light near the ground*: soft edges at wall ends and tops |
| While a lamp is dragged | Classic: relight only after release | `classic_after_release` | Classic lamp lighting only: no room solve while a lamp is dragged, every room of the drag solved once after the release ([lamp-drags](../night-lighting/lamp-drags.md#classic-relight-only-after-release)) |
| | Classic: show all floors together | `classic_together` | Classic lamp lighting only: after an edit the lamp's own story waits with the others and every story changes in one frame ([room-light-queue](../performance/room-light-queue.md#classic-show-all-floors-together)) |
| | Defer extra room sends | `drag_defer_sends` | `DragLite::DeferSend` |
| | Hold rooms solved moments ago | `drag_repick_throttle` | `DragLite::HoldRepick` and the priority call redirect |
| | Spread the update after the stop | `drag_spread_cap` | The 3.5 ms cap after a drag (`DragLite::SolveCapMs`) |
| | Keep the terrain rebuild idle | `drag_terrain_idle` | The terrain countdown reset at night |
| | Slower lamp refresh while dragging | `drag_slow_refresh` | The 250 ms lamp list and local terrain pace |
| | Fast own-room relight after edits | `fast_own_relight` | The lamp's own rooms keep the edit budget after any edit; Instant drags of indoor lamps follow the Classic rules (`DragLite::OwnRoomsWaiting`, `ClassicRulesForDrag`) |
| | Relight when the lamp is let go | `drag_let_go` | Releasing the mouse button ends a drag at once, without the 400 ms still time (`DragLite::LetGo`) |
| | Show the lamp's room as soon as it is solved | `own_room_at_once` | No 300 ms grace for the rooms around the lamp; a room not solved since its mark keeps its maps until it is (`AtriumHold`, trailing) |
| | Furniture changes with its room | `furniture_with_room` | Any lamp edit, not only switches, holds the furniture's lights until its room's maps show the edit (`HoldRig`, `AtriumHold::RoomShowsOldLight`) |
| Per-frame work | Story grid memo, and the fifteen `PerfToggles` caches, filters and pacing rules | `grid_memo`, `perf_*` | The cache or filter; the earlier per-frame path gives the same result |
| | Writable packages' file lists | `perf_key_list_counted_packages` | The kept file lists of writable packages whose writes are counted, valid while their write epoch is the same ([Faster File Lists](../performance/file-list-cache.md#how-apex-radiance-solves-it)); off, only read-only packages' lists are kept. Same keys |
| | Window light channel memo | `perf_window_channel_memo` | The window light's channel test kept per sample batch ([level light share](../night-lighting/level-light-share.md#part-3-walls-of-the-lamps-story)); off, the game is asked for every sample. Same light |
| | Paced relight at rest | `perf_steady_room_pacing` | With the camera still, the frame-time pacing of the game's room solving budgets and of the room queue's drain ([Lot Lighting While Moving](../performance/lot-lighting-motion.md#how-apex-radiance-solves-it)); off, the game's budget every frame. Same light, only how soon rooms finish changes |
| | Paced relight while moving | `perf_motion_paced_raise` | The frame-time pacing of a settling lot's and a lamp switch's raised budgets while the camera moves ([Lot Lighting While Moving](../performance/lot-lighting-motion.md#how-apex-radiance-solves-it)); off, they keep the game's full budget while moving. Same light, only the pace of relighting changes |

A switch that changes the story grids (Colonnades) builds them again and relights every room; Roofed rooms from the
story below relights every room; the rig switches make the object rigs gather again. The other switches act on the next
frame or the next solve.

## Player options and Tester switches

The two never conflict: a switch shown here either writes the player option itself (*Outdoor light through openings*,
*Indoor light through openings*, *Doors block light*, *Smooth light maps*, *Real-time outdoor lamps* = Lamp lighting)
or is a part inside a player option, on by default, that can only switch that part off (the effect is the option AND
the switch):

| Player option | Tester sub-parts |
|---|---|
| Walls shade lamp light near the ground (`paredesSombreiamOChao`) | Walls shade ground and curbs, Soft ground shade |
| Buildings block lamp light on trees and objects (`paredesBloqueiamLuzNosObjetos`) | Buildings block light on trees and objects |
| Outdoor light through openings (`luzDeForaPelasAberturas`) | Roofed rooms take lamps from the story below |
| Smooth light maps (`mapasDosComodosSuaves`) | Fade at the light cut, Dither the light maps, Smooth light map reads |
| Shadow softness (`suavidadeDasSombras`) | Soft edges from the lamp's size |

The fixes, the drag measures (Classic's included) and the per-frame caches have no player switch: always on for
players.

## Notes

- The switches are applied from `[developer]` only in developer mode; in normal mode every part runs.
- The frame readout counts frames while `DragLite::Dragging` (a lamp step within 300 ms) and closes when the lamp has
  been still for 750 ms (400 ms with *Classic: relight only after release* in Classic).
- The tab's texts are translated like the rest of the Developer page.
