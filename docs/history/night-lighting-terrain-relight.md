# Terrain relight: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/terrain-relight.md](../features/night-lighting/terrain-relight.md).

### 2026-09-24: load rebuild and the automatic unlock (combined build)

**Context:** the first terrain relight armed a full rebuild 15 s after "world loaded" (reason "carregamento") and an
automatic "lights changed" kick whenever +0x38 was armed. The `LightProbe-grama3-escura` session log (23:54) showed 16
rebuilds per session, "luzes mudaram" about every 30 s.

**Finding:** the automatic unlock reacted to any armed +0x38, including street lamps of lots streaming in as the camera
moved. The earlier notes' statement "rebuild happens at dusk; needs +0x38 and +0x3C armed" (section 2) is why `Kick`
arms both.

**Outcome:** the unlock fired only when `g_lotLampArms` changed, at most once per minute; later superseded (arms are now
only counted). Do not rebuild on lamp changes that streaming produces (NOTAS 1c).

### 2026-09-25: load delay, lamp signature and lot relight loop (combined build)

**Context:** snow took more than 20 s to look right after a load (16:25); version 5.4 of the lamp tracking.

**Finding:** the load rebuild moved to 5 s after loading. The lamp signature included the position (recalculation 0.7 s
after a move) and the safety recalculation went from 60 s to 15 s. Relighting lots after every rebuild made a loop
(relighting re-registers lot lamps, which re-arms the countdown) that kept invalidating the slow high-quality lot solves.

**Outcome:** lots are re-solved only after the dusk rebuild and only with `recalcularLotesAoAnoitecer`. The fixed load
delay and the signature were superseded on 28/09.

### 2026-09-28: story gate (combined build)

**Context:** captures probe2_clara / probe2_escura and S3SS_LightDiag: a straight cut along the border of some lots that
no terrain rebuild changed.

**Finding:** after the visitor, `FUN_00C292B0` drops every light of a lot (lot id != 0) whose story light+0xD0 != 0
(`cmp dword [edi+0D0h],0; jnz skip` at 0xC294D9, EDI = light). On a house built on a foundation the ground floor is story
1, so its outdoor wall and porch lamps (room 0, lit, d0 = 1/2 in the diagnostic) lit the lot grass through the lot light
map but never the world grass. Evidence: lot 6C11001B51A4D8B0, origin (900.8, 59.41, 1185.6), rotated 20 degrees; lit
pixel at local x +0.70, dark at -0.05 (the exact border); all lit lot lamps type 4 with d0 = 1/2; atlas 0 there. This is
why "Rebuild terrain light now" changed nothing and only some lots had the cut. The bake keeps a light when
`GetLotID() == 0 || light+0xD0 == 0` (`re/out/fn_00c292b0.c` line 114).

**Outcome:** the combined build replaced the 7-byte `cmp` with `call BakeLevelStub; nop; nop` (context checks: 11 bytes at
0xC294CE `8B CF E8 4B 2B A9 FF 0B C2 74 0D` = `mov ecx,edi; call 0x6BC020; or eax,edx; jz keep`, 6 bytes at 0xC294E0
`0F 85 D6 00 00 00`), `BakeLevelTest` keeping story 0, and stories above 0 only for lit outdoor lot lamps; basements
refused; counters "ground story / lot lamps / upper stories / refused". `BakeLevelStub` preserved every register and
set ZF with `cmp al,1`. The standalone does not contain it: lamps on any story reach the bake through
`SplitLevelGroundLight` (GetLotID zeroed, [level light share](../features/night-lighting/level-light-share.md)). With
the official Split-Level Lighting Fix on, the gate was short-circuited and basement lamps accepted by the visitor were
baked although the reconciliation refused them (inferred from the decompile). In the atlas captured at 12:05 even story-0
lot lamps did not appear; the counters were added to find where they were lost, never checked in game before the
freeze.

### 2026-09-28: relight reconciliation (combined build)

