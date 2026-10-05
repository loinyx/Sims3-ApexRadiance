# Light between stories: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/level-light-share.md](../features/night-lighting/level-light-share.md). Dates before
2026-09-28 refer to the combined build (Apex inside a fork of Sims3SettingsSetter); the standalone started from its v0.1.0
baseline (`b84d5f1`), in which this module was already present and later changed only in its status strings (Portuguese
in v0.1.0, for example "Luz entre andares nao confere").

### 2026-09-25: straight cut at the floor line

**Context:** LightProbe captures `andar2-b` (m44) and `andar1-b` (m45): a sconce on the upper outside wall lit the upper
wall and nothing below the floor line. A second report: a
street lamp lit the wall of one story and the window of another.

**Finding:** room 0 has its own light list per story; `FUN_006c6ab0` adds the level-0 lamps a second time only for room 0
of level 0 (`0x006C6B16`), confirmed in `S3SS_LightDiag.txt` 01:39 (type 3 lamps twice, type 11 three times) and
`re/out/dump/asm/006c6ab0.asm`. `FUN_006c7250` already refreshes room 0 of every story when level 0 changes, so the test
in `FUN_006c6ab0` looks inverted. Rigs compare only the room id, so windows already took lamps of every story.

**Outcome:** part 1 (sharing through `OutdoorGather`) and part 2 (JNZ -> JL cascade). Maintainer feedback around 10:00:
almost seamless.

### 2026-09-25: first version without a wall test

**Context:** the first version (around 09:20) shared lights but did not test walls.

**Finding:** LightProbe `andar1-c` (m47, with m46): the side face of the ground story near a corner became lighter than
the upper one. F8 09:50, lot `C49C001BCF2DEA20`: the 19 lights of room 0 were right on every story, but a sconce near a
corner passed above the lower story's walls (`room+0x30` holds only the own story's walls; `FUN_0069aa90` has no base
test).

**Outcome:** part 3, the wall test of the lamp's story. Maintainer feedback: much better.

### 2026-09-25: review of the first design

**Context:** independent review around 10:00 and 10:40.

**Finding:**

- A "cascade by signature" (re-queue the other stories whenever the set of lights of room 0 changed) could re-enter
  rooms whose `+0x28` set was being walked.
- The lot-load path gathers room 0 of every story through level 0 (`FUN_006c54e0`, `0x006C5525`); the real story must be
  read from the manager (`room[0] + 0x88`).
- `FUN_0069fc40` reads the wall mode `+0x639` of the room it is given, not of the room being solved (item 2).
- `re/out/dump/light_vtables.txt` merged `0xFF4408` with `0xFF43A8` and omitted `0xFF4468`; the factory `FUN_006ac590`
  creates 9 classes, so CircleWindowLight and TubeLight were initially not wrapped.
- The batch solve culls walls per light (`FUN_006a30b0`), which the first cross-story test ignored.
- F8 initially had no story section: the hotkey uses `light_diag.cpp`, not `patches/light_diag_patch.cpp`; the section
  was added to both.

**Outcome:** signature cascade removed (only the game's cascade and an explicit refresh on install and uninstall remain;
never queue the story whose set is being iterated); real story read from the manager; basements left out; rooms already
pending skipped; `+0x639` swapped; 9 classes wrapped; per-batch culling mirrored. Option changes of other Night Lighting
settings used to reinstall this module; `ReinstallNow` now leaves it in place.

### 2026-09-29: indoor lamps through stair openings (part 4)

**Context:** a red lamp next to the stairwell on story 2 lit the story-2 wall and stopped at the moulding. LightProbe
`andares2`: the story-1 wall below uses its own room's wall atlas (256x128) with only the room's ambient. Room ids in the
test house: story 1 rooms 1, 2, 3, 4, 8; story 2 rooms 5, 6, 7, 9, 11, 12.

**Finding:** an indoor list only takes lamps whose entry room id equals the room id, and ids are unique per lot. An RE
study found the lighting tiles, the floor grid and the floor objects, and assumed an opening is the never-built key.

**Outcome:** part 4 built. The "Through stair openings" option was renamed "Indoor light between floors" and moved to the
new Lighting > Stories tab with the outdoor share and the ground option.

