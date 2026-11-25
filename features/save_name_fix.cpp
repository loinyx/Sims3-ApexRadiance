// Game fix: the name of a save opened with -worldFile (see save_name_fix.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "save_name_fix.h"
#include "apex_log.h"
#include "save_name_patch.h"
#include "world_session.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <format>
#include <string>
#include <vector>

namespace SaveNameFix {
namespace {

constexpr size_t kChunk = 1u << 20;
constexpr size_t kAround = 6u << 20; // the tokens are in the same assembly (14 MB), near the method (7.4 KB away in 1.67.2)
constexpr ULONGLONG kGiveUpMs = 300'000;
constexpr ULONGLONG kQuietMs = 10'000; // after the last copy: the ones seen came within 1.2 s of the first

bool Wanted() {
    std::wstring cmd = GetCommandLineW();
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    const size_t at = cmd.find(L"-worldfile:");
    return at != std::wstring::npos && cmd.find(L".sims3", at) != std::wstring::npos;
}

// what can be read of it (another thread may free it meanwhile)
size_t Read(uintptr_t at, void* to, size_t size) {
    SIZE_T got = 0;
    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(at), to, size, &got);
    return got;
}
bool Write(uintptr_t at, const void* from, size_t size) {
    SIZE_T done = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(at), from, size, &done) && done == size;
}

// low, high: the readable range it is in
bool PatchAt(uintptr_t startup, uintptr_t low, uintptr_t high, std::string* why) {
    const uintptr_t from = (std::max)(low, startup - (std::min)(startup, static_cast<uintptr_t>(kAround))), to = (std::min)(high, startup + kAround);
    std::vector<uint8_t> around(to - from);
    around.resize(Read(from, around.data(), around.size()));
    const size_t tokens = SaveNamePatch::FindOnly(around.data(), around.size(), SaveNamePatch::kTokensSize, 0x28, SaveNamePatch::IsTokens);
    if (tokens == around.size()) {
        *why = "the calls it needs were not found beside the script";
        return false;
    }
    uint8_t body[SaveNamePatch::kDeadSize], now[SaveNamePatch::kStartupSize];
    SaveNamePatch::Build(around.data() + tokens, body);
    if (Read(startup, now, sizeof now) != sizeof now || !SaveNamePatch::IsStartup(now)) {
        *why = "the script changed meanwhile";
        return false;
    }
    // the block first (nothing reaches it yet), then the branch to it
    if (!Write(startup + SaveNamePatch::kDead, body, sizeof body) || !Write(startup + SaveNamePatch::kBranch, &SaveNamePatch::kNewBranch, 4)) {
        *why = "the script could not be written";
        return false;
    }
    return true;
}

// one pass over the process's own data: how many copies of the script were completed
int Pass(std::vector<uint8_t>& buf, std::string* why) {
    static constexpr uint8_t kAnchor[SaveNamePatch::kAnchor] = {0x06, 0x11, 0x09, 0x11, 0x08, 0x17, 0x28};
    int done = 0;
    const uintptr_t mine = reinterpret_cast<uintptr_t>(buf.data());
    MEMORY_BASIC_INFORMATION m{};
    for (uintptr_t at = 0x10000; VirtualQuery(reinterpret_cast<LPCVOID>(at), &m, sizeof m) == sizeof m;) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress), end = base + m.RegionSize;
        if (end <= at) break;
        const bool data = m.State == MEM_COMMIT && m.Type == MEM_PRIVATE && !(m.Protect & PAGE_GUARD) && (m.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
        if (data && m.RegionSize >= kChunk && !(mine >= base && mine < end))
            for (uintptr_t p = base; p < end; p += kChunk - SaveNamePatch::kStartupSize) {
                const size_t n = Read(p, buf.data(), (std::min)(kChunk, static_cast<size_t>(end - p)));
                const uint8_t *b = buf.data(), *e = b + n;
                for (const uint8_t* h = b; (h = std::search(h, e, kAnchor, kAnchor + sizeof kAnchor)) != e; h++) {
                    if (static_cast<size_t>(e - h) < SaveNamePatch::kStartupSize) break; // (the next chunk has it whole)
                    if (SaveNamePatch::IsStartup(h) && PatchAt(p + (h - b), base, end, why)) done++;
                }
                if (end - p <= kChunk) break;
            }
        at = end;
    }
    return done;
}

DWORD WINAPI Thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const ULONGLONG start = GetTickCount64();
    std::vector<uint8_t> buf(kChunk);
    int total = 0, passes = 0;
    std::string why = "the script was not found";
    // the script comes in with the game's assemblies, some seconds before the menu state; a copy it is made from is
    // completed too, and the search goes on for the one that runs
    ULONGLONG last = start;
    while (!WorldSession::IsActive() && GetTickCount64() - last < (total ? kQuietMs : kGiveUpMs)) {
        const int n = Pass(buf, &why);
        passes++;
        if (n) LOG_INFO(std::format("[SaveNameFix] On after {} ms ({} in pass {}): the save on the command line keeps its name", GetTickCount64() - start, n, passes));
        if (n) last = GetTickCount64();
        total += n;
        Sleep(100);
    }
    if (total) LOG_INFO(std::format("[SaveNameFix] Ended after {} ms, {} passes", GetTickCount64() - start, passes));
    else LOG_WARNING("[SaveNameFix] Off: " + why);
    return 0;
}

} // namespace

void Start() {
    static bool once = false;
    if (once || !Wanted()) return;
    once = true;
    if (HANDLE t = CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr)) CloseHandle(t);
}

} // namespace SaveNameFix
