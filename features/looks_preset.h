#pragma once
// Looks: ReShade preset files (.ini) and where they come from.
//
// A look is referenced by its source and a path relative to that source, never by an absolute path:
//   looks:<relative path>   Documents\...\Apex Radiance\Looks\ (the player's own; Apex writes only here)
//   gshade:<relative path>  Game\Bin\1. Shader Presets\ (GShade's folder, read-only, listed when it exists)
//   apex:<name>             a built-in Apex look (embedded .ini text)
// Everything read from a file is bounded (kMaxPresetBytes, kMaxTechniques, kMaxLooks).

#include <cstddef>
#include <map>
#include <string>
#include <vector>
#include "looks_shaders.h"

namespace Looks {

inline constexpr size_t kMaxPresetBytes = 1u << 20; // a ReShade preset is a few KB; larger files are refused
inline constexpr size_t kMaxTechniques = 256;       // Techniques= / TechniqueSorting= entries kept
inline constexpr size_t kMaxLooks = 2000;            // files listed per source

struct PresetEntry
{
    std::string technique; // "Vibrance"
    std::string file;      // "Vibrance.fx"
    int shader = -1;       // index into the shader table (set by the caller), -1 = not built in
};

using IniSections = std::map<std::string, std::map<std::string, std::string>>; // [File.fx] key=value ("" = top)

struct Preset
{
    std::vector<PresetEntry> order; // Techniques= in TechniqueSorting order
    IniSections sections;
    std::string error;
};

// Parses preset text (BOM removed). The enabled techniques run in TechniqueSorting order, the rest of Techniques= last.
bool ParsePreset(const std::string& text, Preset& out);
// Values of one shader as the .ini sets them (missing keys keep the .fx default; non-finite numbers are ignored).
// 4 floats per parameter.
std::vector<float> PresetValues(const Preset& preset, const LooksShader& shader);
// TechniqueSorting= of the preset (bounded)
std::vector<std::string> PresetSorting(const Preset& preset);
std::vector<std::string> SplitList(const std::string& s);

// ---------------------------------------------------------------------------------------------- sources
enum class Source { None, Apex, Mine, GShade };
std::wstring LooksFolder();  // Apex Radiance\Looks\ (trailing backslash)
bool EnsureLooksFolder();
std::wstring GShadeFolder(); // Game\Bin\1. Shader Presets\ (trailing backslash)
Source RefSource(const std::string& ref);
// The file of a looks: or gshade: reference ("" for apex:, an empty or an unsafe reference)
std::wstring RefPath(const std::string& ref);
// The name the menu shows: the relative path without ".ini" (apex: names as they are)
std::string RefName(const std::string& ref);
// The .ini text of a reference (file or built-in), at most kMaxPresetBytes
bool LookText(const std::string& ref, std::string& text, std::string* error = nullptr);
// An absolute presetPath of older settings as a reference: under GShade's folder gshade:, under Looks\ looks:, else
// looks:<file name>. An empty path was GShade's "[Amelie] Serene 2.0.ini".
std::string MigrateRef(const std::string& absolutePath);

struct LookItem
{
    std::string ref, name;
    Source source = Source::None;
};
// Apex looks (in their own order), then My looks, then GShade (only when its folder exists), sorted by name
std::vector<LookItem> ListLooks();

// Built-in Apex looks (apex:<name>, read-only): their names (English menu keys), and the .ini text of one (nullptr
// when unknown)
std::vector<std::string> BuiltInLooks();
const char* BuiltInLookText(const std::string& name);

// A file name a look can be stored under in Looks\ (no folder, ends in .ini, no reserved or unsafe characters)
bool ValidLookFileName(const std::string& name);
// Stores bytes as Looks\<fileName>: identical bytes already there reuse that file; a different file with the name gets
// a numbered name ("Name-1.ini"). stored = the file name used; created = the file written ("" when one was reused).
bool StoreLook(const std::string& fileName, const std::string& bytes, std::string& stored, std::wstring* created, std::string* error);

} // namespace Looks
