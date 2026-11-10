// GPU room lamps, stage 2b: the kept room solves drawn on the GPU (see gpu_room_lamps.h).
//
// Part of Apex Radiance. Credits: @loinyx

#include "gpu_room_lamps.h"
#include "rt_lamp_gpu.h"
#include "rt_outdoor_lamps.h"
#include "outdoor_delta.h"
#include "level_light_share.h"
#include "rt_lamp_model.h"
#include "phys_lamp_pick.h"
#include "phys_tiles.h"
#include "lamp_mark_filter.h"
#include "drag_lite.h"
#include "arch_ambient.h"
#include "dev_switches.h"
#include "unlit_rooms.h"
#include "hook_guard.h"
#include "apex_log.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace GpuRoomLamps {
namespace {

constexpr const char* kMapName[4] = {"wall atlas", "floor", "ceiling", "objects"};
constexpr float kMoved = 1e-4f; // m: a lamp this far from where its parts were kept has moved
constexpr DWORD kEditWaitMs = 5000; // an edit taken and not shown in every room by then is logged as such

struct Entry {
    uintptr_t mgr = 0;
    int cls = 0, m = 0, story = 0, id = 0;
    uint32_t version = 0;
    uint64_t lot = 0;
    float thr = 0.0f;
    bool ok = false, checked = false, moved = false;
    RtLampGpu::KeptMapGpu gpu;
    std::vector<uintptr_t> lamps;
    std::vector<GpuLampLaw::Lamp> laws;
    std::vector<char> lawOk;
    std::vector<std::pair<uint32_t, uint32_t>> expect; // (y << 16 | x, the game's texel) for the self-check
    std::vector<RtLampGpu::KeptMapData::RoomLamp> roomLamps;
    std::vector<char> colourOnly; // per lamp of `lamps`: its law is its colour alone (a TubeLight without a segment)
    LARGE_INTEGER relitAt{};
    uint32_t relitFrame = 0;
    // (physical indoor lamps) lamps' mult x norm scale, their laws in the lot's space (shadow tiles, law) and positions in the
    // world's (the game's 3D test), the furniture blockers, the lot's space
    bool physical = false;
    std::vector<float> physMult;
    std::vector<char> physReach; // per lamp: the realistic reach share applies on the room's wall texels (outdoor walls)
    std::vector<RtLampModel::Lamp> lotLaws;
    std::vector<std::array<float, 3>> world;
    std::shared_ptr<const RtLampGpu::BlockerTable> blockers; // (its kept room's, shared by the room's maps)
    float blkHeader = -1.0f;
    // (cross_room_shadows, glass_share; DrawnTable) its kept room's own table, the key of what was joined to it, row 1 counts other rooms'
    // records too, and the table's glazed spans
    std::shared_ptr<const RtLampGpu::BlockerTable> blkOwn;
    float ownHeader = -1.0f;
    uint64_t tblKey = 0;
    uint64_t liveKey = 0; // (KeptRef::live as its blkOwn holds it)
    DWORD retryAt = 0;    // (not ok, RetryLater) made again from its kept solve at this tick (0: never, nothing of it to draw)
    bool crossHead = false;
    float paneFirst = 0.0f, paneCount = 0.0f;
    // (the arches it holds as of archGen; room_bounce: their rows now)
    uint32_t archGen = 0;
    std::vector<LevelLightShare::ArchSpan> arches;
    int bounceN = 0;
    float bounceRows[2 * ArchAmbient::kBounceRows][4] = {}, bounceHue[4] = {};
    // (a table dragged with its lamp) the table before the records' offsets, the copy drawn with them, and their hash (0: none)
    std::shared_ptr<const RtLampGpu::BlockerTable> blkBase, blkOut;
    uint64_t offHash = 0;
    std::vector<std::array<float, 3>> carried; // (record, dx, dz) since its solve: kept until the next one
    float xf[16] = {};
    // (physical, stage 4) the solve's lamps out of its base (their rows: mult x norm scale, reach, the room they were registered
    // in), the lights its base holds, the room's normalisation and roofless flag, its texels' world box; the drawn lamps' hash,
    // and the physical edits whose lamp it drew before or draws now since its last relight
    std::vector<uintptr_t> listed, baked;
    std::vector<float> listedMult;
    std::vector<char> listedReach;
    std::vector<int> listedHome;
    float norm = 1.0f;
    bool outdoorRoom = false;
    PhysLampPick::Box box;
    uint64_t physSig = 0;
    // (its lamp list as last built: rebuilt when the round's lot lamps or the occluders' generation changed; never cached while a
    // lamp it lists is out of the lot's snapshot, which LotRoundSig does not read)
    bool physCached = false;
    uint64_t physLot = 0;
    uint32_t physGen = 0;
    std::vector<uintptr_t> editHits;
    // (S8) a roofed room's ambient (+0x110) at its solve's end and the compose's scale of it now (RtLampModel::AmbScale)
    float ambSolve[3] = {}, ambScale[3] = {1.0f, 1.0f, 1.0f};
    // (physical, ComposeOver's partial relight) the places its compose writes in the map (y << 16 | x: texels, block rows, copies,
    // restores), what it is drawn from now (Sig) and what its last draw into the copy was drawn from
    std::vector<uint32_t> writes;
    std::vector<RtLampGpu::Block> runs; // (ComposeOver's kept rooms) writes as runs of a row, made on first use
    uint64_t drawKey = 0, drawnKey = 0;
    // (sharper floors, ComposeHi) the draw its sub-texels in the overlay were last drawn from (0: none there)
    uint64_t hiKey = 0;
};
// (ComposeOver) a rooms-only copy's entries as last drawn in full: their set, and whether no two of them write the same place
struct CopyState {
    uint64_t set = 0;
    bool disjoint = false, valid = false;
    uint64_t rules = 0;                     // (KeptRules) what every room's draw read beside its own inputs, at the last full draw
    const IDirect3DTexture9* out = nullptr; // the copy drawn into
};
uint32_t g_roomsKept = 0, g_roomsDrawn = 0; // (TakeTileCost) rooms of full draws taken from the copy before room 0's compose, drawn
std::map<std::tuple<uintptr_t, int, int>, CopyState> g_copies; // (mgr, cls, m)
// (physical indoor lamps) the kept shadow atlas: a tile per lamp, kept while the lamp stays put and the occluders are the same.
// Its tiles grow with the lamps the maps draw (phys_tiles.h: every lot lamp drawn while it exists, 4 at a time, up to
// PhysTiles::kTiles = 120 in columns: 6 x 256 x 256 x (G16R16F + D24X8) = 3.1 MB a tile); a map with more than 16 lamps is drawn in
// passes of 16 over the same sum. Past the tiles, the first filled stay and only the excess lamps are rendered again in turn
constexpr int kMaxLamps = 64; // a map's lamps drawn at most (4 passes of 16 tiles: the frame's cost bounded)
PhysTiles::Atlas g_tiles;
std::vector<uintptr_t> g_tileSeen; // (the lot lamps drawn since they exist: PhysTiles::Want; BeginFrame drops those gone)
uint32_t g_tileGrowths[2] = {};    // (TileCost) the atlas grown with its tiles kept, made again with none
// (physical indoor lamps) a relight round's maps per story: drawn, and why the others were not (logged when it changes, BeginFrame)
struct Cover {
    int drawn = 0, failed = 0;
    std::string why;
    std::string room0; // (NoteRoom0) how room 0's lamps were drawn
};
std::map<int, Cover> g_cover;
std::map<int, std::string> g_coverLogged;
void NoteCover(int story, const std::string& why) {
    Cover& c = g_cover[story];
    if (why.empty()) {
        c.drawn++;
        return;
    }
    c.failed++;
    if (c.why.find(why) == std::string::npos && c.why.size() < 400) c.why += (c.why.empty() ? "" : "; ") + why;
}
std::atomic<uint32_t> g_frame{0}; // Real-time outdoor lamps' relight rounds (BeginFrame)
std::map<std::tuple<const void*, int, int>, Entry> g_entries; // (room, class, map); render thread
std::vector<OutdoorDelta::KeptRef> g_refs;
size_t g_bytes = 0;
std::atomic<int> g_lines{0};
bool LogAllowed() { return g_lines.fetch_add(1, std::memory_order_relaxed) < 2000; }
// (S8) a room's ambient now (+0x110, which Rooms at Night moves and a lamp edit taken takes again: OutdoorDelta's AmbientDue)
bool ReadAmbient(const void* room, float* c) {
    __try {
        // only a solved room (state +0xF0 4 or 5): a solve zeroes +0x110 until its ambient step, and following that 0 drew a
        // room with no lamps (its texels all ramp x ambient) pure black; the previous scale stays meanwhile
        const int state = *reinterpret_cast<const int*>(static_cast<const BYTE*>(room) + 0xF0);
        if (state != 4 && state != 5) return false;
        std::memcpy(c, static_cast<const BYTE*>(room) + 0x110, 3 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// (room_bounce) a room's normalisation (+0x160), 1 unread
float ReadNorm(const void* room) {
    __try {
        const float v = *reinterpret_cast<const float*>(static_cast<const BYTE*>(room) + 0x160);
        return std::isfinite(v) && v > 0.0f ? v : 1.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1.0f;
    }
}
// The lot's glazed spans as of its generation (Sig; render thread)
std::vector<RtLampProxy::Pane> g_panes;
uint32_t g_panesGen = 0;

// The kept solves drawn: room -> (its version, the maps drawn with it); read by the light tree thread (Drawn)
std::mutex g_drawnMx;
std::unordered_map<const void*, std::pair<uint32_t, unsigned>> g_drawn;
std::unordered_map<const void*, unsigned> g_drawnAny; // (DrawnAny) room -> the maps drawn at any version since their last drop
// Map m of the room no longer drawn (only that map: another map's bit may already be its new version's)
void Undrawn(const void* room, int m, bool rekept) {
    std::lock_guard<std::mutex> lk(g_drawnMx);
    if (const auto it = g_drawn.find(room); it != g_drawn.end() && !(it->second.second &= ~(1u << m))) g_drawn.erase(it);
    if (const auto it = g_drawnAny.find(room); it != g_drawnAny.end() && !rekept && !(it->second &= ~(1u << m))) g_drawnAny.erase(it); // (kept again: DrawnAny)
}
// The lamp edits taken (NoteEdit, light tree thread), until every room of theirs is relit after them (render thread)
struct Edit {
    uintptr_t light = 0;
    const char* kind = "";
    std::vector<const void*> rooms;
    std::string stories;
    LARGE_INTEGER at{};
    DWORD tick = 0;
    uint32_t frame = 0;
    // each map of its rooms that holds its parts: its first relight after the edit (time, relight round)
    std::map<std::tuple<const void*, int, int>, std::pair<LARGE_INTEGER, uint32_t>> shown;
};
std::mutex g_editMx;
std::vector<Edit> g_edits;
// (physical, stage 4) the lamp edits taken without solves (NotePhysEdit, light tree thread; a drag's steps are one, its latest
// step timed), until the maps whose drawn lamps held or hold it were relit (render thread): logged with the maps relit, the time
// from the latest step to the last of them and its relight round
struct PhysEdit {
    uintptr_t light = 0;
    const char* kind = "";
    LARGE_INTEGER at{}, lastAt{};
    DWORD tick = 0;
    uint32_t frame = 0, lastFrame = 0;
    int steps = 0, maps = 0;
    std::vector<std::tuple<const void*, int, int>> relit;
    bool relitSince = false; // a map relit since its latest step
};
std::vector<PhysEdit> g_physEdits; // (under g_editMx)
std::atomic<DWORD> g_physMoveAt{0}, g_physDragAt{0}; // (the latest move taken, and the first of its drag)
std::atomic<uintptr_t> g_physMoveLight{0};
// (stage 4) the active lot's registered lights this round (OutdoorDelta::LotLamps) and where each is (light -> index)
std::vector<OutdoorDelta::LotLamp> g_lot;
std::unordered_map<uintptr_t, size_t> g_lotAt;
// (per g_lot lamp, this round) its world law read once: every physical entry of every map asks each lot lamp whether its reach
// meets the entry's box, which read the lamp again per entry (entries x lamps reads a frame, with nothing edited)
struct LotLaw {
    uint64_t lot = 0;
    char state = 0; // 0 not read, 1 read, 2 no law
    GpuLampLaw::Lamp w;
};
std::vector<LotLaw> g_lotLaw;
// (this round) the lot's lamps as every physical entry's pick reads them: each lamp's row inputs, its laws (world, and with its
// reach radii) and the lamps gone. Unchanged: an entry's last lamp list stands (3.7 ms a frame rebuilding them with nothing
// edited, the 23:37 log of 02/11)
uint64_t g_lotSig = 0, g_lotSigLot = 0;
bool g_lotSigRead = false;
std::vector<uintptr_t> g_lotGone; // (the lot's lamps that left its registry: a kept list holding one no longer draws it)
// (stage 4) the physical relights' CPU cost since TakeRelightCost (maps, ms, the largest round's ms) and this round's
int g_costMaps = 0;
double g_costMs = 0.0, g_costWorst = 0.0, g_costRound = 0.0;

void Drop(Entry& e, const void* room, bool rekept = false) {
    g_bytes -= std::min(g_bytes, e.gpu.bytes);
    e.gpu.Release();
    e.ok = false;
    Undrawn(room, e.m, rekept);
}
std::map<std::tuple<const void*, int, int>, std::pair<uint32_t, int>> g_retries; // (room, cls, m) -> (version, tries)
void RetryLater(Entry& e, const void* room) {
    auto& [version, tries] = g_retries[{room, e.cls, e.m}];
    if (version != e.version) version = e.version, tries = 0;
    e.retryAt = std::max<DWORD>(GetTickCount() + PhysLampPick::RetryMs(tries++), 1);
}
void Mix(uint64_t& h, const void* p, size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
}
uint64_t LotRoundSig(uint64_t lot) {
    if (g_lotSigRead && g_lotSigLot == lot) return g_lotSig;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < g_lot.size(); i++) {
        const OutdoorDelta::LotLamp& L = g_lot[i];
        LotLaw& c = g_lotLaw[i];
        int type = 0, reg = -1;
        if (!c.state || c.lot != lot) c.lot = lot, c.state = RtOutdoorLamps::WorldLaw(L.light, lot, false, c.w, type, reg) ? 1 : 2;
        GpuLampLaw::Lamp reach{};
        const bool hasReach = c.state == 1 && RtOutdoorLamps::WorldLaw(L.light, lot, true, reach, type, reg);
        Mix(h, &L.light, sizeof L.light), Mix(h, &L.story, sizeof L.story), Mix(h, &L.home, sizeof L.home), Mix(h, &L.homeNorm, sizeof L.homeNorm);
        Mix(h, &L.homeOutdoor, 1), Mix(h, &c.state, 1), Mix(h, &hasReach, 1);
        if (c.state == 1) Mix(h, &c.w, sizeof c.w);
        if (hasReach) Mix(h, &reach, sizeof reach);
    }
    for (uintptr_t g : g_lotGone) Mix(h, &g, sizeof g);
    const size_t n = g_lot.size();
    Mix(h, &n, sizeof n);
    g_lotSig = h, g_lotSigLot = lot, g_lotSigRead = true;
    return h;
}
double MsSince(const LARGE_INTEGER& t0, const LARGE_INTEGER& t1) {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart ? 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart) : 0.0;
}
// The relit map read back against the texels the game left at the kept places (the log)
void SelfCheck(IDirect3DDevice9* dev, const Entry& e, IDirect3DTexture9* out, int w, int h) {
    std::vector<uint32_t> px;
    if (!RtLampGpu::Engine::ReadTexture(dev, out, w, h, px)) {
        if (LogAllowed()) LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {} GPU self-check: the relit map could not be read back", e.story, e.id, e.cls, kMapName[e.m]));
        return;
    }
    long n = 0, within = 0, brighter = 0, darker = 0;
    double sumD = 0.0;
    int alphaMax = 0;
    std::vector<std::pair<int, size_t>> worst;
    for (size_t j = 0; j < e.expect.size(); j++) {
        const auto [place, game] = e.expect[j];
        const uint32_t x = place & 0xFFFFu, y = place >> 16;
        if (x >= static_cast<uint32_t>(w) || y >= static_cast<uint32_t>(h)) continue; // (past the room's own map: not in it)
        const uint32_t got = px[static_cast<size_t>(y) * w + x];
        int d = 0;
        for (int s = 0; s < 24; s += 8) d = std::max(d, std::abs(static_cast<int>((game >> s) & 0xFFu) - static_cast<int>((got >> s) & 0xFFu)));
        alphaMax = std::max(alphaMax, std::abs(static_cast<int>(game >> 24) - static_cast<int>(got >> 24)));
        n++;
        within += d <= 2;
        int sd = 0; // (the channel furthest from the game's, signed: GPU brighter > 0)
        for (int s = 0; s < 24; s += 8)
            if (const int v = static_cast<int>((got >> s) & 0xFFu) - static_cast<int>((game >> s) & 0xFFu); std::abs(v) > std::abs(sd)) sd = v;
        sumD += d, brighter += sd > 8, darker += sd < -8;
        if (d > 2 && (worst.size() < 3 || d > worst.back().first)) {
            worst.emplace_back(d, j);
            std::sort(worst.begin(), worst.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            if (worst.size() > 3) worst.pop_back();
        }
    }
    std::string ws;
    for (const auto& [d, j] : worst) {
        const uint32_t x = e.expect[j].first & 0xFFFFu, y = e.expect[j].first >> 16;
        ws += std::format("; {} levels at ({}, {}): game {:08X} GPU {:08X}", d, x, y, e.expect[j].second, px[static_cast<size_t>(y) * w + x]);
    }
    const auto pct = [&](long k) { return n ? 100.0 * static_cast<double>(k) / static_cast<double>(n) : 100.0; };
    if (e.physical) {
        if (LogAllowed())
            LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {} physical vs the game's last solve: {} texels, {:.2f}% within 2/255, mean {:.2f} levels, "
                                 "{:.2f}% brighter > 8 levels, {:.2f}% darker > 8 levels{}",
                                 e.story, e.id, e.cls, kMapName[e.m], n, pct(within), n ? sumD / static_cast<double>(n) : 0.0, pct(brighter), pct(darker), ws));
        return;
    }
    if (LogAllowed())
        LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {} GPU self-check: {} texels, {:.2f}% within 2/255 (alpha within {}/255) of the texels the game left{}",
                             e.story, e.id, e.cls, kMapName[e.m], n, n ? 100.0 * static_cast<double>(within) / static_cast<double>(n) : 100.0, alphaMax, ws));
}
// The edits taken whose rooms' maps holding the lamp's parts are all relit since (render thread). A map without its parts is
// not waited for: nothing of it changes, and its signature does not either (it waited for the room's next kept solve, which
// logged a switch shown in 2 frames as "relit in 502 ms", 2026-11-02)
void LogEdits() {
    std::vector<Edit> done;
    {
        std::lock_guard<std::mutex> lk(g_editMx);
        if (g_edits.empty()) return;
        for (auto it = g_edits.begin(); it != g_edits.end();) {
            bool relit = true;
            for (const auto& [key, e] : g_entries) {
                if (!e.ok || std::find(it->rooms.begin(), it->rooms.end(), std::get<0>(key)) == it->rooms.end() ||
                    std::find(e.lamps.begin(), e.lamps.end(), it->light) == e.lamps.end())
                    continue;
                if (e.relitAt.QuadPart > it->at.QuadPart) it->shown.try_emplace(key, e.relitAt, e.relitFrame);
                else relit = false;
            }
            if (relit || GetTickCount() - it->tick > kEditWaitMs) {
                if (!relit) it->kind = nullptr; // (logged as not shown)
                done.push_back(std::move(*it));
                it = g_edits.erase(it);
            } else
                ++it;
        }
    }
    for (const Edit& e : done) {
        if (!LogAllowed()) continue;
        // per map kind of its rooms holding its parts: when it was shown after the edit, and in which relight round (+0: the round
        // of the edit's frame)
        std::string kinds;
        double all = 0.0;
        for (int m = 0; m < 4; m++) {
            double ms = -1.0;
            int rounds = 0;
            for (const auto& [key, s] : e.shown)
                if (std::get<2>(key) == m) ms = std::max(ms, MsSince(e.at, s.first)), rounds = std::max(rounds, static_cast<int>(s.second - e.frame));
            if (ms < 0.0) continue;
            kinds += std::format("{}{} {:.1f} ms +{}", kinds.empty() ? " (" : ", ", kMapName[m], ms, rounds);
            all = std::max(all, ms);
        }
        kinds += kinds.empty() ? " (no kept map of its rooms holds its parts)" : ")";
        if (!e.kind)
            LOG_INFO(std::format("[GpuRoomLamps] Lamp {:08X}: its edit was not shown in every room within {} ms (rooms on stories {}), shown so far{}", e.light, kEditWaitMs,
                                 e.stories, kinds));
        else
            LOG_INFO(std::format("[GpuRoomLamps] {}: lamp {:08X}, {} rooms on stories {}, no room solved, relit in {:.1f} ms{}", e.kind, e.light, e.rooms.size(), e.stories, all,
                                 kinds));
    }
}

} // namespace

