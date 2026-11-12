// ts3wrap.exe: Apex Radiance's restart host (64-bit), adapted from Dalexlol's sims3-modernization wrapper (PRs #360 and
// #363). Apex starts it next to the game when [ui] restart_host = true:
//   ts3wrap.exe <game pid>
// It waits for the game to end. When the game's last world save left its 32-bit address space low (restart_channel.h)
// and the game quit normally, it asks the player whether to start the game again with that save, then starts the same
// executable from the same folder, with the same command line and environment, plus EA's -worldFile switch. It never
// kills or hosts the game window, and it ends after that one decision; the relaunched game starts its own host.
#include "restart_channel.h"

static const wchar_t kTitle[] = L"The Sims 3";

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
    c.exe[MAX_PATH - 1] = c.savesDir[MAX_PATH - 1] = c.cmd[_countof(c.cmd) - 1] = 0;
    if (!RhShouldOffer(code, c)) return 0;
    wchar_t save[MAX_PATH], text[512];
    if (!RhNewestSave(c.savesDir, save, MAX_PATH)) return 0;
    swprintf_s(text, L"The game was low on memory when you last saved (%ld MB left).\n\nStart The Sims 3 again with \"%s\"?", (long)c.largestMB, save);
    if (MessageBoxW(nullptr, text, kTitle, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) return 0;
    static wchar_t cmd[_countof(c.cmd) + MAX_PATH + 16];
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    // the game started this host from its own start folder and environment: inherited, so the relaunch has both
    if (!RhBuildCommand(c.cmd, save, cmd, _countof(cmd)) || !CreateProcessW(c.exe, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        MessageBoxW(nullptr, L"The game could not be started again. Your game was saved: start The Sims 3 as usual.", kTitle, MB_OK | MB_ICONWARNING);
        return 4;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
