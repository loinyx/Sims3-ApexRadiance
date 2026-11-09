#pragma once
// (physical indoor lamps) the kept shadow atlas's tiles: which lamp's shadow each tile holds (GpuRoomLamps::PhysPasses; offline:
// tools/rt_lamps_test). A tile is rendered again only when its lamp moved, the occluders changed or the tile went to another lamp
#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace PhysTiles {
constexpr int kPass = 16;   // lamps a pass draws: the passes, and so the sums, the same whatever the atlas's tiles
constexpr int kTiles = 120; // tiles at most (6 x 256 x 256 x (G16R16F + D24X8) = 3.1 MB a tile, 377 MB at 120; packed in columns
                            // by RtLampGpu::Engine::EnsureTiles; fewer past the device's texture size)

// Tiles to have: one per lamp drawn lately (seen: the lot's distinct lamps drawn while they exist), 4 at a time, never fewer than
// now. Fewer tiles than the lamps the maps draw in turn: Assign keeps the lamps needed soonest (see Atlas)
inline int Want(int have, int n, int seen, int cap = kTiles) { return std::max(have, std::min(cap, (std::max({n, seen, 1}) + 3) / 4 * 4)); }

struct Tile {
    uintptr_t light = 0;
    float pos[3] = {};
    uint32_t gen = 0, used = 0;
};
// When each lamp was drawn in its last two relight rounds: 64 slices of a round, a bit per slice drawn in
struct Hist {
    uint32_t round = 0;
    uint64_t prev = 0, cur = 0;
};
struct Atlas {
    std::array<Tile, kTiles> t{};
    std::unordered_map<uintptr_t, Hist> hist;
    uint32_t use = 0, round = 1, roundStart = 0, len = 0; // (len: the last round's passes)
    int rows = 0;
    // tiles rendered since the last Take, by why: its lamp moved, the occluders changed, a tile empty (a new lamp or a made-again
    // atlas), another lamp's tile taken
    enum Why { kMoved, kOccluders, kEmpty, kEvicted, kWhys };
    uint32_t why[kWhys] = {};
    // A relight round ends (GpuRoomLamps::BeginFrame; a frame with no pass is no round): lamps drawn in neither of the last two forgotten
    void NewRound() {
        if (use == roundStart) return;
        len = use - roundStart, roundStart = use, round++;
        std::erase_if(hist, [&](const auto& kv) { return kv.second.round + 1 < round; });
    }
    int Slice() const { return len ? static_cast<int>(std::min<uint64_t>(63, uint64_t(use - roundStart) * 64 / len)) : 0; }
    // How far ahead a lamp is drawn again, in slices: the next slice it was drawn in the last round, else its first one in the next
    // round; INT_MAX: drawn in neither of the last two rounds
    int Next(uintptr_t light) const {
        const auto it = hist.find(light);
        if (it == hist.end()) return INT_MAX;
        const Hist& h = it->second;
        const uint64_t prev = h.round == round ? h.prev : h.round + 1 == round ? h.cur : 0, cur = h.round == round ? h.cur : 0;
        if (!(prev | cur)) return INT_MAX;
        const int now = Slice();
        const uint64_t ahead = prev & ~((2ull << now) - 1);
        return ahead ? std::countr_zero(ahead) - now : 64 + std::countr_zero(prev | cur) - now;
    }
    // A pass's n lamps (n <= rows): a lamp's own tile when it holds its shadow now (same light, place and occluders), else a tile no
    // lamp of the pass holds, taken for it (render[i]: its shadow to render there; a failed render clears the tile). The tile taken:
    // the one that held the same lamp (moved, or the occluders changed), else an empty one, else the one whose lamp is drawn again
    // last as the last round drew them (least recently used among equals). More lamps than tiles: the excess is rendered again
    // about once a round instead of every lamp (least recently used over a cycle longer than the tiles missed at every draw)
    template <class Pos> void Assign(int n, const uintptr_t* lights, Pos pos, uint32_t gen, int* row, bool* render) {
        std::array<bool, kTiles> taken{};
        use++;
        for (int i = 0; i < n; i++) {
            row[i] = -1, render[i] = false;
            for (int r = 0; r < rows; r++)
                if (const Tile& g = t[r]; !taken[r] && g.light == lights[i] && g.gen == gen && std::memcmp(g.pos, pos(i), sizeof g.pos) == 0) {
                    row[i] = r, taken[r] = true;
                    break;
                }
        }
        for (int i = 0; i < n; i++) {
            if (row[i] >= 0) continue;
            int r = -1;
            for (int c = 0; c < rows && r < 0; c++)
                if (!taken[c] && t[c].light == lights[i]) r = c;
            for (int c = 0; c < rows && r < 0; c++)
                if (!taken[c] && !t[c].light) r = c;
            if (r < 0) {
                int best = -1;
                for (int c = 0; c < rows; c++) {
                    if (taken[c]) continue;
                    const int nx = Next(t[c].light);
                    if (r < 0 || nx > best || (nx == best && t[c].used < t[r].used)) best = nx, r = c;
                }
            }
            Tile& g = t[r];
            why[g.light != lights[i] ? (g.light ? kEvicted : kEmpty) : g.gen != gen ? kOccluders : kMoved]++;
            g = Tile{};
            g.light = lights[i], g.gen = gen;
            std::memcpy(g.pos, pos(i), sizeof g.pos);
            row[i] = r, taken[r] = true, render[i] = true;
        }
        const uint64_t bit = 1ull << Slice();
        for (int i = 0; i < n; i++) {
            t[row[i]].used = use;
            Hist& h = hist[lights[i]];
            if (h.round != round) h.prev = h.round + 1 == round ? h.cur : 0, h.cur = 0, h.round = round;
            h.cur |= bit;
        }
    }
};
} // namespace PhysTiles