namespace {
// (cross_room_shadows, glass_share) The table an entry draws from: its room's own records, those of the rooms an arch joins to it
// (BlockerTable::Join; their kept solves as they are now, one hop, roofed rooms) and the lot's glazed spans. Made again when a joined
// room is solved, a record of it or of this room moved (KeptRef::live: each frame of a drag) or the lot changes, shared by the room's maps. A
// table carried with its lamp stays carried (e.carried, Sig).
// shortcut: with live_object_tables off, a neighbour's table dragged with its lamp is joined from its solve's place until that room's next solve
void DrawnTable(Entry& e, const void* room, int cls) {
    const bool cross = DevSwitches::CrossRoomShadows(), glass = DevSwitches::GlassShare();
    if (!cross && !glass) return;
    uint64_t key = 1469598103934665603ull;
    if (glass) Mix(key, &g_panesGen, sizeof g_panesGen);
    Mix(key, &e.liveKey, sizeof e.liveKey);
    std::vector<const OutdoorDelta::KeptRef*> rooms;
    if (cross)
        for (const LevelLightShare::ArchSpan& a : e.arches) {
            const auto r = std::find_if(g_refs.begin(), g_refs.end(), [&](const OutdoorDelta::KeptRef& k) { return k.room == a.nb; });
            if (r == g_refs.end() || std::find(rooms.begin(), rooms.end(), &*r) != rooms.end()) continue;
            rooms.push_back(&*r);
            Mix(key, &r->room, sizeof r->room), Mix(key, &r->version, sizeof r->version), Mix(key, &r->live, sizeof r->live);
        }
    if (key == e.tblKey) return;
    e.tblKey = key;
    std::shared_ptr<const RtLampGpu::BlockerTable> t;
    for (int k = 0; k < 4 && !t; k++)
        if (const auto s = g_entries.find(std::make_tuple(room, cls, k)); s != g_entries.end() && &s->second != &e && s->second.tblKey == key && s->second.blkOwn == e.blkOwn && s->second.blkBase)
            t = s->second.blkBase, e.crossHead = s->second.crossHead, e.paneFirst = s->second.paneFirst, e.paneCount = s->second.paneCount;
    if (!t) try {
        std::vector<std::shared_ptr<const RtLampGpu::BlockerTable>> hold;
        std::vector<RtLampGpu::BlockerTable::Part> parts;
        for (const OutdoorDelta::KeptRef* r : rooms) {
            std::shared_ptr<const RtLampGpu::BlockerTable> b;
            int header = -1;
            if (OutdoorDelta::KeptBlockers(r->room, cls, r->version, b, header)) parts.push_back({b.get(), header}), hold.push_back(std::move(b));
        }
        auto j = std::make_shared<RtLampGpu::BlockerTable>(RtLampGpu::BlockerTable::Join({e.blkOwn.get(), static_cast<int>(e.ownHeader)}, parts));
        e.crossHead = j->rows[1].v[1] > j->rows[0].v[1];
        e.paneCount = glass ? static_cast<float>(g_panes.size()) : 0.0f;
        e.paneFirst = glass ? static_cast<float>(j->AppendPanes(g_panes)) : 0.0f;
        t = std::move(j);
    } catch (...) {
        e.tblKey = 0;
        return;
    }
    e.blockers = e.blkBase = std::move(t), e.blkHeader = 0.0f, e.blkOut.reset(), e.offHash = 0;
}
} // namespace

