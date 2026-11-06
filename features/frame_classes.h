#pragma once
// The colours of the classes frame of a capture burst (capture_burst.h): the draw hook paints them, the burst reads them back
// from the finished picture, where ambient occlusion has darkened them and Color may have moved them, so a pixel is matched by
// its hue: its colour scaled to a brightest channel of 255, against the colours scaled the same way.
#include <algorithm>
#include <cstdlib>
#include <windows.h>

namespace FrameClasses {
struct Class {
    const char* name;
    unsigned char r, g, b;
};
enum : int { kReadsCube, kObject, kLotSurface, kRoof, kWall, kFloor, kWater, kTerrain, kOther, kCount };
// (the first one says what is known: a 64 px cube is bound and the shader declares a cube. It does not say the surface is metal)
inline constexpr Class kClasses[kCount] = {{"reads the cube", 0, 255, 255}, {"object on a rig", 255, 150, 0}, {"lot surface", 0, 220, 0},
                                           {"roof", 230, 0, 0},             {"exterior wall", 255, 255, 0},  {"floor", 0, 80, 255},
                                           {"water", 150, 0, 255},          {"terrain", 170, 110, 60},       {"other", 140, 140, 140}};
inline constexpr int kMinBright = 24, kMaxApart = 60; // darker: no hue to read; sum of the 3 channels' differences

// The class of a pixel, -1 when it is none of them (a surface that was not painted)
inline int Nearest(int r, int g, int b) {
    const int top = std::max({r, g, b});
    if (top < kMinBright) return -1;
    int best = -1, bestApart = kMaxApart;
    for (int i = 0; i < kCount; i++) {
        const Class& c = kClasses[i];
        const int ctop = std::max({c.r, c.g, c.b});
        const int apart = std::abs(r * 255 / top - c.r * 255 / ctop) + std::abs(g * 255 / top - c.g * 255 / ctop) + std::abs(b * 255 / top - c.b * 255 / ctop);
        if (apart < bestApart) bestApart = apart, best = i;
    }
    return best;
}

// B G R rows, top-down: how many pixels of each class, on every 4th pixel and row (x 16 is the screen's count)
inline void Count(const BYTE* bgr, int w, int h, long counts[kCount]) {
    std::fill_n(counts, kCount, 0L);
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 4) {
            const BYTE* p = bgr + (static_cast<size_t>(y) * w + x) * 3;
            if (const int c = Nearest(p[2], p[1], p[0]); c >= 0) counts[c]++;
        }
}

// The pixel of a class nearest the screen's centre with its 4 neighbours 2 px away of the same class (not an edge)
inline bool Pick(const BYTE* bgr, int w, int h, int cls, POINT& out) {
    const auto is = [&](int x, int y) {
        const BYTE* p = bgr + (static_cast<size_t>(y) * w + x) * 3;
        return Nearest(p[2], p[1], p[0]) == cls;
    };
    long long best = -1;
    for (int y = 2; y + 2 < h; y += 4)
        for (int x = 2; x + 2 < w; x += 4) {
            if (!is(x, y) || !is(x - 2, y) || !is(x + 2, y) || !is(x, y - 2) || !is(x, y + 2)) continue;
            const long long dx = x - w / 2, dy = y - h / 2, d = dx * dx + dy * dy;
            if (best < 0 || d < best) best = d, out = POINT{x, y};
        }
    return best >= 0;
}
} // namespace FrameClasses