**Context:** the per-event triggers missed lamps that register unlit and switch on later (dusk, scripts, automatic
lights) and lots whose lamps finish loading after the load rebuild: those lots kept a cut until the button.

**Finding:** the combined build added `ForEachOutdoorLotLamp` (every lot light of type 3..6 or 0xB with raw values,
refreshed every 20 frames), `Bakeable` (type 0xB story 0 only; lot lamps with room 0, flags 0x01|0x04|0x40|0x20 and story
0 or the story gate), `g_baked` / `g_current` (`CollectCurrent`, max 8192 lamps) and `Reconcile` about once a second
(not while a full rebuild was pending nor before the world's first): gone / dark / left the bake -> old rect; new or on
-> new rect; moved more than 5 cm -> both; colour, intensity or range more than 2% (abs 0.002) -> both, value-only
changes at most every 5 s; more than 2048 rects = overflow. The local path (`relightLocal`, default on) called
`SmoothStreamingRelightTerrainRects(rects, n, 0.4)`: +0x55 only on chunks whose bake rect overlaps a rect + 1 m
(-1 terrain unreadable, -2 more than 40% of the chunks), queued through Smooth Streaming's terrain spreading (released
nearest first, "Terrain chunks rebuilt per frame" default 2) or set directly; it resolved the WorldManager itself
(`mov eax,[0x011ECBC4]` in the Smooth Streaming budget code, `8B 4E 58 E8` at 0x00C6D68C, a call target outside TS3W
accepted for the Frame Profiler). The bake rect was the cell record `FUN_00C2A470` returns. Fallback: a full rebuild
at most every 15 s. `Settle` after a load hashed the lamp list (pointer, x and z at 0.1 m) every 500 ms and rebuilt once
when quiet for 3 s, at least 5 s after the load (at most 60 s), by day too.

**Outcome:** not carried into the standalone. +0x55 bakes each flagged chunk synchronously, so the reconciliation's
local relights caused 12 to 60 ms hitches and its per-second checks rebuilt on streaming churn
([changes-since-0.1.0.md](changes-since-0.1.0.md) section 2.1). Smooth Streaming was removed
([removed-features.md](../removed-features.md)).

### 2026-09-28: standalone baseline and triggers

**Context:** the standalone started from v0.1.0: visitor 0xC29626, the three arm sites, the dusk kick, the experimental
switches and the developer buttons, with full rebuilds only. v0.1.0 had a load kick 5 s after "world loaded", a lamp-edit
kick 0.7 s after an existing lamp's signature changed, and, at night with c38 == 0 and c3C <= 0, a "lights changed"
kick after 120 frames when `g_lotLampArms` changed, at most every 15 s.

**Finding:** the 5 s load kick fired during the loading screen: the countdown was consumed at the first world update
while the night level still read 0.00 (a lamps-off bake), then the dusk rebuild followed (2 s + 50 frames later).

**Outcome:** the load rebuild waits for the world to be drawn and a steady night level, and merges with the dusk rebuild;
lamp additions and removals count like edits on lots already loaded, coalesced (250 ms quiet, at most one rebuild per
3 s, night only); `kArmFrames` 3 instead of 50 (the extra 50 frames only delayed the result by about 0.8 s); *Lot lamps
light the street* applies live (its predicates installed whatever the option); a reinstall keeps `g_lastCells` and the
maps (`LotLightBridge::Shutdown(true)`), a real uninstall keeps `g_lastCells` but releases the maps; the chunk render
call reports changed chunks. Expected: rebuild armed about 1 to 2 s after the world is first drawn; dusk after
`atrasoSegundos` + 3 frames; a Build-mode add / move / remove up to 20 frames (enumeration) + 250 ms + 3 frames (+ 20
frames for a removal), then the chunk's turn in the sweep; the menu switch about 250 ms + 3 frames (was a 2 s debounce,
a reinstall and a full new-world cycle). Estimates.

