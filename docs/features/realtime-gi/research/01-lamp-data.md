# Real-time GI research 01: the lamp data the game gives us

What a real-time GI path needs to know about every light in The Sims 3 (Steam 1.67.2, `TS3W.exe`, image base
0x00400000), where the game keeps it, how a change reaches it, and what Apex Radiance already reads. Static analysis
only (disassembly of the executable, the decompiled script assemblies); no game run.

Markers: **[verified]** = read in the executable's bytes or the script code for this page, or already marked verified in
the engine pages it cites; **[inferred]** = follows from names, call shapes or behaviour, not proven by the bytes.

Engine background: [light objects and rigs](../../../engine/light-objects-and-rigs.md),
[room light maps](../../../engine/room-light-maps.md), [terrain and light bake](../../../engine/terrain-and-light-bake.md).
This page does not repeat their tables; it adds what the GI needs on top and the new findings (sections 3 to 5).

## 1. Summary

- Every emitter the GI must reproduce is a `BaseLight` object of one of 9 classes (types 3..11) made by the factory
  `0x006AC590`. Lamps, street lamps, candles, fireplaces, fires and the window lights all use it. TVs and glowing Sims do
  not. [verified for the class list and the script callers; inferred for TVs and Sims, section 6]
- Per light the GI needs about 20 fields, all at fixed offsets in the object (section 2). Apex already reads all of them:
  `RtOutdoorLamps::ReadLamp` and `WorldLaw` (`features/rt_outdoor_lamps.cpp`), with the evaluation law per type in
  `features/gpu_lamp_law.h`.
- "On" is the dimmer, not a flag: the script switches a lamp by setting its dimmer to its intensity or to 0. The dimmer
  is the light's `+0x10`. The engine's own lit bit (`+0x100` bit 0x20) is a second gate that the light tree sets.
- The script never touches a light directly. Each change is a render message from the simulation thread, run on the
  main (render) thread by the light manager's handler `0x006B1850`. Creation, removal, moves, the dimmer, the colour and
  enable all arrive that way. [verified for the send and the dispatch; the drain thread inferred from docs]
- One writer is not on the render thread. The light tree's entry update (`0x006C7BA0`, "the light tree thread" in
  Apex) rewrites the lit bit and the lit colour `+0xE0`. A render-thread reader must take a snapshot and tolerate a
  value one frame stale.

## 2. Per-light fields the GI needs

All offsets are from the light object. Types: 3 BareBulb, 4 Spot, 5 ShadedLamp, 6 TubeLight, 7 RectangleWindowLight,
8 CircleWindowLight, 9 RectangleAreaLight, 10 DiscAreaLight, 11 BareBulb world/street class.

