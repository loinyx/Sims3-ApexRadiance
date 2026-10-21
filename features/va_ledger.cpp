// Address-space owner ledger (numbered development builds; see va_ledger.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "va_ledger.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_version.h"
#include "memory_patch.h"
#include "dev_switches.h"
#include <detours/detours.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace VaLedger {

#ifdef APEX_DEVELOPMENT_BUILD_FLAVOR
inline constexpr bool kCompiled = true;
#else
inline constexpr bool kCompiled = false;
#endif

namespace {

constexpr unsigned kFrames = 4;   // kept return addresses outside the system DLLs
constexpr unsigned kWalk = 32;    // frames walked to find them
constexpr int kWriteEvery = 6;    // scans (the monitor scans every 10 s: once a minute)
constexpr size_t kTopSites = 40;

enum Kind : uint8_t { Reserve = 1, ReserveEx = 2, View = 3, ViewEx = 4 };

struct Slot {
    std::atomic<uint32_t> base{0};
    uint32_t size = 0;
    uint32_t frames[kFrames] = {};
    uint8_t kind = 0;
    uint32_t thread = 0;
};
Slot* g_slots = nullptr; // 65536 entries, one per 64 KB allocation granule (VirtualAlloc'd when the ledger turns on)

struct Range {
    uintptr_t lo = 0, hi = 0;
};
Range g_system[12];
unsigned g_systemCount = 0;
DWORD g_pid = 0;
std::atomic<bool> g_enabled{false};
std::atomic<uint32_t> g_recorded{0}, g_released{0};

using NtAllocateVirtualMemory_t = LONG(NTAPI*)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
using NtAllocateVirtualMemoryEx_t = LONG(NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG, ULONG, PVOID, ULONG);
using NtFreeVirtualMemory_t = LONG(NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG);
using NtMapViewOfSection_t = LONG(NTAPI*)(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T, PLARGE_INTEGER, PSIZE_T, DWORD, ULONG, ULONG);
using NtMapViewOfSectionEx_t = LONG(NTAPI*)(HANDLE, HANDLE, PVOID*, PLARGE_INTEGER, PSIZE_T, ULONG, ULONG, PVOID, ULONG);
using NtUnmapViewOfSection_t = LONG(NTAPI*)(HANDLE, PVOID);
using NtUnmapViewOfSectionEx_t = LONG(NTAPI*)(HANDLE, PVOID, ULONG);

NtAllocateVirtualMemory_t o_alloc = nullptr;
NtAllocateVirtualMemoryEx_t o_allocEx = nullptr;
NtFreeVirtualMemory_t o_free = nullptr;
NtMapViewOfSection_t o_map = nullptr;
NtMapViewOfSectionEx_t o_mapEx = nullptr;
NtUnmapViewOfSection_t o_unmap = nullptr;
NtUnmapViewOfSectionEx_t o_unmapEx = nullptr;

bool IsSelf(HANDLE process) {
    return process == reinterpret_cast<HANDLE>(-1) || GetProcessId(process) == g_pid;
}

bool IsSystem(uintptr_t a) {
    for (unsigned i = 0; i < g_systemCount; i++)
        if (a >= g_system[i].lo && a < g_system[i].hi) return true;
    return false;
}

// Inside the hooks: no allocation, no lock, no logging.
void Record(PVOID base, SIZE_T size, Kind kind) {
    const uintptr_t b = reinterpret_cast<uintptr_t>(base);
    if (!b) return;
    Slot& s = g_slots[b >> 16];
    void* raw[kWalk];
    const USHORT n = RtlCaptureStackBackTrace(2, kWalk, raw, nullptr);
    unsigned kept = 0;
    uint32_t frames[kFrames] = {};
    for (USHORT i = 0; i < n && kept < kFrames; i++) {
        const uintptr_t f = reinterpret_cast<uintptr_t>(raw[i]);
        if (!IsSystem(f)) frames[kept++] = static_cast<uint32_t>(f);
    }
    s.base.store(0, std::memory_order_relaxed);
    s.size = static_cast<uint32_t>(size);
    std::memcpy(s.frames, frames, sizeof frames);
    s.kind = kind;
    s.thread = GetCurrentThreadId();
    s.base.store(static_cast<uint32_t>(b), std::memory_order_release);
    g_recorded.fetch_add(1, std::memory_order_relaxed);
}

void Forget(uintptr_t b) {
    if (!b) return;
    uint32_t expected = static_cast<uint32_t>(b);
    if (g_slots[b >> 16].base.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) g_released.fetch_add(1, std::memory_order_relaxed);
}

LONG NTAPI Hooked_Alloc(HANDLE process, PVOID* base, ULONG_PTR zeroBits, PSIZE_T size, ULONG type, ULONG protect) {
    const LONG r = o_alloc(process, base, zeroBits, size, type, protect);
    if (r >= 0 && (type & MEM_RESERVE) && base && IsSelf(process)) Record(*base, size ? *size : 0, Reserve);
    return r;
}

LONG NTAPI Hooked_AllocEx(HANDLE process, PVOID* base, PSIZE_T size, ULONG type, ULONG protect, PVOID params, ULONG count) {
    const LONG r = o_allocEx(process, base, size, type, protect, params, count);
    if (r >= 0 && (type & MEM_RESERVE) && base && IsSelf(process)) Record(*base, size ? *size : 0, ReserveEx);
    return r;
}

LONG NTAPI Hooked_Free(HANDLE process, PVOID* base, PSIZE_T size, ULONG type) {
    const uintptr_t b = base ? reinterpret_cast<uintptr_t>(*base) : 0;
    const LONG r = o_free(process, base, size, type);
    if (r >= 0 && (type & MEM_RELEASE) && IsSelf(process)) Forget(b);
    return r;
}

LONG NTAPI Hooked_Map(HANDLE section, HANDLE process, PVOID* base, ULONG_PTR zeroBits, SIZE_T commit, PLARGE_INTEGER offset, PSIZE_T view,
                      DWORD inherit, ULONG type, ULONG protect) {
    const LONG r = o_map(section, process, base, zeroBits, commit, offset, view, inherit, type, protect);
    if (r >= 0 && base && IsSelf(process)) Record(*base, view ? *view : 0, View);
    return r;
}

LONG NTAPI Hooked_MapEx(HANDLE section, HANDLE process, PVOID* base, PLARGE_INTEGER offset, PSIZE_T view, ULONG type, ULONG protect, PVOID params,
                        ULONG count) {
    const LONG r = o_mapEx(section, process, base, offset, view, type, protect, params, count);
    if (r >= 0 && base && IsSelf(process)) Record(*base, view ? *view : 0, ViewEx);
    return r;
}

LONG NTAPI Hooked_Unmap(HANDLE process, PVOID base) {
    const LONG r = o_unmap(process, base);
    if (r >= 0 && IsSelf(process)) Forget(reinterpret_cast<uintptr_t>(base));
    return r;
}

LONG NTAPI Hooked_UnmapEx(HANDLE process, PVOID base, ULONG flags) {
    const LONG r = o_unmapEx(process, base, flags);
    if (r >= 0 && IsSelf(process)) Forget(reinterpret_cast<uintptr_t>(base));
    return r;
}

void AddSystem(const wchar_t* name) {
    if (g_systemCount >= std::size(g_system)) return;
    const HMODULE m = GetModuleHandleW(name);
    if (!m) return;
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), m, &info, sizeof info)) return;
    const uintptr_t lo = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    g_system[g_systemCount++] = {lo, lo + info.SizeOfImage};
}

