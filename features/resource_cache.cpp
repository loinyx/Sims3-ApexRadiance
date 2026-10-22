#include "ui/widgets.h"
#include "developer_settings.h"
// Resource lookup cache, "Remember missing files" and "Faster file lists" (see resource_cache.h and
// docs/features/performance.md).
//
// ---- The game side (Steam 1.67.2, TS3W.exe; research\engine_map\full.asm; addresses through framework/game_addresses.h) ----
//   0x004AFFC0 ResourceMgr::FindProvider thiscall(key*, int* priorityOut), ret 8. Takes the manager's mutex (this+0x48),
//              walks the vector [this+0x30, this+0x34) of 8-byte {Database*, int priority} entries (begin / end read once),
//              and for each one unlocks, calls db->vfunc+0x34(key, 0, 1, 6, 1, 0) (OpenRecord with no record and no info
//              asked = "do you hold this key?"), relocks; the first yes wins: *priorityOut = its priority, returns db. None:
//              returns 0 and leaves *priorityOut alone. Reached only through slot +0x40 of the base (0x00FB2DA0) and
//              derived (0x00FFE250) vtables and the wrapper 0x004AFDA0 (slot +0x44, calls +0x40).
//   0x007D8110 the key resolve (26 callers: async loader 0x00729E36, CAS, ResourceSystem 0x00736610): looks a key up and, on
//              a miss, looks up the variant 0x007D7580 builds (group ^ 0x08000000; for 5 types with instance-high 0 also
//              instance ^ 0x08000000). About 36% of all lookups are such misses (research\perf2\round3.md section 3).
//   The list (the only writers of [this+0x30..0x38]; its vector helpers 0x004B1F50 / 0x004B25B0 / 0x004B0B90 have no
//   other callers):
//   0x004B2D00 RegisterDatabase thiscall(bool add, db*, int priority), ret 0xC, slot +0x34 of the base vtable: add =
//              insert before the first entry of lower priority (so the list is sorted by priority, highest first, ties in
//              registration order; duplicates refused through slot +0x38), remove = erase. 0x00736A70 is ResourceSystem's
//              override (slot +0x34 of the derived vtable, same arguments), which calls 0x004B2D00 directly.
//   0x004B2EC0 SetDatabasePriority thiscall(db*, int priority), ret 8, slot +0x3C of both: erase + sorted re-insert.
//   Destructors 0x004B35A0 / 0x007366A0 (-> 0x004B30B0): the list goes with the manager (game exit).
//   0x004B0960 DatabaseChanged thiscall(db*, keyVector*), ret 8, slot +0x4C of both: the engine's own notification that
//              keys of a package changed (it runs FindProvider for each key and tells the resource cache's listeners).
//              Sent by the ResourceSystem's file watcher when a package file changed on disk (0x00734D10: the package's
//              outer object +0xC "file changed?" then GetKeyList, then mgr+0x4C at 0x00734DBF) and by the loose-file
//              folder databases' directory watcher after a rescan (0x004A4160, AFTER the folder's key set changed).
//   Package classes (every IDatabase implementation: the constructors that store the interface base 0x00FB21F8 are
//   0x004A550C DDF, 0x004A8F7C DPF, 0x0072B8FB MemoryDB, 0x0072CC6B packed stream, 0x0098B160 ContentManager):
//   0x00FFE078 the read-only package class that closes idle files (ctor 0x007342F0; the list holds its IDatabase subobject,
//              outer+8; registered by the ResourceSystem's path registration 0x00737950 for Resource.cfg packages, i.e. the
//              game's and the mods' .package files). OpenRecord 0x007345D0: with no record / info asked and a key set
//              present ([this+0xB0], built from the index by the idle close 0x00735320 before it closes the file) it only
//              probes that set; otherwise it opens the file (slot +0x50, which drops the key set on success), and asks
//              the index through the base OpenRecord 0x0072D470, which accepts only the open-existing dispositions 6 and 3
//              (it can never create a record) and needs the open mode [this+0x14]. DeleteRecord (slot +0x40, 0x007346A0)
//              calls 0x00624F70 = "return false". So its key set is the file's index: it changes only when the file on
//              disk changes, which the watcher reports through DatabaseChanged (0x00734D10 above). One exception: when the
//              key set is absent and the file cannot be opened at that moment, the probe answers "no" for every key (open
//              failure, nothing changes in memory). After a lookup such a package is closed with no key set ([+0xB0] == 0
//              and [+0x14] == 0): answers that relied on its "no" are not stored (RoReliable).
//   Other classes: 0x00FB2600 DPF (writable package; derived 0x01048DA0), 0x00FB2420 DDF (loose-file folders: Mods/Files,
//              NonPackaged/Ini, UI/Layouts), 0x00FFD790 base packed stream (e.g. the CAS compositor cache at -1000),
//              0x00FFD5F8 MemoryDB, 0x01046EE8 ContentManager (downloaded content: it forwards to sub-databases). All are
//              "can gain keys at any time": probed on every answer, unless their writes are counted (write epochs below).
//
// ---- The lookup cache ----
//   Key = (manager, 16-byte resource key). Value = {package (0 = no package holds it), priority, its index in the list,
//   insert tick, write-epoch sum}. Validity:
//     - g_gen: bumped before and after every RegisterDatabase / SetDatabasePriority / DatabaseChanged (hooked through
//       their vtable slots); g_mutating > 0 while one runs: lookups then go straight to the game and nothing is stored.
//       Entries carry a table stamp tied to the generation they were stored under (a new generation = an empty table,
//       without clearing memory).
//     - a snapshot per (manager, generation): list begin / size / fingerprint and the packages that are not of the
//       read-only class (index, pointer, whether their writes are counted). A list that changed without the hooks noticing
//       (begin / size / fingerprint differ under the same generation) bumps the generation and is logged.
//     - on a lookup: the entry at the stored index still holds {package, priority}; the package still holds the key (one
//       OpenRecord probe, exactly as the game asks); none of the non-read-only packages above it holds the key (one probe
//       each; answers with more than 32 such packages above them are not stored); the generation did not move meanwhile;
//       the entry is younger than 60 s. Any failure: the game's own lookup runs (and its answer is stored again).
//     - storing: only when no list change happened around the game's lookup and every read-only package above the answer
//       (all of them for "absent") could answer for sure (key set present or file open, checked after the lookup).
//   "Remember missing files" adds:
//     - absent entries (package 0, "above" = every non-read-only package): re-checked by probing those packages; the game
//       leaves *priorityOut alone on a miss, so does the cache.
//     - write epochs. Hooks on every traced write path of the DPF (both vtables), DDF and base packed stream classes
//       (EpochSpec below; research by reading every writer of their key structures) call WriteBegin / WriteEnd around the
//       call: a per-database counter (1024 buckets by pointer; a shared bucket only costs extra re-checks), a global
//       sequence and a global "writes in progress" count. An entry stores the sum of the counters of its counted
//       databases (the non-read-only ones above it and its own package when not read-only), taken while no write was in
//       progress and none happened since before the game's lookup (seqlock). A later answer whose sum is unchanged
//       (sequence stable, no write in progress) needs no probe of those databases, nor of a read-only answering package
//       (same premise as above); uncounted classes are still probed. A changed sum: every package is probed as before and,
//       when that passes with no write meanwhile, the sum is refreshed.
//   Nothing of Apex is held (no lock) while game code runs. 32-bit counters (relaxed atomics).
//
// ---- Write paths counted (the probe is OpenRecord(key, 0, 1, 6, 1, 0); V = read in full.asm) ----
//   DPF 0x00FB2600 / 0x01048DA0 (the derived class overrides only +0x7C / +0x84): the probe reads the hash index
//     [this+0x2D0] (vfunc+0x28 Find) under the mutex this+0x270 after an auto-open (0x004A6860 -> slot +0x18) when
//     closed. Writers: +0x00 dtor, +0x08 Shutdown 0x004A6AE0, +0x18 Open 0x004A76E0 (loads the index), +0x1C Close
//     0x004A8D20 (clears it), +0x24 Flush 0x004A9B70 (compaction: Close + rename + Open), +0x34 OpenRecord 0x004A94C0 when
//     a record is asked with write access or a creating disposition (insert / replace), +0x3C CloseRecord 0x004A8E00 for
//     writable records ([rec+8] == 0x12E4A892: commit 0x004A8910 removes and re-inserts, a failed commit leaves it
//     removed), +0x40 DeleteRecord 0x004A85A0, +0x5C SetIndex 0x004A6690, +0x8C LoadIndex 0x004A9950, +0x9C ConvertIndex
//     0x004A6C80, and the non-virtual direct write 0x004A7FC0 (entry hook; callers: the KeyList copy helpers 0x007D7A50 /
//     0x007D7B30 / 0x007D7C10 and the compaction's temporary copy 0x004AE510). 0x004A8120 (CAS compositor cache) removes
//     and re-inserts the same key under the mutex: no visible change.
//   DDF 0x00FB2420: the probe reads the map [this+0x50] under the mutex this+0x80 when [this+0x0C] is set. Writers: +0x00
//     dtor, +0x08 0x004A3950 (-> Close), +0x18 Open 0x004A3AD0 (full rescan), +0x1C Close 0x004A5240, +0x2C SetLocation
//     0x004A4370, +0x34 OpenRecord 0x004A62B0 whenever a record is asked (it inserts a key whose file appeared on disk,
//     even for reading), +0x40 DeleteRecord 0x004A52A0, +0x58 one-file refresh 0x004A6100 and +0x5C full rescan 0x004A5340
//     (both also from the directory watcher thread, through the vtable).
//   Packed stream 0x00FFD790 (not the read-only class, whose vtable is 0x00FFE078): the probe needs the open mode
//     [this+0x14] and the index [this+0x70]. Writers: +0x00 dtor, +0x08 0x0072C6A0 (-> Close), +0x18 Open 0x0072D790,
//     +0x1C Close 0x0072C8F0, +0x2C SetLocation 0x0072CD50. It cannot create or delete records.
//   Not counted (always probed): MemoryDB 0x00FFD5F8 (a non-virtual PutResource 0x0072C350 inserts keys), ContentManager
//   0x01046EE8 (answers depend on sub-databases changed by non-virtual install code), and any other class.
//   Every function's first bytes are checked against the studied ones before its class is counted; a class with any
//   difference, or whose slots do not hold what Apex expects, stays probed (all or nothing per class).
//
// ---- The file list cache ----
//   0x004B1AE0 ResourceMgr::GetKeyList thiscall(vector* out, filter*, bool unique), ret 0xC (slot +0x20 of the base
//              vtable). unique = false: for each {db, priority} of [this+0x30, this+0x34) (read once, no lock) count +=
//              db->vfunc+0x30(out, filter); returns count. The databases append matching keys to out (inline append when
//              end < capacity, else the vector insert 0x006D3810, or resize 0x0045A5A0 then fill) and return how many.
//   0x00736660 ResourceSystem's override (slot +0x20 of the derived vtable): calls 0x004B1AE0 directly, then, when the
//              count and out are not 0, returns 0x004AFCD0(out) (cdecl: sort + unique of the whole vector, returns its size).
//   The type filter {vtable 0x00FD8248, type}: its only virtual used by the databases is +4 = 0x005949F0, "key[2] (type)
//   == this+4". CAS LoadBlendGeometryCallback 0x005DA0C0 asks it for type 0x0A037DDA with unique = false (then sorts and
//   uniques itself); every read-only package walks its whole index (0x004AC9C0) or key set (0x0072DE50) for it.
//   The cache answers only unique = false with that filter class: it runs the same loop itself; for a read-only package
//   whose keys of that type were captured under the current generation it appends them (same package order, same keys;
//   the order inside one package can differ, as it does in the game between the key-set walk and the index walk), every
//   other package is asked for real. A read-only package's list is captured from out after the real call, and stored
//   only when it could answer for sure (key set present or file open before and after).
//
// Part of Apex Radiance. Credits: @loinyx