| GI input | Where | Notes | Marker |
|---|---|---|---|
| Class / type | vtable `+0x00`, int `+0xB0` | 9 vtables `0x00FF42A0..0x00FF4570` | verified |
| World position | float4 `+0x120` (also vfunc `+0x24` = `0x009691E0`) | The lamp head; every falloff measures from here | verified |
| Base colour | float4 `+0xF0` | What the user picks (CASt / colour menu) or the object's stock colour; Apex's tint changes it | verified |
| Dimmer (intensity) | float4 `+0x10`, all four lanes the same | Written by `0x006BDD10` (the dimmer setter, section 4). 0 = switched off by the script | verified |
| Effective colour | float4 `+0xE0` | `+0xF0 x +0x10` while bit 0x20 is set, else 0 (`0x006BDCA0`, `0x006BDD10`) | verified |
| Fade | float `+0x20` | 0..1, written by `0x006BC430` from the light manager (`0x006AC820`) and set to 1 at construction | verified write; meaning (animated switch) inferred |
| Range | float `+0x130` (vfunc `+0x28`) | Typical 40..100; the falloff is `min(Cmax, k2 x range x I / (k1 x d^2))` | verified |
| Influence rect | float[4] `+0x134` (vfunc `+0x50` = `0x006BDDF0` sets it) | About 50 m wide; useless as a visual radius (light-objects page, pitfalls) | verified |
| Spot cone (type 4) | axis `+0x170`, offset `+0x158`, scale `+0x154` | `sat((a . u - off) x scale)` | verified (Apex law, checked offline) |
| Shade cones (type 5) | a1 `+0x1A0`, o1 `+0x174`, a2 `+0x190`, o2 `+0x170`, spill `+0x150` | Up and down cones plus the light outside them | verified |
| Tube (type 6) | end `+0x170`, length `+0x1A0` | Segment light | verified |
| Area (types 9, 10) | U `+0x170`, V `+0x190`, N `+0x180`, extents `+0x1A0`/`+0x1A4` | One-sided, no N.L | verified |
| Window (types 7, 8) | own evaluation `0x006BEFD0` / `0x006BF880`, rect setter `0x006BE780` | Follow the sky (vfunc `+0x18` = `0x007EAEA0`, true); Apex has no GPU law for them | verified (vtable read) |
| Room | int `+0x08` (valid with flag 0x04) | 0 = outdoors on every story | verified |
| Story | int `+0xD0` | | verified |
| Lot | uint64 `+0xC0`/`+0xC4` | 0 = world (street) light; set by `0x006BDC40` on a placement | verified |
| Owner cell container | ptr `+0xD4` | Set by the constructor (`0x006C0209`); the `this` of the cell register/remove/move calls | verified (new) |
| Flags | byte `+0x100` | 0x01 alive, 0x02 in the cells, 0x04 room known, 0x08 dimmer follows a linked light, 0x10 lot assigned, 0x20 lit, 0x40 enabled | 0x01..0x04, 0x20, 0x40 verified; 0x08 and 0x10 meaning inferred (section 3) |

What decides whether a light emits: alive (0x01) and enabled (0x40) and lit (0x20) and `+0x10 > 0` and `+0x20 > 0`.
`+0xE0` already folds in lit and the dimmer, so `+0xE0 x +0x20` is the emitted colour scale. [verified for `+0xE0`;
the fade product is what `ReadLamp` in `lot_light_bridge.cpp` uses, inferred to be the full rule]

Not in the light: the room solve's constants k1, k2, Cmax (`[0x011D1160]`), the street factor 3.333 (`[0x01158DA8]`), the
night level (`lightMgr+0xF0`) and the manmade light intensity (`lightMgr+0xF4`, unverified).

## 3. Lifecycle inside the engine (new)

Light vtable slots used by the change paths (read from the 9 vtables, identical for the lamp classes):

| Slot | Function | Role | Marker |
|---|---|---|---|
| `+0x18` | `0x00620D60` (false) / `0x007EAEA0` (true, window classes) | "Follows the sky": the light tree's long path | verified (bytes); meaning from Apex's LampMarkFilter notes |
| `+0x20` | `0x00620D60` / `0x007EAEA0` (street class `0x00FF42F8` only) | World light test | verified |
| `+0x34` | `0x006BC0C0` | Clears 0x02 and removes the light from the cells (`0x006B6090`) | verified |
| `+0x44` | `0x006BC090` | If in the cells (0x02): cells move/toggle `0x006B6590` (arms the terrain countdown for world lights) | verified |
| `+0x48` | `0x006BC0E0` | Re-registers: in the cells only if alive and enabled and (lot 0 with 0x10, or room 0 with 0x04) | verified |

So the light cells (`lightMgr+0x104`) hold only outdoor and world lights; indoor lamps live only in their room lists
on the light tree. A GI that wants every lamp must enumerate (`0x006ACF70`) rather than walk the cells. [verified]

Setters:

