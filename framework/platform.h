#pragma once
// Where the game runs: Windows, or Wine / Proton (Linux, SteamOS), and whether the machine is a Steam Deck. Read once,
// on first use; any thread.
//   Wine / Proton  ntdll exports wine_get_version (every Wine since 2008); wine_get_host_version names the host system
//   Steam Deck     under Wine, the host's DMI files through Wine's Z: drive (Z:\sys\devices\virtual\dmi\id): vendor
//                  "Valve", product "Jupiter" (LCD) or "Galileo" (OLED). The environment variable SteamDeck=1 (set by
//                  Steam in Gaming Mode, also on some other Linux desktops) is kept as a hint only
//   DXVK           the loaded d3d9.dll exports DXVK_RegisterAnnotation by ordinal 28257 (no name); Proton loads its DXVK
//                  from the prefix's system folder, so the file's place says nothing
//   RTX Remix      the bridge client d3d9.dll exports remixapi_InitializeLibrary (ordinal 39) and not ordinal 28257
#include <string>

namespace Platform {

struct Info {
    bool wine = false;
    std::string wineVersion; // "9.0" ... (Proton builds report their Wine base)
    std::string host;        // "Linux 6.5.0-valve22-1-neptune" ... (empty when not under Wine)
    bool steamDeck = false;  // the DMI says Valve Jupiter / Galileo
    std::string deckModel;   // "LCD", "OLED" or ""
    bool deckHint = false;   // SteamDeck=1 in the environment (Gaming Mode), with or without the DMI
};

const Info& Get();
inline bool UnderWine() { return Get().wine; }
inline bool SteamDeck() { return Get().steamDeck; }

// Whether the game's d3d9.dll is DXVK (by its export); false while d3d9.dll is not loaded yet. Cached once loaded.
bool DxvkD3D9();

// Whether the game's d3d9.dll is the RTX Remix bridge client (it exports remixapi_InitializeLibrary and is not DXVK
// itself): the game's calls go to a 64-bit server process that renders through DXVK. Formats and features are DXVK's;
// DXVK's exports, its config, its memory and the Vulkan driver are not in this process, and every readback (a GPU
// copy read on the CPU, a query's result) waits for a round trip to the server. DxvkD3D9() is false under it.
// False while d3d9.dll is not loaded yet. Cached once loaded.
bool RemixBridge();

// One line for the log and bug reports: "Windows" or "Wine 9.0 on Linux ... (Steam Deck OLED)", with DXVK when seen
std::string Summary();

} // namespace Platform
