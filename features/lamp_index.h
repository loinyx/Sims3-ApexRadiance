#pragma once
// Lamp index cells (real-time GI S3-02 and S3-02b; docs/features/realtime-gi/wp/S3-02.md, S3-02b.md). Pure: no device,
// no game reads, so the offline check (tools/lamp_index_test) and the draw share it.
//
// The per-draw fix of outdoor rig-lit objects picks up to 8 lamps at the object and uploads 33 constant rows a draw. Here
// the same pick is made once per 2 m cell of a world-space window and kept in one float texture, with the lamps' rows in a
// table of the same texture; the patched shaders (ShaderPatches::PatchObjectLampPs, PatchInstancedLamps and
// PatchWindowOutdoor with index) find their cell from the pixel's world position and loop over the cell's entries.
//
// Texture, kWidth x kHeight, A32B32G32R32F, point sampled:
//   rows 0 .. kGrid-1, cell (cx, cz) at texels x = cx * kCellTexels .. + kCellTexels - 1, y = cz:
//     texels 0 .. kSlots-1  one lamp each: (index into the table, visibility, visibility for an object set in a wall, 0);
//                           index 0 is the black lamp, so an empty slot needs no branch
//     texel kSlots          (ground share, highest lamp head, ground share for an object set in a wall, 0)
//   rows kGrid .. kGrid+6, column = lamp index: (position, 1/R^2), (colour, spill), cone row 1, cone row 2: the four rows
//     the per-draw fix uploads for a lamp; (r0^2 of the realistic reach law, 1 for a lamp shining out of a house, 0, 0);
//     the two rows of the lamp's floor-tile shadow (LevelLightShare::LampFloorCap; zero: none)
// Frame constants, pixel shader c208 .. c223 (kFirstConst; c208 .. c214 used, the rest reserved): FrameConstants.
//
// The cells are built in slices (Start, Move, MarkDisc, Step): a change marks the cells it can reach, Step picks a few of
// them a frame, and the caller copies the finished state to the texture (Commit) when none is left. Until then the
// texture keeps the previous state; Stale tells which cells of it a change has made wrong.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace LampIndex {

inline constexpr int kSlots = 8;                 // lamp entries a cell: the per-draw pick's count (kObjectPixelLamps)
inline constexpr int kCellTexels = kSlots + 1;   // + the ground share texel
inline constexpr int kGrid = 64;                 // cells a side
inline constexpr float kCell = 2.0f;             // m
inline constexpr float kSnap = 16.0f;            // the window moves in steps of this, so a slow pan rebuilds rarely
inline constexpr int kTableRows = 7;
inline constexpr int kRowFloats = 4 * kTableRows;
inline constexpr int kMaxLamps = 255;            // table columns 1 .. 255; 0 = the black lamp
inline constexpr int kWidth = kGrid * kCellTexels, kHeight = kGrid + kTableRows;
inline constexpr unsigned kFirstConst = 208, kConstRows = 7, kReservedRows = 16;
inline constexpr float kNoTop = 1e9f;            // no lamp head near: the height gate stays 1
inline constexpr float kHalfDiagonal = 0.7072f * kCell;
static_assert(kMaxLamps < kWidth);

// What the per-draw pick answers at one point (LotLightBridge PickObjectLamps): the lamps' kTableRows rows with the colour
// not yet scaled by the visibility, the visibility of each, the ground share and the highest lamp head (kNoTop: none).
struct PointPick {
    int n = 0;
    float rows[kSlots][kRowFloats] = {};
    float vis[kSlots] = {};
    float share = 1.0f, top = kNoTop;
};

struct Atlas {
    std::vector<float> px; // kWidth * kHeight * 4
    float ox = 0.0f, oz = 0.0f; // world x, z of cell (0, 0)'s corner
    float y = 0.0f;             // the ground height the cells are picked at
    int lamps = 0;              // table columns in use (without the black lamp)
    int cellsLit = 0, cellOverflow = 0, tableOverflow = 0; // (Build) cells with a lamp; entries dropped: no slot left, no column left
    bool valid = false;         // started: px holds a window
    // the slices
    std::vector<uint8_t> dirty; // a cell to pick
    std::vector<uint8_t> stale; // picked again since the last commit for a change in the lamps or walls: the committed copy is wrong there
    int pending = 0, cursor = 0;
    long picked = 0;            // cells picked since the last commit
    bool fresh = true;          // started and not committed yet: every column in the table is in use
    bool full = false;          // the table ran out of columns during a partial rebuild: Start again
    std::map<std::array<uint32_t, kRowFloats>, int> columns;
};

