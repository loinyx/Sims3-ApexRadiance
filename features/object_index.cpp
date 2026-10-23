#include "ui/widgets.h"
#include "developer_settings.h"
// Object lookup index (see object_index.h and docs/features/performance.md, section "Faster object lookups (C8)").
//
// ---- The game side (Steam 1.67.2, TS3W.exe; research\engine_map\full.asm; addresses through framework/game_addresses.h) ----
//   0x00C62D40 ObjectById, thiscall(mgr, idLo, idHi, int* visited), ret 0xC: r = 0x00C60D30(mgr, idLo, idHi, visited);
//              returns r if r && r->vfunc+0x40() == 1, else 0. 233 direct callers; every one passes visited = 0 (push 0 or
//              a register just zeroed; checked 2026-09-29); a non-zero one is passed straight to the game here anyway.
//   0x00C60D30 walk, thiscall(mgr, idLo, idHi, visited), ret 0xC: id 0 -> 0; for i in [0, (mgr[+0xA0] - mgr[+0x9C]) / 4)
//              (size re-read every step): r = 0x00C5FA60(roots[i], idLo, idHi, visited); first non-zero wins.
//   0x00C5FA60 search, cdecl(node, idLo, idHi, visited): node 0 -> 0; node id (+0x48, +0x4C) == key -> node (whatever its
//              type); ++*visited if given; if node->vfunc+0x40() == 2: for i in [0, vfunc+0x58()) (re-read every step):
//              r = search(node->vfunc+0x4C(&i), ...); first non-zero wins. So: the first node in depth-first order
//              (roots in vector order, children in index order, a node before its children) whose id is the key.
//   Tree classes (the only two constructors that call the tree base constructor 0x00C71980, both reached through the
//   WorldManager factory 0x00C64420 and its siblings with "new"):
//     Layer (type 2) vtable 0x010641B0, ctor 0x00AA9370, 0xB8 bytes ("Lot/ObjMgr - Layer"): children = vector of node
//       pointers at +0xA0 / +0xA4 (+0x40 0x00B742C0 "mov eax,2; ret", +0x58 0x00AA93C0 = (end - begin) >> 2, +0x4C
//       0x00AA9190 = i < count ? begin[i] : 0). Mutators, reached only through the Layer vtable slots (no direct CALL):
//       +0x44 AddChild 0x00AAA080 (sets child+0x10 = layer, AddRef, push_back; a child with a parent is first removed from
//       it, a parentless one from the WorldManager roots 0x00C65A20), +0x50 RemoveChildById 0x00AA9830, +0x54 RemoveChild
//       0x00AA9780 (erase, child+0x10 = 0, Release), +0x5C RemoveAll 0x00AA9920, +0x14 Clear 0x00AA9430 (also from the
//       destructor 0x00AA9500), +0x60 SetId 0x00AA9020.
//     Lot (type 1) vtable 0x01065268, ctor 0x00AC19F0, 0x4C0 bytes ("Lot/ObjMgr - Lot"): +0x40 0x00619EF0 "mov eax,1;
//       ret", +0x60 SetId 0x00AB3300.
//     Ids: written only by the base SetId 0x00C6DDF0 (called by the two SetId overrides) and zeroed by the base
//       constructor (the only [+0x48] / [+0x4C] pair writes in the lot and WorldManager code ranges).
//     Roots (mgr+0x9C..+0xA4): written by eight non-virtual WorldManager methods (0x00C65A20, 0x00C65CF0, 0x00C65E00,
//       0x00C66260, 0x00C66310, 0x00C66AB0, 0x00C6CF80, ctor 0x00C671E0). Not hooked: see below.
//   Threads: the render thread (lot lighting 0x00AD7620, camera 0x0096EAE0) and the simulation thread (script natives
//   0x00784F50..0x00797A60). The game's walk takes no lock.
//
// ---- The index ----
//   Key (mgr, idLo, idHi) -> the path where the game's walk found the object: depth d (1..6), for each level k the node
//   pointer ptr[k], its vtable vt[k] and its index idx[k] (idx[0] in the roots, idx[k] in ptr[k-1]'s children). Built
//   after the game's walk returned r: the parent chain r, r+0x10, ... (AddChild's parent pointer; a hint only), each
//   index found by searching the parent's vector, each class checked (containers must have the Layer shape: +0x40 returns
//   2 and +0x4C / +0x58 are the exact vector accessors above, byte for byte; the object must have +0x40 returning 1), then
//   validated once like a hit before it is stored.
//   A hit (Validate): from mgr's live root vector down, idx[k] < size and vector[idx[k]] == ptr[k]; vtable of ptr[k] ==
//   vt[k]; containers' ids != key; the object's id == key. That is exactly what the game's walk reads to reach that node,
//   so the walk would reach it and return it, unless an earlier node in walk order has the same id (the residual
//   assumption, INFERRED: lot ids are unique; checked by the verification). The reads start from memory the caller already
//   trusts (mgr) and each next pointer is taken from a vector the game is using right now, so only live objects are read
//   (and SEH guards against a race with a mutator on another thread, which the game's own walk does not guard).
//   Entries older than kMaxAgeMs are walked again. A remembered path that no longer validates restarts the whole table
//   (a structural change was seen; every answer is proven again by a walk).
//   Why no mutator hooks: the Layer mutators are hookable through their vtable slots, but the root vector's writers are
//   eight direct-called WorldManager methods and Layer loading; unverifiable completeness -> the conservative variant the
//   plan allows: validate every answer, fall back to the game's walk.
//
// Part of Apex Radiance. Credits: @loinyx

