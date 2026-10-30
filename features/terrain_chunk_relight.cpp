// Local terrain relight: see terrain_chunk_relight.h.
//
// Evidence (TS3W.exe Steam 1.67.2; research\perf2\chunkrelight.md, the disassembly re-read on 29/09 for this file):
//  - The terrain update 0x00C845C0 walks the chunk vector terrain+0xB0/+0xB4. Per chunk, when no earlier chunk did work
//    in this call (byte [esp+0Ch], 0x00C84FDC), the LOD state is ready (0x00C853C0 on +0x60 and +0x5C), +0x55 == +0x56
//    == 0, [esp+28h] == 0 and +0x50 == 0, a chunk with +0x54 set is re-rendered by 0x00C7E7A0(chunk, 0) at 0x00C8504C
//    (the call Apex redirects to ChunkRenderThunk) and "work done" is set (0x00C85041..0x00C85056): exactly one chunk per
//    terrain update. The +0x55 branch (0x00C85058..0x00C850B5) instead rebuilds the geometry, re-renders the textures
//    synchronously through 0x00C83060 -> 0x00C7E7A0 (0x00C8307E), runs 0x00C7FA70, sets +0x54 (a second render in the
//    sweep) and marks the road partition, for EVERY flagged chunk in the same call (it never sets "work done"): the
//    ~240 ms frame of a full rebuild and the 12-60 ms hitches of the old local relight. Only +0x54 is ever set here.
//  - 0x00C7E7A0 returns early WITHOUT clearing +0x54 when (0x00C7E7E2..0x00C7E823) WorldManager (terrain+0x14)+0x1B4 != 0
//    and TerrainData (terrain+0x64)+0x1D == 0, or when 0x00C61040(WorldManager) (= [WM+0x54] ? [[WM+0x54]+8] : 0) != 0
//    and TerrainData+0x20 == 0. The sweep branch sets "work done" anyway, so such a chunk would stall every later chunk's
//    work, the game's own included: both gates are mirrored before a chunk is released, and a chunk that is still not
//    rendered after the timeout gets its +0x54 back to 0. The sweep branch itself is skipped (0x00C85011) while
//    byte [[TerrainData+0x0C]+0x6C] != 0 (read at 0x00C8471A..0x00C8473A; 0x0089F6C0 = mov eax,[ecx+0Ch]): mirrored too.
//  - A chunk whose LOD is not ready (0x00C84FF4 / 0x00C85003) takes the synchronous rebuild branch whatever +0x55 says: a
//    released chunk can then be rebuilt and rendered there, counted as "another game path". The +0x55 branch comes after
//    the "work done" test (0x00C84FDC), so it covers every flagged chunk only up to the first chunk that did work.
//  - Its render writes the light map +0xD8 in place (created only when null): a managed 256x256 DXT5 with 4 mips.
//  - Chunk layout (creation 0x00C815E0): nx = terrain+0xC0, nz = +0xC4, cell = +0xC8 = 256; slot = (z0/256)*nx + x0/256
//    (dense row-major vector); corner +0x04/+0x08, size +0x14/+0x18, centre +0x0C/+0x10 = corner + size/2, rect
//    +0x40..+0x4C = {x0, z0, x0+w, z0+h}; +0x54 = +0x50 = (WM+0x1B4 == 0), so in live play nothing re-renders until
//    something flags it.
//  - Bake record: grid = *(*(terrain+0x68)+8), record pointers +0xBC, width +0xCC, height +0xD0, index (centre >> 8);
//    record {x0, z0, w, h} (ints, read as unsigned); the bake draws a light when its rect +0x134 overlaps
//    [x0, x0+w] x [z0, z0+h] inclusively (0x00C29480..0x00C294C8).
//  - WorldManager global 0x011ECBC4 (GameAddr WorldManagerPtr); terrain = WM+0x58 (GameAddr TerrainUpdateCall, the
//    disp8 of "mov ecx,[esi+58h]; call 0x00C845C0" at 0x00C6D68C); the terrain's back pointer +0x14 is the WorldManager
//    (0x00C7E7E2, 0x00C84B03).
// Every read is guarded (SEH) and every layout fact is re-checked when it is used: any difference refuses the local path
// and the caller keeps the full rebuild.
#include "terrain_chunk_relight.h"
#include "terrain_lighting_policy.h"
#include "lightmap_smooth.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "game_addresses.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <utility>

