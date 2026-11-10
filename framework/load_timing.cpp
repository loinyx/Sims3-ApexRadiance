#include "load_timing.h"
#include "apex_log.h"
#include "fast_dxt.h"
#include "fast_refpack.h"
#include "resource_cache.h"
#include "shader_cache.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <format>
#include <string>

namespace LoadTiming {
namespace {

int64_t Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
int64_t Freq() {
    static const int64_t f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return q.QuadPart;
    }();
    return f;
}
// The process start on the QPC clock (its creation time, converted once)
int64_t ProcessStartQpc() {
    static const int64_t at = [] {
        FILETIME create{}, exitT{}, kernel{}, user{}, now{};
        const int64_t q = Qpc();
        GetSystemTimePreciseAsFileTime(&now);
        if (!GetProcessTimes(GetCurrentProcess(), &create, &exitT, &kernel, &user)) return q;
        const auto u = [](const FILETIME& f) { return (static_cast<int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        const int64_t ago100ns = u(now) - u(create);
        return ago100ns > 0 ? q - static_cast<int64_t>(static_cast<double>(ago100ns) * 1e-7 * static_cast<double>(Freq())) : q;
    }();
    return at;
}
double Sec(int64_t from, int64_t to) { return static_cast<double>(to - from) / static_cast<double>(Freq()); }
double SinceStart(int64_t at) { return Sec(ProcessStartQpc(), at); }

// events (0 = none); noted by hooks, read by the pump
std::atomic<int64_t> g_deviceAt{0}, g_featuresAt{0}, g_mapAt{0}, g_worldAt{0}, g_liveAt{0}, g_settledAt{0};
std::atomic<const char*> g_liveSignal{""}, g_settledWhy{""};
// room-light solves (always counted; the load window takes deltas)
std::atomic<uint64_t> c_solveStarts{0}, c_solveEnds{0}, c_solveTicks{0};
std::atomic<int64_t> g_solveStart{0};

struct Snapshot {
    uint64_t rpCalls = 0, rpIn = 0, rpOut = 0;
    double rpMs = 0.0;
    uint64_t dxtImages = 0, dxtIn = 0, dxtOut = 0;
    double dxtMs = 0.0;
    uint64_t rcHits = 0, rcMisses = 0, klHits = 0, klMisses = 0;
    uint64_t solveStarts = 0, solveEnds = 0, solveTicks = 0;
    double shaderWaitMs = 0.0;
    int shaderWaits = 0;
};
Snapshot Take() {
    Snapshot s;
    const FastRefPack::Stats rp = FastRefPack::GetStats();
    s.rpCalls = rp.streams + rp.countingRuns;
    s.rpIn = rp.bytesIn;
    s.rpOut = rp.bytesOut;
    s.rpMs = rp.ms + rp.countingMs;
    const FastDxt::Stats d = FastDxt::GetStats();
    s.dxtImages = d.images;
    s.dxtIn = d.pixels * 4;
    s.dxtOut = d.bytesOut;
    s.dxtMs = d.fastMs;
    const ResourceCache::Stats rc = ResourceCache::GetStats();
    s.rcHits = rc.hits;
    s.rcMisses = rc.misses;
    s.klHits = rc.klCached;
    s.klMisses = rc.klPassed;
    s.solveStarts = c_solveStarts.load(std::memory_order_relaxed);
    s.solveEnds = c_solveEnds.load(std::memory_order_relaxed);
    s.solveTicks = c_solveTicks.load(std::memory_order_relaxed);
    ShaderCache::RenderThreadWaits(&s.shaderWaits, &s.shaderWaitMs);
    return s;
}
double Mb(uint64_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); }
std::string Counters(const Snapshot& a, const Snapshot& b) {
    return std::format("FastRefPack {} calls, {:.0f} ms, {:.1f} MB in -> {:.1f} MB out; FastDxt {} images, {:.0f} ms, {:.1f} MB in -> {:.1f} MB out; "
                       "ResourceCache {} hits / {} misses; FileListCache {} hits / {} misses; room-light solves {} begun, {} ended, {:.0f} ms from begin to end "
                       "(budgeted over frames); shader precompile waits {} ({:.0f} ms)",
                       b.rpCalls - a.rpCalls, b.rpMs - a.rpMs, Mb(b.rpIn - a.rpIn), Mb(b.rpOut - a.rpOut), b.dxtImages - a.dxtImages, b.dxtMs - a.dxtMs,
                       Mb(b.dxtIn - a.dxtIn), Mb(b.dxtOut - a.dxtOut), b.rcHits - a.rcHits, b.rcMisses - a.rcMisses, b.klHits - a.klHits, b.klMisses - a.klMisses,
                       b.solveStarts - a.solveStarts, b.solveEnds - a.solveEnds, static_cast<double>(b.solveTicks - a.solveTicks) * 1000.0 / static_cast<double>(Freq()),
                       b.shaderWaits - a.shaderWaits, b.shaderWaitMs - a.shaderWaitMs);
}

// pump state (pump thread only)
enum class State { Idle, Loading, Live };
State g_state = State::Idle;
bool g_startupLogged = false;
int g_loadNo = 0;
int64_t g_lastEnd = 0;       // the last settled (or dropped) load's end: events before it are old
int64_t g_startAt = 0;       // this load's start
bool g_startByMap = false;   // started by the object map index alone (no world change yet)
bool g_worldSeen = false;
int64_t g_worldStamp = 0;   // the world change this load consumed
int64_t g_drawAt = 0;
Snapshot g_atStart, g_atDraw;
constexpr double kMapOnlyTimeout = 120.0; // an object map index with no world change after it: not a world load

void Begin(int64_t at, bool byMap, int64_t now) {
    g_state = State::Loading;
    g_loadNo++;
    g_startAt = at;
    g_startByMap = byMap;
    g_worldSeen = !byMap;
    g_drawAt = 0;
    g_atStart = Take();
    LOG_INFO(std::format("[LoadTiming] Load {}: world load started at {:.2f} s after process start ({}; seen {:.0f} ms later)", g_loadNo, SinceStart(at),
                         byMap ? "object map indexed" : "world change", Sec(at, now) * 1000.0));
}

} // namespace

std::string DiagnosticWork() { return Counters({}, Take()); }

void NoteDeviceCreated() {
    int64_t z = 0;
    g_deviceAt.compare_exchange_strong(z, Qpc());
}
void NoteFeaturesStarted() { g_featuresAt.store(Qpc()); }
void NoteObjectMapIndexed() { g_mapAt.store(Qpc(), std::memory_order_relaxed); }
void NoteWorldChange() { g_worldAt.store(Qpc(), std::memory_order_relaxed); }
void NoteFirstWorldDraw(const char* signal) {
    g_liveSignal.store(signal ? signal : "", std::memory_order_relaxed);
    g_liveAt.store(Qpc(), std::memory_order_release);
}
void NoteLoadSettled(const char* why) {
    g_settledWhy.store(why ? why : "", std::memory_order_relaxed);
    g_settledAt.store(Qpc(), std::memory_order_release);
}
void NoteRoomSolveStart() {
    c_solveStarts.fetch_add(1, std::memory_order_relaxed);
    g_solveStart.store(Qpc(), std::memory_order_relaxed);
}
void NoteRoomSolveEnd() {
    c_solveEnds.fetch_add(1, std::memory_order_relaxed);
    const int64_t s = g_solveStart.exchange(0, std::memory_order_relaxed);
    if (s) c_solveTicks.fetch_add(static_cast<uint64_t>(Qpc() - s), std::memory_order_relaxed);
}

void Pump() {
    ProcessStartQpc(); // converted once, early
    const int64_t now = Qpc();
    if (!g_startupLogged) {
        const int64_t dev = g_deviceAt.load(), feat = g_featuresAt.load();
        if (dev && feat) {
            g_startupLogged = true;
            LOG_INFO(std::format("[LoadTiming] Start-up: game device created {:.2f} s after process start, features started {:.2f} s ({:+.2f} s from the device)",
                                 SinceStart(dev), SinceStart(feat), Sec(dev, feat)));
        }
    }
    const int64_t mapAt = g_mapAt.load(std::memory_order_relaxed), worldAt = g_worldAt.load(std::memory_order_relaxed);
    const int64_t liveAt = g_liveAt.load(std::memory_order_acquire), settledAt = g_settledAt.load(std::memory_order_acquire);

    // a new world while one is loading or not settled: the next load begins (the previous one is closed unsettled)
    if (g_state != State::Idle && g_worldSeen && worldAt != g_worldStamp) {
        LOG_INFO(std::format("[LoadTiming] Load {}: another world load began before it settled ({:.2f} s after its start)", g_loadNo, Sec(g_startAt, worldAt)));
        g_state = State::Idle;
        g_lastEnd = worldAt - 1;
    }
    switch (g_state) {
    case State::Idle: {
        const bool world = worldAt > g_lastEnd, map = mapAt > g_lastEnd;
        if (world || map) {
            // the earliest of the two that is new; a map index long before the world change belongs to something else
            const bool useMap = map && (!world || (mapAt < worldAt && Sec(mapAt, worldAt) < kMapOnlyTimeout));
            Begin(useMap ? mapAt : worldAt, useMap && !world, now);
            if (world) g_worldStamp = worldAt;
            if (useMap && world) g_worldSeen = true;
            if (g_worldSeen && useMap)
                LOG_INFO(std::format("[LoadTiming] Load {}: world change seen {:.2f} s after the object map index", g_loadNo, Sec(mapAt, worldAt)));
        }
        break;
    }
    case State::Loading:
        if (!g_worldSeen && worldAt > g_startAt) {
            g_worldSeen = true;
            g_worldStamp = worldAt;
            LOG_INFO(std::format("[LoadTiming] Load {}: world change seen {:.2f} s after the load start", g_loadNo, Sec(g_startAt, worldAt)));
        }
        if (!g_worldSeen && Sec(g_startAt, now) > kMapOnlyTimeout) { // the object map was indexed again, no world load followed
            LOG_INFO(std::format("[LoadTiming] Load {}: no world change {:.0f} s after the object map index: not a world load, dropped", g_loadNo, kMapOnlyTimeout));
            g_loadNo--;
            g_state = State::Idle;
            g_lastEnd = now;
            break;
        }
        if (g_worldSeen && liveAt > g_startAt) {
            g_state = State::Live;
            g_drawAt = liveAt;
            g_atDraw = Take();
            LOG_INFO(std::format("[LoadTiming] Load {}: first world draw at {:.2f} s after process start ({}); load screen {:.2f} s", g_loadNo, SinceStart(liveAt),
                                 g_liveSignal.load(std::memory_order_relaxed), Sec(g_startAt, liveAt)));
            LOG_INFO(std::format("[LoadTiming] Load {}: during the load screen: {}", g_loadNo, Counters(g_atStart, g_atDraw)));
        }
        break;
    case State::Live:
        if (settledAt > g_drawAt) {
            const Snapshot end = Take();
            const int64_t dev = g_deviceAt.load(), feat = g_featuresAt.load();
            std::string phases;
            if (g_loadNo == 1 && dev && feat)
                phases = std::format("process start -> device {:.2f} s, device -> features {:.2f} s, features -> world load start {:.2f} s, ", SinceStart(dev),
                                     Sec(dev, feat), Sec(feat, g_startAt));
            LOG_INFO(std::format("[LoadTiming] Load {}: settled ({}) at {:.2f} s after process start; {}load screen {:.2f} s, first draw -> settled {:.2f} s; "
                                 "world load start -> settled {:.2f} s",
                                 g_loadNo, g_settledWhy.load(std::memory_order_relaxed), SinceStart(settledAt), phases, Sec(g_startAt, g_drawAt),
                                 Sec(g_drawAt, settledAt), Sec(g_startAt, settledAt)));
            LOG_INFO(std::format("[LoadTiming] Load {}: from the first draw to settled: {}", g_loadNo, Counters(g_atDraw, end)));
            g_state = State::Idle;
            g_lastEnd = settledAt > now ? settledAt : now;
        }
        break;
    }
}

} // namespace LoadTiming