#include "object_index.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "entry_chain.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include "imgui.h"
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace ObjectIndex {
namespace {

using EntryChain::Layer;
using EntryChain::Site;

// ---- the game's code, as disassembled (Steam 1.67.2); wildcards on rel32s (and on the lookup's first 8 bytes, which
//      the entry chain owns and checks itself) ----
constexpr const char* kLookupBody = "?? ?? ?? ?? ?? ?? ?? ?? 56 50 8B 44 24 0C 52 50 E8 ?? ?? ?? ?? 8B F0 85 F6 74 14 8B 16 8B 42 40 8B CE FF D0 83 F8 01 75 06 8B C6 5E "
                                    "C2 0C 00 33 C0 5E C2 0C 00";
constexpr uint32_t kLookupWalkCall = 0x10;
constexpr const char* kWalkBody =
    "53 8B 5C 24 08 55 8B 6C 24 10 56 8B F1 8B CB 33 C0 0B CD 74 4F 8B 96 A0 00 00 00 2B 96 9C 00 00 "
    "00 57 33 FF C1 FA 02 74 3A 8D A4 24 00 00 00 00 8B 4C 24 1C 8B 86 9C 00 00 00 8B 04 B8 51 55 53 "
    "50 E8 ?? ?? ?? ?? 83 C4 10 85 C0 75 16 8B 96 A0 00 00 00 2B 96 9C 00 00 00 83 C7 01 C1 FA 02 3B "
    "FA 72 CD 5F 5E 5D 5B C2 0C 00";
constexpr uint32_t kWalkSearchCall = 0x41;
constexpr const char* kSearchBody =
    "53 55 56 8B 74 24 10 85 F6 0F 84 80 00 00 00 8B 46 48 8B 5C 24 14 3B C3 8B 6C 24 18 75 07 8B 4E "
    "4C 3B CD 74 6A 57 8B 7C 24 20 85 FF 74 03 83 07 01 8B 16 8B 42 40 8B CE FF D0 83 F8 02 75 49 8B "
    "16 8B 42 58 8B CE C7 44 24 14 00 00 00 00 FF D0 39 44 24 14 73 32 8B 16 8B 52 4C 57 55 53 8D 44 "
    "24 20 50 8B CE FF D2 50 E8 ?? ?? ?? ?? 83 C4 10 85 C0 75 16 8B 06 8B 50 58 83 44 24 14 01 8B CE "
    "FF D2 39 44 24 14 72 CE 33 C0 5F 5E 5D 5B C3 8B C6 5E 5D 5B C3";
constexpr uint32_t kSearchSelfCall = 0x68;
// Class shapes: the functions in the vtable slots the walk calls
constexpr const char* kReturns1 = "B8 01 00 00 00 C3";
constexpr const char* kReturns2 = "B8 02 00 00 00 C3";
constexpr const char* kLayerCount = "8B 81 A4 00 00 00 2B 81 A0 00 00 00 C1 F8 02 C3";
constexpr const char* kLayerChild = "8B 91 A4 00 00 00 2B 91 A0 00 00 00 8B 44 24 04 8B 00 C1 FA 02 3B C2 73 0C 8B 89 A0 00 00 00 8B 04 81 C2 04 00 33 C0 C2 04 00";
constexpr uint32_t kSlotType = 0x40, kSlotChild = 0x4C, kSlotCount = 0x58;
// Offsets (read from the patterns above)
constexpr uint32_t kRootsBegin = 0x9C, kRootsEnd = 0xA0; // mgr
constexpr uint32_t kChildBegin = 0xA0, kChildEnd = 0xA4; // Layer
constexpr uint32_t kIdLo = 0x48, kIdHi = 0x4C;           // every node
constexpr uint32_t kParent = 0x10;                       // every node (set by AddChild; a hint)

// ---- table ----
constexpr int kMaxDepth = 6;
constexpr uint32_t kSlots = 4096; // x 84 bytes = 344 KB, allocated once, never freed
constexpr uint32_t kMask = kSlots - 1;
constexpr uint32_t kMaxFill = kSlots / 4 * 3;
constexpr int kMaxProbe = 32;
constexpr uint32_t kMaxAgeMs = 2000;
constexpr uint32_t kMaxVector = 1u << 16; // longer vectors are not searched when a path is built
constexpr uint32_t kFirstChecks = 64;      // the first answers of each session are always checked

struct Entry {
    uint32_t mgr, idLo, idHi;
    uint32_t stamp; // == g_stamp: live
    uint32_t tick;  // GetTickCount when stored
    uint32_t depth;
    uint32_t ptr[kMaxDepth];
    uint32_t vt[kMaxDepth];
    uint16_t idx[kMaxDepth];
};
static_assert(sizeof(Entry) == 84, "Entry layout");

Entry* g_table = nullptr; // written under g_lock (exclusive), read under g_lock (shared)
SRWLOCK g_lock = SRWLOCK_INIT;
uint32_t g_stamp = 1;    // guarded by g_lock
uint32_t g_stampGen = 0; // the generation the stamp belongs to
uint32_t g_count = 0;

std::atomic<uint32_t> g_gen{1}; // bumped: start, stop, a path that no longer validates, a check that found a difference
std::atomic<bool> g_on{false};
std::atomic<bool> g_selfDisabled{false};

std::mutex g_ctrl;
bool g_started = false;

// Recognised class shapes (vtable -> kind), filled on the store path under g_shapeLock
enum Shape : uint8_t { ShapeNone, ShapeLayer, ShapeObject };
struct KnownVt {
    uint32_t vt;
    uint8_t shape;
};
constexpr int kKnownMax = 32;
KnownVt g_known[kKnownMax] = {};
int g_knownCount = 0;
std::mutex g_shapeLock;
std::atomic<uint32_t> g_layerClasses{0}, g_objectClasses{0};

// ---- statistics ----
// (2.15.3) A 64-bit atomic add is a lock cmpxchg8b loop on 32-bit x86, and a lookup made up to three of them (150 to 180
// thousand lookups in a room-solve frame): each thread now counts in its own slots and adds them to the shared totals
// every kFlushEvery counts. The totals (status text only) lag by at most that many per thread.
constexpr uint32_t kFlushEvery = 256;
constexpr int kCounters = 13;
thread_local uint32_t t_counts[kCounters] = {};
struct Counter {
    int slot;
    std::atomic<uint64_t> v{0};
    explicit Counter(int s) : slot(s) {}
    void Add() {
        uint32_t& c = t_counts[slot];
        if (++c >= kFlushEvery) {
            v.fetch_add(c, std::memory_order_relaxed);
            c = 0;
        }
    }
    uint64_t Get() const { return v.load(std::memory_order_relaxed); }
};
Counter c_lookups{0}, c_hits{1}, c_walks{2}, c_passed{3}, c_notFound{4}, c_stored{5}, c_notStored{6}, c_expired{7}, c_rejected{8},
    c_verified{9}, c_mismatches{10}, c_inconclusive{11}, c_restarts{12};
std::atomic<uint64_t> g_hitTicks{0}, g_walkTicks{0}; // development build only
std::atomic<uint32_t> g_hitSerial{0}; // 32-bit: lock xadd (a wrap only checks the first answers again)
double g_qpcMs = 0.0;
std::atomic<int> g_verifyEvery{64};
std::atomic<uint64_t> g_verifyAllUntil{0};
std::mutex g_mismatchLock;
std::string g_lastMismatch;
thread_local LookupNote t_note;

using FnLookup = uint32_t(__fastcall*)(void* mgr, void* edx, uint32_t idLo, uint32_t idHi, uint32_t visited);

// ---- helpers ----
bool MatchAt(uintptr_t addr, const char* pattern) {
    std::vector<int> want;
    for (const char* p = pattern; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (p[0] == '?') {
            want.push_back(-1);
            while (*p == '?') p++;
            continue;
        }
        want.push_back(static_cast<int>(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
        p += 2;
    }
    std::vector<uint8_t> have(want.size());
    if (!addr || !MemPatch::ReadBytes(addr, have.data(), have.size())) return false;
    for (size_t i = 0; i < want.size(); i++)
        if (want[i] >= 0 && have[i] != static_cast<uint8_t>(want[i])) return false;
    return true;
}

uintptr_t CallTargetAt(uintptr_t call) {
    uint8_t b[5] = {};
    if (!MemPatch::ReadBytes(call, b, 5) || b[0] != 0xE8) return 0;
    int32_t rel;
    std::memcpy(&rel, b + 1, 4);
    return call + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
}

int64_t Qpc() {
    if (kPublicBuild) return 0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

uint32_t Hash(uint32_t mgr, uint32_t lo, uint32_t hi) {
    uint32_t h = lo * 0x9E3779B1u ^ hi * 0x85EBCA6Bu ^ (mgr >> 4) * 0xC2B2AE35u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

uint32_t Rd(uintptr_t a) { return *reinterpret_cast<const volatile uint32_t*>(a); }

// The shape of a vtable (known ones from the list; new ones checked by their functions' bytes). Store path only.
Shape ShapeOf(uint32_t vt) {
    std::lock_guard<std::mutex> lock(g_shapeLock);
    for (int i = 0; i < g_knownCount; i++)
        if (g_known[i].vt == vt) return static_cast<Shape>(g_known[i].shape);
    uint32_t type = 0, child = 0, count = 0;
    Shape s = ShapeNone;
    if (MemPatch::ReadBytes(vt + kSlotType, &type, 4) && MemPatch::ReadBytes(vt + kSlotChild, &child, 4) && MemPatch::ReadBytes(vt + kSlotCount, &count, 4)) {
        if (MatchAt(type, kReturns2) && MatchAt(child, kLayerChild) && MatchAt(count, kLayerCount)) s = ShapeLayer;
        else if (MatchAt(type, kReturns1)) s = ShapeObject;
    }
    if (g_knownCount < kKnownMax) {
        g_known[g_knownCount++] = {vt, static_cast<uint8_t>(s)};
        if (s == ShapeLayer) g_layerClasses.fetch_add(1);
        if (s == ShapeObject) g_objectClasses.fetch_add(1);
    }
    return s;
}

// ---- validation: the live tree still leads to e.ptr[depth-1] through the remembered path ----
enum Why : int { Ok, BadRoot, BadClass, BadChild, BadId, EarlierMatch, Fault };

Why ValidateRaw(uint32_t mgr, const Entry& e, uint32_t lo, uint32_t hi) {
    const uint32_t rb = Rd(mgr + kRootsBegin), re = Rd(mgr + kRootsEnd);
    if (re < rb || e.idx[0] >= (re - rb) / 4 || Rd(rb + 4u * e.idx[0]) != e.ptr[0]) return BadRoot;
    for (uint32_t k = 0; k < e.depth; k++) {
        const uint32_t n = e.ptr[k];
        if (Rd(n) != e.vt[k]) return BadClass;
        const bool match = Rd(n + kIdLo) == lo && Rd(n + kIdHi) == hi;
        if (k + 1 == e.depth) return match ? Ok : BadId;
        if (match) return EarlierMatch; // the walk would stop at this container (and the lookup return 0)
        const uint32_t cb = Rd(n + kChildBegin), ce = Rd(n + kChildEnd);
        if (ce < cb || e.idx[k + 1] >= (ce - cb) / 4 || Rd(cb + 4u * e.idx[k + 1]) != e.ptr[k + 1]) return BadChild;
    }
    return BadId;
}

Why Validate(uint32_t mgr, const Entry& e, uint32_t lo, uint32_t hi) {
    __try {
        return ValidateRaw(mgr, e, lo, hi);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return Fault;
    }
}

// ---- building a path for the object the game's walk returned ----
// Parent chain (hint), root index, child indices; false when it cannot be expressed (then nothing is stored)
bool BuildRaw(uint32_t mgr, uint32_t obj, Entry& e) {
    uint32_t chain[kMaxDepth + 1];
    int d = 0;
    chain[d++] = obj;
    for (;;) {
        const uint32_t p = Rd(chain[d - 1] + kParent);
        if (!p) break;
        if (d == kMaxDepth) return false; // deeper than we keep
        chain[d++] = p;
    }
    e.depth = static_cast<uint32_t>(d);
    for (int k = 0; k < d; k++) e.ptr[k] = chain[d - 1 - k];
    // root index
    const uint32_t rb = Rd(mgr + kRootsBegin), re = Rd(mgr + kRootsEnd);
    if (re < rb) return false;
    const uint32_t roots = (re - rb) / 4;
    uint32_t i = 0;
    for (; i < roots && i < kMaxVector; i++)
        if (Rd(rb + 4u * i) == e.ptr[0]) break;
    if (i >= roots || i >= kMaxVector) return false;
    e.idx[0] = static_cast<uint16_t>(i);
    // child indices
    for (int k = 0; k + 1 < d; k++) {
        const uint32_t n = e.ptr[k];
        const uint32_t cb = Rd(n + kChildBegin), ce = Rd(n + kChildEnd);
        if (ce < cb) return false;
        const uint32_t count = (ce - cb) / 4;
        uint32_t j = 0;
        for (; j < count && j < kMaxVector; j++)
            if (Rd(cb + 4u * j) == e.ptr[k + 1]) break;
        if (j >= count || j >= kMaxVector) return false;
        e.idx[k + 1] = static_cast<uint16_t>(j);
    }
    for (int k = 0; k < d; k++) e.vt[k] = Rd(e.ptr[k]);
    return true;
}

bool Build(uint32_t mgr, uint32_t obj, Entry& e) {
    __try {
        return BuildRaw(mgr, obj, e);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- table ----
bool Find(uint32_t mgr, uint32_t lo, uint32_t hi, uint32_t gen, Entry& out) {
    // The caller still validates every remembered parent/child and ID against the live tree.
    // Consecutive lookups of one lot need not acquire the shared table lock again.
    // (2.15.3) 16 recent answers per thread (was the last one only): a room solve alternates between a few owners, and
    // each miss here took the shared SRW lock. Same answers as the shared table (the caller validates every one).
    struct Recent {
        Entry e{};
        uint32_t gen = 0;
    };
    static thread_local Recent recent[16];
    Recent& last = recent[Hash(mgr, lo, hi) & 15u];
    if(last.gen==gen&&last.e.mgr==mgr&&last.e.idLo==lo&&last.e.idHi==hi&&GetTickCount()-last.e.tick<=kMaxAgeMs){out=last.e;return true;}
    bool found = false;
    AcquireSRWLockShared(&g_lock);
    if (g_table && g_stampGen == gen) {
        uint32_t s = Hash(mgr, lo, hi) & kMask;
        for (int p = 0; p < kMaxProbe; p++, s = (s + 1) & kMask) {
            const Entry& e = g_table[s];
            if (e.stamp != g_stamp) break; // empty: not stored
            if (e.mgr == mgr && e.idLo == lo && e.idHi == hi) {
                out = e;
                found = true;
                break;
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    if(found){last.e=out;last.gen=gen;}
    return found;
}

void Store(Entry& e, uint32_t gen) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_table && g_gen.load(std::memory_order_acquire) == gen) {
        if (g_stampGen != gen || g_count >= kMaxFill) {
            if (g_stampGen == gen) c_restarts.Add(); // full
            g_stamp++;
            if (!g_stamp) g_stamp = 1;
            g_stampGen = gen;
            g_count = 0;
        }
        uint32_t s = Hash(e.mgr, e.idLo, e.idHi) & kMask;
        uint32_t home = s;
        bool placed = false;
        for (int p = 0; p < kMaxProbe; p++, s = (s + 1) & kMask) {
            Entry& t = g_table[s];
            if (t.stamp != g_stamp) {
                e.stamp = g_stamp;
                t = e;
                g_count++;
                placed = true;
                break;
            }
            if (t.mgr == e.mgr && t.idLo == e.idLo && t.idHi == e.idHi) {
                e.stamp = g_stamp;
                t = e;
                placed = true;
                break;
            }
        }
        if (!placed) { // the probe run is full: replace the home slot (lookups stop at the first empty slot, so no hole is made)
            e.stamp = g_stamp;
            g_table[home] = e;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void Restart() {
    g_gen.fetch_add(1, std::memory_order_acq_rel);
    c_restarts.Add();
}

bool CheckThisOne() {
    const uint32_t n = g_hitSerial.fetch_add(1, std::memory_order_relaxed);
    if (n < kFirstChecks) return true;
    const auto until=g_verifyAllUntil.load(std::memory_order_relaxed);
    if (until&&GetTickCount64()<until) return true;
    const int every = g_verifyEvery.load(std::memory_order_relaxed);
    return every > 0 && n % static_cast<uint32_t>(every) == 0;
}

void RecordMismatch(uint32_t mgr, uint32_t lo, uint32_t hi, const Entry& e, uint32_t game) {
    std::string path;
    for (uint32_t k = 0; k < e.depth; k++) path += std::format("{}[{}] {:#010x} (vt {:#010x})", k ? " / " : "", e.idx[k], e.ptr[k], e.vt[k]);
    const std::string line = std::format("manager {:#010x}, id {:08x}{:08x}: index {:#010x} via {}, the game's walk {:#010x}", mgr, hi, lo, e.ptr[e.depth - 1], path, game);
    {
        std::lock_guard<std::mutex> lock(g_mismatchLock);
        g_lastMismatch = line;
    }
    LOG_ERROR("[ObjectIndex] Verification mismatch: " + line + "; the index turns itself off for this session");
}

// ---- the hook: inner layer of the entry chain on 0x00C62D40 ----
uint32_t __fastcall Hook_ObjectById(void* self, void* edx, uint32_t idLo, uint32_t idHi, uint32_t visited) {
    const FnLookup next = reinterpret_cast<FnLookup>(EntryChain::Next(Site::ObjectById, Layer::ObjectIndex));
    t_note = LookupNote{};
    if (!g_on.load(std::memory_order_acquire) || g_selfDisabled.load(std::memory_order_relaxed) || visited || !self || !(idLo | idHi)) {
        c_passed.Add();
        return next(self, edx, idLo, idHi, visited);
    }
    t_note.seen = true;
    c_lookups.Add();
    const uint32_t mgr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    const int64_t t0 = kPublicBuild ? 0 : Qpc();
    const uint32_t gen = g_gen.load(std::memory_order_acquire);
    Entry e;
    if (Find(mgr, idLo, idHi, gen, e)) {
        if (GetTickCount() - e.tick > kMaxAgeMs) {
            c_expired.Add();
        } else {
            const Why why = Validate(mgr, e, idLo, idHi);
            if (why == Ok && g_gen.load(std::memory_order_acquire) == gen) {
                const uint32_t obj = e.ptr[e.depth - 1];
                if (CheckThisOne()) {
                    const uint32_t game = next(self, edx, idLo, idHi, 0);
                    if (game == obj) {
                        c_verified.Add();
                    } else if (Validate(mgr, e, idLo, idHi) != Ok || g_gen.load(std::memory_order_acquire) != gen) {
                        c_inconclusive.Add(); // the tree changed during the check
                    } else {
                        c_mismatches.Add();
                        RecordMismatch(mgr, idLo, idHi, e, game);
                        g_selfDisabled.store(true);
                        Restart();
                    }
                    return game; // a checked lookup answers with the game's own result
                }
                c_hits.Add();
                t_note.hit = true;
                if (!kPublicBuild) g_hitTicks.fetch_add(static_cast<uint64_t>(Qpc() - t0), std::memory_order_relaxed);
                return obj;
            }
            if (why != Ok) {
                c_rejected.Add();
                Restart(); // a structural change was seen: every remembered answer is proven again by a walk
            }
        }
    }
    // the game's own walk
    const uint32_t gen0 = g_gen.load(std::memory_order_acquire);
    const int64_t w0 = kPublicBuild ? 0 : Qpc();
    const uint32_t r = next(self, edx, idLo, idHi, 0);
    if (!kPublicBuild) g_walkTicks.fetch_add(static_cast<uint64_t>(Qpc() - w0), std::memory_order_relaxed);
    c_walks.Add();
    if (!r) {
        c_notFound.Add();
        return r;
    }
    Entry n = {};
    n.mgr = mgr;
    n.idLo = idLo;
    n.idHi = idHi;
    bool ok = Build(mgr, r, n);
    if (ok) {
        for (uint32_t k = 0; k < n.depth && ok; k++) ok = ShapeOf(n.vt[k]) == (k + 1 == n.depth ? ShapeObject : ShapeLayer);
    }
    if (ok) ok = Validate(mgr, n, idLo, idHi) == Ok && g_gen.load(std::memory_order_acquire) == gen0;
    if (ok) {
        n.tick = GetTickCount();
        Store(n, gen0);
        c_stored.Add();
    } else {
        c_notStored.Add();
    }
    return r;
}

} // namespace

bool Start(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_started) return true;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::string missing;
    if (!GameAddr::GroupAvailable("ObjectIndex", &missing)) return fail(GameAddr::NotAvailable(missing));
    const uintptr_t lookup = GameAddr::Get(GameAddr::Id::ObjectById), walk = GameAddr::Get(GameAddr::Id::ObjectTreeWalk),
                    search = GameAddr::Get(GameAddr::Id::ObjectTreeSearch);
    // The three functions must be exactly the walk this index reproduces the reads of
    if (!MatchAt(lookup, kLookupBody) || CallTargetAt(lookup + kLookupWalkCall) != walk)
        return fail(std::format("The object lookup at {:#010x} is not the code Apex was written for", lookup));
    if (!MatchAt(walk, kWalkBody) || CallTargetAt(walk + kWalkSearchCall) != search)
        return fail(std::format("The object tree walk at {:#010x} is not the code Apex was written for", walk));
    if (!MatchAt(search, kSearchBody) || CallTargetAt(search + kSearchSelfCall) != search)
        return fail(std::format("The object tree search at {:#010x} is not the code Apex was written for", search));
    if (!g_table) {
        g_table = static_cast<Entry*>(VirtualAlloc(nullptr, kSlots * sizeof(Entry), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)); // zeroed: every stamp 0 = empty
        if (!g_table) return fail("Could not allocate the table (344 KB)");
    }
    if (g_qpcMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    Restart(); // nothing remembered before this start is used again
    g_hitSerial.store(0);
    g_selfDisabled.store(false);
    g_on.store(true, std::memory_order_release); // before the entry can reach the hook
    std::string err;
    if (!EntryChain::Install(Site::ObjectById, Layer::ObjectIndex, reinterpret_cast<void*>(&Hook_ObjectById), &err)) {
        g_on.store(false);
        return fail("Could not hook the object lookup: " + err);
    }
    g_started = true;
    LOG_INFO(std::format("[ObjectIndex] On: object lookup {:#010x} (walk {:#010x}, search {:#010x}) answered from a validated index of {} entries; checks: the first {} "
                         "answers, then 1 in {}",
                         lookup, walk, search, kSlots, kFirstChecks, g_verifyEvery.load()));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_started) return;
    g_on.store(false, std::memory_order_release); // the layer passes every call through from now on
    EntryChain::Remove(Site::ObjectById, Layer::ObjectIndex);
    Restart();
    g_started = false;
    const Stats s = GetStats();
    LOG_INFO(std::format("[ObjectIndex] Off ({} lookups, {} answered from the index, {} walks, {} paths changed, checks {} equal / {} different / {} inconclusive)", s.lookups,
                         s.hits, s.walks, s.rejected, s.verified, s.mismatches, s.inconclusive));
}

bool Running() { return g_on.load(std::memory_order_acquire); }

void SetVerifyEvery(int n) { g_verifyEvery.store(n < 0 ? 0 : n); }

void VerifyAllFor(double seconds) { g_verifyAllUntil.store(GetTickCount64() + static_cast<uint64_t>(seconds * 1000.0)); }

LookupNote TakeLookupNote() {
    LookupNote n = t_note;
    t_note = LookupNote{};
    return n;
}

Stats GetStats() {
    Stats s;
    s.lookups = c_lookups.Get();
    s.hits = c_hits.Get();
    s.walks = c_walks.Get();
    s.passed = c_passed.Get();
    s.notFound = c_notFound.Get();
    s.stored = c_stored.Get();
    s.notStored = c_notStored.Get();
    s.expired = c_expired.Get();
    s.rejected = c_rejected.Get();
    s.verified = c_verified.Get();
    s.mismatches = c_mismatches.Get();
    s.inconclusive = c_inconclusive.Get();
    s.generation = c_restarts.Get();
    s.hitMs = static_cast<double>(g_hitTicks.load()) * g_qpcMs;
    s.walkMs = static_cast<double>(g_walkTicks.load()) * g_qpcMs;
    s.capacity = kSlots;
    s.layerClasses = g_layerClasses.load();
    s.objectClasses = g_objectClasses.load();
    s.selfDisabled = g_selfDisabled.load();
    AcquireSRWLockShared(&g_lock);
    s.entries = g_stampGen == g_gen.load() ? g_count : 0;
    ReleaseSRWLockShared(&g_lock);
    {
        std::lock_guard<std::mutex> lock(g_mismatchLock);
        s.lastMismatch = g_lastMismatch;
    }
    return s;
}

std::string StatusText() {
    if (!Running()) return "Off";
    const Stats s = GetStats();
    if (s.selfDisabled) return "Turned itself off after a check found a different answer (see ApexRadiance_LOG.txt)";
    if (!s.lookups) return "On (no lookups yet)";
    return std::format("On: {:.0f}% of lookups answered from the index", 100.0 * static_cast<double>(s.hits) / static_cast<double>(s.lookups));
}

void RenderDeveloperUI() {
    if (kPublicBuild) return;
    if (!ImGui::GetCurrentContext()) return;
    const Stats s = GetStats();
    static Stats prev;
    static uint64_t prevTick = 0;
    static double rateLookups = 0.0, rateHits = 0.0, rateHitMs = 0.0, rateWalkMs = 0.0, rateSavedMs = 0.0;
    const uint64_t now = GetTickCount64();
    if (now - prevTick >= 1000) {
        const double dt = prevTick ? static_cast<double>(now - prevTick) / 1000.0 : 0.0;
        if (dt > 0.0) {
            const double hits = static_cast<double>(s.hits - prev.hits);
            const double avgWalk = s.walks ? s.walkMs / static_cast<double>(s.walks) : 0.0;
            rateLookups = static_cast<double>(s.lookups - prev.lookups) / dt;
            rateHits = hits / dt;
            rateHitMs = (s.hitMs - prev.hitMs) / dt;
            rateWalkMs = (s.walkMs - prev.walkMs) / dt;
            rateSavedMs = (hits * avgWalk - (s.hitMs - prev.hitMs)) / dt; // estimate: each answer would have cost an average walk
        }
        prev = s;
        prevTick = now;
    }
    int every = g_verifyEvery.load();
    if (ApexUi::DiagnosticIntRow("Check 1 answer in N against the game##OiVerify", &every, 0, 1024)) SetVerifyEvery(every);
    if (ApexUi::BeginControlRow("Validation run", "Adds comparison work while the test is active", ApexUi::ButtonWidth("Check every answer for 10 s##OiVerifyAll", true))) {
        if (ApexUi::IconTextButton("Check every answer for 10 s##OiVerifyAll", ApexUi::IconId::Scan)) VerifyAllFor(10.0);
        ApexUi::EndControlRow();
    }
    const bool checkingAll = GetTickCount64() < g_verifyAllUntil.load();
    ImGui::TextWrapped("Checks: %llu equal, %llu different, %llu inconclusive (the tree changed during the check)%s", static_cast<unsigned long long>(s.verified),
                        static_cast<unsigned long long>(s.mismatches), static_cast<unsigned long long>(s.inconclusive), checkingAll ? "  [checking every answer]" : "");
    if (!s.lastMismatch.empty()) ImGui::TextColored(ImVec4(0.91f, 0.44f, 0.42f, 1.0f), "Last difference: %s", s.lastMismatch.c_str());
    if (ApexUi::BeginAdvanced("LiveCounters", "Live counters")) {
    ImGui::TextWrapped("%s", ("Faster object lookups: " + StatusText()).c_str());
    ImGui::TextWrapped("Lookups %llu, from the index %llu, game walks %llu (not found %llu), too old %llu, path changed %llu, passed through %llu",
                        static_cast<unsigned long long>(s.lookups), static_cast<unsigned long long>(s.hits), static_cast<unsigned long long>(s.walks),
                        static_cast<unsigned long long>(s.notFound), static_cast<unsigned long long>(s.expired), static_cast<unsigned long long>(s.rejected),
                        static_cast<unsigned long long>(s.passed));
    ImGui::TextWrapped("Stored %llu (not stored %llu), entries %u / %u, table restarts %llu; classes recognised: %u container, %u object",
                        static_cast<unsigned long long>(s.stored), static_cast<unsigned long long>(s.notStored), s.entries, s.capacity,
                        static_cast<unsigned long long>(s.generation), s.layerClasses, s.objectClasses);
    ImGui::TextWrapped("Per second: %.0f lookups, %.0f from the index; time in answers %.2f ms, in game walks %.2f ms; saved about %.2f ms", rateLookups, rateHits, rateHitMs,
                        rateWalkMs, rateSavedMs);
    ImGui::TextWrapped("Average: game walk %.1f us, answer from the index %.2f us", s.walks ? 1000.0 * s.walkMs / static_cast<double>(s.walks) : 0.0,
                        s.hits ? 1000.0 * s.hitMs / static_cast<double>(s.hits) : 0.0);
        ApexUi::EndAdvanced();
    }
}


void SaveDeveloperState(toml::table& out) {
    out.insert("verify_every", g_verifyEvery.load());
}
void LoadDeveloperState(const toml::table& t) {
    if (auto n = t["verify_every"].value<int64_t>()) { const int v = static_cast<int>(*n); g_verifyEvery.store(std::clamp(v, 0, 1024)); }
}
} // namespace ObjectIndex