### 2026-09-29: what an opening is

**Context:** the first in-game test found no opening anywhere; the second (counting the bare `40000000` key) found 200
"openings" on story 2 instead of about 40, and the white lamps of the story below washed out the upper wall around the
red lamp.

**Finding:** F8 maps: a removed floor leaves bit `0x40000000` plus other low bits (test tower `0001E00F` with its floor,
`4001E000` without; ground under the foundation `4001BFFE`; the stairwell `4000E000`). The bare `40000000` lies along walls
and around a floor's edge over rooms that keep their ceiling.

**Outcome:** `RemovedFloorKey` requires the bit and other low bits.

### 2026-09-29: safety review of part 4

**Context:** review of the first part-4 build.

**Finding:** the changed-rooms set is filled inside the room update, so reading it before the update missed most lamp
changes; a floor that cannot be read must not pass light; a fault mid-gather could leave an untested lamp in a list.

**Outcome:** the set is read just before it is emptied (`ChangedClearHook`); unreadable floors count as solid; the room
record is written before each lamp goes in; world-change clears keep the indoor list (`g_indoorList`); the solve context
is used only on the light tree thread; points within 2 cm of the floor plane are tested in place.

### 2026-09-29: re-gather loop

**Context:** atrium house: rooms 19 and 20 sent each other to gather dozens of times in a row, each re-gather resetting
their lighting LOD.

**Finding:** the light entry update `0x006C7BA0` marks a room changed for any lit lamp whose entry is updated, changed or
not. The exact trigger in the test house was not identified; `treeLevel+0x4C` was ruled out (those are object rigs).

**Outcome:** `RoomLampSignature`: a changed room sends its takers only when its lamps or walls differ from the last send.

### 2026-09-29: lighting detail of rooms seen through an opening

**Context:** seen from above through an opening, the lower room's coarse grid showed as a bright step at the floor line
under a sconce; switching the camera story changed the look.

**Finding:** class 0 samples walls every 0.75 m. F8 of the atrium house: the lower room's top sample, 0.47 m over the
lamp (5.65), stretched up to the line, while the finely sampled wall above started at 0.45 (1.22 m over it). The game
never raised the lower room because `FUN_0069e770` gives 0 priority when the class is above what `FUN_0069e710` asks.

**Outcome:** `LodChoiceHook`.

### 2026-09-29: one ambient for stacked rooms

**Context:** with lamps shared and LOD raised, an atrium wall still showed a thin seam. Light Probe: c4 (0.041 0.045
0.044) above, (0.058 0.058 0.054) below.

**Finding:** an atrium's upper room has almost no floor, so its ambient differs, and the wall ramp restarts at the line.
A first merge applied the group's lowest ramp base to every pass; the floor, ceiling and object passes share that
sampler, which made the upper room's floors and ceilings much brighter (review).

**Outcome:** `MergeStackedAmbient`, with the lowest base applied to the wall pass only (`WallPassHook`).

### 2026-09-29: seamless walls (part 5)

**Context:** after parts 1 to 4 the atrium walls still showed a step at the floor line (floors had improved, walls had
not); F7 captures 048/049 showed the same slight step on outside walls on two stories' atlases.

**Finding:** the step is the game's wall sampling, not the lamps. F8: the atrium's lower room had rows up to 46.44 / 46.24
/ 45.92 for the three classes, the upper room's bottom row at the line (46.67). The first fix (19:33) moved samples and
kept edge rows out of the vertical blur; with a sconce just under the line the rows next to the edges were raised by the
blur and the edges were not, giving a crease (east sconce column: slope 0.54/m under the line against 0.11/m above, 4.8x;
a blur continued across the line gives 0.44/0.19, 2.4x). Maintainer feedback: almost perfect, with a very slight
difference left. The solid strip along the atrium's north wall (keys `00028019`) keeps its step correctly.

**Outcome:** second version (around 20:15): ghost rows and a combined blur for atrium groups. Room 0 deliberately left
out.

### 2026-09-29: cost of the per-point hooks

**Context:** standalone review of the point solve's cost.

**Finding:** the hooks called `GetCurrentThreadId` and recorded F8 samples in every lit cross-story evaluation.

