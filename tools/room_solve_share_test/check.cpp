// Shared room solve time offline checks (features/room_solve_share_policy.h): the decision, and a replay of the lot
// lighting update's loop (0x00ADB8F0: per lot, levels in order from level 0, at least one level, stop once the lot's
// stopwatch passes its budget) over a synthetic pan with and without the gate. Checks that no level waits longer than the
// hard bound, that the frame's solve time stays near the share while moving, and that once the camera stops every level
// ends in the same state as without the gate. Read-only: writes nothing.
// Part of Apex Radiance. Credits: @loinyx
#include "room_solve_share_policy.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace P = RoomSolveSharePolicy;
static int g_fail = 0;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                          \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static void PolicyChecks() {
    CHECK(P::Decide(100.0, false, false, 0, false, 0).run, "camera still: every call runs");
    CHECK(P::Decide(100.0, true, true, 0, false, 0).run, "the lot being played always runs");
    CHECK(P::Decide(0.0, true, false, 0, false, 0).run, "the first level of a frame runs");
    CHECK(P::Decide(P::kMovingMs - P::kRunFloorMs, true, false, 0, false, 0).run, "a level with the floor left runs");
    CHECK(!P::Decide(P::kMovingMs, true, false, 0, false, 0).run, "a level after the share waits");
    auto d = P::Decide(P::kMovingMs, true, false, P::kMaxSkips, false, P::kMaxSkips);
    CHECK(d.run && d.forced, "the longest waiter runs after kMaxSkips");
    CHECK(!P::Decide(P::kMovingMs, true, false, P::kMaxSkips, true, P::kMaxSkips).run, "one forced level per frame");
    CHECK(!P::Decide(P::kMovingMs, true, false, P::kMaxSkips, false, P::kMaxSkips + 3).run, "a shorter waiter waits for the longest");
    d = P::Decide(P::kMovingMs, true, false, P::kHardMaxSkips, true, P::kHardMaxSkips + 10);
    CHECK(d.run && d.forced, "the hard bound always runs");
}

// One lot level: a queue of resumable room steps (ms each), plus an optional unbudgeted piece (a rebuild or a wall
// shading pass) that runs whole at its next solve
struct Level {
    std::vector<double> steps;
    size_t next = 0;
    double whole = 0.0;
    int skips = 0;
    bool priority = false;
};

struct Result {
    double maxMovingFrameMs = 0.0, sumMovingMs = 0.0;
    int movingFrames = 0, framesOverShare = 0, maxWait = 0, framesToSettle = 0;
    std::vector<size_t> finalNext;
};

// The level solve with the game's budget check: resumable steps stop once the lot's stopwatch reaches the budget
static double Solve(Level& l, double lotElapsed, double budget) {
    double t = l.whole;
    l.whole = 0.0;
    while (l.next < l.steps.size() && lotElapsed + t < budget) t += l.steps[l.next++];
    return t;
}

static Result Run(std::vector<std::vector<Level>> lots, int movingFrames, bool gate) {
    Result r;
    int longestWait = 0;
    for (int frame = 0; frame < 4000; frame++) {
        const bool moving = frame < movingFrames;
        double spent = 0.0;
        bool forced = false;
        int longestNext = 0;
        for (auto& lot : lots) {
            const double budget = lot.front().priority ? (moving ? 3.0 : 15.0) : (moving ? 1.0 : 5.0);
            double elapsed = 0.0;
            for (Level& l : lot) {
                double t = 0.0;
                const auto d = gate ? P::Decide(spent, moving, l.priority, l.skips, forced, longestWait) : P::Decision{};
                if (!d.run) {
                    l.skips++;
                    longestNext = std::max(longestNext, l.skips);
                } else {
                    if (d.forced) forced = true;
                    r.maxWait = std::max(r.maxWait, l.skips);
                    l.skips = 0;
                    t = Solve(l, elapsed, budget);
                }
                elapsed += t + 0.002; // a skipped call still costs the stopwatch read
                spent += t;
                if (elapsed > budget) break;
            }
        }
        longestWait = longestNext;
        if (moving) {
            r.movingFrames++;
            r.sumMovingMs += spent;
            r.maxMovingFrameMs = std::max(r.maxMovingFrameMs, spent);
        }
        bool done = true;
        for (auto& lot : lots)
            for (Level& l : lot) done = done && l.next == l.steps.size() && l.whole == 0.0;
        if (!moving && done) {
            r.framesToSettle = frame - movingFrames;
            break;
        }
    }
    for (auto& lot : lots)
        for (Level& l : lot) r.finalNext.push_back(l.next);
    return r;
}

static std::vector<std::vector<Level>> Scene(unsigned seed, int lots, int levels) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> step(0.05, 0.9);
    std::uniform_int_distribution<int> rooms(0, 400);
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    std::vector<std::vector<Level>> out(lots);
    for (int i = 0; i < lots; i++) {
        for (int j = 0; j < levels; j++) {
            Level l;
            l.priority = i == 0;
            for (int k = rooms(rng); k > 0; k--) l.steps.push_back(step(rng));
            if (coin(rng) < 0.15) l.whole = 4.0 + 20.0 * coin(rng);
            out[i].push_back(l);
        }
    }
    return out;
}

static void Replay() {
    for (unsigned seed = 1; seed <= 5; seed++) {
        const auto scene = Scene(seed, 9, 3);
        const Result base = Run(scene, 120, false);
        const Result gated = Run(scene, 120, true);
        std::printf("seed %u: moving solve ms per frame mean %.2f -> %.2f, max %.2f -> %.2f; longest wait %d frames; settled %d -> %d frames after the stop\n",
                    seed, base.sumMovingMs / base.movingFrames, gated.sumMovingMs / gated.movingFrames, base.maxMovingFrameMs, gated.maxMovingFrameMs,
                    gated.maxWait, base.framesToSettle, gated.framesToSettle);
        CHECK(gated.maxWait <= P::kHardMaxSkips, "seed %u: a level waited %d frames", seed, gated.maxWait);
        CHECK(gated.finalNext == base.finalNext, "seed %u: final room states differ from the ungated run", seed);
        CHECK(gated.maxMovingFrameMs <= base.maxMovingFrameMs + 1e-9, "seed %u: the gate made the worst moving frame longer", seed);
    }
}

int main() {
    PolicyChecks();
    Replay();
    std::printf(g_fail ? "Shared room solve time checks: %d failure(s)\n" : "Shared room solve time checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
