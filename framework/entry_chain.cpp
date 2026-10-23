// Layered hooks on game function entries (see entry_chain.h).
#include "entry_chain.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include <windows.h>
#include <cstring>
#include <format>
#include <mutex>

namespace EntryChain {

std::atomic<void*> g_next[static_cast<int>(Site::Count)][static_cast<int>(Layer::Count)] = {};

namespace {

constexpr int kSites = static_cast<int>(Site::Count);
constexpr int kLayers = static_cast<int>(Layer::Count);
constexpr int kJmpLen = 5;
constexpr int kMaxPrologue = 8;

struct SiteInfo {
    const char* name;
    GameAddr::Id fn;
    uint8_t prologue[kMaxPrologue]; // the instructions moved to the trampoline (relocation-free, whole instructions)
    int len;
};
// Both encoders start with push ebp; mov ebp,esp; and esp,0FFFFFFF0h. No branch in .text lands on bytes 1..5 of either
// entry (checked in full.asm for the Frame Profiler's hand-made hooks of the same entries). The DPF direct write starts
// with sub esp,28h; push ebx; push esi (5 bytes, no relocation); no jump or call in .text lands on 0x004A7FC1..0x004A7FC4
// (engine_map\jmps.tsv, calls.tsv).
// The object lookup starts with mov eax,[esp+0Ch]; mov edx,[esp+8] (4 + 4 bytes, no relative operand): both move to the
// trampoline, the JMP covers bytes 0..4 and bytes 5..7 are never executed again. No branch in .text lands on bytes 1..7
// (checked in full.asm, 2026-09-29); the only instruction boundary inside the JMP is byte 4, which WriteCodeSuspended
// refuses while a thread is stopped there.
// The scene node sites (2026-09-29; engine_map calls.tsv / jmps.tsv / full.asm: no branch lands on 0x006FD931..0x006FD935,
// 0x006E6481..0x006E6484 or 0x006E4DE1..0x006E4DE5): the destructor's push ebp; mov ebp,esp; and esp,-16 (6 bytes);
// AddNode's push esi; mov esi,[esp+8] (1 + 4 bytes; [esp+8] is read after the push in the trampoline too); the teardown's
// four pushes and mov edi,ecx (6 bytes: byte 5 is never executed again, byte 4 is a boundary WriteCodeSuspended protects).
// The record CRC (2026-09-30) starts like the object lookup: mov ecx,[esp+4]; mov eax,[esp+8] (4 + 4 bytes, no relative
// operand); no branch in .text lands on 0x004FA4C1..0x004FA4C7 (jmps.tsv / calls.tsv).
// mono_type_get_object and mono_domain_free (05/10): register-only prologues; no branch in TS3W.exe lands on
// 0x00EA8A01..0x00EA8A07 or 0x00E75341..0x00E75345 (scan of every E8 / E9 / Jcc / JMP short on Steam 1.67.2).
// The object service's map find / insert / erase (05/10): register-only prologues; no branch lands on 0x00939101..07,
// 0x00939171..76 or 0x00938D01..04 (same scan).
// The light object removal 0x006C7610 (05/10): register-only prologue; no branch lands on 0x006C7611..16 (same scan).
const SiteInfo kSiteInfo[kSites] = {
    {"DXT1 encoder", GameAddr::Id::DxtEncode1, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0}, 6},
    {"DXT5 encoder", GameAddr::Id::DxtEncode5, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0}, 6},
    {"DPF direct record write", GameAddr::Id::DpfWriteDirect, {0x83, 0xEC, 0x28, 0x53, 0x56}, 5},
    {"object lookup by ID", GameAddr::Id::ObjectById, {0x8B, 0x44, 0x24, 0x0C, 0x8B, 0x54, 0x24, 0x08}, 8},
    {"scene node destructor", GameAddr::Id::SceneNodeDtor, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0}, 6},
    {"scene AddNode", GameAddr::Id::SceneAddNode, {0x56, 0x8B, 0x74, 0x24, 0x08}, 5},
    {"scene holder teardown", GameAddr::Id::SceneHolderTeardown, {0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9}, 6},
    {"room invalidate", GameAddr::Id::InvalidateRoom, {0x56, 0x8B, 0xF1, 0x8B, 0x0E}, 5},
    {"room invalidate on flag change", GameAddr::Id::InvalidateFlag, {0x8A, 0x44, 0x24, 0x04, 0x56}, 5},
    {"CAS triangle sort", GameAddr::Id::CasTriSort, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0}, 6},
    {"record CRC", GameAddr::Id::RecordCrc, {0x8B, 0x4C, 0x24, 0x04, 0x8B, 0x44, 0x24, 0x08}, 8},
    {"mono_type_get_object", GameAddr::Id::MonoTypeGetObject, {0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x55, 0x56, 0x57}, 8},
    {"mono_domain_free", GameAddr::Id::MonoDomainFree, {0x55, 0x56, 0x8B, 0x74, 0x24, 0x0C}, 6},
    {"object map find", GameAddr::Id::ObjMapFind, {0x83, 0xEC, 0x08, 0x56, 0x8B, 0x74, 0x24, 0x14}, 8},
    {"object map insert", GameAddr::Id::ObjMapInsert, {0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x14}, 7},
    {"object map erase", GameAddr::Id::ObjMapErase, {0x8B, 0x44, 0x24, 0x04, 0x53}, 5},
    // (no branch lands on 0x006FDFB1..0x006FDFB7, 0x006FE1F1..0x006FE1F6 or 0x006FDF41..0x006FDF44)
    {"scene object map find", GameAddr::Id::RenMapFind, {0x83, 0xEC, 0x08, 0x56, 0x8B, 0x74, 0x24, 0x14}, 8},
    {"scene object map insert", GameAddr::Id::RenMapInsert, {0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x14}, 7},
    {"scene object map erase", GameAddr::Id::RenMapErase, {0x8B, 0x44, 0x24, 0x04, 0x53}, 5},
    {"light object removal", GameAddr::Id::LightObjectRemove, {0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x57}, 7},
    {"lot AddLotObjectsToScene", GameAddr::Id::LotAddObjectsToScene, {0x83, 0xEC, 0x08, 0x57, 0x8B, 0xF9}, 6},
    // no branch lands on 0x006B4721..0x006B4724 or 0x006B4B11..0x006B4B15 (jmps.tsv / calls.tsv)
    {"light probe diffuse write", GameAddr::Id::ProbeDiffuseWrite, {0x83, 0xEC, 0x28, 0x53, 0x55}, 5},
    {"light probe specular write", GameAddr::Id::ProbeSpecularWrite, {0x81, 0xEC, 0xF4, 0x00, 0x00, 0x00}, 6},
};
static_assert(sizeof(kSiteInfo) / sizeof(kSiteInfo[0]) == static_cast<size_t>(Site::Count), "kSiteInfo must list every Site in order");

