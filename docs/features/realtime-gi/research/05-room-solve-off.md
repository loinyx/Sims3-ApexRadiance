# R5: turning the room solve off safely

Lane R5 of the real-time GI idea ([../idea.md](../idea.md)). Question: who consumes the room light maps and the other
outputs of the lot room solve, does gameplay read any lighting value, and where can a per-lot "solve off" switch gate
the solve without breaking lot loading, thumbnails or impostors.

Static only, TS3W.exe Steam 1.67.2 (`game-copy/bin/TS3W.exe`, image base 0x00400000). Call sites were found by scanning
the `.text` section for `E8`/`E9` rel32 targets and imm32 references, then disassembled (capstone). REA timed out on
this binary during the lane, so no REA evidence ids are given.

Markers: **[V]** read in the disassembly or the decompiled scripts in this lane; **[D]** taken from an existing Apex
engine doc (`docs/engine/*.md`, which marks its own verification); **[I]** inferred, not confirmed.

## 1. Short answer

- **Gameplay does not read lamp light.** No script binding returns a light value from the solve or the maps. The
  darkness buffs depend on light objects being switched on, windows, and a room id. The solve can go without any
  gameplay effect. [V] (section 3)
- **The solve cannot simply be skipped.** Lot load stage 20 waits until every lit room has been solved at least once
  (room `+0x104 != 4`) and both of the level's AO solvers have run their first pass. A lot whose solve never runs never
  finishes loading. [V] (section 4.1)
- **Recommended gate:** keep `0x006A8BA0` (level rebuild and the two AO solvers) as it is, and cap the room pass of an
  "off" lot at class 0 through the room priority call `0x006A81DF`. Apex already owns that call (Faster Room Lighting's
  `PriorityHook`). Each room is solved once at the cheapest class, so it leaves state 4 and stage 20 passes; it is never
  refined to class 1 or 2. (section 5)
- **Leave the impostor and LOD-switch path alone.** `0x00ADBAD0 -> 0x006A80E0` solves every room synchronously without
  the scheduler or the priority [V]. It is not the per-frame cost, and Apex PR #94 already leaves it ungated.
- **Lot thumbnails need full readiness:** room `+0x104 == 2` and both AO solver states `== 2` [V]. While the
  ThumbnailManager captures a lot, the switch must step aside for that lot. (section 4.2)

## 2. What the room solve produces

| Output | Where | Written by | Read by | Status |
|---|---|---|---|---|
| Room light maps (walls, floors, ceilings, objects, basis), 3 classes x 4 maps | manager `+0x16C + class x 64`; per room class entry `+0xC` | step 1 lock and step 8 commit of `0x006A3C90` (`0x006A0E00`) | shaders through the binders in 4.3 | [D] |
| Shown class `room+0x104` (4 = never solved) | room | `0x006A20D0` room init sets `+0x100 = +0x104 = 4` at `0x006A217F`; `0x0069F5F0` copies `+0x100` into `+0x104` (called at `0x006A888C` in the room pass) | stage 20 (`0x0069F540(0)`: `!= 4`), lot thumbnail (`0x0069F540(1)`: `== 2`) | [V] |
| Ambients `+0x110` (floors and walls, `c4`), `+0x120` (objects), `+0x650` | room | ambient step `0x006A0F50` | lot and object shaders; probe multipliers | [D] |
| Normalisation `room+0x160` | room | `0x006A0230` at the end of the solve | next solve, the interior map transform | [D] |
| Specular probe multipliers `room+0x130` / `+0x140` | room | surface colour pass `0x006A0F00 -> 0x0069FF20` | InteriorWall `c5`/`c6` through `0x006B2F30` (0 until written) | [D] |
| Reflection probe re-arm | probe | a solve -> `0x006B1CA0(2.0)` | room cube capture | [D] |
| Wall AO image, solver states `lvl+0x2A4` / `lvl+0x2FC` (0 first pass due, 1 refine, 2 done) | level `+0x290` / `+0x2E8` | `0x00688920` slot `+0xC` (vtable entries `0x00FF0580`, `0x00FF05A0`, `0x00FF0720` [V]); wall AO step `0x0068B810` (vtable `0x00FF05B0` [V]) | AO bind `0x00688DB0` (states 1/2), stage 20 (`!= 0`), thumbnail (`== 2`) | [V] states; [D] rest |

The AO solvers are not lamp lighting: they shade outdoor walls against the sky. GI could replace them later, but phase 1
keeps them, because stage 20 and the thumbnail both wait for their states.

## 3. Does gameplay read lighting? No.

