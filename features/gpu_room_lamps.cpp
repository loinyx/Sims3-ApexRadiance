// GPU room lamps, stage 2b: the kept room solves drawn on the GPU (see gpu_room_lamps.h).
//
// Part of Apex Radiance. Credits: @loinyx

#include "gpu_room_lamps.h"
#include "rt_lamp_gpu.h"
#include "rt_outdoor_lamps.h"
#include "outdoor_delta.h"
#include "apex_log.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <tuple>
#include <vector>

namespace GpuRoomLamps {
namespace {

constexpr const char* kMapName[4] = {"wall atlas", "floor", "ceiling", "objects"};

struct Entry {
    uintptr_t mgr = 0;
    int cls = 0, m = 0, story = 0, id = 0;
    uint32_t version = 0;
    uint64_t lot = 0;
    float thr = 0.0f;
    bool ok = false, checked = false;
    RtLampGpu::KeptMapGpu gpu;
    std::vector<uintptr_t> lamps;
    std::vector<GpuLampLaw::Lamp> laws;
    std::vector<char> lawOk;
    std::vector<std::pair<uint32_t, uint32_t>> expect; // (place, the game's texel) for the self-check
};
std::map<std::tuple<const void*, int, int>, Entry> g_entries; // (room, class, map); render thread
std::vector<OutdoorDelta::KeptRef> g_refs;
size_t g_bytes = 0;
long c_built = 0, c_relit = 0, c_fails = 0;
std::atomic<int> g_lines{0};
bool LogAllowed() { return g_lines.fetch_add(1, std::memory_order_relaxed) < 2000; }

void Drop(Entry& e) {
    g_bytes -= std::min(g_bytes, e.gpu.bytes);
    e.gpu.Release();
    e.ok = false;
}
void Mix(uint64_t& h, const void* p, size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
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
        if (place >= px.size()) continue;
        const uint32_t got = px[place];
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
    for (const auto& [d, j] : worst)
        ws += std::format("; {} levels at ({}, {}): game {:08X} GPU {:08X}", d, e.expect[j].first % static_cast<uint32_t>(w), e.expect[j].first / static_cast<uint32_t>(w),
                          e.expect[j].second, px[e.expect[j].first]);
    if (LogAllowed())
        LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {} GPU self-check: {} texels, {:.2f}% within 2/255 (alpha within {}/255) of the texels the game left{}",
                             e.story, e.id, e.cls, kMapName[e.m], n, n ? 100.0 * static_cast<double>(within) / static_cast<double>(n) : 100.0, alphaMax, ws));
}

} // namespace

uint64_t Sig(IDirect3DDevice9* dev, uintptr_t mgr, int cls, int m) {
    if (!OutdoorDelta::RoomLampCapture()) {
        if (!g_entries.empty()) ReleaseAll();
        return 0;
    }
    OutdoorDelta::KeptRooms(mgr, cls, g_refs);
    // this map's entries whose kept solve went or was kept again
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        Entry& e = it->second;
        const void* room = std::get<0>(it->first);
        const bool here = e.mgr == mgr && e.cls == cls && e.m == m;
        if (here && std::none_of(g_refs.begin(), g_refs.end(), [&](const OutdoorDelta::KeptRef& r) { return r.room == room && r.version == e.version; })) {
            Drop(e);
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
                    e.expect = std::move(d.expect);
                    g_bytes += e.gpu.bytes;
                    c_built++;
                } else if (LogAllowed())
                    LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {}: its buffers could not be made (left as the game's)", d.story, r.id, cls, kMapName[m]));
            }
        }
        if (!e.ok) continue;
        any = true;
        e.laws.resize(e.lamps.size());
        e.lawOk.assign(e.lamps.size(), 0);
        for (size_t l = 0; l < e.lamps.size(); l++) {
            int type = 0, reg = -1;
            e.lawOk[l] = RtOutdoorLamps::WorldLaw(e.lamps[l], e.lot, false, e.laws[l], type, reg) ? 1 : 0;
        }
        Mix(sig, &e.version, sizeof e.version);
        Mix(sig, e.lawOk.data(), e.lawOk.size());
        for (size_t l = 0; l < e.lamps.size(); l++)
            if (e.lawOk[l]) Mix(sig, &e.laws[l], sizeof e.laws[l]);
    }
    return any ? sig : 0;
}

void ComposeOver(IDirect3DDevice9* dev, RtLampGpu::Engine& eng, uintptr_t mgr, int cls, int m, IDirect3DBaseTexture9* game, IDirect3DTexture9* out, int w, int h) {
    for (auto& [key, e] : g_entries) {
        if (!e.ok || e.mgr != mgr || e.cls != cls || e.m != m) continue;
        if (e.gpu.w != w || e.gpu.h != h) {
            if (LogAllowed())
                LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {}: kept for a {}x{} map, the map is {}x{} (left as the game's)", e.story, e.id, e.cls, kMapName[m],
                                     e.gpu.w, e.gpu.h, w, h));
            Drop(e);
            continue;
        }
        std::string err;
        if (!eng.RelightKept(dev, e.gpu, e.laws.data(), e.lawOk.data(), e.thr, game, out, err)) {
            c_fails++;
            if (LogAllowed()) LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} {}: not drawn ({})", e.story, e.id, e.cls, kMapName[m], err));
            Drop(e);
            continue;
        }
        c_relit++;
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
}

} // namespace GpuRoomLamps
