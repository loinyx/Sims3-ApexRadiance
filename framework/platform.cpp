// Windows or Wine / Proton, Steam Deck and DXVK detection (see platform.h). Credits: @loinyx
#include "platform.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <format>
#include <fstream>
#include <mutex>

namespace Platform {
namespace {

// The first line of a small text file, trimmed ("" when it cannot be read)
std::string FirstLine(const wchar_t* path) {
    std::ifstream in(path, std::ios::binary);
    std::string s;
    if (!in || !std::getline(in, s)) return {};
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s.size() > 128 ? s.substr(0, 128) : s;
}

Info Detect() {
    Info i;
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    using WineVersion_t = const char*(__cdecl*)();
    using WineHost_t = void(__cdecl*)(const char** sysname, const char** release);
    const auto version = ntdll ? reinterpret_cast<WineVersion_t>(GetProcAddress(ntdll, "wine_get_version")) : nullptr;
    char hint[8] = {};
    i.deckHint = GetEnvironmentVariableA("SteamDeck", hint, sizeof hint) > 0 && hint[0] == '1';
    if (!version) return i;
    i.wine = true;
    if (const char* v = version()) i.wineVersion = v;
    if (const auto host = reinterpret_cast<WineHost_t>(GetProcAddress(ntdll, "wine_get_host_version"))) {
        const char *sys = nullptr, *rel = nullptr;
        host(&sys, &rel);
        i.host = std::format("{} {}", sys ? sys : "", rel ? rel : "");
    }
    // Wine maps the host's root to Z: by default; a prefix without it simply finds no file
    const std::string vendor = FirstLine(L"Z:\\sys\\devices\\virtual\\dmi\\id\\sys_vendor");
    const std::string product = FirstLine(L"Z:\\sys\\devices\\virtual\\dmi\\id\\product_name");
    if (vendor == "Valve" && (product == "Jupiter" || product == "Galileo")) {
        i.steamDeck = true;
        i.deckModel = product == "Galileo" ? "OLED" : "LCD";
    }
    return i;
}

} // namespace

const Info& Get() {
    static const Info info = Detect();
    return info;
}

bool DxvkD3D9() {
    static std::atomic<int> cached{-1};
    const int c = cached.load(std::memory_order_relaxed);
    if (c >= 0) return c == 1;
    const HMODULE m = GetModuleHandleW(L"d3d9.dll");
    if (!m) return false; // not loaded yet: ask again later
    // DXVK_RegisterAnnotation, exported by ordinal only (dxvk src/d3d9/d3d9.def); a name lookup of it fails by design
    // Under Wine a DXVK without that export (older builds, unverified which) is told from Wine's own d3d9 by wined3d.dll,
    // which only Wine's d3d9 loads
    const bool dxvk = GetProcAddress(m, MAKEINTRESOURCEA(28257)) != nullptr || (Get().wine && !GetModuleHandleW(L"wined3d.dll"));
    cached.store(dxvk ? 1 : 0, std::memory_order_relaxed);
    return dxvk;
}

std::string Summary() {
    const Info& i = Get();
    std::string s = i.wine ? std::format("Wine {}{}{}", i.wineVersion, i.host.empty() ? "" : " on ", i.host) : "Windows";
    if (i.steamDeck) s += std::format(" (Steam Deck {})", i.deckModel);
    else if (i.deckHint) s += " (SteamDeck=1)";
    if (GetModuleHandleW(L"d3d9.dll")) s += DxvkD3D9() ? ", d3d9: DXVK" : ", d3d9: not DXVK";
    return s;
}

} // namespace Platform
