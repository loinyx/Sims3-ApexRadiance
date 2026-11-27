// EditInGameArg::Rename offline checks (framework/edit_in_game_arg.h): which command lines lose -editInGameMode.
#include "../../framework/edit_in_game_arg.h"
#include <cstdio>
#include <cstring>
#include <cwchar>

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

int main() {
    { // Apex Launcher 2.3.5+: marker, renamed
        char l[] = "TS3.exe -worldFile:West Arbor.sims3\" -editInGameMode:true -apexDirect";
        const auto s = EditInGameArg::Rename(l);
        Check(s.marked && s.found == 1 && s.renamed == 1 && s.legacy == 0 && !std::strcmp(s.value, "true"), "marked start renamed");
        Check(std::strstr(l, "-apexNoEditMode:true") && !std::strstr(l, "editInGameMode"), "marked: key replaced, same length");
    }
    { // Apex Launcher 2.3.4 and earlier: "true0", no marker
        char l[] = "TS3.exe -worldFile:Save.sims3\" -editInGameMode:true0 -test";
        const auto s = EditInGameArg::Rename(l);
        Check(!s.marked && s.renamed == 1 && s.legacy == 1 && !std::strcmp(s.value, "true0"), "old launcher value renamed");
    }
    { // Create a World style: "true", no marker, kept
        char l[] = "TS3W.exe -editInGameMode:true -worldFile:MyWorld.world";
        const auto s = EditInGameArg::Rename(l);
        Check(s.found == 1 && s.renamed == 0 && std::strstr(l, "-editInGameMode:true "), "unmarked true kept");
    }
    { // other values kept; case-insensitive key
        char l[] = "TS3.exe -EDITINGAMEMODE:1";
        const auto s = EditInGameArg::Rename(l);
        Check(s.found == 1 && s.renamed == 0 && !std::strcmp(s.value, "1"), "unmarked 1 kept, value read");
    }
    { // wide line, no switch at all
        wchar_t l[] = L"TS3.exe -worldFile:A.sims3\"";
        const auto s = EditInGameArg::Rename(l);
        Check(s.found == 0 && s.renamed == 0 && !s.marked, "no switch: nothing");
    }
    { // wide, marked
        wchar_t l[] = L"TS3.exe -editInGameMode:true -apexDirect";
        const auto s = EditInGameArg::Rename(l);
        Check(s.renamed == 1 && std::wcsstr(l, L"apexNoEditMode"), "wide marked renamed");
    }
    { // end of line right after the key, null line
        char l[] = "TS3.exe -editInGameMode";
        const auto s = EditInGameArg::Rename(l);
        Check(s.found == 1 && s.renamed == 0 && s.value[0] == 0, "key at end: kept, empty value");
        Check(EditInGameArg::Rename(static_cast<char*>(nullptr)).found == 0, "null line");
    }
    std::printf("%s\n", g_fail ? "FAILED" : "all edit-in-game argument checks passed");
    return g_fail ? 1 : 0;
}
