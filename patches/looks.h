#pragma once
// Looks (patches/looks_patch.cpp): the feature's entry points for the menu and the captures.
#include <string>
#include <vector>

namespace Looks {

inline constexpr const char* kPatchName = "Looks"; // [patches.Looks]

// The preset file in use (for %PresetName% and "Save the look's preset too"); false while the look is off. file is
// empty for a built-in Apex look.
bool PresetInUse(std::wstring& file);
// The name the menu shows for the chosen look, translated for a built-in one ("" when none is chosen)
std::string LookName();
// The look card's subtitle, translated: "<n> shaders" (then " · <cost>" when cost is given), or "Off", "No look chosen",
// "This look can't be read"
std::string StatusLine(const char* cost = nullptr);
// Skips the look for the next 0.15 s (hold to compare); call it every frame while held
void HoldBypass();
// Image > Shaders, tab Look: the import note card, then the Shaders in this look card
void ShadersCard();
// Image > Shaders, tab Library: Your library, then Get shader packs
void LibraryCards();
// A button asked for the Get shader packs card: the Shaders page opens its Library tab (the card scrolls into view)
bool WantsLibrary();
// The note card offering the packs an imported look or preset misses (draws nothing when there is none)
void OfferCard();
// After a preset import: offers the packs it names (pack names or folders) that are on ReShade's list and not downloaded
void OfferPresetPacks(const std::string& preset, const std::vector<std::string>& packNames);
// The packs of a preset's "packs" list (names or folders) that are on ReShade's list and not downloaded, as folders;
// sentence: "This preset needs <packs> (<size>)" for the import window
std::vector<std::string> MissingPresetPacks(const std::vector<std::string>& packNames, std::string* sentence = nullptr);
// Starts those downloads (progress in Shaders > Library > Get shader packs)
void DownloadPresetPacks(const std::vector<std::string>& folders);
// The names of the packs whose shaders the look's run list uses (downloaded or not), for a preset's [patches.Looks] packs
std::vector<std::string> NeededPacks();
// A shader that doubles an Apex effect (MXAO and Ambient Occlusion): the menu answers for each switched-on shader with
// its row's amber line and the label of the button that turns the Apex effect off; act = true turns it off
struct Twin { std::string note, action; };
using TwinFn = bool (*)(const std::string& technique, Twin& out, bool act);
void SetTwinCheck(TwinFn fn);
// Rows the menu adds to the look card before its Advanced (an amber row while a LUT is on too)
void SetCardRows(void (*fn)());

} // namespace Looks
