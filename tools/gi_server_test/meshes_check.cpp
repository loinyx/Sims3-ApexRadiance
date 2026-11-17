// The bridge server's package reader (native/remix-bridge/server/ts3_meshes.h, MC-1 of X-MESHCAST.md) against casters.py.
// No GPU, no game run. Read-only: prints to stdout; writes only the --objects file.
//   meshes_check --self-check                       RefPack and the LOD 0 reader on synthetic data
//   meshes_check <game folder> [--mods <folder>] [--casters <file> [--objects <out>]] [--survey]
// Indexes the packages, links each LITE to its MODL through the VPXYs and times both. --casters: casters.py's output
// (run.ps1 -Mesh -Game writes it): each model's LOD 0 and shadow LOD must equal the reader's, vertex for vertex. --survey:
// every model's triangle counts, as casters.py --survey prints them. --mods: a Mods folder indexed after the game.
// --objects: the chair's LITE and the caster mesh the server reads for it ("MCOB", LITE u64, vertices u32, triangles
// u32, xyz each, abc each), for the bridge's test_gi_mesh_device (MC-2): the game's data, so it goes to run.ps1's -OutDir.
#include "native/remix-bridge/server/ts3_meshes.h"
#include <chrono>
#include <cmath>
#include <cstdio>

namespace {

using namespace ts3mesh;

double Ms(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool Same(const Mesh& m, const std::vector<float>& p, const std::vector<uint32_t>& t, const char* what, uint64_t inst) {
    double worst = 0;
    for (size_t i = 0; i < p.size() && p.size() == m.pos.size(); i++) worst = std::max(worst, std::fabs(double(p[i]) - m.pos[i]));
    const bool ok = p.size() == m.pos.size() && t == m.tri && worst <= 1e-6;
    std::printf("  MODL %016llX %s: reader %zu vertices %zu triangles, casters.py %zu %zu, worst %.2g m: %s\n", static_cast<unsigned long long>(inst), what,
                m.pos.size() / 3, m.tri.size() / 3, p.size() / 3, t.size() / 3, worst, ok ? "equal" : "DIFFERENT");
    return ok;
}

// The chair's object file of --objects
bool WriteObject(const char* path, uint64_t lite, const Mesh& m) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") || !f) return std::printf("cannot write %s\n", path), false;
    const uint32_t nv = uint32_t(m.pos.size() / 3), nt = uint32_t(m.tri.size() / 3);
    bool ok = std::fwrite("MCOB", 1, 4, f) == 4 && std::fwrite(&lite, 8, 1, f) == 1 && std::fwrite(&nv, 4, 1, f) == 1 && std::fwrite(&nt, 4, 1, f) == 1;
    ok = ok && std::fwrite(m.pos.data(), 4, m.pos.size(), f) == m.pos.size() && std::fwrite(m.tri.data(), 4, m.tri.size(), f) == m.tri.size();
    return std::fclose(f) == 0 && ok;
}

bool CheckCasters(Library& lib, const char* path, const char* objects) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f) return std::printf("cannot open %s\n", path), false;
    const auto get = [f](void* p, size_t n) { return std::fread(p, 1, n, f) == n; };
    char magic[4];
    uint32_t count = 0;
    bool ok = get(magic, 4) && !std::memcmp(magic, "MCST", 4) && get(&count, 4) && count < 64 && count > 0;
    for (uint32_t c = 0; ok && c < count; c++) {
        uint64_t inst;
        char role[9] = {};
        ok = get(&inst, 8) && get(role, 8);
        Key key{};
        lib.ForEachKey(kModl, [&](const Key& k) { if (k.inst == inst) key = k; });
        if (!key.type) return std::printf("  MODL %016llX not in the index\n", static_cast<unsigned long long>(inst)), std::fclose(f), false;
        for (int lod = 0; ok && lod < 2; lod++) {
            uint32_t nv = 0, nt = 0;
            ok = get(&nv, 4) && get(&nt, 4) && nv < (1u << 20) && nt < (1u << 20);
            std::vector<float> p(size_t(nv) * 3);
            std::vector<uint32_t> t(size_t(nt) * 3);
            ok = ok && get(p.data(), p.size() * 4) && get(t.data(), t.size() * 4);
            Mesh m;
            const bool read = lib.Load(key, m, lod == 1) && m.source == (lod ? Source::Shadow : Source::Lod0);
            ok = ok && read && Same(m, p, t, lod ? "shadow LOD" : "LOD 0", inst);
        }
        if (ok) {
            uint64_t lite = 0; // the LITE that names it (the object command's key)
            size_t lites = 0;
            lib.ForEachKey(kLite, [&](const Key& k) { if (const Key* mk = lib.ModelOfLite(k.inst); mk && *mk == key) lite = lite ? lite : k.inst, lites++; });
            std::printf("  %s: %zu LITE(s) name it, first %016llX\n", role, lites, static_cast<unsigned long long>(lite));
            Mesh caster;
            if (objects && !std::strcmp(role, "chair"))
                ok = lite && lib.Load(key, caster, true) && WriteObject(objects, lite, caster);
        }
    }
    std::fclose(f);
    return ok;
}

