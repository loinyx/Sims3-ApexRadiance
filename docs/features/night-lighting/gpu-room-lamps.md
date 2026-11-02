# GPU room lamps

A developer option, the first stages of drawing indoor lamps on the GPU: each solve of a room of the active lot other than
room 0 is kept, split into the light the GPU will not draw and, per texel and lot lamp, the share of the lamp's law that
Apex's tests and the game's tests let through, with the room's tail (tone, ambient, dither, pack, wall blur, tile-edge
copies). The kept rooms are drawn on the GPU over Real-time outdoor lamps' copy of each story map with the lot lamps as they
are now, so a lamp's colour, intensity or switch shows in the frame of the edit. With [the room lamp
probe](gpu-room-lamps-probe.md) on too, each kept solve is recomposed on the CPU and on the GPU and compared with the game's.

## Status

| | |
|---|---|
| Availability | Developer build only, not released |
| Default | Off |
| Menu | Developer > Compare lighting paths > Keep indoor lamps for GPU (with Lamp lighting: Instant) |
| Configuration | `gpuRoomLampsCapture` under `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/outdoor_delta.cpp`](../../../features/outdoor_delta.cpp) (`KeepEnd`, `KeptSelfCheck`, `KeepFinals`, `KeptFinalCheck`, `BuildKeptMap`, `RoomFinalized`), [`features/rt_lamp_model.h`](../../../features/rt_lamp_model.h) (`KeptAdd`, `KeptTexel`), [`features/gpu_room_lamps.cpp`](../../../features/gpu_room_lamps.cpp), [`features/rt_lamp_gpu.cpp`](../../../features/rt_lamp_gpu.cpp) (`RelightKept`), [`shaders/rt_lamps_hlsl.h`](../../../shaders/rt_lamps_hlsl.h) (`AccKeptPS`, `ComposeKeptPS`, `CopyAtPS`, `BlockKeptPS`), [`features/room_map_dither.cpp`](../../../features/room_map_dither.cpp) (the pack input), [`features/level_light_share.cpp`](../../../features/level_light_share.cpp) (`LightEvalHook`, `FinalizeHook`) |

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
   blocks are blurred the game's way in a layout of their own and written back; the tile-edge copies are made again from
   the composed texels. The copy is relit whenever a kept room's lamp changes (its law is part of the map's signature), so a
   recolour, a dimmer or a switch shows at once, before the room's own solve ends.
8. When the game writes a room's texels again (the finalize of any solve of it, on any thread), its kept solve is dropped
   until its next kept solve, and the copy shows the game's texels meanwhile.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Keep indoor lamps for GPU | `gpuRoomLampsCapture` | bool | off | | Developer only, with Lamp lighting: Instant. Keeps each room solve of the active lot and draws it on the GPU; with the probe on, logs the self-checks |

## Limitations

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
  rebuilt without the ambient term, exact while it stays that bright; a lamp edit that darkens it would need the ramp from
  `FUN_006ab210` (not kept yet).
- A roofless room's alpha (the floor mask, the exit light) and the rows lit beyond a class-2 wall block's edges are kept as
  the solve left them, not recomposed from the lamps.
- A kept texel a tile-edge copy of a texel no capture holds overwrote keeps the game's value; a class-2 wall texel in no
  block Apex blurs (or walls the game's own blur did) is not rebuilt. Both are counted in the log.
- The tile-edge copies are found by a dry run of the game's own copy (`0x0069EFC0`) on tagged texels before the finalize,
  after which the maps are put back and the finalize copies them as always. That the copies write only the map they are
  given is UNVERIFIED (`0x006A9DE0` / `0x006A9A50` not read in full); the dry run snapshots that map only.
- Drawn only where Real-time outdoor lamps relights the story map (the active lot, the class each story shows): a kept room
  of a story or class it does not relight stays the game's. The menu text still reads "Keep indoor lamps for GPU": the
  option now also draws.
- Lamp edits still send the rooms to their solves (LampMarkFilter is not extended yet): the GPU shows the edit at once, and
  the room's solve then replaces its kept data. A lamp moved shows its new law over the old shares A and B (the shadows of
  its old place) until that solve ends.
- The sum is added with post-pixel-shader blending into an A32B32G32R32F target; a device without it draws nothing (logged).
- Each part is a 80-byte point in a DEFAULT vertex buffer (estimated, not measured: 0.5 to 6 MB of video memory a room).

## Technical reference

- **Logs** (`[GpuRoomLamps]`): `Story S room R class C: kept N texels, K drawn lamps, P parts (U never tested by the game),
  X MB; M rooms kept, Y MB of 192 MB`; `not kept (the 192 MB budget: ...)`, `not kept (no memory)`, `not kept (why)`; with
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
  `BlockKeptPS` back into the map, `CopyAtPS` from a copy of the composed map and from the game's map. Buffers are DEFAULT
  and dropped at a device reset (made again on use). `LevelLightShare`'s `FinalizeHook` calls `OutdoorDelta::RoomFinalized`
  after the game's finalize on any thread.
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
- **Test**: `tools/outdoor_delta_test` checks `KeptAdd` (the soft light cut and its scaled threshold, the early-out, the
  game's drop, half-float A and B) and `KeptTexel` (20000 roofed texels against the game's tail at their own sum and after
  an edit, weight-0 texels, the alpha floor, a roofless texel's hue and alpha). `tools/rt_lamps_test` draws a synthetic kept
  room on a floor (copies, restores) and a class-2 wall atlas (three blocks, rows beyond above, below, both, none) with the
  production passes on a Direct3D 9 device and compares every texel with `KeptAdd` + `KeptTexel` + the pack + `BlurBlock`.
- **In game** (2026-11-02, stage 2a): final self-check 61 room solves, 207,425 texels, 100.00% within 2/255, alpha within
  1/255, at most 2.6 ms; 23 solves not checked (no free tag range, fixed since). The GPU path has not run in game yet
  (UNVERIFIED).

## See also

- [Room lamp probe](gpu-room-lamps-probe.md), [Real-time outdoor lamps](rt-outdoor-lamps.md), [Quick outdoor lamp edits](outdoor-lamp-delta.md)
- [engine/room-light-maps.md](../../engine/room-light-maps.md)