namespace {
// A kept map's data (OutdoorDelta::BuildKeptMap) with its physical points in the lot's space (base keeps the world position;
// mask.xyz = where the shadow is looked up: a texel past its room's diagonal wall inside the room, RtLampProxy::IntoRoom; a wall
// texel on a floor's plane off it, OffFloorLine). The render thread or a worker: xf and lot are read by the render thread.
// shortcut: the lot's room ids as this entry is built; a lot edit since leaves them until the entry is made again
bool BuildData(const void* room, int cls, uint32_t version, int m, int id, bool xfOk, const float* xf, const std::shared_ptr<const RtLampProxy::Lot>& lot, RtLampGpu::KeptMapData& d,
               bool& space) {
    std::string why;
    if (!OutdoorDelta::BuildKeptMap(room, cls, version, m, d, why)) return false; // (nothing of it in this map: an entry that draws nothing)
    space = !d.physical || xfOk;
    if (space && d.physical)
        for (auto* pts : {&d.phys, &d.physHi})
            for (RtLampGpu::TexelVertex& v : *pts) {
                float p[3], n[3];
                for (int k = 0; k < 3; k++)
                    p[k] = v.base[0] * xf[k] + v.base[1] * xf[4 + k] + v.base[2] * xf[8 + k] + xf[12 + k], n[k] = v.n[0] * xf[k] + v.n[1] * xf[4 + k] + v.n[2] * xf[8 + k];
                std::memcpy(v.p, p, sizeof p), std::memcpy(v.n, n, sizeof n), std::memcpy(v.mask, p, sizeof p);
                if (lot) RtLampProxy::IntoRoom(*lot, d.story, id, v.mask), RtLampProxy::OffFloorLine(*lot, d.story, v.mask, n);
            }
    return true;
}
// A room solved again: its maps' data from a worker, while the entries of its last solve keep drawing. Built in Sig, the four
// maps of a room of 47681 texels held the render thread 76 ms in one frame, and a day/night switch's first rooms 128 ms (the
// hitch before the rooms' light changes, 2026-11-09)
struct Prebuilt {
    uint32_t version = 0;
    bool built = false, space = false, threw = false;
    float xf[16] = {};
    double ms = 0.0;
    RtLampGpu::KeptMapData d;
    std::atomic<int> state{1}; // 2: ended
};
std::map<std::tuple<const void*, int, int>, std::shared_ptr<Prebuilt>> g_prebuilt; // (room, class, map); render thread
constexpr int kPrebuilds = 4; // workers at a time
constexpr double kUploadMs = 2.0; // a round takes prebuilt maps until their uploads took this long (its first always goes)
double g_uploadRound = 0.0, g_uploadWorst = 0.0, g_prebuiltMs = 0.0; // (the 10 s line, BeginFrame)
int g_prebuiltMaps = 0;
std::atomic<int> g_prebuildsRunning{0}; // (a job replaced by a newer solve's still runs: counted until it ends)
// True once this map's data of the room's solve now can be taken (or no worker could start: built here, as a first solve's)
bool PrebuiltReady(const std::tuple<const void*, int, int>& key, uintptr_t mgr, const OutdoorDelta::KeptRef& r) {
    const auto at = g_prebuilt.find(key);
    if (at != g_prebuilt.end() && at->second->version == r.version) return at->second->state.load() == 2 && g_uploadRound < kUploadMs;
    if (at != g_prebuilt.end() && at->second->state.load() == 2) g_prebuilt.erase(at); // (another solve's data, ended: not held)
    if (g_prebuildsRunning.load() >= kPrebuilds) return false;
    auto j = std::make_shared<Prebuilt>();
    j->version = r.version;
    const bool xfOk = RtOutdoorLamps::LotXform(mgr, j->xf);
    uint32_t lotGen = 0;
    const std::shared_ptr<const RtLampProxy::Lot> lot = RtOutdoorLamps::KeptLot(lotGen);
    const void* const room = std::get<0>(key);
    const int cls = std::get<1>(key), m = std::get<2>(key), id = r.id;
    g_prebuildsRunning.fetch_add(1);
    if (!HookGuard::StartDetached("GpuRoomLamps kept map", [j, room, cls, m, id, xfOk, lot] {
            LARGE_INTEGER t0{}, t1{};
            QueryPerformanceCounter(&t0);
            try {
                j->built = BuildData(room, cls, j->version, m, id, xfOk, j->xf, lot, j->d, j->space);
            } catch (...) {
                j->built = false, j->threw = true;
            }
            QueryPerformanceCounter(&t1);
            j->ms = MsSince(t0, t1);
            j->state.store(2);
            g_prebuildsRunning.fetch_sub(1);
        })) {
        g_prebuildsRunning.fetch_sub(1);
        g_prebuilt.erase(key);
        return true;
    }
    g_prebuilt[key] = std::move(j);
    return false;
}
} // namespace

