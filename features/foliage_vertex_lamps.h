#pragma once
// Per-vertex lamp light on instanced foliage (numbered development builds, switch foliage_vertex_lamps).
//
// The foliage vertex shaders (vs_2_0, docs/features/night-lighting/foliage.md) light every leaf of an instance with one
// colour per lamp (c54..c56[a0.w]) computed at the rig's box point nearest the lamp, and one direction (c27..c29[a0.w]): a
// leaf at the top of a crown is as bright as one next to a lamp at ground height (a lighting snapshot: a red garden
// lantern of 6.3 m reach lit a whole bare crown red up to 15 m away). Patch (applied to the copy
// ShaderPatches::PatchFoliageVs made), for each of the three lamps, per vertex:
//   - the real direction and distance from the vertex to the lamp head; Lambert N.l with the wrap constants c255;
//   - the reach law (LampReach::PixelLaw) at the vertex, as the per-pixel lamps give it there, capped so no channel passes 1;
//   - leaf transmission: tau/rho x max(-N.l, 0) (thin green leaves: reflectance about 0.12, transmittance about 0.10 at
//     550 nm, PROSPECT, Jacquemoud & Baret 1990; the pixel shader multiplies by the leaf colour, so the light through a
//     leaf is tinted by it);
//   - the crown's own leaves on the way: Beer-Lambert along the vertex -> lamp segment inside the crown ellipsoid,
//     extinction G x Omega x leaf area density (foliage_canopy.h);
// and, for the first (strongest) lamp only, light reflected by the ground under the vertex: the ground's irradiance from
// the lamp x rho_g / 2 x (1 - n.y) (Lambertian plane view factor; grass reflectance about 0.15 in the visible).
// The sun, the sky, the fog and every other output are untouched.
//
// Constants (B = 1 + the highest constant the shader uses; 130 in the captured VS_9A10A3A8; stride 3 per instance so the
// shader's own instance index a0.w addresses them like c0 / c27 / c54):
//   c[B + 3i + k]       lamp k of instance i: xyz head, camera-relative; w = r0^2
//   c[B + 27 + 3i + k]  x = 1/R^2, y = scale (0: lamp unused), z = minimum d^2, w = head height over the instance's ground
//   c[B + 54 + 3i]      crown centre, camera-relative; w = 1 / horizontal semi-axis
//   c[B + 55 + 3i]      x = 1 / vertical semi-axis, y = extinction x leaf area density / ln 2 (exp is base 2)
//   c[B + 56 + 3i]      x, y, z = the cap of lamps 0, 1, 2 (1 / the brightest channel of the colour); w = tau/rho (0: none)
//   c[B + 81]           x = tau/rho, y = rho_g / 2, w = 1
// The draw sets every one of them or does not use the patched copy. It also replaces the per-instance colours c54..c80 with
// the instance's own lamps (OwnLamp below; F0 x intensity x fade x the cone and the walls' share), so the vertex light of
// lamp k is colour x min(law(d) x gain x scale, 1 / the colour's brightest channel) x the terms above. Credits: @loinyx
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "lamp_reach.h"

