#pragma once
// Lamp index cells (real-time GI S3-02, lab prototype; docs/features/realtime-gi/wp/S3-02.md). Pure: no device, no game
// reads, so the offline check (tools/lamp_index_test) and the draw share it.
//
// The per-draw fix of outdoor rig-lit objects picks up to 8 lamps at the object and uploads 33 constant rows a draw. Here
// the same pick is made once per 2 m cell of a world-space window and kept in one float texture, with the lamps' rows in a
// table of the same texture; the patched shader (ShaderPatches::PatchObjectLampPs, index) finds its cell from the pixel's
// world position and loops over the cell's entries. A draw then sets one row (ObjectRow) and no lamp data.
//
// Texture, kWidth x kHeight, A32B32G32R32F, point sampled:
//   rows 0 .. kGrid-1, cell (cx, cz) at texels x = cx * kCellTexels .. + kCellTexels - 1, y = cz:
//     texels 0 .. kSlots-1  one lamp each: (index into the table, visibility, visibility for an object set in a wall, 0);
//                           index 0 is the black lamp, so an empty slot needs no branch
//     texel kSlots          (ground share, highest lamp head, ground share for an object set in a wall, 0)
//   rows kGrid .. kGrid+3, column = lamp index: (position, 1/R^2), (colour, spill), cone row 1, cone row 2: the four rows
//     the per-draw fix uploads for a lamp
// Frame constants, pixel shader c208 .. c223 (kFirstConst; c208 .. c214 used, the rest reserved): FrameConstants.
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
inline constexpr int kTableRows = 4;
inline constexpr int kMaxLamps = 255;            // table columns 1 .. 255; 0 = the black lamp
inline constexpr int kWidth = kGrid * kCellTexels, kHeight = kGrid + kTableRows;
inline constexpr unsigned kFirstConst = 208, kConstRows = 7, kReservedRows = 16;
inline constexpr float kNoTop = 1e9f;            // no lamp head near: the height gate stays 1
static_assert(kMaxLamps < kWidth);

// What the per-draw pick answers at one point (LotLightBridge PickObjectLamps): the lamps' four rows with the colour not
// yet scaled by the visibility, the visibility of each, the ground share and the highest lamp head (kNoTop: none).
struct PointPick {
    int n = 0;
    float rows[kSlots][16] = {};
    float vis[kSlots] = {};
    float share = 1.0f, top = kNoTop;
};

struct Atlas {
    std::vector<float> px; // kWidth * kHeight * 4
    float ox = 0.0f, oz = 0.0f; // world x, z of cell (0, 0)'s corner
    int lamps = 0;              // table columns in use (without the black lamp)
    int cellsLit = 0, cellOverflow = 0, tableOverflow = 0; // cells with a lamp; entries dropped: no slot left, no column left
    bool valid = false;
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

// Fills the window around (centreX, centreZ). pick(x, y, z, inWall, PointPick&) is the per-draw pick at a point: it is
// asked at every cell's centre at height y, once as a loose object and once as an object set in a wall, so a cell holds
// what a draw at its centre would have uploaded, less the lamps that cannot reach the cell. A lamp only one of the two
// picks takes has visibility 0 for the other.
template <typename PickFn> void Build(Atlas& a, float centreX, float centreZ, float y, PickFn&& pick) {
    a.px.assign(static_cast<size_t>(kWidth) * kHeight * 4, 0.0f);
    a.ox = Snap(centreX), a.oz = Snap(centreZ);
    a.lamps = a.cellsLit = a.cellOverflow = a.tableOverflow = 0;
    // the black lamp: far away, no colour, no cone (the per-draw fix's unused slot)
    Texel(a, 0, kGrid)[0] = Texel(a, 0, kGrid)[2] = 1e6f;
    Texel(a, 0, kGrid + 1)[3] = Texel(a, 0, kGrid + 2)[3] = Texel(a, 0, kGrid + 3)[3] = 1.0f;
    std::map<std::array<uint32_t, 16>, int> columns;
    const auto column = [&](const float* rows) {
        std::array<uint32_t, 16> key;
        std::memcpy(key.data(), rows, sizeof key);
        const auto it = columns.find(key);
        if (it != columns.end()) return it->second;
        if (a.lamps >= kMaxLamps) return 0;
        const int c = ++a.lamps;
        for (int r = 0; r < kTableRows; r++) std::memcpy(Texel(a, c, kGrid + r), rows + 4 * r, 16);
        columns.emplace(key, c);
        return c;
    };
    for (int cz = 0; cz < kGrid; cz++)
        for (int cx = 0; cx < kGrid; cx++) {
            const float x = a.ox + (cx + 0.5f) * kCell, z = a.oz + (cz + 0.5f) * kCell;
            PointPick loose, wall;
            pick(x, y, z, false, loose);
            pick(x, y, z, true, wall);
            float* cell = Texel(a, cx * kCellTexels, cz);
            int n = 0;
            const auto slotOf = [&](const float* rows) -> float* {
                // first law: a lamp whose radius ends before the cell lights no pixel in it (sat(1 - d^2 / R^2) = 0)
                const float dx = rows[0] - x, dz = rows[2] - z;
                if (rows[3] > 0.0f && std::sqrt(dx * dx + dz * dz) - 1.0f / std::sqrt(rows[3]) > 0.7072f * kCell) return nullptr;
                const int c = column(rows);
                if (!c) {
                    a.tableOverflow++;
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
            if (n) a.cellsLit++;
        }
    a.valid = true;
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
// c208 .. c214 (PatchObjectLampPs, index):
//   +0 (1 / cell, grid - 1, -ox / cell, -oz / cell)   +1 (tu, tu / 2, v of table row 0, tv)   +2 (cell texels * tu, tv, tu / 2, tv / 2)
//   +3 (1, lamp strength, the same for a gate, 1e-4)   +4 ground atlas uv   +5 (ground strength, lot y, none / (none - full), 1 / (none - full))
//   +6 (1 / fade above, 0, 0, 0)
inline void FrameConstants(const Atlas& a, const Frame& f, float c[kConstRows][4]) {
    const float tu = 1.0f / kWidth, tv = 1.0f / kHeight, inv = 1.0f / kCell, span = f.poolNone - f.poolFull;
    const float rows[kConstRows][4] = {{inv, static_cast<float>(kGrid - 1), -a.ox * inv, -a.oz * inv},
                                       {tu, 0.5f * tu, (kGrid + 0.5f) * tv, tv},
                                       {kCellTexels * tu, tv, 0.5f * tu, 0.5f * tv},
                                       {1.0f, f.lampStrength, f.gateStrength, 1e-4f},
                                       {f.groundUv[0], f.groundUv[1], f.groundUv[2], f.groundUv[3]},
                                       {f.groundStrength, f.lotY, f.poolNone / span, 1.0f / span},
                                       {1.0f / f.fadeAbove, 0.0f, 0.0f, 0.0f}};
    std::memcpy(c, rows, sizeof rows);
}
// The one row a draw sets (ObjectLampPatch::objectConst): what stays per object
inline void ObjectRow(bool inWall, bool gate, float c[4]) {
    c[0] = inWall ? 1.0f : 0.0f, c[1] = gate ? 1.0f : 0.0f, c[2] = c[3] = 0.0f;
}

} // namespace LampIndex
