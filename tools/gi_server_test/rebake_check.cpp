// Re-bake cost of the per-lamp indirect layers against lamp count (docs/features/realtime-gi/wp/X-REBAKE.md; design.md
// 3.3, plan S2-05). No bridge, no game. Synthetic two-story lots of 5 m x 5 m rooms, two lamps a room (a ceiling lamp,
// a table lamp), a door to the next room along x, every other room a door along z, a window in every outer wall, a table
// box a room. Lots of 60, 120 and 200 lamps (15, 30 and 50 rooms a story).
//  CPU: the S0-01 reference (tools/rt_lamps_test/gi_reference.h) at 1 texel a tile: links once, then one layer a lamp,
//       serial (time a layer) and over all cores (time the lot); a compose of every layer (a toggle); how many lamps
//       touch a texel; the lamps an edit dirties; on the 60-lamp lot the compose against a full solve.
//  Server: check.cpp's Solve (solve.comp) on both back ends: acceleration structures, links, then each lamp's direct and
//       bounces until settled (wall time of submit + wait a dispatch, an upper bound), then direct + 16 bounces a lamp
//       batched 8 lamps a submission (close to GPU time); the per-dispatch submit floor.
// Checked: no light crosses a story (no stair hole; the house is convex, so a ray out of a window never comes back);
// every lamp lights its own room; layers are finite; the parallel layers equal the serial ones; compose = full solve.
// Read-only: prints to stdout, writes no file.
//   --lots <a,b,..>  lamp counts of the lots (60,120,200)   --density <n>  server texels a metre (1)
//   --rays <n>       server rays a texel (256)              --no-cpu / --no-gpu   skip a part   --device <text>
#define main SolveMain // check.cpp's scene, device and solve code as they are
#include "check.cpp"
#undef main
#include <execution>
#include <numeric>
#include <sstream>
#include <thread>
#include "features/lamp_reach.h"
#include "tools/rt_lamps_test/gi_reference.h"

