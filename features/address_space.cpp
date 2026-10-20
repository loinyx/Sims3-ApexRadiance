// Address-space monitor (development build; see address_space.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "address_space.h"
#include "memory_telemetry.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "game_addresses.h"
#include "apex_paths.h"
#include "apex_version.h"
#include "high_alloc.h"
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <format>
#include <mutex>
#include <vector>

namespace AddressSpace {
namespace {

constexpr DWORD kPeriodMs = 10000;
constexpr int kLogEvery = 6; // snapshots (one line a minute)
constexpr int kClasses = 6;
const char* const kClassNames[kClasses] = {"< 64 KB", "64 KB - 1 MB", "1 - 4 MB", "4 - 16 MB", "16 - 64 MB", ">= 64 MB"};

struct Block {
    uintptr_t base = 0;
    uint64_t size = 0;
};
struct Image {
    uintptr_t base = 0;
    uint64_t size = 0;
    std::string name;
};
struct Snapshot {
    std::string when;
    uint64_t free = 0, freeLow = 0, freeHigh = 0, largest = 0, largestLow = 0, largestHigh = 0;
    Block top[5];
    uint64_t committed[3] = {}, reserved[3] = {}; // image, mapped, private: committed / reserved-only
    uint64_t privateExec = 0;                     // committed private PAGE_EXECUTE_READWRITE; ownership unknown
    uint32_t regions = 0, allocations = 0;
    uint32_t classCount[kClasses] = {};
    uint64_t classBytes[kClasses] = {};
    uint32_t gameBigBlocks = 0;
    uint64_t gameBigBytes = 0;
    bool haveGame = false;
    uint64_t sampledAt = 0, elapsedMs = 0, privateBytes = 0, workingSet = 0, peakWorkingSet = 0;
    uint64_t systemPhysicalAvailable = 0, systemCommitBytes = 0, systemCommitLimit = 0;
    uint64_t readBytes = 0, writeBytes = 0, pageFaults = 0, apexHighBytes = 0, queryEnd = 0;
    bool processValid = false, systemValid = false, ioValid = false, scanComplete = false;
    std::vector<Image> images; // largest first, top 8
};

std::mutex g_lock;
Snapshot g_last, g_min; // g_min: the snapshot with the smallest largest free block
bool g_haveLast = false;
HANDLE g_thread = nullptr, g_stop = nullptr;

int ClassOf(uint64_t size) {
    constexpr uint64_t kLim[kClasses - 1] = {64ull << 10, 1ull << 20, 4ull << 20, 16ull << 20, 64ull << 20};
    int c = 0;
    while (c < kClasses - 1 && size >= kLim[c]) c++;
    return c;
}

bool ReadU32(uintptr_t a, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const uint32_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::string NowText() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return std::format("{:02}:{:02}:{:02}", t.wHour, t.wMinute, t.wSecond);
}

Snapshot Take() {
    Snapshot s;
    s.sampledAt = GetTickCount64();
    s.when = NowText();
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uint64_t lo = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uint64_t hi = uint64_t(reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress)) + 1;
    std::vector<Block> frees;
    std::vector<Image> images;
    uintptr_t allocBase = 0, allocType = 0;
    uint64_t allocSize = 0;
    auto closeAlloc = [&] {
        if (!allocBase) return;
        s.allocations++;
        if (allocType == MEM_PRIVATE) {
            const int c = ClassOf(allocSize);
            s.classCount[c]++;
            s.classBytes[c] += allocSize;
        } else if (allocType == MEM_IMAGE) {
            images.push_back({allocBase, allocSize, {}});
        }
        allocBase = 0;
        allocSize = 0;
    };
    MEMORY_BASIC_INFORMATION m;
    uint64_t a = lo;
    for (; a < hi;) {
        if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(a)), &m, sizeof m) != sizeof m) break;
        const uint64_t base = reinterpret_cast<uintptr_t>(m.BaseAddress);
        const uint64_t end = std::min<uint64_t>(hi, base + uint64_t(m.RegionSize));
        if (end <= a) break;
        const uint64_t size = end - a;
        s.regions++;
        if (m.State == MEM_FREE) {
            closeAlloc();
            s.free += size;
            const uint64_t low = a < 0x80000000ull ? std::min<uint64_t>(end, 0x80000000ull) - a : 0;
            const uint64_t high = size - low;
            s.freeLow += low;
            s.freeHigh += high;
            s.largestLow = std::max<uint64_t>(s.largestLow, low);
            s.largestHigh = std::max<uint64_t>(s.largestHigh, high);
            s.largest = std::max<uint64_t>(s.largest, size);
            frees.push_back({static_cast<uintptr_t>(a), size});
        } else {
            const uintptr_t ab = reinterpret_cast<uintptr_t>(m.AllocationBase);
            if (ab != allocBase) {
                closeAlloc();
                allocBase = ab;
                allocType = m.Type;
            }
            allocSize += size;
            const int k = m.Type == MEM_IMAGE ? 0 : m.Type == MEM_MAPPED ? 1 : 2;
            if (m.State == MEM_COMMIT) {
                s.committed[k] += size;
                if (k == 2 && m.Protect == PAGE_EXECUTE_READWRITE) s.privateExec += size;
            } else {
                s.reserved[k] += size;
            }
        }
        a = end;
    }
    s.scanComplete = a >= hi;
    s.queryEnd = hi;
    closeAlloc();
    std::sort(frees.begin(), frees.end(), [](const Block& x, const Block& y) { return x.size > y.size; });
    for (size_t i = 0; i < 5 && i < frees.size(); i++) s.top[i] = frees[i];
    std::sort(images.begin(), images.end(), [](const Image& x, const Image& y) { return x.size > y.size; });
    if (images.size() > 8) images.resize(8);
    for (Image& im : images) {
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(reinterpret_cast<HMODULE>(im.base), path, MAX_PATH)) {
            const wchar_t* n = wcsrchr(path, L'\\');
            for (const wchar_t* c = n ? n + 1 : path; *c; c++) im.name += *c < 128 ? static_cast<char>(*c) : '?';
        } else {
            im.name = std::format("{:08X}", im.base);
        }
    }
    s.images = std::move(images);
    // the game allocator's own VirtualAlloc'd big blocks (128 KB and more)
    if (GameAddr::Resolved()) {
        uint32_t alloc = 0, n = 0, bytes = 0;
        if (const uintptr_t g = GameAddr::Get(GameAddr::Id::AllocGlobal); g && ReadU32(g, &alloc) && alloc && ReadU32(alloc + 0x488, &n) && ReadU32(alloc + 0x48C, &bytes)) {
            s.haveGame = true;
            s.gameBigBlocks = n;
            s.gameBigBytes = bytes;
        }
    }
    PROCESS_MEMORY_COUNTERS_EX process{};
    process.cb = sizeof process;
    s.processValid = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof process) != FALSE;
    if (s.processValid) {
        s.privateBytes = process.PrivateUsage;
        s.workingSet = process.WorkingSetSize;
        s.peakWorkingSet = process.PeakWorkingSetSize;
        s.pageFaults = process.PageFaultCount;
    }
    PERFORMANCE_INFORMATION system{};
    system.cb = sizeof system;
    s.systemValid = GetPerformanceInfo(&system, sizeof system) != FALSE;
    if (s.systemValid) {
        s.systemPhysicalAvailable = uint64_t(system.PhysicalAvailable) * system.PageSize;
        s.systemCommitBytes = uint64_t(system.CommitTotal) * system.PageSize;
        s.systemCommitLimit = uint64_t(system.CommitLimit) * system.PageSize;
    }
    IO_COUNTERS io{};
    s.ioValid = GetProcessIoCounters(GetCurrentProcess(), &io) != FALSE;
    if (s.ioValid) { s.readBytes = io.ReadTransferCount; s.writeBytes = io.WriteTransferCount; }
    s.apexHighBytes = ApexMem::g_highBytes.load(std::memory_order_relaxed);
    s.elapsedMs = GetTickCount64() - s.sampledAt;
    return s;
}

