#pragma once
// Real-time GI, work package S0-01: the CPU reference of the indirect data model (docs/features/realtime-gi/design.md §3.3,
// research 04 §1.5, 12 §4.1). Pure functions over the occluder proxy (rt_lamp_proxy.h) and the GPU lamp law; no device.
//
//   - Patches: one a floor tile (1 m², the layer texel: 1 texel a tile and story), one a roofed tile's ceiling, and the wall
//     pieces (1 m wide along the edge, about 1 m tall, both faces, opening holes left out). A patch's point sits kOff off its
//     surface along its normal.
//   - Links: patch-to-patch form factors A cos cos / (pi d² + A) with one ray of visibility against the proxy (Clear). A wall
//     quad between two points leaves no link, so a closed room gets exactly 0 from a lamp outside it, at every bounce.
//   - Solve (one lamp layer, unit colour): direct irradiance from the lamp law x the proxy's shadow share, then kBounces
//     Jacobi steps of E = d + F (albedo E). The layer is the indirect part F (albedo E) at the floor patches.
//   - Compose: sum(on x colour x layer); a toggle, dim or recolour re-composes, it never solves again.
//   - Upsample: bilinear over the four nearest tile centres of the story, a centre dropped when the proxy blocks the segment
//     from it to the point (no blend across a wall), the rest renormalised.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "features/rt_lamp_model.h"
#include "features/rt_lamp_proxy.h"

