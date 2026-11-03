#pragma once
// Real-time outdoor lamps (Lamp lighting > Instant, "lampLightingMode"; docs/features/night-lighting/rt-outdoor-lamps.md): the
// arithmetic of a relit room-0 texel, shared by the in-game code (rt_outdoor_lamps.cpp), the GPU pass (rt_lamp_gpu.cpp, whose
// HLSL in shaders/rt_lamps_hlsl.h does the same operations) and the offline harness (tools/rt_lamps_test). No game access.
//
// A room-0 texel of a story's map is the plain sum of what each light of room 0's list added, packed as min(sum, 1)
// (FUN_006a31d0; docs/engine/room-light-maps.md). Quick outdoor lamp edits (outdoor_delta.h) keep, per texel, that sum S and
// each light's part of it, as the solve's running sum measured them after every test. So the texel without the lot's
// outdoor lamps is exact:
//   base = S - sum of the parts of the lamps the GPU draws
// and the relit texel is pack(min(base + sum over those lamps of mult x fade(law x shadow), 1)) with the dither, the floor
// mask's alpha and (class-2 walls) the game's wall blur after it. Every term is >= 0, so min(min(base, 1) + G, 1) is the
// same as min(base + G, 1): nothing else of the bake changes.
//   - law: the room solve's evaluation of the lamp (gpu_lamp_law.h Eval: E0 x min(Cmax, W N.l / d^2) x cones x, on an outdoor
//     wall texel with Realistic lamp reach, the reach share);
//   - fade: Smooth light maps' soft light cut (LevelLightShare FadeAtThreshold): 0 under the room's threshold, smoothstep to
//     the full colour at twice it, on r + g + b of the colour after the tests; off: the game's drop under the threshold;
//   - mult: how often the lamp is in that room 0's list (a story-0 lamp twice: docs/engine/room-light-maps.md);
//   - shadow: the share of the lamp ball the texel sees past the lot's walls, roofs and slabs (rt_lamp_proxy.h; on the GPU
//     the lamp's cube shadow map, rt_lamp_gpu.cpp).
// The floor mask of an outdoor floor texel of a story >= 1 (LevelLightShare SolvePointBatchImpl): alpha = max(1 - vis / all,
// under), vis and all the luminance of room 0's lamps at the texel with N.l ignored, with and without the walls (the
// threshold drop, no fade); under = the share of the ground atlas the lamps under the floor's plane put there (UnderShare).
#include <xmmintrin.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "gpu_lamp_law.h"
#include "outdoor_delta_model.h"
#include "terrain_bake_rule.h"