inline float Snap(float v) { return std::floor(v / kSnap) * kSnap - 0.5f * kGrid * kCell; }
inline bool InWindow(const Atlas& a, float x, float z) {
    return a.valid && x >= a.ox && z >= a.oz && x < a.ox + kGrid * kCell && z < a.oz + kGrid * kCell;
}
// The cell of a world position, as the shader finds it (clamped to the window)
inline void CellOf(const Atlas& a, float x, float z, int& cx, int& cz) {
    const float inv = 1.0f / kCell;
    const float fx = std::floor(x * inv + -a.ox * inv), fz = std::floor(z * inv + -a.oz * inv);
    cx = static_cast<int>(std::fmin(std::fmax(fx, 0.0f), static_cast<float>(kGrid - 1)));
    cz = static_cast<int>(std::fmin(std::fmax(fz, 0.0f), static_cast<float>(kGrid - 1)));
}
inline float* Texel(Atlas& a, int x, int y) { return a.px.data() + (static_cast<size_t>(y) * kWidth + x) * 4; }
inline const float* Texel(const Atlas& a, int x, int y) { return a.px.data() + (static_cast<size_t>(y) * kWidth + x) * 4; }
inline const float* CellAt(const Atlas& a, float x, float z) {
    int cx, cz;
    CellOf(a, x, z, cx, cz);
    return Texel(a, cx * kCellTexels, cz);
}
// The committed copy is wrong at (x, z) (a change not yet picked again there), or the working window does not cover it
inline bool Stale(const Atlas& work, float x, float z) {
    if (!InWindow(work, x, z)) return true;
    int cx, cz;
    CellOf(work, x, z, cx, cz);
    return work.stale[static_cast<size_t>(cz) * kGrid + cx] != 0;
}

// A new window around (centreX, centreZ) at ground height y: every cell to pick, the table empty
inline void Start(Atlas& a, float centreX, float centreZ, float y) {
    a.px.assign(static_cast<size_t>(kWidth) * kHeight * 4, 0.0f);
    a.dirty.assign(static_cast<size_t>(kGrid) * kGrid, 1);
    a.stale.assign(static_cast<size_t>(kGrid) * kGrid, 1);
    a.pending = kGrid * kGrid, a.cursor = 0, a.picked = 0, a.fresh = true, a.full = false;
    a.ox = Snap(centreX), a.oz = Snap(centreZ), a.y = y;
    a.lamps = a.cellsLit = a.cellOverflow = a.tableOverflow = 0;
    a.columns.clear();
    // the black lamp: far away, no colour, no cone (the per-draw fix's unused slot)
    Texel(a, 0, kGrid)[0] = Texel(a, 0, kGrid)[2] = 1e6f;
    Texel(a, 0, kGrid + 1)[3] = Texel(a, 0, kGrid + 2)[3] = Texel(a, 0, kGrid + 3)[3] = 1.0f;
    a.valid = true;
}
// The window moved to (centreX, centreZ): the cells both windows share keep their entries, the new ones are picked
inline void Move(Atlas& a, float centreX, float centreZ) {
    const float nox = Snap(centreX), noz = Snap(centreZ);
    if (nox == a.ox && noz == a.oz) return;
    const int dx = static_cast<int>(std::lround((nox - a.ox) / kCell)), dz = static_cast<int>(std::lround((noz - a.oz) / kCell));
    std::vector<float> px(a.px.size(), 0.0f);
    std::memcpy(px.data() + static_cast<size_t>(kGrid) * kWidth * 4, Texel(a, 0, kGrid), static_cast<size_t>(kTableRows) * kWidth * 16); // the table
    std::vector<uint8_t> dirty(a.dirty.size(), 1), stale(a.stale.size(), 1);
    for (int cz = 0; cz < kGrid; cz++)
        for (int cx = 0; cx < kGrid; cx++) {
            const int sx = cx + dx, sz = cz + dz;
            if (sx < 0 || sz < 0 || sx >= kGrid || sz >= kGrid) continue;
            std::memcpy(px.data() + (static_cast<size_t>(cz) * kWidth + cx * kCellTexels) * 4, Texel(a, sx * kCellTexels, sz), kCellTexels * 16);
            dirty[static_cast<size_t>(cz) * kGrid + cx] = a.dirty[static_cast<size_t>(sz) * kGrid + sx];
            stale[static_cast<size_t>(cz) * kGrid + cx] = a.stale[static_cast<size_t>(sz) * kGrid + sx];
        }
    a.px.swap(px), a.dirty.swap(dirty), a.stale.swap(stale);
    a.ox = nox, a.oz = noz;
    a.pending = 0;
    for (uint8_t d : a.dirty) a.pending += d;
}
// Every cell a lamp at (x, z) can change: its centre within r (the pick's reach of that lamp) of the lamp
inline void MarkDisc(Atlas& a, float x, float z, float r) {
    const float reach = r + kHalfDiagonal;
    for (int cz = 0; cz < kGrid; cz++)
        for (int cx = 0; cx < kGrid; cx++) {
            const float ddx = a.ox + (cx + 0.5f) * kCell - x, ddz = a.oz + (cz + 0.5f) * kCell - z;
            if (ddx * ddx + ddz * ddz > reach * reach) continue;
            const size_t i = static_cast<size_t>(cz) * kGrid + cx;
            a.pending += a.dirty[i] ? 0 : 1;
            a.dirty[i] = a.stale[i] = 1;
        }
}

