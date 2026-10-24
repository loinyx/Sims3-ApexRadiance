#include "transition_calls.h"
#if defined(APEX_LOADING_DIAGNOSTICS) || (defined(APEX_DEVELOPMENT_BUILD_FLAVOR) && !defined(APEX_NO_DEV_TOOLS))
#include "transition_calls_state.h"
#include "apex_log.h"
#include "memory_patch.h"
#include <atomic>
#include <format>
#include <string>

namespace TransitionCalls {
namespace {
Calls calls;
SRWLOCK lock = SRWLOCK_INIT;
std::array<Call, 128> events;
size_t head = 0, count = 0;
std::atomic<uint64_t> lost{0};
using OneArg = void(__cdecl*)(uintptr_t);
using NoArg = void(__cdecl*)();
OneArg thumbnails = nullptr, heightMap = nullptr;
NoArg routing = nullptr;
const char* Name(Kind kind) {
    return kind == Kind::Thumbnails ? "UpdateDirtyLotThumbnails" : kind == Kind::Routing ? "BuildRoutingData" : "UnloadHeightMapRelatedData";
}
// Called under lock; all disk output belongs to Pump.
void Queue(const Call& call) {
    if (count == events.size()) ++lost;
    else { events[(head + count) % events.size()] = call; ++count; }
}
uint64_t Begin(Kind kind, uintptr_t argument) noexcept {
    if (!TryAcquireSRWLockExclusive(&lock)) { ++lost; return 0; }
    const auto id = calls.Begin(kind, GetTickCount64(), argument, GetCurrentThreadId());
    if (!id) ++lost;
    else for (const auto& slot : calls.slots) if (slot.id == id) { Queue(slot); break; }
    ReleaseSRWLockExclusive(&lock);
    return id;
}
void End(uint64_t id) noexcept {
    if (!id) return;
    if (!TryAcquireSRWLockExclusive(&lock)) { ++lost; return; }
    Call call;
    if (calls.End(id, GetTickCount64(), &call)) Queue(call);
    else ++lost;
    ReleaseSRWLockExclusive(&lock);
}
void __cdecl Thumbnails(uintptr_t callback) {
    const auto id = Begin(Kind::Thumbnails, callback);
    thumbnails(callback);
    End(id);
}
void __cdecl Routing() {
    const auto id = Begin(Kind::Routing, 0);
    routing();
    End(id);
}
void __cdecl HeightMap(uintptr_t release) {
    const auto id = Begin(Kind::HeightMap, release);
    heightMap(release);
    End(id);
}
}
void Start() {
    if (g_exeTimestamp != 0x52DEC247) return;
    // The internal-call table pairs each ScriptCore method name with its native entry.
    // Managed UpdateDirtyLotThumbnails yields through its callback; native return and
    // callback consumption are separate milestones, so this observer reports only the former.
    const GameAddress thumbnailEntry{"Transition thumbnails", {},
        "55 8B EC 83 E4 F8 81 EC 84 01 00 00 53 56 57 E8 ?? ?? ?? ?? 8B F0 8B 06 8B 90 18 01 00 00", 0,
        {0x55,0x8B,0xEC,0x83,0xE4,0xF8}};
    const GameAddress routingEntry{"Transition routing", {},
        "56 8B 35 ?? ?? ?? ?? 85 F6 74 18 57 8B BE 84 00 00 00 8B CE E8 ?? ?? ?? ?? 50 57 8B CE E8 ?? ?? ?? ?? 5F 5E C3", 0,
        {0x56,0x8B,0x35}};
    const GameAddress heightEntry{"Transition height map", {},
        "8B 0D C4 CB 1E 01 85 C9 74 0A 8B 44 24 04 50 E8 6C C8 4D 00 C3 CC CC CC", 0,
        {0x8B,0x0D}};
    unsigned attached = 0;
    const auto observe = [&](const GameAddress& entry, uintptr_t expected, void** original, void* hook) {
        const auto address = entry.Resolve();
        if (!address || *address != expected) {
            LOG_WARNING(std::format("[TransitionCalls] {} unavailable or already changed; other observers remain independent.", entry.name));
            return;
        }
        *original = reinterpret_cast<void*>(*address);
        if (DetourBatch::InstallHooks({{original, hook}})) {
            ++attached;
            LOG_INFO(std::format("[TransitionCalls] {} attached.", entry.name));
        } else LOG_WARNING(std::format("[TransitionCalls] {} could not attach; other observers remain independent.", entry.name));
    };
    observe(thumbnailEntry, 0x00778CC0, reinterpret_cast<void**>(&thumbnails), reinterpret_cast<void*>(&Thumbnails));
    observe(routingEntry, 0x0078AE40, reinterpret_cast<void**>(&routing), reinterpret_cast<void*>(&Routing));
    observe(heightEntry, 0x00787060, reinterpret_cast<void**>(&heightMap), reinterpret_cast<void*>(&HeightMap));
    LOG_INFO(std::format("[TransitionCalls] {} of 3 native observers attached. Entry/return timing includes yielding and waits, not CPU time. ProgressDialog closure and callback consumption are not observed.", attached));
}
void Pump() {
    static uint64_t last = 0, lastPending = 0, reportedLost = 0;
    static unsigned lines = 0;
    const auto now = GetTickCount64();
    if (now - last < 1000 || lines >= 4096) return;
    last = now;
    std::array<Call, 128> batch;
    std::array<Call, 16> pending;
    size_t n = 0;
    AcquireSRWLockExclusive(&lock);
    while (count) { batch[n++] = events[head]; head = (head + 1) % events.size(); --count; }
    pending = calls.slots;
    ReleaseSRWLockExclusive(&lock);
    for (size_t i = 0; i < n && lines < 4096; ++i) {
        const auto& call = batch[i];
        LOG_INFO(std::format("[TransitionCalls] {} id {} {}; thread {}; argument {:08X}; elapsed {} ms", Name(call.kind), call.id,
            call.active ? "entered" : "returned", call.thread, call.argument, call.active ? 0 : call.ended - call.started));
        ++lines;
    }
    const auto dropped = lost.load();
    if (now - lastPending >= 10000) {
        lastPending = now;
        for (const auto& call : pending) if (call.active && now - call.started >= 10000 && lines < 4096) {
            LOG_WARNING(std::format("[TransitionCalls] {} id {} has no observed return for {} ms; thread {}; argument {:08X}; loss count {}. Missing events can leave a stale pending call.",
                Name(call.kind), call.id, now - call.started, call.thread, call.argument, dropped));
            ++lines;
        }
    }
    if (dropped != reportedLost) {
        LOG_WARNING(std::format("[TransitionCalls] {} observations lost; call matching and pending state are incomplete.", dropped));
        reportedLost = dropped;
    }
    if (lines >= 4096) LOG_WARNING("[TransitionCalls] Session report limit reached; further call reports disabled.");
}
}
#else
namespace TransitionCalls { void Start() {} void Pump() {} }
#endif
