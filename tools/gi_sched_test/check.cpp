// GI update scheduler (S2-01), offline check: event sequences through GiSched::Scheduler with the fake back end, checked
// for the dirty sets, the order, the budget, the crossfade timing and starvation. Prints to stdout; exit code 1 on a failure.
#include "features/gi_sched.h"
#include <cstdio>
#include <vector>

using namespace GiSched;

static int g_fail = 0;
#define CHECK(c, ...)                                                                                                                                \
    do {                                                                                                                                              \
        if (!(c)) {                                                                                                                                   \
            ++g_fail;                                                                                                                                 \
            std::printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c);                                                                                  \
            std::printf(__VA_ARGS__);                                                                                                                 \
            std::printf("\n");                                                                                                                        \
        }                                                                                                                                             \
    } while (0)

constexpr double kFrameMs = 1000.0 / 60.0;

// A row of rooms 5 m apart along x on story 0; lamp i at x = 5 i + 2.5. Its layer touches its room and the next (a door).
struct Rig {
    FakeBackend fake;
    Scheduler s;
    std::vector<LampIn> lamps;
    uint32_t frame = 1;
    double ms = 1000;
    View view;
    int maxIssuedAFrame = 0;
    explicit Rig(int n, Budget b = {}) : s(&fake, b) {
        fake.boxOf = [](const LampIn& l) { return Box{static_cast<int>(l.pos[0]) - 2, 0, static_cast<int>(l.pos[0]) + 7, 4, l.story, l.story}; };
        for (int i = 0; i < n; i++) lamps.push_back({static_cast<uintptr_t>(0x1000 + i), {5.0f * i + 2.5f, 1.5f, 2.5f}, 0, true});
        view.known = true; // camera at the first room
        view.eye[0] = 2.5f, view.eye[1] = 8.0f, view.eye[2] = -5.0f;
    }
    uint64_t Issued() const {
        uint64_t n = 0;
        for (const uint64_t v : s.Stats().issued) n += v;
        return n;
    }
    void Step(int frames = 1) {
        for (int i = 0; i < frames; i++) {
            const uint64_t before = Issued();
            s.Tick(frame++, ms += kFrameMs, view);
            maxIssuedAFrame = std::max(maxIssuedAFrame, static_cast<int>(Issued() - before));
        }
    }
    void Settle(int maxFrames = 5000) {
        for (int i = 0; i < maxFrames; i++) {
            const Counters c = s.Stats();
            if (!c.queued && !c.inFlight && !c.idle && !c.fading) return;
            Step();
        }
    }
};

static void LoadAndOrder() {
    Rig r(100);
    r.s.Load(r.lamps, r.frame);
    r.Step();
    Counters c = r.s.Stats();
    CHECK(c.issued[static_cast<int>(Cause::Load)] == 2, "first frame issues 2, got %llu", c.issued[3]);
    CHECK(r.s.InFlight(0x1000) && r.s.InFlight(0x1001), "the camera-near lamps go first");
    CHECK(c.dropped == 100 - kQueueCap - 0, "over the cap to the idle list: dropped %llu", c.dropped);
    r.Settle();
    c = r.s.Stats();
    CHECK(c.landed == 100, "every lamp lands after a load (no starvation of the idle list): %llu", c.landed);
    CHECK(r.maxIssuedAFrame <= 2 && c.maxInFlight <= 8 && c.overBudget == 0, "budget: %d a frame, %d in flight", r.maxIssuedAFrame, c.maxInFlight);
    for (const LampIn& l : r.lamps) CHECK(r.s.HasLayer(l.id), "lamp %zx has a layer", l.id);
    std::printf("load 100 lamps: landed %llu, dropped to idle %llu, p50 %u max %u frames from dirty to landed\n", c.landed, c.dropped, c.latencyP50, c.latencyMax);
    // lot switch: a new lamp set drops every old layer
    std::vector<LampIn> other(r.lamps.begin(), r.lamps.begin() + 10);
    for (auto& l : other) l.id += 0x10000;
    r.s.Load(other, r.frame);
    CHECK(!r.s.HasLayer(0x1000) && r.s.Queued(0x11000), "lot switch: old layers dropped, new lamps queued");
    r.Settle();
    CHECK(r.s.Stats().landed == 110, "lot switch lands its 10 lamps: %llu", r.s.Stats().landed);
}

static void OffLast() {
    Rig r(4);
    r.lamps[0].on = false; // the nearest lamp is off: it bakes last
    r.s.Load(r.lamps, r.frame);
    r.Step();
    CHECK(!r.s.InFlight(0x1000) && r.s.InFlight(0x1001) && r.s.InFlight(0x1002), "an off lamp bakes after the lamps on");
    // in view beats nearer
    Rig v(4);
    v.view.hasVp = true; // a view that sees only x > 12 (clip x = world x - 16, w = 4)
    for (auto& row : v.view.vp)
        for (float& f : row) f = 0;
    v.view.vp[0][0] = 1, v.view.vp[3][0] = -16, v.view.vp[3][3] = 4, v.view.vp[3][2] = 1;
    v.s.Load(v.lamps, v.frame);
    v.Step();
    CHECK(v.s.InFlight(0x1003) && !v.s.InFlight(0x1000), "a lamp in view bakes before a nearer one out of view");
}

