// Lighting scene replay (docs/validation/dev-tools-scene-replay.md): every room solve of every recorded scene (features/scene_record.h,
// written by a Recording capture) through the current code of the physical indoor lamps' GPU path (law, cube shadow share by
// its CPU reference, the game's 3D test, the soft light cut), against the game's own part of each lamp. Prints a short report;
// fails (exit 1) when a scene's share within 2/255 falls more than --drop points under its baseline.
// Usage: replay.exe [--baseline file] [--update-baseline] [--drop 0.5] [--samples 16] [--selftest] [scene files or folders...]
// Read-only except the baseline file it is told to update.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <tuple>
#include <sstream>
#include <string>
#include <vector>
#include "features/scene_record.h"

namespace fs = std::filesystem;
using namespace SceneRecord;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    if (!ok) g_fail++, std::printf("FAIL %s\n", what);
}

// A synthetic room: one lamp over a floor, a table top between them at some texels; the game's parts made by the same term
static Room SyntheticRoom() {
    Room r;
    r.h.story = 1, r.h.id = 2, r.h.cls = 2, r.h.thr = 0.004f, r.h.fade = 1, r.h.radius = 0.0f;
    for (int i = 0; i < 64; i++) {
        Texel t{};
        t.p[0] = static_cast<float>(i % 8) * 0.5f, t.p[1] = 0.0f, t.p[2] = static_cast<float>(i / 8) * 0.5f;
        t.n[1] = 1.0f, t.x = static_cast<uint16_t>(i % 8), t.y = static_cast<uint16_t>(i / 8), t.map = 1, t.tested = 0;
        r.texels.push_back(t);
    }
    Lamp L;
    L.light = 0x1234, L.law.pos[0] = 2.0f, L.law.pos[1] = 2.5f, L.law.pos[2] = 2.0f, L.law.W = 3.0f;
    L.law.colour[0] = 1.0f, L.law.colour[1] = 0.8f, L.law.colour[2] = 0.6f, L.row = 1.0f, L.type = 3;
    r.lamps.push_back(L);
    std::vector<Part> parts;
    for (uint32_t i = 0; i < r.texels.size(); i++) {
        float c[3];
        GpuTerm(L, r.h, r.texels[i], 1.0f, 1.0f, true, c);
        if (c[0] + c[1] + c[2] > 0.0f) parts.push_back({i, {c[0], c[1], c[2]}});
    }
    r.parts.push_back(parts);
    return r;
}
static void SelfTest() {
    Room r = SyntheticRoom();
    Stats s;
    Compare(r, nullptr, nullptr, s);
    Check(s.n > 0 && s.within == s.n, "scene replay: a game part equal to the GPU's term is within 2/255");
    Room dim = r;
    for (Part& p : dim.parts[0])
        for (float& v : p.rgb) v *= 0.5f;
    Stats sd;
    Compare(dim, nullptr, nullptr, sd);
    Check(sd.tags[kNorm] > 0 && sd.within < sd.n, "scene replay: a game part at half the GPU's term is tagged norm");
    // tags
    const float g[3] = {0.5f, 0.5f, 0.5f}, z[3] = {}, half[3] = {0.25f, 0.25f, 0.25f};
    Check(TagOf(g, g, z, z, z, g) == kOk, "tag: the same is ok");
    Check(TagOf(g, z, g, z, z, g) == kShadow3D, "tag: right without the 3D test is shadow3D");
    Check(TagOf(g, z, z, g, z, g) == kOpening, "tag: right without the cube shadow is opening");
    Check(TagOf(g, z, z, z, g, g) == kReach, "tag: right without the reach share is reach");
    Check(TagOf(g, half, half, half, half, half) == kNorm, "tag: one factor off is norm");
    Check(TagOf(g, z, z, z, z, z) == kPosition, "tag: lit where the law has nothing is position");
    // the file round trip, with a lot
    const fs::path tmp = fs::temp_directory_path() / "apex_scene_selftest.apexscene";
    {
        std::FILE* f = nullptr;
        if (_wfopen_s(&f, tmp.c_str(), L"wb") || !f) return Check(false, "scene file: could not write the temporary file");
        RtLampProxy::Lot lot;
        RtLampProxy::Story st;
        st.story = 1, st.w = 2, st.h = 1, st.floorY = {3.0f, 3.0f}, st.roofed = {15, 15}, st.ceil = {6.0f, 6.0f};
        st.edges.push_back(RtLampProxy::Edge{});
        lot.stories.push_back(st);
        WriteHead(f), WriteLot(f, 7, lot), WriteRoom(f, r), WriteRoom(f, dim);
        std::fclose(f);
    }
    Scene sc;
    std::string why;
    std::FILE* f = nullptr;
    const bool opened = !_wfopen_s(&f, tmp.c_str(), L"rb") && f;
    const bool read = opened && Read(f, sc, why);
    if (f) std::fclose(f);
    fs::remove(tmp);
    Check(read && sc.lots.size() == 1 && sc.lots[0].first == 7 && sc.lots[0].second.stories.size() == 1 && sc.lots[0].second.stories[0].edges.size() == 1 &&
              sc.rooms.size() == 2 && sc.rooms[0].texels.size() == r.texels.size() && sc.rooms[0].parts[0].size() == r.parts[0].size() &&
              sc.rooms[1].parts[0][0].rgb[0] == dim.parts[0][0].rgb[0],
          "scene file: a lot and two rooms read back as written");
}

