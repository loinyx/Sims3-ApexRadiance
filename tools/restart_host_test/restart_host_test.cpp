// The restart host's decision, relaunch command line and save pick (native/ts3wrap/restart_channel.h), and its relaunch
// rules: Steam and EA app starts, the Steam URL, the bridge wait (relaunch_rules.h). Writes three small files under the
// given folder.
#include "restart_channel.h"
#include "relaunch_rules.h"
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
    Check(RhShouldOffer(0, c) && !RhShouldOffer(1, c), "offer after a low save and a normal exit, not after End task");
    Check(!RhShouldOffer(0xC0000005u, c), "no offer after a crash");
    c.exe[0] = 0;
    Check(!RhShouldOffer(0, c), "no offer without the executable");
    wchar_t name[64];
    Check(RhName(name, 64, 1234) && !wcscmp(name, L"Local\\ApexRestartHost-1234"), "channel name");

    const wchar_t steamEnv[] = L"=C:=C:\\\0PATH=C:\\W\0SteamGameId=47890\0SteamAppId=47890\0\0";
    const wchar_t eaEnv[] = L"PATH=C:\\W\0EALaunchCode=x\0\0";
    const wchar_t plainEnv[] = L"PATH=C:\\W\0SteamAppIdX=1\0\0";
    wchar_t app[16], url[512];
    Check(RhSteamAppId(steamEnv, app, 16) && !wcscmp(app, L"47890"), "Steam start: app id from SteamAppId");
    Check(RhSteamAppId(L"steamgameid=47890\0\0", app, 16) && !wcscmp(app, L"47890"), "Steam start: SteamGameId alone, any case");
    Check(!RhSteamAppId(plainEnv, app, 16) && !RhSteamAppId(L"SteamAppId=47890&x\0\0", app, 16) &&
              !RhSteamAppId(L"SteamGameId=12345678901234567890\0\0", app, 16) && !RhSteamAppId(L"SteamAppId=\0\0", app, 16),
          "no Steam start: absent, not digits, a shortcut's 64-bit id, empty");
    Check(RhEaStart(eaEnv, L"C:\\G\\TS3W.exe") && RhEaStart(L"contentid=1\0\0", L"") &&
              RhEaStart(plainEnv, L"C:\\Program Files\\EA Games\\The Sims 3\\Game\\Bin\\TS3W.exe"),
          "EA app start: by its variables or its library folder");
    Check(!RhEaStart(steamEnv, L"C:\\Steam\\steamapps\\common\\The Sims 3\\Game\\Bin\\TS3W.exe") && !RhEaStart(plainEnv, L"C:\\MyEA Games\\TS3W.exe"),
          "no EA app start for Steam or a look-alike folder");
    Check(RhSteamUrl(L"47890", L"My Town.sims3", url, 512) && !wcscmp(url, L"steam://run/47890//-worldFile%3A%22My%20Town.sims3%22"),
          "Steam URL: arguments after the double slash, percent-encoded");
    Check(RhSteamUrl(L"47890", L"Caf\u00e9.sims3", url, 512) && !wcscmp(url, L"steam://run/47890//-worldFile%3A%22Caf%C3%A9.sims3%22"),
          "Steam URL: a non-ASCII name as UTF-8");
    Check(!RhSteamUrl(L"47890", L"Bad\".sims3", url, 512) && !RhSteamUrl(L"47890", L"A.sims3", url, 30), "Steam URL: a quote or overflow refused");

    const wchar_t* dir = L"C:\\G\\Game\\Bin";
    struct { const wchar_t* name; DWORD parent; const wchar_t* path; bool old; } procs[] = {
        {L"NvRemixBridge.exe", 1234, L"", true},                                            // started by the old game, path unread
        {L"nvremixbridge.exe", 4, L"C:\\G\\Game\\Bin\\.trex\\NvRemixBridge.exe", true},     // reparented, in the game's folder
        {L"NvRemixBridge.exe", 4, L"C:\\Other\\.trex\\NvRemixBridge.exe", false},           // another game's bridge
        {L"NvRemixBridge.exe", 4, L"C:\\G\\Game\\Bin2\\NvRemixBridge.exe", false},          // a folder that only starts the same
        {L"TS3W.exe", 1234, L"C:\\G\\Game\\Bin\\TS3W.exe", false},                          // not a bridge server
    };
    bool bridgeOk = true;
    for (const auto& p : procs) bridgeOk &= RhIsOldBridge(p.name, p.parent, p.path, 1234, dir) == p.old;
    Check(bridgeOk, "bridge wait: only the old game's NvRemixBridge.exe, by parent or folder");

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
