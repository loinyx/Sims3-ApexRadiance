# Light between stories: history

### 2026-10-19: replace lower-coverage geometry at the collection limit

**Finding:** the fixed scene filled with 8186 triangles from 23 groups, leaving insufficient capacity for an
1088-triangle stair draw recorded in an earlier capture. A published BVH did not establish coverage of that stair.

**Outcome:** bounded replacement ranks geometric coverage per triangle and stepped instance placements. It preserves
the original scene limits and exact triangle/gap tests. The native fixture reproduces saturation and replacement;
visual correction remains unverified. Raising the global triangle limit alone is not used.

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

**Outcome:** part 1 (sharing through `OutdoorGather`) and part 2 (JNZ -> JL cascade). Observed in game:
almost seamless.

### 2026-09-25: first version without a wall test

**Context:** the first version shared lights but did not test walls.

**Finding:** LightProbe `andar1-c` (m47, with m46): the side face of the ground story near a corner became lighter than
the upper one. F8 09:50, lot `C49C001BCF2DEA20`: the 19 lights of room 0 were right on every story, but a sconce near a
corner passed above the lower story's walls (`room+0x30` holds only the own story's walls; `FUN_0069aa90` has no base
test).

**Outcome:** part 3, the wall test of the lamp's story. Observed in game: much better.

### 2026-09-25: review of the first design

**Context:** two independent reviews the same day.

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
/ 45.92 for the three classes, the upper room's bottom row at the line (46.67). The first fix moved samples and
kept edge rows out of the vertical blur; with a sconce just under the line the rows next to the edges were raised by the
blur and the edges were not, giving a crease (east sconce column: slope 0.54/m under the line against 0.11/m above, 4.8x;
a blur continued across the line gives 0.44/0.19, 2.4x). Observed in game: almost perfect, with a very slight
difference left. The solid strip along the atrium's north wall (keys `00028019`) keeps its step correctly.

**Outcome:** second version: ghost rows and a combined blur for atrium groups. Room 0 deliberately left
out.

### 2026-09-29: cost of the per-point hooks

**Context:** standalone review of the point solve's cost.

**Finding:** the hooks called `GetCurrentThreadId` and recorded F8 samples in every lit cross-story evaluation.

**Outcome:** thread id read from the TEB; F8 sample recording only in the development build and only while armed; the
game's wall test wrapper (`GameWallTest`) only in the development build. Not measured in game.

### 2026-09-29: re-entering a lot and loading

**Context:** observed in game: the indoor light sometimes worked only after reloading the save, and was right only
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
switched off for about 25 s (journal: lights 20, 9, 4, then 2 only when another lamp was switched on). Observed in game:
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

**Context:** observed in game: the indoor light broke for a moment at every floor switch; the light should be kept
and changed only when something changes. F6 092629: every switch restarted room 0 of every story and every room holding a lamp
(`0x006C7451`, twice within 16 ms), solved again in 110-140 ms with the same ambient, light counts and normalisation.

**Finding:** two studies: Apex added one room of its own (room 14 of story 0, through `AfterChangedWalk` deps); the rest
is the game's entry update marking rooms without comparing.

**Outcome:** `lamp_mark_filter.cpp` (`MarkThunk`). Installed as build `81934361`; the earlier confirmed floor build and
the previous ASI were kept as local backups. A follow-up (build `5eba1685`) relights a lot when a lamp is switched or moved,
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
not necessarily 580 ms of visible delay. Settle time and frame cost were not measured. In a later game test, test001 felt
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
intensity, texture-read count, the world atlas or CPU light-map generation. The previously confirmed candidate is kept as a
local backup.

### 2026-10-05: raised-room object-map guard

**Context:** F7 23:54:37 samples a table at basis texel (15, 25): T9 is red there (74, 0, 0) while the rig constants are
grey. F8 23:58:17 identifies only lamp #2084, story 2 room 16, imported into lower stories. Its tower uses floor heights
3.24 / 6.24 while the story minima are 0.99 / 3.99.

**Finding:** nominal ray segments can miss an exterior wall belonging to the raised part of an intermediate story.

