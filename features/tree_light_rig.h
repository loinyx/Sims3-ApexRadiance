#pragma once
// Trees out of the underground rig (Apex Radiance; part of Night Lighting).
//
// A SpeedTree cluster takes a fixed custom light rig instead of the gathered one when its key cluster+0x1F0 is set. The
// game's level setter (vfunc +0x50, 0x0070D370 on Steam 1.67.2) and its custom-rig setter (vfunc +0x6C, 0x0070D800) set
// that key to the script's explicit rig (+0x53C) when one is set, else to 0 for a level >= 0 and to 0x08D11E8C, FNV-1 of
// "undergroundtree", for a level below 0 (+0x1B8), and flag the cluster (+0x536) so its next update (0x0070EA30) applies
// the key to every tree's rig (0x006B8AC0). CustomLightRigging.ini's [UndergroundTree] is four fixed white lights at 0.5:
// such a rig never gathers (no moon, no lamps), and a tree whose cluster the game gives a level below 0 (on a lot with a
// basement, a tree whose root stands under floor 0) is drawn flat and bright white all night, and again whenever the game
// sets its level. After each setter, a cluster left on that key with no script rig gets key 0, the normal rig, applied by
// the same flagged update. Trees really placed in a basement then take their room's gathered light too.
#include <string>

namespace TreeLightRig {

bool Install(std::string* error);
void Uninstall();
bool Installed();
std::string StatusText();

} // namespace TreeLightRig