- **Script bindings** (`ScriptCore/ScriptCore/World.cs`): no extern returns a solved light, room brightness or map value.
  The only getters are `World_LightGetDimmerImpl` (`World.cs:78`), which returns the dimmer the script set itself,
  `World_AreHolidayHouseLightsEnabledImpl`, `World_IsDarkLotRenderingEnabled` and `World_GetUpdateLightRigsImpl`, all flags.
  Searched: every extern whose name has light, lit, dark, bright, lumin, shadow, illum or ambient. [V]
- **Darkness buffs** (ItsDark, ItsDarkNeutral, AfraidOfTheDark; `Sims3GameplaySystems/Sims3.Gameplay.Actors/SimUpdate.cs:614`)
  depend on:
  - `roomPerceivedToHaveLight`, true when an object in the room has `EmitsLight` (`SimUpdate.cs:336-347`). `EmitsLight`
    is a script property: `GameObject.cs:2244` returns false, and lamps return `IsLightOn()` (`LightGameObject.cs:973`). [V]
  - a window that connects to room 0 (`SimUpdate.cs:562`). [V]
  - `LotManager.LightingRoomIdIsOutside` (`Sims3GameplaySystems/Sims3.Gameplay.Core/LotManager.cs:725`):
    `World.GetLotLocation(..., eRoomDefinition.LightBlocking)` then room id == 0. This is a room id, not a light value. [V]
    Whether the native LightBlocking room ids come from the lighting level's room rebuild `0x006A7CF0` is unknown [I].
    The recommended gate never skips that rebuild, so the question does not block the design.
- **Photography:** `Photography.cs` and `PhotographableUtil.cs` have no light, dark or night term (grep only). [V]
  The photo image itself is a scene capture (`0x009DE140`, [D]), which renders with whatever the shaders draw.
- **Autonomy, sleep, reading** run in script and reach light only through the `EmitsLight` and `IsLightOn` state above.
  [V] for the paths found; there is no native "room brightness" query in the binding list.

The switch changes only what is drawn. Lamps still switch on and off in script, so buffs and autonomy
are identical for every Sim, the household's included.

## 4. Consumers that wait for or bind solve results

### 4.1 Lot load stage 20 (blocks lot loading)

`0x00AEA680` case 20 calls `0x00ADBBA0` (call `0x00AEB06F` [V]), which runs `0x006A5B50` for each level (call
`0x00ADBC07` [V]). Ready when [V]:

1. the level is not lit (`[lvl+0x81] == 0`), or
2. `[lvl+0x88] >= 0` requires both AO solver states `[lvl+0x2FC]` and `[lvl+0x2A4]` to be non-zero, and
3. every room of the level table (`+0x234` buckets, `+0x238` count) passes `0x0069F540(0)`: rooms outside the level's
   story range pass; the others need `room+0x104 != 4`.

So skipping the call `0x00ADB9AD` (all of `0x006A8BA0`) or the room pass `0x006A88B0` on a newly loading lot would hold
it in stage 20 indefinitely [I from verified pieces]. A lot in that state is never "done" (`renderer+0x1E`) and blocks the next LoD
promotion (`0x00C69FF0` [D]).

### 4.2 Lot thumbnails

The ThumbnailManager state machine `0x00D59C10` (strings `UI/ThumbnailManager/...`, `UI/ThumbnailManager/LODOverrideHook`)
calls `0x00AE06B0` at `0x00D5BE2F` [V]. That function jumps to `0x00ADBC30` [V], which runs `0x006A5BF0` per level (call
`0x00ADBC97` [V]). `0x006A5BF0` requires both AO states `== 2` and every in-range room `+0x104 == 2`
(`0x0069F540(1)`) [V]. When it is not ready, the state stays below 0x18 and is retried [V]; no timeout was found [I].
Script callers: `Bin.cs:160/249/811` (saving a lot to the library), `GenerateLotThumbnails.cs:39`, `Commands.cs:1867`;
`ThumbnailHelper.GenerateLotThumbnailSet` pauses the game around it (`ThumbnailHelper.cs:217-230`) [V].
Save-game thumbnails are a screen capture, not this lot path [I].

### 4.3 Shader binders (render thread only)

- Lot surface parameters ("LightMap", "SpilloverLightMap", "WorldToLotTransform", "LightBasisMap0..3",
  "CounterLightingConfig"; handles `0x011D04A4..0x011D04C8`, registered by `0x006A4700` [D]) are referenced only in
  `0x006A3FA0..0x006A7990` [V]. The binders and their callers [V]:
  - `0x006A7300` <- `0x00A9D122` (floor, "FloorEdgeHighlightFixupLOD");
  - `0x006A7400` <- `0x00A72583`, `0x00A791B6`, `0x00A7E1DD` (FloorFeature), `0x00A9D106`, `0x00AF9EF6` (LotWaterRenderer),
    `0x00B676FD` (pool geometry);
  - `0x006A7550` <- `0x00A9EF6A` (floor, "FloorThickness").
