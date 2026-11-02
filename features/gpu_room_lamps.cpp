// GPU room lamps, stage 2b: the kept room solves drawn on the GPU (see gpu_room_lamps.h).
//
// Part of Apex Radiance. Credits: @loinyx

#include "gpu_room_lamps.h"
#include "rt_lamp_gpu.h"
#include "rt_outdoor_lamps.h"
#include "outdoor_delta.h"
#include "rt_lamp_model.h"
#include "apex_log.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
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
    RtLampGpu::BlockerTable blockers;
    float blkHeader = -1.0f;
    float xf[16] = {};
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
        return 0;
    }
    LogEdits();
    OutdoorDelta::KeptRooms(mgr, cls, g_refs);
    // this map's entries whose kept solve went or was kept again
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        Entry& e = it->second;
        const void* room = std::get<0>(it->first);
        const bool here = e.mgr == mgr && e.cls == cls && e.m == m;
        if (here && std::none_of(g_refs.begin(), g_refs.end(), [&](const OutdoorDelta::KeptRef& r) { return r.room == room && r.version == e.version; })) {
            Drop(e, room);
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
                    g_bytes += e.gpu.bytes;
                } else if (LogAllowed())
                    LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {}: its buffers could not be made (left as the game's)", d.story, r.id, cls, kMapName[m]));
            }
        }
        if (!e.ok) continue;
        int type = 0, reg = -1;
        if (e.physical) { // (every lamp with its law now: a lamp gone or without a law adds nothing; no parts, so no move rule)
            const uint32_t gen = RtOutdoorLamps::KeptMeshGen();
            Mix(sig, &e.version, sizeof e.version), Mix(sig, &gen, sizeof gen);
            any = true;
            e.lotLaws.assign(e.lamps.size(), RtLampModel::Lamp{});
            e.world.assign(e.lamps.size(), {});
            e.lawOk.assign(e.lamps.size(), 0);
            for (size_t l = 0; l < e.lamps.size(); l++) {
                GpuLampLaw::Lamp w;
                // (the law with its reach radii: the room table's reach column and Params::reachOn decide where they apply)
                if (!RtOutdoorLamps::WorldLaw(e.lamps[l], e.lot, false, w, type, reg) ||
                    !RtOutdoorLamps::WorldLaw(e.lamps[l], e.lot, true, e.lotLaws[l].law, type, reg, e.xf))
                    continue;
                e.lawOk[l] = 1;
                e.lotLaws[l].mult = l < e.physMult.size() ? e.physMult[l] : 1.0f;
                std::memcpy(e.world[l].data(), w.pos, sizeof w.pos);
                Mix(sig, &e.lotLaws[l], sizeof e.lotLaws[l]);
            }
            Mix(sig, e.lawOk.data(), e.lawOk.size()), Mix(sig, e.physReach.data(), e.physReach.size());
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
    const uint32_t gen = RtOutdoorLamps::KeptMeshGen();
    if (!gen) return err = "physical: no occluders with open tops uploaded", false;
    std::vector<RtLampModel::Lamp> lamps;
    std::vector<std::array<float, 3>> world;
    std::vector<uintptr_t> lights;
    std::vector<float> reach;
    for (size_t l = 0; l < e.lamps.size(); l++)
        if (e.lawOk[l])
            lamps.push_back(e.lotLaws[l]), world.push_back(e.world[l]), lights.push_back(e.lamps[l]),
                reach.push_back(l < e.physReach.size() && e.physReach[l] ? 1.0f : 0.0f);
    const int n = static_cast<int>(lamps.size());
    if (n > kMaxLamps) return err = std::format("physical: {} lamps, at most {} are drawn", n, kMaxLamps), false;
    const int rows = std::max(g_tileRows, std::min(kTiles, (std::max(n, 1) + 3) / 4 * 4));
    if (eng.TileRows() != rows) g_tiles = {}; // (made again: no tile holds a shadow)
    if (!eng.EnsureTiles(dev, rows, err)) return false;
    g_tileRows = rows;
    if (!eng.SetBlockers(dev, e.blockers, err)) return false;
    RtLampGpu::Params p = base;
    p.thr = e.thr, p.mask = false, p.maskAllGpu = false, p.keptX = false, p.debugLamp = -1;
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
            if (!eng.RenderShadowTile(dev, lamps[g0 + i].law.pos, t, base.farDist, err)) return false;
            g.light = lights[g0 + i], g.gen = gen;
            std::memcpy(g.pos, lamps[g0 + i].law.pos, sizeof g.pos);
            tiles[i] = t, taken[t] = true;
        }
        for (int i = 0; i < gn; i++) g_tiles[tiles[i]].used = g_tileUse;
        std::vector<RtLampGpu::Engine::PhysRoom> room(1);
        room[0].header = e.blkHeader;
        for (int i = 0; i < gn; i++) room[0].lamp.push_back({lamps[g0 + i].mult, reach[g0 + i]});
        if (!eng.RelightKeptPhys(dev, e.gpu, lamps.data() + g0, tiles.data(), reinterpret_cast<const float (*)[3]>(world.data() + g0), gn, room, p, game, out, w, h, err,
                                 g0 == 0, g0 + rows >= n))
            return false;
    }
    return true;
}
} // namespace

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
            if (e.physical) NoteCover(e.story, "a map not of the kept size");
            Drop(e, room);
            continue;
        }
        std::string err;
        if (e.physical ? !RelightPhys(dev, eng, e, p, game, out, w, h, err) : !eng.RelightKept(dev, e.gpu, e.laws.data(), e.lawOk.data(), e.thr, game, out, w, h, err)) {
            if (LogAllowed()) LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {}: not drawn ({})", e.story, e.id, e.cls, kMapName[m], err));
            if (e.physical) NoteCover(e.story, err);
            Drop(e, room);
            continue;
        }
        if (e.physical) NoteCover(e.story, {});
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
    // (physical indoor lamps) the last round's cover per story, when it changed (room 0 keeps its solve's light: not physical yet)
    for (const auto& [story, c] : g_cover) {
        std::string line = c.failed ? std::format("not covered ({} of {} maps not drawn: {})", c.failed, c.drawn + c.failed, c.why)
                                    : std::format("every map drawn ({} maps of its rooms > 0; room 0 from its solve)", c.drawn);
        if (std::string& was = g_coverLogged[story]; was != line) {
            if (LogAllowed()) LOG_INFO(std::format("[GpuRoomLamps] Story {} physical: {}", story, line));
            was = std::move(line);
        }
    }
    g_cover.clear();
}
bool EditPending(uintptr_t light) {
    std::lock_guard<std::mutex> lk(g_editMx);
    return std::any_of(g_edits.begin(), g_edits.end(), [&](const Edit& e) { return e.light == light; });
}
bool ShowsEdit(uintptr_t mgr, int cls, int m) {
    std::lock_guard<std::mutex> lk(g_editMx);
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

} // namespace GpuRoomLamps
