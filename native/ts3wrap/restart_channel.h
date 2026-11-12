#pragma once
// The restart host's channel: one named file mapping, created by Apex Radiance inside the game (32-bit) and opened by
// ts3wrap.exe (64-bit), so fixed-width fields only (one layout for both builds, checked below). Nothing here is ever
// written into a save. Adapted from Dalexlol's sims3-modernization wrapper, step 1 (PR #363, restartchan.h).
//   Local\ApexRestartHost-<game pid>
// Apex fills exe, cmd and savesDir at the start and sets `restart` after every world save; the host reads the block once
// the game has ended.
#include <windows.h>
#include <stdint.h>
#include <wchar.h>

static const uint32_t kRhMagic = 0x48524158; // "XARH"
static const uint32_t kRhVersion = 1;

struct RestartChannel {
    uint32_t magic, version, size;
    volatile int32_t restart;  // 1: the last world save succeeded with the largest free block under the threshold
    int32_t largestMB;         // the largest free block after that save
    wchar_t exe[MAX_PATH];     // the game's executable
    wchar_t savesDir[MAX_PATH]; // Documents\Electronic Arts\<game folder>\Saves
    wchar_t cmd[2048];         // the game's command line as it started
};
static_assert(sizeof(RestartChannel) == 5 * 4 + 2 * (MAX_PATH * 2 + 2048), "RestartChannel: one layout for the x86 game and the x64 host");

inline bool RhName(wchar_t* out, size_t cap, uint32_t pid) {
    return _snwprintf_s(out, cap, _TRUNCATE, L"Local\\ApexRestartHost-%lu", (unsigned long)pid) > 0;
}

inline bool RhValid(const RestartChannel* c, size_t mapped) {
    return c && mapped >= sizeof(RestartChannel) && c->magic == kRhMagic && c->version == kRhVersion && c->size == sizeof(RestartChannel);
}

// Offer a restart: the game asked for one at its last save and ended normally (an exception code is a crash: no offer).
inline bool RhShouldOffer(uint32_t exitCode, const RestartChannel& c) {
    return c.restart == 1 && exitCode == 0 && c.exe[0] && c.cmd[0]; // (Task Manager's End task gives 1: no offer)
}

// The command line for the relaunch: `cmd` without any -worldFile switch of its own (the game that is ending may itself
// be a relaunch) plus -worldFile:"<save>", EA's switch that loads that save at the first menu. False when `save` holds a
// quote or the result does not fit.
inline bool RhBuildCommand(const wchar_t* cmd, const wchar_t* save, wchar_t* out, size_t cap) {
    if (!cmd || !save || !save[0] || wcschr(save, L'"') || !cap) return false;
    size_t n = 0;
    for (const wchar_t* p = cmd; *p;) {
        if ((p == cmd || p[-1] == L' ') && !_wcsnicmp(p, L"-worldFile:", 11)) {  // skip the switch and its value
            p += 11;
            if (*p == L'"') { p++; while (*p && *p != L'"') p++; if (*p) p++; }
            else while (*p && *p != L' ') p++;
            while (n && out[n - 1] == L' ') n--;
            continue;
        }
        if (n + 1 >= cap) return false;
        out[n++] = *p++;
    }
    while (n && out[n - 1] == L' ') n--;
    out[n] = 0;
    return _snwprintf_s(out + n, cap - n, _TRUNCATE, L" -worldFile:\"%s\"", save) > 0;
}

// The save folder ("<name>.sims3") holding the newest written file: the one the game saved last. False when none.
inline bool RhNewestSave(const wchar_t* savesDir, wchar_t* out, size_t cap) {
    wchar_t pat[MAX_PATH];
    if (_snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%s\\*.sims3", savesDir) < 0) return false;
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(pat, &d);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint64_t best = 0;
    out[0] = 0;
    do {
        const size_t len = wcslen(d.cFileName);
        if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || len < 6 || _wcsicmp(d.cFileName + len - 6, L".sims3")) continue;
        wchar_t inner[MAX_PATH];
        if (_snwprintf_s(inner, MAX_PATH, _TRUNCATE, L"%s\\%s\\*", savesDir, d.cFileName) < 0) continue;
        WIN32_FIND_DATAW f;
        HANDLE g = FindFirstFileW(inner, &f);
        if (g == INVALID_HANDLE_VALUE) continue;
        do {  // the folder's own time misses files rewritten in place: its files' times count
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const uint64_t t = (uint64_t)f.ftLastWriteTime.dwHighDateTime << 32 | f.ftLastWriteTime.dwLowDateTime;
            if (t > best && len < cap) best = t, wcscpy_s(out, cap, d.cFileName);
        } while (FindNextFileW(g, &f));
        FindClose(g);
    } while (FindNextFileW(h, &d));
    FindClose(h);
    return out[0] != 0;
}
