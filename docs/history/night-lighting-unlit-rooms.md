# Rooms at Night: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/unlit-rooms.md](../features/night-lighting/unlit-rooms.md).

### 2026-09-29: first build

**Context:** maintainer feedback: rooms with every lamp off still looked very bright inside, and the original game makes
them strongly blue; Sims3SettingsSetter's *Brady Bunch BEGONE* (all zero) made them far too dark.

**Finding:** the colours at `0x011D0B60` / `0x011D0B40`, the top-up `FUN_006a00a0` and the furniture fill light
`FUN_006b7e70` (see the feature page).

**Outcome:** a card with *Light left*, *Blue* and a furniture slider; the six reads redirected. After each change every
room of every loaded lot relit 0.6 s after the last change (`LevelLightShare::RelightAllRooms`) and world-cell rigs
gathered again (`ObjectLightBridge::RequestRigRefresh`). The defaults were a first guess.

### 2026-09-29: applying a change (second build)

**Context:** changes took very long to apply and the controls seemed unresponsive, especially on the played lot.

**Finding:** relighting every room went one room at a time through the game's queue. A room lit by the unlit colour
alone holds exactly that colour at `+0x110`, which the shader reads through a pointer.

**Outcome:** `Retint` writes the new colour into such rooms (12 colours kept per family at the time), topped-up rooms
with an ambient at most 1.25x the brightest known colour are solved again at rest (400 ms), rigs gather again at most 4
times a second and 1.5 s later. Marked Experimental in the menu.

### 2026-09-29: furniture did not follow

**Context:** F7 Capture_057, a sofa in an unlit room drawn by Apex's indoor-object shader (PS `2FE1CD18`).

**Finding:** its light is the smoothed room map x c20, an irradiance cube (s0) x c12.w, the vertex lights (v0) and the
rig's four directional lights c0..c3 / colours c4..c7 (0.0945 0.104 0.170; 0.0136 grey; 0.042 0.042 0.105; 0.021 0.042
0.063). The room ambient `+0x110` is none of them.

**Outcome:** investigation continued with a "Soft light on furniture" test at 0%.

### 2026-09-29: the colour as a base under the lamps (third build)

**Context:** the controls should set the room's tone also with a lamp on; otherwise turning that lamp off changes the
whole room.

**Finding:** captures 061-065 (several objects in the dark room), all on Apex's indoor-object shader (VS `2B1A5700`),
with the same three rig lights in every room (c4 0.0945 0.104 0.170, c5 0.042 0.042 0.105, c6 0.021 0.042 0.063). The
rig diffuse there was replaced by the room map (later the brighter of the two), so rig colours reached only the specular;
the rest was the cube x c12.w (0.25) and v0.

**Outcome:** `BaseHook` and `MoveBase`.

### 2026-09-30: furniture on the game's object shaders (fourth build)

**Context:** captures 066-079: objects that responded to nothing were drawn by the game's own shaders in room mode, mostly
on the upper floor where Apex's indoor-object shader did not apply.

**Finding:** their light is the rig: slot 0 (then believed to be the sun or moon, 0.0945 0.104 0.170), dim bluish
window or sky lights, the cube x c12.w and the vertex lights.

**Outcome:** `OnDrawInner` wrapped every room-mode draw; `AnalyzeRigPs` found the chain; slot 0 and dim bluish slots
(b > 1.15 r, luma < 0.2) got the furniture colour, times *On furniture*; the cube weight scaled; `PatchCubeTint` added the
tint to the game's shaders (maintainer feedback: the blue tint was the only control missing on furniture). Status line
added. Not tested in game at the time.

### 2026-09-30: furniture Blue tint (second study, two adversarial verifiers)

**Context:** the Blue tint still did not work on furniture. Captures 079-094 and the decompile.

**Finding:**

- The blue on furniture is only the three [NoLight] rig lights (x0.21 in the captured rooms) and the fill (slot 1,
  w = 0.8 x strength). The cube is CASDiffuseProbe, flat grey (0.1935 0.2004 0.1935), so the cube tint had nothing to act
  on. The walls' colour never reaches furniture shaders; v0 = 0 and fog is black in every captured furniture draw.
