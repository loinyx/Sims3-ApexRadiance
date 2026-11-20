#pragma once
// String table index: the index itself and the lookup answer (features/string_table_index.h). Header only, so the offline
// check (tools/string_table_index_test) runs the same code on fake tables. No game code is called from here.
//
// Game layout (Steam 1.67.2 disassembly; the hooks check the instructions that use each offset before patching):
//   service + 0x48   list sentinel {next, prev}; a node is {next, prev, resource key (16 bytes), table} (0x20 bytes)
//   node + 0x18      the string table, 0 while not loaded (the walk skips it)
//   table + 0x28     the table's hash map: 7919 bucket heads (index = low dword of the key % 7919)
//   entry            {next, ?, key low, key high, text}: +0 next in the bucket (0 ends it), +8 / +0xC the 64-bit key,
//                    +0x10 the text (UTF-16); the walk takes an entry only when its text is not null and not empty
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace StringTableIndex::Core {

constexpr uint32_t kListOffset = 0x48;
constexpr uint32_t kNodeTable = 0x18;
constexpr uint32_t kTableMap = 0x28;
constexpr uint32_t kBuckets = 7919;
constexpr uint32_t kEntryKeyLo = 0x08, kEntryKeyHi = 0x0C, kEntryText = 0x10;

inline uintptr_t Rd(uintptr_t a) { return *reinterpret_cast<const uintptr_t*>(a); }

// The answer of the game's walk for one key: the first node in list order whose table has the key with a non-empty
// text, or the sentinel when none has. Used for the verification samples and the offline check.
inline uintptr_t WalkLikeGame(uintptr_t sentinel, uint32_t lo, uint32_t hi) {
    for (uintptr_t node = Rd(sentinel); node != sentinel; node = Rd(node)) {
        const uintptr_t table = Rd(node + kNodeTable);
        if (!table) continue;
        for (uintptr_t e = Rd(table + kTableMap + 4 * (lo % kBuckets)); e; e = Rd(e))
            if (Rd(e + kEntryKeyLo) == lo && Rd(e + kEntryKeyHi) == hi) {
                const auto* text = reinterpret_cast<const wchar_t*>(Rd(e + kEntryText));
                if (text && *text) return node;
                break; // keys are unique in one table (its insert keeps the first entry)
            }
    }
    return sentinel;
}

// Open addressing, 12 bytes a slot: key and the winning node (0 = empty slot). Load factor at most 3/4.
#pragma pack(push, 4)
struct Slot {
    uint32_t lo, hi, node;
};
#pragma pack(pop)
static_assert(sizeof(Slot) == 12);

inline uint32_t Hash(uint32_t lo, uint32_t hi) {
    uint64_t h = (static_cast<uint64_t>(hi) << 32 | lo) * 0x9E3779B97F4A7C15ull;
    return static_cast<uint32_t>(h >> 32);
}

struct Stats {
    std::atomic<uint64_t> hits{0}, misses{0}, fallbacks{0}, verified{0}, mismatches{0};
    std::atomic<uint32_t> builds{0}, invalidations{0}, overCap{0};
    std::atomic<uint32_t> lastEntries{0}, lastTables{0}, lastBytes{0}, lastBuildMs{0}, lastSteps{0};
};

class Index {
  public:
    // Bytes the slot array may use; past it the build stops and every lookup takes the game's walk until the list changes
    size_t maxBytes = 32u << 20;
    // Time one build step may take (in a lookup that finds the index out of date); the lookup itself takes the game's walk
    double stepMs = 2.0;
    // 1 in `verifyEvery` answers is also checked against WalkLikeGame (plus the first 256 after each build); 0 = never
    uint32_t verifyEvery = 64;
    Stats stats;
    // Set by the first mismatch: the index answers nothing more this session
    std::atomic<bool> broken{false};
    bool (*layoutOk)() = nullptr; // checked before each build step; false turns the index off

    ~Index() { Free(); }

    // The hooks around the game's list changes (clear, add / reload one table, rebuild): any change makes the index stale
    void MutationBegin() { mutating_.fetch_add(1); gen_.fetch_add(1); stats.invalidations.fetch_add(1, std::memory_order_relaxed); }
    void MutationEnd() { gen_.fetch_add(1); mutating_.fetch_sub(1); }

    // The node the game's walk starts from for this key: the winning node, the sentinel (no table has it) or the list's
    // first node (no answer: the game walks as before). Any thread; never blocks.
    uintptr_t Start(uintptr_t sentinel, const uint32_t* key) {
        const uintptr_t first = Rd(sentinel);
        if (broken.load(std::memory_order_relaxed) || mutating_.load(std::memory_order_acquire) != 0 || first == sentinel) return Fallback(first);
        const uint32_t gen = gen_.load(std::memory_order_acquire);
        if (TryAcquireSRWLockShared(&lock_)) {
            uintptr_t answer = 0;
            const bool ok = ready_ && builtGen_ == gen && sentinel_ == sentinel && fpFirst_ == first && fpLast_ == Rd(sentinel + 4);
            if (ok) answer = Find(key[0], key[1]);
            ReleaseSRWLockShared(&lock_);
            if (ok) {
                if (!answer) answer = sentinel;
                if (NeedVerify() && !Verify(sentinel, key, answer)) return Fallback(first);
                (answer == sentinel ? stats.misses : stats.hits).fetch_add(1, std::memory_order_relaxed);
                return answer;
            }
        }
        if (layoutOk && !layoutOk()) { // the table layout this index reads changed (another mod resized the hash maps)
            broken.store(true);
            return Fallback(first);
        }
        if (TryAcquireSRWLockExclusive(&lock_)) {
            Step(sentinel, gen);
            ReleaseSRWLockExclusive(&lock_);
        }
        return Fallback(first);
    }

    bool Ready() const { return ready_; }
    uint32_t Entries() const { return count_; }
    size_t Bytes() const { return static_cast<size_t>(cap_) * sizeof(Slot); }

    void Free() {
        AcquireSRWLockExclusive(&lock_);
        Release();
        ready_ = false;
        building_ = false;
        ReleaseSRWLockExclusive(&lock_);
    }

  private:
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::atomic<uint32_t> gen_{1}, mutating_{0};
    std::atomic<uint32_t> verifyCounter_{0};
    Slot* slots_ = nullptr;
    uint32_t cap_ = 0, count_ = 0; // cap_ a power of two
    bool ready_ = false;
    uint32_t builtGen_ = 0;
    uintptr_t sentinel_ = 0, fpFirst_ = 0, fpLast_ = 0;
    // build in progress
    bool building_ = false, capped_ = false;
    uint32_t buildGen_ = 0;
    uintptr_t cursor_ = 0;
    uint32_t tables_ = 0, steps_ = 0;
    double buildMs_ = 0;

    uintptr_t Fallback(uintptr_t first) {
        stats.fallbacks.fetch_add(1, std::memory_order_relaxed);
        return first;
    }

    bool NeedVerify() {
        if (!verifyEvery) return false;
        const uint32_t n = verifyCounter_.fetch_add(1, std::memory_order_relaxed);
        return n < 256 || n % verifyEvery == 0;
    }

    bool Verify(uintptr_t sentinel, const uint32_t* key, uintptr_t answer) {
        stats.verified.fetch_add(1, std::memory_order_relaxed);
        if (WalkLikeGame(sentinel, key[0], key[1]) == answer) return true;
        stats.mismatches.fetch_add(1, std::memory_order_relaxed);
        broken.store(true);
        return false;
    }

    uint32_t Find(uint32_t lo, uint32_t hi) const {
        if (!cap_) return 0;
        for (uint32_t i = Hash(lo, hi) & (cap_ - 1);; i = (i + 1) & (cap_ - 1)) {
            const Slot& s = slots_[i];
            if (!s.node) return 0;
            if (s.lo == lo && s.hi == hi) return s.node;
        }
    }

    void Release() {
        if (slots_) VirtualFree(slots_, 0, MEM_RELEASE);
        slots_ = nullptr;
        cap_ = count_ = 0;
    }

    bool Alloc(uint32_t cap) {
        if (static_cast<size_t>(cap) * sizeof(Slot) > maxBytes) return false;
        auto* s = static_cast<Slot*>(VirtualAlloc(nullptr, static_cast<size_t>(cap) * sizeof(Slot), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!s) return false;
        Slot* old = slots_;
        const uint32_t oldCap = cap_;
        slots_ = s, cap_ = cap, count_ = 0;
        for (uint32_t i = 0; i < oldCap; i++)
            if (old[i].node) Put(old[i].lo, old[i].hi, old[i].node);
        if (old) VirtualFree(old, 0, MEM_RELEASE);
        return true;
    }

    // First insert of a key wins (tables are visited in the walk's order)
    bool Put(uint32_t lo, uint32_t hi, uint32_t node) {
        if ((count_ + 1) * 4 > cap_ * 3 && !Alloc(cap_ ? cap_ * 2 : 1u << 16)) return false;
        for (uint32_t i = Hash(lo, hi) & (cap_ - 1);; i = (i + 1) & (cap_ - 1)) {
            Slot& s = slots_[i];
            if (!s.node) {
                s = {lo, hi, node};
                count_++;
                return true;
            }
            if (s.lo == lo && s.hi == hi) return true;
        }
    }

    static double NowMs() {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
    }

    // Under the exclusive lock: one budgeted piece of the build (restarted when the list changed since it began)
    void Step(uintptr_t sentinel, uint32_t gen) {
        if (ready_ && builtGen_ == gen && sentinel_ == sentinel && fpFirst_ == Rd(sentinel) && fpLast_ == Rd(sentinel + 4))
            return; // up to date (the shared lock was only busy)
        // (an index whose list ends differ although no hooked change ran is rebuilt from scratch below)
        if (!building_ || buildGen_ != gen || sentinel_ != sentinel) {
            if (capped_ && buildGen_ == gen) return; // over the cap for this list: wait for the next change
            ready_ = false, capped_ = false, building_ = true;
            buildGen_ = gen, sentinel_ = sentinel;
            fpFirst_ = Rd(sentinel), fpLast_ = Rd(sentinel + 4);
            cursor_ = fpFirst_;
            tables_ = steps_ = 0, buildMs_ = 0;
            if (slots_) std::memset(slots_, 0, static_cast<size_t>(cap_) * sizeof(Slot));
            count_ = 0;
        }
        const double t0 = NowMs();
        steps_++;
        while (cursor_ != sentinel) {
            if (const uintptr_t table = Rd(cursor_ + kNodeTable)) {
                tables_++;
                for (uint32_t b = 0; b < kBuckets; b++)
                    for (uintptr_t e = Rd(table + kTableMap + 4 * b); e; e = Rd(e)) {
                        const auto* text = reinterpret_cast<const wchar_t*>(Rd(e + kEntryText));
                        if (!text || !*text) continue; // the walk passes over an empty text: a later table may answer
                        if (!Put(static_cast<uint32_t>(Rd(e + kEntryKeyLo)), static_cast<uint32_t>(Rd(e + kEntryKeyHi)), static_cast<uint32_t>(cursor_))) {
                            capped_ = true, building_ = false;
                            Release();
                            stats.overCap.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                    }
            }
            cursor_ = Rd(cursor_);
            if (NowMs() - t0 >= stepMs) break;
        }
        buildMs_ += NowMs() - t0;
        if (cursor_ != sentinel) return;
        building_ = false, ready_ = true, builtGen_ = buildGen_;
        stats.builds.fetch_add(1, std::memory_order_relaxed);
        stats.lastEntries = count_, stats.lastTables = tables_, stats.lastBytes = static_cast<uint32_t>(Bytes());
        stats.lastBuildMs = static_cast<uint32_t>(buildMs_ + 0.5), stats.lastSteps = steps_;
        verifyCounter_ = 0;
    }
};

} // namespace StringTableIndex::Core