**Outcome:** thread id read from the TEB; F8 sample recording only in the development build and only while armed; the
game's wall test wrapper (`GameWallTest`) only in the development build. Not measured in game.

### 2026-09-29: re-entering a lot and loading

**Context:** maintainer feedback: the indoor light sometimes worked only after reloading the save, and was right only
after a lamp was moved; the target is a lot that is right as it loads. After a restart the stairwell light was gone until
a floor was edited.

**Finding:** `LotState`, keyed by the tracker address, kept the counts of the lot as it was; a lot whose lighting was
rebuilt (new story managers) or a reused tracker counted the same floors and openings, so its rooms were never sent. A
lot also gathers its rooms while it loads, before its floors or room ids are ready.

**Outcome:** the state keeps the lot's 8 story managers and restarts when any changes; floor objects are recorded at
construction (`LevelCtorHook`); a lot that finds its openings sends its rooms near them once more (settle); every 2 s the
known floor objects and opening count are compared. F8 *SOLVES* journal added, later with `Q`, `H`, lot id, camera story
and the room flag `+0x19`.

### 2026-09-29: slow convergence in the atrium house

**Context:** the lot was entered at night with its lamps still off; they switched on one after another over about 1.5 s.

**Finding:** every switch changed a lamp's values, so every taker was sent again at once and its solve thrown away (room
10 of story 2 solved 10 times in 2 s; room 20, the atrium's upper room, restarted twice before its first full solve). The
first round was over 2.25 s after the lamps came on, but the fixed 6 s settle came 2.5 s later. On a world loaded at
night (some lights failed only right after starting the game; Night Lighting off and on fixed it), lots were solved during the
load screen and the early settle could fire before a lot was complete. Invalidation callers (F8 with `I` / `F` notes):
465 of 589 came from `0x006C7404`, the part-2 cascade; room 0 of stories 1-3 of lot `7D6F001A00D62480` restarted about 140
times each in 70 s and was never shown above class 0 (outside lights were slow on both floors and weak after a load until the
map view was opened). The same F8 had 1028 "sent at once" with the camera still: animated lamps
wobble in position and range.

**Outcome:** shape and value hashes with the 1 s / 300 ms / 1.5 s rule; early settle; `OnWorldLive`; the cascade replaced
by `90 E9` at `0x006C73B0` plus `AfterChangedWalk` when the indoor hook is in; position and range tracked per lamp
(10 cm, 2%). The first build hashed position among the values and the light lagged behind a dragged lamp; position moved
to the shape.

### 2026-09-30: rooms holding a switched-off lamp

**Context:** F8 01:26, house `C49C001BCF2DEA20`, every lamp off: after the load, room 2 of story 1 still held two lamps
switched off for about 25 s (journal: lights 20, 9, 4, then 2 only when another lamp was switched on). Maintainer report:
the background colour was wrong right after entering and fixed itself later.

**Finding:** the game switches a lamp off through its intensity and keeps the lit flag; why the game did not gather the
room again is not known.

**Outcome:** the once-a-second stale-lamp scan.

### 2026-09-30: pairing a story with its floor

**Context:** a double-height room in lot `8C41002E4010A180`: the lamp below never lit the walls above. F8 paired story 2
with a floor of another shape (`0000C007` / `00000006`, 1064 quadrants, no removed floor); after leaving and entering the
lot, the real floor (`0000C041`, `4000C000` removed x224, `0000C010`) was found.

**Finding:** the floor object's `+0x238` copy goes stale when the lot's lighting is rebuilt, and a new manager at the old
address made another floor belong to that story. That was only half of it: in the next session 37 of 37 copies were
fresh and story 2 was again paired with `0000C007` / `00000006`, which is story 2's ceiling layer (world level 3, byte
`+0x234` = 0, the attic outline). The pairing took whichever of the two objects was noted last; *Refresh the lighting*
did not help.

**Outcome:** the manager is found through the floor's lot (`LevelManager`) and `LevelFor` keeps only the story's own
floor (`LevelOwnFloor`); F8 lists every object naming each story. Confirmed in game with build `e3fab9d4`.

### 2026-09-30: floor switches restart lit rooms