static void ToggleStorm() {
    Rig r(30);
    r.s.Load(r.lamps, r.frame);
    r.Settle();
    const uint64_t issued = r.Issued();
    const Box box = r.s.LayerBox(0x1005);
    CHECK(box.Tiles() == 50, "touch box of lamp 5: %lld tiles", box.Tiles());
    uint64_t tiles = 0;
    for (int i = 0; i < 600; i++) { // 600 switches, dims and recolours over 300 frames
        LampIn l = r.lamps[(i * 7) % 30];
        l.on = (i & 1) != 0;
        r.s.OnLamp(i % 3 ? Event::Switched : Event::Value, l, r.frame);
        if (i & 1) {
            r.Step();
            tiles += r.s.ComposeTiles().size();
        }
    }
    r.Settle();
    const Counters c = r.s.Stats();
    CHECK(r.Issued() == issued, "toggles bake nothing: %llu solves", r.Issued() - issued);
    CHECK(c.composeOnly == 600, "600 compose-only updates: %llu", c.composeOnly);
    // one switch composes exactly the lamp's box
    r.s.OnLamp(Event::Switched, r.lamps[5], r.frame);
    r.Step();
    CHECK(r.s.ComposeTiles().size() == 50, "a switch composes its layer's 50 tiles, got %zu", r.s.ComposeTiles().size());
    const auto* at = r.s.TileLamps(TileKey(0, 30, 2));
    CHECK(at && at->size() == 2, "tile (30, 2) sums lamps 5 and 6 (its box and the next room's)");
    std::printf("toggle storm: 600 events, 0 solves, %llu tiles composed\n", tiles);
}

static void Drag() {
    Rig r(10);
    r.s.Load(r.lamps, r.frame);
    r.Settle();
    const uint64_t issued = r.Issued(), landed = r.s.Stats().landed;
    LampIn l = r.lamps[3];
    const uint32_t start = r.frame;
    for (int i = 0; i < 40; i++) { // a 40-frame drag: a move every frame
        l.pos[0] += 0.1f;
        r.s.OnLamp(Event::Moved, l, r.frame);
        r.Step();
    }
    CHECK(r.Issued() == issued, "no bake while the lamp moves: %llu", r.Issued() - issued);
    const uint32_t stop = r.frame;
    while (r.Issued() == issued && r.frame < stop + 50) r.Step();
    CHECK(r.frame - stop == static_cast<uint32_t>(kStillFrames), "it bakes %d still frames after the drag: %u", kStillFrames, r.frame - stop);
    CHECK(r.s.Stats().issued[static_cast<int>(Cause::Moved)] == 1, "one moved bake");
    // moved again while its bake is in flight: cancelled, queued once
    l.pos[0] += 1;
    r.s.OnLamp(Event::Moved, l, r.frame);
    r.s.OnLamp(Event::Moved, l, r.frame);
    r.Step();
    CHECK(r.s.Stats().restarts == 1 && r.fake.cancels == 1, "one restart, one cancel: %llu %u", r.s.Stats().restarts, r.fake.cancels);
    r.Settle();
    const Counters c = r.s.Stats();
    CHECK(c.landed == landed + 1, "one layer landed for the drag: %llu", c.landed - landed);
    CHECK(c.latencyMax >= stop - start, "latency counts from the first move");
    // a lamp added waits for its still frames too, a removed one drops its layer and composes its tiles
    LampIn n{0x2000, {12.5f, 1.5f, 2.5f}, 0, true};
    r.s.OnLamp(Event::Added, n, r.frame);
    r.Step();
    CHECK(r.s.Queued(0x2000) && !r.s.InFlight(0x2000), "added: waiting");
    r.Settle();
    CHECK(r.s.HasLayer(0x2000) && r.s.Stats().issued[static_cast<int>(Cause::Added)] == 1, "added lamp baked once");
    r.s.OnLamp(Event::Removed, n, r.frame);
    CHECK(!r.s.HasLayer(0x2000) && r.s.ComposeTiles().size() == 50, "removed: layer dropped, 50 tiles composed (%zu)", r.s.ComposeTiles().size());
    CHECK(!r.s.TileLamps(TileKey(0, 21, 2)) || r.s.TileLamps(TileKey(0, 21, 2))->size() == 2, "removed lamp out of the tile lists");
}

