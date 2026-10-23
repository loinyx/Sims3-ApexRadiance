// Object ID side index for the object service's hash map (see object_id_map.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "object_id_map.h"
#include "apex_log.h"
#include "apex_util.h"
#include "hook_guard.h"
#include "load_timing.h"
#include "build_flavor.h"
#include "entry_chain.h"
#include "game_addresses.h"
#include <windows.h>
#include <atomic>
#include <format>
#include <mutex>

namespace ObjectIdMap {
namespace {

using EntryChain::Layer;
using EntryChain::Site;

constexpr uint32_t kBuckets = 0x409, kEndOff = 0x1024, kCountOff = 0x1028;
constexpr uint32_t kIdLo = 8, kIdHi = 0xC, kNext = 0x10;
constexpr uint32_t kMinSlots = 8192, kMaxSlots = 1u << 20;
constexpr uint32_t kMaxNodes = 1u << 19;
constexpr uint32_t kStartupChecks = 256, kDevCheckEvery = 64;
constexpr uintptr_t kTomb = 1;

using FnFind = void(__fastcall*)(uint8_t* map, void* edx, uint32_t* out, const uint32_t* key);
using FnInsert = void(__fastcall*)(uint8_t* map, void* edx, uint32_t* out, uint8_t* node, uint32_t flag);
using FnErase = void(__fastcall*)(uint8_t* map, void* edx, uint32_t* out, uint8_t* node, uint32_t* bucket);

struct Slot {
    uint32_t lo, hi;
    uintptr_t node; // 0 = empty, kTomb = erased
};

// guarded by g_lock (exclusive: insert / erase / build; shared: find)
SRWLOCK g_lock = SRWLOCK_INIT;
uint8_t* g_map = nullptr; // the map indexed (nullptr = not built yet)
Slot* g_slots = nullptr;
uint32_t g_mask = 0, g_live = 0, g_used = 0;

std::mutex g_ctrl;
bool g_started = false;
std::atomic<bool> g_on{false}, g_off{false}; // g_off: turned itself off for the session
std::atomic<uint32_t> g_answers{0};
std::atomic<uint64_t> c_finds{0}, c_hits{0}, c_misses{0}, c_busy{0}, c_checked{0}, c_inserts{0}, c_erases{0}, c_grows{0};
std::atomic<uint32_t> g_builtCount{0};

uint32_t Hash(uint32_t lo, uint32_t hi) {
    uint32_t h = lo * 0x9E3779B1u ^ (hi + 0x7F4A7C15u) * 0x85EBCA77u;
    return h ^ (h >> 15);
}

// caller holds the lock (any mode); returns the node or 0
uintptr_t Lookup(uint32_t lo, uint32_t hi) {
    for (uint32_t i = Hash(lo, hi) & g_mask, n = 0; n <= g_mask; i = (i + 1) & g_mask, n++) {
        const Slot& s = g_slots[i];
        if (!s.node) return 0;
        if (s.node != kTomb && s.lo == lo && s.hi == hi) return s.node;
    }
    return 0;
}

// exclusive: the table only grows through Rehash
void Put(uint32_t lo, uint32_t hi, uintptr_t node) {
    uint32_t tomb = UINT32_MAX;
    for (uint32_t i = Hash(lo, hi) & g_mask, n = 0; n <= g_mask; i = (i + 1) & g_mask, n++) {
        Slot& s = g_slots[i];
        if (s.node == kTomb) {
            if (tomb == UINT32_MAX) tomb = i;
            continue;
        }
        if (!s.node) {
            Slot& d = g_slots[tomb != UINT32_MAX ? tomb : i];
            if (tomb == UINT32_MAX) g_used++;
            d = {lo, hi, node};
            g_live++;
            return;
        }
        if (s.lo == lo && s.hi == hi) {
            s.node = node;
            return;
        }
    }
}

void Drop(uint32_t lo, uint32_t hi) {
    for (uint32_t i = Hash(lo, hi) & g_mask, n = 0; n <= g_mask; i = (i + 1) & g_mask, n++) {
        Slot& s = g_slots[i];
        if (!s.node) return;
        if (s.node != kTomb && s.lo == lo && s.hi == hi) {
            s.node = kTomb;
            g_live--;
            return;
        }
    }
}

void FreeTable() {
    if (g_slots) VirtualFree(g_slots, 0, MEM_RELEASE);
    g_slots = nullptr;
    g_map = nullptr;
    g_mask = g_live = g_used = 0;
}

// exclusive: a table of `slots` entries with every live entry of the current one
bool Rehash(uint32_t slots) {
    Slot* const fresh = static_cast<Slot*>(VirtualAlloc(nullptr, sizeof(Slot) * slots, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!fresh) return false;
    Slot* const old = g_slots;
    const uint32_t oldSlots = old ? g_mask + 1 : 0;
    g_slots = fresh;
    g_mask = slots - 1;
    g_live = g_used = 0;
    for (uint32_t i = 0; i < oldSlots; i++)
        if (old[i].node > kTomb) Put(old[i].lo, old[i].hi, old[i].node);
    if (old) VirtualFree(old, 0, MEM_RELEASE);
    c_grows.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// exclusive, before an insert: room for one more under 70 % (live + erased)
bool Room() {
    if ((g_used + 1) * 10 <= (g_mask + 1) * 7) return true;
    uint32_t slots = g_mask + 1;
    while ((g_live + 1) * 10 > slots * 4 && slots < kMaxSlots) slots <<= 1; // grow when live > 40 %, else only sweep the erased
    if ((g_live + 1) * 10 > slots * 7) return false;
    return Rehash(slots);
}

// 07/10, players' Runtime Error: this runs inside the game's object lookups, sometimes under g_lock. The index goes off first;
// the reason's text is built here (why() returns it) and a failure to build or log it stays here.
template <class Why> void TurnOff(Why&& why) noexcept {
    if (g_off.exchange(true)) return;
    try {
        LOG_ERROR("[ObjectIdMap] " + std::string(why()) + ". Object lookups by ID go back to the game's own search for this session.");
    } catch (...) {
    }
}

// SEH only: every node of the game's map into the (fresh) table; -1 = unreadable / too many / a cycle
int64_t WalkMap(uint8_t* map, Slot* slots, uint32_t mask) {
    uint32_t n = 0;
    __try {
        for (uint32_t b = 0; b < kBuckets; b++)
            for (uintptr_t node = *reinterpret_cast<uintptr_t*>(map + b * 4); node; node = *reinterpret_cast<uintptr_t*>(node + kNext)) {
                if (++n > kMaxNodes) return -1;
                const uint32_t lo = *reinterpret_cast<uint32_t*>(node + kIdLo), hi = *reinterpret_cast<uint32_t*>(node + kIdHi);
                for (uint32_t i = Hash(lo, hi) & mask;; i = (i + 1) & mask)
                    if (!slots[i].node) {
                        slots[i] = {lo, hi, node};
                        break;
                    }
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return n;
}

// exclusive: indexes `map` from the game's own links (no mutation can run: insert / erase wait on the lock)
bool Build(uint8_t* map) {
    const uint32_t count = *reinterpret_cast<const uint32_t*>(map + kCountOff);
    if (count > kMaxNodes) {
        TurnOff([&] { return std::format("The object map holds {} objects, more than the index takes", count); });
        return false;
    }
    uint32_t slots = kMinSlots;
    while (slots < count * 3 && slots < kMaxSlots) slots <<= 1;
    FreeTable();
    g_slots = static_cast<Slot*>(VirtualAlloc(nullptr, sizeof(Slot) * slots, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_slots) {
        TurnOff([] { return "The index could not be allocated"; });
        return false;
    }
    g_mask = slots - 1;
    const int64_t n = WalkMap(map, g_slots, g_mask);
    if (n < 0 || static_cast<uint32_t>(n) != count) {
        FreeTable();
        TurnOff([&] { return std::format("The object map could not be read as expected ({} nodes linked, count {})", n, count); });
        return false;
    }
    g_live = g_used = static_cast<uint32_t>(n);
    g_map = map;
    g_builtCount.store(static_cast<uint32_t>(n), std::memory_order_relaxed);
    LoadTiming::NoteObjectMapIndexed(); // atomics only (the earliest world load sign)
    HookGuard::Try("ObjectIdMap index note", [&] { // under g_lock, inside the game's lookup: the line may not throw (07/10)
        LOG_INFO(std::format("[ObjectIdMap] Indexed the object map {:#010x}: {} objects in {} slots ({} KB)", reinterpret_cast<uintptr_t>(map), n, slots,
                             sizeof(Slot) * slots / 1024));
    });
    return true;
}

void __fastcall Hook_Find(uint8_t* map, void* edx, uint32_t* out, const uint32_t* key) {
    const auto next = reinterpret_cast<FnFind>(EntryChain::Next(Site::ObjMapFind, Layer::ObjectIdMap));
    if (!g_on.load(std::memory_order_acquire) || g_off.load(std::memory_order_relaxed)) return next(map, edx, out, key);
    c_finds.fetch_add(1, std::memory_order_relaxed);
    if (!g_map) { // first use: index this map (the only one these functions serve)
        ApexUtil::SrwExclusive srw(g_lock);
        // (g_on again under the lock: Stop turns it off, then frees the table under this lock, so a find that passed the test
        // above before Stop builds nothing once Stop has freed it)
        if (!g_map && !g_off.load() && g_on.load(std::memory_order_acquire)) Build(map);
    }
    if (!TryAcquireSRWLockShared(&g_lock)) {
        c_busy.fetch_add(1, std::memory_order_relaxed);
        return next(map, edx, out, key);
    }
    if (map != g_map || g_off.load(std::memory_order_relaxed)) {
        ReleaseSRWLockShared(&g_lock);
        return next(map, edx, out, key);
    }
    if (*reinterpret_cast<const uint32_t*>(map + kCountOff) != g_live) {
        const uint32_t theirs = *reinterpret_cast<const uint32_t*>(map + kCountOff), ours = g_live;
        ReleaseSRWLockShared(&g_lock);
        TurnOff([&] { return std::format("The object map holds {} objects and the index {}: a change went past the hooks", theirs, ours); });
        return next(map, edx, out, key);
    }
    const uint32_t lo = key[0], hi = key[1];
    const uintptr_t node = Lookup(lo, hi);
    uint32_t got[2];
    if (node) {
        got[0] = static_cast<uint32_t>(node);
        got[1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(map) + (lo % kBuckets) * 4);
    } else {
        got[0] = *reinterpret_cast<const uint32_t*>(map + kEndOff);
        got[1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(map) + kEndOff);
    }
    const uint32_t n = g_answers.fetch_add(1, std::memory_order_relaxed);
    const bool check = n < kStartupChecks || (!kPublicBuild && n % kDevCheckEvery == 0);
    if (check) {
        uint32_t want[2] = {};
        next(map, edx, want, key); // under the shared lock: nothing can change the map meanwhile
        c_checked.fetch_add(1, std::memory_order_relaxed);
        if (want[0] != got[0] || want[1] != got[1]) {
            ReleaseSRWLockShared(&g_lock);
            TurnOff([&] { return std::format("An answer differed from the game's (ID {:08x}{:08x}: the index {:#010x}, the game {:#010x})", hi, lo, got[0], want[0]); });
            out[0] = want[0];
            out[1] = want[1];
            return;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    (node ? c_hits : c_misses).fetch_add(1, std::memory_order_relaxed);
    out[0] = got[0];
    out[1] = got[1];
}

void __fastcall Hook_Insert(uint8_t* map, void* edx, uint32_t* out, uint8_t* node, uint32_t flag) {
    const auto next = reinterpret_cast<FnInsert>(EntryChain::Next(Site::ObjMapInsert, Layer::ObjectIdMap));
    ApexUtil::SrwExclusive srw(g_lock); // the game's insert is a leaf: no game code waits on this lock inside it
    next(map, edx, out, node, flag);
    if (map == g_map && g_slots && reinterpret_cast<const uint8_t*>(out)[8] == 1) {
        if (Room()) {
            Put(*reinterpret_cast<const uint32_t*>(node + kIdLo), *reinterpret_cast<const uint32_t*>(node + kIdHi), reinterpret_cast<uintptr_t>(node));
            c_inserts.fetch_add(1, std::memory_order_relaxed);
        } else {
            FreeTable();
            g_off.store(true);
        }
    }
}

void __fastcall Hook_Erase(uint8_t* map, void* edx, uint32_t* out, uint8_t* node, uint32_t* bucket) {
    const auto next = reinterpret_cast<FnErase>(EntryChain::Next(Site::ObjMapErase, Layer::ObjectIdMap));
    ApexUtil::SrwExclusive srw(g_lock);
    const bool ours = map == g_map && g_slots && node;
    const uint32_t lo = ours ? *reinterpret_cast<const uint32_t*>(node + kIdLo) : 0, hi = ours ? *reinterpret_cast<const uint32_t*>(node + kIdHi) : 0;
    next(map, edx, out, node, bucket);
    if (ours) {
        Drop(lo, hi);
        c_erases.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

bool Start(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_started) return true;
    std::string missing;
    if (!GameAddr::GroupAvailable("ObjectIdMap", &missing)) {
        if (error) *error = GameAddr::NotAvailable(missing);
        return false;
    }
    // Writers first, the reader last: the index is built on the first find, with both writers already hooked
    struct H {
        Site site;
        void* hook;
    };
    const H hooks[] = {{Site::ObjMapInsert, reinterpret_cast<void*>(&Hook_Insert)},
                       {Site::ObjMapErase, reinterpret_cast<void*>(&Hook_Erase)},
                       {Site::ObjMapFind, reinterpret_cast<void*>(&Hook_Find)}};
    for (size_t i = 0; i < std::size(hooks); i++) {
        std::string err;
        if (!EntryChain::Install(hooks[i].site, Layer::ObjectIdMap, hooks[i].hook, &err)) {
            for (size_t k = 0; k < i; k++) EntryChain::Remove(hooks[k].site, Layer::ObjectIdMap);
            if (error) *error = "could not hook the object map: " + err;
            return false;
        }
    }
    {
        ApexUtil::SrwExclusive srw(g_lock);
        FreeTable(); // built again on the first find: changes made while off were not seen
    }
    g_answers.store(0);
    g_off.store(false);
    g_on.store(true, std::memory_order_release);
    g_started = true;
    LOG_INFO(std::format("[ObjectIdMap] On: the object service's lookups by ID (find {:#010x}) answer from an index kept by its insert {:#010x} and "
                         "erase {:#010x}; the first {} answers are checked against the game",
                         GameAddr::Get(GameAddr::Id::ObjMapFind), GameAddr::Get(GameAddr::Id::ObjMapInsert), GameAddr::Get(GameAddr::Id::ObjMapErase),
                         kStartupChecks));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_started) return;
    g_on.store(false, std::memory_order_release);
    for (Site s : {Site::ObjMapFind, Site::ObjMapInsert, Site::ObjMapErase}) EntryChain::Remove(s, Layer::ObjectIdMap);
    {
        ApexUtil::SrwExclusive srw(g_lock);
        FreeTable();
    }
    g_started = false;
    LOG_INFO(std::format("[ObjectIdMap] Off ({} lookups: {} found, {} not found, {} left to the game while busy, {} checked; {} inserts, {} erases, {} grows)",
                         c_finds.load(), c_hits.load(), c_misses.load(), c_busy.load(), c_checked.load(), c_inserts.load(), c_erases.load(), c_grows.load()));
}

bool Running() { return g_on.load(std::memory_order_acquire); }

std::string StatusText() {
    if (!Running()) return "Object service index: off";
    if (g_off.load()) return "Object service index: turned itself off (see ApexRadiance_LOG.txt)";
    return std::format("Object service index: {} lookups ({} found, {} not found), {} checked against the game, all equal", c_finds.load(), c_hits.load(),
                       c_misses.load(), c_checked.load());
}

} // namespace ObjectIdMap