**Context:** maintainer feedback: the indoor light broke for a moment at every floor switch; the light should be kept
and changed only when something changes. F6 092629: every switch restarted room 0 of every story and every room holding a lamp
(`0x006C7451`, twice within 16 ms), solved again in 110-140 ms with the same ambient, light counts and normalisation.

**Finding:** two studies: Apex added one room of its own (room 14 of story 0, through `AfterChangedWalk` deps); the rest
is the game's entry update marking rooms without comparing.

**Outcome:** `lamp_mark_filter.cpp` (`MarkThunk`). Installed as build `81934361`; the approved floor build is backup 108,
the previous ASI backup 127. A follow-up (build `5eba1685`) relights a lot when a lamp is switched or moved,
provided it costs no performance, with a 0.7 s delay at the time.

### 2026-09-30: the directional basis maps ignore the floor test

**Context:** F7 128 and F8 10:45: a TV in room 5 of story 2 (closed from below; the only opening is the stairwell in room
7) took the green lamp of story 1.

**Finding:** the lamp is in the lists of rooms 5, 6 and 7. The room light map was right (21 points of the lamp on story
2, all blocked, map black), but the four directional basis maps are filled by another routine that never goes through
the hooked point solve. The game's furniture shader does not read them; Apex's indoor-object shader does.

**Outcome:** a shader-side cap first: `min(basis, 2 x room light map)` per channel (`kBasisCap`, def `cS+11`), checked on
4 captured indoor-object shaders (captures 096-103: light map 0.239, basis 0.157). A third study then located the
routine (`FUN_006a09f0`, `FUN_0069f280`) and `BasisLightHook` (build `c05df4fe`) tests borrowed lamps there; the shader
cap stays as a fallback.

### 2026-09-30: rooms lit only by lamps of another story

**Context:** F6 105204, build `96f17fbe`: a room ignored the room brightness for a while, mostly after loading.
Rooms 5 and 7 of story 2 held only the green lamp of story 1, blocked at every point: ambient (0, 0, 0), normalisation
inf, the Brightness slider had no effect (room 6, almost all blocked: x98).

**Finding:** limit / 0 = inf, ambient NaN, clamped to 0.

**Outcome:** `RoomNormHook`.

### 2026-09-30: refresh after a load

**Context:** a room lit only by lamps of another story was black after a load until *Refresh the lighting* ran.

**Outcome:** `RequestRefreshAfterLoad`, 8 s after the world is live at the time; later made adaptive (see 2026-10-01,
test005).

### 2026-10-01: indoor light on multi-story lots, investigation and roadmap

**Context:** report: on two-story lots at night the unlit-room correction and the indoor light between stories sometimes
fail (a room or a whole story keeps the wrong light, or furniture loses its colours); leaving and entering the lot or
switching floors sometimes fixes it. Floor switches should stay instant without losing the corrections. Method: code and
docs only, a first fix, four parallel studies (adversarial review of the fix, furniture, unlit-room colour, story and
floor pairing) and a synthesis, on branch `fix/indoor-light-floors` based on 2.5.2. Nothing was compiled or tested in game.