// Picks up to maxCells dirty cells. pick(x, y, z, inWall, PointPick&) is the per-draw pick at a point: it is asked at the
// cell's centre at height a.y, once as a loose object and once as an object set in a wall, so a cell holds what a draw at
// its centre would have uploaded, less the lamps that cannot reach the cell. A lamp only one of the two picks takes has
// visibility 0 for the other. Returns the cells picked.
template <typename PickFn> int Step(Atlas& a, PickFn&& pick, int maxCells) {
    int done = 0;
    while (a.pending > 0 && done < maxCells && !a.full) {
        const int i = a.cursor;
        a.cursor = (a.cursor + 1) % (kGrid * kGrid);
        if (!a.dirty[i]) continue;
        a.dirty[i] = 0, a.pending--, done++, a.picked++;
        const int cx = i % kGrid, cz = i / kGrid;
        const float x = a.ox + (cx + 0.5f) * kCell, z = a.oz + (cz + 0.5f) * kCell;
        PointPick loose, wall;
        pick(x, a.y, z, false, loose);
        pick(x, a.y, z, true, wall);
        float* cell = Texel(a, cx * kCellTexels, cz);
        std::memset(cell, 0, kCellTexels * 16);
        int n = 0;
        const auto column = [&](const float* rows) {
            std::array<uint32_t, kRowFloats> key;
            std::memcpy(key.data(), rows, sizeof key);
            const auto it = a.columns.find(key);
            if (it != a.columns.end()) return it->second;
            if (a.lamps >= kMaxLamps) return 0;
            const int c = ++a.lamps;
            for (int r = 0; r < kTableRows; r++) std::memcpy(Texel(a, c, kGrid + r), rows + 4 * r, 16);
            a.columns.emplace(key, c);
            return c;
        };
        const auto slotOf = [&](const float* rows) -> float* {
            // a lamp whose radius ends before the cell lights no pixel in it (both laws are 0 past R)
            const float dx = rows[0] - x, dz = rows[2] - z;
            if (rows[3] > 0.0f && std::sqrt(dx * dx + dz * dz) - 1.0f / std::sqrt(rows[3]) > kHalfDiagonal) return nullptr;
            const int c = column(rows);
            if (!c) {
                a.tableOverflow++;
                if (!a.fresh) a.full = true; // (a partial rebuild: the table holds columns nothing uses any more)
                return nullptr;
            }
            for (int s = 0; s < n; s++)
                if (static_cast<int>(cell[4 * s]) == c) return cell + 4 * s;
            if (n >= kSlots) {
                a.cellOverflow++;
                return nullptr;
            }
            cell[4 * n] = static_cast<float>(c);
            return cell + 4 * n++;
        };
        for (int k = 0; k < loose.n && k < kSlots; k++)
            if (float* s = slotOf(loose.rows[k])) s[1] = loose.vis[k];
        for (int k = 0; k < wall.n && k < kSlots; k++)
            if (float* s = slotOf(wall.rows[k])) s[2] = wall.vis[k];
        float* g = cell + 4 * kSlots;
        g[0] = loose.share, g[1] = loose.top, g[2] = wall.share;
    }
    return done;
}
// After the commit: nothing in the committed copy is stale any more
inline void Committed(Atlas& a) {
    std::fill(a.stale.begin(), a.stale.end(), uint8_t{0});
    a.picked = 0, a.fresh = false, a.cellsLit = 0;
    for (int cz = 0; cz < kGrid; cz++)
        for (int cx = 0; cx < kGrid; cx++) a.cellsLit += Texel(a, cx * kCellTexels, cz)[0] > 0.0f ? 1 : 0;
}
// The whole window at once (the offline check)
template <typename PickFn> void Build(Atlas& a, float centreX, float centreZ, float y, PickFn&& pick) {
    Start(a, centreX, centreZ, y);
    Step(a, pick, kGrid * kGrid);
    Committed(a);
}