**Outcome:** the exterior-wall veto for basis-map lamps (PR #2, `2584e01`), together with the structure-change refresh
and the 250 ms floor-edit delay (was 1.5 s of quiet).

### 2026-10-05: walls block lamp light on outdoor floors and objects

**Context:** commits `bf0e71a` and `5fe54b4`. A wall lamp outside lit the floor of a yard without a roof behind the
wall, an upper deck behind its half wall, and the objects there: outdoor floors take `max(floor map, ground atlas)`
and the atlas carries room 0's lamps with no walls.

**Outcome:** the blocked share of room 0's lamps stored in the alpha of outdoor floor texels of stories 1 and up (the
floor atlas shader takes `atlas x (1 - alpha)`), and a copy of each story's outside walls for a render-thread segment
test on objects. Two switches under Lighting > Stories, both on. Released in 2.7.0.

### 2026-10-06: indoor objects take lamps of another story

**Context:** commit `b3741b4`. A light capture showed an atrium's upper window frame black (0.02), its rig holding only
fill lights, right over the lower frame lit by the sconce below: the game's rig gather takes only lamps of the object's
own room.

**Outcome:** `ObjectLightBridge` runs the game's gather once more for a room-mode rig with the list's lamps of other
rooms that reach its centre (`CrossLampReach`: share at least 0.25, within 15 m) and gathers those rigs again after every
lamp edit. Released in 2.7.0.

### 2026-10-06: upper half walls lit by a lamp of the story below

**Context:** commit `bec724d`. A ground-floor sconce left the upper story's half wall of a balcony dark beside the lit
wall next to it; in a recording all 21 half-wall points were blocked by the game's wall test and passed by WallPass.

**Finding:** the game's own wall test of the lit story (`GameWallTest`, CALL at `0x0069FE93`) has no wall base and its
soft mode shades rays passing near a wall's end.

**Outcome:** for an outdoor lamp of a lower story, that test runs from where the ray reaches the lit story's lowest floor
(`OutdoorEntry`), as for indoor rooms; a ray that comes up through a floor a player placed on that story keeps the
whole ray. Released in 2.7.0.

### 2026-10-07: avoid repeated exterior floor-link searches

**Context:** exterior wall corrections applied slowly while placing or moving lamps in Build/Buy. Terrain batches in
the reported scene completed roughly 100 ms after dispatch; baked wall solves involved substantially more work.

**Finding:** the lower-wall pass's last-manager shortcut misses repeatedly when rays alternate stories. Each miss
called `LevelFor` again, locking and walking the floor links even when `OutdoorEntry` or another lamp had already
resolved the same floor at that sample. The existing `LevelForPoint` cache is bounded to eight slots and expires with
the point generation; the original last-manager shortcut and owner checks remain.

**Outcome:** reuse that per-point lookup in the exterior lower-wall pass. Faster updates were confirmed in game in
the reported gameplay scene. No lighting calculation, quality, gather wait, queue order or time budget changes.
Released in 2.10.1; quantitative timing and wider-scene validation remain open.

A preceding broad exterior-priority experiment did not improve the reported delay and is not included. Its initially
reported colour concern was subsequently withdrawn; it is not recorded as a confirmed visual regression.

### 2026-10-12: spatial lighting and generation update preparation

**Context:** outdoor walls update between stories; fences and gates use different lighting compositions.

**Finding:** shared fence-group lamp brightness and directionless ground light on vertical object faces can obscure spatial lamp response. Deferred wall refresh requests can already be covered by a newer generation.

**Outcome:** retain spatial fence lighting with native fallback, per-pixel object cones, vertical ground-light isolation and generation-aware wall refresh. Offline CPU/GPU exploration remains research rather than a shipped companion DLL. Final release identity and gameplay coverage are recorded in the validation page.

### 2026-10-12: slabs block lamps below them; balcony share removed

**Context:** commits `2c6d41f` to `f134a51` (released in 2.10.0) added *Light behind balconies* (`luzSobSacadas`,
`forcaSobSacadas`): walls behind a balcony kept a soft share (35 to 45% at the edge) of a lower lamp's light, because the
wall behind a 3 m deep balcony had stayed black beside its lit neighbours.

**Finding:** snapshot 18-38-41: a red post stands 0.5 m behind a balcony's front edge, under the story-2 slab, so every
ray from its head to the story-2 wall behind the balcony crosses a placed floor. The share left a red pool on that wall
(red excess up to 68 at pixel (1459, 960), 79% of a directly lit ground-floor wall), and a window frame behind the balcony
was red too (77/13/13 at (1950, 800)): the per-pixel object lamps tested walls only (`WallBlocks`).

