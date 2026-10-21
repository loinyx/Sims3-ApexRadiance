# Faster Room Lighting: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/room-light-queue.md](../features/performance/room-light-queue.md).

### 2026-09-29: room lighting queue study

**Context:** five parallel studies (private research notes) of why rooms light up slowly.

**Finding:** from entering a lot to the last room solve took 12 to 36 s, with only 1.2 to 2.8 s of solve work in it; in
95 to 98% of the frames with solve work a single lot story was being solved. The cause is the game's queue, not the
solve itself.

**Outcome:** the five changes of the feature page; shipped on by default.

### 2026-09-29: adversarial review

**Context:** an independent review of the queue and of Apex's own requeues.

**Finding and outcome:** the drain runs only when the room current at the previous pick is done and the new one is of
that same lot and the priority lot. `QueueRoom` holds back only requeues after a setting or ambient change, never a
requeue caused by a lamp list change, and only while the room update can send it later; a whole-world relight asked
while the story share is off runs at once. In `level_light_share.cpp` the same day: no invalidation of the room being
solved, burst relights coalesced to one 250 ms after the last ask, the settle requeue armed once per lot state, early
return for a light the game is about to drop, and no relight of every lot 3 s after a world loads at night.

### 2026-10-01: all-floor priority (Test005)

**Context:** level light share's installed full-detail-all-floors policy.

**Finding:** while that policy is active, `PriorityHook` applies the x4000 boost to all floors of the priority lot,
including those above the camera. With the policy off, the camera-floor x4000 / below x2000 rule stays. Other lots and
zero native priority are unchanged; native priority differences remain within the boosted floors. The extra drain
stays at 4 ms still / 1 ms moving and the lot lighting budgets are unchanged. Ambient publication is coordinated
separately. No FPS improvement was inferred from these policy checks.

**Outcome:** kept; released in 2.5.3.

### 2026-10-05: light object removal returns at once

**Context:** commit `91160df`. An object removal walks a level's five light maps (0x6C7610) for twelve levels per
removal, also when they are empty.

**Outcome:** it returns at once when the five maps are empty. Released in 2.7.0.

### 2026-10-06: many lamps at once, the quick pass and Smooth light changes

**Context:** switching all the lights of a big lot (5 stories, rooms with 35 to 98 lights) took 3 to 7 s before every
room showed the new light. The notes of that day, kept as written in the feature page during development:

#### Many lamps at once (06/10)

Observed in game: switching all the lights of a big lot (5 stories, rooms with 35-98 lights) took 3-7 s before every room
showed the new light (the same house took up to 10 s on 05/10). Measured: the game's class-2 solve cost 24.8 s of the session
against 1.0 s for class 0.

- **Quick update for lamp switches** (`[patches.RoomLightQueue] quickPass`, menu "Quick update for lamp switches", default
  on, Experimental; once per room and switch): after any player switch (`LampMarkFilter::SwitchActive`: a lamp switched on
  or off where it is, not a light switching itself, not at dusk or dawn; held 1.5 s after the last, switches closer than
  that are one event, `SwitchEventId`), the priority hook puts a lamp edit's room that is still waiting (state 2) at a
  class above 0 back to class 0 before the game reads its priority. Class 0 is the game's own fast first solve and has 100x
  the priority of class 2, so every room of the switch takes its new light within a few frames, the camera's story first;
  "No middle step" then takes each room straight to its class. Until 06/10 evening only bursts of 3 or more lights
  (`MassSwitchActive`) took it: one lamp of the atrium had its 4 rooms solved at class 2 one after another, so the
  quick pass was extended to single-lamp switches. Drags and value edits never take it. Status line: "quick pass for lamp switches" and its
  room count; each lamp edit's log line ends with the rooms that took the quick pass.
- **No second send for waiting rooms**: the lamp switch's safety net (LevelLightShare::RelightLot, about 120 ms after the
  switch) skips a room that is queued (state 2) and has not started its solve, as it would skip a fresh solve: its gather
  reads the lamps as they are when it starts.
