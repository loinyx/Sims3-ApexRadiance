#pragma once
// Who sends rooms to the solve (development builds, switch room_solve_causes; measures only).
//
// A room is solved again for one of two reasons (docs/engine/room-light-maps.md): an invalidate (0x0069EED0, or 0x0069F160
// on a flag change), counted here by its caller's return address, or the class refinement the end of a solve starts
// (0x0069EA70: a class below LodChoice goes back to state 2), seen as several finished solves of the same room. Each
// finished solve (FUN_006a0e00) is counted by class and by room. An invalidate made by Apex's QueueRoom is counted by
// QueueRoom's own caller, with the time since the same room's previous invalidate (any caller) and how many were a story's
// room 0 (the outdoor room the game's room-0 cascade at 0x006C7250 restarts on every story). Every count is split by the camera: settled (still for 10 s or more), stopped (still for less:
// the rooms of a move still settling) or moving (it moved in the last 300 ms; LotLightingMotion::SampleCameraMoving,
// sampled once a frame). Once a minute the window is written to the log
// as one [RoomSolveCauses] line and started again. Nothing here changes what the game solves or shows.
//
// Part of Apex Radiance. Credits: @loinyx
#include <cstdint>
#include <string>

namespace RoomSolveCauses {

// Any thread (the light tree thread and the render thread solve rooms)
// room / roomId / nowMs: the invalidated room (its address and id) and the time, for the gap since its previous invalidate
void NoteInvalidate(uintptr_t caller, bool onFlagChange, bool viaQueueRoom, uintptr_t room, int roomId, uint64_t nowMs);
void NoteSolve(uintptr_t mgr, int id, int level, uint32_t lot, int cls);

// Render thread, once a frame: the camera state for the counts until the next call. Returns true with the window's line in
// *line once windowMs have passed since the window started (the counts start again).
bool Frame(bool cameraMoving, uint64_t nowMs, std::string* line, uint64_t windowMs = 60000);

// The address as the log prints it: "ApexRadiance.asi+0x1234" inside this module, else the absolute address
std::string CallerText(uintptr_t caller);

} // namespace RoomSolveCauses