namespace {
using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

constexpr int kRoomSize = 5, kMargin = 4;
struct BigLamp {
    float pos[3], range, radius, col[3];
    int room, story;
};
struct BigLot {
    int lamps = 0, nx = 0, nz = 0, G = 0; // rooms a story along x, z; tiles a side
    RtLampProxy::Lot lot;                 // stories with walls, openings, floors, ceilings; placed floors upstairs
    std::vector<RtLampProxy::ObjShape> boxes;
    std::vector<BigLamp> lamp;
};
int BigRoom(const BigLot& b, int s, int x, int z) {
    x -= kMargin, z -= kMargin;
    if (x < 0 || z < 0 || x >= b.nx * kRoomSize || z >= b.nz * kRoomSize) return 0;
    return 1 + (s * b.nx + x / kRoomSize) * b.nz + z / kRoomSize;
}
BigLot MakeLot(int lamps) {
    using namespace RtLampProxy;
    BigLot b;
    b.lamps = lamps;
    const int rooms = lamps / 4; // a story: two lamps a room, two stories
    b.nz = rooms <= 15 ? 3 : 5, b.nx = (rooms + b.nz - 1) / b.nz;
    b.G = std::max(b.nx, b.nz) * kRoomSize + 2 * kMargin;
    for (int s = 0; s < 2; s++) {
        Story st;
        st.story = s, st.w = st.h = b.G;
        const size_t n = static_cast<size_t>(b.G) * b.G;
        st.floorY.assign(n, kNoFloor), st.roofed.assign(n, 0), st.ceil.assign(n, kNoFloor), st.placed.assign(n, 0);
        const float foot = 3.0f * s;
        st.plane = foot;
        for (int z = 0; z < b.G; z++)
            for (int x = 0; x < b.G; x++) {
                const int r = BigRoom(b, s, x, z);
                const size_t t = static_cast<size_t>(z) * b.G + x;
                if (r) st.floorY[t] = foot, st.roofed[t] = 0xF, st.ceil[t] = foot + 3.0f, st.placed[t] = s ? 0xF : 0;
                for (int a = 0; a < 2; a++) { // a wall on every tile line between two rooms or a room and outside, toward -x, -z
                    const int o = BigRoom(b, s, x - (a == 0), z - (a == 1));
                    if (o == r) continue;
                    Edge e;
                    e.x0 = static_cast<float>(x), e.z0 = static_cast<float>(z), e.x1 = static_cast<float>(x + (a == 1)), e.z1 = static_cast<float>(z + (a == 0));
                    e.foot = e.base = foot, e.topL = e.topM = e.topR = foot + 3.0f;
                    const int along = ((a == 0 ? z : x) - kMargin) % kRoomSize, roomCol = (x - kMargin) / kRoomSize;
                    const bool outer = !r || !o, door = !outer && along == 2 && (a == 0 || roomCol % 2 == 0);
                    if (door || (outer && along == 2)) {
                        e.open = true, e.o.u0 = 0.1f, e.o.u1 = 0.9f, e.o.sill = door ? 0.0f : 0.85f;
                        for (float& h : e.o.head) h = door ? 2.2f : 2.25f;
                    }
                    st.edges.push_back(e);
                }
            }
        b.lot.stories.push_back(std::move(st));
    }
    for (int r = 0; r < rooms * 2 && static_cast<int>(b.lamp.size()) < lamps; r++) {
        const int s = r / (b.nx * b.nz), ix = (r % (b.nx * b.nz)) / b.nz, iz = r % b.nz;
        const float x0 = static_cast<float>(kMargin + ix * kRoomSize), z0 = static_cast<float>(kMargin + iz * kRoomSize), foot = 3.0f * s;
        b.boxes.push_back({{x0 + 1.5f, foot + 0.4f, z0 + 3.5f}, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {0.8f, 0.4f, 0.5f}}); // a table
        b.lamp.push_back({{x0 + 2.5f, foot + 2.75f, z0 + 2.5f}, 60.0f, 0.15f, {3.0f, 3.0f, 3.0f}, r + 1, s});          // ceiling lamp
        b.lamp.push_back({{x0 + 4.0f, foot + 1.2f, z0 + 1.0f}, 40.0f, 0.10f, {1.5f, 1.2f, 0.8f}, r + 1, s});           // table lamp
    }
    return b;
}
int HouseTiles(const BigLot& b) { return b.nx * b.nz * kRoomSize * kRoomSize; }

// The edits: an interior wall in the house's middle (story 0), a corner room's outer wall (story 0), a middle wall upstairs
std::vector<std::array<float, 4>> EditSites(const BigLot& b) { // x, y, z, story
    const float mx = static_cast<float>(kMargin + (b.nx / 2) * kRoomSize), mz = kMargin + 2.5f;
    return {{mx, 1.5f, mz, 0}, {static_cast<float>(kMargin), 1.5f, kMargin + 2.5f, 0}, {mx, 4.5f, mz + kRoomSize, 1}};
}
float Dist(const float a[3], const float b[3]) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }
double Pct(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5))];
}

