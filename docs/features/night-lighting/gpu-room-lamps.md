# GPU room lamps

Indoor lamps drawn on the GPU, part of Lamp lighting: Instant (GPU): each solve of a room of the active lot other than
room 0 is kept, split into the light the GPU will not draw and, per texel and lot lamp, the share of the lamp's law that
Apex's tests and the game's tests let through, with the room's tail (tone, ambient, dither, pack, wall blur, tile-edge
copies). The kept rooms are drawn on the GPU over Real-time outdoor lamps' copy of each story map with the lot lamps as they
are now, so a lamp's colour, intensity or switch shows in the frame of the edit, and such an edit solves no room until a settle 2 s
after it when every room listing the lamp is drawn so. The four directional maps objects and Sims read follow such an edit at the end of the same room update: the edited lamp's part of each directional sample is computed again with the game's own step and the room's samples are packed again into the game's maps. A lamp moved within its room is relit the same way once a worker thread has redone its parts there from a copy of the game's tests (only for lamps whose light reaches those rooms by the plain path, and room 0 through openings of the lamp's own story). With [the room lamp
probe](gpu-room-lamps-probe.md) on too, each kept solve is recomposed on the CPU and on the GPU and compared with the game's.

## Status

| | |
|---|---|
| Availability | Released in 3.0.0, on Steam 1.67.2 and EA app 1.69.47 (once its addresses are found) with Lamp lighting: Instant (GPU); other game builds keep the game's room solves (see Limitations) |
| Default | On, physical model, sharper floors; with Classic, on another game build, or when Quick outdoor lamp edits cannot run on the game build, the game's room solves |
| Menu | No switch of its own (System > Performance > Lamp lighting); its comparison: Developer > Compare lighting paths > Keep indoor lamps for GPU |
| Configuration | `gpuRoomLamps` under `[patches.NightTerrainRelight]` in `ApexRadiance.toml`, read only in developer mode. An older config with `gpuRoomLampsCapture = false` takes the new model and floor defaults once; with it on, its saved values stay |
| Source | [`features/outdoor_delta.cpp`](../../../features/outdoor_delta.cpp) (`KeepEnd`, `KeptSelfCheck`, `KeepFinals`, `KeptFinalCheck`, `BuildKeptMap`, `RoomFinalized`), [`features/rt_lamp_model.h`](../../../features/rt_lamp_model.h) (`KeptAdd`, `KeptTexel`), [`features/gpu_room_lamps.cpp`](../../../features/gpu_room_lamps.cpp), [`features/rt_lamp_gpu.cpp`](../../../features/rt_lamp_gpu.cpp) (`RelightKept`), [`shaders/rt_lamps_hlsl.h`](../../../shaders/rt_lamps_hlsl.h) (`AccKeptPS`, `ComposeKeptPS`, `CopyAtPS`, `BlockKeptPS`), [`features/room_map_dither.cpp`](../../../features/room_map_dither.cpp) (the pack input), [`features/level_light_share.cpp`](../../../features/level_light_share.cpp) (`LightEvalHook`, `FinalizeHook`, `LotRooms`), [`features/lamp_mark_filter.cpp`](../../../features/lamp_mark_filter.cpp) (`MarkDecideImpl`), [`features/room_lamp_move.h`](../../../features/room_lamp_move.h) and [`features/room_light_tests.h`](../../../features/room_light_tests.h) (lamp moves), [`features/outdoor_delta_model.h`](../../../features/outdoor_delta_model.h) (`BasisPass`, `BasisPartNow`, `BasisPackInput`: directional maps) |

## The problem

Indoor lamp edits wait for the CPU room solves. To draw a lot lamp's light in a room on the GPU instantly, the GPU needs, per
texel, what the solve's tests did to that lamp: Apex's own tests (the floor under the story, light through openings, light
between stories, the soft light cut) and the game's wall and furniture tests. Those depend on the room's geometry and the
lamp's position, not on its colour or intensity.

## How Apex Radiance solves it

1. Turned on, every room of the active lot is sent to its solve once.
2. A solve of a room other than room 0 of the active lot, on the light tree thread, is captured with the room solve hooks the
   probe uses: per texel the batch sample (position, normal), the map and place, the solve's sum before the room's tone and
   each light's part of it; per evaluation the colour Apex's tests leave before the soft light cut, the threshold scale an
   outdoor lamp through an opening takes, and whether Apex's threshold early-out returned.
3. At the solve's end, for each lamp the GPU draws (a lot lamp of classes 3, 4, 5 or 11, alive, a shade its law describes:
   `RtOutdoorLamps::WorldLaw`), per texel where its evaluation was lit before the soft light cut down to a quarter of the
   threshold:
   - A = the colour before the soft light cut over the lamp's law there (r + g + b);
   - B = the lamp's part over its colour after the soft light cut: the game's 2D wall and 3D test share (0 when dropped, or
     never tested because the evaluation was dark);
   - the lamp's entries in the list, the threshold scale and the early-out.
   A, B and the scale are kept as half floats, per lamp by texel. The texel's base is its sum less the parts of those lamps.
4. The kept rooms stay above 2 GB (`ApexMem::HighAllocator`) within 192 MB for the active lot. Past it a new solve is not
   kept (logged); no kept room is dropped for another. A solve of another lot's room drops the kept rooms of the lot before.
   Turning the option off drops them all.
5. With the room lamp probe on, each kept room is recomposed at once with the lamps' laws as they are then:
   base + the sum of mult x fade(law x A) x B (`RtLampModel::KeptAdd`), and compared with the solve's sum.
