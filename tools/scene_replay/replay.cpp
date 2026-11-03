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
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--baseline" && i + 1 < argc) baseline = argv[++i];
        else if (a == "--update-baseline") update = true;
        else if (a == "--drop" && i + 1 < argc) drop = std::atof(argv[++i]);
        else if (a == "--samples" && i + 1 < argc) samples = std::max(1, std::atoi(argv[++i]));
        else if (a == "--selftest") self = true;
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
        std::FILE* f = nullptr;
        if (_wfopen_s(&f, file.c_str(), L"rb") || !f) {
            std::printf("%s: could not be opened\n", file.filename().string().c_str());
            continue;
        }
        Scene sc;
        std::string why;
        const bool ok = Read(f, sc, why);
        std::fclose(f);
        if (!ok) {
            std::printf("%s: %s\n", file.filename().string().c_str(), why.c_str());
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
            Stats s;
            Compare(r, mesh, lot, s, samples);
            fileStats.n += s.n, fileStats.within += s.within;
            for (int t = 0; t < kTags; t++) fileStats.tags[t] += s.tags[t];
            for (int m = 0; m < 4; m++) fileStats.mapN[m] += s.mapN[m], fileStats.mapWithin[m] += s.mapWithin[m];
            int top = kOk;
            for (int t = 1; t < kTags; t++)
                if (s.tags[t] > s.tags[top] || top == kOk) top = s.tags[t] ? t : top;
            if (s.n)
                rooms.push_back({std::format("{} story {} room {} class {}", file.stem().string(), r.h.story, r.h.id, r.h.cls), 100.0 * s.within / s.n, s.n, top});
        }
        const double pct = fileStats.n ? 100.0 * fileStats.within / fileStats.n : 100.0;
        const std::string key = file.filename().string();
        now[key] = pct;
        const auto b = base.find(key);
        const bool worse = b != base.end() && pct < b->second - drop;
        regressions += worse;
        std::printf("%s: %zu room solves, %ld lamp texels, %.2f%% within 2/255%s\n", key.c_str(), sc.rooms.size(), fileStats.n, pct,
                    b == base.end() ? "" : std::format(" (baseline {:.2f}%{})", b->second, worse ? ", REGRESSION" : "").c_str());
        all.n += fileStats.n, all.within += fileStats.within;
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
    std::sort(rooms.begin(), rooms.end(), [](const RoomLine& a, const RoomLine& b) { return a.within < b.within; });
    for (size_t i = 0; i < rooms.size() && i < 5 && rooms[i].within < 100.0; i++)
        std::printf("  worst: %s: %.1f%% of %ld (mostly %s)\n", rooms[i].name.c_str(), rooms[i].within, rooms[i].n, TagName(rooms[i].tag));
    if (update && !baseline.empty()) {
        std::ofstream out(baseline);
        for (const auto& [name, v] : now) out << std::quoted(name) << ' ' << v << '\n';
        std::printf("baseline written: %s\n", baseline.string().c_str());
    }
    if (regressions) std::printf("%d scene(s) fell more than %.2f points under the baseline\n", regressions, drop);
    return regressions ? 1 : 0;
}