namespace FoliageVertexLamps {

constexpr float kTransmitOverReflect = 0.83f; // tau / rho, green leaf at 550 nm
constexpr float kGroundHalfAlbedo = 0.075f;   // rho_g / 2, grass in the visible (0.15)

// Patches the token stream in place (false: unchanged; the shader is not of the captured family or would not fit
// vs_2_0). base: B above.
bool Patch(std::vector<uint32_t>& tokens, int& base);
// Why the last Patch or PatchTreeSunShadow refused (log only; "" after a patch). Render thread.
const char* LastRefusal();

// The sun's shadow on trees . The game's tree pixel shaders (8 in Shaders_Win32.precomp, ps_2_0: sun = sat(t1.z)
// x c0, lamps, ambient cube) never read the sun shadow map, although the foliage vertex shader writes the shadow position
// in t5 and the shadow map's edge fade in t6.x (a captured draw: VS c105..c108 = the shadow matrix of the
// draws that sample s5 in the same frame; the probe saw no s5 on the tree): no tree is shaded by another tree, a house or
// itself. The patch (before the sun term "mad rS.xyz, rW.y, c0, rX", rW from "mov_sat rW.xy, t1.wzyx") reads the shadow
// map on s5 at four taps half a texel around t5 (texldp: the hardware depth compare, as the game's bushes do), fades it to
// 1 at the map's edge (t6.x) and keeps a floor for the light that passes a crown (foliage_canopy.h: a 4 m crown keeps about
// 0.23): sun x (floor + (1 - floor) x shadow). Constants (base B = 1 + the highest the shader uses):
//   c[B .. B + 3] = the four taps (-o, -o), (o, -o), (-o, o), (o, o) with z = -bias, w = 0; o = half a texel of the map
//   c[B + 4]      = (1/4, 1, 1 - floor, floor)
// The draw binds the frame's shadow map to s5 and sets both, or keeps the game's shader.
bool PatchTreeSunShadow(std::vector<uint32_t>& tokens, int& base);
constexpr float kTreeShadowFloor = 0.25f; // light through a crown in its own or another's shadow (gap fraction 0.1 to 0.35)

// The law of a lamp on foliage (in game: a street lamp lit the pavement and planters under it and left the trees a
// few metres away dark): plain inverse square, Gain / d^2 (the base of the game's own ground law, 0.25 w per list entry
// over d^2), faded out smoothly near the lamp's reach R only: min(1, R^2 / d^2 x (1 - (d / R)^4)^2), 1 up to about 0.7 R and
// 0 from R on. The outdoor walls' law (LampReach::PixelLaw) keeps r0^2 / d^2 in that min, which falls as 1 / d^4 past r0
// (2.5 to 3.7 m for a street lamp): a crown 6 m from one took a sixth of the inverse square. In the shader this is the walls'
// law with r0 = R (SlotConstants: r0sq = 1 / invR2).
inline float FoliageLaw(float d2, float w, float entries, float minD2 = 1e-4f) {
    float r0, R;
    LampReach::Radii(w, r0, R);
    d2 = std::max(d2, minD2);
    const float q = d2 / (R * R), win = std::clamp(1.0f - q * q, 0.0f, 1.0f);
    return LampReach::Gain(w, entries) / d2 * std::min(1.0f, R * R / d2 * win * win);
}

// Squared distance from p to the box centre +- ext (0 inside)
inline float BoxDistance2(const float* p, const float* centre, const float* ext) {
    float d2 = 0.0f;
    for (int k = 0; k < 3; k++) {
        const float d = std::max(std::fabs(p[k] - centre[k]) - ext[k], 0.0f);
        d2 += d * d;
    }
    return d2;
}

// One lamp of a foliage instance (the instance picks its own lamps, as the per-pixel lamps of outdoor objects do,
// instead of following the rig's three slots): head, colour (F0 x intensity x fade x the cone and the walls' share at the
// crown's middle), the reach law's r0^2, 1/R^2 and gain (LampReach), and the light it gives at the mesh box (rank only).
struct OwnLamp {
    float pos[3] = {}, colour[3] = {};
    float r0sq = 0.0f, invR2 = 0.0f, gain = 0.0f, score = 0.0f;
    uint64_t key = 0;   // the lamp's row without its colour (place, reach, cone), to take a new colour without a new pick
    float share = 0.0f; // the cone and the walls' share the colour was multiplied by
};
// Keeps the three brightest lamps (score) in out[0..2], brightest first (the ground bounce takes slot 0). Returns the count.
inline int KeepBrightest(const OwnLamp& l, OwnLamp* out, int n) {
    if (!(l.score > 0.0f) || !std::isfinite(l.score)) return n;
    int at = n < 3 ? n : 3;
    while (at > 0 && out[at - 1].score < l.score) at--;
    if (at >= 3) return n;
    for (int k = std::min(n, 2); k > at; k--) out[k] = out[k - 1];
    out[at] = l;
    return std::min(n + 1, 3);
}
// The shader's constants for one picked lamp (see the layout above): the head relative to the origin of c0 (eye) with
// r0^2; 1/R^2, gain x the reach law's scale, the minimum d^2, the head's height over the instance's ground; the cap (no
// channel of colour x weight passes 1: the hue kept).
inline void SlotConstants(const OwnLamp& l, const float* eye, float groundY, float lawScale, float lp[4], float law[4], float& cap) {
    for (int a = 0; a < 3; a++) lp[a] = l.pos[a] - eye[a];
    lp[3] = l.r0sq;
    law[0] = l.invR2, law[1] = l.gain * lawScale, law[2] = 1e-4f, law[3] = std::max(l.pos[1] - groundY, 0.0f);
    const float top = std::max({l.colour[0], l.colour[1], l.colour[2]});
    cap = top > 0.0f ? 1.0f / top : 1e30f;
}

// The camera the game drew an instanced foliage draw with (in game: lamp light on trees moved with the camera and
// crowns blinked). The instance positions c0[3i] are relative to the eye the game built the frame with; the eye Apex reads
// from the game's camera (LotLightingMotion::CameraEye) can differ from it while the camera moves, which shifted every
// lamp against the leaves by that difference and changed the instances' keys (new picks every frame). Trees do not move:
// their world positions are learned while the camera is still (both eyes agree), and while it moves the offset that puts a
// draw's instances back on their learned places corrects the eye. Pure; render thread.
class EyeAnchors {
public:
    static constexpr float kCell = 2.0f;     // m, grid of the learned places
    static constexpr float kMatch = 2.5f;    // m, the largest drift looked for
    static constexpr float kAgree = 0.3f;    // m, the instances of a draw must agree on the offset within this
    static constexpr size_t kMax = 60000;    // learned places before starting over