#include "resource_cache.h"
#include "apex_log.h"
#include "apex_util.h"
#include "hook_guard.h"
#include "build_flavor.h"
#include "entry_chain.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include "slot_chain.h"
#include "imgui.h"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ResourceCache {
namespace {

using SlotChain::Layer;
using SlotChain::Site;

// ---- table ----
constexpr uint32_t kSlots = 1u << 16; // 64k entries x 44 bytes = 2.8 MB, allocated once, never freed
constexpr uint32_t kMask = kSlots - 1;
constexpr uint32_t kMaxFill = kSlots / 4 * 3; // then the table starts empty again
constexpr int kMaxProbe = 64;                 // linear probing distance
constexpr int kMaxWritableAbove = 32;         // more non-read-only packages above an answer: not stored
constexpr int kMaxRecorded = 64;              // non-read-only packages recorded per snapshot (by index)
constexpr uint32_t kMaxAgeMs = 60000;         // older answers are looked up again
constexpr int kSnapshots = 4;                 // (manager, generation) snapshots kept
constexpr uint32_t kFingerprintEvery = 1024;  // cache answers between two full-list fingerprint checks
constexpr uint32_t kNoSum = 0xFFFFFFFFu;      // entry: no valid write-epoch sum (re-checked by probes, then refreshed)
constexpr uint32_t kAllAbove = 0xFFFFFFFFu;   // "index" of an absent entry: every non-read-only package is above it

struct Entry {
    uint32_t key[4];
    uint32_t mgr;
    uint32_t provider; // 0 = no package holds the key
    int32_t priority;
    uint32_t index;    // of the provider in the list when stored (kAllAbove for absent)
    uint32_t stamp;    // == g_stamp: live; anything else: empty
    uint32_t tick;     // GetTickCount when stored
    uint32_t esum;     // write-epoch sum of its counted databases, or kNoSum
};
static_assert(sizeof(Entry) == 44, "Entry layout");

struct Snapshot {
    uint32_t gen = 0; // 0 = unused
    uintptr_t mgr = 0;
    uintptr_t begin = 0;
    uint32_t count = 0;
    uint32_t fingerprint = 0;
    int writable = 0; // non-read-only packages in the list
    int recorded = 0; // the first kMaxRecorded of them
    int traced = 0;   // recorded ones whose writes are counted
    uint16_t idx[kMaxRecorded] = {};
    uint32_t prov[kMaxRecorded] = {};
    uint8_t counted[kMaxRecorded] = {}; // its class's write epochs are hooked
};

Entry* g_table = nullptr; // written under g_lock (exclusive), read under g_lock (shared)
SRWLOCK g_lock = SRWLOCK_INIT;
uint32_t g_stamp = 1;    // guarded by g_lock
uint32_t g_stampGen = 0; // the generation the current stamp belongs to
uint32_t g_count = 0;    // live entries
Snapshot g_snap[kSnapshots]; // guarded by g_lock
int g_snapNext = 0;

// ---- validity ----
std::atomic<uint32_t> g_gen{2};
std::atomic<int32_t> g_mutating{0};
std::atomic<bool> g_on{false};
std::atomic<bool> g_selfDisabled{false};
std::atomic<bool> g_negOn{false};    // absent entries (with "Remember missing files" and the cache both on)
std::atomic<bool> g_epochsOn{false}; // write epochs usable (hooks of at least one class installed)
std::atomic<uint64_t> g_epochTrustTick{0}; // GetTickCount64 from which epoch sums are taken and used (see EpochsUsable)
uintptr_t g_readOnlyVtable = 0; // set by Start before the hooks
bool g_readOnlyOk = false;

// ---- control ----
std::mutex g_ctrl;
bool g_started = false;       // lookup cache
std::atomic<bool> g_missesWanted{false}; // "Remember missing files" switched on (written under g_ctrl)
bool g_listStarted = false;   // file list cache
int g_watcherUsers = 0;       // the package-list watchers are installed while > 0

// ---- statistics (relaxed; 32-bit so the hot path stays one locked add) ----
struct Counter {
    std::atomic<uint32_t> v{0};
    void Add(uint32_t n = 1) { v.fetch_add(n, std::memory_order_relaxed); }
    uint32_t Get() const { return v.load(std::memory_order_relaxed); }
};
Counter c_lookups, c_hits, c_misses, c_bypassed, c_rejected, c_notFound, c_inserted, c_notCached, c_probes, c_listChanges, c_notices, c_unhooked, c_fullClears,
    c_verified, c_mismatches, c_inconclusive, c_fingerprintChecks, c_negHits, c_negInserted, c_negUnreliable, c_epochHits, c_epochRefreshes, c_epochWrites;
std::atomic<uint64_t> g_hitTicks{0}, g_missTicks{0}; // development build only
double g_qpcMs = 0.0;
std::atomic<int> g_verifyEvery{kPublicBuild ? 0 : 64};
std::atomic<uint64_t> g_verifyAllUntil{0}; // GetTickCount64
std::mutex g_mismatchLock;
std::string g_lastMismatch;   // guarded by g_mismatchLock
std::string g_klLastMismatch; // guarded by g_mismatchLock
std::atomic<bool> g_unhookedLogged{false};

thread_local LookupNote t_note;
thread_local KeyListNote t_klNote;
thread_local uint32_t t_verifyCount = 0;
thread_local uint32_t t_klVerifyCount = 0;

uint64_t Qpc() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<uint64_t>(q.QuadPart);
}

// ---- game calls ----
using FindFn = uint32_t(__fastcall*)(void* mgr, void* edx, const uint32_t* key, int32_t* priorityOut);
using OpenRecordFn = uint8_t(__fastcall*)(void* db, void* edx, const uint32_t* key, uint32_t record, uint32_t access, uint32_t disposition, uint32_t flag, uint32_t info);
using DbKeyListFn = int32_t(__fastcall*)(void* db, void* edx, void* out, void* filter);
using MgrKeyListFn = int32_t(__fastcall*)(void* mgr, void* edx, void* out, void* filter, uint32_t unique);
using VecInsertFn = uint32_t(__fastcall*)(void* vec, void* edx, uint32_t pos, const uint32_t* value);
using SortUniqueFn = int32_t(__cdecl*)(void* vec);
using This3Fn = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t);
using This2Fn = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t);

// db->OpenRecord(key, 0, 1, 6, 1, 0), exactly as FindProvider asks (0x004B0002..0x004B000F). Not guarded: a fault here
// is the game's own fault too.
inline bool Holds(uint32_t db, const uint32_t* key) {
    void* const* vt = *reinterpret_cast<void* const* const*>(static_cast<uintptr_t>(db));
    return reinterpret_cast<OpenRecordFn>(vt[0x34 / 4])(reinterpret_cast<void*>(static_cast<uintptr_t>(db)), nullptr, key, 0, 1, 6, 1, 0) != 0;
}

