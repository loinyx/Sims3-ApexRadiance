#pragma once
// Real-time GI, the game's lighting reading the irradiance atlas (work package S2-05). Pure functions, no device and no game
// reads: realtime_gi.cpp builds the grid from the scene and fills it with GiCompose::Scatter, lot_light_bridge.cpp binds it
// in the room-map copies of ShaderPatches::PatchRoomMapGi, tools/gi_indirect_test checks both against SampleCpu.
//
// The atlas keeps a texel at whatever pixel the scene's allocator gave it (GiScene::Built), so a shader cannot find a texel
// from its place. The grid is the atlas's floor and ceiling texels laid out by place: one layer a story and kind (floor,
// ceiling) of the lot's tiles at the scene's density (texel (gx, gz) of a layer = the texel whose lot-space cell is
// floor(x d), floor(z d)), layers one above the other with one empty row between them so a bilinear read never reaches the
// next layer. A grid texel holds (irradiance rgb, ready): ready 1 where every lamp that may light the texel's tile has its
// layer, 0 where it still waits (the copy then keeps the game's ambient) and 0 in texels no texel fills.
// The patched floor shader reads it bilinearly at the pixel's lot position (the floor vertex shader's TEXCOORD4: lot / 64 +
// (1/1024, 1/512), the same input the arch copies read), so the light is continuous between texels.
// Walls (S2-06): no lot position reaches their pixel shader, so a vertex shader copy computes the grid uv of a wall region
// (WallUvCpu) and hands it over in TEXCOORD7. Diagonal surfaces have no consumer yet.
#include "gi_scene.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace GiIndirect {

enum Kind : int { kFloor = 0, kCeiling = 1, kWall = 3 }; // (kWall: the wall region, one for every story)

// A room's texture set (lod + class x 64 + 0xC) holds four map wrappers by index: 0 floor, 1 ceiling, 2 objects, 3 wall
// (RtOutdoorLamps::kSetIdx, OutdoorDelta::kSlotIdx). The kind a set index feeds, -1 none (objects).
inline int KindOfSetIndex(int idx) { return idx == 0 ? kFloor : idx == 1 ? kCeiling : idx == 3 ? kWall : -1; }

struct Point {
    uint32_t grid = 0;  // pixel of the grid (y * w + x)
    uint32_t atlas = 0; // pixel of the atlas
    uint64_t tile = 0;  // GiSched::TileKey of the texel (world tile, its story): the ready mask is per tile
};
struct Grid {
    int d = 2;                                // texels a metre (the scene's density)
    uint32_t w = 0, h = 0, layerRows = 0;     // layerRows: the lot's z cells (each layer, then one empty row)
    std::map<std::pair<int, int>, uint32_t> rows; // (story, kind) -> the layer's first row
    std::vector<Point> points;
    // walls (S2-06): a region right of the layers from column wallX0, 5 blocks of wallA + 1 columns (+x, +z, -x, -z facing,
    // block 4 empty: caps and diagonals), each a stack of slices (wall line li = round(line coord)) of wallY + 1 rows from
    // height wallYO. wallA = 0: no walls
    uint32_t wallX0 = 0, wallA = 0, wallY = 0;
    float wallYO = 0.0f;
    // (S2-06) per grid pixel, 4 bytes as the flags texture (A8R8G8B8: b, g, r, a in memory): r, g, b, a = 255 where the read may
    // blend toward -u, +u, -v, +v (OpenTo). A pixel no texel fills: all 0
    std::vector<uint8_t> flags;
    bool Empty() const { return points.empty(); }
};

// A texel key's lot-space place (1/64 m) and facing code (8 up, 9 down, 0..7 wall directions): GiScene::TexelKey
inline void DecodeKey(uint64_t key, float p[3], int& code) {
    for (int k = 0; k < 3; k++) p[k] = static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(key >> (48 - 16 * k)))) / 64.0f;
    code = static_cast<int>(key & 0xFFFFu);
}