// --detail story:id: each lamp of that room's solves alone (its tags, the median game / GPU ratio where both are lit)
static void Detail(const Room& r, const RtLampProxy::Mesh* mesh, const RtLampProxy::Lot* lot, int samples) {
    std::printf("story %d room %d class %d, tick %u: %u texels, %zu lamps, thr %.4f, norm %.3f, reach %d, outdoor %d\n", r.h.story, r.h.id, r.h.cls, r.h.tick,
                r.h.texels, r.lamps.size(), r.h.thr, r.h.norm, r.h.reachOn, r.h.outdoor);
    for (size_t l = 0; l < r.lamps.size(); l++) {
        Room one = r;
        one.lamps = {r.lamps[l]}, one.parts = {r.parts[l]};
        Stats s;
        Compare(one, mesh, lot, s, samples);
        if (!s.n) continue;
        std::string tags;
        for (int t = 1; t < kTags; t++)
            if (s.tags[t]) tags += std::format(" {} {}", TagName(t), s.tags[t]);
        const Lamp& L = r.lamps[l];
        static std::set<std::tuple<uint64_t, float, float, float>> s_seen; // (one report per lamp and place)
        if (!s_seen.insert({L.light, L.law.pos[0], L.law.pos[1], L.law.pos[2]}).second) continue;
        std::printf("  lamp %llX type %d row %.3f reach %d pos (%.2f %.2f %.2f) W %.3f cmax %.3f: %ld texels, %.1f%% within, ratio game/GPU %.3f;%s\n",
                    static_cast<unsigned long long>(L.light), L.type, L.row, L.reach, L.law.pos[0], L.law.pos[1], L.law.pos[2], L.law.W, L.law.cmax, s.n,
                    100.0 * s.within / s.n, s.ratio, tags.c_str());
        // its worst texels: where, the game's part, the GPU's term and its shares
        struct Miss {
            float d;
            uint32_t i;
            float g[3], gpu[3], vis, t3d;
        };
        std::vector<Miss> miss;
        std::vector<float> game(r.texels.size() * 3, 0.0f);
        for (const Part& p : r.parts[l])
            if (p.texel < r.texels.size()) std::memcpy(&game[p.texel * 3], p.rgb, sizeof p.rgb);
        RtLampGpu::BlockerTable blk;
        blk.rows = r.blk;
        float Ll[3] = {L.law.pos[0], L.law.pos[1], L.law.pos[2]};
        if (lot) RtLampProxy::ToLot(*lot, L.law.pos, Ll);
        std::vector<RtLampProxy::Pane> panes;
        if (lot) RtLampProxy::PanesOf(*lot, panes);
        for (uint32_t i = 0; i < r.texels.size(); i++) {
            const Texel& t = r.texels[i];
            float vis = 1.0f, pane = 1.0f;
            if (mesh && lot) {
                float Pl[3];
                RtLampProxy::ToLot(*lot, t.p, Pl);
                pane = RtLampProxy::PaneShare(panes, Ll, Pl);
                RtLampProxy::IntoRoom(*lot, r.h.story, r.h.id, Pl);
                float Nl[3];
                RtLampProxy::DirToLot(*lot, t.n, Nl);
                RtLampProxy::OffFloorLine(*lot, r.h.story, Pl, Nl);
                vis = RtLampProxy::Visibility(*mesh, Ll, r.h.radius, Pl, samples);
            }
            const float t3d = r.h.blkHeader >= 0.0f && !r.blk.empty() && t.tested ? blk.Occlude(static_cast<int>(r.h.blkHeader), {L.law.pos[0], L.law.pos[1], L.law.pos[2]}, {t.p[0], t.p[1], t.p[2]}, 0.0f, r.h.radius) : 1.0f;
            Miss m{0.0f, i, {}, {}, vis, t3d};
            std::memcpy(m.g, &game[i * 3], sizeof m.g);
            GpuTerm(L, r.h, t, vis, t3d, true, m.gpu, 0.0f, pane);
            m.d = MaxDiff(m.g, m.gpu);
            if (m.d > kWithin) miss.push_back(m);
        }
        std::sort(miss.begin(), miss.end(), [](const Miss& a, const Miss& b) { return a.d > b.d; });
        for (size_t k = 0; k < miss.size() && k < 6; k++) {
            const Miss& m = miss[k];
            const Texel& t = r.texels[m.i];
            const float dx = t.p[0] - L.law.pos[0], dy = t.p[1] - L.law.pos[1], dz = t.p[2] - L.law.pos[2];
            std::printf("    map %d (%u,%u) p (%.2f %.2f %.2f) n (%.2f %.2f %.2f) d %.2f tested %d: game (%.3f %.3f %.3f) GPU (%.3f %.3f %.3f) vis %.2f t3d %.2f\n",
                        t.map, t.x, t.y, t.p[0], t.p[1], t.p[2], t.n[0], t.n[1], t.n[2], std::sqrt(dx * dx + dy * dy + dz * dz), t.tested, m.g[0], m.g[1], m.g[2],
                        m.gpu[0], m.gpu[1], m.gpu[2], m.vis, m.t3d);
            if (mesh && lot && m.vis < 0.5f) { // the occluder the lamp's centre meets first (lot space)
                float Pl[3], tHit = 0.0f;
                RtLampProxy::ToLot(*lot, t.p, Pl);
                RtLampProxy::IntoRoom(*lot, r.h.story, r.h.id, Pl);
                float Nl[3];
                RtLampProxy::DirToLot(*lot, t.n, Nl);
                RtLampProxy::OffFloorLine(*lot, r.h.story, Pl, Nl);
                const int hit = RtLampProxy::FirstHit(*mesh, Ll, Pl, 0.01f, 0.01f, tHit);
                if (hit >= 0) {
                    const RtLampProxy::Tri& tr = mesh->tris[static_cast<size_t>(hit)];
                    std::printf("      first hit at t %.3f: kind %d story %d, tri (%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f); lamp lot (%.2f %.2f %.2f) texel lot (%.2f %.2f %.2f)\n",
                                tHit, mesh->kind[static_cast<size_t>(hit)], mesh->story[static_cast<size_t>(hit)], tr.v[0][0], tr.v[0][1], tr.v[0][2], tr.v[1][0],
                                tr.v[1][1], tr.v[1][2], tr.v[2][0], tr.v[2][1], tr.v[2][2], Ll[0], Ll[1], Ll[2], Pl[0], Pl[1], Pl[2]);
                }
            }
        }
    }
}

