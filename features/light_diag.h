#pragma once
// Light diagnostics used by Night Lighting (patches/night_terrain_relight_patch.cpp).
#include <filesystem>
#include <string>
namespace LightDiag {
bool Init();                    // validates game addresses; false if this game version does not match
void RequestDump();             // writes ApexRadiance_LightDiag.txt on the next frame
void OnPresent();               // call every frame from the render thread (handles Ctrl+Shift+F8 too)
void WriteInto(const std::filesystem::path& folder); // render thread: the snapshot now, as a file of that capture (no Finish of its own)
const std::string& Status();
}
