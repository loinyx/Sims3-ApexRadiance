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
    std::vector<uintptr_t> editHits;
    // (S8) a roofed room's ambient (+0x110) at its solve's end and the compose's scale of it now (RtLampModel::AmbScale)
    float ambSolve[3] = {}, ambScale[3] = {1.0f, 1.0f, 1.0f};
};
// (physical indoor lamps) the kept shadow atlas: a tile per lamp, kept while the lamp stays put and the occluders are the same.
// Its rows grow with the most lamps a map has drawn (4 at a time, up to kTiles: 6 x 256 x 256 x (G16R16F + D24X8) = 3.1 MB a
// row, 50 MB at 16); a map with more lamps is drawn in several passes over the same sum, its tiles rendered again in turn
constexpr int kTiles = 16;    // (6 x 256 by 16 x 256: within a 4096 texture limit)
constexpr int kMaxLamps = 64; // a map's lamps drawn at most (4 passes of 16 tiles: the frame's cost bounded)
struct Tile {
    uintptr_t light = 0;
    float pos[3] = {};
    uint32_t gen = 0, used = 0;
};
std::array<Tile, kTiles> g_tiles;
uint32_t g_tileUse = 0;
int g_tileRows = 0;
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
        std::memcpy(c, static_cast<const BYTE*>(room) + 0x110, 3 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The kept solves drawn: room -> (its version, the maps drawn with it); read by the light tree thread (Drawn)
std::mutex g_drawnMx;
std::unordered_map<const void*, std::pair<uint32_t, unsigned>> g_drawn;
void Undrawn(const void* room) {
    std::lock_guard<std::mutex> lk(g_drawnMx);
    g_drawn.erase(room);
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
};
std::vector<PhysEdit> g_physEdits; // (under g_editMx)
std::atomic<DWORD> g_physMoveAt{0};
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
std::vector<uintptr_t> g_lotGone; // (the lot's lamps that left its registry: a kept list holding one no longer draws it)
// (stage 4) the physical relights' CPU cost since TakeRelightCost (maps, ms, the largest round's ms) and this round's
int g_costMaps = 0;
double g_costMs = 0.0, g_costWorst = 0.0, g_costRound = 0.0;

void Drop(Entry& e, const void* room) {
    g_bytes -= std::min(g_bytes, e.gpu.bytes);
    e.gpu.Release();
    e.ok = false;
    Undrawn(room);
}
void Mix(uint64_t& h, const void* p, size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
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
    // this map's entries whose kept solve went or was kept again
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        Entry& e = it->second;
        const void* room = std::get<0>(it->first);
        const bool here = e.mgr == mgr && e.cls == cls && e.m == m;
        if (here && std::none_of(g_refs.begin(), g_refs.end(), [&](const OutdoorDelta::KeptRef& r) { return r.room == room && r.version == e.version; })) {
            Drop(e, room);
            if (e.physical && std::none_of(g_refs.begin(), g_refs.end(), [&](const OutdoorDelta::KeptRef& r) { return r.room == room; }))
                OutdoorDelta::SolveTouched(room); // (no kept solve of it any more: its game's map, which a lamp edit taken may lack)
            it = g_entries.erase(it);
        } else
            ++it;
    }
    uint64_t sig = 1469598103934665603ull;
    bool any = false;
    for (const OutdoorDelta::KeptRef& r : g_refs) {
        const auto [it, fresh] = g_entries.try_emplace(std::make_tuple(r.room, cls, m));
        Entry& e = it->second;
        if (fresh) {
            e.mgr = mgr, e.cls = cls, e.m = m, e.version = r.version, e.id = r.id;
            RtLampGpu::KeptMapData d;
            std::string why;
            if (OutdoorDelta::BuildKeptMap(r.room, cls, r.version, m, d, why)) { // (else nothing of it in this map: an entry that draws nothing)
                e.story = d.story, e.lot = d.lot, e.thr = d.thr, e.physical = d.physical;
                const bool space = !d.physical || RtOutdoorLamps::LotXform(mgr, e.xf);
                if (space && d.physical) // (the points to the lot's space; base keeps the world position)
                    for (RtLampGpu::TexelVertex& v : d.phys) {
                        float p[3], n[3];
                        for (int k = 0; k < 3; k++)
                            p[k] = v.base[0] * e.xf[k] + v.base[1] * e.xf[4 + k] + v.base[2] * e.xf[8 + k] + e.xf[12 + k],
                            n[k] = v.n[0] * e.xf[k] + v.n[1] * e.xf[4 + k] + v.n[2] * e.xf[8 + k];
                        std::memcpy(v.p, p, sizeof p), std::memcpy(v.n, n, sizeof n);
                    }
                if (!space) {
                    if (LogAllowed())
                        LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {}: physical: the lot's space could not be read (left as the game's)", d.story, r.id, cls,
                                             kMapName[m]));
                    OutdoorDelta::SolveTouched(r.room);
                } else if (RtLampGpu::Engine::UploadKept(dev, d, e.gpu)) {
                    e.physMult = std::move(d.physMult);
                    e.physReach = std::move(d.physReach);
                    e.blockers = std::move(d.blockers);
                    e.blkHeader = d.blkHeader;
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
                }
            }
        }
        if (!e.ok) continue;
        int type = 0, reg = -1;
        if (e.physical) { // (no parts, so no move rule: every lamp drawn with its law now; a lamp gone or without a law adds nothing)
            const uint32_t gen = RtOutdoorLamps::KeptMeshGen();
            Mix(sig, &e.version, sizeof e.version), Mix(sig, &gen, sizeof gen);
            any = true;
            // (stage 4) the lot's lamps whose reach meets the map: the solve's lamps out of its base with their rows (a fresh row
            // once registered in another room), then every other lot lamp but those its base holds with a fresh row
            // (phys_lamp_pick.h: never one counted twice, in the base and drawn)
            const std::vector<uintptr_t> prev = std::move(e.lamps);
            e.lamps.clear(), e.physMult.clear(), e.physReach.clear(), e.lotLaws.clear(), e.world.clear(), e.lawOk.clear();
            uint64_t es = 1469598103934665603ull;
            const auto fresh = [&](const OutdoorDelta::LotLamp& L, float& mult, char& reach) {
                mult = PhysLampPick::NormKFresh(L.story, L.home, L.homeNorm, e.story, e.id, e.norm, e.outdoorRoom);
                reach = PhysLampPick::ReachFresh(L.homeOutdoor, e.outdoorRoom) ? 1 : 0;
            };
            // (at: its index in g_lot, whose world law is read once a round; none: read here)
            const auto worldOf = [&](uintptr_t light, size_t at, GpuLampLaw::Lamp& w) {
                if (at >= g_lotLaw.size()) return RtOutdoorLamps::WorldLaw(light, e.lot, false, w, type, reg);
                LotLaw& c = g_lotLaw[at];
                if (!c.state || c.lot != e.lot) c.lot = e.lot, c.state = RtOutdoorLamps::WorldLaw(light, e.lot, false, c.w, type, reg) ? 1 : 2;
                if (c.state != 1) return false;
                w = c.w;
                return true;
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
            // (S8) the ceiling pass's ambient follows the room's now: Rooms at Night's sliders and the lamp edits taken, no solve
            float now[3];
            if (ReadAmbient(r.room, now)) RtLampModel::AmbScale(e.ambSolve, now, e.ambScale);
            Mix(sig, e.ambScale, sizeof e.ambScale);
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
bool RelightPhys(IDirect3DDevice9* dev, RtLampGpu::Engine& eng, Entry& e, const RtLampGpu::Params& base, IDirect3DBaseTexture9* game, IDirect3DTexture9* out, int w, int h,
                 std::string& err) {
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
    return PhysPasses(dev, eng, lights.data(), lamps.data(), n, base.farDist, err, [&](int g0, int gn, const int* tiles, bool last) {
        std::vector<RtLampGpu::Engine::PhysRoom> room(1);
        room[0].header = e.blkHeader;
        for (int i = 0; i < gn; i++) room[0].lamp.push_back({lamps[g0 + i].mult, reach[g0 + i], 0.0f});
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
    const int rows = std::max(g_tileRows, std::min(kTiles, (std::max(n, 1) + 3) / 4 * 4));
    if (eng.TileRows() != rows) g_tiles = {}; // (made again: no tile holds a shadow)
    if (!eng.EnsureTiles(dev, rows, err)) return false;
    g_tileRows = rows;
    for (int g0 = 0; g0 == 0 || g0 < n; g0 += rows) { // (a pass per `rows` lamps; none: one pass, the sum 0)
        const int gn = std::min(rows, n - g0);
        std::vector<int> tiles(gn, -1);
        std::array<bool, kTiles> taken{};
        g_tileUse++;
        for (int i = 0; i < gn; i++) // (a lamp's own tile when it holds its shadow now)
            for (int t = 0; t < rows; t++)
                if (const Tile& g = g_tiles[t]; !taken[t] && g.light == lights[g0 + i] && g.gen == gen && std::memcmp(g.pos, lamps[g0 + i].law.pos, sizeof g.pos) == 0) {
                    tiles[i] = t, taken[t] = true;
                    break;
                }
        for (int i = 0; i < gn; i++) {
            if (tiles[i] >= 0) continue;
            int t = -1; // the least recently used tile no lamp of this pass holds
            for (int c = 0; c < rows; c++)
                if (!taken[c] && (t < 0 || g_tiles[c].used < g_tiles[t].used)) t = c;
            Tile& g = g_tiles[t];
            g = Tile{};
            if (!eng.RenderShadowTile(dev, lamps[g0 + i].law.pos, t, farDist, err)) return false;
            g.light = lights[g0 + i], g.gen = gen;
            std::memcpy(g.pos, lamps[g0 + i].law.pos, sizeof g.pos);
            tiles[i] = t, taken[t] = true;
        }
        for (int i = 0; i < gn; i++) g_tiles[tiles[i]].used = g_tileUse;
        if (!pass(g0, gn, tiles.data(), g0 + rows >= n)) return false;
    }
    return true;
}

void ComposeOver(IDirect3DDevice9* dev, RtLampGpu::Engine& eng, uintptr_t mgr, int cls, int m, IDirect3DBaseTexture9* game, IDirect3DTexture9* out, int w, int h,
                 const RtLampGpu::Params& p) {
    for (auto& [key, e] : g_entries) {
        if (!e.ok || e.mgr != mgr || e.cls != cls || e.m != m) continue;
        const void* room = std::get<0>(key);
        // the room's own map holds the working map's top-left corner (smaller or as large; a blurred wall atlas exactly)
        if (w > e.gpu.w || h > e.gpu.h || (e.gpu.blur && (w != e.gpu.w || h != e.gpu.h))) {
            if (LogAllowed())
                LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {}: kept for a {}x{} map, the map is {}x{} (left as the game's)", e.story, e.id, e.cls, kMapName[m],
                                     e.gpu.w, e.gpu.h, w, h));
            if (e.physical) NoteCover(e.story, "a map not of the kept size"), OutdoorDelta::SolveTouched(room);
            Drop(e, room);
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
            continue;
        }
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
        }
        if (!e.checked && !e.expect.empty() && OutdoorDelta::RoomProbe()) { // (a move's copy has none: no read-back stall)
            e.checked = true;
            SelfCheck(dev, e, out, w, h);
        }
    }
}

void ReleaseAll() {
    g_tiles = {};
    g_tileRows = 0;
    for (auto& [key, e] : g_entries) e.gpu.Release();
    g_entries.clear();
    g_bytes = 0;
    std::lock_guard<std::mutex> lk(g_drawnMx);
    g_drawn.clear();
}

void BeginFrame() {
    g_frame.fetch_add(1, std::memory_order_relaxed);
    // (physical, stage 4) the round's lot lamps, its relights' cost, and the edits whose maps are all relit (none relit in the last
    // 2 rounds, at least one relit; or none in 2 s: logged as such)
    g_costWorst = std::max(g_costWorst, g_costRound), g_costMs += g_costRound, g_costRound = 0.0;
    g_lot.clear(), g_lotAt.clear(), g_lotGone.clear();
    if (OutdoorDelta::RoomLampCapture() && OutdoorDelta::RoomLampPhysical()) {
        OutdoorDelta::LotLamps(g_lot, &g_lotGone);
        for (size_t i = 0; i < g_lot.size(); i++) g_lotAt.emplace(g_lot[i].light, i);
    }
    g_lotLaw.assign(g_lot.size(), LotLaw{});
    std::vector<PhysEdit> done;
    {
        const uint32_t frame = g_frame.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_editMx);
        for (auto it = g_physEdits.begin(); it != g_physEdits.end();) {
            const bool waiting = std::any_of(g_entries.begin(), g_entries.end(), [&](const auto& kv) {
                return kv.second.ok && std::find(kv.second.editHits.begin(), kv.second.editHits.end(), it->light) != kv.second.editHits.end();
            });
            const bool shown = !it->relit.empty() && !waiting && frame - it->lastFrame >= 2;
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
    if (std::strcmp(kind, "move") == 0) g_physMoveAt.store(GetTickCount() | 1, std::memory_order_relaxed);
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
}
bool PhysMoveRecent(unsigned long ms) {
    const DWORD at = g_physMoveAt.load(std::memory_order_relaxed);
    return at && GetTickCount() - at < ms;
}
void TakeRelightCost(int& maps, double& ms, double& worst) {
    maps = g_costMaps, ms = g_costMs, worst = g_costWorst;
    g_costMaps = 0, g_costMs = g_costWorst = 0.0;
}

} // namespace GpuRoomLamps
