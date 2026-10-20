#include "memory_telemetry.h"
#include "allocator_tuning.h"
#include "memory_policy.h"
#include "fast_refpack.h"
#include "fast_memory.h"
#include "game_version.h"
#include <psapi.h>
// Room to save: a reserve of address space and the game's idle resource cache, given back when the game needs a large
// free block (see memory_guard.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "memory_guard.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include "imgui.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

namespace MemoryGuard {
namespace {

// Steam 1.67.2 (research\engine_map\full.asm, 05/10)
// 0x007377F0: mov eax,[ecx+14h]; push eax; add ecx,18h; call 0x00737560; ret 8 (the ResourceSystem's per-frame update,
// which trims both caches with Trim(0, 0, 0) at 0x007375A3 / 0x007375B4; thiscall, 1 argument, ret 4)
uintptr_t kUpdateCall = 0x007377F7, kUpdate = 0x00737560;
const BYTE kUpdateThunkBytes[] = {0x8B, 0x41, 0x14, 0x50, 0x83, 0xC1, 0x18, 0xE8};
// 0x00733E70: ResourceSystem vtable 0x00FFE2F0 slot +0x50: Trim(budget, 0, force) on both caches (+0x1E0, +0x1E4), which
// frees every idle entry (the "Shrink cache" of the game's live settings, 0x00733120, for each)
uintptr_t kShrinkBoth = 0x00733E70;
const BYTE kShrinkBytes[] = {0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0xE0, 0x01, 0x00, 0x00, 0x85, 0xC9};
// 0x00AAC31C: mov ecx,[esp+30h]; call 0x00C6D460 (the world save, thiscall, 2 arguments, ret 8, al = saved); its false
// makes 0x00AAC110 return 12 (Error 12)
uintptr_t kSaveCall = 0x00AAC320, kWorldSave = 0x00C6D460;
constexpr uintptr_t kDispatchCall=0x00AB12AF;
uintptr_t kDispatch=0x00AAC110;
std::vector<MemPatch::PatchLocation> g_savePatches;
std::atomic<bool> g_earlySave{false};
struct SaveStages { bool seen[3]{},ok[3]{}; uint64_t before[3]{},after[3]{},ms[3]{}; };
thread_local SaveStages* t_stages=nullptr;
uint64_t PrivateBytes() noexcept {
    const DWORD error=GetLastError();
    PROCESS_MEMORY_COUNTERS_EX p{};p.cb=sizeof p;
    const bool ok=GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&p),sizeof p)!=FALSE;
    SetLastError(error);
    return ok?p.PrivateUsage:0;
}
constexpr uintptr_t kStageCalls[]={0x00C6D529,0x00C6D537,0x00C6D544};
uintptr_t kStageTargets[]={0x00C6BCC0,0x00C61DC0,0x00C62860};
template<unsigned I> bool __fastcall SaveStageHook(void* wm,void*,void* a,void* b) {
    using Fn=bool(__thiscall*)(void*,void*,void*);
    auto* stage=t_stages;
    if (!stage) return reinterpret_cast<Fn>(kStageTargets[I])(wm,a,b);
    stage->seen[I]=true;
    stage->before[I]=PrivateBytes();
    const uint64_t start=GetTickCount64();
    const bool ok=reinterpret_cast<Fn>(kStageTargets[I])(wm,a,b);
    const DWORD error=GetLastError();
    stage->ms[I]=GetTickCount64()-start;
    stage->after[I]=PrivateBytes();stage->ok[I]=ok;
    SetLastError(error);
    return ok;
}
const BYTE kSaveCallBytes[] = {0x8B, 0x4C, 0x24, 0x30, 0xE8};
const BYTE kWorldSaveBytes[] = {0x83, 0xEC, 0x24, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9};

