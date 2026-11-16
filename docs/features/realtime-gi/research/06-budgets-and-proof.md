# 06. Budgets and proof plan (address space, frame time, stage 1 test, risks)

Desk research, 2026-11-17. No game run. Sources: `idea.md`, `00-camera-pan-profile.md` (the profile, "P" below),
`docs/features/rtx-remix-bridge.md`, `docs/validation/rtx-remix-bridge.md`, `native/remix-bridge/patches/0002/0003/0008`,
`docs/features/night-lighting/gpu-room-lamps.md`, `docs/features/memory-budget.md`, `tools/ingame_runner/README.md`,
and on our side `docs/280-camera-sweep.md` (branch `feature/camera-sweep-1117`). Marks: **[V]** verified in code or a
measured report, **[R]** reported in a doc, **[I]** inferred, **[?]** unverified, needs a measurement.

## 1. Address space under the bridge

### 1.1 What still costs game-process address space

The bridge moves DXVK, the Vulkan driver and every GPU allocation into the 64-bit `NvRemixBridge.exe` [V, validation
census: image -216 MiB, committed -649 MiB, largest free block +774 MiB vs in-process DXVK]. What stays in the game:

| Item | Cost in the 32-bit process | Source |
|---|---|---|
| **Client CPU copy ("shadow") of every surface or buffer the game LOCKS** | Full resource size per locked level. Made on the first lock; MANAGED copies of 64 KB and more come from `VirtualAlloc`, and 4.0.x frees them after 2 s idle (`freeIdleManagedCopiesSec = 2`); smaller ones use `new[]` and are never returned | patches 0002, 0003 [V] |
| Static vertex/index buffers (locked) | Copy kept; freed after 2 s idle for 64 KB to 4 MB (`freeIdleBufferCopiesSec = 2`) | patch 0008 [V] |
| MANAGED textures | Copy only if locked. Before the fixes: 585 MB MANAGED at 30 min; with them: 7.4 MB (al313) | validation [R] |
| SYSTEMMEM surfaces (readback targets) | 13.6 MB at 30 min (al285). Each is a full client buffer | validation [R] |
| **DEFAULT-pool render targets and textures never locked** | **No pixel copy.** DEFAULT pool was 0.2 MB at 30 min (al285). Cost is the COM object, handle-map entries and the 296-byte client objects: about 1 KB per resource [I] | validation [R] |
| Volume textures | The client creates them like other textures; D3D9 cannot render into a volume, so any fill of a volume means `LockBox` or `UpdateTexture`, which is a client copy [I, the client's volume lock path not read] | [?] |
| Apex's own memory (kept rooms, caches) | Kept rooms live above 2 GB (`ApexMem::HighAllocator`), a 192 MB cap for the active lot | gpu-room-lamps item 4 [V] |
| Client heap | 41 to 64 MB, fragmentation only. Do not add diagnostics (`perfLogIntervalFrames`) to a measured arm | rtx-remix-bridge.md [R] |
| Bridge shared-memory channels | pagefile mappings +78 MB (already in the census) | validation [R] |

Today's headroom: about 1.6 GB free and a 1.38-1.42 GB largest block in the pan runs (P section 10 #5); 1.8-1.94 GB
largest block at 60 min (al321-al325). Apex's state thresholds: Getting low below 320 MB, Critical below 160 MB, a
128 MB reserve for saves [V, memory-budget.md].

**Rule for the GI design: every GI resource is a DEFAULT-pool render target or texture that is written by draws and read
by shaders only. Never `Lock`, `LockBox`, `UpdateTexture` source or `GetRenderTargetData` a big resource.** The last also
costs 2.1-2.5 ms a call under the bridge (P section 6), so it is out for the render thread anyway. Small CPU-fed data
(the lamp table) goes in a render-target-free path: float constants, or a tiny MANAGED texture whose copy is under
64 KB (so it costs its size forever; 16 KB is nothing).

### 1.2 Estimated cost: K shadow maps + probe volume + light buffer

Game-process address space is, by the rule above, the object overhead plus the lamp table. VRAM and the server's memory
(x64, about 3.0 GB steady today [R]) carry the pixels. Precedent in Apex: the GPU room-lamp kept atlas already allocates
**6 faces x 256 x 256, G16R16F + D24X8 = 3.1 MB a lamp tile, 75 MB at 24 tiles, 377 MB at 120** in VRAM, with no
address-space finding [V, gpu-room-lamps item 20]. That is the proof the pattern works under the bridge. [I] that its
game-process cost is negligible: no census line separates it; measure (section 3).

| Resource | Size | Game-process address space | VRAM / server |
|---|---|---|---|
| Point lamp cube, face 256, G16R16F + D24X8 | 3.1 MB | ~1 KB | 3.1 MB |
| Point lamp cube, face 512 | 12.6 MB | ~1 KB | 12.6 MB |
| Spot lamp, one 512 face | 2.1 MB | ~1 KB | 2.1 MB |
| K = 16 cubes at 256 | 50 MB | ~16 KB | 50 MB |
| K = 24 cubes at 256 (the existing tile count) | 75 MB | ~24 KB | 75 MB |
| K = 8 cubes at 512 | 101 MB | ~8 KB | 101 MB |
| Probe volume 128 x 24 x 128 cells (0.5 m, 4 stories of 6 m), RGBA16F, as a 2D atlas of slices (no volume RT in D3D9) | 3.1 MB per channel | ~1 KB | 3.1 MB |
| L1 SH, 3 colour channels x 4 coefficients = 12 atlases, double-buffered for propagation | ~75 MB | ~24 KB | ~75 MB |
| Cheaper stage-2 start: a single RGB ambient-cube volume, 6 faces x 3.1 MB, ping-pong | ~38 MB | ~12 KB | ~38 MB |
| Lamp table (position, colour, cone, flags, slot): 120 lamps x 64 B | 7.7 KB | 7.7 KB, constants or a small MANAGED texture | none |
| Reflective-shadow-map colour (stage 2): +1 RGBA8 face per cube | +0.25 MB a face | ~0 | +25% of the cube |

Totals [I]: stage 1 (direct light only, K = 16 at 256) is about 50 MB of VRAM and **under 100 KB of game address
space**. Stage 2 adds 40-75 MB of VRAM, still under 100 KB. The bridge needs no larger channel for it (no data crosses
for resources that are only drawn into). **The address-space risk is not the resources. It is (a) any accidental lock or
readback, (b) the extra D3D calls' temporary command buffers in the shared channel (`serverChannelMemSize = 72 MB`, a
raised draw-call volume fills it faster, [?]), (c) Apex's own CPU code for casters (see section 4).**
VRAM: the test machine class (Alienware m15 R6, RTX 30 laptop, 6-8 GB [I from CLAUDE.md]) must fit game + Apex AO/picture
passes + 100-150 MB of GI; the 4070 Ti SUPER of the profile (16 GB) hides this. Test on the laptop class [?].