// The grid of a built scene (density d, GiScene::Options::density) of a lot lotW x lotH tiles (its stories' largest grid): a
// layer spans the whole lot, so a floor pixel anywhere on it reads its own layer (outside the rooms: 0, not ready). Lots up to
// 4096 / d metres a side.
inline void BuildFlags(const GiScene::Built& b, const RtLampProxy::Lot* lot, Grid& g);
inline bool Build(const GiScene::Built& b, int d, int lotW, int lotH, Grid& out, const RtLampProxy::Lot* lot = nullptr) {
    out = Grid{};
    out.d = std::max(1, d);
    struct Cell {
        int story, kind, gx, gz;
        uint32_t atlas;
        uint64_t tile;
    };
    std::vector<Cell> cells;
    struct WallCell {
        int block, li, ga;
        float y;
        uint32_t atlas;
        uint64_t tile;
    };
    std::vector<WallCell> walls;
    int maxX = 0, maxZ = 0, maxLi = 0;
    float minY = 1e30f;
    for (const auto& [key, slot] : b.slots) {
        float p[3];
        int code;
        DecodeKey(key, p, code);
        if (code < 8 && !(code & 1)) { // an axis-aligned wall texel (diagonals: no consumer)
            const bool xFacing = code == 0 || code == 4;
            const int li = static_cast<int>(std::floor((xFacing ? p[0] : p[2]) + 0.5f)), ga = static_cast<int>(std::floor((xFacing ? p[2] : p[0]) * out.d));
            if (li < 0 || ga < 0) continue;
            maxLi = std::max(maxLi, li), minY = std::min(minY, p[1]);
            walls.push_back({code / 2, li, ga, p[1], slot.pixel,
                             GiSched::TileKey(slot.story, static_cast<int>(std::floor(slot.world[0])), static_cast<int>(std::floor(slot.world[2])))});
            continue;
        }
        if (code != 8 && code != 9) continue;
        const int gx = static_cast<int>(std::floor(p[0] * out.d)), gz = static_cast<int>(std::floor(p[2] * out.d));
        if (gx < 0 || gz < 0) continue;
        maxX = std::max(maxX, gx), maxZ = std::max(maxZ, gz);
        const uint64_t tile = GiSched::TileKey(slot.story, static_cast<int>(std::floor(slot.world[0])), static_cast<int>(std::floor(slot.world[2])));
        cells.push_back({slot.story, code == 8 ? kFloor : kCeiling, gx, gz, slot.pixel, tile});
        out.rows[{slot.story, code == 8 ? kFloor : kCeiling}] = 0;
    }
    if (cells.empty()) return false;
    out.w = static_cast<uint32_t>(std::max(maxX + 1, lotW * out.d)), out.layerRows = static_cast<uint32_t>(std::max(maxZ + 1, lotH * out.d));
    uint32_t row = 0;
    for (auto& [k, r] : out.rows) r = row, row += out.layerRows + 1;
    out.h = row;
    // walls: rows gy = floor((y - yO) d), yO half a texel under the lowest wall texel (lines whose base x d is not whole
    // read off-centre); one empty column after the layers and after each block, one empty row after each slice
    std::vector<WallCell> kept;
    if (!walls.empty()) {
        out.wallX0 = out.w + 1, out.wallA = static_cast<uint32_t>(std::max(lotW, lotH) * out.d), out.wallYO = minY - 0.5f / out.d;
        for (const WallCell& c : walls)
            if (c.ga < static_cast<int>(out.wallA)) kept.push_back(c), out.wallY = std::max(out.wallY, static_cast<uint32_t>(std::floor((c.y - out.wallYO) * out.d)) + 1);
        out.w = out.wallX0 + 5 * (out.wallA + 1), out.h = std::max(out.h, static_cast<uint32_t>(maxLi + 1) * (out.wallY + 1));
    }
    if (out.w > 4096 || out.h > 4096) return out = Grid{}, false;
    out.points.reserve(cells.size() + kept.size());
    for (const Cell& c : cells) out.points.push_back({(out.rows[{c.story, c.kind}] + static_cast<uint32_t>(c.gz)) * out.w + static_cast<uint32_t>(c.gx), c.atlas, c.tile});
    for (const WallCell& c : kept) {
        const uint32_t x = out.wallX0 + static_cast<uint32_t>(c.block) * (out.wallA + 1) + static_cast<uint32_t>(c.ga),
                       y = static_cast<uint32_t>(c.li) * (out.wallY + 1) + static_cast<uint32_t>(std::floor((c.y - out.wallYO) * out.d));
        out.points.push_back({y * out.w + x, c.atlas, c.tile});
    }
    BuildFlags(b, lot, out);
    return true;
}

