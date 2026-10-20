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
    std::cout << "Memory observer: reserve, commit, release, partitions and process/system queries passed\n";
}
