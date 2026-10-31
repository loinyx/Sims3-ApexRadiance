// Developer Tester tab: runtime switches for the parts ported onto 2.12.3 + Real-time outdoor lamps, so their frame cost
// and look can be compared in game without another build. Every switch defaults to the normal build's behaviour (on);
// the Tester tab (apex_gui.cpp, DevTesterTab; tester_settings.h) shows them, saves them in [developer.controls.tester]
// and restores them.
// Player-facing options of the same features keep their own settings; the tab shows those too, writing the same settings
// (Outdoor / Indoor light through openings, Doors block light, Smooth light maps, Lamp lighting). The switches below are
// either always on for players (fixes, drag measures, caches) or sub-parts of a player option, on by default, that only
// switch a part off inside it (docs/features/developer-mode.md, Tester):
//   groundShade, groundSoftEdges  -> "Walls shade lamp light near the ground" (paredesSombreiamOChao)
//   rigBuildingBlock              -> "Buildings block lamp light on trees and objects" (paredesBloqueiamLuzNosObjetos)
//   roofedFromBelow               -> "Outdoor light through openings" (luzDeForaPelasAberturas)
//   smoothCut, smoothDither, smoothBSpline -> "Smooth light maps" (mapasDosComodosSuaves)
//   Penumbra::g_on                -> "Shadow softness" (suavidadeDasSombras)
#pragma once
#include <atomic>

namespace TesterToggles {
// Fixes
inline std::atomic<bool> disabledLampFix{true};  // a lit lamp a player disabled gives no rig boost to objects (126b297)
inline std::atomic<bool> foliageDayFix{true};    // outdoor lamps on foliage take the daytime curve (413f0bb)
inline std::atomic<bool> colonnadeFix{true};     // grid walls ending at a floor above are capped there (CapLowerWalls, 940989f)
// Features (sub-parts of player options, see above)
inline std::atomic<bool> groundShade{true};      // walls shade lamp light on lot ground, decks, curbs, stairs and low fences
inline std::atomic<bool> rigBuildingBlock{true}; // buildings block outdoor lamps on trees, bushes and rig objects
inline std::atomic<bool> roofedFromBelow{true};  // roofed rooms take the garden posts of the story below
// Classic lamp lighting (14/10; Instant (GPU) is unchanged by both):
inline std::atomic<bool> classicAfterRelease{true}; // a dragged lamp's rooms are solved once, after the lamp is let go (DragLite)
inline std::atomic<bool> classicTogether{true};     // after a lamp edit every story, the lamp's own too, changes in one frame (AtriumHold)
// Drag safety
inline std::atomic<bool> dragDeferSends{true};   // Apex's extra room sends wait while a lamp is dragged, sent once at the stop
inline std::atomic<bool> dragRepickThrottle{true}; // the room priority call holds rooms re-solved too soon while dragging
inline std::atomic<bool> dragSpreadCap{true};    // the stop's sends are spread under 3.5 ms a frame
inline std::atomic<bool> dragTerrainIdle{true};  // the game's terrain countdown a drag arms is left idle at night
inline std::atomic<bool> dragSlowRefresh{true};  // while dragged: lamp list re-reads and local terrain relights every 250 ms
// 2.13.1: after any lamp edit the lamp's own rooms (urgency tier 0) take the normal edit budget, never the spread cap; only
// the extra sends (neighbours, openings, dependents) are spread. With Lamp lighting Instant, a dragged lamp the game solves
// (an indoor lamp) follows the Classic drag rules; a colour, intensity or switch is never taken for a drag (DragLite)
inline std::atomic<bool> fastOwnRelight{true};
// 2.13.1: the mouse button letting go of a dragged lamp ends the drag at once, without the 400 ms still time (DragLite)
inline std::atomic<bool> dragLetGo{true};
// 2.13.1: all floors together shows the lamp's own rooms as soon as they are solved (was up to 300 ms later); the rooms
// around them keep their maps until each one's own solve ends (AtriumHold)
inline std::atomic<bool> ownRoomAtOnce{true};
// 2.13.1: after any lamp edit (moves and colours too, not only switches) the furniture keeps its lights until its room's
// maps show the new light (lot_light_bridge.cpp, HoldRig)
inline std::atomic<bool> furnitureWithRoom{true};
// Soft light (2.12.3-rt-plus2; the player options Shadow softness and Smooth light maps keep their own settings: these
// switch each part off within them)
inline std::atomic<bool> smoothCut{true};       // lamps fade out at the room light cut (LevelLightShare::SetSoftLightCut)
inline std::atomic<bool> smoothDither{true};    // room light map texels dithered before the pack (RoomMapDither)
inline std::atomic<bool> smoothBSpline{true};   // room-lit shaders read the room light map as a B-spline (LotLightBridge)
inline std::atomic<bool> groundSoftEdges{true}; // the lot ground shade takes the lamp's penumbra at wall ends and tops (GroundShadow)
inline std::atomic<bool> groundShadeGpu{true};  // the lot ground shade and the world grass's are built on the GPU (GroundShadeGpu), off: on the worker thread
inline std::atomic<bool> openingLampCut{true};  // a lamp through an opening stays on an indoor object under the game's cut (ObjectLightBridge)
// Performance
inline std::atomic<bool> gridMemo{true};         // a story's grid asked again in the same frame is not looked up again

} // namespace TesterToggles
