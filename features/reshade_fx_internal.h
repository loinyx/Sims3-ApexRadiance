#pragma once
// ReShade FX effects: shared between the runtime (reshade_fx.cpp) and its file side (reshade_fx_files.cpp)
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ReShadeFx::Detail {

std::wstring Sub(const wchar_t* name);  // EffectsFolder() + name + "\" (Shaders, Textures, Presets, Cache, Internal)
std::wstring LowerW(std::wstring s);
std::string Lower(std::string s);

// Writes FX\Internal\ReShade.fxh and ReShadeUI.fxh (the headers most effects include) when missing or outdated.
// Searched last, so a pack's own copies win.
bool EnsureInternalHeaders(std::string& error);
// The .fx files below Shaders (the first one of each name, sorted by name)
std::vector<std::filesystem::path> ScanEffects();
// Every folder below Shaders (as ReShade's "Shaders\**"), then Internal
std::vector<std::filesystem::path> IncludeFolders();

// A texture's source file ("source" annotation): below Textures, then Shaders. Empty when missing.
std::filesystem::path FindTexture(const std::string& source);
// Decodes an image (PNG, JPEG, BMP, DDS, GIF, TIFF: whatever Windows Imaging Component reads) to BGRA8 at the given size
// (scaled when it differs). Worker thread (initialises COM for itself).
struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bgra;
};
bool LoadImageFile(const std::filesystem::path& path, uint32_t width, uint32_t height, Image& out, std::string& error);

// Compiled shader cache (FX\Cache\<hash>.cso)
bool ReadCache(uint64_t hash, std::string& bytecode);
void WriteCache(uint64_t hash, const std::string& bytecode);
uint64_t Hash64(const std::string& a, const std::string& b, const std::string& c);

} // namespace ReShadeFx::Detail
