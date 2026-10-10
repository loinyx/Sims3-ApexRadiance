#include "s3ss_detect.h"
#include "s3ss_ambient_policy.h"
#include "apex_version.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_util.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include "ui/i18n.h"
#include <psapi.h>
#include <toml++/toml.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace S3SSDetect {
namespace {

constexpr uint8_t kKey = 0x5A;

template <size_t N> constexpr std::array<uint8_t, N - 1> Encode(const char (&s)[N]) {
    std::array<uint8_t, N - 1> out{};
    for (size_t i = 0; i + 1 < N; i++) out[i] = static_cast<uint8_t>(s[i]) ^ kKey;
    return out;
}

// Encoded at compile time; decoded into a buffer only while scanning.
constexpr auto kLogHeader = Encode("S3SS Log - Started at ");
constexpr auto kWindowId = Encode("###S3SSWindow");
constexpr auto kApexName = Encode("Sims3 Settings Setter Apex Edition");

template <size_t N> std::vector<uint8_t> Decode(const std::array<uint8_t, N>& enc) {
    volatile uint8_t key = kKey; // keeps the decode at run time
    std::vector<uint8_t> out(N);
    for (size_t i = 0; i < N; i++) out[i] = enc[i] ^ key;
    return out;
}

// SEH-guarded substring search in [p, p + n) (no C++ objects in here)
bool Contains(const uint8_t* p, size_t n, const uint8_t* needle, size_t m) {
    if (m == 0 || n < m) return false;
    __try {
        const uint8_t first = needle[0];
        for (size_t i = 0; i + m <= n; i++) {
            if (p[i] != first) continue;
            if (std::memcmp(p + i + 1, needle + 1, m - 1) == 0) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

struct Needles {
    std::vector<uint8_t> log, window, apex;
};

// Which needles a module's read-only data holds: bit 0 log header, bit 1 window id, bit 2 Apex name.
int ScanModule(HMODULE mod, const Needles& nd) {
    const auto* base = reinterpret_cast<const uint8_t*>(mod);
    IMAGE_DOS_HEADER dos{};
    if (!MemPatch::ReadBytes(reinterpret_cast<uintptr_t>(base), &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS nt{};
    if (!MemPatch::ReadBytes(reinterpret_cast<uintptr_t>(base) + dos.e_lfanew, &nt, sizeof nt) || nt.Signature != IMAGE_NT_SIGNATURE) return 0;
    const uintptr_t sections = reinterpret_cast<uintptr_t>(base) + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    int found = 0;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections && i < 96; i++) {
        IMAGE_SECTION_HEADER sh{};
        if (!MemPatch::ReadBytes(sections + i * sizeof sh, &sh, sizeof sh)) break;
        const DWORD c = sh.Characteristics;
        if (!(c & IMAGE_SCN_MEM_READ) || (c & IMAGE_SCN_MEM_EXECUTE)) continue; // strings live in read-only / data sections
        const size_t size = sh.Misc.VirtualSize ? sh.Misc.VirtualSize : sh.SizeOfRawData;
        if (!size || size > (64u << 20)) continue;
        const uint8_t* p = base + sh.VirtualAddress;
        if (!(found & 1) && Contains(p, size, nd.log.data(), nd.log.size())) found |= 1;
        if (!(found & 2) && Contains(p, size, nd.window.data(), nd.window.size())) found |= 2;
        if (!(found & 4) && Contains(p, size, nd.apex.data(), nd.apex.size())) found |= 4;
    }
    return found;
}

std::mutex g_lock;
Info g_info;

std::wstring ModuleFileName(HMODULE mod, bool withPath) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(mod, path, MAX_PATH);
    if (withPath) return path;
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    return base;
}

Info DoScan() {
    Info info;
    info.scanned = true;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&DoScan), &self);
    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    const size_t winLen = wcslen(winDir);
    std::vector<HMODULE> mods(1024);
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &needed)) return info;
    mods.resize(std::min<size_t>(mods.size(), needed / sizeof(HMODULE)));
    const Needles nd{Decode(kLogHeader), Decode(kWindowId), Decode(kApexName)};
    for (HMODULE mod : mods) {
        if (!mod || mod == self || mod == GetModuleHandleW(nullptr)) continue;
        const std::wstring path = ModuleFileName(mod, true);
        if (winLen && _wcsnicmp(path.c_str(), winDir, winLen) == 0) continue; // system DLLs
        const int f = ScanModule(mod, nd);
        if ((f & 3) != 3) {
            // The previous standalone build (S3SSApex.asi): the Apex Edition name in its data (log header) without
            // S3SS's two strings, or simply its file name. It idles when this build loaded first (it finds its
            // Local\S3SSApex.<pid> mutex taken, see AcquireInstanceMutex), but it should still be deleted.
            const std::wstring name = ModuleFileName(mod, false);
            if ((f & 4) || _wcsicmp(name.c_str(), L"S3SSApex.asi") == 0) {
                info.oldStandalone = true;
                info.oldStandaloneModule = name;
            }
            continue;
        }
        MODULEINFO mi{};
        GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof mi);
        if (f & 4) {
            info.oldCombinedBuild = true;
            info.combinedModule = ModuleFileName(mod, false);
        } else {
            info.s3ssLoaded = true;
            info.s3ssModule = ModuleFileName(mod, false);
            info.s3ssBase = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
            info.s3ssSize = mi.SizeOfImage;
        }
    }
    return info;
}

