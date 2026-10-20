// Game allocation categories (numbered development builds; see game_alloc_categories.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "game_alloc_categories.h"
#include "dev_switches.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_version.h"
#include "memory_patch.h"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

namespace GameAllocCategories {
namespace {

constexpr uintptr_t kAlloc = 0x004E3F90, kAllocAligned = 0x004E3FB0, kFree = 0x004E3F70;
// Whole, relocation-free instructions moved to the trampolines (stack-relative MOVs, TEST)
constexpr BYTE kAllocPrologue[8] = {0x8B, 0x44, 0x24, 0x0C, 0x8B, 0x4C, 0x24, 0x04};
constexpr BYTE kAlignedPrologue[8] = {0x8B, 0x44, 0x24, 0x14, 0x8B, 0x4C, 0x24, 0x0C};
constexpr BYTE kFreePrologue[6] = {0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0};

using Alloc_t = void*(__cdecl*)(size_t, const char*, unsigned);
using AllocAligned_t = void*(__cdecl*)(size_t, size_t, size_t, const char*, unsigned);
using Free_t = void(__cdecl*)(void*);
Alloc_t o_alloc = nullptr;
AllocAligned_t o_aligned = nullptr;
Free_t o_free = nullptr;

// ---- categories: key = name pointer (a string literal of TS3W.exe), or 0x80000000 | caller for generic names ----
constexpr unsigned kCategoryBits = 13, kCategories = 1u << kCategoryBits;
struct Category {
    std::atomic<uint32_t> key{0};
    std::atomic<int64_t> liveBytes{0};
    std::atomic<int32_t> liveBlocks{0};
    std::atomic<int64_t> peakBytes{0};
    std::atomic<uint32_t> allocations{0};
    std::atomic<int64_t> smallBytes{0}; // allocated below the tracking size since start (never subtracted)
};
Category* g_categories = nullptr; // kCategories entries, VirtualAlloc'd when the ledger turns on
std::atomic<uint32_t> g_categoryOverflow{0};

uint32_t CategoryOf(uint32_t key) {
    uint32_t h = (key * 2654435761u) >> (32 - kCategoryBits);
    for (unsigned probe = 0; probe < 64; probe++, h = (h + 1) & (kCategories - 1)) {
        Category& c = g_categories[h];
        const uint32_t k = c.key.load(std::memory_order_acquire);
        if (k == key) return h;
        if (!k) {
            uint32_t expected = 0;
            if (c.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel) || expected == key) return h;
        }
    }
    g_categoryOverflow.fetch_add(1, std::memory_order_relaxed);
    return kCategories; // untracked
}

// ---- live blocks of 1 KB and more: 8-way buckets; entry = ptr << 32 | category << 19 | KB (19 bits) ----
constexpr size_t kTrackMin = 1024;
constexpr unsigned kWays = 8, kBucketBits = 17;
std::atomic<uint64_t>* g_blocks = nullptr; // kWays << kBucketBits entries (8 MiB), VirtualAlloc'd at install
std::atomic<uint32_t> g_blockOverflow{0}, g_tracked{0};

inline uint32_t BucketOf(uint32_t p) { return ((p >> 4) * 2654435761u) >> (32 - kBucketBits); }

void Remember(void* p, uint32_t category, size_t bytes) {
    const uint32_t ptr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
    const uint32_t kb = static_cast<uint32_t>(std::min<size_t>((bytes + 1023) >> 10, (1u << 19) - 1));
    const uint64_t entry = (uint64_t(ptr) << 32) | (uint64_t(category) << 19) | kb;
    std::atomic<uint64_t>* bucket = g_blocks + (size_t(BucketOf(ptr)) * kWays);
    for (unsigned w = 0; w < kWays; w++) {
        uint64_t expected = 0;
        if (bucket[w].compare_exchange_strong(expected, entry, std::memory_order_acq_rel)) {
            Category& c = g_categories[category];
            const int64_t live = c.liveBytes.fetch_add(int64_t(kb) << 10, std::memory_order_relaxed) + (int64_t(kb) << 10);
            c.liveBlocks.fetch_add(1, std::memory_order_relaxed);
            int64_t peak = c.peakBytes.load(std::memory_order_relaxed);
            while (live > peak && !c.peakBytes.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {}
            g_tracked.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    g_blockOverflow.fetch_add(1, std::memory_order_relaxed);
}

void Forget(void* p) {
    const uint32_t ptr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
    std::atomic<uint64_t>* bucket = g_blocks + (size_t(BucketOf(ptr)) * kWays);
    for (unsigned w = 0; w < kWays; w++) {
        uint64_t e = bucket[w].load(std::memory_order_acquire);
        if (!e || uint32_t(e >> 32) != ptr) continue;
        if (!bucket[w].compare_exchange_strong(e, 0, std::memory_order_acq_rel)) return;
        const uint32_t category = uint32_t(e >> 19) & (kCategories - 1);
        const int64_t bytes = int64_t(e & ((1u << 19) - 1)) << 10;
        g_categories[category].liveBytes.fetch_sub(bytes, std::memory_order_relaxed);
        g_categories[category].liveBlocks.fetch_sub(1, std::memory_order_relaxed);
        g_tracked.fetch_sub(1, std::memory_order_relaxed);
        return;
    }
}

// Names that are a source path (EASTL's default allocator name) or empty say nothing: those are grouped by caller.
uint32_t KeyOf(const char* name, void* caller) {
    const uintptr_t n = reinterpret_cast<uintptr_t>(name);
    if (n >= 0x00400000 && n < 0x01400000) { // a literal inside TS3W.exe's image
        const char c0 = name[0];
        if (c0 && !(c0 == 'c' && name[1] == ':')) return static_cast<uint32_t>(n);
    }
    return 0x80000000u | static_cast<uint32_t>(reinterpret_cast<uintptr_t>(caller));
}

void Note(void* p, size_t size, const char* name, void* caller) {
    if (!p) return;
    const uint32_t category = CategoryOf(KeyOf(name, caller));
    if (category >= kCategories) return;
    Category& c = g_categories[category];
    c.allocations.fetch_add(1, std::memory_order_relaxed);
    if (size >= kTrackMin) Remember(p, category, size);
    else c.smallBytes.fetch_add(int64_t(size), std::memory_order_relaxed);
}

void* __cdecl Hook_Alloc(size_t size, const char* name, unsigned flags) {
    void* p = o_alloc(size, name, flags);
    Note(p, size, name, _ReturnAddress());
    return p;
}

void* __cdecl Hook_AllocAligned(size_t size, size_t align, size_t offset, const char* name, unsigned flags) {
    void* p = o_aligned(size, align, offset, name, flags);
    Note(p, size, name, _ReturnAddress());
    return p;
}

void __cdecl Hook_Free(void* p) {
    if (p) Forget(p); // before the game's free: the address cannot be handed out again yet
    o_free(p);
}

std::atomic<bool> g_active{false};
std::mutex g_reportLock;
std::string g_report;

BYTE* MakeTrampoline(BYTE* at, const BYTE* prologue, size_t len, uintptr_t resume) {
    std::memcpy(at, prologue, len);
    at[len] = 0xE9;
    const int32_t rel = static_cast<int32_t>(resume - (reinterpret_cast<uintptr_t>(at) + len + 5));
    std::memcpy(at + len + 1, &rel, 4);
    return at;
}

bool CopyName(uint32_t key, char (&buf)[72]) { // POD only (SEH)
    __try {
        const char* s = reinterpret_cast<const char*>(static_cast<uintptr_t>(key));
        for (int i = 0; i < 70 && s[i]; i++) buf[i] = (s[i] >= 32 && s[i] < 127 && s[i] != '"') ? s[i] : '?';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string NameOf(uint32_t key) {
    if (key & 0x80000000u) return std::format("generic, caller TS3W+0x{:X}", (key & 0x7FFFFFFFu) - 0x400000);
    char buf[72] = {};
    return CopyName(key, buf) ? std::string(buf) : std::string("?");
}

constexpr double MB(int64_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); }

} // namespace

bool Active() { return g_active.load(std::memory_order_acquire); }

bool Install() {
    if (!DevSwitches::Measurements() || Active()) return Active();
    const HMODULE exe = GetModuleHandleW(nullptr);
    BYTE* base = nullptr;
    size_t size = 0;
    if (!MemPatch::GetModuleInfo(exe, &base, &size) || reinterpret_cast<uintptr_t>(base) != 0x00400000 || size < 0x01000000) return false;
    if (!MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kAlloc), kAllocPrologue, sizeof kAllocPrologue) ||
        !MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kAllocAligned), kAlignedPrologue, sizeof kAlignedPrologue) ||
        !MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kFree), kFreePrologue, sizeof kFreePrologue)) {
        LOG_INFO("[GameAllocCategories] Allocator entries differ from Steam 1.67.2 (another build or another module's hook): off");
        return false;
    }
    g_blocks = static_cast<std::atomic<uint64_t>*>(
        VirtualAlloc(nullptr, sizeof(uint64_t) * (size_t(kWays) << kBucketBits), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    BYTE* code = static_cast<BYTE*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    g_categories = static_cast<Category*>(VirtualAlloc(nullptr, sizeof(Category) * kCategories, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_blocks || !code || !g_categories) return false;
    o_alloc = reinterpret_cast<Alloc_t>(MakeTrampoline(code, kAllocPrologue, sizeof kAllocPrologue, kAlloc + sizeof kAllocPrologue));
    o_aligned = reinterpret_cast<AllocAligned_t>(
        MakeTrampoline(code + 32, kAlignedPrologue, sizeof kAlignedPrologue, kAllocAligned + sizeof kAlignedPrologue));
    o_free = reinterpret_cast<Free_t>(MakeTrampoline(code + 64, kFreePrologue, sizeof kFreePrologue, kFree + sizeof kFreePrologue));
    FlushInstructionCache(GetCurrentProcess(), code, 4096);
    static BYTE jmp[3][5];
    const uintptr_t sites[3] = {kAlloc, kAllocAligned, kFree};
    const void* hooks[3] = {reinterpret_cast<void*>(&Hook_Alloc), reinterpret_cast<void*>(&Hook_AllocAligned), reinterpret_cast<void*>(&Hook_Free)};
    MemPatch::CodeWrite writes[3];
    for (int i = 0; i < 3; i++) {
        jmp[i][0] = 0xE9;
        const int32_t rel = MemPatch::CalculateRelativeOffset(sites[i], reinterpret_cast<uintptr_t>(hooks[i]));
        std::memcpy(&jmp[i][1], &rel, 4);
        writes[i] = {sites[i], jmp[i], 5, i == 2 ? size_t(6) : size_t(8)};
    }
    if (!MemPatch::WriteCodeBatchSuspended(writes, 3)) {
        LOG_WARNING("[GameAllocCategories] Hooks could not be written: off");
        return false;
    }
    g_active.store(true, std::memory_order_release);
    LOG_INFO("[GameAllocCategories] On (" APEX_VERSION_STRING "): the game allocator's Alloc/AllocAligned/Free entries attribute blocks to their EA "
             "memory categories; blocks from 1 KB are followed until freed");
    return true;
}

void Report(unsigned long long uptimeMs) {
    if (!Active()) return;
    struct Row {
        uint32_t key;
        int64_t live, peak, smallAllocated;
        int32_t blocks;
        uint32_t allocations;
    };
    std::vector<Row> rows;
    rows.reserve(2048);
    int64_t total = 0;
    for (unsigned i = 0; i < kCategories; i++) {
        const Category& c = g_categories[i];
        const uint32_t key = c.key.load(std::memory_order_acquire);
        if (!key) continue;
        Row r{key, c.liveBytes.load(), c.peakBytes.load(), c.smallBytes.load(), static_cast<int32_t>(c.liveBlocks.load()), static_cast<uint32_t>(c.allocations.load())};
        total += r.live;
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.live > b.live; });
    static std::wstring file;
    static uint64_t written = 0;
    static bool disabled = false;
    if (!disabled && file.empty() && ApexPaths::EnsureApexDirectory()) {
        SYSTEMTIME t{};
        GetLocalTime(&t);
        file = ApexPaths::ApexDirectory() + std::format(L"GameCategories-{}-{:04}{:02}{:02}-{:02}{:02}{:02}.csv", GetCurrentProcessId(), t.wYear, t.wMonth,
                                                        t.wDay, t.wHour, t.wMinute, t.wSecond);
    }
    std::string csv;
    for (size_t i = 0; i < rows.size() && i < 150; i++)
        csv += std::format("{},{},\"{}\",{},{},{},{},{}\n", APEX_VERSION_STRING, uptimeMs, NameOf(rows[i].key), rows[i].live, rows[i].blocks, rows[i].peak,
                           rows[i].allocations, rows[i].smallAllocated);
    if (!disabled && !file.empty() && written + csv.size() < 32ull * 1024 * 1024) {
        FILE* out = nullptr;
        if (!_wfopen_s(&out, file.c_str(), written ? L"ab" : L"wb") && out) {
            if (!written) {
                const char* header = "build,uptime_ms,category,live_bytes,live_blocks,peak_bytes,allocations,small_bytes_allocated\n";
                written += fwrite(header, 1, strlen(header), out);
            }
            written += fwrite(csv.data(), 1, csv.size(), out);
            fclose(out);
        } else {
            disabled = true;
        }
    }
    std::string line = std::format("[GameAllocCategories] {:.0f} MB live in blocks of 1 KB or more ({} blocks, {} not followed, {} category overflows); largest:",
                                   MB(total), g_tracked.load(), g_blockOverflow.load(), g_categoryOverflow.load());
    std::string text = "Game allocation categories (live, blocks of 1 KB or more):\n";
    for (size_t i = 0; i < rows.size() && i < 25; i++) {
        if (i < 8) line += std::format(" {} {:.1f};", NameOf(rows[i].key), MB(rows[i].live));
        text += std::format("   {:<56} {:8.1f} MB  peak {:8.1f}  blocks {}\n", NameOf(rows[i].key), MB(rows[i].live), MB(rows[i].peak), rows[i].blocks);
    }
    LOG_INFO(line);
    std::lock_guard<std::mutex> lk(g_reportLock);
    g_report = std::move(text);
}

std::string ReportText() {
    std::lock_guard<std::mutex> lk(g_reportLock);
    return g_report;
}

} // namespace GameAllocCategories
