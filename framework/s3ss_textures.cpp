#include "s3ss_textures.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_util.h"
#include "memory_patch.h"
#include "platform.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>

namespace S3SSTextures {
namespace {

State g_state;
bool g_looked = false;
FILETIME g_stamp = {};
DWORD g_nextRefresh = 0;
uintptr_t g_lotSite = 0, g_simSite = 0;
bool g_sitesLooked = false;
// S3SS's patches/uncompressed_*.cpp: the lot format choice (original or as S3SS writes it) and the Sim build options
constexpr const char* kLotOriginal = "0F B6 85 84 00 00 00 83 E0 01 03 C0 68 ?? ?? ?? ?? 03 C0 6A 02 03 C0 83 C8 02";
constexpr const char* kLotS3SS = "B8 3E 00 00 00 90 90 90 90 90 90 90 68 ?? ?? ?? ?? 90 90 6A 02 90 90 90 90 90";
constexpr const char* kSim = "83 B9 A0 01 00 00 ?? 8A 44 24 0C 88 81 A4 01 00 00 73 21 8A 54 24 08 8A 44 24 10 F6 DA 88 81 A5 01 00 00 1B D2 83 E2 ??";

bool ReadFile(const std::wstring& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

FILETIME Stamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) ? a.ftLastWriteTime : FILETIME{};
}
void ReadToml() {
    std::string text;
    g_state.lot = g_state.sim = false;
    if (!g_state.toml.empty() && ReadFile(g_state.toml, text)) g_state.lot = Enabled(text, kLotSection), g_state.sim = Enabled(text, kSimSection);
    g_stamp = Stamp(g_state.toml);
}
// One match in the game's .text (none or several: not used)
uintptr_t Unique(const char* pattern) {
    BYTE* image = nullptr;
    size_t size = 0;
    if (!MemPatch::GetModuleInfo(GetModuleHandleW(nullptr), &image, &size)) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (std::memcmp(s->Name, ".text", 5) != 0) continue;
        BYTE* base = image + s->VirtualAddress;
        const size_t len = s->Misc.VirtualSize;
        const uintptr_t a = MemPatch::ScanPattern(base, len, pattern);
        if (!a) return 0;
        const size_t next = a + 1 - reinterpret_cast<uintptr_t>(base);
        return next < len && MemPatch::ScanPattern(reinterpret_cast<BYTE*>(a + 1), len - next, pattern) ? 0 : a;
    }
    return 0;
}
void ReadLive() {
    if (!g_sitesLooked) { // (once: a scan of .text)
        g_sitesLooked = true;
        g_lotSite = Unique(kLotOriginal);
        if (!g_lotSite) g_lotSite = Unique(kLotS3SS);
        g_simSite = Unique(kSim);
    }
    BYTE lot[2] = {}, mask = 0xCD;
    g_state.liveLot = g_lotSite && MemPatch::ReadBytes(g_lotSite, lot, 2) && lot[0] == 0xB8 && lot[1] == 0x3E;
    g_state.liveSim = g_simSite && MemPatch::ReadBytes(g_simSite + 39, &mask, 1) && mask == 0x00;
}

} // namespace

const State& Get() {
    if (g_looked) return g_state;
    g_looked = true;
    g_state.s3ss = GetModuleHandleW(L"Sims3SettingsSetter.asi") != nullptr;
    g_state.toml = ApexPaths::S3SSConfigFile();
    if (g_state.s3ss) ReadToml();
    if (g_state.s3ss && Platform::RemixBridge()) ReadLive();
    if (g_state.s3ss && (g_state.lot || g_state.sim))
        LOG_INFO(std::format("[Compatibility] Sims3SettingsSetter: Uncompressed Compositor Textures {}, Uncompressed Sim Textures {}; RTX Remix bridge {}",
                             g_state.lot ? "on" : "off", g_state.sim ? "on" : "off", Platform::RemixBridge() ? "in use" : "not in use"));
    return g_state;
}

void Refresh() {
    const State& s = Get();
    if (!s.s3ss || !Platform::RemixBridge()) return;
    const DWORD now = GetTickCount();
    if (static_cast<int32_t>(now - g_nextRefresh) < 0) return;
    g_nextRefresh = now + 1000;
    const bool before = s.lot || s.sim || s.liveLot || s.liveSim;
    if (const FILETIME t = Stamp(s.toml); CompareFileTime(&t, &g_stamp) != 0) {
        ReadToml();
        if (g_state.turnedOff && (g_state.lot || g_state.sim)) g_state.turnedOff = false; // turned on again in S3SS's menu and saved
    }
    ReadLive();
    if (const bool after = g_state.lot || g_state.sim || g_state.liveLot || g_state.liveSim; after != before)
        LOG_INFO(std::format("[Compatibility] Sims3SettingsSetter's uncompressed textures turned {} during play (file: lot {}, Sim {}; game code: lot {}, Sim {})",
                             after ? "on" : "off", g_state.lot ? "on" : "off", g_state.sim ? "on" : "off", g_state.liveLot ? "on" : "off", g_state.liveSim ? "on" : "off"));
}

bool Recommended() {
    const State& s = Get();
    return Platform::RemixBridge() && s.s3ss && (s.lot || s.sim || s.liveLot || s.liveSim) && !s.turnedOff;
}

bool Apply() {
    State& s = g_state;
    s.error.clear();
    std::string text;
    if (s.toml.empty() || !ReadFile(s.toml, text)) s.error = "could not read the file";
    else if (const std::string changed = TurnOff(text); changed != text) {
        // a copy first, then the new text through a temporary file, so a failed write never leaves half a file
        const std::wstring backup = s.toml + L".apex-backup", temp = s.toml + L".apex-new";
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(changed.data(), static_cast<std::streamsize>(changed.size()));
        out.close();
        SetFileAttributesW(backup.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!CopyFileW(s.toml.c_str(), backup.c_str(), FALSE)) s.error = std::format("could not copy it (error {})", GetLastError());
        else if (!out) s.error = "could not write the new file";
        else if (!MoveFileExW(temp.c_str(), s.toml.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            s.error = std::format("could not replace it (error {})", GetLastError());
        if (!s.error.empty()) DeleteFileW(temp.c_str());
    }
    s.turnedOff = s.error.empty();
    if (s.turnedOff)
        LOG_INFO("[Compatibility] S3SS.toml: Uncompressed Compositor Textures and Uncompressed Sim Textures turned off (old file kept as S3SS.toml.apex-backup)");
    else LOG_WARNING(std::format("[Compatibility] S3SS.toml not changed: {} ({})", s.error, ApexUtil::ToUtf8(s.toml)));
    return s.turnedOff;
}

} // namespace S3SSTextures
