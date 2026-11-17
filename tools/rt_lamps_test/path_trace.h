// Real-time GI, S0-02 (docs/features/realtime-gi/plan.md): the offline path-traced reference. Included by check.cpp after its
// scenes, LoadWorld and ToLamp; run with --pathref (run.ps1 -PathRef). CPU only, no device.
//
// Scene: the occluder proxy (RtLampProxy::Build), story 0's ground as one quad a tile at its floor (FloorOf), furniture as
// boxes (RtLampProxy::AddShape), and the lamp table (LampDef, the game's law through ToLamp and GpuLampLaw::Eval, colour set
// to white: the layers are unit colour). Every surface is grey Lambertian with one reflectance (--albedo, default 0.5: the
// constant of design 4.3 until S0-12 names a per-room colour). The sky is black (night).
//
// Estimator, per texel point P with normal N and per lamp: direct = law(P, N) x the share of the lamp ball (radius 0.15 m)
// P sees, one random ball point a sample (the expected value of RtLampProxy::Visibility). Indirect irradiance = the mean,
// over cosine-weighted directions from P, of albedo x the irradiance at the first hit, which is again direct + indirect
// there: a path of up to --bounces vertices with next-event estimation at each; the albedo^b weight carries no division.
//
// Texels in the buffer layout of design 3.3: one a tile and story (the tile's centre), five faces: up at floor + 0.02 m
// (floors), +x, -x, +z, -z at floor + 1 m (the directional layers). A tile is a texel when it has a floor (story 0:
// FloorOf; stories 1 and up: a floor height or a placed quadrant); other texels stay 0.
//
// Output (--out DIR): one PFM a scene, lamp and story, `pathref_<scene>_l<lamp>_s<story>.pfm`: RGB float, width 5 w
// (faces up, +x, -x, +z, -z, each w wide, x left to right), height 2 h (image rows top down: h rows of indirect, tile row
// z = image row, then h rows of direct). Values are the law's units (1.0 = 255 in an 8-bit map, before any clamp).
//
// Checks: two seeds agree within 1/255 mean (all texels, and also reported over lit texels); in the sealed scene (the rooms
// with the partition's door shut) a lamp adds exactly 0 to every texel outside its own room, direct and indirect.
#pragma once
#include <atomic>
#include <thread>

