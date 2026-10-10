#include "apex_paths.h"
#include "apex_util.h"
#include <windows.h>
#include <shlobj.h>
#include <cwctype>
#include <mutex>

#pragma comment(lib, "shell32.lib")

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
// two ids later (blocks of 100 ids from 1000). Pick the block of the user's locale, else of the same language.
std::wstring GameFolderName() {
    HMODULE exe = GetModuleHandleW(nullptr);
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH] = {};
    if (!exe || !GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH)) return L"The Sims 3";
    const std::wstring locale = Lower(localeName);
    const std::wstring language = locale.substr(0, locale.find(L'-'));
    std::wstring sameLanguage;
    for (UINT block = 1000; block < 4000; block += 100) {
        wchar_t code[32] = {};
        if (LoadStringW(exe, block, code, 32) <= 0) continue;
        const std::wstring blockCode = Lower(code);
        wchar_t folder[MAX_PATH] = {};
        if (LoadStringW(exe, block + 2, folder, MAX_PATH) <= 0) continue;
        if (blockCode == locale) return folder;
        if (sameLanguage.empty() && blockCode.substr(0, blockCode.find(L'-')) == language) sameLanguage = folder;
    }
    return sameLanguage.empty() ? L"The Sims 3" : sameLanguage;
}

std::once_flag g_once;
std::wstring g_game, g_s3ss, g_apex;
std::string g_iniUtf8;

void Resolve() {
    std::call_once(g_once, [] {
        wchar_t docs[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs))) return;
        const std::wstring ea = std::wstring(docs) + L"\\Electronic Arts\\";
        std::wstring game = ea + GameFolderName() + L"\\";
        // If the localized folder does not exist but the English one does (a game switched language), use that one.
        if (!IsDirectory(game) && IsDirectory(ea + L"The Sims 3\\")) game = ea + L"The Sims 3\\";
        g_game = game;
        g_s3ss = game + L"S3SS\\";
        g_apex = game + L"Apex Radiance\\";
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
