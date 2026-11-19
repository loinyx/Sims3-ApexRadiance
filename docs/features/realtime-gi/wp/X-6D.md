# X-6D: which per-draw lighting fixes the GI carrier and the lamp index replace

**Status:** done (design note, no code). Branch `gi/x-6d`, commit "docs: real-time GI note on retiring the per-draw
lighting fixes".

Source: [research 09 §6](../research/09-salvage-apex-dropped.md) (row 6d), [research 00 §7](../research/00-camera-pan-profile.md),
[research 02 §3.1](../research/02-shader-integration.md), [research 12 §2.1](../research/12-prior-art-lighting-gi.md),
[design](../design.md) §3.1, §3.6, §3.8, §11, plan S3-01 to S3-G1, [S0-05](S0-05.md), `features/lot_light_bridge.cpp`
(`ClaimSource`, `OnDrawInnerCore`), the night-lighting pages, and the per-draw light cache of the draft branch
`feature/per-draw-light-cache` (`FixLaps`, `ObjDrawMemoFor`, lazy shader restore; read only).

Marks: **[V]** verified in the source or a page named beside it, **[I]** inferred, **[O]** open.

## 1. Verdict

1. **A deferred or screen lamp pass replaces none of the fixes exactly. Do not build it for this purpose.**
   - Before the scene it needs this frame's depth, which the game does not have: rejected in research 02 §3.1 [V].
   - After the scene it can only add or multiply. The fixes are not additive: outdoor objects are
     `max(sun + rig or per-pixel lamps, ground)`, floors `max(floor map, world atlas)` before the game's lamp scale,
     walls a gain on the baked term with the game's bloom alpha kept [V, objects-and-rigs, floors, walls pages]. A pass
     after the shader has run cannot take the rig term back out of the pixel [I].
   - It also needs albedo and normal targets that do not exist yet (research 14; GB-01 delivered capabilities and
     camera motion only) [V].
2. **The route of design §3 replaces five fix families:** the texel-space carrier takes the room-lit ones, the lamp
   index block (S3-02, looped form of S0-05) the outdoor lamp-pick ones, the rig write the furniture one.
3. **"Replace" means a stand-down per lot, not deleted code.** With `lots = active` and `nightOnly = true` every fix
   still runs for neighbour lots, by day, and with `RealtimeGI` off [V, design §3.8, §5]. The per-draw light cache
   stays the answer for those cases; design §11 calls it superseded on GI lots only.
4. **What always stays per draw on a retired family:** the shader class lookup and the swap to the patched copy and
   back. Only picks, constant runs and their restores go [I]. The lazy shader restore of the per-draw light cache
   branch cuts exactly that rest, so it keeps its value after S3; the outdoor object draw cache does not on GI lots.

## 2. Table: every claim source

Far-pan cost is research 00 §7 (ms a frame, run C) [V]; "-" means not listed there (small or not drawn in that run).