// ---- scan side (address-space monitor thread only) ----

struct Totals {
    uint32_t allocations = 0;
    uint64_t committed = 0, reserved = 0;
};
struct Aggregate {
    std::unordered_map<std::string, Totals> owners, sites;
    uint64_t uptime = 0;
    uint32_t recorded = 0, released = 0;
};

std::map<uintptr_t, std::string> g_moduleNames; // allocation base of an image -> file name
Aggregate g_current;
std::mutex g_reportLock;
std::string g_report;
int g_scans = 0;

std::string Narrow(const wchar_t* w) {
    std::string out;
    for (; w && *w; w++) out += *w < 128 ? static_cast<char>(*w) : '?';
    return out;
}

std::string BaseName(const wchar_t* path) {
    const wchar_t* n = wcsrchr(path, L'\\');
    return Narrow(n ? n + 1 : path);
}

// "module+0xRVA", or "code 0xADDRESS" for executable memory outside any image (JIT, trampolines)
std::string Where(uint32_t address, std::string* module) {
    MEMORY_BASIC_INFORMATION m{};
    if (!address || VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), &m, sizeof m) != sizeof m) {
        if (module) *module = "unknown";
        return "?";
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(m.AllocationBase);
    if (m.Type != MEM_IMAGE) {
        if (module) *module = "code outside modules (JIT or hooks)";
        return std::format("code 0x{:08X}", address);
    }
    auto it = g_moduleNames.find(base);
    if (it == g_moduleNames.end()) {
        wchar_t path[MAX_PATH] = {};
        std::string name = GetModuleFileNameW(reinterpret_cast<HMODULE>(base), path, MAX_PATH) ? BaseName(path) : std::format("image 0x{:08X}", base);
        it = g_moduleNames.emplace(base, std::move(name)).first;
    }
    if (module) *module = it->second;
    return std::format("{}+0x{:X}", it->second, address - base);
}