**Outcome:** the share is rejected. A placed floor of an upper story blocks a lamp below it, in the room solve
(`OutdoorEntry`, `LampVis::SlabGate`) and for windows, objects and fences on the render thread (`LampVisibility`, over a
copy of each story's placed floors); a ray passing a slab's edge keeps the share of a 0.3 m lantern. Both settings are
removed; their keys are ignored when a configuration, profile or preset is loaded. Objects and windows standing above
every lamp head around also lose the ground term (the height gate fences already had). Not yet checked in game.

### 2026-10-13: wall openings filed under the posting lot's id

**Context:** a roofed arcade on lot `B1280029950F0CA0`, streamed in after the world, showed no opening. Its log had
every checked cutout post marked as differing from its story manager's lot id, and no opening on the story's grid. The
lots loaded with the world were not affected.

**Finding:** the capture filed cuts under the story manager's `+0x90/+0x94` id. The first post whose manager carried
another id made a permanent alias and moved every earlier cut of that lot under the manager's id, and the grid asked
for the id its manager carried when it was built. Unverified which read was wrong; most likely the manager's id, read
while the lot was still being built. The
payload's id is the posting lot's `+0x48/+0x4C`, the id the game routes the message by (`0x00C52AA0`). Rejected posts
were not logged with a reason, and a 300-line log cap was used up by the lots loaded with the world. Story 0 had a
grid only when it held a roofless room: grids were built only for roofless rooms and for the stories lit from below.

**Outcome:** cuts are filed under the post's own id and looked up under the id of the lot that owns the grid's story
manager (its floor's owner lot `+0x214`, `+0x48/+0x4C`). The manager's own id is accepted only when it is not 0 and a
post carried it. Room 0's gather of any story builds the grid of a story with openings, in a cache of its own, so no
lighting changes yet. Developer mode logs the first 10 posts of each lot with their rejection reasons and each lot id
whose manager carried another id; F8 lists the cuts per lot and story and where each loaded story looks them up.
Not yet checked in game: that the owner lot's id equals the posts' id on a lot streamed in late.

### 2026-10-13: outdoor light through openings into roofed rooms

**Context:** a colonnade on lot `B1280029950F0CA0`, story 0: the inside of its roofed room (room 48, the columns' room)
stayed dark next to a garden post (type 11, room 0) that stood in front of its arches. The room's list held its own
lamps only, and the F7 of a column listed the room's lamps without the post.

**Finding:** a roofed room gathers only the lamps registered for its id (only room 0 takes a story's outdoor lights,
`0x006C6B0D`), and the game's 2D wall test sees whole wall runs (an arch's wall blocks like a solid one). The object
rigs of the room have the same limits and more: the room-list gather `FUN_006bb2f0` skips the street-lamp class
(`vfunc+0x20` true) and pops a candidate whose ray from the rig's centre crosses a wall of the room (`0x0069D4C0`,
`0x006BB3B7`), decompiled 2026-10-13.

**Outcome:** with the new option *Outdoor light through openings* (on in the 2.12.4 development branch) a roofed room
with a door, window or arch noted on its outline toward room 0 or a roofless room takes room 0's lamps of its story that
see one of those openings, tested per point through the openings' holes and under its roof, with the game's wall test
of those lamps skipped, as the yards' lamps are. Its objects take them through the game's per-light test
`FUN_006bb270` alone, scaled by the same share. A room with no known opening takes none. The offline check written for
it found a half-dark line where a ray crossed the seam between two cells of one wide opening (the soft rim was applied
on a side with no jamb); that rim is gone. Not yet checked in game.

### 2026-10-13: openings found on streamed lots; light through the outside walls' openings

**Context:** with the build above in game (log 10:42), the colonnade lot `B1280029950F0CA0` built its grids of stories
1..4 with no wall edge (79 / 98 / 38 / 8 noted cuts with no wall) while it streamed in, and no room of those stories
gathered again; once its walls were readable its grids looked their cuts up under its owner lot's id `3647CD2033346670`
and found none (the same on lot `7975270033346670`, whose posts carried `B128002B8215A110`). Every roofed room logged no
door to the outside. With a garden post moved inside the arcade (F8 10:44:26: type 11, room 0 of story 0), the grass in
front of the arches took hard lit rectangles and the columns, the facade and the ground further out stayed dark.

**Finding:** the owner lot's `+0x48` is not always the id the posts carry; the story manager's `+0x90` was, on both lots.
A grid built before its walls are readable was kept until something sent its rooms again. The post is an outdoor lamp
(room 0): the ground shade, the objects and fences and room 0's own solve tested it against walls that the copies and
the game's 2D test both saw whole, so its light left the arcade only past wall ends.

**Outcome:** the cuts are looked up under whichever of the two ids a post carried; a grid with no wall edge on a story
with cuts or roofed tiles is not kept and its rooms gather again every second, at most 12 times. The outside walls'
copies carry each wall's openings and half-wall cells from the story grid, used by objects, fences, windows, the object
rigs and the ground shade, and room 0's wall test lets a ray through when the grid walk passed an opening. Opening
shapes stay the kinds' defaults: no resource read path exists for the cutout masks. Not yet checked in game.

### 2026-10-13: roofed rooms on a foundation take the garden posts of the story below

**Context:** with the build above in game (log 11:18 to 11:21, F8 11:20:15 and 11:21:29, F7 11:20:10 to 11:21:31) the
post inside the colonnade of lot `B1280029950F0CA0` lit the grass through the arches, but the columns and the floor on
the foundation stayed dark. A mismatch between the story in the cut posts and the lighting stories was suspected.

**Finding:** the cuts are filed on the right story. That lot's cuts sit on stories -1 and 1 to 4 and every one of them lies
on a wall edge of the grid of the same story (story 1: 79 of 79, story 2: 98 of 98); story 0 has no cut. As noted for
foundation houses in [room-light-maps.md](../engine/room-light-maps.md), the ground floor on the foundation is story 1:
its colonnade rooms 4, 7, 11 and 16 found 13 to 16 doors each (the arches) and considered 0 lamps, because the post is a
room 0 lamp of story 0 and only the room's own story was walked.

**Outcome:** a roofed room of story S also takes room 0's lamps of story S - 1 that see one of its doors past the walls
of both stories, and per point they are also stopped by the floors of story S (the slab gate; the objects use the slab
copy). Lamp edits on a story also send the roofed rooms of the story above. The floor on the foundation stays dark when
the bulb is under its plane. Not yet checked in game.

### 2026-10-13: doors block lamp light

**Context:** the openings of walls were all open holes, doors included, while the game keeps a door closed except during
its open animation: a garden post lit a room through its closed front door.

**Finding:** every post carries the product's kind (`+0xD0`); in the logs of 13/10 every post was a door (`00000002`,
`80000002`), a window (`00000004`) or an arch (`00000100`).