namespace PathRef {

constexpr float kBall = 0.15f, kEps = 0.005f, kRay = 200.0f;
constexpr int kFaces = 5;
const float kFaceN[kFaces][3] = {{0, 1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};

struct Rng { // splitmix64: a fixed stream per (seed, texel, face), whatever the thread split
    uint64_t s;
    float Next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull, z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return static_cast<float>((z ^ (z >> 31)) >> 40) * (1.0f / 16777216.0f);
    }
};
struct Stamp { // per-call triangle marks (as RtLampProxy::Clear's), kept per thread
    std::vector<uint32_t> seen;
    uint32_t mark = 0;
    uint32_t Next() {
        if (++mark == 0) std::fill(seen.begin(), seen.end(), 0u), mark = 1;
        return mark;
    }
};
inline void Basis(const float n[3], float u[3], float w[3]) {
    const float t[3] = {std::fabs(n[1]) < 0.9f ? 0.0f : 1.0f, std::fabs(n[1]) < 0.9f ? 1.0f : 0.0f, 0.0f};
    u[0] = n[1] * t[2] - n[2] * t[1], u[1] = n[2] * t[0] - n[0] * t[2], u[2] = n[0] * t[1] - n[1] * t[0];
    const float l = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (int k = 0; k < 3; k++) u[k] /= l;
    w[0] = n[1] * u[2] - n[2] * u[1], w[1] = n[2] * u[0] - n[0] * u[2], w[2] = n[0] * u[1] - n[1] * u[0];
}

// The nearest triangle along o + t d (d unit, t in (0, tMax)), over the 1 m cells the ray's xz crosses (Clear's walk, but
// to the nearest hit): its index or -1; tHit and the unit normal facing o
inline int Nearest(const RtLampProxy::Mesh& m, const float o[3], const float d[3], float tMax, Stamp& st, float& tHit, float nOut[3]) {
    if (m.gw <= 0) return -1;
    const uint32_t mark = st.Next();
    float px = o[0] - static_cast<float>(m.gx0), pz = o[2] - static_cast<float>(m.gz0);
    int cx = static_cast<int>(std::floor(px)), cz = static_cast<int>(std::floor(pz));
    const int sx = d[0] > 0 ? 1 : -1, sz = d[2] > 0 ? 1 : -1;
    const float tdx = std::fabs(d[0]) > 1e-9f ? 1.0f / std::fabs(d[0]) : 1e30f, tdz = std::fabs(d[2]) > 1e-9f ? 1.0f / std::fabs(d[2]) : 1e30f;
    float tx = tdx < 1e29f ? (sx > 0 ? (static_cast<float>(cx + 1) - px) : (px - static_cast<float>(cx))) * tdx : 1e30f;
    float tz = tdz < 1e29f ? (sz > 0 ? (static_cast<float>(cz + 1) - pz) : (pz - static_cast<float>(cz))) * tdz : 1e30f;
    int best = -1;
    tHit = tMax;
    for (int guard = 0; guard < 100000; guard++) {
        const bool inside = cx >= 0 && cz >= 0 && cx < m.gw && cz < m.gh;
        if (inside) {
            const size_t c = static_cast<size_t>(cz) * m.gw + cx;
            for (uint32_t k = m.cellStart[c]; k < m.cellStart[c + 1]; k++) {
                const uint32_t i = m.cellTris[k];
                if (st.seen[i] == mark) continue;
                st.seen[i] = mark;
                const RtLampProxy::Tri& t = m.tris[i];
                const float e1[3] = {t.v[1][0] - t.v[0][0], t.v[1][1] - t.v[0][1], t.v[1][2] - t.v[0][2]};
                const float e2[3] = {t.v[2][0] - t.v[0][0], t.v[2][1] - t.v[0][1], t.v[2][2] - t.v[0][2]};
                const float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
                const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
                if (std::fabs(det) < 1e-12f) continue;
                const float inv = 1.0f / det;
                const float s[3] = {o[0] - t.v[0][0], o[1] - t.v[0][1], o[2] - t.v[0][2]};
                const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
                if (u < 0.0f || u > 1.0f) continue;
                const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
                const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
                if (v < 0.0f || u + v > 1.0f) continue;
                const float tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
                if (tt > 0.0f && tt < tHit) { // (no skip: o lies off every surface, and a wall a hair from o is still met)
                    tHit = tt, best = static_cast<int>(i);
                    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                    const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    const float sgn = (n[0] * d[0] + n[1] * d[1] + n[2] * d[2]) > 0.0f ? -1.0f : 1.0f;
                    for (int k2 = 0; k2 < 3; k2++) nOut[k2] = n[k2] / l * sgn;
                }
            }
        }
        const float tNext = std::min(tx, tz); // the ray leaves this cell here: a hit before it is the nearest
        if (best >= 0 && tHit <= tNext) break;
        if (tNext > tMax || (!inside && (cx < -1 || cz < -1 || cx > m.gw || cz > m.gh))) break;
        if (tx < tz) cx += sx, tx += tdx;
        else cz += sz, tz += tdz;
    }
    return best;
}

// True when the segment a -> b meets no triangle
inline bool Seg(const RtLampProxy::Mesh& m, const float a[3], const float b[3], Stamp& st) {
    float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-5f)) return true;
    for (int k = 0; k < 3; k++) d[k] /= len;
    float t, n[3];
    return Nearest(m, a, d, len, st, t, n) < 0;
}

// The share of the ball (L, kBall) P sees, one sample: a random point of the disc across L -> P that the lamp's centre sees
// (up to 4 tries, else the centre), then that point to P. No skip at either end (Visibility skips 1 cm: a ball point within 1 cm
// under a ceiling then saw through it, and a path vertex near a wall junction saw through the wall); P lies off every surface
inline float BallVis(const RtLampProxy::Mesh& m, const float L[3], const float P[3], Rng& rng, Stamp& st) {
    float d[3] = {P[0] - L[0], P[1] - L[1], P[2] - L[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-4f)) return 1.0f;
    for (float& v : d) v /= len;
    float u[3], w[3];
    Basis(d, u, w);
    for (int tries = 0; tries < 4; tries++) {
        const float r = kBall * std::sqrt(rng.Next()), a = 6.2831853f * rng.Next();
        const float ca = std::cos(a) * r, sa = std::sin(a) * r;
        const float q[3] = {L[0] + u[0] * ca + w[0] * sa, L[1] + u[1] * ca + w[1] * sa, L[2] + u[2] * ca + w[2] * sa};
        if (!Seg(m, L, q, st)) continue;
        return Seg(m, q, P, st) ? 1.0f : 0.0f;
    }
    return Seg(m, L, P, st) ? 1.0f : 0.0f;
}