constexpr SIZE_T kReserveBytes = 128u << 20;  // the reserve
constexpr uint64_t kLowBytes = 320ull << 20;    // the largest free block under this: the game's idle resource cache emptied
constexpr uint64_t kLetGoBytes = 160ull << 20;  // under this: the reserve let go too (it is then the largest block left)
constexpr uint64_t kRoomBytes = 512ull << 20; // free left beside the reserve before it is taken again
constexpr DWORD kPeriodMs = 4000, kShrinkGapMs = 60000, kRetakeAfterMs = 30000;

std::vector<MemPatch::PatchLocation> g_patches;
std::atomic<bool> g_running{false};
HANDLE g_thread = nullptr, g_stop = nullptr;
std::mutex g_mx;           // the reserve
void* g_reserve = nullptr; // held reserve (MEM_RESERVE only)
DWORD g_releasedAt = 0;
std::atomic<void*> g_rs{nullptr};      // the ResourceSystem, seen by its update
std::atomic<DWORD> g_rsThread{0};       // the thread that runs its update
std::atomic<bool> g_shrinkWanted{false};
std::atomic<uint64_t> g_largest{0}, g_lowest{~0ull};
std::atomic<long> g_saves{0}, g_saveFails{0}, g_shrinks{0}, g_lets{0}, g_takes{0};
std::atomic<uint64_t> g_freedBytes{0};
std::atomic<bool> g_autoTrimWanted{false};
std::atomic<uint64_t> g_nextAutoTrim{0}, g_pressureAt{0};
std::atomic<bool> g_pressure{false};
uintptr_t g_partialTrim = 0; // derived from the verified native call, Steam test only
MemoryPolicy::Space ScanSpace() {
    MemoryPolicy::Space s;
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    uint64_t at=reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uint64_t hi=uint64_t(reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress))+1;
    MEMORY_BASIC_INFORMATION m{};
    for (;at<hi;) {
        if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(at)),&m,sizeof m)!=sizeof m) break;
        const auto end=std::min(hi,uint64_t(reinterpret_cast<uintptr_t>(m.BaseAddress))+m.RegionSize);
        if (end<=at) break;
        if (m.State==MEM_FREE) s.AddFree(at,end);
        at=end;
    }
    s.complete=at>=hi;
    PROCESS_MEMORY_COUNTERS_EX p{}; p.cb=sizeof p;
    s.processValid=GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&p),sizeof p)!=FALSE;
    if (s.processValid) s.privateBytes=p.PrivateUsage;
    return s;
}

// Same native-thread cache operation as the original ResourceSystem update. The
// experiment requests small steps and backs off when accounting does not decrease.
uint64_t TrimIdleStepRaw(void* rs) noexcept {
    uint64_t decreased=0;
    __try {
        for (int i=0;i<2;++i) {
            auto* cache=*reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(rs)+0x1E0+i*4);
            if (!cache) continue;
            const uint32_t idle=*reinterpret_cast<uint32_t*>(cache+0x98);
            const uint32_t argument=MemoryPolicy::TrimArgument(*reinterpret_cast<uint32_t*>(cache+0x8C),
                *reinterpret_cast<uint32_t*>(cache+0x94),idle);
            if (!argument) continue;
            reinterpret_cast<uint32_t(__thiscall*)(void*,uint32_t,void*,bool)>(g_partialTrim)(cache,argument,nullptr,true);
            const uint32_t after=*reinterpret_cast<uint32_t*>(cache+0x98);
            if (idle>after) decreased+=idle-after;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { g_partialTrim=0; }
    return decreased;
}


// The largest free block of the process's address space (bytes)
uint64_t LargestFree() {
    uint64_t largest = 0;
    uintptr_t at = 0x10000;
    MEMORY_BASIC_INFORMATION mbi{};
    while (at < 0xFFFF0000u && VirtualQuery(reinterpret_cast<LPCVOID>(at), &mbi, sizeof mbi) == sizeof mbi) {
        if (mbi.State == MEM_FREE && mbi.RegionSize > largest) largest = mbi.RegionSize;
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= at) break;
        at = next;
    }
    return largest;
}

