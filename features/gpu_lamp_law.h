#pragma once
// GPU lamps (developer prototype, features/gpu_lamps.h): the per-pixel law the patched shaders evaluate
// (ShaderPatches::PatchGpuLampsPs), on the CPU. The room solve's texel keeps every lamp but the GPU ones; for those it stores
// vis = (the light the game and Apex's tests let through to the texel) / Eval(texel), so at the texels the pixel's
// vis x Eval(pixel) is the light the bake would have stored, and between texels and after a lamp edit the law moves the
// light at once. Pure functions, no game reads (tools/gpu_lamps_check includes this header).
//
// The law is the room solve's evaluation of a lamp (Steam 1.67.2, read 13/10 with dumpbin): bare bulb FUN_006bde90 and street
// lamp FUN_006be020 = E0 x min(Cmax, k2 x range x intensity.x x max(0, N.l) / (k1 x d^2)) (x 3.333 [0x01158DA8] for the
// street-lamp class), the spot (FUN_006bffb0) and the shaded lamp (FUN_006be1c0) times their cone blends
// (docs/engine/light-objects-and-rigs.md, lamp_shape.h), and on outdoor wall texels Apex's realistic reach share
// (lamp_reach.h). Every factor after that (walls, slabs, openings, the soft light cut, a lamp listed twice) is in vis.
// A TubeLight (type 6, 0x006BFA70 over 0x006BD100, read 02/11 with dumpbin) has its own law (Tube below), and so do the area
// lights (types 9 and 10, Area below).
#include "fast_math.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace GpuLampLaw {

// One lamp as the shaders take it (world space)
struct Lamp {
    float pos[3] = {};
    float W = 0.0f;                  // k2 x range x intensity.x / k1 (x the street factor for the street-lamp class); 0 = no light
    float colour[3] = {};            // the effective colour E0 (+0xE0): 0 while the lamp is off
    float cmax = 2.0f;               // Cmax.x [0x011D1160]
    float axis[3] = {};              // the cone's axis a (spot +0x170; shaded lamp a1 +0x1A0, a2 = -a1)
    float sc1 = 1.0f, off1 = -1.0f;  // t1 = sc1 (a . u - off1), u = lamp -> point (the default gives t1 = 1: no cone)
    float sc2 = 0.0f, off2 = 0.0f;   // t2 = sc2 ((-a) . u - off2)
    float spill[3] = {};             // S: the light outside the cones (shaded lamp +0x150; 0 for a spot)
    float r0sq = 1e30f, invR2 = 0.0f; // the reach share's r0^2 and 1 / R^2 (the defaults: share 1)
};

inline float Sat(float v) { return std::clamp(v, 0.0f, 1.0f); }

// A lamp on a wall lights the wall it hangs on: a sconce 0.14 m in front of its wall reached the wall above it at 5 to 10
// degrees, N.l 0.1 to 0.15, and left it dark beside a perpendicular wall lit head on (F7 of a sconce on a screen wall, the
// wall of the story above). A real wall glows over its sconce (the shade's opening and the wall's roughness). On a wall
// texel (|n.y| < 0.5) with the lamp in front of its plane within kMountReach, N.l is at least kMountFloor x (1 - plane /
// kMountReach): the floor is gone 0.4 m away from the wall, so free-standing lamps and other walls keep the game's law.
constexpr float kMountReach = 0.4f, kMountFloor = 0.5f;
// plane: n . (lamp - p), the lamp's height over the texel's plane (unnormalised l)
inline float MountedNdl(float ndl, float plane, float ny) {
    const float wall = Sat((0.5f - std::fabs(ny)) * 1e4f), front = Sat(plane * 1e4f);
    return std::max(ndl, kMountFloor * wall * front * Sat(1.0f - plane * (1.0f / kMountReach)));
}
// The colour's scale for that raise as LightEvalHook makes it (level_light_share.cpp): after the game's evaluation, Cmax cut
// included, raised / N.l at most 8, none where N.l is 0 (the game's colour is). Raising N.l before the cut instead held a sconce's
// wall at Cmax where the game's goes over it (2.0 against 2.7 at 0.3 m, the scene of 03/11)
inline float MountScale(float ndl, float plane, float ny) {
    const float raised = MountedNdl(ndl, plane, ny);
    return raised > ndl && ndl > 1e-3f ? std::min(raised / ndl, 8.0f) : 1.0f;
}

