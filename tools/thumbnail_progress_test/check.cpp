#define APEX_LOADING_DIAGNOSTICS
#include "../../framework/thumbnail_progress.cpp"
#include <cassert>
#include <iostream>
#include <vector>

uint32_t g_exeTimestamp = 0;
namespace ApexLog {
void Write(Level, const std::string&, const std::source_location&) noexcept {}
}
unsigned installed = 0;
namespace DetourBatch {
bool InstallHooks(const std::vector<Hook>& hooks) { installed += static_cast<unsigned>(hooks.size()); return true; }
}
std::optional<uintptr_t> GameAddress::Resolve() const {
    const std::string s = name;
    if (s == "Thumbnail progress owner") return 0x006EBB70;
    if (s == "Thumbnail progress readiness") return 0x00AE06B0;
    if (s == "Thumbnail progress lighting") return 0x006B0500;
    if (s == "Thumbnail progress lot pass") return 0x00ADB8F0;
    if (s == "Thumbnail progress call site") return 0x00D5BE29;
    return std::nullopt;
}
alignas(16) BYTE root[0x300]{}, service[0x120]{}, control[0x100]{}, renderer[0x250]{}, manager[0x60]{};
int countdown = 5, roomState = 1, waitState = 0;
unsigned gathers = 0, passes = 0, originals = 0;
bool __fastcall OriginalReady(BYTE* value, void*) {
    assert(value == renderer);
    return waitState == 2;
}
void __fastcall OriginalUpdate(BYTE* value, void*) { assert(value == root); ++originals; }
void __fastcall NativeLighting(BYTE* value, void*, bool animate) {
    assert(value == service && !animate);
    ++gathers;
    if (--countdown == 0) roomState = 2;
}
void __fastcall NativeLotPass(BYTE* value, void*) {
    assert(value == manager);
    ++passes;
    if (roomState == 2) waitState = 2;
}
int main() {
    using namespace ThumbnailProgress;
    Start();
    assert(installed == 0);
    g_exeTimestamp = 0x52DEC247;
    Start();
    assert(installed == 2);

    Waits table;
    auto* w = table.Observe(1, 7, 100, false);
    assert(w && !Due(*w, 1599, true, false, true, false, 0));
    assert(Due(*w, 1600, true, false, true, false, 0));
    for (auto flags : {0, 1, 2, 3})
        assert(!Due(*w, 1600, flags != 0, flags == 1, flags != 2, flags == 3, 0));
    assert(!Due(*w, 1600, true, false, true, false, 1590));
    w->pumped = 1600;
    assert(!Due(*w, 1632, true, false, true, false, 0));
    assert(Due(*w, 1633, true, false, true, false, 0));
    assert(!Due(*w, 30100, true, false, true, false, 0));
    assert(!table.Observe(1, 7, 1700, true));
    assert(table.Observe(1, 8, 1800, false)->since == 1800);
    assert(table.Observe(1, 8, 3000, false)->since == 3000);
    table = {};
    for (unsigned n = 1; n <= 16; ++n) assert(table.Observe(n, n, 100, false));
    assert(!table.Observe(17, 17, 100, false));
    assert(table.Observe(17, 17, 1200, false));

    void* mapped = VirtualAlloc(reinterpret_cast<void*>(0x011C0000), 0x20000,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(mapped == reinterpret_cast<void*>(0x011C0000));
    *reinterpret_cast<BYTE**>(0x011D1860) = root;
    *reinterpret_cast<BYTE**>(0x011CECC8) = control;
    *reinterpret_cast<BYTE**>(root + 0x1C0) = service;
    *reinterpret_cast<uint64_t*>(renderer + 0x10) = 7;
    *reinterpret_cast<BYTE**>(renderer + 0x23C) = manager;
    renderer[0x1E] = manager[0x18] = 1;
    control[0x8D] = 1;
    ready = &OriginalReady;
    update = &OriginalUpdate;
    lighting = reinterpret_cast<Lighting>(&NativeLighting);
    lotLighting = reinterpret_cast<LotLighting>(&NativeLotPass);
    UpdateHook(root, nullptr);
    assert(originals == 1 && ownerThread.load() == GetCurrentThreadId());
    const auto caller = reinterpret_cast<void*>(0x00D5BE34);
    auto arm = [&] {
        waits = {};
        const auto now = GetTickCount64();
        waits.Observe(reinterpret_cast<uintptr_t>(renderer), 7, now - 2000, false)->seen = now;
    };
    arm();
    assert(!Check(renderer, nullptr, nullptr) && gathers == 0);
    recovering.test_and_set();
    assert(!Check(renderer, nullptr, caller) && gathers == 0);
    recovering.clear();
    *reinterpret_cast<BYTE**>(0x011D1860) = nullptr;
    assert(!Check(renderer, nullptr, caller) && gathers == 0);
    *reinterpret_cast<BYTE**>(0x011D1860) = root;
    root[0x290] = 1;
    assert(!Check(renderer, nullptr, caller) && gathers == 0);
    root[0x290] = 0;
    manager[0x4E] = 1;
    assert(!Check(renderer, nullptr, caller) && gathers == 0);
    manager[0x4E] = 0;
    ownerThread.store(GetCurrentThreadId() + 1);
    assert(!Check(renderer, nullptr, caller) && gathers == 0);
    ownerThread.store(GetCurrentThreadId());
    nativeTurn.store(GetTickCount64());
    assert(!Check(renderer, nullptr, caller) && gathers == 0);
    nativeTurn.store(0);
    for (int n = 0; n < 5; ++n) {
        arm();
        const bool result = Check(renderer, nullptr, caller);
        assert(result == (n == 4));
    }
    assert(gathers == 5 && passes == 5 && roomState == 2 && waitState == 2);
    assert(Check(renderer, nullptr, caller) && gathers == 5);
    VirtualFree(mapped, 0, MEM_RELEASE);
    std::cout << "Thumbnail progress: owner thread, pause, native completion, pacing and bounds passed\n";
}
