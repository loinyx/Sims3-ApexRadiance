#include "ui/widgets.h"
#include "developer_settings.h"
// Scene node budget (see scene_budget.h and docs/features/performance.md, section "How it works: Spread New Objects Over
// Frames (C6)").
//
// ---- The game side (Steam 1.67.2, TS3W.exe; research\engine_map\full.asm; addresses through framework/game_addresses.h) ----
// VERIFIED = read in the disassembly; INFERRED = deduction, not proven.
//   Pending holder ([scene+8], 0x68 bytes, ctor 0x006E4530): +0x04..+0x08 the slot array of its nodes, +0x18 counter
//   (nodes processed by the last drain; not a list size: nothing else counts the list), +0x20 / +0x24 the sentinel
//   {next, prev} of a circular intrusive list of pending scene nodes, +0x2C the spatial tree. VERIFIED.
//   Scene node (base ctor 0x006FD710, vtable 0x00FF9D00, refcounted: +4 count with lock xadd, vfunc +0 AddRef, +4 Release,
//   +8 deleting destructor): +0x18 / +0x1C its pending link {next, prev}: 0 = not queued (a drain processed it), self-loop =
//   not queued (constructor, RemoveNode), else linked in a list. +0x30 its owner (the holder). VERIFIED.
//   Queueing (no lock anywhere):
//     0x006FAC70 MarkDirty, 0x006FCA20 (children), 0x006FCB10 (LOD band), 0x006FD9F0 SetOwner: `if (link.next == 0) { link =
//                self-loop; if (owner) 0x006E42E0(owner, node); }` (push_back at the tail). Every push needs owner != 0. VERIFIED.
//     0x006E6480 AddNode(node, group): returns at once when node->owner != 0; else takes a slot, AddRef, SetOwner(holder),
//                then at 0x006E64EF pushes the link again when it is non-zero WITHOUT unlinking it. VERIFIED.
//   Unlinking / freeing:
//     0x006E4920 RemoveNode: only when node->owner == this; clears the slot, unlinks the link if non-zero and self-loops it
//                (0x006E4984), out of the spatial tree, SetOwner(0,0,0), Release. VERIFIED.
//     0x006E4DE0 holder teardown (only caller 0x006E97F0, which frees the holder right after): destroys the spatial tree,
//                then SetOwner(0,0,0) + Release on every slot WITHOUT unlinking; the list dies with the holder, and a node
//                that survives keeps a stale link into freed memory (AddNode later overwrites it). VERIFIED.
//     0x006FD930 node base destructor: detaches its children (0x006FC860), leaves its spatial cell (0x00706200), owner = 0,
//                unlinks +0x20 (its parent's child list), never touches +0x18. The base vtable 0x00FF9D00 is written only by
//                the base ctor and this destructor, so every derived destructor ends in it (23 call / tail-jmp sites). It
//                ends by storing vtable 0x00FA1B78 (the destroyed-node vtable the development checks look for). VERIFIED.
//   So in the game a node linked in a LIVE holder's list is always owned by it and held by its slot (AddRef in AddNode,
//   Release only by RemoveNode after the unlink, or by the teardown whose list dies too): it is never freed while linked,
//   whatever the time it waits (the guarantee is structural, not "drained within the frame"). VERIFIED for the direct paths;
//   INFERRED that no other code calls SetOwner(0) (vfunc +0x1C) on a linked node or releases the holder's reference (the
//   indirect vfunc +0x1C calls with three zero arguments were searched: RemoveNode, the teardown and one non-scene class).
//   0x006E4130 the drain, thiscall(holder), ret (0xD1 bytes, checked byte for byte at Start):
//     splices the whole list into a local sentinel on its stack and empties the holder's list; [this+0x18] = 0; then
//     while the local list is not empty: l = local.prev (the most recently queued first; re-read every iteration); unlink it;
//     l->prev = l->next = 0; node = l - 0x18; node->vfunc+0x48(); bounds = 0x006FB4B0(node, &aligned32); 0x006FAD70(node,
//     bounds) (a no-op when owner == 0: its only caller is the drain); [this+0x18] += 1. Nodes queued during the loop go to
//     the holder's (now empty) list: processed by the next drain. VERIFIED.
//   Callers (all direct): 0x006EBC49 Scene::BeginFrame (every frame), 0x006DF9B5, 0x006EDBC7 (a scene query: drains first
//   so its answer is current), 0x006EF07D, 0x006F226B (render-to-texture, when asked), 0x006F3CF1. Only the BeginFrame CALL
//   is redirected; the other five still drain everything, including what this feature left queued, exactly as they drain
//   what the game queued after BeginFrame (the list is always well formed when Apex returns). VERIFIED.
//   Threads: the list has no lock in any of the functions above, so the game must touch it from one thread (the render
//   thread, which runs BeginFrame). INFERRED. Release is atomic (lock xadd), so a node's destructor may run on any thread.
//
// ---- The copy with a budget (BudgetedDrain) ----
//   Exactly the loop above (same list operations in the same order, same calls, same counter), plus a stop test before
//   each node: at least kMinNodes, then stop at nodesPerFrame nodes or msPerFrame ms. The nodes not reached stay linked,
//   in their order, and the remainder is spliced back at the TAIL of the holder's list (where the drain takes from), so they
//   are processed first next frame and stay queued exactly as the game would have them: RemoveNode unlinks them, MarkDirty
//   sees them as queued, any other drain processes them.
//   Draw state (issue #37): two kinds of node are drawn every frame from state that only their update refreshes, so the
//   game relies on BeginFrame draining them before the passes draw. The Swarm effect renderer drawable (update 0x00722190
//   rebuilds its bucket draw lists from the entry array; a remove 0x0071FF10 blanks an entry and leaves the lists
//   pointing at it, then the draw 0x0071D440 writes through the blanked entry: access violation at 0x0071D56E) and the
//   SceneModelFur (an LOD unload 0x006DBCE0 empties the model handle's current LOD slot and MarkDirty queues the node;
//   its update 0x006F7B40 refills the slot or clears the draw flags; the fur draw 0x006FA0B0 reads the slot without a
//   NULL check: access violation at 0x006C93D0). VERIFIED in the disassembly; that these two stale-state crashes come from
//   the deferral is INFERRED from crash logs (the Swarm one: last gather by this copy, removes after it). Past the budget
//   the copy keeps walking the rest and still updates every node of these two classes, in the game's order; the others are
//   moved, in order, to a second local list and go back as before. The two are told by their vtable (0x00FFC248, 0x00FF9B50),
//   not by a slot's content: another module that swaps the update or draw slot for its own stub (a profiler, a crash
//   logger) leaves the vtable as it is, and a slot compare would then miss every such node. The Swarm drawable is queued
//   again by its own update every frame and fur models are few, so the budget is unchanged for ordinary nodes.
//   The rule: camera still -> the game's drain; camera moving -> the budgeted copy; once a node waited maxDeferMs the budget
//   grows every frame (x2, x4 ... x64, doubling every kGrowStepMs) instead of the whole backlog at once (05/10: the forced full
//   drain was a single long frame in the middle of a camera move); a node waited kHardFactor x maxDeferMs -> the game's drain
//   (so the backlog cannot grow without end while the camera keeps moving).
//
// ---- The node lifetime guard (the registry and three entry hooks) ----
//   Every node the budgeted copy leaves queued is recorded (link -> holder). Invariant kept for recorded nodes: when the
//   link is neither 0 nor a self-loop it is in a LIVE list (the holder's, or a drain's local list on the stack while that
//   drain runs). It holds because a recorded node can only be queued into its owner's list, the owner changes only through
//   AddNode (hook below forgets the record) or the teardown (hook below forgets the holder's records), and drains leave
//   links at 0. With it:
//     - Hook_NodeDtor (0x006FD930, any thread): a recorded node that is still linked is unlinked (neighbours checked to
//       point back at it) and self-looped before the game destroys it; the record is dropped. Expected never to unlink.
//     - Hook_AddNode (0x006E6480): a recorded node with owner 0 that is still linked is unlinked first (the game would push
//       it again over a live link and corrupt the list); the record is dropped. Expected never to unlink.
//     - Hook_HolderTeardown (0x006E4DE0): the holder's records are dropped before its nodes are released (their links then
//       point into a list that dies: the game leaves them, so must Apex).
//   Records are replaced after every budgeted drain (the nodes left now) and dropped after every game drain through the
//   hook; they are kept while a drain runs, so a recorded node destroyed during a drain is unlinked from that drain's local
//   list, which the drain re-reads every iteration. The registry is guarded by an SRW lock never held across game calls.
//   Development build checks (on failure: logged once, budgeting stops until the game restarts, the game's drain runs):
//     before each node of the budgeted copy: its neighbours point back at it and its vtable is inside TS3W.exe .rdata, is not
//     the destroyed-node vtable and its +0x48 slot is inside .text; before each budgeted drain: the same for every recorded
//     node still linked. A node found destroyed (destroyed-node vtable) but still well linked is taken out by writing only
//     its neighbours, never called, counted as "repaired" and logged once, without stopping.
//   After Stop the nodes left are processed by the next BeginFrame (the game's drain): the same exposure as any node the
//   game queues after BeginFrame.
//
// Part of Apex Radiance. Credits: @loinyx