// Segment p-q (lot x, z) properly crosses edge e (an end of e on the segment counts; collinear does not)
inline bool Crosses(const float p[2], const float q[2], const RtLampProxy::Edge& e) {
    const auto o = [](float ax, float az, float bx, float bz, float cx, float cz) { return (bx - ax) * (cz - az) - (bz - az) * (cx - ax); };
    const float d1 = o(e.x0, e.z0, e.x1, e.z1, p[0], p[1]), d2 = o(e.x0, e.z0, e.x1, e.z1, q[0], q[1]);
    const float d3 = o(p[0], p[1], q[0], q[1], e.x0, e.z0), d4 = o(p[0], p[1], q[0], q[1], e.x1, e.z1);
    return d1 * d2 < 0 && d3 * d4 <= 0;
}
// The bleed flags (S2-06): a grid pixel's read may blend toward its neighbour (u: the next pixel of the row, v: of the column)
// when both hold a texel of the same story and the segment between their places crosses no wall edge of that story (a wall
// between two rooms' floors, a perpendicular wall at a junction along a wall, the stories along a wall line). shortcut: the
// diagonal texel of a bilinear read is not checked (an L corner can still mix a little); every edge blocks, open ones too.
inline void BuildFlags(const GiScene::Built& b, const RtLampProxy::Lot* lot, Grid& g) {
    const size_t n = static_cast<size_t>(g.w) * g.h;
    g.flags.assign(n * 4, 0);
    struct Cell {
        int story = -1;
        float x = 0, z = 0;
    };
    std::vector<Cell> cell(n);
    {
        std::unordered_map<uint32_t, std::pair<uint64_t, int>> byAtlas; // atlas pixel -> (texel key, story)
        for (const auto& [key, slot] : b.slots) byAtlas[slot.pixel] = {key, slot.story};
        for (const Point& p : g.points) {
            const auto& [key, story] = byAtlas[p.atlas];
            float q[3];
            int code;
            DecodeKey(key, q, code);
            cell[p.grid] = {story, q[0], q[2]};
        }
    }
    // the stories' edges by tile (each tile an edge's bounding box touches)
    std::map<std::tuple<int, int, int>, std::vector<const RtLampProxy::Edge*>> byTile;
    if (lot)
        for (const RtLampProxy::Story& st : lot->stories)
            for (const RtLampProxy::Edge& e : st.edges)
                for (int z = static_cast<int>(std::floor(std::min(e.z0, e.z1) - 0.01f)); z <= static_cast<int>(std::floor(std::max(e.z0, e.z1) + 0.01f)); z++)
                    for (int x = static_cast<int>(std::floor(std::min(e.x0, e.x1) - 0.01f)); x <= static_cast<int>(std::floor(std::max(e.x0, e.x1) + 0.01f)); x++)
                        byTile[{st.story, x, z}].push_back(&e);
    const auto walled = [&](const Cell& a, const Cell& c) {
        const float p[2] = {a.x, a.z}, q[2] = {c.x, c.z};
        for (const Cell* t : {&a, &c}) {
            const auto it = byTile.find({a.story, static_cast<int>(std::floor(t->x)), static_cast<int>(std::floor(t->z))});
            if (it == byTile.end()) continue;
            for (const RtLampProxy::Edge* e : it->second)
                if (Crosses(p, q, *e)) return true;
        }
        return false;
    };
    const int byte[4] = {2, 1, 0, 3}; // -u r, +u g, -v b, +v a (memory order b, g, r, a)
    for (uint32_t y = 0; y < g.h; y++)
        for (uint32_t x = 0; x < g.w; x++) {
            const size_t i = static_cast<size_t>(y) * g.w + x;
            if (cell[i].story < 0) continue;
            const bool has[4] = {x > 0, x + 1 < g.w, y > 0, y + 1 < g.h};
            const size_t nb[4] = {i - 1, i + 1, i - g.w, i + g.w};
            for (int k = 0; k < 4; k++)
                if (has[k] && cell[nb[k]].story == cell[i].story && !walled(cell[i], cell[nb[k]])) g.flags[i * 4 + byte[k]] = 255;
        }
}
// The flags of grid pixel (x, y) as (-u, +u, -v, +v) open (0 / 1); all 0 outside the grid or without flags
inline void OpenTo(const Grid& g, int x, int y, float open[4]) {
    open[0] = open[1] = open[2] = open[3] = 0.0f;
    if (g.flags.empty() || x < 0 || y < 0 || x >= static_cast<int>(g.w) || y >= static_cast<int>(g.h)) return;
    const uint8_t* f = &g.flags[(static_cast<size_t>(y) * g.w + x) * 4];
    open[0] = f[2] / 255.0f, open[1] = f[1] / 255.0f, open[2] = f[0] / 255.0f, open[3] = f[3] / 255.0f;
}