bool TakeReserve() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (MemoryTelemetry::saveDepth.load(std::memory_order_acquire)) return false;
    if (g_reserve) return true;
    g_reserve = VirtualAlloc(nullptr, kReserveBytes, MEM_RESERVE | MEM_TOP_DOWN, PAGE_NOACCESS);
    if (g_reserve) g_takes.fetch_add(1, std::memory_order_relaxed);
    return g_reserve != nullptr;
}
bool LetReserveGo() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_reserve) return false;
    if (!VirtualFree(g_reserve, 0, MEM_RELEASE)) return false;
    g_reserve = nullptr;
    g_releasedAt = GetTickCount() | 1;
    g_lets.fetch_add(1, std::memory_order_relaxed);
    return true;
}
bool Holding() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_reserve != nullptr;
}

// The idle bytes of both caches (+0x98: bytes of the entries on the idle list; reading of the trim at 0x00732E90, unverified
// in game), 0 when unreadable
uint64_t IdleBytes(void* rs) {
    uint64_t sum = 0;
    __try {
        for (int k = 0; k < 2; k++)
            if (const uintptr_t cache = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(rs) + 0x1E0 + k * 4))
                sum += *reinterpret_cast<const uint32_t*>(cache + 0x98);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return sum;
}

// The game's own shrink of both caches (on the thread that runs the ResourceSystem's update)
bool ShrinkNow(void* rs, uint64_t& freed) {
    freed = 0;
    if (!rs) return false;
    const uint64_t before = IdleBytes(rs);
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(kShrinkBoth)(rs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    const uint64_t after = IdleBytes(rs);
    freed = before > after ? before - after : 0;
    g_shrinks.fetch_add(1, std::memory_order_relaxed);
    g_freedBytes.fetch_add(freed, std::memory_order_relaxed);
    return true;
}

// 07/10, players' Runtime Error: these lines are written when address space is short, inside the game's calls or on the watch
// thread: built with snprintf in a stack buffer (std::format allocates), and a line that cannot reach the log is dropped.
void LogLine(ApexLog::Level level, const char* text) noexcept {
    try {
        ApexLog::Write(level, text);
    } catch (...) {
    }
}

void ReportAllocationFailures() noexcept {
    if constexpr(kMemoryOptimizationsEnabled) {
        if(!kPublicBuild) return;
        MemoryTelemetry::Event events[64];
        const auto n=MemoryTelemetry::Drain(events);
        char line[256];
        for(unsigned i=0;i<n;++i) {
            const auto& e=events[i];
            std::snprintf(line,sizeof line,"[MemoryBudget] Observed native VirtualAlloc failure: %llu bytes, caller 0x%08X, flags 0x%X, Windows error %lu, thread %lu, private %llu KiB; one of the existing five allocation sites",
                static_cast<unsigned long long>(e.size),static_cast<unsigned>(e.caller),static_cast<unsigned>(e.type),
                e.error,e.thread,static_cast<unsigned long long>(e.privateBytes>>10));
            LogLine(ApexLog::Level::Warning,line);
        }
        const auto lost=MemoryTelemetry::dropped.exchange(0,std::memory_order_relaxed);
        if(lost) {
            std::snprintf(line,sizeof line,"[MemoryBudget] Allocation failure observations dropped: %llu",static_cast<unsigned long long>(lost));
            LogLine(ApexLog::Level::Warning,line);
        }
    }
}

using Update_t = void(__thiscall*)(void* rs, int arg);
void __fastcall UpdateHook(void* rs, void*, int arg) {
    reinterpret_cast<Update_t>(kUpdate)(rs, arg);
    g_rs.store(rs, std::memory_order_relaxed);
    g_rsThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const bool saving=MemoryTelemetry::saveDepth.load(std::memory_order_acquire)!=0;
    if (!saving && kMemoryOptimizationsEnabled && g_autoTrimWanted.exchange(false,std::memory_order_relaxed)) {
        const uint64_t now=GetTickCount64();
        if (g_partialTrim && g_pressure.load() && now-g_pressureAt.load()<12000 && now>=g_nextAutoTrim.load()) {
            const uint64_t decreased=TrimIdleStepRaw(rs);
            const uint64_t spent=GetTickCount64()-now;
            g_nextAutoTrim.store(now+((decreased<(1ull<<20) || spent>8) ? 600000 : 120000));
            if (decreased) {
                char line[192];
                std::snprintf(line,sizeof line,"[MemoryBudget] Idle accounting decreased %llu KiB in %llu ms; requested steps up to 16 MiB per cache (not measured physical RAM)",
                    static_cast<unsigned long long>(decreased>>10),static_cast<unsigned long long>(spent));
                LogLine(ApexLog::Level::Info,line);
            }
        }
    }
    if (!saving && g_shrinkWanted.exchange(false, std::memory_order_relaxed)) {
        uint64_t freed = 0;
        if (ShrinkNow(rs, freed)) {
            char line[160];
            std::snprintf(line, sizeof line, "[MemoryGuard] Largest free block low: the game's resource cache emptied (%llu MB of idle files)",
                          static_cast<unsigned long long>(freed >> 20));
            LogLine(ApexLog::Level::Info, line);
        }
    }
}

using WorldSave_t = bool(__thiscall*)(void* wm, void* a, void* b);
bool __fastcall WorldSaveHook(void* wm, void*, void* a, void* b) {
    MemoryTelemetry::SaveScope saving;
    MemoryTelemetry::Mark(2);
    PROCESS_MEMORY_COUNTERS_EX startMemory{};startMemory.cb=sizeof startMemory;
    const bool startKnown=GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&startMemory),sizeof startMemory)!=FALSE;
    const uint64_t before = LargestFree();
    const auto queuedReleased=kMemoryOptimizationsEnabled ? FastMemory::PrepareForSave() : 0;
    const auto apexFreed=kMemoryOptimizationsEnabled ? FastRefPack::PrepareForSave() : 0;
    uint64_t freed = 0;
    void* rs = g_rs.load(std::memory_order_relaxed);
    const bool shrunk = rs && g_rsThread.load(std::memory_order_relaxed) == GetCurrentThreadId() && ShrinkNow(rs, freed);
    const bool let = LetReserveGo();
    const uint64_t room = LargestFree();
    SaveStages stages;
    auto* previousStages=t_stages;t_stages=&stages;
    const bool ok = reinterpret_cast<WorldSave_t>(kWorldSave)(wm, a, b);
    t_stages=previousStages;
    MemoryTelemetry::Mark(ok ? 3 : 4);
    g_saves.fetch_add(1, std::memory_order_relaxed);
    if (!ok) g_saveFails.fetch_add(1, std::memory_order_relaxed);
    char line[384], extra[96] = "";
    if (shrunk) std::snprintf(extra, sizeof extra, ", resource cache emptied: %llu MB of idle files", static_cast<unsigned long long>(freed >> 20));
    std::snprintf(line, sizeof line, "[MemoryGuard] World save %s: largest free block %llu MB, %llu MB for the save (%s%s)", ok ? "done" : "FAILED (Error 12)",
                  static_cast<unsigned long long>(before >> 20), static_cast<unsigned long long>(room >> 20), let ? "reserve let go" : "no reserve held", extra);
    LogLine(ApexLog::Level::Info, line);
    PROCESS_MEMORY_COUNTERS_EX endMemory{};endMemory.cb=sizeof endMemory;
    const bool endKnown=GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&endMemory),sizeof endMemory)!=FALSE;
    std::snprintf(line,sizeof line,"[MemoryBudget] Save preparation: %llu KiB of inactive Apex storage released; queued releases processed %u; native cache %s; private bytes before %llu KiB (%s), after %llu KiB (%s); endpoints are not the save peak",
        static_cast<unsigned long long>(apexFreed>>10),queuedReleased,shrunk?"trimmed on its update thread":"not trimmed (update thread unavailable or different)",
        static_cast<unsigned long long>(startMemory.PrivateUsage>>10),startKnown?"valid":"unavailable",
        static_cast<unsigned long long>(endMemory.PrivateUsage>>10),endKnown?"valid":"unavailable");
    LogLine(ApexLog::Level::Info,line);
    if (g_earlySave) {
        for(unsigned i=0;i<3;++i) {
            if(!stages.seen[i]) continue;
            std::snprintf(line,sizeof line,"[MemoryBudget] Native save stage %u %s in %llu ms: private entry %llu KiB, exit %llu KiB; endpoints do not measure the peak",
                i+1,stages.ok[i]?"returned true":"returned false",static_cast<unsigned long long>(stages.ms[i]),
                static_cast<unsigned long long>(stages.before[i]>>10),static_cast<unsigned long long>(stages.after[i]>>10));
            LogLine(ApexLog::Level::Info,line);
        }
    }
    return ok;
}

