// Offline check of the lamp index cells (real-time GI S3-02, S3-02b: features/lamp_index.h, ShaderPatches::PatchObjectLampPs,
// PatchInstancedLamps and PatchWindowOutdoor with index). Read-only: prints to stdout, writes no file.
//   check.exe <lamp_index.hlsl> [Shaders_Win32.precomp]
// 1. Transport on the CPU: a model of the per-draw fix (the pick at the object, 8 lamps, the shader's sum under the first
//    law or the realistic reach law, the ground share and both height gates) against the same pixel lit from the index
//    texture LampIndex::Build filled with that pick. Asserted arms (both laws): open ground with objects anywhere, and walls
//    with objects at cell centres (loose, set in a wall, gates in a fence gap): 0 pixels over 1/255. Reported arms: walls
//    with objects anywhere, tall objects, a dense lamp cluster.
// 2. Slices: a rebuild in slices after lamps moved, came and went (only the cells those lamps can reach), and after the
//    window moved, equals a whole new build cell for cell; the CPU time of a build with the model pick.
// 3. The HLSL reference of the block (lamp_index.hlsl): compiled as ps_3_0, priced, accepted by native D3D9, and its pixels
//    on a hidden device against the CPU model of 1 (2 LSB).
// 4. With the precomp: every pixel shader the per-draw patches take (objects under both laws, fences and stairs under both
//    laws with and without the floor-tile shadows, window frames under both laws), patched with the index block: slots,
//    temporaries, native D3D9 accepts it, and its pixels equal the per-draw copy's (1 LSB) with the same lamps.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include "../../features/lamp_index.h"
#include "../../features/shader_patches.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace {

int g_checks = 0, g_failures = 0;
void Check(bool ok, const char* what) {
    g_checks++;
    if (!ok) {
        g_failures++;
        if (g_failures < 40) std::printf("  FAIL %s\n", what);
    }
}
template <class T> void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}
float Sat(float v) { return std::clamp(v, 0.0f, 1.0f); }
constexpr int N8 = LampIndex::kSlots;

// ---- 1. the CPU model ----
struct SceneLamp {
    float row[LampIndex::kRowFloats]; // the table rows: (pos, 1/R^2), (colour, spill), cone 1, cone 2, (r0^2, out of a house), floor shadow a, b
    float radius;
};
struct Wall {
    float x0, z0, x1, z1, pass; // a lamp's light crossing it keeps `pass`
};
struct Scene {
    std::vector<SceneLamp> lamps;
    std::vector<Wall> walls;
    bool reach = false;
};
struct Flags {
    bool inWall = false, gate = false;
};
constexpr float kLooseSkip = 0.2f, kWallSkip = 0.75f; // LotLightBridge: the nearest wall an object ignores
constexpr float kHost = 0.02f, kGroundTexel = 0.3f;   // the host's own light, the ground atlas (uniform)

float ConeAt(const float* row, const float* p) {
    float l[3] = {row[0] - p[0], row[1] - p[1], row[2] - p[2]};
    const float d = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    if (!(d > 1e-3f)) return 1.0f;
    for (float& v : l) v /= d;
    const float t1 = l[0] * row[8] + l[1] * row[9] + l[2] * row[10] + row[11], t2 = l[0] * row[12] + l[1] * row[13] + l[2] * row[14] + row[15];
    return row[7] + (1.0f - row[7]) * Sat(std::max(t1, t2));
}
float Visibility(const Scene& s, const float* lamp, const float* p, float nearSkip) {
    float v = 1.0f;
    const float dx = p[0] - lamp[0], dz = p[2] - lamp[2], len = std::sqrt(dx * dx + dz * dz);
    for (const Wall& w : s.walls) {
        const float ex = w.x1 - w.x0, ez = w.z1 - w.z0, den = dx * ez - dz * ex;
        if (std::fabs(den) < 1e-9f) continue;
        const float t = ((w.x0 - lamp[0]) * ez - (w.z0 - lamp[2]) * ex) / den, u = ((w.x0 - lamp[0]) * dz - (w.z0 - lamp[2]) * dx) / den;
        if (t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f && (1.0f - t) * len > nearSkip) v *= w.pass;
    }
    return v;
}
// LotLightBridge PickObjectLampsImpl with SelectLamps: the 16 best by distance - radius within 40 m, the first 8 a wall
// leaves, the ground share and the highest head within 15 m
void Pick(const Scene& s, float x, float y, float z, bool inWall, LampIndex::PointPick& out) {
    struct Cand {
        float score;
        const SceneLamp* l;
    };
    std::vector<Cand> c;
    for (const SceneLamp& l : s.lamps) {
        const float dx = l.row[0] - x, dz = l.row[2] - z, score = std::sqrt(dx * dx + dz * dz) - l.radius;
        if (score <= 40.0f) c.push_back({score, &l});
    }
    std::stable_sort(c.begin(), c.end(), [](const Cand& a, const Cand& b) { return a.score < b.score; });
    if (c.size() > 16) c.resize(16);
    out = {};
    const float obj[3] = {x, y + 0.5f, z};
    float seen = 0.0f, all = 0.0f, top = -1e9f;
    for (const Cand& k : c) {
        const float* r = k.l->row;
        const float dx = r[0] - obj[0], dy = r[1] - obj[1], dz = r[2] - obj[2], rad = std::max(k.l->radius, 0.1f);
        if (dx * dx + dz * dz <= 15.0f * 15.0f) top = std::max(top, r[1]);
        const float w = (r[4] * 0.2126f + r[5] * 0.7152f + r[6] * 0.0722f) * ConeAt(r, obj) / (1.0f + (dx * dx + dy * dy + dz * dz) / (rad * rad));
        all += w;
        const float v = Visibility(s, r, obj, inWall ? kWallSkip : kLooseSkip);
        if (v < 0.01f) continue;
        seen += w * v;
        if (out.n >= N8) continue;
        std::memcpy(out.rows[out.n], r, sizeof out.rows[0]);
        out.vis[out.n++] = v;
    }
    if (all > 1e-5f) out.share = Sat(seen / all);
    out.top = top > -1e8f ? top : LampIndex::kNoTop;
}
// One lamp at a pixel: the patched shader's law and cones (AppendLampLaw and the cone rows), before colour and visibility
float Law(const float* row, const float* P, const float* N, bool reach) {
    const float l[3] = {row[0] - P[0], row[1] - P[1], row[2] - P[2]};
    const float d2 = std::max(l[0] * l[0] + l[1] * l[1] + l[2] * l[2], 1e-4f), inv = 1.0f / std::sqrt(d2);
    const float L[3] = {l[0] * inv, l[1] * inv, l[2] * inv};
    const float t1 = L[0] * row[8] + L[1] * row[9] + L[2] * row[10] + row[11], t2 = L[0] * row[12] + L[1] * row[13] + L[2] * row[14] + row[15];
    const float t = Sat(std::max(t1, t2)), cone = t - t * row[7] + row[7], ndl = Sat(N[0] * L[0] + N[1] * L[1] + N[2] * L[2]);
    const float q = d2 * row[3];
    if (!reach) {
        const float s = Sat(1.0f - q);
        return ndl * s * s * cone;
    }
    const float s = Sat(1.0f - q * q), inv2 = inv * inv; // the outdoor walls' law: sat(N.l) / d^2 x min(1, r0^2 / d^2 x sat(1 - q^2)^2)
    return ndl * inv2 * std::min(1.0f, inv2 * row[16] * s * s) * cone;
}
void HueClamp(float Q[3]) {
    const float m = std::max({Q[0], Q[1], Q[2]}), k = m > 0.0f ? Sat(1.0f / m) : 1.0f;
    for (int c = 0; c < 3; c++) Q[c] *= k;
}
void Finish(const float* Q, float lampStrength, float ground, const float* N, float rgb[3]) {
    for (int c = 0; c < 3; c++) rgb[c] = Sat(std::max(kHost + Q[c] * lampStrength, kGroundTexel * Sat(N[1]) * ground));
}
// The per-draw fix: the pick at the object's point, everything else decided once for the object (DrawObjectLamp)
void ShadePerDraw(const LampIndex::PointPick& pick, bool reach, float originY, Flags f, const LampIndex::Frame& fr, const float* P, const float* N, float rgb[3]) {
    float Q[3] = {};
    for (int k = 0; k < pick.n; k++) {
        const float law = Law(pick.rows[k], P, N, reach);
        for (int c = 0; c < 3; c++) Q[c] += pick.rows[k][4 + c] * pick.vis[k] * law;
    }
    if (reach) HueClamp(Q);
    const float height = pick.top < 1e8f ? Sat(1.0f - (originY - pick.top) / fr.fadeAbove) : 1.0f;
    const float pool = Sat((fr.poolNone - std::fabs(originY - fr.lotY)) / (fr.poolNone - fr.poolFull));
    Finish(Q, f.gate ? fr.gateStrength : fr.lampStrength, fr.groundStrength * pick.share * height * pool, N, rgb);
}
void TableRows(const LampIndex::Atlas& a, int col, float* row) {
    for (int r = 0; r < LampIndex::kTableRows; r++) std::memcpy(row + 4 * r, LampIndex::Texel(a, col, LampIndex::kGrid + r), 16);
}
// The index block: the cell of the pixel, its entries, the share and the gates at the pixel (lamp_index.hlsl)
void ShadeIndex(const LampIndex::Atlas& a, bool reach, Flags f, const LampIndex::Frame& fr, const float* P, const float* N, float rgb[3]) {
    const float* cell = LampIndex::CellAt(a, P[0], P[2]);
    float Q[3] = {};
    for (int s = 0; s < N8; s++) {
        const float* e = cell + 4 * s;
        float row[LampIndex::kRowFloats];
        TableRows(a, static_cast<int>(e[0]), row);
        const float k = Law(row, P, N, reach) * (f.inWall ? e[2] : e[1]);
        for (int c = 0; c < 3; c++) Q[c] += row[4 + c] * k;
    }
    if (reach) HueClamp(Q);
    const float* g = cell + 4 * N8;
    const float span = fr.poolNone - fr.poolFull;
    const float ground = (f.inWall ? g[2] : g[0]) * Sat(1.0f + (g[1] - P[1]) / fr.fadeAbove) * Sat(fr.poolNone / span - std::fabs(P[1] - fr.lotY) / span) * fr.groundStrength;
    Finish(Q, f.gate ? fr.gateStrength : fr.lampStrength, ground, N, rgb);
}

