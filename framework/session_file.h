#pragma once
// Per-session diagnostic files (developer mode): Apex Radiance\Diagnostics\<family>-<pid>-<date>-<time>.<ext>, and only the
// newest kKeepSessions of each family stay. Copies that older builds left at the Apex Radiance root move here first.
#include "apex_paths.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace ApexPaths {

inline constexpr size_t kKeepSessions = 10;

// The family's files in `dir` ("<family>-<digit>...", so GameplayMemory never matches Game...).
inline std::vector<std::filesystem::path> SessionFiles(const std::filesystem::path& dir, const std::wstring& family) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    const std::wstring prefix = family + L"-";
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring n = it->path().filename().wstring();
        std::error_code fe;
        if (n.size() > prefix.size() && n.rfind(prefix, 0) == 0 && std::iswdigit(n[prefix.size()]) && it->is_regular_file(fe)) out.push_back(it->path());
    }
    return out;
}

// This session's file of `family`; empty when the folder cannot be made. Call once per session and family.
inline std::wstring NewSessionFile(const wchar_t* family, const wchar_t* ext) {
    namespace fs = std::filesystem;
    if (!EnsureApexDirectory()) return {};
    std::error_code ec;
    const fs::path root = ApexDirectory(), dir = root / L"Diagnostics";
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) return {};
    for (const fs::path& p : SessionFiles(root, family)) fs::rename(p, dir / p.filename(), ec);
    std::vector<std::pair<fs::file_time_type, fs::path>> kept;
    for (const fs::path& p : SessionFiles(dir, family)) kept.emplace_back(fs::last_write_time(p, ec), p);
    if (kept.size() >= kKeepSessions) {
        std::sort(kept.begin(), kept.end());
        for (size_t i = 0; i + kKeepSessions - 1 < kept.size(); i++) fs::remove(kept[i].second, ec);
    }
    SYSTEMTIME t{};
    GetLocalTime(&t);
    return (dir / std::format(L"{}-{}-{:04}{:02}{:02}-{:02}{:02}{:02}.{}", family, GetCurrentProcessId(), t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, ext))
        .wstring();
}

} // namespace ApexPaths