int __fastcall SaveDispatchHook(void* owner,void*,void* a,void* b) {
    const DWORD entryError=GetLastError();
    MemoryTelemetry::SaveScope saving;
    const uint64_t before=PrivateBytes();
    const auto queued=FastMemory::PrepareForSave();
    const auto released=FastRefPack::PrepareForSave();
    AllocatorTuning::TrimNow("save"); // numbered development builds, allocator_trim: the allocator's own trim of free core tops
    const bool reserve=LetReserveGo();
    char line[256];
    std::snprintf(line,sizeof line,"[MemoryBudget] Early save preparation: %llu KiB of inactive Apex storage released, %u queued releases, reserve %s; private entry %llu KiB",
        static_cast<unsigned long long>(released>>10),queued,reserve?"released":"not held",static_cast<unsigned long long>(before>>10));
    LogLine(ApexLog::Level::Info,line);
    using Fn=int(__thiscall*)(void*,void*,void*);
    SetLastError(entryError);
    const int result=reinterpret_cast<Fn>(kDispatch)(owner,a,b);
    const DWORD error=GetLastError();
    std::snprintf(line,sizeof line,"[MemoryBudget] Save dispatch result %d; private exit %llu KiB; preparation covers this native dispatch, not the entire save request",
        result,static_cast<unsigned long long>(PrivateBytes()>>10));
    LogLine(ApexLog::Level::Info,line);
    SetLastError(error);
    return result;
}