struct RoomLine {
    std::string name;
    double within;
    long n;
    int tag;
};
int main(int argc, char** argv) {
    std::vector<fs::path> inputs;
    fs::path baseline;
    bool update = false, self = false;
    double drop = 0.5;
    int samples = 16;
    int detailStory = -99, detailId = -1;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--baseline" && i + 1 < argc) baseline = argv[++i];
        else if (a == "--update-baseline") update = true;
        else if (a == "--drop" && i + 1 < argc) drop = std::atof(argv[++i]);
        else if (a == "--samples" && i + 1 < argc) samples = std::max(1, std::atoi(argv[++i]));
        else if (a == "--selftest") self = true;
        else if (a == "--detail" && i + 1 < argc) sscanf_s(argv[++i], "%d:%d", &detailStory, &detailId);
        else inputs.emplace_back(a);
    }
    SelfTest();
    if (self || g_fail) {
        std::printf("scene replay self-test: %s\n", g_fail ? "FAILED" : "ok");
        return g_fail ? 1 : 0;
    }
    std::vector<fs::path> files;
    for (const fs::path& p : inputs) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            for (const auto& e : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec))
                if (e.is_regular_file() && e.path().extension() == ".apexscene") files.push_back(e.path());
        } else if (fs::exists(p, ec))
            files.push_back(p);
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::printf("scene replay: no .apexscene file found (record one: a Recording capture with the physical indoor lamp model on)\n");
        return 0;
    }
    std::map<std::string, double> base;
    if (!baseline.empty()) {
        std::ifstream in(baseline);
        std::string name;
        double v;
        while (in >> std::quoted(name) >> v) base[name] = v;
    }
    std::map<std::string, double> now;
    Stats all;
    std::vector<RoomLine> rooms;
    int regressions = 0;
    for (const fs::path& file : files) {
        // (every capture is 'Lighting scene.apexscene': a scene is named by its capture folder and the folder above it, a session's
        // recordings nested in it)
        const std::string key = (file.parent_path().parent_path().filename() / file.parent_path().filename()).generic_string();
        std::FILE* f = nullptr;
        if (_wfopen_s(&f, file.c_str(), L"rb") || !f) {
            std::printf("%s: could not be opened\n", key.c_str());
            continue;
        }
        Scene sc;
        std::string why;
        const bool ok = Read(f, sc, why);
        std::fclose(f);
        if (!ok) {
            std::printf("%s: %s\n", key.c_str(), why.c_str());
            continue;
        }
        Stats fileStats;
        std::map<uint32_t, RtLampProxy::Mesh> meshes;
        for (const Room& r : sc.rooms) {
            const RtLampProxy::Lot* lot = nullptr;
            for (const auto& [gen, l] : sc.lots)
                if (gen == r.h.lotGen) lot = &l;
            const RtLampProxy::Mesh* mesh = nullptr;
            if (lot) {
                auto [it, fresh] = meshes.try_emplace(r.h.lotGen);
                if (fresh) RtLampProxy::Build(*lot, it->second, true);
                mesh = &it->second;
            }
            if (r.h.story == detailStory && r.h.id == detailId) Detail(r, mesh, lot, samples);
            Stats s;
            Compare(r, mesh, lot, s, samples);
            fileStats.n += s.n, fileStats.within += s.within, fileStats.filled += s.filled;
            for (int t = 0; t < kTags; t++) fileStats.tags[t] += s.tags[t];
            for (int m = 0; m < 4; m++) fileStats.mapN[m] += s.mapN[m], fileStats.mapWithin[m] += s.mapWithin[m];
            int top = kOk;
            for (int t = 1; t < kTags; t++)
                if (s.tags[t] > s.tags[top] || top == kOk) top = s.tags[t] ? t : top;
            if (s.n)
                rooms.push_back({std::format("{} story {} room {} class {}", key, r.h.story, r.h.id, r.h.cls), 100.0 * s.within / s.n, s.n, top});
        }
        const double pct = fileStats.n ? 100.0 * fileStats.within / fileStats.n : 100.0;
        now[key] = pct;
        const auto b = base.find(key);
        const bool worse = b != base.end() && pct < b->second - drop;
        regressions += worse;
        std::printf("%s: %zu room solves, %ld lamp texels, %.2f%% within 2/255%s\n", key.c_str(), sc.rooms.size(), fileStats.n, pct,
                    b == base.end() ? "" : std::format(" (baseline {:.2f}%{})", b->second, worse ? ", REGRESSION" : "").c_str());
        all.n += fileStats.n, all.within += fileStats.within, all.filled += fileStats.filled;
        for (int t = 0; t < kTags; t++) all.tags[t] += fileStats.tags[t];
        for (int m = 0; m < 4; m++) all.mapN[m] += fileStats.mapN[m], all.mapWithin[m] += fileStats.mapWithin[m];
    }
    const auto pct = [](long k, long n) { return n ? 100.0 * k / n : 100.0; };
    static const char* const kMap[4] = {"walls", "floors", "ceilings", "objects"};
    std::string maps, tags;
    for (int m = 0; m < 4; m++)
        if (all.mapN[m]) maps += std::format("{}{} {:.1f}%", maps.empty() ? "" : ", ", kMap[m], pct(all.mapWithin[m], all.mapN[m]));
    for (int t = 1; t < kTags; t++)
        if (all.tags[t]) tags += std::format("{}{} {:.1f}%", tags.empty() ? "" : ", ", TagName(t), pct(all.tags[t], all.n));
    std::printf("all: %.2f%% within 2/255 of %ld lamp texels; by surface: %s\n", pct(all.within, all.n), all.n, maps.c_str());
    std::printf("misses by likely cause: %s\n", tags.empty() ? "none" : tags.c_str());
    std::printf("furniture fill (a lamp's own table, not in the score): lifts %.2f%% of the lamp texels\n", pct(all.filled, all.n));
    std::sort(rooms.begin(), rooms.end(), [](const RoomLine& a, const RoomLine& b) { return a.within < b.within; });
    std::set<std::string> shown; // (each room once, at its worst solve)
    for (size_t i = 0; i < rooms.size() && shown.size() < 8 && rooms[i].within < 100.0; i++)
        if (shown.insert(rooms[i].name).second)
            std::printf("  worst: %s: %.1f%% of %ld (mostly %s)\n", rooms[i].name.c_str(), rooms[i].within, rooms[i].n, TagName(rooms[i].tag));
    if (update && !baseline.empty()) {
        std::ofstream out(baseline);
        for (const auto& [name, v] : now) out << std::quoted(name) << ' ' << v << '\n';
        std::printf("baseline written: %s\n", baseline.string().c_str());
    }
    if (regressions) std::printf("%d scene(s) fell more than %.2f points under the baseline\n", regressions, drop);
    return regressions ? 1 : 0;
}
