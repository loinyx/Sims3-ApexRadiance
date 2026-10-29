// Dragged-lamp terrain stamps, offline: features/terrain_stamps.cpp (the mod's own source, included below) against a
// simulated terrain. 4 x 4 chunks of 256 m; a local relight queues the chunks a rect touches (+1 m) and the game
// re-renders one queued chunk every few frames, baking every lamp whose rect overlaps the chunk and the bake does not
// exclude; a re-render that bakes the same lamps leaves the same map (no new version: no smoothed build by itself); the
// GPU smoothing builds a chunk when its map version or its stamp version changed, with the stamps ForChunk gives then.
// Every frame, every chunk a lamp's rect overlaps must show it at most once (bake or stamp), and a chunk under a lamp's
// current rect may miss it only for a few frames; once still, every lamp is baked where it stands and nothing is stamped.
// S1  a drag across a chunk border, released
// S2  a drag that pauses while the drag is engaged, then goes on
// S3  a slow drag (5 mm a frame): it still counts as a move
// S4  a lamp next to a border (rect 0.5 m short of the next chunk): same-map re-renders
// S5  deleted while dragged
// S6  the GPU smoothing off while dragged
// S7  the relight refused when the drag starts / when it ends (retried)
// S8  dragged again while it returns to the bake
// S9  two lamps dragged at once, a slow re-render queue
#include <windows.h>
static DWORD g_tick = 1000;
static DWORD FakeTick() { return g_tick; }
#define GetTickCount FakeTick
#include "features/terrain_stamps.cpp"
#undef GetTickCount
#include <array>
#include <cstdio>
#include <deque>
#include <map>
#include <set>

namespace {
constexpr int kN = 4;
constexpr DWORD kFrameMs = 16;
using Key = std::pair<int, int>;

struct Chunk {
    uint64_t gver = 1, rver = 0, sver = 0, builtG = 0, builtS = ~0ull;
    std::map<uintptr_t, std::array<float, 2>> baked; // lamps in its map and where
    std::vector<TerrainStamps::Lamp> shown;           // stamps in its smoothed map
};
struct FakeLamp {
    bool exists = true, listed = true;
    float x = 0, z = 0, range = 8;
};
std::map<Key, Chunk> W;
std::map<uintptr_t, FakeLamp> lamps;
std::set<uintptr_t> excluded;
std::deque<Key> queue;
int renderEvery = 1, frame = 0, refuse = 0; // refuse: the next n QueueLocal calls fail
bool gpu = true, engaged = false;
long queued = 0;

void RectOf(const FakeLamp& l, float r[4]) { r[0] = l.x - l.range, r[1] = l.z - l.range, r[2] = l.x + l.range, r[3] = l.z + l.range; }
bool Touch(const Key& k, const float r[4], float grow) {
    const float x0 = k.first - 128.0f, z0 = k.second - 128.0f;
    return r[0] - grow <= x0 + 256 && x0 <= r[2] + grow && r[1] - grow <= z0 + 256 && z0 <= r[3] + grow;
}
bool Overlaps(const Key& k, float x, float z, float range) {
    const float r[4] = {x - range, z - range, x + range, z + range};
    const float x0 = k.first - 128.0f, z0 = k.second - 128.0f;
    return r[0] < x0 + 256 && x0 < r[2] && r[1] < z0 + 256 && z0 < r[3]; // light reaches a texel (no margin)
}
} // namespace

namespace ApexLog {
void Write(Level, const std::string&, const std::source_location&) noexcept {}
} // namespace ApexLog
namespace LightmapSmooth {
bool Enabled() { return true; }
bool GpuActive() { return gpu; }
void BumpStamps(const float rect[4]) {
    for (auto& [k, c] : W)
        if (Touch(k, rect, 1)) c.sver++;
}
std::vector<std::pair<Key, uint64_t>> ChunkVersions(const float rect[4]) {
    std::vector<std::pair<Key, uint64_t>> out;
    for (const auto& [k, c] : W)
        if (Touch(k, rect, 1)) out.emplace_back(k, c.rver);
    return out;
}
} // namespace LightmapSmooth
namespace ChunkRelight {
bool LikelyAvailable() { return true; }
int QueueLocal(const std::vector<Lamp>& ls, std::string& why, std::string&, bool, std::vector<std::pair<int, int>>*) {
    if (refuse > 0) {
        refuse--;
        why = "refused by the test";
        return 0;
    }
    for (const Lamp& l : ls)
        for (int i = 0; i < l.rects; i++)
            for (const auto& [k, c] : W)
                if (Touch(k, l.rect[i], 1) && std::find(queue.begin(), queue.end(), k) == queue.end()) queue.push_back(k);
    return static_cast<int>(++queued);
}
} // namespace ChunkRelight
namespace DragLite {
bool Engaged() { return engaged; }
} // namespace DragLite
namespace NightTerrainBake {
void SetExcluded(const std::vector<uintptr_t>& lights) { excluded = {lights.begin(), lights.end()}; }
bool StampOf(uintptr_t light, float col[4], float pos[3], float rect[4], float& ground) {
    const auto it = lamps.find(light);
    if (it == lamps.end() || !it->second.exists) return false;
    col[0] = col[2] = 1, col[1] = static_cast<float>(light), col[3] = it->second.range * 0.2f; // (green: the lamp's id)
    pos[0] = it->second.x, pos[1] = 3, pos[2] = it->second.z;
    RectOf(it->second, rect);
    ground = 0;
    return true;
}
} // namespace NightTerrainBake