uint64_t Sig(IDirect3DDevice9* dev, uintptr_t mgr, int cls, int m) {
    if (!OutdoorDelta::RoomLampCapture()) {
        if (!g_entries.empty()) ReleaseAll();
        OutdoorDelta::SolveTouched(nullptr); // (the game's maps again: the rooms with lamp edits taken since their solve)
        return 0;
    }
    if (!OutdoorDelta::RoomLampPhysical()) OutdoorDelta::SolveTouched(nullptr); // (the replica model: the physical edits' rooms solved)
    OutdoorDelta::SolveUnkept();
    LogEdits();
    OutdoorDelta::KeptRooms(mgr, cls, g_refs);
    {
        uint32_t lotGen = 0;
        const std::shared_ptr<const RtLampProxy::Lot> lot = RtOutdoorLamps::KeptLot(lotGen);
        if (lotGen != g_panesGen) {
            g_panes.clear(), g_panesGen = lotGen;
            if (lot) RtLampProxy::PanesOf(*lot, g_panes);
        }
    }
    // this map's entries whose kept solve went or was kept again
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        Entry& e = it->second;
        const void* room = std::get<0>(it->first);
        const bool here = e.mgr == mgr && e.cls == cls && e.m == m;
        if (here && std::none_of(g_refs.begin(), g_refs.end(), [&](const OutdoorDelta::KeptRef& r) { return r.room == room && r.version == e.version; })) {
            const auto now = std::find_if(g_refs.begin(), g_refs.end(), [&](const OutdoorDelta::KeptRef& r) { return r.room == room; });
            const bool gone = now == g_refs.end();
            if (gone) g_prebuilt.erase(it->first);
            else if (e.ok && !PrebuiltReady(it->first, mgr, *now)) { // (its last solve's entry draws meanwhile)
                ++it;
                continue;
            }
            Drop(e, room, !gone);
            if (e.physical && gone) OutdoorDelta::SolveTouched(room); // (no kept solve of it any more: its game's map, which a lamp edit taken may lack)
            it = g_entries.erase(it);
        } else if (here && !e.ok && e.retryAt && static_cast<int32_t>(GetTickCount() - e.retryAt) >= 0)
            it = g_entries.erase(it);
        else {
            // (its solve is current again, a day/night switch turned back: another solve's data built meanwhile is not held)
            if (here)
                if (const auto p = g_prebuilt.find(it->first); p != g_prebuilt.end() && p->second->state.load() == 2) g_prebuilt.erase(p);
            ++it;
        }
    }
    uint64_t sig = 1469598103934665603ull;
    bool any = false;
    for (const OutdoorDelta::KeptRef& r : g_refs) {
        const auto [it, fresh] = g_entries.try_emplace(std::make_tuple(r.room, cls, m));
        Entry& e = it->second;
        if (fresh) {
            e.mgr = mgr, e.cls = cls, e.m = m, e.version = r.version, e.id = r.id;
            RtLampGpu::KeptMapData d;
            bool built = false, space = false, prebuilt = false;
            // (a worker's build that threw, out of memory: built here as before)
            if (const auto p = g_prebuilt.find(it->first); p != g_prebuilt.end() && p->second->version == r.version && p->second->state.load() == 2 && !p->second->threw) {
                built = p->second->built, space = p->second->space, d = std::move(p->second->d), prebuilt = true;
                std::memcpy(e.xf, p->second->xf, sizeof e.xf);
                g_prebuiltMaps++, g_prebuiltMs += p->second->ms;
                g_prebuilt.erase(p);
            } else {
                uint32_t lotGen = 0;
                const bool xfOk = RtOutdoorLamps::LotXform(mgr, e.xf);
                built = BuildData(r.room, cls, r.version, m, r.id, xfOk, e.xf, RtOutdoorLamps::KeptLot(lotGen), d, space);
            }
            LARGE_INTEGER u0{}, u1{};
            QueryPerformanceCounter(&u0);
            if (built) {
                e.story = d.story, e.lot = d.lot, e.thr = d.thr, e.physical = d.physical;
                if (!space) {
                    if (LogAllowed())
                        LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {}: physical: the lot's space could not be read (left as the game's)", d.story, r.id, cls,
                                             kMapName[m]));
                    OutdoorDelta::SolveTouched(r.room);
                    RetryLater(e, r.room);
                } else if (RtLampGpu::Engine::UploadKept(dev, d, e.gpu)) {
                    for (const auto* v : {&d.texels, &d.copies, &d.restores})
                        for (const RtLampGpu::TexelVertex& t : *v) e.writes.push_back(static_cast<uint32_t>(t.y) << 16 | static_cast<uint32_t>(t.x));
                    for (const RtLampGpu::KeptBlockOut& b : d.outs)
                        for (int y = b.y0; y <= b.y1; y++)
                            for (int x = b.x0; x <= b.x1; x++) e.writes.push_back(static_cast<uint32_t>(y) << 16 | static_cast<uint32_t>(x));
                    e.physMult = std::move(d.physMult);
                    e.physReach = std::move(d.physReach);
                    e.blockers = std::move(d.blockers);
                    e.blkHeader = d.blkHeader;
                    e.blkOwn = e.blockers, e.ownHeader = e.blkHeader;
                    e.ok = true;
                    e.lamps = std::move(d.lamps);
                    e.colourOnly = std::move(d.colourOnly);
                    e.expect = std::move(d.expect);
                    e.roomLamps = std::move(d.roomLamps);
                    if (e.physical) { // (stage 4: the lamps drawn are picked each round, below)
                        e.listed = std::move(e.lamps), e.listedMult = std::move(e.physMult), e.listedReach = std::move(e.physReach);
                        e.listedHome = std::move(d.physHome), e.baked = std::move(d.physBaked), e.norm = d.physNorm, e.outdoorRoom = d.physOutdoor;
                        // (S8: only a map with texels of a pass that adds the ambient, the ceilings', follows it: the others are not
                        // relit while a slider moves)
                        const auto ambOf = [](const std::vector<RtLampGpu::TexelVertex>& v) {
                            return std::any_of(v.begin(), v.end(), [](const RtLampGpu::TexelVertex& t) { return t.base[3] > 0.5f && t.n[0] + t.n[1] + t.n[2] > 0.0f; });
                        };
                        if (ambOf(d.texels) || ambOf(d.wallTexels) || ambOf(d.copies)) std::memcpy(e.ambSolve, d.physAmb, sizeof e.ambSolve);
                        std::memcpy(e.box.lo, d.physBox, sizeof e.box.lo), std::memcpy(e.box.hi, d.physBox + 3, sizeof e.box.hi);
                        e.lamps.clear();
                    }
                    g_bytes += e.gpu.bytes;
                } else {
                    if (LogAllowed())
                        LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {}: its buffers could not be made (left as the game's)", d.story, r.id, cls, kMapName[m]));
                    if (d.physical) OutdoorDelta::SolveTouched(r.room);
                    RetryLater(e, r.room);
                }
            }
            QueryPerformanceCounter(&u1);
            if (prebuilt) g_uploadRound += MsSince(u0, u1);
        }
        if (!e.ok) continue;
        int type = 0, reg = -1;
        if (e.physical) { // (no parts, so no move rule: every lamp drawn with its law now; a lamp gone or without a law adds nothing)
            const uint32_t gen = RtOutdoorLamps::KeptMeshGen();
            // (at: its index in g_lot, whose world law is read once a round; none: read here)
            const auto worldOf = [&](uintptr_t light, size_t at, GpuLampLaw::Lamp& w) {
                if (at >= g_lotLaw.size()) return RtOutdoorLamps::WorldLaw(light, e.lot, false, w, type, reg);
                LotLaw& c = g_lotLaw[at];
                if (!c.state || c.lot != e.lot) c.lot = e.lot, c.state = RtOutdoorLamps::WorldLaw(light, e.lot, false, c.w, type, reg) ? 1 : 2;
                if (c.state != 1) return false;
                w = c.w;
                return true;
            };
            // A record whose top a kept lamp stood on at its solve (RoomLightTests::StandsOn) moves with the lamp while its own object
            // moved lately (LampMarkFilter::ShapeMoved: the occluder entry of the record's first shape) and the lamp kept its height:
            // a table dragged with its lamp. A lamp dragged alone leaves its table where it is, still casting its shadow; nothing else
            // moves the record. The offset stays where the drag left it until the next solve reads the room again. Identity, not
            // place: in a table drag the one entry that moves lies by the lamp's head (03/11 recordings 11:47 to 15:25, all table
            // drags); UNVERIFIED that its shape is the record's (PhysLampPick::ShapeMoved; the [CarryTable] line names both)
            if (const uint32_t ag = LevelLightShare::ArchGen(); ag != e.archGen) e.archGen = ag, LevelLightShare::ArchSpansOf(r.room, e.arches);
            if (r.live != e.liveKey && r.version == e.version) { // (an object of the room moved: the records as it stands, ahead of its solve)
                e.liveKey = r.live;
                std::shared_ptr<const RtLampGpu::BlockerTable> t;
                int header = -1;
                if (OutdoorDelta::KeptBlockers(r.room, cls, r.version, t, header)) e.blkOwn = e.blockers = std::move(t), e.ownHeader = e.blkHeader = static_cast<float>(header);
            }
            DrawnTable(e, r.room, cls);
            if (e.blockers != e.blkOut && e.blockers != e.blkBase) e.blkBase = e.blockers, e.blkOut.reset(), e.carried.clear(), e.offHash = 0; // (a solve's table)
            if (e.blkBase && e.blkHeader >= 0.0f) {
                const RtLampGpu::BlockerTable& t = *e.blkBase;
                // (the joined rooms' records too: a lamp on a table of the next room, its table's copy here carried with it)
                const int header = static_cast<int>(e.blkHeader), recs = static_cast<int>(t.rows[static_cast<size_t>(header) + (e.crossHead ? 1 : 0)].v[1]);
                std::vector<std::array<float, 3>>& off = e.carried;
                for (const auto& rl : e.roomLamps) {
                    if (rl.colourOnly) continue;
                    GpuLampLaw::Lamp now;
                    const auto at = g_lotAt.find(rl.light);
                    if (!worldOf(rl.light, at != g_lotAt.end() ? at->second : SIZE_MAX, now)) continue;
                    const float dx = now.pos[0] - rl.pos[0], dy = now.pos[1] - rl.pos[1], dz = now.pos[2] - rl.pos[2];
                    if (dx * dx + dz * dz < 1e-8f || std::fabs(dy) > 0.02f) continue;
                    // (the carry's decisions, a line a second while they happen)
                    static long s_moved = 0, s_carried = 0, s_kept = 0, s_none = 0;
                    static DWORD s_logAt = 0;
                    s_moved++;
                    bool first = false; // (the first carry, left in place and no record are logged at once)
                    int rec = -1, kind = 0; // (this move's last decided record and PhysLampPick::CarryOf; 0: it stands on no record)
                    uintptr_t shape = 0; // (that record's first blocker: the 'O' journal lines' shape when carried)
                    for (int i = 0; i < recs; i++) {
                        const bool on = t.StandsOn(header, i, rl.pos);
                        const uintptr_t s = on ? t.ShapeOf(header, i) : 0;
                        const bool before = std::any_of(off.begin(), off.end(), [&](const std::array<float, 3>& c) { return c[0] == static_cast<float>(i); });
                        const int k = PhysLampPick::CarryOf(on, on && (LampMarkFilter::ShapeMoved(s) || (before && LampMarkFilter::ShapeMovedWithin(s, std::min(LampMarkFilter::kCarryStickMs, PhysDragMs())))));
                        if (!k) continue;
                        rec = i, kind = k, shape = s;
                        if (k == 1) {
                            first = first || !s_kept++;
                            continue;
                        }
                        first = first || !s_carried++;
                        const auto o = std::find_if(off.begin(), off.end(), [&](const std::array<float, 3>& k) { return k[0] == static_cast<float>(i); });
                        if (o == off.end()) off.push_back({static_cast<float>(i), dx, dz});
                        else (*o)[1] = dx, (*o)[2] = dz;
                    }
                    if (!kind) first = first || !s_none++;
                    if (const DWORD tick = GetTickCount(); first || tick - s_logAt >= 1000) {
                        s_logAt = tick;
                        if (LogAllowed())
                            LOG_INFO(std::format("[CarryTable] story {} room {}: lamp {:08X} moved from its solve's place {} times: its table carried {}, left in place {}, "
                                                 "on no record {} (this move: {})",
                                                 e.story, e.id, rl.light, s_moved, s_carried, s_kept, s_none,
                                                 !kind ? std::string("it stands on no record")
                                                       : std::format("record {} with shape {:08X} {}", rec, shape,
                                                                     kind == 2 ? "carried, its object moved" : "left in place, its object did not move")));
                    }
                }
                // (a record read live stands where its object does: no shape, LiveTests. Carried too it would move twice)
                std::erase_if(off, [&](const std::array<float, 3>& c) { return !t.ShapeOf(header, static_cast<int>(c[0])); });
                uint64_t oh = 0;
                if (!off.empty()) oh = 1469598103934665603ull, Mix(oh, off.data(), off.size() * sizeof off[0]);
                if (oh != e.offHash) {
                    e.offHash = oh;
                    const auto sibling = [&]() -> const Entry* { // (the room's maps share one moved table: SetBlockers binds it once)
                        for (int k = 0; k < 4; k++)
                            if (const auto s = g_entries.find(std::make_tuple(r.room, cls, k));
                                s != g_entries.end() && &s->second != &e && s->second.blkOut && s->second.blkBase == e.blkBase && s->second.offHash == oh)
                                return &s->second;
                        return nullptr;
                    };
                    if (!oh) e.blockers = e.blkBase, e.blkOut.reset();
                    else if (const Entry* sib = sibling())
                        e.blkOut = e.blockers = sib->blkOut;
                    else
                        try {
                            auto moved = std::make_shared<RtLampGpu::BlockerTable>(t);
                            for (const auto& o : off) moved->Offset(header, static_cast<int>(o[0]), o[1], o[2]);
                            e.blkOut = moved, e.blockers = std::move(moved);
                        } catch (...) {
                            e.offHash = 0, e.blockers = e.blkBase, e.blkOut.reset();
                        }
                }
            }
            Mix(sig, &e.version, sizeof e.version), Mix(sig, &gen, sizeof gen), Mix(sig, &e.offHash, sizeof e.offHash), Mix(sig, &e.tblKey, sizeof e.tblKey), Mix(sig, &e.liveKey, sizeof e.liveKey);
            any = true;
            // (S8) the ceiling pass's ambient follows the room's now: Rooms at Night's sliders and the lamp edits taken, no solve
            const auto ambient = [&] {
                float now[3];
                const bool haveNow = ReadAmbient(r.room, now);
                if (haveNow) RtLampModel::AmbScale(e.ambSolve, now, e.ambScale);
                Mix(sig, e.ambScale, sizeof e.ambScale);
                // (room_bounce) the neighbours' lamp shares now, in steps of 1/1024 (a quarter of a map level): a drag moves them with no solve,
                // and a room not listing the lamp relights only when a step is crossed
                e.bounceN = 0;
                if (DevSwitches::RoomBounce() && !e.outdoorRoom) {
                    ArchAmbient::BounceRow b[ArchAmbient::kBounceRows];
                    int n = 0;
                    for (const LevelLightShare::ArchSpan& a : e.arches) {
                        float s[3];
                        if (n == ArchAmbient::kBounceRows || !UnlitRooms::LampAmbient(a.nb, s)) continue;
                        const float scale = e.norm / ReadNorm(a.nb);
                        b[n] = {a.ax, a.az, a.bx, a.bz, a.y0, a.y1, {}};
                        for (int c = 0; c < 3; c++) b[n].e[c] = std::round(s[c] * scale * 1024.0f) / 1024.0f;
                        // (the blend's reach, in steps of 1/8 m for the same reason as the shares)
                        float nb[3];
                        if (haveNow && ReadAmbient(a.nb, nb)) {
                            float reach = ArchAmbient::Reach(std::hypot(a.bx - a.ax, a.bz - a.az), now, nb);
                            for (const LevelLightShare::ArchSpan& o : e.arches)
                                if (o.nbId != a.nbId) reach = ArchAmbient::HeldReach(reach, a.ax, a.az, a.bx, a.bz, o.ax, o.az, o.bx, o.bz);
                            if (reach > ArchAmbient::kReach && LevelLightShare::ArchAmbientShared(r.room, now)) reach = ArchAmbient::kReach;
                            b[n].reach = std::round(reach * 8.0f) / 8.0f;
                        }
                        n++;
                    }
                    e.bounceN = ArchAmbient::BounceConsts(b, n, e.bounceRows, e.bounceHue);
                    if (e.bounceN) Mix(sig, e.bounceRows, sizeof e.bounceRows), Mix(sig, e.bounceHue, sizeof e.bounceHue);
                }
                // (what its draw reads beside the round's rules: its kept solve, the occluders, its lamps' laws and rows, the ambient)
                uint64_t k = 1469598103934665603ull;
                Mix(k, &e.version, sizeof e.version), Mix(k, &gen, sizeof gen), Mix(k, &e.offHash, sizeof e.offHash), Mix(k, &e.physSig, sizeof e.physSig),
                    Mix(k, e.ambScale, sizeof e.ambScale), Mix(k, &e.tblKey, sizeof e.tblKey), Mix(k, &e.liveKey, sizeof e.liveKey), Mix(k, &e.bounceN, sizeof e.bounceN);
                if (e.bounceN) Mix(k, e.bounceRows, sizeof e.bounceRows), Mix(k, e.bounceHue, sizeof e.bounceHue);
                e.drawKey = k;
            };
            const uint64_t lotNow = LotRoundSig(e.lot);
            if (e.physCached && e.physLot == lotNow && e.physGen == gen) { // (its lamp list as last built: the same signature)
                Mix(sig, &e.physSig, sizeof e.physSig);
                ambient();
                continue;
            }
            // (stage 4) the lot's lamps whose reach meets the map: the solve's lamps out of its base with their rows (a fresh row
            // once registered in another room), then every other lot lamp but those its base holds with a fresh row
            // (phys_lamp_pick.h: never one counted twice, in the base and drawn)
            const std::vector<uintptr_t> prev = std::move(e.lamps);
            e.lamps.clear(), e.physMult.clear(), e.physReach.clear(), e.lotLaws.clear(), e.world.clear(), e.lawOk.clear();
            uint64_t es = 1469598103934665603ull;
            const auto fresh = [&](const OutdoorDelta::LotLamp& L, float& mult, char& reach) {
                mult = PhysLampPick::NormKFresh(L.story, L.home, L.homeNorm, e.story, e.id, e.norm, e.outdoorRoom);
                reach = PhysLampPick::ReachFresh(L.homeOutdoor, e.outdoorRoom, e.id == 0 && L.story + 1 == e.story && RtOutdoorLamps::RoomOpenAbove(L.story, L.home)) ? 1 : 0;
            };
            const auto draw = [&](uintptr_t light, size_t at, float mult, char reach) {
                GpuLampLaw::Lamp w;
                RtLampModel::Lamp lotLaw;
                // (the law with its reach radii: the room table's reach column and Params::reachOn decide where they apply)
                if (!worldOf(light, at, w) || !PhysLampPick::Meets(e.box, w.pos, PhysLampPick::Reach2(w, mult, e.thr)) ||
                    !RtOutdoorLamps::WorldLaw(light, e.lot, true, lotLaw.law, type, reg, e.xf))
                    return;
                lotLaw.mult = mult;
                e.lamps.push_back(light), e.physMult.push_back(mult), e.physReach.push_back(reach), e.lotLaws.push_back(lotLaw), e.lawOk.push_back(1);
                e.world.push_back({w.pos[0], w.pos[1], w.pos[2]});
                Mix(es, &light, sizeof light), Mix(es, &lotLaw, sizeof lotLaw), Mix(es, &reach, 1);
            };
            for (size_t l = 0; l < e.listed.size(); l++) {
                if (std::find(g_lotGone.begin(), g_lotGone.end(), e.listed[l]) != g_lotGone.end()) continue; // (deleted: its light may be freed)
                float mult = l < e.listedMult.size() ? e.listedMult[l] : 1.0f;
                char reach = l < e.listedReach.size() ? e.listedReach[l] : 0;
                const auto at = g_lotAt.find(e.listed[l]);
                if (at != g_lotAt.end() && l < e.listedHome.size() && g_lot[at->second].home != e.listedHome[l]) fresh(g_lot[at->second], mult, reach);
                draw(e.listed[l], at != g_lotAt.end() ? at->second : SIZE_MAX, mult, reach);
            }
            for (size_t i = 0; i < g_lot.size(); i++) {
                const OutdoorDelta::LotLamp& L = g_lot[i];
                if (PhysLampPick::UseOf(L.light, e.listed, e.baked) != PhysLampPick::Use::Fresh) continue;
                float mult = 1.0f;
                char reach = 0;
                fresh(L, mult, reach);
                draw(L.light, i, mult, reach);
            }
            const size_t n = e.lamps.size();
            Mix(es, &n, sizeof n);
            if (es != e.physSig) { // (the physical edits whose lamp it drew or draws: their maps, the edit's log)
                e.physSig = es;
                std::lock_guard<std::mutex> lk(g_editMx);
                for (const PhysEdit& pe : g_physEdits)
                    if ((std::find(prev.begin(), prev.end(), pe.light) != prev.end() || std::find(e.lamps.begin(), e.lamps.end(), pe.light) != e.lamps.end()) &&
                        std::find(e.editHits.begin(), e.editHits.end(), pe.light) == e.editHits.end())
                        e.editHits.push_back(pe.light);
            }
            Mix(sig, &es, sizeof es);
            e.physSig = es;
            e.physLot = lotNow, e.physGen = gen;
            e.physCached = std::all_of(e.listed.begin(), e.listed.end(), [](uintptr_t l) { return g_lotAt.count(l) != 0; });
            ambient();
            continue;
        }
        // a lamp moved since its parts were kept (their shares follow its place): drawn with the law its parts were kept with (the
        // room's last parts as they were, never its law now over them) until a copy with its parts at its new place is kept (the
        // move worker) or the room is solved
        // (a tube, its colour alone: its intensity, ends or radius changed, which its parts' share holds)
        std::vector<const RtLampGpu::KeptMapData::RoomLamp*> movedLamps;
        for (const auto& rl : e.roomLamps) {
            GpuLampLaw::Lamp now;
            float colour[3] = {};
            uint64_t geo = 0;
            // (switched off, its colour 0: a switch, whatever its share's inputs read then)
            if (rl.colourOnly ? RtOutdoorLamps::ColourLaw(rl.light, e.lot, colour, geo) && geo != rl.geo && colour[0] + colour[1] + colour[2] > 0.0f
                              : RtOutdoorLamps::WorldLaw(rl.light, e.lot, false, now, type, reg) &&
                                    GpuLampLaw::Moved(now, rl.law, kMoved))
                movedLamps.push_back(&rl);
        }
        const bool moved = !movedLamps.empty();
        e.moved = moved;
        Mix(sig, &e.version, sizeof e.version);
        Mix(sig, &moved, sizeof moved);
        any = true;
        e.laws.resize(e.lamps.size());
        e.lawOk.assign(e.lamps.size(), 0);
        for (size_t l = 0; l < e.lamps.size(); l++) {
            float colour[3] = {};
            uint64_t geo = 0;
            if (l < e.colourOnly.size() && e.colourOnly[l]) {
                e.laws[l] = GpuLampLaw::Lamp{};
                if (RtOutdoorLamps::ColourLaw(e.lamps[l], e.lot, colour, geo)) std::memcpy(e.laws[l].colour, colour, sizeof colour), e.lawOk[l] = 2;
            } else
                e.lawOk[l] = RtOutdoorLamps::WorldLaw(e.lamps[l], e.lot, false, e.laws[l], type, reg) ? 1 : 0;
            for (const auto* rl : movedLamps) // (its parts' own law)
                if (rl->light == e.lamps[l] && e.lawOk[l]) e.laws[l] = rl->law;
        }
        Mix(sig, e.lawOk.data(), e.lawOk.size());
        for (size_t l = 0; l < e.lamps.size(); l++)
            if (e.lawOk[l]) Mix(sig, &e.laws[l], sizeof e.laws[l]);
    }
    return any || !g_refs.empty() ? sig : 0;
}