DWORD WINAPI Watch(LPVOID) {
    DWORD lastShrink = 0;
    MemoryPolicy::Pressure pressure;
    for (;;) {
        if (WaitForSingleObject(g_stop, kPeriodMs) != WAIT_TIMEOUT) break;
        ReportAllocationFailures();
        const auto space=ScanSpace();
        const uint64_t largest = space.largest;
        if (!space.complete) continue;
        if (kMemoryOptimizationsEnabled) {
            const uint64_t now64=GetTickCount64();
            const auto failed=MemoryTelemetry::allocationFailureTick.load(std::memory_order_relaxed);
            const bool stressed=pressure.Observe(space,failed && now64>=failed && now64-failed<60000);
            g_pressure.store(stressed);
            MemoryTelemetry::pressure.store(stressed,std::memory_order_relaxed);
            g_pressureAt.store(now64);
            if (stressed && now64>=g_nextAutoTrim.load()) g_autoTrimWanted.store(true);
        }
        g_largest.store(largest, std::memory_order_relaxed);
        if (largest < g_lowest.load(std::memory_order_relaxed)) g_lowest.store(largest, std::memory_order_relaxed);
        const DWORD now = GetTickCount();
        if (largest < kLowBytes) {
            if (largest < kLetGoBytes && LetReserveGo()) {
                char line[128];
                std::snprintf(line, sizeof line, "[MemoryGuard] Largest free block %llu MB: the reserve of %llu MB let go", static_cast<unsigned long long>(largest >> 20),
                              static_cast<unsigned long long>(kReserveBytes >> 20));
                LogLine(ApexLog::Level::Warning, line);
            }
            if (!lastShrink || now - lastShrink >= kShrinkGapMs) {
                lastShrink = now | 1;
                g_shrinkWanted.store(true, std::memory_order_relaxed);
            }
        } else if (!Holding() && largest >= kReserveBytes + kRoomBytes) {
            DWORD released = 0;
            {
                std::lock_guard<std::mutex> lk(g_mx);
                released = g_releasedAt;
            }
            if (!released || now - released >= kRetakeAfterMs) TakeReserve();
        }
    }
    return 0;
}

