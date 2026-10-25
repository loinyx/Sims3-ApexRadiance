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
#include "fast_math.h"
#include <algorithm>
#include <cmath>

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

// The lamp's light at a point of normal n, as the shader computes it (the same operations in the same order).
// wallReach: the reach share's weight (1 on an outdoor wall pixel with the reach on, 0 otherwise).
inline void Eval(const Lamp& L, const float p[3], const float n[3], float wallReach, float out[3]) {
    float l[3] = {L.pos[0] - p[0], L.pos[1] - p[1], L.pos[2] - p[2]};
    const float d2 = std::max(l[0] * l[0] + l[1] * l[1] + l[2] * l[2], 1e-4f);
    const float inv = 1.0f / ApexMath::Sqrt(d2);
    for (float& v : l) v *= inv;
    const float ndl = Sat(n[0] * l[0] + n[1] * l[1] + n[2] * l[2]);
    const float f = std::min(ndl * (inv * inv) * L.W, L.cmax);
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

// An outdoor (raw) texel without the GPU lamps: the game stores min(sum, 1) per channel (FUN_006a31d0, rooms with +0x18
// set); the solve's sum minus what the GPU lamps added, at least 0
inline void RawWithout(const float sum[3], const float gpu[3], float out[3]) {
    for (int c = 0; c < 3; c++) out[c] = std::clamp(sum[c] - gpu[c], 0.0f, 1.0f);
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
inline float Weight(const float c[3], float k) { return 1.0f - Sat(k * ((c[2] + c[1]) + c[0])); }
// The texel (colour, alpha; before the pack's clamp) from x, ramp x ambient and ramp
inline void IndoorTexel(const float x[3], const float rampAmb[3], float ramp, float k, float out[4]) {
    float c[3];
    Tone(x, c);
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

} // namespace GpuLampLaw
