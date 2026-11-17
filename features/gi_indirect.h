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
// Walls (no lot position reaches their pixel shader) and diagonal surfaces have no consumer yet.
#include "gi_scene.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace GiIndirect {

enum Kind : int { kFloor = 0, kCeiling = 1 };

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
inline bool Build(const GiScene::Built& b, int d, int lotW, int lotH, Grid& out) {
    out = Grid{};
    out.d = std::max(1, d);
    struct Cell {
        int story, kind, gx, gz;
        uint32_t atlas;
        uint64_t tile;
    };
    std::vector<Cell> cells;
    int maxX = 0, maxZ = 0;
    for (const auto& [key, slot] : b.slots) {
        float p[3];
        int code;
        DecodeKey(key, p, code);
        if (code != 8 && code != 9) continue; // (walls: no consumer)
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
    if (out.w > 4096 || out.h > 4096) return out = Grid{}, false;
    out.points.reserve(cells.size());
    for (const Cell& c : cells) out.points.push_back({(out.rows[{c.story, c.kind}] + static_cast<uint32_t>(c.gz)) * out.w + static_cast<uint32_t>(c.gx), c.atlas, c.tile});
    return true;
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

// The grid texture as Scatter leaves it (rgba, w x h): each point's atlas rgb and its ready weight
inline void FillCpu(const Grid& g, const std::vector<float>& atlas, const std::vector<float>& ready, std::vector<float>& grid) {
    grid.assign(static_cast<size_t>(g.w) * g.h * 4, 0.0f);
    for (size_t i = 0; i < g.points.size(); i++) {
        const Point& p = g.points[i];
        for (int c = 0; c < 3; c++) grid[static_cast<size_t>(p.grid) * 4 + c] = atlas[static_cast<size_t>(p.atlas) * 4 + c];
        grid[static_cast<size_t>(p.grid) * 4 + 3] = i < ready.size() ? ready[i] : 0.0f;
    }
}

// What the patched shader reads at a lot position (x, z) of that story and kind: the grid's bilinear read (clamped) at
// UvRow's uv. False: no such layer
inline bool SampleCpu(const Grid& g, const std::vector<float>& grid, int story, int kind, float lotX, float lotZ, float out[4]) {
    float c[4];
    if (!UvRow(g, story, kind, c)) return false;
    const float tx = (lotX + 1.0f / 16.0f) / 64.0f, tz = (lotZ + 1.0f / 8.0f) / 64.0f; // (the vertex shader's packing)
    const float u = tx * c[0] + c[2], v = tz * c[1] + c[3];
    const float fx = u * g.w - 0.5f, fy = v * g.h - 0.5f;
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

// The ambient term of a room-lit pixel after the patch (PatchRoomMapGi, per-draw kConst k): map rgb' = rgb k.y + G a k.x,
// alpha' = alpha (k.z - a k.w); the game's "rgb' + alpha' x ambient" follows. G, a: the grid read.
// Mode 1 (atlas when ready): k = (gain, 1, 1, night): by night the ready share of the ambient becomes the atlas's light, by day
// the atlas adds to it. Mode 2 (debug): k = (gain, 0, 0, 0): the atlas alone, no lamp light and no ambient.
inline void Apply(const float map[4], const float g[4], const float k[4], const float ambient[3], float out[3]) {
    const float alpha = map[3] * (k[2] - g[3] * k[3]);
    for (int c = 0; c < 3; c++) out[c] = map[c] * k[1] + g[c] * g[3] * k[0] + alpha * ambient[c];
}

} // namespace GiIndirect