| Claim source | What it does per draw | Far, ms | Replaced by | Verdict |
|---|---|---|---|---|
| RoomMapSmooth | Patched copy with a B-spline read, map constants, arch rows, caps [V] | 0.54-0.63 | Carrier: the copy holds smooth light at `bufferScale` 2-4, the game's shader reads it bilinear | **Retire in S1** |
| IndoorBasisSmooth | Bicubic read of the room basis maps, capped by the bilinear read [V] | - | Directional-map copies (S3-04) | **Retire in S3** |
| IndoorObjectSmooth | The same on indoor objects [V] | - | Directional-map copies (S3-04) | **Retire in S3** |
| OutdoorObjectGroundLight | Lamp pick at the object, up to 8 lamps x 4 rows as one constant run, atlas set, restore [V, `kObjectPixelLamps` = 8] | 0.37-0.75 | Lamp index cells + frame constants (S3-01, S3-02) | **Retire in S3**, first |
| FenceStairGroundLight | The same per instanced group, plus cones, per-lamp visibility and the floor-tile shadow [V, fences page] | 0.55-0.69 | Lamp index cells; table carries cone and shadow rectangle | **Retire in S3**, second |
| WindowOutdoorLight | Outdoor terms on window frames, experimental, off [V] | - | Lamp index cells | Retire with the objects; no own test |
| "several fixes" | A draw claimed by two or more sources | 0.70-0.73 | Follows its members | **[O]** composition unknown until the finer cost log of the cache branch prints its three costliest sets in game |
| RoomsAtNightFurniture | Reads, edits and restores four vertex-light rows and the ambient cube weight around the draw [V, source] | 0.23-0.38 | Rig write cached per model (S3-03, S3-05) for the rows; the cube weight needs S3-04 | **Retire in S3**, after the rig write; cube weight **[O]** |
| FloorLight, OutdoorFloorLight, SnowFloorLight | `max(floor map, world atlas)` in a patched copy [V] | 0.07-0.11 | Could be baked into the floor copy of a GI lot [I] | **Keep.** Gain under 0.1 ms; the atlas is world data the carrier does not own |
| ExteriorWallGain | Reads, scales and restores one constant [V] | 0.07 | Could be baked into the wall copy; clips in 8 bit above 100% [I] | **Keep.** Also needed by day |
| WorldTerrainLight, RoadLight, LotLightAtlas, LotSnowLight | Terrain, road and lot-grass consumers of the world atlas | 0.10-0.11 | Nothing: design §3.8 keeps the terrain stamp [V] | **Keep** |
| FoliageVertex | Patched vertex shader, 82 + 27 vertex constants saved and restored [V, source] | 0.19-0.27 | Nothing: `ps_2_0` and vertex-lit, no dependent reads [V, research 12 §2.1] | **Keep**; candidate for the lazy restore |
| ObjectRigNightFix, FoliageMoonShadowPS | Moon-shadow shader replacement, no lamp data [V] | - | Nothing: not a lamp fix | **Keep** |
| RoofLampFix, RoofSnowLampFix, LakeLampFix | Lamp light on surfaces with no room map; two are extra additive draws [V] | - | The looped index block fits the roof shader (110 of its free slots, S0-05) [I] | **Keep until S4**; the additive draws stay in any case |
| SnowOnObjectGroundLight, SnowOnStairGroundLight | Ground light on snow cover and relief | - | Lamp index if they carry lamp rows [O: not read for this note] | Decide after the fences |
| CinemaMarqueeDayBloomGuard | Daytime bloom guard | - | Nothing | **Keep** |
| Left to the game | Class lookup only | 0.09-0.11 | Nothing | Stays |

Sum of the retired rows, far pan: 2.4-3.2 of the 3.3-3.8 ms [I, from the table; assumes "several fixes" is mostly
objects, fences and furniture, as research 12 §2.1 does]. The rest after S3 is about 0.6-0.9 ms plus the shader swaps
of the retired families, not yet measured apart [O].

## 3. Why the index block is not bit-equal, and what S3-02 must carry to come close

The per-draw fixes decide per object or per group; the index block decides per pixel by cell. Four differences
[I, from `ObjDrawMemo`, `PickFenceLamps` and the fences page]:

| Per draw today | In the index block | Consequence for S3-02 |
|---|---|---|
| Up to 8 lamps an object or group | Design says 4 a cell | Cells need 8 entries (two index texels), or pictures differ wherever 5-8 lamps reach one object. The looped form costs the same slots for any count [V, S0-05] |
| Wall test and partial visibility at the object centre or group centre (`vis`, "dimmed") | Visibility decided at the cell | Store a weight beside each index, or shadow per pixel from the atlas. Differences up to half a cell at wall lines are expected and are an improvement, not a fault |
| Ground pool gate from the object's height above the lot, highest lamp head, ground share | Per pixel from world height and frame constants | Equal on ground objects; elevated objects and balcony fences need the lot height in the frame block |
| Per-object exceptions: a gate in a fence gap (`NoteGate`), an object set in a wall (`kWindowWallSkip`), the reach-law copy chosen per group | No per-object data in a cell | These stay per draw: one flag or the copy choice, no pick. `NoteGate` keeps running, as it does under the draw cache [V] |

## 4. Order and exactness checks

Each retirement is proven in two steps, so a difference is never ambiguous:

- **Transport check:** the new path carries the *old* lighting (the game's map, or the per-draw pick) and must equal
  the per-draw fix. This is the exactness test.
- **Look check:** the new lighting through the new path, judged by the look pairs of design §9. Not an exactness test.

| Step | Retire | Needs | Transport check (must hold before the stand-down ships) | Gain to confirm |
|---|---|---|---|---|
| 0 | Nothing. Land the finer cost log and check mode of the per-draw light cache branch | Its in-game run | Its own: `differed` = 0 | Baseline by part: pick, sets, shaders, restore; the "several fixes" sets |
| 1 | RoomMapSmooth on the GI lot | S1 carrier and swap | Fill the copy with the game's map resampled by the fix's own B-spline and caps; still-camera night pictures on 3 lots against the per-draw fix: within 1/255, except a 1-texel band at wall lines, counted | 0.5-0.6 ms; the fix's draws a frame on the lot = 0 |
| 2 | OutdoorObjectGroundLight | S3-01, S3-02 | Check switch (developer builds): the per-draw pick still runs and is compared with the cell at the object's point: same lamp set, same colour bits; counter "pick not in cell" = 0 over a pan. Pictures within 1/255 away from cell borders (plan S3-02) | Pick, sets and restore parts of step 0 gone |
| 3 | FenceStairGroundLight, WindowOutdoorLight | Step 2, cone and shadow rectangle in the lamp table | The same check per group centre; a balcony band and a gate in a fence gap in the picture set | 0.55-0.69 ms less the swaps |
| 4 | IndoorBasisSmooth, IndoorObjectSmooth | S3-04 | As step 1 on the four directional maps; first confirm how readers weight the maps (S0-04) | Small; removes members of "several fixes" |
| 5 | RoomsAtNightFurniture (vertex-light rows) | S3-03, S3-05 | Rig rows written at the source equal the rows the per-draw edit set, bit for bit, on a Sim-free furniture set in a lit and an unlit room | 0.23-0.38 ms less the cube weight |
| 6 | Roofs (lamp data only) | S4, step 2 | As step 2 on the roof shader; one looped shader created on the device first (S0-05 open point) | Not in the far-pan table; measure first, skip if under 0.1 ms |

Rules for every step:

- A stand-down is per lot and per fix, logged with its reason (design §3.8). A surface the swap or the cells miss
  falls back to the per-draw fix for that draw, counted; the counter is part of the pass criterion.
- The check switches follow `draw_light_cache_check`: both paths computed, compared, the first difference logged,
  the new path turned off for the session after a difference.
- S3-G1's "per-draw time of the covered fixes removed" is read from the cost log of step 0, by part, not from the
  hook total.

## 5. Findings

- **[V]** 24 claim sources exist; the far-pan table of research 00 lists 9 of them plus "several fixes".
- **[V]** The per-draw light cache keys the object answers by world matrix and law and holds pick, share, lamp head,
  pool gate and wall point: exactly the values the index block makes unnecessary. Its lazy shader restore is
  independent of them.
- **[I]** 2.4-3.2 ms of 3.3-3.8 ms is retirable on a GI lot; design §7's "1.5-2 ms of per-draw picks in S3" is the
  S3 share and agrees.
- **[I]** A screen pass cannot be exact because the fixes are `max` laws inside the game's shader.
- **[I]** Floors and the wall gain are not worth retiring: under 0.2 ms together, and both are needed off GI lots.

## 6. Open issues

- No game run: every ms figure is from research 00, taken before the cost log was split. Step 0 replaces them.
- The composition of "several fixes" and the size of the remaining shader swaps decide whether S3 reaches the
  1.5-2 ms of design §7.
- Design §3.6 item 3 says 4 indices a cell; §3 above asks for 8 and a weight. S3-02 should settle it and edit the
  design in the same commit.
- The snow cover and snow relief handlers and the ambient cube weight of RoomsAtNightFurniture were not read to the
  end; their rows are marked [O].
- Non-GI lots and daytime keep the full per-draw cost until `lots = all` and `windowLights` (S4).

## 7. Unblocks

S3-02 (cell capacity, weight, per-object flags, check switch), S1-G1 (step 1 transport check), S3-G1 (pass criterion
by part), and the review of the per-draw light cache branch (keep the lazy restore long term; the object cache is
interim on GI lots).