- Objects: the rig binder `0x006B8B30` calls `0x006A7700` (LightMap and LightBasisMap0..3 plus CounterLightingConfig)
  at `0x006B8BF1` and `0x006B9124`, and `0x006A4500` at `0x006B904A` [V]. Only rig mode 0 (roofed room) uses the
  interior technique that reads these maps [D].
- **Sims and outdoor objects:** rigs gather light objects directly (`0x006BB270`, `0x006BB2F0`) and never read the room
  maps [D]. Sims are therefore lit by the rig path whatever the solve does. Whether the Sim skin shaders have an interior
  technique that reads `LightMap` in rig mode 0 is open [I]. The idea's "Sims need the probe grid" point stands.
- **CAS:** CAS lights with CASDiffuseProbe and CustomLightRigging [D]; no lot lighting manager was found on that path
  [I].
- **Build mode previews and photography** draw through the same binders. They see whatever the maps hold [I].
- **Impostors:** the lot impostor material binds albedo, AO, water, detail and holiday lights and is lit when drawn [D,
  material inferred there]. Whether the impostor albedo capture bakes room light into the impostor is open [I].

### 4.4 Daylight is in the maps too

Window lights (RectangleWindowLight, CircleWindowLight) enter room light lists like lamps and carry daylight [D]. A
room map holds lamp light only at night. GI that replaces the maps must also bring daylight through windows, or
interiors go dark by day.

## 5. The per-lot "solve off" switch

### 5.1 Gate options