**Finding:** several modules keep per-room or per-lamp state keyed by addresses or by (tracker, story, room id) and
assume the key names the same thing while the lot is loaded. A lot whose lighting is rebuilt gets new story managers and
rooms while its tracker and tree levels stay, its lamps are registered again (often identical), and freed blocks come
back at old addresses. A stale entry answers "nothing changed" for a new room, so the room is never sent; a floor switch
or re-entry happens to send it through another path (the game's restart at `0x006C7451`, a new build). Findings:

| # | Where | Finding | Proposed fix |
|---|---|---|---|
| 1 | `lamp_mark_filter.cpp` | Marks dropped by (tree level, light) for the whole session, also for rebuilt stories; never cleared on a world change | Remember the manager per lamp; drop a mark only when the room holds the lamp exactly when the game's filter takes it (`LevelLightShare::GameWouldTake`); clear on a world change |
| 2 | same | The first fix used "lit" instead of the game's filter, and ran game code under the filter's mutex | Fixed in the branch |
| 3 | same | Only the lamp's own room is checked, not rooms of another story that took it | A lamp taken by another story always marks (`TakenByOtherStory`) |
| 4 | `level_light_share.cpp` `g_depSig` / `g_deps` | Signatures keyed by (tracker, story, room) without a manager check, never cleared | Store the manager; clear per lot on rebuild (`ForgetLot`) and on a world change |
| 5 | same, `g_ambOrig` / `g_ambApplied` / `g_ambQueuedAt` | The merge averaged a new room with the previous build's values | `ForgetLot` on rebuild |
| 6 | same, `g_lots` | Not cleared on a world change | Clear on the light tree thread |
| 7 | same, `g_staleSent` | Keyed by room address; a busy room erased its entry and could be re-sent every second | Store manager and id; busy rooms keep their entry |
| 8 | `unlit_rooms.cpp` `g_baseRooms` | A record of the previous room at the same address made `MoveBase` leave a rebuilt room alone | Mismatch erases and falls through to a solve; `UnlitRooms::OnWorldChanged` |
| 9 | `object_light_bridge.cpp` / `rig_tracker.cpp` | Indoor furniture rigs (mode 0) were never gathered again by Apex: `0x006B58F0` walks only the world cells and the room-rig regather accepted mode 1 only | Collect mode-0 rigs bound in the next 2 frames (`RigTracker::CollectRoomRigs`) and update them through `0x006BBF90`, 128 per frame (unverified that `0x006BBF90` runs the room gather for mode 0) |
| 10 | `room_map_padding.cpp` | Pairing of a room light map with its basis maps throttled per (basis map, shader), so a story's floor map and object map could stay unpaired | Make the light map part of the key |

Rejected or deferred in the synthesis: dropping a mark when room 0 does not hold the lamp (relies on an unverified guess);
treating the mark filter as the whole cause; manager pointers as the only rebuild test (a manager can come back at the
same address); merging only the lamp share of stacked rooms (changes atrium looks; needs a comparison first); acting on
mode-1 furniture (until a mode histogram shows such draws); pre-seeding a rebuilt room's ambient from a cache (a correct
colour on wrong texels); recording the stale-lamp send only when the queue accepted it (small effect).

Roadmap: phase 1, test the branch (enter, leave, re-enter, switch floors; status lines *Rooms keep their light*, indoor
rigs gathered again, *lots rebuilt*). Phase 2, instrumentation: rig mode histogram, per-rig gather age and unlit slots, a
"stale rig" line, `[furniture] A miss`, per-story floor objects in F8. Phase 3, a per-story cache keyed by stable
identities (lot id `+0xC0/+0xC4`, story): manager, own floor object and grid, opening mask with a hash, rooms near
openings, a version; validated by about 8 reads per lookup; floor set or remove marks it dirty at once; `Cross` records
(lot id, story, version) instead of a raw floor object; `LotState`, the ambient merge and the lamp signatures keyed by
lot id; the floor registry re-appends an object reconstructed at an old address and prefers the most recently set own
floor. Phase 4, quality: merge only the lamp share of stacked rooms, give the game's other basis-reading shaders the floor
test, mode-1 furniture.

**Outcome:** the branch was not merged as written: `GameWouldTake`, `TakenByOtherStory`, `ForgetLot` and
`RigTracker::CollectRoomRigs` do not exist in the code at PR #2. Later candidates (test004 and test005 below) handle
manager changes through the lot state's story managers, per-lot cache forgetting on manager change and the room list's
manager comparison. F8 already lists every floor object per story. The per-story cache (phase 3) is not implemented.

### 2026-10-01: pending group ambient updates (`2.5.2-room-sync-pending-test`)

**Context:** session `2026-10-01 14-48-54`, lot `27D24720`, recognises 540 / 1356 / 4 openings in three snapshots. Before a
lamp toggle, the journal ends with rooms 19/20/3 near 0.017 and room 23 near 0.012; after switching lamps off all four
agree near 0.012. Lamp changes at 14:49:04.965 and 14:49:13.433 requeue 31 rooms.

**Finding:** the 3 s cooldown branch discarded differing group updates outright.

**Outcome:** the latest group target per room is kept until a merge confirms convergence; deadlines are checked on each
lot update; failed or waiting queue attempts retry after the cooldown; pending ambient counts as busy for the settle;
world, topology and uninstall resets clear it. A world-wide periodic refresh was ruled out as a replacement.

