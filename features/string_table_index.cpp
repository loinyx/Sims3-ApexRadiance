// String table index: the game's text lookup starts its table walk at the table that answers (see string_table_index.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "string_table_index.h"
#include "string_table_index_core.h"
#include "string_table_index_culprit.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "game_addresses.h"
#include "hook_chain.h"
#include "memory_patch.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <format>
#include <vector>

namespace StringTableIndex {
namespace {

Core::Index g_index;
std::atomic<bool> g_running{false};
uintptr_t g_walk = 0, g_walkResume = 0;
BYTE g_walkOriginal[6] = {};
std::vector<DetourBatch::Hook> g_hooks;
std::atomic<uint32_t> g_loggedBuilds{0};
std::atomic<bool> g_loggedBroken{false};

// GetString's entry: sub esp,78h; push ebx; push ebp; push esi; mov ebx,ecx; push edi
constexpr BYTE kEntry[] = {0x83, 0xEC, 0x78, 0x53, 0x55, 0x56, 0x8B, 0xD9, 0x57};
// The walk's start (0x0059C5C2): mov esi,[ebx+48h]; add ebx,48h; mov edi,eax; xor ebp,ebp; cmp esi,ebx; mov [esp+10h],edi
constexpr BYTE kWalk[] = {0x8B, 0x73, 0x48, 0x83, 0xC3, 0x48, 0x8B, 0xF8, 0x33, 0xED, 0x3B, 0xF3, 0x89, 0x7C, 0x24, 0x10};
// Its loop test (walk + 0x4C): cmp esi,ebx; jne <next table>; mov edi,[esp+10h] (the "missing" path)
constexpr int kResume = 0x4C;
constexpr BYTE kResumeBytes[] = {0x3B, 0xF3, 0x75};
constexpr BYTE kResumeTail[] = {0x8B, 0x7C, 0x24, 0x10};
// The list changers' entries: clear (push esi; mov esi,[ecx+48h]), load one table (sub esp,30h; ...; mov ebx,ecx), rebuild
constexpr BYTE kClear[] = {0x56, 0x8B, 0x71, 0x48, 0x57, 0x8D, 0x79, 0x48};
constexpr BYTE kAdd[] = {0x83, 0xEC, 0x30, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xD9};
constexpr BYTE kRebuild[] = {0x83, 0xEC, 0x34, 0x55, 0x57, 0x8B, 0xE9};

// The table layout the index reads (Core::kBuckets, Core::kTableMap): the probe the walk calls hashes with
// mov edi,7919; div edi; mov eax,[ecx+edx*4] and returns lea eax,[ecx+7BBCh] when missing; the walk compares with [edi+7BE4h]
constexpr int kWalkProbeCall = 0x29, kWalkEndCmp = 0x32;
constexpr BYTE kProbeHash[] = {0xBF, 0xEF, 0x1E, 0x00, 0x00, 0xF7, 0xF7, 0x8B, 0x04, 0x91};
constexpr int kProbeHashAt = 0x0D, kProbeEndAt = 0x33;
constexpr BYTE kProbeEnd[] = {0x8D, 0x81, 0xBC, 0x7B, 0x00, 0x00};
constexpr BYTE kWalkEnd[] = {0x3B, 0x8F, 0xE4, 0x7B, 0x00, 0x00};

// STBL_Find, from the walk's call to it; 0 when that call is not there
uintptr_t Probe() {
    const uintptr_t call = g_walk + kWalkProbeCall;
    if (*reinterpret_cast<const BYTE*>(call) != 0xE8) return 0;
    return call + 5 + *reinterpret_cast<const int32_t*>(call + 1);
}

bool LayoutOk() {
    const uintptr_t probe = Probe();
    if (!probe) return false;
    return MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(g_walk + kWalkEndCmp), kWalkEnd, sizeof kWalkEnd) &&
           MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(probe + kProbeHashAt), kProbeHash, sizeof kProbeHash) &&
           MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(probe + kProbeEndAt), kProbeEnd, sizeof kProbeEnd);
}