namespace {
int checks = 0, failures = 0;
void Check(bool ok, const std::string& text) {
    ++checks;
    if (!ok) ++failures;
    std::printf("%s%s\n", ok ? "  ok   " : "  FAIL ", text.c_str());
}

void Bake(Chunk& c, const Key& k) { // what the game's chunk bake takes now
    c.baked.clear();
    for (const auto& [L, l] : lamps)
        if (l.exists && !excluded.count(L) && Overlaps(k, l.x, l.z, l.range)) c.baked[L] = {l.x, l.z};
}
void Reset() {
    TerrainStamps::Clear();
    lamps.clear();
    queue.clear();
    W.clear();
    for (int i = 0; i < kN; i++)
        for (int j = 0; j < kN; j++) W[{128 + 256 * i, 128 + 256 * j}] = Chunk{};
    renderEvery = 1, refuse = 0, gpu = true, engaged = false;
}
void Place(uintptr_t L, float x, float z) { // placed and baked before the test (a full rebuild), and seen
    lamps[L].x = x, lamps[L].z = z;
    for (auto& [k, c] : W) Bake(c, k);
    std::vector<uintptr_t> all;
    for (const auto& [M, m] : lamps) all.push_back(M);
    TerrainStamps::Update(all);
}

struct Stats {
    long doubled = 0, maxGap = 0, frames = 0;
    double maxLag = 0;
    std::map<std::pair<Key, uintptr_t>, long> gapRun;
};
// One frame: the game's terrain update, the smoothed builds, the draw (checked), EndScene (TerrainStamps::Update)
void Frame(Stats& st) {
    g_tick += kFrameMs;
    if (++frame % renderEvery == 0 && !queue.empty()) {
        const Key k = queue.front();
        queue.pop_front();
        Chunk& c = W[k];
        const auto before = c.baked;
        Bake(c, k);
        c.rver++;
        if (c.baked != before) c.gver++;
    }
    for (auto& [k, c] : W)
        if (gpu && (c.builtG != c.gver || c.builtS != c.sver)) {
            c.shown.clear();
            TerrainStamps::ForChunk(k, c.rver, c.shown);
            c.builtG = c.gver, c.builtS = c.sver;
        }
    for (auto& [k, c] : W)
        for (const auto& [L, l] : lamps) {
            const bool baked = c.baked.count(L) && Overlaps(k, c.baked[L][0], c.baked[L][1], l.range);
            int stamped = 0;
            if (gpu)
                for (const auto& s : c.shown)
                    if (s.col[1] == static_cast<float>(L) && Overlaps(k, s.pos[0], s.pos[2], l.range)) {
                        stamped++;
                        st.maxLag = std::max(st.maxLag, static_cast<double>(std::fabs(l.x - s.pos[0]) + std::fabs(l.z - s.pos[2])));
                    }
            if (baked + stamped > 1) st.doubled++;
            long& run = st.gapRun[{k, L}];
            run = l.exists && Overlaps(k, l.x, l.z, l.range) && !baked && !stamped ? run + 1 : 0;
            st.maxGap = std::max(st.maxGap, run);
        }
    std::vector<uintptr_t> listed;
    for (const auto& [L, l] : lamps)
        if (l.exists && l.listed) listed.push_back(L);
    TerrainStamps::Update(listed);
    st.frames++;
}
void Run(Stats& st, int frames) {
    for (int i = 0; i < frames; i++) Frame(st);
}
void Drag(Stats& st, uintptr_t L, float dx, float dz, int frames) {
    for (int i = 0; i < frames; i++) {
        lamps[L].x += dx, lamps[L].z += dz;
        Frame(st);
    }
}
// Still for long: every lamp baked exactly where it stands, no stamp, no exclusion, no state
void Settled(const char* name, Stats& st) {
    Run(st, 400);
    bool exact = true;
    for (const auto& [k, c] : W) {
        for (const auto& [L, l] : lamps) {
            const bool want = l.exists && Overlaps(k, l.x, l.z, l.range);
            const auto it = c.baked.find(L);
            exact = exact && want == (it != c.baked.end()) && (!want || (it->second[0] == l.x && it->second[1] == l.z));
        }
        exact = exact && (!gpu || c.shown.empty());
    }
    Check(exact && excluded.empty() && TerrainStamps::g_states.empty(),
          std::format("{}: settled, every lamp baked where it stands, no stamp ({} states, {} excluded)", name, TerrainStamps::g_states.size(), excluded.size()));
    Check(st.doubled == 0, std::format("{}: never shown twice ({} chunk frames doubled)", name, st.doubled));
}
void Gap(const char* name, const Stats& st, long most) {
    Check(st.maxGap <= most, std::format("{}: a chunk under the lamp misses it for at most {} frames ({})", name, most, st.maxGap));
}
} // namespace

