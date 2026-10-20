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