constexpr double MB(uint64_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); }

std::string LineOf(const Snapshot& s) {
    return std::format("free {:.0f} MB (below 2 GB {:.0f}, above {:.0f}), largest free block {:.0f} MB (below 2 GB {:.0f}, above {:.0f}); committed: images {:.0f}, "
                       "mapped {:.0f}, private {:.0f} MB (private executable {:.0f}, ownership unknown); reserved only: {:.0f} MB; {} allocations",
                       MB(s.free), MB(s.freeLow), MB(s.freeHigh), MB(s.largest), MB(s.largestLow), MB(s.largestHigh), MB(s.committed[0]), MB(s.committed[1]),
                       MB(s.committed[2]), MB(s.privateExec), MB(s.reserved[0] + s.reserved[1] + s.reserved[2]), s.allocations);
}

void WriteMemorySample(const Snapshot& s) {
    static std::wstring file;
    static uint64_t bytesWritten = 0;
    static bool disabled = false;
    static uint64_t baseline = 0;
    static bool haveBaseline = false;
    if (disabled) return;
    if (file.empty()) {
        if (!ApexPaths::EnsureApexDirectory()) { disabled = true; LOG_WARNING("[MemoryAnalysis] Output directory unavailable"); return; }
        SYSTEMTIME t{}; GetLocalTime(&t);
        file = ApexPaths::ApexDirectory() + std::format(L"MemoryAnalysis-{}-{:04}{:02}{:02}-{:02}{:02}{:02}.csv", GetCurrentProcessId(), t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
        LOG_INFO("[MemoryAnalysis] CSV enabled for " APEX_VERSION_STRING "; samples every 10 s on a background thread, capped at 16 MiB per session. No memory is freed or trimmed. Private executable pages are not attributed to the script heap.");
    }
    if (s.processValid && !haveBaseline) { baseline = s.privateBytes; haveBaseline = true; }
    const int64_t growth = s.processValid && haveBaseline ? int64_t(s.privateBytes) - int64_t(baseline) : 0;
    const std::string row = std::format("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
        s.sampledAt, s.elapsedMs, s.scanComplete, s.queryEnd, s.processValid, s.privateBytes, s.workingSet, s.peakWorkingSet,
        s.free, s.largest, s.freeLow, s.largestLow, s.freeHigh, s.largestHigh,
        s.committed[0], s.committed[1], s.committed[2], s.reserved[0]+s.reserved[1]+s.reserved[2], s.privateExec,
        s.systemValid, s.systemPhysicalAvailable, s.systemCommitBytes, s.systemCommitLimit, s.ioValid, s.readBytes, s.writeBytes,
        s.pageFaults, s.apexHighBytes, growth, s.regions);
    constexpr const char* header = "uptime_ms,scan_ms,scan_complete,address_end_exclusive,process_valid,private_bytes,working_set_bytes,peak_working_set_bytes,free_va_bytes,largest_free_va_bytes,free_below_2g_bytes,largest_below_2g_bytes,free_above_2g_bytes,largest_above_2g_bytes,image_commit_bytes,mapped_commit_bytes,private_commit_bytes,reserved_bytes,private_executable_bytes,system_valid,system_available_physical_bytes,system_commit_bytes,system_commit_limit_bytes,io_valid,read_bytes,write_bytes,page_fault_count,apex_high_allocator_bytes,private_growth_since_baseline_bytes,regions\n";
    if (bytesWritten + row.size() + strlen(header) > 16ull*1024*1024) { disabled = true; LOG_WARNING("[MemoryAnalysis] Session CSV reached its 16 MiB cap; sampling continues in the log"); return; }
    FILE* out = nullptr;
    if (_wfopen_s(&out, file.c_str(), bytesWritten ? L"ab" : L"wb") || !out) { disabled = true; LOG_WARNING("[MemoryAnalysis] CSV could not be opened"); return; }
    bool ok = true;
    if (!bytesWritten) { const size_t n = strlen(header); ok = fwrite(header, 1, n, out) == n; bytesWritten += n; }
    ok = (fwrite(row.data(), 1, row.size(), out) == row.size()) && ok;
    if (fclose(out)) ok = false;
    bytesWritten += row.size();
    if (!ok) { disabled = true; LOG_WARNING("[MemoryAnalysis] CSV write failed; further file writes disabled"); }
}


void WriteGameplayTelemetry(const Snapshot& s) {
    static std::wstring path;
    static uint64_t bytes = 0;
    static bool disabled = false;
    if (disabled) return;
    if (path.empty()) {
        if (!ApexPaths::EnsureApexDirectory()) { disabled = true; return; }
        SYSTEMTIME t{}; GetLocalTime(&t);
        path = ApexPaths::ApexDirectory() + std::format(L"GameplayMemory-{}-{:04}{:02}{:02}-{:02}{:02}{:02}.csv",
            GetCurrentProcessId(), t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
    }
    MemoryTelemetry::Event batch[64]{};
    const unsigned n = MemoryTelemetry::Drain(batch);
    constexpr const char* header = "build,uptime_ms,kind,hook_active,size_class,attempts,requested_bytes,failed_attempts,request_size,caller_return,thread_id,error,allocation_type,event_private_bytes,event_working_set_bytes,dropped_events,game_counters_valid,game_big_blocks,game_big_bytes\n";
    std::string rows;
    for (unsigned c=0; c<6; ++c)
        rows += std::format("{},{},summary,{},{},{},{},{},0,0,0,0,0,0,0,{},{},{},{}\n", APEX_VERSION_STRING,
            s.sampledAt, MemoryTelemetry::active.load(), c, MemoryTelemetry::calls[c].load(),
            MemoryTelemetry::requested[c].load(), MemoryTelemetry::failures[c].load(), MemoryTelemetry::dropped.load(),
            s.haveGame, s.gameBigBlocks, s.gameBigBytes);
    for (unsigned i=0; i<n; ++i) {
        const auto& e = batch[i];
        const char* kind = e.kind==1 ? "allocation_failure" : e.kind==2 ? "save_start" : e.kind==3 ? "save_success" : "save_failure";
        rows += std::format("{},{},{},{},0,0,0,0,{},{},{},{},{},{},{},{},false,0,0\n", APEX_VERSION_STRING,
            e.tick, kind, MemoryTelemetry::active.load(), e.size, e.caller, e.thread, e.error, e.type,
            e.privateBytes, e.workingSet, MemoryTelemetry::dropped.load());
    }
    if (bytes + rows.size() + strlen(header) > 16ull*1024*1024) {
        disabled = true; LOG_WARNING("[MemoryAnalysis] Gameplay CSV reached its 16 MiB cap"); return;
    }
    FILE* out = nullptr;
    if (_wfopen_s(&out, path.c_str(), bytes ? L"ab" : L"wb") || !out) { disabled = true; return; }
    bool ok = true;
    if (!bytes) { const size_t len = strlen(header); ok = fwrite(header,1,len,out)==len; bytes += len; }
    ok = fwrite(rows.data(),1,rows.size(),out)==rows.size() && ok;
    if (fclose(out)) ok = false;
    bytes += rows.size();
    if (!ok) { disabled = true; LOG_WARNING("[MemoryAnalysis] Gameplay CSV write failed"); }
}

std::string TableOf(const Snapshot& s) {
    std::string t = "   " + LineOf(s) + "\n   largest free blocks:";
    for (const Block& b : s.top)
        if (b.size) t += std::format(" {:.0f} MB at {:08X};", MB(b.size), b.base);
    t += "\n   private allocations by size:";
    for (int c = 0; c < kClasses; c++)
        if (s.classCount[c]) t += std::format(" {} x{} = {:.0f} MB;", kClassNames[c], s.classCount[c], MB(s.classBytes[c]));
    t += "\n   largest images:";
    for (const Image& im : s.images) t += std::format(" {} {:.1f} MB;", im.name, MB(im.size));
    if (s.haveGame) t += std::format("\n   game allocator big blocks (own VirtualAlloc, 128 KB and more): {} = {:.0f} MB", s.gameBigBlocks, MB(s.gameBigBytes));
    return t + "\n";
}

DWORD WINAPI Proc(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    int n = 0;
    do {
      try {
        Snapshot s = Take();
        {
            std::lock_guard<std::mutex> lk(g_lock);
            if (s.scanComplete && (!g_haveLast || !g_min.scanComplete || s.largest < g_min.largest)) g_min = s;
            g_last = s;
            g_haveLast = true;
        }
        WriteMemorySample(s);
        WriteGameplayTelemetry(s);
        if (n++ % kLogEvery == 0) {
            LOG_INFO("[AddressSpace] " + LineOf(s));
            LOG_INFO(std::format("[MemoryAnalysis] {} | private {:.0f} MiB, working set {:.0f} MiB, system available physical {:.0f} MiB | scan {} in {} ms | Apex high allocator {:.1f} MiB (not all Apex memory)",
                APEX_VERSION_STRING, MB(s.privateBytes), MB(s.workingSet), MB(s.systemPhysicalAvailable), s.scanComplete ? "complete" : "incomplete", s.elapsedMs, MB(s.apexHighBytes)));
        }
      } catch (...) { LOG_WARNING("[MemoryAnalysis] Sample failed; observer will retry"); }
    } while (WaitForSingleObject(g_stop, kPeriodMs) == WAIT_TIMEOUT);
    return 0;
}

} // namespace

void Start() {
    if (!kDevToolsBuild || kPublicBuild) return;
    if (g_thread) return;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop) g_thread = CreateThread(nullptr, 64 * 1024, Proc, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
}

void Stop() {
    if (!g_thread) return;
    SetEvent(g_stop); // not waited for: called under the loader lock, which a thread's exit needs
    CloseHandle(g_thread);
    g_thread = nullptr;
}

std::string ReportText() {
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g_haveLast) return "";
    return std::format("Address space (VirtualQuery every 10 s; the game is 32-bit, 4 GB):\n   latest ({}):\n{}   session minimum of the largest free block ({}):\n{}", g_last.when,
                       TableOf(g_last), g_min.when, TableOf(g_min));
}

} // namespace AddressSpace
