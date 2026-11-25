#include "apex_paths.h"
#include "apex_util.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <mutex>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace ApexPaths {
namespace {

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool IsDirectory(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// The game names its Documents folder after the language it runs in ("The Sims 3", "Die Sims 3", "Les Sims 3", ...).
// TS3W.exe carries one string-table block per language: the language code at the block's first id and the folder name
// two ids later (blocks of 100 ids from 1000).
std::wstring FolderOf(const std::wstring& locale, bool sameLanguage) {
    const std::wstring language = locale.substr(0, locale.find(L'-'));
    std::wstring found;
    for (UINT block = 1000; block < 4000; block += 100) {
        wchar_t code[32] = {};
        if (LoadStringW(GetModuleHandleW(nullptr), block, code, 32) <= 0) continue;
        const std::wstring blockCode = Lower(code);
        wchar_t folder[MAX_PATH] = {};
        if (LoadStringW(GetModuleHandleW(nullptr), block + 2, folder, MAX_PATH) <= 0) continue;
        if (blockCode == locale) return folder;
        if (sameLanguage && found.empty() && blockCode.substr(0, blockCode.find(L'-')) == language) found = folder;
    }
    return found.empty() ? L"The Sims 3" : found;
}

// Steam's exe has its own registry key, named in its read-only data.
bool IsSteamExe() {
    const char* const base = reinterpret_cast<const char*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    static const char kKey[] = "Software\\Sims(Steam)"; // with its ending zero (the launcher tells this rule by this text: Scan.cs)
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (memcmp(s->Name, ".rdata", 6) != 0) continue;
        const char* const b = base + s->VirtualAddress;
        const char* const e = b + s->Misc.VirtualSize;
        if (std::search(b, e, kKey, kKey + sizeof(kKey)) != e) return true;
    }
    return false;
}

// The game's language, as TS3W.exe reads it: the first "Locale" under its registry key. Empty when there is none.
std::wstring GameLocale() {
    HKEY root = nullptr;
    const wchar_t* const path = IsSteamExe() ? L"Software\\Sims(Steam)" : L"Software\\Sims";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | KEY_WOW64_32KEY, &root) != ERROR_SUCCESS) return {};
    std::wstring locale;
    for (DWORD i = 0; locale.empty(); ++i) {
        wchar_t name[256];
        DWORD length = ARRAYSIZE(name);
        if (RegEnumKeyExW(root, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t value[32] = {};
        DWORD size = sizeof(value);
        if (RegGetValueW(root, name, L"Locale", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS) locale = Lower(value);
    }
    RegCloseKey(root);
    for (wchar_t& c : locale)
        if (c == L'_') c = L'-';
    return locale;
}

// The folder of Windows' language: what the game uses with no language in its registry, and what Sims3SettingsSetter
// and earlier Apex versions always used.
std::wstring WindowsFolder() {
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
    return GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) ? FolderOf(Lower(locale), true) : L"The Sims 3";
}

bool IsFile(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// A folder's files into another: what is not there yet, nothing replaced. keep: they stay where they were too.
void MoveMissing(const std::wstring& from, const std::wstring& to, bool keep) {
    if (!keep && MoveFileW(from.c_str(), to.c_str())) return;
    const DWORD a = GetFileAttributesW(from.c_str());
    if (keep && a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) CopyFileW(from.c_str(), to.c_str(), TRUE);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) return;
    CreateDirectoryW(to.c_str(), nullptr);
    WIN32_FIND_DATAW found;
    const HANDLE find = FindFirstFileW((from + L"\\*").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(found.cFileName, L".") != 0 && wcscmp(found.cFileName, L"..") != 0) MoveMissing(from + L"\\" + found.cFileName, to + L"\\" + found.cFileName, keep);
    } while (FindNextFileW(find, &found));
    FindClose(find);
    if (!keep) RemoveDirectoryW(from.c_str()); // only when nothing stayed
}

std::once_flag g_once;
std::wstring g_game, g_s3ss, g_apex;
std::string g_iniUtf8;

