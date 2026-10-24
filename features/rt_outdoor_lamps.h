#pragma once
// Real-time outdoor lamps (Performance > Lamp lighting > Instant, "lampLightingMode", Instant by default since 14/10;
// docs/features/night-lighting/rt-outdoor-lamps.md).
//
// The outdoor lot lamps of the active lot are drawn into its stories' room-0 light maps on the GPU, from their current
// values, with shadows of the lot's walls, roofs and slabs: a lamp moved, recoloured, dimmed, switched, added or removed
// shows on every story in the next frame, and its edit solves no room 0.
//   - The base: Quick outdoor lamp edits (outdoor_delta.h) keep each room 0's sum and each light's part of it per texel
//     (OutdoorDelta::ExportRoom0). The parts of the lamps the GPU draws (RtLampModel::Drawn) leave the sum: what remains is
//     the texel without them, exact (window lights, street lamps of the world, other lots' lamps stay in it).
//   - The maps: each story's own maps of the class it shows (the room's texture set [room+4] + class x 64 + 0xC, which the
//     solve's unlock 0x0069E950 copies the class's working maps into; Steam 1.67.2, read with dumpbin) get a DEFAULT-pool
//     copy: the game's map copied, every room-0 texel of the kept solve relit over it (rt_lamp_gpu.h) as
//     pack(min(base + sum of mult x fade(law x shadow), 1)) with the dither, the floor mask's alpha and the class-2 wall blur.
//   - The swap: a SetTexture callback binds the copy wherever the game binds its map, so every draw that reads room 0's light
//     (outdoor walls, floors, lot grass, snow, rain, Apex's own passes) takes it, with no shader patch.
//   - Edits: LampMarkFilter's lamp entry update asks HandlesLamp; a lamp the GPU draws on every story of a lot whose maps are
//     all relit marks no room (no room 0 is solved again); the rooms that take room 0's lamps (yards, roofed rooms through
//     openings) are still sent, once the update is over (AfterRoomUpdate).
//   - Shadows: the lot's occluders from its story grids (LevelLightShare::ExportLotProxy, rt_lamp_proxy.h), a cube shadow
//     map per lamp; each map is relit only when something it depends on changed (a lamp, its kept solve, the game's map, the
//     occluders, an option); nothing is drawn otherwise.
// Off: nothing of this runs and no texture is swapped (the copies are dropped, the game's maps bound again).
#include <d3d9.h>
#include <cstdint>
#include <string>

namespace RtOutdoorLamps {

// Render thread, every frame (Night Lighting's Present): the Instant lamp lighting, and Quick outdoor lamp edits keeping parts
void SetEnabled(bool on);
// (2.15.1) Lamps lying on the lot's ground light the lawn from at least 40 cm (developer A/B and how many times it applied)
bool LowLampLift();
void SetLowLampLift(bool on);
long LowLampLifted();
bool Enabled();
bool Watching(); // any thread: on, or copies still to drop
// Render thread, every frame: installs the callbacks once (EndScene pass, SetTexture swap, device reset)
void OnPresent(IDirect3DDevice9* dev);
// Render thread, SetTexture: a room-0 map with a relit copy is bound as the copy (CallOriginalSetTexture). True when it
// was. Called by the one SetTexture callback of both relit-copy swaps (RoomLampsGpu::RegisterMapSwap).
bool SwapTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* t);
void OnWorldChanged();
// Render thread: discard relit copies, jobs and shadows at the next Present.
// Old proxy builds cannot publish into the new generation.
void RestartLighting();
bool RestartPending();
bool RestartIncomplete(); // terminal map fallback, rather than work still running

// ---- the light tree thread (LevelLightShare's and LampMarkFilter's hooks) ----
// The lamp entry update of `light` (registered in `room` of tree level `treeLevel`): true = the GPU draws it on every story
// of its lot now, so its edit marks no room
bool HandlesLamp(uintptr_t treeLevel, uintptr_t light, int room);
// Any thread: `room` is a room 0 of the active lot while every story's maps are relit (OutdoorDelta's lock step leaves the
// deltas of drawn lamps out then)
bool CoversRoom(const void* room);
// An edit of such a lamp (kind 0 a value or a switch, 1 a move): timed until every story shows it; the rooms that take room
// 0's lamps are sent after the room update
void NoteLampEdit(uintptr_t treeLevel, uintptr_t light, int kind);
void AfterRoomUpdate(const void* treeLevel);
// A GPU lamp switched on: its edit took the usual path (its stories gather it again); the log follows the stories that draw it
void NoteSwitchOn(uintptr_t treeLevel, uintptr_t light);
// Any thread: a room's solve ended (its story's maps of its class were written back); on the light tree thread (gather) room
// 0's end also reads the lot's occluders again when its walls, floors or openings changed
void NoteRoomSolved(const void* room, bool gather);
// Quick outdoor lamp edits rewrote room 0's maps outside a solve
void NoteMapsWritten(const void* room);
// Any thread (AtriumHold's texture unlock): a texture was written
void NoteUnlock(const void* texture);
// Render thread: the game's map behind a relit copy (Apex's own checks of a bound map), else the texture itself
const void* GameMapOf(const void* texture);

// The menu: the last measured GPU time of a relight (ms), -1 while off or not measured
float GpuMs();
// The menu: why this device cannot run it (an English menu text, translated by the menu), empty when it can or before the
// device was read
std::string Unsupported();
// The menu: why it is on but draws nothing for a reason the player can fix or should know (an English menu text), else empty
std::string InactiveNote();
std::string Status();

} // namespace RtOutdoorLamps
