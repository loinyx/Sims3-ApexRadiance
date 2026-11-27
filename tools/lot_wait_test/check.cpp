// Lot wait (features/lot_wait.cpp) offline checks, x86 only:
//   1. the hooks with stand-in originals: a start opens a wait, its done closes it, the original gets every argument and
//      its result back; KeepFullBudget is true only while a wait is open and the switch is on; a full table and a done
//      without a start change nothing; a wait past the ceiling no longer keeps the budget;
//   2. --exe <TS3W.exe>: the start and done signatures match once each.
//   check.exe [--exe <TS3W.exe>]
#include "../../features/lot_wait.cpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace ApexLog { void Write(Level, const std::string&, const std::source_location&) noexcept {} }
namespace MemPatch {
bool GetModuleInfo(HMODULE, BYTE**, size_t*) { return false; }
uintptr_t ScanPattern(const BYTE*, size_t, const char*) { return 0; }
}
namespace DetourBatch { bool InstallHooks(const std::vector<Hook>&) { return false; } }
namespace HookChain { std::string AddressText(const void*) { return {}; } }

static int g_fail = 0;
static void Expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    g_fail += !ok;
}

static uint32_t g_args[4], g_doneResult;
static void* g_doneWaiter;
static uint64_t __cdecl FakeStart(void* d, uint32_t lo, uint32_t hi, uint32_t f) {
    g_args[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(d)), g_args[1] = lo, g_args[2] = hi, g_args[3] = f;
    return 0x1122334455667788ull;
}
static void __fastcall FakeDoneBody(void* w, uint32_t r) { g_doneWaiter = w, g_doneResult = r; }
static void __declspec(naked) FakeDone() { // thiscall(waiter, result), ret 4
    __asm {
        mov edx, [esp + 4]
        call FakeDoneBody
        ret 4
    }
}

struct Waiter { uint8_t pad[0x18] = {}; uint32_t lotLo, lotHi, flags; };

static void CallDone(Waiter& w, uint32_t result) { // as the lot check calls done: thiscall through the hook
    using Done = void(__thiscall*)(void*, uint32_t);
    uint32_t espBefore, espAfter;
    __asm mov espBefore, esp
    reinterpret_cast<Done>(&HookDone)(&w, result);
    __asm mov espAfter, esp
    Expect(espBefore == espAfter, "done: stack balanced after the call");
}

static int CountMatches(const std::vector<uint8_t>& exe, const char* sig) {
    std::vector<int> p;
    for (const char* s = sig; *s;) {
        while (*s == ' ') ++s;
        if (!*s) break;
        p.push_back(s[0] == '?' ? -1 : static_cast<int>(std::strtoul(std::string(s, 2).c_str(), nullptr, 16)));
        s += 2;
    }
    int n = 0;
    for (size_t i = 0; i + p.size() <= exe.size(); ++i) {
        size_t j = 0;
        while (j < p.size() && (p[j] < 0 || exe[i + j] == p[j])) ++j;
        n += j == p.size();
    }
    return n;
}

int main(int argc, char** argv) {
    static_assert(offsetof(Waiter, lotLo) == 0x18 && offsetof(Waiter, flags) == 0x20);
    g_start = &FakeStart;
    g_done = reinterpret_cast<DoneFn>(&FakeDone);
    Expect(!LotWait::KeepFullBudget(), "no wait open: the cuts run");

    const uint64_t r = HookStart(reinterpret_cast<void*>(0x1234), 0xAAAA0001, 0xBBBB0002, 0x1200000);
    Expect(r == 0x1122334455667788ull && g_args[0] == 0x1234 && g_args[1] == 0xAAAA0001 && g_args[2] == 0xBBBB0002 && g_args[3] == 0x1200000,
           "start: the original gets every argument and its edx:eax result is returned");
    Expect(LotWait::KeepFullBudget(), "a wait open: the game's budget");
    PerfToggles::LotWaitFullBudget = false;
    Expect(!LotWait::KeepFullBudget(), "switch off: the cuts run while a wait is open");
    PerfToggles::LotWaitFullBudget = true;

    Waiter other{{}, 0xAAAA0001, 0xBBBB0002, 0};
    CallDone(other, 1);
    Expect(LotWait::KeepFullBudget(), "a done of another key leaves the wait open");
    Expect(g_doneWaiter == &other && g_doneResult == 1, "done: the original gets the waiter and the result");
    Waiter w{{}, 0xAAAA0001, 0xBBBB0002, 0x1200000};
    CallDone(w, 1);
    Expect(!LotWait::KeepFullBudget(), "its done closes the wait");

    for (int i = 0; i < LotWaitLogic::kSlots + 2; ++i) HookStart(nullptr, i, 0, 0);
    Expect(g_open.load() == LotWaitLogic::kSlots, "a full table tracks kSlots waits and drops the rest");
    for (int i = 0; i < LotWaitLogic::kSlots + 2; ++i) { Waiter x{{}, static_cast<uint32_t>(i), 0, 0}; CallDone(x, 0); }
    Expect(g_open.load() == 0 && !LotWait::KeepFullBudget(), "every tracked wait closes; untracked dones change nothing");

    HookStart(nullptr, 7, 0, 0);
    HookStart(nullptr, 7, 0, 0);
    Waiter same{{}, 7, 0, 0};
    CallDone(same, 1);
    Expect(g_open.load() == 1, "two waits with the same key close one at a time");
    CallDone(same, 1);
    Expect(g_open.load() == 0, "and the second closes too");

    LotWaitLogic::Table t;
    t.Open(1, 0, 1000);
    t.Open(2, 0, 50000);
    Expect(t.OldestStart() == 1000, "oldest start");
    t.Expire(1000 + LotWaitLogic::kCeilingMs);
    Expect(t.Count() == 2, "a wait at the ceiling stays open");
    t.Expire(1001 + LotWaitLogic::kCeilingMs);
    Expect(t.Count() == 1 && t.OldestStart() == 50000, "a wait past the ceiling is freed, the newer one stays");
    // through KeepFullBudget: a wait started 'ceiling + 1 s' ago
    AcquireSRWLockExclusive(&g_lock);
    g_table.Open(9, 0, GetTickCount64() - LotWaitLogic::kCeilingMs - 1000);
    Mirror();
    ReleaseSRWLockExclusive(&g_lock);
    Expect(!LotWait::KeepFullBudget() && g_open.load() == 0, "a wait past the ceiling no longer keeps the budget and is freed");

    if (argc == 3 && std::strcmp(argv[1], "--exe") == 0) {
        std::ifstream f(argv[2], std::ios::binary);
        const std::vector<uint8_t> exe((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const int s = CountMatches(exe, kSigStart), d = CountMatches(exe, kSigDone);
        std::printf("start matches: %d, done matches: %d\n", s, d);
        Expect(s == 1 && d == 1, "the start and done signatures match once each in the executable");
    }
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