- Room-mode rigs have no sun: slot 0 is the strongest room light, and the guard dimmed and greyed a lamp there (087: the
  red lamp at (1.79 0 0)).
- The tint was multiplied by *On furniture* (at 57.9% furniture moved only between 0.42 and 1).
- Path A replaced the whole rig diffuse by the basis light, so unlit furniture there had only the grey cube (b/r 1.000
  at Blue 0% and 100%; the installed 02:59 ASI emitted only `mul D, Acc, cStr.x`).
- Matte shaders (`EB0C8C35`, `25F66827`) were not recognised.
- `BicubicSetup`'s first instruction read two constant registers, which native D3D9 refuses.

**Outcome:** exact `IsUnlitLight` tests (the colour fallback was rejected by the final review: it also took blue, purple
and dim cool-white lamps, greying them and counting them twice on path A); `FurnitureTintNow` independent of *On
furniture*; path A's diffuse chain pointed at `diffuseConst`; a first try `max(rig diffuse, basis)` rejected by both
verifiers (it brought the per-object rig lamp back next to lamps: HDR 1.79 against the 8-bit basis maps); chain analysis
through the accumulator; `BicubicSetup` split into mul + add; tooltips updated in all languages.

### 2026-09-30: furniture in dark rooms at any hour

**Context:** a session at dawn and by day (night level 0 to 0.33): Brightness sometimes stopped working on objects while
walls followed it.

**Outcome:** `IsDarkRoomLight` and `SetDrawDark`; F6 furniture line every 100 ms.

### 2026-09-30: brightness fixes and the Refresh button (F6 092629, build `871930f4`)

**Context:** the first slider left rooms extremely dark even at 100%, and room brightness could jump to a very high range
while dragging.

**Finding:** `RetintUnlit` told a room's family from the colour it held (the first family was (0.01 0.01 0.01) with S3SS's
*Brady Bunch* setting, the second (0.15 0.15 0.30)). Near 0 both match: dragging to 0.001 and back left every unlit room
of lots `CF2DEA20` and `51A4D8B0` in the first family, 16x too dark ((0.0019 0.0019 0.0019) at 0.19 instead of (0.030
0.030 0.034)), or 15-30x too bright the other way. Lit rooms (20 and 29 lamps; lot `E4606BD0`, story 1 room 14 and story
3 room 8) came within tolerance of a colour set during the drag, were retinted as unlit and lost their lamps' share.

**Outcome:** family from the game's lot test (`SlotOfRoom`, cached 5 s); only rooms with an empty list (or roofless) are
matched by colour. A *Refresh the lighting* row at the bottom of the card (shortcut chip Ctrl+Shift+G / 3 / F9 by preset);
`RequeueAllRooms` also sends basement stories. Seen and left alone: at the first floor switch 11 furniture parts drew one
frame (16 ms) on the game's shader before `RoomMapPadding` paired the new room map (`326E1F60`) with its directional maps;
68 of 459 room-mode parts stay on path B (mostly PS `27F98E68` and `27E3DBB0`), 2 untouched.

### 2026-09-30: one Brightness for walls and furniture

**Context:** the furniture slider and Brightness interfered. At *On furniture* 38.5% and Brightness 8.6%, furniture kept
x0.65 of the game's light while walls kept x0.09, so furniture glowed in dark rooms.

**Outcome:** `efeitoNosMoveis` removed (it had replaced `luzSuaveNosMoveis` earlier the same day; the key is ignored if
still in a file). `UnlitRooms::Set(on, light, blue)`.

### 2026-09-30: furniture cube takes the room's colour (build `a6a253e2`)

**Context:** F7 110-113: a sofa on Apex's shader and a rug on the game's shader, Blue tint 98% then 3.9%. The sofa's turned
rig lights followed ((0.032 0.035 0.057) -> (0.036 0.036 0.037)), but with every lamp off most of the sofa is lit by the
grey cube; walls, floors and the rug take the room's colour ((0.051 0.051 0.101) at Brightness 33.9%).

**Outcome:** both cube patches end with `mul cube.xyz, cube, tintConst.yzww`, set by `FurnitureCubeColour`. Path B takes
the tinted copy also when only the colour differs from 1.