#ifndef PATHREF_CORE_ONLY // (tools/gi_server_test uses the tracer above on its own scene)
struct Texel {
    int story, ix, iz, face;
    float p[3];
};
struct Result {
    std::string name;
    std::vector<int> w, h;               // per story index
    std::vector<std::vector<float>> map; // [lamp * stories + story]: (2 h) x (5 w) x rgb, the PFM's rows top down
    std::vector<double> bounceLum;       // luminance summed over texels and lamps, per bounce (0 = direct)
    std::vector<Texel> texels;
    double seconds = 0.0;
    long long rays = 0;
};
inline size_t Idx(const Result& r, int s, int row, int col) { return (static_cast<size_t>(row) * 5 * r.w[s] + col) * 3; }

inline bool HasTexel(const RtLampProxy::Story& s, int ix, int iz, float& y) {
    y = RtLampProxy::FloorOf(s, ix, iz);
    if (!(y < RtLampProxy::kNoFloor * 0.5f)) return false;
    if (s.story <= 0) return true;
    const size_t t = static_cast<size_t>(iz) * s.w + ix;
    return (t < s.floorY.size() && s.floorY[t] < RtLampProxy::kNoFloor * 0.5f) || (t < s.placed.size() && s.placed[t]);
}

// The proxy of a scene plus story 0's ground and the boxes
inline void SceneMesh(const Scene& sc, const std::vector<RtLampProxy::ObjShape>& boxes, RtLampProxy::Mesh& m) {
    RtLampProxy::Build(sc.lot, m, sc.ceilingsOpen);
    int low = 1 << 30;
    for (const RtLampProxy::Story& s : sc.lot.stories) low = std::min(low, s.story);
    m.curKind = RtLampProxy::Mesh::kSlab;
    for (const RtLampProxy::Story& s : sc.lot.stories) {
        if (s.story != low) continue;
        for (int z = 0; z < s.h; z++)
            for (int x = 0; x < s.w; x++) {
                const float y = RtLampProxy::FloorOf(s, x, z);
                if (!(y < RtLampProxy::kNoFloor * 0.5f)) continue;
                const float a[3] = {float(x), y, float(z)}, b[3] = {float(x + 1), y, float(z)}, c[3] = {float(x + 1), y, float(z + 1)}, d[3] = {float(x), y, float(z + 1)};
                RtLampProxy::AddTri(m, a, b, c), RtLampProxy::AddTri(m, a, c, d);
            }
    }
    for (const RtLampProxy::ObjShape& o : boxes) RtLampProxy::AddShape(m, o);
    RtLampProxy::BuildGrid(m);
}

