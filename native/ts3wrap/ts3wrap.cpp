// ts3wrap.exe: Apex Radiance's restart host (64-bit), adapted from Dalexlol's sims3-modernization wrapper (PRs #360 and
// #363). Apex starts it next to the game when [ui] restart_host = true:
//   ts3wrap.exe <game pid>
// It waits for the game to end. When the game's last world save left its 32-bit address space low (restart_channel.h)
// and the game quit normally, it asks the player whether to start the game again with that save (the folder that save
// wrote; the newest save only when the game did not name one), writes which one to ts3wrap_LOG.txt, then starts the same
// executable from the same folder, with the same command line and environment, plus EA's -worldFile switch. It never
// kills or hosts the game window, and it ends after that one decision; the relaunched game starts its own host. A relaunch
// that ends within 15 s without a crash, and is not followed by the game starting again within 10 s, is retried once through
// Steam (steam://run) for a Steam start, or the player is told to start the game from the EA app for an EA app start.
#include "restart_channel.h"
#include "relaunch_rules.h"
#include <shellapi.h>
#include <tlhelp32.h>

static const wchar_t kTitle[] = L"The Sims 3";
static const wchar_t kFailed[] = L"The game could not be started again. Your game was saved: start The Sims 3 as usual.";

// True when `f` matches one of the running processes.
template <class F> static bool AnyProcess(F f) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{sizeof e};
    bool hit = false;
    for (BOOL ok = Process32FirstW(s, &e); ok && !hit; ok = Process32NextW(s, &e)) hit = f(e);
    CloseHandle(s);
    return hit;
}

// One line per decision, appended to ts3wrap_LOG.txt beside the Saves folder (the game's documents folder).
static void HostLog(const RestartChannel& c, RhRule rule, const wchar_t* save) {
    wchar_t path[MAX_PATH + 32], line[3 * MAX_PATH];
    wcscpy_s(path, c.savesDir);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, _countof(path) - (slash + 1 - path), L"ts3wrap_LOG.txt");
    SYSTEMTIME t;
    GetLocalTime(&t);
    static const wchar_t* const how[] = {L"none: no save folder found", L"the save the game wrote", L"the newest save (the game named none, or its folder is gone)"};
    const int n = swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u restart offer: \"%s\", rule %s; named by the game: \"%s\"\r\n", t.wYear, t.wMonth, t.wDay,
                             t.wHour, t.wMinute, t.wSecond, save, how[rule], c.save);
    char utf8[4 * _countof(line)];
    const int k = n > 0 ? WideCharToMultiByte(CP_UTF8, 0, line, n, utf8, sizeof utf8, nullptr, nullptr) : 0;
    HANDLE h = k > 0 ? CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, utf8, static_cast<DWORD>(k), &w, nullptr);
    CloseHandle(h);
}

