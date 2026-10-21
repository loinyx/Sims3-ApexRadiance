#pragma once
// Room ambient through arches (development builds, switch arch_ambient in [dev_build]; pure part, tools/arch_ambient_test).
// The game gives every indoor room one ambient colour (room+0x110, the bounce of its lamps); the floor and wall shaders add
// it as lightmap.a x c4 (PS 2BEB2548: "mad r2.xyz, r0.w, c4, r0"), so two rooms joined by an arch meet in a hard step of
// colour on the arch's line (a capture: room 3, a green lamp, ambient (0.010 0.037 0.007), beside room 4, (0.010 0.010
// 0.007), the same light map on both floors). Each room's texels near an arch take the part of the neighbour's ambient
// that is higher than their own, with the texel's own ambient weight, fading from all of it on the arch's line to none
// kReach metres in: on the line both sides show the higher of the two ambients (no step), and no room ever gets darker.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ArchAmbient {

constexpr float kReach = 1.5f; // m from the arch's opening (lot space, horizontal) at which the neighbour's ambient is gone

struct Opening {
    float ax = 0, az = 0, bx = 0, bz = 0; // the opening's span on its wall line (lot space)
    float amb[3] = {};                    // the neighbour room's ambient colour
};

// Horizontal distance from (px, pz) to the segment a-b
inline float SegDist(float px, float pz, float ax, float az, float bx, float bz) {
    const float dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz;
    const float t = l2 > 1e-12f ? std::clamp(((px - ax) * dx + (pz - az) * dz) / l2, 0.0f, 1.0f) : 0.0f;
    const float ex = ax + t * dx - px, ez = az + t * dz - pz;
    return std::sqrt(ex * ex + ez * ez);
}

// 1 on the line, 0 from kReach on, smooth (1 - smoothstep)
inline float Weight(float d) {
    if (!(d >= 0.0f)) return 0.0f;
    const float s = std::clamp(d / kReach, 0.0f, 1.0f);
    return 1.0f - s * s * (3.0f - 2.0f * s);
}

// A floor draw's ambient (its c4) against a room's (+0x110): equal within 2 % (plus 1e-4) per channel. Exact float equality
// lost the arch's rooms after the camera's story changed (a recording of a story change).
inline bool Same(const float* a, const float* b) {
    for (int c = 0; c < 3; c++)
        if (!(std::fabs(a[c] - b[c]) <= 1e-4f + 0.02f * std::max(std::fabs(a[c]), std::fabs(b[c])))) return false;
    return true;
}

// Adds to rgb (the texel's colour before its pack, alpha = its ambient weight) the share of the neighbours' ambients over
// `own`, the largest per channel over the openings. True when anything was added.
inline bool Add(const float own[3], const Opening* o, int n, float px, float pz, float alpha, float* rgb) {
    if (!(alpha > 0.0f) || n <= 0) return false;
    float add[3] = {};
    for (int i = 0; i < n; i++) {
        const float w = Weight(SegDist(px, pz, o[i].ax, o[i].az, o[i].bx, o[i].bz));
        if (w <= 0.0f) continue;
        for (int c = 0; c < 3; c++) {
            const float diff = o[i].amb[c] - own[c];
            if (std::isfinite(diff) && diff > 0.0f) add[c] = std::max(add[c], diff * w);
        }
    }
    bool any = false;
    for (int c = 0; c < 3; c++)
        if (add[c] > 0.0f) {
            rgb[c] = std::min(1.0f, rgb[c] + alpha * add[c]);
            any = true;
        }
    return any;
}

// Which rooms whose ambient matches a floor draw's give their arches (LevelLightShare::ArchAmbientConsts). The arch rows hold
// lot metres and the shader rebuilds the pixel's lot position from its own floor map, so a room of another story or lot must
// never give its arches: its segment would be drawn at the same lot coordinates on an unrelated floor.
struct Candidate {
    std::uintptr_t mgr = 0, tracker = 0; // the room's story manager and its lot
};
enum class Pick {
    None,     // no room matches the draw's ambient
    Story,    // the draw's story is known and rooms of it match: those rooms (Takes)
    OffStory, // the draw's story is known and only rooms of other stories or lots match: none
    Fallback, // the draw's story is not known and every match is on one story of one lot: all of them
    Spread,   // the draw's story is not known and the matches are on more than one story or lot: none
};
// drawMgr: the draw's story manager, 0 = not known
inline Pick Select(const Candidate* c, int n, std::uintptr_t drawMgr) {
    if (n <= 0) return Pick::None;
    if (drawMgr) {
        for (int i = 0; i < n; i++)
            if (c[i].mgr == drawMgr) return Pick::Story;
        return Pick::OffStory;
    }
    for (int i = 1; i < n; i++)
        if (c[i].mgr != c[0].mgr || c[i].tracker != c[0].tracker) return Pick::Spread;
    return Pick::Fallback;
}
// Whether candidate c gives its arches under pick
inline bool Takes(Pick pick, const Candidate& c, std::uintptr_t drawMgr) {
    return pick == Pick::Fallback || (pick == Pick::Story && c.mgr == drawMgr);
}

} // namespace ArchAmbient