// A TubeLight (type 6): its evaluation 0x006BFA70 is E0 x min(Cmax, k2 x range x intensity.x / (k1 d^2)) x max(0, N.l)
// (0x006BD100; the facing after the cap, [0x0107A538] = 1 and the normal's w taken as 0 as for the bare bulb), d and l to the
// nearest point of the segment from its position A (+0x120) to its end B (+0x170): A when (B - A) . (p - A) < 0, B when it is
// over len^2 (len +0x1A0, its length), else A + the projection. The law keeps it as a lamp with axis = B - A, sc1 = len^2 and
// sc2 = kTube (no cone, no spill; reach from A, as ReachShare). No MountedNdl: LightEvalHook raises a wall texel near A after
// the game's evaluation (in A), so the law is the game's evaluation itself (RoomLampMove::GameOverLaw 1).
// Checked against the probe's hook colours of every TubeLight of dumps 135710 and 145922 (78048 texels, all within 2/255).
constexpr float kTube = -1.0f;
// A RectangleAreaLight (type 9, 0x006BFBA0) or DiscAreaLight (type 10, 0x006BFDC0; both over 0x006BFAF0, read 02/11 with
// dumpbin): with v = A - p (A its position +0x120), L = (U.v, V.v, N.v) (U +0x170, V +0x190, N +0x180, dots summed (z + y) + x);
// C = L's first two lanes held in the area (the rectangle: each within +-e (e0 +0x1A0, e1 +0x1A4); the disc 0x006BD7C0: scaled
// to length e1 when their x^2 + y^2 is over e0) and 0; D = L - C; out = E0 x min(Cmax, k2 x range x intensity.x / (k1 |D|^2))
// x sat(D.z / |D|): no N.l, the cap before the facing. The law keeps it with axis = U, spill = V, (sc1, off1, off2) = N,
// (r0sq, invR2) = (e0, e1) and sc2 = kRect or kDisc (no reach: an area light is never drawn with one). At |D| = 0 (a point on
// the area) the game's evaluation is NaN, and the room's sum and texel with it (the hook keeps no part there): the law gives 0.
constexpr float kRect = -2.0f, kDisc = -3.0f;
// 0 a lamp, 1 a tube, 2 a rectangle, 3 a disc (sc2's mark)
inline int Kind(const Lamp& L) { return L.sc2 < -0.5f ? static_cast<int>(0.5f - L.sc2) : 0; }
inline bool IsTube(const Lamp& L) { return Kind(L) == 1; }
inline void TubeShape(Lamp& L, const float s[3], float len) {
    for (int i = 0; i < 3; i++) L.axis[i] = s[i], L.spill[i] = 0.0f;
    L.sc1 = len * len, L.off1 = 0.0f, L.sc2 = kTube, L.off2 = 0.0f;
}
// A vertical tube's 3D test points (T3dPS): the game tests from its low end only, under a table's top, so a floor lamp beside a table
// cast no shadow of it (05/11). lowY moved to the first point, returns the step; 0: any other lamp, the game's one point at +0x120
constexpr int kTubeTests = 4;
inline float TubeStep(const Lamp& L, float& lowY) {
    const float d = (L.axis[2] * L.axis[2] + L.axis[1] * L.axis[1]) + L.axis[0] * L.axis[0];
    if (!IsTube(L) || L.axis[1] * L.axis[1] <= 0.98f * d) return 0.0f;
    const float s = std::fabs(L.axis[1]) / kTubeTests;
    lowY += std::min(L.axis[1], 0.0f) + 0.5f * s;
    return s;
}
inline void AreaShape(Lamp& L, const float u[3], const float v[3], const float nrm[3], float e0, float e1, bool disc) {
    for (int i = 0; i < 3; i++) L.axis[i] = u[i], L.spill[i] = v[i];
    L.sc1 = nrm[0], L.off1 = nrm[1], L.off2 = nrm[2], L.sc2 = disc ? kDisc : kRect;
    L.r0sq = e0, L.invR2 = e1;
}
// Its place (a tube's end, an area's axes and size: a turn too) differs by more than eps from b's: a move
inline bool Moved(const Lamp& a, const Lamp& b, float eps) {
    const int k = Kind(a);
    const auto apart = [eps](float x, float y) { return std::fabs(x - y) > eps; };
    bool m = k != Kind(b) || (k && apart(a.sc1, b.sc1)) || (k > 1 && (apart(a.off1, b.off1) || apart(a.off2, b.off2) || apart(a.r0sq, b.r0sq) || apart(a.invR2, b.invR2)));
    for (int i = 0; i < 3; i++) m = m || apart(a.pos[i], b.pos[i]) || (k && apart(a.axis[i], b.axis[i])) || (k > 1 && apart(a.spill[i], b.spill[i]));
    return m;
}
inline void Area(const Lamp& L, const float p[3], float out[3]) {
    const float v[3] = {L.pos[0] - p[0], L.pos[1] - p[1], L.pos[2] - p[2]};
    const float lx = (L.axis[2] * v[2] + L.axis[1] * v[1]) + L.axis[0] * v[0], ly = (L.spill[2] * v[2] + L.spill[1] * v[1]) + L.spill[0] * v[0];
    const float lz = (L.off2 * v[2] + L.off1 * v[1]) + L.sc1 * v[0];
    float cx = std::min(std::max(lx, -L.r0sq), L.r0sq), cy = std::min(std::max(ly, -L.invR2), L.invR2);
    if (Kind(L) == 3) {
        const float s = ly * ly + lx * lx, k = s > L.r0sq ? 1.0f / ApexMath::Sqrt(s) * L.invR2 : 1.0f;
        cx = lx * k, cy = ly * k;
    }
    const float dx = lx - cx, dy = ly - cy;
    const float d2 = std::max((lz * lz + dy * dy) + dx * dx, 1e-20f);
    const float inv = 1.0f / ApexMath::Sqrt(d2);
    const float f = std::min((inv * inv) * L.W, L.cmax) * Sat(lz * inv);
    for (int c = 0; c < 3; c++) out[c] = f * L.colour[c];
}
inline void Tube(const Lamp& L, const float p[3], const float n[3], float wallReach, float out[3]) {
    const float v[3] = {p[0] - L.pos[0], p[1] - L.pos[1], p[2] - L.pos[2]};
    const float t = (L.axis[2] * v[2] + L.axis[1] * v[1]) + L.axis[0] * v[0];
    const float k = t / ((L.axis[2] * L.axis[2] + L.axis[1] * L.axis[1]) + L.axis[0] * L.axis[0]);
    float e[3];
    for (int i = 0; i < 3; i++) e[i] = 0.0f > t ? v[i] : t > L.sc1 ? v[i] - L.axis[i] : v[i] - k * L.axis[i];
    const float d2 = std::max(e[0] * e[0] + e[1] * e[1] + e[2] * e[2], 1e-4f);
    const float inv = 1.0f / ApexMath::Sqrt(d2);
    const float facing = std::max(-(n[0] * e[0] + n[1] * e[1] + n[2] * e[2]) * inv, 0.0f);
    const float f = std::min((inv * inv) * L.W, L.cmax) * facing;
    const float a2 = std::max(v[0] * v[0] + v[1] * v[1] + v[2] * v[2], 1e-4f), q = a2 * L.invR2;
    float win = Sat(1.0f - q * q);
    win *= win;
    float share = std::min(win * ((1.0f / a2) * L.r0sq), 1.0f);
    share = (share - 1.0f) * wallReach + 1.0f;
    for (int c = 0; c < 3; c++) out[c] = f * share * L.colour[c];
}