// ---------------------------------------------------------------- CPU reference
void CpuPart(const BigLot& b) {
    std::printf("\n[CPU reference] %d lamps, %d x %d rooms a story, lot %d x %d tiles, 2 stories\n", b.lamps, b.nx, b.nz, b.G, b.G);
    RtLampProxy::Mesh mesh;
    RtLampProxy::Build(b.lot, mesh, true);
    for (const auto& box : b.boxes) RtLampProxy::AddShape(mesh, box);
    RtLampProxy::BuildGrid(mesh);
    GiRef::Model m;
    auto t0 = Clock::now();
    GiRef::Build(b.lot, mesh, m);
    const double linkMs = Ms(t0);
    std::printf("  proxy %zu triangles; %zu patches, %zu texels, %zu links: links built in %.0f ms (one thread)\n", mesh.tris.size(), m.patches.size(), m.texels.size(),
                m.links.size(), linkMs);
    std::vector<RtLampModel::Lamp> lamps;
    std::vector<GiRef::Rgb> colour;
    for (const BigLamp& d : b.lamp) {
        RtLampModel::Lamp L;
        for (int k = 0; k < 3; k++) L.law.pos[k] = d.pos[k], L.law.colour[k] = 1.0f;
        L.law.W = 0.075f * d.range, L.law.cmax = 2.0f;
        GpuLampLaw::NoCone(L.law);
        float r0, R;
        LampReach::Radii(d.range, r0, R);
        L.law.r0sq = r0 * r0, L.law.invR2 = 1.0f / (R * R);
        lamps.push_back(L), colour.push_back({d.col[0] / 3.0f, d.col[1] / 3.0f, d.col[2] / 3.0f});
    }
    const size_t L = lamps.size(), T = m.texels.size();
    std::vector<std::vector<GiRef::Rgb>> layers(L), par(L);
    std::vector<double> ms(L);
    for (size_t l = 0; l < L; l++) {
        t0 = Clock::now();
        GiRef::Layer(m, mesh, lamps[l], layers[l]);
        ms[l] = Ms(t0);
    }
    std::vector<size_t> idx(L);
    std::iota(idx.begin(), idx.end(), size_t{0});
    t0 = Clock::now();
    std::for_each(std::execution::par, idx.begin(), idx.end(), [&](size_t l) { GiRef::Layer(m, mesh, lamps[l], par[l]); });
    const double parMs = Ms(t0);
    const double sum = std::accumulate(ms.begin(), ms.end(), 0.0);
    std::printf("  layer a lamp: median %.1f ms, p90 %.1f, max %.1f; all %zu layers %.0f ms on one thread, %.0f ms on %u threads\n", Pct(ms, 0.5), Pct(ms, 0.9),
                Pct(ms, 1.0), L, sum, parMs, std::thread::hardware_concurrency());
    Check(par == layers, "layers on all threads equal the serial ones");
    // compose = a toggle, dim or recolour
    std::vector<bool> on(L, true);
    std::vector<GiRef::Rgb> comp;
    t0 = Clock::now();
    GiRef::Compose(layers, on, colour, comp);
    const double composeMs = Ms(t0);
    on[0] = false;
    t0 = Clock::now();
    GiRef::Compose(layers, on, colour, comp);
    std::printf("  compose of %zu layers over %zu texels (dense): %.2f ms; after a toggle %.2f ms, 0 solves\n", L, T, composeMs, Ms(t0));
    // checks: finite, own room lit, no light across a story; sparsity
    bool finite = true, ownLit = true;
    long crossStory = 0;
    const float eps = 0.5f / 255.0f; // a layer value under this adds under half a step at colour 1
    std::vector<int> perTexel(T, 0);
    std::vector<double> touched;
    for (size_t l = 0; l < L; l++) {
        const BigLamp& d = b.lamp[l];
        double own = 0.0;
        long n = 0;
        for (size_t t = 0; t < T; t++) {
            const GiRef::Patch& P = m.patches[m.texels[t]];
            const float v = std::max({layers[l][t][0], layers[l][t][1], layers[l][t][2]});
            finite = finite && std::isfinite(v);
            if (P.story != d.story && v != 0.0f) crossStory++;
            if (P.story == d.story && BigRoom(b, P.story, static_cast<int>(P.p[0]), static_cast<int>(P.p[2])) == d.room) own = std::max(own, static_cast<double>(v));
            if (v > eps) perTexel[t]++, n++;
        }
        ownLit = ownLit && own > eps;
        touched.push_back(static_cast<double>(n));
    }
    Check(finite, "every layer is finite");
    Check(ownLit, "every lamp lights its own room (layer over 0.5/255)");
    Check(crossStory == 0, "no light crosses a story (" + std::to_string(crossStory) + " texels)");
    std::vector<double> pt(perTexel.begin(), perTexel.end());
    std::printf("  sparsity (layer > 0.5/255): a lamp touches median %.0f texels (max %.0f) of %zu; a texel is touched by median %.0f lamps, max %.0f\n",
                Pct(touched, 0.5), Pct(touched, 1.0), T, Pct(pt, 0.5), Pct(pt, 1.0));
    // edits: the lamps a wall edit or a moved table dirties
    for (const auto& e : EditSites(b)) {
        long byLayer = 0, byReach = 0, nearPatch = 0;
        for (size_t l = 0; l < L; l++) {
            const BigLamp& d = b.lamp[l];
            float r0, R;
            LampReach::Radii(d.range, r0, R);
            byReach += d.story == static_cast<int>(e[3]) && Dist(d.pos, e.data()) < R;
            bool hit = false;
            for (size_t t = 0; t < T && !hit; t++) {
                const GiRef::Patch& P = m.patches[m.texels[t]];
                hit = P.story == static_cast<int>(e[3]) && std::fabs(P.p[0] - e[0]) < 2.0f && std::fabs(P.p[2] - e[2]) < 2.0f &&
                      std::max({layers[l][t][0], layers[l][t][1], layers[l][t][2]}) > eps;
            }
            byLayer += hit;
        }
        for (const GiRef::Patch& P : m.patches) nearPatch += Dist(P.p, e.data()) < GiRef::kReach;
        const double frac = static_cast<double>(nearPatch) / m.patches.size();
        std::printf("  edit at (%.1f, %.1f, %.1f): %ld lamps lit within 2 m (layer), %ld within the lamp's reach R; patches within the link reach %.0f%% -> "
                    "local re-link about %.0f ms, re-bake %ld layers about %.0f ms (one thread)\n",
                    e[0], e[1], e[2], byLayer, byReach, 100.0 * frac, frac * linkMs, byLayer, byLayer * Pct(ms, 0.5));
    }
    if (b.lamps <= 60) { // compose = full solve: the data model is linear on this lot too
        std::fill(on.begin(), on.end(), true);
        GiRef::Compose(layers, on, colour, comp);
        std::vector<GiRef::Rgb> full;
        t0 = Clock::now();
        GiRef::FullSolve(m, mesh, lamps, on, colour, full);
        const double fullMs = Ms(t0);
        double d = 0.0;
        for (size_t t = 0; t < T; t++)
            for (int c = 0; c < 3; c++) d = std::max(d, static_cast<double>(std::fabs(comp[t][c] - full[t][c])));
        std::printf("  full solve of all lamps at once %.0f ms; compose - full %.2e\n", fullMs, d);
        Check(d < 1.0 / 255.0, "compose of the layers equals the full solve within 1/255");
    }
}