constexpr double MB(uint64_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); }

void Add(std::unordered_map<std::string, Totals>& map, const std::string& key, uint64_t committed, uint64_t reserved) {
    Totals& t = map[key];
    t.allocations++;
    t.committed += committed;
    t.reserved += reserved;
}

template <class Map>
std::vector<std::pair<std::string, Totals>> Sorted(const Map& map) {
    std::vector<std::pair<std::string, Totals>> v(map.begin(), map.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.committed + a.second.reserved > b.second.committed + b.second.reserved; });
    return v;
}

std::string Csv(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + '"';
}

void WriteCsv(const Aggregate& a) {
    static std::wstring file;
    static uint64_t bytes = 0;
    static bool disabled = false;
    if (disabled) return;
    if (file.empty()) {
        if (!ApexPaths::EnsureApexDirectory()) { disabled = true; return; }
        SYSTEMTIME t{};
        GetLocalTime(&t);
        file = ApexPaths::ApexDirectory() + std::format(L"AddressOwners-{}-{:04}{:02}{:02}-{:02}{:02}{:02}.csv", GetCurrentProcessId(), t.wYear, t.wMonth,
                                                        t.wDay, t.wHour, t.wMinute, t.wSecond);
    }
    constexpr const char* header = "build,uptime_ms,level,name,allocations,committed_bytes,reserved_bytes\n";
    std::string rows;
    for (const auto& [name, t] : Sorted(a.owners))
        rows += std::format("{},{},owner,{},{},{},{}\n", APEX_VERSION_STRING, a.uptime, Csv(name), t.allocations, t.committed, t.reserved);
    auto sites = Sorted(a.sites);
    if (sites.size() > kTopSites) sites.resize(kTopSites);
    for (const auto& [name, t] : sites)
        rows += std::format("{},{},site,{},{},{},{}\n", APEX_VERSION_STRING, a.uptime, Csv(name), t.allocations, t.committed, t.reserved);
    if (bytes + rows.size() + strlen(header) > 32ull * 1024 * 1024) { disabled = true; LOG_WARNING("[AddressOwners] CSV reached its 32 MiB cap"); return; }
    FILE* out = nullptr;
    if (_wfopen_s(&out, file.c_str(), bytes ? L"ab" : L"wb") || !out) { disabled = true; return; }
    bool ok = true;
    if (!bytes) {
        const size_t n = strlen(header);
        ok = fwrite(header, 1, n, out) == n;
        bytes += n;
    }
    ok = fwrite(rows.data(), 1, rows.size(), out) == rows.size() && ok;
    if (fclose(out)) ok = false;
    bytes += rows.size();
    if (!ok) { disabled = true; LOG_WARNING("[AddressOwners] CSV write failed"); }
}

