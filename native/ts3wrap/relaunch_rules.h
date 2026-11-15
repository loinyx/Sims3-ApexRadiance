#pragma once
// The restart host's relaunch rules that need no running process (ts3wrap.cpp): who started the game, the Steam URL and
// which bridge server belongs to the game that ended. Pure, so tools/restart_host_test checks them.
#include <windows.h>
#include <wchar.h>

// `name` in an environment block (NAME=value\0...\0\0), any case as Windows reads it. `out` null: presence only. False when
// absent or the value does not fit.
inline bool RhEnvGet(const wchar_t* env, const wchar_t* name, wchar_t* out, size_t cap) {
    const size_t len = wcslen(name);
    for (const wchar_t* p = env; p && *p; p += wcslen(p) + 1)
        if (!_wcsnicmp(p, name, len) && p[len] == L'=') return !out || _snwprintf_s(out, cap, _TRUNCATE, L"%s", p + len + 1) >= 0;
    return false;
}

// The Steam app that started the game: SteamAppId, else SteamGameId (Steam sets both for a game it starts). Digits only, at
// most 10 (an app id is 32-bit; a non-Steam shortcut's 64-bit game id is refused), since it goes into a URL.
inline bool RhSteamAppId(const wchar_t* env, wchar_t* out, size_t cap) {
    static const wchar_t* const names[] = {L"SteamAppId", L"SteamGameId"};
    for (const wchar_t* name : names) {
        if (!RhEnvGet(env, name, out, cap)) continue;
        const size_t n = wcslen(out);
        if (n && n <= 10 && out[0] != L'0' && wcsspn(out, L"0123456789") == n) return true;
    }
    return false;
}

// An EA app (or Origin) start: a variable the EA app sets for the game it starts, or the executable under an "EA Games"
// folder (its default library). Unverified on a real EA app install (docs/validation/performance-restart-host.md).
inline bool RhEaStart(const wchar_t* env, const wchar_t* exe) {
    static const wchar_t* const names[] = {L"EALaunchCode", L"ContentId", L"OriginSessionKey"};
    for (const wchar_t* name : names)
        if (RhEnvGet(env, name, nullptr, 0)) return true;
    for (const wchar_t* p = exe; p && *p; p++)
        if (!_wcsnicmp(p, L"\\EA Games\\", 10)) return true;
    return false;
}

// steam://run/<appid>//-worldFile:"<save>": Steam URL-decodes what follows the double slash and passes it to the game, so it
// is percent-encoded as UTF-8 (a space is %20). False for a quote in the name or a result that does not fit.
inline bool RhSteamUrl(const wchar_t* appId, const wchar_t* save, wchar_t* out, size_t cap) {
    wchar_t arg[MAX_PATH + 16];
    char utf8[3 * _countof(arg)];
    if (!save[0] || wcschr(save, L'"') || _snwprintf_s(arg, _TRUNCATE, L"-worldFile:\"%s\"", save) < 0 ||
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, arg, -1, utf8, sizeof utf8, nullptr, nullptr) <= 0)
        return false;
    int k = _snwprintf_s(out, cap, _TRUNCATE, L"steam://run/%s//", appId);
    if (k < 0) return false;
    for (const char* p = utf8; *p; p++) {
        const unsigned char b = static_cast<unsigned char>(*p);
        const bool plain = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '-' || b == '_' || b == '.' || b == '~';
        const int w = plain ? _snwprintf_s(out + k, cap - k, _TRUNCATE, L"%c", b) : _snwprintf_s(out + k, cap - k, _TRUNCATE, L"%%%02X", b);
        if (w < 0) return false;
        k += w;
    }
    return true;
}

// A bridge server of the game that ended: NvRemixBridge.exe started by that game (its bridge client starts it), or running
// from the game's folder (Game\Bin\.trex). `path` is empty when the process could not be queried.
inline bool RhIsOldBridge(const wchar_t* name, DWORD parent, const wchar_t* path, DWORD gamePid, const wchar_t* gameDir) {
    if (_wcsicmp(name, L"NvRemixBridge.exe")) return false;
    const size_t n = wcslen(gameDir);
    return parent == gamePid || (n && !_wcsnicmp(path, gameDir, n) && path[n] == L'\\');
}
