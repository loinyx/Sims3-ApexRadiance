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
#include <climits>
#include <cstdint>

namespace ArchAmbient {

constexpr float kReach = 1.5f; // m from the arch's opening (lot space, horizontal) at which the neighbour's ambient is gone, at the least

// (arch_reach) How far the blend runs: an F7 capture, rooms of ambient 0.0288 and 0.0116 through a 2.66 m arch, showed the floor falling
// 2.5x inside 1.5 m. Two rooms close in tone keep kReach; past a contrast (nb - own) / (nb + own) of kReachFree it grows, up to
// kReachPerWidth x the opening, so a narrow arch keeps kReach too, and to kMaxReach. Contrast, not a ratio's log: no CRT call per draw.
// UNVERIFIED in game: the four numbers, tuned to keep the fall under about 0.4 of a natural log per metre
constexpr float kReachFree = 0.2f, kReachPerContrast = 9.0f, kReachPerWidth = 1.25f, kMaxReach = 4.0f;
inline float Reach(float width, const float own[3], const float nb[3]) {
    float c = 0.0f;
    for (int i = 0; i < 3; i++)
        if (nb[i] > own[i] && nb[i] + own[i] > 0.0f) c = std::max(c, (nb[i] - own[i]) / (nb[i] + own[i]));
    return std::clamp(kReach + kReachPerContrast * (c - kReachFree), kReach, std::clamp(kReachPerWidth * width, kReach, kMaxReach));
}

struct Opening {
    float ax = 0, az = 0, bx = 0, bz = 0; // the opening's span on its wall line (lot space)
    float amb[3] = {};                    // the neighbour room's ambient colour
    float reach = kReach;                 // (Reach)
};

// Horizontal distance from (px, pz) to the segment a-b
inline float SegDist(float px, float pz, float ax, float az, float bx, float bz) {
    const float dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz;
    const float t = l2 > 1e-12f ? std::clamp(((px - ax) * dx + (pz - az) * dz) / l2, 0.0f, 1.0f) : 0.0f;
    const float ex = ax + t * dx - px, ez = az + t * dz - pz;
    return std::sqrt(ex * ex + ez * ez);
}

// A blend past kReach ends at the room's next opening into another room: the floor beyond that line takes no share of this neighbour, so
// the lift would draw there the step the blend removes. a-b the opening, o-p the other; openings never cross, so their ends give the gap
inline float HeldReach(float reach, float ax, float az, float bx, float bz, float ox, float oz, float px, float pz) {
    const float gap = std::min(std::min(SegDist(ax, az, ox, oz, px, pz), SegDist(bx, bz, ox, oz, px, pz)), std::min(SegDist(ox, oz, ax, az, bx, bz), SegDist(px, pz, ax, az, bx, bz)));
    return std::min(reach, std::max(kReach, gap));
}

// 1 on the line, 0 from reach on, smooth (1 - smoothstep)
inline float Weight(float d, float reach = kReach) {
    if (!(d >= 0.0f)) return 0.0f;
    const float s = std::clamp(d / reach, 0.0f, 1.0f);
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
        const float w = Weight(SegDist(px, pz, o[i].ax, o[i].az, o[i].bx, o[i].bz), o[i].reach);
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

// The reflection cube a floor pixel reads near an arch: the neighbour's on the arch's line (w = 1), its own from the reach on (w = 0).
// A lerp, the shader's: the floors on both sides then show one value on the line, whichever cube is brighter
inline float CubeAt(float own, float nb, float w) { return own + (nb - own) * w; }

// The one neighbour whose cube a draw takes, cell by cell (ArchAmbientConsts): the largest difference (sum) among the rooms with a cube.
// flags[n]: 1 on cell n when it goes to that room; a room that takes over clears the cells of the one before it. cubeOf(room): null = none
struct CubePick {
    std::uintptr_t room = 0;
    float sum = 0.0f;
    void* tex = nullptr;
};
template <typename CubeFn> void TakeCube(CubePick& p, std::uintptr_t room, float sum, float* flags, int n, CubeFn cubeOf) {
    if (room == p.room) flags[n] = 1.0f;
    else if (void* const tex = sum > p.sum ? cubeOf(room) : nullptr) {
        std::fill(flags, flags + n, 0.0f);
        flags[n] = 1.0f, p = CubePick{room, sum, tex};
    }
}

// The probe proxy an atrium room draws its reflection with (LevelLightShare ProbeBindHook): its own once its probe is ready,
// else the ready member of the nearest other story (the one across the floor line; a group can hold several rooms of one story
// under one upper room), a member of its own story only when no other story has one, else its own (the game's default reflection).
// gaps[k]: |story of others[k] - own story|
template <typename ReadyFn> std::uintptr_t AtriumProbe(std::uintptr_t own, const std::uintptr_t* others, const int* gaps, int n, ReadyFn ready) {
    if (ready(own)) return own;
    std::uintptr_t best = own;
    int bestRank = INT_MAX;
    for (int k = 0; k < n; k++) {
        const int rank = gaps[k] > 0 ? gaps[k] : INT_MAX - 1;
        if (rank < bestRank && others[k] && ready(others[k])) best = others[k], bestRank = rank;
    }
    return best;
}

// Which rooms whose ambient matches a floor draw's give their arches (LevelLightShare::ArchAmbientConsts). The arch rows hold
// lot metres and the shader takes the pixel's lot position from the floor VS, so a room of another story or lot must
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

// (room_bounce) The light a lit room sends through an arch into the next. The arch's own blend above ends 1.5 m in, and the room behind it
// stayed flat past that: a room's ambient is one colour, whatever its neighbour holds. Each opening is a rectangle of the neighbour's
// radiosity, stood for by e, the part of its ambient its lamps gave (live, so a drag needs no solve; a lampless room gives none). A
// texel takes e x the exact form factor of the rectangle (contour formula, within 0.1% of numeric integration d = 0.25-6 m), on a
// floor x 1 - Weight at the blend's own reach, so the blend owns the line and the two never stack past the neighbour's level. Added after the tone
// (AccPhys into the sum's alpha, ComposeKeptPS): before it, the tone's knee drew a contour along the gradient.
// shortcut: one hue per room (the openings' weighted by area), two neighbours of different colours mix; upgrade: a sum per channel
struct BounceRow {
    float ax, az, bx, bz, y0, y1;
    float e[3]; // the neighbour's lamp share over this room's norm
    float reach = kReach; // the arch blend's (Reach)
};
constexpr int kBounceRows = 6;
inline float BounceEdge(const float a[3], const float b[3], const float n[3]) {
    const float c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const float dt = std::clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1.0f, 1.0f);
    return std::acos(dt) * (c[0] * n[0] + c[1] * n[1] + c[2] * n[2]) / std::sqrt(std::max(c[0] * c[0] + c[1] * c[1] + c[2] * c[2], 1e-12f));
}
// The opening's form factor at P (lot space) with the normal N
inline float BounceForm(const BounceRow& r, const float P[3], const float N[3]) {
    const float q[4][3] = {{r.ax, r.y0, r.az}, {r.bx, r.y0, r.bz}, {r.bx, r.y1, r.bz}, {r.ax, r.y1, r.az}};
    float v[4][3];
    for (int i = 0; i < 4; i++) {
        const float d[3] = {q[i][0] - P[0], q[i][1] - P[1], q[i][2] - P[2]}, l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int k = 0; k < 3; k++) v[i][k] = d[k] / std::max(l, 1e-6f);
    }
    return std::fabs(BounceEdge(v[0], v[1], N) + BounceEdge(v[1], v[2], N) + BounceEdge(v[2], v[3], N) + BounceEdge(v[3], v[0], N)) * 0.15915494f;
}
// The shader's rows from n openings: gBo as (a.xz, b.xz), (y0, y1, weight, 1 / reach), the hue gBc so that weight x form x hue = e x form for one
// neighbour. Returns the rows used (a neighbour with no share gives none)
inline int BounceConsts(const BounceRow* r, int n, float rows[2 * kBounceRows][4], float hue[4]) {
    int used = 0;
    double a = 0.0, h[3] = {};
    for (int i = 0; i < n && used < kBounceRows; i++) {
        const float m = (r[i].e[0] + r[i].e[1] + r[i].e[2]) / 3.0f;
        const float w = std::sqrt((r[i].bx - r[i].ax) * (r[i].bx - r[i].ax) + (r[i].bz - r[i].az) * (r[i].bz - r[i].az)) * (r[i].y1 - r[i].y0);
        if (!(m > 0.0f) || !(w > 0.0f)) continue;
        float* s = rows[2 * used];
        s[0] = r[i].ax, s[1] = r[i].az, s[2] = r[i].bx, s[3] = r[i].bz;
        s[4] = r[i].y0, s[5] = r[i].y1, s[6] = m, s[7] = 1.0f / r[i].reach;
        a += static_cast<double>(w) * m;
        for (int c = 0; c < 3; c++) h[c] += static_cast<double>(w) * r[i].e[c];
        used++;
    }
    for (int c = 0; c < 3; c++) hue[c] = a > 0.0 ? static_cast<float>(h[c] / a) : 0.0f;
    hue[3] = 0.0f;
    return used;
}
// The bounce at a texel, as the shader's sum (Bounce) and the compose's hue give it: out += hue x sum of weight x form
inline void BounceAt(const float rows[2 * kBounceRows][4], int used, const float hue[4], const float P[3], const float N[3], float out[3]) {
    float s = 0.0f;
    for (int i = 0; i < used; i++) {
        const float* r = rows[2 * i];
        const BounceRow b{r[0], r[1], r[2], r[3], r[4], r[5], {}};
        const float own = N[1] > 0.5f ? 1.0f - Weight(SegDist(P[0], P[2], r[0], r[1], r[2], r[3]), 1.0f / r[7]) : 1.0f;
        s += r[6] * own * BounceForm(b, P, N);
    }
    for (int c = 0; c < 3; c++) out[c] += hue[c] * s;
}

// A room's ambient read at most once per present frame (ArchAmbientConsts compares every room with arches at each floor
// draw, and again when none matched: up to two live reads a room per draw, and its arch rows took 28 us a floor draw in a
// recording; UNVERIFIED how much of it the reads were). The game writes it on the light tree thread with no counter this
// code can see, so a value written during a frame is taken at the next one, as a draw issued a moment later would have.
// read: the live read into its float[3], false when the room cannot be read (remembered for the frame too).
struct FrameRead {
    std::uint32_t frame = ~0u; // (none yet)
    bool ok = false;
    float amb[3] = {};
};
template <typename ReadFn> bool ReadOnce(FrameRead& m, std::uint32_t frame, ReadFn read, float* out) {
    if (m.frame != frame) m.frame = frame, m.ok = read(m.amb);
    if (m.ok) out[0] = m.amb[0], out[1] = m.amb[1], out[2] = m.amb[2];
    return m.ok;
}

} // namespace ArchAmbient