namespace {
// (physical indoor lamps) the entry's lamps drawn from their laws over its phys points (rt_lamp_gpu.h RelightKeptPhys): a shadow
// tile per lamp with a law (rendered unless the atlas holds it), the room's furniture blockers, one room row (mult x norm scale,
// the reach share where the solve applies it: Params::reachOn x the lamp's flag x a wall texel). More lamps than the atlas's
// rows: several passes over the same sum. False (err) leaves the game's map
// hiOut (sharper floors): the entry's sub-texels into that 2w x 2h overlay instead (RelightKeptPhysHi)
bool RelightPhys(IDirect3DDevice9* dev, RtLampGpu::Engine& eng, Entry& e, const RtLampGpu::Params& base, IDirect3DBaseTexture9* game, IDirect3DTexture9* out, int w, int h,
                 std::string& err, IDirect3DTexture9* hiOut = nullptr) {
    std::vector<RtLampModel::Lamp> lamps;
    std::vector<std::array<float, 3>> world;
    std::vector<uintptr_t> lights;
    std::vector<float> reach;
    for (size_t l = 0; l < e.lamps.size(); l++)
        if (e.lawOk[l])
            lamps.push_back(e.lotLaws[l]), world.push_back(e.world[l]), lights.push_back(e.lamps[l]),
                reach.push_back(l < e.physReach.size() && e.physReach[l] ? 1.0f : 0.0f);
    const int n = static_cast<int>(lamps.size());
    if (!eng.SetBlockers(dev, e.blockers, err)) return false;
    RtLampGpu::Params p = base;
    p.thr = e.thr, p.mask = false, p.maskAllGpu = false, p.keptX = false, p.physX = false, p.debugLamp = -1;
    p.toneSoft = DevSwitches::ToneSoft(), p.paneFirst = e.paneFirst, p.paneCount = e.paneCount, p.bounceN = e.bounceN;
    if (e.bounceN) std::memcpy(p.bounceRows, e.bounceRows, sizeof p.bounceRows), std::memcpy(p.bounceHue, e.bounceHue, sizeof p.bounceHue);
    return PhysPasses(dev, eng, lights.data(), lamps.data(), n, base.farDist, err, [&](int g0, int gn, const int* tiles, bool last) {
        std::vector<RtLampGpu::Engine::PhysRoom> room(1);
        room[0].header = e.blkHeader;
        for (int i = 0; i < gn; i++) {
            // (a lamp of another room: the joined rooms' records too; the table's z = 2, T3dDensePS. Its own room's lamps walk their own)
            const auto at = g_lotAt.find(lights[g0 + i]);
            const bool cross = e.crossHead && at != g_lotAt.end() && (g_lot[at->second].home != e.id || g_lot[at->second].story != e.story);
            room[0].lamp.push_back({lamps[g0 + i].mult, reach[g0 + i], cross ? 2.0f : 0.0f});
            room[0].fill.push_back(e.blockers ? e.blockers->FillOf(static_cast<int>(e.blkHeader) + (cross ? 1 : 0), world[g0 + i].data()) : 0.0f);
        }
        if (hiOut)
            return eng.RelightKeptPhysHi(dev, e.gpu, lamps.data() + g0, tiles, reinterpret_cast<const float (*)[3]>(world.data() + g0), gn, room, p, hiOut, 2 * w, 2 * h, err,
                                         g0 == 0, last, e.ambScale);
        return eng.RelightKeptPhys(dev, e.gpu, lamps.data() + g0, tiles, reinterpret_cast<const float (*)[3]>(world.data() + g0), gn, room, p, game, out, w, h, err, g0 == 0,
                                   last, e.ambScale);
    });
}
} // namespace