SceneLamp MakeLamp(std::mt19937& rng, int i, float spread, float lotY, bool cones) {
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    SceneLamp l{};
    l.radius = 2.0f + 8.0f * u(rng);
    l.row[0] = 1000.0f + (u(rng) - 0.5f) * spread, l.row[1] = lotY + 1.5f + 2.5f * u(rng), l.row[2] = 2000.0f + (u(rng) - 0.5f) * spread;
    l.row[3] = 1.0f / (l.radius * l.radius);
    for (int c = 0; c < 3; c++) l.row[4 + c] = 0.2f + 1.3f * u(rng);
    l.row[7] = 1.0f, l.row[11] = l.row[15] = 1.0f; // no cone
    if (cones && i % 3 == 0) { // a spot pointing down and aside (LotLightBridge LampCone: t = l.k.xyz + k.w)
        const float ax = u(rng) - 0.5f, az = u(rng) - 0.5f, n = std::sqrt(ax * ax + 1.0f + az * az), k = 5.0f;
        l.row[8] = k * ax / n, l.row[9] = k / n, l.row[10] = k * az / n, l.row[11] = -k * 0.7f;
        l.row[12] = l.row[8], l.row[13] = l.row[9], l.row[14] = l.row[10], l.row[15] = -k * 0.8f;
        l.row[7] = 0.15f * u(rng);
    }
    const float r0 = l.radius * (0.2f + 0.3f * u(rng)); // (the reach law's inner radius)
    l.row[16] = r0 * r0;
    l.row[17] = i % 5 == 4 ? 1.0f : 0.0f; // a lamp shining out of a house
    if (i % 2 == 0) { // a floor-tile shadow (any rows: both copies run the same instructions on them)
        const float a[4] = {0.35f + 0.1f * u(rng), 0.05f, 0.5f, 1.0f}, b[4] = {0.05f, 0.35f + 0.1f * u(rng), 0.5f, 0.0f};
        std::memcpy(l.row + 20, a, 16), std::memcpy(l.row + 24, b, 16);
    }
    return l;
}
Scene MakeScene(std::mt19937& rng, int lamps, float spread, bool walls, float lotY, bool cones = true) {
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    Scene s;
    for (int i = 0; i < lamps; i++) s.lamps.push_back(MakeLamp(rng, i, spread, lotY, cones));
    if (walls)
        for (int i = 0; i < 14; i++) {
            const float x = 1000.0f + (u(rng) - 0.5f) * spread * 0.8f, z = 2000.0f + (u(rng) - 0.5f) * spread * 0.8f, len = 4.0f + 8.0f * u(rng);
            const bool alongX = i % 2 == 0;
            s.walls.push_back({x, z, x + (alongX ? len : 0.0f), z + (alongX ? 0.0f : len), i % 5 == 0 ? 0.4f : 0.0f});
        }
    return s;
}
auto PickOf(const Scene& s) {
    return [&s](float x, float y, float z, bool inWall, LampIndex::PointPick& o) { Pick(s, x, y, z, inWall, o); };
}

struct ArmResult {
    long pixels = 0, over = 0;
    float worst = 0.0f;
    LampIndex::Atlas atlas;
};
// centre: objects stand at cell centres with their pixels inside the cell (the transport case); else anywhere
ArmResult RunArm(const char* name, unsigned seed, int lamps, float spread, bool walls, bool centre, float tall, bool assert0, bool reach) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    LampIndex::Frame fr;
    fr.lampStrength = reach ? 4.0f : 0.8f, fr.gateStrength = reach ? 2.5f : 0.5f, fr.groundStrength = 0.6f, fr.lotY = 10.0f;
    Scene s = MakeScene(rng, lamps, spread, walls, fr.lotY);
    s.reach = reach;
    ArmResult r;
    LampIndex::Build(r.atlas, 1003.0f, 2001.0f, fr.lotY, PickOf(s));
    long truncated = 0;
    for (int o = 0; o < 600; o++) {
        float at[3] = {1000.0f + (u(rng) - 0.5f) * 100.0f, fr.lotY, 2000.0f + (u(rng) - 0.5f) * 100.0f};
        if (centre) {
            int cx, cz;
            LampIndex::CellOf(r.atlas, at[0], at[2], cx, cz);
            at[0] = r.atlas.ox + (cx + 0.5f) * LampIndex::kCell, at[2] = r.atlas.oz + (cz + 0.5f) * LampIndex::kCell;
        }
        const Flags f{o % 3 == 1, o % 4 == 2};
        LampIndex::PointPick pick;
        Pick(s, at[0], at[1], at[2], f.inWall, pick);
        // (more than 8 lamps in reach of the object: the per-draw fix drops some too, by its own order)
        int nearby = 0;
        for (const SceneLamp& l : s.lamps) {
            const float dx = l.row[0] - at[0], dz = l.row[2] - at[2];
            if (std::sqrt(dx * dx + dz * dz) - l.radius < 2.5f) nearby++;
        }
        if (nearby > N8) truncated++;
        const float half = centre ? 0.98f : 0.6f;
        for (int p = 0; p < 64; p++) {
            const float P[3] = {at[0] + (u(rng) * 2.0f - 1.0f) * half, at[1] + u(rng) * tall, at[2] + (u(rng) * 2.0f - 1.0f) * half};
            float N[3] = {u(rng) - 0.5f, u(rng) * 1.2f - 0.2f, u(rng) - 0.5f};
            const float n = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]) + 1e-6f;
            for (float& v : N) v /= n;
            float a[3], b[3];
            ShadePerDraw(pick, reach, at[1], f, fr, P, N, a);
            ShadeIndex(r.atlas, reach, f, fr, P, N, b);
            const float d = std::max({std::fabs(a[0] - b[0]), std::fabs(a[1] - b[1]), std::fabs(a[2] - b[2])});
            r.pixels++, r.worst = std::max(r.worst, d);
            if (d > 1.0f / 255.0f) r.over++;
        }
    }
    std::printf("  %-50s %ld pixels, %ld over 1/255 (worst %.4f) | table %d lamps, %d lit cells, dropped %d (cell) %d (table), objects with over 8 lamps in reach %ld\n", name,
                r.pixels, r.over, r.worst, r.atlas.lamps, r.atlas.cellsLit, r.atlas.cellOverflow, r.atlas.tableOverflow, truncated);
    if (assert0) {
        char what[200];
        std::snprintf(what, sizeof what, "transport, %s: 0 pixels over 1/255", name);
        Check(r.over == 0 && r.pixels > 30000, what);
        Check(r.atlas.cellOverflow == 0 && r.atlas.tableOverflow == 0 && truncated == 0, "transport: the sample keeps to 8 lamps a cell");
        Check(r.atlas.lamps > 10 && r.atlas.cellsLit > 200 && r.atlas.cellsLit < LampIndex::kGrid * LampIndex::kGrid, "transport: the sample lights the window");
    }
    return r;
}
void Transport() {
    std::puts("Transport on the CPU (per-draw fix against the index texture):");
    RunArm("open ground, objects anywhere", 11, 40, 110.0f, false, false, 1.0f, true, false);
    RunArm("walls, objects at cell centres, flags", 12, 40, 110.0f, true, true, 1.0f, true, false);
    RunArm("reach law: open ground, objects anywhere", 16, 40, 110.0f, false, false, 1.0f, true, true);
    RunArm("reach law: walls, objects at cell centres, flags", 17, 40, 110.0f, true, true, 1.0f, true, true);
    const ArmResult w = RunArm("walls, objects anywhere (reported)", 13, 40, 110.0f, true, false, 1.0f, false, false);
    const ArmResult t = RunArm("open ground, pixels up to 3 m (reported)", 14, 40, 110.0f, false, false, 3.0f, false, false);
    const ArmResult d = RunArm("dense cluster, 60 lamps in 20 m (reported)", 15, 60, 20.0f, false, false, 1.0f, false, false);
    Check(w.over > 0 && w.over < w.pixels / 4, "walls, objects anywhere: the visibility of the cell differs from the object's for a minority of pixels");
    Check(t.over > 0, "tall objects: the height gates at the pixel differ from the gates at the origin");
    Check(d.atlas.lamps <= LampIndex::kMaxLamps, "dense cluster: the table holds");
}

