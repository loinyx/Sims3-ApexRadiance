#pragma once
// Bounded observation of existing native allocation hooks. Credits: @loinyx
#include "build_flavor.h"
#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <cstdint>
namespace MemoryTelemetry {
inline std::atomic<bool> active{false};
inline std::atomic<uint64_t> allocationFailureTick{0};
inline std::atomic<bool> pressure{false};
inline std::atomic<unsigned> saveDepth{0};
struct SaveScope {
    SaveScope() noexcept {saveDepth.fetch_add(1,std::memory_order_acq_rel);}
    ~SaveScope(){saveDepth.fetch_sub(1,std::memory_order_acq_rel);}
    SaveScope(const SaveScope&)=delete;
    SaveScope& operator=(const SaveScope&)=delete;
};
inline std::atomic<uint64_t> calls[6]{}, requested[6]{}, failures[6]{}, dropped{0};
struct Event {
    uint64_t tick = 0, size = 0, privateBytes = 0, workingSet = 0;
    uintptr_t caller = 0;
    DWORD thread = 0, error = 0, type = 0;
    unsigned kind = 0; // 1: allocation failure, 2: save start, 3: save success, 4: save failure
};
inline SRWLOCK lock = SRWLOCK_INIT;
inline Event events[64]{};
inline unsigned count = 0;
inline unsigned ClassOf(uint64_t n) noexcept {
    const uint64_t limits[] = {64ull<<10, 1ull<<20, 4ull<<20, 16ull<<20, 64ull<<20};
    unsigned i = 0; while (i < 5 && n >= limits[i]) ++i; return i;
}
inline void Mark(unsigned kind, uint64_t size = 0, uintptr_t caller = 0, DWORD error = 0, DWORD type = 0) noexcept {
    if constexpr (kDevToolsBuild || kMemoryOptimizationsEnabled) {
    if (kPublicBuild && kind!=1) return;
    const DWORD savedError = GetLastError();
    Event e{}; e.tick = GetTickCount64(); e.size = size; e.caller = caller;
    e.thread = GetCurrentThreadId(); e.error = error; e.type = type; e.kind = kind;
    PROCESS_MEMORY_COUNTERS_EX m{}; m.cb = sizeof m;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof m)) {
        e.privateBytes = m.PrivateUsage; e.workingSet = m.WorkingSetSize;
    }
    if (TryAcquireSRWLockExclusive(&lock)) {
        if (count < 64) events[count++] = e; else dropped.fetch_add(1, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&lock);
    } else dropped.fetch_add(1, std::memory_order_relaxed);
    SetLastError(savedError);
    }
}
inline void Attempt(uint64_t size, bool success, uintptr_t caller, DWORD error, DWORD type) noexcept {
    if constexpr (kMemoryOptimizationsEnabled) {
        if (!success) {
            allocationFailureTick.store(GetTickCount64(),std::memory_order_relaxed);
            if (kPublicBuild) Mark(1,size,caller,error,type);
        }
    }
    if constexpr (kDevToolsBuild) {
    if(kPublicBuild) return;
    const unsigned c = ClassOf(size);
    calls[c].fetch_add(1, std::memory_order_relaxed);
    requested[c].fetch_add(size, std::memory_order_relaxed);
    if (!success) {
        failures[c].fetch_add(1, std::memory_order_relaxed);
        Mark(1, size, caller, error, type);
    }
    }
}
inline unsigned Drain(Event* out) noexcept {
    AcquireSRWLockExclusive(&lock);
    const unsigned n = count;
    for (unsigned i=0; i<n; ++i) out[i] = events[i];
    count = 0;
    ReleaseSRWLockExclusive(&lock);
    return n;
}
}
