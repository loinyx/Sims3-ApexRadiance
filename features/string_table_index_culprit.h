#pragma once
// String table index: who changed the game code it needs (string_table_index.cpp stand-downs). Decodes the jump another
// module wrote over that code and words the stand-down with the module's file name and, for a known mod, the setting to
// turn off. Header only, so the offline check (tools/string_table_index_test) runs it on fixture bytes.
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

namespace StringTableIndex::Culprit {

// The code each stand-down checks
enum class Site { Entry, Walk, ListFunctions, Layout };

// Reads the 32-bit pointer at an address (jmp [abs32]); false when it cannot
using ReadPtr = bool (*)(uintptr_t at, uintptr_t* value);

// The destination of a jump written at `at` (b = the n bytes there): jmp rel32 (E9), jmp rel8 (EB), jmp [abs32] (FF 25),
// push imm32; ret (68 .. C3), mov reg,imm32; jmp reg (B8+r .. FF E0+r). 0 when the bytes are no such jump.
inline uintptr_t JumpTarget(const uint8_t* b, size_t n, uintptr_t at, ReadPtr read) {
    int32_t rel = 0;
    uint32_t imm = 0;
    if (n >= 5 && b[0] == 0xE9) {
        std::memcpy(&rel, b + 1, 4);
        return at + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
    }
    if (n >= 2 && b[0] == 0xEB) return at + 2 + static_cast<uintptr_t>(static_cast<intptr_t>(static_cast<int8_t>(b[1])));
    if (n >= 6 && b[0] == 0xFF && b[1] == 0x25) {
        std::memcpy(&imm, b + 2, 4);
        uintptr_t v = 0;
        return read && read(imm, &v) ? v : 0;
    }
    if (n >= 6 && b[0] == 0x68 && b[5] == 0xC3) {
        std::memcpy(&imm, b + 1, 4);
        return imm;
    }
    if (n >= 7 && (b[0] & 0xF8) == 0xB8 && b[5] == 0xFF && b[6] == (0xE0 | (b[0] & 7))) {
        std::memcpy(&imm, b + 1, 4);
        return imm;
    }
    return 0;
}

// Mods known to patch this code, with the setting that stops it per site (nullptr: no setting known)
struct Known {
    const char* file;
    const char* name;
    const char* fix[4]; // by Site
};
// Sims3Performance.ini [Patches] keys from the mod's own ini and readme (beta 2.1): LocalizedStringCache detours the
// entry, StblBucketLayoutTuning changes the hash layout. ts3native's [Verify] Threads probes detour the rebuild
// (ListFunctions) for its diagnostics (its verify.cpp).
#define APEX_S3PP_INI " in the [Patches] section of Sims3Performance.ini (Game\\Bin)"
inline constexpr Known kKnown[] = {
    {"Sims3Performance.asi", "Sims 3 Performance Patch",
     {"set LocalizedStringCache=0" APEX_S3PP_INI, "set LocalizedStringCache=0 and StblBucketLayoutTuning=0" APEX_S3PP_INI,
      "set LocalizedStringCache=0 and StblBucketLayoutTuning=0" APEX_S3PP_INI, "set StblBucketLayoutTuning=0" APEX_S3PP_INI}},
    {"ts3native.asi", "Sims 3 Modernization", {nullptr, nullptr, "set Threads=0 in the [Verify] section of ts3native.ini (Game\\Bin)", nullptr}},
};
#undef APEX_S3PP_INI

inline bool SameFile(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto lo = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
        if (lo(a[i]) != lo(b[i])) return false;
    }
    return true;
}

inline const Known* FindKnown(std::string_view file) {
    for (const auto& k : kKnown)
        if (SameFile(file, k.file)) return &k;
    return nullptr;
}

inline const char* SiteText(Site s) {
    switch (s) {
    case Site::Entry: return "the text lookup's entry";
    case Site::Walk: return "the table walk";
    case Site::ListFunctions: return "the string table list's functions";
    default: return "the string tables' hash layout";
    }
}

// What patched it, for the stand-down. file: the module the patch jumps into ("" = none found); inMemory: a jump was
// found but lands outside every module; likely: no jump decodes (an in-place change) and file is a known mod that is loaded.
// "changed by <name> (<file>)" / "changed by <file>" is read back by Setup (installer/ApexSetup.Core/Compat.cs).
inline std::string Describe(Site site, uintptr_t address, std::string_view file, bool inMemory, bool likely = false) {
    const std::string what = std::format("{} ({:#010x})", SiteText(site), address);
    if (file.empty())
        return inMemory ? what + " was changed by a patch in memory (no file), likely a loader or trainer"
                        : what + " was changed by another mod (its file was not found)";
    const Known* k = FindKnown(file);
    const std::string by = k ? std::format("{} ({})", k->name, file) : std::string(file);
    const char* fix = k ? k->fix[static_cast<int>(site)] : nullptr;
    return std::format("{} was changed {}by {}. To use Faster text lookups, {}, then restart the game", what, likely ? "likely " : "", by,
                       fix ? std::string(fix) : std::format("turn off the part of that mod that changes text lookups or remove {}", file));
}

// A jump into the game's own exe: the exe file was patched on disk, or a mod writes code into it
inline std::string DescribeExe(Site site, uintptr_t address) {
    return std::format("{} ({:#010x}) was changed to jump elsewhere in the game's exe (a patched exe file, or a mod that writes code into it)",
                       SiteText(site), address);
}

} // namespace StringTableIndex::Culprit
