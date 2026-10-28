#include "s3pp_settings.h"
#include "apex_log.h"
#include "apex_util.h"
#include <windows.h>
#include <format>
#include <fstream>
#include <iterator>

namespace S3PPSettings {
namespace {

State g_state;
bool g_looked = false;

bool ReadFile(const std::wstring& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

const State& Get() {
    if (g_looked) return g_state;
    g_looked = true;
    g_state.ts3native = GetModuleHandleW(L"ts3native.asi") != nullptr;
    const HMODULE s3pp = GetModuleHandleW(L"Sims3Performance.asi");
    g_state.s3pp = s3pp != nullptr;
    wchar_t path[MAX_PATH] = {};
    if (s3pp && GetModuleFileNameW(s3pp, path, MAX_PATH)) {
        std::wstring ini(path);
        ini.resize(ini.find_last_of(L"\\/") + 1);
        g_state.ini = ini + L"Sims3Performance.ini";
        std::string text;
        if (ReadFile(g_state.ini, text)) {
            g_state.spin = ReadPatch(text, "ShortWaitSpin") > 0;
            g_state.diet = ReadPatch(text, "ThreadStackDiet") > 0;
        }
    }
    if (g_state.s3pp || g_state.ts3native)
        LOG_INFO(std::format("[Compatibility] sims3-modernization {}, Sims3Performance {} (ShortWaitSpin {}, ThreadStackDiet {})",
                             g_state.ts3native ? "loaded" : "not loaded", g_state.s3pp ? "loaded" : "not loaded", g_state.spin ? "on" : "off",
                             g_state.diet ? "on" : "off"));
    return g_state;
}

bool Recommended() {
    const State& s = Get();
    return s.ts3native && s.s3pp && (s.spin || s.diet) && !s.turnedOff;
}

bool Apply() {
    State& s = g_state;
    s.error.clear();
    std::string text;
    if (s.ini.empty() || !ReadFile(s.ini, text)) s.error = "could not read the file";
    else if (const std::string changed = TurnOff(text); changed != text) {
        // a copy first, then the new text through a temporary file, so a failed write never leaves half a file
        const std::wstring backup = s.ini + L".apex-backup", temp = s.ini + L".apex-new";
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(changed.data(), static_cast<std::streamsize>(changed.size()));
        out.close();
        SetFileAttributesW(backup.c_str(), FILE_ATTRIBUTE_NORMAL); // CopyFile keeps a read-only ini's flag: a retry must still replace it
        if (!CopyFileW(s.ini.c_str(), backup.c_str(), FALSE)) s.error = std::format("could not copy it (error {})", GetLastError());
        else if (!out) s.error = "could not write the new file";
        else if (!MoveFileExW(temp.c_str(), s.ini.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            s.error = std::format("could not replace it (error {})", GetLastError());
        if (!s.error.empty()) DeleteFileW(temp.c_str());
    }
    s.turnedOff = s.error.empty();
    if (s.turnedOff) LOG_INFO("[Compatibility] Sims3Performance.ini: ShortWaitSpin and ThreadStackDiet set to 0 (old file kept as Sims3Performance.ini.apex-backup)");
    else LOG_WARNING(std::format("[Compatibility] Sims3Performance.ini not changed: {} ({})", s.error, ApexUtil::ToUtf8(s.ini)));
    return s.turnedOff;
}

} // namespace S3PPSettings
