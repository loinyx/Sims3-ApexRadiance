// Faster scripts: the Mono interpreter's NaN tests done inline instead of calling msvcr80!_isnan, and the type objects of
// mono_type_get_object answered from a cache (see script_math.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "script_math.h"
#include "apex_log.h"
#include "memory_patch.h"
#include "build_flavor.h"
#include "entry_chain.h"
#include "game_addresses.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <vector>

namespace ScriptMath {
namespace {

std::atomic<bool> g_running{false};
uint32_t g_handlers = 0;

constexpr size_t kLenA = 22, kLenB = 13;
constexpr int kMaxHandlers = 64;

// block A with the slot and the esi step as wildcards (-1)
constexpr int kBlockA[kLenA] = {0x8B, 0x1D, -1, -1, -1, -1, 0x83, 0xEE, -1, 0x83, 0xEC, 0x08, 0xDD, 0x1C, 0x24, 0xFF, 0xD3, 0x83, 0xC4, 0x08, 0x85, 0xC0};
constexpr uint8_t kBlockB[kLenB] = {0x83, 0xEC, 0x08, 0xDD, 0x1C, 0x24, 0xFF, 0xD3, 0x83, 0xC4, 0x08, 0x85, 0xC0};

// The new code keeps the instructions from the old return address of `call ebx` on (A +17 "add esp, 8 ; test eax, eax",
// B +8): a thread inside _isnan while the code is rewritten returns into the same instructions. Only the bytes before
// it change, written with every other thread suspended outside them.
constexpr size_t kKeepA = 17, kKeepB = 8;
// A: mov ebx, _isnan ; sub esi, step ; xor eax, eax ; push eax ; push eax ; fucomip st0, st0 ; jnp +1 ; inc eax
//    (fucomip pops the operand; PF = 1 only when it is a NaN; the two pushes stand for the old "sub esp, 8")
// B: eax is 0 here (A's "jnz" fell through, and nothing jumps into B or onto the load before it):
//    push eax ; push eax ; fucomip st0, st0 ; jnp +1 ; inc eax ; nop
constexpr uint8_t kNewB[kKeepB] = {0x50, 0x50, 0xDF, 0xE8, 0x7B, 0x01, 0x40, 0x90};

struct Saved {
    uintptr_t a, b;
    uint8_t oldA[kKeepA], newA[kKeepA], oldB[kKeepB];
};
std::vector<Saved> g_saved;

bool WriteAll(bool on) {
    std::vector<MemPatch::CodeWrite> w;
    w.reserve(g_saved.size() * 2);
    for (const Saved& s : g_saved) {
        w.push_back({s.a, on ? s.newA : s.oldA, kKeepA, kKeepA});
        w.push_back({s.b, on ? kNewB : s.oldB, kKeepB, kKeepB});
    }
    return MemPatch::WriteCodeBatchSuspended(w.data(), w.size());
}

bool MatchA(const uint8_t* p) {
    for (size_t i = 0; i < kLenA; i++)
        if (kBlockA[i] >= 0 && p[i] != kBlockA[i]) return false;
    return true;
}

bool TextRange(const uint8_t*& begin, const uint8_t*& end) {
    const auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (std::memcmp(s->Name, ".text", 6) == 0) {
            begin = base + s->VirtualAddress;
            end = begin + s->Misc.VirtualSize;
            return true;
        }
    return false;
}

struct Site {
    uintptr_t a, b;
    uint8_t step;
};

// SEH only (no C++ objects): every handler's blocks; the slot all of them read. -1 = unreadable, else how many.
int Scan(const uint8_t* begin, const uint8_t* end, Site* out, uint32_t* slotOut) {
    int n = 0;
    uint32_t slot = 0;
    __try {
        for (const uint8_t* p = begin; p + kLenA + 0x40 < end; p++) {
            if (p[0] != 0x8B || p[1] != 0x1D || !MatchA(p)) continue;
            uint32_t s;
            std::memcpy(&s, p + 2, 4);
            // the jump on the result, then the second operand's load
            const uint8_t* q = p + kLenA;
            if (q[0] == 0x0F && q[1] == 0x85) q += 6;
            else if (q[0] == 0x75) q += 2;
            else continue;
            if (q[0] == 0xDD && q[1] == 0x46) q += 3;
            else if (q[0] == 0xDD && q[1] == 0x06) q += 2;
            else continue;
            if (std::memcmp(q, kBlockB, kLenB) != 0) continue;
            if (slot && s != slot) return -2; // two different slots: not the code this was written for
            slot = s;
            if (n >= kMaxHandlers) return -3;
            out[n++] = {reinterpret_cast<uintptr_t>(p), reinterpret_cast<uintptr_t>(q), p[8]};
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    *slotOut = slot;
    return n;
}


// ---- Type object cache (mono_type_get_object) ----
// mono_type_get_object (0x00EA8A00) enters the domain lock and looks the type up in domain->type_hash (hashing the type
// with mono_metadata_type_hash) on every call; the scripts reach it for typeof / GetType / reflection. A type's object is
// inserted only there, on a miss, and never replaced or removed (05/10: no other insert into domain +0x40 among the 24
// callers of the hash insert 0x00EBC140; the table goes with the domain), and Boehm's GC does not move objects, so the
// answer for (domain, type) stays the same until mono_domain_free. One answer is kept out: a TypeBuilder class answers
// klass->reflection_info (+0xB0) without the hash, which can change; such answers are never stored.
// Lock-free direct-mapped table: an entry is written under an odd sequence number (CAS from even) and read when the number
// is even and unchanged across the read. Entries carry the generation, bumped by every mono_domain_free and every Start.
using FnTypeGetObject = void*(__cdecl*)(void* domain, void* type);
using FnDomainFree = void(__cdecl*)(void* domain, int force);
using FnClassFromType = void*(__cdecl*)(void* type);

constexpr uint32_t kTypeSlots = 4096; // TypeSlot gives 12 bits
constexpr uint32_t kTypeStartupChecks = 256, kTypeDevCheckEvery = 64;
constexpr uint32_t kReflectionInfoOff = 0xB0;

struct TypeEntry {
    std::atomic<uint32_t> seq{0};
    std::atomic<void*> domain{nullptr};
    std::atomic<void*> type{nullptr};
    std::atomic<void*> result{nullptr};
    std::atomic<uint32_t> gen{0};
};
struct TypeLast {void* domain=nullptr;void* type=nullptr;void* result=nullptr;uint32_t gen=0;};
thread_local TypeLast t_typeLast;
TypeEntry g_types[kTypeSlots];
std::atomic<uint32_t> g_typeGen{1};
std::atomic<bool> g_typeOn{false}, g_typeSelfDisabled{false};
bool g_typeStarted = false;
FnClassFromType g_classFromType = nullptr;
std::atomic<uint32_t> g_typeHits{0};
std::atomic<uint64_t> c_typeCalls{0}, c_typeHits{0}, c_typeStored{0}, c_typeChecked{0}, c_typeMismatch{0}, c_domainFrees{0};

uint32_t TypeSlot(const void* domain, const void* type) {
    const uint32_t h = (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(type)) >> 2) ^ (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(domain)) >> 4);
    return (h * 2654435761u) >> 20;
}

void* LookupType(void* domain, void* type, uint32_t gen) {
    if(t_typeLast.domain==domain&&t_typeLast.type==type&&t_typeLast.gen==gen)return t_typeLast.result;
    TypeEntry& e = g_types[TypeSlot(domain, type)];
    const uint32_t s1 = e.seq.load(std::memory_order_acquire);
    if (s1 & 1) return nullptr;
    void* const d = e.domain.load(std::memory_order_relaxed);
    void* const t = e.type.load(std::memory_order_relaxed);
    void* const r = e.result.load(std::memory_order_relaxed);
    const uint32_t g = e.gen.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (e.seq.load(std::memory_order_relaxed) != s1) return nullptr;
    if(d!=domain||t!=type||g!=gen)return nullptr;
    t_typeLast={d,t,r,g};return r;
}

void StoreType(void* domain, void* type, void* result, uint32_t gen) {
    TypeEntry& e = g_types[TypeSlot(domain, type)];
    uint32_t s = e.seq.load(std::memory_order_relaxed);
    if ((s & 1) || !e.seq.compare_exchange_strong(s, s + 1, std::memory_order_acquire)) return; // another writer: skip
    e.domain.store(domain,std::memory_order_relaxed);
    e.type.store(type,std::memory_order_relaxed);
    e.result.store(result,std::memory_order_relaxed);
    e.gen.store(gen,std::memory_order_relaxed);
    e.seq.store(s + 2, std::memory_order_release);
    t_typeLast={domain,type,result,gen};
}

// SEH only: klass->reflection_info of the type's class (1 when unreadable: never equal to an object, so nothing stored)
void* ReflectionInfo(void* type) {
    __try {
        void* const klass = g_classFromType(type);
        return klass ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(klass) + kReflectionInfoOff) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return reinterpret_cast<void*>(1);
    }
}

void* __cdecl Hook_TypeGetObject(void* domain, void* type) {
    const auto next = reinterpret_cast<FnTypeGetObject>(EntryChain::Next(EntryChain::Site::MonoTypeGetObject, EntryChain::Layer::ScriptMath));
    if (!g_typeOn.load(std::memory_order_acquire) || g_typeSelfDisabled.load(std::memory_order_relaxed) || !domain || !type) return next(domain, type);
    c_typeCalls.fetch_add(1, std::memory_order_relaxed);
    const uint32_t gen = g_typeGen.load(std::memory_order_acquire);
    if (void* const hit = LookupType(domain, type, gen)) {
        const uint32_t n = g_typeHits.fetch_add(1, std::memory_order_relaxed);
        c_typeHits.fetch_add(1, std::memory_order_relaxed);
        if (n >= kTypeStartupChecks && (kPublicBuild || n % kTypeDevCheckEvery != 0)) return hit;
        void* const want = next(domain, type);
        c_typeChecked.fetch_add(1, std::memory_order_relaxed);
        if (want == hit) return hit;
        c_typeMismatch.fetch_add(1, std::memory_order_relaxed);
        if (!g_typeSelfDisabled.exchange(true))
            LOG_ERROR(std::format("[ScriptMath] A stored type object differs from the game's (type {:#010x}: stored {:#010x}, the game {:#010x}). The game's "
                                  "was used; type objects are looked up by the game for the rest of the session.",
                                  reinterpret_cast<uintptr_t>(type), reinterpret_cast<uintptr_t>(hit), reinterpret_cast<uintptr_t>(want)));
        return want;
    }
    void* const r = next(domain, type);
    if (r && r != ReflectionInfo(type)) {
        StoreType(domain, type, r, gen); // gen read before the call: a domain freed meanwhile makes the entry stale
        c_typeStored.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

void __cdecl Hook_DomainFree(void* domain, int force) {
    const auto next = reinterpret_cast<FnDomainFree>(EntryChain::Next(EntryChain::Site::MonoDomainFree, EntryChain::Layer::ScriptMath));
    g_typeGen.fetch_add(1, std::memory_order_acq_rel); // before its table goes: nothing stored so far is answered again
    c_domainFrees.fetch_add(1, std::memory_order_relaxed);
    next(domain, force);
}

bool StartTypeCache(std::string* why) {
    std::string missing;
    if (!GameAddr::GroupAvailable("ScriptTypeCache", &missing)) {
        *why = GameAddr::NotAvailable(missing);
        return false;
    }
    const uintptr_t fn = GameAddr::Get(GameAddr::Id::MonoTypeGetObject);
    // its first call (+9, inside the signature) is mono_class_from_mono_type (FUN_00e70d50 on Steam)
    uint8_t call[5] = {};
    if (!MemPatch::ReadBytes(fn + 9, call, 5) || call[0] != 0xE8) {
        *why = "mono_type_get_object differs";
        return false;
    }
    int32_t rel;
    std::memcpy(&rel, call + 1, 4);
    g_classFromType = reinterpret_cast<FnClassFromType>(fn + 14 + static_cast<uintptr_t>(static_cast<intptr_t>(rel)));
    std::string err;
    if (!EntryChain::Install(EntryChain::Site::MonoDomainFree, EntryChain::Layer::ScriptMath, reinterpret_cast<void*>(&Hook_DomainFree), &err)) {
        *why = "could not hook mono_domain_free: " + err;
        return false;
    }
    g_typeGen.fetch_add(1); // anything stored before a Stop / Start is not trusted
    g_typeHits.store(0);
    g_typeSelfDisabled.store(false);
    g_typeOn.store(true, std::memory_order_release);
    if (!EntryChain::Install(EntryChain::Site::MonoTypeGetObject, EntryChain::Layer::ScriptMath, reinterpret_cast<void*>(&Hook_TypeGetObject), &err)) {
        g_typeOn.store(false);
        EntryChain::Remove(EntryChain::Site::MonoDomainFree, EntryChain::Layer::ScriptMath);
        *why = "could not hook mono_type_get_object: " + err;
        return false;
    }
    g_typeStarted = true;
    return true;
}

void StopTypeCache() {
    if (!g_typeStarted) return;
    g_typeOn.store(false, std::memory_order_release);
    EntryChain::Remove(EntryChain::Site::MonoTypeGetObject, EntryChain::Layer::ScriptMath);
    EntryChain::Remove(EntryChain::Site::MonoDomainFree, EntryChain::Layer::ScriptMath);
    g_typeStarted = false;
    LOG_INFO(std::format("[ScriptMath] Type objects: {} calls, {} answered from the cache, {} stored, {} checked against the game, {} different, {} domains freed",
                         c_typeCalls.load(), c_typeHits.load(), c_typeStored.load(), c_typeChecked.load(), c_typeMismatch.load(), c_domainFrees.load()));
}

bool StartNan(std::string* why) {
    auto fail = [&](const std::string& reason) {
        *why = reason;
        return false;
    };
    const HMODULE crt = GetModuleHandleW(L"msvcr80.dll");
    using FnIsNan = int(__cdecl*)(double);
    const auto isnan = crt ? reinterpret_cast<FnIsNan>(GetProcAddress(crt, "_isnan")) : nullptr;
    if (!isnan) return fail("the game's C runtime (msvcr80.dll) is not loaded");
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    if (isnan(nan) != 1 || isnan(-nan) != 1 || isnan(1.0) != 0 || isnan(inf) != 0 || isnan(-0.0) != 0)
        return fail("_isnan does not answer 0 / 1 as expected");
    const uint8_t *begin = nullptr, *end = nullptr;
    if (!TextRange(begin, end)) return fail("the game's code section was not found");
    Site sites[kMaxHandlers];
    uint32_t slot = 0;
    const int n = Scan(begin, end, sites, &slot);
    if (n <= 0) return fail(n == 0 ? "the interpreter's NaN tests were not found" : "the game code differs");
    uint32_t held = 0;
    if (!MemPatch::ReadBytes(slot, &held, 4) || held != reinterpret_cast<uint32_t>(isnan)) return fail("the import slot does not hold _isnan");
    g_saved.clear();
    for (int i = 0; i < n; i++) {
        const Site& s = sites[i];
        Saved v{s.a, s.b};
        if (!MemPatch::ReadBytes(s.a, v.oldA, kKeepA) || !MemPatch::ReadBytes(s.b, v.oldB, kKeepB)) return fail("the game code could not be read");
        const uint8_t a[kKeepA] = {0xBB, 0, 0, 0, 0, 0x83, 0xEE, s.step, 0x31, 0xC0, 0x50, 0x50, 0xDF, 0xE8, 0x7B, 0x01, 0x40};
        std::memcpy(v.newA, a, kKeepA);
        std::memcpy(v.newA + 1, &held, 4);
        g_saved.push_back(v);
    }
    if (!WriteAll(true)) {
        WriteAll(false); // a batch that failed half-way: put back what it wrote
        g_saved.clear();
        return fail("could not patch the game (a thread stayed inside the code)");
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_handlers = static_cast<uint32_t>(n);
    LOG_INFO(std::format("[ScriptMath] {} interpreter compare handlers test NaN inline ({} calls to _isnan removed; first {:#010x}, slot {:#010x})", n, n * 2,
                         sites[0].a, slot));
    return true;
}

void StopNan() {
    if (g_saved.empty()) return;
    if (!WriteAll(false)) LOG_ERROR("[ScriptMath] The game code could not be put back (a thread stayed inside it)");
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_saved.clear();
    g_handlers = 0;
}

} // namespace

bool Start(std::string* error) {
    if (g_running.load()) return true;
    std::string nanWhy, typeWhy;
    const bool nan = StartNan(&nanWhy);
    const bool types = StartTypeCache(&typeWhy);
    if (!nan) LOG_WARNING("[ScriptMath] NaN tests left to the game: " + nanWhy);
    if (types)
        LOG_INFO(std::format("[ScriptMath] Type objects (mono_type_get_object {:#010x}) answered from a cache; the first {} answers are checked against the game",
                             GameAddr::Get(GameAddr::Id::MonoTypeGetObject), kTypeStartupChecks));
    else
        LOG_WARNING("[ScriptMath] Type object cache not used: " + typeWhy);
    if (!nan && !types) {
        if (error) *error = "Faster scripts: " + nanWhy + "; " + typeWhy;
        return false;
    }
    g_running = true;
    return true;
}

void Stop() {
    if (!g_running.exchange(false)) return;
    StopNan();
    StopTypeCache();
}

bool Running() { return g_running.load(); }

std::string StatusText() {
    if (!g_running.load()) return "off";
    std::string s = g_handlers ? std::format("{} script compare handlers test NaN without a call", g_handlers) : "NaN tests: the game's own";
    if (!g_typeStarted) return s + "; type object cache: not used";
    if (g_typeSelfDisabled.load()) return s + "; type object cache: turned itself off (an answer differed, see ApexRadiance_LOG.txt)";
    return s + std::format("; type objects: {} of {} from the cache, {} checked against the game, all equal", c_typeHits.load(), c_typeCalls.load(), c_typeChecked.load());
}

} // namespace ScriptMath