// The lamp's light at a point of normal n, as the shader computes it (the same operations in the same order).
// wallReach: the reach share's weight (1 on an outdoor wall pixel with the reach on, 0 otherwise).
inline void Eval(const Lamp& L, const float p[3], const float n[3], float wallReach, float out[3]) {
    if (IsTube(L)) return Tube(L, p, n, wallReach, out);
    if (Kind(L) > 1) return Area(L, p, out);
    float l[3] = {L.pos[0] - p[0], L.pos[1] - p[1], L.pos[2] - p[2]};
    const float plane = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
    const float d2 = std::max(l[0] * l[0] + l[1] * l[1] + l[2] * l[2], 1e-4f);
    const float inv = 1.0f / ApexMath::Sqrt(d2);
    for (float& v : l) v *= inv;
    const float ndl = Sat(n[0] * l[0] + n[1] * l[1] + n[2] * l[2]);
    const float f = std::min(ndl * (inv * inv) * L.W, L.cmax) * MountScale(ndl, plane, n[1]);
    const float al = L.axis[0] * l[0] + L.axis[1] * l[1] + L.axis[2] * l[2];
    const float t1 = (al + L.off1) * -L.sc1, t2 = (al - L.off2) * L.sc2;
    const float tsel = t1 > 0.0f ? t1 : t2;
    const float m = Sat((std::max(t1, t2) - 1.0f) * 1e4f + 1.0f);
    const float t = Sat(std::max(tsel, m));
    const float q = d2 * L.invR2;
    float win = Sat(1.0f - q * q);
    win *= win;
    float share = std::min(win * ((1.0f / d2) * L.r0sq), 1.0f);
    share = (share - 1.0f) * wallReach + 1.0f;
    const float s = f * share;
    for (int c = 0; c < 3; c++) out[c] = (L.spill[c] - L.spill[c] * t + t) * s * L.colour[c];
}