namespace ChunkRelight {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kCell = 256;
constexpr float kMargin = 1.0f;         // metres around each lamp rect, on top of the game's own inclusive test
constexpr size_t kMaxBatchChunks = 16;  // a local relight that would hit more takes the full rebuild
constexpr size_t kMaxQueued = 32;       // local chunks waiting at once
constexpr size_t kMaxPerSecond = 8;     // baseline releases within any 1 s; measured cheap priority work has a small reserve
constexpr uint32_t kIdleTimeout = 120;  // frames in flight while no other chunk had sweep / rebuild work pending
constexpr uint32_t kHardTimeout = 1200; // frames in flight whatever else was pending
constexpr float kMaxRectSize = 4096.0f; // a lamp rect wider than this (or not finite, or inverted) is not trusted

// ---- resolved at Init ----
uintptr_t g_wmPtr = 0;
uint32_t g_terrainOff = 0;
bool g_resolved = false;
bool g_hooked = false;
std::string g_resolveInfo = "not resolved yet";
double g_ticksPerMs = 0.0;

// ---- handoff from ChunkRenderThunk (same thread in practice; atomics keep it safe regardless) ----
std::atomic<uintptr_t> g_seenTerrain{0}; // terrain of the last sweep render seen in this world
std::atomic<uintptr_t> g_flightChunk{0}; // the chunk Apex released, until it is rendered
std::atomic<bool> g_flightDone{false};
std::atomic<int64_t> g_flightTicks{0};
std::atomic<int> g_sweepRenders{0};

// ---- queue (render thread) ----
struct Entry {
    uintptr_t chunk = 0;
    uint32_t slot = 0;
    int ix = 0, iz = 0;
    std::vector<int> batches;
    bool urgent = false;
};
struct Batch {
    int id = 0;
    bool sweep = false;
    int chunks = 0, remaining = 0, timed = 0;
    uint32_t startFrame = 0;
    double ms = 0.0, maxMs = 0.0;
};
std::deque<Entry> g_queue;
Entry g_flight;
bool g_haveFlight = false, g_flightSetByUs = false;
uint32_t g_flightFrames = 0, g_flightIdle = 0;
std::vector<Batch> g_batches;
uintptr_t g_queueTerrain = 0, g_queueBegin = 0;
uint32_t g_frame = 0, g_lastDoneFrame = 0;
std::deque<Clock::time_point> g_releases;
int g_nextId = 1;
bool g_rebuiltThisWorld = false, g_offThisWorld = false;
int g_timeoutsThisWorld = 0; // in-flight timeouts in this world (the second one turns the local path off)
std::string g_offWhy;

// Paced full rebuild (QueueRebuild): one chunk's +0x55 at a time
constexpr uint32_t kRebuildTimeout = 600; // frames one chunk may keep its +0x55 (the branch waits behind other chunks' work)
struct Rebuild {
    bool active = false, flight = false;
    int id = 0;
    std::vector<Entry> order;
    size_t next = 0;
    Entry cur;
    uint32_t frames = 0, startFrame = 0, maxFrames = 0;
    uintptr_t terrain = 0, begin = 0;
};
Rebuild g_rb;
int g_statRebuilds = 0;

// ---- developer status ----
int g_statLocal = 0, g_statSweeps = 0, g_statChunks = 0, g_statOtherPath = 0, g_statRefused = 0, g_statFailed = 0;
int g_waitRebuildFlags = 0, g_waitGates = 0, g_waitRate = 0, g_timedChunks = 0;
double g_lastMs = -1.0, g_sumMs = 0.0, g_maxMs = 0.0;
std::string g_lastRefusal = "none", g_lastDone = "none";

// ---- guarded game memory access (POD only inside __try) ----
bool ReadCode(uintptr_t at, void* out, size_t n) {
    if (!at) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(at), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t ReadPtr(uintptr_t at) {
    uintptr_t v = 0;
    return ReadCode(at, &v, sizeof v) ? v : 0;
}

struct View {
    uintptr_t wm = 0, terrain = 0, td = 0, begin = 0;
    uint32_t n = 0, nx = 0, nz = 0, cell = 0, live = 0;
};

int ReadViewRaw(uintptr_t wmPtr, uint32_t terrainOff, View& v) {
    __try {
        v.wm = *reinterpret_cast<const uintptr_t*>(wmPtr);
        if (!v.wm) return 1;
        v.terrain = *reinterpret_cast<const uintptr_t*>(v.wm + terrainOff);
        if (!v.terrain) return 2;
        if (*reinterpret_cast<const uintptr_t*>(v.terrain + 0x14) != v.wm) return 3;
        v.live = *reinterpret_cast<const uint32_t*>(v.wm + 0x1B4);
        v.td = *reinterpret_cast<const uintptr_t*>(v.terrain + 0x64);
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(v.terrain + 0xB0);
        const uintptr_t e = *reinterpret_cast<const uintptr_t*>(v.terrain + 0xB4);
        if (!b || e <= b || ((e - b) & 3) != 0) return 4;
        v.begin = b;
        v.n = static_cast<uint32_t>((e - b) / 4);
        v.nx = *reinterpret_cast<const uint32_t*>(v.terrain + 0xC0);
        v.nz = *reinterpret_cast<const uint32_t*>(v.terrain + 0xC4);
        v.cell = *reinterpret_cast<const uint32_t*>(v.terrain + 0xC8);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 5;
    }
}

bool ReadView(View& v, std::string& why) {
    if (!g_resolved) {
        why = g_resolveInfo;
        return false;
    }
    v = View{};
    switch (ReadViewRaw(g_wmPtr, g_terrainOff, v)) {
    case 0:
        break;
    case 1:
        why = "no WorldManager";
        return false;
    case 2:
        why = "no terrain";
        return false;
    case 3:
        why = "the terrain's WorldManager link (+0x14) differs";
        return false;
    case 4:
        why = "the terrain chunk list is empty or unreadable";
        return false;
    default:
        why = "the terrain could not be read";
        return false;
    }
    if (v.cell != kCell || v.nx == 0 || v.nz == 0 || v.nx > 1024 || v.nz > 1024 || v.n != v.nx * v.nz) {
        why = std::format("the terrain grid is not as studied (nx {}, nz {}, cell {}, {} chunks)", v.nx, v.nz, v.cell, v.n);
        return false;
    }
    return true;
}

struct ChunkRaw {
    uint32_t x0 = 0, z0 = 0, cx = 0, cz = 0, w = 0, h = 0;
    int32_t r[4] = {};
    uintptr_t lightMap = 0;
    uint8_t f50 = 0, f54 = 0, f55 = 0, f56 = 0;
};

bool ReadChunkRaw(uintptr_t p, ChunkRaw& c) {
    if (!p) return false;
    __try {
        const BYTE* b = reinterpret_cast<const BYTE*>(p);
        c.x0 = *reinterpret_cast<const uint32_t*>(b + 0x04);
        c.z0 = *reinterpret_cast<const uint32_t*>(b + 0x08);
        c.cx = *reinterpret_cast<const uint32_t*>(b + 0x0C);
        c.cz = *reinterpret_cast<const uint32_t*>(b + 0x10);
        c.w = *reinterpret_cast<const uint32_t*>(b + 0x14);
        c.h = *reinterpret_cast<const uint32_t*>(b + 0x18);
        std::memcpy(c.r, b + 0x40, sizeof c.r);
        c.f50 = b[0x50];
        c.f54 = b[0x54];
        c.f55 = b[0x55];
        c.f56 = b[0x56];
        c.lightMap = *reinterpret_cast<const uintptr_t*>(b + 0xD8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The chunk at `slot` has exactly the layout of 0x00C815E0 (see the header comment)
bool LayoutOk(const View& v, uint32_t slot, const ChunkRaw& c) {
    if (c.x0 % kCell != 0 || c.z0 % kCell != 0 || c.w != kCell || c.h != kCell) return false;
    if (c.x0 / kCell >= v.nx || c.z0 / kCell >= v.nz || (c.z0 / kCell) * v.nx + c.x0 / kCell != slot) return false;
    if (c.cx != c.x0 + kCell / 2 || c.cz != c.z0 + kCell / 2) return false;
    return c.r[0] == static_cast<int32_t>(c.x0) && c.r[1] == static_cast<int32_t>(c.z0) && c.r[2] == static_cast<int32_t>(c.x0 + kCell) &&
           c.r[3] == static_cast<int32_t>(c.z0 + kCell);
}

// The chunk's bake record rect in world units, converted like the bake does (ints read as unsigned)
bool BakeRectRaw(uintptr_t terrain, uint32_t cx, uint32_t cz, float out[4]) {
    __try {
        const uintptr_t comp = *reinterpret_cast<const uintptr_t*>(terrain + 0x68);
        if (!comp) return false;
        const uintptr_t grid = *reinterpret_cast<const uintptr_t*>(comp + 0x08);
        if (!grid) return false;
        const uintptr_t cells = *reinterpret_cast<const uintptr_t*>(grid + 0xBC);
        const uint32_t gnx = *reinterpret_cast<const uint32_t*>(grid + 0xCC);
        const uint32_t gnz = *reinterpret_cast<const uint32_t*>(grid + 0xD0);
        const uint32_t ix = cx >> 8, iz = cz >> 8;
        if (!cells || ix >= gnx || iz >= gnz || gnx > 1024 || gnz > 1024) return false;
        const uintptr_t rec = reinterpret_cast<const uintptr_t*>(cells)[iz * gnx + ix];
        if (!rec) return false;
        const int32_t* r = reinterpret_cast<const int32_t*>(rec);
        if (r[2] <= 0 || r[3] <= 0) return false;
        out[0] = static_cast<float>(static_cast<uint32_t>(r[0]));
        out[1] = static_cast<float>(static_cast<uint32_t>(r[1]));
        out[2] = static_cast<float>(static_cast<uint32_t>(r[0] + r[2]));
        out[3] = static_cast<float>(static_cast<uint32_t>(r[1] + r[3]));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 0 when the sweep branch would run and 0x00C7E7A0 would render the chunk (and clear +0x54) rather than skip it or return
// early with +0x54 still set
int GatesRaw(const View& v) {
    __try {
        if (v.live != 0 && (!v.td || *reinterpret_cast<const uint8_t*>(v.td + 0x1D) == 0)) return 1;
        const uintptr_t sub = *reinterpret_cast<const uintptr_t*>(v.wm + 0x54); // 0x00C61040
        const uint32_t c = sub ? *reinterpret_cast<const uint32_t*>(sub + 8) : 0;
        if (c != 0 && v.td && *reinterpret_cast<const uint8_t*>(v.td + 0x20) == 0) return 2;
        const uintptr_t tool = v.td ? *reinterpret_cast<const uintptr_t*>(v.td + 0x0C) : 0; // 0x00C8471A: the sweep branch is skipped
        if (tool && *reinterpret_cast<const uint8_t*>(tool + 0x6C) != 0) return 4;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 3;
    }
}

// What the terrain update has pending on the other chunks
struct Pending {
    uint32_t f55 = 0, f56 = 0, other54 = 0, f50 = 0;
    bool fault = false;
};
void ScanFlagsRaw(uintptr_t begin, uint32_t n, uintptr_t except, Pending& p) {
    __try {
        for (uint32_t i = 0; i < n; i++) {
            const BYTE* c = reinterpret_cast<const BYTE*>(reinterpret_cast<const uintptr_t*>(begin)[i]);
            if (!c) continue;
            if (c[0x55]) p.f55++;
            if (c[0x56]) p.f56++;
            if (c[0x50]) p.f50++;
            if (c[0x54] && reinterpret_cast<uintptr_t>(c) != except) p.other54++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        p.fault = true;
    }
}

// Writes chunk+0x54; returns the previous value, or -1 when the write faulted
int WriteFlag54(uintptr_t chunk, uint8_t value) {
    __try {
        BYTE* b = reinterpret_cast<BYTE*>(chunk);
        const int prev = b[0x54];
        b[0x54] = value;
        return prev;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int WriteFlag55(uintptr_t chunk, uint8_t value) {
    __try {
        BYTE* b = reinterpret_cast<BYTE*>(chunk);
        const int prev = b[0x55];
        b[0x55] = value;
        return prev;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool RectOk(const float* r) {
    for (int k = 0; k < 4; k++)
        if (!std::isfinite(r[k])) return false;
    return r[0] <= r[2] && r[1] <= r[3] && r[2] - r[0] <= kMaxRectSize && r[3] - r[1] <= kMaxRectSize;
}

int Refuse(const std::string& why) {
    g_statRefused++;
    g_lastRefusal = why;
    return 0;
}

void ClearQueue() {
    g_queue.clear();
    g_batches.clear();
    g_flight = Entry{};
    g_haveFlight = false;
    g_flightSetByUs = false;
    g_flightChunk.store(0, std::memory_order_release);
    g_flightDone.store(false, std::memory_order_release);
}

bool Queued(uintptr_t chunk) {
    if (g_haveFlight && g_flight.chunk == chunk) return true;
    return std::any_of(g_queue.begin(), g_queue.end(), [chunk](const Entry& e) { return e.chunk == chunk; });
}

// Attaches `id` to the entry of `e.chunk` (in flight or queued), or queues `e` for it. Returns false when it was attached.
bool Attach(Entry e, int id, bool urgent = false, bool fresh = false) {
    // An edit may arrive after the in-flight bake read its lamps. Urgent work must run again,
    // rather than being declared complete when that older bake finishes.
    if (!urgent && !fresh && g_haveFlight && g_flight.chunk == e.chunk) {
        g_flight.batches.push_back(id);
        return false;
    }
    for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
        if (it->chunk == e.chunk) {
            it->batches.push_back(id);
            if (urgent) {
                Entry moved = std::move(*it);
                moved.urgent = true;
                g_queue.erase(it);
                g_queue.push_front(std::move(moved));
            }
            return false;
        }
    e.batches = {id};
    e.urgent = urgent;
    if (urgent) g_queue.push_front(std::move(e));
    else g_queue.push_back(std::move(e));
    return true;
}

// Common prerequisites; `v` is the terrain as read now
bool Check(View& v, std::string& why) {
    if (!g_resolved) {
        why = g_resolveInfo;
        return false;
    }
    if (!g_hooked) {
        why = "the chunk re-render call (0x00C8504C) is not hooked";
        return false;
    }
    if (g_offThisWorld) {
        why = "off for this world after: " + g_offWhy;
        return false;
    }
    if (!g_rebuiltThisWorld) {
        why = "the world's first terrain rebuild has not run yet";
        return false;
    }
    if (!ReadView(v, why)) return false;
    if (v.live == 0) {
        why = "not in live play (engine tool mode)";
        return false;
    }
    if (g_seenTerrain.load(std::memory_order_acquire) != v.terrain) {
        why = "no chunk re-render of this terrain seen yet";
        return false;
    }
    if ((g_haveFlight || !g_queue.empty()) && (v.terrain != g_queueTerrain || v.begin != g_queueBegin)) {
        why = "the terrain changed under the queue";
        return false;
    }
    if (const int gate = GatesRaw(v)) {
        const char* detail = gate == 1 ? "terrain data +0x1D is not ready" :
                             gate == 2 ? "terrain data +0x20 is not ready" :
                             gate == 4 ? "terrain tool +0x6C blocks the sweep" : "terrain gate read failed";
        why = std::string("the terrain is not ready for texture renders: ") + detail;
        return false;
    }
    return true;
}

double TicksToMs(int64_t ticks) { return g_ticksPerMs > 0.0 ? static_cast<double>(ticks) / g_ticksPerMs : -1.0; }

void FinishFlight(bool viaThunk) {
    // The LOD rebuild can clear the flag without going through ChunkRenderThunk.
    // Notify the same consumers before declaring that local chunk complete.
    if (!viaThunk) LightmapSmooth::NoteChunkRendered(g_flight.ix, g_flight.iz);
    const double ms = viaThunk ? TicksToMs(g_flightTicks.load(std::memory_order_acquire)) : -1.0;
    g_flightChunk.store(0, std::memory_order_release);
    g_haveFlight = false;
    g_lastDoneFrame = g_frame;
    g_statChunks++;
    if (!viaThunk) g_statOtherPath++;
    if (ms >= 0.0) {
        g_lastMs = ms;
        g_sumMs += ms;
        g_timedChunks++;
        g_maxMs = std::max(g_maxMs, ms);
    }
    for (int id : g_flight.batches)
        for (Batch& b : g_batches)
            if (b.id == id) {
                b.remaining--;
                if (ms >= 0.0) {
                    b.ms += ms;
                    b.timed++;
                    b.maxMs = std::max(b.maxMs, ms);
                }
            }
    g_flight = Entry{};
}

void FlushDone(FrameResult& out) {
    for (auto it = g_batches.begin(); it != g_batches.end();) {
        if (it->remaining > 0) {
            ++it;
            continue;
        }
        Done d;
        d.id = it->id;
        d.sweep = it->sweep;
        d.text = std::format("{} chunk{} in {} frames", it->chunks, it->chunks == 1 ? "" : "s", g_frame - it->startFrame);
        if (it->timed > 0) d.text += std::format(", {:.2f} ms per chunk (max {:.2f})", it->ms / it->timed, it->maxMs);
        g_lastDone = (d.sweep ? "sweep: " : "local: ") + d.text;
        out.done.push_back(std::move(d));
        it = g_batches.erase(it);
    }
}

// keepOn: only this batch falls back to a full rebuild (a first timeout in the world)
void Fail(FrameResult& out, const std::string& why, bool keepOn = false) {
    ClearQueue();
    if (!keepOn) {
        g_offThisWorld = true;
        g_offWhy = why;
    }
    g_statFailed++;
    out.failed = true;
    out.why = why;
    LOG_WARNING("[ChunkRelight] " + why + (keepOn ? ": queue dropped, one full rebuild instead (a second time turns the local relight off for this world)"
                                                  : ": queue dropped, local terrain relight off for this world (full rebuilds instead)"));
}

} // namespace

void Init() {
    g_resolved = false;
    g_wmPtr = GameAddr::Get(GameAddr::Id::WorldManagerPtr);
    const uintptr_t site = GameAddr::Get(GameAddr::Id::TerrainUpdateCall);
    BYTE b[4] = {};
    if (!g_wmPtr || !site)
        g_resolveInfo = "the WorldManager access was not found on this game version";
    else if (!ReadCode(site, b, sizeof b) || b[0] != 0x8B || b[1] != 0x4E || b[3] != 0xE8) // mov ecx,[esi+disp8]; call
        g_resolveInfo = std::format("the terrain link differs at {:#010x}", site);
    else {
        g_terrainOff = b[2];
        g_resolved = true;
        g_resolveInfo = std::format("WorldManager global {:#010x}, terrain at +{:#x}", g_wmPtr, g_terrainOff);
    }
    LARGE_INTEGER f{};
    g_ticksPerMs = QueryPerformanceFrequency(&f) && f.QuadPart > 0 ? static_cast<double>(f.QuadPart) / 1000.0 : 0.0;
    if (!kPublicBuild) LOG_INFO("[ChunkRelight] " + g_resolveInfo);
}

void SetHooked(bool hooked) { g_hooked = hooked; }

void OnChunkRendered(void* terrain, const void* chunk, bool rendered, int64_t ticks) {
    if (!rendered) return;
    g_seenTerrain.store(reinterpret_cast<uintptr_t>(terrain), std::memory_order_release);
    g_sweepRenders.fetch_add(1, std::memory_order_relaxed);
    const uintptr_t c = reinterpret_cast<uintptr_t>(chunk);
    if (c && c == g_flightChunk.load(std::memory_order_acquire)) {
        g_flightTicks.store(ticks, std::memory_order_release);
        g_flightDone.store(true, std::memory_order_release);
    }
}

void OnWorldChanged() {
    ClearQueue();
    g_rb = Rebuild{};
    g_rebuiltThisWorld = false;
    g_offThisWorld = false;
    g_offWhy.clear();
    g_timeoutsThisWorld = 0;
    g_seenTerrain.store(0, std::memory_order_release);
    g_releases.clear();
    g_lastMs = -1.0; // the priority reserve needs a measurement from this world
}

bool Drop() {
    const bool had = g_rb.active || g_haveFlight || !g_queue.empty() || !g_batches.empty();
    g_rb = Rebuild{}; // a chunk already flagged +0x55 is still rebuilt by the game
    ClearQueue();
    return had;
}

bool OnFullRebuild() {
    g_rebuiltThisWorld = true;
    const bool rebuilding = g_rb.active; // the game flagged every chunk: a paced rebuild is covered (its flag is consumed too)
    g_rb = Rebuild{};
    return Drop() || rebuilding;
}

bool RebuildActive() { return g_rb.active; }

bool LikelyAvailable() { return g_resolved && g_hooked && g_rebuiltThisWorld && !g_offThisWorld; }

namespace {
// The texture `tex` is the object +0xD8 holds or one of its first 32 dwords (the game's texture holder); SEH. Once the same
// offset matched in two holders it is taken as the holder's texture field and is the only one read afterwards: past the
// holder's end the scan could reach a neighbouring object holding another texture.
int g_holderOffset = 0, g_holderSeen = 0; // (g_holderSeen: the offset matched once, in g_holderFirst)
uintptr_t g_holderFirst = 0;
int ScanHolder(uintptr_t lm, uintptr_t tex) {
    __try {
        const auto at = reinterpret_cast<const uintptr_t*>(lm);
        if (g_holderOffset) return at[g_holderOffset / 4] == tex ? g_holderOffset : -1;
        for (int k = 1; k < 32; k++)
            if (at[k] == tex) return k * 4;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}
int MatchRebuilt(uintptr_t lm, uintptr_t tex) {
    if (lm == tex) return 0;
    const int found = ScanHolder(lm, tex);
    if (found > 0 && !g_holderOffset) {
        if (found != g_holderSeen) g_holderSeen = found, g_holderFirst = lm;
        else if (lm != g_holderFirst) {
            g_holderOffset = found;
            LOG_INFO(std::format("[ChunkRelight] Rebuilt light map: the texture is at +0x{:X} of the object +0xD8 holds", found));
        }
    }
    return found;
}
} // namespace
int RebuiltLightMapMatch(int centreX, int centreZ, const void* texture) {
    // (only chunk centres: another position would be floored into the chunk around it)
    if (!texture || centreX < static_cast<int>(kCell / 2) || centreZ < static_cast<int>(kCell / 2) || (centreX - static_cast<int>(kCell / 2)) % static_cast<int>(kCell) ||
        (centreZ - static_cast<int>(kCell / 2)) % static_cast<int>(kCell))
        return -2;
    View v;
    std::string why;
    if (!ReadView(v, why)) return -2;
    const uint32_t ix = static_cast<uint32_t>(centreX - kCell / 2) / kCell, iz = static_cast<uint32_t>(centreZ - kCell / 2) / kCell;
    if (ix >= v.nx || iz >= v.nz) return -2;
    const uint32_t slot = iz * v.nx + ix;
    ChunkRaw c;
    if (slot >= v.n || !ReadChunkRaw(ReadPtr(v.begin + 4 * static_cast<uintptr_t>(slot)), c) || !LayoutOk(v, slot, c) || !c.lightMap) return -2;
    return MatchRebuilt(c.lightMap, reinterpret_cast<uintptr_t>(texture));
}

bool Editing() {
    __try {
        if (!g_wmPtr) return false;
        const uintptr_t wm = *reinterpret_cast<const uintptr_t*>(g_wmPtr);
        return wm && *reinterpret_cast<const uint32_t*>(wm + 0x1B4) == 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Busy() { return g_rb.active || g_haveFlight || !g_queue.empty() || !g_batches.empty(); }

// A chunk render that took long (06/10, a session: 44 ms a chunk while the game's whole frames took 72-164 ms; the same
// 9 lamps and 4 chunks took 4.7-6 ms in the sessions after: the render probably waits for the GPU, so it costs about what a
// frame costs then): at most kSlowPerSecond such renders a second, so a lamp edit does not stack heavy frames back to back
constexpr size_t kSlowPerSecond = 3;
constexpr double kSlowMs = 20.0;
size_t ReleaseLimit(bool urgent, double recentMs) {
    if (recentMs > kSlowMs) return kSlowPerSecond;
    // Small reserve for an interactive edit behind a sweep, only after measured
    // chunk costs are low. Unknown/expensive chunks retain the original limit.
    return urgent && recentMs >= 0.0 && recentMs <= 12.0 ? kMaxPerSecond + 4 : kMaxPerSecond;
}

int QueueLocal(const std::vector<Lamp>& lamps, std::string& why, std::string& chunks, bool urgent, std::vector<std::pair<int, int>>* cells) {
    View v;
    if (!Check(v, why)) return Refuse(why);
    std::vector<Entry> hits; // relight order: each lamp's own chunk, then its other chunks nearest first
    for (const Lamp& L : lamps) {
        std::vector<std::pair<float, Entry>> mine;
        const long ownX = static_cast<long>(std::floor(L.x / static_cast<float>(kCell)));
        const long ownZ = static_cast<long>(std::floor(L.z / static_cast<float>(kCell)));
        for (int k = 0; k < L.rects && k < 2; k++) {
            const float* R = L.rect[k];
            if (!RectOk(R)) {
                why = std::format("a lamp rect is not usable ({:.1f}, {:.1f}, {:.1f}, {:.1f})", R[0], R[1], R[2], R[3]);
                return Refuse(why);
            }
            // candidate cells: the rect +1 m, one more cell on each side (a bake record may reach past its cell)
            const long x0 = std::max(0L, static_cast<long>(std::floor((R[0] - kMargin) / static_cast<float>(kCell))) - 1);
            const long x1 = std::min(static_cast<long>(v.nx) - 1, static_cast<long>(std::floor((R[2] + kMargin) / static_cast<float>(kCell))) + 1);
            const long z0 = std::max(0L, static_cast<long>(std::floor((R[1] - kMargin) / static_cast<float>(kCell))) - 1);
            const long z1 = std::min(static_cast<long>(v.nz) - 1, static_cast<long>(std::floor((R[3] + kMargin) / static_cast<float>(kCell))) + 1);
            for (long iz = z0; iz <= z1; iz++)
                for (long ix = x0; ix <= x1; ix++) {
                    const uint32_t slot = static_cast<uint32_t>(iz) * v.nx + static_cast<uint32_t>(ix);
                    if (std::any_of(mine.begin(), mine.end(), [slot](const auto& m) { return m.second.slot == slot; })) continue;
                    const uintptr_t ch = ReadPtr(v.begin + 4 * static_cast<uintptr_t>(slot));
                    ChunkRaw c;
                    if (!ReadChunkRaw(ch, c) || !LayoutOk(v, slot, c)) return Refuse(why = std::format("the chunk at ({},{}) is not laid out as studied", ix, iz));
                    float br[4];
                    if (!BakeRectRaw(v.terrain, c.cx, c.cz, br)) { // no bake record: the chunk rect
                        br[0] = static_cast<float>(c.x0);
                        br[1] = static_cast<float>(c.z0);
                        br[2] = static_cast<float>(c.x0 + kCell);
                        br[3] = static_cast<float>(c.z0 + kCell);
                    }
                    const bool overlaps = R[0] - kMargin <= br[2] && br[0] <= R[2] + kMargin && R[1] - kMargin <= br[3] && br[1] <= R[3] + kMargin;
                    if (!overlaps) continue;
                    if (!c.lightMap) return Refuse(why = std::format("the chunk at ({},{}) has no rebuilt light map yet", ix, iz));
                    Entry e;
                    e.chunk = ch;
                    e.slot = slot;
                    e.ix = static_cast<int>(ix);
                    e.iz = static_cast<int>(iz);
                    const float dx = static_cast<float>(c.cx) - L.x, dz = static_cast<float>(c.cz) - L.z;
                    mine.emplace_back(ix == ownX && iz == ownZ ? -1.0f : dx * dx + dz * dz, std::move(e));
                }
        }
        std::stable_sort(mine.begin(), mine.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& m : mine) {
            const uintptr_t chunk = m.second.chunk;
            if (std::none_of(hits.begin(), hits.end(), [chunk](const Entry& h) { return h.chunk == chunk; })) hits.push_back(std::move(m.second));
        }
    }
    if (hits.size() > kMaxBatchChunks || hits.size() * 4 > v.n)
        return Refuse(why = std::format("{} chunks would be relit (at most {} and a quarter of the world's {})", hits.size(), kMaxBatchChunks, v.n));
    size_t fresh = 0;
    for (const Entry& h : hits) {
        const bool inQueue = std::any_of(g_queue.begin(), g_queue.end(), [&](const Entry& e) { return e.chunk == h.chunk; });
        fresh += (urgent ? inQueue : Queued(h.chunk)) ? 0 : 1;
    }
    const bool sweep = std::any_of(g_batches.begin(), g_batches.end(), [](const Batch& b) { return b.sweep; });
    const size_t limit = urgent && sweep ? static_cast<size_t>(v.n) + kMaxBatchChunks : kMaxQueued;
    if (g_queue.size() + fresh > limit) return Refuse(why = std::format("the local queue is full ({} chunks waiting)", g_queue.size()));
    if (!g_haveFlight && g_queue.empty()) {
        g_queueTerrain = v.terrain;
        g_queueBegin = v.begin;
    }
    Batch b;
    b.id = g_nextId++;
    b.chunks = b.remaining = static_cast<int>(hits.size());
    b.startFrame = g_frame;
    g_batches.push_back(b);
    chunks.clear();
    if (cells) cells->clear();
    for (const Entry& h : hits) {
        chunks += std::format("{}({},{})", chunks.empty() ? "" : " ", h.ix, h.iz);
        if (cells) cells->emplace_back(h.ix, h.iz);
    }
    // push_front reverses insertion order: queue the farthest first to keep the lamp's own chunk first.
    if (urgent) for (auto it = hits.rbegin(); it != hits.rend(); ++it) Attach(std::move(*it), b.id, true);
    else for (Entry& h : hits) Attach(std::move(h), b.id);
    if (chunks.empty()) chunks = "none overlaps";
    g_statLocal++;
    return b.id;
}

int QueueSweep(const float* eyeXZ, std::string& why, std::string& info, bool interactive) {
    View v;
    if (!Check(v, why)) return Refuse(why);
    std::vector<std::pair<float, Entry>> all;
    all.reserve(v.n);
    for (uint32_t slot = 0; slot < v.n; slot++) {
        const uintptr_t ch = ReadPtr(v.begin + 4 * static_cast<uintptr_t>(slot));
        ChunkRaw c;
        const int ix = static_cast<int>(slot % v.nx), iz = static_cast<int>(slot / v.nx);
        if (!ReadChunkRaw(ch, c) || !LayoutOk(v, slot, c)) return Refuse(why = std::format("the chunk at ({},{}) is not laid out as studied", ix, iz));
        if (!c.lightMap) return Refuse(why = std::format("the chunk at ({},{}) has no rebuilt light map yet", ix, iz));
        Entry e;
        e.chunk = ch;
        e.slot = slot;
        e.ix = ix;
        e.iz = iz;
        float d = static_cast<float>(slot);
        if (eyeXZ) {
            const float dx = static_cast<float>(c.cx) - eyeXZ[0], dz = static_cast<float>(c.cz) - eyeXZ[1];
            d = dx * dx + dz * dz;
        }
        all.emplace_back(d, std::move(e));
    }
    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    // This is a new lamp/phase state. A bake already in flight may have read the
    // previous state: finish it safely, but queue it again for the new batch.
    g_queue.clear();
    g_batches.clear();
    if (g_haveFlight) g_flight.batches.clear();
    else {
        g_queueTerrain = v.terrain;
        g_queueBegin = v.begin;
    }
    Batch b;
    b.id = g_nextId++;
    b.sweep = true;
    b.chunks = b.remaining = static_cast<int>(all.size());
    b.startFrame = g_frame;
    g_batches.push_back(b);
    const size_t priority = TerrainLightingPolicy::PreviewPriorityChunks(all.size(), interactive, eyeXZ != nullptr);
    for (size_t i = priority; i < all.size(); ++i) Attach(std::move(all[i].second), b.id, false, true);
    // Attach pushes priority work to the front: insert in reverse to retain the
    // nearest-first order. The existing measured-cost reserve and gates apply.
    for (size_t i = priority; i > 0; --i) Attach(std::move(all[i - 1].second), b.id, true, true);
    info = std::format("{} chunks, {}", all.size(), eyeXZ ? "nearest to the camera first" : "in grid order");
    if (priority) info += std::format("; {} nearby chunks prioritized for Build preview", priority);
    g_statSweeps++;
    return b.id;
}

int QueueRebuild(const float* eyeXZ, std::string& why, std::string& info) {
    if (!g_resolved) return Refuse(why = g_resolveInfo);
    if (g_rb.active) return Refuse(why = "a paced rebuild is already running");
    View v;
    if (!ReadView(v, why)) return Refuse(why);
    if (v.live == 0) return Refuse(why = "not in live play (engine tool mode)");
    std::vector<std::pair<float, Entry>> all;
    all.reserve(v.n);
    for (uint32_t slot = 0; slot < v.n; slot++) {
        const uintptr_t ch = ReadPtr(v.begin + 4 * static_cast<uintptr_t>(slot));
        ChunkRaw c;
        const int ix = static_cast<int>(slot % v.nx), iz = static_cast<int>(slot / v.nx);
        if (!ReadChunkRaw(ch, c) || !LayoutOk(v, slot, c)) return Refuse(why = std::format("the chunk at ({},{}) is not laid out as studied", ix, iz));
        if (c.f55) return Refuse(why = std::format("the chunk at ({},{}) already waits for a rebuild", ix, iz));
        Entry e;
        e.chunk = ch;
        e.slot = slot;
        e.ix = ix;
        e.iz = iz;
        float d = static_cast<float>(slot);
        if (eyeXZ) {
            const float dx = static_cast<float>(c.cx) - eyeXZ[0], dz = static_cast<float>(c.cz) - eyeXZ[1];
            d = dx * dx + dz * dz;
        }
        all.emplace_back(d, std::move(e));
    }
    if (all.empty()) return Refuse(why = "the terrain has no chunks");
    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    ClearQueue(); // every chunk is rebuilt and re-rendered: pending +0x54 work is covered (a chunk in flight is left to the game)
    g_rb = Rebuild{};
    g_rb.active = true;
    g_rb.id = g_nextId++;
    g_rb.order.reserve(all.size());
    for (auto& a : all) g_rb.order.push_back(std::move(a.second));
    g_rb.startFrame = g_frame;
    g_rb.terrain = v.terrain;
    g_rb.begin = v.begin;
    g_statRebuilds++;
    info = std::format("{} chunks, {}", all.size(), eyeXZ ? "nearest to the camera first" : "in grid order");
    return g_rb.id;
}

namespace {
void FailRebuild(FrameResult& out, const std::string& why) {
    g_rb = Rebuild{};
    g_statFailed++;
    out.failed = true;
    out.why = why;
    LOG_WARNING("[ChunkRelight] Paced rebuild: " + why + ": one full rebuild instead");
}

// One step of the paced rebuild: the chunk in flight done (+0x55 cleared by the game), then the next one flagged
void StepRebuild(FrameResult& out) {
    View v;
    std::string why;
    if (!ReadView(v, why)) return FailRebuild(out, "the terrain could not be read (" + why + ")");
    if (v.terrain != g_rb.terrain || v.begin != g_rb.begin) return FailRebuild(out, "the terrain changed under it");
    if (g_rb.flight) {
        ChunkRaw c;
        if (!ReadChunkRaw(g_rb.cur.chunk, c)) return FailRebuild(out, "a chunk could not be read");
        if (c.f55 != 0) {
            if (++g_rb.frames > kRebuildTimeout)
                return FailRebuild(out, std::format("the chunk at ({},{}) was not rebuilt within {} frames", g_rb.cur.ix, g_rb.cur.iz, kRebuildTimeout));
            return;
        }
        g_rb.flight = false;
        g_rb.maxFrames = std::max(g_rb.maxFrames, g_rb.frames);
        g_statChunks++;
    }
    if (g_rb.next >= g_rb.order.size()) {
        Done d;
        d.id = g_rb.id;
        d.rebuild = true;
        d.text = std::format("{} chunks in {} frames (a chunk waited at most {} frames)", g_rb.order.size(), g_frame - g_rb.startFrame, g_rb.maxFrames);
        g_lastDone = "rebuild: " + d.text;
        out.done.push_back(std::move(d));
        g_rb = Rebuild{};
        g_rebuiltThisWorld = true; // as a consumed full rebuild: every chunk has its rebuilt light map now
        return;
    }
    const Entry& e = g_rb.order[g_rb.next];
    ChunkRaw c;
    if (ReadPtr(v.begin + 4 * static_cast<uintptr_t>(e.slot)) != e.chunk || !ReadChunkRaw(e.chunk, c) || !LayoutOk(v, e.slot, c))
        return FailRebuild(out, std::format("the chunk at ({},{}) changed while it was queued", e.ix, e.iz));
    if (WriteFlag55(e.chunk, 1) < 0) return FailRebuild(out, std::format("the chunk at ({},{}) could not be flagged", e.ix, e.iz));
    g_rb.cur = e;
    g_rb.next++;
    g_rb.flight = true;
    g_rb.frames = 0;
}
} // namespace

void OnPresent(FrameResult& out) {
    g_frame++;
    if (g_rb.active) return StepRebuild(out); // the +0x54 queue waits (it was cleared when the rebuild started)
    if (!g_haveFlight && g_queue.empty()) {
        FlushDone(out);
        return;
    }
    View v;
    std::string why;
    if (!ReadView(v, why)) return Fail(out, "the terrain could not be read under the queue (" + why + ")");
    if (v.terrain != g_queueTerrain || v.begin != g_queueBegin) return Fail(out, "the terrain changed under the queue");
    Pending p;
    ScanFlagsRaw(v.begin, v.n, g_haveFlight ? g_flight.chunk : 0, p);
    if (p.fault) return Fail(out, "the terrain chunks could not be read");

    // 1. The chunk in flight: rendered through the redirected call (timed), or by another game path (0x00C83060 during a
    //    LOD change or a rebuild clears +0x54 too)?
    if (g_haveFlight) {
        const bool viaThunk = g_flightDone.exchange(false, std::memory_order_acq_rel);
        bool done = viaThunk;
        ChunkRaw c;
        if (!done && ReadChunkRaw(g_flight.chunk, c) && c.f54 == 0) done = true;
        if (done) FinishFlight(viaThunk);
        else {
            g_flightFrames++;
            // nothing else ahead of it, and nothing that makes the game skip it on purpose
            if (p.f55 == 0 && p.f56 == 0 && p.other54 == 0 && p.f50 == 0 && GatesRaw(v) == 0) g_flightIdle++;
            if (g_flightIdle > kIdleTimeout || g_flightFrames > kHardTimeout) {
                const Entry f = g_flight;
                // never leave a flag the game will not clear: the sweep branch would stall every later chunk
                if (g_flightSetByUs && p.f55 == 0 && p.f56 == 0) WriteFlag54(f.chunk, 0);
                // Frames count Presents, not terrain updates: a first timeout in a world may be a stretch without terrain
                // updates, so it falls back to one full rebuild and keeps the local path; a second one turns it off.
                return Fail(out, std::format("the chunk at ({},{}) was not re-rendered within {} frames ({} with nothing else pending)", f.ix, f.iz, g_flightFrames,
                                             g_flightIdle),
                            ++g_timeoutsThisWorld < 2);
            }
        }
    }
    FlushDone(out);
    if (g_haveFlight || g_queue.empty()) return;

    // 2. Release the next chunk: never in the frame right after a render (a free frame in between), baseline 8 per second,
    //    with four extra slots only for urgent work after a measured render of at most 12 ms;
    //    never while a full rebuild's +0x55 / +0x56 work is pending, never when the render would return early.
    //    Urgent work (a player's lamp edit) after such a cheap render goes back to back, one chunk a frame (06/10: a lamp
    //    switch's ground now changes in one frame once its last chunk is done, LightmapSmooth::HoldChunks).
    const bool backToBack = g_queue.front().urgent && g_lastMs >= 0.0 && g_lastMs <= 12.0;
    if (g_frame <= g_lastDoneFrame && !backToBack) return;
    const auto now = Clock::now();
    while (!g_releases.empty() && now - g_releases.front() >= std::chrono::seconds(1)) g_releases.pop_front();
    if (g_releases.size() >= ReleaseLimit(g_queue.front().urgent, g_lastMs)) {
        g_waitRate++;
        return;
    }
    if (p.f55 || p.f56) {
        g_waitRebuildFlags++;
        return;
    }
    if (GatesRaw(v) != 0) {
        g_waitGates++;
        return;
    }
    Entry e = std::move(g_queue.front());
    g_queue.pop_front();
    ChunkRaw c;
    if (ReadPtr(v.begin + 4 * static_cast<uintptr_t>(e.slot)) != e.chunk || !ReadChunkRaw(e.chunk, c) || !LayoutOk(v, e.slot, c) || !c.lightMap)
        return Fail(out, std::format("the chunk at ({},{}) changed while it was queued", e.ix, e.iz));
    g_flightDone.store(false, std::memory_order_release);
    g_flightTicks.store(0, std::memory_order_release);
    const int prev = WriteFlag54(e.chunk, 1);
    if (prev < 0) return Fail(out, std::format("the chunk at ({},{}) could not be flagged", e.ix, e.iz));
    g_flight = std::move(e);
    g_haveFlight = true;
    g_flightSetByUs = prev == 0;
    g_flightFrames = g_flightIdle = 0;
    g_flightChunk.store(g_flight.chunk, std::memory_order_release);
    g_releases.push_back(now);
}

std::string Status() {
    std::string state;
    if (!g_resolved) state = g_resolveInfo;
    else if (!g_hooked) state = "the chunk re-render call is not hooked";
    else if (g_offThisWorld) state = "off for this world after: " + g_offWhy;
    else if (!g_rebuiltThisWorld) state = "waiting for the world's first terrain rebuild";
    else state = "ready";
    std::string s = std::format("{} | local relights {}, paced sweeps {}, chunks re-rendered {} ({} by another game path), refused {} (last: {}), failures {} | queue {}",
                                state, g_statLocal, g_statSweeps, g_statChunks, g_statOtherPath, g_statRefused, g_lastRefusal, g_statFailed, g_queue.size());
    if (g_haveFlight) s += std::format(", in flight ({},{}) for {} frames", g_flight.ix, g_flight.iz, g_flightFrames);
    s += std::format(" | paced rebuilds {}", g_statRebuilds);
    if (g_rb.active) s += std::format(" (running: {} of {} chunks)", g_rb.next, g_rb.order.size());
    if (g_timedChunks > 0) s += std::format(" | chunk render: last {:.2f} ms, average {:.2f}, max {:.2f}", g_lastMs, g_sumMs / g_timedChunks, g_maxMs);
    s += std::format(" | waits: rebuild flags {}, terrain not ready {}, 8 per second {} | sweep renders seen {} | last: {}", g_waitRebuildFlags, g_waitGates, g_waitRate,
                     g_sweepRenders.load(std::memory_order_relaxed), g_lastDone);
    return s;
}

} // namespace ChunkRelight