bool CallsTo(uintptr_t site, uintptr_t target) {
    BYTE op = 0;
    int32_t rel = 0;
    return MemPatch::ReadBytes(site, &op, 1) && op == 0xE8 && MemPatch::ReadBytes(site + 1, &rel, 4) && site + 5 + rel == target;
}
bool Redirect(uintptr_t site, uintptr_t target, const void* to) {
    const DWORD orig = static_cast<DWORD>(target - (site + 5));
    return MemPatch::WriteDWORD(site + 1, static_cast<DWORD>(reinterpret_cast<uintptr_t>(to) - (site + 5)), &g_patches, &orig);
}

} // namespace

bool Start(std::string* error) {
    if (g_running.load()) return true;
    const auto fail = [&](const char* why) {
        if (error) *error = std::string("Room to save: ") + why;
        return false;
    };
    using GameAddr::Id;
    std::string missing;
    if (!GameAddr::Have({Id::ResUpdateCall, Id::ResUpdate, Id::ResShrinkBoth, Id::WorldSaveCall, Id::WorldSave}, &missing))
        return fail(GameAddr::NotAvailable(missing).c_str());
    kUpdateCall = GameAddr::Get(Id::ResUpdateCall);
    kUpdate = GameAddr::Get(Id::ResUpdate);
    kShrinkBoth = GameAddr::Get(Id::ResShrinkBoth);
    kSaveCall = GameAddr::Get(Id::WorldSaveCall);
    kWorldSave = GameAddr::Get(Id::WorldSave);
    // the calls land where they should on every build; Steam: every byte around them as well (other builds: the signatures
    // already matched the bytes around the calls; the called functions' own prologues are not assumed)
    bool same = CallsTo(kUpdateCall, kUpdate) && CallsTo(kSaveCall, kWorldSave);
    if (GameAddr::IsFixed())
        same = same && MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kUpdateCall - 7), kUpdateThunkBytes, sizeof kUpdateThunkBytes) &&
               MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kShrinkBoth), kShrinkBytes, sizeof kShrinkBytes) &&
               MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kSaveCall - 4), kSaveCallBytes, sizeof kSaveCallBytes) &&
               MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kWorldSave), kWorldSaveBytes, sizeof kWorldSaveBytes);
    if (!same) return fail("the game code differs");
    if (!Redirect(kUpdateCall, kUpdate, reinterpret_cast<const void*>(&UpdateHook)) || !Redirect(kSaveCall, kWorldSave, reinterpret_cast<const void*>(&WorldSaveHook))) {
        MemPatch::RestoreAll(g_patches);
        return fail("could not patch the game");
    }
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = g_stop ? CreateThread(nullptr, 64 * 1024, Watch, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr) : nullptr;
    if (!g_thread) {
        MemPatch::RestoreAll(g_patches);
        if (g_stop) CloseHandle(g_stop);
        g_stop = nullptr;
        return fail("could not start its thread");
    }
    if (kMemoryOptimizationsEnabled && g_exeTimestamp==0x52DEC247) {
        const BYTE dispatchBefore[]={0x6A,0x01,0x56,0x8B,0xCB,0xC6,0x45,0x45,0x01};
        const BYTE dispatchEntry[]={0x81,0xEC,0x30,0x04,0x00,0x00};
        bool valid=CallsTo(kDispatchCall,kDispatch) &&
            MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kDispatchCall-sizeof dispatchBefore),dispatchBefore,sizeof dispatchBefore) &&
            MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kDispatch),dispatchEntry,sizeof dispatchEntry);
        for(unsigned i=0;i<3;++i) valid=valid && CallsTo(kStageCalls[i],kStageTargets[i]);
        const void* hooks[]={reinterpret_cast<const void*>(&SaveStageHook<0>),reinterpret_cast<const void*>(&SaveStageHook<1>),reinterpret_cast<const void*>(&SaveStageHook<2>)};
        auto redirectSave=[&](uintptr_t site,uintptr_t target,const void* hook) {
            const DWORD orig=static_cast<DWORD>(target-(site+5));
            return MemPatch::WriteDWORD(site+1,static_cast<DWORD>(reinterpret_cast<uintptr_t>(hook)-(site+5)),&g_savePatches,&orig);
        };
        if(valid) {
            for(unsigned i=0;i<3 && valid;++i) valid=redirectSave(kStageCalls[i],kStageTargets[i],hooks[i]);
            if(valid) valid=redirectSave(kDispatchCall,kDispatch,reinterpret_cast<const void*>(&SaveDispatchHook));
        }
        if(!valid) MemPatch::RestoreAll(g_savePatches);
        g_earlySave=valid;
        LOG_INFO(valid ? "[MemoryBudget] Early native save preparation and three stage outcomes enabled"
                       : "[MemoryBudget] Early save sites differ; existing world-save preparation retained");
        const BYTE native[] = {0x81,0xEC,0x48,0x02,0x00,0x00,0x53,0x55,0x56};
        const BYTE call[] = {0x8B,0x81,0x8C,0x00,0x00,0x00,0x6A,0x01,0x6A,0x00,0x50,0xE8};
        if (MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(0x00732E90),native,sizeof native) &&
            MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kShrinkBoth+13),call,sizeof call)) g_partialTrim=0x00732E90;
        LOG_INFO(g_partialTrim ? "[MemoryBudget] Public test: lower/upper pressure policy; native idle-cache steps enabled, at least 120 s apart with 10 min no-benefit/slow-call backoff"
                              : "[MemoryBudget] Native cache step not validated; experiment inactive");
    }
    TakeReserve();
    g_running = true;
    LOG_INFO(std::format("[MemoryGuard] On: a reserve of {} MB of address space for world saves{}", kReserveBytes >> 20, Holding() ? "" : " (not taken yet)"));
    return true;
}

