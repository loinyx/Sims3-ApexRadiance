#pragma once
// What's new: the short player-facing changelog shown when the version at the bottom of the menu is clicked.
// The release process (docs/release-process.md) adds the new version at the top of apex_changelog.cpp,
// written from docs/releases/<version>.md, and adds the lines to i18n/tr_menu.cpp. Newest first.

namespace ApexChangelog {

struct Release {
    const char* version;          // "2.6.0"
    const char* date;             // "2026-10-05"; empty while unreleased
    const char* const* added;     // nullptr-terminated English lines (translated with I18n::Tr when drawn)
    const char* const* improved;
    const char* const* fixed;
    // (optional, newest versions) one sentence on what the player will notice; replaces the counts on a closed version
    const char* headline = nullptr;           // with it, the first menu after updating shows a small card (UpdateCard)
    const char* const* highlights = nullptr; // up to 4 short titles, nullptr-terminated: the open version's first lines
    const char* settingsNote = nullptr;      // what this version moved in saved settings, on the update card
};

int Count();
const Release& Get(int index); // 0 = the newest

} // namespace ApexChangelog