using ClearFn = void(__fastcall*)(void*, void*);
using AddFn = bool(__fastcall*)(void*, void*, const void*);
using RebuildFn = bool(__fastcall*)(void*, void*);
ClearFn g_clear = nullptr;
AddFn g_add = nullptr;
RebuildFn g_rebuild = nullptr;

void __fastcall ClearHook(void* service, void* edx) {
    g_index.MutationBegin();
    g_clear(service, edx);
    g_index.MutationEnd();
}
bool __fastcall AddHook(void* service, void* edx, const void* key) {
    g_index.MutationBegin();
    const bool r = g_add(service, edx, key);
    g_index.MutationEnd();
    return r;
}
bool __fastcall RebuildHook(void* service, void* edx) {
    g_index.MutationBegin();
    const bool r = g_rebuild(service, edx);
    g_index.MutationEnd();
    return r;
}

__declspec(noinline) void LogBuild() {
    const auto& s = g_index.stats;
    const uint32_t builds = s.builds.load();
    if (builds <= g_loggedBuilds.load()) return;
    const uint32_t n = g_loggedBuilds.exchange(builds);
    if (n >= 8 && builds % 100 != 0) return; // the first 8 builds, then every 100th
    LOG_INFO(std::format("[StringTableIndex] Index built (#{}): {} strings from {} tables, {} KB, {} ms in {} steps", builds, s.lastEntries.load(),
                         s.lastTables.load(), s.lastBytes.load() / 1024, s.lastBuildMs.load(), s.lastSteps.load()));
}

__declspec(noinline) void LogBroken() {
    if (g_loggedBroken.exchange(true)) return;
    LOG_WARNING(std::format("[StringTableIndex] Index off for this session ({}): {}",
                            g_index.stats.mismatches.load() ? "an answer differed from the game's walk" : "the string tables' hash layout changed", ReportLine()));
}

// Called from the stub: ecx = the list sentinel (service + 0x48), edx = the 64-bit key on the game's stack
uintptr_t __fastcall WalkStart(uintptr_t sentinel, const uint32_t* key) {
    const uintptr_t start = g_running.load(std::memory_order_relaxed) ? g_index.Start(sentinel, key) : Core::Rd(sentinel);
    if (g_index.stats.builds.load(std::memory_order_relaxed) != g_loggedBuilds.load(std::memory_order_relaxed)) LogBuild();
    if (g_index.broken.load(std::memory_order_relaxed) && !g_loggedBroken.load(std::memory_order_relaxed)) LogBroken();
    return start;
}

// Replaces the walk's first 6 bytes with a jump here. Does what those 16 bytes did (esi = first node, ebx = sentinel,
// edi = [esp+10h] = the string table service, ebp = 0), asks WalkStart for the node to start from and enters the walk's
// loop test with it: a node is probed as the game's next table, the sentinel takes the "missing" path.
__declspec(naked) void WalkStub() {
    __asm {
        mov esi, [ebx + 48h]
        add ebx, 48h
        mov edi, eax
        xor ebp, ebp
        mov [esp + 10h], edi
        lea edx, [esp + 8Ch] // the key (GetString's first argument), as the game's probe passes it
        mov ecx, ebx
        call WalkStart
        mov esi, eax
        jmp dword ptr [g_walkResume]
    }
}

// Code a stand-down compares: address, the bytes Apex expects there
struct Span {
    uintptr_t at;
    const BYTE* bytes;
    size_t n;
};

bool ReadPtr(uintptr_t at, uintptr_t* value) { return MemPatch::ReadBytes(at, value, sizeof *value); }

constexpr const char* kGameExe = "<game exe>"; // (JumpModule: the jump lands in the game's own exe)

// The module a jump lands in, following up to 4 jumps through memory outside any module (trampolines, loader stubs)
std::string JumpModule(uintptr_t at, bool* jumped) {
    for (int hop = 0; hop < 4; ++hop) {
        BYTE b[8] = {};
        if (!MemPatch::ReadBytes(at, b, sizeof b)) return {};
        const uintptr_t to = Culprit::JumpTarget(b, sizeof b, at, &ReadPtr);
        if (!to) return {};
        *jumped = true;
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(to), &mod) && mod)
            return mod == GetModuleHandleW(nullptr) ? std::string(kGameExe) : HookChain::ModuleOf(reinterpret_cast<const void*>(to));
        at = to;
    }
    return {};
}

