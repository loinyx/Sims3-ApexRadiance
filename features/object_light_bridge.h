#pragma once
// Street lamps light nearby outdoor objects (fences, bushes, props) like they light the ground (see object_light_bridge.cpp).
#include <cstdint>
#include <string>
#include <vector>
namespace ObjectLightBridge {
bool Install(std::string& error);
void Uninstall();
void SetStrength(float s);
void SetAllObjects(bool on); // also railings, stairs, columns (objects whose script never enabled lamp light); new rigs only
void OnPresent();   // render thread, every frame
std::string Status();
// Stock pink lamp colour -> warm white (0 = game colour, 1 = warm white), for street lamps and lot lamps. Applies to
// lights created afterwards; RetintLamps re-colours the stock lamps already there.
bool InstallLampColour();
void UninstallLampColour();
void SetLampTint(float street, float lot);
// Re-colours the stock lamps among `lights` (the game's light enumeration) that still have the colour this mod gave them,
// with the current tints; their object rigs gather again. Render thread. Returns how many changed.
int RetintLamps(const std::vector<uintptr_t>& lights);
// Object rigs gather their lights again at the next OnPresent (e.g. the moonlight changed)
void RequestRigRefresh();
bool RigRefreshPending(); // render thread: refresh request not yet dispatched
std::string LampColourStatus();
// The room a room-mode rig gathered its lights for (its last gather; null when not seen). Any thread.
const void* RigRoom(uintptr_t rig);
// (F7) the outdoor lamps a room-mode rig took through its room's openings at its last gather, with their share and whether
// the game kept them over its luminance cut; empty when none. Any thread.
std::string RigOpeningNote(uintptr_t rig);
}
