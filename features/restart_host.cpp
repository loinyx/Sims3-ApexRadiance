// Restart host: the game side (see restart_host.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "restart_host.h"
#include "../native/ts3wrap/restart_channel.h"
#include "apex_config.h"
#include "apex_log.h"
#include "apex_paths.h"
#include <cstdio>
#include <string>

namespace RestartHost {
namespace {

constexpr uint64_t kLowBytes = 320ull << 20;  // Room to save's own "low" line (memory_guard.cpp)
RestartChannel* g_chan = nullptr;

} // namespace

void Start() {
    if (!ApexConfig::GetUi().restartHost) return;
    wchar_t host[MAX_PATH], name[64];
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Start), &self);
    const DWORD n = GetModuleFileNameW(self, host, MAX_PATH);
    wchar_t* slash = n && n < MAX_PATH ? wcsrchr(host, L'\\') : nullptr;
    if (!slash || wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - host), L"ts3wrap.exe") || GetFileAttributesW(host) == INVALID_FILE_ATTRIBUTES) {
        LOG_WARNING("[RestartHost] On, but ts3wrap.exe is not next to ApexRadiance.asi: no restart offers");
        return;
    }
    const std::wstring saves = ApexPaths::GameDocumentsDirectory() + L"Saves";
    const wchar_t* cmd = GetCommandLineW();
    HANDLE map = RhName(name, 64, GetCurrentProcessId()) ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(RestartChannel), name) : nullptr;
    auto* c = map ? static_cast<RestartChannel*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RestartChannel))) : nullptr;
    if (!c || saves.size() < 6 || saves.size() >= MAX_PATH || wcslen(cmd) >= _countof(c->cmd) || !GetModuleFileNameW(nullptr, c->exe, MAX_PATH)) {
        LOG_WARNING("[RestartHost] Channel not created (no documents folder, or the command line is too long): no restart offers");
        if (c) UnmapViewOfFile(c);
        if (map) CloseHandle(map);
        return;
    }
    c->magic = kRhMagic, c->version = kRhVersion, c->size = sizeof(RestartChannel);
    wcscpy_s(c->savesDir, saves.c_str());
    wcscpy_s(c->cmd, cmd);
    wchar_t line[MAX_PATH + 32];
    swprintf_s(line, L"\"%s\" %lu", host, GetCurrentProcessId());
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(host, line, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {  // the game's folder and environment
        LOG_WARNING("[RestartHost] ts3wrap.exe did not start (error " + std::to_string(GetLastError()) + "): no restart offers");
        UnmapViewOfFile(c);
        CloseHandle(map);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g_chan = c;  // the mapping stays open for the session
    LOG_INFO("[RestartHost] ts3wrap.exe started: a normal quit after a save under 320 MB of largest free block offers a restart into that save");
}

void AfterSave(bool ok, uint64_t largestBytes) {
    if (!g_chan || !ok) return;  // a failed save keeps the previous answer: the save on disk is the last good one
    const bool low = largestBytes < kLowBytes;
    g_chan->largestMB = static_cast<int32_t>(largestBytes >> 20);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_chan->restart), low ? 1 : 0);
    char line[128];
    std::snprintf(line, sizeof line, "[RestartHost] World save: largest free block %llu MB, restart %s at quit", static_cast<unsigned long long>(largestBytes >> 20),
                  low ? "offered" : "not offered");
    LOG_INFO(line);
}

} // namespace RestartHost