- **Refinement in the background**: once every room of the burst shows its quick solve (logged: "Many lamps: the N rooms
  of the burst showed their new light (quick pass) after X ms"), the extra solving a frame for the burst drops from 12 ms
  to 4 ms. Measured before (06/10): 35-39 ms of solving a frame for 2-3 s while the rooms already showed the right light;
  Apex's own light tests were 21-33% of those solves (the rest is the game's).
  Fixed the same day: the quick pass is seen as done when the room's class rose again (the end of its class-0 solve), not by
  "shown" (the step writes the new class there too, so the first version only saw it at the very end); and while refining,
  the lot lighting budget (LotLightingMotion's hook, which raised it to 25 ms for a lamp edit) is held at 6 ms.

#### Smooth light changes indoors (06/10, removed the same day)

Removed after the fixes below (the result looked better in game without it): the light changes at once; the module became AtriumHold
(`features/atrium_hold.{h,cpp}`), which only makes an atrium's stories wait for each other. History:
`[patches.RoomLightQueue] lightFade` (default on, Experimental), `features/room_light_fade.{h,cpp}`. In game the quick
pass showed as a blink (recorded video: the lower walls went darker for ~1 s, the class-0 solve has no wall blur, then the refinement
brightened them). The room solves write the story maps (MANAGED single-level A8R8G8B8: wall atlas, floor, ceiling, room light
map, basis maps) through LockRect / UnlockRect; both are detoured (vtable 19 / 20 of a probe texture). While a lamp edit is
pending, a map first locked during a room solve (the queue's current room in state 3, render thread: never the UI) is kept
(AddRef, two buffers): at the game's lock the exact content is put back (the game never reads a blend: the wall blur reads the
atlas); at its unlock the new content is the target and what was on screen goes back; every frame the smoothstep blend over
250 ms is written, ending on the exact content. Maps are released 3 s after their last change. Status: Developer page and the
Faster room lighting status line ("smooth light changes").

Also fixed the quick pass: a room sent back to its class by the switch's safety net before its class-0 solve ran is set
to 0 again until its quick solve is shown, and never after.

Its quick solve is shown when the room's solve ends (06/10 evening, from the first light update trace recording):
LevelLightShare's `FinalizeHook` (the solve's step 8, `0x006A0E00` called at `0x6A3E65`) calls
`RoomLightQueue::NoteSolveEnd`, and on the render thread a room of the burst counts as shown at the first solve end after the
quick pass set it to 0. The earlier test, "shown" (+0x100) is 0, was already true for a room still showing the previous
switch's quick pass while it refined: lights off, then on 2.6 s later, ground floor room 25 was set to 0, given its class 2
back by the safety net's invalidate 31 ms later, then taken as done, and it kept the lights-off light 3.5 s, waiting for its
class-2 solve (the lights-on log line said every room showed its new light after 313 ms). Without the hook (another game
build) the old test stays. Bounds: a room is set to 0 at most 3 times per burst; after 3 s the burst counts as refining even
if some rooms never ran (logged: "N of the M rooms ... showed their new light").

Every story together (06/10; observed: the light's story changed first and the others up to 0.5 s later): while the edit's
first solves run (the quick pass of a burst, or every room of a smaller edit; not its refinement, not a dragged lamp; at most
2.5 s) a changed map holds what was on screen, then every held map starts its fade in the same frame. Maps are kept only
for the edit's own rooms (the solved room must have lamp-edit urgency: other rooms, even of other lots, had filled the 128
slots). A burst is now 3 different lights switching within 1.5 s, even when each switched more than 3 times in 10 s (the
self-switching rule had turned repeated tests of "all the lights" into no edit at all).
That hold was turned off the same day (worse in game: the light's story waited for a cascade of 2-4 s). The causes were
found and fixed instead (below); only an atrium's maps wait now.

#### Every story at once: what fixed it (06/10, confirmed in game)

Recordings of a 4-story house with an atrium (rooms 23, 19 + 3, 20 on stories 0-2, 35-98 lights each):

1. **One round per atrium** (level_light_share, "One round for an atrium"). The stacked-ambient merge used the other
   members' values from their last solve, so the first member solved after an edit took a mixed target, the next another,
   and every normalisation change (compared bit-exact) sent every member to solve again: 2-3 rounds of the atrium's
   biggest rooms. Each member's ambient is now taken at the merge with the game's own step (`0x006A0F50`, fields put back)
   at the top class's light threshold (read from the game's table), cached per gather. A waiting member is no longer sent
   back to its gather by the ambient pass or by the lamp's safety net (that also threw its quick pass away).
2. **The quick pass looks like its refinement.** Class 0 tests no wall and no object and drops more faint lights
   ([room-light-maps.md](../engine/room-light-maps.md), "per-class switches"): with the lamps off, window lights lit
   walls through them, and the refinement then took that light away ("right, then wrong"). A quick-pass room takes the
   wall mode, pass switches and threshold of the class it will be refined to (the wall pass's through its table bytes
   around each step).
3. **A refinement's maps show at once.** They are the other class's maps, not on screen and still holding the light from
   before the edit; fading from them brought the old light back for a quarter of a second, room after room.