| Setter | Writes | Then calls | Marker |
|---|---|---|---|
| `0x006BDD10(light, dimmer, notify)` | `+0x10` = dimmer (ignored if it moves by less than 0.01, `[0x0106D6CC]`); `+0xE0` | vfunc `+0x44` when notify | verified |
| `0x006BDCA0(light, lit)` | bit 0x20; `+0xE0` | vfunc `+0x44` | verified |
| `0x006BC050(light, enabled)` | bit 0x40 | vfunc `+0x48` | verified |
| `0x006BDC40(light, lotLo, lotHi)` | `+0xC0`/`+0xC4`, bit 0x10 | vfunc `+0x48`, `0x006BC430` | verified |
| `0x006BC3E0(light, rgba)` | `+0xF0`, `+0xE0` (MOVAPS: 16-byte aligned argument) | | verified (light-objects page) |
| `0x006BDA90` | `+0xF0`, `+0xE0` at creation | | verified (light-objects page) |
| Constructor base (around `0x006C0180`) | flags = 0x60 (lit, enabled), `+0x10` = 1, `+0x20` = 1, `+0xD4` = cells | `0x006BDD10` | verified |

Creation from an object: `0x006B0EF0` switches over a light record's type (jump table `0x006B1168`) and calls the factory
with types 3..8 (and the rest); its only caller is `0x006B1539`. That the records come from the model's LITE resource is
**inferred** (the message carries a 16-byte key, section 4).

## 4. How changes are signalled

### 4.1 From the script (simulation thread) to the light manager

The script's calls go through a per-object light component (component id `[0x011E8FC0]`, looked up with `0x00935770`),
which keeps a copy of the value and posts a render message through `0x00692F70` -> `FrameDb_Get 0x005934C0` ->
`RenderMessageBuffer_Post 0x00593E60` (critical section `+0xD0`). The main thread drains these messages in
`FrameDb_DrainEvents 0x005937B0`. [verified send path; drain from the frame-loop study, verified there]

The light manager subscribes to 7 message ids (calls at `0x006B1724..0x006B17B5`, vtable slot `+0x2C`, priority -10000)
and handles them in `0x006B1850`: [verified]