6. The room's tail is kept too (stage 2a), so that the final texel can be rebuilt: per texel the solve's pack input before
   the clamp (Smooth light maps' pack thunk), the room's normalisation as the texel was toned, whether the dither and the
   roofed alpha floor applied, and what the texel's place holds when the solve ends; the class-2 wall blocks before the blur
   with the rows lit beyond their edges; at class 2 the floor and ceiling maps' tile-edge copies. With the probe on, every
   kept texel is then rebuilt from its recomposed sum (the tone, the ramp and ambient read back from the pack input, the
   dither, the game's pack, the wall blur, the copies) and compared with the texel the game left in the map.
7. The kept rooms are drawn on the GPU (stage 2b). Real-time outdoor lamps keeps a copy of each story map of the active lot
   (the game's map with room 0's texels relit) and binds it wherever the game binds the map. Each kept room whose story and
   class that copy shows is drawn over it in the same relight: per lamp, its parts are added into a float sum at their
   texels with the lamp's law as it is now; each texel is then composed as `KeptTexel` composes it and packed; class-2 wall
   blocks are blurred the game's way in a layout of their own and written back; each tile-edge copy is its source texel
   composed again at the copy's place. The copy is relit whenever a kept room's lamp changes (its law is part of the map's
   signature). A room's own map can be smaller than the working map the solve wrote (the commit copies its top-left corner,
   as for room 0): the texels are drawn at their place and those past it fall outside the viewport; a copy whose source lies
   past the corner still lands inside (the game copies before its commit). A class-2 wall atlas must have the exact size.
8. When the game writes a room's texels again (the finalize of any solve of it, on any thread), its kept solve is dropped
   until its next kept solve, and the copy shows the game's texels meanwhile. A room whose class changed between its solve's
   start and end is not kept (it wrote the other class's maps).
9. Lamp edits (LampMarkFilter's lamp entry update, light tree thread): a colour, intensity or switch edit of a lamp that every
   room of the lot listing it draws on the GPU marks no room: no solve, the copies are relit in the frame. A room other than
   room 0 counts when its kept solve of the class it shows has every texel drawn (no class-2 wall left to the game's blur),
   does not hold the lamp in its base, and was drawn on every map it wrote; a room 0 listing it, when Real-time outdoor lamps
   draws the lamp there. A switch on is taken only while the rooms listing the lamp keep the solves they had when its switch
   off was taken (a room gathered or solved again while it was off would lack it). Any other edit takes the game's path.
   The settle: 2 s after a lamp's last edit taken, the rooms (other than room 0) that listed it are sent to their solve once,
   with no urgency (as a GPU edit's tail sends), so their normalisation (+0x160), ambient (+0x110, +0x650) and light list
   follow the lamp as a solve sets them; each is kept again as usual, and the GPU draws their kept solve until then (the
   light changes only where the normalisation or the ambient did). A drag's or a slider's edits make one settle per lamp.
10. Room 0's entries of indoor lamps (its list takes them through openings): a room 0 solve of the active lot also captures,
   as a kept room's, each lamp of its list Real-time outdoor lamps does not draw (`BuildKept0`). Such a lamp leaves the base
   Real-time outdoor lamps relights from, and its parts are added back into that map's sum on the GPU, with its law now,
   before the compose (`AddKeptParts`). Two more kinds of part are kept so that every lamp of the list is drawn exactly:
   - its parts of the floor mask's two copies (an outdoor floor of a story >= 1): the walls-tested and the no-walls
     luminance, each over the luminance of its law there with N.l ignored (as the copies evaluate it: `KeptWrapLum`); the
     GPU adds that law's luminance now times the walls-tested share into the sum's alpha ("vis") and times the no-walls
     share into a second sum ("all"), and the compose takes the mask's alpha from both;
   - an exit lamp (a roofed room's lamp story 0's room 0 takes through an opening): its light is also the alpha of the
     story-0 ground texels, min(the luminance of the exit lamps' light, 254/255), 0 under a floor tile. The capture notes
     which ground texels take it (tapped, and not under a floor tile); the GPU adds the kept exit lamps' light's luminance
     into the second sum, and the compose gives those texels `ExitAlpha` of it plus the exit lamps' light left in the base.
   A lamp with a floor mask part where its law is dark (no share to keep) stays in the base. A delta that changes a kept lamp
   puts it back in the base. Room 0 then counts as drawing the lamp for an edit (`CoversLamp`); the other lamps of room 0
   still take the solves.
11. Maps without room-0 texels (an objects map, some ceilings): Real-time outdoor lamps' slot for that map gets a copy of its
   own while kept rooms of it are drawn (the game's map, those rooms over it), bound by the same swap.
12. TubeLights (type 6) have a GPU law of their own (`GpuLampLaw::Tube`, read 02/11 with dumpbin): their evaluation
   `0x006BFA70` is E0 (+0xE0) x min(Cmax, k2 x range x intensity / (k1 d^2)) x max(0, N.l) (`0x006BD100`; the facing after
   the cap, unlike the bare bulb's), d and l to the nearest point of the segment from its position A (+0x120) to its end B
   (+0x170): A when (B - A) . (p - A) < 0, B past len^2 (len +0x1A0, its length), else the projection. The law keeps
   axis = B - A, sc1 = len^2 and sc2 = kTube (the shaders' `KeptLaw` takes `TubeLaw` on that mark); it has no MountedNdl
   floor (LightEvalHook raises a wall texel near A after the evaluation, so the law is the game's evaluation and
   `GameOverLaw` is 1), and Realistic lamp reach measures from A as `ReachShare`. A tube is then kept, edited (colour,
   intensity, range, switch) and moved like a lamp of types 3, 4, 5 and 11 (a turn about A is a move: `GpuLampLaw::Moved`
   compares its end too); Real-time outdoor lamps' own draw still takes none (`RtLampModel::Drawn`). Checked: the probe's
   hook colours of every TubeLight in dumps `RoomLampProbe-20261102-135710` and `-145922` (78,048 lit texels; every tube a
   1 m segment straight up, W 3.375) are all within 2/255 of the law with MountFactor (largest 0.0026), and a replica of the
   two functions (`tools/outdoor_delta_test` TubeChecks, same SSE approximations) gives the law at 200,000 points.
   A tube whose segment cannot be read (non-finite, or shorter than 1 mm) keeps the earlier fallback: its colour as its law
   (`ColourLaw`; exact for a switch or a colour edit, its share's inputs kept as a signature whose change takes the solves).
   The area lights (RectangleAreaLight 9 and DiscAreaLight 10) have one too (`GpuLampLaw::Area`, read 02/11 with dumpbin:
   `0x006BFBA0`, `0x006BFDC0` with `0x006BD7C0`, both over `0x006BFAF0`): E0 x min(Cmax, k2 x range x intensity / (k1 |D|^2)) x
   sat(D.z / |D|), no N.l, where L = (U.v, V.v, N.v) for v = A - p (U +0x170, V +0x190, N +0x180) and D = L minus its first two
   lanes held in the rectangle (+-(+0x1A0, +0x1A4)) or the disc (scaled to radius +0x1A4 past +0x1A0 = radius^2), third lane 0.
   The law keeps axis = U, spill = V, (sc1, off1, off2) = N, (r0sq, invR2) = the two sizes and sc2 = kRect (-2) or kDisc (-3)
   (`KeptLaw` takes `AreaLaw` on that mark; no reach, `GameOverLaw` 1). At |D| = 0 the game's evaluation is NaN (its texel
   too; the hook keeps no part there): the law gives 0. Kept, edited and moved like a tube; a turn or a resize is a move
   (`GpuLampLaw::Moved` compares U, V, N and the sizes). An area light with a non-finite axis or a size outside 0 to 1e6 has
   no law and stays in the base, as before. Checked only offline: a replica of the three functions (`tools/outdoor_delta_test`
   AreaChecks, the same SSE approximations, turned, scaled and skewed frames, points 1 mm to 5 cm off the area) gives the law
   at 200,000 points of each kind (largest relative difference 1.6e-5); the test lot has no area light, so the probe (which
   now dumps their laws) checks them on the next dump of a lot with one.
13. A lamp moved (its law's position differs from the one its parts were kept with) is drawn in the rooms holding it with the
   law its parts were kept with (the room's last complete parts as they were, never its law now over them; room 0 alike), until
   a copy of the room with its parts at its new place is kept or the room is solved.
   A move within its room (stage 3) is taken without solves when every room listing the lamp is a kept room (no room 0)
   drawn on the GPU where a copy of the tests can redo its parts (`LevelLightShare::MovePath`): a lamp with a GPU law (a tube's
   included), not an outdoor lamp, not under the room's story floor (`LampOverOwnFloor`: then `OwnFloorBlocksAt` cuts no
   point), not one of the room record's inner, opening or cross lamps, in a roofed room or one without a record, and the room's
   geometry key (`RoomGeoKey`, see 14) is that of its kept solve. On the light tree thread the edit copies what the
   game's tests read of each such room (its walls, default wall list and wall mode, its 3D records and their blockers; a
   blocker of another kind than the soft rectangle or disc refuses the move) and queues one job per room for the move worker
   (one thread; a later move of the same lamp replaces a job not started). The worker gives every kept texel where the lamp's
   law at its new place is lit a part (`room_lamp_move.h`): A = MountedNdl's factor as LightEvalHook applies it, B = the
   game's 2D wall test from the lamp with the batch's wall list (`wallCull` from the batch centroid when the solve built the
   lists) when the batch tests walls, times its 3D test when it tests objects, dropped under 0.01 (`room_light_tests.h`,
   the code the offline replays run), the threshold scale 1, the entries the room's list holds now, the early-out bit with a
   record and the soft light cut bit as the room's parts were kept. It keeps a copy of the room with those parts and the
   lamp's law there under a new version (dropped when the room was solved or kept again meanwhile), which the GPU draws as
   any kept room; that copy counts as drawn for the next edit while the GPU still draws the one it replaced. During a drag
   each step is taken this way (the next step's job replaces the last one's): the light follows in the worker's steps, each
   drawn from complete parts of one place. The settle solves the rooms 2 s after the drag's last move. Anything else (a room
   0 listing it, a move into another room, a refusal above) takes the game's path, logged.
   Room 0 (stage 3b): a room 0 listing the lamp no longer refuses the move when its kept record holds the lamp (kept0) with no
   floor mask part, the lamp is one of its record's inner lamps of its own story (not innerUp, `LevelLightShare::Room0MovePath`),
   steady_indoor_light is on and the lot's geometry key is its solve's. A lamp of a double-height room of the story below
   (level light share, *Double-height rooms*: in room 0's `cross`, not an inner lamp) is refused there ("room 0 takes it from
   another story ..."), so a drag of such a lamp takes the game's path; its value and switch edits are drawn from its kept0
   parts like any lamp Real-time outdoor lamps does not draw (its floor and wall tests do not depend on its colour). The edit copies the record's samples and batches, room
   0's 3D tests, the story's grids InnerPass reads, the map's half texel, the norm scale (InnerNormScale, read now) and the
   reach inputs; the worker gives every texel where the law at the new place is lit and past the threshold early-out a part
   (`room_lamp_move.h Part0At`): A = MountFactor x the reach share (wall texels) x InnerPass (`InnerPassAt`, the production
   grid walk) x the norm scale, B = the 3D test alone (the game's wall test is skipped for such a lamp), kept as a capture keeps
   them (down to kKeepFloor of the threshold), and puts them in the record's kept lamp with the new law under a new version
   (dropped when the record changed meanwhile); Real-time outdoor lamps exports it again (after a drag: its uploads wait for
   the lamp to be still). An exit lamp's ground alpha follows from its parts (the exit texels are marked by the solve for every
   lamp). Every A is over the game's evaluation, and is taken over the law x `GameOverLaw` (the law raises a wall texel's N.l
   itself; until stage 3b a moved part on such a texel took the raise twice).
14. Switch on after the settle: a room kept again while a lamp is switched off by a taken edit (the settle's solve, or any
   solve) holds none of the lamp's parts. Its new kept solve takes them from its earlier kept solve (the one the switch off
   was taken with, `CarryOff`) when the texels are the same places with the same samples, the room's geometry key is the same
   as at that solve's start, and the lamp has not moved (a tube: its end too; a tube without a segment: what its share reads is the same). The key (`RoomGeoKey`) is
   the lot's geometry key with what the game's tests read of the room: each wall's three segments, normal, length, junction
   counts and test bits (+0x40 to +0xEC), the default wall list and wall mode, each 3D record's centre and squared radii
   (+0x00, +0x10, +0x20), and each blocker's vtable and the fields its transmission reads (a rectangle: +0x10, +0x70, +0x80,
   +0x90, +0xA0, +0xB0, +0xD0, +0xE0, +0xF0; a disc: +0x10, +0x20, +0x30, +0x40, +0x70, +0x80, +0x90; another kind: its first
   0x100 bytes). The records' raw bytes, keyed until 2026-11-02, changed at every solve (their blocker vectors are made again
   by the gather; the lot's key did not change), so no part was ever carried. The switch on is then taken and drawn from them in the frame; anything else drops them, and the switch on
   takes the solves.
15. Directional maps (stage 4): the four maps objects and Sims read (`FUN_006a09f0`, one sample a tile) are written only by
   the room solves. A kept solve also keeps, per directional sample, its texel and position and each light's part of its
   accumulator (the accumulator before and after the light in `BasisLightHook`), with its share of the game's step
   `FUN_0069f280` called again there (the part over the step: 1 where Apex's tests leave the step as it is); lights kept in
   the room's base are summed. At the solve's end the parts are packed again and compared with the texels the game wrote:
   only a room where every texel is within 1/255 takes edits. An edit taken (switch, colour, intensity, a move) queues the
   lamp; at the end of the room update, while no solve holds the working maps, each of its rooms gets the lamp's part again
   (switched off: 0; otherwise its share times the game's step called now, which reads the rig colour and place the lamp has
   now; a move's share is 1, the plain path's) and its samples are packed and written into the game's own textures of
   the set [room+8] (only texels that still hold what the capture or Apex's last write left). A switched-off lamp's part is
   carried into the room's next kept solve with its other parts (`CarryOff`), so its switch on is written too.
16. Physical model, stage 1 (`roomLampModel` physical, 02/11): a kept room keeps its base (the sum less the measured parts of
   every lamp with a GPU law, as above; a light whose law is its colour alone stays in the base), its tail, samples, batches,
   directional samples and the room's 3D test records as its solve started (`ReadRoomTests`, read with `RoomGeoKey`:
   `KeptRoom::tests`), and lists its lamps with their laws but builds no part. Room 0 builds no kept lamps;
   `ExportRoom0(physical)` takes every such lot lamp out of its base (its sum, floor mask vis and all, exit light; `phys`), but
   no caller asks it yet. Until the physical draw lands, a physical kept room is not drawn (`KeptRooms` leaves it out: its maps
   keep the game's texels, and Real-time outdoor lamps' room 0 is drawn as with the capture off) and every lamp edit listing it
   takes the room solves (`physical: drawing not implemented yet`). A solve that started under the other model is not kept.
17. Physical model, stage 2 (02/11, nothing drawn yet): the occluders come from `RtLampProxy::Build(lot, mesh, true)`. Its
   `OpenTops` drops the roofed quadrants (ceiling at floor + 3 m) of a room open to the story above (`OpenAbove::Classify`, the
   rule room 0 takes the lamps below by) where no floor is placed over them; `ExportLotProxy` now carries the quadrants' room
   ids for it. Lot C49C (F8 maps): story 2's rooms 5, 6 and 7 carried such a ceiling under story 3 (592 quadrants), which cut
   their lamps at the story line. Real-time outdoor lamps build without `ceilingsOpen` and are unchanged. The furniture tests
   go to the GPU as `RtLampGpu::BlockerTable` (`features/rt_lamp_blockers.h`): float4 rows for an A32B32G32R32F texture
   (width up to 4096, point sampled), a header per room, 2 rows per record, 7 per rectangle or disc blocker. Its decoder
   `Occlude` walks the rows as the shader will and is `RoomLightTests::occlude` bit for bit (70,912 of 70,912 samples of the
   dump 20261102-135710, `tools/room_lamp_3d_replay`).
18. Physical model, stage 3a (02/11, the GPU side only; nothing in the game calls it yet): `Engine::RelightKeptPhys` is
   `RelightKept` with the sum drawn from the lamps instead of the parts. Each kept texel is a phys point (lot position and
   normal for the law and the cube shadow, world position for the 3D test, bits + 16 x room slot, bit 8 = its batch ran the 3D
   test). Per batch of 4 lamps, `T3dPS` runs the game's 3D test (`Occlude3D`, the port of `quadT`/`discT`/`occlude` over the
   blocker table, in world space as the records; records and blockers walked as one sequence in two nested loops of 255, past
   ps_3_0's loop limit), then `AccPhysPS` adds mult x fade(law x shadow share x T3D, dropped under 0.01; T3D lifted by the
   furniture fill since 03/11, see 31) for any law kind (point,
   spot, shaded, street, tube, area). The 3D test is a pass of its own: inside the lamp loop it exceeds ps_3_0's 32 temporary
   registers. The compose, class-2 blur, tile-edge copies and restores are `RelightKept`'s own (`ComposeKept`). Furniture stays
   out of the cube map: its soft shadows are the game's 3D test, evaluated analytically. `rt_lamps_test` `PhysKeptCheck`:
   Occlude3D within 0.00003 of `RoomLightTests::occlude` at 12,952 tested point x lamp pairs (98.4% bit for bit), shadow shares
   within 2/255 of the reference at 97.4-100% per lamp, 5,472 relit texels 0/255 from the CPU (floor and blurred wall atlas).
19. Physical model, stage 3b, part 1 (02/11): physical kept rooms > 0 are drawn. `KeptRooms` lists them again; `BuildKeptMap`
   gives one phys point per texel (world position; bit 8 when its batch ran the 3D test), every lamp of the solve with a law
   (cross-room ones included, so every lamp the base lacks is drawn) with mult x norm scale (`KeepEnd`: a lamp registered in
   another room of the story scales by `InnerLight::NormScale` of the two rooms' normalisations), and the room's blocker table.
   `GpuRoomLamps` takes the points and laws to the lot's space (the story manager's +0xE0, `RtOutdoorLamps::LotXform`), keeps
   16 shadow tiles (least recently used, kept while the lamp stays put and the occluders are the same) rendered from the
   open-top occluders (`RtLampProxy::Build(lot, mesh, true)`, uploaded as the engine's tile proxy, `SetProxy(..., true)`), and
   draws with `RelightKeptPhys` (no reach: indoor maps). Any failure (more than 16 lamps, no occluders, a device error) logs
   `not drawn (why)` and leaves the game's map. The three physical shaders are optional: missing from the shader cache with no
   compiler, the engine still runs the outdoor lamps and physical rooms keep the game's maps. Lamp edits of physical rooms
   still take the solves; room 0 is not physical yet. `rt_lamps_test` 58 checks (the tiles from the tile proxy, the engine
   without the physical shaders).
20. Physical model, stage 3b, part 2 (02/11): a lamp's norm scale comes from the room record's `innerSrc` (InnerShadow's
   own source room, any story; `LevelLightShare::InnerNormScaleOf`), else the same-story lookup above. The realistic reach
   share applies where the solve applies it (`LevelLightShare::ReachAppliesIn`: a roofless room's lamps except a double-height
   room's below, or a room-0 lamp through a roofed room's openings), on wall texels with the reach on. A map with more lamps
   than the atlas holds is drawn in passes of 16 over one sum (`RelightKeptPhys(..., first, compose)`; at most 64 lamps). The
   kept atlas holds a tile (one lamp's 6 faces) per distinct lot lamp drawn while it exists, grown 4 tiles at a time, up to 120
   (`PhysTiles::kTiles`): 3.1 MB a tile (6 x 256 x 256, G16R16F + D24X8), 75 MB at 24, 377 MB at 120. Tiles are packed in
   columns of up to 32 (8192 texels; a 15360-texel column moved one sum of 2720 by a bit, 8192 is bit for bit), then side by side
   (120: 4 columns of 30, 6144 x 7680), within the device's texture size up to 8192 x 8192 (160 tiles at face 256; a 4096 device:
   32). The shader takes a lamp's tile as its row and its column's first face (`misc.x`, `misc.z` of c80..c95); no other register
   or sampler. Past the tiles, a lamp takes, in order, its own old tile, an empty one, else the one whose lamp the last relight round
   drew again latest (64 slices a round per lamp, `PhysTiles::Atlas::Next`): the excess cycles instead of every lamp. Bench (132 maps
   all drawn again at each step of a drag): 120 lamps 1 cube a step (534 with the 32-tile atlas), 130 lamps 17.3 cubes a step (725
   with the 32-tile atlas, 61 least recently used over 120). Each relight round logs per story, when it changes, `Story S physical: every map drawn (...)` or `not
   covered (K of N maps not drawn: why)`. Room 0 is still drawn from its solve. `rt_lamps_test` 59 checks (passes of 3
   lamps over a 3-row atlas give the one pass's map texel for texel).
21. Physical model, stage 3b, part 3 (02/11): room 0 of the active lot is physical too. A room 0 solve kept under the physical
   model also keeps, per lamp, its norm scale (as in 20) and reach flag, and room 0's 3D test records read as the solve starts
   (UNVERIFIED that they are the outdoor objects its batches test; logged in `base without N lot lamps (physical; 3D test ...)`).
   `ExportRoom0(physical)` takes every lot lamp with a law out of the base and gives its room row (mult x norm scale, reach,
   exit) and the texels whose batch ran the 3D test. Real-time outdoor lamps draws them first over phys points of their own
   (one per place of the map, DEFAULT; prepared again after a device reset) from their laws now, with the kept atlas's tiles
   shared with the kept rooms (`GpuRoomLamps::PhysPasses`) and the soft furniture shadows of the game's 3D test, then its
   outdoor batches add to the same sum. `AccPhysXPS` adds their floor mask "all" / "vis" (masked texels) and exit light (exit
   ground texels) into a second sum (`Engine::AccumulatePhys`, one A32B32G32R32F texture of the sums' size) that
   `ComposeK0PS` reads. No light is lost: a solve kept by the replica model, missing physical shaders or no occluders with open
   tops yet keep the lamps in the base (`Story S room 0: its lot lamps stay in its base (why)`), and a failed physical draw
   shows the game's map and prepares that map again with its lamps in the base. The per-story line ends `room 0 physical (N
   lamps)`. Lamp edits still take the solves. `rt_lamps_test` 64 checks (`Room0PhysCheck`: 2048 texels 0/255 from the CPU,
   floor mask and exit alpha exact, two calls over one sum as one).
22. Physical model, stage 4, part 1 (02/11): lamp edits without the room solves. Each physical map draws every lamp of the active
   lot with a GPU law whose reach (`ReachBound2` of its law at its room row, plus a tube's segment or an area light's size)
   meets the box of its texels, not only the lamps its solve listed (`phys_lamp_pick.h`; the lot's registered lights walked on the
   light tree thread, `OutdoorDelta::LotLamps`). Never one twice: a light in the solve's base (`baked`) is never drawn, a listed
   lamp (its part out of the base) keeps its row, a lamp the solve did not list (moved here, switched on, new) takes a fresh row
   (mult 1, the norm scale of its own room on the same story, the reach share in roofless rooms or for outdoor lamps). Room 0 is
   always prepared physical and does the same, leaving out the outdoor lamps its batches draw. A value, switch or move-within-
   its-room edit of a lamp of the active lot marks no room when every story 0 to 7 is covered (`TakesLampEditPhys`: each room >
   0 a kept physical solve of the class it shows drawn on every map, the lamp not in its base; each room 0 drawn physically;
   a basement listing it refuses); its object's occluder mark is dropped as before. Only a switch or an intensity change
   settles (its listing rooms and its own room, 2 s after the last edit); colour and moves solve nothing. A move is a drag for
   Real-time outdoor lamps (fast taps, one exact pass once still). A room solved off the light tree thread keeps drawing its kept
   physical copy while its geometry is the same and is sent to one solve there; a light tree thread solve keeps it until its
   own KeepEnd replaces it, so a settle shows no frame of the game's texels. A failed physical draw with an edit pending sends
   the room to its solve. Still the game's: a move into another room, a new lamp, a deleted lamp (stage 4, part 2).
   Also fixed: `AddKeptParts` set the batch's lamp constants back after its draws (ComposeK0PS's `MaskAll` read the last kept
   lamp's law: 118 of 512 masked texels within 1/255 before, 512 after). `rt_lamps_test` 69 checks, `outdoor_delta_test` 83.
23. Physical model, stage 4, part 2 (02/11): moves into another room, new and deleted lamps. `FUN_006c7ba0` marks the room a
   lamp left at 0x006C7CCA, right before its mark of the new room at 0x006C7CD6 (GameAddr `LampLeftMarkCall`, `3B 46 1C 74 09 8B
   4E 14 50 E8` +9, unique in `TS3W.exe`). `LeftThunk` holds that mark until the new room's `MarkDecide`, which drops it when
   `TakesLampEditPhys` takes the move ("move to another room": the rooms listing the lamp and its new room settle) and makes it
   otherwise. A move into room 0 stays the game's (Real-time outdoor lamps draws room 0's lamps from room 0's light list, which
   only its gather fills); room 0 now draws a lamp that left room 0 with a fresh row (it was out of its base as a batch lamp). A
   lamp first seen in a room > 0 (new, or moved in from another story) is taken the same way, with the mark of room 0 its new light entry "left" (its
   constructor sets entry+0x1C to 0 at 0x006C8061) ("new" settles its room only
   when no room lists it yet; "move to another story" settles). A lamp of a basement is refused. A deleted lamp keeps the game's
   object removal marks (0x006C75A1 in `FUN_006c7550`, from `FUN_006c7610`, for its light and occluder entries: its room's light
   list holds the removed light until that room gathers again, and its object's 3D blockers are gone); the snapshot notes it
   gone (`PhysLampPick::NoteGone`) and no kept list draws it again. The occluder entry of a moved or new lamp's object still
   marks the room under it (0x006C7939, its 3D blockers). After an outdoor lamp's move or colour edit, `RtOutdoorLamps::
   AfterRoomUpdate` no longer sends the rooms that take room 0's lamps when every story draws the edited lamps
   (`OutdoorDelta::PhysCoversLamp`); a switch or an intensity change still sends them (their bases' normalisation and ambient).
   Fresh rows: room 0 takes a roofed room's lamp of the story above at that room's norm scale, and a fresh lamp of a roofed room
   of story 0 or 1 is an exit lamp of story 0's room 0 (its alpha only on texels its solve marked as exit ground); the reach share
   of a fresh lamp stays approximate (the openings and open-top tests read the gather's lists). `outdoor_delta_test` 86 checks.
24. Physical model, stage 5 (02/11): the directional maps. After a physical edit taken (value, switch, move, new, a lamp that left
   the registry) every kept room > 0 whose directional samples the lamp's reach meets now, or whose last write drew it (before
   any: its capture listed it), gets the parts of every lit lamp of the active lot with a law whose reach meets its samples,
   picked as its maps pick them: row (mult x norm scale) x share x the game's step `FUN_0069f280` called now, share =
   `RtLampProxy::Visibility` over the open-top proxy (16 rays, Shadow softness), with no 3D test (the game's step takes no room,
   so its directional maps hold no furniture shadow), dropped under 0.01 (`PhysLampPick::BasisShare`, `OutdoorDeltaModel::BasisPartPhys`); its base holds only the lights
   without a law (`BuildBasis` puts a listed light no light map texel evaluated in the base by `LawOfLight`, as `KeepEnd`'s
   `baked`). Steps on the light tree thread, shares on a worker, the write with stage 4's rules; a room written so is written
   again after its next kept solve (the settle's too). The occluder mark of a lamp's object (0x006C7939) is dropped for a
   physical move and a new lamp taken (`OwnObjectEditTaken`): the probe's dumps of 02/11 hold no 3D record of a lamp object (of
   235 lamps listed by 77 rooms with records, 231 lie outside every record's sphere; the other 4 stand on or beside furniture).
   A new lamp is found in the story's registry (UNVERIFIED that it is there when its object's occluder updates: the
   `LampMarkFilter` status counts it). `outdoor_delta_test` 90 checks. Room 0 writes no directional map: the solve's state 0
   (`0x006A18B0`) sets up the directional pass (room+0x540, `0x006AA120` with 1 and 0x40 at `0x006A1BAB`) only for a room
   whose outdoor byte (+0x18) is 0 (`0x006A1AE8` returns first for an outdoor room), and Quick outdoor lamp edits never keep a
   room 0 solve that ran a directional step (none fails so), so there is no room 0 directional texel to rewrite.
25. Physical model, S8 (02/11): Rooms at Night without a solve. The kept compose scales a roofed texel's ramp x ambient (the
   ceiling pass's `+0x650`, read back by `IndoorSplit`) by the room's `+0x110` now over its solve's (`RtLampModel::AmbScale`,
   `ComposeKeptPS` `gAmb`, c108): exactly 1 while the ambient is unchanged, so the texel is the one before (byte for byte), and
   only maps holding such texels follow it. After every physical edit taken, the rooms listing the lamp and its own room run the
   game's ambient step again on the light tree thread (`AmbientDue` -> `UnlitRooms::RetakeAmbient` ->
   `LevelLightShare::ProbeRoomAmbient`: the lamps' share now plus Rooms at Night's colour), written to `+0x110` / `+0x120` and
   to the room's base record (so `MoveBase` keeps moving it with the sliders). Walls, floors and the furniture fill read
   `+0x110` live; the ceilings follow through the compose. Not run: Rooms at Night off, an atrium member or a room with a
   stairwell's share (its ambient is not the one its solve gave it), a room being solved, a list holding a deleted lamp, an empty list (a lamp switched on in a room
   whose solve had none on: the colour alone until the settle's solve gathers it). The settle stays for the normalisation (`+0x160`) and what the base holds (a stairwell's bounce and fill). Log `physical <kind>:
   ambient taken again in N of M rooms in X ms, no room solved (K with another normalisation from their solve); story S room
   R: why` (once a second during a drag). `outdoor_delta_test` 93 checks, `rt_lamps_test` 73.
26. Physical model, review fixes (02/11): a room whose map falls back to the game's after a lamp edit taken without its solve
   is sent to one (`OutdoorDelta::SolveTouched`, `PhysLampPick::TouchedRooms`): the rooms listing the lamp, its own room, every
   room 0 of stories 0 to 7 and every map that drew it are noted at the edit and forgotten at their next solve's end; a failed
   physical draw (rooms > 0 or room 0, including more than 64 lamps), a kept solve dropped, capture or Real-time outdoor lamps
   off, the replica model and another active lot send them (`physical: N rooms shown from the game's maps again ...`). A step
   under 10 cm is a move only while a drag goes on (`PhysLampPick::EditKind`): an animated lamp's wobble takes the game's path.
   An occluder mark is dropped only for the lamp's own object (its point within 0.5 m of the head and moved with it,
   `OwnObjectStep`; UNVERIFIED that every lamp object lies that close) or a new lamp absent from the registry snapshot
   (`NewLampEvidence`). The blocker table is built once per kept room or room-0 record (in their bytes) and its texture is
   4096 wide, only taller, uploaded only when another table is bound. An object removal (`0x006C7610`) reads the lamp snapshot
   again after the next room update, and the directional maps' job reads it again before calling the game's step. The ambient
   probe flag is per thread; `RetakeAmbient` writes nothing when the room's state or ambient changed during the probe. An
   empty open-top proxy leaves the tiles without occluders, and a failed upload is tried again 5 s later.
   `outdoor_delta_test` 97 checks, `rt_lamps_test` 75.
27. Physical model, first game test fixes (02/11): a room whose light list is empty (no buffer) counts as wrapped, so it is
   kept and drawn (the log: story 0 room 13, story 1 rooms 4 and 8, story 2 rooms 9 and 11, story 3 room 10 were never kept, and
   every lamp edit took the room solves: `story 0 room 13: no kept physical solve of the class it shows`, 616 times, 0 edits
   taken). A lamp missing from the registry stays in the lamp snapshot for 1 s (`PhysLampPick::CarryMissing`): the registry
   drops a dragged lamp for a few frames and every lamp of the lot while build mode edits it (709 `left the lot's registry`
   lines in 3 minutes, 175 at once within 50 ms, none deleted), which took them off every map and rewrote the directional
   maps without them. `ExportLotProxy` takes each tile's floor from the tile instead of its grid ceiling less 3 m: where the
   grid ceiling is the story above's floor (story 3's 4.74 m tiles over story 2), story 2's floor came out at 1.74 m and
   `CeilingsUnderAbove` put a ceiling 0.75 m over story 1's rooms 2 and 8 (35 tiles; the probe: 24-27% of room 8's texels
   the game lit shaded by the occluders only), the square patches and the diagonal checkerboard on their floors. This also
   applies to Real-time outdoor lamps' occluders. `outdoor_delta_test` 98 checks, `rt_lamps_test` 75.
28. Physical model, second game test fixes (02/11):
   - Empty light lists: `ReadList` (outdoor_delta.cpp) read a room with no list buffer as unreadable too, so such a room was
     never kept and every edit of its lot was refused (0 of 423 taken). The length rule is `PhysLampPick::ListLen`, with its
     own checks.
   - Open tops: the open-tops build (`RtLampProxy::OpenTops`) opened nothing in game (`0 quadrants opened`). The story
     above's real walls never reach the open-above rule's three quarters of a double-height room's outline under a sloped
     roof, and a floor hole was never opened. A roofed quadrant under a removed floor of the story above
     (`OpenUnderRemoved`, `Story::removed` from `ReadPlacedBits`) now loses its ceiling, and so does a room open by the
     floor rule with a wall of its own on the story above, one with no floor placed beyond it (`OpenAbove::Classify`'s
     `ownWalls`, off for the room-0 light rule). The occluders line counts the opened quadrants.
   - Relit copies: Real-time outdoor lamps binds a relit DEFAULT copy in place of the game's map, an objects map too, and
     three lookups keyed by the bound map missed it: Smooth indoor light's directional maps (`RoomMapPadding::NoteDraw`,
     `BasisFor`: modular furniture such as curtains fell back to the per-object rig and showed light steps), the floor
     map's wall mask (`FloorWallMaskTexture`) and the arch ambient memo (`ArchRowsForImpl`). Each now maps a copy back
     with `RtOutdoorLamps::GameMapOf`.
   - Unkept rooms: a lot made active solves its rooms at class 0 and shows them at class 2, so a room no lamp lists stayed
     unkept until an edit's fallback reached it. `KeepEnd` notes such a room, or one whose solve saw the model change, and
     `OutdoorDelta::SolveUnkept` sends it to a solve again from the render thread, at most 3 times a minute per room.
   - Loose samples: a sample outside the solve's batch vector failed the capture. In a physical capture of a room above 0
     it is a batch of its own when it comes between two batches; inside a batch it still fails, and room 0 and the replica
     model stay strict.
   - Registry snapshot: a lot no longer lit at the high quality unregisters its lamps, and every lamp was taken as gone.
     `RefreshLotLamps` keeps the last snapshot until the lot is active again.
   - Replay lists: the outdoor delta's replay points a room's light list at its one-lamp copy during a batch solve on the
     light tree thread. Real-time outdoor lamps' `ReadList` reads such a list as unreadable (`LevelLightShare::IsReplayList`),
     so the kept list stands in for that frame.
   - Cost: each lot lamp is read once a round for every physical map, a physical map keeps its lamp list while the lot's
     lamps stay the same, and every edit taken refreshes the per-pixel object lamps in its frame.
   `outdoor_delta_test` 101 checks, `rt_lamps_test` 75.
29. Lighting scene recordings (03/11): a Recording capture also records the physical rooms' solves with the game's part of
   each lamp, sending the lot's kept physical rooms and each taken edit's rooms to a solve (`OutdoorDelta::SceneBegin`,
   `SceneTick`, `SceneWriteRoom`; format `scene_record.h`), and logs a `[GpuOracle]` line per solve.
   `tools/scene_replay` replays every recorded solve through the current GPU term against the game's parts, and
   `tools/apex_health.py` sums up a log; the game warns (`[Health]`) when over 10% of the physical edits in 10 s are
   refused. See [dev-tools-scene-replay.md](../../validation/dev-tools-scene-replay.md).
30. Sharper floors (03/11, on by default): a class-2 floor map holds 4 texels a metre, so the light of a lamp
   through a door or a window reached the floor beyond in 25 cm steps (the shadow tile, about 2 cm a texel at 3 m, is not the
   limit). With *Sharper floor light* on, `BuildKeptMap` adds each physical floor texel's 4 sub-texels: phys points at their
   own places on the floor (`PhysLampPick::SubTexelPos`: a step toward a same-plane neighbour the solve wrote, never across a
   wall or a step) and compose points at their places of a map twice the size. Real-time outdoor lamps keeps two more render
   targets per floor map at twice its size: an overlay the rooms' sub-texels are drawn into when their draw changes
   (`Engine::RelightKeptPhysHi`), and the map the draws read instead of the copy (`GpuRoomLamps::ComposeHi`: the copy
   upsampled, the rooms' sub-texels over it), bound only while it holds a room's. `MapSize` gives a relit copy's game map
   size to every reader (wall masks, tiles, uv scales); only the floor's B-spline read takes the sharper map's own size.
   A restore keeps the game's texel through the upsample. Cost: 4 phys points per floor texel in a floor map's relight, and
   2 x (2w x 2h x 4) bytes of render targets per floor map (2 MB for a 512 x 256 map).
   Room 0's floor (decks and ground outside, 03/11 15:23 F8: the light through the diagonal V wall's glass doors came out in
   25 cm diamonds) takes the same: the map job gives each unmasked room-0 floor texel 4 sub-texels (`PhysLampPick::SubTexels`
   over room 0's own texels only, so never across a wall or a diagonal wall's other half tile), their phys points carry the
   footprint mean (`AccPhysXPS` reads mask.w too: the exit ground's light), and the slot runs room 0's sums at those points,
   composes them into a third render target and copies their places over the sharper map. A map with more than 16384 such
   texels, or room 0 with kept (Replica) parts, keeps the upsampled copy. Cost: 3 points of 80 B per sub-texel (15 MB of
   DEFAULT buffers at the cap) and room 0's floor sums again at 4 times its points in a floor map's relight.
   A whole room-0 tile beside a diagonal wall keeps its texels in the split tiles' atlas, while outdoor floors read the map by position, so a deck showed
   those tiles with no lamp light, in steps along the wall ([room-light-maps](../../engine/room-light-maps.md)). `PrepareJob` adds a copy of such a
   tile's texels at its place in the tile columns (`PhysLampPick::AddFloorGridTwins`, 64 points a tile at class 2 and only with no kept (Replica) parts).
   A map with no relit copy keeps the steps, and a half tile's room-0 triangle still reads the other half's texels. Offline only, unverified in game.
31. Physical model, 03/11 fixes:
   - Furniture fill: the game's point-light test leaves the floor under a lamp's own table near black (14:35 capture). A lamp
     standing on a top of its room's records (`BlockerTable::FillOf`, `RoomLightTests::StandsOn`; not a dropped one) keeps
     at least 10% of its light under furniture, more towards the shadow's edge (`kFurnitureFill`, `kFillGlow`) (`RtLampGpu::FurnitureFill` = fill + (1 - fill) x T3D, the room table's column
     w, in `AccPhysPS` and `AccPhysXPS`); every other lamp keeps the game's 3D test, so the soft furniture shadows of ceiling
     lamps are unchanged. The fill is an estimate (floor albedo about 0.3 x about half the sky open under a table), unverified
     in game. The directional maps keep the game's dark (`BasisPartPhys`). `scene_replay` and `[GpuOracle]` score the game's
     term and report the share of texels the fill lifts apart.
   - Street lamps on upper walls: room 0's export keeps the parts of lamps of no lot or another lot (unreadable lots left
     out) apart from the base (`Room0Export::Texel::worldBase`), and `PrepareJob` takes them off the base of an upper wall of
     a double-height room (bit 8, `RtLampModel::ComposeBase`); every other texel keeps its base.
32. Lighter drag rounds (05/11, offline only): a kept map's phys points are `RtLampGpu::PhysPoints`, four A32B32G32R32F textures in
   the sum's layout (64 bytes a point against 80 in a vertex buffer), and the passes draw them as dense quads (`AccPhysInPS`,
   `T3dDensePS`): a point fills a 2 x 2 pixel quad of the GPU with one live pixel. `UploadKept` refuses a layout with a hole.
   `T3dDensePS` takes one lamp a pass into its channel (the fetched position exceeds ps_3_0's temporaries inside `T3dPS`'s lamp
   loop) and walks the records only where `AccPhys` will read the result, by the same law test with the threshold 1% lower.
   Room 0 stays on points: its texels sit at sparse places of the map. `rt_lamps_test` `PhysKeptCheck` compares the quads with
   one point per texel bit for bit (furniture fills, tubes, shifted lookups and two batches). Scratch benchmark, RTX 4070
   Ti SUPER, the 05/11 capture's shape (47 maps, 436,140 points, 3 lamps): a round 10.9 to 7.5 ms, 0 sums different. The small
   maps' draws are bound by the longest texel's walk, not by their pixels: the same points in one map take 1.9 ms.
33. Table legs (physical model): the furniture records hold a table's top alone, so its feet cast nothing. `ObjectLegs`
   ([`object_legs.h`](../../../features/object_legs.h), table [`object_legs_data.inc`](../../../features/object_legs_data.inc)) bakes
   205 capsules for 36 of the game's table models from their meshes (`tools/object_legs_table`), each a soft streak in the 3D test
   (`PostT`). A lamp up to `kPostHighM` (1.5 m) over a leg's top casts the full streak. Above that the streak fades on a smoothstep,
   gone at `kPostGateM` (3.0 m), and its floor edge widens by up to `kPostHighW` (0.125 m), inside `kPostReach`: under a ceiling lamp
   a leg adds a faint soft foot to the furniture's soft shadow, never a hard streak. Custom content keeps the top alone.
34. Stairs in the shadow tiles (physical model): the occluders hold walls, roofs and slabs only. `StairSet`
   ([`stair_set.h`](../../../features/stair_set.h)) keeps, from the opaque instanced draws the mesh collector reads, the triangles
   over or beside a floor hole, between the hole's story and the one below, at most 4,096 a lot, in lot space until the lot's
   geometry changes. The set reaches the GPU tiles alone: the CPU occluders never hold it and no room goes to its solve for it.

### Objects moved in a drag

An object's shadow used to change only at the room's solve, and that solve waits for the drag to end, so the shadow stayed behind for the
whole drag plus about 0.48 s. The game's move writes each blocker's fields in place, so the addresses a solve read keep following the object.
At most every 100 ms after an object move or occluder mark, `LiveTests` in `outdoor_delta.cpp` compares those blockers with the kept records.
A room that already has a moved record is read in every frame an object moved (`RoomLightTests::LiveDue`), once a frame, in the round that
reads the lamps. At 100 ms a table dragged with its lamp stood up to 0.9 m behind the lamp, its floor shadow swung by 2.3 times that, and its
fill went off whenever the lamp left the old top. If a record changed, the room gets a new table with the record's sphere made again (`RoomLightTests::Bound`) and its legs placed again.
The room's maps are then relit, as for a step of a lamp drag. The room's next solve replaces the table. Switch: `live_object_tables`.

- A record read live has no shape in the table, so the carry of a table dragged with its lamp leaves it alone and nothing moves twice.
- The carry stays until a capture shows the move hook fires on every drag step in Live mode too. In the Build mode drag of 05/11 it fired about
  3.7 times a step (124 moves a second, 33 steps a second) and the carry moved nothing; read the `[LiveTable]` lines of a minute of Live mode.
- A table dragged with no lamp moving now relights its room and the rooms joined to it at each step, not 10 times a second. UNVERIFIED in the game.
- Room 0, the directional maps, a new object and an object dragged into another room still wait for the solve.

### Box-type seats

The LITE of a box-type chair or sofa holds its arms as zero-thickness sheets at the box's full height. Seen nearly edge-on, the sheets' band
draws a thin streak far past the shadow. `DrawnBlockers` in `rt_lamp_blockers.h` gives the GPU's copy of exactly that shape short arms and a
level cap. The records as read stay what the directional maps and the comparisons use. Switch: `chair_arm_caps`.

This trades agreement with the game for a shape closer to the mesh, so the `physical vs the game's last solve` lines read lower on these
records. `kArmHeight` is tuned and unverified in game.

### The end of a furniture band

The game drops a record the moment the line leaves its sphere. Past a chair's top the 0.35 m band still has about 0.45 of its opacity there, so
the shadow stopped at a step along the sphere's silhouette, a crisp round arc on the floor. `CutFade` in `rt_lamp_blockers.h` and the shader's
`BlockerT` fade the band over the sphere's last 0.12 m (`kCutFadeM`, read from the table's header row). Posts and true-size edges keep the game's
test. The end stays a soft taper, it does not become square. Offline on the 23-40-21 records, the step falls from 0.80 to 0.16 and a ceiling lamp's
shadow core stays: 8.3% of the floor moves by more than 0.05, mean T 0.370 to 0.393. The `physical vs the game's last solve` lines read lower in that
zone, and a scene file recorded before this keeps the step when replayed. Switch: `sphere_cut_fade`. UNVERIFIED in game.

### Day and night switch

A quick switch between day and night (the night level from one end to the other within 3 s) used to show each room's old
solve until the game solved it again: the last room's first solve landed 4.2 to 8.3 s after the switch (recording
16-17-45, lot CF2DEA20). Window lights (types 7 and 8) have no GPU law, so the day light stays in a kept room's base.

