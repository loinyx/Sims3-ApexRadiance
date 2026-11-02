#pragma once
// GPU room lamps, stage 2b (developer, "Keep indoor lamps for GPU" with Lamp lighting: Instant;
// docs/features/night-lighting/gpu-room-lamps.md): the kept room solves of the active lot (OutdoorDelta's kept captures) drawn
// on the GPU over Real-time outdoor lamps' copy of each story map, with the lot lamps as they are now.
//   - Real-time outdoor lamps relights a story map (its copy: the game's map, room 0's texels over it) when its signature
//     changes; Sig adds the kept rooms of that map (their versions) and their lamps' laws, so a lamp edit relights it in the
//     frame. After room 0, ComposeOver draws each kept room over the same copy (rt_lamp_gpu.h RelightKept), which the one
//     SetTexture swap then binds: the two paths share one copy per map.
//   - A room whose texels the game writes again (its finalize, any thread) leaves the kept rooms until its next kept solve
//     (OutdoorDelta::RoomFinalized): the copy shows the game's texels meanwhile.
//   - A lamp moved since a room's parts were kept: that room is left as the game's until its solve is kept again (a move
//     takes the game's path, its rooms are solved).
//   - A value or switch edit of a lamp every room listing it draws here marks no room (OutdoorDelta::TakesLampEdit): it shows
//     in the frame, logged once every room of it was relit.
//   - With the room lamp probe on, each kept room's map is read back once after its first relight and compared with the texels
//     the game left there (the log).
// Render thread only. Nothing runs while the option is off (Sig returns 0 and nothing is drawn).
#include <d3d9.h>
#include <cstdint>
#include <string>
#include <vector>

namespace RtLampGpu {
class Engine;
}

namespace GpuRoomLamps {

// The kept rooms of story manager mgr at class cls drawn in map m (0 wall atlas, 1 floor, 2 ceiling, 3 objects) and their
// lamps as they are now (0: none); builds their GPU buffers on first use
uint64_t Sig(IDirect3DDevice9* dev, uintptr_t mgr, int cls, int m);
// After Real-time outdoor lamps composed that map's copy (out, w x h; between the engine's Begin and End): the kept rooms over
// it. A room that cannot be drawn is logged and left as the game's
void ComposeOver(IDirect3DDevice9* dev, RtLampGpu::Engine& eng, uintptr_t mgr, int cls, int m, IDirect3DBaseTexture9* game, IDirect3DTexture9* out, int w, int h);
// A device reset or Real-time outdoor lamps dropping its maps: every GPU buffer goes (made again on use)
void ReleaseAll();
// Any thread (the light tree thread's lamp edits, OutdoorDelta::TakesLampEdit): that kept solve of the room (its version) was
// drawn on every map of `maps` (bit m) and is still (no lamp of it moved since)
bool Drawn(const void* room, uint32_t version, unsigned maps);
// Any thread: a lamp edit the GPU took (kind "value", "switch on" or "switch off": a literal), logged once every one of its
// rooms was relit after it
void NoteEdit(uintptr_t light, const char* kind, const std::vector<const void*>& rooms, const std::string& stories);

} // namespace GpuRoomLamps