inline Result Solve(const Scene& sc, const std::vector<RtLampProxy::ObjShape>& boxes, int spp, int bounces, float albedo, uint64_t seed) {
    Result res;
    res.name = sc.name;
    RtLampProxy::Mesh m;
    SceneMesh(sc, boxes, m);
    std::vector<GpuLampLaw::Lamp> laws;
    for (LampDef d : sc.lamps) {
        d.colour[0] = d.colour[1] = d.colour[2] = 1.0f;
        laws.push_back(ToLamp(d).law);
    }
    const int nS = static_cast<int>(sc.lot.stories.size()), nL = static_cast<int>(laws.size());
    for (const RtLampProxy::Story& s : sc.lot.stories) res.w.push_back(s.w), res.h.push_back(s.h);
    res.map.resize(static_cast<size_t>(nL) * nS);
    for (int l = 0; l < nL; l++)
        for (int s = 0; s < nS; s++) res.map[static_cast<size_t>(l) * nS + s].assign(static_cast<size_t>(2 * res.h[s]) * 5 * res.w[s] * 3, 0.0f);
    for (int s = 0; s < nS; s++) {
        const RtLampProxy::Story& st = sc.lot.stories[s];
        for (int z = 0; z < st.h; z++)
            for (int x = 0; x < st.w; x++) {
                float y;
                if (!HasTexel(st, x, z, y)) continue;
                for (int f = 0; f < kFaces; f++) res.texels.push_back({s, x, z, f, {x + 0.5f, y + (f == 0 ? 0.02f : 1.0f), z + 0.5f}});
            }
    }
    const int nT = static_cast<int>(res.texels.size());
    const int threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::vector<double>> bl(threads, std::vector<double>(bounces + 1, 0.0));
    std::vector<long long> rays(threads, 0);
    std::atomic<int> next{0};
    const auto t0 = std::chrono::steady_clock::now();
    const auto work = [&](int th) {
        Stamp st;
        st.seen.assign(m.tris.size(), 0);
        std::vector<float> dir(static_cast<size_t>(nL) * 3), ind(static_cast<size_t>(nL) * 3);
        float law[3];
        for (int i; (i = next.fetch_add(1)) < nT;) {
            const Texel& tx = res.texels[i];
            Rng rng{seed * 0x100000001B3ull ^ (static_cast<uint64_t>(i) << 20)};
            std::fill(dir.begin(), dir.end(), 0.0f), std::fill(ind.begin(), ind.end(), 0.0f);
            const float* N = kFaceN[tx.face];
            const float wr = GpuLampLaw::WallReach(N, true);
            for (int k = 0; k < spp; k++) {
                for (int l = 0; l < nL; l++) { // direct
                    GpuLampLaw::Eval(laws[l], tx.p, N, wr, law);
                    if (std::max({law[0], law[1], law[2]}) <= 0.0f) continue;
                    const float v = BallVis(m, laws[l].pos, tx.p, rng, st);
                    for (int c = 0; c < 3; c++) dir[l * 3 + c] += law[c] * v;
                    bl[th][0] += v * OutdoorDeltaModel::Lum3(law);
                }
                float o[3] = {tx.p[0], tx.p[1], tx.p[2]}, n[3] = {N[0], N[1], N[2]}, thr = 1.0f;
                for (int b = 1; b <= bounces; b++) { // indirect: cosine-weighted, next-event estimation at each hit
                    float u[3], w[3];
                    Basis(n, u, w);
                    const float r1 = rng.Next(), r2 = rng.Next(), r = std::sqrt(r1), a = 6.2831853f * r2, cz = std::sqrt(std::max(0.0f, 1.0f - r1));
                    const float d[3] = {u[0] * r * std::cos(a) + w[0] * r * std::sin(a) + n[0] * cz, u[1] * r * std::cos(a) + w[1] * r * std::sin(a) + n[1] * cz,
                                        u[2] * r * std::cos(a) + w[2] * r * std::sin(a) + n[2] * cz};
                    float tHit, hn[3];
                    rays[th]++;
                    if (Nearest(m, o, d, kRay, st, tHit, hn) < 0) break;
                    thr *= albedo;
                    // the vertex: kEps back along the ray (the side o is on, whatever else meets there) and kEps off the surface
                    const float tb = std::max(tHit - kEps, 0.5f * tHit);
                    const float h[3] = {o[0] + d[0] * tb + hn[0] * kEps, o[1] + d[1] * tb + hn[1] * kEps, o[2] + d[2] * tb + hn[2] * kEps};
                    const float hwr = GpuLampLaw::WallReach(hn, true);
                    for (int l = 0; l < nL; l++) {
                        GpuLampLaw::Eval(laws[l], h, hn, hwr, law);
                        if (std::max({law[0], law[1], law[2]}) <= 0.0f) continue;
                        const float v = BallVis(m, laws[l].pos, h, rng, st) * thr;
                        for (int c = 0; c < 3; c++) ind[l * 3 + c] += law[c] * v;
                        bl[th][b] += v * OutdoorDeltaModel::Lum3(law);
                    }
                    for (int c = 0; c < 3; c++) o[c] = h[c], n[c] = hn[c];
                }
            }
            for (int l = 0; l < nL; l++) {
                std::vector<float>& mp = res.map[static_cast<size_t>(l) * nS + tx.story];
                const size_t ii = Idx(res, tx.story, tx.iz, tx.face * res.w[tx.story] + tx.ix), di = Idx(res, tx.story, res.h[tx.story] + tx.iz, tx.face * res.w[tx.story] + tx.ix);
                for (int c = 0; c < 3; c++) mp[ii + c] = ind[l * 3 + c] / spp, mp[di + c] = dir[l * 3 + c] / spp;
            }
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; t++) pool.emplace_back(work, t);
    for (std::thread& t : pool) t.join();
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    res.bounceLum.assign(bounces + 1, 0.0);
    for (int t = 0; t < threads; t++) {
        res.rays += rays[t];
        for (int b = 0; b <= bounces; b++) res.bounceLum[b] += bl[t][b] / spp;
    }
    return res;
}

inline bool WritePfm(const std::string& path, int w, int h, const std::vector<float>& rgb) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << "PF\n" << w << " " << h << "\n-1.0\n";
    for (int row = h - 1; row >= 0; row--) f.write(reinterpret_cast<const char*>(&rgb[static_cast<size_t>(row) * w * 3]), static_cast<std::streamsize>(w) * 3 * sizeof(float));
    return static_cast<bool>(f);
}