struct SiteState {
    uintptr_t fn = 0;
    uint8_t* tramp = nullptr;
    uint8_t orig[kJmpLen] = {};
    void* hook[kLayers] = {}; // installed layers (nullptr = not installed)
};
SiteState g_state[kSites]; // guarded by g_mutex
std::mutex g_mutex;

uint8_t* g_trampPool = nullptr; // one RWX page, never freed
size_t g_trampUsed = 0;
constexpr size_t kTrampPool = 4096, kTrampSlot = 32;

void MakeJmp(uint8_t out[kJmpLen], uintptr_t from, uintptr_t to) {
    out[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(to - (from + kJmpLen));
    std::memcpy(out + 1, &rel, 4);
}

// The pointer the entry JMP targets now: the outermost installed layer's hook, or 0 (no layer: original bytes)
uintptr_t Outermost(const SiteState& s) {
    for (int l = 0; l < kLayers; l++)
        if (s.hook[l]) return reinterpret_cast<uintptr_t>(s.hook[l]);
    return 0;
}

// The next inner installed layer's hook after `layer`, else the trampoline
uintptr_t InnerOf(const SiteState& s, int layer) {
    for (int l = layer + 1; l < kLayers; l++)
        if (s.hook[l]) return reinterpret_cast<uintptr_t>(s.hook[l]);
    return reinterpret_cast<uintptr_t>(s.tramp);
}

// The nearest installed layer outside `layer`, or -1
int OuterOf(const SiteState& s, int layer) {
    for (int l = layer - 1; l >= 0; l--)
        if (s.hook[l]) return l;
    return -1;
}

bool Resolve(int site, std::string* error) {
    SiteState& s = g_state[site];
    const SiteInfo& info = kSiteInfo[site];
    if (s.fn) return true;
    if (!GameAddr::Resolved()) {
        if (error) *error = "the game code was not scanned yet";
        return false;
    }
    const uintptr_t fn = GameAddr::Get(info.fn);
    if (!fn) {
        if (error) *error = GameAddr::NotAvailable(GameAddr::Name(info.fn));
        return false;
    }
    uint8_t cur[kMaxPrologue] = {};
    if (!MemPatch::ReadBytes(fn, cur, static_cast<size_t>(info.len))) {
        if (error) *error = std::format("the entry {:#010x} is not readable", fn);
        return false;
    }
    if (std::memcmp(cur, info.prologue, static_cast<size_t>(info.len)) != 0) {
        if (error) *error = std::format("the entry bytes at {:#010x} changed (hooked by another module?)", fn);
        return false;
    }
    if (!g_trampPool) g_trampPool = static_cast<uint8_t*>(VirtualAlloc(nullptr, kTrampPool, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_trampPool || g_trampUsed + kTrampSlot > kTrampPool) {
        if (error) *error = "no memory for the trampoline";
        return false;
    }
    uint8_t* t = g_trampPool + g_trampUsed;
    g_trampUsed += kTrampSlot;
    std::memcpy(t, cur, static_cast<size_t>(info.len));
    MakeJmp(t + info.len, reinterpret_cast<uintptr_t>(t + info.len), fn + static_cast<uintptr_t>(info.len));
    FlushInstructionCache(GetCurrentProcess(), t, kTrampSlot);
    s.tramp = t;
    std::memcpy(s.orig, cur, kJmpLen);
    s.fn = fn;
    return true;
}

// Writes `bytes` over the entry if it still holds `expect` (every other thread suspended, none inside the bytes)
bool WriteEntry(const SiteState& s, const uint8_t bytes[kJmpLen], const uint8_t expect[kJmpLen], std::string* error) {
    uint8_t cur[kJmpLen] = {};
    if (!MemPatch::ReadBytes(s.fn, cur, kJmpLen) || std::memcmp(cur, expect, kJmpLen) != 0) {
        if (error) *error = std::format("the entry bytes at {:#010x} were changed by another module", s.fn);
        return false;
    }
    if (!MemPatch::WriteCodeSuspended(s.fn, bytes, kJmpLen)) {
        if (error) *error = std::format("could not write the entry at {:#010x} (a thread kept running its first instructions)", s.fn);
        return false;
    }
    return true;
}

} // namespace

bool Install(Site site, Layer layer, void* hook, std::string* error) {
    const int si = static_cast<int>(site), li = static_cast<int>(layer);
    if (si < 0 || si >= kSites || li < 0 || li >= kLayers || !hook) {
        if (error) *error = "bad arguments";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    SiteState& s = g_state[si];
    if (s.hook[li] == hook) return true;
    if (s.hook[li]) {
        if (error) *error = "another hook is installed for this layer";
        return false;
    }
    if (!Resolve(si, error)) return false;
    const uintptr_t inner = InnerOf(s, li);
    g_next[si][li].store(reinterpret_cast<void*>(inner), std::memory_order_release); // before anything can reach the hook
    const int o = OuterOf(s, li);
    if (o >= 0) {
        // An outer layer keeps the entry: it now calls this one
        s.hook[li] = hook;
        g_next[si][o].store(hook, std::memory_order_release);
    } else {
        uint8_t expect[kJmpLen], jmp[kJmpLen];
        const uintptr_t cur = Outermost(s);
        if (cur) MakeJmp(expect, s.fn, cur);
        else std::memcpy(expect, s.orig, kJmpLen);
        MakeJmp(jmp, s.fn, reinterpret_cast<uintptr_t>(hook));
        if (!WriteEntry(s, jmp, expect, error)) return false;
        s.hook[li] = hook;
    }
    LOG_INFO(std::format("[EntryChain] {} ({:#010x}): layer {} installed (next = {:#010x}{})", kSiteInfo[si].name, s.fn, li, inner,
                         o >= 0 ? ", inside an outer layer" : (inner == reinterpret_cast<uintptr_t>(s.tramp) ? ", the game's code" : "")));
    return true;
}

bool Remove(Site site, Layer layer) {
    const int si = static_cast<int>(site), li = static_cast<int>(layer);
    if (si < 0 || si >= kSites || li < 0 || li >= kLayers) return true;
    std::lock_guard<std::mutex> lock(g_mutex);
    SiteState& s = g_state[si];
    if (!s.hook[li]) return true;
    const uintptr_t inner = InnerOf(s, li);
    const int o = OuterOf(s, li);
    if (o >= 0) {
        g_next[si][o].store(reinterpret_cast<void*>(inner), std::memory_order_release);
    } else {
        uint8_t expect[kJmpLen], bytes[kJmpLen];
        MakeJmp(expect, s.fn, reinterpret_cast<uintptr_t>(s.hook[li]));
        if (inner != reinterpret_cast<uintptr_t>(s.tramp)) MakeJmp(bytes, s.fn, inner);
        else std::memcpy(bytes, s.orig, kJmpLen);
        std::string error;
        if (!WriteEntry(s, bytes, expect, &error)) {
            uint8_t cur[kJmpLen] = {};
            if (MemPatch::ReadBytes(s.fn, cur, kJmpLen) && std::memcmp(cur, expect, kJmpLen) != 0) {
                // changed by someone else after us: left as it is (it may still reach the removed layer, which keeps forwarding)
                LOG_WARNING(std::format("[EntryChain] {}: {}; left as it is", kSiteInfo[si].name, error));
            } else {
                LOG_ERROR(std::format("[EntryChain] {}: layer {} not removed: {}", kSiteInfo[si].name, li, error));
                return false;
            }
        }
    }
    s.hook[li] = nullptr; // g_next[si][li] keeps its value for threads still inside the removed hook
    LOG_INFO(std::format("[EntryChain] {}: layer {} removed", kSiteInfo[si].name, li));
    return true;
}

bool Installed(Site site, Layer layer) {
    const int si = static_cast<int>(site), li = static_cast<int>(layer);
    if (si < 0 || si >= kSites || li < 0 || li >= kLayers) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state[si].hook[li] != nullptr;
}

uintptr_t GameFunction(Site site) {
    const int si = static_cast<int>(site);
    if (si < 0 || si >= kSites) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state[si].fn;
}

void* Original(Site site) {
    const int si = static_cast<int>(site);
    if (si < 0 || si >= kSites) return nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state[si].tramp;
}

} // namespace EntryChain