// ---- package list (read without the manager's lock: SEH-guarded, validated by the generation) ----
bool ReadList(uintptr_t mgr, uintptr_t& begin, uint32_t& count) {
    __try {
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(mgr + 0x30);
        const uintptr_t e = *reinterpret_cast<const uintptr_t*>(mgr + 0x34);
        if (!b || e < b || ((e - b) & 7) || e - b > 8u * 8192u) return false;
        begin = b;
        count = static_cast<uint32_t>((e - b) / 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t Fnv(uint32_t h, uint32_t v) {
    for (int i = 0; i < 4; i++) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 16777619u;
    return h;
}

// ---- write epochs ----
constexpr uint32_t kEpochBuckets = 1024;
std::atomic<uint32_t> g_epoch[kEpochBuckets];
std::atomic<uint32_t> g_writeSeq{0};
std::atomic<int32_t> g_writeBusy{0};
enum EpochClass : int { kClsDpf, kClsDpfDerived, kClsDdf, kClsPackedStream, kClsCount };
uintptr_t g_clsVtable[kClsCount] = {};           // guarded by g_ctrl (read by BuildSnapshot under the generation rules)
std::atomic<bool> g_clsCounted[kClsCount] = {}; // its hooks are installed

inline uint32_t Bucket(uintptr_t db) { return (static_cast<uint32_t>(db) * 0x9E3779B1u) >> 22; }

// A write that was already running inside a game function when its slot was swapped is not bracketed; sums are taken and
// trusted only kEpochGraceMs after the hooks went in (every such call has long returned by then)
constexpr uint64_t kEpochGraceMs = 10000;
inline bool EpochsUsable() {
    return g_epochsOn.load(std::memory_order_acquire) && GetTickCount64() >= g_epochTrustTick.load(std::memory_order_relaxed);
}

void WriteBegin(const void* db) {
    g_writeBusy.fetch_add(1, std::memory_order_seq_cst);
    g_writeSeq.fetch_add(1, std::memory_order_seq_cst);
    g_epoch[Bucket(reinterpret_cast<uintptr_t>(db))].fetch_add(1, std::memory_order_seq_cst);
    c_epochWrites.Add();
}
void WriteEnd(const void* db) {
    g_epoch[Bucket(reinterpret_cast<uintptr_t>(db))].fetch_add(1, std::memory_order_seq_cst);
    g_writeSeq.fetch_add(1, std::memory_order_seq_cst);
    g_writeBusy.fetch_sub(1, std::memory_order_seq_cst);
}

bool IsCountedVtable(uint32_t vt) {
    if (!vt) return false;
    for (int c = 0; c < kClsCount; c++)
        if (g_clsVtable[c] == vt && g_clsCounted[c].load(std::memory_order_acquire)) return true;
    return false;
}

// Snapshot of the list: begin, size, fingerprint, the non-read-only packages. False when unreadable.
bool BuildSnapshot(uintptr_t mgr, Snapshot& s) {
    uintptr_t begin = 0;
    uint32_t count = 0;
    if (!ReadList(mgr, begin, count)) return false;
    s.mgr = mgr;
    s.begin = begin;
    s.count = count;
    s.writable = 0;
    s.recorded = 0;
    s.traced = 0;
    const bool epochs = g_epochsOn.load(std::memory_order_acquire);
    uint32_t fp = 2166136261u;
    __try {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(begin);
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t db = p[2 * i], prio = p[2 * i + 1];
            fp = Fnv(Fnv(fp, db), prio);
            const uint32_t vt = db ? *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(db)) : 0;
            if (g_readOnlyOk && vt == g_readOnlyVtable) continue;
            if (s.recorded < kMaxRecorded && i < 0xFFFF) {
                s.idx[s.recorded] = static_cast<uint16_t>(i);
                s.prov[s.recorded] = db;
                s.counted[s.recorded] = epochs && IsCountedVtable(vt) ? 1 : 0;
                s.traced += s.counted[s.recorded];
                s.recorded++;
            }
            s.writable++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    s.fingerprint = fp;
    return true;
}

bool Fingerprint(uintptr_t mgr, uintptr_t& begin, uint32_t& count, uint32_t& fp) {
    if (!ReadList(mgr, begin, count)) return false;
    fp = 2166136261u;
    __try {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(begin);
        for (uint32_t i = 0; i < 2 * count; i++) fp = Fnv(fp, p[i]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// The entry `index` of the list still is {provider, priority}, and the list is where the snapshot saw it
bool ListEntryIs(uintptr_t mgr, uintptr_t begin, uint32_t count, uint32_t index, uint32_t provider, int32_t priority) {
    __try {
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(mgr + 0x30);
        const uintptr_t e = *reinterpret_cast<const uintptr_t*>(mgr + 0x34);
        if (b != begin || e < b || (e - b) / 8 != count || index >= count) return false;
        const uint32_t* p = reinterpret_cast<const uint32_t*>(b);
        return p[2 * index] == provider && static_cast<int32_t>(p[2 * index + 1]) == priority;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The list is still where the snapshot saw it (absent entries)
bool ListIs(uintptr_t mgr, uintptr_t begin, uint32_t count) {
    __try {
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(mgr + 0x30);
        const uintptr_t e = *reinterpret_cast<const uintptr_t*>(mgr + 0x34);
        return b == begin && e >= b && (e - b) / 8 == count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Where {provider, priority} sits in the list now
bool FindInList(uintptr_t mgr, uint32_t provider, int32_t priority, uint32_t& index) {
    uintptr_t begin = 0;
    uint32_t count = 0;
    if (!ReadList(mgr, begin, count)) return false;
    __try {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(begin);
        for (uint32_t i = 0; i < count; i++)
            if (p[2 * i] == provider) {
                index = i;
                return static_cast<int32_t>(p[2 * i + 1]) == priority;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// A read-only package answers for sure: its key set is present ([db+0xB0]) or its file is open ([db+0x14], the open
// mode its base OpenRecord needs). Closed with no key set = its last probe may have been an open failure.
bool RoReliable(uint32_t db) {
    __try {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(db));
        return *reinterpret_cast<const uint32_t*>(p + 0xB0) != 0 || *reinterpret_cast<const uint32_t*>(p + 0x14) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The last whole-list "every read-only package answers for sure" of this thread (05/10: every lookup that reaches the
// game ran the walk over the ~300 packages before its probe, and Remember ran it again; sampled hot on 10/02). Reliability
// only goes from unreliable to reliable while the list and its generation stay (see Remember), so a "yes" for the same
// list, generation and size holds; it is still walked again after kRoLatchMs.
struct RoLatch {
    uintptr_t mgr = 0, begin = 0;
    uint32_t count = 0, gen = 0;
    DWORD at = 0;
};
thread_local RoLatch t_roLatch;
constexpr DWORD kRoLatchMs = 100;

// Every read-only package in the list before `index` (all of them for kAllAbove) answers for sure
bool ReadOnlyAboveReliable(uintptr_t mgr, uint32_t index) {
    uintptr_t begin = 0;
    uint32_t count = 0;
    if (!ReadList(mgr, begin, count)) return false;
    const uint32_t gen = g_gen.load(std::memory_order_acquire);
    const bool quiet = g_mutating.load(std::memory_order_acquire) == 0;
    RoLatch& l = t_roLatch;
    if (quiet && l.at && l.mgr == mgr && l.begin == begin && l.count == count && l.gen == gen && GetTickCount() - l.at < kRoLatchMs) return true; // the whole list held: any part of it holds
    const uint32_t n = index == kAllAbove || index > count ? count : index;
    __try {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(begin);
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t db = p[2 * i];
            if (!db || *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(db)) != g_readOnlyVtable) continue;
            if (!RoReliable(db)) return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (n == count && quiet && g_mutating.load(std::memory_order_acquire) == 0 && g_gen.load(std::memory_order_acquire) == gen)
        l = RoLatch{mgr, begin, count, gen, GetTickCount() | 1};
    return true;
}

// ---- generation ----
void BeginChange() {
    g_mutating.fetch_add(1, std::memory_order_seq_cst);
    g_gen.fetch_add(1, std::memory_order_seq_cst);
}
void EndChange() {
    g_gen.fetch_add(1, std::memory_order_seq_cst);
    g_mutating.fetch_sub(1, std::memory_order_seq_cst);
}

// The list moved while the generation did not: something changed it outside the hooked methods
void NoteUnhookedChange(uintptr_t mgr, const char* how) {
    BeginChange();
    EndChange();
    c_unhooked.Add();
    if (!g_unhookedLogged.exchange(true)) // inside the game's lookup: building the line may not throw into it (07/10)
        HookGuard::Try("ResourceCache unhooked change note", [mgr, how] {
            LOG_WARNING(std::format("[ResourceCache] The package list of manager {:#010x} changed without RegisterDatabase / SetDatabasePriority ({}); every "
                                    "remembered answer was dropped. Further ones are only counted (Developer page).",
                                    mgr, how));
        });
}

// ---- table access (callers hold g_lock) ----
inline uint32_t Hash(uint32_t mgr, const uint32_t* k) {
    uint32_t h = mgr * 0x9E3779B1u;
    h = _rotl(h ^ (k[0] * 0x85EBCA77u), 13);
    h = _rotl(h ^ (k[1] * 0xC2B2AE3Du), 13);
    h = _rotl(h ^ (k[2] * 0x27D4EB2Fu), 13);
    h ^= k[3] * 0x165667B1u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

inline bool SameKey(const Entry& e, uint32_t mgr, const uint32_t* k) {
    return e.mgr == mgr && e.key[0] == k[0] && e.key[1] == k[1] && e.key[2] == k[2] && e.key[3] == k[3];
}

const Snapshot* SnapFor(uintptr_t mgr, uint32_t gen) {
    for (const Snapshot& s : g_snap)
        if (s.gen == gen && s.mgr == mgr) return &s;
    return nullptr;
}

// What a lookup needs, copied out of the table (no lock held afterwards)
struct Answer {
    uint32_t provider = 0; // 0 = absent
    int32_t priority = 0;
    uint32_t index = 0;
    uint32_t tick = 0;
    uint32_t esum = kNoSum;
    uintptr_t begin = 0;
    uint32_t count = 0;
    int n = 0;                          // non-read-only packages above the answer
    uint32_t prov[kMaxWritableAbove] = {};
    uint8_t counted[kMaxWritableAbove] = {};
    bool providerRo = true;      // the answering package is of the read-only class
    bool providerCounted = false; // ... or of a class whose writes are counted
};

// The non-read-only packages above `index` in the snapshot, into a (false when they cannot all be listed); also the
// answering package's class
bool WritableAbove(const Snapshot& s, uint32_t index, Answer& a) {
    a.n = 0;
    a.providerRo = true;
    a.providerCounted = false;
    int i = 0;
    for (; i < s.recorded && s.idx[i] < index; i++) {
        if (a.n >= kMaxWritableAbove) return false;
        a.counted[a.n] = s.counted[i];
        a.prov[a.n++] = s.prov[i];
    }
    if (index != kAllAbove && i < s.recorded && s.idx[i] == index) {
        a.providerRo = false;
        a.providerCounted = s.counted[i] != 0;
    } else if (index != kAllAbove && i == s.recorded && s.recorded != s.writable) {
        a.providerRo = false; // beyond the recorded ones: its class is unknown here
    }
    // packages past the recorded ones may lie above the answer only when every one was recorded
    return i < s.recorded || s.recorded == s.writable;
}

bool Find(uintptr_t mgr, const uint32_t* key, uint32_t gen, Answer& a) {
    bool found = false;
    ApexUtil::SrwShared srw(g_lock);
    if (g_table && g_stampGen == gen) {
        uint32_t slot = Hash(static_cast<uint32_t>(mgr), key) & kMask;
        for (int d = 0; d < kMaxProbe; d++, slot = (slot + 1) & kMask) {
            const Entry& e = g_table[slot];
            if (e.stamp != g_stamp) break; // empty
            if (!SameKey(e, static_cast<uint32_t>(mgr), key)) continue;
            const Snapshot* s = SnapFor(mgr, gen);
            if (s && (e.provider || g_negOn.load(std::memory_order_relaxed)) && WritableAbove(*s, e.index, a)) {
                a.provider = e.provider;
                a.priority = e.priority;
                a.index = e.index;
                a.tick = e.tick;
                a.esum = e.esum;
                a.begin = s->begin;
                a.count = s->count;
                found = true;
            }
            break;
        }
    }
    return found;
}

// Sum of the write epochs of the answer's counted databases (the ones above it, and the answering one)
uint32_t EpochSum(const Answer& a) {
    uint32_t sum = 0;
    for (int i = 0; i < a.n; i++)
        if (a.counted[i]) sum += g_epoch[Bucket(a.prov[i])].load(std::memory_order_acquire);
    if (a.provider && a.providerCounted) sum += g_epoch[Bucket(a.provider)].load(std::memory_order_acquire);
    return sum;
}

// The sum when no counted write is in progress and none happens while it is read (seqlock), else false
bool StableEpochSum(const Answer& a, uint32_t& sum, uint32_t& seq) {
    seq = g_writeSeq.load(std::memory_order_seq_cst);
    if (g_writeBusy.load(std::memory_order_seq_cst) != 0) return false;
    sum = EpochSum(a);
    return g_writeSeq.load(std::memory_order_seq_cst) == seq;
}

// A new write-epoch sum for a live entry (after the probes passed with no write meanwhile)
void RefreshSum(uintptr_t mgr, const uint32_t* key, uint32_t gen, const Answer& a, uint32_t sum) {
    ApexUtil::SrwExclusive srw(g_lock);
    if (g_table && g_stampGen == gen) {
        uint32_t slot = Hash(static_cast<uint32_t>(mgr), key) & kMask;
        for (int d = 0; d < kMaxProbe; d++, slot = (slot + 1) & kMask) {
            Entry& e = g_table[slot];
            if (e.stamp != g_stamp) break;
            if (!SameKey(e, static_cast<uint32_t>(mgr), key)) continue;
            if (e.provider == a.provider && e.index == a.index && e.tick == a.tick) {
                e.esum = sum;
                c_epochRefreshes.Add();
            }
            break;
        }
    }
}

// The stored answer still is what the game's lookup would return. probes = packages asked.
bool Recheck(uintptr_t mgr, const uint32_t* key, uint32_t gen, const Answer& a, uint32_t& probes, bool& byEpoch) {
    probes = 0;
    byEpoch = false;
    if (GetTickCount() - a.tick > kMaxAgeMs) return false;
    if (a.provider ? !ListEntryIs(mgr, a.begin, a.count, a.index, a.provider, a.priority) : !ListIs(mgr, a.begin, a.count)) return false;
    const bool epochs = EpochsUsable();
    // Write epochs: the counted databases did not change since the answer was checked
    if (epochs && a.esum != kNoSum) {
        uint32_t sum = 0, seq = 0;
        if (StableEpochSum(a, sum, seq) && sum == a.esum) {
            for (int i = 0; i < a.n; i++)
                if (!a.counted[i]) {
                    probes++;
                    if (Holds(a.prov[i], key)) return false;
                }
            // A read-only answering package is skipped only while it is reliable (key set loaded or file open): a closed one
            // whose file cannot open right now would answer "no" in the game
            if (a.provider && ((!a.providerRo && !a.providerCounted) || (a.providerRo && !RoReliable(a.provider)))) {
                probes++;
                if (!Holds(a.provider, key)) return false;
            }
            byEpoch = true;
            return g_mutating.load(std::memory_order_acquire) == 0 && g_gen.load(std::memory_order_acquire) == gen;
        }
    }
    // Every package, as before; with epochs, the sum is refreshed when no counted write happened meanwhile
    uint32_t sum = 0, seq = 0;
    const bool stable = epochs && StableEpochSum(a, sum, seq);
    if (a.provider) {
        probes++;
        if (!Holds(a.provider, key)) return false;
    }
    for (int i = 0; i < a.n; i++) {
        probes++;
        if (Holds(a.prov[i], key)) return false;
    }
    const bool ok = g_mutating.load(std::memory_order_acquire) == 0 && g_gen.load(std::memory_order_acquire) == gen;
    if (ok && stable && g_writeBusy.load(std::memory_order_seq_cst) == 0 && g_writeSeq.load(std::memory_order_seq_cst) == seq) RefreshSum(mgr, key, gen, a, sum);
    return ok;
}

// Stores the game's answer (provider 0 = absent), found under generation gen with no list change around it. seq0 = the
// write sequence before the game's lookup (valid only when no counted write was in progress then). roBefore: every
// read-only package of the list answered for sure already before the game's lookup (checked only for absent answers).
void Remember(uintptr_t mgr, const uint32_t* key, uint32_t provider, int32_t priority, uint32_t gen, bool seqValid, uint32_t seq0, bool roBefore) {
    uint32_t index = kAllAbove;
    if (provider && !FindInList(mgr, provider, priority, index)) {
        c_notCached.Add();
        return;
    }
    // Every read-only package above the answer answered for sure (not a failed open). An absent answer needs it both
    // before and after the game's lookup: a package whose open failed transiently during the probe (then reopened, or
    // given its key set, before this check) would otherwise be stored as "absent" although it may hold the key.
    // Reliability only goes from unreliable to reliable, so the check before the lookup is the one that matters.
    if ((!provider && !roBefore) || !ReadOnlyAboveReliable(mgr, index)) {
        (provider ? c_notCached : c_negUnreliable).Add();
        return;
    }
    // The snapshot of this list under gen: build it when missing (outside the lock)
    Snapshot fresh;
    bool haveSnap = false, moved = false;
    {
        ApexUtil::SrwShared srw(g_lock);
        if (const Snapshot* s = SnapFor(mgr, gen)) {
            haveSnap = true;
            uintptr_t begin = 0;
            uint32_t count = 0;
            moved = !ReadList(mgr, begin, count) || begin != s->begin || count != s->count;
        }
    }
    if (moved) {
        if (g_gen.load() == gen && g_mutating.load() == 0) NoteUnhookedChange(mgr, "list moved or resized");
        c_notCached.Add();
        return;
    }
    if (!haveSnap && !BuildSnapshot(mgr, fresh)) {
        c_notCached.Add();
        return;
    }
    const uint32_t tick = GetTickCount();
    ApexUtil::SrwExclusive srw(g_lock);
    bool stored = false;
    if (g_gen.load() == gen && g_mutating.load() == 0) {
        if (!haveSnap && !SnapFor(mgr, gen)) { // (another thread may have stored it meanwhile)
            fresh.gen = gen;
            // a free slot (another generation) first, else round robin
            int slot = -1;
            for (int k = 0; k < kSnapshots && slot < 0; k++)
                if (g_snap[k].gen != gen) slot = k;
            if (slot < 0) slot = g_snapNext++ % kSnapshots;
            g_snap[slot] = fresh;
        }
        Answer probe;
        const Snapshot* s = SnapFor(mgr, gen);
        if (s && g_table && WritableAbove(*s, index, probe)) {
            probe.provider = provider;
            // the write-epoch sum now, valid when no counted write happened since before the game's lookup
            uint32_t esum = kNoSum;
            if (seqValid && EpochsUsable()) {
                uint32_t sum = 0, seq = 0;
                if (StableEpochSum(probe, sum, seq) && seq == seq0) esum = sum;
            }
            if (g_stampGen != gen) {
                if (static_cast<int32_t>(gen - g_stampGen) > 0) { // a newer generation: the table starts empty
                    g_stamp++;
                    g_stampGen = gen;
                    g_count = 0;
                }
            }
            if (g_stampGen == gen) {
                if (g_count >= kMaxFill) {
                    g_stamp++;
                    g_count = 0;
                    c_fullClears.Add();
                }
                uint32_t slot = Hash(static_cast<uint32_t>(mgr), key) & kMask;
                for (int d = 0; d < kMaxProbe; d++, slot = (slot + 1) & kMask) {
                    Entry& e = g_table[slot];
                    const bool empty = e.stamp != g_stamp;
                    if (!empty && !SameKey(e, static_cast<uint32_t>(mgr), key)) continue;
                    if (empty) g_count++;
                    std::memcpy(e.key, key, sizeof e.key);
                    e.mgr = static_cast<uint32_t>(mgr);
                    e.provider = provider;
                    e.priority = priority;
                    e.index = index;
                    e.tick = tick;
                    e.esum = esum;
                    e.stamp = g_stamp;
                    stored = true;
                    break;
                }
            }
        }
    }
    if (provider) (stored ? c_inserted : c_notCached).Add();
    else if (stored) c_negInserted.Add();
    else c_notCached.Add();
}

// Every kFingerprintEvery answers: the whole list still matches the snapshot of this generation
void MaybeFingerprint(uintptr_t mgr, uint32_t gen) {
    static std::atomic<uint32_t> s_hits{0};
    if ((s_hits.fetch_add(1, std::memory_order_relaxed) % kFingerprintEvery) != 0) return;
    c_fingerprintChecks.Add();
    uintptr_t begin = 0;
    uint32_t count = 0, fp = 0;
    if (!Fingerprint(mgr, begin, count, fp)) return;
    bool differs = false;
    {
        ApexUtil::SrwShared srw(g_lock);
        if (const Snapshot* s = SnapFor(mgr, gen)) differs = s->begin != begin || s->count != count || s->fingerprint != fp;
    }
    if (differs && g_gen.load() == gen && g_mutating.load() == 0) NoteUnhookedChange(mgr, "list content differs");
}

std::string KeyText(const uint32_t* k) { return std::format("{:08X}:{:08X}:{:08X}:{:08X}", k[0], k[1], k[2], k[3]); }

uint32_t VtableOf(uint32_t db) {
    uint32_t vt = 0;
    MemPatch::ReadBytes(db, &vt, 4);
    return vt;
}

// Development build: the game's own answer for the same question, compared with the cache's
bool ShouldVerify(uint32_t& counter) {
    if (kPublicBuild) return false;
    if (GetTickCount64() < g_verifyAllUntil.load(std::memory_order_relaxed)) return true;
    const int every = g_verifyEvery.load(std::memory_order_relaxed);
    return every > 0 && (++counter % static_cast<uint32_t>(every)) == 0;
}

uint32_t Verify(FindFn next, void* self, void* edx, const uint32_t* key, int32_t* priorityOut, const Answer& a, uint32_t gen) {
    uint32_t keyBefore[4];
    std::memcpy(keyBefore, key, sizeof keyBefore);
    int32_t priority = INT32_MIN;
    const int32_t m0 = g_mutating.load();
    const uint32_t r = next(self, edx, key, &priority);
    const bool keyChanged = std::memcmp(keyBefore, key, sizeof keyBefore) != 0;
    const bool settled = m0 == 0 && g_mutating.load() == 0 && g_gen.load() == gen;
    if (r) *priorityOut = priority; // the game's answer is returned in every case below
    if (!settled) {
        c_inconclusive.Add();
        return r;
    }
    if (r == a.provider && (!r || priority == a.priority) && !keyChanged) {
        c_verified.Add();
        return r;
    }
    c_mismatches.Add();
    g_selfDisabled.store(true);
    // the game's answer is returned whatever happens to the report (07/10, players' Runtime Error)
    HookGuard::Try("ResourceCache verification report", [&] {
        uint32_t gameIndex = 0xFFFFFFFF;
        if (r) FindInList(reinterpret_cast<uintptr_t>(self), r, priority, gameIndex);
        const std::string cacheSaid = a.provider ? std::format("package {:#010x} (vtable {:#010x}, priority {}, index {})", a.provider, VtableOf(a.provider), a.priority, a.index)
                                                 : std::string("no package holds it");
        const std::string gameSays = r ? std::format("package {:#010x} (vtable {:#010x}, priority {}, index {})", r, VtableOf(r), priority, static_cast<int32_t>(gameIndex))
                                       : std::string("no package holds it");
        const std::string text = std::format("key {}: cache said {}{}, the game says {}{}", KeyText(keyBefore), cacheSaid, a.esum != kNoSum ? " (write epochs)" : "", gameSays,
                                             keyChanged ? "; the game's lookup changed the key in place" : "");
        {
            std::lock_guard<std::mutex> lock(g_mismatchLock);
            g_lastMismatch = text;
        }
        LOG_ERROR("[ResourceCache] Verification mismatch, the cache turned itself off for this session: " + text);
    });
    return r;
}

// ---- hooks (every thread) ----
uint32_t __fastcall Hook_FindProvider(void* self, void* edx, const uint32_t* key, int32_t* priorityOut) {
    const FindFn next = reinterpret_cast<FindFn>(SlotChain::Next(Site::FindProvider, Layer::ResourceCache));
    LookupNote& note = t_note;
    note.seen = true;
    note.hit = false;
    note.negative = false;
    note.probes = 0;
    if (!g_on.load(std::memory_order_acquire) || g_selfDisabled.load(std::memory_order_relaxed) || !key || !priorityOut) return next(self, edx, key, priorityOut);
    c_lookups.Add();
    const uintptr_t mgr = reinterpret_cast<uintptr_t>(self);
    if (g_mutating.load(std::memory_order_acquire) != 0) {
        c_bypassed.Add();
        return next(self, edx, key, priorityOut);
    }
    const uint32_t gen = g_gen.load(std::memory_order_acquire);
    const uint64_t t0 = kPublicBuild ? 0 : Qpc();
    Answer a;
    if (Find(mgr, key, gen, a)) {
        uint32_t probes = 0;
        bool byEpoch = false;
        if (Recheck(mgr, key, gen, a, probes, byEpoch)) {
            note.hit = true;
            note.negative = a.provider == 0;
            note.probes = probes;
            c_hits.Add();
            c_probes.Add(probes);
            if (!a.provider) c_negHits.Add();
            if (byEpoch) c_epochHits.Add();
            if (!kPublicBuild) {
                g_hitTicks.fetch_add(Qpc() - t0, std::memory_order_relaxed);
                if (ShouldVerify(t_verifyCount)) return Verify(next, self, edx, key, priorityOut, a, gen); // returns (and writes) the game's answer
            }
            if (a.provider) *priorityOut = a.priority; // what the game writes on a find: the package's priority (nothing on a miss)
            MaybeFingerprint(mgr, gen);
            return a.provider;
        }
        c_rejected.Add();
    }
    // The game's own lookup; its answer is stored when no list change happened around it
    c_misses.Add();
    const int32_t m0 = g_mutating.load(std::memory_order_acquire);
    const uint32_t g0 = g_gen.load(std::memory_order_acquire);
    const uint32_t seq0 = g_writeSeq.load(std::memory_order_seq_cst);
    const bool seqValid = g_writeBusy.load(std::memory_order_seq_cst) == 0;
    // "Remember missing files": whether every read-only package answers for sure before the game's probe (an absent
    // answer is stored only when this held before and after it; see Remember)
    const bool roBefore = g_negOn.load(std::memory_order_acquire) && ReadOnlyAboveReliable(mgr, kAllAbove);
    const uint64_t t1 = kPublicBuild ? 0 : Qpc();
    const uint32_t r = next(self, edx, key, priorityOut);
    if (!kPublicBuild) g_missTicks.fetch_add(Qpc() - t1, std::memory_order_relaxed);
    if (!r) c_notFound.Add();
    if (!r && !g_negOn.load(std::memory_order_acquire)) return r;
    if (m0 == 0 && g_mutating.load(std::memory_order_acquire) == 0 && g_gen.load(std::memory_order_acquire) == g0)
        HookGuard::Run("ResourceCache store (after the game's lookup)", [&] { Remember(mgr, key, r, r ? *priorityOut : 0, g0, seqValid, seq0, roBefore); });
    else c_notCached.Add();
    return r;
}

// List changes and change notices: the generation moves before and after, and lookups meanwhile go to the game
uint64_t __fastcall Hook_RegisterDb(void* self, void* edx, uint32_t add, uint32_t db, uint32_t priority) {
    BeginChange();
    const uint64_t r = reinterpret_cast<This3Fn>(SlotChain::Next(Site::RegisterDb, Layer::ResourceCache))(self, edx, add, db, priority);
    EndChange();
    c_listChanges.Add();
    return r;
}
uint64_t __fastcall Hook_RegisterDbDerived(void* self, void* edx, uint32_t add, uint32_t db, uint32_t priority) {
    BeginChange();
    const uint64_t r = reinterpret_cast<This3Fn>(SlotChain::Next(Site::RegisterDbDerived, Layer::ResourceCache))(self, edx, add, db, priority);
    EndChange();
    c_listChanges.Add();
    return r;
}
uint64_t __fastcall Hook_SetDbPriority(void* self, void* edx, uint32_t db, uint32_t priority) {
    BeginChange();
    const uint64_t r = reinterpret_cast<This2Fn>(SlotChain::Next(Site::SetDbPriority, Layer::ResourceCache))(self, edx, db, priority);
    EndChange();
    c_listChanges.Add();
    return r;
}
uint64_t __fastcall Hook_DbChanged(void* self, void* edx, uint32_t db, uint32_t keys) {
    BeginChange(); // its own FindProvider calls (one per changed key) go straight to the game
    const uint64_t r = reinterpret_cast<This2Fn>(SlotChain::Next(Site::DbChanged, Layer::ResourceCache))(self, edx, db, keys);
    EndChange();
    c_notices.Add();
    return r;
}

// ---- checks of the running build's code ----
bool MatchBytes(uintptr_t at, const char* pattern) {
    size_t n = 0;
    for (const char* p = pattern; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        n++;
        p += 2;
    }
    if (!n) return false;
    uint8_t buf[96];
    if (n > sizeof buf || !MemPatch::ReadBytes(at, buf, n)) return false;
    return MemPatch::ScanPattern(buf, n, pattern) == reinterpret_cast<uintptr_t>(buf);
}

uintptr_t CallTarget(uintptr_t at) {
    uint8_t b[5];
    if (!MemPatch::ReadBytes(at, b, 5) || b[0] != 0xE8) return 0;
    int32_t rel;
    std::memcpy(&rel, b + 1, 4);
    return at + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
}

// Target of the first "mov ecx,esi; call X" followed by `after` (2 bytes) within `range` bytes of fn, or 0
uintptr_t CallAfterMovEcxEsi(uintptr_t fn, size_t range, uint8_t after0, uint8_t after1) {
    uint8_t b[0x100];
    if (range > sizeof b || !MemPatch::ReadBytes(fn, b, range)) return 0;
    for (size_t i = 0; i + 9 <= range; i++)
        if (b[i] == 0x8B && b[i + 1] == 0xCE && b[i + 2] == 0xE8 && b[i + 7] == after0 && b[i + 8] == after1) {
            int32_t rel;
            std::memcpy(&rel, b + i + 3, 4);
            return fn + i + 7 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
        }
    return 0;
}

bool CheckReadOnlyClass(uintptr_t vtable, std::string& why) {
    uint32_t openRecord = 0, deleteRecord = 0;
    if (!vtable || !MemPatch::ReadBytes(vtable + 0x34, &openRecord, 4) || !MemPatch::ReadBytes(vtable + 0x40, &deleteRecord, 4)) {
        why = "its vtable is not readable";
        return false;
    }
    // OpenRecord 0x007345D0: fast path through the key set when no record / info is asked
    if (!MatchBytes(openRecord, "53 55 8B 6C 24 10 85 ED 56 57 8B 7C 24 14 8B F1 75 ?? 83 7C 24 28 00 75 ?? 6A 01 32 DB E8")) {
        why = std::format("its OpenRecord {:#010x} differs", openRecord);
        return false;
    }
    // ... whose slow path (mov ecx,esi; call; mov bl,al) is the base OpenRecord 0x0072D470, which only opens existing
    // records (dispositions 6 and 3)
    const uintptr_t base = CallAfterMovEcxEsi(openRecord, 0xA0, 0x8A, 0xD8);
    if (!base || !MatchBytes(base, "8B 44 24 10 83 EC 1C 83 F8 06 56 8B F1 74 ?? 83 F8 03 74 ?? 32 C0")) {
        why = std::format("its base OpenRecord {:#010x} can create records", base);
        return false;
    }
    // DeleteRecord 0x007346A0 -> 0x00624F70 "xor al,al; ret 4"
    const uintptr_t del = CallAfterMovEcxEsi(deleteRecord, 0x30, 0x8B, 0x16);
    if (!del || !MatchBytes(del, "32 C0 C2 04 00")) {
        why = std::format("its DeleteRecord {:#010x} can delete records", deleteRecord);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Write-epoch hooks: one wrapper per (class, slot), calling the function the slot held
// ---------------------------------------------------------------------------------------------------------------------
enum class Bump : uint8_t {
    Always,
    RecordAsked,      // OpenRecord: a record pointer was passed (arg 2 != 0)
    RecordMayCreate,  // OpenRecord: a record pointer and write access (arg 3 & 2) or a disposition other than 6 / 3 (arg 4)
    WritableRecord,   // CloseRecord: the record's type [rec+8] is the writable one (0x12E4A892), or unreadable
};
struct EpochSpec {
    int cls;
    uint16_t off;
    uint8_t args; // stack arguments (ret args*4)
    Bump mode;
    const char* name;
    const char* bytes; // the studied function's first bytes
};
// args = the real stack arguments: each function's own return followed to its end, through internal jumps and tail
// jumps (checked 2026-09-29 for every row). ConvertIndex 0x004A6C80 ends with "jmp 004E17B0" (the lock release, a plain
// ret): 0 arguments. It was listed with 2 (the "ret 8" after it belongs to the next function, 0x004A6D60), so the
// wrapper popped 8 bytes of its caller's stack: the crash in DeleteRecord 0x004A85A0 at 0x004A8639 (v1.8.0).
// clang-format off
constexpr EpochSpec kSpecs[] = {
    // DPF 0x00FB2600 and its derived class 0x01048DA0: the same functions except the destructor
    {kClsDpf, 0x00, 1, Bump::Always, "DPF destructor", "56 8B F1 E8 ?? ?? ?? ?? F6 44 24 08 01 74 09 56 E8"},
    {kClsDpf, 0x08, 0, Bump::Always, "DPF shutdown", "56 8B F1 80 7E 0C 00 74 49 83 7E 14 00 74 07 8B 06 8B 50 1C"},
    {kClsDpf, 0x18, 3, Bump::Always, "DPF open", "53 55 8B 6C 24 0C 56 8B F1 8A 4C 24 18 32 C0 84 C9 57"},
    {kClsDpf, 0x1C, 0, Bump::Always, "DPF close", "6A 01 E8 ?? ?? ?? ?? C3"},
    {kClsDpf, 0x24, 0, Bump::Always, "DPF flush", "53 55 56 8B F1 8D AE 70 02 00 00 68 ?? ?? ?? ?? 8B CD B3 01"},
    {kClsDpf, 0x34, 6, Bump::RecordMayCreate, "DPF OpenRecord", "83 EC 20 53 55 56 33 ED F6 44 24 38 03 57 8B F1 0F 84"},
    {kClsDpf, 0x3C, 1, Bump::WritableRecord, "DPF CloseRecord", "83 EC 08 53 55 56 8B D9 57 8D 8B 70 02 00 00 68"},
    {kClsDpf, 0x40, 1, Bump::Always, "DPF DeleteRecord", "83 EC 14 53 56 8B F1 8D 8E 70 02 00 00 68"},
    {kClsDpf, 0x5C, 1, Bump::Always, "DPF set index", "83 79 14 00 8B 81 D0 02 00 00 75 0A 8B 54 24 04 89 91 D0 02"},
    {kClsDpf, 0x8C, 0, Bump::Always, "DPF load index", "83 EC 24 53 56 8B F1 8B 8E D0 02 00 00 32 DB 85 C9"},
    {kClsDpf, 0x9C, 0, Bump::Always, "DPF convert index", "83 EC 08 55 56 8B F1 8D AE 70 02 00 00 68"},
    {kClsDpfDerived, 0x00, 1, Bump::Always, "DPF (derived) destructor", "56 8B F1 C7 06 ?? ?? ?? ?? C7 46 04 ?? ?? ?? ?? 8B 86 98 03"},
    {kClsDpfDerived, 0x08, 0, Bump::Always, "DPF (derived) shutdown", "56 8B F1 80 7E 0C 00 74 49 83 7E 14 00 74 07 8B 06 8B 50 1C"},
    {kClsDpfDerived, 0x18, 3, Bump::Always, "DPF (derived) open", "53 55 8B 6C 24 0C 56 8B F1 8A 4C 24 18 32 C0 84 C9 57"},
    {kClsDpfDerived, 0x1C, 0, Bump::Always, "DPF (derived) close", "6A 01 E8 ?? ?? ?? ?? C3"},
    {kClsDpfDerived, 0x24, 0, Bump::Always, "DPF (derived) flush", "53 55 56 8B F1 8D AE 70 02 00 00 68 ?? ?? ?? ?? 8B CD B3 01"},
    {kClsDpfDerived, 0x34, 6, Bump::RecordMayCreate, "DPF (derived) OpenRecord", "83 EC 20 53 55 56 33 ED F6 44 24 38 03 57 8B F1 0F 84"},
    {kClsDpfDerived, 0x3C, 1, Bump::WritableRecord, "DPF (derived) CloseRecord", "83 EC 08 53 55 56 8B D9 57 8D 8B 70 02 00 00 68"},
    {kClsDpfDerived, 0x40, 1, Bump::Always, "DPF (derived) DeleteRecord", "83 EC 14 53 56 8B F1 8D 8E 70 02 00 00 68"},
    {kClsDpfDerived, 0x5C, 1, Bump::Always, "DPF (derived) set index", "83 79 14 00 8B 81 D0 02 00 00 75 0A 8B 54 24 04 89 91 D0 02"},
    {kClsDpfDerived, 0x8C, 0, Bump::Always, "DPF (derived) load index", "83 EC 24 53 56 8B F1 8B 8E D0 02 00 00 32 DB 85 C9"},
    {kClsDpfDerived, 0x9C, 0, Bump::Always, "DPF (derived) convert index", "83 EC 08 55 56 8B F1 8D AE 70 02 00 00 68"},
    // DDF 0x00FB2420
    {kClsDdf, 0x00, 1, Bump::Always, "DDF destructor", "56 8B F1 E8 ?? ?? ?? ?? F6 44 24 08 01 74 09 56 E8"},
    {kClsDdf, 0x08, 0, Bump::Always, "DDF shutdown", "80 79 0C 00 74 0B 8B 01 8B 50 1C C6 41 0C 00 FF D2 B0 01 C3"},
    {kClsDdf, 0x18, 3, Bump::Always, "DDF open", "53 56 8B F1 80 7E 0C 00 57 74 62 8B 7C 24 14 83 FF 06"},
    {kClsDdf, 0x1C, 0, Bump::Always, "DDF close", "53 56 57 8B F9 E8 ?? ?? ?? ?? 8B 47 5C 8D 77 50 50 8B CE E8"},
    {kClsDdf, 0x2C, 1, Bump::Always, "DDF set location", "57 8B F9 83 7F 18 00 75 33 8B 4C 24 08 66 83 39 00"},
    {kClsDdf, 0x34, 6, Bump::RecordAsked, "DDF OpenRecord", "81 EC 10 04 00 00 53 55 56 8B F1 57 8D BE 80 00 00 00 68"},
    {kClsDdf, 0x40, 1, Bump::Always, "DDF DeleteRecord", "81 EC 10 02 00 00 53 55 56 8B F1 57 8D 8E 80 00 00 00 68"},
    {kClsDdf, 0x58, 2, Bump::Always, "DDF refresh one file", "81 EC 3C 08 00 00 53 55 56 8B F1 57 8D 8E 80 00 00 00 68"},
    {kClsDdf, 0x5C, 1, Bump::Always, "DDF rescan", "83 EC 24 53 57 8B F9 33 DB 38 5F 0C 0F 84"},
    // base packed stream 0x00FFD790
    {kClsPackedStream, 0x00, 1, Bump::Always, "packed stream destructor", "56 8B F1 E8 ?? ?? ?? ?? F6 44 24 08 01 74 09 56 E8"},
    {kClsPackedStream, 0x08, 0, Bump::Always, "packed stream shutdown", "56 8B F1 80 7E 0C 00 74 11 83 7E 14 00 74 07 8B 06 8B 50 1C"},
    {kClsPackedStream, 0x18, 3, Bump::Always, "packed stream open", "8B 44 24 04 83 EC 68 53 32 DB A8 02 56 8B F1"},
    {kClsPackedStream, 0x1C, 0, Bump::Always, "packed stream close", "56 8B F1 83 7E 14 00 74 0C E8 ?? ?? ?? ?? C7 46 14 00 00 00"},
    {kClsPackedStream, 0x2C, 1, Bump::Always, "packed stream set location", "83 79 14 00 75 2B 8B 54 24 04 66 83 3A 00"},
};
// clang-format on
constexpr int kSpecCount = static_cast<int>(std::size(kSpecs));
const char* const kClsName[kClsCount] = {"DPF", "DPF (derived)", "DDF", "packed stream"};
// The DPF's direct record write 0x004A7FC0 (entry hook): part of both DPF classes
constexpr const char* kDpfWriteDirectBytes = "83 EC 28 53 56 8B F1 8D 8E 70 02 00 00 68 ?? ?? ?? ?? 89 4C 24 10 E8 ?? ?? ?? ?? B3 02 84 5E 14";

std::atomic<uintptr_t> g_specOrig[kSpecCount] = {}; // the function the slot held (constant once set: hooks may still run)
uintptr_t g_specSlot[kSpecCount] = {};              // guarded by g_ctrl
bool g_specInstalled[kSpecCount] = {};              // guarded by g_ctrl
bool g_dpfDirectInstalled = false;                  // guarded by g_ctrl

bool ShouldBump(Bump mode, const uint32_t* args) {
    switch (mode) {
    case Bump::Always:
        return true;
    case Bump::RecordAsked:
        return args[1] != 0;
    case Bump::RecordMayCreate:
        return args[1] != 0 && ((args[2] & 2) != 0 || (args[3] != 6 && args[3] != 3));
    case Bump::WritableRecord: {
        uint32_t type = 0;
        if (!args[0] || !MemPatch::ReadBytes(static_cast<uintptr_t>(args[0]) + 8, &type, 4)) return true;
        return type == 0x12E4A892u;
    }
    }
    return true;
}

// WriteBegin / the game's write / WriteEnd. WriteEnd is in a __finally: an exception unwinding through the game's write
// (SEH, or a C++ exception thrown inside it) must not leave g_writeBusy raised, which would keep every epoch sum from
// being trusted until the game restarts. No object with a destructor lives in this frame (C2712).
template <class Call> uint64_t Bracketed(void* db, const Call& call) {
    uint64_t r = 0;
    WriteBegin(db);
    __try {
        r = call();
    } __finally {
        WriteEnd(db);
    }
    return r;
}

template <int I, int N> struct EpochHook;
template <int I> struct EpochHook<I, 0> {
    static uint64_t __fastcall Fn(void* self, void* edx) {
        const uint32_t args[6] = {};
        const auto game = reinterpret_cast<uint64_t(__fastcall*)(void*, void*)>(g_specOrig[I].load(std::memory_order_acquire));
        if (!ShouldBump(kSpecs[I].mode, args)) return game(self, edx);
        return Bracketed(self, [&] { return game(self, edx); });
    }
};
template <int I> struct EpochHook<I, 1> {
    static uint64_t __fastcall Fn(void* self, void* edx, uint32_t a) {
        const uint32_t args[6] = {a};
        const auto game = reinterpret_cast<uint64_t(__fastcall*)(void*, void*, uint32_t)>(g_specOrig[I].load(std::memory_order_acquire));
        if (!ShouldBump(kSpecs[I].mode, args)) return game(self, edx, a);
        return Bracketed(self, [&] { return game(self, edx, a); });
    }
};
template <int I> struct EpochHook<I, 2> {
    static uint64_t __fastcall Fn(void* self, void* edx, uint32_t a, uint32_t b) {
        const uint32_t args[6] = {a, b};
        const auto game = reinterpret_cast<uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t)>(g_specOrig[I].load(std::memory_order_acquire));
        if (!ShouldBump(kSpecs[I].mode, args)) return game(self, edx, a, b);
        return Bracketed(self, [&] { return game(self, edx, a, b); });
    }
};
template <int I> struct EpochHook<I, 3> {
    static uint64_t __fastcall Fn(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c) {
        const uint32_t args[6] = {a, b, c};
        const auto game = reinterpret_cast<uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t)>(g_specOrig[I].load(std::memory_order_acquire));
        if (!ShouldBump(kSpecs[I].mode, args)) return game(self, edx, a, b, c);
        return Bracketed(self, [&] { return game(self, edx, a, b, c); });
    }
};
template <int I> struct EpochHook<I, 6> {
    static uint64_t __fastcall Fn(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
        const uint32_t args[6] = {a, b, c, d, e, f};
        const auto game = reinterpret_cast<uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)>(
            g_specOrig[I].load(std::memory_order_acquire));
        if (!ShouldBump(kSpecs[I].mode, args)) return game(self, edx, a, b, c, d, e, f);
        return Bracketed(self, [&] { return game(self, edx, a, b, c, d, e, f); });
    }
};
template <int I> void* EpochHookOf() { return reinterpret_cast<void*>(&EpochHook<I, kSpecs[I].args>::Fn); }
template <size_t... Is> std::array<void*, sizeof...(Is)> MakeEpochHooks(std::index_sequence<Is...>) { return {EpochHookOf<static_cast<int>(Is)>()...}; }
const std::array<void*, kSpecCount> g_specHook = MakeEpochHooks(std::make_index_sequence<kSpecCount>{});

// The DPF's direct record write 0x004A7FC0, thiscall(5 args), ret 0x14 (entry hook)
uint64_t __fastcall Hook_DpfWriteDirect(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    using F = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    const F game = reinterpret_cast<F>(EntryChain::Next(EntryChain::Site::DpfWriteDirect, EntryChain::Layer::ResourceCache));
    return Bracketed(self, [&] { return game(self, edx, a, b, c, d, e); });
}

// One aligned 4-byte vtable slot: compare-exchange while its page is writable
bool SwapSlot(uintptr_t slot, uintptr_t expect, uintptr_t value) {
    if (!slot || (slot & 3)) return false;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(slot), 4, PAGE_READWRITE, &old)) return false;
    const long prev = _InterlockedCompareExchange(reinterpret_cast<volatile long*>(slot), static_cast<long>(value), static_cast<long>(expect));
    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), 4, old, &tmp);
    return static_cast<uintptr_t>(static_cast<unsigned long>(prev)) == expect;
}

void RemoveClassHooks(int cls) {
    for (int i = 0; i < kSpecCount; i++) {
        if (kSpecs[i].cls != cls || !g_specInstalled[i]) continue;
        if (!SwapSlot(g_specSlot[i], reinterpret_cast<uintptr_t>(g_specHook[i]), g_specOrig[i].load()))
            LOG_WARNING(std::format("[ResourceCache] The vtable slot {:#010x} ({}) was changed by another module; left as it is (it still reaches Apex's wrapper, which keeps "
                                    "forwarding)",
                                    g_specSlot[i], kSpecs[i].name));
        g_specInstalled[i] = false;
    }
}

// Checks every studied method of the class and swaps its slots (all or nothing). Caller holds g_ctrl.
bool InstallClassHooks(int cls, std::string& why) {
    const uintptr_t vt = g_clsVtable[cls];
    if (!vt) {
        why = "vtable not found";
        return false;
    }
    uintptr_t fn[kSpecCount] = {};
    for (int i = 0; i < kSpecCount; i++) {
        if (kSpecs[i].cls != cls) continue;
        uint32_t v = 0;
        if (!MemPatch::ReadBytes(vt + kSpecs[i].off, &v, 4)) {
            why = std::format("slot +{:#x} not readable", kSpecs[i].off);
            return false;
        }
        if (v == reinterpret_cast<uintptr_t>(g_specHook[i])) v = static_cast<uint32_t>(g_specOrig[i].load()); // left from an earlier start
        if (!MatchBytes(v, kSpecs[i].bytes)) {
            why = std::format("{} {:#010x} differs from the studied code", kSpecs[i].name, v);
            return false;
        }
        fn[i] = v;
    }
    for (int i = 0; i < kSpecCount; i++) {
        if (kSpecs[i].cls != cls) continue;
        const uintptr_t slot = vt + kSpecs[i].off;
        uint32_t cur = 0;
        MemPatch::ReadBytes(slot, &cur, 4);
        if (cur == reinterpret_cast<uintptr_t>(g_specHook[i])) { // still ours (a removal left it)
            g_specSlot[i] = slot;
            g_specInstalled[i] = true;
            continue;
        }
        g_specOrig[i].store(fn[i], std::memory_order_release); // before the slot can reach the wrapper
        if (!SwapSlot(slot, fn[i], reinterpret_cast<uintptr_t>(g_specHook[i]))) {
            RemoveClassHooks(cls);
            why = std::format("could not write the vtable slot {:#010x} ({})", slot, kSpecs[i].name);
            return false;
        }
        g_specSlot[i] = slot;
        g_specInstalled[i] = true;
    }
    return true;
}

// Write epochs on (caller holds g_ctrl): every class that passes its checks is counted; the rest stays probed
void EnableEpochs() {
    g_clsVtable[kClsDpf] = GameAddr::Get(GameAddr::Id::DpfVtable);
    g_clsVtable[kClsDpfDerived] = GameAddr::Get(GameAddr::Id::DpfDerivedVtable);
    g_clsVtable[kClsDdf] = GameAddr::Get(GameAddr::Id::DdfVtable);
    g_clsVtable[kClsPackedStream] = GameAddr::Get(GameAddr::Id::PackedStreamVtable);
    std::string summary;
    // The DPF classes also need the direct record write (not a vtable method)
    bool dpfDirect = g_dpfDirectInstalled;
    std::string dpfWhy;
    if (!dpfDirect) {
        const uintptr_t direct = GameAddr::Get(GameAddr::Id::DpfWriteDirect);
        std::string err;
        if (!direct || !MatchBytes(direct, kDpfWriteDirectBytes)) dpfWhy = std::format("direct record write {:#010x} differs from the studied code", direct);
        else if (!EntryChain::Install(EntryChain::Site::DpfWriteDirect, EntryChain::Layer::ResourceCache, reinterpret_cast<void*>(&Hook_DpfWriteDirect), &err))
            dpfWhy = "direct record write not hooked: " + err;
        else dpfDirect = g_dpfDirectInstalled = true;
    }
    for (int c = 0; c < kClsCount; c++) {
        std::string why;
        bool ok = false;
        if ((c == kClsDpf || c == kClsDpfDerived) && !dpfDirect) why = dpfWhy;
        else ok = InstallClassHooks(c, why);
        g_clsCounted[c].store(ok, std::memory_order_release);
        summary += std::format("{}{} {}", summary.empty() ? "" : "; ", kClsName[c], ok ? "counted" : "probed (" + why + ")");
    }
    bool any = false;
    for (int c = 0; c < kClsCount; c++) any = any || g_clsCounted[c].load();
    if (!any && g_dpfDirectInstalled) {
        EntryChain::Remove(EntryChain::Site::DpfWriteDirect, EntryChain::Layer::ResourceCache);
        g_dpfDirectInstalled = false;
    }
    g_epochTrustTick.store(GetTickCount64() + kEpochGraceMs, std::memory_order_relaxed);
    g_epochsOn.store(any, std::memory_order_release);
    BeginChange(); // snapshots and entries from now on know which databases are counted
    EndChange();
    LOG_INFO("[ResourceCache] Write epochs: " + summary);
}

void DisableEpochs() {
    g_epochsOn.store(false, std::memory_order_release);
    for (int c = 0; c < kClsCount; c++) g_clsCounted[c].store(false, std::memory_order_release);
    BeginChange(); // nothing stored with epoch sums is used again
    EndChange();
    for (int c = 0; c < kClsCount; c++) RemoveClassHooks(c);
    if (g_dpfDirectInstalled && EntryChain::Remove(EntryChain::Site::DpfWriteDirect, EntryChain::Layer::ResourceCache)) g_dpfDirectInstalled = false;
}

std::string EpochClassText() {
    std::string s;
    for (int c = 0; c < kClsCount; c++)
        if (g_clsCounted[c].load()) s += (s.empty() ? "" : ", ") + std::string(kClsName[c]);
    return s.empty() ? std::string("none") : s;
}

// "Remember missing files" follows the lookup cache (caller holds g_ctrl)
void UpdateMisses() {
    const bool want = g_started && g_missesWanted.load();
    if (want == g_negOn.load()) return;
    if (want) {
        EnableEpochs();
        g_negOn.store(true, std::memory_order_release);
    } else {
        g_negOn.store(false, std::memory_order_release);
        DisableEpochs();
    }
    BeginChange();
    EndChange();
}

// ---------------------------------------------------------------------------------------------------------------------
// Package-list watchers (shared by the lookup cache and the file list cache)
// ---------------------------------------------------------------------------------------------------------------------
struct Hooked {
    Site site;
    void* hook;
};
const Hooked kWatchers[] = {
    {Site::RegisterDb, reinterpret_cast<void*>(&Hook_RegisterDb)},
    {Site::RegisterDbDerived, reinterpret_cast<void*>(&Hook_RegisterDbDerived)},
    {Site::SetDbPriority, reinterpret_cast<void*>(&Hook_SetDbPriority)},
    {Site::DbChanged, reinterpret_cast<void*>(&Hook_DbChanged)},
};

// Caller holds g_ctrl
bool AcquireWatchers(std::string& why) {
    if (g_watcherUsers > 0) {
        g_watcherUsers++;
        return true;
    }
    for (int done = 0; done < static_cast<int>(std::size(kWatchers)); done++) {
        std::string err;
        if (!SlotChain::Install(kWatchers[done].site, Layer::ResourceCache, kWatchers[done].hook, &err)) {
            for (int k = 0; k < done; k++) SlotChain::Remove(kWatchers[k].site, Layer::ResourceCache);
            why = "Could not hook the package list: " + err;
            return false;
        }
    }
    g_watcherUsers = 1;
    return true;
}
void ReleaseWatchers() {
    if (g_watcherUsers <= 0) return;
    if (--g_watcherUsers > 0) return;
    for (const Hooked& h : kWatchers) SlotChain::Remove(h.site, Layer::ResourceCache);
}

// Caller holds g_ctrl
bool CheckReadOnly(std::string& why) {
    if (g_readOnlyOk) return true;
    std::string err;
    g_readOnlyVtable = GameAddr::Get(GameAddr::Id::ShadowedDbVtable);
    g_readOnlyOk = CheckReadOnlyClass(g_readOnlyVtable, err);
    if (!g_readOnlyOk) why = "The game's read-only package class was not recognised (" + err + "); nothing could be cached";
    return g_readOnlyOk;
}

// ---------------------------------------------------------------------------------------------------------------------
// File list cache
// ---------------------------------------------------------------------------------------------------------------------
constexpr size_t kKlMaxKeys = 1u << 18;    // keys kept in total (4 MB); more: everything is dropped and captured again
constexpr size_t kKlMaxPerList = 1u << 16; // longer lists are not kept
constexpr uint32_t kKlMaxAgeMs = 60000;

// (2.15.3) The keys are shared, read only: a lookup took a copy of each package's list under the lock (about 207 packages a
// call, one allocation each; 2 ms a call on average, up to 4 ms, on the render thread). It now takes a reference.
using KlKeys = std::shared_ptr<const std::vector<uint32_t>>;
struct KlEntry {
    uint32_t tick = 0;
    int32_t ret = 0; // what the package's GetKeyList returned
    KlKeys keys;     // 4 dwords per key, in the order it appended them (never null once stored)
};
std::unordered_map<uint64_t, KlEntry> g_kl; // guarded by g_klLock
SRWLOCK g_klLock = SRWLOCK_INIT;
uint32_t g_klGen = 0;     // guarded by g_klLock: the generation of every entry in g_kl
size_t g_klKeyCount = 0;  // guarded by g_klLock
std::atomic<bool> g_klOn{false};
std::atomic<bool> g_klSelfDisabled{false};
uintptr_t g_typeFilterVt = 0, g_vecInsert = 0, g_sortUnique = 0; // set by StartKeyLists
Counter c_klCalls, c_klCached, c_klPassed, c_klFromMem, c_klAsked, c_klStored, c_klNotStored, c_klVerified, c_klMismatches, c_klInconclusive;

inline uint64_t KlKey(uint32_t db, uint32_t type) { return (static_cast<uint64_t>(db) << 32) | type; }

// 07/10, players' Runtime Error: these run inside the game's GetKeyList on any thread. The locks are scoped and the copies
// caught: a list that cannot be copied is simply not remembered / not found (the package is asked, as without the cache).
bool KlFind(uint32_t db, uint32_t type, uint32_t gen, KlKeys& keys, int32_t& ret) {
    keys.reset();
    try {
        ApexUtil::SrwShared srw(g_klLock);
        if (g_klGen == gen) {
            const auto it = g_kl.find(KlKey(db, type));
            if (it != g_kl.end() && it->second.keys && GetTickCount() - it->second.tick <= kKlMaxAgeMs) {
                keys = it->second.keys;
                ret = it->second.ret;
                return true;
            }
        }
    } catch (...) {
        keys.reset();
    }
    return false;
}

void KlStore(uint32_t db, uint32_t type, uint32_t gen, int32_t ret, const std::vector<uint32_t>& keys) {
    if (keys.size() / 4 > kKlMaxPerList) {
        c_klNotStored.Add();
        return;
    }
    try {
        KlKeys copy = std::make_shared<const std::vector<uint32_t>>(keys); // outside the lock; moved in below, so a failure never leaves a half-updated entry
        ApexUtil::SrwExclusive srw(g_klLock);
        if (g_gen.load() == gen && g_mutating.load() == 0) {
            if (g_klGen != gen || g_klKeyCount + keys.size() / 4 > kKlMaxKeys) {
                g_kl.clear();
                g_klKeyCount = 0;
                g_klGen = gen;
            }
            KlEntry& e = g_kl[KlKey(db, type)]; // the only allocation under the lock: nothing changed yet if it throws
            if (e.keys) g_klKeyCount -= e.keys->size() / 4;
            e.tick = GetTickCount();
            e.ret = ret;
            e.keys = std::move(copy);
            g_klKeyCount += e.keys->size() / 4;
            c_klStored.Add();
        } else {
            c_klNotStored.Add();
        }
    } catch (...) {
        c_klNotStored.Add();
    }
}

// The out vector {begin, end, capacity}: its size in keys (SEH: a bad vector is the game's fault too, but no C++ here)
bool VecRange(const void* out, uintptr_t& begin, uintptr_t& end) {
    __try {
        begin = *reinterpret_cast<const uintptr_t*>(out);
        end = *reinterpret_cast<const uintptr_t*>(static_cast<const uint8_t*>(out) + 4);
        return end >= begin && ((end - begin) & 15) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Appends one key as the game's own loops do (0x004B1B59 / 0x0072DF20): in place when end < capacity, else the
// vector's insert 0x006D3810 (which grows it with its own allocator)
void AppendKey(uint8_t* out, const uint32_t* key) {
    uint32_t* end = *reinterpret_cast<uint32_t**>(out + 4);
    uint32_t* cap = *reinterpret_cast<uint32_t**>(out + 8);
    if (end < cap) {
        *reinterpret_cast<uint32_t**>(out + 4) = end + 4;
        if (end) std::memcpy(end, key, 16);
    } else {
        reinterpret_cast<VecInsertFn>(g_vecInsert)(out, nullptr, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(end)), key);
    }
}

// The keys appended to out since it had `fromBytes` bytes
bool CapturedKeys(const void* out, uintptr_t fromBytes, std::vector<uint32_t>& keys) {
    uintptr_t b = 0, e = 0;
    if (!VecRange(out, b, e) || e - b < fromBytes) return false;
    try {
        keys.resize((e - b - fromBytes) / 4);
    } catch (...) { // no memory for the copy: not remembered (07/10)
        keys.clear();
        return false;
    }
    return keys.empty() || MemPatch::ReadBytes(b + fromBytes, keys.data(), keys.size() * 4);
}

// The out vector back to `bytes` bytes (its keys are plain data): the game's own call then appends them again
void TruncateVec(void* out, uintptr_t bytes) {
    __try {
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(out);
        *reinterpret_cast<uintptr_t*>(static_cast<uint8_t*>(out) + 4) = b + bytes;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Same multiset of keys (the order inside one package differs between the game's two walks)
bool SameKeys(std::vector<uint32_t> a, std::vector<uint32_t> b) {
    if (a.size() != b.size()) return false;
    auto sortKeys = [](std::vector<uint32_t>& v) {
        std::vector<std::array<uint32_t, 4>> k(v.size() / 4);
        std::memcpy(k.data(), v.data(), v.size() * 4);
        std::sort(k.begin(), k.end());
        std::memcpy(v.data(), k.data(), v.size() * 4);
    };
    sortKeys(a);
    sortKeys(b);
    return a == b;
}

void KlMismatch(uint32_t db, uint32_t type, size_t cached, size_t game, int32_t cachedRet, int32_t gameRet) {
    c_klMismatches.Add();
    g_klSelfDisabled.store(true);
    const std::string text = std::format("package {:#010x}, type {:08X}: remembered {} keys (returned {}), the game gives {} keys (returns {})", db, type, cached, cachedRet, game, gameRet);
    {
        std::lock_guard<std::mutex> lock(g_mismatchLock);
        g_klLastMismatch = text;
    }
    LOG_ERROR("[FileListCache] Verification mismatch, the file list cache turned itself off for this session: " + text);
}

// The list [begin, end) of the manager, read once as the game does (0x004B1BD6)
bool ReadListRaw(uintptr_t mgr, uintptr_t& begin, uintptr_t& end) {
    __try {
        begin = *reinterpret_cast<const uintptr_t*>(mgr + 0x30);
        end = *reinterpret_cast<const uintptr_t*>(mgr + 0x34);
        return end >= begin && ((end - begin) & 7) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t ReadU32(uintptr_t a) {
    uint32_t v = 0;
    MemPatch::ReadBytes(a, &v, 4);
    return v;
}

// The non-unique GetKeyList loop of 0x004B1AE0, with the read-only packages' lists from memory when known
int32_t EmulateKeyList(uintptr_t begin, uintptr_t end, uint8_t* out, void* filter, uint32_t type, KeyListNote& note) {
    const uint32_t gen = g_gen.load(std::memory_order_acquire);
    const bool verify = ShouldVerify(t_klVerifyCount);
    KlKeys cached;
    std::vector<uint32_t> got; // per call (a database call never re-enters GetKeyList, but nothing depends on it)
    int32_t count = 0;
    for (uintptr_t p = begin; p != end; p += 8) {
        void* db = *reinterpret_cast<void* const*>(p);
        void* const* vt = *reinterpret_cast<void* const* const*>(db);
        const DbKeyListFn fn = reinterpret_cast<DbKeyListFn>(vt[0x30 / 4]);
        const uint32_t dbv = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(db));
        if (reinterpret_cast<uintptr_t>(vt) != g_readOnlyVtable) {
            count += fn(db, nullptr, out, filter);
            c_klAsked.Add();
            continue;
        }
        int32_t cachedRet = 0;
        const bool have = KlFind(dbv, type, gen, cached, cachedRet);
        if (have && !verify) {
            for (size_t k = 0; k + 4 <= cached->size(); k += 4) AppendKey(out, cached->data() + k);
            count += cachedRet;
            note.cachedPackages++;
            c_klFromMem.Add();
            continue;
        }
        uintptr_t b0 = 0, e0 = 0;
        const bool rangeOk = VecRange(out, b0, e0);
        const bool reliable0 = RoReliable(dbv);
        const int32_t r = fn(db, nullptr, out, filter);
        count += r;
        c_klAsked.Add();
        const bool reliable1 = RoReliable(dbv);
        if (!rangeOk || !CapturedKeys(out, e0 - b0, got)) continue;
        if (have) {
            const bool settled = g_gen.load() == gen && g_mutating.load() == 0;
            if (!settled) c_klInconclusive.Add();
            else if (r == cachedRet && SameKeys(*cached, got)) c_klVerified.Add();
            else KlMismatch(dbv, type, cached->size() / 4, got.size() / 4, cachedRet, r);
            continue;
        }
        // an empty list may come from a failed open: kept only when the package answered from its key set or open index
        if (g_gen.load() == gen && g_mutating.load() == 0 && (!got.empty() || (reliable0 && reliable1))) KlStore(dbv, type, gen, r, got);
        else c_klNotStored.Add();
    }
    return count;
}

// Cacheable call: unique = false, a type filter, an out vector, no list change in progress
bool KlCacheable(void* out, void* filter, uint32_t unique, uint32_t& type) {
    if (!g_klOn.load(std::memory_order_acquire) || g_klSelfDisabled.load(std::memory_order_relaxed) || (unique & 0xFF) || !out || !filter) return false;
    if (g_mutating.load(std::memory_order_acquire) != 0) return false;
    const uintptr_t f = reinterpret_cast<uintptr_t>(filter);
    if (ReadU32(f) != g_typeFilterVt) return false;
    type = ReadU32(f + 4);
    return true;
}

// 07/10, players' Runtime Error: a C++ exception in the cache's loop (a copy, the verification's sort or text) must not
// reach the game. The keys appended so far are dropped (the vector back to its size before the call), the cache turns
// itself off for the session, and the game's own call answers.
template <class Game>
int32_t EmulateKeyListOrGame(uintptr_t begin, uintptr_t end, void* out, void* filter, uint32_t type, KeyListNote& note, Game&& game) {
    uintptr_t b0 = 0, e0 = 0;
    const bool rangeOk = VecRange(out, b0, e0);
    try {
        return EmulateKeyList(begin, end, static_cast<uint8_t*>(out), filter, type, note);
    } catch (...) {
        g_klSelfDisabled.store(true);
        HookGuard::Note("FileListCache key list (the game's own call answered)");
    }
    if (rangeOk) TruncateVec(out, e0 - b0);
    note.cachedPackages = 0;
    return game();
}

int32_t __fastcall Hook_KeyListBase(void* self, void* edx, void* out, void* filter, uint32_t unique) {
    const MgrKeyListFn next = reinterpret_cast<MgrKeyListFn>(SlotChain::Next(Site::KeyListBase, Layer::ResourceCache));
    KeyListNote& note = t_klNote;
    note.seen = true;
    note.cachedPackages = 0;
    uint32_t type = 0;
    uintptr_t begin = 0, end = 0;
    if (!g_klOn.load(std::memory_order_acquire)) return next(self, edx, out, filter, unique);
    c_klCalls.Add();
    if (!KlCacheable(out, filter, unique, type) || !ReadListRaw(reinterpret_cast<uintptr_t>(self), begin, end)) {
        c_klPassed.Add();
        return next(self, edx, out, filter, unique);
    }
    c_klCached.Add();
    return EmulateKeyListOrGame(begin, end, out, filter, type, note, [&] { return next(self, edx, out, filter, unique); });
}

// ResourceSystem's override 0x00736660: the base's result, then sort + unique of the whole vector when count and out
int32_t __fastcall Hook_KeyListDerived(void* self, void* edx, void* out, void* filter, uint32_t unique) {
    const MgrKeyListFn next = reinterpret_cast<MgrKeyListFn>(SlotChain::Next(Site::KeyListDerived, Layer::ResourceCache));
    KeyListNote& note = t_klNote;
    note.seen = true;
    note.cachedPackages = 0;
    uint32_t type = 0;
    uintptr_t begin = 0, end = 0;
    if (!g_klOn.load(std::memory_order_acquire)) return next(self, edx, out, filter, unique);
    c_klCalls.Add();
    if (!KlCacheable(out, filter, unique, type) || !ReadListRaw(reinterpret_cast<uintptr_t>(self), begin, end)) {
        c_klPassed.Add();
        return next(self, edx, out, filter, unique);
    }
    c_klCached.Add();
    bool byGame = false;
    int32_t r = EmulateKeyListOrGame(begin, end, out, filter, type, note, [&] {
        byGame = true;
        return next(self, edx, out, filter, unique); // the whole override: its own sort + unique
    });
    if (r && out && !byGame) r = reinterpret_cast<SortUniqueFn>(g_sortUnique)(out);
    return r;
}

// The studied GetKeyList code (0x004B1AE0): its start, and the non-unique loop at +0xF6 that the cache runs itself
constexpr const char* kKeyListStart = "83 EC 10 53 33 C0 38 44 24 20 56 57 89 44 24 0C 0F 84 E0 00 00 00";
constexpr const char* kKeyListLoop =
    "8B 71 30 8B 79 34 3B F7 74 2B 8B 5C 24 24 55 8B 6C 24 24 8D A4 24 00 00 00 00 8B 0E 8B 11 8B 42 30 53 55 FF D0 01 44 24 10 83 C6 08 3B F7 75 EA";
constexpr uintptr_t kKeyListLoopOffset = 0xF6;
constexpr uintptr_t kKeyListInsertCall = 0xA4; // "call 0x006D3810" (the unique path's append)
constexpr const char* kVecInsertBytes = "53 56 8B F1 8B 46 04 3B 46 08 57 74";
constexpr const char* kKeyListDerived = "8B 44 24 0C 8B 54 24 08 56 8B 74 24 08 50 52 56 E8 ?? ?? ?? ?? 85 C0 74 0D 85 F6 74 09 56 E8 ?? ?? ?? ?? 83 C4 04 5E C2 0C 00";
constexpr const char* kSortUniqueBytes = "53 56 8B 74 24 0C 8B 46 04 8B 0E 57 50 51 E8";
constexpr const char* kTypePredicate = "8B 44 24 04 8B 50 08 33 C0 3B 51 04 0F 94 C0 C2 04 00"; // key[2] == this+4
constexpr const char* kRoKeyList = "56 57 6A 01 8B F1 33 FF E8 ?? ?? ?? ?? 8B 8E B0 00 00 00 85 C9 74";          // 0x00734550: key set [this+0xB0] first

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
bool Start(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_started) return true;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::string missing;
    if (!GameAddr::GroupAvailable("ResourceCache", &missing)) return fail(GameAddr::NotAvailable(missing));
    std::string why;
    if (!CheckReadOnly(why)) return fail(why);
    if (!g_table) {
        g_table = static_cast<Entry*>(VirtualAlloc(nullptr, kSlots * sizeof(Entry), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)); // zeroed: every stamp 0 = empty
        if (!g_table) return fail("Could not allocate the table (2.8 MB)");
    }
    if (g_qpcMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    // Nothing stored before this start is used again
    BeginChange();
    EndChange();
    // The list watchers first, so no change can happen unseen once lookups are answered; then the lookup itself
    if (!AcquireWatchers(why)) return fail(why);
    std::string err;
    if (!SlotChain::Install(Site::FindProvider, Layer::ResourceCache, reinterpret_cast<void*>(&Hook_FindProvider), &err)) {
        ReleaseWatchers();
        return fail("Could not hook the resource lookup: " + err);
    }
    g_selfDisabled.store(false);
    g_unhookedLogged.store(false);
    g_on.store(true, std::memory_order_release);
    g_started = true;
    UpdateMisses();
    LOG_INFO(std::format("[ResourceCache] On: FindProvider {:#010x} and the package-list methods hooked through their vtable slots; read-only package class {:#010x}; "
                         "table {} entries{}",
                         SlotChain::GameFunction(Site::FindProvider), g_readOnlyVtable, kSlots, g_negOn.load() ? "; missing files remembered" : ""));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_started) return;
    g_on.store(false, std::memory_order_release); // the layer passes every call through from now on
    SlotChain::Remove(Site::FindProvider, Layer::ResourceCache);
    g_started = false;
    UpdateMisses();
    ReleaseWatchers();
    BeginChange(); // drop everything stored (a later start begins empty)
    EndChange();
    const Stats s = GetStats();
    LOG_INFO(std::format("[ResourceCache] Off ({} lookups, {} answered from the cache ({} as absent, {} with no probe of the counted packages), {} list changes, {} change "
                         "notices, {} unhooked changes, {} mismatches)",
                         s.lookups, s.hits, s.negHits, s.epochHits, s.listChanges, s.changeNotices, s.unhookedChanges, s.mismatches));
}

bool Running() { return g_on.load(std::memory_order_acquire); }

bool StartMisses(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    (void)error;
    g_missesWanted.store(true);
    UpdateMisses();
    return true;
}

void StopMisses() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    g_missesWanted.store(false);
    UpdateMisses();
}

bool MissesRunning() { return g_missesWanted.load(std::memory_order_acquire); } // switched on (the lookup cache itself may be off)
bool EpochsActive() { return g_epochsOn.load(std::memory_order_acquire); }

bool StartKeyLists(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_listStarted) return true;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::string missing;
    if (!GameAddr::GroupAvailable("FileListCache", &missing)) return fail(GameAddr::NotAvailable(missing));
    std::string why;
    if (!CheckReadOnly(why)) return fail(why);
    // The code the cache runs itself or calls, checked on the running build
    const uintptr_t base = GameAddr::Get(GameAddr::Id::ResKeyList), derived = GameAddr::Get(GameAddr::Id::ResKeyListDerived);
    if (!MatchBytes(base, kKeyListStart) || !MatchBytes(base + kKeyListLoopOffset, kKeyListLoop))
        return fail(std::format("The game's file list function {:#010x} differs from the one studied; nothing was changed", base));
    const uintptr_t insert = CallTarget(base + kKeyListInsertCall);
    if (!insert || !MatchBytes(insert, kVecInsertBytes)) return fail(std::format("The key vector insert {:#010x} differs from the one studied; nothing was changed", insert));
    if (!MatchBytes(derived, kKeyListDerived) || CallTarget(derived + 0x10) != base)
        return fail(std::format("The ResourceSystem file list function {:#010x} differs from the one studied; nothing was changed", derived));
    const uintptr_t sortUnique = CallTarget(derived + 0x1E);
    if (!sortUnique || !MatchBytes(sortUnique, kSortUniqueBytes)) return fail(std::format("The key sort {:#010x} differs from the one studied; nothing was changed", sortUnique));
    const uintptr_t filterVt = GameAddr::Get(GameAddr::Id::KeyTypeFilterVtable);
    if (!filterVt || !MatchBytes(ReadU32(filterVt + 4), kTypePredicate))
        return fail(std::format("The key type filter {:#010x} differs from the one studied; nothing was changed", filterVt));
    if (!MatchBytes(ReadU32(g_readOnlyVtable + 0x30), kRoKeyList))
        return fail(std::format("The read-only package's file list {:#010x} differs from the one studied; nothing was changed", ReadU32(g_readOnlyVtable + 0x30)));
    g_vecInsert = insert;
    g_sortUnique = sortUnique;
    g_typeFilterVt = filterVt;
    if (!AcquireWatchers(why)) return fail(why);
    // A new generation: lists stored before a Stop (possibly by a call still inside the emulation then, or unseen list
    // changes while no watcher was installed) can never be served after this restart
    BeginChange();
    EndChange();
    g_klSelfDisabled.store(false);
    g_klOn.store(true, std::memory_order_release); // before the slots can reach the hooks
    std::string err;
    if (!SlotChain::Install(Site::KeyListBase, Layer::ResourceCache, reinterpret_cast<void*>(&Hook_KeyListBase), &err) ||
        !SlotChain::Install(Site::KeyListDerived, Layer::ResourceCache, reinterpret_cast<void*>(&Hook_KeyListDerived), &err)) {
        g_klOn.store(false);
        SlotChain::Remove(Site::KeyListBase, Layer::ResourceCache);
        SlotChain::Remove(Site::KeyListDerived, Layer::ResourceCache);
        ReleaseWatchers();
        return fail("Could not hook the file list: " + err);
    }
    g_listStarted = true;
    LOG_INFO(std::format("[FileListCache] On: GetKeyList {:#010x} / {:#010x} answered for the key type filter {:#010x}; read-only packages' lists kept per key type "
                         "(vector insert {:#010x}, sort {:#010x})",
                         base, derived, filterVt, insert, sortUnique));
    return true;
}

void StopKeyLists() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_listStarted) return;
    g_klOn.store(false, std::memory_order_release); // the layers pass every call through from now on
    SlotChain::Remove(Site::KeyListBase, Layer::ResourceCache);
    SlotChain::Remove(Site::KeyListDerived, Layer::ResourceCache);
    ReleaseWatchers();
    {
        ApexUtil::SrwExclusive srw(g_klLock);
        g_kl.clear();
        g_klKeyCount = 0;
        g_klGen = 0;
    }
    g_listStarted = false;
    LOG_INFO(std::format("[FileListCache] Off ({} calls, {} answered by the cache, {} package lists from memory, {} packages asked, {} mismatches)", c_klCalls.Get(), c_klCached.Get(),
                         c_klFromMem.Get(), c_klAsked.Get(), c_klMismatches.Get()));
}

bool KeyListsRunning() { return g_klOn.load(std::memory_order_acquire); }

void SetVerifyEvery(int n) { g_verifyEvery.store(n < 0 ? 0 : n); }

void VerifyAllFor(double seconds) { g_verifyAllUntil.store(GetTickCount64() + static_cast<uint64_t>(seconds * 1000.0)); }

LookupNote TakeLookupNote() {
    LookupNote n = t_note;
    t_note = LookupNote{};
    return n;
}

KeyListNote TakeKeyListNote() {
    KeyListNote n = t_klNote;
    t_klNote = KeyListNote{};
    return n;
}

Stats GetStats() {
    Stats s;
    s.lookups = c_lookups.Get();
    s.hits = c_hits.Get();
    s.misses = c_misses.Get();
    s.bypassed = c_bypassed.Get();
    s.rejected = c_rejected.Get();
    s.notFound = c_notFound.Get();
    s.inserted = c_inserted.Get();
    s.notCached = c_notCached.Get();
    s.probesOnHits = c_probes.Get();
    s.negHits = c_negHits.Get();
    s.negInserted = c_negInserted.Get();
    s.negUnreliable = c_negUnreliable.Get();
    s.epochHits = c_epochHits.Get();
    s.epochRefreshes = c_epochRefreshes.Get();
    s.epochWrites = c_epochWrites.Get();
    s.listChanges = c_listChanges.Get();
    s.changeNotices = c_notices.Get();
    s.unhookedChanges = c_unhooked.Get();
    s.fullClears = c_fullClears.Get();
    s.verified = c_verified.Get();
    s.mismatches = c_mismatches.Get();
    s.inconclusive = c_inconclusive.Get();
    s.hitMs = static_cast<double>(g_hitTicks.load()) * g_qpcMs;
    s.missMs = static_cast<double>(g_missTicks.load()) * g_qpcMs;
    s.capacity = kSlots;
    s.readOnlyClassOk = g_readOnlyOk;
    s.selfDisabled = g_selfDisabled.load();
    {
        ApexUtil::SrwShared srw(g_lock);
        s.entries = g_stampGen == g_gen.load() ? g_count : 0;
        uint32_t newest = 0;
        for (const Snapshot& snap : g_snap)
            if (snap.gen && snap.gen >= newest) {
                newest = snap.gen;
                s.listSize = static_cast<int>(snap.count);
                s.writableProviders = snap.writable;
                s.tracedProviders = snap.traced;
            }
    }
    s.epochClasses = EpochsActive() ? EpochClassText() : std::string("off");
    s.klCalls = c_klCalls.Get();
    s.klCached = c_klCached.Get();
    s.klPassed = c_klPassed.Get();
    s.klPackagesFromMemory = c_klFromMem.Get();
    s.klPackagesAsked = c_klAsked.Get();
    s.klStored = c_klStored.Get();
    s.klNotStored = c_klNotStored.Get();
    s.klVerified = c_klVerified.Get();
    s.klMismatches = c_klMismatches.Get();
    s.klInconclusive = c_klInconclusive.Get();
    s.klSelfDisabled = g_klSelfDisabled.load();
    {
        ApexUtil::SrwShared srw(g_klLock);
        s.klEntries = static_cast<uint32_t>(g_kl.size());
        s.klKeys = static_cast<uint32_t>(g_klKeyCount);
    }
    {
        std::lock_guard<std::mutex> lock(g_mismatchLock);
        s.lastMismatch = g_lastMismatch;
        s.klLastMismatch = g_klLastMismatch;
    }
    return s;
}

std::string StatusText() {
    if (!Running()) return "Off";
    const Stats s = GetStats();
    if (s.selfDisabled) return "Turned itself off after a check found a different answer (see ApexRadiance_LOG.txt)";
    if (!s.lookups) return "On (no lookups yet)";
    const double pct = 100.0 * static_cast<double>(s.hits) / static_cast<double>(s.lookups);
    const double asked = s.hits ? static_cast<double>(s.probesOnHits) / static_cast<double>(s.hits) : 0.0;
    return std::format("On: {:.0f}% of lookups answered from memory ({:.1f} packages asked instead of up to {})", pct, asked,
                       s.listSize >= 0 ? std::to_string(s.listSize) : std::string("all"));
}

std::string MissesStatusText() {
    if (!g_missesWanted.load()) return "Off";
    if (!Running()) return "Waiting: needs Faster game file lookups";
    const uint64_t neg = c_negHits.Get();
    return std::format("On: {} missing files answered from memory; packages whose changes are counted: {}", neg, EpochsActive() ? EpochClassText() : std::string("none"));
}

std::string KeyListStatusText() {
    if (!KeyListsRunning()) return "Off";
    if (g_klSelfDisabled.load()) return "Turned itself off after a check found a different list (see ApexRadiance_LOG.txt)";
    const uint32_t calls = c_klCached.Get();
    if (!calls) return "On (no file lists asked yet)";
    const uint32_t mem = c_klFromMem.Get(), asked = c_klAsked.Get();
    return std::format("On: {} file lists, {:.0f}% of the packages answered from memory", calls, mem + asked ? 100.0 * mem / (mem + asked) : 0.0);
}

std::string ReportLine() {
    const Stats s = GetStats();
    std::string t = std::format("lookups {}, from memory {} (absent {}, no probe of counted packages {}), game lookups {} (not found {}), re-check failed {}, bypassed {}, "
                                "stored {} + absent {} (not stored {}, unreliable read-only package {}), list changes {}, notices {}, missed changes {}, packages {} (not "
                                "read-only {}, counted {}), counted classes: {}, counted writes {}, sums refreshed {}, checks {} / {} different",
                                s.lookups, s.hits, s.negHits, s.epochHits, s.misses, s.notFound, s.rejected, s.bypassed, s.inserted, s.negInserted, s.notCached, s.negUnreliable,
                                s.listChanges, s.changeNotices, s.unhookedChanges, s.listSize, s.writableProviders, s.tracedProviders, s.epochClasses, s.epochWrites,
                                s.epochRefreshes, s.verified, s.mismatches);
    if (KeyListsRunning() || s.klCalls)
        t += std::format("; file lists: calls {}, cached {} (passed {}), package lists from memory {}, asked {}, kept {} ({} keys), not kept {}, checks {} / {} different",
                         s.klCalls, s.klCached, s.klPassed, s.klPackagesFromMemory, s.klPackagesAsked, s.klEntries, s.klKeys, s.klNotStored, s.klVerified, s.klMismatches);
    return t;
}

void RenderDeveloperUI() {
    if (kPublicBuild) return;
    if (!ImGui::GetCurrentContext()) return;
    const Stats s = GetStats();
    // per-second rates from the previous call (render thread only)
    static Stats prev;
    static uint64_t prevTick = 0;
    static double rateHits = 0.0, rateLookups = 0.0, rateSavedMs = 0.0, rateHitMs = 0.0, rateMissMs = 0.0;
    const uint64_t now = GetTickCount64();
    if (now - prevTick >= 1000) {
        const double dt = prevTick ? static_cast<double>(now - prevTick) / 1000.0 : 0.0;
        if (dt > 0.0) {
            const double hits = static_cast<double>(s.hits - prev.hits), lookups = static_cast<double>(s.lookups - prev.lookups);
            const double hitMs = s.hitMs - prev.hitMs, missMs = s.missMs - prev.missMs;
            const double avgMiss = s.misses ? s.missMs / static_cast<double>(s.misses) : 0.0; // long-run average of the game's lookup
            rateHits = hits / dt;
            rateLookups = lookups / dt;
            rateHitMs = hitMs / dt;
            rateMissMs = missMs / dt;
            rateSavedMs = (hits * avgMiss - hitMs) / dt; // estimate: each answer from memory would have cost an average game lookup
        }
        prev = s;
        prevTick = now;
    }
    int every = g_verifyEvery.load();
    if (ApexUi::DiagnosticIntRow("Check 1 answer in N against the game##RcVerify", &every, 0, 1024)) SetVerifyEvery(every);
    if (ApexUi::BeginControlRow("Validation run", "Adds comparison work while the test is active", ApexUi::ButtonWidth("Check every answer for 10 s##RcVerifyAll", true))) {
        if (ApexUi::IconTextButton("Check every answer for 10 s##RcVerifyAll", ApexUi::IconId::Scan)) VerifyAllFor(10.0);
        ApexUi::EndControlRow();
    }
    const bool checkingAll = GetTickCount64() < g_verifyAllUntil.load();
    ImGui::TextWrapped("Lookup checks: %llu equal, %llu different, %llu inconclusive (the list changed during the check)%s", static_cast<unsigned long long>(s.verified),
                        static_cast<unsigned long long>(s.mismatches), static_cast<unsigned long long>(s.inconclusive), checkingAll ? "  [checking every answer]" : "");
    ImGui::TextWrapped("File list checks: %llu equal, %llu different, %llu inconclusive", static_cast<unsigned long long>(s.klVerified), static_cast<unsigned long long>(s.klMismatches),
                        static_cast<unsigned long long>(s.klInconclusive));
    if (!s.lastMismatch.empty()) ImGui::TextColored(ImVec4(0.91f, 0.44f, 0.42f, 1.0f), "Last difference: %s", s.lastMismatch.c_str());
    if (!s.klLastMismatch.empty()) ImGui::TextColored(ImVec4(0.91f, 0.44f, 0.42f, 1.0f), "Last file list difference: %s", s.klLastMismatch.c_str());
    if (ApexUi::BeginAdvanced("LiveCounters", "Live counters")) {
    ImGui::TextWrapped("%s", ("Resource lookup cache: " + StatusText()).c_str());
    ImGui::TextWrapped("Lookups %llu, answered from memory %llu, game lookups %llu (not found %llu), re-check failed %llu, passed through during list changes %llu",
                        static_cast<unsigned long long>(s.lookups), static_cast<unsigned long long>(s.hits), static_cast<unsigned long long>(s.misses),
                        static_cast<unsigned long long>(s.notFound), static_cast<unsigned long long>(s.rejected), static_cast<unsigned long long>(s.bypassed));
    ImGui::TextWrapped("Stored %llu (not stored %llu), entries %u / %u, table restarts when full %llu", static_cast<unsigned long long>(s.inserted),
                        static_cast<unsigned long long>(s.notCached), s.entries, s.capacity, static_cast<unsigned long long>(s.fullClears));
    ImGui::TextWrapped("Package list: %d packages, %d not of the read-only class (asked on every answer unless counted: %d)%s", s.listSize, s.writableProviders, s.tracedProviders,
                        s.readOnlyClassOk ? "" : "; read-only class NOT recognised");
    ImGui::TextWrapped("Invalidations: list changes %llu, change notices %llu, changes the hooks missed %llu", static_cast<unsigned long long>(s.listChanges),
                        static_cast<unsigned long long>(s.changeNotices), static_cast<unsigned long long>(s.unhookedChanges));
    ImGui::TextWrapped("Missing files (%s): answered from memory %llu, remembered %llu, not remembered (a read-only package could not answer for sure) %llu",
                        MissesRunning() ? "on" : "off", static_cast<unsigned long long>(s.negHits), static_cast<unsigned long long>(s.negInserted),
                        static_cast<unsigned long long>(s.negUnreliable));
    ImGui::TextWrapped("Write epochs: counted classes %s; answers with no probe of them %llu, sums refreshed after a write %llu, writes counted %llu", s.epochClasses.c_str(),
                        static_cast<unsigned long long>(s.epochHits), static_cast<unsigned long long>(s.epochRefreshes), static_cast<unsigned long long>(s.epochWrites));
    ImGui::TextWrapped("Per second: %.0f lookups, %.0f from memory; time in answers %.2f ms, in game lookups %.2f ms; saved about %.2f ms", rateLookups, rateHits, rateHitMs,
                        rateMissMs, rateSavedMs);
    ImGui::TextWrapped("Average: game lookup %.1f us, answer from memory %.2f us (%.1f packages asked)", s.misses ? 1000.0 * s.missMs / static_cast<double>(s.misses) : 0.0,
                        s.hits ? 1000.0 * s.hitMs / static_cast<double>(s.hits) : 0.0, s.hits ? static_cast<double>(s.probesOnHits) / static_cast<double>(s.hits) : 0.0);
    ImGui::TextWrapped("%s", ("File list cache: " + KeyListStatusText()).c_str());
    ImGui::TextWrapped("Calls %llu (cacheable %llu, passed to the game %llu); package lists from memory %llu, packages asked %llu; kept %u lists (%u keys), not kept %llu",
                        static_cast<unsigned long long>(s.klCalls), static_cast<unsigned long long>(s.klCached), static_cast<unsigned long long>(s.klPassed),
                        static_cast<unsigned long long>(s.klPackagesFromMemory), static_cast<unsigned long long>(s.klPackagesAsked), s.klEntries, s.klKeys,
                        static_cast<unsigned long long>(s.klNotStored));
        ApexUi::EndAdvanced();
    }
}


void SaveDeveloperState(toml::table& out) {
    out.insert("verify_every", g_verifyEvery.load());
}
void LoadDeveloperState(const toml::table& t) {
    if (auto n = t["verify_every"].value<int64_t>()) { const int v = static_cast<int>(*n); g_verifyEvery.store(std::clamp(v, 0, 1024)); }
}
} // namespace ResourceCache