#include "scene_budget.h"
#include "apex_log.h"
#include "hook_guard.h"
#include "build_flavor.h"
#include "call_chain.h"
#include "entry_chain.h"
#include "game_addresses.h"
#include "lot_lighting_motion.h"
#include "memory_patch.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace SceneBudget {
namespace {

using CallChain::Layer;
using CallChain::Site;

// The drain as disassembled (Steam 1.67.2), wildcards only on the two CALL rel32s (0x006FB4B0 and 0x006FAD70)
constexpr const char* kDrainBody =
    "55 8B EC 83 E4 F0 83 EC 34 53 56 57 8B F9 8B 77 20 8B 5F 24 8D 47 20 3B F0 8D 4C 24 18 8B D1 89 "
    "74 24 18 89 5C 24 1C 89 10 89 48 04 75 0A 89 4C 24 1C 89 54 24 18 EB 0D 89 0B 8B 4C 24 18 89 51 "
    "04 8B 4C 24 1C 8D 54 24 18 39 10 75 07 89 40 04 89 00 EB 0E 8B 48 04 89 01 8B 10 89 42 04 8B 4C "
    "24 1C 8D 44 24 18 3B C8 C7 47 18 00 00 00 00 74 59 BB 01 00 00 00 8B 51 04 8B C1 83 C1 04 8D 74 "
    "24 18 89 32 8B 54 24 1C 8B 52 04 89 54 24 1C 8D 70 E8 C7 01 00 00 00 00 C7 00 00 00 00 00 8B 06 "
    "8B 50 48 8B CE FF D2 8D 44 24 20 50 8B CE E8 ?? ?? ?? ?? 50 8B CE E8 ?? ?? ?? ?? 8B 4C 24 1C 01 "
    "5F 18 8D 54 24 18 3B CA 75 AC 5F 5E 5B 8B E5 5D C3";
// What the three lifetime hooks rely on (Steam 1.67.2), checked at Start:
//   AddNode: "push esi; mov esi,[esp+8]; cmp dword ptr [esi+30h],0; push edi; mov edi,ecx; jne (return)" (owner test)
constexpr const char* kAddNodeHead = "56 8B 74 24 08 83 7E 30 00 57 8B F9 0F 85";
//   the destructor: stores the base vtable, and at +0x8D "mov dword ptr [edi], <destroyed-node vtable>"
constexpr const char* kDtorHead = "55 8B EC 83 E4 F0 81 EC 94 00 00 00 53 56 57 8B F9 8D 9F 94 01 00 00 C7 07";
constexpr uint32_t kDtorDeadVtableStore = 0x8D; // C7 07 imm32
//   the teardown: "push ebx; push ebp; push esi; push edi; mov edi,ecx; mov ecx,[edi+2Ch]" (the holder's spatial tree)
constexpr const char* kTeardownHead = "53 55 56 57 8B F9 8B 4F 2C";

constexpr uint32_t kCounterOff = 0x18; // holder: nodes processed by the last drain
constexpr uint32_t kListOff = 0x20;    // holder: pending list sentinel {next, prev}
constexpr uint32_t kLinkOff = 0x18;    // node: its pending link
constexpr uint32_t kOwnerOff = 0x30;   // node: its owner (the holder), 0 = in no scene
constexpr uint32_t kUpdateSlot = 0x48; // node vfunc: per-node update
constexpr uint32_t kMinNodes = 8;      // processed every frame whatever the budget (progress)
constexpr uint32_t kCountCap = 1u << 20;

struct Link {
    Link* next;
    Link* prev;
};

using FnDrain = void(__fastcall*)(void* holder, void* edx);
using FnThis0 = void(__fastcall*)(void* self, void* edx);
using FnAddNode = void(__fastcall*)(void* holder, void* edx, void* node, int group);
using FnUpdate = void(__fastcall*)(void* node, void* edx);
using FnBounds = void*(__fastcall*)(void* node, void* edx, void* out);
using FnSpatial = void(__fastcall*)(void* node, void* edx, void* bounds);

std::mutex g_ctrl;
bool g_started = false;
std::atomic<bool> g_on{false};
std::atomic<bool> g_stopped{false}; // a safety check failed: the game's drain from then on (sticky until the game restarts)
uintptr_t g_drainFn = 0;
uintptr_t g_swarmVtable = 0; // 0x00FFC248: nodes whose update must not wait (see "Draw state" above)
uintptr_t g_furVtable = 0;   // 0x00FF9B50: same
FnBounds g_bounds = nullptr;   // 0x006FB4B0
FnSpatial g_spatial = nullptr; // 0x006FAD70
std::atomic<DWORD> g_drainThread{0};

std::atomic<int> g_nodesPerFrame{512};
std::atomic<float> g_msPerFrame{2.0f};
std::atomic<int> g_maxDeferMs{500};

// TS3W.exe sections and the destroyed-node vtable, for the development checks (set at Start)
uintptr_t g_textBegin = 0, g_textEnd = 0, g_rdataBegin = 0, g_rdataEnd = 0, g_deadVtable = 0;

// ---- the registry of nodes left queued (link -> holder); guarded by g_regLock, never held across a game call ----
SRWLOCK g_regLock = SRWLOCK_INIT;
std::vector<std::pair<Link*, void*>> g_deferred; // sorted by link: rebuilt once per budgeted drain without allocating
std::atomic<uint32_t> g_deferredCount{0}; // mirror of g_deferred.size() for the lock-free fast path of the hooks

// The record of l, or g_deferred.end() (caller holds the lock)
std::vector<std::pair<Link*, void*>>::iterator FindDeferred(Link* l) {
    auto it = std::lower_bound(g_deferred.begin(), g_deferred.end(), l, [](const std::pair<Link*, void*>& e, Link* k) { return e.first < k; });
    return it != g_deferred.end() && it->first == l ? it : g_deferred.end();
}

struct RegGuard {
    RegGuard() { AcquireSRWLockExclusive(&g_regLock); }
    ~RegGuard() { ReleaseSRWLockExclusive(&g_regLock); }
    RegGuard(const RegGuard&) = delete;
    RegGuard& operator=(const RegGuard&) = delete;
};

// render thread only: per pending holder (one per scene; a few at most), since when nodes we left have been waiting
struct Waiting {
    void* holder;
    uint64_t since;    // GetTickCount64 of the first budgeted frame that left nodes (0 = nothing of ours waiting)
    uint64_t lastSeen; // GetTickCount64 of this holder's previous drain call
};
constexpr int kHolders = 8;
constexpr uint64_t kEvictAfterMs = 1000; // a slot is reused only when its holder has not drained for this long
constexpr uint64_t kGrowStepMs = 50;     // past maxDeferMs the budget doubles this often
constexpr uint64_t kHardFactor = 3;      // past this many times maxDeferMs the game's drain runs
constexpr uint64_t kSteadyMs = 100;      // only holders drained every frame (the world scene) are budgeted
Waiting g_waiting[kHolders] = {};
double g_qpcMs = 0.0;

// The holder's slot, or nullptr when every slot belongs to a holder still in use (the caller then drains everything,
// so a holder can never lose its "waiting since" and escape the forced full drain). prevSeen = its previous call (0 = new).
Waiting* WaitingOf(void* holder, uint64_t now, uint64_t& prevSeen) {
    for (Waiting& w : g_waiting)
        if (w.holder == holder) {
            prevSeen = w.lastSeen;
            w.lastSeen = now;
            return &w;
        }
    prevSeen = 0;
    for (Waiting& w : g_waiting)
        if (!w.holder || now - w.lastSeen >= kEvictAfterMs) {
            w = {holder, 0, now};
            return &w;
        }
    return nullptr;
}
thread_local DrainNote t_note;

struct Counter {
    std::atomic<uint64_t> v{0};
    void Add(uint64_t n = 1) { v.fetch_add(n, std::memory_order_relaxed); }
    uint64_t Get() const { return v.load(std::memory_order_relaxed); }
};
Counter c_grown;
Counter c_calls, c_fullStill, c_fullForced, c_budgeted, c_framesLeft, c_nodesBudgeted, c_nodesLeft;
Counter c_drawState; // nodes updated past the budget because their draw this frame needs it
Counter c_dtorUnlinked, c_addUnlinked, c_teardownDropped, c_otherThread, c_foreignOwner, c_repaired;
std::atomic<uint32_t> g_maxLeft{0}, g_lastDone{0}, g_lastLeft{0};
std::atomic<float> g_lastMs{0.0f};
std::atomic<bool> g_loggedDtor{false}, g_loggedAdd{false}, g_loggedThread{false}, g_loggedRepair{false};

int64_t Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

bool Queued(const Link* l) { return l->next && l->next != l; }

// ---- guarded reads (no C++ objects in these functions: SEH) ----

// True when both neighbours of l point back at it (only reads; false if any of it is unreadable)
bool LinkConsistent(const Link* l) {
    __try {
        const Link* n = l->next;
        const Link* p = l->prev;
        return n && p && n->prev == l && p->next == l;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Development check of a node before the game's code is called on it: its vtable is inside TS3W.exe .rdata, is not the
// destroyed-node vtable, and its update slot points into .text. *vtableOut = the vtable read (0 if unreadable).
bool NodeLooksAlive(const uint8_t* node, uintptr_t* vtableOut) {
    *vtableOut = 0;
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(node);
        *vtableOut = vt;
        if (vt < g_rdataBegin || vt + kUpdateSlot + 4 > g_rdataEnd || vt == g_deadVtable) return false;
        const uintptr_t fn = *reinterpret_cast<const uintptr_t*>(vt + kUpdateSlot);
        return fn >= g_textBegin && fn < g_textEnd;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Takes l out of the list that holds it and self-loops it (as RemoveNode 0x006E4984 does). Caller: LinkConsistent(l), and
// the node is alive (its own memory is written).
void Unlink(Link* l) {
    l->prev->next = l->next;
    l->next->prev = l->prev;
    l->next = l;
    l->prev = l;
}

// The same for a node already destroyed: only its neighbours are written, never its (freed) memory
void UnlinkDead(const Link* l) {
    l->prev->next = l->next;
    l->next->prev = l->prev;
}

// Registry: forgets the holder's records (caller holds the lock). Returns how many.
uint32_t DropHolderLocked(void* holder) {
    const auto end = std::remove_if(g_deferred.begin(), g_deferred.end(), [holder](const std::pair<Link*, void*>& e) { return e.second == holder; });
    const uint32_t n = static_cast<uint32_t>(g_deferred.end() - end);
    g_deferred.erase(end, g_deferred.end());
    g_deferredCount.store(static_cast<uint32_t>(g_deferred.size()), std::memory_order_release);
    return n;
}

void DropHolder(void* holder) {
    if (!g_deferredCount.load(std::memory_order_acquire)) return;
    RegGuard lock;
    DropHolderLocked(holder);
}

// Stops budgeting for the rest of the session after a failed safety check (logged once). why() builds the reason, only for
// the first failure; these run inside the game's scene code, so a failure to build or log it stays here (07/10, players'
// Runtime Error).
template <class Why> void StopForCheck(Why&& why) noexcept {
    if (g_stopped.exchange(true)) return;
    try {
        LOG_ERROR("[SceneBudget] Safety check failed: " + std::string(why()) + ". Spreading new objects over frames is stopped until the game restarts; the game's "
              "own drain runs from now on.");
    } catch (...) {
    }
}

// ---- the three lifetime hooks ----

// 0x006FD930 (any thread): a recorded node still linked is unlinked before the game destroys it
void __fastcall Hook_NodeDtor(void* node, void* edx) {
    const FnThis0 next = reinterpret_cast<FnThis0>(EntryChain::Next(EntryChain::Site::SceneNodeDtor, EntryChain::Layer::SceneBudget));
    if (node && g_deferredCount.load(std::memory_order_acquire)) {
        Link* const l = reinterpret_cast<Link*>(static_cast<uint8_t*>(node) + kLinkOff);
        bool found = false, unlinked = false, bad = false;
        {
            RegGuard lock;
            auto it = FindDeferred(l);
            if (it != g_deferred.end()) {
                found = true;
                g_deferred.erase(it);
                g_deferredCount.store(static_cast<uint32_t>(g_deferred.size()), std::memory_order_release);
                if (Queued(l)) {
                    if (LinkConsistent(l)) {
                        Unlink(l);
                        unlinked = true;
                    } else {
                        bad = true;
                    }
                }
            }
        }
        if (found) {
            const DWORD drainThread = g_drainThread.load(std::memory_order_relaxed);
            if (drainThread && GetCurrentThreadId() != drainThread) {
                c_otherThread.Add();
                if (!g_loggedThread.exchange(true))
                    HookGuard::Try("SceneBudget note", [&] {
                        LOG_WARNING(std::format("[SceneBudget] A node this feature left queued was destroyed on thread {} (the drain runs on thread {}); "
                                            "counted, not an error by itself", GetCurrentThreadId(), drainThread));
                    });
            }
        }
        if (unlinked) {
            c_dtorUnlinked.Add();
            if (!g_loggedDtor.exchange(true))
                HookGuard::Try("SceneBudget note", [&] {
                    LOG_WARNING(std::format("[SceneBudget] Node {:#010x} was destroyed while still queued; unlinked before its destructor (the game's "
                                        "code was read not to do this: please report)", reinterpret_cast<uintptr_t>(node)));
                });
        }
        if (bad) StopForCheck([&] { return std::format("node {:#010x} is being destroyed while queued, and its list neighbours do not point back at it", reinterpret_cast<uintptr_t>(node)); });
    }
    next(node, edx);
}

// 0x006E6480: AddNode pushes the link again when the node has no owner; a recorded node that is still linked is unlinked
// first (the game would overwrite a live link). A node that has an owner makes AddNode return at once: left alone.
void __fastcall Hook_AddNode(void* holder, void* edx, void* node, int group) {
    const FnAddNode next = reinterpret_cast<FnAddNode>(EntryChain::Next(EntryChain::Site::SceneAddNode, EntryChain::Layer::SceneBudget));
    if (node && g_deferredCount.load(std::memory_order_acquire) && !*reinterpret_cast<void**>(static_cast<uint8_t*>(node) + kOwnerOff)) {
        Link* const l = reinterpret_cast<Link*>(static_cast<uint8_t*>(node) + kLinkOff);
        bool unlinked = false, bad = false;
        {
            RegGuard lock;
            auto it = FindDeferred(l);
            if (it != g_deferred.end()) {
                g_deferred.erase(it);
                g_deferredCount.store(static_cast<uint32_t>(g_deferred.size()), std::memory_order_release);
                if (Queued(l)) {
                    if (LinkConsistent(l)) {
                        Unlink(l);
                        unlinked = true;
                    } else {
                        bad = true;
                    }
                }
            }
        }
        if (unlinked) {
            c_addUnlinked.Add();
            if (!g_loggedAdd.exchange(true))
                HookGuard::Try("SceneBudget note", [&] {
                    LOG_WARNING(std::format("[SceneBudget] Node {:#010x} was added to a scene while still queued without an owner; unlinked first (the "
                                        "game's code was read not to do this: please report)", reinterpret_cast<uintptr_t>(node)));
                });
        }
        if (bad) StopForCheck([&] { return std::format("node {:#010x} is added to a scene while queued, and its list neighbours do not point back at it", reinterpret_cast<uintptr_t>(node)); });
    }
    next(holder, edx, node, group);
}

// 0x006E4DE0: the holder's list dies with it (the game leaves its nodes' links as they are): forget its records first
void __fastcall Hook_HolderTeardown(void* holder, void* edx) {
    const FnThis0 next = reinterpret_cast<FnThis0>(EntryChain::Next(EntryChain::Site::SceneHolderTeardown, EntryChain::Layer::SceneBudget));
    if (holder && g_deferredCount.load(std::memory_order_acquire)) {
        uint32_t n = 0;
        {
            RegGuard lock;
            n = DropHolderLocked(holder);
        }
        c_teardownDropped.Add(n);
    }
    next(holder, edx);
}

// Development check before a budgeted drain: every recorded node of this holder that is still linked is well linked and
// alive. One found destroyed but still linked (what the destructor hook exists to prevent) is unlinked; anything else
// stops budgeting. False = do not budget (the caller runs the game's drain).
bool CheckRecords(void* holder) {
    if (kPublicBuild) return true;
    if (!g_deferredCount.load(std::memory_order_acquire)) return true;
    std::string failure;
    uintptr_t repairedNode = 0;
    {
        RegGuard lock;
        for (auto it = g_deferred.begin(); it != g_deferred.end();) {
            Link* const l = it->first;
            if (it->second != holder || !Queued(l)) {
                ++it;
                continue;
            }
            uint8_t* const node = reinterpret_cast<uint8_t*>(l) - kLinkOff;
            uintptr_t vt = 0;
            const bool consistent = LinkConsistent(l);
            const bool alive = NodeLooksAlive(node, &vt);
            if (consistent && alive) {
                ++it;
                continue;
            }
            if (consistent && g_deadVtable && vt == g_deadVtable) {
                UnlinkDead(l); // destroyed but still linked: its neighbours point at it, take it out before any drain calls it
                c_repaired.Add();
                repairedNode = reinterpret_cast<uintptr_t>(node);
                it = g_deferred.erase(it);
                continue;
            }
            failure = std::format("recorded node {:#010x} (vtable {:#010x}) is {}", reinterpret_cast<uintptr_t>(node), vt,
                                  consistent ? "not a live scene node" : "badly linked (its list neighbours do not point back at it)");
            break;
        }
        g_deferredCount.store(static_cast<uint32_t>(g_deferred.size()), std::memory_order_release);
    }
    if (repairedNode && !g_loggedRepair.exchange(true))
        HookGuard::Try("SceneBudget note", [&] {
            LOG_WARNING(std::format("[SceneBudget] Node {:#010x} was found destroyed but still queued before a drain; unlinked (please report)", repairedNode));
        });
    if (!failure.empty()) {
        StopForCheck([&] { return failure; });
        return false;
    }
    return true;
}

// One node of the drain: the tail of the local list out of it and updated, as the game's loop does
void UpdateTail(Link* local, uint32_t* counter, float* bounds) {
    Link* const l = local->prev;
    Link* const p = l->prev;
    uint8_t* const node = reinterpret_cast<uint8_t*>(l) - kLinkOff;
    p->next = local;
    local->prev = p;
    l->prev = nullptr;
    l->next = nullptr;
    const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(node);
    reinterpret_cast<FnUpdate>(*reinterpret_cast<const uintptr_t*>(vtable + kUpdateSlot))(node, nullptr);
    void* const b = g_bounds(node, nullptr, bounds);
    g_spatial(node, nullptr, b);
    *counter += 1;
}

// True when the node's draw this frame depends on its update (see "Draw state" in the header comment); reads only, and an
// unreadable node is left to the development checks (no C++ objects here: SEH)
bool MustNotWait(const uint8_t* node) {
    __try {
        const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(node);
        return vtable == g_swarmVtable || vtable == g_furVtable;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Room for `count` more records, in the local list and in the registry; false (noted) when there is no memory for them
bool ReserveRecords(std::vector<Link*>& rest, uint32_t count) {
    try {
        rest.reserve(count);
        RegGuard lock;
        g_deferred.reserve(g_deferred.size() + count);
        return true;
    } catch (...) {
        HookGuard::Note("SceneBudget records of the nodes left (drained in full instead)");
        return false;
    }
}

// The budgeted copy of 0x006E4130 (see the header comment). Returns the nodes processed; *left = nodes still queued.
uint32_t BudgetedDrain(uint8_t* holder, uint32_t cap, int64_t deadline, uint32_t* left) {
    Link* const head = reinterpret_cast<Link*>(holder + kListOff);
    uint32_t* const counter = reinterpret_cast<uint32_t*>(holder + kCounterOff);
    Link local;
    // splice everything into the local list; the holder's list is empty afterwards (0x006E413E..0x006E4192)
    Link* const first = head->next;
    Link* const last = head->prev;
    if (first == head) {
        local.next = &local;
        local.prev = &local;
    } else {
        local.next = first;
        local.prev = last;
        first->prev = &local;
        last->next = &local;
    }
    head->next = head;
    head->prev = head;
    *counter = 0;
    uint32_t done = 0;
    alignas(16) float bounds[8] = {}; // 0x006FB4B0 stores two aligned xmm into it
    // Past the budget, nodes that may wait move here (each to the front, so the list keeps their order); a live list like
    // the local one, so RemoveNode can still unlink them; spliced back after the local list below
    Link keep{&keep, &keep};
    bool over = false;
    // the loop (0x006E41A6..0x006E41F8), newest first; local.prev is re-read after every node (game code may unlink others)
    while (local.prev != &local) {
        Link* const l = local.prev;
        uint8_t* const node = reinterpret_cast<uint8_t*>(l) - kLinkOff;
        if (!over && done >= kMinNodes && (done >= cap || Qpc() >= deadline)) over = true;
        if (over && !MustNotWait(node)) {
            Link* const p = l->prev;
            p->next = &local;
            local.prev = p;
            l->prev = &keep;
            l->next = keep.next;
            keep.next->prev = l;
            keep.next = l;
            continue;
        }
        if (!kPublicBuild) {
            uintptr_t vt = 0;
            const bool linked = LinkConsistent(l) && l->next == &local; // l is the tail: its next is our sentinel
            const bool alive = NodeLooksAlive(node, &vt);
            if (!linked || !alive) {
                if (linked && g_deadVtable && vt == g_deadVtable) {
                    // destroyed but still queued (what the lifetime hooks exist to prevent): out of the list, never called
                    UnlinkDead(l);
                    c_repaired.Add();
                    if (!g_loggedRepair.exchange(true))
                        HookGuard::Try("SceneBudget note", [&] {
                            LOG_WARNING(std::format("[SceneBudget] Node {:#010x} was found destroyed but still queued in a drain; unlinked and skipped (please report)",
                                                reinterpret_cast<uintptr_t>(node)));
                        });
                    continue;
                }
                StopForCheck([&] { return std::format("before the update of node {:#010x} (vtable {:#010x}): {}", reinterpret_cast<uintptr_t>(node), vt,
                                         linked ? "it is not a live scene node" : "its list neighbours do not point back at it"); });
                break; // it stays in the list, untouched; the game's drain runs from the next frame on
            }
        }
        UpdateTail(&local, counter, bounds);
        done++;
        if (over) c_drawState.Add();
    }
    if (keep.next != &keep) { // after what a failed check left in the local list: the original order
        Link* const t = local.prev;
        t->next = keep.next;
        keep.next->prev = t;
        keep.prev->next = &local;
        local.prev = keep.prev;
    }
    // 07/10, players' Runtime Error: the records of the nodes left need memory, taken before anything goes back to the
    // holder. Without it this drain ends as the game's own does: every node processed, nothing left to record (after a
    // failed safety check the nodes are not touched: they go back unrecorded, and the game's drain runs from now on).
    std::vector<Link*> rest;
    uint32_t leftCount = 0;
    for (Link* x = local.next; x != &local && leftCount < kCountCap; x = x->next) leftCount++;
    const bool record = !leftCount || ReserveRecords(rest, leftCount);
    if (!record && !g_stopped.load(std::memory_order_acquire))
        while (local.prev != &local) {
            UpdateTail(&local, counter, bounds);
            done++;
        }
    // The rest goes back to the TAIL of the holder's list, in its order. The game queues at the tail (0x006E42E0) and the
    // drain takes from the tail (local.prev first), so the leftover, being newer in the list than anything queued
    // meanwhile, is processed first next frame; at the front it would be processed last and only cleared by the forced
    // full drain.
    uint32_t n = 0, foreign = 0;
    bool capped = false;
    if (local.next != &local) {
        Link* const f = local.next;
        Link* const t = local.prev;
        Link* const tail = head->prev; // == head when nothing was queued meanwhile
        tail->next = f;
        f->prev = tail;
        t->next = head;
        head->prev = t;
        for (Link* x = f;; x = x->next) { // recorded for the lifetime hooks, counted for the statistics
            if (record) rest.push_back(x); // within the capacity ReserveRecords took: no allocation
            if (*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(x) - kLinkOff + kOwnerOff) != holder) foreign++;
            n++;
            if (x == t) break;
            if (n >= kCountCap) {
                capped = true;
                break;
            }
        }
    }
    // The records of this holder now are exactly the nodes left (the others were processed: link 0)
    {
        RegGuard lock;
        DropHolderLocked(holder);
        for (Link* x : rest) g_deferred.emplace_back(x, holder);
        std::sort(g_deferred.begin(), g_deferred.end());
        g_deferredCount.store(static_cast<uint32_t>(g_deferred.size()), std::memory_order_release);
    }
    if (foreign) c_foreignOwner.Add(foreign);
    if (capped) StopForCheck([&] { return std::format("more than {} nodes left queued (a list that does not end?)", kCountCap); });
    *left = n;
    return done;
}

// Scene::BeginFrame's CALL of the drain (render thread), inner layer of the call chain
void __fastcall Hook_SceneDrain(void* holder, void* edx) {
    const FnDrain next = reinterpret_cast<FnDrain>(CallChain::Next(Site::SceneDrain, Layer::SceneBudget));
    t_note = DrainNote{};
    if (!g_on.load(std::memory_order_acquire) || !holder) {
        next(holder, edx);
        return;
    }
    g_drainThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    t_note.seen = true;
    c_calls.Add();
    const bool moving = LotLightingMotion::SampleCameraMoving();
    const uint64_t now = GetTickCount64();
    uint64_t prevSeen = 0;
    Waiting* const wp = WaitingOf(holder, now, prevSeen);
    // Budget only a scene drained every frame (the world); a scene drawn once or now and then (UI / off-screen) gets
    // everything, so it is never drawn with objects missing
    const bool steady = wp && prevSeen && now - prevSeen <= kSteadyMs;
    const uint64_t maxDefer = static_cast<uint64_t>(g_maxDeferMs.load(std::memory_order_relaxed));
    const uint64_t waited = wp && wp->since ? now - wp->since : 0;
    const bool waitedTooLong = waited >= maxDefer * kHardFactor;
    const uint32_t grow = waited >= maxDefer ? 1u << std::min<uint64_t>(6, 1 + (waited - maxDefer) / kGrowStepMs) : 1u;
    const bool stopped = g_stopped.load(std::memory_order_acquire);
    if (!moving || waitedTooLong || !steady || stopped || !HookGuard::Run("SceneBudget record check", false, [holder] { return CheckRecords(holder); })) {
        (waitedTooLong && moving ? c_fullForced : c_fullStill).Add();
        next(holder, edx); // the game's drain: everything, the nodes we left included
        DropHolder(holder); // all processed: their links are 0 now (kept during the drain: see the header comment)
        if (wp) wp->since = 0;
        return;
    }
    Waiting& w = *wp;
    const int64_t t0 = Qpc();
    const float ms = std::max(0.1f, g_msPerFrame.load(std::memory_order_relaxed)) * static_cast<float>(grow);
    const int64_t deadline = t0 + static_cast<int64_t>(static_cast<double>(ms) / g_qpcMs);
    const uint32_t cap = static_cast<uint32_t>(std::max(static_cast<int>(kMinNodes), g_nodesPerFrame.load(std::memory_order_relaxed))) * grow;
    if (grow > 1) c_grown.Add();
    uint32_t left = 0;
    const uint32_t done = BudgetedDrain(static_cast<uint8_t*>(holder), cap, deadline, &left);
    const float took = static_cast<float>(static_cast<double>(Qpc() - t0) * g_qpcMs);
    c_budgeted.Add();
    c_nodesBudgeted.Add(done);
    g_lastDone.store(done, std::memory_order_relaxed);
    g_lastLeft.store(left, std::memory_order_relaxed);
    g_lastMs.store(took, std::memory_order_relaxed);
    t_note.budgeted = true;
    t_note.left = left;
    if (left) {
        c_framesLeft.Add();
        c_nodesLeft.Add(left);
        if (left > g_maxLeft.load(std::memory_order_relaxed)) g_maxLeft.store(left, std::memory_order_relaxed);
        if (!w.since) w.since = now;
    } else {
        w.since = 0;
    }
}

// True when the pattern (hex bytes, "??" = any) matches at `addr`
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
    if (!MemPatch::ReadBytes(addr, have.data(), have.size())) return false;
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

// .text and .rdata of the game's module (TS3W.exe)
bool FindSections() {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + static_cast<uintptr_t>(dos->e_lfanew));
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        const uintptr_t b = base + s->VirtualAddress, e = b + s->Misc.VirtualSize;
        if (std::strncmp(reinterpret_cast<const char*>(s->Name), ".text", IMAGE_SIZEOF_SHORT_NAME) == 0) g_textBegin = b, g_textEnd = e;
        if (std::strncmp(reinterpret_cast<const char*>(s->Name), ".rdata", IMAGE_SIZEOF_SHORT_NAME) == 0) g_rdataBegin = b, g_rdataEnd = e;
    }
    return g_textBegin && g_rdataBegin;
}

constexpr EntryChain::Site kLifetimeSites[] = {EntryChain::Site::SceneNodeDtor, EntryChain::Site::SceneAddNode, EntryChain::Site::SceneHolderTeardown};

void RemoveLifetimeHooks() {
    for (EntryChain::Site s : kLifetimeSites) EntryChain::Remove(s, EntryChain::Layer::SceneBudget);
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
    if (!GameAddr::GroupAvailable("SceneNodeBudget", &missing)) return fail(GameAddr::NotAvailable(missing));
    const uintptr_t drain = GameAddr::Get(GameAddr::Id::SceneDrain);
    const uintptr_t boundsCall = GameAddr::Get(GameAddr::Id::SceneBoundsCall), spatialCall = GameAddr::Get(GameAddr::Id::SceneSpatialCall);
    const uintptr_t bounds = GameAddr::Get(GameAddr::Id::SceneNodeBounds), spatial = GameAddr::Get(GameAddr::Id::SceneNodeSpatial);
    const uintptr_t dtor = GameAddr::Get(GameAddr::Id::SceneNodeDtor), addNode = GameAddr::Get(GameAddr::Id::SceneAddNode);
    const uintptr_t teardown = GameAddr::Get(GameAddr::Id::SceneHolderTeardown);
    const uintptr_t swarmVtable = GameAddr::Get(GameAddr::Id::SwarmDrawableVtable), furVtable = GameAddr::Get(GameAddr::Id::SceneModelFurVtable);
    // The whole drain must be the loop this copy reproduces, and its two calls must reach the functions the copy calls
    if (!MatchAt(drain, kDrainBody)) return fail(std::format("The scene's pending-node drain at {:#010x} is not the code Apex was written for", drain));
    if (boundsCall != drain + 0xAE || spatialCall != drain + 0xB6 || CallTargetAt(boundsCall) != bounds || CallTargetAt(spatialCall) != spatial)
        return fail(std::format("The calls inside the pending-node drain at {:#010x} are not the expected ones", drain));
    // The lifetime hooks rely on AddNode's owner test, the destructor's layout and the teardown's start
    if (!MatchAt(addNode, kAddNodeHead)) return fail(std::format("The scene's AddNode at {:#010x} is not the code Apex was written for", addNode));
    if (!MatchAt(dtor, kDtorHead)) return fail(std::format("The scene node destructor at {:#010x} is not the code Apex was written for", dtor));
    if (!MatchAt(teardown, kTeardownHead)) return fail(std::format("The scene holder teardown at {:#010x} is not the code Apex was written for", teardown));
    if (!FindSections()) return fail("Could not read the game's code sections");
    {
        uint8_t store[6] = {};
        uint32_t vt = 0;
        if (MemPatch::ReadBytes(dtor + kDtorDeadVtableStore, store, sizeof store) && store[0] == 0xC7 && store[1] == 0x07) std::memcpy(&vt, store + 2, 4);
        g_deadVtable = (vt >= g_rdataBegin && vt < g_rdataEnd) ? vt : 0;
    }
    if (g_qpcMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    g_drainFn = drain;
    g_swarmVtable = swarmVtable;
    g_furVtable = furVtable;
    g_bounds = reinterpret_cast<FnBounds>(bounds);
    g_spatial = reinterpret_cast<FnSpatial>(spatial);
    LotLightingMotion::SampleCameraMoving(); // parses the camera position now (Start's thread), not in the first frame
    // The lifetime hooks first: from the first node left queued on, every one is covered
    std::string err;
    void* const hooks[] = {reinterpret_cast<void*>(&Hook_NodeDtor), reinterpret_cast<void*>(&Hook_AddNode), reinterpret_cast<void*>(&Hook_HolderTeardown)};
    for (int i = 0; i < 3; i++)
        if (!EntryChain::Install(kLifetimeSites[i], EntryChain::Layer::SceneBudget, hooks[i], &err)) {
            RemoveLifetimeHooks();
            return fail("Could not hook the scene node lifetime functions: " + err);
        }
    g_on.store(true, std::memory_order_release); // before the CALL can reach the hook
    if (!CallChain::Install(Site::SceneDrain, Layer::SceneBudget, reinterpret_cast<void*>(&Hook_SceneDrain), &err)) {
        g_on.store(false);
        RemoveLifetimeHooks();
        return fail("Could not hook the scene's pending-node drain: " + err);
    }
    g_started = true;
    LOG_INFO(std::format("[SceneBudget] On: Scene::BeginFrame's drain call {:#010x} -> {:#010x} goes through Apex (bounds {:#010x}, spatial {:#010x}); node lifetime "
                         "hooks on the destructor {:#010x}, AddNode {:#010x}, holder teardown {:#010x}; while the camera moves: at most {} nodes / {:.1f} ms per "
                         "frame, longest wait {} ms{}",
                         CallChain::CallAddress(Site::SceneDrain), drain, bounds, spatial, dtor, addNode, teardown, g_nodesPerFrame.load(), g_msPerFrame.load(),
                         g_maxDeferMs.load(), kPublicBuild ? "" : std::format("; development checks on (destroyed-node vtable {:#010x})", g_deadVtable)));
    if (g_stopped.load()) LOG_WARNING("[SceneBudget] A safety check failed earlier in this session: the game's own drain keeps running until the game restarts");
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_started) return;
    g_on.store(false, std::memory_order_release); // the hook runs the game's drain from now on (everything waiting included)
    CallChain::Remove(Site::SceneDrain, Layer::SceneBudget);
    // The nodes still left are processed by the next BeginFrame (the game's drain): the same wait as any node the game
    // queues after BeginFrame, so the records and the lifetime hooks can go now
    RemoveLifetimeHooks();
    uint32_t forgotten = 0;
    {
        RegGuard reg;
        forgotten = static_cast<uint32_t>(g_deferred.size());
        g_deferred.clear();
        g_deferredCount.store(0, std::memory_order_release);
    }
    g_started = false;
    const Stats s = GetStats();
    LOG_INFO(std::format("[SceneBudget] Off ({} drains, {} with a budget, {} left nodes for later, largest backlog {}, {} frames with a grown budget, {} forced full drains; lifetime guard: {} "
                         "unlinked at destruction, {} before AddNode, {} records dropped at teardown, {} destroyed on another thread, {} not owned by their "
                         "holder, {} repaired; {} records forgotten now)",
                         s.calls, s.budgeted, s.framesLeft, s.maxLeft, s.grown, s.fullForced, s.dtorUnlinked, s.addUnlinked, s.teardownDropped, s.otherThread, s.foreignOwner,
                         s.repaired, forgotten));
}

bool Running() { return g_on.load(std::memory_order_acquire); }

void SetNodesPerFrame(int n) { g_nodesPerFrame.store(std::clamp(n, static_cast<int>(kMinNodes), 65536)); }
void SetMsPerFrame(float ms) { g_msPerFrame.store(std::clamp(ms, 0.1f, 50.0f)); }
void SetMaxDeferMs(int ms) { g_maxDeferMs.store(std::clamp(ms, 16, 10000)); }

DrainNote TakeDrainNote() {
    DrainNote n = t_note;
    t_note = DrainNote{};
    return n;
}

Stats GetStats() {
    Stats s;
    s.calls = c_calls.Get();
    s.fullStill = c_fullStill.Get();
    s.fullForced = c_fullForced.Get();
    s.grown = c_grown.Get();
    s.budgeted = c_budgeted.Get();
    s.framesLeft = c_framesLeft.Get();
    s.nodesBudgeted = c_nodesBudgeted.Get();
    s.nodesLeft = c_nodesLeft.Get();
    s.maxLeft = g_maxLeft.load();
    s.lastDone = g_lastDone.load();
    s.lastLeft = g_lastLeft.load();
    s.lastMs = g_lastMs.load();
    s.tracked = g_deferredCount.load();
    s.dtorUnlinked = c_dtorUnlinked.Get();
    s.addUnlinked = c_addUnlinked.Get();
    s.teardownDropped = c_teardownDropped.Get();
    s.otherThread = c_otherThread.Get();
    s.foreignOwner = c_foreignOwner.Get();
    s.repaired = c_repaired.Get();
    s.drawState = c_drawState.Get();
    s.stopped = g_stopped.load();
    return s;
}

std::string StatusText() {
    if (!Running()) return "Off";
    const Stats s = GetStats();
    if (s.stopped) return "Stopped: a safety check failed (see the log); the game places new objects as usual until it restarts";
    if (!s.calls) return "On (no frames yet)";
    if (!s.budgeted) return "On: camera still, the game places new objects as usual";
    return std::format("On: {} of {} frames spread new objects while moving (largest wait {} objects)", s.framesLeft, s.budgeted, s.maxLeft);
}

void RenderDeveloperUI() {
    if (kPublicBuild) return;
    if (!ImGui::GetCurrentContext()) return;
    const Stats s = GetStats();
    int nodes = g_nodesPerFrame.load();
    if (ApexUi::DiagnosticIntRow("Nodes per frame while moving##SbNodes", &nodes, static_cast<int>(kMinNodes), 4096)) SetNodesPerFrame(nodes);
    float ms = g_msPerFrame.load();
    if (ImGui::SliderFloat("ms per frame while moving##SbMs", &ms, 0.1f, 10.0f, "%.1f ms")) SetMsPerFrame(ms);
    int wait = g_maxDeferMs.load();
    if (ApexUi::DiagnosticIntRow("Longest wait (ms)##SbWait", &wait, 16, 5000)) SetMaxDeferMs(wait);
    if (ApexUi::BeginAdvanced("LiveCounters", "Live counters")) {
    ImGui::TextWrapped("%s", ("Spread new objects over frames: " + StatusText()).c_str());
    ImGui::TextWrapped("Drain call %#010x -> %#010x; drains %llu: game's (camera still) %llu, game's (a node waited too long) %llu, with a budget %llu (%llu of them grown: a node waited too long)",
                        static_cast<unsigned>(CallChain::CallAddress(Site::SceneDrain)), static_cast<unsigned>(g_drainFn), static_cast<unsigned long long>(s.calls),
                        static_cast<unsigned long long>(s.fullStill), static_cast<unsigned long long>(s.fullForced), static_cast<unsigned long long>(s.budgeted),
                        static_cast<unsigned long long>(s.grown));
    ImGui::TextWrapped("With a budget: %llu nodes processed (%llu past the budget for their draw state), %llu frames left nodes (%llu node-frames waiting), largest backlog %u; last: %u done, %u left, %.2f ms",
                        static_cast<unsigned long long>(s.nodesBudgeted), static_cast<unsigned long long>(s.drawState), static_cast<unsigned long long>(s.framesLeft),
                        static_cast<unsigned long long>(s.nodesLeft), s.maxLeft, s.lastDone, s.lastLeft, s.lastMs);
    ImGui::TextWrapped("Lifetime guard: %u nodes recorded; unlinked at destruction %llu, before AddNode %llu, repaired %llu (all three expected 0); dropped at "
                        "teardown %llu; destroyed on another thread %llu; left nodes not owned by their holder %llu",
                        s.tracked, static_cast<unsigned long long>(s.dtorUnlinked), static_cast<unsigned long long>(s.addUnlinked),
                        static_cast<unsigned long long>(s.repaired), static_cast<unsigned long long>(s.teardownDropped), static_cast<unsigned long long>(s.otherThread),
                        static_cast<unsigned long long>(s.foreignOwner));
        ApexUi::EndAdvanced();
    }
}


void SaveDeveloperState(toml::table& out) {
    out.insert("nodes", g_nodesPerFrame.load());
    out.insert("time_ms", g_msPerFrame.load());
    out.insert("max_wait_ms", g_maxDeferMs.load());
}
void LoadDeveloperState(const toml::table& t) {
    if (auto n = t["nodes"].value<int64_t>()) { const int v = static_cast<int>(*n); SetNodesPerFrame(v); }
    if (auto n = t["time_ms"].value<double>()) { const float v = static_cast<float>(*n); if (std::isfinite(v)) SetMsPerFrame(v); }
    if (auto n = t["max_wait_ms"].value<int64_t>()) { const int v = static_cast<int>(*n); SetMaxDeferMs(v); }
}
} // namespace SceneBudget