namespace RtLampModel {

// A lamp the GPU draws: the law's lamp (world or lot space, as the texels) and how often the room's list holds it
struct Lamp {
    GpuLampLaw::Lamp law;
    float mult = 1.0f;
};

// Which lights the GPU draws (read at the solve that kept the parts, and every frame): a lamp of the lamp classes (bare bulb
// 3, spot 4, shaded lamp 5) or a lot's street-lamp class (11), of the lot itself, alive, registered outdoors (room 0).
// Street lamps of the world (lot id 0), window lights (7, 8), tubes and area lights, and lamps of other lots stay baked.
struct LightFields {
    int type = 0;
    uint64_t lot = 0;  // +0xC0 / +0xC4
    int room = -1;     // +0x08
    uint8_t flags = 0; // +0x100: 0x01 alive
    bool unmodelled = false; // a shaded lamp (5) whose two shade axes are not opposite: the GPU law has no cone for it
};
// A shaded lamp (type 5) the GPU law describes: its shade axes (+0x1A0, +0x190) opposite and both cone values (+0x174,
// +0x170) finite. Another one stays baked (14/10: the law fell back to a bare bulb's sphere for it)
inline bool ShadedModelled(const float* a1, const float* a2, float o1, float o2) {
    return std::fabs(a1[0] + a2[0]) + std::fabs(a1[1] + a2[1]) + std::fabs(a1[2] + a2[2]) < 1e-3f && std::isfinite(o1) && std::isfinite(o2);
}
inline bool Drawn(const LightFields& f, uint64_t roomLot) {
    const bool cls = f.type == 3 || f.type == 4 || f.type == 5 || f.type == 11;
    return cls && !f.unmodelled && f.lot != 0 && f.lot == roomLot && f.room == 0 && (f.flags & 0x01);
}

// Smooth light maps' soft light cut on one evaluation's colour (after the tests); fade = false: the game's drop
inline void Fade(float c[3], float thr, bool fade) {
    const float sum = (c[2] + c[1]) + c[0];
    if (!(sum >= thr)) {
        c[0] = c[1] = c[2] = 0.0f;
        return;
    }
    if (!fade || sum >= 2.0f * thr) return;
    const float x = (sum - thr) / thr;
    const float f = x * x * (3.0f - 2.0f * x);
    for (int i = 0; i < 3; i++) c[i] *= f;
}
// GPU room lamps, stage 1 (outdoor_delta.cpp KeepEnd): one kept part of a room's texel recomposed with the lamp's law there.
// A: the colour Apex's tests left before the soft light cut over the law (r + g + b); B: the game's 2D wall x 3D share (0:
// dropped, or never tested); early: Apex's threshold early-out returned (under the threshold the colour stays dark); fade:
// the soft light cut ran, at thr x thrScale (off: the game's drop under thr). Adds mult x fade(law x A) x B.
inline void KeptAdd(const float law[3], float A, float B, float thrScale, float mult, bool early, bool fade, float thr, float out[3]) {
    float c[3] = {law[0] * A, law[1] * A, law[2] * A};
    if (early && thr > (c[2] + c[1]) + c[0]) return;
    Fade(c, fade ? thr * thrScale : thr, fade);
    for (int i = 0; i < 3; i++) out[i] += mult * c[i] * B;
}
// GPU room lamps, room 0's kept lamps (outdoor_delta.cpp BuildKept0, AccKept0PS): the luminance of a kept lamp's law at p with N.l
// ignored, as the floor mask's copies evaluate it (the normal toward the lamp, no reach; colourOnly: its colour). A floor mask
// part is kept as its luminance over this, and added back as this now x that share
inline float KeptWrapLum(const GpuLampLaw::Lamp& l, bool colourOnly, const float p[3]) {
    float law[3];
    if (colourOnly) {
        for (int i = 0; i < 3; i++) law[i] = l.colour[i];
        return OutdoorDeltaModel::Lum3(law);
    }
    float d[3] = {l.pos[0] - p[0], l.pos[1] - p[1], l.pos[2] - p[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len > 1e-6f)
        for (float& v : d) v /= len;
    else
        d[0] = 0, d[1] = 1, d[2] = 0;
    GpuLampLaw::Eval(l, p, d, 0.0f, law);
    return OutdoorDeltaModel::Lum3(law);
}
// The ground alpha an exit lamp's light gives a story-0 ground texel (LevelLightShare SolveGroundTexel): E = the luminance of
// the exit lamps' light there, min(E, 254 / 255), 0 when none (a texel under a floor tile is not one: it keeps alpha 0)
inline float ExitAlpha(float E) { return E > 0.0f && std::isfinite(E) ? std::min(E, 254.0f / 255.0f) : 0.0f; }
// (rt_outdoor_lamps.cpp PrepareJob) a room-0 texel's base under the GPU sum: an upper wall of a double-height room (inner, bit 8)
// without the street lamps' light the solve left in it (world), never under 0; any other texel the solve's base as it is; not
// finite: 0
inline float ComposeBase(float base, float world, bool inner) { return !std::isfinite(base) ? 0.0f : inner ? std::max(base - world, 0.0f) : base; }
// GPU room lamps, stage 2a (outdoor_delta.cpp KeptFinalCheck): the colour the game's pack 0x0060CD10 takes for a kept texel
// whose sum is now R. S and p: the solve's sum there and its pack input before the clamp (FUN_006a31d0 [esp+60h]); norm: the
// room's +0x160 the solve toned it with. Roofed: the solve's tail (gpu_lamp_law.h IndoorTexel) with the ramp x ambient and the
// ramp p gives back at S (IndoorSplit); false when it cannot (the weight 0 there: the ambient is left out). Roofless: the hue
// kept and min 1 (PackInput) with p's alpha (the floor mask's or the exit light's, as the solve left it). Then min(., 1) as
// the solve's minps, RoomMapDither's roofed alpha floor (alphaMin, 0: none) and dither (in colour units, 0: none).
// ambScale (physical indoor lamps, S8; null: 1): the ramp x ambient read back times the room's ambient now over the one its solve
// left (AmbScale), so a pass's ambient follows Rooms at Night without a solve; exactly 1 when the ambient is unchanged
inline bool KeptTexel(const float R[3], const float S[3], const float p[4], float norm, float k, bool roofed, float alphaMin, float dither, float out[4],
                      const float* ambScale = nullptr) {
    bool split = true;
    if (roofed) {
        float xs[3], x[3], rampAmb[3] = {}, ramp = 0.0f;
        for (int i = 0; i < 3; i++) xs[i] = norm * S[i], x[i] = norm * std::max(R[i], 0.0f);
        split = GpuLampLaw::IndoorSplit(xs, p, k, rampAmb, ramp);
        if (!split) rampAmb[0] = rampAmb[1] = rampAmb[2] = ramp = 0.0f;
        if (ambScale)
            for (int i = 0; i < 3; i++) rampAmb[i] *= ambScale[i];
        GpuLampLaw::IndoorTexel(x, rampAmb, ramp, k, out);
        for (int i = 0; i < 4; i++) out[i] = std::isfinite(out[i]) ? std::clamp(out[i], 0.0f, 1.0f) : 0.0f;
        if (out[3] < alphaMin) out[3] = alphaMin;
    } else
        OutdoorDeltaModel::PackInput(R, p[3], out);
    if (dither != 0.0f) OutdoorDeltaModel::Dither(out, dither);
    return split;
}
// (S8) KeptTexel's ambScale: the room's +0x110 now over the one at its solve's end, per channel (x / x is exactly 1). The ceiling
// pass adds the sampler's copy (+0x650), equal to +0x110 at the solve's end. shortcut: a channel the solve left at 0 (or not
// finite) keeps 1, since its ramp x ambient read back is 0 there; upgrade by keeping the ramp and a per-texel "the pass adds the
// ambient" flag if a room with a 0 ambient channel ever needs it
inline void AmbScale(const float solve[3], const float now[3], float out[3]) {
    for (int i = 0; i < 3; i++) {
        const float s = now[i] / solve[i];
        out[i] = solve[i] > 1e-6f && std::isfinite(s) && s >= 0.0f ? s : 1.0f;
    }
}
// What one GPU lamp adds to a texel at p (normal n) with shadow share vis: mult x fade(law x vis)
inline void Add(const Lamp& L, const float p[3], const float n[3], float vis, bool reachOn, float thr, bool fade, float out[3]) {
    float law[3];
    GpuLampLaw::Eval(L.law, p, n, GpuLampLaw::WallReach(n, reachOn), law);
    float c[3] = {law[0] * vis, law[1] * vis, law[2] * vis};
    Fade(c, thr, fade);
    for (int i = 0; i < 3; i++) out[i] += L.mult * c[i];
}
// The floor mask's two luminances of one GPU lamp at a floor texel (N.l ignored: the wrapped normal; no reach on floors; the
// threshold drop on the evaluation, no fade)
inline void AddMask(const Lamp& L, const float p[3], float vis, float thr, float& visLum, float& allLum) {
    GpuLampLaw::Lamp l = L.law;
    float law[3];
    // N.l ignored: a normal along the lamp direction gives sat(N.l) = 1
    float d[3] = {l.pos[0] - p[0], l.pos[1] - p[1], l.pos[2] - p[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len > 1e-6f)
        for (float& v : d) v /= len;
    else
        d[0] = 0, d[1] = 1, d[2] = 0;
    GpuLampLaw::Eval(l, p, d, 0.0f, law);
    if (!(thr <= (law[2] + law[1]) + law[0])) return;
    const float lum = OutdoorDeltaModel::Lum3(law);
    allLum += L.mult * lum;
    visLum += L.mult * lum * vis;
}

// The terrain bake's stamp of a lamp at a floor point (LevelLightShare UnderFloorShare): 0.4 range intensity h / d^3, at most
// 1, times the colour summed (+0xF0; a cone lamp at its spill when the bake stub does it), h = head - lot base
struct BakeLamp {
    float head[3] = {};
    float range = 0.0f, intensity = 0.0f;
    float colour = 0.0f; // sum of the colour channels the bake takes
    // 1: a lot lamp the bake stamps from kLowLampLift when it lies on the ground (TerrainBakeRule::Liftable). A 32-bit field:
    // the lamps are compared with memcmp and hashed whole (RtOutdoorLamps UnderDelta, the bake signature), so the struct
    // must have no padding (2.15.1 had a bool here: its padding bytes made every lamp look changed every frame, and the
    // under-floor share of every floor texel was computed again each frame while the camera moved)
    uint32_t liftable = 1;
};
static_assert(sizeof(BakeLamp) == 7 * sizeof(float), "BakeLamp is compared and hashed byte for byte: no padding");
// One lamp's stamp at s (v > 0) and whether its head is at most eps over s (under); false when it stamps nothing there
inline bool BakeStamp(const BakeLamp& L, const float s[3], float base, float eps, float& v, bool& under) {
    const float h = TerrainBakeRule::StampHeight(L.head[1] - base, L.liftable != 0); // a lamp on the lawn: from kLowLampLift, as the bake
    if (!(h > 0.0f)) return false;
    const float dx = L.head[0] - s[0], dz = L.head[2] - s[2];
    const float d2 = dx * dx + h * h + dz * dz;
    float stamp = 0.4f * L.range * L.intensity * h / (d2 * std::sqrt(d2));
    if (!(stamp > 0.0f)) return false;
    if (stamp > 1.0f) stamp = 1.0f;
    v = L.colour * stamp;
    if (!(v > 0.0f)) return false;
    under = L.head[1] <= s[1] + eps;
    return true;
}
// BakeStamp at 4 points (x, y, z by lane): the same single-precision operations in the same order, the distance and the
// stamp 4 at a time (SSE), so each lane equals BakeStamp bit for bit (/fp:precise, no FMA; rt_lamps_test checks it).
// ok[k] is false where BakeStamp returns false
inline void BakeStamp4(const BakeLamp& L, const float sx[4], const float sy[4], const float sz[4], float base, float eps, float v[4], bool under[4],
                       bool ok[4]) {
    const float h = TerrainBakeRule::StampHeight(L.head[1] - base, L.liftable != 0);
    if (!(h > 0.0f)) {
        ok[0] = ok[1] = ok[2] = ok[3] = false;
        return;
    }
    const __m128 dx = _mm_sub_ps(_mm_set1_ps(L.head[0]), _mm_loadu_ps(sx)), dz = _mm_sub_ps(_mm_set1_ps(L.head[2]), _mm_loadu_ps(sz));
    const __m128 d2 = _mm_add_ps(_mm_add_ps(_mm_mul_ps(dx, dx), _mm_set1_ps(h * h)), _mm_mul_ps(dz, dz));
    float stamp[4];
    _mm_storeu_ps(stamp, _mm_div_ps(_mm_set1_ps(0.4f * L.range * L.intensity * h), _mm_mul_ps(d2, _mm_sqrt_ps(d2))));
    for (int k = 0; k < 4; k++) {
        ok[k] = false;
        float st = stamp[k];
        if (!(st > 0.0f)) continue;
        if (st > 1.0f) st = 1.0f;
        v[k] = L.colour * st;
        if (!(v[k] > 0.0f)) continue;
        under[k] = L.head[1] <= sy[k] + eps;
        ok[k] = true;
    }
}
// The share of the stamps at s from lamps whose head is at most eps over s; -1 when nothing stamps there
inline float UnderShare(const BakeLamp* lamps, int n, const float s[3], float base, float eps) {
    float all = 0.0f, under = 0.0f;
    for (int i = 0; i < n; i++) {
        float v;
        bool u;
        if (!BakeStamp(lamps[i], s, base, eps, v, u)) continue;
        all += v;
        if (u) under += v;
    }
    return all > 1e-4f ? std::clamp(under / all, 0.0f, 1.0f) : -1.0f;
}

// One relit texel, packed as the solve packs it (the dither offset d in levels / 255; masked: the floor mask's alpha from its
// luminances, gated: under applies)
struct TexelIn {
    float base[3] = {};
    float alpha = 0.0f; // the packed alpha of a texel that is not masked (the record's)
    bool masked = false, gated = false;
    float baseVis = 0.0f, baseAll = 0.0f, under = -1.0f;
    float dither = 0.0f; // levels / 255 (RoomMapDither::Offset / 255), 0 when the dither is off
    // (GPU room lamps) a story-0 ground texel whose alpha is the exit lamps' light: ExitAlpha(exitBase + gExit), exitBase the
    // luminance of the exit lamps' light left in the base
    bool exitGround = false;
    float exitBase = 0.0f;
};
inline uint32_t Compose(const TexelIn& t, const float G[3], float gVis, float gAll, float gExit = 0.0f) {
    float S[3] = {t.base[0] + G[0], t.base[1] + G[1], t.base[2] + G[2]};
    float alpha = t.alpha;
    if (t.masked) alpha = OutdoorDeltaModel::MaskAlpha(t.baseVis + gVis, t.baseAll + gAll, t.gated ? t.under : -1.0f);
    else if (t.exitGround) alpha = ExitAlpha(t.exitBase + gExit);
    float c[4];
    OutdoorDeltaModel::PackInput(S, alpha, c);
    if (t.dither != 0.0f) OutdoorDeltaModel::Dither(c, t.dither);
    return OutdoorDeltaModel::PackModel(c);
}

} // namespace RtLampModel
