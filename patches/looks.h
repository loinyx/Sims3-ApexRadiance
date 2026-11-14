#pragma once
// Looks (patches/looks_patch.cpp): the feature's entry points for the menu and the captures.
#include <string>

namespace Looks {

inline constexpr const char* kPatchName = "Looks"; // [patches.Looks]

// The preset file in use (for %PresetName% and "Save the look's preset too"); false while the look is off. file is
// empty for a built-in Apex look.
bool PresetInUse(std::wstring& file);
// The Looks card's status line, translated: "<look name> · <n> shaders", or "Off"
std::string StatusLine();
// Skips the look for the next 0.15 s (hold to compare); call it every frame while held
void HoldBypass();
// The Shaders card under the ReShade look card (Image > Color > Looks)
void ShadersCard();

} // namespace Looks