// ---------------------------------------------------------------- server solve
Scene ServerScene(const BigLot& b, int density) {
    using namespace RtLampProxy;
    Scene sc;
    sc.density = density, sc.lot = b.G, sc.stories = 2;
    Lot lot = b.lot;
    for (Story& st : lot.stories) st.placed.clear(); // as BuildScene: floors added below, no slab over a ceiling texel
    Mesh& m = sc.mesh;
    Build(lot, m);
    m.curKind = Mesh::kSlab;
    for (const Story& st : lot.stories)
        for (int z = 0; z < st.h; z++)
            for (int x = 0; x < st.w; x++) {
                const float y = st.floorY[static_cast<size_t>(z) * st.w + x];
                if (!(y < kNoFloor * 0.5f)) continue;
                const float a[3] = {float(x), y, float(z)}, c1[3] = {float(x + 1), y, float(z)}, c2[3] = {float(x + 1), y, float(z + 1)}, c3[3] = {float(x), y, float(z + 1)};
                AddTri(m, a, c1, c2), AddTri(m, a, c2, c3);
            }
    for (const auto& box : b.boxes) AddShape(m, box);
    BuildGrid(m);
    const int S = 2, G = b.G, D = density, GD = G * D;
    sc.map.assign(static_cast<size_t>(2) * S * GD * GD + static_cast<size_t>(4) * S * (G + 1) * GD * 3 * D, -1);
    const float step = 1.0f / D;
    const auto add = [&](size_t cell, float x, float y, float z, float nx, float ny, float nz, int room, const float alb[3]) {
        sc.map[cell] = static_cast<int32_t>(sc.texels.size());
        sc.texels.push_back({{x, y, z, static_cast<float>(room)}, {nx, ny, nz, 0}, {alb[0], alb[1], alb[2], 0}});
    };
    const float kFloor[3] = {0.50f, 0.40f, 0.30f}, kCeil[3] = {0.80f, 0.80f, 0.80f}, kWall[3] = {0.70f, 0.70f, 0.70f};
    for (int s = 0; s < S; s++) {
        for (int iz = 0; iz < GD; iz++)
            for (int ix = 0; ix < GD; ix++) {
                const int room = BigRoom(b, s, ix / D, iz / D);
                if (!room) continue;
                const float x = (ix + 0.5f) * step, z = (iz + 0.5f) * step;
                add((static_cast<size_t>(0 * S + s) * GD + iz) * GD + ix, x, kStory * s + 0.02f, z, 0, 1, 0, room, kFloor);
                add((static_cast<size_t>(1 * S + s) * GD + iz) * GD + ix, x, kStory * (s + 1) - 0.02f, z, 0, -1, 0, room, kCeil);
            }
        for (const Edge& e : lot.stories[s].edges) {
            const int axis = e.x1 == e.x0 ? 0 : 1;
            const int line = static_cast<int>(axis == 0 ? e.x0 : e.z0), along0 = static_cast<int>(axis == 0 ? e.z0 : e.x0) * D;
            for (int side = 0; side < 2; side++) {
                const float sg = side ? 1.0f : -1.0f;
                const int tx = axis == 0 ? line - (side ? 0 : 1) : static_cast<int>(e.x0), tz = axis == 1 ? line - (side ? 0 : 1) : static_cast<int>(e.z0);
                const int room = BigRoom(b, s, tx, tz);
                if (!room) continue;
                for (int k = 0; k < D; k++)
                    for (int h = 0; h < 3 * D; h++) {
                        const float u = (k + 0.5f) * step, y = (h + 0.5f) * step;
                        if (e.open && u > e.o.u0 && u < e.o.u1 && y > e.o.sill && y < OpeningHead(e.o, u)) continue;
                        const size_t cell = static_cast<size_t>(2) * S * GD * GD + ((((static_cast<size_t>(axis) * 2 + side) * S + s) * (G + 1) + line) * GD + along0 + k) * (3 * D) + h;
                        const float px = axis == 0 ? line + 0.05f * sg : e.x0 + u, pz = axis == 1 ? line + 0.05f * sg : e.z0 + u;
                        add(cell, px, kStory * s + y, pz, axis == 0 ? sg : 0, 0, axis == 1 ? sg : 0, room, kWall);
                    }
            }
        }
    }
    for (const BigLamp& d : b.lamp) sc.lamps.push_back({{d.pos[0], d.pos[1], d.pos[2]}, d.radius, {d.col[0], d.col[1], d.col[2]}, d.room, "lamp"});
    return sc;
}