// The patched shader's uv row (PatchRoomMapGi uvConst) for a draw of that story and kind: uv = TEXCOORD4.xy x c.xy + c.zw,
// where TEXCOORD4 = lot / 64 + (1/1024, 1/512), so lot = 64 t - (1/16, 1/8) and the grid uv = (lot x d / w, (lot z d + row) / h).
// False when the grid has no such layer.
inline bool UvRow(const Grid& g, int story, int kind, float c[4]) {
    const auto it = g.rows.find({story, kind});
    if (it == g.rows.end() || !g.w || !g.h) return false;
    const float d = static_cast<float>(g.d), w = static_cast<float>(g.w), h = static_cast<float>(g.h);
    c[0] = 64.0f * d / w, c[1] = 64.0f * d / h, c[2] = -d / 16.0f / w, c[3] = (-d / 8.0f + static_cast<float>(it->second)) / h;
    return true;
}

// The grid texture as Scatter leaves it (rgba, w x h): each point's atlas rgb x its ready weight, and that weight
inline void FillCpu(const Grid& g, const std::vector<float>& atlas, const std::vector<float>& ready, std::vector<float>& grid) {
    grid.assign(static_cast<size_t>(g.w) * g.h * 4, 0.0f);
    for (size_t i = 0; i < g.points.size(); i++) {
        const Point& p = g.points[i];
        const float a = i < ready.size() ? ready[i] : 0.0f;
        for (int c = 0; c < 3; c++) grid[static_cast<size_t>(p.grid) * 4 + c] = atlas[static_cast<size_t>(p.atlas) * 4 + c] * a;
        grid[static_cast<size_t>(p.grid) * 4 + 3] = a;
    }
}

inline bool SampleUv(const Grid& g, const std::vector<float>& grid, float u, float v, float out[4]);
// What the patched shader reads at a lot position (x, z) of that story and kind: the grid's bilinear read (clamped) at
// UvRow's uv. False: no such layer
inline bool SampleCpu(const Grid& g, const std::vector<float>& grid, int story, int kind, float lotX, float lotZ, float out[4]) {
    float c[4];
    if (!UvRow(g, story, kind, c)) return false;
    const float tx = (lotX + 1.0f / 16.0f) / 64.0f, tz = (lotZ + 1.0f / 8.0f) / 64.0f; // (the vertex shader's packing)
    return SampleUv(g, grid, tx * c[0] + c[2], tz * c[1] + c[3], out);
}