std::string ReportOf(const Aggregate& a) {
    std::string t = std::format("Address-space owners ({} allocations recorded since start, {} released; untracked = made before the hooks or by the kernel):\n",
                                a.recorded, a.released);
    t += "   by module (committed / reserved-only MB, allocations):\n";
    for (const auto& [name, x] : Sorted(a.owners)) t += std::format("     {:<44} {:8.1f} / {:8.1f}  x{}\n", name, MB(x.committed), MB(x.reserved), x.allocations);
    t += "   largest call sites:\n";
    auto sites = Sorted(a.sites);
    if (sites.size() > 20) sites.resize(20);
    for (const auto& [name, x] : sites) t += std::format("     {:<70} {:8.1f} / {:8.1f}  x{}\n", name, MB(x.committed), MB(x.reserved), x.allocations);
    return t;
}

} // namespace

bool Enabled() { return g_enabled.load(std::memory_order_acquire); }

void InstallEarly() {
    if constexpr (!kCompiled) return;
    if (Enabled() || !DevSwitches::Measurements()) return;
    g_pid = GetCurrentProcessId();
    for (const wchar_t* name : {L"ntdll.dll", L"kernelbase.dll", L"kernel32.dll", L"ucrtbase.dll", L"msvcrt.dll", L"vcruntime140.dll", L"msvcp140.dll",
                                L"msvcr90.dll", L"msvcr80.dll", L"msvcr100.dll"})
        AddSystem(name);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    o_alloc = reinterpret_cast<NtAllocateVirtualMemory_t>(GetProcAddress(ntdll, "NtAllocateVirtualMemory"));
    o_allocEx = reinterpret_cast<NtAllocateVirtualMemoryEx_t>(GetProcAddress(ntdll, "NtAllocateVirtualMemoryEx"));
    o_free = reinterpret_cast<NtFreeVirtualMemory_t>(GetProcAddress(ntdll, "NtFreeVirtualMemory"));
    o_map = reinterpret_cast<NtMapViewOfSection_t>(GetProcAddress(ntdll, "NtMapViewOfSection"));
    o_mapEx = reinterpret_cast<NtMapViewOfSectionEx_t>(GetProcAddress(ntdll, "NtMapViewOfSectionEx"));
    o_unmap = reinterpret_cast<NtUnmapViewOfSection_t>(GetProcAddress(ntdll, "NtUnmapViewOfSection"));
    o_unmapEx = reinterpret_cast<NtUnmapViewOfSectionEx_t>(GetProcAddress(ntdll, "NtUnmapViewOfSectionEx"));
    if (!o_alloc || !o_free || !o_map || !o_unmap) {
        LOG_WARNING("[AddressOwners] ntdll entry points not found: ledger off");
        return;
    }
    // WOW64 ntdll stubs start with "mov eax, imm32" (5 bytes, position-independent): that instruction moves to a trampoline
    // and a 5-byte JMP replaces it, written with every other thread suspended and none inside the bytes (game threads call
    // these functions all the time); nothing else is hooked unless every stub has that shape
    struct Stub { void** original; void* hook; };
    Stub stubs[7] = {{reinterpret_cast<void**>(&o_alloc), reinterpret_cast<void*>(&Hooked_Alloc)},
                     {reinterpret_cast<void**>(&o_free), reinterpret_cast<void*>(&Hooked_Free)},
                     {reinterpret_cast<void**>(&o_map), reinterpret_cast<void*>(&Hooked_Map)},
                     {reinterpret_cast<void**>(&o_unmap), reinterpret_cast<void*>(&Hooked_Unmap)},
                     {reinterpret_cast<void**>(&o_allocEx), reinterpret_cast<void*>(&Hooked_AllocEx)},
                     {reinterpret_cast<void**>(&o_mapEx), reinterpret_cast<void*>(&Hooked_MapEx)},
                     {reinterpret_cast<void**>(&o_unmapEx), reinterpret_cast<void*>(&Hooked_UnmapEx)}};
    BYTE* code = static_cast<BYTE*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    g_slots = static_cast<Slot*>(VirtualAlloc(nullptr, sizeof(Slot) * 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!code || !g_slots) {
        LOG_WARNING("[AddressOwners] No memory for the hooks: ledger off");
        return;
    }
    static BYTE jmp[7][5];
    MemPatch::CodeWrite writes[7];
    size_t count = 0;
    for (size_t i = 0; i < 7; i++) {
        const auto entry = reinterpret_cast<uintptr_t>(*stubs[i].original);
        if (!entry) continue;
        BYTE head[5] = {};
        if (!MemPatch::ReadBytes(entry, head, 5) || head[0] != 0xB8) {
            LOG_WARNING(std::format("[AddressOwners] ntdll stub {} does not start with mov eax,imm32: ledger off", i));
            return;
        }
        BYTE* t = code + i * 16;
        std::memcpy(t, head, 5);
        t[5] = 0xE9;
        const int32_t back = static_cast<int32_t>(entry + 5 - (reinterpret_cast<uintptr_t>(t) + 10));
        std::memcpy(t + 6, &back, 4);
        jmp[i][0] = 0xE9;
        const int32_t rel = MemPatch::CalculateRelativeOffset(entry, reinterpret_cast<uintptr_t>(stubs[i].hook));
        std::memcpy(&jmp[i][1], &rel, 4);
        writes[count++] = {entry, jmp[i], 5, 5};
        *stubs[i].original = t; // the original is called through the trampoline once the JMP is in place
    }
    FlushInstructionCache(GetCurrentProcess(), code, 4096);
    if (!MemPatch::WriteCodeBatchSuspended(writes, count)) {
        LOG_WARNING("[AddressOwners] Hooks could not be written: ledger off");
        return;
    }
    g_enabled.store(true, std::memory_order_release);
    LOG_INFO(std::format("[AddressOwners] On ({}): ntdll reserve/release/map/unmap hooked from the init thread; {} system DLLs skipped when attributing",
                         APEX_VERSION_STRING, g_systemCount));
}

void BeginScan() {
    if (!Enabled()) return;
    g_current = {};
}

void Account(uintptr_t allocationBase, DWORD type, uint64_t committed, uint64_t reserved, bool hasGuard) {
    if (!Enabled() || !allocationBase) return;
    if (type == MEM_IMAGE) {
        std::string module;
        Where(static_cast<uint32_t>(allocationBase), &module);
        Add(g_current.owners, "image: " + module, committed, reserved);
        return;
    }
    const Slot& s = g_slots[allocationBase >> 16];
    const bool tracked = s.base.load(std::memory_order_acquire) == static_cast<uint32_t>(allocationBase);
    std::string mapped;
    if (type == MEM_MAPPED) {
        wchar_t name[MAX_PATH] = {};
        mapped = GetMappedFileNameW(GetCurrentProcess(), reinterpret_cast<LPVOID>(allocationBase), name, MAX_PATH) ? BaseName(name) : "pagefile-backed";
    }
    const char* kind = type == MEM_MAPPED ? "view" : "private";
    if (!tracked) {
        const std::string owner = hasGuard ? "untracked: thread stacks" : std::format("untracked {}{}", kind, type == MEM_MAPPED ? " (" + mapped + ")" : "");
        Add(g_current.owners, owner, committed, reserved);
        return;
    }
    std::string module;
    const std::string first = Where(s.frames[0], &module);
    std::string site = first;
    for (unsigned i = 1; i < kFrames && s.frames[i]; i++) site += " <- " + Where(s.frames[i], nullptr);
    if (type == MEM_MAPPED) site += " [" + mapped + "]";
    Add(g_current.owners, std::format("{} ({})", module, kind), committed, reserved);
    Add(g_current.sites, site, committed, reserved);
}

void EndScan(uint64_t uptimeMs) {
    if (!Enabled()) return;
    g_current.uptime = uptimeMs;
    g_current.recorded = g_recorded.load(std::memory_order_relaxed);
    g_current.released = g_released.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_reportLock);
        g_report = ReportOf(g_current);
    }
    if (g_scans++ % kWriteEvery != 0) return;
    WriteCsv(g_current);
    std::string line = "[AddressOwners] committed/reserved MB:";
    auto owners = Sorted(g_current.owners);
    for (size_t i = 0; i < owners.size() && i < 8; i++)
        line += std::format(" {} {:.0f}/{:.0f};", owners[i].first, MB(owners[i].second.committed), MB(owners[i].second.reserved));
    LOG_INFO(line);
}

std::string ReportText() {
    if (!Enabled()) return "";
    std::lock_guard<std::mutex> lk(g_reportLock);
    return g_report;
}

} // namespace VaLedger