namespace GiRef {

constexpr float kOff = 0.05f;    // m: a patch's point off its surface
constexpr float kAlbedo = 0.5f;  // shortcut: one grey reflectance for every surface, until S0-12 names a per-room colour
constexpr float kReach = 12.0f;  // m: no link between patches farther apart (keeps a 64 x 64 lot's link count bounded)
constexpr int kBounces = 16;     // albedo^16 < 2e-5: converged far below 1/255
constexpr float kLampBall = 0.15f;
constexpr int kLampRays = 16;
using Rgb = std::array<float, 3>;

// A segment the proxy lets through. Clear alone is not watertight: a segment through the shared edge of two wall quads (tile
// centre to tile centre crosses a wall exactly at a tile corner) can miss both triangles, which let a closed room's floor see
// its neighbour's. A second segment shifted by kNudge (off every grid line) has to be clear too
constexpr float kNudge[3] = {1.3e-3f, 0.7e-3f, 0.9e-3f};
inline bool Seen(const RtLampProxy::Mesh& mesh, const float a[3], const float b[3], float skipA, float skipB, std::vector<uint32_t>& stamp, uint32_t& mark) {
    const auto next = [&] {
        if (++mark == 0) std::fill(stamp.begin(), stamp.end(), 0u), mark = 1;
        return mark;
    };
    if (!RtLampProxy::Clear(mesh, a, b, skipA, skipB, &stamp, next())) return false;
    const float a2[3] = {a[0] + kNudge[0], a[1] + kNudge[1], a[2] + kNudge[2]}, b2[3] = {b[0] + kNudge[0], b[1] + kNudge[1], b[2] + kNudge[2]};
    return RtLampProxy::Clear(mesh, a2, b2, skipA, skipB, &stamp, next());
}
// The share of the lamp ball (centre L, radius kLampBall) P sees: RtLampProxy::Visibility's stratified disc, with no skip at the
// emitter end. (Visibility skips 1 cm at each end of the point-to-P segment: a disc point within 1 cm of a ceiling, as for a lamp
// hung 5 cm under it, lit the story above through the ceiling.)
inline float LampShare(const RtLampProxy::Mesh& mesh, const float L[3], const float P[3], std::vector<uint32_t>& stamp, uint32_t& mark) {
    float d[3] = {P[0] - L[0], P[1] - L[1], P[2] - L[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-4f)) return 1.0f;
    for (float& v : d) v /= len;
    const float t[3] = {std::fabs(d[1]) < 0.9f ? 0.0f : 1.0f, std::fabs(d[1]) < 0.9f ? 1.0f : 0.0f, 0.0f};
    float u[3] = {d[1] * t[2] - d[2] * t[1], d[2] * t[0] - d[0] * t[2], d[0] * t[1] - d[1] * t[0]};
    const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (float& v : u) v /= ul;
    const float w[3] = {d[1] * u[2] - d[2] * u[1], d[2] * u[0] - d[0] * u[2], d[0] * u[1] - d[1] * u[0]};
    int clear = 0, emit = 0;
    for (int i = 0; i < kLampRays; i++) {
        const float r = kLampBall * std::sqrt((i + 0.5f) / kLampRays), a = i * 2.39996323f, ca = std::cos(a) * r, sa = std::sin(a) * r;
        const float q[3] = {L[0] + u[0] * ca + w[0] * sa, L[1] + u[1] * ca + w[1] * sa, L[2] + u[2] * ca + w[2] * sa};
        if (!Seen(mesh, L, q, 0.0f, 0.0f, stamp, mark)) continue; // behind a surface from the lamp's centre: no light there
        emit++;
        clear += Seen(mesh, q, P, 0.0f, 0.01f, stamp, mark);
    }
    if (!emit) return Seen(mesh, L, P, 0.0f, 0.01f, stamp, mark) ? 1.0f : 0.0f;
    return static_cast<float>(clear) / emit;
}

struct Patch {
    float p[3], n[3], area;
    int story;
    uint8_t kind; // 0 floor (a layer texel), 1 ceiling, 2 wall
};
struct StoryIndex {
    int story = 0, w = 0, h = 0;
    std::vector<int> texel; // per tile: its layer texel, -1 none
};
struct Model {
    std::vector<Patch> patches;
    std::vector<int> texels; // the layer's texels: their patch index
    std::vector<StoryIndex> stories;
    std::vector<uint32_t> start; // CSR: patch i gathers links[start[i] .. start[i + 1])
    std::vector<std::pair<uint32_t, float>> links;
};

inline void Build(const RtLampProxy::Lot& lot, const RtLampProxy::Mesh& mesh, Model& m) {
    using namespace RtLampProxy;
    m = Model{};
    for (const Story& s : lot.stories) {
        StoryIndex si{s.story, s.w, s.h, std::vector<int>(static_cast<size_t>(s.w) * s.h, -1)};
        for (int iz = 0; iz < s.h; iz++)
            for (int ix = 0; ix < s.w; ix++) {
                const size_t t = static_cast<size_t>(iz) * s.w + ix;
                const float x = ix + 0.5f, z = iz + 0.5f;
                if (t < s.floorY.size() && s.floorY[t] < kNoFloor) {
                    si.texel[t] = static_cast<int>(m.texels.size());
                    m.texels.push_back(static_cast<int>(m.patches.size()));
                    m.patches.push_back({{x, s.floorY[t] + kOff, z}, {0, 1, 0}, 1.0f, s.story, 0});
                }
                if (t < s.roofed.size() && s.roofed[t] && t < s.ceil.size() && s.ceil[t] < kNoFloor)
                    m.patches.push_back({{x, s.ceil[t] - kOff, z}, {0, -1, 0}, 1.0f, s.story, 1});
            }
        m.stories.push_back(std::move(si));
        for (const Edge& e : s.edges) {
            const float dx = e.x1 - e.x0, dz = e.z1 - e.z0, len = std::sqrt(dx * dx + dz * dz);
            if (!(len > 1e-4f)) continue;
            const float nx = -dz / len, nz = dx / len;
            const int nu = std::max(1, static_cast<int>(std::lround(len)));
            for (int iu = 0; iu < nu; iu++) {
                const float u = (iu + 0.5f) / nu, top = EdgeTop(e, u), h = top - e.foot;
                const int nv = std::max(1, static_cast<int>(std::lround(h)));
                for (int iv = 0; iv < nv; iv++) {
                    const float y = e.foot + h * (iv + 0.5f) / nv;
                    if (e.open && u > e.o.u0 && u < e.o.u1 && y > e.base + e.o.sill && y < e.base + OpeningHead(e.o, u)) continue;
                    for (int sd = -1; sd <= 1; sd += 2)
                        m.patches.push_back({{e.x0 + dx * u + nx * kOff * sd, y, e.z0 + dz * u + nz * kOff * sd}, {nx * sd, 0, nz * sd}, len / nu * h / nv, s.story, 2});
                }
            }
        }
    }
    // links: geometry only, shared by every lamp
    std::vector<uint32_t> stamp(mesh.tris.size(), 0);
    uint32_t mark = 0;
    m.start.assign(m.patches.size() + 1, 0);
    for (size_t i = 0; i < m.patches.size(); i++) {
        const Patch& r = m.patches[i];
        for (size_t j = 0; j < m.patches.size(); j++) {
            if (i == j) continue;
            const Patch& e = m.patches[j];
            const float d[3] = {e.p[0] - r.p[0], e.p[1] - r.p[1], e.p[2] - r.p[2]};
            const float d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            if (d2 > kReach * kReach || d2 < 1e-8f) continue;
            const float inv = 1.0f / std::sqrt(d2);
            const float cr = (r.n[0] * d[0] + r.n[1] * d[1] + r.n[2] * d[2]) * inv, ce = -(e.n[0] * d[0] + e.n[1] * d[1] + e.n[2] * d[2]) * inv;
            if (cr <= 0.0f || ce <= 0.0f) continue;
            if (!Seen(mesh, r.p, e.p, 0.01f, 0.01f, stamp, mark)) continue;
            m.links.emplace_back(static_cast<uint32_t>(j), e.area * cr * ce / (3.14159265f * d2 + e.area));
        }
        m.start[i + 1] = static_cast<uint32_t>(m.links.size());
    }
}

// The lamp's direct irradiance on every patch, unit colour (the law's colour set to 1; the compose applies the real one)
inline void Direct(const Model& m, const RtLampProxy::Mesh& mesh, RtLampModel::Lamp L, std::vector<Rgb>& d) {
    for (float& c : L.law.colour) c = 1.0f;
    d.assign(m.patches.size(), Rgb{});
    std::vector<uint32_t> stamp(mesh.tris.size(), 0);
    uint32_t mark = 0;
    for (size_t i = 0; i < m.patches.size(); i++) {
        const Patch& P = m.patches[i];
        float c[3] = {};
        RtLampModel::Add(L, P.p, P.n, 1.0f, false, 0.0f, false, c);
        if (!(c[0] > 0.0f || c[1] > 0.0f || c[2] > 0.0f)) continue;
        const float vis = LampShare(mesh, L.law.pos, P.p, stamp, mark);
        for (int k = 0; k < 3; k++) d[i][k] = c[k] * vis;
    }
}

// The indirect irradiance on every patch from direct irradiance d, all bounces (kBounces Jacobi steps)
inline void Bounce(const Model& m, const std::vector<Rgb>& d, std::vector<Rgb>& ind) {
    const size_t n = m.patches.size();
    ind.assign(n, Rgb{});
    std::vector<Rgb> next(n);
    for (int b = 0; b < kBounces; b++) {
        for (size_t i = 0; i < n; i++) {
            Rgb s{};
            for (uint32_t k = m.start[i]; k < m.start[i + 1]; k++) {
                const auto& [j, f] = m.links[k];
                for (int c = 0; c < 3; c++) s[c] += f * kAlbedo * (d[j][c] + ind[j][c]);
            }
            next[i] = s;
        }
        ind.swap(next);
    }
}

// One lamp's layer: the indirect light at the layer texels, unit colour
inline void Layer(const Model& m, const RtLampProxy::Mesh& mesh, const RtLampModel::Lamp& L, std::vector<Rgb>& layer) {
    std::vector<Rgb> d, ind;
    Direct(m, mesh, L, d);
    Bounce(m, d, ind);
    layer.resize(m.texels.size());
    for (size_t t = 0; t < m.texels.size(); t++) layer[t] = ind[m.texels[t]];
}

// sum(on x colour x layer)
inline void Compose(const std::vector<std::vector<Rgb>>& layers, const std::vector<bool>& on, const std::vector<Rgb>& colour, std::vector<Rgb>& out) {
    out.assign(layers.empty() ? 0 : layers[0].size(), Rgb{});
    for (size_t l = 0; l < layers.size(); l++)
        if (on[l])
            for (size_t t = 0; t < out.size(); t++)
                for (int c = 0; c < 3; c++) out[t][c] += colour[l][c] * layers[l][t][c];
}

// The full solve the compose stands for: every lit lamp's direct light in colour, one bounce solve
inline void FullSolve(const Model& m, const RtLampProxy::Mesh& mesh, const std::vector<RtLampModel::Lamp>& lamps, const std::vector<bool>& on,
                      const std::vector<Rgb>& colour, std::vector<Rgb>& out) {
    std::vector<Rgb> d(m.patches.size(), Rgb{}), dl, ind;
    for (size_t l = 0; l < lamps.size(); l++) {
        if (!on[l]) continue;
        Direct(m, mesh, lamps[l], dl);
        for (size_t i = 0; i < d.size(); i++)
            for (int c = 0; c < 3; c++) d[i][c] += colour[l][c] * dl[i][c];
    }
    Bounce(m, d, ind);
    out.resize(m.texels.size());
    for (size_t t = 0; t < m.texels.size(); t++) out[t] = ind[m.texels[t]];
}

// The composed value at a floor point (x, z) of a story (its height from the tile texels): wall-aware bilinear
inline Rgb Upsample(const Model& m, const RtLampProxy::Mesh& mesh, const std::vector<Rgb>& v, int story, float x, float y, float z) {
    const StoryIndex* s = nullptr;
    for (const StoryIndex& si : m.stories)
        if (si.story == story) s = &si;
    Rgb out{};
    if (!s) return out;
    const float fx = x - 0.5f, fz = z - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx)), z0 = static_cast<int>(std::floor(fz));
    const float ax = fx - x0, az = fz - z0;
    const float P[3] = {x, y + kOff, z};
    float wsum = 0.0f;
    std::vector<uint32_t> stamp(mesh.tris.size(), 0);
    uint32_t mark = 0;
    for (int k = 0; k < 4; k++) {
        const int ix = x0 + (k & 1), iz = z0 + (k >> 1);
        if (ix < 0 || iz < 0 || ix >= s->w || iz >= s->h) continue;
        const int t = s->texel[static_cast<size_t>(iz) * s->w + ix];
        if (t < 0) continue;
        const float w = ((k & 1) ? ax : 1.0f - ax) * ((k >> 1) ? az : 1.0f - az);
        if (!(w > 0.0f) || !Seen(mesh, m.patches[m.texels[t]].p, P, 0.0f, 0.0f, stamp, mark)) continue;
        for (int c = 0; c < 3; c++) out[c] += w * v[t][c];
        wsum += w;
    }
    if (wsum > 0.0f)
        for (float& c : out) c /= wsum;
    return out;
}

} // namespace GiRef