4. **An atrium's stories together** (AtriumHold). The atrium's members of an edit (any member urgent) are solved right
   after the lamp's room whatever their story; their new maps wait while another member is still waiting for or in its
   solve (at most 1.5 s; not while a lamp is dragged), then show in the same frame. Only atrium rooms' maps are kept. The switched lamp's own room is no longer solved twice
   when its gather fell in the change's tick (a gather serial now orders gathers and lamp marks).
5. **Loads** (entering the lot took long to correct; one log: 17 s after the world went live). The
   after-load refresh keeps every room gathered again since the world went live (it re-sent 75 rooms of 16 lots after the
   world-live round had settled them), and for 15 s after the world goes live the lot being played gets 25 ms of solving a
   frame with the camera still (the game's own budget while it moves; it averaged 4.4 ms) and the queue drains 12 ms.

Measuring tools that found these: the recorder's [solve] / [room] journal (F8), the "Wall seams.csv" of a recording (the
same wall points at class 0 and class 2), and frames extracted from a gameplay video with VLC's scene filter.

Then (same day), three further improvements:
- **The switch's safety net sends only the rooms a lamp can reach** (`LevelLightShare::RelightLampSwitch`): those holding
  the lamp, its own room, the rooms near the stair openings of its story and the stories next to it, and every story's
  outdoor rooms for an outdoor lamp (fresh solves kept). It sent the whole lot before (17 rooms for one sconce). A lamp
  moved into another room, moved and switched, or more than 8 lamps at once ("all the lights") still send the lot.
  06/10 evening: the lamp's story and room are its home (`LampHome`: room id light+8, the one the object rigs' gather
  compares, on the story whose lowest floor is the highest at or under the lamp's height +0x124); the lamp mark's room was
  only the first room the game marked (room 0 of every story in that recording), so every switch counted as an
  outdoor lamp and sent the outdoor rooms of every story. When only lamps switched off, a room whose light list is empty is
  left alone (none of them reached it: rooms 22 and 24 of the atrium house's ground floor were solved again for nothing).
  The log line counts both ("N with no light left alone", "M of them found in their own room").
- **Windows the game takes back are left alone**: every load, the window activation recheck changed the same 50 windows of
  the atrium house 2-3 times within half a second (the game set them back in between), and each change solved their rooms
  again. An entry found back in the state it had before Apex's last update of it (within 10 s) is now left as the game
  keeps it. The log line also shows the real lot id (it printed the tracker's +0x90, a float 1.0).
- **Slow terrain chunk renders are spread** (terrain_chunk_relight `ReleaseLimit`): after a chunk took more than 20 ms
  (44 ms in one session whose game frames took 72-164 ms; 4.7-6 ms in the others, same lamps and chunks), at most 3 chunks a
  second instead of 8.

Left as it is: the refinement of a burst ends room by room (now only a change of resolution).

The "Smooth light changes indoors" option was then removed (with it off the result looked better in game): its first reason, the quick pass's blink, was gone. An old `lightFade` key is ignored.

**Outcome:** the quick pass, the safety net that skips waiting rooms, the background refinement, the solve-end test, one
round per atrium, AtriumHold and the narrower lamp safety net were kept. Smooth light changes indoors (a 250 ms fade of
the room maps) and the hold of every story until the first solves ended were removed the same day; neither was released.

### 2026-10-06: lamp switches all at once

**Context:** commit `c635c53`. The light update trace (a recording with the lights on) showed the quick pass at about
0.3 s and each room then correcting itself at +0.8, +1.7 and +2.3 s, by up to 17 levels: class 0 samples 1 point per tile
and class 2 four (`0xFF36AC`), so an approximate map can never match the final one.

**Finding:** one change, when the final light is ready, was preferred over a faster approximate light with corrections.

**Outcome:** `switchAllAtOnce` (on by default, Experimental): every map, rig, ground map and per-pixel lamp list of a
switch waits for the final light and they change in the same frame; the quick pass stays as the option for players who
turn it off. Commit `645fc34` (pre-release review): solve starts stamped with a switch serial instead of a tick (a solve
begun in the switch's own 10 to 16 ms tick counted as before it and the switch waited the whole 2.5 s), maps kept from
the very frame of the switch and only when the game locks them in step 1 of a solve, at most 96 MB of copies, and room by
room without both solve hooks. Released in 2.7.0.

### 2026-10-12: lamp edits all at once, the lamp's own story free

**Context:** after moving, switching or editing a lamp, the stories showed the corrected light one at a time over 1 to
20 s: only switches were held, value edits and moves were not, the safety net's rooms were never held, an urgent room
lost its first place 3 s after its mark even when its solve had not begun, and the drain served only the priority lot.

**Finding:** the 2026-10-06 rejection (holding every story until an edit's first solves ended made the lamp's own story
wait 2 to 4 s) concerned the lamp's own story. Holding only the other stories keeps the edited story immediate.

**Outcome:** (not yet validated in game) the switch hold became an edit hold triggered by every player edit (switch,
value, move, drag release, a lamp into another story, a lamp added or removed). The lamp's own story (its own room's
story, `LevelLightShare::OnEditStory`) is never held; every other affected story, the safety net's rooms included
(urgent tier 2), is released in one frame once solved since its latest mark, at the latest 2.5 s after the latest edit or
4 s after the first (a drag: one hold per settled drag), or at once at the 192-map / 96 MB cap. The visibility filter of
the switch hold (`WallUpdatePolicy::VisibleForBarrier`) is no longer applied, since hidden stories are held too. To cut
the waits: an urgent room keeps its urgency while it waits (at most 8 s), an edit's room is drained on any lot and keeps up
to 6 ms of the lot budget while the camera moves, a move's safety net comes after 200 ms (was 300) and may run again after
500 ms for a relocation or many lamps (2 s for switches), and only a relocation resends every room (the F8 bug of
2026-10-05); other whole-lot refreshes keep their fresh solves. The gather countdown of an added or removed lamp
(`f65626e`) is unchanged. The menu row became *Lamp edits all at once* (same `switchAllAtOnce` key).

### 2026-10-20: automatic lamps first and a step budget (2.14.0)

**Context:** in a development measurement, frames of 15 to 21 ms occurred while the extra solves drained the queue: the
stopwatch is checked only between the game's steps, so "elapsed under 80% of the budget" still started long steps.
Lamps switched on automatically waited behind the rest of the queue.

**Finding:** one solve step can take several milliseconds; an edit's pacing must still follow the lamp.

**Outcome:** a step starts only while the running average of a step's cost fits (lamp edits, held edits and urgent
rooms excluded), and the after-load budget is 6 ms instead of 12 ms (`room_drain_budget`). An automatic switch-on puts
its own room first with no extra budget and no hold (`auto_light_priority`). Both are on in 2.14.0 with no setting.
Frame times in game are not yet measured.

## 2026-10-20: lot entry and other lots' urgency

**Evidence:** a 20-minute log on a 36-room, 6-story house at 3840x2160, 58 fps. Over 159 lamp-edit bursts the wall time
was 352 s and the solving 110.7 s; in the 31 bursts of 2 s or more, 299 s against 99 s. 199 of 326 outdoor lamp edits
were on other lots (lamps the game disabled or enabled, lamps moved into another room by the game), whose rooms took the
urgent tiers (x1e4 to x1e6) on any lot and became the world's current room before the played lot's. 38 of 134 atrium
holds timed out with every room still waiting. Entering the lot took 10 s and, coming back, 17 s, with the 4 ms drain
(1 ms while the camera moved): the settle window only followed a world load.

**Outcome:** an edit's room on another lot keeps an urgency of at most 1000 (below the played lot's camera story and the
stories under it), and entering a lot opens the settle window for 10 s. Rejected for now, without more evidence: not
resending rooms 0 when RtOutdoorLamps drops a lot's maps (their maps may lack the lamp edits the GPU stood in for), and
keeping the stairwell bounce context for a whole solve (a solve continued on another thread would keep a lamp's earlier
light).

## 2026-10-21: a recording with the lot-entry changes

**Evidence:** a 20 s recording on the same house (29 rooms, 6 stories). The cap on other lots' urgency acted (492
times), and other lots no longer went first. Remaining causes: the window activation recheck ran 9 passes that evaluated
144 windows and changed none, yet each pass marked the same 13 window rooms changed (about 117 invalidations in 20 s),
the main source of rooms solved twice or thrown away mid-solve; room 0 of the stories below the camera took the first
tier and went before the camera story's rooms (4.1 s of a lot entry); the game switching lamps on other lots kept the
edit pacing and the edit hold busy for 16 s; the load screen's lamps registering counted as a 16 s lamp edit.

**Outcome:** the recheck's room marks are dropped when the window did not change; room 0 takes the first tier only on
the camera's story; a pending lamp edit counts only the high-quality lot's rooms; lamps registering before the world is
live are no edit. Not changed: the atrium's stories keep their shared first place (they are shown together), and
OutdoorDelta's full solves after `steady_indoor_light`'s generation bump (`PhysicsKey`) need their own study.