// ---- 2. slices ----
// Two atlases hold the same cells: every cell's entries resolved to their table rows, bit for bit (column numbers may differ)
long CellsDiffer(const LampIndex::Atlas& a, const LampIndex::Atlas& b) {
    if (a.ox != b.ox || a.oz != b.oz) return LampIndex::kGrid * LampIndex::kGrid;
    long differ = 0;
    for (int cz = 0; cz < LampIndex::kGrid; cz++)
        for (int cx = 0; cx < LampIndex::kGrid; cx++) {
            const float *ca = LampIndex::Texel(a, cx * LampIndex::kCellTexels, cz), *cb = LampIndex::Texel(b, cx * LampIndex::kCellTexels, cz);
            bool same = std::memcmp(ca + 4 * N8, cb + 4 * N8, 16) == 0;
            for (int s = 0; s < N8 && same; s++) {
                float ra[LampIndex::kRowFloats], rb[LampIndex::kRowFloats];
                TableRows(a, static_cast<int>(ca[4 * s]), ra), TableRows(b, static_cast<int>(cb[4 * s]), rb);
                same = (ca[4 * s] == 0) == (cb[4 * s] == 0) && std::memcmp(ra, rb, sizeof ra) == 0 && std::memcmp(ca + 4 * s + 1, cb + 4 * s + 1, 12) == 0;
            }
            differ += same ? 0 : 1;
        }
    return differ;
}
double Ms(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
void Slices() {
    std::puts("Slices (a rebuild of the cells a change can reach, a few cells at a time):");
    std::mt19937 rng(41);
    const float lotY = 10.0f, reach = 40.0f; // (SelectLamps: a lamp is a candidate where distance - radius <= 40 m)
    Scene s1 = MakeScene(rng, 40, 110.0f, true, lotY);
    LampIndex::Atlas inc;
    auto t0 = std::chrono::steady_clock::now();
    LampIndex::Build(inc, 1003.0f, 2001.0f, lotY, PickOf(s1));
    const double full = Ms(t0);
    // a lamp moves 3 m, one goes, one comes, one changes colour (a fade)
    Scene s2 = s1;
    std::vector<SceneLamp> changed = {s1.lamps[0], s1.lamps[1], s1.lamps[2]};
    s2.lamps[0].row[0] += 3.0f;
    s2.lamps[2].row[4] *= 0.5f;
    s2.lamps.erase(s2.lamps.begin() + 1);
    s2.lamps.push_back(MakeLamp(rng, 3, 110.0f, lotY, true));
    changed.push_back(s2.lamps[0]), changed.push_back(s2.lamps.back());
    for (const SceneLamp& l : changed) LampIndex::MarkDisc(inc, l.row[0], l.row[2], reach + l.radius);
    const int marked = inc.pending;
    int slices = 0;
    t0 = std::chrono::steady_clock::now();
    while (inc.pending > 0 && !inc.full) LampIndex::Step(inc, PickOf(s2), 97), slices++;
    const double part = Ms(t0);
    LampIndex::Committed(inc);
    LampIndex::Atlas fresh;
    LampIndex::Build(fresh, 1003.0f, 2001.0f, lotY, PickOf(s2));
    const long d1 = CellsDiffer(inc, fresh);
    std::printf("  lamps changed (4 lamps, 5 discs): %d of %d cells picked again in %d slices of 97 | cells differing from a new build: %ld\n", marked,
                LampIndex::kGrid * LampIndex::kGrid, slices, d1);
    Check(d1 == 0 && marked > 0 && marked < LampIndex::kGrid * LampIndex::kGrid, "slices: a rebuild of the changed lamps' discs equals a new build");
    // the window moves 16 m east and 32 m north
    LampIndex::Move(inc, 1003.0f + 16.0f, 2001.0f + 32.0f);
    const int moved = inc.pending;
    while (inc.pending > 0) LampIndex::Step(inc, PickOf(s2), 97);
    LampIndex::Committed(inc);
    LampIndex::Build(fresh, 1003.0f + 16.0f, 2001.0f + 32.0f, lotY, PickOf(s2));
    const long d2 = CellsDiffer(inc, fresh);
    std::printf("  window moved (16 m, 32 m): %d cells picked | cells differing from a new build: %ld\n", moved, d2);
    Check(d2 == 0 && moved == 8 * 64 + 16 * 64 - 8 * 16, "slices: a moved window keeps the shared cells and equals a new build");
    // the CPU time, the model pick (the game's pick also tests walls and slabs: read the in-game build line)
    Scene big = MakeScene(rng, 150, 128.0f, true, lotY);
    LampIndex::Atlas b;
    t0 = std::chrono::steady_clock::now();
    LampIndex::Build(b, 1003.0f, 2001.0f, lotY, PickOf(big));
    const double bigMs = Ms(t0);
    std::printf("  CPU time of a whole build (4096 cells, 8192 picks), model pick: 40 lamps %.2f ms, 150 lamps %.2f ms (%.1f us a cell); the discs above %.2f ms\n", full, bigMs,
                1000.0 * bigMs / 4096.0, part);
}

// ---- hidden D3D9 device (64 x 64 target), as tools/floor_shade_check ----
struct Gpu {
    HWND window = nullptr;
    IDirect3D9* api = nullptr;
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9 *target = nullptr, *readback = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    Gpu() {
        window = CreateWindowExW(0, L"STATIC", L"Apex lamp index check", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        api = Direct3DCreate9(D3D_SDK_VERSION);
        D3DPRESENT_PARAMETERS p{};
        p.Windowed = TRUE;
        p.SwapEffect = D3DSWAPEFFECT_DISCARD;
        p.hDeviceWindow = window;
        p.BackBufferWidth = p.BackBufferHeight = 64;
        if (api) api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &p, &dev);
        if (!dev) return;
        dev->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr);
        dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr);
        const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                        {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                                        D3DDECL_END()};
        dev->CreateVertexDeclaration(el, &decl);
        dev->SetRenderTarget(0, target);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        if (!target || !readback || !decl) Release(dev);
    }
    ~Gpu() {
        Release(decl);
        Release(target);
        Release(readback);
        Release(dev);
        Release(api);
        if (window) DestroyWindow(window);
    }
    IDirect3DPixelShader9* Ps(const std::vector<DWORD>& t) {
        IDirect3DPixelShader9* s = nullptr;
        if (t.empty() || FAILED(dev->CreatePixelShader(t.data(), &s))) return nullptr;
        return s;
    }
    // a full-screen quad, TEXCOORD0 = (0, 0) top left .. (1, 1) bottom right
    std::vector<DWORD> Render(IDirect3DPixelShader9* ps, IDirect3DVertexShader9* vs) {
        struct Vertex {
            float x, y, z, w, u, v;
        };
        const Vertex q[4] = {{-1, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 1, 0}, {-1, -1, 0, 1, 0, 1}, {1, -1, 0, 1, 1, 1}};
        dev->SetVertexDeclaration(decl);
        dev->SetVertexShader(vs);
        dev->SetPixelShader(ps);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00123456, 1.0f, 0);
        dev->BeginScene();
        const bool drawn = SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vertex)));
        dev->EndScene();
        std::vector<DWORD> px(4096);
        D3DLOCKED_RECT r{};
        if (drawn && SUCCEEDED(dev->GetRenderTargetData(target, readback)) && SUCCEEDED(readback->LockRect(&r, nullptr, D3DLOCK_READONLY))) {
            for (int y = 0; y < 64; y++) std::memcpy(px.data() + y * 64, static_cast<char*>(r.pBits) + y * r.Pitch, 256);
            readback->UnlockRect();
        } else
            Check(false, "GPU draw and readback");
        return px;
    }
};
std::vector<DWORD> Compile(const std::string& src, const char* profile) {
    ID3DBlob *code = nullptr, *err = nullptr;
    const HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr) && err) std::printf("%s\n", static_cast<const char*>(err->GetBufferPointer()));
    std::vector<DWORD> out;
    if (code) out.assign(static_cast<const DWORD*>(code->GetBufferPointer()), static_cast<const DWORD*>(code->GetBufferPointer()) + code->GetBufferSize() / 4);
    Release(code);
    Release(err);
    return out;
}
std::string Disasm(const std::vector<DWORD>& t) {
    ID3DBlob* b = nullptr;
    if (t.empty() || FAILED(D3DDisassemble(t.data(), t.size() * 4, 0, nullptr, &b)) || !b) return {};
    std::string s(static_cast<const char*>(b->GetBufferPointer()), b->GetBufferSize());
    b->Release();
    return s;
}
int Slots(const std::string& text) {
    const auto at = text.find("approximately ");
    return at == std::string::npos ? -1 : std::atoi(text.c_str() + at + 14);
}
int Temps(const std::string& text) { // registers r0 .. rN the listing names
    static const std::regex re("[ ,(-]r([0-9]+)");
    int top = -1;
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) top = std::max(top, std::atoi((*it)[1].str().c_str()));
    return top + 1;
}
int TopConst(const std::string& text) { // the highest float constant c0 .. cN the listing names
    static const std::regex re("[ ,(-]c([0-9]+)");
    int top = -1;
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) top = std::max(top, std::atoi((*it)[1].str().c_str()));
    return top;
}

