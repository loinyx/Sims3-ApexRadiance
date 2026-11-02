// GPU room lamps, stage 2b: the kept room solves drawn on the GPU (see gpu_room_lamps.h).
//
// Part of Apex Radiance. Credits: @loinyx

#include "gpu_room_lamps.h"
#include "rt_lamp_gpu.h"
#include "rt_outdoor_lamps.h"
#include "outdoor_delta.h"
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
    std::vector<char> colourOnly; // per lamp of `lamps`: its law is its colour alone (a TubeLight)
    LARGE_INTEGER relitAt{};
    uint32_t relitFrame = 0;
};
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
    long n = 0, within = 0;
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
    if (LogAllowed())
        LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {} GPU self-check: {} texels, {:.2f}% within 2/255 (alpha within {}/255) of the texels the game left{}",
                             e.story, e.id, e.cls, kMapName[e.m], n, n ? 100.0 * static_cast<double>(within) / static_cast<double>(n) : 100.0, alphaMax, ws));
}
// The edits taken whose rooms are all relit since (render thread)
void LogEdits() {
    std::vector<Edit> done;
    {
        std::lock_guard<std::mutex> lk(g_editMx);
        if (g_edits.empty()) return;
        for (auto it = g_edits.begin(); it != g_edits.end();) {
            bool relit = true;
            for (const void* room : it->rooms)
                for (const auto& [key, e] : g_entries)
                    if (std::get<0>(key) == room && e.ok && e.relitAt.QuadPart <= it->at.QuadPart) relit = false;
            if (relit || GetTickCount() - it->tick > kEditWaitMs) {
                if (!relit) it->kind = nullptr; // (logged as not shown)
                done.push_back(std::move(*it));
                it = g_edits.erase(it);
            } else
                ++it;
        }
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    for (const Edit& e : done) {
        if (!LogAllowed()) continue;
        // the frame each map kind of its rooms was shown in, after the edit's (+0: the frame of the edit)
        std::string frames;
        for (int m = 0; m < 4; m++) {
            int last = -1;
            for (const auto& [key, en] : g_entries)
                if (en.m == m && en.ok && std::find(e.rooms.begin(), e.rooms.end(), std::get<0>(key)) != e.rooms.end())
                    last = std::max(last, static_cast<int>(en.relitFrame - e.frame));
            if (last >= 0) frames += std::format("{}{} +{}", frames.empty() ? " (frames:" : ",", kMapName[m], last);
        }
        if (!frames.empty()) frames += ")";
        if (!e.kind)
            LOG_INFO(std::format("[GpuRoomLamps] Lamp {:08X}: its edit was not shown in every room within {} ms (rooms on stories {})", e.light, kEditWaitMs, e.stories));
        else
            LOG_INFO(std::format("[GpuRoomLamps] {}: lamp {:08X}, {} rooms on stories {}, no room solved, relit in {:.1f} ms{}", e.kind, e.light, e.rooms.size(), e.stories,
                                 MsSince(e.at, now), frames));
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
                e.story = d.story, e.lot = d.lot, e.thr = d.thr;
                if (RtLampGpu::Engine::UploadKept(dev, d, e.gpu)) {
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
                                    (std::fabs(now.pos[0] - rl.pos[0]) > kMoved || std::fabs(now.pos[1] - rl.pos[1]) > kMoved || std::fabs(now.pos[2] - rl.pos[2]) > kMoved))
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

void ComposeOver(IDirect3DDevice9* dev, RtLampGpu::Engine& eng, uintptr_t mgr, int cls, int m, IDirect3DBaseTexture9* game, IDirect3DTexture9* out, int w, int h) {
    for (auto& [key, e] : g_entries) {
        if (!e.ok || e.mgr != mgr || e.cls != cls || e.m != m) continue;
        const void* room = std::get<0>(key);
        // the room's own map holds the working map's top-left corner (smaller or as large; a blurred wall atlas exactly)
        if (w > e.gpu.w || h > e.gpu.h || (e.gpu.blur && (w != e.gpu.w || h != e.gpu.h))) {
            if (LogAllowed())
                LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {}: kept for a {}x{} map, the map is {}x{} (left as the game's)", e.story, e.id, e.cls, kMapName[m],
                                     e.gpu.w, e.gpu.h, w, h));
            Drop(e, room);
            continue;
        }
        std::string err;
        if (!eng.RelightKept(dev, e.gpu, e.laws.data(), e.lawOk.data(), e.thr, game, out, w, h, err)) {
            if (LogAllowed()) LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {}: not drawn ({})", e.story, e.id, e.cls, kMapName[m], err));
            Drop(e, room);
            continue;
        }
        QueryPerformanceCounter(&e.relitAt);
        e.relitFrame = g_frame.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(g_drawnMx);
            auto& d = g_drawn[room];
            if (d.first != e.version) d = {e.version, 0u};
            d.second |= 1u << m;
        }
        if (!e.checked && OutdoorDelta::RoomProbe()) {
            e.checked = true;
            SelfCheck(dev, e, out, w, h);
        }
    }
}

void ReleaseAll() {
    for (auto& [key, e] : g_entries) e.gpu.Release();
    g_entries.clear();
    g_bytes = 0;
    std::lock_guard<std::mutex> lk(g_drawnMx);
    g_drawn.clear();
}

void BeginFrame() { g_frame.fetch_add(1, std::memory_order_relaxed); }
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