// The rooms of IndoorRooms (story 0: A x 4..8, B x 9..13; story 1: one room) with a lamp's room: 0 A, 1 B, 2 upstairs, -1 outside
inline int RoomOf(int story, int ix, int iz) {
    if (iz < 4 || iz >= 12 || ix < 4 || ix >= 14) return -1;
    return story == 1 ? 2 : ix < 9 ? 0 : 1;
}
inline Scene Sealed() {
    Scene sc = IndoorRooms();
    sc.name = "sealed rooms (the partition's door shut)";
    for (RtLampProxy::Story& st : sc.lot.stories)
        for (RtLampProxy::Edge& e : st.edges) e.open = false;
    for (LampDef& l : sc.lamps) l.room = RoomOf(l.pos[1] >= 3.0f ? 1 : 0, static_cast<int>(l.pos[0]), static_cast<int>(l.pos[2]));
    return sc;
}
// A table, a cupboard against the outer wall, a chair and a sofa in the rooms of IndoorRooms (axis-aligned boxes)
inline std::vector<RtLampProxy::ObjShape> Boxes() {
    std::vector<RtLampProxy::ObjShape> out;
    const auto add = [&](float x0, float y0, float z0, float x1, float y1, float z1) {
        RtLampProxy::ObjShape o{};
        o.c[0] = 0.5f * (x0 + x1), o.c[1] = 0.5f * (y0 + y1), o.c[2] = 0.5f * (z0 + z1);
        o.h[0] = 0.5f * (x1 - x0), o.h[1] = 0.5f * (y1 - y0), o.h[2] = 0.5f * (z1 - z0);
        o.u[0][0] = o.u[1][1] = o.u[2][2] = 1.0f;
        out.push_back(o);
    };
    add(5.9f, 0.70f, 7.4f, 7.1f, 0.75f, 8.6f);  // table top
    add(10.2f, 0.0f, 4.2f, 11.4f, 1.9f, 4.8f);  // cupboard
    add(5.2f, 0.0f, 10.0f, 5.8f, 0.45f, 10.6f); // chair seat
    add(5.2f, 0.45f, 10.5f, 5.8f, 1.0f, 10.6f); // chair back
    add(10.0f, 0.0f, 10.8f, 12.0f, 0.8f, 11.8f); // sofa
    return out;
}

