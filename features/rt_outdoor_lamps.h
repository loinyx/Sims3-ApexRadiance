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
#include <memory>
#include <string>

namespace RtLampGpu {
struct DeviceCalls;
}
namespace RtLampProxy {
struct Lot;
}
namespace GpuLampLaw {
struct Lamp;
}

namespace RtOutdoorLamps {

// The device calls Apex's own hooks never see (Apex's CallOriginal*), for other GPU passes (lot_light_bridge.cpp: the
// ground shade)
RtLampGpu::DeviceCalls DeviceCallsOfGame();

// Render thread, every frame (Night Lighting's Present): the Instant lamp lighting, and Quick outdoor lamp edits keeping parts
void SetEnabled(bool on);
// (2.15.1) Lamps lying on the lot's ground light the lawn from at least 40 cm (developer A/B and how many times it applied)
bool LowLampLift();
void SetLowLampLift(bool on);
long LowLampLifted();
bool Enabled();
bool Watching(); // any thread: on, or copies still to drop
bool RetiredAny(); // any thread: relit copies of lots left are still bound (their maps' unlocks must reach NoteUnlock)
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
// Any thread: `room0` is a room 0 of the active lot while every story's maps are relit, and the GPU draws `light` there (GPU
// room lamps: an indoor lamp's edit skips the solves only when every room 0 listing it is covered so)
bool CoversLamp(const void* room0, uintptr_t light);
// Any thread: the law the GPU draws `light` with, in world space (the lamp classes 3, 4, 5 and 11 of lot `lot`, alive, a
// shade the law describes; a TubeLight 6 with a segment, GpuLampLaw::Tube, and the area lights 9 and 10, GpuLampLaw::Area, which only the kept rooms draw); false for any other light. regRoom = the room it is registered in (0 outdoors). For the
// developer's room lamp probe (OutdoorDelta::SetRoomProbe)
// lotXf (16 floats, LotXform): the law in that lot space instead (physical indoor lamps' shadow tiles)
bool WorldLaw(uintptr_t light, uint64_t lot, bool reachOn, GpuLampLaw::Lamp& out, int& type, int& regRoom, const float* lotXf = nullptr);
// Any thread: WorldLaw for the lamp's own lot (no reach), false for a light with no law
bool OwnLotLaw(uintptr_t light, GpuLampLaw::Lamp& out);
// Any thread: a story manager's lot space (its +0xE0 row-major 4x4: world row vector x it = lot), false when unreadable
bool LotXform(uintptr_t mgr, float out[16]);
// Render thread: the generation of the occluders the kept shadow tiles use (physical indoor lamps' open tops), 0 none
uint32_t KeptMeshGen();
bool RoomOpenAbove(int story, int room); // (render thread) the physical occluders opened that room's ceiling to the story above
// Any thread (physical indoor lamps, stage 5): the lot whose open-top build (RtLampProxy::Build(lot, mesh, true)) is the kept
// shadow tiles' occluders now, and its generation; null while none is uploaded
std::shared_ptr<const RtLampProxy::Lot> KeptLot(uint32_t& gen);
// Any thread (GPU room lamps): a lamp of lot `lot` WorldLaw gives no law (a tube without a segment) whose evaluation is its colour (+0xE0) times a share its
// colour does not change: a TubeLight (type 6, 0x006BFA70). colour: that colour now; geo: a signature of what the share reads
// (intensity, the tube's ends and length, range): an edit that changes it is not a colour edit. False for any other light
bool ColourLaw(uintptr_t light, uint64_t lot, float colour[3], uint64_t& geo);
// Any thread: the story tracker of the lot this path draws when `room` belongs to it (else 0; for the room lamp probe's
// occluders, LevelLightShare::ExportLotProxy)
uintptr_t TrackerOf(const void* room);
// Any thread: `room` is a room 0 of the active lot while every story's maps are relit (OutdoorDelta's lock step leaves the
// deltas of drawn lamps out then)
bool CoversRoom(const void* room);
// Any thread (physical indoor lamps, stage 4): CoversRoom, and every map of that room 0 was relit physically (its lot lamps with a law
// drawn from their laws by reach, OutdoorDelta::TakesLampEditPhys)
bool PhysCoversRoom0(const void* room0);
// Any thread: the active lot's tracker (the one whose occluders are wanted), 0 none
uintptr_t ActiveTracker();
// An edit of such a lamp (kind 0 a value or a switch, 1 a move; settle: a switch or an intensity change): timed until every
// story shows it; the rooms that take room 0's lamps are sent after the room update (with the physical model, only after a
// switch or an intensity change when every story draws the lamp from its law)
void NoteLampEdit(uintptr_t treeLevel, uintptr_t light, int kind, bool settle);
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
// Render thread: the outdoor scale of a bound relit copy (same size, A8R8G8B8; a blurred wall atlas's not blurred), else null. A room-0 texel's sum is packed with
// its hue kept past 1, divided by its largest channel m; the scale texture holds log2(m) / kScaleLog2 (0: within 1), so a
// draw that multiplies the copy's colour by 2^(kScaleLog2 x r) gets the sum back, up to 8 times. Texels of other rooms: 0.
IDirect3DTexture9* ScaleOf(const void* texture);
// A room's own map shown while its relit copy is not (the game wrote it again, a relight failed or is under way): the scale texture
// of its last relight when that marked texels of a space a room of the story below continues into (.y = 1; read with no outdoor
// scale: the draw sets the scale's constant to 0), so those walls and floors keep the indoor response with no sky between
// relights; null otherwise
IDirect3DTexture9* IndoorFlagsOf(const void* texture);
// The draw's inputs for those texels (ShaderPatches IndoorPick rooms: WallScalePatch / FloorPatch roomSampler) on a map bound
// to a draw: the UV map, each room's own wall map as the room's own draw reads it this frame (its relit copy when shown), its
// ambient (+0x110, the InteriorWall's c4) with the sky reflection's scale in .w, and whether the scale bound with it holds the lamps'
// fall-off ratios in .z (its relit copy's; the game's own map: none, x 1). False: not such a map, or a room's ambient or map not read
struct IndoorRooms {
    IDirect3DTexture9* uv = nullptr;
    IDirect3DTexture9* map[4] = {};
    float amb[4][4] = {};
    bool ratio = false;
};
bool IndoorRoomsOf(const void* texture, IndoorRooms& out);
constexpr float kIndoorReflection = 0.117f;
constexpr float kScaleLog2 = 3.0f;

// The menu: the last measured GPU time of a relight (ms), -1 while off or not measured
float GpuMs();
// The menu: why this device cannot run it (an English menu text, translated by the menu), empty when it can or before the
// device was read
std::string Unsupported();
// The menu: why it is on but draws nothing for a reason the player can fix or should know (an English menu text), else empty
std::string InactiveNote();
std::string Status();

} // namespace RtOutdoorLamps