### 2026-09-29: lamp change decisions

**Context:** research\perf2\round3.md section 5: the worst stutters (about 240 ms: terrain update about 172 ms + DXT
encode about 56 ms in one frame) are full terrain rebuilds. In the 00:37 session 5 of 8 were armed by Apex ("lot lamps
changed" / "lights changed"), two about 1 s after a rebuild the game made itself; an older session froze every 15 s (the
stuck-countdown fallback). The same lots reported "4 edited" / "7 edited" again and again.

**Finding:** from `ApexRadiance_LOG.txt` and `ApexRadiance_LightDiag.txt` (28/09 19:53): lot 7D6F0019FAF78910 ("7
edited") had exactly 7 outdoor type-3 lamps, all disabled (flags 0x35 / 0xB5, no 0x40), never baked but counted; lot
6C11001B182E9ED0 ("3 edited"): 3 disabled type-11 lamps (0xB5) and 3 type-5 (no 0x04, not tracked); lots
4522001BE6BBCA40 ("4 edited") and 6C11001BA7277E30 ("2 edited"): lit, enabled lamps of types 3..5 with intensity 0.00
next to others at 1.00 (inferred: intensity switched by the game); lot 6C11001B18CA1000 ("7 edited"): magenta and blue
lamps and type-9 lights (inferred: colour-cycling). The old signature compared raw colour, intensity, lit flag and
position bits, so any flicker counted.

**Outcome:** only bake changes count; animated lamps; the snapshot of the last rebuild (`DiffBake`); camera still for
1 s; user-driven changes at most every 3 s; automatic changes and the stuck countdown at most once per 30 s after the
last rebuild. Expected: from about 20 Apex rebuilds in 20 minutes of night play to a few. Accepted risks at the time:
an automatic change could take up to 30 s; a Build-mode recolour took the automatic path; the first lamp placed on a
lot with no lamp of these types was not seen (its lot looks like one streaming in) until the next rebuild.

### 2026-09-29: local terrain relight and paced sweep (developer toggles)

**Context:** a full rebuild is a 229 to 241 ms frame; the combined build's local relight used +0x55 on 1 to 25 chunks
and had the same synchronous cost per chunk.

**Finding:** +0x54 alone is the game's one-chunk-per-terrain-update sweep branch (`0x00C85041`, the call `0x00C8504C`):
about 3.5 ms for one chunk, no geometry, no road mark, no duplicate (research\perf2\chunkrelight.md).

**Outcome:** `ChunkRelight` with `relightNearbyChunks` and `relightPacedSweep`, both developer-only and off: every
assumption is checked at use but nothing had run, and +0x54 does not refresh the road partition mark (`0x00B789B0`).
The world-load rebuild and the button stay full rebuilds (the load rebuild creates the light maps and fixes the
world-file chunk borders; the local path needs it).

### 2026-09-30: camera wait, one-lot switches, live signal

**Context:** a town square's 57 lamps reached the ground about 16 s late while panning, and were otherwise only caught
by the stuck countdown 15 to 30 s late; lamps toggled while testing stayed ignored as animated; a 54 s loading screen and
a session that sat 2 minutes at the main menu went "live" through the 30 s fallback.

**Finding:** the camera wait had no maximum; the bulk rule (more than 8 changes) also discarded one lot's own lamps
switched together; animated was permanent; the fallback did not require a world.

**Outcome:** the camera wait is at most 2 s (`kCameraWaitMax`); all switches of one lot are not bulk; an animated lamp
counts again after 2 min without an automatic change; the live fallback waits 120 s with the bridge on and needs a
loaded lot. Kept.

### 2026-09-30: automatic interval (2.1.0)

**Context:** lamps switched by Sims stayed on the ground too long with the 30 s rule.

**Finding:** flickering lamps are already handled as animated.

**Outcome:** automatic changes rebuild 5 s after the last rebuild, 30 s once two automatic rebuilds ran within the last
minute (lamps switching one by one, sensor lamps at dusk across lots). Kept.

### 2026-10-01: after-load refresh (Test005)

**Context:** the rooms and lots refresh after a load waited a fixed 8 s.

**Finding:** most loads are ready earlier.

**Outcome:** from 500 ms after the world is live, a cached-room readiness check polls every 200 ms; 250 ms quiet with a
fresh post-load enumeration and no room or ambient work lets it run early; 8 s remains the upper bound. A bounded
heuristic for loaded cached rooms, not proof that every later streamed lot is loaded. Terrain trigger rules unchanged.

### 2026-10-01: paced sweep default (2.5.3)

**Context:** test007 build with the paced sweep on.

**Finding:** gradual external terrain updates accepted in the tested scene.

**Outcome:** `relightPacedSweep` defaults to on in every mode; the toggles stay developer-only; the world-load rebuild
stays full. Released in 2.5.3.

### 2026-10-02: edit response after 2.5.3

**Context:** a capture showed manual type-11 recolours classified as animated after three changes, and 64-chunk sweeps
taking about 7 s.

**Finding:** manual edits went through the automatic route.

**Outcome:** placements, moves and type-11 base-colour changes request the local relight even with
`relightNearbyChunks` off; resolved Build editing (mode 2, not assumed to cover every Build/Buy state) keeps edits out of
the animation filter; urgent local chunks move ahead of a background sweep with batch ownership preserved; an edit on a
chunk already rendering queues another pass. No change to light intensity or edge-feather shaders.

### 2026-10-02: type-11 switch-off (01:08:41)

**Context:** the 01:08:41 capture.

**Finding:** intensity 1 -> 0 was still automatic (64-chunk sweep, 7.2 s), and enabled 1 -> 0 with flags 0x77 -> 0x35 was
ignored as below threshold. The type-11 visitor vtable points to `FUN_007EAEA0`, whose decompile returns 1
unconditionally.

**Outcome:** lot-owned outdoor type-11 lights are filtered for alive / enabled / lit before the original acceptance, the
snapshot model uses the same enabled condition, and type-11 effective light changes take the priority local path.
World-owned lights keep the original test.

### 2026-10-02: first switch after entering a lot

**Context:** an edit of an existing lamp right after entering a lot was lost.

**Finding:** the edit occurred before the 10 s / 5 s settle window; then the settled lot's already-off state joined the
snapshot.

**Outcome:** the first observed state of newly seen lots is kept without scheduling work; confirmed value edits of an
observed lamp (no additions or removals on that lot) can trigger before the settle window.

### 2026-10-02: ordinary switches and entry response (02:03:26)

**Context:** the installed binary matched the terrain-variant build `C1257AF4...`. Type-3 lamps switched enabled 1 -> 0
(0x77 -> 0x35) on lot 6C11001AE6152170 took the automatic path; an automatic change was held by the 30 s busy rate;
64-chunk sweeps took about 7.2 s; at 02:03:52 three switches within 60 s made the lamps animated; at 02:04:03.570 the
stuck countdown scheduled a 64-chunk sweep for 28 switched-off lamps on two lots, done at 02:04:10.726. The priority
path handled 31 lamps in five chunks at 02:03:46.568, done at 02:03:46.683. Log timings, not input-to-screen latency.

**Finding:** ordinary enable switches were automatic and became animated.

**Outcome:** a discrete 0x40 change of the same lamp on the same lot whose bake contribution changes takes the priority
path and resets the animated state; continuous intensity/colour changes keep their suppression. A confirmed value edit
with no additions or removals on its lot bypasses the settle window and bulk suppression. New lots are adopted into the
snapshot while another edit waits, except lots in the pending priority edit. No change to shaders, room budgets, render
gates or pacing.

### 2026-10-02: colour, intensity and visible-lot arrival (02:26:00)

**Context:** the installed `CBCF9E88...` binary contained the switch fix. 35 lamps on four chunks were queued at
02:28:04.425 and completed at 02:28:04.482 (57 ms, average render 7.11 ms, maximum 8.31 ms). Remaining failures: missing
contributions at load took a 64-chunk sweep (7.6 s); 26 ordinary lamps changing intensity 1 -> 0 without 0x40 were
automatic; repeated nonzero intensity edits became animated. Comparison baseline: release v2.5.3 targets `c8453d9`, source
tree `68d1b3d` matching local `39fd8c3`; the settle guards, automatic camera/rate waits, three-change animation
suppression and 8-per-second pacing exist there.

**Finding:** value changes and late lot entry still had slow routes.

**Outcome:** the lamp-entry mark requests an earlier lamp read (at least 50 ms apart); ordinary intensity crossings
through zero are switches; effective colour, intensity and range changes on a drawn lot are priority edits (a
visibility heuristic, not proof of a human edit), compared exactly; priority quiet 80 ms or 500 ms continuous; visible-lot
arrivals refresh old and new footprints (150 ms / 500 ms, retry 500 ms for up to 8 s, one batch per frame), never a
full rebuild; disabled lot lamps leave the direct lamp pool at once; the 4-release reserve after a measured chunk cost
of at most 12 ms. Released in 2.5.4.

### 2026-10-02: 2.5.4 hotfix acceptance

**Context:** the complete private response build `76B4C32D...` tested in game.

**Finding:** maintainer feedback: the response was correct and felt more responsive.

**Outcome:** the same lighting logic shipped in 2.5.4, development tools excluded. No universal latency guarantee or
validation of unverified variants follows from it.

### 2026-10-04: daylight transitions (02-05-41, PR #2)

**Context:** the session captured daytime terrain with the lamp map bound but its RGB factor zero; the recording does not
include the day/night transition, so it cannot establish the history of the boundary in the screenshot.

**Finding:** priority lot edits and forced feature changes were discarded by the daytime/dusk deferral; only the night
endpoint rebuilt.

**Outcome:** `DeferDayEdit` defers only ordinary automatic events; both settled endpoints (> 0.99, < 0.01) rebuild,
including Build preview; reversal cancels a stale target; loading merges phase work; each endpoint queues the paced
sweep or the full fallback; refusal reasons are logged in normal mode. The daylight lamp term is in
[world atlas history](night-lighting-world-atlas-and-smoothed-maps.md).

### 2026-10-04: Build preview response (03-15-30, PR #2)

**Context:** the recording reached day at 03:15:36.438 and the sweep started at 03:15:38.362; night at 03:15:40.501, sweep
at 03:15:42.487. The local lamp change queued in about 113 ms; its completion was not logged.

**Finding:** the 1.924 s / 1.986 s waits match the saved `atrasoSegundos = 2.0`; a 64-chunk sweep at 8 releases per second
takes seconds. A new-phase sweep counted an older in-flight bake as its own completion.

**Outcome:** no phase delay while Build editing (Live keeps it); at most four nearest sweep chunks get priority with a
valid camera, the reserve only after a measured cost of at most 12 ms; refusal logs name the gates +0x1D / +0x20 / +0x6C;
a new sweep re-queues the in-flight chunk; local completion is logged in normal mode. Exterior wall lighting is a
separate path ([walls](../features/night-lighting/walls.md)).

### 2026-10-04: priority edit with an armed countdown (11-32-34, PR #2)

**Context:** the recording stayed at night level 1.00; a lamp recolour (green to blue) at 11:32:40.996 was consumed at
11:32:41.096 as "merged into the armed rebuild", which is not proof of fresh pixels.

**Finding:** an armed countdown does not prove that a new manual edit reached the bake.

**Outcome:** priority edits keep their reconciliation when a countdown is armed and may queue footprints ahead of
arrival batches and sweeps; automatic edits still merge; load and phase work still coalesce; no synchronous full
rebuild added.