inline int Main(int argc, char** argv) {
    int spp = 512, bounces = 4, maxLamps = 12;
    float albedo = 0.5f;
    std::string out;
    std::vector<std::string> worlds;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--pathref") continue;
        if (a == "--spp" && i + 1 < argc) spp = std::max(1, std::atoi(argv[++i]));
        else if (a == "--bounces" && i + 1 < argc) bounces = std::clamp(std::atoi(argv[++i]), 0, 16);
        else if (a == "--albedo" && i + 1 < argc) albedo = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--lamps" && i + 1 < argc) maxLamps = std::max(1, std::atoi(argv[++i]));
        else worlds.push_back(a);
    }
    std::printf("[path-traced reference] %d samples a texel face, %d bounces, albedo %.2f, %u threads\n", spp, bounces, albedo, std::max(1u, std::thread::hardware_concurrency()));
    struct Job {
        Scene sc;
        std::vector<RtLampProxy::ObjShape> boxes;
        std::string tag;
    };
    std::vector<Job> jobs;
    jobs.push_back({IndoorRooms(), Boxes(), "rooms"});
    jobs.back().sc.name += " + furniture boxes";
    jobs.push_back({Sealed(), Boxes(), "sealed"});
    jobs.push_back({Synthetic(), {}, "house"});
    for (const std::string& w : worlds) {
        std::vector<Scene> scs;
        if (!LoadWorld(w, scs, maxLamps)) {
            Check(false, "capture not readable: " + w);
            continue;
        }
        for (Scene& s : scs) jobs.push_back({std::move(s), {}, std::format("lot{}", jobs.size())});
    }
    for (const Job& j : jobs) {
        const Result a = Solve(j.sc, j.boxes, spp, bounces, albedo, 1), b = Solve(j.sc, j.boxes, spp, bounces, albedo, 2);
        double tot = 0.0;
        for (double v : a.bounceLum) tot += v;
        std::printf("\n== %s: %zu lamps, %zu texel faces, %.2f s a seed, %.2f M rays/s (path segments; shadow rays not counted)\n", j.sc.name.c_str(), j.sc.lamps.size(),
                    a.texels.size(), a.seconds, a.rays / std::max(a.seconds, 1e-6) / 1e6);
        std::printf("   light by bounce (share of the total):");
        for (size_t k = 0; k < a.bounceLum.size(); k++) std::printf(" %zu: %.3f", k, tot > 0 ? a.bounceLum[k] / tot : 0.0);
        std::printf("\n");
        // two seeds: mean |a - b| over every value, and over the lit ones (either seed at 1/255 or more)
        double sum = 0.0, sumLit = 0.0, worst = 0.0;
        long long n = 0, nLit = 0;
        for (size_t k = 0; k < a.map.size(); k++)
            for (size_t i = 0; i < a.map[k].size(); i++) {
                const double d = std::fabs(static_cast<double>(a.map[k][i]) - b.map[k][i]);
                sum += d, n++, worst = std::max(worst, d);
                if (std::max(a.map[k][i], b.map[k][i]) >= 1.0f / 255.0f) sumLit += d, nLit++;
            }
        const double mean = n ? sum / n : 0.0, meanLit = nLit ? sumLit / nLit : 0.0;
        std::printf("   two seeds: mean |diff| %.3f/255 (all %lld values), %.3f/255 (lit %lld), largest %.3f/255\n", mean * 255, n, meanLit * 255, nLit, worst * 255);
        Check(n > 0 && mean * 255 < 1.0, j.tag + ": two seeds differ by 1/255 or more on the mean");
        const int nS = static_cast<int>(j.sc.lot.stories.size());
        for (size_t l = 0; l < j.sc.lamps.size(); l++) {
            double di = 0.0, ii = 0.0, peak = 0.0;
            for (int s = 0; s < nS; s++) {
                const std::vector<float>& mp = a.map[l * nS + s];
                const size_t half = mp.size() / 2;
                for (size_t i = 0; i < half; i++) ii += mp[i], peak = std::max(peak, static_cast<double>(mp[i]));
                for (size_t i = half; i < mp.size(); i++) di += mp[i];
            }
            std::printf("   lamp %-40s indirect / direct %.3f, indirect peak %.1f/255\n", j.sc.lamps[l].name.c_str(), di > 0 ? ii / di : 0.0, peak * 255);
        }
        if (j.tag == "sealed") {
            long long outside = 0, lit = 0;
            for (size_t l = 0; l < j.sc.lamps.size(); l++)
                for (int s = 0; s < nS; s++) {
                    const std::vector<float>& mp = a.map[l * nS + s];
                    const int w = a.w[s], h = a.h[s];
                    for (int row = 0; row < 2 * h; row++)
                        for (int col = 0; col < 5 * w; col++) {
                            if (RoomOf(j.sc.lot.stories[s].story, col % w, row % h) == j.sc.lamps[l].room) continue;
                            const size_t i = Idx(a, s, row, col);
                            outside++;
                            if (mp[i] != 0.0f || mp[i + 1] != 0.0f || mp[i + 2] != 0.0f) lit++;
                        }
                }
            std::printf("   sealed: %lld of %lld values outside a lamp's own room are not exactly 0\n", lit, outside);
            Check(outside > 0 && lit == 0, "sealed: a lamp lights a texel outside its closed room");
        }
        if (!out.empty()) {
            CreateDirectoryA(out.c_str(), nullptr);
            for (size_t l = 0; l < j.sc.lamps.size(); l++)
                for (int s = 0; s < nS; s++) {
                    const std::string p = std::format("{}\\pathref_{}_l{}_s{}.pfm", out, j.tag, l, j.sc.lot.stories[s].story);
                    Check(WritePfm(p, 5 * a.w[s], 2 * a.h[s], a.map[l * nS + s]), "cannot write " + p);
                }
        }
    }
    if (!out.empty()) std::printf("\nmaps written to %s\n", out.c_str());
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
#endif

} // namespace PathRef