struct Fixture {
    IDirect3DVertexShader9 *vs = nullptr, *vs1 = nullptr, *vsFence = nullptr; // vs1: COLOR1 in the place of TEXCOORD7 (11 outputs at most)
    IDirect3DTexture9 *white = nullptr, *ground = nullptr, *index = nullptr, *clear = nullptr;
    IDirect3DCubeTexture9* cube = nullptr;
    void Free() {
        Release(vs);
        Release(vs1);
        Release(vsFence);
        Release(white);
        Release(ground);
        Release(index);
        Release(clear);
        Release(cube);
    }
};
bool MakeFixture(Gpu& gpu, Fixture& f) {
    // every input a game object shader declares: TEXCOORD0..7 (an upward normal where one is read), COLOR0 (COLOR1: vs1), and the world
    // position (x, z, y) in TEXCOORD8 as PatchObjectLampVs writes it
    const std::vector<DWORD> vs = Compile("float4 rows[3]:register(c0);\n"
                                          "struct O{float4 p:POSITION;float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;float4 t4:TEXCOORD4;"
                                          "float4 t5:TEXCOORD5;float4 t6:TEXCOORD6;float4 t7:TEXCOORD7;float4 w:TEXCOORD8;float4 c0:COLOR0;};\n"
                                          "O main(float4 p:POSITION,float2 uv:TEXCOORD0){O o;o.p=p;o.t0=float4(uv,0.5,1);float4 n=float4(0.3,0.85,0.4,1);"
                                          "o.t1=n;o.t2=n;o.t3=n;o.t4=n;o.t5=n;o.t6=n;o.t7=n;o.w=float4(rows[0].xyz+uv.x*rows[1].xyz+uv.y*rows[2].xyz,1);"
                                          "o.c0=float4(0.03,0.03,0.03,1);return o;}",
                                          "vs_3_0");
    if (vs.empty() || FAILED(gpu.dev->CreateVertexShader(vs.data(), &f.vs))) return false;
    const std::vector<DWORD> vs1 = Compile("float4 rows[3]:register(c0);\n"
                                           "struct O{float4 p:POSITION;float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;float4 t4:TEXCOORD4;"
                                           "float4 t5:TEXCOORD5;float4 t6:TEXCOORD6;float4 w:TEXCOORD8;float4 c0:COLOR0;float4 c1:COLOR1;};\n"
                                           "O main(float4 p:POSITION,float2 uv:TEXCOORD0){O o;o.p=p;o.t0=float4(uv,0.5,1);float4 n=float4(0.3,0.85,0.4,1);"
                                           "o.t1=n;o.t2=n;o.t3=n;o.t4=n;o.t5=n;o.t6=n;o.w=float4(rows[0].xyz+uv.x*rows[1].xyz+uv.y*rows[2].xyz,1);"
                                           "o.c0=float4(0.03,0.03,0.03,1);o.c1=float4(0.5,0.5,0.5,1);return o;}",
                                           "vs_3_0");
    if (vs1.empty() || FAILED(gpu.dev->CreateVertexShader(vs1.data(), &f.vs1))) return false;
    // the instanced fence VS's outputs: TEXCOORD1.zw = world x, z, TEXCOORD2 = (the world normal, world y), COLOR0 the vertex lights
    const std::vector<DWORD> vsf = Compile("float4 rows[3]:register(c0);\n"
                                           "struct O{float4 p:POSITION;float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;float4 t4:TEXCOORD4;"
                                           "float4 t5:TEXCOORD5;float4 t6:TEXCOORD6;float4 t7:TEXCOORD7;float4 c0:COLOR0;};\n"
                                           "O main(float4 p:POSITION,float2 uv:TEXCOORD0){O o;o.p=p;o.t0=float4(uv,0.5,1);float4 n=float4(0.3,0.85,0.4,1);"
                                           "float3 w=rows[0].xyz+uv.x*rows[1].xyz+uv.y*rows[2].xyz;o.t1=float4(uv,w.x,w.y);o.t2=float4(n.xyz,w.z);"
                                           "o.t3=n;o.t4=n;o.t5=n;o.t6=n;o.t7=n;o.c0=float4(0.03,0.03,0.03,1);return o;}",
                                           "vs_3_0");
    if (vsf.empty() || FAILED(gpu.dev->CreateVertexShader(vsf.data(), &f.vsFence))) return false;
    const auto solid = [&](DWORD argb) {
        IDirect3DTexture9* t = nullptr;
        D3DLOCKED_RECT r{};
        if (SUCCEEDED(gpu.dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) && SUCCEEDED(t->LockRect(0, &r, nullptr, 0))) {
            *static_cast<DWORD*>(r.pBits) = argb;
            t->UnlockRect(0);
        }
        return t;
    };
    f.white = solid(0xFFFFFFFFu);
    f.ground = solid(0xFF4D4D4Du); // 77 / 255 = 0.302
    f.clear = solid(0x00606060u);  // (windows: the house plan's alpha 0, outside the house)
    if (SUCCEEDED(gpu.dev->CreateCubeTexture(1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &f.cube, nullptr)))
        for (int face = 0; face < 6; face++) {
            D3DLOCKED_RECT r{};
            if (SUCCEEDED(f.cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face), 0, &r, nullptr, 0))) {
                *static_cast<DWORD*>(r.pBits) = 0xFF101010u;
                f.cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face), 0);
            }
        }
    gpu.dev->CreateTexture(LampIndex::kWidth, LampIndex::kHeight, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &f.index, nullptr);
    return f.white && f.ground && f.cube && f.index && f.clear;
}
void Upload(Fixture& f, const LampIndex::Atlas& a) {
    D3DLOCKED_RECT r{};
    if (FAILED(f.index->LockRect(0, &r, nullptr, 0))) return;
    for (int y = 0; y < LampIndex::kHeight; y++) std::memcpy(static_cast<char*>(r.pBits) + y * r.Pitch, LampIndex::Texel(a, 0, y), LampIndex::kWidth * 16);
    f.index->UnlockRect(0);
}
void PointClamp(IDirect3DDevice9* dev, DWORD s) {
    dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
}
// the quad: world (x, z, y) = corner + u * (du, 0, 0) + v * (0, dv, 0)
void SetQuad(IDirect3DDevice9* dev, float x, float z, float y, float size) {
    const float rows[3][4] = {{x, z, y, 0}, {size, 0, 0, 0}, {0, size, 0, 0}};
    dev->SetVertexShaderConstantF(0, rows[0], 3);
}
int WorstDiff(const std::vector<DWORD>& a, const std::vector<DWORD>& b, int* differing = nullptr) {
    int worst = 0, n = 0;
    for (size_t i = 0; i < a.size(); i++) {
        int d = 0;
        for (int sh = 0; sh < 24; sh += 8) d = std::max(d, std::abs(static_cast<int>((a[i] >> sh) & 255) - static_cast<int>((b[i] >> sh) & 255)));
        worst = std::max(worst, d);
        if (d) n++;
    }
    if (differing) *differing = n;
    return worst;
}