| Site | Effect | Verdict |
|---|---|---|
| `0x00ADB8F0` early-out on `mgr+0x4E` (set by message `0x06C11079`, cleared by `0x06C11080`, handler `0x00ADBD30`, vtable slot `0x0106814C`) [V] | The game's own "suspend lot lighting". Senders are build tools (`0x00CA7440` near "DeckFoundation", `0x00D3B9F0` near "UserToolWallDrag") [V]; it brackets a drag [I] | No: stalls stage 20 on a loading lot, and a build tool's end message clears the flag. Useful as proof that the update tolerates being idle |
| The CALL `0x00ADB9AD` (Apex `RoomSolveShare` call chain, PR #94) | Skips the rebuild, the AO solvers and the room pass | No: stage 20 and the thumbnail need the AO states |
| The CALL of the room pass `0x006A88B0` at `0x006A8C0E` (5-byte rel32; args stopwatch* and float, `ret 8`) [V] | Skips every room of the level | Only for lots that already passed stage 20, and it also stops lamp-edit re-solves. Coarse |
| **Room priority, CALL `0x0069E770` at `0x006A81DF`** (Apex `PriorityHook`, Faster Room Lighting) [D] | Return 0 for a room of an off lot whose class `room+0xF4` is above 0. The scheduler `0x006C5C20` only picks rooms with priority above 0.001 [D] | **Recommended** |

### 5.2 Recommended design

1. **Off set:** keyed by lot id (lot `+0x48`/`+0x4C`, or the story manager's `+0x90`/`+0x94` [D]), held outside the lighting
   manager. The manager is destroyed and rebuilt on demotion and re-promotion (stage 1 creates it [D]), so the setting
   must survive that. Proof of concept: one lot from the TOML. Later: every lot whose shaders are covered by GI.
2. **Class-0 cap:** in `PriorityHook`, for an off lot, return 0 when `room+0xF4 > 0`; otherwise keep the game's value.
   - A new room (`+0x100 == 4`) is invalidated to class 0 (`0x0069EF58` / `0x0069F1C5` [D]) and solved once at class 0:
     no 2D or 3D wall tests, 1 texel per tile, threshold 2 x 3/255 [D]. Then `+0x104 != 4`, so stage 20 passes.
   - After the class-0 solve, `0x0069EA70` moves the room to class 1, or 2 with Apex's direct step, in state 2 [D]. Its
     priority is then 0, so it is never picked again: no refinement.
   - A lamp edit invalidates the room. It gets class LodChoice only when it was shown at LodChoice or above, otherwise
     class 0 [D]. Shown stays 0 here, so the edit re-solves at class 0. That keeps unpatched shaders roughly current at
     the lowest cost. Variant **hold**: return 0 for any room with `+0x100 != 4` (no re-solve after the first).
   - Rooms in state 2 that are never picked are harmless for load and LoD [I]. Only the thumbnail check (4.2) waits for
     class 2.
3. **Keep:** the level rebuild `0x006A7CF0` (also the LightBlocking question in 3), both AO solvers (stage 20, the
   thumbnail, the wall AO image), the light gather (rig lists and probes), and the sync impostor/LOD path
   `0x00ADBAD0 -> 0x006A80E0`. That path calls `0x006C54E0` then `0x006A3EC0` for every room of the level table directly
   (calls at `0x006A8159`, `0x006A8160` [V]), bypassing the scheduler and the priority.
4. **Thumbnail exemption:** while `0x00AE06B0` is being polled for a lot (hook the CALL at `0x00D5BE2F`, or an entry
   layer on `0x00AE06B0`), drop that lot from the off set until it returns true, then put it back. Without this, saving a lot to the library can wait forever [I].
5. **What the maps hold for unpatched shaders.** Pick one by look:
   - (a) class-0 result as is (cheap, light leaks through walls: class 0 tests no walls [D]);
   - (b) neutral: at the end-of-solve hook `0x006A3E65` (Apex `FinalizeHook`) clear the rgb of an off lot's room maps
     and keep the ambient alpha, so unpatched families draw the room as "no lamps" (ambient x alpha only).
   Patched GI families ignore the map, except for its alpha ambient term, which the GI shading must replace or keep.
6. **Apex features to stand down for off lots:** level light share (gather share, cross-story wall test), outdoor
   delta, RtOutdoorLamps relights, room-map smoothing, Rooms at Night base, the Faster Room Lighting drain for the
   priority lot, and the LotLightBridge per-draw fixes (idea: "mostly gone"). Each reads or writes maps that no longer
   carry the final light.

### 5.3 Unload and LoD paths

- **Demotion to impostor:** the lighting manager and its map sets are freed (`0x006A805A` teardown [D]). Nothing to
  gate. The off set lives outside the manager.
- **LoD switch and impostor build** (`0x00AD9E30 -> 0x00AEB3F0 -> 0x00ADBAD0`, call `0x00AEB404` [V]; wait loop
  `0x00AD97E0` [D]): solves synchronously, untouched. On an off lot each room is solved at its current class, usually
  class 0 [I]. That is the one place an off lot still pays a full room pass.
- **Re-promotion:** a new manager starts every room at state 4 and class 0, and the cap applies again from stage 1.
- **Floor switch** (`0x006C7451` invalidates the shown story's rooms [D]): goes through invalidate and the priority, so
  the cap holds.
- **Dusk:** the game re-solves nothing when lamps switch on [D]. Same with the switch.

## 6. Hook-site notes

- `0x006A81DF`: Apex already redirects this CALL (`room_light_queue.cpp`, header lines 9-12, 27-28); the cap is a
  condition inside the existing hook, not a new site.
- `0x00D5BE2F` / `0x00AE06B0` (the thumbnail exemption): no Apex, S3SS or S3PP hook was found there. Our repo's
  `docs/68-hook-conflict-map.md` and `docs/51-inference-ledger.md` have no entry for these addresses, `0x006A8C0E` or
  `0x006A88B0` (grep).
- Threads: the lot pass, the scheduler `0x006C5E20` and the binders run on the render thread [D]. Room light lists are
  written on the light-tree thread [D]. The cap reads only `room+0xF4` and the lot id, on the render thread.

## 7. Open questions

1. How the 2.8 ms of "Lot room solve" (inclusive `0x006A8BA0`) splits between the rebuild `0x006A7CF0`, the AO slots
   and the room pass `0x006A88B0`. This decides how much the class-0 cap saves. Frame profiler targets: the call
   `0x006A8BAD`, the two `call edx` at `0x006A8BD7` / `0x006A8BF1`, the call `0x006A8C0E`.
2. Does the LightBlocking room definition (`World.GetLotLocation`) come from the lighting level's rooms? It does not
   matter while the rebuild is kept.
3. Do the Sim skin shaders read `LightMap` in rig mode 0?
4. Does the impostor albedo capture bake room light (night impostors)?
5. Does the ThumbnailManager lot capture time out, and does it force a LoD switch through `LODOverrideHook`, which
   would sync-solve the lot anyway?

## 8. Proposed ledger entries (our repo, docs/51; not added in this lane)

- `0x00ADB8F0` returns at once while `mgr+0x4E` is set; message `0x06C11079` sets it and `0x06C11080` clears it
  (handler `0x00ADBD30`); the senders are build tools [V]. What they bracket: inferred.
- Stage 20 readiness (`0x006A5B50`) and lot thumbnail readiness (`0x006A5BF0`) conditions, as in 4.1 and 4.2 [V].
- Room init `0x006A20D0` sets `+0x100 = +0x104 = 4`; `0x0069F5F0` copies `+0x100` into `+0x104` [V].
- The sync level solve `0x006A80E0` solves every room of the table directly, without the scheduler [V].
- Script surface: no binding returns a light value; the darkness buffs use `EmitsLight`, windows and a room id [V].
