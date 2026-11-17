#pragma once
// Real-time GI (docs/features/realtime-gi/design.md §3.1, §3.3): the room id of each indirect-layer texel (one texel a tile
// and story) and the wall-aware upsample weights from those texels to a shaded point. Pure: fed by RtLampProxy::Story, no
// game reads, no device. Checked by tools/gi_rooms_test.
//
// Room ids are the game's own: Story::room holds, per tile, the room of each quadrant as LevelLightShare reads it from the
// lighting tile (+0x7C + q * 0x14, FUN_006a9760; 0 = outside). Nothing is derived from the wall graph.
// A texel takes the room most of its tile's quadrants belong to (ties: the lower quadrant index); its point is the centroid
// of those quadrants. A tile with no floor (and no placed quadrant) or a negative id has no texel.
// Upsample: bilinear over the four nearest texels (tile centres), keeping only taps of the point's room whose texel point
// the point sees without crossing a wall edge of the story (any edge, open or not: an opening's frame is a wall face too),
// renormalised. When no bilinear tap is kept (the minority side of a diagonal split), the nearest kept texel of the 3 x 3
// tiles around the point takes weight 1; when none is kept, the point gets no indirect light (0 taps).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "rt_lamp_proxy.h"

namespace GiRooms {

constexpr int32_t kNone = -1;

struct Grid {
    int story = 0, w = 0, h = 0;
    std::vector<int32_t> id;        // per tile (iz * w + ix): the texel's room, kNone = no texel
    std::vector<float> px, pz;      // per tile: the texel's point (lot space x, z)
    std::vector<RtLampProxy::Edge> edges;
    std::vector<std::vector<uint32_t>> near; // per tile: the edges whose bounds touch the tile
};
struct Tap {
    int tile = 0;
    float w = 0.0f;
};

// The quadrant centroid of quadrant q (tile coordinates, RtLampProxy::QuadrantCorners' order)
inline void QuadCentroid(int q, float& cx, float& cz) {
    constexpr float c[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    cx = (c[q][0] + c[(q + 1) & 3][0] + 0.5f) / 3.0f, cz = (c[q][1] + c[(q + 1) & 3][1] + 0.5f) / 3.0f;
}

// False when the story carries no room ids (Story::room empty)
inline bool Build(const RtLampProxy::Story& s, Grid& g) {
    const size_t nt = static_cast<size_t>(s.w) * s.h;
    if (s.w <= 0 || s.h <= 0 || s.room.size() != 4 * nt) return false;
    g = Grid{};
    g.story = s.story, g.w = s.w, g.h = s.h, g.edges = s.edges;
    g.id.assign(nt, kNone), g.px.assign(nt, 0.0f), g.pz.assign(nt, 0.0f), g.near.assign(nt, {});
    for (size_t t = 0; t < nt; t++) {
        const bool floor = (t < s.floorY.size() && s.floorY[t] < RtLampProxy::kNoFloor) || (t < s.placed.size() && s.placed[t]);
        if (!floor) continue;
        const int32_t* q = &s.room[4 * t];
        int best = -1, bestN = 0;
        for (int k = 0; k < 4; k++) {
            if (q[k] < 0) continue;
            const int n = static_cast<int>(std::count(q, q + 4, q[k]));
            if (n > bestN) best = k, bestN = n;
        }
        if (best < 0) continue;
        g.id[t] = q[best];
        float sx = 0, sz = 0;
        for (int k = 0; k < 4; k++) {
            if (q[k] != q[best]) continue;
            float cx, cz;
            QuadCentroid(k, cx, cz);
            sx += cx, sz += cz;
        }
        g.px[t] = static_cast<float>(t % s.w) + sx / bestN, g.pz[t] = static_cast<float>(t / s.w) + sz / bestN;
    }
    for (uint32_t e = 0; e < g.edges.size(); e++) {
        const auto& E = g.edges[e];
        const int x0 = static_cast<int>(std::floor(std::min(E.x0, E.x1))) - 1, x1 = static_cast<int>(std::floor(std::max(E.x0, E.x1)));
        const int z0 = static_cast<int>(std::floor(std::min(E.z0, E.z1))) - 1, z1 = static_cast<int>(std::floor(std::max(E.z0, E.z1)));
        for (int iz = std::max(z0, 0); iz <= std::min(z1, g.h - 1); iz++)
            for (int ix = std::max(x0, 0); ix <= std::min(x1, g.w - 1); ix++) g.near[static_cast<size_t>(iz) * g.w + ix].push_back(e);
    }
    return true;
}

// The room of the quadrant under lot point (x, z), kNone outside the grid (lamp_visibility.h Quadrant)
inline int32_t RoomAt(const RtLampProxy::Story& s, float x, float z) {
    const float fx0 = std::floor(x), fz0 = std::floor(z);
    const int ix = static_cast<int>(fx0), iz = static_cast<int>(fz0);
    if (ix < 0 || iz < 0 || ix >= s.w || iz >= s.h || s.room.size() != 4 * static_cast<size_t>(s.w) * s.h) return kNone;
    const float fx = x - fx0, fz = z - fz0;
    const int q = std::fabs(fx - 0.5f) <= std::fabs(fz - 0.5f) ? (fz > 0.5f ? 2 : 0) : (fx > 0.5f ? 1 : 3);
    return s.room[4 * (static_cast<size_t>(iz) * s.w + ix) + q];
}

// Segment a -> b crosses edge e (touching the wall's line inside the segment, or a wall end on the segment, counts)
inline bool Crosses(float ax, float az, float bx, float bz, const RtLampProxy::Edge& e) {
    const auto side = [](float ox, float oz, float px, float pz, float qx, float qz) { return (px - ox) * (qz - oz) - (pz - oz) * (qx - ox); };
    const float d1 = side(e.x0, e.z0, e.x1, e.z1, ax, az), d2 = side(e.x0, e.z0, e.x1, e.z1, bx, bz);
    const float d3 = side(ax, az, bx, bz, e.x0, e.z0), d4 = side(ax, az, bx, bz, e.x1, e.z1);
    return ((d1 < 0 && d2 > 0) || (d1 > 0 && d2 < 0)) && d3 * d4 <= 0;
}
// A wall edge of the grid lies between lot points a and b (tiles at most one apart on each axis)
inline bool Blocked(const Grid& g, float ax, float az, float bx, float bz) {
    const int x0 = std::max(static_cast<int>(std::floor(std::min(ax, bx))), 0), x1 = std::min(static_cast<int>(std::floor(std::max(ax, bx))), g.w - 1);
    const int z0 = std::max(static_cast<int>(std::floor(std::min(az, bz))), 0), z1 = std::min(static_cast<int>(std::floor(std::max(az, bz))), g.h - 1);
    for (int iz = z0; iz <= z1; iz++)
        for (int ix = x0; ix <= x1; ix++)
            for (uint32_t e : g.near[static_cast<size_t>(iz) * g.w + ix])
                if (Crosses(ax, az, bx, bz, g.edges[e])) return true;
    return false;
}

// The texels a point (lot x, z) of room `room` reads and their weights (sum 1); returns the tap count, 0 = no indirect light
inline int Weights(const Grid& g, float x, float z, int32_t room, Tap out[4]) {
    if (room < 0) return 0;
    const auto keep = [&](int ix, int iz) {
        if (ix < 0 || iz < 0 || ix >= g.w || iz >= g.h) return -1;
        const int t = iz * g.w + ix;
        return g.id[t] == room && !Blocked(g, x, z, g.px[t], g.pz[t]) ? t : -1;
    };
    const float fx = x - 0.5f, fz = z - 0.5f;
    const int i0 = static_cast<int>(std::floor(fx)), j0 = static_cast<int>(std::floor(fz));
    const float a = fx - static_cast<float>(i0), b = fz - static_cast<float>(j0);
    const float bw[4] = {(1 - a) * (1 - b), a * (1 - b), (1 - a) * b, a * b};
    int n = 0;
    float sum = 0.0f;
    for (int k = 0; k < 4; k++) {
        if (!(bw[k] > 0.0f)) continue;
        const int t = keep(i0 + (k & 1), j0 + (k >> 1));
        if (t < 0) continue;
        out[n++] = {t, bw[k]};
        sum += bw[k];
    }
    if (n) {
        for (int k = 0; k < n; k++) out[k].w /= sum;
        return n;
    }
    const int cx = static_cast<int>(std::floor(x)), cz = static_cast<int>(std::floor(z));
    float best = 1e30f;
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            const int t = keep(cx + dx, cz + dz);
            if (t < 0) continue;
            const float d2 = (g.px[t] - x) * (g.px[t] - x) + (g.pz[t] - z) * (g.pz[t] - z);
            if (d2 < best) best = d2, out[0] = {t, 1.0f}, n = 1;
        }
    return n;
}

} // namespace GiRooms