static void WallBurst() {
    Rig r(20);
    r.s.Load(r.lamps, r.frame);
    r.Settle();
    const uint64_t issued = r.Issued();
    const Box wall{40, 0, 40, 4, 0, 0}; // a wall at x = 40: rooms 7 (box 35..44) and 8 (38..47); lamp 6's box is 30..39
    for (int i = 0; i < 120; i++) { // an edit every 6 frames (100 ms) for 2 s
        if (i % 6 == 0) r.s.OnBuildEdit(wall, r.ms);
        r.Step();
    }
    CHECK(r.Issued() == issued && r.s.Stats().sceneSubmits == 1, "nothing while edits go on (the load's scene only)");
    const uint32_t last = r.frame;
    while (r.s.Stats().sceneSubmits == 1 && r.frame < last + 100) r.Step();
    const double idleMs = (r.frame - last + 5) * kFrameMs; // (the last edit was 5 frames before `last`)
    CHECK(idleMs >= kBuildIdleMs && idleMs < kBuildIdleMs + 2 * kFrameMs, "scene sent %.0f ms after the last edit", idleMs);
    CHECK(r.fake.scenes == 2, "one scene submit for the load, one for the burst: %u", r.fake.scenes);
    CHECK(r.s.Stats().issued[static_cast<int>(Cause::Build)] == 2, "the scene, then the 2 lamps whose box holds the edit: %llu", r.s.Stats().issued[2]);
    r.Settle();
    // a build edit with no box: every lamp with a layer
    r.s.OnBuildEdit({}, r.ms);
    r.Step(40);
    r.Settle();
    CHECK(r.s.Stats().issued[static_cast<int>(Cause::Build)] == 22, "whole-lot edit bakes all 20: %llu", r.s.Stats().issued[2]);
}

static void Crossfade() {
    Rig r(2);
    r.s.Load(r.lamps, r.frame);
    while (!r.s.HasLayer(0x1000)) r.Step();
    const uint32_t landedAt = r.frame;
    CHECK(r.s.Blend(0x1000) < 0.1f, "a new layer starts at the old slot: %.2f", r.s.Blend(0x1000));
    float last = r.s.Blend(0x1000);
    int frames = 0;
    while (r.s.Blend(0x1000) < 1.0f && frames < 100) {
        r.Step();
        ++frames;
        CHECK(r.s.Blend(0x1000) >= last, "blend never goes back");
        last = r.s.Blend(0x1000);
        if (r.s.Blend(0x1000) < 1.0f) CHECK(!r.s.ComposeTiles().empty(), "tiles composed every frame of the fade");
    }
    const double fadeMs = frames * kFrameMs;
    CHECK(fadeMs >= kFadeMs - kFrameMs && fadeMs <= kFadeMs + kFrameMs, "fade %.0f ms", fadeMs);
    std::printf("crossfade: %d frames (%.0f ms) from frame %u\n", frames, fadeMs, landedAt);
    // a re-bake keeps the old layer shown until the new one lands, then fades over both boxes
    LampIn l = r.lamps[0];
    l.pos[0] += 3;
    r.s.OnLamp(Event::Moved, l, r.frame);
    r.Step();
    CHECK(r.s.HasLayer(0x1000) && r.s.LayerBox(0x1000).x0 == 0, "stale layer stays while it re-bakes");
    while (r.s.Blend(0x1000) >= 1.0f && r.frame < 10000) r.Step();
    CHECK(r.s.TileLamps(TileKey(0, 0, 0)) && r.s.TileLamps(TileKey(0, 12, 0)), "both slots in the tile lists while fading");
    r.Settle();
    CHECK(!r.s.TileLamps(TileKey(0, 0, 0)) && r.s.LayerBox(0x1000).x0 == 3, "old slot gone after the fade");
}

static void Starvation() {
    Budget b;
    b.issuePerFrame = 1, b.maxInFlight = 1;
    Rig r(12, b);
    r.fake.latency = 20;
    r.s.Load(r.lamps, r.frame);
    r.Settle();
    // the far lamp (11) is dirtied once; the 10 near ones are moved again and again
    r.s.OnBuildEdit(r.s.LayerBox(0x100B), r.ms);
    r.Step(40);
    uint32_t queuedAt = r.frame;
    bool landed = false;
    const uint64_t before = r.s.Stats().landed;
    for (int i = 0; i < 2000 && !landed; i++) {
        if (i % 3 == 0)
            for (int k = 0; k < 10; k++) {
                LampIn l = r.lamps[k];
                l.pos[0] += 0.01f * (i % 2);
                r.s.OnLamp(Event::Moved, l, r.frame);
            }
        r.Step();
        landed = !r.s.Queued(0x100B) && !r.s.InFlight(0x100B);
    }
    CHECK(landed, "the far lamp lands while near lamps keep re-baking");
    CHECK(r.frame - queuedAt <= kStarveFrames + 3 * 20 + 10, "within the starve bound: %u frames", r.frame - queuedAt);
    CHECK(r.s.Stats().landed > before, "landings");
    // a failed bake waits in the idle list and is retried once the queue drains
    Rig f(3);
    f.fake.fail.insert(0x1001);
    f.s.Load(f.lamps, f.frame);
    f.Settle();
    CHECK(f.s.Stats().failed == 1 && f.s.HasLayer(0x1001), "failed once, retried, landed");
}

int main() {
    LoadAndOrder();
    OffLast();
    ToggleStorm();
    Drag();
    WallBurst();
    Crossfade();
    Starvation();
    std::printf(g_fail ? "gi_sched_test: %d failures\n" : "gi_sched_test: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
