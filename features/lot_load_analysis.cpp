#include "lot_load_analysis.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_version.h"
#include "game_version.h"
#include "frame_profiler.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <format>
#include <string>
#include <unordered_map>

namespace LotLoadAnalysis {
namespace {
struct Event {
    State before, after;
    uint64_t start = 0, end = 0;
    DWORD thread = 0;
    bool request = false, detailed = false;
    uint64_t epoch = 0;
};
constexpr size_t kCapacity = 2048;
std::array<Event, kCapacity> events;
SRWLOCK lock = SRWLOCK_INIT;
size_t head = 0, tail = 0, size = 0;
std::atomic<uint64_t> dropped{0};
uint64_t captureEpoch = 0; // protected by the queue lock
uint64_t Tick() noexcept { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return t.QuadPart; }
bool Supported() noexcept { return g_exeTimestamp == 0x52DEC247; }
void Push(const Event& event) noexcept {
    if (!TryAcquireSRWLockExclusive(&lock)) { dropped.fetch_add(1); return; }
    if (size == kCapacity) dropped.fetch_add(1);
    else { events[tail] = event; events[tail].epoch=captureEpoch; tail = (tail+1)%kCapacity; ++size; }
    ReleaseSRWLockExclusive(&lock);
}
}
State ReadRenderer(void* renderer) noexcept {
    State s;
    if (!Supported() || !renderer) return s;
    __try {
        const auto p = static_cast<const unsigned char*>(renderer);
        s.lot = *reinterpret_cast<const uint64_t*>(p+0x10);
        s.stage = *reinterpret_cast<const uint32_t*>(p+0x24);
        s.step = *reinterpret_cast<const uint32_t*>(p+0x28);
        s.done = p[0x1E] != 0;
        s.failed = p[0x1F] != 0;
        s.valid = s.lot && (s.stage <= 0x15 || s.done || s.failed);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return s;
}
uint64_t Clock() noexcept { return Tick(); }
void Reset() noexcept {
    AcquireSRWLockExclusive(&lock);
    dropped.fetch_add(size);
    head=tail=size=0;
    ++captureEpoch;
    ReleaseSRWLockExclusive(&lock);
}
void Request(void* lot, bool detailed) noexcept {
    if (!Supported() || !lot) return;
    Event e;
    __try { e.before.lot = *reinterpret_cast<const uint64_t*>(static_cast<unsigned char*>(lot)+0x48); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!e.before.lot) return;
    e.request = true; e.detailed = detailed; e.start = e.end = Tick(); e.thread = GetCurrentThreadId();
    Push(e);
}
// (2.15.3) Stage 20 of the lot load (0x00AEA680, case 20) waits until 0x00ADBBA0 (the lot lighting manager [renderer+0x23C])
// says every level of its deque (+0x24..+0x40, 64 per block) is ready (0x006A5B50): a level with [lvl+0x81] == 0 is; else,
// when [lvl+0x88] >= 0, both solver states [lvl+0x2A4] / [lvl+0x2FC] must be non-zero; then every room light of the level's
// table (+0x234 buckets, +0x238 count, next node +0x80, the room light at node+0x10) must pass 0x0069F540(0), which, when it
// does not let the room through on its level numbers ([lvl+0x88], [lvl+0x284], [room+0x18]), wants [room+0x104] != 4.
// The note says which of these holds the lot.
namespace {
constexpr uint64_t kWaitNoteMs = 10000, kWaitRepeatMs = 30000;
struct Waiting { uint64_t since = 0, noted = 0; };
std::unordered_map<uint64_t, Waiting> g_waiting; // render thread (the load calls)
SRWLOCK g_noteLock = SRWLOCK_INIT;
std::string g_notes; // under g_noteLock; taken by Pump
bool RoomBlocks(const unsigned char* room) {
    const auto* lvl = *reinterpret_cast<const unsigned char* const*>(room);
    const int32_t a = *reinterpret_cast<const int32_t*>(lvl + 0x88), b = *reinterpret_cast<const int32_t*>(lvl + 0x284);
    const bool dl = room[0x18] != 0;
    if (a > b || (dl && a < 0) || (a < b && !dl)) return false;
    return *reinterpret_cast<const int32_t*>(room + 0x104) == 4;
}
std::string WaitText(const unsigned char* mgr) {
    std::string s;
    const auto d = [&](uint32_t off) { return *reinterpret_cast<const uintptr_t*>(mgr + off); };
    const intptr_t count = ((intptr_t(d(0x40)) - intptr_t(d(0x30))) >> 2) * 64 + ((intptr_t(d(0x34)) - intptr_t(d(0x38))) >> 2) +
                           ((intptr_t(d(0x2C)) - intptr_t(d(0x24))) >> 2) - 64;
    if (count <= 0 || count > 64) return std::format("{} levels", count);
    const auto* blocks = reinterpret_cast<const uintptr_t*>(d(0x30));
    const intptr_t first = (intptr_t(d(0x24)) - intptr_t(d(0x28))) >> 2;
    for (intptr_t i = 0; i < count; i++) {
        const intptr_t k = first + i;
        const auto* lvl = reinterpret_cast<const unsigned char*>(reinterpret_cast<const uintptr_t*>(blocks[k >> 6])[k & 63]);
        if (!lvl) continue;
        const int32_t l88 = *reinterpret_cast<const int32_t*>(lvl + 0x88);
        s += std::format("{}level {:08X}: lit {}, step {}/{}, solvers {}/{}", s.empty() ? "" : "; ", reinterpret_cast<uintptr_t>(lvl), lvl[0x81],
                         l88, *reinterpret_cast<const int32_t*>(lvl + 0x284), *reinterpret_cast<const int32_t*>(lvl + 0x2A4),
                         *reinterpret_cast<const int32_t*>(lvl + 0x2FC));
        if (!lvl[0x81]) continue;
        const auto* buckets = *reinterpret_cast<const uintptr_t* const*>(lvl + 0x234);
        const uint32_t nb = *reinterpret_cast<const uint32_t*>(lvl + 0x238);
        int rooms = 0, blocking = 0;
        std::string which;
        for (uint32_t b = 0; buckets && b < nb && b < 4096; b++)
            for (auto node = reinterpret_cast<const unsigned char*>(buckets[b]); node && rooms < 4096;
                 node = *reinterpret_cast<const unsigned char* const*>(node + 0x80)) {
                rooms++;
                const auto* room = *reinterpret_cast<const unsigned char* const*>(node + 0x10);
                if (room && RoomBlocks(room) && ++blocking <= 4) which += std::format(" {:08X}", reinterpret_cast<uintptr_t>(room));
            }
        s += std::format(", rooms {}, waiting in state 4: {}{}", rooms, blocking, which);
    }
    return s;
}
void FillWaitText(const unsigned char* mgr, std::string* out) { *out = mgr ? WaitText(mgr) : std::string("no lighting manager"); }
// (no object to unwind in a function with __try: the text is built in FillWaitText)
bool TryWaitText(const void* renderer, std::string* out) {
    __try {
        FillWaitText(*reinterpret_cast<const unsigned char* const*>(static_cast<const unsigned char*>(renderer) + 0x23C), out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}
void NoteWait(void* renderer, const State& after) noexcept {
    if (!Supported() || !renderer || !after.valid) return;
    if (after.stage != 20 || after.done || after.failed) {
        if (!g_waiting.empty()) g_waiting.erase(after.lot);
        return;
    }
    const uint64_t now = GetTickCount64();
    try {
        Waiting& w = g_waiting[after.lot];
        if (!w.since) w.since = now;
        if (now - w.since < kWaitNoteMs || (w.noted && now - w.noted < kWaitRepeatMs)) return;
        w.noted = now;
        if (g_waiting.size() > 256) g_waiting.clear();
        std::string text;
        if (!TryWaitText(renderer, &text)) text += " (unreadable)";
        AcquireSRWLockExclusive(&g_noteLock);
        if (g_notes.size() < 16384) g_notes += std::format("[LotLoadAnalysis] Lot {:016X} waits in stage 20 (lighting) for {:.0f} s: {}\n", after.lot, (now - w.since) / 1000.0, text);
        ReleaseSRWLockExclusive(&g_noteLock);
    } catch (...) {
    }
}
void Sample(const State& before, const State& after, uint64_t started, uint64_t ended) noexcept {
    if (!before.valid || !after.valid || before.lot != after.lot || ended < started) return;
    Push({before, after, started, ended, GetCurrentThreadId(), false, false});
}
void Pump() {
    static ULONGLONG last = 0;
    const auto now = GetTickCount64();
    if (last && now-last < 1000) return;
    last = now;
    static bool described = false;
    if (!described) {
        LOG_INFO(Supported() ? "[LotLoadAnalysis] Steam layout verified; capture follows Frame Profiler hooks. Enable developer mode and Frame Profiler to record. Time inside a call is inclusive elapsed time, not CPU time or visibility completion."
                             : "[LotLoadAnalysis] Per-lot layout not verified on this executable; layout reads disabled. Frame Profiler aggregate timings remain separate.");
        described = true;
    }
    static uint64_t lastDropped = 0;
    const auto lost = dropped.load();
    if (lost != lastDropped) { LOG_WARNING(std::format("[LotLoadAnalysis] {} events lost in total; request-to-done intervals may be incomplete", lost)); lastDropped=lost; }
    {
        std::string notes;
        AcquireSRWLockExclusive(&g_noteLock);
        notes.swap(g_notes);
        ReleaseSRWLockExclusive(&g_noteLock);
        for (size_t at = 0; at < notes.size();) {
            const size_t nl = notes.find('\n', at);
            LOG_INFO(notes.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
            if (nl == std::string::npos) break;
            at = nl + 1;
        }
    }
    std::array<Event, 512> batch;
    size_t count = 0;
    AcquireSRWLockExclusive(&lock);
    while (size && count < batch.size()) { batch[count++] = events[head]; head=(head+1)%kCapacity; --size; }
    ReleaseSRWLockExclusive(&lock);
    if (!count) return;
    static LARGE_INTEGER frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f; }();
    const auto ms = [](uint64_t ticks) { return double(ticks)*1000.0/double(frequency.QuadPart); };
    static std::wstring path;
    static uint64_t written = 0;
    static bool stopped = false;
    if (stopped) return;
    if (path.empty()) {
        if (!ApexPaths::EnsureApexDirectory()) return;
        SYSTEMTIME t{}; GetLocalTime(&t);
        path = ApexPaths::ApexDirectory()+std::format(L"LotLoadAnalysis-{}-{:04}{:02}{:02}-{:02}{:02}{:02}.csv",GetCurrentProcessId(),t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
    }
    struct Load { uint64_t request=0, calls=0; double elapsed=0; };
    static std::unordered_map<uint64_t,Load> loads;
    static uint64_t lastEpoch = 0;
    FILE* out=nullptr;
    if (_wfopen_s(&out,path.c_str(),written?L"ab":L"wb") || !out) { stopped=true; LOG_WARNING("[LotLoadAnalysis] Cannot open CSV"); return; }
    bool ok=true;
    if (!written) {
        const std::string header="build,qpc_start,qpc_frequency,thread,kind,lot_id,detailed,stage_before,step_before,stage_after,step_after,done,failed,call_elapsed_ms,request_to_done_ms,calls_since_request,inclusive_ms_since_request,dropped_total,capture_epoch\n";
        ok=fwrite(header.data(),1,header.size(),out)==header.size(); written+=header.size();
    }
    for (size_t i=0;i<count;++i) {
        const auto& e=batch[i];
        if (e.epoch!=lastEpoch) { loads.clear(); lastEpoch=e.epoch; }
        if (loads.size()>=256 && !loads.contains(e.before.lot)) { loads.clear(); LOG_WARNING("[LotLoadAnalysis] Tracking limit reached; prior request baselines discarded"); }
        auto& load=loads[e.before.lot];
        if (e.request) { if (e.detailed) load={e.start,0,0}; else load={}; }
        else { ++load.calls; load.elapsed+=ms(e.end-e.start); }
        const double wall=!e.request && e.after.done && load.request && e.end>=load.request ? ms(e.end-load.request) : -1;
        const std::string row=std::format("{},{},{},{},{},{:016X},{},{},{},{},{},{},{},{:.4f},{:.4f},{},{:.4f},{},{}\n",
            APEX_VERSION_STRING,e.start,frequency.QuadPart,e.thread,e.request?"request":"load_call",e.before.lot,e.detailed,
            e.before.stage,e.before.step,e.after.stage,e.after.step,e.after.done,e.after.failed,e.request?0:ms(e.end-e.start),wall,load.calls,load.elapsed,lost,e.epoch);
        if (written+row.size()>16ull*1024*1024) { stopped=true; LOG_WARNING("[LotLoadAnalysis] CSV reached its 16 MiB cap"); break; }
        ok=(fwrite(row.data(),1,row.size(),out)==row.size())&&ok; written+=row.size();
        if (!e.request && (e.after.done || e.after.failed)) {
            LOG_INFO(std::format("[LotLoadAnalysis] Lot {:016X}: done {}, failed {}, request-to-done {:.1f} ms (-1 means no matched request), {} calls, {:.1f} ms inclusive load-call time; lost events {}",
                e.before.lot,e.after.done,e.after.failed,wall,load.calls,load.elapsed,lost));
            loads.erase(e.before.lot);
        }
    }
    if (fclose(out)) ok=false;
    if (!ok) { stopped=true; LOG_WARNING("[LotLoadAnalysis] CSV write failed"); }
}
}