### 2026-10-01: live group background (`2.5.2-room-sync-live-test`)

**Context:** recording 15-06-08 in session `2026-10-01 15-03-22`: lot `27D24720` rooms 19/20 held their merged ambient
while the slider changed, although both were idle (state 5).

**Finding:** the merged colour no longer matched the original `BaseHook` result, so `MoveBase` refused the direct update
until rest.

**Outcome:** `StageAmbientBaseChange`, `StageUnlitAmbientChange` and `ApplyAmbientBaseChanges`, using cached group
membership instead of scanning tiles per slider update; unchanged unlit colours do not request a rig refresh.

### 2026-10-01: window activation recheck (`2.5.2-window-sync-test`)

**Context:** session 15-26-42 confirms rooms 23/19/3/20 change their merged ambient during a drag (from 15:28:39). The
15-30-33 snapshot has every ordinary lamp of room 20 at zero intensity but RectangleWindowLight `L3ACB0300` (#2326) lit
with effective white (1, 1, 1, 1). After removing and restoring a wall, the 15-37-13 snapshot shows the same light with
flag 0x20 cleared and colour zero (the background setting also differed slightly).

**Finding:** the residual bright room was a window light, not a frozen group colour. `FUN_006bdca0` alone only sets the
flag; the computed lit boolean comes from `FUN_006c7ba0`.

**Outcome:** bounded window-only re-evaluation through the game's evaluator. No night-time disable, no roof flag rewrite,
no forced wall edit.

### 2026-10-01: test001, faster switch reconciliation

**Context:** session 15-43-24, recording 15-44-07: room 20 finishes at 15:43:58.042, but the delayed whole-lot
reconciliation sends 31 rooms at 15:43:58.513; after switching on, it finishes at 15:44:03.042 before another
reconciliation at 15:44:03.190.

**Outcome:** pure on/off changes wait 120 ms instead of 700 ms (moves and mixed batches keep 700 ms); a switch-only
reconciliation may keep rooms already re-gathered after the switch (`NoteGatherStamp`). This removes 580 ms of debounce,
not necessarily 580 ms of visible delay. Settle time and frame cost were not measured. A later report said test001 felt
laggier in ordinary camera movement; the profiler run at 16:16:36-16:17:05 did not coincide with the switches. Hitch
#2562 took 33.95 ms (texture creation 27.20 ms, lot lighting 0.01 ms), #2571 24.79 ms (driver Present 19.67 ms, lighting
0.02 ms); other hitches were dominated by texture creation. This neither establishes nor excludes a lighting regression.

### 2026-10-01: test002, fresh group sources (rejected)

**Context:** session 16-11-41: room 20 had no lights at 16:11:58.747 but merged RGB (0.0422, 0.0402, 0.0384) while its
second ambient was (0.0287, 0.0287, 0.0287); the merge reached that at 58.856.

**Finding:** the candidate erased each room's cached source at every gather and merged only when all sources were
present. Recording `2026-10-01 16-37-06`: after a lamp change at 16:36:59, room 20 solved without a merge at 59.856 and
again at 16:37:00.028; at 16:37:04.497 rooms 19 and 20 were both at rest with different ambient (0.0512, 0.0532, 0.0486)
and (0.0525, 0.0543, 0.0515). Responsiveness was reported worse. The six readiness tests only checked the gating rule,
not the asynchronous scheduler. A separate wall investigation (session 16-18-44): the lower- and upper-wall probes at
16:18:48 and 16:18:51 use the same wall shader and PS c4 (0.05354, 0.05010, 0.05087, 0.05263); shared ambient explains
the upper wall's background, and the probes do not show a ray crossing a solid balcony slab.

**Outcome:** rejected and reverted to test001; the all-members-ready gate is not to be repeated.

### 2026-10-01: test004, reviewed synchronisation

**Outcome:** cached stacked-room sources kept; fresh post-switch gathers complete without restart; furniture refresh
waits for the tracked rooms with a bounded fallback; compatible colour-only updates applied before another native solve;
pending idle updates resume from a key-based cursor (128 visits, 16 writes per 50 ms poll); the stale-lamp scan keeps its
sent signature during states 1-3; development diagnostics stop collecting at capacity. Test001 hitch samples also held
texture creation and Present waits, so no overall performance gain was claimed.

