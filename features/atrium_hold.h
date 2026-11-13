#pragma once
// An atrium's stories change together (part of Faster room lighting, not an option).
//
// A double-height room is one room per story (LevelLightShare's stacked-ambient groups), each solved on its own. When a lamp
// of an atrium switched, the lamp's story changed first and its wall showed a step at the floor line against the story above
// for about a second, until that story's solve ended (on video, some corners looked right, then wrong again).
//
// The room solves write the story maps (wall atlas, floor and ceiling maps, room light map, basis maps: MANAGED single-level
// A8R8G8B8 textures, docs/engine/room-light-maps.md) through LockRect / UnlockRect; both are detoured. While a lamp edit is
// pending (LevelLightShare::LampEditPending or LampMarkFilter::MassSwitchActive), a map an atrium room's solve writes is kept
// (AddRef, two buffers):
//   - at the game's UnlockRect, while another room of the same atrium is still waiting for or in its solve
//     (LevelLightShare::GroupPending), what was on screen goes back and the new content waits; otherwise it stays as the
//     game wrote it;
//   - at the game's LockRect of a waiting map, the exact content is put back first, so the game never reads or builds on
//     what is shown (the wall blur reads the atlas);
//   - every frame (Present, render thread): the waiting maps of an atrium whose rooms are all solved, or that waited
//     kHoldMs, take their content in the same frame.
// A refinement (a quick-pass room solved again at its own class) writes the maps of another class, not the ones on screen:
// they never wait. A map the game holds locked is never written by Apex; a dragged lamp's rooms never wait. Buffers exist
// only while a map waits (and 3 s after). The 250 ms fade this module began with ("Smooth light changes indoors") was
// removed: the light changing at once looked better.
#include <d3d9.h>
#include <string>

// Lamp edits all at once ([patches.RoomLightQueue] switchAllAtOnce, default on; 12/10: every edit, was switches only):
// from a player's edit (switch, value, move, drag, a lamp added or removed) until every room it marked on another story than
// the lamp's own has its final light (at the latest 2.5 s after the latest edit, 4 s after the first; a drag: 2.5 s after it
// is let go), every map of those rooms waits the same way, and for a switch the furniture rigs of those stories
// (lot_light_bridge.cpp), the ground (LightmapSmooth) and the per-pixel lamp list (LotLightBridge) keep what was on screen;
// the quick pass is off meanwhile (its light would never be seen). Then the other stories change in the same frame. The
// lamp's own story is never held: it shows its new light as soon as it is solved (atrium_hold.cpp).
// 14/10, Classic: show all floors together (Tester switch, on; Classic lamp lighting only): the lamp's own story is held
// too, every edit holds the furniture, ground and per-pixel lamps as a switch does, and the hold ends once every room of the
// edit is solved, at the latest 6 s after the edit (a drag: after its release; atrium_hold.cpp).
namespace AtriumHold {
void SetAllAtOnce(bool on);
bool AllAtOnce();     // on, and the texture lock hooks are installed
bool UnlockHooked();  // the texture lock hooks are installed (the game's map writes are seen)
bool SwitchHolding(); // an edit hold covering a switch (or any edit, all floors together) is on now (any thread)
bool EditHolding();   // an edit hold is on now, or an edit happened this frame (any thread)
bool OwnStoryHeld();  // an edit hold is on that holds the lamp's own story too (show all floors together; any thread)
// 2.13.1 (Tester switch ownRoomAtOnce): after an edit hold, rooms not yet solved since their mark keep their maps until
// they are ("trailing"); true while that can happen (any thread)
bool Trailing();
// 2.13.1 (Tester switch furnitureWithRoom): this room's maps do not show the current lamp edit yet: held by the edit hold
// (its story held, an urgent room), trailing after it, or on the lamp's own story and not solved since its mark when that
// story is not held. Its furniture keeps its lights meanwhile (lot_light_bridge.cpp). Render thread
bool RoomShowsOldLight(const void* room);
// Render thread (Present): installs the texture lock hooks once (false: not available, logged), updates the switch hold,
// then releases the maps whose atrium is solved or whose switch is over
void OnPresent(IDirect3DDevice9* dev);
// Releases every kept map, the waiting ones with their content (device reset, uninstall)
void Clear();
std::string Status();
}