bool PhysPasses(IDirect3DDevice9* dev, RtLampGpu::Engine& eng, const uintptr_t* lights, const RtLampModel::Lamp* lamps, int n, float farDist, std::string& err,
                const std::function<bool(int first, int count, const int* tiles, bool last)>& pass) {
    const uint32_t gen = RtOutdoorLamps::KeptMeshGen();
    if (!gen) return err = "physical: no occluders with open tops uploaded", false;
    if (n > kMaxLamps) return err = std::format("physical: {} lamps, at most {} are drawn", n, kMaxLamps), false;
    for (int i = 0; i < n; i++)
        if (std::find(g_tileSeen.begin(), g_tileSeen.end(), lights[i]) == g_tileSeen.end()) g_tileSeen.push_back(lights[i]);
    const int before = eng.TileRows();
    bool kept = false;
    if (!eng.EnsureTiles(dev, PhysTiles::Want(g_tiles.rows, n, static_cast<int>(g_tileSeen.size())), err, &kept)) return false;
    const int rows = eng.TileRows();
    // (grown with its tiles copied: each lamp's shadow kept at its index; made again: no tile holds a shadow)
    if (rows != before || rows != g_tiles.rows) {
        if (kept && before == g_tiles.rows && before > 0) g_tiles.rows = rows, g_tileGrowths[0]++;
        else {
            uint32_t why[PhysTiles::Atlas::kWhys];
            std::memcpy(why, g_tiles.why, sizeof why);
            g_tiles = {}, g_tiles.rows = rows, g_tileGrowths[1]++;
            std::memcpy(g_tiles.why, why, sizeof why);
        }
    }
    const int step = std::min(rows, PhysTiles::kPass);
    for (int g0 = 0; g0 == 0 || g0 < n; g0 += step) { // (a pass per `step` lamps; none: one pass, the sum 0)
        const int gn = std::min(step, n - g0);
        int tiles[PhysTiles::kPass] = {};
        bool render[PhysTiles::kPass] = {};
        g_tiles.Assign(gn, lights + g0, [&](int i) { return lamps[g0 + i].law.pos; }, gen, tiles, render);
        for (int i = 0; i < gn; i++)
            if (render[i] && !eng.RenderShadowTile(dev, lamps[g0 + i].law.pos, tiles[i], farDist, err)) return g_tiles.t[tiles[i]] = {}, false;
        if (!pass(g0, gn, tiles, g0 + step >= n)) return false;
    }
    return true;
}

