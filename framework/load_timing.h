#pragma once
#include <string>
// Load-phase timing for the log ([LoadTiming], 07/10): logging only, no behaviour change. Hooks and features note events
// with one QueryPerformanceCounter read and an atomic store (no lock, no log line on their thread); the start-up pump thread
// (10 ms) turns them into log lines and takes the counter snapshots of the load window:
//   process start -> game device created -> features started;
//   world load start (the first of: the object map indexed, the world change seen by Night Lighting) -> first world draw
//   (the world terrain drawn, end of the load screen) -> load settled; numbered for every load of the session;
//   during the load window: FastRefPack, FastDxt, ResourceCache / FileListCache, room-light solves, shader precompile waits.
// The save or world name is not logged: no existing hook reads it.

namespace LoadTiming {
// Cumulative work counters, read by diagnostic reports on the initialization thread.
std::string DiagnosticWork();

void NoteDeviceCreated();          // any thread, first call counts
void NoteFeaturesStarted();        // init thread
void NoteObjectMapIndexed();       // any thread (inside the game's lookup: atomics only)
void NoteWorldChange();            // render thread: Night Lighting saw a new world
void NoteFirstWorldDraw(const char* signal); // render thread: the world went live (signal: a string literal)
void NoteLoadSettled(const char* why);       // any thread: Night Lighting's "Load settled" (why: a string literal)
void NoteRoomSolveStart();         // render thread: the game began a room's light solve
void NoteRoomSolveEnd();           // a room's solve ended (its maps shown from the next frame)

void Pump();                       // start-up pump thread: logs the phases

} // namespace LoadTiming