int main() {
    const uintptr_t A = 0x1000, B = 0x2000;
    std::printf("[S1] drag across a chunk border, released\n");
    {
        Reset();
        Place(A, 240, 300);
        Stats st;
        const long q0 = queued, e0 = TerrainStamps::g_entered;
        Drag(st, A, 0.1f, 0, 120); // 6 m/s for 2 s: 240 -> 252 m, across x = 256
        Check(TerrainStamps::g_entered - e0 == 1 && excluded.count(A), "S1: entered once, excluded from the bake while dragged");
        Check(st.maxLag <= 0.1f * (TerrainStamps::kBumpMs / kFrameMs + 2) + 1e-3, std::format("S1: the stamp lags the lamp by at most a bump interval ({:.2f} m)", st.maxLag));
        Gap("S1 dragged", st, 4);
        Settled("S1", st);
        Gap("S1 released", st, 4);
        Check(queued - q0 == 2, std::format("S1: two local relights (drag start, release): {}", queued - q0));
    }
    std::printf("[S2] pause while engaged\n");
    {
        Reset();
        Place(A, 300, 300);
        Stats st;
        engaged = true;
        Drag(st, A, 0.2f, 0, 30);
        Run(st, 200); // 3.2 s still, still engaged: stays drawn
        Check(excluded.count(A) == 1, "S2: still excluded during an engaged pause longer than kQuietMs");
        Drag(st, A, 0, 0.2f, 30);
        engaged = false;
        Settled("S2", st);
        Gap("S2", st, 4);
    }
    std::printf("[S3] slow drag\n");
    {
        Reset();
        Place(A, 300, 300);
        Stats st;
        Drag(st, A, 0.005f, 0, 600); // 5 mm a frame: 3 m in 9.6 s
        Check(TerrainStamps::g_states.count(A) == 1 && excluded.count(A), "S3: a slow drag enters and stays drawn on the GPU");
        Settled("S3", st);
        Gap("S3", st, 4);
    }
    std::printf("[S4] next to a chunk border (same-map re-renders)\n");
    {
        Reset();
        Place(A, 247.5f, 300); // rect to 255.5: chunk x 256.. is in the relight (+1 m) but not lit
        Stats st;
        Drag(st, A, 0.05f, 0, 10); // a small step away from the border: chunk 256.. re-rendered with the same map
        Drag(st, A, 0.5f, 0, 20);  // then across: chunk 256.. must take the stamp at once
        Settled("S4", st);
        Gap("S4", st, 4);
    }
    std::printf("[S5] deleted while dragged\n");
    {
        Reset();
        Place(A, 300, 300);
        Stats st;
        Drag(st, A, 0.2f, 0, 30);
        lamps[A].exists = false;
        Settled("S5", st);
    }
    std::printf("[S6] GPU smoothing off while dragged\n");
    {
        Reset();
        Place(A, 300, 300);
        Stats st;
        Drag(st, A, 0.2f, 0, 30);
        gpu = false;
        Drag(st, A, 0.2f, 0, 1);
        Check(excluded.empty(), "S6: smoothing off: back in the bake at once");
        Settled("S6", st);
    }
    std::printf("[S7] relight refused\n");
    {
        Reset();
        Place(A, 300, 300);
        Stats st;
        refuse = 1;
        Drag(st, A, 0.2f, 0, 1);
        Check(excluded.empty() && TerrainStamps::g_states.empty(), "S7: refused at the drag start: the lamp stays in the bake");
        Drag(st, A, 0.2f, 0, 30);
        Check(excluded.count(A) == 1, "S7: enters at its next move");
        refuse = 1;
        Run(st, 100); // 1.6 s still: the release is refused, tried again 1.5 s later
        Check(excluded.count(A) == 1, "S7: refused at the release: still drawn on the GPU");
        Settled("S7", st);
        Gap("S7", st, 4);
    }
    std::printf("[S8] dragged again while it returns\n");
    {
        Reset();
        Place(A, 300, 300);
        Stats st;
        renderEvery = 6; // a slow queue: the release's chunks wait
        Drag(st, A, 0.2f, 0, 30);
        Run(st, 96); // released (1.5 s)
        Drag(st, A, 0, 0.3f, 40);
        Check(excluded.count(A) == 1, "S8: entered again");
        Settled("S8", st);
        Gap("S8", st, 4);
    }
    std::printf("[S9] two lamps, slow queue\n");
    {
        Reset();
        Place(A, 250, 250);
        Place(B, 262, 262);
        Stats st;
        renderEvery = 4;
        for (int i = 0; i < 90; i++) {
            lamps[A].x += 0.15f, lamps[B].z -= 0.15f;
            Frame(st);
        }
        Check(excluded.size() == 2, "S9: both drawn on the GPU");
        Settled("S9", st);
        Gap("S9", st, 4);
    }
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
