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
// Any thread (room_bounce): the part of a room's ambient (+0x110) its lamps gave, its base taken out; false: no lit room of ours (a lampless
// room holds the colour alone, and bounces nothing)
bool LampAmbient(const void* room, float out[3]);
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
// Such a light turned the way the walls are (Brightness and Blue tint); false: left as it was. chroma (luma 1, or null):
// in a lit room the hue of the room's own ambient (its walls' colour), taken instead of grey at the same brightness
bool FurnitureColour(float* rgb, const float* dir, const float* chroma = nullptr);
// Render thread, every frame: the game's night level (0 = day, 1 = night); the furniture part acts by it (none by day)
void SetNightLevel(float level);
// Render thread, per furniture draw (the guard, reset after the draw): the rig holds a [NoLight] light (IsDarkRoomLight),
// so its room is dark and the furniture part acts fully whatever the night level
void SetDrawDark(bool dark);
// Render thread: the object rigs gather once more in ms (after rooms were sent to light again: "Refresh the lighting")
void RigsAgainIn(unsigned ms);
bool RigRefreshPending(); // render thread: scheduled room-object refresh not yet dispatched
bool IsDarkRoomLight(const float* colour, const float* dir);
// On while LevelLightShare runs the game's ambient step on a room only to read its result (the room's fields are put back
// after): the base is added as usual, but not remembered as what the room holds. Per thread (a solve on another thread notes
// its base meanwhile); returns the thread's previous value, which the probe puts back
bool SetAmbientProbe(bool on);
// Light tree thread (physical indoor lamps, S8): a lit indoor room's ambient (+0x110, +0x120) taken again from its lamps now by
// the game's ambient step with Rooms at Night's base, without a solve; norm: the normalisation that step gives (the room keeps
// its own until its solve). False (why): left as its solve set it
bool RetakeAmbient(unsigned char* room, float& norm, const char*& why);
// Light tree thread (the instant day/night switch, OutdoorDelta): true when the room holds pair's second colour (+0x120) as its
// solve's ambient step gave it with Rooms at Night's base (its base record); ce: that base's colour; own: the record's own
// colour (+0x110 as that step gave it); shared: pair's own colour is another one (LevelLightShare wrote a share over it)
bool SolveBase(const unsigned char* room, const float (*pair)[4], float* ce, float* own, bool& shared);
// The room's ambient put back to a cached solve's pair, written as RetakeAmbient writes it (+0x110, +0x120; the base record takes
// own and the second colour): only while the room holds its record's values (RoomAmbientPolicy::SwapHolds; shown / shownOwn: the
// own colour its kept solve showed and that solve's record), is not being solved and the base's colour is still ce. False (why):
// unchanged. was / wasOwn (optional): the pair it showed and the record's own colour before
bool RestoreAmbient(unsigned char* room, const float (*pair)[4], const float* own, const float* ce, const char*& why, const float* shown = nullptr,
                    const float* shownOwn = nullptr, float (*was)[4] = nullptr, float* wasOwn = nullptr);
std::string Status();
std::string SettingsText(); // development tools (F6): the sliders now and what they give furniture
}