// What changes without a rebuild
struct Frame {
    float lampStrength = 1.0f; // the per-draw fix's cS.y
    float gateStrength = 1.0f; // the same for a gate in a fence gap (ObjectRow gate)
    float groundStrength = 1.0f;
    float lotY = 0.0f;                          // the lot's ground height (the ground pool's height gate)
    float poolNone = 1.25f, poolFull = 1.0f;     // GroundShadow kFloorGateNone, kFloorGateFull
    float fadeAbove = 1.5f;                     // GroundLightGate kFadeAbove
    float groundUv[4] = {};                     // the ground light atlas: uv = xz * .xy + .zw
};
// c208 .. c214 (PatchObjectLampPs, index; the fence and window copies read +0, +1, +2 and +6):
//   +0 (1 / cell, grid - 1, -ox / cell, -oz / cell)   +1 (tu, tu / 2, v of table row 0, tv)   +2 (cell texels * tu, tv, tu / 2, tv / 2)
//   +3 (1, lamp strength, the same for a gate, 1e-4)   +4 ground atlas uv   +5 (ground strength, lot y, none / (none - full), 1 / (none - full))
//   +6 (1 / fade above, 3 tv, 2 tv, 0): the table row steps from row 1 to row 4 and from row 3 to row 5
inline void FrameConstants(const Atlas& a, const Frame& f, float c[kConstRows][4]) {
    const float tu = 1.0f / kWidth, tv = 1.0f / kHeight, inv = 1.0f / kCell, span = f.poolNone - f.poolFull;
    const float rows[kConstRows][4] = {{inv, static_cast<float>(kGrid - 1), -a.ox * inv, -a.oz * inv},
                                       {tu, 0.5f * tu, (kGrid + 0.5f) * tv, tv},
                                       {kCellTexels * tu, tv, 0.5f * tu, 0.5f * tv},
                                       {1.0f, f.lampStrength, f.gateStrength, 1e-4f},
                                       {f.groundUv[0], f.groundUv[1], f.groundUv[2], f.groundUv[3]},
                                       {f.groundStrength, f.lotY, f.poolNone / span, 1.0f / span},
                                       {1.0f / f.fadeAbove, 3.0f * tv, 2.0f * tv, 0.0f}};
    std::memcpy(c, rows, sizeof rows);
}
// The one row an object draw sets (ObjectLampPatch::objectConst): what stays per object
inline void ObjectRow(bool inWall, bool gate, float c[4]) {
    c[0] = inWall ? 1.0f : 0.0f, c[1] = gate ? 1.0f : 0.0f, c[2] = c[3] = 0.0f;
}

} // namespace LampIndex
