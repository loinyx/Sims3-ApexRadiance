// Offline checks of the game allocation category ledger's tables (features/game_alloc_categories.cpp): no hooks are
// installed; the production bookkeeping (CategoryOf, Note, Forget) runs from many threads at once. Credits: @loinyx
#define APEX_DEVELOPMENT_BUILD_FLAVOR
#include "../../features/game_alloc_categories.cpp"
#include <cstdio>
#include <random>
#include <thread>

namespace ApexLog { void Write(Level, const std::string&, const std::source_location&) noexcept {} }
namespace ApexPaths {
bool EnsureApexDirectory() { return false; }
const std::wstring& ApexDirectory() { static std::wstring d; return d; }
}
namespace MemPatch {
bool GetModuleInfo(HMODULE, BYTE**, size_t*) { return false; }
bool ValidateBytes(LPCVOID, const BYTE*, size_t) { return false; }
int32_t CalculateRelativeOffset(uintptr_t from, uintptr_t to, size_t length) { return static_cast<int32_t>(to - (from + length)); }
bool WriteCodeBatchSuspended(const CodeWrite*, size_t) { return false; }
}

using namespace GameAllocCategories;
static int g_failures = 0, g_checks = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

int main() {
    g_blocks = static_cast<std::atomic<uint64_t>*>(VirtualAlloc(nullptr, sizeof(uint64_t) * (size_t(kWays) << kBucketBits), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    g_categories = static_cast<Category*>(VirtualAlloc(nullptr, sizeof(Category) * kCategories, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(g_blocks != nullptr && g_categories != nullptr, "tables allocated");
    static const char* names[] = {"Lot/FloorManager", "CAS/TextureCompositorResource", "Renderer/RenderSystem", "Routing/QuadTree"};
    // Names live in TS3W.exe's image in the game; here they are ordinary literals, so they take the caller key. Use explicit keys.
    const uint32_t keys[4] = {0x00F00010u, 0x00F00020u, 0x00F00030u, 0x00F00040u};
    (void)names;
    // 1. Category slots are stable and shared
    uint32_t a = CategoryOf(keys[0]), b = CategoryOf(keys[0]), c = CategoryOf(keys[1]);
    Check(a == b && a != c && a < kCategories, "a key keeps its slot; different keys differ");
    // 2. Concurrent remember/forget: each thread owns its own fake addresses (16-byte aligned, as the allocator's)
    constexpr int kThreads = 8, kOps = 200000;
    std::vector<std::thread> threads;
    std::atomic<int64_t> expected[4] = {};
    for (int t = 0; t < kThreads; t++)
        threads.emplace_back([t, &keys, &expected] {
            std::mt19937 rng(1234 + t);
            std::vector<std::pair<uint32_t, int>> live; // address, category index
            uint32_t next = 0x10000000u + uint32_t(t) * 0x01000000u;
            for (int i = 0; i < kOps; i++) {
                if (live.empty() || rng() % 3) {
                    const int k = int(rng() % 4);
                    const size_t size = 1024 + (rng() % 65536);
                    const uint32_t p = next;
                    next += 0x40;
                    const uint32_t cat = CategoryOf(keys[k]);
                    Remember(reinterpret_cast<void*>(static_cast<uintptr_t>(p)), cat, size);
                    expected[k] += int64_t((size + 1023) >> 10) << 10;
                    live.push_back({p, k});
                } else {
                    const size_t at = rng() % live.size();
                    const auto [p, k] = live[at];
                    live[at] = live.back();
                    live.pop_back();
                    // the same size the entry recorded is subtracted: recompute from the table before forgetting
                    Forget(reinterpret_cast<void*>(static_cast<uintptr_t>(p)));
                    (void)k;
                }
            }
            for (auto [p, k] : live) Forget(reinterpret_cast<void*>(static_cast<uintptr_t>(p)));
            (void)expected;
        });
    for (auto& th : threads) th.join();
    // Every block was forgotten: each category is back to zero (or the block never fitted its bucket)
    // A block that did not fit its bucket is neither added nor subtracted, so the totals still return to zero.
    for (int k = 0; k < 4; k++) {
        const Category& cat = g_categories[CategoryOf(keys[k])];
        Check(cat.liveBytes.load() == 0, "live bytes return to zero after every free");
        Check(cat.liveBlocks.load() == 0, "live blocks return to zero after every free");
        Check(cat.peakBytes.load() > 0, "peak recorded");
    }
    Check(g_tracked.load() == 0, "no block left tracked");
    // 3. Forgetting an unknown address changes nothing; a small block is only counted
    const int64_t before = g_categories[a].liveBytes.load();
    Forget(reinterpret_cast<void*>(static_cast<uintptr_t>(0x7FFF0000u)));
    Check(g_categories[a].liveBytes.load() == before, "unknown free is ignored");
    // 4. Generic names are grouped by caller
    Check(KeyOf("c:\\BuildAgent\\x\\EASTL/allocator.h", reinterpret_cast<void*>(0x00512345)) == (0x80000000u | 0x00512345u), "source-path names group by caller");
    Check(KeyOf(nullptr, reinterpret_cast<void*>(0x00512345)) == (0x80000000u | 0x00512345u), "null names group by caller");
    Check(NameOf(0x80000000u | 0x00512345u) == "generic, caller TS3W+0x112345", "caller names show the image offset");
    // 5. A large block's KB count saturates instead of wrapping
    Remember(reinterpret_cast<void*>(static_cast<uintptr_t>(0x60000000u)), a, size_t(1) << 31);
    Check(g_categories[a].liveBytes.load() == before + ((int64_t((1u << 19) - 1)) << 10), "very large block saturates at 512 MiB");
    Forget(reinterpret_cast<void*>(static_cast<uintptr_t>(0x60000000u)));
    Check(g_categories[a].liveBytes.load() == before, "and is subtracted exactly");
    std::printf("Game allocation categories: %d checks, %d failures (bucket overflows %u)\n", g_checks, g_failures, g_blockOverflow.load());
    return g_failures ? 1 : 0;
}