// (basis_lamp_law) A lamp's part of a directional map sample (the game's step 0x0069F280: 4 directions x rgba, the rig colour at
// p split over the directions) scaled per channel so its rgb sum is the law's light at p on a surface facing the lamp, as walls
// and floors take it. The directions' split and the alpha stay the game's; a channel the step leaves at 0 stays 0.
inline void BasisToLaw(const Lamp& L, const float p[3], float step[16]) {
    float n[3] = {L.pos[0] - p[0], L.pos[1] - p[1], L.pos[2] - p[2]};
    const float len = ApexMath::Sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(len > 1e-4f)) return;
    for (float& v : n) v /= len;
    float want[3];
    Eval(L, p, n, 0.0f, want);
    for (int c = 0; c < 3; c++) {
        const float rig = ((step[c] + step[4 + c]) + step[8 + c]) + step[12 + c];
        if (!(rig > 1e-6f) || !std::isfinite(want[c])) continue;
        const float s = want[c] / rig;
        for (int d = 0; d < 4; d++) step[d * 4 + c] *= s;
    }
}

// The reach share's weight at a normal, as the shader: sat((0.5 - |n.y|) x 1e4) x reachOn
inline float WallReach(const float n[3], bool reachOn) { return reachOn ? Sat((0.5f - std::fabs(n[1])) * 1e4f) : 0.0f; }

// The 5 constant rows of a lamp (shader_patches.h kGpuLampRows)
inline void Rows(const Lamp& L, float r[5][4]) {
    r[0][0] = L.pos[0], r[0][1] = L.pos[1], r[0][2] = L.pos[2], r[0][3] = L.W;
    r[1][0] = L.colour[0], r[1][1] = L.colour[1], r[1][2] = L.colour[2], r[1][3] = L.cmax;
    r[2][0] = L.axis[0], r[2][1] = L.axis[1], r[2][2] = L.axis[2], r[2][3] = L.sc1;
    r[3][0] = L.off1, r[3][1] = L.sc2, r[3][2] = L.off2, r[3][3] = L.r0sq;
    r[4][0] = L.spill[0], r[4][1] = L.spill[1], r[4][2] = L.spill[2], r[4][3] = L.invR2;
}

// The cones by lamp type. Spot (type 4): the game's sat((a . u - off) x scale) (+0x170, +0x158, +0x154). Shaded lamp
// (type 5): t_i = s (a_i . u - o_i) or, with Lamp shade shapes on, LampShape::ShapedCone(o_i) (sc, off), blended with S.
inline void SpotCone(Lamp& L, const float a[3], float off, float scale) {
    for (int i = 0; i < 3; i++) L.axis[i] = a[i], L.spill[i] = 0.0f;
    L.sc1 = scale, L.off1 = off, L.sc2 = 0.0f, L.off2 = 0.0f;
}
inline void ShadedCone(Lamp& L, const float a1[3], float sc1, float off1, float sc2, float off2, const float S[3]) {
    for (int i = 0; i < 3; i++) L.axis[i] = a1[i], L.spill[i] = S[i];
    L.sc1 = sc1, L.off1 = off1, L.sc2 = sc2, L.off2 = off2;
}
inline void NoCone(Lamp& L) {
    for (int i = 0; i < 3; i++) L.axis[i] = 0.0f, L.spill[i] = 0.0f;
    L.sc1 = 1.0f, L.off1 = -1.0f, L.sc2 = 0.0f, L.off2 = 0.0f;
}