namespace {
// What a physical room's draw reads beside its own inputs (drawKey): room 0's rules it takes (RelightPhys), the game's map, the size
uint64_t KeptRules(const RtLampGpu::Params& p, const IDirect3DBaseTexture9* game, int w, int h) {
    uint64_t k = 1469598103934665603ull;
    const float f[] = {p.radius, p.aa, p.normalOffset, p.biasM, p.biasTexels, p.farDist, p.edgeR};
    const bool b[] = {p.fade, p.reachOn, p.fast, DevSwitches::ToneSoft()};
    Mix(k, f, sizeof f), Mix(k, b, sizeof b), Mix(k, &game, sizeof game), Mix(k, &w, sizeof w), Mix(k, &h, sizeof h);
    return k;
}
// The copy's entries now: the set its last full draw had, all physical (drawKey) and no two writing the same place, with the same
// rules into the same copy: then a room whose draw inputs did not change holds its last draw at its places (a room's compose reads
// only its own sum and the game's map, never the copy: its places come out as a full draw over the game's copy would leave them)
bool SameCopy(uintptr_t mgr, int cls, int m, const IDirect3DTexture9* out, uint64_t rules) {
    uint64_t set = 1469598103934665603ull;
    bool allPhys = true;
    for (const auto& [key, e] : g_entries)
        if (e.mgr == mgr && e.cls == cls && e.m == m) {
            const void* room = std::get<0>(key);
            Mix(set, &room, sizeof room), Mix(set, &e.version, sizeof e.version), Mix(set, &e.ok, sizeof e.ok);
            allPhys = allPhys && (!e.ok || e.physical);
        }
    const auto it = g_copies.find({mgr, cls, m});
    return it != g_copies.end() && it->second.valid && it->second.disjoint && it->second.set == set && allPhys && it->second.out == out &&
           it->second.rules == rules;
}
// (a room holding an edit's lamp is drawn: its draw marks the edit shown there)
bool Unchanged(const Entry& e) { return e.ok && e.physical && e.drawnKey && e.drawnKey == e.drawKey && e.editHits.empty(); }
const std::vector<RtLampGpu::Block>& RunsOf(Entry& e) {
    if (e.runs.empty() && !e.writes.empty()) e.runs = RtLampGpu::RunsOf(e.writes);
    return e.runs;
}
} // namespace

bool KeepsRooms(uintptr_t mgr, int cls, int m, const IDirect3DTexture9* out, const IDirect3DBaseTexture9* game, int w, int h, const RtLampGpu::Params& p) {
    return SameCopy(mgr, cls, m, out, KeptRules(p, game, w, h)) && std::any_of(g_entries.begin(), g_entries.end(), [&](const auto& kv) {
               return kv.second.mgr == mgr && kv.second.cls == cls && kv.second.m == m && Unchanged(kv.second);
           });
}

bool ComposeOver(IDirect3DDevice9* dev, RtLampGpu::Engine& eng, uintptr_t mgr, int cls, int m, IDirect3DBaseTexture9* game, IDirect3DTexture9* out, int w, int h,
                 const RtLampGpu::Params& p, bool partial, IDirect3DTexture9* before) {
    // a partial relight: only the rooms whose draw inputs changed (SameCopy). A full draw with `before` (the copy as it was before
    // room 0's compose wrote over it, the game's map not written since): an unchanged room's places copied back from it
    const uint64_t rules = KeptRules(p, game, w, h);
    const bool same = (partial || before) && SameCopy(mgr, cls, m, out, rules);
    if (partial && !same) return false;
    CopyState& cs = g_copies[{mgr, cls, m}];
    bool drewAll = true;
    for (auto& [key, e] : g_entries) {
        if (!e.ok || e.mgr != mgr || e.cls != cls || e.m != m) continue;
        if (partial && e.drawnKey == e.drawKey) continue; // (its places hold its last draw, from the same inputs)
        if (!partial && same && Unchanged(e)) {
            std::string err;
            if (eng.CopyRects(dev, RunsOf(e), before, out, w, h, err)) {
                NoteCover(e.story, {});
                g_roomsKept++;
                continue;
            }
            if (LogAllowed())
                LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {}: not taken from the copy, drawn ({})", e.story, e.id, e.cls, kMapName[m], err));
        }
        const void* room = std::get<0>(key);
        // the room's own map holds the working map's top-left corner (smaller or as large; a blurred wall atlas exactly)
        if (w > e.gpu.w || h > e.gpu.h || (e.gpu.blur && (w != e.gpu.w || h != e.gpu.h))) {
            if (LogAllowed())
                LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {}: kept for a {}x{} map, the map is {}x{} (left as the game's)", e.story, e.id, e.cls, kMapName[m],
                                     e.gpu.w, e.gpu.h, w, h));
            if (e.physical) NoteCover(e.story, "a map not of the kept size"), OutdoorDelta::SolveTouched(room);
            Drop(e, room);
            if (partial) return false; // (the full draw puts the game's map back under it)
            drewAll = false;
            continue;
        }
        std::string err;
        LARGE_INTEGER c0{};
        QueryPerformanceCounter(&c0);
        if (e.physical ? !RelightPhys(dev, eng, e, p, game, out, w, h, err) : !eng.RelightKept(dev, e.gpu, e.laws.data(), e.lawOk.data(), e.thr, game, out, w, h, err)) {
            if (LogAllowed()) LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {}: not drawn ({})", e.story, e.id, e.cls, kMapName[m], err));
            if (e.physical) {
                NoteCover(e.story, err);
                // (stage 4: its lamps' edits may have been taken without its solve, which the game's map then lacks; the rooms of
                // earlier ones relit since: OutdoorDelta's touched rooms)
                if (!e.editHits.empty()) LevelLightShare::QueueRoomAgain(room);
                else OutdoorDelta::SolveTouched(room);
            }
            Drop(e, room);
            RetryLater(e, room);
            if (partial) return false; // (its places may hold part of this draw: the full draw puts the game's map back under it)
            drewAll = false;
            continue;
        }
        if (!partial) g_roomsDrawn++;
        if (e.physical) {
            NoteCover(e.story, {});
            LARGE_INTEGER c1{};
            QueryPerformanceCounter(&c1);
            g_costMaps++, g_costRound += MsSince(c0, c1);
            if (!e.editHits.empty()) {
                OutdoorDelta::NoteTouched(room); // (it shows an edited lamp its game's map may lack, until its next solve)
                std::lock_guard<std::mutex> lk(g_editMx);
                for (PhysEdit& pe : g_physEdits)
                    if (std::find(e.editHits.begin(), e.editHits.end(), pe.light) != e.editHits.end()) {
                        const auto key = std::make_tuple(room, e.cls, m);
                        if (std::find(pe.relit.begin(), pe.relit.end(), key) == pe.relit.end() && pe.relit.size() < 256) pe.relit.push_back(key);
                        QueryPerformanceCounter(&pe.lastAt);
                        pe.lastFrame = g_frame.load(std::memory_order_relaxed);
                        pe.relitSince = true;
                    }
                e.editHits.clear();
            }
        }
        QueryPerformanceCounter(&e.relitAt);
        e.relitFrame = g_frame.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(g_drawnMx);
            auto& d = g_drawn[room];
            if (d.first != e.version) d = {e.version, 0u};
            d.second |= 1u << m;
            g_drawnAny[room] |= 1u << m;
        }
        // (a move's copy has none: no read-back stall; nor during a drag, its first relight after the drag checks it)
        if (!e.checked && !e.expect.empty() && OutdoorDelta::RoomProbe() && !PhysMoveRecent(DragLite::kStillMs)) {
            e.checked = true;
            SelfCheck(dev, e, out, w, h);
        }
        e.drawnKey = e.drawKey;
    }
    if (partial) return true;
    // (a full draw: the set it leaves, a room dropped by it out of it; the places checked again only for another set)
    uint64_t now = 1469598103934665603ull;
    std::vector<const std::vector<uint32_t>*> writes;
    for (const auto& [key, e] : g_entries)
        if (e.mgr == mgr && e.cls == cls && e.m == m) {
            const void* room = std::get<0>(key);
            Mix(now, &room, sizeof room), Mix(now, &e.version, sizeof e.version), Mix(now, &e.ok, sizeof e.ok);
            if (e.ok) writes.push_back(&e.writes);
        }
    if (!cs.valid || cs.set != now) cs.disjoint = PhysLampPick::WritesDisjoint(w, h, writes);
    cs.set = now, cs.valid = drewAll, cs.rules = rules, cs.out = out;
    return true;
}

int ComposeHi(IDirect3DDevice9* dev, RtLampGpu::Engine& eng, uintptr_t mgr, int cls, int m, IDirect3DTexture9* copy, int w, int h,
              IDirect3DTexture9* over, bool redrawAll, IDirect3DTexture9* hi, const RtLampGpu::Params& p) {
    std::string err;
    const auto mine = [&](const Entry& e) { return e.ok && e.physical && e.mgr == mgr && e.cls == cls && e.m == m && e.gpu.physHi.t[0] && e.drawnKey; };
    // each room drawn into the copy since its sub-texels were: drawn again into the overlay (all of them after a full relight)
    for (auto& [key, e] : g_entries) {
        if (!mine(e)) continue;
        if (!redrawAll && e.hiKey == e.drawnKey) continue;
        e.hiKey = 0;
        if (!RelightPhys(dev, eng, e, p, nullptr, copy, w, h, err, over)) {
            if (LogAllowed()) LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {}: its sharper floor not drawn ({})", e.story, e.id, e.cls, kMapName[m], err));
            continue;
        }
        e.hiKey = e.drawnKey;
    }
    // the copy at twice its size, each room's sub-texels over it
    if (!eng.Upsample(dev, copy, hi, 2 * w, 2 * h, err)) return -1;
    int rooms = 0; // (none: the copy is read as before)
    for (auto& [key, e] : g_entries)
        if (mine(e) && e.hiKey == e.drawnKey) {
            if (!eng.CopyPlaces(dev, e.gpu, over, hi, 2 * w, 2 * h, err)) return -1;
            rooms++;
        }
    return rooms;
}