// The wall vertex shader copy's rows (S2-06): r0 = (d / W, d / H, (A + 1) / W, (Y + 1) / H), r1 = (x0 / W, -yO d / H, 0, 0).
// False: no walls
inline bool WallRows(const Grid& g, float r0[4], float r1[4]) {
    if (!g.wallA || !g.w || !g.h) return false;
    const float d = static_cast<float>(g.d), w = static_cast<float>(g.w), h = static_cast<float>(g.h);
    r0[0] = d / w, r0[1] = d / h, r0[2] = static_cast<float>(g.wallA + 1) / w, r0[3] = static_cast<float>(g.wallY + 1) / h;
    r1[0] = static_cast<float>(g.wallX0) / w, r1[1] = -g.wallYO * d / h, r1[2] = r1[3] = 0.0f;
    return true;
}
// The uv the wall vertex shader copy gives a wall point (lot position p, lot normal n), as it computes it: the x or z facing
// (s) picks the line and along coordinates; the block by facing, 4 (empty) for caps and diagonals.
inline bool WallUvCpu(const Grid& g, const float p[3], const float n[3], float uv[2]) {
    float r0[4], r1[4];
    if (!WallRows(g, r0, r1)) return false;
    const bool s = std::fabs(n[0]) >= std::fabs(n[2]);
    const float line = s ? p[0] : p[2], along = s ? p[2] : p[0], facing = s ? n[0] : n[2];
    const float block = std::max(std::min(std::fabs(n[0]), std::fabs(n[2])), std::fabs(n[1])) > 0.3f ? 4.0f : (s ? 0.0f : 1.0f) + (facing < 0 ? 2.0f : 0.0f);
    uv[0] = along * r0[0] + block * r0[2] + r1[0], uv[1] = std::floor(line + 0.5f) * r0[3] + p[1] * r0[1] + r1[1];
    return true;
}

// The grid's bilinear read (clamped) at uv, as the patched pixel shader makes it: the place inside its texel is held at the
// texel's centre on each side whose flag is closed (OpenTo), so the read never blends across it
inline bool SampleUv(const Grid& g, const std::vector<float>& grid, float u, float v, float out[4]) {
    float px = u * g.w, py = v * g.h;
    if (!g.flags.empty()) {
        const float cx = std::floor(px), cy = std::floor(py);
        float open[4];
        OpenTo(g, static_cast<int>(cx), static_cast<int>(cy), open);
        px = cx + 0.5f + std::clamp(px - cx - 0.5f, -0.5f * open[0], 0.5f * open[1]);
        py = cy + 0.5f + std::clamp(py - cy - 0.5f, -0.5f * open[2], 0.5f * open[3]);
    }
    const float fx = px - 0.5f, fy = py - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const float ax = fx - x0, ay = fy - y0;
    const auto at = [&](int x, int y, int ch) {
        x = std::clamp(x, 0, static_cast<int>(g.w) - 1), y = std::clamp(y, 0, static_cast<int>(g.h) - 1);
        return grid[(static_cast<size_t>(y) * g.w + x) * 4 + ch];
    };
    for (int ch = 0; ch < 4; ch++)
        out[ch] = (at(x0, y0, ch) * (1 - ax) + at(x0 + 1, y0, ch) * ax) * (1 - ay) + (at(x0, y0 + 1, ch) * (1 - ax) + at(x0 + 1, y0 + 1, ch) * ax) * ay;
    return true;
}

// The ambient term of a room-lit pixel after the patch (PatchRoomMapGi, per-draw rows k and lamp L): map rgb' = rgb k.y + G k.x -
// alpha a L, alpha' = alpha k.z; the game's "rgb' + alpha' x ambient" follows. G (premultiplied by a), a: the grid read.
// Mode 1 (atlas when ready): k = (gain, 1, 1, 0), L = the lamp share of the room's ambient (LampShare): the ready share of the
// lamp part becomes the atlas's light, the base colour (daylight, Rooms at Night) stays. Mode 2 (debug): k = (gain, 0, 0, 0),
// L = 0: the atlas alone, no lamp light and no ambient.
inline void Apply(const float map[4], const float g[4], const float k[4], const float ambient[3], const float lamp[3], float out[3]) {
    for (int c = 0; c < 3; c++) out[c] = map[c] * k[1] + g[c] * k[0] - map[3] * g[3] * lamp[c] + map[3] * k[2] * ambient[c];
}
// The lamp share of a room's ambient cA (S2-06): its part over the base colour C, max(0, cA - C) a channel; without C (unknown
// room) the night share of cA (S2-05's rule: by day no lamp part is replaced)
inline void LampShare(const float ambient[3], const float* base, float night, float out[3]) {
    for (int c = 0; c < 3; c++) out[c] = base ? std::max(0.0f, ambient[c] - base[c]) : ambient[c] * std::clamp(night, 0.0f, 1.0f);
}

} // namespace GiIndirect