std::optional<toml::table> ReadS3SSConfig() {
    std::string text;
    if (!ApexUtil::ReadFileBytes(ApexPaths::S3SSConfigFile(), text)) return std::nullopt;
    try {
        return toml::parse(text);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

std::string SummaryLocked(); // caller holds g_lock

Info Scan() {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_info.scanned) return g_info;
    }
    return Rescan();
}

Info Rescan() {
    Info info = DoScan();
    std::lock_guard<std::mutex> lock(g_lock);
    const bool changed = info.s3ssLoaded != g_info.s3ssLoaded || info.oldCombinedBuild != g_info.oldCombinedBuild || info.oldStandalone != g_info.oldStandalone ||
                         !g_info.scanned;
    g_info = std::move(info);
    if (changed) LOG_INFO("[S3SSDetect] " + SummaryLocked());
    return g_info;
}

bool IsInS3SS(uintptr_t address) {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_info.s3ssLoaded && address >= g_info.s3ssBase && address < g_info.s3ssBase + g_info.s3ssSize;
}

namespace {

enum class MutexState { Created, Exists, Failed };

// Creates the named mutex and keeps it for the life of the process (never closed), or tells that it already exists.
MutexState CreateNamedMutex(const wchar_t* prefix) {
    const std::wstring name = std::wstring(prefix) + std::to_wstring(GetCurrentProcessId());
    HANDLE h = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!h) return MutexState::Failed;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        return MutexState::Exists;
    }
    return MutexState::Created;
}

} // namespace

Instance AcquireInstanceMutex() {
    static Instance result = Instance::Unset;
    if (result != Instance::Unset) return result;
    // 1. this build's own name: another ApexRadiance.asi (a second copy, e.g. in another ASI folder) got here first
    if (CreateNamedMutex(L"Local\\ApexRadiance.") == MutexState::Exists) return result = Instance::DuplicateSelf;
    // 2. the previous standalone build's name. Held: an old S3SSApex.asi loaded first and is running. Free: this build
    //    now holds it, so an S3SSApex.asi that loads later finds it taken and stays idle (its own duplicate check).
    //    A failed create (either one) cannot tell anything: run.
    if (CreateNamedMutex(L"Local\\S3SSApex.") == MutexState::Exists) return result = Instance::OldStandaloneFirst;
    return result = Instance::Owner;
}

bool S3SSPatchEnabled(const char* patchName) {
    const auto root = ReadS3SSConfig();
    if (!root) return false;
    return (*root)["patches"][patchName]["enabled"].value_or(false);
}