// ---- 3. the HLSL reference ----
void Reference(Gpu& gpu, Fixture& fx, const std::string& hlsl) {
    const std::vector<DWORD> code = Compile(hlsl, "ps_3_0");
    Check(!code.empty(), "reference: lamp_index.hlsl compiles as ps_3_0");
    if (code.empty()) return;
    const std::string text = Disasm(code);
    std::printf("HLSL reference (stand-in host and the block): %d slots, %d temporaries\n", Slots(text), Temps(text));
    Check(Slots(text) > 0 && Slots(text) <= 512 && Temps(text) <= 32, "reference: within ps_3_0's 512 slots and 32 temporaries");
    Check(text.find("rep i") != std::string::npos || text.find("loop aL") != std::string::npos, "reference: the lamps are a loop");
    if (!gpu.dev) return;
    IDirect3DPixelShader9* ps = gpu.Ps(code);
    Check(ps != nullptr, "reference: native D3D9 accepts the looped shader");
    if (!ps) return;
    std::mt19937 rng(21);
    LampIndex::Frame fr;
    fr.lampStrength = 0.8f, fr.gateStrength = 0.5f, fr.groundStrength = 0.6f, fr.lotY = 10.0f;
    const Scene s = MakeScene(rng, 40, 60.0f, true, fr.lotY);
    LampIndex::Atlas a;
    LampIndex::Build(a, 1003.0f, 2001.0f, fr.lotY, PickOf(s));
    Upload(fx, a);
    float c[LampIndex::kConstRows][4];
    LampIndex::FrameConstants(a, fr, c);
    gpu.dev->SetPixelShaderConstantF(LampIndex::kFirstConst, c[0], LampIndex::kConstRows);
    const float host[4] = {kHost, kHost, kHost, 0};
    gpu.dev->SetPixelShaderConstantF(0, host, 1);
    gpu.dev->SetTexture(0, fx.white);
    gpu.dev->SetTexture(1, fx.ground);
    gpu.dev->SetTexture(2, fx.index);
    for (DWORD i = 0; i < 3; i++) PointClamp(gpu.dev, i);
    float N[3] = {0.3f, 0.85f, 0.4f};
    const float n = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
    for (float& v : N) v /= n;
    long compared = 0, over = 0, lit = 0;
    int worst = 0;
    for (int run = 0; run < 12; run++) {
        const Flags f{run % 3 == 1, run % 4 == 2};
        float row[4];
        LampIndex::ObjectRow(f.inWall, f.gate, row);
        gpu.dev->SetPixelShaderConstantF(1, row, 1);
        const float x0 = 975.013f + 4.7f * run, z0 = 1978.031f + 3.9f * run, y = fr.lotY + 0.1f * run, size = 6.0f;
        SetQuad(gpu.dev, x0, z0, y, size);
        const std::vector<DWORD> px = gpu.Render(ps, fx.vs);
        for (int j = 0; j < 64; j++)
            for (int i = 0; i < 64; i++) {
                const float P[3] = {x0 + size * i / 64.0f, y, z0 + size * j / 64.0f}; // (D3D9: pixel i reads uv i / 64)
                // a pixel within 3 mm of a cell border may fall in either cell on the GPU
                const float bx = std::fabs(std::remainder(P[0] - a.ox, LampIndex::kCell)), bz = std::fabs(std::remainder(P[2] - a.oz, LampIndex::kCell));
                if (bx < 0.003f || bz < 0.003f) continue;
                float want[3];
                ShadeIndex(a, false, f, fr, P, N, want);
                int d = 0;
                for (int ch = 0; ch < 3; ch++)
                    d = std::max(d, std::abs(static_cast<int>((px[j * 64 + i] >> (16 - 8 * ch)) & 255) - static_cast<int>(std::lround(want[ch] * 255.0f))));
                compared++, worst = std::max(worst, d);
                if (d > 2) over++;
                if (want[0] > kHost + 0.02f) lit++;
            }
    }
    std::printf("HLSL reference on the device against the CPU model: %ld pixels (%ld lit by a lamp or the ground), %ld over 2 LSB (worst %d)\n", compared, lit, over, worst);
    Check(over == 0 && compared > 30000 && lit > 5000, "reference: the device's pixels equal the CPU model within 2 LSB");
    gpu.dev->SetPixelShader(nullptr);
    Release(ps);
}