void Stop() {
    if (!g_running.exchange(false)) return;
    if (g_stop) SetEvent(g_stop);
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stop) {
        CloseHandle(g_stop);
        g_stop = nullptr;
    }
    MemPatch::RestoreAll(g_savePatches);
    g_earlySave=false;
    MemPatch::RestoreAll(g_patches);
    LetReserveGo();
    g_shrinkWanted = false;
    g_autoTrimWanted = false;
    g_pressure=false;
    MemoryTelemetry::pressure=false;
}

bool Running() { return g_running.load(); }

std::string StatusText() {
    if (!g_running.load()) return "off";
    const uint64_t largest = g_largest.load(), lowest = g_lowest.load();
    return std::format("on | largest free block {} MB (lowest {} MB) | reserve {} | world saves {} (failed {}) | resource cache emptied {} times ({} MB of idle files)",
                       largest >> 20, lowest == ~0ull ? 0 : lowest >> 20, Holding() ? std::format("{} MB held", kReserveBytes >> 20) : std::string("let go"),
                       g_saves.load(), g_saveFails.load(), g_shrinks.load(), g_freedBytes.load() >> 20);
}

void RenderDeveloperUI() {
    ImGui::TextUnformatted(("Room to save: " + StatusText()).c_str());
    void* rs = g_rs.load();
    ImGui::TextDisabled("Resource cache: idle files %llu MB", static_cast<unsigned long long>(rs ? IdleBytes(rs) >> 20 : 0));
    if (ImGui::Button("Empty the game's resource cache now")) g_shrinkWanted = true;
}

} // namespace MemoryGuard