bool S3SSPatchBoolSettingEnabled(const char* patchName, const char* settingName, bool defaultValue) {
    const auto root = ReadS3SSConfig();
    if (!root) return false;
    const auto patch = (*root)["patches"][patchName];
    if (!patch["enabled"].value_or(false)) return false;
    return patch[settingName].value_or(defaultValue);
}

bool S3SSOverlayDisabled() {
    const auto root = ReadS3SSConfig();
    if (!root) return false;
    return (*root)["qol"]["ui"]["disable_overlay"].value_or(false);
}

std::optional<std::array<float, 3>> SavedRoomAmbientOverride(bool fresh) {
    static std::mutex lock;
    static ULONGLONG checkedAt = 0;
    static std::optional<std::array<float, 3>> saved;
    std::lock_guard<std::mutex> guard(lock);
    const ULONGLONG now = GetTickCount64();
    if (!fresh && checkedAt && now - checkedAt < 3000) return saved;
    checkedAt = now;
    saved.reset();
    if (!Scan().s3ssLoaded) return saved;
    std::string text;
    if (!ApexUtil::ReadFileBytes(ApexPaths::S3SSConfigFile(), text)) return saved;
    if (const auto correction = S3SSAmbientPolicy::Prepare(text)) saved = correction->rgb;
    return saved;
}

// The menu's line (Settings > Compatibility > Details): SummaryLocked's text in the menu language (the log keeps the
// English one)
std::string Summary() {
    std::lock_guard<std::mutex> lock(g_lock);
    const Info& i = g_info;
    if (!i.scanned) return I18n::Tr("not scanned yet");
    std::string s = i.s3ssLoaded ? I18n::Trf("official Sims3SettingsSetter loaded ({})", ApexUtil::ToUtf8(i.s3ssModule))
                                 : std::string(I18n::Tr("official Sims3SettingsSetter not loaded"));
    if (i.oldCombinedBuild) s += I18n::Trf("; OLD COMBINED BUILD loaded ({}): " APEX_PRODUCT_NAME "'s features stay off", ApexUtil::ToUtf8(i.combinedModule));
    if (i.oldStandalone) s += I18n::Trf("; an older {} is also installed (idle): delete it from Game\\Bin", ApexUtil::ToUtf8(i.oldStandaloneModule));
    return s;
}

std::string SummaryLocked() {
    const Info& i = g_info;
    if (!i.scanned) return "not scanned yet";
    std::string s = i.s3ssLoaded ? "official Sims3SettingsSetter loaded (" + ApexUtil::ToUtf8(i.s3ssModule) + ")" : "official Sims3SettingsSetter not loaded";
    if (i.oldCombinedBuild) s += "; OLD COMBINED BUILD loaded (" + ApexUtil::ToUtf8(i.combinedModule) + "): " APEX_PRODUCT_NAME "'s features stay off";
    if (i.oldStandalone) s += "; an older " + ApexUtil::ToUtf8(i.oldStandaloneModule) + " is also installed (idle): delete it from Game\\Bin";
    return s;
}

} // namespace S3SSDetect

namespace S3SSDetect {

bool SplitLevelFixActive() {
    if (S3SSPatchEnabled("SplitLevelLightingFix")) return true;
    // GetLotID (0x6BC020 on Steam, found by signature on other builds): mov eax,[ecx+0C0h]; mov edx,[ecx+0C4h]; ret
    const uintptr_t getLotId = GameAddr::Get(GameAddr::Id::GetLotId);
    if (!getLotId) return false;
    static const BYTE kVanilla[] = {0x8B, 0x81, 0xC0, 0x00, 0x00, 0x00, 0x8B, 0x91, 0xC4, 0x00, 0x00, 0x00, 0xC3};
    BYTE now[sizeof kVanilla] = {};
    if (!MemPatch::ReadBytes(getLotId, now, sizeof now)) return false;
    return std::memcmp(now, kVanilla, sizeof now) != 0;
}

} // namespace S3SSDetect