// Names who changed the code of a stand-down: the module a patch over the first changed span jumps into (decoded at the
// span's start, then at its first changed byte), else a loaded known mod with a setting for this site (an in-place change
// has no jump).
// shown: the address in the text (0: the first changed span's)
std::string Changed(Culprit::Site site, uintptr_t shown, std::initializer_list<Span> spans) {
    bool jumped = false;
    for (const Span& s : spans) {
        if (!s.at || MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(s.at), s.bytes, s.n)) continue;
        if (!shown) shown = s.at;
        std::vector<BYTE> cur(s.n);
        if (!MemPatch::ReadBytes(s.at, cur.data(), s.n)) continue; // (the span itself may be unreadable)
        size_t d = 0;
        while (d + 1 < s.n && cur[d] == s.bytes[d]) ++d;
        if (const std::string m = JumpModule(s.at, &jumped); !m.empty() && m != "?")
            return m == kGameExe ? Culprit::DescribeExe(site, shown) : Culprit::Describe(site, shown, m, false);
        // mid-instruction: an immediate changed in place can read as a jump, so only a jump into another module counts
        bool midJump = false;
        if (const std::string m = d ? JumpModule(s.at + d, &midJump) : std::string(); !m.empty() && m != "?" && m != kGameExe) {
            jumped = true;
            return Culprit::Describe(site, shown, m, false);
        }
    }
    if (!jumped)
        for (const auto& k : Culprit::kKnown)
            if (k.fix[static_cast<int>(site)] && GetModuleHandleA(k.file)) return Culprit::Describe(site, shown, k.file, false, true);
    return Culprit::Describe(site, shown, {}, jumped);
}

bool StandDown(std::string* error, const std::string& why) {
    LOG_WARNING("[StringTableIndex] Off: " + why);
    if (error) *error = "Faster text lookups: " + why;
    return false;
}

} // namespace

