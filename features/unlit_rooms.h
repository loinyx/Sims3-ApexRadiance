#pragma once
// Rooms with every lamp off: the light the game leaves in them (its blue glow) and the fill light it adds to furniture,
// under the player's control (part of Night Lighting; see unlit_rooms.cpp).
#include <string>
#include "s3ss_detect.h"
namespace UnlitRooms {
bool Install(std::string& error); // finds and checks the game code (nothing is patched until Set turns it on)
void Uninstall();                 // the game's own colours again
// Render thread, every frame: on = Apex's colours; light = how much of the game's unlit-room light stays (0..1, walls and
// furniture alike), blue = how much of its blue tint (0 = grey; furniture too)
void Set(bool on, float light, float blue);
void OnPresent(); // render thread: relights the rooms and objects a moment after a change
void OnWorldChanged(); // render thread: discard room/lot identities from the previous world
void OnRoomsChanged(); // render thread: a loaded lot or story manager changed
void OnRoomChanged(uintptr_t room); // render thread: structure changed; address is a cache key, never dereferenced
// Any thread: the weight of the ambient cube on furniture drawn by Apex's indoor-object shader (lot_light_bridge): the
// Brightness (blended by the night level) while on, 1 while off (their lamp light comes from the room maps, not this)
float FurnitureAmbient();
// Any thread: whether Rooms at Night changes furniture now (on, at night or in a dark room)
bool FurnitureActive();
// How much of the ambient cube's own colour stays on furniture: 1 = all (off), lower = greyer (the Blue tint x night)
float FurnitureTint();
// Render thread: the colour the (grey) ambient cube of furniture is multiplied by: the chroma of the game's unlit-room blue by the
// Blue tint (luma 1; 1, 1, 1 at 0% or while Rooms at Night does not act)
void FurnitureCubeColour(float* rgb);
// A room-mode rig light that belongs to the unlit-room light, not a lamp: the fill light (colour.w > 0) or one of the
// three [NoLight] lights (by direction dir: PS c0..c3 for colours c4..c7, VS vl-4.. for the vertex lights). No direction:
// false. Room-mode rigs have no sun: slot 0 is the strongest room light.
bool IsUnlitLight(const float* colour, const float* dir);
// Such a light turned the way the walls are (Brightness and Blue tint); false: left as it was
bool FurnitureColour(float* rgb, const float* dir);
// Render thread, every frame: the game's night level (0 = day, 1 = night); the furniture part acts by it (none by day)
void SetNightLevel(float level);
// Render thread, per furniture draw (the guard, reset after the draw): the rig holds a [NoLight] light (IsDarkRoomLight),
// so its room is dark and the furniture part acts fully whatever the night level
void SetDrawDark(bool dark);
// Render thread: the object rigs gather once more in ms (after rooms were sent to light again: "Refresh the lighting")
void RigsAgainIn(unsigned ms);
bool IsDarkRoomLight(const float* colour, const float* dir);
// Light tree thread: on while LevelLightShare runs the game's ambient step on a room only to read its result (the room's
// fields are put back after): the base is added as usual, but not remembered as what the room holds
void SetAmbientProbe(bool on);
std::string Status();
std::string SettingsText(); // development tools (F6): the sliders now and what they give furniture
}
