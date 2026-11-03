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
//   - A lamp moved since a room's parts were kept: drawn there with the law its parts were kept with until a copy of the room
//     with its parts at its new place is kept (OutdoorDelta's move worker) or the room is solved.
//   - A value or switch edit of a lamp every room listing it draws here marks no room (OutdoorDelta::TakesLampEdit): it shows
//     in the frame, logged once every room of it was relit.
//   - With the room lamp probe on, each kept room's map is read back once after its first relight and compared with the texels
//     the game left there (the log).
// Render thread only. Nothing runs while the option is off (Sig returns 0 and nothing is drawn).
#include <d3d9.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace RtLampGpu {
class Engine;
struct Params;
}
namespace RtLampModel {
struct Lamp;
}

namespace GpuRoomLamps {

// The kept rooms of story manager mgr at class cls drawn in map m (0 wall atlas, 1 floor, 2 ceiling, 3 objects) and their
// lamps as they are now (0: none); builds their GPU buffers on first use
uint64_t Sig(IDirect3DDevice9* dev, uintptr_t mgr, int cls, int m);
// After Real-time outdoor lamps composed that map's copy (out, w x h; between the engine's Begin and End): the kept rooms over
// it. A room that cannot be drawn is logged and left as the game's. p: room 0's relight rules (physical rooms take its softness,
// fade and drag taps). partial (a rooms-only copy kept from its last draw, the game's map and the rules unchanged): only the rooms
// whose draw inputs changed are drawn again; false when that cannot give what a full draw would (the caller then copies the
// game's map and draws in full)
bool ComposeOver(IDirect3DDevice9* dev, RtLampGpu::Engine& eng, uintptr_t mgr, int cls, int m, IDirect3DBaseTexture9* game, IDirect3DTexture9* out, int w, int h,
                 const RtLampGpu::Params& p, bool partial = false);
// (sharper floors, after ComposeOver into copy) hi (2w x 2h) = copy upsampled, each physical room's floor sub-texels over it from
// over (2w x 2h, kept between relights: a room's sub-texels are drawn into it again when its draw changed, all of them when
// redrawAll). False: hi is not complete or holds no room's (the caller shows the copy)
bool ComposeHi(IDirect3DDevice9* dev, RtLampGpu::Engine& eng, uintptr_t mgr, int cls, int m, IDirect3DTexture9* copy, int w, int h,
               IDirect3DTexture9* over, bool redrawAll, IDirect3DTexture9* hi, const RtLampGpu::Params& p);
// A device reset or Real-time outdoor lamps dropping its maps: every GPU buffer goes (made again on use)
void ReleaseAll();
// (physical indoor lamps; render thread, between the engine's Begin and End) lamps[i] (lot space) of lights[i] in passes of the
// kept shadow atlas's rows, which kept rooms and room 0 share: each pass gets its lamps' tiles (rendered unless the atlas holds
// them, least recently used first) and last = the final pass. False (err: no occluders with open tops, too many lamps, a tile
// not rendered): stopped there
bool PhysPasses(IDirect3DDevice9* dev, RtLampGpu::Engine& eng, const uintptr_t* lights, const RtLampModel::Lamp* lamps, int n, float farDist, std::string& err,
                const std::function<bool(int first, int count, const int* tiles, bool last)>& pass);
// (physical indoor lamps) how a story's room 0 was drawn this round (Real-time outdoor lamps), for the per-story line BeginFrame logs
void NoteRoom0(int story, const std::string& how);
// Render thread: Real-time outdoor lamps starts a relight round (the frame count the edit log gives per map kind)
void BeginFrame();
// Any thread (the light tree thread's lamp edits, OutdoorDelta::TakesLampEdit): that kept solve of the room (its version) was
// drawn on every map of `maps` (bit m) and is still (no lamp of it moved since)
bool Drawn(const void* room, uint32_t version, unsigned maps);
// Any thread (physical model): every map of `maps` has been drawn on the GPU since it was last dropped for a reason other than
// its room kept again, at any version. A physical relight draws every lamp from its law, so a solve kept again but not relit yet
// shows the lamp next frame
bool DrawnAny(const void* room, unsigned maps);
// Any thread: a lamp edit the GPU took (kind "value", "switch on" or "switch off": a literal), logged once every one of its
// rooms was relit after it
void NoteEdit(uintptr_t light, const char* kind, const std::vector<const void*>& rooms, const std::string& stories);
// Any thread: an edit of that lamp taken and not yet shown in every map of its rooms holding its parts (its maps go first)
bool EditPending(uintptr_t light);
// Render thread, after Sig: map m of that story manager and class draws a room of an edit taken that holds the lamp's parts and
// was not relit since the edit (Real-time outdoor lamps relights such maps first, the floor first); (physical) a map whose drawn
// lamps held or hold the lamp of a physical edit, not relit since
bool ShowsEdit(uintptr_t mgr, int cls, int m);
// ---- physical indoor lamps, stage 4: lamp edits without the room solves (OutdoorDelta::TakesLampEditPhys) ----
// A physical kept map draws every lamp of the active lot with a GPU law whose reach meets its texels (OutdoorDelta::LotLamps,
// phys_lamp_pick.h), so an edit relights exactly the maps its old or new reach meets.
// Light tree thread: an edit taken ("value", "switch on", "switch off", "move"; a literal), logged once its maps were relit:
// "physical <kind>: lamp L, no room solved, M maps relit in N ms (frames +k)" (a drag's steps are one edit, timed from its latest)
void NotePhysEdit(uintptr_t light, const char* kind);
// Any thread: a physical move taken within the last ms (Real-time outdoor lamps draws its maps with the drag's taps meanwhile)
bool PhysMoveRecent(unsigned long ms);
// Render thread: the physical relights since the last call (maps, CPU ms, the largest round's CPU ms; OutdoorDelta's 10 s summary)
void TakeRelightCost(int& maps, double& ms, double& worst);

} // namespace GpuRoomLamps