bool Start(std::string* error) {
    if (g_running.load()) return true;
    using GameAddr::Id;
    std::string missing;
    if (!GameAddr::Have({Id::StblGetString, Id::StblWalk, Id::StblClear, Id::StblAdd, Id::StblRebuild}, &missing)) {
        if (error) *error = "Faster text lookups: " + GameAddr::NotAvailable(missing);
        return false;
    }
    const std::wstring docs = ApexPaths::GameDocumentsDirectory();
    if (!docs.empty() && GetFileAttributesW((docs + L"Mods\\Packages\\CCStringMerge_Strings.package").c_str()) != INVALID_FILE_ATTRIBUTES)
        return StandDown(error, "CCStringMerge_Strings.package is in Mods\\Packages (string tables already merged)");
    const uintptr_t entry = GameAddr::Get(Id::StblGetString);
    g_walk = GameAddr::Get(Id::StblWalk);
    g_walkResume = g_walk + kResume;
    const uintptr_t clear = GameAddr::Get(Id::StblClear), add = GameAddr::Get(Id::StblAdd), rebuild = GameAddr::Get(Id::StblRebuild);
    using Culprit::Site;
    if (!MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(entry), kEntry, sizeof kEntry))
        return StandDown(error, Changed(Site::Entry, entry, {{entry, kEntry, sizeof kEntry}}));
    if (g_walk <= entry || g_walk - entry > 0x40)
        return StandDown(error, std::format("the table walk ({:#010x}) is not where it should be", g_walk));
    if (!MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(g_walk), kWalk, sizeof kWalk) ||
        !MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(g_walkResume), kResumeBytes, sizeof kResumeBytes) ||
        !MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(g_walkResume + 4), kResumeTail, sizeof kResumeTail))
        return StandDown(error, Changed(Site::Walk, g_walk,
                                        {{g_walk, kWalk, sizeof kWalk}, {g_walkResume, kResumeBytes, sizeof kResumeBytes}, {g_walkResume + 4, kResumeTail, sizeof kResumeTail}}));
    if (!MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(clear), kClear, sizeof kClear) || !MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(add), kAdd, sizeof kAdd) ||
        !MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(rebuild), kRebuild, sizeof kRebuild))
        return StandDown(error, Changed(Site::ListFunctions, 0, {{clear, kClear, sizeof kClear}, {add, kAdd, sizeof kAdd}, {rebuild, kRebuild, sizeof kRebuild}}));
    if (!LayoutOk()) {
        const uintptr_t probe = Probe();
        return StandDown(error, Changed(Site::Layout, probe ? probe : g_walk,
                                        {{g_walk + kWalkEndCmp, kWalkEnd, sizeof kWalkEnd},
                                         {probe ? probe + kProbeHashAt : 0, kProbeHash, sizeof kProbeHash},
                                         {probe ? probe + kProbeEndAt : 0, kProbeEnd, sizeof kProbeEnd}}));
    }

    g_clear = reinterpret_cast<ClearFn>(clear);
    g_add = reinterpret_cast<AddFn>(add);
    g_rebuild = reinterpret_cast<RebuildFn>(rebuild);
    g_hooks = {{reinterpret_cast<void**>(&g_clear), reinterpret_cast<void*>(&ClearHook)},
               {reinterpret_cast<void**>(&g_add), reinterpret_cast<void*>(&AddHook)},
               {reinterpret_cast<void**>(&g_rebuild), reinterpret_cast<void*>(&RebuildHook)}};
    if (!DetourBatch::InstallHooks(g_hooks)) return StandDown(error, "could not hook the string table list's functions");
    g_index.Free();
    g_index.broken = false;
    g_loggedBroken = false;
    g_index.layoutOk = &LayoutOk;
    g_running = true; // before the jump can reach the stub
    std::memcpy(g_walkOriginal, reinterpret_cast<const void*>(g_walk), sizeof g_walkOriginal);
    BYTE jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
    const int32_t rel = MemPatch::CalculateRelativeOffset(g_walk, reinterpret_cast<uintptr_t>(&WalkStub));
    std::memcpy(jmp + 1, &rel, 4);
    if (!MemPatch::WriteCodeSuspended(g_walk, jmp, sizeof jmp)) {
        g_running = false;
        DetourBatch::RemoveHooks(g_hooks);
        return StandDown(error, "could not patch the table walk");
    }
    LOG_INFO(std::format("[StringTableIndex] On at {:#010x} (list changes at {:#010x}, {:#010x}, {:#010x}): the index is built from the loaded tables at the "
                         "first text lookups",
                         g_walk, clear, add, rebuild));
    return true;
}

void Stop() {
    if (!g_running.exchange(false)) return;
    MemPatch::WriteCodeSuspended(g_walk, g_walkOriginal, sizeof g_walkOriginal);
    DetourBatch::RemoveHooks(g_hooks);
    LOG_INFO("[StringTableIndex] Off: " + ReportLine());
    g_index.Free();
}

bool Running() { return g_running.load(); }

std::string StatusText() {
    if (!g_running.load()) return "off";
    if (g_index.broken.load()) return "on, index off for this session (an answer differed from the game's walk or the table layout changed)";
    return g_index.Ready() ? std::format("on, {} strings indexed", g_index.Entries()) : "on, index building";
}

std::string ReportLine() {
    const auto& s = g_index.stats;
    return std::format("hits {}, missing {}, game walks {}, checked {} (differed {}), builds {}, list changes {}, over cap {}, last build {} strings / {} tables / {} KB / {} ms",
                       s.hits.load(), s.misses.load(), s.fallbacks.load(), s.verified.load(), s.mismatches.load(), s.builds.load(), s.invalidations.load(),
                       s.overCap.load(), s.lastEntries.load(), s.lastTables.load(), s.lastBytes.load() / 1024, s.lastBuildMs.load());
}

} // namespace StringTableIndex
