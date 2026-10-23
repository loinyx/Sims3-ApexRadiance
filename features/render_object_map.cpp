// Scene object map index (see render_object_map.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "render_object_map.h"
#include "apex_log.h"
#include "apex_util.h"
#include "hook_guard.h"
#include "build_flavor.h"
#include "entry_chain.h"
#include "game_addresses.h"
#include <windows.h>
#include <atomic>
#include <format>
#include <mutex>

namespace RenderObjectMap {
namespace {

using EntryChain::Layer;
using EntryChain::Site;

constexpr uint32_t kBuckets = 0x409, kEndOff = 0x1024, kCountOff = 0x1028;
constexpr uint32_t kIdLo = 8, kIdHi = 0xC, kNext = 0; // (the object service's map links at +0x10)
constexpr uint32_t kMinSlots = 4096, kMaxSlots = 1u << 20;
constexpr uint32_t kMaxNodes = 1u << 19;
constexpr uint32_t kStartupChecks = 256, kDevCheckEvery = 64;
constexpr uint32_t kMaps = 8, kMaxRebuilds = 16;
constexpr uintptr_t kTomb = 1;

using FnFind = void(__fastcall*)(uint8_t* map, void* edx, uint32_t* out, const uint32_t* key);
using FnInsert = void(__fastcall*)(uint8_t* map, void* edx, uint32_t* out, uint8_t* node, uint32_t flag);
using FnErase = void(__fastcall*)(uint8_t* map, void* edx, uint32_t* out, uint8_t* node, uint32_t* bucket);

struct Slot {
    uint32_t lo, hi;
    uintptr_t node; // 0 = empty, kTomb = erased
};
struct Index {
    uint8_t* map = nullptr; // the map indexed (nullptr = a free entry)
    Slot* slots = nullptr;  // nullptr = to build at its next find
    uint32_t mask = 0, live = 0, used = 0;
    uint32_t rebuilds = 0;
    bool left = false; // left to the game (too many rebuilds, too big, unreadable)
};

// guarded by g_lock (exclusive: insert / erase / build; shared: find)
SRWLOCK g_lock = SRWLOCK_INIT;
Index g_idx[kMaps];
std::atomic<uint32_t> g_answers[kMaps]; // per entry of g_idx: answers given (under the shared lock, from any thread)

std::mutex g_ctrl;
bool g_started = false;
std::atomic<bool> g_on{false}, g_off{false}; // g_off: turned itself off for the session
std::atomic<uint64_t> c_finds{0}, c_hits{0}, c_misses{0}, c_busy{0}, c_checked{0}, c_inserts{0}, c_erases{0}, c_grows{0}, c_rebuilds{0}, c_passed{0};
std::atomic<uint32_t> g_maps{0};

uint32_t Hash(uint32_t lo, uint32_t hi) {
    uint32_t h = lo * 0x9E3779B1u ^ (hi + 0x7F4A7C15u) * 0x85EBCA77u;
    return h ^ (h >> 15);
}

// caller holds the lock (any mode)
Index* IndexOf(const uint8_t* map) {
    for (Index& x : g_idx)
        if (x.map == map) return &x;
    return nullptr;
}

uintptr_t Lookup(const Index& x, uint32_t lo, uint32_t hi) {
    for (uint32_t i = Hash(lo, hi) & x.mask, n = 0; n <= x.mask; i = (i + 1) & x.mask, n++) {
        const Slot& s = x.slots[i];
        if (!s.node) return 0;
        if (s.node != kTomb && s.lo == lo && s.hi == hi) return s.node;
    }
    return 0;
}

void Put(Index& x, uint32_t lo, uint32_t hi, uintptr_t node) {
    uint32_t tomb = UINT32_MAX;
    for (uint32_t i = Hash(lo, hi) & x.mask, n = 0; n <= x.mask; i = (i + 1) & x.mask, n++) {
        Slot& s = x.slots[i];
        if (s.node == kTomb) {
            if (tomb == UINT32_MAX) tomb = i;
            continue;
        }
        if (!s.node) {
            Slot& d = x.slots[tomb != UINT32_MAX ? tomb : i];
            if (tomb == UINT32_MAX) x.used++;
            d = {lo, hi, node};
            x.live++;
            return;
        }
        if (s.lo == lo && s.hi == hi) {
            s.node = node;
            return;
        }
    }
}

void Drop(Index& x, uint32_t lo, uint32_t hi) {
    for (uint32_t i = Hash(lo, hi) & x.mask, n = 0; n <= x.mask; i = (i + 1) & x.mask, n++) {
        Slot& s = x.slots[i];
        if (!s.node) return;
        if (s.node != kTomb && s.lo == lo && s.hi == hi) {
            s.node = kTomb;
            x.live--;
            return;
        }
    }
}

void FreeTable(Index& x) {
    if (x.slots) VirtualFree(x.slots, 0, MEM_RELEASE);
    x.slots = nullptr;
    x.mask = x.live = x.used = 0;
}

bool Rehash(Index& x, uint32_t slots) {
    Slot* const fresh = static_cast<Slot*>(VirtualAlloc(nullptr, sizeof(Slot) * slots, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!fresh) return false;
    Slot* const old = x.slots;
    const uint32_t oldSlots = old ? x.mask + 1 : 0;
    x.slots = fresh;
    x.mask = slots - 1;
    x.live = x.used = 0;
    for (uint32_t i = 0; i < oldSlots; i++)
        if (old[i].node > kTomb) Put(x, old[i].lo, old[i].hi, old[i].node);
    if (old) VirtualFree(old, 0, MEM_RELEASE);
    c_grows.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// exclusive, before an insert: room for one more under 70 % (live + erased)
bool Room(Index& x) {
    if ((x.used + 1) * 10 <= (x.mask + 1) * 7) return true;
    uint32_t slots = x.mask + 1;
    while ((x.live + 1) * 10 > slots * 4 && slots < kMaxSlots) slots <<= 1;
    if ((x.live + 1) * 10 > slots * 7) return false;
    return Rehash(x, slots);
}

template <class Why> void TurnOff(Why&& why) noexcept {
    if (g_off.exchange(true)) return;
    try {
        LOG_ERROR("[RenderObjectMap] " + std::string(why()) + ". The scene's lookups by object ID go back to the game's own search for this session.");
    } catch (...) {
    }
}

// SEH only: every node of the game's map into the (fresh) table; -1 = unreadable / too many / a cycle
int64_t WalkMap(uint8_t* map, Slot* slots, uint32_t mask) {
    uint32_t n = 0;
    __try {
        for (uint32_t b = 0; b < kBuckets; b++)
            for (uintptr_t node = *reinterpret_cast<uintptr_t*>(map + b * 4); node; node = *reinterpret_cast<uintptr_t*>(node + kNext)) {
                if (++n > kMaxNodes || n > mask) return -1; // (release review: a full table would probe forever)
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

// exclusive: the table of x from the game's own links (no mutation can run: insert / erase wait on the lock)
bool Build(Index& x) {
    const uint32_t count = *reinterpret_cast<const uint32_t*>(x.map + kCountOff);
    FreeTable(x);
    if (count > kMaxNodes) {
        x.left = true;
        return false;
    }
    uint32_t slots = kMinSlots;
    while (slots < count * 3 && slots < kMaxSlots) slots <<= 1;
    x.slots = static_cast<Slot*>(VirtualAlloc(nullptr, sizeof(Slot) * slots, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!x.slots) {
        x.left = true;
        return false;
    }
    x.mask = slots - 1;
    const int64_t n = WalkMap(x.map, x.slots, x.mask);
    if (n < 0 || static_cast<uint32_t>(n) != count) {
        FreeTable(x);
        x.left = true;
        HookGuard::Try("RenderObjectMap unreadable note", [&] {
            LOG_WARNING(std::format("[RenderObjectMap] The scene map {:#010x} could not be read as expected ({} nodes linked, count {}): left to the game",
                                    reinterpret_cast<uintptr_t>(x.map), n, count));
        });
        return false;
    }
    x.live = x.used = static_cast<uint32_t>(n);
    c_rebuilds.fetch_add(1, std::memory_order_relaxed);
    HookGuard::Try("RenderObjectMap index note", [&] {
        if (x.rebuilds <= 1)
            LOG_INFO(std::format("[RenderObjectMap] Indexed the scene map {:#010x}: {} objects in {} slots ({} KB)", reinterpret_cast<uintptr_t>(x.map), n, slots,
                                 sizeof(Slot) * slots / 1024));
    });
    return true;
}

void __fastcall Hook_Find(uint8_t* map, void* edx, uint32_t* out, const uint32_t* key) {
    const auto next = reinterpret_cast<FnFind>(EntryChain::Next(Site::RenMapFind, Layer::RenderObjectMap));
    if (!g_on.load(std::memory_order_acquire) || g_off.load(std::memory_order_relaxed)) return next(map, edx, out, key);
    c_finds.fetch_add(1, std::memory_order_relaxed);
    if (!TryAcquireSRWLockShared(&g_lock)) {
        c_busy.fetch_add(1, std::memory_order_relaxed);
        return next(map, edx, out, key);
    }
    Index* x = IndexOf(map);
    const bool ready = x && x->slots && !x->left && *reinterpret_cast<const uint32_t*>(map + kCountOff) == x->live;
    if (!ready) {
        // a new map, or one to build (again): under the exclusive lock (the shared one first goes)
        ReleaseSRWLockShared(&g_lock);
        if (g_off.load(std::memory_order_relaxed)) return next(map, edx, out, key);
        {
            ApexUtil::SrwExclusive srw(g_lock);
            // (Stop turns g_on off, then frees the tables under this lock: a find that passed the test above before Stop builds
            // nothing once Stop has freed them; one that gets here first is freed by Stop after it)
            Index* y = g_on.load(std::memory_order_acquire) ? IndexOf(map) : nullptr;
            if (!y && g_on.load(std::memory_order_acquire))
                for (Index& f : g_idx)
                    if (!f.map) {
                        y = &f;
                        y->map = map;
                        g_answers[y - g_idx].store(0, std::memory_order_relaxed);
                        g_maps.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
            if (y && !y->left && (!y->slots || *reinterpret_cast<const uint32_t*>(map + kCountOff) != y->live)) {
                if (++y->rebuilds > kMaxRebuilds) {
                    FreeTable(*y);
                    y->left = true;
                    HookGuard::Try("RenderObjectMap rebuild note", [&] {
                        LOG_WARNING(std::format("[RenderObjectMap] The scene map {:#010x} changed past the hooks {} times: left to the game", reinterpret_cast<uintptr_t>(map),
                                                kMaxRebuilds));
                    });
                } else {
                    Build(*y);
                }
            }
        }
        c_passed.fetch_add(1, std::memory_order_relaxed);
        return next(map, edx, out, key); // (this one the game's walk; the next one from the table)
    }
    const uint32_t lo = key[0], hi = key[1];
    const uintptr_t node = Lookup(*x, lo, hi);
    uint32_t got[2];
    if (node) {
        got[0] = static_cast<uint32_t>(node);
        got[1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(map) + (lo % kBuckets) * 4);
    } else {
        got[0] = *reinterpret_cast<const uint32_t*>(map + kEndOff);
        got[1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(map) + kEndOff);
    }
    const uint32_t n = g_answers[x - g_idx].fetch_add(1, std::memory_order_relaxed);
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
    const auto next = reinterpret_cast<FnInsert>(EntryChain::Next(Site::RenMapInsert, Layer::RenderObjectMap));
    ApexUtil::SrwExclusive srw(g_lock); // the game's insert is a leaf: no game code waits on this lock inside it
    next(map, edx, out, node, flag);
    Index* x = IndexOf(map);
    if (x && x->slots && reinterpret_cast<const uint8_t*>(out)[8] == 1) {
        if (Room(*x)) {
            Put(*x, *reinterpret_cast<const uint32_t*>(node + kIdLo), *reinterpret_cast<const uint32_t*>(node + kIdHi), reinterpret_cast<uintptr_t>(node));
            c_inserts.fetch_add(1, std::memory_order_relaxed);
        } else {
            FreeTable(*x); // built again at its next find
        }
    }
}

void __fastcall Hook_Erase(uint8_t* map, void* edx, uint32_t* out, uint8_t* node, uint32_t* bucket) {
    const auto next = reinterpret_cast<FnErase>(EntryChain::Next(Site::RenMapErase, Layer::RenderObjectMap));
    ApexUtil::SrwExclusive srw(g_lock);
    Index* x = IndexOf(map);
    const bool ours = x && x->slots && node;
    const uint32_t lo = ours ? *reinterpret_cast<const uint32_t*>(node + kIdLo) : 0, hi = ours ? *reinterpret_cast<const uint32_t*>(node + kIdHi) : 0;
    next(map, edx, out, node, bucket);
    if (ours) {
        Drop(*x, lo, hi);
        c_erases.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

bool Start(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_started) return true;
    std::string missing;
    if (!GameAddr::GroupAvailable("RenderObjectMap", &missing)) {
        if (error) *error = GameAddr::NotAvailable(missing);
        return false;
    }
    // Writers first, the reader last: a table is built at a map's first find, with both writers already hooked
    struct H {
        Site site;
        void* hook;
    };
    const H hooks[] = {{Site::RenMapInsert, reinterpret_cast<void*>(&Hook_Insert)},
                       {Site::RenMapErase, reinterpret_cast<void*>(&Hook_Erase)},
                       {Site::RenMapFind, reinterpret_cast<void*>(&Hook_Find)}};
    for (size_t i = 0; i < std::size(hooks); i++) {
        std::string err;
        if (!EntryChain::Install(hooks[i].site, Layer::RenderObjectMap, hooks[i].hook, &err)) {
            for (size_t k = 0; k < i; k++) EntryChain::Remove(hooks[k].site, Layer::RenderObjectMap);
            if (error) *error = "could not hook the scene object map: " + err;
            return false;
        }
    }
    {
        ApexUtil::SrwExclusive srw(g_lock);
        for (Index& x : g_idx) FreeTable(x), x = Index{}; // built again at the first finds: changes made while off were not seen
        for (auto& a : g_answers) a.store(0, std::memory_order_relaxed);
    }
    g_maps.store(0);
    g_off.store(false);
    g_on.store(true, std::memory_order_release);
    g_started = true;
    LOG_INFO(std::format("[RenderObjectMap] On: the scene's lookups by object ID (find {:#010x}) answer from an index kept by its insert {:#010x} and "
                         "erase {:#010x}; the first {} answers of each map are checked against the game",
                         GameAddr::Get(GameAddr::Id::RenMapFind), GameAddr::Get(GameAddr::Id::RenMapInsert), GameAddr::Get(GameAddr::Id::RenMapErase),
                         kStartupChecks));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_started) return;
    g_on.store(false, std::memory_order_release);
    for (Site s : {Site::RenMapFind, Site::RenMapInsert, Site::RenMapErase}) EntryChain::Remove(s, Layer::RenderObjectMap);
    {
        ApexUtil::SrwExclusive srw(g_lock);
        for (Index& x : g_idx) FreeTable(x), x = Index{};
    }
    g_started = false;
    LOG_INFO(std::format("[RenderObjectMap] Off ({} lookups: {} found, {} not found, {} left to the game while busy, {} while a table was built, {} checked; "
                         "{} inserts, {} erases, {} grows, {} tables built)",
                         c_finds.load(), c_hits.load(), c_misses.load(), c_busy.load(), c_passed.load(), c_checked.load(), c_inserts.load(), c_erases.load(),
                         c_grows.load(), c_rebuilds.load()));
}

bool Running() { return g_on.load(std::memory_order_acquire); }

std::string StatusText() {
    if (!Running()) return "Scene object index: off";
    if (g_off.load()) return "Scene object index: turned itself off (see ApexRadiance_LOG.txt)";
    return std::format("Scene object index: {} lookups ({} found, {} not found) in {} maps, {} checked against the game, all equal", c_finds.load(), c_hits.load(),
                       c_misses.load(), g_maps.load(), c_checked.load());
}

} // namespace RenderObjectMap
