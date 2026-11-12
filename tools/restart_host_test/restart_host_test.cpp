// The restart host's decision, relaunch command line and save pick (native/ts3wrap/restart_channel.h). Writes three small
// files under the given folder.
#include "restart_channel.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

static std::wstring Cmd(const wchar_t* cmd, const wchar_t* save, size_t cap = 512) {
    wchar_t out[512];
    return RhBuildCommand(cmd, save, out, cap) ? out : L"<false>";
}

static void Touch(const std::wstring& dir, const std::wstring& file, uint64_t ft) {
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE h = CreateFileW((dir + L"\\" + file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    FILETIME t{static_cast<DWORD>(ft), static_cast<DWORD>(ft >> 32)};
    SetFileTime(h, nullptr, nullptr, &t);
    CloseHandle(h);
}

int wmain(int argc, wchar_t** argv) {
    Check(Cmd(L"\"C:\\G\\TS3W.exe\"", L"Pleasantview.sims3") == L"\"C:\\G\\TS3W.exe\" -worldFile:\"Pleasantview.sims3\"", "switch appended");
    Check(Cmd(L"\"C:\\G\\TS3W.exe\" -x -worldFile:\"Old.sims3\"", L"New.sims3") == L"\"C:\\G\\TS3W.exe\" -x -worldFile:\"New.sims3\"",
          "a relaunch's own quoted -worldFile replaced");
    Check(Cmd(L"TS3W.exe -WORLDFILE:Old.sims3 -y", L"New.sims3") == L"TS3W.exe -y -worldFile:\"New.sims3\"", "unquoted switch in the middle replaced, any case");
    Check(Cmd(L"TS3W.exe -noworldFile:x", L"A.sims3") == L"TS3W.exe -noworldFile:x -worldFile:\"A.sims3\"", "only a whole switch is removed");
    Check(Cmd(L"TS3W.exe", L"Bad\".sims3") == L"<false>", "a save name with a quote refused");
    Check(Cmd(L"TS3W.exe", L"") == L"<false>", "an empty save name refused");
    Check(Cmd(L"TS3W.exe", L"A.sims3", 12) == L"<false>", "a result that does not fit refused");

    static RestartChannel c{};
    c.magic = kRhMagic, c.version = kRhVersion, c.size = sizeof c;
    Check(RhValid(&c, sizeof c) && !RhValid(&c, sizeof c - 1), "block checked by magic, version and mapped size");
    wcscpy_s(c.exe, L"TS3W.exe"), wcscpy_s(c.cmd, L"TS3W.exe");
    Check(!RhShouldOffer(0, c), "no offer without a low save");
    c.restart = 1;
    Check(RhShouldOffer(0, c) && RhShouldOffer(1, c), "offer after a low save and a normal exit");
    Check(!RhShouldOffer(0xC0000005u, c), "no offer after a crash");
    c.exe[0] = 0;
    Check(!RhShouldOffer(0, c), "no offer without the executable");
    wchar_t name[64];
    Check(RhName(name, 64, 1234) && !wcscmp(name, L"Local\\ApexRestartHost-1234"), "channel name");

    const std::wstring root = argc > 1 ? argv[1] : L".";
    const std::wstring saves = root + L"\\Saves";
    CreateDirectoryW(saves.c_str(), nullptr);
    const uint64_t day = 864000000000ull, base = 133000000000000000ull;
    Touch(saves + L"\\Old.sims3", L"a.nhd", base);
    Touch(saves + L"\\New.sims3", L"b.nhd", base + day);
    Touch(saves + L"\\New.sims3.backup", L"c.nhd", base + 2 * day);  // a backup is never the save
    wchar_t pick[MAX_PATH];
    Check(RhNewestSave(saves.c_str(), pick, MAX_PATH) && !wcscmp(pick, L"New.sims3"), "newest save picked by its files, backups ignored");
    Check(!RhNewestSave((root + L"\\none").c_str(), pick, MAX_PATH), "no saves folder: no pick");
    return g_fail ? 1 : 0;
}
