#include "../../features/address_space.cpp"
#include <cassert>
#include <iostream>

namespace ApexLog { void Write(Level, const std::string&, const std::source_location&) noexcept {} }
namespace ApexPaths {
bool EnsureApexDirectory() { return true; }
const std::wstring& ApexDirectory() {
    static std::wstring s = [] { wchar_t p[MAX_PATH]{}; GetCurrentDirectoryW(MAX_PATH, p); return std::wstring(p) + L"\\"; }();
    return s;
}
}
namespace GameAddr { bool Resolved() { return false; } uintptr_t Get(Id) { return 0; } }
// The development-build ledgers are observed separately; here they stay off.
namespace VaLedger {
bool Enabled() { return false; }
void BeginScan() {}
void Account(uintptr_t, DWORD, uint64_t, uint64_t, bool) {}
void EndScan(uint64_t) {}
std::string ReportText() { return {}; }
}
namespace GameAllocCategories { void Report(unsigned long long) {} std::string ReportText() { return {}; } }
GameVersion g_gameVersion = GameVersion::Unknown;
uint32_t g_exeTimestamp = 0;

// The game's domains: two allocators with core rings and the script heap's globals, at addresses of this process.
void CheckGameDomains() {
    const auto put = [](uintptr_t a, uint32_t v) { *reinterpret_cast<uint32_t*>(a) = v; };
    static uint8_t first[0x500], second[0x500], cores[3][0x20];
    static uint32_t globals[5];
    const auto a = reinterpret_cast<uintptr_t>(first), b = reinterpret_cast<uintptr_t>(second);
    const uintptr_t c0 = reinterpret_cast<uintptr_t>(cores[0]), c1 = reinterpret_cast<uintptr_t>(cores[1]), c2 = reinterpret_cast<uintptr_t>(cores[2]);
    const auto at = [&](int i) { return reinterpret_cast<uintptr_t>(&globals[i]); };
    const AddressSpace::GameGlobals g0{{at(0), at(1)}, at(2), at(3), at(4)};
    put(at(0), uint32_t(a)), put(at(1), uint32_t(b));
    put(a + 0x460, uint32_t(c0)), put(c0 + 4, 16u << 20), put(c0 + 0x18, uint32_t(c1)), put(c1 + 4, 4u << 20), put(c1 + 0x18, uint32_t(a + 0x448));
    put(b + 0x460, uint32_t(c2)), put(c2 + 4, 1u << 20), put(c2 + 0x18, uint32_t(b + 0x448));
    put(at(2), 40u << 20), put(at(3), 10u << 20), put(at(4), 3);
    AddressSpace::GameDomains g;
    AddressSpace::ReadGameDomains(g, g0);
    assert(g.cores && g.coreCount[0] == 2 && g.coreCommit[0] == (20u << 20) && g.coreCount[1] == 1 && g.coreCommit[1] == (1u << 20));
    assert(g.script && g.scriptHeap == (40u << 20) && g.scriptFree == (10u << 20) && g.scriptSections == 3);
    assert(AddressSpace::GameLineOf(g) == "game allocator cores 2 = 20 MB committed, second allocator cores 1 = 1 MB; script heap 40 MB in 3 sections, 10 MB of it free blocks");
    put(at(1), uint32_t(a)); // the same object twice is counted once
    AddressSpace::GameDomains same;
    AddressSpace::ReadGameDomains(same, g0);
    assert(same.cores && same.coreCount[0] == 2 && same.coreCount[1] == 0);
    put(c1 + 0x18, 0x00001000); // a ring broken by a concurrent change is dropped, not followed
    AddressSpace::GameDomains broken;
    AddressSpace::ReadGameDomains(broken, g0);
    assert(!broken.cores && broken.script);
    put(at(3), 50u << 20); // free over size: not a consistent read
    AddressSpace::GameDomains torn;
    AddressSpace::ReadGameDomains(torn, g0);
    assert(!torn.script);
    std::cout << "Game domains: core rings, the second instance, a broken ring and the script heap passed\n";
}

int main() {
    kPublicBuild=false;
    SetLastError(1234);
    MemoryTelemetry::Attempt(1024, true, 0x123456, 0, MEM_COMMIT);
    MemoryTelemetry::Attempt(4u<<20, false, 0x123456, ERROR_NOT_ENOUGH_MEMORY, MEM_RESERVE);
    assert(GetLastError()==1234);
    assert(MemoryTelemetry::calls[0].load()==1 && MemoryTelemetry::failures[3].load()==1);
    MemoryTelemetry::Mark(2);
    MemoryTelemetry::Mark(4);
    MemoryTelemetry::Event ev[64]{};
    assert(MemoryTelemetry::Drain(ev)==3);
    assert(ev[0].kind==1 && ev[0].size==(4u<<20) && ev[0].caller==0x123456);
    for (int i=0;i<70;++i) MemoryTelemetry::Mark(2);
    assert(MemoryTelemetry::dropped.load()==6 && MemoryTelemetry::Drain(ev)==64);
    assert(MemoryTelemetry::Drain(ev)==0);
    auto before = AddressSpace::Take();
    assert(before.scanComplete && before.processValid && before.systemValid && before.ioValid);
    assert(before.freeLow + before.freeHigh == before.free);
    assert(before.largest <= before.free && before.largestLow <= before.freeLow && before.largestHigh <= before.freeHigh);
    constexpr size_t reserve = 64u << 20, commit = 8u << 20;
    void* p = VirtualAlloc(nullptr, reserve, MEM_RESERVE, PAGE_READWRITE);
    assert(p);
    auto reserved = AddressSpace::Take();
    const auto totalReserved = [](const auto& s) { return s.reserved[0]+s.reserved[1]+s.reserved[2]; };
    assert(totalReserved(reserved) >= totalReserved(before) + (60u << 20));
    assert(VirtualAlloc(p, commit, MEM_COMMIT, PAGE_READWRITE) == p);
    memset(p, 1, commit);
    auto committed = AddressSpace::Take();
    assert(committed.privateBytes >= before.privateBytes + (6u << 20));
    assert(committed.committed[2] >= reserved.committed[2] + (6u << 20));
    assert(VirtualFree(p, 0, MEM_RELEASE));
    auto after = AddressSpace::Take();
    assert(totalReserved(after) + (50u << 20) < totalReserved(reserved));
    assert(after.scanComplete && after.freeLow + after.freeHigh == after.free);
    AddressSpace::WriteMemorySample(before);
    AddressSpace::WriteMemorySample(reserved);
    AddressSpace::WriteMemorySample(committed);
    AddressSpace::WriteMemorySample(after);
    MemoryTelemetry::Mark(2);
    AddressSpace::WriteGameplayTelemetry(after);
    CheckGameDomains();
    std::cout << "Memory observer: reserve, commit, release, partitions and process/system queries passed\n";
}
