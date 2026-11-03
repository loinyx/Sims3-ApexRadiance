#pragma once
// Records a few seconds of what the lighting does, with the clock time on every line (its shortcut starts, again stops;
// it stops by itself after 60 s). Writes Captures\<date time> Recording\Recording.txt (features/captures.h). Both builds.
#include "build_flavor.h"
#include <cstdint>
#include <string>
namespace Recorder {
void OnPresent(); // render thread, every frame (Night Lighting's Present)
// For the on-screen note: the seconds recorded so far (-1 = not recording), and the file just saved ("" after 4 s)
int SecondsRecorded();
void RequestToggle(); // any thread: start (or stop) a recording at the next frame, as its shortcut does
void RequestStop(); // idempotent: stop and save only if recording
void RequestCancel(); // stop without creating a capture; an idle cancel never starts recording
const char* JustSaved();
// Render thread: whether a recording runs, and a line added to it with the time now (the furniture tracer, the light probe)
bool Active();
void Note(const std::string& text);
// Any thread: a lamp the player edited (switched, moved or changed) while a recording runs, for its light update summary;
// treeLevel = the story tree level whose room marks showed the change first (not always the lamp's own story)
void NoteLampEdit(uintptr_t treeLevel, bool on, bool moved);
// Render thread (LightmapSmooth): a terrain chunk's smoothed light map shows new light from this frame; together = a lamp
// switch's held ground, released with the rest of its batch
void NoteGround(int ix, int iz, bool together);
// The detailed lighting log lines are written: always in the development build, and while a recording runs in the public one
inline bool Verbose() { return !kPublicBuild || Active(); }
}
