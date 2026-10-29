#pragma once
// Lamp cones in the terrain light bake (part of Night Lighting; patches/night_terrain_relight_patch.cpp).
//
// The game's bake pixel shader (technique StaticTerrainLightmap, ps_2_0, 576 bytes) stamps every light all round: a spot
// (type 4) lit the ground behind it, and a shaded lamp (type 5), stamped at its spill since 2.15, left the pool under its
// shade to the lot's room map, so the pool stopped in a straight line at the lot border. Apex's copy of the shader
// (shaders/terrain_bake_ps.hlsl BakeConePS) is the same law times the cones the room solve gives the lamp
// (gpu_lamp_law.h: t from the axis and the two cone values, blended with the spill), read from c5..c7, which the bake's
// position stub writes for every light it bakes. StampPS (the dragged lamps on the GPU) takes the same rows. Pure
// functions, no game reads (tools/terrain_stamp_test includes this header).
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace TerrainBakeCone {

constexpr size_t kGamePsBytes = 576;
constexpr uint64_t kGamePsFnv = 0x73c24b1d8d01e7d4ull; // FNV-1a 64 of the game's bake pixel shader (Steam 1.67.2)
constexpr float kMagic = 1234.5f; // c7.w while the stub writes the rows: other values leave the game's law (cones off)

inline uint64_t Fnv(const void* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
    return h;
}

// What the light holds for its cones (light-objects-and-rigs.md): a spot's axis +0x170, offset +0x158, scale +0x154; a
// shaded lamp's axes +0x1A0 / +0x190, cone values +0x174 / +0x170, spill +0x150
struct Fields {
    int type = 0;
    float axis4[3] = {}, off4 = 0.0f, scale4 = 0.0f;
    float a1[3] = {}, a2[3] = {}, o1 = 1.0f, o2 = 1.0f, S[3] = {};
};

// The rows c5..c7 for one light (s5: the game's shaded-lamp cone scale [0x011D11A0]):
//   c5 = (axis, sc1), c6 = (off1, sc2, off2, 0), c7 = (spill rgb, kMagic)
// as GpuLampLaw's SpotCone / ShadedCone / NoCone. A shaded lamp whose axes are not opposite (the GPU law has no cone for
// it) keeps its spill all round, as the bake had it.
inline void Rows(const Fields& f, float s5, float r[3][4]) {
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 4; k++) r[i][k] = 0.0f;
    r[0][3] = 1.0f, r[1][0] = -1.0f; // no cone: t = 1
    r[2][3] = kMagic;
    if (f.type == 4 && std::isfinite(f.off4) && std::isfinite(f.scale4)) {
        for (int i = 0; i < 3; i++) r[0][i] = f.axis4[i];
        r[0][3] = f.scale4, r[1][0] = f.off4;
    } else if (f.type == 5) {
        const bool opposite = std::fabs(f.a1[0] + f.a2[0]) + std::fabs(f.a1[1] + f.a2[1]) + std::fabs(f.a1[2] + f.a2[2]) < 1e-3f;
        for (int i = 0; i < 3; i++) r[2][i] = std::isfinite(f.S[i]) ? f.S[i] : 0.0f;
        if (opposite && std::isfinite(f.o1) && std::isfinite(f.o2)) {
            for (int i = 0; i < 3; i++) r[0][i] = f.a1[i];
            r[0][3] = s5, r[1][0] = f.o1, r[1][1] = s5, r[1][2] = f.o2;
        } else
            r[0][3] = 0.0f, r[1][0] = 0.0f; // t = 0: the spill
    }
}

// The cone factor per channel at direction l (unit, from the point to the light), as BakeConePS and StampPS compute it
// (GpuLampLaw::Eval's t and S - S t + t)
inline void Factor(const float r[3][4], const float l[3], float out[3]) {
    const float al = r[0][0] * l[0] + r[0][1] * l[1] + r[0][2] * l[2];
    const float t1 = (al + r[1][0]) * -r[0][3], t2 = (al - r[1][2]) * r[1][1];
    const float tsel = t1 > 0.0f ? t1 : t2;
    const float m = std::fmin(std::fmax((std::fmax(t1, t2) - 1.0f) * 1e4f + 1.0f, 0.0f), 1.0f);
    const float t = std::fmin(std::fmax(std::fmax(tsel, m), 0.0f), 1.0f);
    for (int c = 0; c < 3; c++) out[c] = r[2][c] - r[2][c] * t + t;
}

} // namespace TerrainBakeCone