// ---- 4. the game's shaders ----
std::vector<std::vector<DWORD>> PixelShaders(const std::vector<unsigned char>& d) { // docs/engine/shaders.md
    std::vector<std::vector<DWORD>> out;
    size_t shdb = std::string::npos;
    for (size_t i = 0; i + 8 <= d.size() && shdb == std::string::npos; i++)
        if (std::memcmp(&d[i], "SHDB", 4) == 0) shdb = i;
    if (shdb == std::string::npos) return out;
    uint32_t size;
    std::memcpy(&size, &d[shdb + 4], 4);
    const size_t end = std::min(d.size(), shdb + 8 + size);
    for (size_t j = shdb + 8; j + 8 <= end;) {
        const bool v = std::memcmp(&d[j], "VSHD", 4) == 0, p = std::memcmp(&d[j], "PSHD", 4) == 0;
        if (!v && !p) {
            j++;
            continue;
        }
        uint32_t n;
        std::memcpy(&n, &d[j + 4], 4);
        const unsigned char* b = &d[j + 8];
        const size_t len = std::min<size_t>(n, d.size() - j - 8);
        for (size_t k = 0; p && k + 4 <= std::min<size_t>(len, 12); k += 4) {
            DWORD ver;
            std::memcpy(&ver, b + k, 4);
            if (ver != 0xFFFF0300u) continue;
            for (size_t q = k + 4; q + 4 <= len;) {
                DWORD tok;
                std::memcpy(&tok, b + q, 4);
                if (tok == 0x0000FFFFu) {
                    std::vector<DWORD> c((q + 4 - k) / 4);
                    std::memcpy(c.data(), b + k, c.size() * 4);
                    out.push_back(std::move(c));
                    break;
                }
                q += ((tok & 0xFFFF) == 0xFFFE) ? 4 + 4 * ((tok >> 16) & 0x7FFF) : 4 + 4 * ((tok >> 24) & 0xF);
            }
            break;
        }
        j += 8 + n;
    }
    return out;
}
// The host's declarations: false when the fixture cannot feed it. cube[s]: sampler s is a cube.
bool HostInputs(const std::vector<DWORD>& t, bool cube[16], bool& color1) {
    bool tc7 = false;
    for (size_t i = 1; i < t.size();) {
        const DWORD tok = t[i];
        if (tok == 0x0000FFFFu) break;
        if ((tok & 0xFFFF) == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const size_t len = (tok >> 24) & 0xF;
        if ((tok & 0xFFFF) == 0x1F && len == 2) {
            const DWORD use = t[i + 1] & 0x1F, idx = (t[i + 1] >> 16) & 0xF, r = t[i + 2], type = ((r >> 28) & 7) | (((r >> 11) & 3) << 3);
            if (type == 10) {
                const DWORD kind = (t[i + 1] >> 27) & 0xF;
                if (kind != 2 && kind != 3) return false;
                cube[r & 15] = kind == 3;
            } else if (type != 1 || !((use == 5 && idx <= 7) || (use == 10 && idx <= 1)))
                return false;
            else
                tc7 |= use == 5 && idx == 7, color1 |= use == 10 && idx == 1;
        }
        i += 1 + len;
    }
    return !(tc7 && color1);
}

// One family's counts: copies taken and refused, their price, native D3D9's answer and the pictures against the per-draw copy
struct Tally {
    const char* name;
    int hosts = 0, taken = 0, refused = 0, slotsOk = 0, worstSlots = 0, worstTemps = 0, accepted = 0, drawn = 0, equal = 0, sensitive = 0, worstDiff = 0;
    void Price(const std::vector<DWORD>& copy) {
        taken++;
        const std::string text = Disasm(copy);
        const int slots = Slots(text);
        if (slots > 0 && slots <= 512) slotsOk++;
        worstSlots = std::max(worstSlots, slots), worstTemps = std::max(worstTemps, Temps(text));
    }
    void Print(bool device) const {
        std::printf("  %-44s hosts %d | index copy taken %d, refused %d | within 512 slots %d (most %d), temporaries most %d", name, hosts, taken, refused, slotsOk, worstSlots,
                    worstTemps);
        if (device) std::printf(" | native D3D9 accepts %d | drawn %d: equal within 1 LSB %d (worst %d), the lamps change the picture in %d", accepted, drawn, equal, worstDiff, sensitive);
        std::puts("");
    }
};
// The per-draw rows of one point: cS row, kSlots x (pos, 1/R^2) (colour x visibility, spill), kSlots x cones, then the 2 rows of
// r0^2. shaded (fences): a lamp not shining out of a house keeps its colour whole. noExit (windows): those lamps left out.
// The colour takes the visibility in every case, as the per-draw picks give it.
int PerDrawRows(const LampIndex::PointPick& pick, float strength, float shadedFlag, bool shaded, bool noExit, float (*lamps)[4]) {
    std::memset(lamps, 0, sizeof(float) * 4 * (3 + 4 * N8));
    lamps[0][0] = 1.0f, lamps[0][1] = strength, lamps[0][2] = shadedFlag, lamps[0][3] = 1e-4f;
    for (int s = 0; s < N8; s++) {
        lamps[1 + 2 * s][0] = lamps[1 + 2 * s][2] = 1e6f;
        lamps[2 + 2 * s][3] = lamps[1 + 2 * N8 + 2 * s][3] = lamps[2 + 2 * N8 + 2 * s][3] = 1.0f;
    }
    int n = 0;
    for (int k = 0; k < pick.n; k++) {
        const float* r = pick.rows[k];
        if (noExit && r[17] > 0.5f) continue;
        const float v = shaded && r[17] < 0.5f ? 1.0f : pick.vis[k];
        std::memcpy(lamps[1 + 2 * n], r, 32);
        for (int ch = 0; ch < 3; ch++) lamps[2 + 2 * n][ch] *= v;
        std::memcpy(lamps[1 + 2 * N8 + 2 * n], r + 8, 32);
        lamps[1 + 4 * N8 + n / 4][n % 4] = r[16];
        n++;
    }
    return n;
}

void GameShaders(Gpu& gpu, Fixture& fx, const std::vector<std::vector<DWORD>>& shaders) {
    std::set<std::vector<DWORD>> seen;
    std::vector<const std::vector<DWORD>*> unique;
    for (const auto& code : shaders)
        if (seen.insert(code).second) unique.push_back(&code);
    std::mt19937 rng(31);
    LampIndex::Frame fr;
    fr.lampStrength = 0.8f, fr.gateStrength = 0.5f, fr.groundStrength = 0.6f, fr.lotY = 10.0f;
    fr.groundUv[2] = fr.groundUv[3] = 0.5f;
    // two samples: five lamps on open ground under a 6 m quad over nine cells; walls, with the quad inside one cell; the
    // windows': no cone (the per-draw window copy takes the cone at the origin) and one lamp shining out of a house
    Scene open = MakeScene(rng, 5, 8.0f, false, fr.lotY), walled = MakeScene(rng, 12, 14.0f, true, fr.lotY), window = MakeScene(rng, 6, 9.0f, true, fr.lotY, false);
    for (int k = 0; k < 6; k++) window.lamps[k].row[17] = k == 1 ? 1.0f : 0.0f;
    LampIndex::Atlas atlas[3];
    const Scene* scene[3] = {&open, &walled, &window};
    for (int k = 0; k < 3; k++) LampIndex::Build(atlas[k], 1003.0f, 2001.0f, fr.lotY, PickOf(*scene[k]));
    const auto point = [&](int k, float size, float y, float at[3]) {
        int cx, cz;
        LampIndex::CellOf(atlas[k], 1000.3f, 2000.4f, cx, cz);
        at[0] = atlas[k].ox + (cx + 0.5f) * LampIndex::kCell, at[1] = y, at[2] = atlas[k].oz + (cz + 0.5f) * LampIndex::kCell;
        SetQuad(gpu.dev, at[0] - 0.5f * size, at[2] - 0.5f * size, y, size);
    };
    float base[224][4];
    for (auto& r : base) r[0] = r[1] = r[2] = r[3] = 0.25f;
    std::memset(base[5], 0, sizeof(float) * 12); // the rig's lamps, zeroed by the draw
    const auto bind = [&](const bool cube[16], IDirect3DBaseTexture9* flat) {
        for (DWORD s = 0; s < 16; s++) {
            gpu.dev->SetTexture(s, cube[s] ? static_cast<IDirect3DBaseTexture9*>(fx.cube) : flat);
            PointClamp(gpu.dev, s);
        }
    };
    const auto frame = [&](int k) {
        Upload(fx, atlas[k]);
        float c[LampIndex::kConstRows][4];
        LampIndex::FrameConstants(atlas[k], fr, c);
        gpu.dev->SetPixelShaderConstantF(LampIndex::kFirstConst, c[0], LampIndex::kConstRows);
    };

    // -- objects, both laws --
    for (int law = 0; law < 2; law++) {
        const bool reach = law == 1;
        Tally t{reach ? "objects, reach law" : "objects, first law"};
        for (const auto* code : unique) {
            std::vector<DWORD> plain = *code, index = *code;
            ShaderPatches::ObjectLampPatch pp, ip;
            if (!ShaderPatches::PatchObjectLampPs(plain, pp, reach)) continue;
            t.hosts++;
            if (!ShaderPatches::PatchObjectLampPs(index, ip, reach, true)) {
                t.refused++;
                const std::string host = Disasm(*code);
                std::printf("  object index copy refused (%s), the per-draw copy keeps drawing it: %d slots, %d temporaries, highest constant c%d, %s\n", t.name, Slots(host), Temps(host),
                            TopConst(host), host.find("defi") != std::string::npos ? "an integer constant of its own" : "no integer constant");
                continue;
            }
            t.Price(index);
            if (!gpu.dev) continue;
            IDirect3DPixelShader9 *a = gpu.Ps(plain), *b = gpu.Ps(index);
            if (b) t.accepted++;
            bool cube[16] = {}, color1 = false;
            if (a && b && HostInputs(*code, cube, color1)) {
                IDirect3DVertexShader9* hostVs = color1 ? fx.vs1 : fx.vs;
                t.drawn++;
                bind(cube, fx.white);
                bool hostEqual = true, hostSensitive = false;
                for (int k = 0; k < 2; k++)
                    for (int flag = 0; flag < (k ? 4 : 1); flag++) {
                        const Flags f{(flag & 1) != 0, (flag & 2) != 0};
                        float at[3];
                        const float y = fr.lotY + 0.3f;
                        point(k, k ? 1.9f : 6.0f, y, at);
                        LampIndex::PointPick pick;
                        Pick(*scene[k], at[0], fr.lotY, at[2], f.inWall, pick);
                        float lamps[3 + 4 * N8][4];
                        PerDrawRows(pick, f.gate ? fr.gateStrength : fr.lampStrength, 0.0f, false, false, lamps);
                        const float height = pick.top < 1e8f ? Sat(1.0f - (y - pick.top) / fr.fadeAbove) : 1.0f;
                        const float pool = Sat((fr.poolNone - std::fabs(y - fr.lotY)) / (fr.poolNone - fr.poolFull));
                        const float strength[4] = {fr.groundStrength * pick.share * height * pool, 0, 0, 0};
                        gpu.dev->SetPixelShaderConstantF(0, base[0], 208);
                        gpu.dev->SetPixelShaderConstantF(pp.atlasConst, fr.groundUv, 1);
                        gpu.dev->SetPixelShaderConstantF(pp.strengthConst, strength, 1);
                        gpu.dev->SetPixelShaderConstantF(pp.lampParamConst, lamps[0], 1 + 4 * N8 + (reach ? 2 : 0));
                        gpu.dev->SetTexture(pp.atlasSampler, fx.ground);
                        const std::vector<DWORD> perDraw = gpu.Render(a, hostVs);
                        if (k == 0) { // the lamps and the ground light show in this host's picture
                            const float off[4] = {};
                            gpu.dev->SetPixelShaderConstantF(pp.strengthConst, off, 1);
                            gpu.dev->SetPixelShaderConstantF(pp.lampParamConst + 1, off, 1); // lamp 0 moved to the origin: out of reach
                            float dark[4] = {1.0f, 0.0f, 0.0f, 1e-4f};
                            gpu.dev->SetPixelShaderConstantF(pp.lampParamConst, dark, 1);
                            int differing = 0;
                            WorstDiff(perDraw, gpu.Render(a, hostVs), &differing);
                            hostSensitive = differing > 400;
                        }
                        gpu.dev->SetTexture(pp.atlasSampler, cube[pp.atlasSampler] ? static_cast<IDirect3DBaseTexture9*>(fx.cube) : fx.white);
                        // the index copy: the frame block, one row, two textures
                        frame(k);
                        float row[4];
                        LampIndex::ObjectRow(f.inWall, f.gate, row);
                        gpu.dev->SetPixelShaderConstantF(0, base[0], 208);
                        gpu.dev->SetPixelShaderConstantF(ip.objectConst, row, 1);
                        gpu.dev->SetTexture(ip.atlasSampler, fx.ground);
                        gpu.dev->SetTexture(ip.indexSampler, fx.index);
                        const int d = WorstDiff(perDraw, gpu.Render(b, hostVs));
                        t.worstDiff = std::max(t.worstDiff, d);
                        if (d > 1) hostEqual = false;
                        gpu.dev->SetTexture(ip.indexSampler, fx.white);
                    }
                if (hostEqual) t.equal++;
                if (hostSensitive) t.sensitive++;
            }
            Release(a);
            Release(b);
        }
        t.Print(gpu.dev != nullptr);
        Check(t.hosts > 20, "objects: the precomp has object shaders the per-draw patch takes");
        Check(t.refused * 50 <= t.hosts, "objects: at most 2% refuse the index block (they keep the per-draw copy)");
        Check(t.slotsOk == t.taken && t.worstTemps <= 32, "objects: every index copy within 512 slots and 32 temporaries");
        if (gpu.dev) {
            Check(t.accepted == t.taken, "objects: native D3D9 accepts every index copy");
            Check(t.drawn > 20 && t.equal == t.drawn, "objects: the index copy draws the per-draw copy's pixels (1 LSB)");
            Check(t.sensitive * 2 > t.drawn, "objects: the lamps show in most of the compared pictures");
        }
    }

    // -- fences and stairs: both laws, with and without the floor-tile shadows --
    for (int variant = 0; variant < 4; variant++) {
        const bool reach = variant & 1, caps = variant >= 2;
        static const char* names[4] = {"fences, first law", "fences, reach law", "fences, first law, floor shadows", "fences, reach law, floor shadows"};
        Tally t{names[variant]};
        for (const auto* code : unique) {
            std::vector<DWORD> plain = *code, index = *code;
            ShaderPatches::InstancedPatch pp, ip;
            if (!ShaderPatches::PatchInstancedLamps(plain, pp, reach, true, caps) || !pp.pixelLamps) continue;
            t.hosts++;
            if (!ShaderPatches::PatchInstancedLamps(index, ip, reach, true, caps, true)) {
                t.refused++;
                continue;
            }
            t.Price(index);
            if (!gpu.dev) continue;
            IDirect3DPixelShader9 *a = gpu.Ps(plain), *b = gpu.Ps(index);
            if (b) t.accepted++;
            bool cube[16] = {}, color1 = false;
            if (a && b && HostInputs(*code, cube, color1) && !color1) {
                t.drawn++;
                bind(cube, fx.white);
                bool hostEqual = true, hostSensitive = false;
                for (int k = 0; k < 2; k++)
                    for (int shaded = 0; shaded < (k ? 2 : 1); shaded++) {
                        float at[3];
                        point(k, k ? 1.9f : 6.0f, fr.lotY + 0.3f, at);
                        LampIndex::PointPick pick;
                        Pick(*scene[k], at[0], fr.lotY, at[2], false, pick);
                        float lamps[3 + 4 * N8][4];
                        PerDrawRows(pick, reach ? 6.0f : fr.lampStrength, 0.0f, shaded != 0, false, lamps); // (reach: 1 / d^2 light under the max with the atlas needs the walls' larger scale to show)
                        const float weight[4] = {1.0f, 0.0f, 1.0f, 0.0f}, strength[4] = {0.6f, 0, 0, 0};
                        float capRows[2 * N8 + 1][4] = {};
                        const float params[4] = {0.5f, 0.3f, 0.5f, 1e-4f};
                        for (int s = 0, n = 0; s < pick.n && n < N8; s++, n++) std::memcpy(capRows[2 * n], pick.rows[s] + 20, 32);
                        std::memcpy(capRows[2 * N8], params, 16);
                        gpu.dev->SetPixelShaderConstantF(0, base[0], 208);
                        gpu.dev->SetPixelShaderConstantF(pp.atlasConst, fr.groundUv, 1);
                        gpu.dev->SetPixelShaderConstantF(pp.strengthConst, strength, 1);
                        gpu.dev->SetPixelShaderConstantF(pp.groundWeightConst, weight, 1);
                        gpu.dev->SetPixelShaderConstantF(pp.lampParamConst, lamps[0], 1 + 4 * N8 + (reach ? 2 : 0));
                        if (caps) gpu.dev->SetPixelShaderConstantF(pp.capConst, capRows[0], 2 * N8 + 1);
                        gpu.dev->SetTexture(pp.atlasSampler, fx.ground);
                        const std::vector<DWORD> perDraw = gpu.Render(a, fx.vsFence);
                        if (k == 0) {
                            float dark[1 + 2 * N8][4] = {};
                            dark[0][0] = 1.0f, dark[0][3] = 1e-4f;
                            gpu.dev->SetPixelShaderConstantF(pp.lampParamConst, dark[0], 1 + 2 * N8);
                            int differing = 0;
                            WorstDiff(perDraw, gpu.Render(a, fx.vsFence), &differing);
                            hostSensitive = differing > 400;
                        }
                        gpu.dev->SetTexture(pp.atlasSampler, fx.white);
                        frame(k);
                        lamps[0][2] = shaded ? 1.0f : 0.0f;
                        gpu.dev->SetPixelShaderConstantF(0, base[0], 208);
                        gpu.dev->SetPixelShaderConstantF(ip.atlasConst, fr.groundUv, 1);
                        gpu.dev->SetPixelShaderConstantF(ip.strengthConst, strength, 1);
                        gpu.dev->SetPixelShaderConstantF(ip.groundWeightConst, weight, 1);
                        gpu.dev->SetPixelShaderConstantF(ip.lampParamConst, lamps[0], 1);
                        if (caps) gpu.dev->SetPixelShaderConstantF(ip.capConst, params, 1);
                        gpu.dev->SetTexture(ip.atlasSampler, fx.ground);
                        gpu.dev->SetTexture(ip.indexSampler, fx.index);
                        const int d = WorstDiff(perDraw, gpu.Render(b, fx.vsFence));
                        t.worstDiff = std::max(t.worstDiff, d);
                        if (d > 1) hostEqual = false;
                        gpu.dev->SetTexture(ip.indexSampler, fx.white);
                    }
                if (hostEqual) t.equal++;
                if (hostSensitive) t.sensitive++;
            }
            Release(a);
            Release(b);
        }
        t.Print(gpu.dev != nullptr);
        Check(t.hosts > 5, "fences: the precomp has fence shaders with the per-pixel lamps");
        Check(t.refused * 50 <= t.hosts, "fences: at most 2% refuse the index block");
        Check(t.slotsOk == t.taken && t.worstTemps <= 32, "fences: every index copy within 512 slots and 32 temporaries");
        if (gpu.dev) {
            Check(t.accepted == t.taken, "fences: native D3D9 accepts every index copy");
            Check(t.drawn > 5 && t.equal == t.drawn, "fences: the index copy draws the per-draw copy's pixels (1 LSB)");
            Check(t.sensitive * 2 > t.drawn, "fences: the lamps show in most of the compared pictures");
        }
    }

    // -- window frames, both laws (the cones and the lamps shining out of a house: see the window sample) --
    for (int law = 0; law < 2; law++) {
        const bool reach = law == 1;
        Tally t{reach ? "window frames, reach law" : "window frames, first law"};
        for (const auto* code : unique) {
            std::vector<DWORD> plain = *code, index = *code;
            ShaderPatches::WindowOutdoorPatch pp, ip;
            if (!ShaderPatches::PatchWindowOutdoor(plain, pp, reach)) continue;
            t.hosts++;
            if (!ShaderPatches::PatchWindowOutdoor(index, ip, reach, true)) {
                t.refused++;
                continue;
            }
            t.Price(index);
            if (!gpu.dev) continue;
            IDirect3DPixelShader9 *a = gpu.Ps(plain), *b = gpu.Ps(index);
            if (b) t.accepted++;
            bool cube[16] = {}, color1 = false;
            if (a && b && HostInputs(*code, cube, color1)) {
                IDirect3DVertexShader9* hostVs = color1 ? fx.vs1 : fx.vs;
                t.drawn++;
                bind(cube, fx.clear);
                float at[3];
                point(2, 1.9f, fr.lotY + 0.3f, at);
                LampIndex::PointPick pick;
                Pick(window, at[0], fr.lotY, at[2], true, pick);
                float lamps[3 + 4 * N8][4];
                PerDrawRows(pick, fr.lampStrength, 0.0f, false, true, lamps);
                const float strength[4] = {0.6f, 0, 0, 0}, plan[2][4] = {{0.01f, 0.0f, 0, 0}, {0.0f, 0.01f, 0, 0}};
                gpu.dev->SetPixelShaderConstantF(0, base[0], 208);
                gpu.dev->SetPixelShaderConstantF(pp.atlasConst, fr.groundUv, 1);
                gpu.dev->SetPixelShaderConstantF(pp.strengthConst, strength, 1);
                gpu.dev->SetPixelShaderConstantF(pp.lampParamConst, lamps[0], 1 + 2 * N8);
                gpu.dev->SetPixelShaderConstantF(pp.planConst, plan[0], 2);
                if (reach) gpu.dev->SetPixelShaderConstantF(pp.reachConst, lamps[1 + 4 * N8], 2);
                gpu.dev->SetTexture(pp.atlasSampler, fx.ground);
                const std::vector<DWORD> perDraw = gpu.Render(a, hostVs);
                float dark[4] = {1.0f, 0.0f, 0.0f, 1e-4f};
                gpu.dev->SetPixelShaderConstantF(pp.lampParamConst, dark, 1);
                int differing = 0;
                WorstDiff(perDraw, gpu.Render(a, hostVs), &differing);
                gpu.dev->SetTexture(pp.atlasSampler, fx.clear);
                frame(2);
                gpu.dev->SetPixelShaderConstantF(0, base[0], 208);
                gpu.dev->SetPixelShaderConstantF(ip.atlasConst, fr.groundUv, 1);
                gpu.dev->SetPixelShaderConstantF(ip.strengthConst, strength, 1);
                gpu.dev->SetPixelShaderConstantF(ip.lampParamConst, lamps[0], 1);
                gpu.dev->SetPixelShaderConstantF(ip.planConst, plan[0], 2);
                gpu.dev->SetTexture(ip.atlasSampler, fx.ground);
                gpu.dev->SetTexture(ip.indexSampler, fx.index);
                const int d = WorstDiff(perDraw, gpu.Render(b, hostVs));
                t.worstDiff = std::max(t.worstDiff, d);
                if (d <= 1) t.equal++;
                if (differing > 400) t.sensitive++;
                gpu.dev->SetTexture(ip.indexSampler, fx.white);
            }
            Release(a);
            Release(b);
        }
        t.Print(gpu.dev != nullptr);
        Check(t.hosts > 0 && t.refused == 0, "windows: every window shader the per-draw patch takes takes the index copy");
        Check(t.slotsOk == t.taken && t.worstTemps <= 32, "windows: every index copy within 512 slots and 32 temporaries");
        if (gpu.dev) {
            Check(t.accepted == t.taken, "windows: native D3D9 accepts every index copy");
            Check(t.drawn > 0 && t.equal == t.drawn, "windows: the index copy draws the per-draw copy's pixels (1 LSB)");
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::puts("usage: check <lamp_index.hlsl> [Shaders_Win32.precomp]");
        return 2;
    }
    Transport();
    Slices();
    std::ifstream h(argv[1], std::ios::binary);
    const std::string hlsl((std::istreambuf_iterator<char>(h)), {});
    Check(!hlsl.empty(), "lamp_index.hlsl read");
    Gpu gpu;
    Fixture fx;
    if (gpu.dev && !MakeFixture(gpu, fx)) Check(false, "device fixture (float texture, cube, vertex shaders)");
    if (!gpu.dev) std::puts("SKIP device checks: no native D3D9 device");
    Reference(gpu, fx, hlsl);
    if (argc > 2) {
        std::ifstream f(argv[2], std::ios::binary);
        const std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), {});
        std::puts("Game shaders (the per-draw copies against the index copies):");
        GameShaders(gpu, fx, PixelShaders(d));
    } else
        std::puts("SKIP game shaders: no precomp given");
    if (gpu.dev) {
        for (DWORD s = 0; s < 16; s++) gpu.dev->SetTexture(s, nullptr);
        gpu.dev->SetPixelShader(nullptr);
        gpu.dev->SetVertexShader(nullptr);
    }
    fx.Free();
    std::printf("%d checks, %d failures\n%s\n", g_checks, g_failures, g_failures ? "PROBLEMS" : "OK");
    return g_failures ? 1 : 0;
}