- **The cache** (`PhaseCache` in `outdoor_delta_model.h`, `g_phaseKept`): per room and end, the last physical kept solve of
  that end: begun and kept while the level was at it or moving to it (from the level leaving the other end, so the switch's own
  solves begun in its fade count; 17-11-35: every departure cached 0 rooms when a solve had to begin at the end), with no
  departure between, drawn whole (`editable`), whose ambient
  pair (+0x110, +0x120) is the one its solve's ambient step gave it with Rooms at Night's base. It is taken at the solve's
  keep and again as the level leaves the end (each room's kept solve of that stay, `PhaseDeparture`). The mode change (window
  lights, types 7 and 8, switch about 0.3 s before the level moves, 14-05-00) is dated by the first window light mark of the
  burst before the departure (`LampMarkFilter::WindowBurstStart`, marks under 1.5 s apart, within 3 s before it; else 1 s before
  the departure): a solve begun at or after it is not cached, and is dropped if it was. The level's ends come from the render
  thread (`NoteNightLevel`, from `SweepDayRooms`). A lot change and turning the capture off clear it; it shares memory with the
  kept rooms, and its other end's bytes give way to any kept solve at the 192 MB budget.
- **Sends**: as the level leaves an end, the rooms with no cached solve of the other end are sent to their solve at once (the
  class the room shows first; the room queue spreads them at its budget). Such a room whose solve began in the fade (the sky
  between the ends) is sent once more, so that end's cache takes it at the next departure. In the 16-54-11 session 5 of the 13
  rooms were never solved again after the load and showed the load's light all along.