void ReleaseAll() {
    g_tiles = {};
    g_tileSeen.clear();
    for (auto& [key, e] : g_entries) e.gpu.Release();
    g_entries.clear();
    g_copies.clear();
    g_retries.clear();
    g_prebuilt.clear();
    g_bytes = 0;
    std::lock_guard<std::mutex> lk(g_drawnMx);
    g_drawn.clear(), g_drawnAny.clear();
}

void BeginFrame() {
    g_frame.fetch_add(1, std::memory_order_relaxed);
    OutdoorDelta::LiveFrame();
    // (physical, stage 4) the round's lot lamps, its relights' cost, and the edits whose maps are all relit (none relit in the last
    // 2 rounds, at least one relit; or none in 2 s: logged as such)
    g_costWorst = std::max(g_costWorst, g_costRound), g_costMs += g_costRound, g_costRound = 0.0;
    g_uploadWorst = std::max(g_uploadWorst, g_uploadRound), g_uploadRound = 0.0;
    static DWORD s_prebuiltAt = 0;
    if (const DWORD tick = GetTickCount(); g_prebuiltMaps && tick - s_prebuiltAt >= 10000) {
        if (LogAllowed())
            LOG_INFO(std::format("[GpuRoomLamps] Rooms solved again: {} maps' data built on workers ({:.1f} ms there), their uploads' largest frame {:.2f} ms on the render thread",
                                 g_prebuiltMaps, g_prebuiltMs, g_uploadWorst));
        s_prebuiltAt = tick, g_prebuiltMaps = 0, g_prebuiltMs = g_uploadWorst = 0.0;
    }
    g_lot.clear(), g_lotAt.clear(), g_lotGone.clear();
    if (OutdoorDelta::RoomLampCapture() && OutdoorDelta::RoomLampPhysical()) {
        OutdoorDelta::LotLamps(g_lot, &g_lotGone);
        for (size_t i = 0; i < g_lot.size(); i++) g_lotAt.emplace(g_lot[i].light, i);
    }
    std::erase_if(g_tileSeen, [](uintptr_t l) { return !g_lotAt.count(l); });
    g_tiles.NewRound();
    g_lotLaw.assign(g_lot.size(), LotLaw{});
    g_lotSigRead = false;
    std::vector<PhysEdit> done;
    {
        const uint32_t frame = g_frame.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_editMx);
        for (auto it = g_physEdits.begin(); it != g_physEdits.end();) {
            const bool waiting = std::any_of(g_entries.begin(), g_entries.end(), [&](const auto& kv) {
                return kv.second.ok && std::find(kv.second.editHits.begin(), kv.second.editHits.end(), it->light) != kv.second.editHits.end();
            });
            const bool shown = !it->relit.empty() && !waiting && frame - it->lastFrame >= 2 && (it->relitSince || GetTickCount() - it->tick > 2000);
            if (shown || (!waiting && it->relit.empty() && GetTickCount() - it->tick > 2000) || GetTickCount() - it->tick > kEditWaitMs) {
                done.push_back(std::move(*it));
                it = g_physEdits.erase(it);
            } else
                ++it;
        }
    }
    for (const PhysEdit& pe : done)
        if (LogAllowed())
            LOG_INFO(pe.relit.empty() ? std::format("[GpuRoomLamps] physical {}: lamp {:08X}, no room solved, no map relit (its reach meets no kept map drawn)", pe.kind, pe.light)
                                      : std::format("[GpuRoomLamps] physical {}: lamp {:08X}, no room solved, {} maps relit in {:.1f} ms (frames +{}){}", pe.kind, pe.light,
                                                    pe.relit.size(), MsSince(pe.at, pe.lastAt), pe.lastFrame - pe.frame,
                                                    pe.steps > 1 ? std::format(", {} steps", pe.steps) : std::string()));
    // (physical indoor lamps) the last round's cover per story, when it changed (room 0: NoteRoom0)
    for (const auto& [story, c] : g_cover) {
        std::string line = c.failed ? std::format("not covered ({} of {} maps not drawn: {})", c.failed, c.drawn + c.failed, c.why)
                                    : std::format("every map drawn ({} maps of its rooms > 0; room 0 {})", c.drawn, c.room0.empty() ? "from its solve" : c.room0);
        if (std::string& was = g_coverLogged[story]; was != line) {
            if (LogAllowed()) LOG_INFO(std::format("[GpuRoomLamps] Story {} physical: {}", story, line));
            was = std::move(line);
        }
    }
    g_cover.clear();
}
void NoteRoom0(int story, const std::string& how) {
    std::string& r = g_cover[story].room0;
    if (r.find(how) == std::string::npos && r.size() < 200) r += (r.empty() ? "" : "; ") + how;
}
bool EditPending(uintptr_t light) {
    std::lock_guard<std::mutex> lk(g_editMx);
    return std::any_of(g_edits.begin(), g_edits.end(), [&](const Edit& e) { return e.light == light; });
}
bool ShowsEdit(uintptr_t mgr, int cls, int m) {
    std::lock_guard<std::mutex> lk(g_editMx);
    for (const auto& [key, e] : g_entries) // (physical, stage 4: a map whose drawn lamps held or hold an edit's lamp)
        if (e.ok && e.physical && e.mgr == mgr && e.cls == cls && e.m == m && !e.editHits.empty()) return true;
    for (const Edit& ed : g_edits)
        for (const auto& [key, e] : g_entries)
            if (e.ok && e.mgr == mgr && e.cls == cls && e.m == m && e.relitAt.QuadPart <= ed.at.QuadPart &&
                std::find(ed.rooms.begin(), ed.rooms.end(), std::get<0>(key)) != ed.rooms.end() &&
                std::find(e.lamps.begin(), e.lamps.end(), ed.light) != e.lamps.end())
                return true;
    return false;
}
bool Drawn(const void* room, uint32_t version, unsigned maps) {
    std::lock_guard<std::mutex> lk(g_drawnMx);
    const auto it = g_drawn.find(room);
    return it != g_drawn.end() && it->second.first == version && (it->second.second & maps) == maps;
}
bool DrawnAny(const void* room, unsigned maps) {
    std::lock_guard<std::mutex> lk(g_drawnMx);
    const auto it = g_drawnAny.find(room);
    return it != g_drawnAny.end() && (it->second & maps) == maps;
}

void NoteEdit(uintptr_t light, const char* kind, const std::vector<const void*>& rooms, const std::string& stories) {
    Edit e;
    e.light = light, e.kind = kind, e.rooms = rooms, e.stories = stories;
    QueryPerformanceCounter(&e.at);
    e.tick = GetTickCount();
    e.frame = g_frame.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_editMx);
    if (g_edits.size() < 64) g_edits.push_back(std::move(e));
}

void NotePhysEdit(uintptr_t light, const char* kind) {
    if (std::strcmp(kind, "move") == 0) {
        // (a stamp never ahead of GetTickCount: "| 1" put an even tick's stamp 1 ms in the future for the rest of that ~16 ms tick,
        // every "within ms" read wrapped to false and held rooms were picked mid-drag, 6-14 solves per 10 s, 03/11 captures)
        const DWORD now = std::max<DWORD>(GetTickCount(), 1), was = g_physMoveAt.exchange(now, std::memory_order_relaxed);
        if (!was || now - was >= 300) g_physDragAt.store(now, std::memory_order_relaxed);
        g_physMoveLight.store(light, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lk(g_editMx);
    auto it = std::find_if(g_physEdits.begin(), g_physEdits.end(), [&](const PhysEdit& pe) { return pe.light == light; });
    if (it == g_physEdits.end()) {
        if (g_physEdits.size() >= 64) return;
        it = g_physEdits.insert(g_physEdits.end(), PhysEdit{});
        it->light = light;
    }
    it->kind = kind, it->steps++;
    QueryPerformanceCounter(&it->at);
    it->tick = GetTickCount();
    it->frame = g_frame.load(std::memory_order_relaxed);
    // (the latest step's relight is timed from it: "-8.8 ms (frames +4294967295)"; a relight drawn before its mark came stays)
    it->lastAt = it->at, it->lastFrame = it->frame, it->relitSince = false;
}
uintptr_t PhysDragLight(unsigned long ms) { return PhysMoveRecent(ms) ? g_physMoveLight.load(std::memory_order_relaxed) : 0; }
unsigned long PhysDragMs() {
    const DWORD at = g_physDragAt.load(std::memory_order_relaxed);
    return at && PhysMoveRecent(300) ? GetTickCount() - at : 0;
}
bool PhysMoveRecent(unsigned long ms) {
    const DWORD at = g_physMoveAt.load(std::memory_order_relaxed);
    return at && GetTickCount() - at < ms;
}
void TakeRelightCost(int& maps, double& ms, double& worst) {
    maps = g_costMaps, ms = g_costMs, worst = g_costWorst;
    g_costMaps = 0, g_costMs = g_costWorst = 0.0;
}
std::string TakeTileCost() {
    const auto& w = g_tiles.why;
    std::string s = std::format("; lamps' shadow cubes rendered {} (moved {}, occluders changed {}, new or atlas made again {}, another lamp's tile taken {}), "
                                "atlas of {} tiles grown {} times with its tiles kept, made again {} times; rooms in full draws of a map: {} drawn, {} unchanged "
                                "taken from the copy",
                                w[0] + w[1] + w[2] + w[3], w[0], w[1], w[2], w[3], g_tiles.rows, g_tileGrowths[0], g_tileGrowths[1], g_roomsDrawn, g_roomsKept);
    std::fill(std::begin(g_tiles.why), std::end(g_tiles.why), 0u), g_tileGrowths[0] = g_tileGrowths[1] = 0, g_roomsDrawn = g_roomsKept = 0;
    return s;
}

} // namespace GpuRoomLamps
