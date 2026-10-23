// Offline checks of the lamp shade on lot ground and on the world grass around lots (features/ground_lamp_shadow.h):
// the lot build's time on a synthetic 40 m lot (about 20 walls, 8 lamps), its result against a reference copy of the header
// (run.ps1 -Reference <revision>: bit for bit), and a chunk texture made again over a changed lot's box only against the
// whole chunk made again (bit for bit, every mip). Read-only, no game. Credits: @loinyx
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <type_traits>
#include "features/ground_lamp_shadow.h"
#ifdef APEX_SHADE_REF
#define GroundShadow GroundShadowRef
#include "ground_lamp_shadow_ref.h"
#undef GroundShadow
#endif

static int g_checks = 0, g_failures = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// ---- A synthetic lot: 40 x 40 m, a house with windows and a door, garden walls, fences; 8 lamps on it, 4 past it
struct Scene {
    GroundShadow::Grid grid;
    int ox = 0, oz = 0, lotW = 0, lotH = 0;
    std::vector<GroundShadow::Lamp> lamps, others;
    std::vector<GroundShadow::Wall> walls;
    std::vector<WallOpenings::Patch> patches;
};
static WallOpenings::Patch Opening(float u0, float u1, float sill, float head) {
    WallOpenings::Patch p;
    p.u0 = u0, p.u1 = u1, p.ou0 = u0 + 0.02f, p.ou1 = u1 - 0.02f, p.sill = sill, p.open = 1, p.jamb0 = p.jamb1 = 1;
    for (float& h : p.head) h = head;
    return p;
}
// turn: the lot's rotation (radians); soft: Grid::soft
static Scene MakeScene(float turn, bool soft, float tx = 1000.0f, float tz = 2000.0f) {
    Scene s;
    const float c = std::cos(turn), sn = std::sin(turn), ty = 10.0f, L = 40.0f;
    const auto world = [&](float lx, float lz, float& x, float& z) { x = c * lx - sn * lz + tx, z = sn * lx + c * lz + tz; };
    const auto lamp = [&](float lx, float lz, float h, float k) {
        GroundShadow::Lamp l;
        world(lx, lz, l.x, l.z);
        l.y = ty + h, l.k = k;
        l.rx0 = l.x - 20.0f, l.rx1 = l.x + 20.0f, l.rz0 = l.z - 20.0f, l.rz1 = l.z + 20.0f;
        return l;
    };
    const float lampAt[8][4] = {{4, 4, 2.0f, 6}, {36, 5, 2.2f, 8}, {20, 25, 1.8f, 5}, {5, 35, 2.5f, 10}, {35, 36, 2.0f, 7}, {14, 8, 0.3f, 3}, {28, 18, 2.4f, 9}, {22, 38, 2.0f, 4}};
    for (const auto& a : lampAt) s.lamps.push_back(lamp(a[0], a[1], a[2], a[3]));
    const float otherAt[4][4] = {{-8, 10, 2.0f, 6}, {50, 20, 2.0f, 5}, {20, -10, 2.2f, 8}, {45, 48, 2.0f, 4}};
    for (const auto& a : otherAt) s.others.push_back(lamp(a[0], a[1], a[2], a[3]));
    const auto wall = [&](float ax, float az, float bx, float bz, float h, int openings = 0) {
        GroundShadow::Wall w;
        world(ax, az, w.x0, w.z0);
        world(bx, bz, w.x1, w.z1);
        w.y0 = ty, w.y1 = ty + h;
        if (openings) {
            w.p0 = static_cast<uint32_t>(s.patches.size());
            if (openings & 1) s.patches.push_back(Opening(0.3f, 0.5f, ty + 0.9f, ty + 2.1f)); // a window
            if (openings & 2) s.patches.push_back(Opening(0.6f, 0.8f, ty, ty + 2.2f));        // a door
            w.pn = static_cast<uint32_t>(s.patches.size()) - w.p0;
        }
        s.walls.push_back(w);
    };
    // the house: 12 x 10 m, each side in 2 or 3 runs in line
    wall(10, 12, 16, 12, 3, 1), wall(16, 12, 22, 12, 3, 3), wall(22, 12, 22, 17, 3), wall(22, 17, 22, 22, 3, 1), wall(22, 22, 16, 22, 3),
        wall(16, 22, 10, 22, 3, 2), wall(10, 22, 10, 17, 3), wall(10, 17, 10, 12, 3, 1), wall(16, 12, 16, 17, 3), wall(16, 17, 16, 22, 3);
    // garden walls and fences
    wall(2, 28, 8, 28, 1.2f), wall(8, 28, 14, 28, 1.2f), wall(26, 4, 26, 10, 1.6f), wall(30, 26, 38, 26, 1.0f), wall(30, 30, 30, 38, 1.0f),
        wall(4, 15, 4, 22, 2.0f), wall(32, 10, 38, 14, 1.5f), wall(12, 32, 18, 34, 1.0f), wall(24, 30, 24, 34, 2.4f), wall(0.5f, 0.5f, 6, 0.5f, 1.0f);
    // the grid: the lot grown by its lamps' reach, at most 24 m (lot_light_bridge.cpp ShadeGrid)
    s.lotW = s.lotH = static_cast<int>(L * GroundShadow::kTexelsPerMetre);
    float lx0 = 0, lz0 = 0, lx1 = L, lz1 = L;
    for (const GroundShadow::Lamp& l : s.lamps) {
        const float dx = l.x - tx, dz = l.z - tz, lx = c * dx + sn * dz, lz = -sn * dx + c * dz, R = GroundShadow::Reach(l, ty);
        lx0 = std::min(lx0, lx - R), lz0 = std::min(lz0, lz - R), lx1 = std::max(lx1, lx + R), lz1 = std::max(lz1, lz + R);
    }
    const auto texels = [](float m) { return static_cast<int>(std::ceil(std::clamp(m, 0.0f, 24.0f) * GroundShadow::kTexelsPerMetre)); };
    s.ox = texels(-lx0), s.oz = texels(-lz0);
    GroundShadow::Grid& g = s.grid;
    g.w = s.lotW + s.ox + texels(lx1 - L), g.h = s.lotH + s.oz + texels(lz1 - L);
    g.m0 = c, g.m2 = sn, g.m8 = -sn, g.m10 = c, g.ty = ty, g.soft = soft;
    const float sx = -s.ox / GroundShadow::kTexelsPerMetre, sz = -s.oz / GroundShadow::kTexelsPerMetre;
    g.tx = tx + c * sx - sn * sz, g.tz = tz + sn * sx + c * sz;
    return s;
}