constexpr int kBatchBounces = 16; // the settle count measured per lamp is 13 to 17
void GpuPart(const Gpu& g, const BigLot& b, int density, uint32_t nRays) {
    const Scene sc = ServerScene(b, density);
    const size_t T = sc.texels.size(), L = sc.lamps.size();
    std::printf("\n[server] %d lamps on %s: %zu triangles, %zu texels at %d a metre, %u rays a texel\n", b.lamps, g.name.c_str(), sc.mesh.tris.size(), T, density, nRays);
    for (int pass = 0; pass < 2; pass++) {
        const bool rays = pass == 0;
        if (rays && !g.rays) continue;
        Solve(g, rays, sc, nRays, false); // (the device's first passes run slow)
        const Result r = Solve(g, rays, sc, nRays, false, kBatchBounces);
        const int bounces = std::accumulate(r.bounces.begin(), r.bounces.end(), 0);
        const double layerMs = (r.directSec + r.bounceSec) * 1e3 / L;
        std::printf("  [%s] accel %.1f ms, links %.1f ms; a lamp: direct %.2f ms, %.1f bounces x %.3f ms = layer %.2f ms; all %zu layers %.0f ms; lot from "
                    "nothing %.0f ms\n",
                    rays ? "rays" : "compute", r.accelSec * 1e3, r.linkSec * 1e3, r.directSec * 1e3 / L, static_cast<double>(bounces) / L, r.bounceSec * 1e3 / bounces,
                    layerMs, L, layerMs * L, (r.accelSec + r.linkSec + r.directSec + r.bounceSec) * 1e3);
        std::printf("  [%s] batched, 8 lamps a submission: direct + %d bounces a lamp %.2f ms; all %zu layers %.0f ms; lot from nothing %.0f ms\n", rays ? "rays" : "compute",
                    kBatchBounces, r.batchSec * 1e3 / L, L, r.batchSec * 1e3, (r.accelSec + r.linkSec + r.batchSec) * 1e3);
        bool finite = true, ownLit = true;
        long cross = 0;
        for (size_t l = 0; l < L; l++) {
            const Lamp& lamp = sc.lamps[l];
            const int story = lamp.pos[1] >= kStory ? 1 : 0;
            float own = 0.0f;
            for (size_t t = 0; t < T; t++) {
                const float* ind = &r.ind[(l * T + t) * 4];
                const float* dir = &r.direct[(l * T + t) * 4];
                const float v = std::max({ind[0], ind[1], ind[2]}) + std::max({dir[0], dir[1], dir[2]});
                finite = finite && std::isfinite(v);
                const int ts = sc.texels[t].pos[1] >= kStory ? 1 : 0;
                if (ts != story && v != 0.0f) cross++;
                if (static_cast<int>(sc.texels[t].pos[3]) == lamp.room) own = std::max(own, std::max({ind[0], ind[1], ind[2]}));
            }
            ownLit = ownLit && own > 0.0f;
        }
        const std::string tag = rays ? "[rays] " : "[compute] ";
        Check(finite, tag + "every layer is finite");
        Check(ownLit, tag + "every lamp's indirect light reaches its own room");
        Check(cross == 0, tag + "no light crosses a story (" + std::to_string(cross) + " texel values)");
    }
}
} // namespace