**Outcome:** a new option, *Doors block light* (`portasBloqueiamLuz`, default on): the story grids leave doors, gates and
cuts of unknown kind closed, so the yards, the roofed rooms' doors, the outside walls' copies (objects, the ground shade,
the rigs) and room 0's wall test all see the same holes; arches and windows stay open. A change sends again only the
stories with doors. A door seen open during its animation and glass doors are not modelled: the first belongs to
per-pixel lighting on the GPU, the second needs the cutout mask's shape. Not yet checked in game.

### 2026-10-13: walls ending at a floor above in the story grids

**Context:** a colonnade on a foundation (lighting story 1), a garden post on the terrain in front of its arches: the
floor, the columns and the back wall inside stayed dark (F7 of the floor: the room's light map; of a column: its rig with
only the fill light). The post's head stands 0.5 m over the foundation's top.

**Finding:** the story grids gave the foundation's sides, walls of story 0 drawn only up to the floor on the foundation,
a full story's 3 m. The roofed room's per-point test of a lamp of the story below walks that story's grid, so every ray
from the post to a point inside crossed a side under its top; the rigs' copy did the same. The render copy and the room
solve's wall test already capped such walls (`CapFoundationSides`, `PassesOverLowerWall`). Separately, the colonnade's
gather took the post at 11:48:25 and lost it at 11:48:29 with the same grids of its story and of the story below ("no
door in sight"): only the story above's grid can have changed between them, and a wall of that story whose drawn foot
reached past its floor was asked as a wall of the story below.

**Outcome:** the grids cap such walls at the floor beside them (`CapLowerWalls`) and ask the story above only above its
floor. The gather's log names the grid that hides a lamp's nearest door. Not yet checked in game.

### 2026-10-13: indoor light through openings

**Context:** a lit room behind an arch or a window left the terrace, the garden, the outside walls and the next room dark:
the game gathers into a room only the lamps registered for its own id, and room 0 only room 0's.

**Outcome:** a new option, *Indoor light through openings* (`luzInternaPelasAberturas`, on in the 2.12.4 development
branch): room 0, the roofless rooms and the roofed rooms take the lamps of the story's roofed rooms that see an opening on
their outline into those rooms, each lit per point only through the opening and past every wall of the story; room 0 of
the story below takes those shining out of the story above. The objects and fences outside take them as per-pixel lamps,
a roofed neighbour's objects through their rigs. One opening only. Offline: `tools/roofed_openings_test`. Not yet checked
in game.

### 2026-10-13: the balcony outside a lit window dim

**Context:** a floor lamp inside an upper-story room beside an arched window; the balcony floor outside stayed almost dark
(F7 of the floor: the outdoor floor draw, the floor map's own light used).

**Finding:** the story's floor map held the patch through the window (room 0 took the lamp through the opening, the
`[InnerLamps]` lines), 0.20 at the F7 point 0.45 m outside, about half the room's own floor at the same distance from the
lamp: the game multiplies an indoor room's sum by its normalisation (`room+0x160`) and room 0's by nothing. The strip
within about 0.3 m of the wall was dark under the window's default sill (0.85 m; no cutout mask is read).

**Outcome:** a lamp shining out of a roofed room is scaled by its room's normalisation over the solving room's (1
outdoors). Offline: `tools/roofed_openings_test` (the normalisation checks). Not yet checked in game; the window's real sill
is not known.

### 2026-10-13: the columns inside an arcade brighter than those outside

**Context:** at night a garden post on the terrain in front of a colonnade on a foundation: the columns inside, seen
through the arches, were almost white, far brighter than the outer columns and the facade facing the post.

**Finding:** the rigs of the roofed room took the post through the per-light test with the game's colour raised by the
ground footprint's boost (`BoostRec`, the lamp's whole colour x the object strength within half its bounds), while the
outer columns are drawn with the per-pixel lamps on the outdoor walls' reach law and scale: at 6 to 10 m the boost is at
least 6x that law.

