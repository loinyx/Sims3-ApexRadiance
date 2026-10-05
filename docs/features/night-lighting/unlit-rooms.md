# Rooms at Night

Controls the soft background light inside rooms. With every lamp of a room off, the game does not leave the room dark: it
fills walls, floors and furniture with a bright, strongly blue ambient. Rooms at Night lowers that background light and
turns its blue towards a neutral grey, on walls, floors and furniture alike. Rooms with lamps keep their lamps' light and
colour; the background under them follows the same controls, so switching the last lamp off does not change the room's
tone. Part of [Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released (present since 2.1.0, the first version in this repository). The 10 to 80% Brightness range, the 0% Blue tint default, the structure-change recovery and the *S3SS compatibility* correction: in development (PR #2) |
| Default | On; Brightness 35%; Blue tint 0% |
| Menu | Lighting > Buildings > Rooms at Night |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/unlit_rooms.cpp`](../../../features/unlit_rooms.cpp), [`features/room_ambient_policy.h`](../../../features/room_ambient_policy.h), [`framework/s3ss_detect.cpp`](../../../framework/s3ss_detect.cpp), [`framework/s3ss_ambient_policy.h`](../../../framework/s3ss_ambient_policy.h); furniture side in [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) and [`features/shader_patches.cpp`](../../../features/shader_patches.cpp); card in [`patches/night_terrain_relight_patch.cpp`](../../../patches/night_terrain_relight_patch.cpp) (`DrawRoomsCard`) |

## The problem

A room with no lamp light gets a fixed ambient colour, by default (0.15, 0.15, 0.30): a strong blue glow over every
surface. A room whose lamps give less light than that colour is topped up to it, so dim rooms turn blue too. Furniture
in such rooms gets three fixed bluish "[NoLight]" rig lights and a bluish fill light opposite its main light.
Sims3SettingsSetter's *Brady Bunch BEGONE* sets the colour to zero, which leaves rooms far too dark, and its
`disableFillLights` removes the fill light. Neither offers a value in between, and the colour applies only when the
lamps are dim, so turning the last lamp off changes the whole room's tone.

## How Apex Radiance solves it

Apex Radiance points the game's reads of the unlit-room colours at its own values, scaled by *Brightness* and moved
towards a grey of the same luminance by *Blue tint*. The same colour is added as a base under the lamps of lit rooms.
Furniture draws in such rooms get the same treatment on the game's unlit-room rig lights and ambient cube. Changes reach
walls and floors on the next frame, without a new light solve where possible.

1. **Colours.** For each of the game's two colour families, `colour = Brightness x (grey + Blue x (game - grey))`, with
   grey the Rec. 709 luminance of the game's colour.
2. **Base under the lamps.** The game's top-up is replaced by "lamps plus the colour" in lit indoor rooms.
3. **Live retint.** Rooms that hold a known colour get the new colour in place; rooms whose result needs a solve are sent
   to the game's queue once the slider rests.
4. **Furniture.** In room-mode furniture draws, only the fill and [NoLight] rig lights and the ambient cube follow the
   controls; lamps keep their colour and strength.
5. **Recovery.** Rooms that were busy, rebuilt, streamed in or structurally changed are reconciled once a second.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Adjust the background light | `comodosEscurosSemLuz` | bool | on | | Off restores the game's colours (all patches removed) |
| Brightness | `luzQueSobraNosComodos` | float | 35% | 10 to 80% | Share of the game's unlit-room light kept on walls, floors and furniture |
| Blue tint | `azulNosComodos` | float | 0% | 0 to 100% | 0% is neutral grey of the same luminance, 100% the game's blue, on walls and furniture |
| Refresh the lighting (button, with its shortcut chip) | (not saved) | action | | | Relights terrain, lot stories, every room of every loaded lot (basements too) and object rigs now and 2 s later (`NightLighting::RefreshAll`) |
| S3SS compatibility > Back up and correct (shown only while official Sims3SettingsSetter is loaded) | (not saved) | action | | | Backs up `S3SS.toml` and removes only its saved `BradyBunchBlue RGB` override (see *Compatibility*) |

Values outside 10 to 80% saved by older versions or profiles are clamped on load; the keys are unchanged. Reset sets the
defaults above. All settings apply live (`UnlitRooms::Set` every frame). Any Night Lighting setting change also triggers
the automatic *Refresh the lighting* 1 s after the last change.

## Compatibility and interactions

- **Sims3SettingsSetter.** With the card on, Apex's colours win, because the game code reads Apex's vectors; with it off,
  whatever S3SS set applies again. If S3SS patched the same six reads after Apex started, Apex's patch fails (the bytes
  are not what Apex saw) and the log says so. Apex reads the game's colours through the original pointers every 2 s, so a
  colour another mod writes there becomes the base the controls scale.
- **S3SS saved room-light colour.** S3SS can save `[settings.'BradyBunchBlue RGB']` independently of
  `[patches.BradyBunchBegone].enabled`; S3SS applies saved settings when it registers them, so turning *Brady Bunch
  BEGONE* off does not restore the blue. A dark saved grey such as (0.01, 0.01, 0.01) then becomes the base, and the
  controls cannot brighten past it or restore blue. The *Back up and correct* action:
  - runs only when the player presses it, and only while official S3SS is loaded; enabling Rooms at Night never edits S3SS;
  - writes a content-specific backup `S3SS.toml.before-room-ambient-fix.<FNV-1a 64 hash>.bak` in the Apex Radiance folder
    (an existing backup is reused only if identical);
  - removes only that saved entry when it holds exactly three values in 0 to 1; invalid or unsupported values are left
    alone. The ordinary standalone section is cut out with comments and layout kept; other layouts are rewritten with a
    semantic TOML formatter, and the result is checked to parse to the same document minus that entry;
  - aborts if the file changed after it was read, and writes atomically;
  - after a successful save, while Rooms at Night is on, uses the standard blue (0.15, 0.15, 0.30) as the base of the
    second colour family whenever the game's current colour exactly matches the removed override, for the rest of the
    session. The first family's legitimate grey, the alpha and the native globals are not changed. S3SS uses its default
    colour after the game restarts.
  The card reports each outcome: S3SS not loaded, config unreadable, no supported override, backup failed, config changed
  during correction, write failed, or saved.
- **Light between stories** ([level-light-share.md](level-light-share.md)): a room whose own lamps are off but which
  takes lamps through an opening has lamps in its list, so it is lit by them and topped up by this colour. The atrium
  ambient merge works on the colour produced here, and slider changes move whole atrium groups together.
- **Indoor objects** ([objects-and-rigs.md](objects-and-rigs.md)): furniture changes need the rig tracker (*Doors and
  windows stay lit*) and the lot light bridge (*Street lamps light lots*) running.
- **Rendering**: the furniture path patches pixel shaders by pattern; a shader the patch does not recognise keeps the
  game's look for its tint.

## Limitations

- Equal multipliers do not give identical pixel brightness on different materials and shaders.
- Furniture follows less strongly than walls: the environment specular (DefaultSpecProbe, about 25 to 35% of its blue
  channel) is left alone. Rooms whose rigs gathered enough lamp light have no [NoLight] slots, so their furniture has no
  game blue for the slider to change.
- Room-mode furniture keeps its fill until its rig gathers again (a lamp of the room switched, the object moved, the lot
  loaded, a refresh).
- Furniture drawn with game shaders the rig analysis does not recognise (vertex-light and normal-mapped variants such as
  PS `27F98E68` and `27E3DBB0`) keeps the game's response.
- When a story changes, furniture can draw one frame on the game's shader before Apex pairs the new room light map with
  its directional maps.
- Outdoor and roofless rooms (room 0, fenced yards) keep the game's value.
- Steam 1.67.2 only. If the six read sites differ, the card cannot act; if only the top-up hook fails, lit rooms keep the
  game's top-up (log: *Base under the lamps: the game code differs*).

## Technical reference

TS3W.exe Steam 1.67.2.

### The game's unlit-room colour

- `FUN_006a0f50` (the room's ambient, state 0 of the room solve, called from `FUN_006a18b0`): when the room's light list
  (`room+0xC8..+0xCC`) is empty it sets the normalisation `room+0x160` to 1 and the ambient `room+0x110` and `+0x120` to
  the vector at `0x011D0B60` or `0x011D0B40` (`mov ecx, imm32` at `0x006A0F94` / `0x006A0F9B`, through the identity
  `FUN_00f626f0`). Which one depends on `FUN_00c63140(worldMgr, x, z)`: the lot there, its type (vfunc+0x40 == 1) and its
  byte `+0x430`. Both vectors are in `.bss`; no code writes them by address (unverified where the values come from).
- With lamps, the end of `FUN_006a0f50` calls `FUN_006a00a0` twice (`0x006A13F0` -> `+0x110` with pow 1, `0x006A1410` ->
  `+0x120` with pow 0): the same colour (the same two `mov ecx` at `0x006A00C1` / `0x006A00C8`) times `[0x011D0B88]`.
  When the lamps' light sums under 0.0005 (`0x00FE3500`) the room gets that colour alone, otherwise the lamps' light plus
  the shortfall when the colour is brighter.
- Walls and floors add `light map alpha x room ambient` (wall PS `mad r0.xyz, r0.w, c4, r0`). The shader parameter
  `InteriorBuildingAmbientColor` (id `[0x01158AEC]`) is set to a pointer to `room+0x110` when a room is drawn
  (`0x0069EB70`, `0x006A06BF`; parameter entry `+0x0C` = data pointer), so a value written there shows at the next frame.
- The furniture fill light: `FUN_006ba340` (fills a rig's light slots, from `FUN_006bba50`) calls `FUN_006b7e70` when
  the rig is room-mode (`rig+0x1D4 == 0`) or has flag `0x20` in `rig+0x224`, while the byte `0x01158D5C` is set (`cmp
  byte [0x01158D5C], 0` at `0x006BA57C`; 1 in the file). `FUN_006b7e70` sums the rig's lights into a main direction and,
  when strong enough, shifts the slots and adds a light opposite with colour `[0x011D0E10]` x the amount (`movaps xmm2,
  [0x011D0E10]` at `0x006B816A`), set once to (0.8, 0.8, 1.0, 0.8) (`0x00F9D514` = 0.8). Its w is 0.8 x the strength;
  every other slot has w = 0.
- The three [NoLight] lights (CustomLightRigging.ini) are added by `0x006BB3E0` to room-mode rigs of dark rooms. Their
  normalised directions: (0.09535, 0.95346, 0.28604), (0, 0.70711, -0.70711), (-0.66667, -0.33333, -0.66667). Room-mode
  rigs have no sun (`0x006BBDE0`: start slot = (mode == 1)), so slot 0 is the strongest room light. The ambient cube of
  room-mode objects is CASDiffuseProbe, flat grey (about 0.1935, 0.2004, 0.1935).

### Read sites

| Id | Steam | Pattern | Check |
|---|---|---|---|
| `RoomAmbient` | `0x006A0F50` | `55 8B EC 83 E4 F0 81 EC 04 01 00 00 53 56 8B F1 8B 86 C8 00 00 00 3B 86 CC 00 00 00 57 75` | 1 match |
| `UnlitColourA` / `B` | `0x006A0F95` / `0x006A0F9C` | in `RoomAmbient` (0x80): `84 C0 B9 ?? ?? ?? ?? 75 05 B9 ?? ?? ?? ?? E8` +3 / +10 | `B9` before each, same values as the `Dim` pair, A != B |
| `DimAmbient` | `0x006A00A0` | `55 8B EC 83 E4 F0 83 EC 3C 8B C1 8B 50 14 8B 40 10 8B 0D` | 1 match |
| `DimColourA` / `B` | `0x006A00C2` / `0x006A00C9` | in `DimAmbient` (0x40): same pattern | as above |
| `FillGate` | `0x006BA57E` | `80 3D ?? ?? ?? ?? 00 74 26 8B CF E8` +2 | `80 3D` before, `00` after |
| `FillColour` | `0x006B816D` | `0F 28 15 ?? ?? ?? ?? 0F 58 C1 0F 59 C5 0F 57 C9` +3 | `0F 28 15` before, 16-byte aligned target |
| `DimAmbientCall0` / `1` | `0x006A13F0` / `0x006A1410` | `E8` to `DimAmbient` | `mov ecx,[WorldManager]` at `DimAmbient+0x11`, lot test CALL at +0x1A, `mov ecx, scalar` at +0x32 |

All six reads are redirected together or not at all (`Patch`), and restored when the option is off. The four colour
reads point at Apex's two colours; the gate byte and the fill colour point at copies that keep the game's values (1 and
(0.8, 0.8, 1.0, 0.8)), because furniture is adjusted per draw instead. The game's colours are read through the original
pointers every 2 s; an unreadable or all-zero colour uses the default (0.15, 0.15, 0.30) (status "(default)"). The log
line at start is `[UnlitRooms] Ready: unlit-room colours at 0x11d0b60 (...) and 0x11d0b40 (...), fill gate ..., fill
colour ...` (*not set yet: default* while still zero).

### Base under the lamps (`BaseHook`)

The two top-up calls go through `BaseHook<0>` / `BaseHook<1>` while Rooms at Night is on, for indoor rooms that are not
roofless. The unlit colour C (family by the lot test `FUN_00c63140` on the room's lot id `+0x10/+0x14`, times the scalar
`[0x011D0B88]`) is always added to the lamps' ambient instead of only topping it up. The lamps' share is recovered from
the game's result (sums over x, y, z with weight 1.0 `[0x0107A538]`; a top-up happened iff sum(out) < sum(C), then
sum(lamp) = (sum(out) - sum(C)^2) / (1 - sum(C))). Each room's C and results are kept (`g_baseRooms`, keyed by room
address with manager and id), so a slider change moves lit rooms at once (`MoveBase`: value + new C - old C, only while
the room still holds what `BaseHook` wrote). Turning the option on sends every lit room once (at rest) for a solve with the
base; turning it off sends the based rooms back to the game's top-up. Status: *base under the lamps: on (given in solves
N, moved at once N)*.

### Applying a change

- `Retint` visits every room of every loaded lot (`LevelLightShare::ForEachRoom`, stories -4..7; the list is rebuilt at
  most every 3 s or at once after a lot, manager or structure change). An unlit room (empty light list, or roofless) that
  holds a known colour of either family (the game's or Apex's, up to 64 per family) gets its own family's new colour,
  the family coming from the game's lot test (`SlotOfRoom`, cached per lot for 5 s). A room with lamps is never matched
  by colour; it is moved by `MoveBase` or solved again.
- Colour matching uses a 2e-4 RGB tolerance to recognise owned colours; the "already at target" exit also requires a
  1e-7 match, so small slider steps are not skipped.
- Walls and floors follow every 50 ms at most while a slider moves. Object rigs gather again after a retint at most 4
  times a second. 400 ms after the last change, rooms whose result needs a solve (unknown or merged ambient, topped-up
  lit rooms) are sent to the game's queue (deferred while being solved), then the rigs once more 1.5 s later.
- A lit room's base survives a rejected queue request and is retired only when the queue accepts the restoring solve
  for the same manager and id; a fresh `NoteBase` result cancels the retirement.
- Unset periodic deadlines (0) are always due; real deadlines use wrap-safe comparisons.
- Once a second, a reconciliation retries unknown or unbased idle rooms (once per target and per address, manager and
  id), including rooms skipped while their solve was active and rooms streamed in. Failed queue requests drop their sent
  record so they can retry. Unchanged scans request no rig refresh.
- `OnRoomChanged` (from the structure-change watch of [level-light-share.md](level-light-share.md): wall list or roof
  classification changed) clears that room's sent record and triggers an immediate reconciliation. `OnRoomsChanged`
  (lot, manager or floor change) and `OnWorldChanged` reset the records; a world change also clears the room bases and
  lot families.
- Connected atrium rooms are moved as a group by the light-between-stories group updater (`StageAmbientBaseChange`).
- Disabling the option keeps a base record until its restoring solve can be requested; recovery runs while disabled if
  work remains.
- Status: *changes applied N, rooms given the new colour at once N, rooms topped up from their lamps solved again N*.

### Furniture

- `FurnitureShareNow() = 1 + (Brightness - 1) x night` (`RoomAmbientPolicy::BackgroundShare`, linear like the walls),
  `FurnitureTintNow() = 1 + (Blue - 1) x night`. Night is the game's night level, or 1 for a draw whose rig holds a
  [NoLight] light with luma > 0.01 (`IsDarkRoomLight`, `SetDrawDark`): a dark room is adjusted at any hour, a lit room by
  day keeps the game's look. `FurnitureActive` = on and night > 0.001.
- `IsUnlitLight(colour, dir)`: the fill (w > 0) or a [NoLight] light by direction within 1e-3. Exact tests only.
  `FurnitureColour` turns only those: `share x (grey + tint x (rgb - grey))`.
- **Game shaders** (`OnDrawFurniture` in `lot_light_bridge.cpp`, every room-mode object draw, RigTracker mode 0):
  `AnalyzeRigPs` (cached per pixel shader) finds the rig chain c4..c7 and the cube weight constant; it follows the chain
  through its accumulator when the four steps are not consecutive (unswizzled colour constant, replicated multiplier,
  unswizzled accumulator) and recognises matte shaders with `dp3_sat` between the steps (for example `EB0C8C35`,
  `25F66827`). PS c4..c7 (directions in c0..c3) and the VS vertex lights (direction `c(vl-4+k)`, colour `c(vl+k)`) are
  turned with `FurnitureColour`; the cube weight `.w` is multiplied by `FurnitureAmbient()`. When the tint or cube colour
  differs from 1, the draw uses a copy from `ShaderPatches::PatchCubeTint`: after the cube read weighted by `c.w`,
  `dp3 Tmp.x, cube, cL` (cL = Rec. 709 weights), `lrp cube.xyz, cT.x, cube, Tmp.x`, `mul cube.xyz, cube, cT.yzww`, with
  cT and cL above the shader's own constants. Every constant is restored after the draw.
- **Apex's indoor-object shader** (`PatchIndoorBasis`, path A): the rig diffuse chain's c4..c7 point at four new constants
  (`diffuseConst` = cS+7..cS+10), filled by `DrawIndoorObject` with the rig's unlit-room lights (lamp slots 0), and the
  chain ends with `mad D, Acc, cStr.x, D`. Furniture there is lit by the turned unlit-room lights plus the basis light
  (lamps per pixel); the specular chain keeps c4..c7 and the vertex lights stay zeroed (overflow lamps are in the basis
  maps). The cube is tinted the same way (`tintConst`).
- `FurnitureCubeColour` sets `tintConst.yzw` to the chroma of the second family's base (0.15, 0.15, 0.30 gives x0.93,
  0.93, 1.86 at luma 1) by the tint: (1, 1, 1) at 0%, by day, or while Rooms at Night does not act. `.yzw` must be set
  wherever `tintConst` is set (0 would make the cube black).
- Status (`ObjectStatus`): *Rooms at Night on furniture: N draws (M with the blue tint in the game's shader)*. The F6
  recorder adds a line every 100 ms: room-mode draws (not acting / no rig chain / turned / Apex indoor-object shader), rig
  slots turned and lamps kept, the control values and the night level.

### Developer status

Developer page status *Rooms at night: on | the game's unlit-room colours (...) | Apex's (...) | furniture fill (...) |
relights: N*.

## Rejected approaches

- Relighting every room of every lot after each change: far too slow to follow a slider. Details in
  [history](../../history/night-lighting-unlit-rooms.md).
- Telling a room's colour family from the colour it holds: both families match near 0% and rooms switched family.
- Matching lit rooms by colour: lit rooms lost their lamps' share.
- A separate *On furniture* slider, and a square-root furniture curve: superseded by one linear Brightness.
- Tinting the furniture cube towards its own grey: the cube is already grey.
- Treating rig slot 0 as the sun in room mode: dimmed a lamp.
- A "dim and bluish" colour test for unlit lights: also caught blue, purple and cool-white lamps.
- `max(rig diffuse, basis)` on Apex's indoor-object shader: brought the per-object rig lamp back next to lamps.
- Editing S3SS automatically when Rooms at Night is enabled: replaced by an explicit player action.

## See also

- [Validation](../../validation/night-lighting-unlit-rooms.md)
- [History](../../history/night-lighting-unlit-rooms.md)
- [Light between stories](level-light-share.md), [Objects and rigs](objects-and-rigs.md),
  [Room light maps](../../engine/room-light-maps.md)
