#pragma once
// Which light rig lit the object being drawn right now (see rig_tracker.cpp).
#include <cstdint>
namespace RigTracker {
bool Install();
void Uninstall();
bool IsInstalled();
// Mode of the rig bound for the current draw (rig+0x1D4: 0 room, 1 roofless room, 2 outdoor), -1 when unknown.
// Only meaningful inside a SceneModel part draw, on the thread that draws.
int CurrentMode();
// Development tools: the rig bound for the object being drawn now (0 = none): one per object, so it names the object
uintptr_t CurrentRig();
// Called at each rig bind (before the game's binder, on the drawing thread) with a rig whose vtable checked; null: none.
// One hook at a time (real-time GI's rig write, S2-06 item 8)
using BindHook = void (*)(uintptr_t rig);
void SetBindHook(BindHook hook);
}