**Outcome:** the record of an outdoor lamp taken through openings is the per-pixel law at the rig's centre
(`LotLightBridge::ObjectLampColourAt`), times its share through the openings. Offline: the rig's colour equals the shader's
steps at the same point within 1e-5 and the wall texels' law beyond r0 (`tools/roofed_openings_test`). Not yet checked in
game; the rig shader's own use of the record (N.l per vertex or per pixel) is not compared.

### 2026-10-13: a lamp put down after a drag left the next room dark

**Context:** a floor lamp moved inside a room next to a doorway; after one release the next room stayed fully dark. The log
showed room 56 (the next room) and both rooms 0 gathering with no lamp considered while the lamp was lifted; room 0 gathered
again 180 ms later and took it back, room 56 never did.

**Finding:** the game takes a dragged lamp out of its room's registry until it is put down. The sends after the drop
reached only the rooms holding the lamp (the move's safety net) or, for a value change, the rooms that took one of its
lamps at their last gather: a room that lost the lamp was in neither.

**Outcome:** a room that misses a source room's lamps it took before gathers again after 0.7, 1.5, 3 and 6 s while it
keeps missing them; a value change of the source room sends it too; a lamp's switch or move sends every room with an
opening into its room that holds none of the lamps. Offline: `tools/roofed_openings_test` (the retry list). Not yet checked
in game.

### 2026-10-13: penumbra from the lamp's size

**Context:** the light of a lamp through an arch into the next room ended in a hard, stair-stepped edge on the floor. Every
opening, wall top and wall end had a fixed 4 cm rim (the stories in between 0.3 m, a slab's edge a 0.3 m lantern), so an
edge 3 m behind a jamb was as sharp as one at the jamb, and on a light map of 4 texels a metre a 4 cm edge draws as texel
stairs.

**Outcome:** every edge takes the penumbra of a lamp of radius R (*Shadow softness*, 0.15 m by default): half width
R (1 - t) at the crossing, measured on the wall by the ray's slant, spread over the cells beside where the wall goes on and
past a wall's free end, never across an L or T joint; on a light map at least 1.5 texels at the point. Offline: the
penumbra 10-90% width behind a jamb 3 m from the lamp is 0.027, 0.067, 0.133 and 0.265 m at 0.05, 1, 2 and 4 m behind it
(linear, sharp at the jamb), the light rises monotonically, no step at the cells' seams, the outside walls' copy within
0.03 of the grid walk, no light through a closed room's corners at R = 0.5 m (`tools/roofed_openings_test`). Not yet
checked in game.

### 2026-10-14: soft edges of the lot ground shade

**Context:** in game the shade walls cast on lot grass and paving next to an opening ended in steps of the shade mask's
texels (4 a metre), while the room maps' edges had the lamp's penumbra.

**Outcome:** the mask (`ground_lamp_shadow.h`, `Grid::soft`) takes the same penumbra: at each wall's free end, top and
opening the ray's spot of the lamp's ball (*Shadow softness*) is spread over the wall's run, at least 1.5 mask texels at
the ground; a joint or a corner keeps the shadow whole and the base stays hard. Built a slice at a time as before (more
tests per texel, no frame cost beyond the slice budget). Offline: the edge 3 m behind a free end is 0.32 m wide (10-90%)
instead of hard, monotonic, no light through a joint (`tools/roofed_openings_test`). Developer Tester switch *Soft ground
shade*. Not yet checked in game.

### 2026-10-20: mesh blockers retired from indoor light

**Context:** in the 2.14.0 development builds the arch seam came back a few seconds after it looked right, and stairs
changed colour by themselves. A recording showed every room of the lots solved again every one to two seconds (504
invalidations in 13 s) with no lamp change.

**Finding:** the opaque mesh set collected from the screen followed the camera: its budget is by on-screen coverage, and
each of its 54 publications relit every room near an opening. Its test was hard, so a stair cut the light where it
entered the set and stopped cutting when it left. The sample points of the stair's tiles (0.5 m and 2.0 m over the
floor) also lie under the stair's own treads, which blocked a sconce above.

**Outcome:** indoor light through openings and between stories no longer tests the set, and its publications relight
nothing (`steady_indoor_light`, shipped on in 2.14.0). With that behaviour the draw hooks read no mesh buffers and the
set is dropped once. After each lamp edit the rooms near openings are sent again once, after the lamps have been still
for 400 ms, which restores an update of the story below that the periodic relight had been providing by accident.
The hard shadow of stair treads is lost. Details of the arch side are in
[night-lighting-arches.md](night-lighting-arches.md).

### 2026-10-21: receiving-story wall veto

**Context:** a light capture of room 7 on story 0 showed a story-1 lamp over an atrium lighting a 2.6 m wedge of the
story-0 floor through two full walls: the walls of the room under the opening and of its neighbour. The partial
adaptation then raised room 7 by x1.785.

**Finding:** the game's 2D wall test that follows Apex's checks in the point solve covers only the solving room's own
walls, and softly. Nothing tested the other walls of the receiving story between the opening and the point.

**Outcome:** the stretch from where the ray enters the receiving story through the opening to the point is tested on
that story's wall grid (`GridWallHitHard`, under `indoor_lamp_walls`) in `IndoorPassImpl`. It is a veto only: a ray
that crosses a wall is stopped, and every other ray keeps the game's answer. Visual confirmation in game is open.

### 2026-10-22: room solve hot spots

**Context:** commits `522ef32` and `7d69737`, from Frame Profiler samples inside the game's room solve. Under
`/fp:precise`, `std::sqrt(float)` and `std::fabs(float)` were calls to the CRT and among the hottest Apex samples; the
lamp position cache had 8 slots tried in turn, filled round robin, while a room sees up to 32 lamps; and the window
light's evaluation asked for every sample whether the light belongs to channel 1, an object lookup by lot id each time
(150 to 180 thousand lookups in one room-solve frame).

**Finding:** `sqrtss` and a sign-bit mask give the same bits as `sqrtf` and `fabsf` for all 2^32 float inputs, NaN
payloads included ([validation](../validation/night-lighting-level-light-share.md#latest-results)). Within one sample
batch on the light tree thread the light, its lot and the lot's room table do not change, so the channel answer cannot
change either.

**Outcome:** `ApexMath::Sqrt` / `Fabs` in the solve's hot loops, a 64-slot position cache with one slot per light, and
the channel answer kept per light and channel for the batch (Tester switch *Window light channel memo*). The light is
unchanged by design; gameplay timing is open.

### Outdoor gather queue recovery

**Context:** returning from Build or Buy mode leaves the lot-thumbnail task waiting for final lighting.

**Finding:** the ground outdoor room remains in gather state 1 with countdown 5 and readiness state 0, while its changed and pending sets are empty. Native world updates continue. The originating invalidation remains unidentified. A native gather erases its pending key after the gather call, so a same-key requeue during that call is one reproducible model of the lost entry, not an established origin in gameplay.

**Outcome:** restore the missing pending key on the native tree-level update thread. Native collection and solving complete the work. The extra-update recovery is rejected because normal updates already continue. The reproduced Edit Town transition completes with the queue correction.

### Release 2.15.6

An outdoor room can lose its key while unrelated rooms remain in the two work sets. Recovery now tests that key's
membership in each set and honours the native insertion result. Invalid membership remains a reason to abstain.
Broader solve-context caching and forced completion are not introduced. Lamp evaluation reuses the exact SSE
square root; wall blur avoids clearing pixels that are immediately overwritten.

### 2026-10-26: an outdoor sconce brighter indoors than outdoors, and the camera's cut posts

**Context:** a player's light capture and snapshots: a green wall sconce (`L19ABA670`, type 5, intensity 1.7, colour
(0, 1, 0), story 0, room 0) on a free-standing courtyard wall lit the courtyard faintly and the kitchen behind a glass door
in saturated green (screen 0.808); the kitchen's light also changed with the camera.

**Finding:** the courtyard's lot pass draws the lot map x 0.75 x 0.756 = 0.567, cut at 1 per channel; the roofed room took
the lamp twice (its weight in room 0, as intended) with no outdoor scale and no cut, its normalisation on top, and the
room's mean-based curves let a one-channel colour grow three times further than a white one. Glass (0.765) and the
opening's share applied; the lamp was not counted more than in room 0 and the reach law is for walls only. The camera
dependence: the game posted the lot's cuts again on camera turns, and the window cuts closed and reopened 0.18 to 0.67 s
later, so the roofed room gathered with 2 doors, then 5.

**Outcome:** outdoor lamps through openings take the outdoor cut and scale over the room's normalisation
(`opening_lamp_balance.h`); a cut's close counts after 2 s (`cut_close_hold.h`). Offline checks in
`tools/roofed_openings_test`. In-game confirmation pending.

**Follow-up:** a later capture showed the camera keeping a wall's window cuts closed while it stays (`windows 6 -> 3`, 18
closes past the hold), the kitchen again gathering with 8 or 5 doors. Windows now count by the window built: a close
marks the cut, and the grid keeps it while a window light stands in its cell (`KeepBuiltWindows`); doors, arches and
gates keep the hold. In-game confirmation pending.

### 2026-10-26: the red sconce yellow in the courtyard, red behind the glass

**Context:** the same courtyard with the sconce turned red (`L39F18C40`, colour (1.7, 0, 0), story 0, room 0, listed
twice): the kitchen behind the glass red, the courtyard ground by the lamp and the lattice wall carrying it yellow and
cream (F7 13-00-02, pixel 2161, 1026: screen (0.557, 0.498, 0.427)).

**Finding:** the pixel is the terrain (draw #17) with the lot pass (#29, Apex's). Its lot map (Real-time outdoor lamps'
copy) held (1, 1, 0.41) by the lamp; the shade is 1 in the courtyard (0.31 behind the decorative wall, which blocks the
lamp correctly), the ground atlas holds the lamp's red, and the alpha E is at most 3/255, so the indoor light on the ground
(`ex`) is at most 1.3 % of the map and luzInternaSomaNoChao on or off gives the same pixel. The red lamp adds no green: the
yellow is light baked in room 0's sum around the glass doors and windows, at least 1 in green, and the lamp's red, many
times 1 in red, was cut at 1 channel by channel, so the weaker yellow decided the colour.

**Outcome:** outdoor texels keep their hue past 1 (`OutdoorDeltaModel::KeepHue`, the solve, the deltas and the compose
shader alike); the same rule the per-pixel object lamps took in 2.15.2. Offline: `tools/outdoor_delta_test`,
`tools/rt_lamps_test`. Unverified in game; which baked light is yellow (the windows' type 7 lights or another) is not
read.

### 2026-10-26: light past 1 outdoors, the strip behind a screen wall and a window's light cut short

**Context:** with the courtyard's sconce green, the room behind its glass door still looked brighter than the courtyard
(F7 sessions of the living room and the kitchen). The light reaching each floor, albedo taken out, was 0.50 inside the
window against 0.567 outside, the outdoor map's cut at 1 times *Ground brightness* x *Lot lamp brightness*: inside got
less light, but the courtyard by a 1.7 sconce was flat at the cut for metres while a pale carpet reflected more than the
stone.

**Finding:** the cut is room 0's 8-bit map itself; no world atlas or terrain map takes room-0 texels (an inventory of
every writer and reader of room 0's texels). Real-time outdoor lamps already compose each texel from its whole sum.

**Outcome:** the outdoor scale ([rt-outdoor-lamps](../features/night-lighting/rt-outdoor-lamps.md)): a second render target
of the compose draw holds `log2` of the factor the hue-kept pack took off, read by the lot pass, the summer outdoor
floors and the ps_3_0 outside walls. Rejected: a soft-knee encoding of the map's own colour (every reader, including
the game's unpatched shaders and fallbacks, would have read compressed values above the knee); 8-bit scale steps of
0.8 % show no banding where 7 % steps of a knee would.

**Follow-up 1:** behind a lattice screen wall carrying the sconce, a green strip lay along the wall's foot (F7, "the light
passes under the floor"). The room-0 solve does not test that wall for this lamp (the game's test does not run for it,
F8 F7 point), and the lot pass shades the ground with the walls' mask, soft over a texel at the foot: 0.3 to 0.5 there,
once multiplied by up to 6 times. The part past 1 now takes the shade to the 4th power.

**Follow-up 2:** the walls stayed at the cut beside a ground past it; the wall atlas got its scale (through a scratch
map, the atlas being blurred) and the ExteriorWall shaders a copy that reads it.

**Follow-up 3:** a living room behind a courtyard window took the red sconce only in a strip 1.75 to 2.75 m in at 8/255
and nothing past it (F7 and the map). The sill (0.55 m, sized from the panes) shades the first 1.3 m correctly for a lamp
4 m away at 2.2 m; past the strip, the soft light cut compared the colour scaled after the law (the opening's cut, the
outdoor factor, the colour response) with the room's threshold of 3/255. The cut now uses the threshold times those
scales; the light then fades to the back wall (map after the change: 9 down to 4 levels from 1.5 m to 4 m).

**Follow-up 4:** a wall over its sconce stayed dark beside a perpendicular wall lit head on (F7 session of the story
above). Measured on screen, the wall above was about 80 levels against 100 on the side wall, the ratio N.l and distance
give: no extra blocking. On request, wall lamps now light their own wall with N.l at least `0.5 x (1 - plane / 0.4)`
(walls.md).

### 2026-10-27: old lamp light left on the courtyard after the lamps went off

**Context:** after recolouring the courtyard's sconces several times and moving one, the player switched every lamp of the
lot off; the courtyard stayed green-white until *Restart lighting* (F8 with every lamp at intensity 0, F7 of the ground).

**Finding:** the lot pass bound the relit copy of story 0's floor map, which held (130, 255, 134) with its outdoor scale up
to 2.1 at the courtyard. The last full class-2 solve of story 0 came while the green sconce was on; every later edit took
the Instant path that keeps the record as it is. So the light was in the record's base (the sum less the drawn lamps'
parts) and in no drawn lamp's part. Which path put it there was not found in the code.

**Outcome:** a solve is kept only when its lights' parts account for its sum (2 levels at a texel). In game the check has
not refused a solve (21 kept solves over recolours, moves and switches, each logged in development builds with its drawn
lamps' parts), and the stale light did not come back. The cause stays open.