- **The swap** (`PhaseSwitchDue`, light tree thread, four rooms per room update while no solve holds the working maps): once
  the level is half way through a quick fade (+0.3 s; else at the arrival within 3 s; a fade turned back switches back), each kept room of the active lot takes its candidate back when its kept solve began before the switch, it is not
  being solved, its story, id and class are the same, no player's lamp edit came since (`EditLastTick`, never dusk or dawn),
  `RoomGeoKey` (walls, doors, 3D blockers) is the candidate's, the texels, samples and lamps are the same, every light of the
  candidate with a law is now as kept (place, shape, colour, strength: a lamp switching with the hour back as it was holds; one
  not yet is tried again every 250 ms while the switch is open), and its directional maps can follow. Its ambient pair is
  written as `RetakeAmbient` writes (`UnlitRooms::RestoreAmbient`, refused when the room no longer holds its base record or
  Rooms at Night's colour changed); the same kept object goes back in `g_kept` (its old version: the maps' entries are made
  again and relit first as a changed signature); the directional texels holding what the room showed take the candidate's own
  values (`WritePhysCarry`), then the physical redo. Snap, no blend. The game's solves still run and replace the candidate at
  their `KeepEnd`.
- **Log**: 10 s after each quick switch, `Day/night switch to night|day: N rooms; swapped from the cache S (last +x s); waited,
  solved by the game W (last +y s; A sent by Apex at the switch's start); not solved within 10 s M; last room +z s; K swapped
  rooms solved again since; why solved: <reason> xN; why not solved: <reason> xN`, and at each departure `Day/night cache: the
  level left day|night: C of N rooms cached for it (F new), D dropped (...), R rooms with no cached ... solve sent to their
  solve; not cached: story S room R (why), ...` (not physical, no tail kept, not drawn whole, its ambient is not its solve's,
  begun heading the other end, the level left an end during its solve, solved in an earlier stay, begun after the mode
  change, no kept solve).