int main(int argc, char** argv) {
    std::vector<int> lots = {60, 120, 200};
    bool cpu = true, gpu = true;
    std::string want;
    int density = 1, nRays = 256;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--lots" && i + 1 < argc) {
            lots.clear();
            std::stringstream ss(argv[++i]);
            for (std::string v; std::getline(ss, v, ',');) lots.push_back(std::clamp(std::atoi(v.c_str()), 4, 400));
        } else if (a == "--no-cpu") cpu = false;
        else if (a == "--no-gpu") gpu = false;
        else if (a == "--device" && i + 1 < argc) want = argv[++i];
        else if (a == "--density" && i + 1 < argc) density = std::clamp(std::atoi(argv[++i]), 1, 8);
        else if (a == "--rays" && i + 1 < argc) nRays = std::clamp(std::atoi(argv[++i]), 1, 4096);
        else {
            std::printf("FAIL  unknown argument %s\n", a.c_str());
            return 1;
        }
    }
    Gpu g;
    if (gpu) {
        g = OpenGpu(want, true);
        double floor = 0.0;
        for (int i = 0; i < 100; i++) floor += Run(g, [](VkCommandBuffer) {});
        std::printf("Device %s (ray queries: %s); empty submit + wait %.3f ms\n", g.name.c_str(), g.rays ? "yes" : "no", floor * 10.0);
    }
    for (int n : lots) {
        const BigLot b = MakeLot(n);
        std::printf("\n==== %d lamps: %d rooms, %d house tiles a story (%d x %d m)\n", n, b.nx * b.nz * 2, HouseTiles(b), b.nx * kRoomSize, b.nz * kRoomSize);
        if (cpu) CpuPart(b);
        if (gpu) GpuPart(g, b, density, static_cast<uint32_t>(nRays));
    }
    if (gpu) {
        vkDestroyCommandPool(g.dev, g.pool, nullptr);
        vkDestroyDevice(g.dev, nullptr);
        vkDestroyInstance(g.inst, nullptr);
    }
    std::printf(g_failed ? "FAILED: %d check(s)\n" : "All checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