void Resolve() {
    std::call_once(g_once, [] {
        wchar_t docs[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs))) return;
        const std::wstring ea = std::wstring(docs) + L"\\Electronic Arts\\";
        // The folder the game has played in (it holds Options.ini) goes first, for editions that keep their language elsewhere.
        const std::wstring locale = GameLocale();
        const std::wstring windows = ea + WindowsFolder() + L"\\";
        const std::wstring first = locale.empty() ? windows : ea + FolderOf(locale, false) + L"\\";
        std::wstring game = first;
        for (const std::wstring& candidate : {first, windows, ea + L"The Sims 3\\"}) {
            if (!IsFile(candidate + L"Options.ini")) continue;
            game = candidate;
            break;
        }
        g_game = game;
        g_apex = game + L"Apex Radiance\\";
        g_s3ss = game + L"S3SS\\";
        if (_wcsicmp(game.c_str(), windows.c_str()) != 0) {
            // Apex's own folder left in the one of Windows' language comes along, while this one has no settings. The launcher
            // goes by the same rule (Scan.cs ApexDir).
            const std::wstring old = windows + L"Apex Radiance";
            if (!IsFile(g_apex + L"ApexRadiance.toml") && IsDirectory(old)) {
                SHCreateDirectoryExW(nullptr, game.c_str(), nullptr);
                // (another edition plays in that one, and keeps its settings)
                MoveMissing(old, game + L"Apex Radiance", IsFile(windows + L"Options.ini"));
                // Settings that could not come (in use): read there until the next start moves them.
                if (!IsFile(g_apex + L"ApexRadiance.toml") && IsFile(old + L"\\ApexRadiance.toml")) g_apex = old + L"\\";
            }
            // Sims3SettingsSetter keeps using that one: the file saved last is in use, or with none the place it will read.
            WIN32_FILE_ATTRIBUTE_DATA here = {}, there = {};
            const bool hasHere = GetFileAttributesExW((g_s3ss + L"S3SS.toml").c_str(), GetFileExInfoStandard, &here) != 0;
            const bool hasThere = GetFileAttributesExW((windows + L"S3SS\\S3SS.toml").c_str(), GetFileExInfoStandard, &there) != 0;
            if (hasHere ? hasThere && CompareFileTime(&there.ftLastWriteTime, &here.ftLastWriteTime) > 0 : IsDirectory(windows)) g_s3ss = windows + L"S3SS\\";
        }
        g_iniUtf8 = ApexUtil::ToUtf8(g_apex + L"apex_radiance_imgui.ini");
    });
}

} // namespace

const std::wstring& GameDocumentsDirectory() {
    Resolve();
    return g_game;
}
const std::wstring& S3SSDirectory() {
    Resolve();
    return g_s3ss;
}
const std::wstring& ApexDirectory() {
    Resolve();
    return g_apex;
}

bool EnsureApexDirectory() {
    const std::wstring& dir = ApexDirectory();
    if (dir.empty()) return false;
    if (IsDirectory(dir)) return true;
    const int r = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS || IsDirectory(dir);
}

std::wstring ConfigFile() { return ApexDirectory() + L"ApexRadiance.toml"; }
std::wstring DiagnosticsDirectory() { return ApexDirectory() + L"Diagnostics\\"; }
std::wstring LogFile() { return ApexDirectory() + L"ApexRadiance_LOG.txt"; }
std::wstring S3SSConfigFile() { return S3SSDirectory() + L"S3SS.toml"; }
std::wstring MigrationBackup() { return ApexDirectory() + L"S3SS.toml.pre-split.bak"; }

std::wstring LegacyApexDirectory() { return S3SSDirectory().empty() ? std::wstring() : S3SSDirectory() + L"Apex\\"; }
std::wstring LegacyConfigFile() {
    const std::wstring dir = LegacyApexDirectory();
    return dir.empty() ? std::wstring() : dir + L"Apex.toml";
}

const char* ImGuiIniFileUtf8() {
    Resolve();
    return g_iniUtf8.empty() ? nullptr : g_iniUtf8.c_str();
}

} // namespace ApexPaths
