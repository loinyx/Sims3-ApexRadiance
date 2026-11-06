#pragma once
// Lighting scene recordings (docs/validation/dev-tools-scene-replay.md): what the physical indoor lamps' GPU path reads for a room's
// solve, and the game's own answer for each of its lamps there, written while a Recording capture runs (OutdoorDelta) and
// replayed offline through the current code (tools/scene_replay). Pure: no game access, no device.
//
// File: header, then blocks. 'L' = the lot's occluders (RtLampProxy::Lot) when their generation changes; 'R' = one room
// solve: its texels (world position, normal, map place, 3D test switch), the furniture blocker table rows, and per listed lamp
// with a law: its law (world space), mult, norm scale, reach flag and the game's part at each texel (the solve's evaluation
// of that lamp alone, the colour the room's sum took from it).
// The comparison (Compare): the GPU's term per lamp and texel as AccPhysPS takes it: mult x norm scale x the soft light cut of
// law x share, share = the cube shadow's visibility (here RtLampProxy::Visibility, the CPU reference) x the 3D test, dropped
// under 0.01; within 2/255 when every channel is within 2/255 of the game's part.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "gpu_lamp_law.h"
#include "room_light_tests.h"
#include "rt_lamp_blockers.h"
#include "rt_lamp_proxy.h"