    // A place seen while the camera is still (w: world position; mesh: the mesh's id; scale: the instance's)
    void Learn(uint32_t mesh, float scale, const float w[3]) {
        auto& v = m_cells[Cell(w)];
        for (Anchor& a : v)
            if (a.mesh == mesh && std::fabs(a.scale - scale) < 1e-3f && Dist2(a.pos, w) < 0.05f * 0.05f) return;
        if (m_count >= kMax) m_cells.clear(), m_count = 0;
        m_cells[Cell(w)].push_back({mesh, scale, {w[0], w[1], w[2]}});
        m_count++;
    }
    // The offset to add to the eye so the n instances (rel: c0, relative to the eye the game used; eye: the eye read) land
    // on their learned places: false when none is known or they disagree
    bool Offset(uint32_t mesh, const float* scales, const float (*rel)[3], int n, const float eye[3], float out[3]) const {
        float d[9][3];
        int found = 0;
        for (int i = 0; i < n && i < 9; i++) {
            const float w[3] = {rel[i][0] + eye[0], rel[i][1] + eye[1], rel[i][2] + eye[2]};
            const int cx = CellOf(w[0]), cy = CellOf(w[1]), cz = CellOf(w[2]);
            float best = kMatch * kMatch;
            const Anchor* hit = nullptr;
            for (int x = -1; x <= 1; x++)
                for (int y = -1; y <= 1; y++)
                    for (int z = -1; z <= 1; z++) {
                        const auto it = m_cells.find(Key(cx + x, cy + y, cz + z));
                        if (it == m_cells.end()) continue;
                        for (const Anchor& a : it->second) {
                            if (a.mesh != mesh || std::fabs(a.scale - scales[i]) > 1e-3f) continue;
                            const float q = Dist2(a.pos, w);
                            if (q < best) best = q, hit = &a;
                        }
                    }
            if (!hit) continue;
            for (int k = 0; k < 3; k++) d[found][k] = hit->pos[k] - w[k];
            found++;
        }
        if (!found) return false;
        for (int k = 0; k < 3; k++) { // the median per axis, then every instance within kAgree of it
            float s[9];
            for (int i = 0; i < found; i++) s[i] = d[i][k];
            std::nth_element(s, s + found / 2, s + found);
            out[k] = s[found / 2];
        }
        for (int i = 0; i < found; i++)
            for (int k = 0; k < 3; k++)
                if (std::fabs(d[i][k] - out[k]) > kAgree) return false;
        return true;
    }
    size_t Count() const { return m_count; }

private:
    struct Anchor {
        uint32_t mesh;
        float scale;
        float pos[3];
    };
    static float Dist2(const float* a, const float* b) {
        const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
        return x * x + y * y + z * z;
    }
    static int CellOf(float v) { return static_cast<int>(std::floor(v / kCell)); }
    static uint64_t Key(int x, int y, int z) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x) & 0x1FFFFF) << 42) | (static_cast<uint64_t>(static_cast<uint32_t>(y) & 0x1FFFFF) << 21) |
               (static_cast<uint64_t>(static_cast<uint32_t>(z) & 0x1FFFFF));
    }
    static uint64_t Cell(const float* w) { return Key(CellOf(w[0]), CellOf(w[1]), CellOf(w[2])); }
    std::unordered_map<uint64_t, std::vector<Anchor>> m_cells;
    size_t m_count = 0;
};

} // namespace FoliageVertexLamps
