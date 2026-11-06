#pragma once
// Records a few seconds of what the lighting does, with the clock time on every line (its shortcut starts, again asks to
// stop: it saves 8 s later, once more saves at once; it stops by itself after 60 s). Writes Captures\<date time> Recording\Recording.txt (features/captures.h). Both builds.
#include "build_flavor.h"
#include <cstdint>
#include <string>
#include <vector>
namespace Recorder {
void OnPresent(); // render thread, every frame (Night Lighting's Present)
// For the on-screen note: the seconds recorded so far (-1 = not recording), and the file just saved ("" after 4 s)
int SecondsRecorded();
void RequestToggle(); // any thread: start, stop or end the wait of a recording at the next frame, as its shortcut does
void RequestStop(); // idempotent: stop and save at once, only if recording
void RequestCancel(); // stop without creating a capture; an idle cancel never starts recording
bool Settling(); // the stop key was pressed: the recording goes on for the light to settle, then saves
// Render thread: whether a recording runs, and a line added to it with the time now (the furniture tracer, the light probe)
bool Active();
void Note(const std::string& text);
// Any thread: a lamp the player edited (switched, moved or changed) while a recording runs, for its light update summary;
// treeLevel = the story tree level whose room marks showed the change first (not always the lamp's own story)
void NoteLampEdit(uintptr_t treeLevel, bool on, bool moved);
// Render thread (LightmapSmooth): a terrain chunk's smoothed light map shows new light from this frame; together = a lamp
// switch's held ground, released with the rest of its batch
void NoteGround(int ix, int iz, bool together);
// A light probe's cube was written. mean = its 6 faces' mean colours; the last one of each probe is kept, with strip = its 6
// faces side by side, B G R rows of size * 6 pixels (developer mode; else empty)
void NoteCube(uintptr_t probe, const char* what, const float mean[6][3], unsigned size, std::vector<unsigned char> strip = {});
// Render thread: something ran once more (the [ran] lines); what = a string literal
void Count(const char* what);
// The detailed lighting log lines are written: always in the development build, and while a recording runs in the public one
inline bool Verbose() { return !kPublicBuild || Active(); }
}
