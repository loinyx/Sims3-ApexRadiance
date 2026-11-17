#pragma once
// Real-time GI update scheduler, client side (work package S2-01; docs/features/realtime-gi/wp/X-REBAKE.md "Scheduler",
// design.md 3.3). Pure logic, no game reads and no device: realtime_gi.cpp feeds it the lamp snapshot's change events
// (S1-02), build edits and the camera, and it decides which (lamp) layers re-bake, in what order and under what budget,
// and which tiles the compose redraws. tools/gi_sched_test drives it with event sequences and a fake back end.
//
// Rules (X-REBAKE):
//   - switch, dim, recolour: no solve; the tiles the lamp's layer touches are composed again (compose only);
//   - moved or added: the layer re-bakes once the lamp stood still for kStillFrames frames;
//   - removed: the layer is dropped, its tiles composed again;
//   - build edit (walls, doors, windows, floors): once edits stopped for kBuildIdleMs, the scene is sent again, then every
//     lamp whose touch box holds the edit re-bakes; nothing is issued while edits go on;
//   - lot load: every lamp, camera-near first.
// Priority: starved entries (waited kStarveFrames) first, then lamps on before lamps off, in view, nearer the camera, by
// cause (moved, added, build, load), then age. At most kQueueCap entries; the rest wait in an idle list that refills the
// queue once it is empty. A lamp dirtied while its bake is in flight cancels it and is queued once (a restart), not once
// an edit. A landed layer goes to a second slot and crossfades in over kFadeMs; the old one stays shown until then.
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GiSched {

inline constexpr int kStillFrames = 2;
inline constexpr double kBuildIdleMs = 500.0;
inline constexpr double kFadeMs = 200.0;
inline constexpr int kQueueCap = 64;
inline constexpr uint32_t kStarveFrames = 180; // about 3 s at 60 fps: an entry this old goes first

// Tiles (1 m, world x and z) on stories s0..s1, inclusive. Empty when any range is reversed.
struct Box {
    int x0 = 1, z0 = 1, x1 = 0, z1 = 0, s0 = 1, s1 = 0;
    bool Empty() const { return x1 < x0 || z1 < z0 || s1 < s0; }
    bool Overlaps(const Box& b) const {
        return !Empty() && !b.Empty() && x0 <= b.x1 && b.x0 <= x1 && z0 <= b.z1 && b.z0 <= z1 && s0 <= b.s1 && b.s0 <= s1;
    }
    Box Union(const Box& b) const;
    long long Tiles() const { return Empty() ? 0 : (long long)(x1 - x0 + 1) * (z1 - z0 + 1) * (s1 - s0 + 1); }
};
// The box of a point grown by r metres on one story
Box Around(const float pos[3], int story, float r);
inline uint64_t TileKey(int story, int x, int z) {
    return (uint64_t)(uint8_t)(story + 8) << 48 | (uint64_t)(uint32_t)(x + 0x800000) << 24 | (uint32_t)(z + 0x800000);
}

// S1-02's change kinds, same order as RealtimeGI::LampChange
enum class Event : uint8_t { Added, Removed, Switched, Moved, Value };
enum class Cause : uint8_t { Moved, Added, Build, Load, Count }; // priority order; also the counters' order
inline constexpr const char* kCauseNames[] = {"moved", "added", "build", "load"};

struct LampIn {
    uintptr_t id = 0;
    float pos[3] = {};
    int story = 0;
    bool on = false;
};

// The solve back end: the bridge server's GI export (RemixBridge_GiSubmit / GiSolve / GiPoll / GiCancel, R-03 / R-04) or
// the fake one below. Solve and Poll run on the caller's thread and never wait.
struct Backend {
    virtual ~Backend() = default;
    virtual void BeginFrame(uint32_t /*frame*/) {}
    virtual bool SubmitScene() = 0;                         // the geometry changed (server: acceleration structure + links)
    virtual bool Solve(const LampIn& lamp, uint32_t& ticket) = 0; // false: busy, try a later frame
    virtual int Poll(uint32_t ticket, Box& touched) = 0;   // 0 pending, 1 landed (touched: texels over 0.5/255), -1 failed
    virtual void Cancel(uint32_t ticket) = 0;
};

// Lands every solve `latency` frames after it was issued with the box boxOf gives (a lamp's reach in the game, a set box in
// the tests); fails the ids in `fail` once
struct FakeBackend : Backend {
    int latency = 3;
    std::function<Box(const LampIn&)> boxOf;
    std::unordered_set<uintptr_t> fail;
    uint32_t frame = 0, next = 1, scenes = 0, solves = 0, cancels = 0;
    struct Job { LampIn lamp; uint32_t ready; };
    std::unordered_map<uint32_t, Job> jobs;
    void BeginFrame(uint32_t f) override { frame = f; }
    bool SubmitScene() override { ++scenes; return true; }
    bool Solve(const LampIn& lamp, uint32_t& ticket) override {
        ticket = next++;
        jobs[ticket] = {lamp, frame + static_cast<uint32_t>(latency)};
        ++solves;
        return true;
    }
    int Poll(uint32_t ticket, Box& touched) override {
        const auto it = jobs.find(ticket);
        if (it == jobs.end()) return -1;
        if (frame < it->second.ready) return 0;
        const LampIn l = it->second.lamp;
        jobs.erase(it);
        if (fail.erase(l.id)) return -1;
        touched = boxOf ? boxOf(l) : Around(l.pos, l.story, 4.0f);
        return 1;
    }
    void Cancel(uint32_t ticket) override { cancels += static_cast<uint32_t>(jobs.erase(ticket)); }
};

struct Budget {
    int issuePerFrame = 2; // solves started a frame (d3d9: 2 a frame, X-REBAKE; the server slices on its own device)
    int maxInFlight = 8;   // tickets outstanding (the bridge has 64)
};

struct View {
    bool known = false;
    float eye[3] = {};
    bool hasVp = false;
    float vp[4][4] = {}; // world -> clip, row vectors (PostScene::CameraViewProj)
};

struct Counters {
    uint64_t issued[static_cast<int>(Cause::Count)] = {};
    uint64_t landed = 0, failed = 0, composeOnly = 0, tilesComposed = 0, restarts = 0, dropped = 0, sceneSubmits = 0, removed = 0;
    uint64_t overBudget = 0; // must be 0
    int queued = 0, inFlight = 0, idle = 0, fading = 0, maxInFlight = 0;
    uint32_t latencyP50 = 0, latencyMax = 0; // frames from dirty to landed, the last 256 landings
};

class Scheduler {
  public:
    explicit Scheduler(Backend* backend = nullptr, Budget budget = {}) : backend_(backend), budget_(budget) {}
    void SetBackend(Backend* b) { backend_ = b; }
    // A new lot (or the first in a world): every layer dropped, the scene sent, every lamp queued as a load
    void Load(const std::vector<LampIn>& lamps, uint32_t frame);
    // One change of the snapshot. pos / story / on: the lamp now (ignored for Removed)
    void OnLamp(Event e, const LampIn& lamp, uint32_t frame);
    // Walls, doors, windows or floors changed in `box` (an empty box: the whole lot)
    void OnBuildEdit(const Box& box, double nowMs);
    // Once a frame: queue the still lamps, poll, issue under the budget, step the crossfades; ComposeTiles() then holds
    // this frame's tiles to compose again (with those events added since the last Tick)
    void Tick(uint32_t frame, double nowMs, const View& view);
    void Clear();

    const std::vector<uint64_t>& ComposeTiles() const { return compose_; }
    // The lamps whose layers a tile's compose sums (the crossfading ones with both slots)
    const std::vector<uintptr_t>* TileLamps(uint64_t tile) const;
    float Blend(uintptr_t id) const;   // the new slot's weight while a layer crossfades, 1 otherwise
    bool HasLayer(uintptr_t id) const; // a layer landed for it (else: direct light only)
    Box LayerBox(uintptr_t id) const;
    bool Queued(uintptr_t id) const;
    bool InFlight(uintptr_t id) const;
    Counters Stats() const;

  private:
    enum class Phase : uint8_t { Idle, WaitStill, Queued, InFlight, Parked, Restart }; // Restart: its bake was cancelled
    struct Lamp {
        LampIn in;
        Phase phase = Phase::Idle;
        Cause cause = Cause::Load;
        uint32_t dirtyFrame = 0, queuedFrame = 0, movedFrame = 0, ticket = 0;
        bool hasLayer = false;
        Box box;        // the shown layer's touch box
        Box fadeBox;    // while crossfading: the new layer's box
        double fadeT = -1; // 0..1 while crossfading
    };
    void Dirty(Lamp& l, Cause c, uint32_t frame);
    void Enqueue(Lamp& l, Cause c, uint32_t frame);
    void ComposeBox(const Box& b);
    void IndexBox(uintptr_t id, const Box& b, bool add);
    void Drop(uintptr_t id);
    static Box Shown(const Lamp& l); // the box its shown slots cover
    void EndFade(uintptr_t id, Lamp& l);

    Backend* backend_;
    Budget budget_;
    std::unordered_map<uintptr_t, Lamp> lamps_;
    std::unordered_map<uint64_t, std::vector<uintptr_t>> tiles_;
    std::vector<uint64_t> compose_;
    std::unordered_set<uint64_t> composeSet_;
    size_t published_ = 0; // compose_ entries the last Tick published
    bool buildPending_ = false;
    Box buildBox_;
    bool buildAll_ = false;
    double buildMs_ = 0, lastMs_ = 0;
    std::vector<uint32_t> latencies_;
    size_t latencyNext_ = 0;
    Counters c_;
};

} // namespace GiSched