// The visibility the solve stores for a lamp at a texel: what reached the texel (r + g + b) over the law there; 0 when the
// law gives (almost) nothing there; held under kMaxVis (a lamp listed twice in a room gives 2)
constexpr float kMaxVis = 16.0f;
inline float Visibility(const float reached[3], const float law[3]) {
    const float a = reached[0] + reached[1] + reached[2], b = law[0] + law[1] + law[2];
    if (!(b > 1e-7f) || !(a > 0.0f)) return 0.0f;
    const float v = a / b;
    return std::isfinite(v) ? std::min(v, kMaxVis) : 0.0f;
}


// ---- An indoor texel (a room with +0x18 clear; FUN_006a31d0, read 13/10 and replayed by room_bounce_model.h) ----
// x = the room's normalisation (+0x160) x the sum; c = x tanh(m / 2) / m for the mean m >= 0.01 (0x0069EC40), else x;
// alpha = (1 - sat(k (c.r + c.g + c.b))) x ramp (k = [0x011D0BA0], ramp = FUN_006ab210 at the sample); a pass with its
// ambient adds alpha x that ambient to the colour; the pack takes min(., 1). The patched room-lit shaders rebuild it per
// pixel from x (without the GPU lamps), ramp x ambient and ramp, with the lamps added to x (PatchGpuLampsPs indoor).
inline void Tone(const float x[3], float c[3]) {
    const float m = (x[2] * 0.33333334f + x[1] * 0.33333334f) + x[0] * 0.33333334f;
    if (!(m >= 0.01f)) {
        for (int i = 0; i < 3; i++) c[i] = x[i];
        return;
    }
    const float f = static_cast<float>((2.0 / (std::exp(-static_cast<double>(m)) + 1.0) - 1.0) / static_cast<double>(m));
    for (int i = 0; i < 3; i++) c[i] = x[i] * f;
}
// (physical rooms; tone_soft) Tone with its step at mean 0.01 spread over 0.004 .. 0.02: the game's curve halves a texel's light there, so
// a brighter texel showed darker than its neighbour (G 8 beside G 4 on a kitchen floor, 05/11) and a gradient drew a contour. The same
// curve past 0.02; a faint texel keeps x. Monotonic in the light (tools/rt_lamps_test)
inline void ToneSoft(const float x[3], float c[3]) {
    const float m = std::max((x[2] * 0.33333334f + x[1] * 0.33333334f) + x[0] * 0.33333334f, 1e-6f);
    const float s = std::clamp((m - 0.004f) / (0.02f - 0.004f), 0.0f, 1.0f), a = s * s * (3.0f - 2.0f * s);
    const float f = 1.0f + ((2.0f / (std::exp(-m) + 1.0f) - 1.0f) / m - 1.0f) * a;
    for (int i = 0; i < 3; i++) c[i] = x[i] * f;
}
inline float Weight(const float c[3], float k) { return 1.0f - Sat(k * ((c[2] + c[1]) + c[0])); }
// The texel (colour, alpha; before the pack's clamp) from x, ramp x ambient and ramp (soft: ToneSoft, a physical room's compose)
inline void IndoorTexel(const float x[3], const float rampAmb[3], float ramp, float k, float out[4], bool soft = false) {
    float c[3];
    if (soft) ToneSoft(x, c);
    else Tone(x, c);
    const float w = Weight(c, k);
    for (int i = 0; i < 3; i++) out[i] = c[i] + w * rampAmb[i];
    out[3] = w * ramp;
}
// ramp x ambient and ramp from the game's own pack input p (before the clamp) and its x; false when p is not the game's
// tail for that x (a texel this model does not describe) or its weight is 0 (the ramp cannot be read back): such a texel
// keeps its lamps baked
inline bool IndoorSplit(const float x[3], const float p[4], float k, float rampAmb[3], float& ramp) {
    float c[3];
    Tone(x, c);
    const float w = Weight(c, k);
    if (!(w > 1e-4f) || !std::isfinite(p[3]) || p[3] < 0.0f) return false;
    for (int i = 0; i < 3; i++) {
        const float amb = p[i] - c[i]; // alpha x the pass's ambient (0 for passes without it)
        if (!std::isfinite(amb) || amb < -1e-4f - 1e-4f * std::fabs(c[i])) return false;
        rampAmb[i] = std::max(amb, 0.0f) / w;
    }
    ramp = p[3] / w;
    return std::isfinite(ramp) && std::isfinite(rampAmb[0]) && std::isfinite(rampAmb[1]) && std::isfinite(rampAmb[2]);
}
// The ramp terms of a map's texels whose split failed only because the solve's lamps saturated them (weight 0: the game's texel
// holds no ambient, so none can be read back). Kept as 0 they lit nothing once the lamp moved away: the old light pool showed
// as a dark, texel-edged patch until the room's next solve (0.67-0.72x, 23:07 videos and the recorded scenes of 03/11). The
// ramp (FUN_006ab210) depends on the height alone and the ambient is the pass's, so they are taken from the map's texels that
// split well: a least-squares line of ramp in y (within their range), times their median ambient-to-ramp ratio. A filled
// texel's weight is 0 at its solve's sum, so the solve's own light is unchanged
struct SplitFill {
    bool ok = false;
    float a = 0.0f, b = 0.0f, lo = 0.0f, hi = 0.0f, ratio[3] = {};
    float Ramp(float y) const { return std::clamp(a + b * y, lo, hi); }
};
// good: (y, ramp, rampAmb r, g, b) of texels whose split worked with a weight of at least 0.2
inline SplitFill FitSplit(std::vector<std::array<float, 5>> good) {
    SplitFill f;
    if (good.size() < 4) return f;
    double n = 0, sy = 0, sr = 0, syy = 0, syr = 0;
    f.lo = f.hi = good[0][1];
    for (const auto& g : good) n++, sy += g[0], sr += g[1], syy += g[0] * g[0], syr += g[0] * g[1], f.lo = std::min(f.lo, g[1]), f.hi = std::max(f.hi, g[1]);
    const double var = syy / n - (sy / n) * (sy / n);
    f.b = var > 1e-6 ? static_cast<float>((syr / n - (sy / n) * (sr / n)) / var) : 0.0f;
    f.a = static_cast<float>(sr / n - f.b * (sy / n));
    std::erase_if(good, [](const std::array<float, 5>& g) { return !(g[1] > 1e-3f); });
    if (good.empty()) return f;
    for (int q = 0; q < 3; q++) {
        std::nth_element(good.begin(), good.begin() + good.size() / 2, good.end(),
                         [q](const std::array<float, 5>& x, const std::array<float, 5>& y) { return x[2 + q] / x[1] < y[2 + q] / y[1]; });
        f.ratio[q] = good[good.size() / 2][2 + q] / good[good.size() / 2][1];
    }
    f.ok = std::isfinite(f.a) && std::isfinite(f.b) && std::isfinite(f.ratio[0]) && std::isfinite(f.ratio[1]) && std::isfinite(f.ratio[2]);
    return f;
}
// IndoorSplit, with a fill for a texel the solve's lamps saturated (weight 0, p otherwise the game's tail)
inline bool IndoorSplitOrFill(const float x[3], const float p[4], float k, float y, const SplitFill& fill, float rampAmb[3], float& ramp) {
    if (IndoorSplit(x, p, k, rampAmb, ramp)) return true;
    float c[3];
    Tone(x, c);
    // (the game's tail at weight 0: its colour the toned sum, its alpha 0; any other p is a texel this model does not describe)
    bool tail = fill.ok && !(Weight(c, k) > 1e-4f) && std::isfinite(p[3]) && p[3] >= 0.0f && p[3] <= 1e-4f;
    for (int q = 0; q < 3 && tail; q++) tail = std::isfinite(p[q]) && std::fabs(p[q] - c[q]) <= 1e-4f + 1e-4f * std::fabs(c[q]);
    if (!tail) {
        rampAmb[0] = rampAmb[1] = rampAmb[2] = ramp = 0.0f;
        return false;
    }
    ramp = fill.Ramp(y);
    for (int q = 0; q < 3; q++) rampAmb[q] = ramp * fill.ratio[q];
    return true;
}

} // namespace GpuLampLaw