- **The first switch** (no cache; 17-18-28, departure +0 s): the game solves one room at a time at the lot's budget; small rooms
  ended +0.66 to +1.92 s, the tall story 1 room 2 (47,681 texels) took 1.8 s a solve and was solved three times in a row
  (+10.27 s), each time sent while it was being solved and solved again in full after; story 2 rooms 5, 6 and 12 waited
  behind it (+10.7 to +14.9 s). A stacked room whose group colour changed is no longer sent while it is being solved
  (`BeforeRoomUpdate`): after that solve its colour is applied without a solve when its maps allow (`ApplyIdleAmbient`).
  Where the 15.5 s went: the last room ended +15.53 s; 8.2 s of it were repeat solves (room 2's second, from the
  normalisation settle: the window lights moved its normalisation from 1.000 to 0.120, room 3's to 0.518 and room 8's to 0.728,
  so each was held and solved again in full; room 2's third and room 8's third, from the unlit-rooms retint of a stacked
  ambient changed after the base, which came out the same), and from +6 s the steady pacing held the played lot at about
  3 ms of solving a frame (the game's per-class solve time rose 0.7 s in 3.9 s for room 2's third solve). The game's own window
  marks (three bursts, -0.25, +0.36 and +1.03 s) threw away or redid story 2 room 7 and room 8's first solve. Not the LOD
  class (every solve was class 2) nor Build mode.
- **The switch's solve window** (`RoomAmbientPolicy::DayNightWindow`, `LevelLightShare::DayNightSwitching`, fed by
  `SweepDayRooms`): from the level leaving an end while it reaches one within 3 s, then 10 s more. Meanwhile the lot being
  played gets 25 ms of room solving a frame with the camera still (unpaced; at least 6 ms moving) and the queue's drain 8 ms
  (33 ms at most); a room whose normalisation moved since its samples read it runs the game's ambient step again inside the
  same solve (`SettleNormInPlace`: the step, its sampler copy +0x650, the stacked rooms' merge, at most 3 times), so it
  is not solved again in full; the unlit-rooms retint sends no room to a solve until the window closes. Log 10 s after each
  switch: `[RoomLightQueue] Day/night switch solves (t s): N rooms of the played lot, the last ended +x s; solves begun B,
  ended E (thrown away, solved again); the game's solving C ms; story S room R: first began, last ended, solves, ms on screen,
  ms of the game's solving`; the solve journal's `H` now names who held the room. Replay (`room_ambient_test`): the 17-18-28
  first switch with each room solved once at the window's budget ends at +2.14 s (room 2 at +1.35 s; before +15.53 s and
  +10.01 s). UNVERIFIED in game: the gain, the frame rate during the window, and that the ambient step run again gives the
  look the second full solve gave (it reads the same normalisation; rooms solved in between are not seen again).
- **Proof**: `outdoor_delta_test` `PhaseCacheChecks`: the clock, the mode change, the fill, each validity fact, and the
  16-54-11 session's six switches replayed with its rooms' solves (the first after the load sends all 13; the second swaps 8
  and sends the 5 rooms solved in the load's fade; from the third all 13 are swapped; a lamp not as kept leaves its rooms
  waiting with that reason); the swap restores the same object and the directional texels bit for bit. UNVERIFIED in game:
  the gain, the mode change's date from the window marks, and that a twilight solve landing after a swap shows no step.

### Long sessions

No failure stops the GPU's light for good. Each case below used to last until a solve, a lot change or a device reset, and now tries again on a timer:

| Failure | Next try |
|---|---|
| A map failed to draw or upload (`RetryLater`, `PhysLampPick::RetryMs`) | Made again from its kept solve after 5 s, doubling up to 320 s; a new kept solve starts the count again |
| A shader object could not be made 5 times in a row (`g_makeFails`) | Once a minute |
| Room 0 sent to its solve `kSolveAsks` (3) times with no kept solve or occluders | Once a minute (`kSolveRetryMs`) |
| The lot's kept parts overflowed the memory budget (`g_budgetOut`) | Clears when the kept bytes fall under 80% of the budget |

`kMaxWatch` in `rt_outdoor_lamps.cpp` holds 128 maps, in a two-word dirty mask, so a round of 87 maps leaves none unwatched.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Keep indoor lamps for GPU | `gpuRoomLamps` | bool | on | | The switch is developer only, with Lamp lighting: Instant; players always have it on. Keeps each room solve of the active lot and draws its lamps on the GPU; colour, intensity and switch edits of lamps drawn in every room listing them solve no room at once (the directional maps objects read are written again at the end of the room update) (their rooms are solved 2 s after the last edit); with the probe on, logs the self-checks |
| Indoor lamp model | `roomLampModel` | enum | physical | replica, physical | The choice is developer only, shown with Keep indoor lamps for GPU on. Replica: everything above. Physical: see 16 to 25; rooms > 0 and room 0 drawn from the lot lamps' laws by reach; value, switch and move edits and new lamps solve no room (a switch, an intensity change, a move into another room or a new lamp settles its rooms 2 s later; a move into room 0 and a deleted lamp take the game's solves). A change drops the kept rooms and sends the active lot's rooms to their solve |
| Sharper floor light | `roomLampSharpFloors` | bool | on | | The switch is developer only, shown with the physical model. Floor maps drawn at twice their size (30); a change drops the kept rooms and sends the active lot's rooms to their solve |
| True-size furniture shadows | `roomLampTrueShadows` | bool | off | | Developer only, shown with the physical model. Off keeps the game's soft band, the soft furniture shadows of ceiling lamps |

## Limitations

- Table legs cast nothing from a lamp 3 m or more over their tops, and only a faint foot past 1.5 m; both heights and the streak's
  strength are unverified in game.
- Other builds: the 3D blockers' vtables and the test constants (`room_light_tests.h`) are GameAddr ids (`BlockerQuadVt`, `BlockerDiscVt`, `TestZero`..`TestWallY`, [game versions](../../engine/game-versions.md#gpu-room-lamps-addresses)). The capture runs once all of them are found; otherwise `[GpuRoomLamps] Rooms keep the game's solves: Not available on <version>: missing ...` is logged and the game solves the rooms. A game blocker's vtable is renamed to the Steam value when it is read, so the copies, dumps and replays keep Steam's names. UNVERIFIED on EA 1.69.47: the room and probe field offsets the physical path reads are assumed to be Steam's.
- A is a share of the law's r + g + b: where the law's hue differs from the game's evaluation, the recomposed channels
  differ slightly.
- B is unknown where the game never tested the lamp (its colour faded or was dropped before the wall test); it is kept as 0,
  so a lamp made brighter would stay dark there. An evaluation darker than a quarter of the threshold is not kept.
- After Apex's threshold early-out the lamp's tests for light through openings and between stories never ran; a brighter
  law than the solve's is recomposed without them.
- Only solves on the light tree thread with Lamp lighting: Instant; quick passes and rooms with a light whose evaluation is
  not wrapped are not kept (as the probe).
- The tail needs Smooth light maps' pack thunk (`RoomMapDither`): without it the room is kept without its tail
  (`final self-check: not run (...)`).
- A roofed texel whose ambient weight is 0 at the solve (bright texels: k x the toned sum >= 1) gives no ramp back: it is
  filled from the map's well-split texels (`GpuLampLaw::FitSplit`, `IndoorSplitOrFill`; about 1.0 of truth in the offline model,
  not checked against game pixels). `KeptTexel` and `KeptFinalCheck`'s noSplit count still use the plain split.
- A roofless room's alpha (the floor mask, the exit light) and the rows lit beyond a class-2 wall block's edges are kept as
  the solve left them, not recomposed from the lamps.
- A kept texel a tile-edge copy of a texel no capture holds overwrote keeps the game's value; a class-2 wall texel in no
  block Apex blurs (or walls the game's own blur did) is not rebuilt. Both are counted in the log.
- The tile-edge copies are found by a dry run of the game's own copy (`0x0069EFC0`) on tagged texels before the finalize,
  after which the maps are put back and the finalize copies them as always. That the copies write only the map they are
  given is UNVERIFIED (`0x006A9DE0` / `0x006A9A50` not read in full); the dry run snapshots that map only.
- Drawn only where Real-time outdoor lamps relights the story (the active lot, the class each story shows): a kept room of a
  story or class it does not relight stays the game's, and its lamps' edits take the room solves.
- Basements (stories -4 .. -1) keep the game's solves: a basement lamp's edit, and an edit a basement room lists, take the
  room solves. A room's maps are drawn over its story's copy, which Real-time outdoor lamps builds from room 0's kept solve
  for stories 0 .. 7 only (its story-indexed state, the outdoor lamps' relight of room 0 and the terrain bake); covering a
  basement needs those copies for negative stories, the basement stories in the occluders (`ExportLotProxy`), its lamps in
  the registry snapshot (`RegisteredLights`) and the story 0 floor openings over it. No basement room with an id above 0 was
  ever solved in the game tests' logs or probe dumps (only story -1's room 0, 03/11), so none of it could be checked.
- In room 0, a lamp with a floor mask part where its law is dark stays in the base (its edits take the solves). A kept lamp's
  floor mask part is its share of the law's luminance with no threshold drop: exact for a switch or the law as kept, a lamp
  dimmed near the room's threshold keeps a part the copies would drop. A ground texel's exit light is the kept lamps' light
  plus the exit lamps' light left in the base as luminances added (the game takes the luminance of their sum: equal to
  rounding). A room-0 kept lamp that moved is drawn with its law when kept (the light room 0's solve gave) until that solve.
  Not checked in game yet (UNVERIFIED): no in-game self-check compares room 0's relit maps with the game's. Area lights (9, 10), window lights (7, 8)
  and shaded lamps whose shade the GPU law does not describe have no law here: they stay in the bases (UNVERIFIED which of
  them the user's lots hold).
- Until the settle's solve (2 s after the lamp's last edit taken, then the solve's own time) a room keeps the normalisation
  of its last solve (physical: its ambient is taken again at the edit, see 25; replica: also its ambient, so a room whose only
  lamp is switched off shows the lit room's ambient for that long). A switch
  on after the settle's solves is drawn from parts carried from the room's earlier kept solve, with the normalisation and
  ambient of the solve without the lamp until its own settle; it takes the room solves when a room was not kept again on the
  light tree thread, its layout or geometry changed, or the lamp moved. Room 0 carries nothing: a room 0 solved again while
  the lamp is off no longer draws it, and its switch on takes the solves. That an object moved in the room always changes
  the fields its 3D blockers' tests read is UNVERIFIED (only those fields, the room's walls and the lot's geometry key are
  compared); that the key now stays the same between two solves of an unchanged room is UNVERIFIED in game.
- A kept texel a tile-edge copy of another room's texel overwrote keeps the game's value: after an edit taken, such a strip
  one texel wide at a tile edge can lag until the rooms are solved (UNVERIFIED in game).
- A moved lamp's parts are redone only for the plain path and room 0's own-story inner path (see 13); its light through
  other openings and between stories (innerUp, cross lamps, floor mask parts) is not, so such moves take the solves. A room 0
  is not solved by the settle: a moved lamp's light there stays drawn from the worker's parts (the light tree thread's lists
  and InnerNormScale as at the move; that the room 0 lists stay the same is UNVERIFIED), and the roofed rooms whose opening
  outdoor total (OpeningOutdoorTotal) includes the lamp keep their kept entry cut until their own solve, as for a switch. A is MountedNdl's factor over the game's evaluation at the new place (x GameOverLaw over the law): the game's evaluation
  there is the law to 99.5% (the probe), so the kept capture's A (the game's colour over the law) and a moved lamp's differ
  where they differ. The room's light lists are read as the edit sees them: that a move within the room changes no list is
  UNVERIFIED, as is that the lamp object's own 3D records change only after the edit (the move compares them with the kept
  solve's at the edit; the settle's solve takes them in either way). The lamp's occluder marks are dropped as Real-time
  outdoor lamps drops them for a move. A switch on after a moved copy was kept takes the solves (its rooms' versions
  changed). A copy is a whole kept room (about 0.5 MB) made per job landed; it replaces the room's kept solve within the 192 MB budget as a solve is kept (past it the move is left to the room's solve: `move: lamp L story S room R: left to the room solve (the 192 MB budget: ...)`).
- Directional maps (stage 4): written into the game's own textures of the set, on the light tree thread, as the game's
  unlock writes them; between two of the four maps' writes a frame may draw with some maps written and others not (as
  with the game's own copy). A solve that locks the working maps on another thread between the check and the write would copy
  the set it read back over the write (UNVERIFIED that solves run on another thread while the light tree thread updates).
  Rooms whose capture did not pack back exactly, a room whose solve was not kept, a part that is not the game's step times
  a share (stair-height samples of a lamp of the story above) where the step is lit, a lamp the capture never saw in the
  room (unless carried) and moves with basis_wall_filter on keep the game's directional texels until the settle's solve
  (logged). That the step's rig colour (vfunc+0x10) already holds the edit at the end of the room update, and is 0 while
  the lamp is off, is UNVERIFIED (switch off is written as 0 without it). Waits at most 1 s for the working maps, then
  leaves the maps to the settle. Not checked in game yet.
- The sum is added with post-pixel-shader blending into an A32B32G32R32F target; a device without it draws nothing (logged).
- Each part is a 80-byte point in a DEFAULT vertex buffer (estimated, not measured: 0.5 to 6 MB of video memory a room).
- Glass is charged on kept rooms only: room 0's physical draw has no pane pass, so an indoor lamp through a window lights the outside
  at full strength. If the game charges that pass (read in its code, not measured), the outside is up to 31% brighter than its solve.
- `GpuRoomLamps::PhysPasses` does not fall back to fewer tile rows when growing the atlas fails: that room shows the game's map until a
  retry works. Reachable only when video memory runs out.
- `T3dDensePS` walks every pane of the lot for each texel and lamp; its cost on a big lot is not measured.

## Technical reference

- **Logs** (`[GpuRoomLamps]`): `Story S room R class C: kept N texels, K drawn lamps, P parts (U never tested by the game),
  X MB; M rooms kept, Y MB of 192 MB`; physical: `Story S room R class C: kept N texels (physical: no parts), X MB; M rooms, Y
  MB of 192`, `Story S room 0: base without K lot lamps (physical)` (from `ExportRoom0`), `Model physical|replica: ...`, `not
  kept (the model changed during its solve)`; `Story S room R class C <map>: not drawn (physical: ...)`, `Story S physical:
  every map drawn (N maps of its rooms > 0; room 0 from its solve)` / `not covered (K of N maps not drawn: why)`; `physical <kind>: lamp L, no room
  solved, M maps relit in N ms (frames +k)` / `physical <kind>: lamp L left to the room solves (why)`, every 10 s with edits `physical lamp
  edits in the last 10 s: T taken, R left to the room solves; S room solves of the lot within 3 s of an edit taken outside the
  settle, Z settle or resend solves; physical relights M maps, X ms CPU (largest frame Y ms)`, `Story S room R: solved off the
  light tree thread, its kept physical copy stays drawn (the same geometry); sent to one solve here`, `physical: lamp L left the
  lot's registry (deleted, or moved to a basement or off the lot): no map draws it now; its rooms take the game's object removal
  marks`, `[LampMarkFilter] physical move to another room: lamp L left to the room solves (into room 0: ...)`, `[RtOutdoorLamps]
  physical: the rooms that take room 0's lamps not sent after the edit of N lamps (every story draws them from their laws)`; with the probe,
  `... physical vs the game's last solve: N texels, P% within 2/255, mean M levels, X% brighter > 8 levels, Y% darker > 8
  levels`; `not kept (the 192 MB budget: ...)`, `not kept (no memory)`, `not kept (why)`; with
  the probe on, `Story S room R class C self-check: N texels lit, P% within 2/255 of the solve's sum recomposed from the kept
  parts (L lamps without a law now)`, then up to three of the worst texels: position, map, the solve's sum and the
  recomposed one, and the texel's largest kept part (lamp, entries, A, B, the law there, bits: 1 early-out, 2 never tested,
  4 soft light cut).