// The scene's inputs as another copy of the header's types (the reference's: the same members, bit for bit)
template <class T, class S> static T As(const S& s) {
    static_assert(sizeof(T) == sizeof(S) && std::is_trivially_copyable_v<S>);
    T t;
    std::memcpy(static_cast<void*>(&t), &s, sizeof t);
    return t;
}
template <class T, class S> static std::vector<T> As(const std::vector<S>& v) {
    std::vector<T> out;
    for (const S& s : v) out.push_back(As<T>(s));
    return out;
}
template <class Job, class Grid, class Lamp, class Wall> static void Run(const Scene& s, std::vector<uint8_t>& res, std::vector<float>& den, double& ms) {
    const auto t0 = std::chrono::steady_clock::now();
    Job job;
    job.Init(As<Grid>(s.grid), As<Lamp>(s.lamps), As<Wall>(s.walls), s.patches, {}, As<Lamp>(s.others));
    job.Step([] { return true; });
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    res = job.Result();
    den = job.StampWeight();
}
// The fastest of reps builds (the least disturbed by other work on the machine)
template <class Job, class Grid, class Lamp, class Wall> static double Fastest(const Scene& s, int reps, std::vector<uint8_t>& res, std::vector<float>& den) {
    double best = 1e30;
    for (int r = 0; r < reps; r++) {
        double v;
        Run<Job, Grid, Lamp, Wall>(s, res, den, v);
        best = std::min(best, v);
    }
    return best;
}

static void LotBuildChecks(int reps) {
    for (const float turn : {0.0f, 0.5235988f})
        for (const bool soft : {true, false}) {
            const Scene s = MakeScene(turn, soft);
            std::vector<uint8_t> res;
            std::vector<float> den;
            const double ms = Fastest<GroundShadow::ShadeJob, GroundShadow::Grid, GroundShadow::Lamp, GroundShadow::Wall>(s, reps, res, den);
            int dark = 0;
            for (uint8_t v : res) dark += v < 128;
            std::printf("lot build: 40 m lot, turn %.0f deg, soft %s: grid %d x %d, %zu lamps (+%zu), %zu walls: %.2f ms (fastest of %d), %d dark texels\n",
                        turn * 57.29578f, soft ? "on" : "off", s.grid.w, s.grid.h, s.lamps.size(), s.others.size(), s.walls.size(), ms, reps, dark);
            Check(res.size() == static_cast<size_t>(s.grid.w) * s.grid.h && dark > 500, "lot build: the mask builds and the walls shade it");
#ifdef APEX_SHADE_REF
            std::vector<uint8_t> refRes;
            std::vector<float> refDen;
            const double refMs = Fastest<GroundShadowRef::ShadeJob, GroundShadowRef::Grid, GroundShadowRef::Lamp, GroundShadowRef::Wall>(s, reps, refRes, refDen);
            std::printf("lot build: reference %.2f ms -> now %.2f ms (%.2fx)\n", refMs, ms, refMs / ms);
            Check(res == refRes, "lot build: the mask equals the reference's, bit for bit");
            Check(den.size() == refDen.size() && std::memcmp(den.data(), refDen.data(), den.size() * sizeof(float)) == 0,
                  "lot build: the stamp weight equals the reference's, bit for bit");
#endif
        }
}

// GroundShadow::ToByte against std::lround over every float in [0, 255]
static void ToByteChecks() {
    uint32_t bad = 0;
    for (uint32_t b = 0; b <= 0x437F0000u; b++) { // 0 .. 255.0f
        float v;
        std::memcpy(&v, &b, sizeof v);
        bad += GroundShadow::ToByte(v) != static_cast<uint8_t>(std::lround(v));
    }
    Check(bad == 0, "ToByte: lround's byte for every float in [0, 255]");
}

int main(int argc, char** argv) {
    int reps = 9;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::max(1, std::atoi(argv[++i]));
    ToByteChecks();
    LotBuildChecks(reps);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
