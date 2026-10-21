// Offline checks of the crown occluder (features/foliage_canopy.h) and its phase in the lot shade mask
// (features/ground_lamp_shadow.h ShadeJob). Read-only, no game. Credits: @loinyx
#include <chrono>
#include <cstdio>
#include "features/ground_lamp_shadow.h"

using namespace Canopy;
static int g_checks = 0, g_failures = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

static std::vector<uint8_t> Build(const GroundShadow::Grid& g, const std::vector<GroundShadow::Lamp>& lamps, const std::vector<GroundShadow::Wall>& walls,
                                  const std::vector<Ellipsoid>& crowns, std::vector<float>* weight = nullptr, long long* tests = nullptr) {
    GroundShadow::ShadeJob job;
    job.Init(g, lamps, walls, {}, crowns);
    while (!job.Step([] { return true; })) {}
    if (weight) *weight = job.StampWeight();
    if (tests) *tests = job.CanopyTests();
    return job.Result();
}

int main() {
    const Ellipsoid tree{0, 4, 0, 2, 2, 2, 1, 0, kLadTree}; // a round crown of 2 m radius, centre 4 m up
    // 1. The model
    Check(std::fabs(ChordLength(tree, 0, 10, 0, 0, 0, 0) - 4.0f) < 1e-4f, "vertical chord through the centre is the diameter");
    const float T = Transmission(tree, 0, 10, 0, 0, 0, 0);
    Check(T > 0.2f && T < 0.26f, "transmission through the centre within the measured gap fraction (0.1 to 0.35)");
    Check(Transmission(tree, 5, 10, 0, 5, 0, 0) == 1.0f, "a ray beside the crown keeps all the light");
    Check(Transmission(tree, 0, 1.5f, 0, 1, 0, 1) == 1.0f, "a head below the crown's base lights the ground freely");
    float x0, z0, x1, z1;
    Check(!ShadowBox(tree, 0, 1.5f, 0, 0, 20, x0, z0, x1, z1), "no shadow box for a head below the crown");
    float prev = 0;
    bool monotone = true;
    for (float x = 0; x <= 2.2f; x += 0.01f) {
        const float t = Transmission(tree, x, 10, 0, x, 0, 0);
        monotone = monotone && t >= prev - 1e-5f;
        prev = t;
    }
    Check(monotone && prev == 1.0f, "the shadow fades continuously to the crown's shell (soft edge, no step)");
    Check(std::fabs(ChordLength(tree, 0, 4, 0, 0, 0, 0) - 2.0f) < 1e-4f, "a head inside the crown counts only the leaves below it");
    const Ellipsoid a{0, 4, 0, 2, 1, 0.5f, 1, 0, 1}, b{0, 4, 0, 0.5f, 1, 2, 0, 1, 1};
    Check(std::fabs(ChordLength(a, -5, 4, 0, 5, 4, 0) - ChordLength(b, -5, 4, 0, 5, 4, 0)) < 1e-4f, "yaw turns the crown");

    // 2. The shade mask: a 20 x 20 m lot, a garden post 3 m high at (6, 10), a tree beside it
    GroundShadow::Grid g;
    g.w = g.h = 80;
    const std::vector<GroundShadow::Lamp> lamps = {{6.0f, 3.0f, 10.0f, 1.0f}};
    const std::vector<Ellipsoid> crowns = {{9.0f, 2.5f, 10.0f, 1.5f, 1.0f, 1.5f, 1, 0, kLadTree}};
    std::vector<float> w0, w1;
    long long tests = 0;
    const auto without = Build(g, lamps, {}, {}, &w0);
    const auto with = Build(g, lamps, {}, crowns, &w1, &tests);
    Check(without.size() == with.size() && !with.empty(), "both masks built");
    bool never_lighter = true, darker_behind = false;
    for (size_t i = 0; i < with.size(); i++) never_lighter = never_lighter && with[i] <= without[i];
    float fi, fj;
    GroundShadow::WorldTexel(g, 12.0f, 10.0f, fi, fj); // behind the crown, seen from the post
    const size_t behind = static_cast<size_t>(fj) * g.w + static_cast<size_t>(fi);
    darker_behind = with[behind] < without[behind];
    GroundShadow::WorldTexel(g, 2.0f, 10.0f, fi, fj); // on the post's other side
    const size_t front = static_cast<size_t>(fj) * g.w + static_cast<size_t>(fi);
    Check(never_lighter, "a crown never lights the ground");
    Check(darker_behind, "the ground behind the crown takes less of the post's light");
    Check(with[front] == without[front], "the ground on the post's other side is unchanged");
    Check(w0 == w1, "the stamp weight is the same (a crown changes only the share that reaches)");
    Check(tests > 0, "the crown phase ran");
    // 3. Walls and crowns in series: never lighter than the walls alone
    const std::vector<GroundShadow::Wall> walls = {{11.0f, 5.0f, 11.0f, 15.0f, 0.0f, 2.0f}};
    const auto wallsOnly = Build(g, lamps, walls, {});
    const auto both = Build(g, lamps, walls, crowns);
    bool series = true;
    for (size_t i = 0; i < both.size(); i++) series = series && both[i] <= wallsOnly[i] && both[i] <= with[i];
    Check(series, "walls and crowns multiply (never lighter than either alone)");
    // 4. No crowns: identical to the previous build
    Check(Build(g, lamps, walls, {}) == wallsOnly, "an empty crown list changes nothing");

    // 5. Cost: 32 lamps x 60 crowns over a 120 x 120 texel lot (worker thread, not the render thread)
    GroundShadow::Grid big;
    big.w = big.h = 120;
    std::vector<GroundShadow::Lamp> many;
    for (int i = 0; i < 32; i++) many.push_back({static_cast<float>(i % 6) * 5.0f + 2, 3.0f, static_cast<float>(i / 6) * 5.0f + 2, 1.0f});
    std::vector<Ellipsoid> forest;
    for (int i = 0; i < 60; i++) forest.push_back({static_cast<float>(i % 8) * 4.0f + 1, 4, static_cast<float>(i / 8) * 4.0f + 1, 2, 2, 2, 1, 0, kLadTree});
    const auto t0 = std::chrono::steady_clock::now();
    long long heavy = 0;
    Build(big, many, {}, forest, nullptr, &heavy);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("Worst case: %lld crown tests in %.1f ms (worker thread)\n", heavy, ms);
    std::printf("Canopy shade: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