- **Kept part** (12 bytes): texel index, A, B and threshold scale as halves, entries, bits. Per texel 92 bytes: the 0x30
  sample, the capture's texel record and the base; with the tail 32 bytes more (`KeptTail`: pack input, normalisation,
  final texel, its source texel, bits), plus 4 bytes per class-2 wall block texel (and 16 per block column with rows
  beyond its edges) and 16 bytes per tile-edge copy to a place no kept texel holds.
- **Tail capture** (stage 2a): `RoomMapDither`'s thunk passes the caller's esp; the pack input is the caller's `[esp+60h]`
  (`movaps [esp+60h],xmm0; movss [esp+6Ch],xmm1` at `0x006A34B2`, checked at start) and goes to `OutdoorDelta::NotePack`
  only for the texel `SampleEnd` just recorded on the light tree thread (a thread-local hand-off). `room+0x160` is read at
  each pack. k (`[0x011D0BA0]`) is read from the immediate of `mov ecx,imm32` at `FUN_006a31d0 + 0x1D0`, after checking
  the `call 0x00F626F0` (`mov eax,ecx; ret`) and `movss xmm0,[eax]` around it. `BeforeFinalize` reads each kept place
  (walls already blurred), and at class 2 (`FUN_006a8ca0` true) runs `0x0069EFC0` on the floor and ceiling maps with every
  place holding its own index, reads which index landed where, and writes the map back. (Tagging only the kept places with
  a value range no other texel held failed in 23 of 84 solves: black texels of every alpha left no free range.)