| Message id | Sent by (component method) | Script source | Handler | What it does | Marker |
|---|---|---|---|---|---|
| `0x1361A133` | `0x00949220` (sends a 16-byte key from the component) | object added to the scene (inferred) | `0x006B1430` | Builds the object's lights (path to `0x006B0EF0` and the factory) | send and handler verified; "added" inferred |
| `0x5361C4C5` | `0x00949290` | object removed (inferred) | `0x006B1610` | Removes the object's light entries (`0x006C4F50`, `0x006B1580`) | send and handler verified; "removed" inferred |
| `0x3361F6C9` | `0x00949935` | transform (object placed or moved) | `0x006B09C0` | New matrix and lot id per light (`0x006BDC40`) | verified; the name "transform" from Apex's LampMarkFilter notes |
| `0xB361CC42` | `0x00949300` | `World.LightSetDimmer` (`World_LightSetDimmerImpl 0x0078A3A0`) | `0x006B0AB0` | `0x006BDD10(dimmer, 1)` on every light of the object; if one changed, flags the object's light tree entries (`0x006C4CF0`) | verified |
| `0x966EC80A` | `0x009496C0` (sent only if a channel moved by more than 1.5e-5) | `World.LightSetColor` (`World_LightSetColorImpl 0x0078A410`) | `0x006B0B50` | `0x006BC3E0` on every light of the object | verified |
| `0xF33E398B` | `0x00949ED6`, also from scene code (`0x00702DC6`, `0x007039D5`, `0x00704EEA`, `0x00944DCF`, `0x0095B7D4`) | object shown or hidden (inferred) | `0x006B0940` | `0x006BC050(enabled)` on every light of the object | handler verified; source inferred |
| `0xE128229B` | `0x0096077C`, `0x00960F4D` | object alpha (Apex's name) | `0x006B0C20` | Sets bit 1 of the object's light record (`+0xA0`) and flags its tree entries; the tree turns it into the lit bit (below) | handler verified; meaning inferred |
| `0x09714EE4` | `0x00949360` | unknown | `0x006B08A0` | For the object's world lights, arms the terrain countdown (`0x006B5730`) | verified; source unknown |

### 4.2 Inside the engine: the light tree

`0x006C4CF0(objId)` flags the object's entries in the light update tree (`lightMgr+0xD4`). The tree's room update
`0x006C7250` runs the entry update `0x006C7BA0` for each flagged entry: it finds the lamp's room (`0x006C7B20`), applies a
linked light's dimmer when flag 0x08 is set (`0x006C7C5C` -> `0x006BDD10`), rewrites the lit bit from the object record's
bit 1 (`0x006C7CB8` -> `0x006BDCA0`), and marks the room it left and the room it is in (`0x006C7CCA`, `0x006C7CD6` ->
`0x006C7160`). [verified bytes; the flag 0x08 reading inferred]

### 4.3 Which game action produces what

| Action | Path | Marker |
|---|---|---|
| A Sim or the player switches a lamp | `LightGameObject.SwitchLight` -> `World.LightSetDimmer(Intensity or 0)` (`Sims3.Gameplay.Abstracts/LightGameObject.cs`, `SwitchLightInternal`, lines 1398-1413); `IsLightOn` = dimmer > 0 (line 1367) | verified |
| Dimmer setting (Low/Normal/High/Custom) | `LightGameObject.SetLightIntensity` (line 1470) -> the same dimmer message | verified (call site); values not read |
| Colour from the light menu, CASt or tomb triggers | `LightGameObject.SetColorLight` (line 1526) -> `World.LightSetColor` | verified |
| Lamp moved or rotated, dragged in Build/Buy | transform message `0x3361F6C9` | inferred from the handler's matrix payload and Apex's drag work |
| Lamp bought, deleted, lot loaded or unloaded | `0x1361A133` / `0x5361C4C5` | inferred |
| Dusk and dawn, weather | no message: `lightMgr+0xF0`, the sky; window lights follow the sky through the tree | verified for the night level (light-objects page) |
| Curtains | `Curtain.UpdateWindow` sets the window's dimmer to 1 - opacity and its colour to the filter colour (`Sims3.Gameplay.Objects.Decorations/Curtain.cs`, lines 100-110) | verified |

Apex already hooks this stream: `LampMarkFilter` sits at `0x006C7CD6` / `0x006C7CCA` / `0x006C7939` and turns each entry
update into an edit note (`EditLastTick`, `EditSerial`, `HandledLately`), and `OutdoorDelta` / `GpuRoomLamps` redraw a
lamp edit on the GPU in the same frame (`docs/features/night-lighting/gpu-room-lamps.md`). A GI path can take its "lamp
changed" events from there instead of adding hooks.

## 5. Threads and read safety

| Who writes | Thread | Fields | Marker |
|---|---|---|---|
| Script calls (section 4.1) | posted from the simulation thread, run on the main thread in the message drain | everything | verified send; drain thread from the frame-loop study |
| Light tree entry update `0x006C7BA0` | the light tree thread (Apex: `g_gatherThread`, "on another thread") | `+0x10` (linked lights), bit 0x20, `+0xE0` | write verified; thread from Apex's own measurements |
| Room solves (readers) | light tree thread | read only | Apex docs |

Rules for a GI reader:

1. Read on the render thread, once a frame (or every N frames), into Apex's own snapshot; never keep a raw pointer
   across frames without re-checking the vtable against the 9 classes and the alive bit. Light objects are created and
   deleted only in the message drain on the main thread, so a pointer read and used within one render-thread frame stays
   valid. [inferred from 4.1; the free path in `0x006B1610` not traced to the delete]
2. Wrap every read in SEH, as `ReadLamp` already does.
3. `+0xE0` and bit 0x20 can change under the reader from the light tree thread. Each float is one aligned 4-byte store,
   so no value is torn, but `+0xE0` and the flags can disagree for one frame. Use `+0xF0 x +0x10` with the lit bit for the
   colour, and debounce a lit change over a frame. [inferred]
4. Never call the game's setters, `0x006B58F0` or the cell functions from another thread (light-objects page, pitfalls).

Cost: `0x006ACF70` visits every light in the world (a few hundred on a big world). Apex's readers already run it every
20 frames; a GI snapshot can reuse that list and refresh only the lamps an edit note names. [inferred]

## 6. Light sources by kind

| Kind | Is it a BaseLight? | Where its data is | Marker |
|---|---|---|---|
| Ceiling, table, floor, wall lamps | Yes, types 3..6 | section 2 | verified |
| Spots and floods | Type 4 | cone fields | verified |
| Street lamps | Type 11, lot 0; two lights per post (range 97 at 1.7 m and 40 at 3.7 m) | | verified (F8 dumps) |
| Area lights (park lots, some EP objects) | Types 9, 10 | | verified |
| Window light (sun or sky through a window) | Types 7, 8, one per window object; dimmer = curtain | own evaluation, follows the sky | verified |
| Sun, moon, sky | No: ExteriorLightData, the sky object (`*(lightMgr+0xD0) - 0x210`), sun direction `sky+0x6B0` | | sky object verified, sun direction unverified |
| Candles, fireplaces, bonfires, fires | Yes: the script drives them with the same dimmer and colour calls (`Candle.cs` 72-122, `Fireplace.cs` 1760-1856, `Fire.cs` 426-427, `BonFire.cs`) | flicker, if any, comes from the dimmer or a VFX, not from the light | calls verified; flicker inferred |
| Daylight lamps, mood lamps, club, strobe and stage lights | Yes, same calls (`DaylightLight.cs` 66, `MoodLamp.cs`, `StrobeLight.cs`, `StageLightObject.cs`) | strobe and stage lights change the dimmer or colour many times a second | verified calls; rate inferred |
| TV screens | No light calls in `TV.cs`; the glow is the screen material or a VFX | | absence verified by search; the rest inferred |
| Glowing Sims (vampires, fairies, ghosts), magic effects | No BaseLight found; Sims are lit by rigs (`World.ObjectSetCustomLightRig` sets a rig, it does not emit) | VFX (Swarm) | inferred |
| Holiday house lights | `World.LotSetHolidayHouseLights`, a lot-level setting | separate path, not traced | inferred |

## 7. Where Apex already tracks this

| Need | Apex code |
|---|---|
| Read one light (all fields of section 2, per type) | `RtOutdoorLamps::ReadLamp`, `ReadTubeShare` (`features/rt_outdoor_lamps.cpp`) |
| The light's law as the GPU evaluates it | `RtOutdoorLamps::WorldLaw` -> `GpuLampLaw::Lamp` (`features/gpu_lamp_law.h`) |
| List of outdoor and lot lamps, lamp selection per draw | `LotLightBridge::UpdateLampList`, `ForEachOutdoorLotLamp`, `SelectLamps`, `SelectPixelLamps` (`features/lot_light_bridge.cpp`) |
| Every light, debug dump | `LightDiag` (`features/light_diag.cpp`, uses `0x006ACF70`) |
| Edits (switch, value, move, add, remove), drag state | `LampMarkFilter` (`EditLastTick`, `EditSerial`, `SwitchActive`, `HandledLately`, `NoteEdit`), `DragLite` |
| Lamps under a moved object, occluder shapes | `PhysLampPick` (`features/phys_lamp_pick.h`), `LampMarkFilter::ShapeMoved` |
| Colour changes and the stock tint | `ObjectLightBridge` (`LampColourSet`, `LampColourSetScript`) |
| The rig a draw uses (Sims and objects) | `RigTracker` (`features/rig_tracker.cpp`) |

## 8. Open questions

1. Which script calls send `0x1361A133`, `0x5361C4C5`, `0xF33E398B`, `0xE128229B` and `0x09714EE4`. The component methods
   are known; their callers are not.
2. What flag 0x08 (the linked light's dimmer, `0x006C7C3F`) links to: light groups, or one lamp object with several lights.
3. Whether `+0x20` (fade) is an animated switch or a global manmade-light scale (`0x006BC430` reads `0x006AC820`).
4. The exact free path of a light object in `0x006B1610`, and whether the light tree thread can still hold a pointer to it
   (it matters for any reader on a third thread).
5. Strobe, club and stage lights: how many dimmer messages a second they send, which sets the GI's update budget.
6. Light records: confirm the LITE resource as the source of `0x006B0EF0`'s records and map its fields to section 2.
7. EA app 1.69: none of the new addresses in sections 3 and 4 have been found there yet.