## 2. Frame budget table (from the profile)

All render-thread ms per frame, far pan (150 m, run D, p50 12.98 ms) unless stated; near pan (40 m) from run B.

### 2.1 What stage 1 and the full idea remove

| Item | ms per frame | Replaced by GI? | Notes |
|---|---|---|---|
| Lot room solve (`0x006A8BA0`) | **2.81** avg; 8.97 per hitch; worst 60 | Yes, for the GI lot (stage 1) | Biggest. Leads 81% of moving hitches (167 of them) |
| LotLightBridge draws (the per-draw fixes) | **1.76** at far; its own time 2.4-4.4 near, 3.3-3.8 far (the brief's 1.8-3.4 is the same range) | Mostly, those that fix lamp maps (RoomMapSmooth .54-.63, FenceStairGround .55-.69, OutdoorObjectGround .37-.75, RoomsAtNightFurniture .23-.38, FloorLight .07-.11, WorldTerrainLight .10, several .70) | FoliageVertex (.19-.27) and ExteriorWallGain (.07) are sun/foliage, likely stay in part [I] |
| RtOutdoorLamps relights, wall tests, picks | 0.17-0.41 (largest frame 3-9.6) | Yes | 5-33 relights and 0-13 story maps on a worker per 10 s |
| RtOutdoorLampsMapSwap (SetTexture) | 0.064 | Yes | |
| NightTerrainRelight (Present) | 0.094 | Yes | |
| Lamp refresh (mod) | 0.06 | Yes | |
| Lot lighting update (`FUN_00ADB8F0`) | 0.01 | Skipped with the solve | |
| **Sum replaced, upper bound** | **about 5.0-6.9** | | ~40-50% of a 13 ms frame [I] |
| **Sum replaced, conservative** (only RoomMapSmooth, Fence, OutdoorObject, RoomsAtNight, FloorLight, Terrain, room solve, lamp stuff) | **about 4.5-5.5** | | |
| Not removed: hook dispatch itself (callbacks 7.5-10 k a frame, 2.8-3.5 ms) | 2.8-3.3 | No: GI hooks sit on the same dispatch | Fewer fix hooks = fewer callbacks, but the draw hooks stay |
| Not removed: Wall AO pass (game) 0.08, Scene::BeginFrame 1.08, Lot load stages 0.12, WorldManager | ~1.3 | No | Hitch causes after the solve: D3D hooks 3.95, BeginFrame 2.42, lot load 1.31 per hitch |

### 2.2 What it adds

Unit costs we have [V]: Apex's draw callback 1.2-2.5 us a DrawIndexedPrimitive; a re-issued draw 0.21 ms per 946 draws a
frame in the driver (0.23 us); bridge in a fixed scene under 0.6 ms a frame for 1-2.5 k draws, about 0.25-0.6 us a draw
[R]. A shadow draw is a re-issue plus state (shader, VB, IB, declaration, matrices: ~5 calls), so **about 1.5-3 us each
[I]**. Draws per frame today: 1000-1600 near, 2100-2500 far.

| Item | Scenario | ms per frame [I] | Basis |
|---|---|---|---|
| Static shadow cache fill | One lamp changes or a lot loads: 300 casters x 6 faces culled to about 2.5 faces = 750 draws | 1.1-2.3 once per change, spread over frames at <= 0.5 ms a frame | Cache hit rate must be near 100% in steady state |
| Static cache, steady | No redraw | 0 | |
| Moving casters (Sims, doors) into the nearest M lamps | 8 Sims x 8 draws x M = 3 lamps x 1 face (spots) to 2.5 faces (points): 190 to 480 draws | 0.3-1.4; budget M = 2-3 spot-like lamps at <= 0.6 | The costly part (idea.md agrees) |
| Alpha-tested foliage in shadow passes | Extra texture binds; 16-24 draws a lamp | 0.05-0.15 | Foliage draws are few (P: instance picks 5-20 a frame) |
| Lamp table upload, constants, the lit-pass sampler binds | per shader family, SetTexture ~0.04 us | <0.1 | |
| Shader patching: per-draw extra loop over N lamps | GPU cost, not CPU | 0 CPU | |
| **CPU total, steady state** | | **0.3-0.9** | |
| **CPU total, change bursts** | | 1-2.5 for a few frames; capped by a budget | |
| GPU: shadow passes + per-pixel lamp loop (4 to 8 lamps per pixel) at 4K, stage 1 | AO for scale: 1.3 (VL) to 4.5 ms (Ultra) at 4K on the 4070 Ti SUPER | 1.5-3 [?] | GPU idle today; the laptop 30-series is 2-3x slower [I]. Target <= 2 ms |

### 2.3 Net

CPU (the limit today): remove about 4.5-6.9 ms, add 0.3-0.9 steady. **Net -3.6 to -6 ms a frame at 150 m [I]**, with the
caveat that this is the upper end of what survives (the shader families, section 4 risk R1, may keep some fixes).
Hitches: removing the solve takes out the dominant item of 81% of moving hitches and 100% of the still ones (P section
4); the remaining causes (hooks 3.95, BeginFrame 2.42, lot load 1.31, terrain 0.57, Script GC on the sim thread 19.6)
stay, so expect fewer and shorter hitches, **not zero**. GPU rises by a few ms and must stay under the new CPU time.

## 3. Proof plan for stage 1

Scope: one lot (the active lot of the harness save), direct light only, room solve skipped for that lot, floors and
walls shaders patched; Sims and objects keep the game's rig lights [idea.md "Hard parts"].

### 3.1 Harness jobs

Two legs. Both run on the same save and camera so they can be compared with the profile.

**Leg A, Apex runner** (`tools/ingame_runner`, in the GI worktree; author-runnable). New job
`realtime-gi-stage1-pan.json`, copied from `bench-apex-off-on.json` and `night-lot-switch-pan.json`:

- save `Sunset Valley` at night (winter, in-game evening, as the profile) and a second one of `ApexTest-Night` [?];
- variants (ABBA with `-AB`, order off,on,on,off): **off** = current Apex, GI switch off; **on** = same build with
  `[patches.RealtimeGI] enabled = true` and stage 1 only. Same ASI binary, one switch: "change one thing at a time".
- steps: `speed normal`; `camera_pan {lots 8, height 40, seconds 60, passes 12}` then the same with `height 150`
  (the two zooms of the profile); a `memory` step before, after 10 min and at the end; F8 screenshots at 6 fixed
  bookmarks (`tools/ingame_runner/bookmarks`), before the pans, solve-affected lot in view;
- run `run.ps1 -AB -Profiler Both` (frame times, visible freezes) plus a third pass per leg with the frame profiler
  (`[qol.frame_profiler] start_at_load`, Developer build; the runner cannot start it, README line ~391) for room-solve
  counters and the hitch report. The profiler arm is separate from the headline timing arm (it adds 0.3-0.5 ms).

**Leg B, our camera sweep** (`docs/280`, queue `camera-sweep-1117.json`, `camera-sweep-apexprof-1117.json`). Add a second
Bin state / arm `apex-gi` (the GI Apex build), one queue pair `camera-sweep-gi-1117` with arms `apex` and `apex-gi`, order
ABBA, one world (`bp-camsweep`; `ip-camsweep` second). The sweep gives zoom (5-180 m), rotation and pan segments that the
profile never covered, a better test for shadow cache invalidation (cameras moving into new lamps' range). Per segment
it reports frames, p50, p99, p99.9, max, frames over 25/50/100 ms and `apexDefHitches`; with `apexProfiler: true` also
`roomSolveCalls`, `roomSolveMs`, `hitchesLedByRoomSolve` [R, docs/280 §2-3]. Known gap there: its queues are one arm and
need the two-arm change; the sweep run is 182 s, so use several repeats (three per arm) for p99.9 [I].
Smoke job first (rule: before any queue over about 1 h): proves per-arm split, heavy tools off, the GI switch read back
from the Apex log (`[RealtimeGI] On:` line, to be added with a tested regex), the profiler running.

### 3.2 Arms and rules

| Arm | Build | Notes |
|---|---|---|
| A: baseline | Apex release/4.0.2, the same bridge package | GI off |
| B: GI stage 1 | same branch + stage 1, one switch | solve skipped for the GI lot only |
| C (optional): GI on, solve left on | B with solve not skipped | separates the cost of GI draws from the gain of removing the solve |
| M: memory arm | B and A, 30 and 60 min idle-pan, diagnostics `perfLogIntervalFrames` on in a separate load (it fragments the client heap) | address space only; not a timing arm |

Run on a quiet machine (the profile loads were flagged noisy at 11-15% external CPU; rerun any load with 10% or more
external load, a lower clock or a cut profile). Baseline table first: re-measure A in the same session as B, never reuse
the 11-16 numbers (different build). Both frame limiters lifted as in `bench-apex-off-on`.

### 3.3 Metrics

Per arm and per zoom (40 m, 150 m) and per sweep segment:

- frame time p50, p95, p99, **p99.9**, max (ms), FPS mean;
- hitches per minute (Apex rule: frame > max(2 x median of the last 120, 8 ms)) and **share and count led by the room
  solve** (must be 0 for the GI lot);
- frames over 50 ms per minute; visible freezes of 100 ms and 300 ms per 10 min;
- room-solve calls and ms per frame (P: 2.81 ms, 27 calls per hitch) for the GI lot and all lots;
- LotLightBridge ms per frame, D3D callbacks per frame, draws per frame (the shadow draws must show as a separate hook
  line `RealtimeGI`, with its own ms per frame and draws per frame);
- GPU frame time (Apex GPU pass profiler, or an event query pair) per pass: shadow, lamp loop;
- **address space** (the runner's `memory` step: free and largest free block below 4 GB, at load, +10 min, end of load;
  and our `diag: mem` free/big/priv, memcensus at +55 min) for A and B, same duration; bridge client copy MB (`shadowMB
  surf= buf=`) and the MANAGED/SYSTEMMEM/DEFAULT pool lines in the M arm; server working set and VRAM from outside
  (`nvidia-smi`, Task Manager data, read from the harness, no game focus);
- page faults per frame in hitch frames (P: 2504 vs 167), to check that nothing GI touches first-time memory.

### 3.4 Screenshot pair for the look

F8 at the same 6 bookmarks (camera positions from the bookmark set, paused game, fixed time of day, Sims removed or
paused so poses match), arms A and B, plus an optional C-day shot to prove daylight is unchanged. Look checks, by the
author and by diff image (not a pass/fail number, because the light is meant to differ):

1. a lamp on/off or colour edit shows in the next frame, no pause;
2. shadows of chair, plant and Sim (while paused, one Sim) lie on the floor and wall, soft edge widens with distance;
3. no light behind a wall or on another floor (leak check, 3 bookmarks across a wall and a floor);
4. lamp colour, cone and cut-off match the game's data (a debug dump of the lamp table against the game's lamp record
   fields, 0 differences in position, colour, cone, on/off);
5. overall brightness inside the "GI is darker and more contrasted" note, with the style pass off for stage 1; record the
   mean luminance of the floor and wall pixels lit by lamps, A and B, as an objective number.

### 3.5 Pass criteria (stage 1 goes on to stage 2 only if all hold)

| # | Criterion | Number |
|---|---|---|
| P1 | Room solve for the GI lot | 0 calls in all of arm B; other lots unchanged |
| P2 | Hitches per minute at 150 m moving | at least 40% fewer than A (the solve led 81% in P; the rest stay) |
| P3 | p99 and p99.9 at 40 m and 150 m | not worse than A by more than 0.5 ms; target p99 -3 ms or better |
| P4 | p50 | not worse than A by more than 0.3 ms (cost-neutral at worst); target -3 ms |
| P5 | Max frame and frames over 50 ms per minute | not worse than A; frames over 100 ms not more |
| P6 | Steady-state CPU of the GI hook | at most 0.9 ms a frame averaged; any frame over 3 ms must be in a burst after a lamp change, not in a still scene |
| P7 | GPU time of the GI passes at 4K | at most 2 ms on the 4070 Ti SUPER; the laptop GPU result recorded, not gating stage 1 |
| P8 | Largest free block | at +10 and +60 min, within 64 MB of A; free total within 64 MB; surface copy MB (client) within 5 MB of A |
| P9 | No `GetRenderTargetData` or lock on a GI resource | caller-address count of `GetRenderTargetData` from the GI code is 0; the pick read-back count is unchanged |
| P10 | Look checks 1-5 | all five pass on 6 bookmark pairs; any leak, flicker or colour mismatch fails |
| P11 | Stability | no crash, no ScriptError, no Error 12, lot loads to the end and saves, in all loads; a fail in one load is rerun, not averaged |

Statistical rule: two ABBA pairs per leg; the harness's own pair tables (`results.md`) give the verdict; with no clear
difference the result is "no clear difference", not a win [R, METHODOLOGY.md].

## 4. Risks

| # | Risk | Why | Check / mitigation |
|---|---|---|---|
| R1 | **Shader coverage**: some lit families already use all 16 samplers; GI needs the shadow atlas, lamp table and probe atlas as more samplers | idea.md hard parts; docs/engine/shaders.md patchers use "first free" sampler and constant | Offline test over every captured shader first (project rule); stage 1 floors and walls only, pack lamp data into constants and the existing lamp-map sampler slot; one atlas sampler for all lamps (as the 3.1 MB tile atlas does with `misc.x/z`) |
| R2 | **Hidden consumers of the room solve**: the solve may also feed lot-load completion (lot "lighting ready"), Sim light level, wall/floor edits, or save data | `0x006A8BA0` is called from the lot lighting update; lot load stages (0x00AEB306) sit next to it; [?] nothing verified | Static RE before building: callers and readers of the solve's outputs; skipping must still set the done flags. Test: lot load completes, save/load, build mode edits |
| R3 | **Per-draw cost and bridge channel**: shadow draws are D3D calls that cross the bridge; the render thread is the limit, not the GPU | Dispatch 3.3-5.7 ms a frame today (P section 7); 72 MB channel | Draw budget per frame, static cache, nearest-M moving lamps, pass criteria P6; count `RealtimeGI` draws; check `no response from server` / semaphore stalls in bridge logs |
| R4 | **Moving casters dominate** (Sims, doors, animated) | The one costly item in idea.md | Cap moving-caster lamps at 2-3; cull faces; single-face spots; fall back to blob/AO for the rest; test with 8 Sims in view |
| R5 | **Cache invalidation errors**: object moved/added, lot streamed in, floor/wall edit, lamp move, LOD switch | "correctness: follow the game's lamp data exactly" | Dirty tracking keyed by scene node IDs; a debug mode that redraws everything and diffs the cache, at 0 differences on the sweep |
| R6 | **Address-space regressions via locks/readbacks** | A single `Lock` of a 3 MB tile makes a 3 MB client copy; a readback adds 2 ms and a 13.6 MB SYSTEMMEM surface | Rule in 1.1; P8, P9; never use GPU readback for GI data; check the 64 KB `new[]` copies (never returned) for the lamp table |
| R7 | **Volume textures under the bridge** | D3D9 volumes cannot be render targets; the client's volume lock path was not read | Use a 2D atlas; confirm the bridge forwards volume sampling and `UpdateTexture`, or avoid volumes entirely [?]. Stage 2 gate |
| R8 | **VRAM on a 6-8 GB laptop** | The profile GPU had 16 GB; game + AO + picture passes + 50-150 MB GI + 4K targets | Measure VRAM on the laptop class (nvidia-smi); tile count follows free VRAM; fall back to K = 8 at 256 |
| R9 | **Sims and objects lit by rig lights** look out of place beside GI floors | idea.md; stage 1 does not touch them | Look check 5; accept mismatch in stage 1 and record it; stage 2 reads the probe volume for them |
| R10 | **The look**: real shadows are darker and sharper; "does not feel like Sims 3" | idea.md | Style pass is a stage-2 item; stage 1 keeps the game's lamp intensity law (per lamp luminance matched to the existing `rt_lamps_test` physical model) so the pair is comparable |
| R11 | **Overlap with existing GPU room lamps** (`RtLampGpu`, `PhysTiles`, kept rooms) | Apex already draws lamp cubes, kept atlases and relights | Reuse the physical model and the tile atlas rather than a new one [I]; GI replaces its use for the GI lot only; one owner per lot (stand-down) |
| R12 | **Foliage and alpha-tested casters** (SpeedTree, fences) | Alpha test needs texture binds and the right alpha reference | Per-family list; shadow pass draws only the alpha-tested families and tests leaf shape on one tree bookmark |
| R13 | **Noise in the measurement** | The profile loads had 11-15% external CPU; p99.9 needs several minutes | Quiet-machine check, repeats, the runner's `invalid load` rules; do not report a win under the run-to-run spread (p50 11.7-13.4, p99 33.6-37.8 across three identical far pans in P) |
| R14 | **Save compatibility and household rule** (our repo) | Our CLAUDE.md: household Sims untouched | GI changes lighting only; nothing it does touches Sim state or saves. No game-side data written; the solve skip must not change what the game saves [?] |
| R15 | **EA app 1.69.47 and other builds** | Apex supports Steam 1.67.2 and EA 1.69.47 addresses | Pattern scans only; the switch off on a build whose solve address is not found (same as GPU room lamps) |
| R16 | **Mod conflicts** (Sims3SettingsSetter, lot streaming mods, light mods) | Apex stands down for others today | Stand-down rules and compat scan; test with the baseline S3SS set |

## 5. Open measurements before a build is justified

1. Static RE of the room solve's outputs and readers (R2). Cheapest and highest value; decides whether skipping is safe.
2. Count draws per lamp's range and caster counts on the Sunset Valley bookmarks (offline from scene recordings) to turn
   section 2.2's draws into a measured number.
3. A throwaway test arm (the harness, not the player) that creates 24 DEFAULT cubes of 256 and draws a fixed set of
   re-issued draws into them each frame, no shading: gives the real address-space delta (expect under 100 KB), the real
   bridge cost per extra draw and the GPU time. It validates sections 1 and 2.2 at once, before any shader work.
4. VRAM on the laptop class.
