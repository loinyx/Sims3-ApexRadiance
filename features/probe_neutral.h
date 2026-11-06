#pragma once
// Lamp colours stay near lamps (one coloured wall lamp tinted every outdoor floor of the lot, the closed roofless
// rooms and a glossy rug in a far room with its colour). The game's light probes (the exterior one at lightMgr+0x64 and the room
// probes) render the scene into a cube, read it back and write on the CPU a diffuse cube (+0x2C50) and a 7-mip specular cube
// (+0x2C5C) that outdoor floors, roofs, sims and glossy objects sample as their sky light (0x006B2EB0, 0x006B42C0). At night
// the capture is black except the lamps, so one coloured lamp tints everything that samples it. Right after the game writes a
// probe's new cubes (0x006B4720 diffuse, 0x006B4B10 specular) each texel's rgb goes towards its luma by the night level:
// brightness kept (never brighter), hue gone; nothing by day.
#include <string>
namespace ProbeNeutral {
bool Install(std::string& error); // checks and hooks the two writers (no texel changes until Set turns it on)
void Uninstall();
void Set(bool on, float strength); // render thread, every frame; strength 0..1
void SetNightLevel(float level);   // render thread, every frame
long Writes();                     // the cube writes so far, by day too
void NoteArmed(const void* probe); // any thread: Apex just made this probe due (the reflection line of its next capture says so)
std::string Status();
}