### 2026-10-01: test005, coordinated floors

**Context:** session `2026-10-01 17-58-56`: unlit room 22 followed a brightness drag while atrium members 23/19/20
lagged, and ambient was inconsistent during a later drag. Code review found a retry gap for busy or unknown rooms and
global streaming cache eviction. The installed capture binary (SHA256
`677D742EB3DD9EDB5D02C7A0072184BD3055BF77EC4BAD8E72525C2510AAC1D3`) differed from the test004 baseline, and the capture
does not prove every cause.

**Outcome:** whole-group colour publication in the lot's room update (16 members per 50 ms, 1.5 s wait); caches forgotten
only for removed lots or changed managers; fresh cached room ids reused for complete relights; every floor of the
priority lot boosted with full detail (drain limits and lot budgets unchanged); the after-load refresh polls from 500 ms
with 250 ms of quiet and keeps the 8 s bound, with the room enumeration expired at world-live. Shipped in 2.5.3.

### 2026-10-03: lamp-change window burst (2.5.6 notes)

**Context:** the 2.5.6 notes described a window re-evaluation burst on changed lamp signatures (immediately, then at most
every 250 ms for 6 s). Session 00-29-16: a lamp switch at 00:29:34.970, terrain completion at 35.092 and two window
activation changes only at 43.163.

**Outcome:** that burst is not in the 2.5.6 code or at PR #2: the code re-evaluates windows at 0, 2 and 6 s after the
triggers listed in the feature page. Recorded here so the timing is not assumed.

### 2026-10-04: multi-story indoor openings (`2.5.6-indoor-stories-test1`, `test2`)

**Context:** a three-story open shaft: lamps two stories away were ignored.

**Finding:** test1 required one ray/plane crossing per nominal story boundary; ghost wall rows and split-level tile
heights can place a sample on the lamp's side of a boundary despite its room's story. Recording 16-45-19 confirmed shared
ambient RGB and normalisation on the red atrium's three rooms but did not identify every component of its visible seam.

**Outcome:** test2 keeps every floor test but builds wall segments from actual crossings, including a terminal
intermediate-story segment. Native range and attenuation unchanged; no brightness increase.

### 2026-10-04: indoor seam investigation and wall-seam recording

**Context:** session `2026-10-04 16-58-09`, recording 18-11-40, on test2: the connected rooms (story 0 room 23, story 1
room 19, story 2 room 20) converge to identical ambient, but their native wall passes finish separately; the walls were
still reported inconsistent.

**Outcome:** candidate `indoor-seam-trace` adds `Wall seams.csv` to ordinary recordings.

### 2026-10-04: indoor wall guard (`2.5.6-indoor-wall-guard-test`)

**Context:** after the seam build was validated visually, isolated captures showed a story-2 lamp leaking into furniture
behind walls. The ground capture also showed contracted regular-lot UVs sampling the indoor row outside a closed wall.

**Finding:** the directional basis builder performs no receiving-room wall test.

**Outcome:** borrowed lamps in the basis builder test source, intermediate and receiving wall segments, and partial
transmission scales only that lamp's contribution. For the ground, the exact recognised regular lot vertex shader
(`kLotLightVs`, `contractedLotUv` in `lot_light_bridge.cpp`) inverts the UV contraction for the CPU floor map when its map
constants (VS c12) are valid and square; unsupported layouts keep native coordinates. Neither change alters light
intensity, texture-read count, the world atlas or CPU light-map generation. The previously approved candidate and backup
368 are kept.

### 2026-10-05: raised-room object-map guard

**Context:** F7 23:54:37 samples a table at basis texel (15, 25): T9 is red there (74, 0, 0) while the rig constants are
grey. F8 23:58:17 identifies only lamp #2084, story 2 room 16, imported into lower stories. Its tower uses floor heights
3.24 / 6.24 while the story minima are 0.99 / 3.99.

**Finding:** nominal ray segments can miss an exterior wall belonging to the raised part of an intermediate story.

**Outcome:** the exterior-wall veto for basis-map lamps (PR #2, `2584e01`), together with the structure-change refresh
and the 250 ms floor-edit delay (was 1.5 s of quiet).