- **Rebuild** (`KeptFinalCheck`): `RtLampModel::KeptTexel` (roofed: `IndoorSplit` at the solve's sum gives ramp x ambient
  and ramp, `IndoorTexel` at the new sum, [0, 1], the 5/255 alpha floor; roofless: `PackInput` with the solve's alpha;
  then the dither), the game's pack `0x0060CD10`, `BlurBlock` per block in BlurWalls' order, the copies from their source.
  `ComposeKeptPS` has the alpha floor; `ComposeRlgPS` (the earlier experiment's) does not.
- **GPU** (`gpu_room_lamps.cpp`, render thread): `RtOutdoorLamps` mixes `GpuRoomLamps::Sig` (the kept rooms' versions, each
  lamp's law from `RtOutdoorLamps::WorldLaw` now) into each active-lot map's signature, and after its `Compose` calls
  `GpuRoomLamps::ComposeOver`. `OutdoorDelta::BuildKeptMap` turns a kept room's map into points (`RtLampGpu::KeptMapData`):
  parts grouped by lamp in the sum's layout (texel j of the map at `(j % 1024, j / 1024)`), one compose point per place's last
  write (ramp x ambient and ramp from `IndoorSplit` on the CPU), the class-2 blocks with their rows beyond packed in shelves
  of a layout as wide as the atlas, the copies (to places kept or not) and the restores. `Engine::RelightKept`: `AccKeptPS`
  per lamp (blend one, one), `ComposeKeptPS` into the map or the layout, `BlurRowPS` / `BlurColPS` over the layout's blocks,
  `BlockKeptPS` back into the map, the copies with `ComposeKeptPS` (the source texel at the copy's place), the restores with
  `CopyAtPS` from the game's map; the map targets take the room's own map size. Buffers are DEFAULT and dropped at a device
  reset (made again on use). `LevelLightShare`'s `FinalizeHook` calls `OutdoorDelta::RoomFinalized` after the game's
  finalize on any thread.
- **Room 0's kept lamps**: `Capture::keep0Lights` (set at room 0's solve start: the lot lamps of its list with a law and not
  drawn by Real-time outdoor lamps); the probe's hooks run for them (`OnEvalOut` and the rest take `keep0` as `probe`);
  `BuildKept0` after the solve (`KeptPartsOf`, shared with `KeepEnd`) into `Record::kept0`; `BaseHash` and `ExportRoom0`
  take their parts out of the base; `Room0Export::kept0` (A, B and the threshold scale as floats, the law when kept, `exit`,
  `mask`: per masked texel the walls-tested and no-walls luminance over `RtLampModel::KeptWrapLum` as floats);
  exit ground texels: `SolveGroundTexel` reports whether it tapped the texel, `SolvePointBatchImpl` then runs
  `GroundFloorTileAt` for it too while the capture keeps room 0 lamps (`OutdoorDelta::KeepsGround`) and calls
  `OutdoorDelta::ExitGround(not a tile)` (`kExitGround` 8 in the texel's bits); with a kept exit lamp `ExportRoom0` marks
  those texels (bit 4) with `exitBase`, the luminance of the exit lamps' light left in the base; `PrepareJob` puts them in
  the texel's mask.z / mask.w, the exit lamps' parts get mask.z = 1 and each mask part is a point of no light (A 0) with
  mask.xy = its shares; `AddKeptParts(extra)` draws `AccKept0PS` (alpha = the walls-tested luminance) into the sum, then
  `AccKept0XPS` (x "all", y the exit light's luminance) into the other sum (cleared), and `Compose` with `Params::keptX`
  draws `ComposeK0PS` (ComposePS reading that sum at s3); ComposePS itself is unchanged;
  `PrepareJob` makes one point per part at its texel's place (each place's last write), world position and normal;
  `Slot::kvb` (MANAGED); every frame each one's law now (moved: its law when kept) goes into the map's signature, and
  `Engine::AddKeptParts` adds them into the current sum (blend one, one) between `Accumulate` and `Compose`. Coverage:
  `Cover::kept0` (story, lamp) for `CoversLamp`.
- **Maps without room-0 texels**: a slot with no room-0 texel (`used` false) of the active lot takes the game's own map
  (`OwnMap`), a copy (`CopyTexture` of the game's map), then `ComposeOver`, while `GpuRoomLamps::Sig` has rooms for it; its
  copy goes when none is left.
- **Area lights**: `WorldLaw` reads +0x170, +0x190, +0x180, +0x1A0 and +0x1A4 for types 9 and 10 (`ReadLamp`, `ToLaw`,
  `GpuLampLaw::AreaShape`, after the reach).
- **TubeLights**: `RtOutdoorLamps::WorldLaw` reads +0x170 and +0x1A0 for type 6 (`ReadLamp`, `ToLaw`, `GpuLampLaw::TubeShape`);
  without a segment, `RtOutdoorLamps::ColourLaw` (type 6, of the lot, alive): its colour +0xE0 and a signature of
  +0x10, +0x120..+0x130, +0x170..+0x17C and +0x1A0 (`ReadTubeShare`). `KeptLamp::colourOnly`; `AccKeptPS` takes the colour
  as its law when c96.z is 1 (lawOk 2). `LawOfLight` / `LawNow` / `LawAt` in `outdoor_delta.cpp`.
- **Edits**: `LampMarkFilter`'s `MarkDecideImpl`, after Real-time outdoor lamps' own branch, computes the edit's kind as that
  branch does (0 a value or a switch) and asks `OutdoorDelta::TakesLampEdit`: `LevelLightShare::LotRooms` (stories -4 to 7:
  room 0 and the rooms the lighting tiles name), each room's list (+0xC8..+0xCC); a room 0 listing it needs
  `RtOutdoorLamps::CoversLamp`; another room its kept solve (`editable`, the lamp not in `baked`) and
  `GpuRoomLamps::Drawn(room, version, maps)` (set by `ComposeOver` per map drawn, cleared when the room is dropped or a lamp
  of it moved). Taken: the lamp's state is updated as Real-time outdoor lamps' branch does, `NoteHandledLight`, and
  `NoteGpuEdit` for a player's edit. The settle (`OutdoorDelta::SettleDue`, from `OutdoorDelta::AfterRoomUpdate` on the light
  tree thread): one entry per lamp (its lot, its rooms as story and id, merged over its edits, and its last edit's tick);
  2000 ms after it, each room is sent with `LevelLightShare::QueueRoomId(tracker, story, id, -1)` (no urgency, no edit hold).
- **Carried parts** (`CarryOff`, from `KeepEnd` on the light tree thread): `RoomFinalized` keeps the kept solves it drops in
  `g_finalized` (the last finalize's); the room's next kept solve takes each lamp of its earlier one listed in `g_takenOff`
  with (room, that version) and absent from the new solve, when `KeptRoom::geo` (at the solve's start, `ProbeStart`:
  `RoomGeoKey`, read with `ReadRoomTests`, kept with its three parts) is the same and non-zero, the texels'
  (x, y, map) and the 0x30 samples are equal, and `LawOfLight` gives the same position (a tube: the same signature). The lamp
  is copied with `carried` set and its `g_takenOff` entry moves to the new version.
- **The lamp's own object** (switch and value edits): `FUN_006c7250` updates the occluder entries (tl+0xD4) before the light entries (tl+0x90), so the occluder mark of a lamp's object (`0x006C7939`) comes before `MarkDecideImpl` takes its edit; the game then gathered and solved that room and the changed-room walk sent the rooms of other stories taking its lamps (in game, 2026-11-02 11:55:20: a switch off taken, story 2 room 5 and story 1 room 2 solved before the settle, their kept solves replaced). With the option on, `OccDecide` drops an occluder mark within 1.5 m of a lamp of the story whose values differ from its last update, that did not move, and whose edit `TakesLampEdit(..., dry)` would take (`[LampMarkFilter] Occluder mark of room R dropped: the object of a lamp whose switch or value edit the GPU room lamps take`); a value edit then refused still marks the lamp's room through its own update. UNVERIFIED in game: that the message changes what `Signature` reads before that update.
- **Edit logs**: `[GpuRoomLamps] switch on|switch off|value: lamp L, R rooms on stories S, no room solved, relit in N ms (wall
  atlas X ms +F, floor ...)` (from the mark to the first relight of every map of its rooms holding its parts, then per map
  kind that time and the relight round it came in, +0 the edit's own; a map without its parts changes nothing and is not
  waited for: until 2026-11-02 it was, and a switch shown in 2 frames was logged as `relit in 502.2 ms` when an unrelated
  solve of the room replaced its kept solve; `(no kept map of its rooms holds its parts)` when none does); `Lamp L: its edit
  was not shown in every room within 5000 ms (...), shown so far (...)`. The maps of an edit taken (their kept rooms' signature
  changed, or room 0's kept lamps hold the lamp of an edit not yet shown) are relit first in Real-time outdoor lamps' round
  and past its 2 ms frame budget up to 8 ms (`kEditFrameCpuMs`); the throttle of relights made only for game writes
  (`kGameWriteGapMs`) never applies to them (their signature changed);
  `<kind>: lamp L left to the room solves (story S room R: why)` (no kept solve of its class, a wall texel the GPU does not
  blur, its kept solve has the lamp in its base, a map of it not drawn on the GPU, a room 0 Real-time outdoor lamps does not
  draw it in (no law, or a floor mask part where its law is dark)`, `a light without a GPU law whose intensity or shape
  changed`); `switch on: lamp L left to the room solves (its rooms changed since it was switched off)`; `switch on: lamp L
  drawn from its parts carried into N of its R rooms' later kept solves`; `Story S room R class C: switched-off lamps' parts
  carried from its last kept solve: L, ...[; not carried: L (the lot's geometry changed | the room's walls changed | the
  room's 3D blockers changed | the room's walls or 3D blockers could not be read | its
  texels or samples changed | it has no law now | it moved or its shape changed), ...]`; `Story S room 0 class C: K lamps
  kept for the GPU (E exit lamps, M with floor mask parts: P parts)[; left in the base (a floor mask part where its law is
  dark): L, ...]`; `settle: lamp L, R
  rooms sent to their solve 2 s after its last edit[, N sent for another lamp just before][, N refused by the room queue
  (already waiting for their gather, or gone)]` (each room once for the lamps due together); a tube switched off (its lit
  bit clear or its intensity 0) counts as a switch, not a change of its share; `Story S room R class
  C: not kept (its class was C0 when its solve started)`.
- **GPU logs**: `Story S room R class C <map> GPU self-check: N texels, P% within 2/255 (alpha within A/255) of the texels the
  game left` (with the probe on, once per kept solve and map, after its first relight; the texels the game left at the
  solve's end against the relit copy read back), then up to three worst places; `... <map>: not drawn (why)`, `... its buffers
  could not be made`, `... kept for a WxH map, the map is WxH`.
- **Logs**: with the probe on, after the self-check, `Story S room R class C final self-check: N texels, P% within 2/255
  (alpha within A/255)`, up to three worst texels (levels, map and place, copy source, the recomposed and solve's sums, the
  toned colour, ramp, ramp x ambient, dither, pack input, packed game vs recomposed before the blur, final game vs
  recomposed), then the room (norm, k, ambient +0x110, pass ambient +0x650), the copies to other places, the texels
  overwritten by copies of texels no capture holds, the wall texels not blurred again, the roofed texels without a ramp
  read back and the time of both checks; or `final self-check: not run (why)`.
- **Lamp moves** (`outdoor_delta.cpp`: `MoveJobFor`, `ReadRoomTests`, `QueueMove`, `MoveWorker`; `KeptRoom::batches` and
  `movedFrom`): `[GpuRoomLamps] move: lamp L story S room R class C: N texels x 1 lamp recomputed (lit, parts) in X ms on the
  move worker, Y ms after the move; kept as version V | dropped (the room was solved or kept again meanwhile)`, then the edit
  line `move: lamp L, 1 rooms on stories S, no room solved, relit in N ms (...)` from that landing to the first relight
  with its parts; `move: lamp L left to the room solves (story S room R: why)` (room 0: a lamp not of its own story's inner list, floor mask parts, no kept record holding it, the lot's geometry changed; an outdoor lamp, the lamp
  under the room's story floor, a roofless room, through openings or from another story, a light without a GPU law, the
  lot's geometry, the room's walls or its 3D blockers changed since its solve, a 3D blocker of a kind the copy does not know, ...). The game's tests' constants are read
  once from the game (`RoomLightTests::kConstAt`). A room 0 move asks only that Real-time outdoor lamps covers room 0
  (`CoversRoom`) and that its kept record holds the lamp (`MoveJobFor0`), not the maps drawn: no slot is uploaded during a
  drag, so a lamp that came into room 0's list mid-drag was refused at every step (`a room 0 Real-time outdoor lamps does not
  draw it in`, 263 times in one test) and each step sent room 0 to the game's solve (12-15 ms of delta) and the lamp's own
  room to the game's path. Room 0's kept lamps are part of `BaseHash` (a solve that keeps another set of them over the same
  base is a new export; before, such a lamp was never uploaded). The maps of an edit not yet shown (`GpuRoomLamps::ShowsEdit`)
  are relit first, the floor first, past every frame budget (a moved lamp's floor waited 6 rounds, 260 ms, behind other
  rooms' new kept solves); a move's copy has no GPU self-check (its texels are not the game's; the read-back stalled the
  frame). Room 0's own maps still show a move only once the drag ends (UNVERIFIED in game).
- **Directional maps** (stage 4, verified with dumpbin on Steam 1.67.2): state 7 of the solve calls `FUN_006a3b80` on
  room+0x540, which calls `FUN_006a09f0` (this = room, the pass); it lays the samples into the vector `[0x01158AC8,
  0x01158ACC)` (0x30 each, texel x / y at +0x20 / +0x22), and per sample zeroes 16 floats, calls `FUN_0069f280`
  stdcall(sample + (0, 0.5, 0, 0), light, acc) for each light of room+0xC8..+0xCC (the call at 0x006A0C56, LevelLightShare's
  `BasisLightHook`), then per map i packs min(max(acc_i, 0), 1) with alpha [0x0107A538] through 0x0060CD10 into the lock
  record at the pass + 0x44 + i x 0x28 = room+0x584. `FUN_0069f280`: the light's place (vfunc+0x24), its rig colour at the
  point (vfunc+0x10, not the wrapped evaluation +0x4C), the unit direction d (rsqrt with two Newton steps), w_i = clamp(b_i .
  d, 0, 1) for the four constant directions, acc_i += w_i x colour / sum w. The lock step 0x0069FA40 copies the set [room+8]
  into the working set 0x01158AF0; the finalize's unlock 0x0069E950 (0x006A0E3D) copies it back when room+0x629 is set
  (0x006A8FD0: per map 0x00619A00, LockRect / UnlockRect of [wrapper+0x30]). Apex: `LevelLightShare::BasisSamplesVec` reads the
  vector from `mov eax,[0x01158AC8]` at the hooked call - 0x86; `BasisLightHook` (with the capture on, on the light tree
  thread, for a kept capture) runs Apex's body, calls the step again with a zero accumulator and hands both to
  `OutdoorDelta::NoteBasis`; `KeepBasisFinal` (BeforeFinalize) reads the four lock records and each sample's texels;
  `BuildBasis` (KeepEnd) sums the baked lights into the base, keeps each other light's part and share (`BasisPass`,
  `outdoor_delta_model.h`) and packs back (`PackBasis`: `BasisPackInput` and the game's pack). Edits: `TakesLampEdit` queues
  (`QueueBasis`), `AfterRoomUpdate` runs `BasisDue` (waits for `LevelLightShare::RoomMapsFree`), `LampBasisNow` (the step
  through `LevelLightShare::BasisStep`, `BasisPartNow`), `WriteBasis` (`WriteBasisMapTry`: [set + 4m], [wrapper+0x2C] 0, an
  A8R8G8B8 texture of the captured size, each texel that holds the expected value). Logs: `Story S room R class C: D
  directional samples, L lamps (P parts, Q of them the game's step called again times a share) packed again from their
  parts: K of M texels within 1/255 (largest X/255)[; its directional maps take no edit]`; per edit taken `<kind>: lamp L
  directional maps written in N of R rooms T ms after the edit (W texels[, S left as another solve wrote them])[; not
  written: story S room R: why, ... (they follow its rooms' solves)]`; `... directional maps not written (another room's maps
  stayed locked for 1 s; ...)`. Physical (stage 5): `physical <kind>: lamp L directional maps written in N of R rooms T ms
  after the edit (W texels[, S left ...]; K lamps' shares in M rooms in X ms on a worker[, U 3D blockers of another vtable
  taken as 1])[; not written: ...]`, `physical: directional maps written again in N of R rooms after their kept solve; ...`.
- **Test**: `tools/outdoor_delta_test` checks `KeptAdd` (the soft light cut and its scaled threshold, the early-out, the
  game's drop, half-float A and B) and `KeptTexel` (20000 roofed texels against the game's tail at their own sum and after
  an edit, weight-0 texels, the alpha floor, a roofless texel's hue and alpha). `tools/rt_lamps_test` draws a synthetic kept
  room on a floor (copies, restores; its own map 48x40 under a 64x64 working map, copies from past the corner) and a class-2
  wall atlas (three blocks, rows beyond above, below, both, none) with the production passes on a Direct3D 9 device and
  compares every texel of the own map with `KeptAdd` + `KeptTexel` + the pack + `BlurBlock`, one of its lamps a light with its
  colour alone, one a TubeLight and one a RectangleAreaLight; a wall atlas of another size and a map larger than the working map are refused. Room 0: two kept lamps (one
  its colour alone) added by `AddKeptParts` between `Accumulate` and `ComposePS`, against `KeptAdd` + `PackInput` + the pack.
  Room 0's exit and floor mask lamps (`Room0KeptXCheck`): masked, exit ground and plain texels, two exit lamps with floor mask
  parts (one its colour alone), the laws as kept, dimmed and recoloured, one switched off, and the first as a TubeLight, a RectangleAreaLight and a DiscAreaLight, through `AddKeptParts(extra)`
  and `ComposeK0PS` against `KeptAdd`, `KeptWrapLum` and `RtLampModel::Compose` with the exit light: 12,288 texels, all exact
  (0/255). `tools/apex_paths_replay` also compares the dumped law x `GameOverLaw` with the game's evaluation at
  every T and Z line (dumps 135710 and 145922: 131,290 and 122,547 lines, all within 2/255); a TubeLight's lines are
  counted apart (the probe dumps a tube's law since the tube law, and an area light's since the area law: the next dump checks them; area lights fall in the first line).
  A moved lamp (`KeptGpuCheck`, lamp 4): its parts from `RoomLampMove::PartAt` at its new place behind a wall and over a
  disc blocker, drawn on the GPU against the CPU: 0/255. Offline, `tools/apex_paths_replay` runs `PartAt` with the game's
  evaluation as the law and `KeptAdd` at every recorded texel of the plain path (`RoomLampProbe-20261101-165037`): own lamps
  72,534 lines, 55,139 lit, 100.00% within 2/255 of the measured part; the 28,044 lines of other rooms' lamps are all out of
  its scope (floor cut, openings, between stories). `tools/room_lamp_3d_replay` runs the same `room_light_tests.h`. The directional maps (`BasisChecks` in
  `tools/outdoor_delta_test`): 300 samples of a model of the game's fill with six lights (one scaled by Apex's share, one
  blocked, one listed twice, one in the base): the parts pack back to the game's texels and each share is found (1200 of 1200
  texels, 1500 of 1500 shares); a switch off, a scaled lamp recoloured and dimmed, a lamp listed twice made brighter and a plain
  lamp moved, each part computed again and packed, against the model's fill with the edit: 1200 of 1200 texels exact each;
  a part that is not a multiple of the step has no share, an unknown share is refused only where the step is lit.
- **In game** (2026-11-02, stage 2a): final self-check 61 room solves, 207,425 texels, 100.00% within 2/255, alpha within
  1/255, at most 2.6 ms; 23 solves not checked (no free tag range, fixed since). Stage 2b (09:50-09:52): GPU self-check 148
  maps, 273,754 texels, 100.00% within 2/255 (alpha within 1/255), all of them wall atlases: floors and ceilings were refused
  (own map 256x256 under a 512x256 working map, and a class-0 solve kept as class 2 with 64x64 maps; both fixed since). The
  own-map size rule, the edits and the moves have not run in game yet (UNVERIFIED). Round 2 (10:28): GPU self-checks wall
  atlas 137, floor 121, ceiling 136 maps, all 100%; no edit taken (indoor lamps listed in room 0, and maps with no room-0
  texels; both handled since, UNVERIFIED in game).

## See also

- [Validation](../../validation/night-lighting-gpu-room-lamps.md), [history](../../history/night-lighting-gpu-room-lamps.md)
- [Room lamp probe](gpu-room-lamps-probe.md), [Real-time outdoor lamps](rt-outdoor-lamps.md), [Quick outdoor lamp edits](outdoor-lamp-delta.md)
- [engine/room-light-maps.md](../../engine/room-light-maps.md)