namespace SceneRecord {

constexpr uint32_t kMagic = 0x53585041u; // "APXS"
constexpr uint32_t kVersion = 2; // (2: Edge::glazed written; a version 1 file holds whatever its padding held there)
struct FileHead {
    uint32_t magic = kMagic, version = kVersion;
    uint32_t edgeSize = sizeof(RtLampProxy::Edge), lawSize = sizeof(GpuLampLaw::Lamp);
};
struct RoomHead {
    uint32_t tick = 0, lotGen = 0;
    uint64_t lot = 0;
    int32_t story = 0, id = 0, cls = 0;
    float thr = 0.0f, norm = 1.0f, radius = 0.0f, blkHeader = -1.0f;
    uint8_t fade = 0, reachOn = 0, outdoor = 0, pad = 0;
    uint32_t texels = 0, lamps = 0, blkRows = 0;
};
struct Texel {
    float p[3], n[3]; // world space
    uint16_t x, y;
    uint8_t map, tested, pad[2];
};
struct Lamp {
    uint64_t light = 0;
    GpuLampLaw::Lamp law; // world space
    int32_t type = 0;
    float row = 1.0f; // mult x norm scale (the room table's x)
    uint8_t reach = 0, pad[3] = {};
    uint32_t parts = 0;
};
struct Part {
    uint32_t texel;
    float rgb[3];
};
struct Room {
    RoomHead h;
    std::vector<Texel> texels;
    std::vector<RtLampGpu::BlockerTable::Row> blk;
    std::vector<Lamp> lamps;
    std::vector<std::vector<Part>> parts; // per lamp
};
struct Scene {
    std::vector<std::pair<uint32_t, RtLampProxy::Lot>> lots; // (generation, lot)
    std::vector<Room> rooms;
};

// ---- writing (the game) ----
template <class T>
inline void Put(std::FILE* f, const T& v) { std::fwrite(&v, sizeof v, 1, f); }
template <class T>
inline void PutVec(std::FILE* f, const std::vector<T>& v) {
    Put(f, static_cast<uint32_t>(v.size()));
    if (!v.empty()) std::fwrite(v.data(), sizeof(T), v.size(), f);
}
inline void WriteHead(std::FILE* f) { Put(f, FileHead{}); }
inline void WriteLot(std::FILE* f, uint32_t gen, const RtLampProxy::Lot& lot) {
    Put(f, 'L'), Put(f, gen), Put(f, lot.worldToLot), Put(f, static_cast<uint32_t>(lot.stories.size()));
    for (const RtLampProxy::Story& s : lot.stories) {
        Put(f, s.story), Put(f, s.w), Put(f, s.h), Put(f, s.plane);
        PutVec(f, s.edges), PutVec(f, s.floorY), PutVec(f, s.roofed), PutVec(f, s.ceil), PutVec(f, s.placed), PutVec(f, s.removed), PutVec(f, s.room);
    }
}
inline void WriteRoom(std::FILE* f, const Room& r) {
    RoomHead h = r.h;
    h.texels = static_cast<uint32_t>(r.texels.size()), h.lamps = static_cast<uint32_t>(r.lamps.size()), h.blkRows = static_cast<uint32_t>(r.blk.size());
    Put(f, 'R'), Put(f, h);
    if (!r.texels.empty()) std::fwrite(r.texels.data(), sizeof(Texel), r.texels.size(), f);
    if (!r.blk.empty()) std::fwrite(r.blk.data(), sizeof(RtLampGpu::BlockerTable::Row), r.blk.size(), f);
    for (size_t l = 0; l < r.lamps.size(); l++) {
        Lamp L = r.lamps[l];
        L.parts = static_cast<uint32_t>(l < r.parts.size() ? r.parts[l].size() : 0);
        Put(f, L);
        if (L.parts) std::fwrite(r.parts[l].data(), sizeof(Part), L.parts, f);
    }
}

// ---- reading (tools/scene_replay) ----
struct In {
    std::FILE* f;
    bool ok = true;
    template <class T>
    void Get(T& v) { ok = ok && std::fread(&v, sizeof v, 1, f) == 1; }
    template <class T>
    void GetN(std::vector<T>& v, size_t n) {
        if (!ok || n > (1u << 28)) return void(ok = false);
        v.resize(n);
        ok = n == 0 || std::fread(v.data(), sizeof(T), n, f) == n;
    }
    template <class T>
    void GetVec(std::vector<T>& v) {
        uint32_t n = 0;
        Get(n);
        GetN(v, n);
    }
};
// False (why) for another layout or a cut file; a file cut inside a block keeps the blocks before it
inline bool Read(std::FILE* f, Scene& s, std::string& why) {
    In in{f};
    FileHead h{}, want{};
    in.Get(h);
    if (!in.ok || h.magic != kMagic || h.version < 1 || h.version > kVersion) return why = "not a scene recording of this version", false;
    if (h.edgeSize != want.edgeSize || h.lawSize != want.lawSize) return why = "recorded with another layout", false;
    char kind = 0;
    while (std::fread(&kind, 1, 1, f) == 1) {
        if (kind == 'L') {
            uint32_t gen = 0, n = 0;
            RtLampProxy::Lot lot;
            in.Get(gen), in.Get(lot.worldToLot), in.Get(n);
            for (uint32_t i = 0; in.ok && i < n && i < 64; i++) {
                RtLampProxy::Story st;
                in.Get(st.story), in.Get(st.w), in.Get(st.h), in.Get(st.plane);
                in.GetVec(st.edges), in.GetVec(st.floorY), in.GetVec(st.roofed), in.GetVec(st.ceil), in.GetVec(st.placed), in.GetVec(st.removed), in.GetVec(st.room);
                if (h.version < 2)
                    for (RtLampProxy::Edge& e : st.edges) e.glazed = 0;
                lot.stories.push_back(std::move(st));
            }
            if (!in.ok) break;
            s.lots.emplace_back(gen, std::move(lot));
        } else if (kind == 'R') {
            Room r;
            in.Get(r.h), in.GetN(r.texels, r.h.texels), in.GetN(r.blk, r.h.blkRows);
            for (uint32_t l = 0; in.ok && l < r.h.lamps; l++) {
                Lamp L;
                in.Get(L);
                std::vector<Part> p;
                in.GetN(p, L.parts);
                r.lamps.push_back(L), r.parts.push_back(std::move(p));
            }
            if (!in.ok) break;
            s.rooms.push_back(std::move(r));
        } else
            break;
    }
    return true;
}

// ---- the comparison (shared by the tool and its checks) ----
// The GPU's term of one lamp at one texel (AccPhysPS): vis = the cube shadow's share, t3d = the raw 3D test (1 untested), fill =
// the lamp's furniture fill floor (RtLampGpu::FurnitureFill; 0: the game's own term, which Compare scores)
// pane: the glass share (RtLampProxy::PaneShare; 1 none), after the soft cut as AccPhys takes it
inline void GpuTerm(const Lamp& L, const RoomHead& h, const Texel& t, float vis, float t3d, bool useReach, float out[3], float fill = 0.0f, float pane = 1.0f) {
    const bool wall = std::fabs(t.n[1]) < 0.5f;
    float law[3];
    GpuLampLaw::Eval(L.law, t.p, t.n, h.reachOn && L.reach && wall && useReach ? 1.0f : 0.0f, law);
    float sh = vis;
    if (t.tested) {
        const float prod = vis * RtLampGpu::FurnitureFill(t3d, fill);
        sh = RoomLightTests::K_DROP > prod ? 0.0f : prod;
    }
    float c[3] = {law[0] * sh, law[1] * sh, law[2] * sh};
    const float cs = (c[2] + c[1]) + c[0];
    float k = 1.0f;
    if (!(cs >= h.thr)) k = 0.0f;
    else if (h.fade && cs < 2.0f * h.thr) {
        const float x = (cs - h.thr) / h.thr;
        k = x * x * (3.0f - 2.0f * x);
    }
    for (int q = 0; q < 3; q++) out[q] = L.row * c[q] * k * pane;
}
inline float MaxDiff(const float a[3], const float b[3]) {
    return std::max({std::fabs(a[0] - b[0]), std::fabs(a[1] - b[1]), std::fabs(a[2] - b[2])});
}
constexpr float kWithin = 2.0f / 255.0f;
// What a texel's miss looks like: the variant of the GPU's term that lands within 2/255 names it
enum Tag { kOk, kShadow3D, kOpening, kReach, kNorm, kPosition, kUnknown, kTags };
inline const char* TagName(int t) {
    static const char* const n[kTags] = {"ok", "shadow3D", "opening", "reach", "norm", "position", "unknown"};
    return t >= 0 && t < kTags ? n[t] : "?";
}
// game = the game's part, gpu = the GPU's term; noT3d / noVis / noReach = the term without the 3D test, without the cube
// shadow, without the reach share. norm: the game's part is the GPU's times one factor (within 2/255 after the ratio of
// their sums). position: the game lit where the GPU's law has nothing (or the reverse, both shares 1)
inline int TagOf(const float game[3], const float gpu[3], const float noT3d[3], const float noVis[3], const float noReach[3], const float lawOnly[3]) {
    if (MaxDiff(game, gpu) <= kWithin) return kOk;
    if (MaxDiff(game, noT3d) <= kWithin) return kShadow3D;
    if (MaxDiff(game, noVis) <= kWithin) return kOpening;
    if (MaxDiff(game, noReach) <= kWithin) return kReach;
    const float gs = game[0] + game[1] + game[2], ps = gpu[0] + gpu[1] + gpu[2], ls = lawOnly[0] + lawOnly[1] + lawOnly[2];
    if (gs > kWithin && ls <= 1e-6f) return kPosition;
    if (gs > kWithin && ps > kWithin) {
        const float k = gs / ps;
        const float scaled[3] = {gpu[0] * k, gpu[1] * k, gpu[2] * k};
        if (MaxDiff(game, scaled) <= kWithin && std::fabs(k - 1.0f) > 0.05f) return kNorm;
    }
    return kUnknown;
}
struct Stats {
    long n = 0, within = 0, tags[kTags] = {};
    long mapN[4] = {}, mapWithin[4] = {}; // by map: wall atlas, floor, ceiling, objects
    float worst = 0.0f;
    uint32_t worstTexel = 0;
    uint64_t worstLight = 0;
    float ratio = 0.0f; // the median game / GPU ratio of the summed channels where both are lit (Compare's call alone)
    long filled = 0;    // of n, the texels the GPU lifts by a lamp's furniture fill (its own table's shadow; not in the score)
};
// One room's lamps against the game's parts; mesh = the lot's occluders with open tops (null: the cube shadow taken as 1,
// in game), lot = its space. Texels with neither a game part nor a GPU term over 0 are not counted
inline void Compare(const Room& r, const RtLampProxy::Mesh* mesh, const RtLampProxy::Lot* lot, Stats& s, int visSamples = 16) {
    RtLampGpu::BlockerTable blk;
    blk.rows = r.blk;
    const bool haveBlk = r.h.blkHeader >= 0.0f && !r.blk.empty();
    std::vector<RtLampProxy::Pane> panes;
    if (lot) RtLampProxy::PanesOf(*lot, panes);
    std::vector<uint32_t> stamp;
    uint32_t mark = 0;
    std::vector<float> game(r.texels.size() * 3);
    std::vector<float> ratios;
    for (size_t l = 0; l < r.lamps.size(); l++) {
        const Lamp& L = r.lamps[l];
        std::fill(game.begin(), game.end(), 0.0f);
        for (const Part& p : r.parts[l])
            if (p.texel < r.texels.size()) std::memcpy(&game[p.texel * 3], p.rgb, sizeof p.rgb);
        float Ll[3] = {L.law.pos[0], L.law.pos[1], L.law.pos[2]};
        if (lot) RtLampProxy::ToLot(*lot, L.law.pos, Ll);
        const float fill = haveBlk ? blk.FillOf(static_cast<int>(r.h.blkHeader), L.law.pos) : 0.0f;
        for (size_t i = 0; i < r.texels.size(); i++) {
            const Texel& t = r.texels[i];
            float lawOnly[3];
            GpuTerm(L, r.h, t, 1.0f, 1.0f, true, lawOnly);
            const float* g = &game[i * 3];
            if (g[0] + g[1] + g[2] <= 0.0f && lawOnly[0] + lawOnly[1] + lawOnly[2] <= 0.0f) continue;
            float vis = 1.0f, pane = 1.0f;
            if (mesh && lot) {
                float Pl[3];
                RtLampProxy::ToLot(*lot, t.p, Pl);
                pane = RtLampProxy::PaneShare(panes, Ll, Pl); // (the texel's true point)
                RtLampProxy::IntoRoom(*lot, r.h.story, r.h.id, Pl); // (as the GPU's shadow lookup: gpu_room_lamps.cpp)
                vis = RtLampProxy::Visibility(*mesh, Ll, r.h.radius, Pl, visSamples, &stamp, &mark);
            }
            const float t3d = haveBlk && t.tested && vis > 0.0f
                                  ? blk.Occlude(static_cast<int>(r.h.blkHeader), {L.law.pos[0], L.law.pos[1], L.law.pos[2]}, {t.p[0], t.p[1], t.p[2]}, 0.0f, r.h.radius)
                                  : 1.0f;
            s.filled += t.tested && fill > 0.0f && t3d < 1.0f;
            float gpu[3], noT3d[3], noVis[3], noReach[3];
            GpuTerm(L, r.h, t, vis, t3d, true, gpu, 0.0f, pane);
            GpuTerm(L, r.h, t, vis, 1.0f, true, noT3d, 0.0f, pane);
            GpuTerm(L, r.h, t, 1.0f, t3d, true, noVis, 0.0f, pane);
            GpuTerm(L, r.h, t, vis, t3d, false, noReach, 0.0f, pane);
            const int tag = TagOf(g, gpu, noT3d, noVis, noReach, lawOnly);
            s.n++, s.within += tag == kOk, s.tags[tag]++, s.mapN[t.map & 3]++, s.mapWithin[t.map & 3] += tag == kOk;
            if (const float d = MaxDiff(g, gpu); d > s.worst) s.worst = d, s.worstTexel = static_cast<uint32_t>(i), s.worstLight = L.light;
            if (const float gs = g[0] + g[1] + g[2], ps = gpu[0] + gpu[1] + gpu[2]; gs > kWithin && ps > kWithin) ratios.push_back(gs / ps);
        }
    }
    if (!ratios.empty()) {
        std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end());
        s.ratio = ratios[ratios.size() / 2];
    }
}

} // namespace SceneRecord