void Survey(Library& lib) {
    std::vector<uint32_t> lod0, shadow;
    uint32_t models = 0;
    std::vector<double> ratio;
    lib.ForEachKey(kModl, [&](const Key& k) {
        models++;
        Mesh a, s;
        if (!lib.Load(k, a, false)) return;
        lod0.push_back(uint32_t(a.tri.size() / 3));
        if (lib.Load(k, s, true) && s.source == Source::Shadow) shadow.push_back(uint32_t(s.tri.size() / 3)), ratio.push_back(double(s.tri.size()) / a.tri.size());
    });
    const auto q = [](std::vector<uint32_t> v, double f) { std::sort(v.begin(), v.end()); return v.empty() ? 0u : v[std::min(v.size() - 1, size_t(f * v.size()))]; };
    std::sort(ratio.begin(), ratio.end());
    std::printf("%zu of %u models read with LOD 0: median %u, 90 %% %u, 99 %% %u triangles\n", lod0.size(), models, q(lod0, 0.5), q(lod0, 0.9), q(lod0, 0.99));
    std::printf("shadow LOD on %zu: median %u, 90 %% %u, 99 %% %u triangles; shadow / LOD 0 median %.2f\n", shadow.size(), q(shadow, 0.5), q(shadow, 0.9),
                q(shadow, 0.99), ratio.empty() ? 0.0 : ratio[ratio.size() / 2]);
}

int SelfCheck() {
    // RefPack: 4 literals, a copy of 4 from 4 back, the end
    const uint8_t packed[] = {0x10, 0xFB, 0, 0, 8, 0xE0, 'a', 'b', 'c', 'd', 0x04, 0x03, 0xFC};
    const std::vector<uint8_t> un = Refpack({packed, sizeof packed}, 8);
    bool ok = std::string(un.begin(), un.end()) == "abcdabcd";
    bool threw = false;
    try {
        Refpack({packed, 8}, 8); // cut inside the literals
    } catch (const std::out_of_range&) {
        threw = true;
    }
    ok = ok && threw;
    // LOD 0 (casters.py's self-check): VRTF int16 x4 at 0, stride 8; VBUF of 3 vertices; differenced IBUF; MLOD with one mesh
    std::vector<uint8_t> vrtf = {'V', 'R', 'T', 'F', 2, 0, 0, 0, 8, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0};
    std::vector<uint8_t> vbuf = {'V', 'B', 'U', 'F', 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int v : {0, 0, 0, 16383, 16383, 0, 0, 16383, 0, 32767, 0, 32767}) vbuf.push_back(uint8_t(v)), vbuf.push_back(uint8_t(v >> 8));
    std::vector<uint8_t> ibuf = {'I', 'B', 'U', 'F', 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0};
    std::vector<uint8_t> mlod = {'M', 'L', 'O', 'D', 3, 2, 0, 0, 1, 0, 0, 0};
    const auto u32 = [](std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> 8 * i)); };
    std::vector<uint8_t> body;
    for (uint32_t v : {0u, 0u, 0x10000002u, 0x10000003u, 0x10000004u, 0x4003u, 0u, 0u, 0u, 0u, 3u, 1u}) u32(body, v);
    body.resize(body.size() + 24 + 8 + 4); // bounds; two words with a count of 0; no material
    u32(mlod, uint32_t(body.size()));
    mlod.insert(mlod.end(), body.begin(), body.end());
    Rcol r;
    for (auto* c : {&mlod, &vrtf, &vbuf, &ibuf}) r.chunks.push_back({c->data(), c->size()});
    Mesh m;
    for (const MlodMesh& mm : MlodMeshes(r.chunks[0])) AppendLod0(r, mm, m);
    ok = ok && m.pos == std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0} && m.tri == std::vector<uint32_t>{0, 1, 2};
    mlod[12 + 4 + 20 + 1] = 0x48; // the drop shadow flag: left out
    Mesh d;
    for (const MlodMesh& mm : MlodMeshes(r.chunks[0])) AppendLod0(r, mm, d);
    ok = ok && d.tri.empty();
    // a truncated MLOD is an exception, never a read past the end
    threw = false;
    try {
        MlodMeshes({mlod.data(), 30});
    } catch (const std::out_of_range&) {
        threw = true;
    }
    ok = ok && threw;
    std::printf("self-check %s\n", ok ? "passed" : "FAILED");
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--self-check")) return SelfCheck();
    if (argc < 2) return std::printf("usage: meshes_check <game folder> [--mods <folder>] [--casters <file>] [--survey] | --self-check\n"), 2;
    const char *mods = nullptr, *casters = nullptr, *objects = nullptr;
    bool survey = false;
    for (int i = 2; i < argc; i++) {
        if (!std::strcmp(argv[i], "--mods") && i + 1 < argc) mods = argv[++i];
        else if (!std::strcmp(argv[i], "--casters") && i + 1 < argc) casters = argv[++i];
        else if (!std::strcmp(argv[i], "--objects") && i + 1 < argc) objects = argv[++i];
        else if (!std::strcmp(argv[i], "--survey")) survey = true;
        else return std::printf("unknown argument %s\n", argv[i]), 2;
    }
    if (SelfCheck()) return 1;
    Library lib;
    auto t0 = std::chrono::steady_clock::now();
    lib.AddFolder(argv[1], true);
    const uint32_t gamePackages = lib.GetStats().packages;
    if (mods) lib.AddFolder(mods, false);
    const double indexMs = Ms(t0);
    t0 = std::chrono::steady_clock::now();
    const Library::Stats& s = lib.BuildLinks();
    std::printf("%u packages (%u game, %u unreadable) indexed in %.0f ms; %u VPXYs, %u link a LITE and a MODL (%zu LITEs, %u unreadable) in %.0f ms\n",
                s.packages, gamePackages, s.unreadablePackages, indexMs, s.vpxys, s.links, lib.LinkCount(), s.unreadableVpxys, Ms(t0));
    bool ok = s.links > 0;
    if (casters) ok = CheckCasters(lib, casters, objects) && ok;
    if (survey) {
        t0 = std::chrono::steady_clock::now();
        Survey(lib);
        std::printf("survey %.0f ms\n", Ms(t0));
    }
    std::printf("%s\n", ok ? "PASSED" : "FAILED");
    return ok ? 0 : 1;
}
