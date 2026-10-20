#pragma once
// Address-space owner ledger (numbered development builds only: APEX_DEVELOPMENT_BUILD_FLAVOR).
//
// The address-space monitor (address_space.h) sees how the 4 GB are used, not who asked for them. This ledger hooks the
// lowest user-mode entry points that create or remove an allocation (ntdll NtAllocateVirtualMemory with MEM_RESERVE,
// NtFreeVirtualMemory with MEM_RELEASE, NtMapViewOfSection, NtUnmapViewOfSection) from the init thread (trampolines, written with every other thread suspended) and keeps, for every
// 64 KB allocation base, the first return addresses outside the system DLLs (ntdll, kernelbase, kernel32, the CRTs).
// Heap growth, the game's allocator, the script runtime, DXVK and the graphics driver therefore show up under the module
// and call site that asked. A fixed table indexed by base >> 16 is written lock-free inside the hooks; nothing in a hook
// allocates, logs or takes a lock. Allocations made before the hooks, by the kernel (thread stacks under WOW64) or
// through other paths stay "untracked". Read-only observation: nothing is freed, moved or refused.
#include <cstdint>
#include <string>
#include <windows.h>

namespace VaLedger {

bool Enabled();       // compiled in and hooks installed
void InstallEarly();  // init thread, after the dev switches; no-op outside numbered development builds or with measurements off

// Called by the address-space scan for each allocation it finds (scan thread only).
void BeginScan();
void Account(uintptr_t allocationBase, DWORD type, uint64_t committed, uint64_t reserved, bool hasGuard);
void EndScan(uint64_t uptimeMs); // aggregates; writes the CSV and a log line about once a minute

std::string ReportText(); // latest aggregate for the profiler report ("" when disabled or before the first scan)

} // namespace VaLedger
