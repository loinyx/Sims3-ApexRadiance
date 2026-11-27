#pragma once
// The game's -editInGameMode switch on the process's command line (apex_main.cpp, DllMain). The game reads it (any value
// starting with t or 1 is on) and then loads the save in its edit-in-game mode with time stopped (WorldManager mode 2).
// Apex Launcher's direct start of the EA app copy passes it to skip the Sims 3 launcher's window; Rename turns every
// "editInGameMode" into a key of the same length the game does not know, in place, when the start is Apex Launcher's:
//  - the -apexDirect marker (Apex Launcher 2.3.5 and later), or
//  - the value "true0", which only Apex Launcher 2.3.4 and earlier passed (without a marker), so a save opened by an older
//    launcher after Apex Radiance updated does not start paused either.
// Anything else keeps the switch (Create a World's own edit-in-game start).
#include <cstddef>

namespace EditInGameArg {
struct Scan {
    bool marked = false;     // -apexDirect on the line
    int found = 0;           // editInGameMode occurrences
    int renamed = 0;
    int legacy = 0;          // occurrences renamed for the old launcher's "true0" alone
    char value[12] = {};     // the first occurrence's value (ASCII, cut at a space or quote)
};

template <typename Char> Scan Rename(Char* line) {
    Scan s;
    if (!line) return s;
    constexpr char kFrom[] = "editInGameMode", kTo[] = "apexNoEditMode", kMark[] = "-apexDirect", kLegacy[] = "true0";
    const auto lower = [](Char c) { return c >= 'A' && c <= 'Z' ? Char(c - 'A' + 'a') : c; };
    const auto at = [&](const Char* p, const char* word) {
        for (; *word; ++p, ++word) if (lower(*p) != lower(Char(*word))) return false;
        return true;
    };
    for (const Char* p = line; *p && !s.marked; ++p) s.marked = at(p, kMark);
    for (Char* p = line; *p; ++p) {
        if (!at(p, kFrom)) continue;
        const Char* v = p + (sizeof kFrom - 1);
        if (*v == ':' || *v == '=') ++v;
        if (!s.found) {
            size_t n = 0;
            for (const Char* q = v; *q && *q != ' ' && *q != '"' && *q != '\t' && n + 1 < sizeof s.value; ++q)
                s.value[n++] = (*q >= 0x20 && *q < 0x7F) ? static_cast<char>(*q) : '?';
        }
        ++s.found;
        const bool legacy = at(v, kLegacy);
        if (!s.marked && !legacy) continue;
        for (int i = 0; kTo[i]; ++i) p[i] = Char(kTo[i]);
        ++s.renamed;
        if (!s.marked) ++s.legacy;
    }
    return s;
}
} // namespace EditInGameArg
