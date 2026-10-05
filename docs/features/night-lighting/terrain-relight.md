# Terrain relight (lot lamps in the terrain bake, day/night rebuilds, lamp change response)

Outdoor lamps on lots light the world grass outside the lot, and the ground light follows the lamps: it is rebuilt when
lamps switch on at dusk and off at dawn, after a world loads, and when a lamp is placed, moved, removed, switched,
dimmed or recoloured. Changes are applied to the few terrain chunks under the changed lamps when possible, one chunk at
a time, so the game does not freeze for a full terrain rebuild. Part of [Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released (visitor patch, arm sites and dusk rebuild since 0.1.0; paced sweep default since 2.5.3; local relight of user and visible-lot edits since 2.5.4; world-owned type-11 lamps since 2.5.6). Day/night endpoint rebuilds, Build-mode phase response, priority re-queue of in-flight chunks and the world-lamp rig request on early completion: in development (PR #2) |
| Default | On |
| Menu | Lighting > Ground > Ground & Lots > *Lot lamps light the street*; Lighting > Ground > Updates (*Update at dusk*, *Delay after dusk*); Lighting > Overview > Refresh lighting |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` (keys below) |
| Source | [`patches/night_terrain_relight_patch.cpp`](../../../patches/night_terrain_relight_patch.cpp), [`features/terrain_chunk_relight.cpp`](../../../features/terrain_chunk_relight.cpp), [`features/terrain_lighting_policy.h`](../../../features/terrain_lighting_policy.h), lamp tracking in [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) |

## The problem

Reverse-engineering summary (Steam 1.67.2.024037; details in
[engine/terrain-and-light-bake.md](../../engine/terrain-and-light-bake.md)):

- World terrain chunks are relit by `FUN_00c845c0` (per frame, render thread). When the light cells' countdown fires it
  sets byte **chunk+0x55** on every chunk; that rebuilds the chunk and its light textures (`FUN_00c834f0` /
  `FUN_00c83060` / `FUN_00c7fa70` / `FUN_00c7e7a0` / `FUN_00c25a90`, bake `FUN_00C292B0` "staticTerrainLightmap").
- cells = *(lightMgr + 0x104), lightMgr = *(*(0x011D1860) + 0x1C0). **+0x38 / +0x3C** are countdowns, decremented each
  frame by `FUN_006b5da0` and reset to -1 by `FUN_006b5770` when consumed. The light register / unregister / move-toggle
  sites (`FUN_006b64b0`, `FUN_006b6090`, `FUN_006b6590`) write 50 to **+0x38 only**; +0x3C, the one that triggers the
  rebuild in normal play, is written only by `FUN_006b5730` (called from `0x006B08A0`). See *Countdown consume* below.
- Picking up a street lamp in Build mode arms the countdown (call trace: 128 chunk rebuilds and 243 light-texture
  rebuilds follow). The night-level setter `FUN_006add60` switches lamps on at dusk but never arms it, so a save loaded
  by day keeps the "lamps off" terrain light.
- Only lights whose vfunc+0x20 returns 1 (class 0xFF42F8, type 0xB "world light") can arm the countdown or enter the
  bake: the visitor at 0xC29620 (vtable 0x010768A0) filters with that vfunc. Lot lamps (types 3..6) never reach the world
  terrain: a hard edge around lots.
- The terrain maps baked into the world file miss the part of a lamp's light that crosses into the neighbouring 256 m
  chunk (straight cut on world grass at chunk borders, `LightProbe-grama2`); any rebuild fixes it.
- A full rebuild bakes and DXT-encodes every chunk synchronously in the consume frame (`0x00C83060` -> `0x00C7E7A0` at
  `0x00C8307E`), then sets +0x54 so the game's sweep renders every chunk a second time: measured 229 to 241 ms frames
  (#927: terrain 173 ms + DXT 57 ms, 512 DXT calls = 64 chunks x 2 textures x 4 mips) plus about 3.5 ms per frame for 64
  frames (research\perf2\chunkrelight.md; engine sections 2, 3.3, 4.1).

## How Apex Radiance solves it

1. **Bake lot lamps.** The visitor and the three arm sites call Apex predicates that also accept outdoor lot lamps
   (`TerrainLightTest`, `ArmTest`).
2. **Rebuild at the right moments.** After a world load (once it is drawn and the night level is steady), at the
   settled day and night endpoints, on the Refresh terrain button, and when the lamps the bake takes changed.
3. **Track lamp changes.** Every light enumeration is compared with a snapshot of what the last rebuild baked, so only
   changes the bake would show count, and the same state is never rebuilt twice.
4. **Relight locally.** A lamp change re-renders only the chunks under the changed lamps' old and new light rects through
   the game's one-chunk sweep branch (chunk+0x54); a change the local path refuses, and the day/night endpoint rebuilds,
   become a paced sweep of every chunk, nearest to the camera first; a full rebuild is the last fallback.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Ground > Lot lamps light the street | `luzDoLoteNaGrama` | bool | on | | Outdoor lot lamps in the terrain bake. Live: the predicates read it at run time; a switch is one rebuild (a "switch" edit). Reinstall only when its code could not be installed |
| Ground > Updates > Update at dusk | `automaticoAoAnoitecer` | bool | on | | Rebuild at each settled endpoint (night above 0.99, day below 0.01) |
| Ground > Updates > Delay after dusk | `atrasoSegundos` | float | 2.0 s | 0.5 to 10 s | Wait before an endpoint rebuild; 0 while Build mode editing (`WorldManager+0x1B4 == 2`) |
| Developer: Relight only nearby terrain | `relightNearbyChunks` | bool | off | | Developer mode only. Automatic lamp changes also try the local relight (user-driven edits always do) |
| Developer: Paced terrain sweep | `relightPacedSweep` | bool | on | | Registered in developer mode only; the default is active in every mode. Endpoint, lamp-change and stuck-countdown rebuilds become a paced sweep |
| Individual options | `recalcularLotesAoAnoitecer` | bool | off | | Experimental: re-solve room 0 of every lot 3 s after a dusk rebuild (not after a load rebuild); fallback 6 s after the dusk endpoint |
| Individual options | `postesAcesosNoCalculo` | bool | off | | Experimental: street lamps count as lit in lot light solves (0x6BE18C, [lot-light-pass](lot-light-pass.md)); reinstall |

Buttons (Lighting > Overview > Refresh lighting, also Developer > Lighting; enabled once the world is live): *Refresh
terrain* (`g_kickRequested`: arms both countdowns like a lamp pick-up, a full rebuild, day or night) and *Refresh lots*
(`g_relightLotsRequested`: queues room 0 of every loaded lot story). *Refresh lights* and the hotkey run
`NightLighting::RefreshAll` ([README](README.md#refresh-controls)).

## Compatibility and interactions

- [World atlas and smoothed maps](world-atlas-and-smoothed-maps.md): every armed rebuild tells the smoothed maps
  (`NoteKick`, `ExpectRebuild`, `OnTerrainRebuilt`); the chunk render thunk reports each re-rendered chunk
  (`NoteChunkRendered`). `ExpectRebuild(30)` is called while a load rebuild, an endpoint rebuild, an armed kick (under
  5 s old) or a user-driven or switch edit the local path will not take is coming; not for automatic changes nor for changes the
  local relight will likely take.
- [World lamp response](world-lamp-response.md): world-owned type-11 lamps share snapshot group 0; their edits also
  request a native rig refresh.
- [Level light share](level-light-share.md): lot relights (`QueueAllLotOutdoorRooms`) and story sharing both touch room
  0 of every story. Lamps on stories above the ground reach the bake through the separate `SplitLevelGroundLight` patch
  (GetLotID 0x006BC020 zeroed for the bake's story test at 0xC294D9); that page also describes the official
  Sims3SettingsSetter Split-Level Lighting Fix. The lamp-entry mark filter (`LampMarkFilter`) documented there also
  requests an early lamp read when a lamp's values change.
- Rooms: any lot lamp switched on or off anywhere (indoors too, `LotLightBridge::LampSwitches`) gives rooms the game's own
  lighting budget for 3 s (`LotLightingMotion::Boost(3000)`); a lamp toggled 3 times within 60 s no longer counts.
- [Lot light pass](lot-light-pass.md): records which lots are drawn (visible-lot arrivals).
- Street and lot lamp brightness and lamp colours ([README](README.md#brightness-controls)) enter the bake through
  `BakeColourStub`; a change, once the slider is released, is a "switch" edit (one rebuild).

## Limitations

- Basement lamps and lot lights other than outdoor lamps (types 3..6 and lot-owned 11, room 0, lit) stay out of the bake,
  as in the game.
- A full rebuild consumed by the game itself (a Build-mode pick-up) is treated as covering every lamp.
- Animated lamps (3 automatic changes within 60 s) do not trigger work; their state reaches the ground with the next
  rebuild made for another reason, or after 2 min without an automatic change.
- The first-observation baseline of a newly seen lot is an assumption about the current bake, not a readback.
- A local relight is refused (full path instead) for more than 16 chunks or a quarter of the world, a lamp rect that does
  not hold its lamp, a lot the last rebuild did not have, or an unexpected terrain layout. Arrival work is never escalated
  to a full rebuild.
- `+0x54` does not refresh the road partition mark (`0x00B789B0`); roads are expected to see the in-place light map.
- A paced sweep of 64 chunks takes several seconds at 8 releases per second; at most the 4 nearest chunks are
  prioritised during Build mode editing.
- The developer status line *Rebuild pending: the game only rebuilds the terrain at night (or in Build mode)* and the
  wait text *rate-limited (automatic: 30 s after the last rebuild)* are stale wording (see *Countdown consume* and
  *Decision rules*).

## Technical reference

### Install (`NightTerrainRelightPatch::Install`)

1. `LoadAddresses`; root getter check (imm32 masked); `g_rootPtrAddr` = imm32. `LightDiag::Init()` (warning if
   unavailable).
2. Visitor site and the three arm sites validated byte for byte. On a mismatch, install fails when `luzDoLoteNaGrama`
   is on; otherwise a warning and the option cannot be turned on (`installedLotLampCode = false`). Written whatever the
   option: visitor `mov esi,ecx; push edi; call TerrainLightTest; nop x3` (11 bytes); each arm site `push edi; call
   ArmTest; nop x3` (9 bytes).
3. Room queue prologue (Steam: exact bytes; other builds: signature). Experimental patches
   (`postesAcesosNoCalculo`, `qualidadeAltaEmTodosOsLotes`, `gramaDoLoteUsaLuzDoLote`, see [lot-light-pass](lot-light-pass.md)),
   the bake colour stub at 0xC2950F (optional), the chunk render redirect at 0x00C8504C (optional), the Present hook and
   the other modules ([README](README.md)).

### Light predicates (called from game code)

| Function | Replaces | Accepts |
|---|---|---|
| `TerrainLightTest(light)` (visitor 0xC29626) | vfunc+0x20 | A lot-owned outdoor type-11 lamp (lot id != 0, room known 0x04, room 0) is refused unless flags have 0x01 alive, 0x20 lit and 0x40 enabled (its vfunc+0x20 is `FUN_007EAEA0`, always true). Then the original test (world lights). With `luzDoLoteNaGrama`: `IsOutdoorLotLamp` and lit (+0x100 & 0x20). Counters "on the ground" (`g_lotLampsBaked`) and "off" (`g_lotLampsSkippedOff`) |
| `ArmTest(light)` (0x6B6516, 0x6B60D3, 0x6B6618) | vfunc+0x20 | World lights, or outdoor lot lamps with `luzDoLoteNaGrama` (counted in `g_lotLampArms`, "armed") |

`IsOutdoorLotLamp(L)`: lot id (+0xC0 | +0xC4) != 0, type +0xB0 in 3..6, flags +0x100 with 0x01 alive, 0x40 enabled,
0x04 room known, and room +0x08 == 0. All reads inside `__try`.

### Per frame (`OnPresent`, render thread = the game's light and terrain update thread)

1. `ReadLightState`: root -> lightMgr (+0x1C0) -> cells (+0x104), level = lightMgr+0xF0. None: status "Waiting for the
   game to load a world". night = level > 0.99. Countdowns c38 / c3C read.
2. **New world** (cells pointer changed): phase cycle reset, pending edit, rig request, snapshot, local batches, arrivals
   and relit-lamp memory cleared; `ChunkRelight::OnWorldChanged`; `LotLightBridge::OnWorldChanged` (chunk maps, smoothed
   maps, atlas, lamp pool), `LevelLightShare::OnWorldChanged`, `UnlitRooms::OnWorldChanged`; log `World loaded (...)`;
   load rebuild pending.
3. **World live**: the bridge recorded a world terrain chunk draw (`ChunkCount() > 0`), or, with no draw, a fallback after
   30 s (120 s when the bridge is on) once at least one lot is loaded. Then `GameAddr::CheckWorldStructs`,
   `LevelLightShare::OnWorldLive`, `RequestRefreshAfterLoad` (polls from 500 ms, at most 8 s).
4. **Rebuild consumed** (previous c38 >= 0, now -1): log `Terrain rebuilt (<reason>|by the game itself; ...)`,
   `LightmapSmooth::OnTerrainRebuilt`, `ChunkRelight::OnFullRebuild` (drops pending local work, enables the local path for
   this world), a pending edit is covered, a lamp enumeration is forced and the next one becomes the snapshot (wait at
   most 2 s, else no snapshot). Lot relight 3 s later only for a dusk rebuild with `recalcularLotesAoAnoitecer`.
5. **Phase cycle** (`TerrainLightingPolicy::Cycle`): phases Day (< 0.01), Twilight, Night (> 0.99). Reaching a settled
   endpoint different from the last one schedules a rebuild after `PhaseDelay` (`atrasoSegundos`, 0 while editing);
   reversing direction cancels it; disabled or loading cancels it. Consumed: `RebuildAll("dusk" | "daylight transition")`.
6. **Load rebuild**: live for 1 s and the level within 0.02 for 1 s (or 20 s after live): `Kick` with reason "world load"
   or "world load (night: also the dusk rebuild)"; a scheduled endpoint rebuild is merged. Always a full rebuild.
7. Camera sample, snapshot capture, lamp-switch boost, new edits (`NoteEdit`), adoption of new lots into the snapshot,
   bake gains and lamp colours (on slider release), moonlight, stuck countdown, `DecideEdit` once quiet,
   `RefreshArrivingLots`, `ChunkRelight::OnPresent` and its completions, `ExpectRebuild`, lot relight, status string.

`Kick` writes `kArmFrames` = 3 to cells+0x38 **and** +0x3C (the game's own sites write 50; every Apex kick is already
debounced; the decrement `0x006B5DA0` and the consume `0x00C84C1B` treat any positive value the same), logs `Rebuild
armed: <reason> (night level x)` and calls `LightmapSmooth::NoteKick`.

`RebuildAll`: with `relightPacedSweep`, `ChunkRelight::QueueSweep` (every chunk, nearest to the camera eye first, else grid
order); it starts like a consumed rebuild (snapshot of the lamps now, `g_lastRebuildAt`, pending local batches dropped).
Not possible: log `Paced sweep not possible (...): full rebuild instead` and `Kick`.

### Lamp change tracking (`LotLightBridge::TrackLotLampEdits`, every light enumeration)

The enumeration (`FUN_006ACF70`) runs every 20 frames, at once after a rebuild or world change, and on an edit read
request (`RequestLampEditRefresh` from the lamp-entry mark, at least 50 ms after the previous read). `ReadLotLamp`
admits lamps by `WorldLampPolicy::Eligible` (lot types 3..6/11 outdoors, world-owned type 11) and reads base colour
+0xF0, intensity +0x10, range +0x130, position +0x120, light rect +0x134 {minX, minZ, maxX, maxZ} (set by `FUN_006BDDF0`
from position and range), type and flags.

What the bake uses ([engine 4.2](../../engine/terrain-and-light-bake.md)): accepted lights whose rect overlaps the chunk,
at position (vfunc+0x24), with colour +0xF0 and weight range +0x130 x intensity +0x10 x 0.2; not the fade +0x20 nor the
effective colour +0xE0. So "light" = colour x intensity x range per channel, and `InBake` = lit 0x20, enabled 0x40,
non-zero weight and colour. Whether the street-lamp class test (vfunc+0x20) needs the lit flag is unverified; it is
assumed (a save loaded by day keeps a lamps-off terrain light).

| Rule | Value |
|---|---|
| Moved | More than 5 cm (`kMoveTol`), both in the bake: user-driven |
| Light changed (automatic) | More than 5% relative per channel with an absolute floor of 0.05 (`kLightRel`, `kLightAbs`) |
| Priority value edit | Exact comparison (`LightChanged`) for: Build mode editing; type-11 edits on the same lot (lot-owned any change, world-owned light, enable or zero-intensity change); ordinary enable switches (0x40 or intensity through 0, `ObservedEnableSwitch`); any change of a plain lamp on a visible lot. Resets the animated state |
| Animated | 3 automatic changes within 60 s; expires after 120 s without one |
| Settled lot | Seen for 10 s and no uncounted change for 5 s; required unless the change is an observed value edit with no addition or removal on that lot |
| Bulk | More than 8 changes in one enumeration, unless all are switches of one lot; ignored unless observed value edits |
| Removal | Confirmed at the next enumeration if the lot is still there and lost no more lamps |
| World lamps (lot 0) | `WorldLampPolicy::AcceptEdit`: observed value edits only, no additions or removals |

Additions, removals, moves and priority value edits are user-driven; the rest is automatic. A late lamp registration on a
visible lot, a lot's first visibility, and reappearance after 2 s without a draw are lot arrivals (visibility expires
500 ms after the last regular lot-pass draw).

### Snapshot (`DiffBake`)

`g_baked` holds the lamps the last rebuild baked. `DiffBake(g_baked, current, plainLamps, priorityLots)` looks only at
settled lots present in the snapshot; lamps are matched by type and place (5 cm), not pointer. Changes listed: removed
or moved away (old rect), placed or moved here (new rect), switched on / off, light change (exact for pending priority
lots, else the 5% rule); animated lamps are counted but not listed. `AdoptNewLots` adds lots first seen after the last
rebuild with their first observed state, except lots in the pending priority edit. A finished local batch updates only
its changed lamps (`CoverLots`), so small unrelit changes still add up.

### Decision rules (`DecideEdit`, once quiet)

Quiet: 250 ms (`kEditQuiet`); user-driven and switch edits 80 ms, or 500 ms after the first event even if changes
continue (`EditReady`). In order:

1. `DeferDayEdit`: by day with *Update at dusk* on, an automatic, non-switch, non-world-lamp edit is left to the next
   endpoint rebuild.
2. Merged into a pending load or endpoint rebuild; an automatic edit is merged into an armed countdown.
3. Waits while the snapshot is being taken; an automatic or switch edit waits while a local relight or sweep is queued.
4. Snapshot compare (not for switches): no difference -> skipped ("the terrain was rebuilt after the change" when a
   rebuild ran at most 2 s before the change was seen, else "no change the terrain bake uses"). A user-driven change on a
   lot the snapshot does not have is not skipped.
5. Local relight (`TryLocal`) for user-driven edits, or any edit with `relightNearbyChunks`, when a snapshot exists and it
   is not a switch. Automatic edits wait for the camera and relight the same lamp at most once per 5 s
   (`kLocalAutoPerLamp`). Refused: full path until the next change.
6. Camera: no rebuild while the eye moved within 1 s (more than 2 cm from the reference; eye
   `[[root]+camera]+eye`, offsets parsed from the code, see [engine/camera-and-map-view.md](../../engine/camera-and-map-view.md);
   unknown = still), at most 2 s after the change was first seen.
7. Rate: user-driven and switch edits at most one rebuild every 3 s; automatic edits 5 s after the last rebuild of any
   kind, 30 s once two automatic rebuilds ran within the last minute.
8. `RebuildAll`.

`FinishEdit` ends the edit and first consumes a pending world-lamp rig request
([world lamp response](world-lamp-response.md)).

**Stuck countdown** (fallback): at night with c38 == 0 and c3C <= 0 for 120 frames, when the lot-lamp arm count changed,
at most every 15 s: an automatic edit "lights changed (stuck countdown)". The game's sites arm only +0x38, which never
rebuilds in play, so a lamp change the tracking did not count would otherwise wait forever.

### Visible-lot arrivals (`RefreshArrivingLots`, night only)

At most 64 lots tracked, each for 8 s. A lot is processed after 150 ms quiet or 500 ms from its first event, at most one
batch per frame, not within 500 ms of its previous arrival relight. It refreshes the union of the old snapshot rects and
the current rects of the lot's lamps (`ArrivalFootprint`; plain lamps only with *Lot lamps light the street*), queued as
urgent local work. Refused: retried after 500 ms. Skipped while a user-driven or switch edit is pending, the snapshot is being
taken, the load rebuild is pending, or a local batch (not a sweep) is running. Developer log: `Entry relight queued:
visible lot <id>, N lamp footprints, chunks ...`.

### Local relight and paced sweep (`ChunkRelight`, `terrain_chunk_relight.cpp`)

**Selection** (`QueueLocal`): for each rect + 1 m, the grid cells it covers plus one on each side; a chunk is kept when
its bake record (`*(*(terrain+0x68)+8)`: records +0xBC, width +0xCC, height +0xD0; record {x0, z0, w, h}; else the chunk
rect) overlaps the rect + 1 m (the bake draws a light when its rect overlaps the record inclusively, 0x00C29480..0x00C294C8).
The lamp's own chunk first, then nearest first, deduplicated. Urgent work goes to the front of the queue; a chunk
already in flight is queued again for urgent work (the in-flight bake may have read the old light).

**Validated at use** (any failure refuses): WorldManager from `0x011ECBC4`, terrain = WM + disp8 of `0x00C6D68C` (0x58),
terrain+0x14 == WM; live play (WM+0x1B4 != 0); vector size == nx x nz (terrain +0xC0 / +0xC4), cell +0xC8 == 256; per
chunk: corner multiple of 256 at slot `(z0/256)*nx + x0/256`, size 256, centre = corner + 128, rect = corner..+256; the
world's first rebuild seen and a sweep render of this terrain seen by the thunk; the gates open; every chunk has a light
map (`+0xD8`); at most 16 chunks and a quarter of the world; at most 32 queued (urgent work behind a sweep: the world's
chunk count + 16); rects finite, not inverted, at most 4096 m. A light rect must hold its lamp's place within 1 m (the
rect updaters 0x006BDE66 / 0x006BE816 / 0x006BE8AB are not verified to run in the call that moves the lamp).

**Release** (every frame): one chunk in flight, its +0x54 set only after the previous one finished and never in the
frame right after a render; at most 8 releases in any 1 s, 12 for urgent work once the latest measured chunk render in
this world took at most 12 ms; never while any chunk has +0x55 or +0x56; never while a gate is closed. Gates mirrored
from `0x00C7E7A0`: live and TerrainData (terrain+0x64) +0x1D == 0; `[WM+0x54] ? [[WM+0x54]+8] : 0` (`0x00C61040`) != 0
and TerrainData +0x20 == 0; byte `[[TerrainData+0x0C]+0x6C] != 0` (the sweep branch is skipped at `0x00C85011`, read at
`0x00C8471A..0x00C8473A`, probably a tool state). Refusal text names the gate. A chunk flagged while closed would stall
the game's whole per-chunk loop: `0x00C7E7A0` returns without clearing +0x54 and the branch still sets "work done".

**Sweep** (`QueueSweep`): every chunk; during Build mode editing with a known eye, the 4 nearest are urgent
(`PreviewPriorityChunks`). A new sweep clears the queue and re-queues a chunk in flight. Log `Terrain sweep started:
<reason> (N chunks, ...)` / `Terrain sweep done: ...`.

**Completion**: the thunk sees the chunk rendered (+0x54 back to 0, QPC time), or +0x54 is 0 at Present (rendered by
another game path, `0x00C83060` during a LOD change; `FinishFlight` then calls `NoteChunkRendered` itself). **Timeout**:
still set after 120 frames with nothing else pending and the gates open, or 1200 frames in all -> +0x54 put back to 0
when Apex set it and no rebuild is in progress, queue dropped, one full rebuild ("local terrain relight failed (...)").
Frames count Presents, so the first timeout in a world keeps the local path; the second, or "the terrain changed under
the queue", turns it off for this world. A consumed rebuild drops the queue; a world change resets it.

### Lot relight (`QueueAllLotOutdoorRooms`)

Walks the lot tree (lightMgr+0xD4, buckets +0x58, count +0x5C, node+8 = tracker, next +0x10) and, for levels -4..7 whose
treeLevel (`tracker + 0x6A0 + L*0x1A4`) manager belongs to this lightMgr, calls `FUN_006c7160(treeLevel, 0)`. Log
`Lots: N lot stories queued (...)`.

### Countdown consume (BL at 0xC84C29)

`0x00C84C1B..0x00C84C43`: `call 0x6B5750` (cells test) must be true; `test bl,bl; jnz` skips the night test, else
`call 0x6AC560` (night) must be true; then `call 0x6B5770` (reset) and the loop over chunks (+0xB0/+0xB4) setting +0x55.
BL is set at `0x00C84B03..0x00C84B11`: `bl = (WorldManager+0x1B4 != 0)`, and `0x006B5750(bl)` tests `cells+0x3C == 0`
when BL is set, `cells+0x38 == 0` otherwise. WorldManager+0x1B4 is 1/2/3 in every normal game mode (0 only in the
engine's tool mode), so in play the only trigger is +0x3C reaching 0, by day or night; +0x38 alone never triggers it and
the night test runs only in tool mode. The patch header comment and the status line "the game only rebuilds the terrain
light at night (or in Build mode)" describe the tool-mode branch. `Kick` arms both. Engine sections 3.1 and 8.

### Chunk render redirect (0x00C8504C)

The CALL in the +0x54 sweep branch (`0x00C85041..0x00C85056`) goes to `ChunkRenderThunk`, which times the original
`0x00C7E7A0`, reports the chunk to the smoothed maps and to `ChunkRelight::OnChunkRendered`
([world atlas](world-atlas-and-smoothed-maps.md#chunk-re-render-notice-call-site-0x00c8504c)).

### Game addresses

The root getter, visitor, arm sites, room queue and light enumeration are listed with their byte patterns in
[README: game addresses owned by the patch file](README.md#game-addresses-owned-by-the-patch-file).

| Address | What | Verification |
|---|---|---|
| 0x00C84C1B..0x00C84C43 | Countdown consume in `FUN_00C845C0` | `re/out/dump/asm/00c845c0.asm` |
| 0x011ECBC4 | WorldManager global (`WorldManagerPtr`: store at 0x00C6D0CC, `8D 8D 9C 00 00 00 89 2D ?? ?? ?? ?? E8`) | sigcheck.pl 1 / 1, Steam ok |
| 0x00C6D68C | `mov ecx,[esi+58h]; call 0x00C845C0` (`TerrainUpdateCall`; disp8 = terrain offset, `8B 4E ?? E8` checked at run time) | sigcheck.pl 1 / 1, Steam ok |
| 0x00C85041..0x00C85056 | +0x54 sweep branch; 0x00C7E7E2..0x00C7E823 early-exit gates; 0x00C815E0 chunk layout | full.asm (engine 2, 3.3) |
| light +0x08, +0x10, +0x20, +0xB0, +0xC0/+0xC4, +0xD0, +0xE0, +0xF0, +0x100, +0x120, +0x130, +0x134 | Room, intensity, fade, type, lot id, story, effective colour, base colour, flags, position, range, bake rect | [engine/light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md) |

### Files and functions

| File | Function | Role |
|---|---|---|
| night_terrain_relight_patch.cpp | `TerrainLightTest`, `ArmTest`, `IsOutdoorLotLamp`, `OriginalWorldLightTest` | Predicates |
| | `Install` / `Uninstall` / `Update` / `ReinstallNow` / `DeferredReinstall`, `CallPatch` | Patching lifecycle |
| | `OnPresent`, `ReadLightState`, `ReadCounters`, `ArmCounters`, `Kick`, `RebuildAll`, `StartSweep` | Rebuilds |
| | `NoteEdit`, `EditReady`, `DecideEdit`, `FinishEdit`, `WaitEdit`, `EditKind`, `RefreshWorldRigs`; `ResolveCamera`, `ReadEye`, `SampleCamera`, `CameraStill`, `CameraAllowsEdit` | Lamp change decisions |
| | `TryLocal`, `LocalLamps`, `ArrivalFootprint`, `RefreshArrivingLots`, `ChunkRenderThunk`; `g_localBatches`, `g_relitLamps`, `g_arrivals`, `g_sweepId` | Local relight driver |
| | `QueueAllLotOutdoorRooms` | Lot relight |
| terrain_chunk_relight.cpp | `ChunkRelight::Init`, `QueueLocal`, `QueueSweep`, `OnPresent`, `OnChunkRendered`, `OnFullRebuild`, `OnWorldChanged`, `Drop`, `Busy`, `Editing`, `LikelyAvailable`, `Status`; `Attach`, `Check`, `ReleaseLimit`, `FinishFlight`, `Fail`, `ReadViewRaw`, `ReadChunkRaw`, `LayoutOk`, `BakeRectRaw`, `GatesRaw`, `ScanFlagsRaw`, `WriteFlag54` | Selection, validation, paced release, completion, timeout |
| terrain_lighting_policy.h | `DeferDayEdit`, `PhaseDelay`, `PreviewPriorityChunks`, `Phase`, `LevelPhase`, `Cycle` | Day/night policy |
| lot_light_bridge.cpp | `ReadLotLamp`, `InBake`, `TrackLotLampEdits`, `ObservedEnableSwitch`, `DiffBake`, `AdoptNewLots`, `LampsOfLots`, `CoverLots`, `BakeTakes`, `TakeLotArrivals`, `LotVisible`, `RequestLampEditRefresh`, `LampSwitches` | Lamp tracking and snapshot |
| world_lamp_policy.h | `Eligible`, `AcceptEdit`, `Track` | [World lamp response](world-lamp-response.md) |

### Diagnostics

Developer > Lighting > *Rebuild events and terrain tests*: last event; night level and countdowns; "Terrain: armed /
rebuilt / last: reason: armed -> rebuilt ms"; "World load: ... | night level crossings"; "Lamp changes: counted /
ignored / not counted (outside the bake, below the threshold, animated) / last"; "Lamp change decisions: rebuilt U
user-driven / A automatic | skipped ... | deferred: camera, rate-limited | pending | camera | last rebuild's lamps: N
lamps on K lots, taken S s ago | last"; "Chunk re-render notices"; the two developer checkboxes; "Local terrain relight:
relit locally U / A, done, refused (last), failures | paced sweeps"; "Terrain chunks: <ready | why not> | local relights,
sweeps, chunks re-rendered (by another game path), refused, failures | queue, in flight | chunk render: last / average /
max ms | waits: rebuild flags, terrain not ready, 8 per second | sweep renders seen | last"; lot relights; "Lot lamps:
armed | on the ground | off".

Log (normal mode): `World loaded`, `Rebuild armed`, `Terrain rebuilt (...) N ms after it was armed`, `Terrain sweep
started / done`, `Local relight done: ...`, `Paced sweep not possible`, `[ChunkRelight] <why>: queue dropped, ...`.
Developer mode adds `World live`, `Night level crossed 0.99 upwards|downwards`, snapshot adoption, entry relights and,
with verbose logging, one line per decision (`Lamp change: <reason> (<kind>): rebuilt | relit locally | skipped |
deferred | rate-limited | merged ... | left to the dusk rebuild (day)`), per-lamp change text from the bridge
(`L... type T: lit 1->0, intensity 1.000->0.000 [leaves the bake]`) and, at most once a minute per lot, `Lamp changes
that do not rebuild the terrain`.

## Rejected approaches

- Fixed-delay load rebuild (15 s, then 5 s after the cells change): ran during the loading screen with lamps off.
- Rebuilding on any armed +0x38 or on streaming lamp changes: a rebuild every 15 to 30 s.
- Once-a-second relight reconciliation with `+0x55` on 1 to 25 chunks: the same synchronous bake per chunk, 12 to 60 ms
  hitches.
- Relighting lots after every rebuild: a re-arm loop invalidating slow lot solves.
- The "Rebuild terrain light" button for houses on foundations: the story gate drops their lamps from every bake.
- Fixed 30 s rate for automatic changes: lamps switched by Sims stayed on the ground too long.

Details in [history](../../history/night-lighting-terrain-relight.md).

## See also

- [Validation](../../validation/night-lighting-terrain-relight.md)
- [History](../../history/night-lighting-terrain-relight.md)
- [Engine: terrain and light bake](../../engine/terrain-and-light-bake.md)
- [World lamp response](world-lamp-response.md), [world atlas](world-atlas-and-smoothed-maps.md)
