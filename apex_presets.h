#pragma once
// Built-in presets: Performance, Balanced and Quality, shipped with the mod (no file in the Profiles folder).
// They are offered on the first menu open (the welcome page, [ui] start_profile_done) and in Settings > Profiles >
// Saved profiles, where they cannot be deleted. Each one is a profile table like the files ApexConfig::SaveProfile
// writes (feature parts only: never shortcuts or developer settings); applying goes through ApexConfig::ApplyFeatureState.
// Every Lighting page switch (Lamps, Ground, Objects, Buildings, Stories) is on in all three. Values: the tuned profiles of 2.5.6. Water & Snow and the Banding Fix (with the High-precision scene) are not part of
// them: applying a built-in profile leaves those settings as they are. Every Performance switch they hold is on, though applying keeps only the switches with a profile part (ApexConfig::KeepProfileParts drops the others, which stay as they are); Lot Streaming is not part of them either (off by default). Keys a later version does not know are ignored.
#include <toml++/toml.hpp>
#include <string>

namespace ApexPresets {

struct Preset {
    const char* name;        // English, for the menu and the log ("Performance")
    const char* description; // English, one line
    const char* icon;        // Lucide name (ApexUi::IconFromName)
    const char* toml;        // the profile table
};

inline constexpr int kCount = 3;
inline constexpr int kDefault = 1; // Balanced (listed second)

const Preset& Get(int index); // 0 = Performance, 1 = Balanced, 2 = Quality
bool Read(int index, toml::table& out, std::string* error = nullptr);

} // namespace ApexPresets