static bool OldBridgeRunning(DWORD gamePid, const wchar_t* gameDir) {
    return AnyProcess([&](const PROCESSENTRY32W& e) {
        if (_wcsicmp(e.szExeFile, L"NvRemixBridge.exe")) return false;  // only a bridge server is worth opening
        wchar_t path[MAX_PATH] = L"";
        DWORD n = MAX_PATH;
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID)) {
            if (!QueryFullProcessImageNameW(h, 0, path, &n)) path[0] = 0;
            CloseHandle(h);
        }
        return RhIsOldBridge(e.szExeFile, e.th32ParentProcessID, path, gamePid, gameDir);
    });
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR args, int) {
    const DWORD pid = wcstoul(args, nullptr, 10);
    HANDLE game = pid ? OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!game) return 2;
    wchar_t name[64];
    HANDLE map = RhName(name, 64, pid) ? OpenFileMappingW(FILE_MAP_READ, FALSE, name) : nullptr;  // kept open past the game's end
    const auto* chan = map ? static_cast<const RestartChannel*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0)) : nullptr;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!chan || !VirtualQuery(chan, &mbi, sizeof mbi) || !RhValid(chan, mbi.RegionSize)) return 3;
    WaitForSingleObject(game, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(game, &code);
    CloseHandle(game);
    static RestartChannel c;  // a copy, terminated: the block is shared memory
    c = *chan;
    c.exe[MAX_PATH - 1] = c.savesDir[MAX_PATH - 1] = c.cmd[_countof(c.cmd) - 1] = c.save[MAX_PATH - 1] = 0;
    if (!RhShouldOffer(code, c)) return 0;
    wchar_t save[MAX_PATH] = L"", text[512];
    const RhRule rule = RhPickSave(c, save, MAX_PATH);
    HostLog(c, rule, save);
    if (rule == kRhNoSave) return 0;
    swprintf_s(text, L"The game was low on memory when you last saved (%ld MB left).\n\nStart The Sims 3 again with \"%s\"?", (long)c.largestMB, save);
    if (MessageBoxW(nullptr, text, kTitle, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) return 0;
    static wchar_t gameDir[MAX_PATH], cmd[_countof(c.cmd) + MAX_PATH + 16];
    wcscpy_s(gameDir, c.exe);
    wchar_t* exeName = wcsrchr(gameDir, L'\\');
    if (!exeName) return MessageBoxW(nullptr, kFailed, kTitle, MB_OK | MB_ICONWARNING), 4;
    *exeName++ = 0;
    // the new game's bridge client must not meet the old bridge server still shutting down; never killed, at most 20 s
    for (int i = 0; i < 80 && OldBridgeRunning(pid, gameDir); i++) Sleep(250);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    // Apex starts this host in the game's start folder (read when the ASI loaded) with the game's environment: null folder and
    // environment here pass both on, so DXVK finds the same dxvk.conf and DXVK_* variables, the bridge server starts in the
    // same folder, and d3d9.dll still comes from the exe's own folder
    if (!RhBuildCommand(c.cmd, save, cmd, _countof(cmd)) || !CreateProcessW(c.exe, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        MessageBoxW(nullptr, kFailed, kTitle, MB_OK | MB_ICONWARNING);
        return 4;
    }
    CloseHandle(pi.hThread);
    const bool quick = WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0;
    DWORD childCode = 0;
    GetExitCodeProcess(pi.hProcess, &childCode);
    CloseHandle(pi.hProcess);
    if (!quick) return 0;
    // an NTSTATUS error code is a crash, not a launcher handoff: retrying it through Steam would only crash again
    if (childCode >= 0xC0000000) return MessageBoxW(nullptr, kFailed, kTitle, MB_OK | MB_ICONWARNING), 4;
    // a launcher's own handoff (DRM restarting the game through Steam or the EA app) starts the new game a few seconds later
    for (int i = 0; i < 40; i++) {
        if (AnyProcess([&](const PROCESSENTRY32W& e) { return e.th32ProcessID != pi.dwProcessId && !_wcsicmp(e.szExeFile, exeName); }))
            return 0;
        Sleep(250);
    }
    const wchar_t* env = GetEnvironmentStringsW();  // the game's, inherited
    wchar_t app[16], url[2 * MAX_PATH + 64];
    if (RhSteamAppId(env, app, _countof(app)) && RhSteamUrl(app, save, url, _countof(url))) {
        // synchronous and without the shell's own error box, so a missing steam: handler reaches kFailed alone
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW sei{sizeof sei, SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI};
        sei.lpVerb = L"open";
        sei.lpFile = url;
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) return 0;
    }
    if (RhEaStart(env, c.exe)) {
        swprintf_s(text, L"Your game was saved, but it could not be started again from here.\n\nStart The Sims 3 from the EA app and load \"%s\".", save);
        MessageBoxW(nullptr, text, kTitle, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return 5;
    }
    MessageBoxW(nullptr, kFailed, kTitle, MB_OK | MB_ICONWARNING);
    return 4;
}