### 2026-09-30: square-root furniture share

**Context:** after the merge, furniture at 33.9% kept x0.34 and a sofa looked far darker than the rug beside it. One of
three proposed curves was chosen.

**Outcome:** `FurnitureShareNow() = 1 + (sqrt(Brightness) - 1) x night` (33.9% -> 0.58, 8.6% -> 0.29). Superseded on
2026-10-01.

### 2026-09-30: automatic refresh after any lighting setting (build `2f441b8d`)

**Context:** every change of a lighting setting should run the F9 refresh automatically.

**Outcome:** `RequestAutoRefresh`, called by `Edit` (every Night Lights card, Rooms at Night, lamp colour, reset),
`ApplyTableLive` when a setting differed, `ReinstallNow` and the *Upper floors light the ground* switch. The Present hook
runs `NightLighting::RefreshAll("a setting changed")` once, 1 s after the last change, never while a reinstall is due.

### 2026-10-01: room synchronisation (`2.5.2-room-sync-test`)

**Context:** the 14:06 recording held room ambient at 0.0317 after Brightness reached 0.105 (target about 0.0169) until the
global refresh completed. A no-lamp ambient absent from the colour history never requested a solve.

**Outcome:** linear furniture share again (same as room ambient; day/night blend and the dark-room override kept); unknown
or merged ambient requests a solve after a change settles (also when disabling); busy rooms go through the queue; lit-room
ownership validated before the unchanged-base shortcut; room enumeration compares story managers; topology changes clear
merged caches; once-per-second reconciliation; world changes clear bases and lot families.

### 2026-10-01: test005 recovery and coordinated controls

**Context:** recording session 17-58-56: some rooms followed the slider while connected lit rooms lagged.

**Outcome:** a room skipped in states 1-3 keeps the recovery pending without invalidating its solve; reconciliation
retries once per target and (address, manager, id); failed queue requests drop their sent record; retarget and world
change reset the records; disabling keeps a base until its restoring solve can be requested; reused addresses with another
manager or id drop their record; an unchanged merged base is accepted only when source, merged ambient and second ambient
match the cache; a failed live move keeps recovery armed; connected slider changes stage the whole group. No colour-family
inference or lamp-contribution guessing.

### 2026-10-01: 2.5.3

**Outcome:** 2.5.3 carries the test005 path and later performance changes (test007, test008). The Experimental badge was
removed on the maintainer's decision; Brightness, Blue tint, keys and defaults unchanged.

### 2026-10-03: reliability audit

**Context:** an intermittent response reported in 2.5.6.

**Finding:** three updater weaknesses reproduced offline: a lit-room base erased when its restoring solve was requested,
before the queue acknowledged it (lost if the queue refused while disabled); the 2e-4 ownership tolerance doubling as the
already-at-target test, skipping small steps (especially the first family's .01 grey); an unset deadline (0) compared as
a tick, failing above about 24.9 days of uptime.

**Outcome:** fixed (see the feature page). No shader, formula, default, key or profile layout changed.

### 2026-10-03: inherited dark grey base

**Context:** a running-game inspection with both controls at 1.0 (see validation).

**Finding:** S3SS's saved `BradyBunchBlue RGB` (.01, .01, .01) applies independently of `BradyBunchBegone.enabled`, and
Apex inherited it as the base of both families.

**Outcome:** on explicit request, only that saved table was removed from the local S3SS TOML (backup folder
`336-disable-s3ss-ambient-rgb`), with every other parsed setting verified unchanged and no live memory changed. The
running session kept the applied RGB until restart.

### 2026-10-03: controls and S3SS compatibility (`b4f9da2`)

**Outcome:** Brightness range 10 to 80% (default 35% kept); Blue tint default 0%; legacy values clamped on load. The first
compatibility version checked and corrected the S3SS override automatically the first time Rooms at Night was turned on.

### 2026-10-04: correction made an explicit action (`f0b8d54`)

**Context:** the first version edited another mod's configuration as a side effect of turning Rooms at Night on.

**Outcome:** the correction runs only from the *Back up and correct* action in the card's *S3SS compatibility* section,
shown while official S3SS is loaded. The effective-base substitution applies only after a successful correction.
