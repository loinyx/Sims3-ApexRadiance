// Real-time GI, X-05 (docs/features/realtime-gi/wp/X-05.md): the radiance cascades of features/plan_cascades.h against the
// path-traced reference of path_trace.h on its three scenes. Included by check.cpp after path_trace.h; run with --cascades
// (run.ps1 -Cascades). CPU only, no device, writes no file.
//
// The cascades carry light from an emitter image to the probes; what feeds the image decides their role:
//   all bounces, exact start   the lamp's direct light on walls (three heights), floor and ceiling a field pixel, then the
//                              cascades' own output, 4 times: bounces 1 to 4
//   all bounces, tile start    the same from the reference's direct maps (one texel a tile, five faces: the light buffer of
//                              design 3.3; the ceiling is taken as the floor)
//   bounces 2+                 the reference's first bounce a tile (what a texel layer's first gather gives), then the
//                              cascades 3 times for bounces 2 to 4
// each against the reference's indirect light of 4 bounces at its own texels (tile centres), with "the first bounce
// alone" as the bar a later-bounce method must beat. Sealed rooms: what crosses to a room no opening joins. One room at
// 4 texels a metre: whether the cascades add detail a tile layer has not.
#pragma once
#include "plan_cascades_case.h"

namespace CascadesRef {
using namespace PlanCascades;
using PathRef::Rng;
using PathRef::Stamp;

struct Faces { // kOut values a cell: irradiance of +x, -x, +z, -z, floor, ceiling
    int nx = 0, nz = 0;
    float cell = 1.0f;
    std::vector<float> v;
};
struct Ctx {
    const RtLampProxy::Story* st = nullptr;
    const RtLampProxy::Story* above = nullptr;
    Field f;
    std::vector<uint8_t> floor, ceil; // per tile
    std::vector<float> floorY;
};
inline Ctx MakeCtx(const RtLampProxy::Lot& lot, int s) {
    Ctx c;
    c.st = &lot.stories[s], c.above = s + 1 < static_cast<int>(lot.stories.size()) ? &lot.stories[s + 1] : nullptr;
    c.f = PlanField::BuildField(*c.st);
    const int w = c.st->w, h = c.st->h;
    c.floor.assign(static_cast<size_t>(w) * h, 0), c.ceil.assign(static_cast<size_t>(w) * h, 0), c.floorY.assign(static_cast<size_t>(w) * h, 0.0f);
    for (int z = 0; z < h; z++)
        for (int x = 0; x < w; x++) {
            float y = 0.0f;
            c.floor[static_cast<size_t>(z) * w + x] = PathRef::HasTexel(*c.st, x, z, y);
            c.floorY[static_cast<size_t>(z) * w + x] = y < RtLampProxy::kNoFloor * 0.5f ? y : c.f.base;
            c.ceil[static_cast<size_t>(z) * w + x] = !PlanField::SlabOpen(*c.st, c.above, x + 0.5f, z + 0.3f);
        }
    return c;
}
inline float PxX(const Field& f, int ix) { return (static_cast<float>(ix - PlanField::kPad) + 0.5f) / static_cast<float>(f.px); }
// The unit normal of the nearest wall at a field pixel, toward the pixel's side (the field's gradient); false: none
inline bool WallNormal(const Field& f, int band, int ix, int iz, float n[2]) {
    if (ix < 1 || iz < 1 || ix >= f.w - 1 || iz >= f.h - 1) return false;
    const float* d = &f.d[(static_cast<size_t>(band) * f.h + iz) * f.w + ix];
    n[0] = d[1] - d[-1], n[1] = d[f.w] - d[-f.w];
    const float l = std::hypot(n[0], n[1]);
    if (l < 1e-3f) return false;
    n[0] /= l, n[1] /= l;
    return true;
}
inline int TileOf(const Ctx& c, float x, float z) {
    return std::clamp(static_cast<int>(std::floor(z)), 0, c.st->h - 1) * c.st->w + std::clamp(static_cast<int>(std::floor(x)), 0, c.st->w - 1);
}

// One lamp's direct irradiance at P, N: the law x the share of its ball seen, k samples (path_trace.h's direct term)
inline float Direct(const RtLampProxy::Mesh& m, const GpuLampLaw::Lamp& law, const float P[3], const float N[3], int k, Rng& rng, Stamp& st) {
    float l[3];
    GpuLampLaw::Eval(law, P, N, GpuLampLaw::WallReach(N, true), l);
    if (std::max({l[0], l[1], l[2]}) <= 0.0f) return 0.0f;
    float vis = 0.0f;
    for (int i = 0; i < k; i++) vis += PathRef::BallVis(m, law.pos, P, rng, st);
    return l[0] * vis / static_cast<float>(k);
}

// The emitter image of one lamp's direct light: walls a field pixel (the mean of three heights), floor and ceiling a quarter tile
inline Emit EmitDirect(const Ctx& c, const RtLampProxy::Mesh& m, const GpuLampLaw::Lamp& law, float albedo, int band) {
    const Field& f = c.f;
    Emit e;
    e.w = f.w, e.h = f.h, e.v.assign(static_cast<size_t>(f.w) * f.h * 4, 0.0f);
    const int cw = c.st->w * 4, ch = c.st->h * 4;
    std::vector<float> sheet(static_cast<size_t>(cw) * ch * 2, 0.0f);
    Rows(ch, [&](int cz) {
        Stamp st;
        st.seen.assign(m.tris.size(), 0);
        Rng rng{0xC0FFEEull ^ (static_cast<uint64_t>(cz) << 24)};
        for (int cx = 0; cx < cw; cx++) {
            const float x = (cx + 0.5f) * 0.25f, z = (cz + 0.5f) * 0.25f;
            const int t = TileOf(c, x, z);
            const float up[3] = {0, 1, 0}, down[3] = {0, -1, 0}, pf[3] = {x, c.floorY[t] + 0.02f, z}, pc[3] = {x, c.f.base + kStoryH - 0.02f, z};
            if (c.floor[t]) sheet[(static_cast<size_t>(cz) * cw + cx) * 2] = albedo * Direct(m, law, pf, up, 8, rng, st);
            if (c.ceil[t]) sheet[(static_cast<size_t>(cz) * cw + cx) * 2 + 1] = albedo * Direct(m, law, pc, down, 8, rng, st);
        }
    });
    Rows(f.h, [&](int iz) {
        Stamp st;
        st.seen.assign(m.tris.size(), 0);
        Rng rng{0xBEEFull ^ (static_cast<uint64_t>(iz) << 24)};
        for (int ix = 0; ix < f.w; ix++) {
            const float x = PxX(f, ix), z = PxX(f, iz);
            float* o = &e.v[(static_cast<size_t>(iz) * f.w + ix) * 4];
            const int cx = std::clamp(static_cast<int>(std::floor(x * 4.0f)), 0, cw - 1), cz = std::clamp(static_cast<int>(std::floor(z * 4.0f)), 0, ch - 1);
            o[1] = sheet[(static_cast<size_t>(cz) * cw + cx) * 2], o[2] = sheet[(static_cast<size_t>(cz) * cw + cx) * 2 + 1];
            const float d = f.d[(static_cast<size_t>(band) * f.h + iz) * f.w + ix];
            float n[2];
            if (d >= 4.0f || !WallNormal(f, band, ix, iz, n)) continue;
            const float off = std::max(0.0f, 0.5f - d) / static_cast<float>(f.px), N[3] = {n[0], 0, n[1]};
            float sum = 0.0f;
            for (const float y : {0.5f, 1.5f, 2.5f}) {
                const float P[3] = {x + n[0] * off, f.base + y, z + n[1] * off};
                sum += Direct(m, law, P, N, 4, rng, st);
            }
            o[0] = albedo * sum / 3.0f;
        }
    });
    return e;
}
// The emitter image of a light buffer: a pixel reads the cell it lies in; a wall takes the faces its normal looks along
inline Emit EmitFaces(const Ctx& c, const Faces& fc, float albedo, int band) {
    const Field& f = c.f;
    Emit e;
    e.w = f.w, e.h = f.h, e.v.assign(static_cast<size_t>(f.w) * f.h * 4, 0.0f);
    for (int iz = 0; iz < f.h; iz++)
        for (int ix = 0; ix < f.w; ix++) {
            const float x = PxX(f, ix), z = PxX(f, iz);
            const int cx = std::clamp(static_cast<int>(std::floor(x / fc.cell)), 0, fc.nx - 1), cz = std::clamp(static_cast<int>(std::floor(z / fc.cell)), 0, fc.nz - 1), t = TileOf(c, x, z);
            const float* v = &fc.v[(static_cast<size_t>(cz) * fc.nx + cx) * kOut];
            float* o = &e.v[(static_cast<size_t>(iz) * f.w + ix) * 4];
            o[1] = c.floor[t] ? albedo * v[4] : 0.0f, o[2] = c.ceil[t] ? albedo * v[5] : 0.0f;
            float n[2];
            if (f.d[(static_cast<size_t>(band) * f.h + iz) * f.w + ix] >= 4.0f || !WallNormal(f, band, ix, iz, n)) continue;
            const float px = std::max(n[0], 0.0f), mx = std::max(-n[0], 0.0f), pz = std::max(n[1], 0.0f), mz = std::max(-n[1], 0.0f);
            o[0] = albedo * (px * px * v[0] + mx * mx * v[1] + pz * pz * v[2] + mz * mz * v[3]);
        }
    return e;
}
// A reference map's rows (0: indirect, h: direct) as a light buffer; the ceiling is taken as the floor
inline Faces FromMap(const PathRef::Result& r, int s, const std::vector<float>& map, int row0) {
    Faces fc;
    fc.nx = r.w[s], fc.nz = r.h[s], fc.cell = 1.0f, fc.v.assign(static_cast<size_t>(fc.nx) * fc.nz * kOut, 0.0f);
    for (int z = 0; z < fc.nz; z++)
        for (int x = 0; x < fc.nx; x++) {
            float* o = &fc.v[(static_cast<size_t>(z) * fc.nx + x) * kOut];
            for (int k = 0; k < 4; k++) o[k] = map[PathRef::Idx(r, s, row0 + z, (k + 1) * fc.nx + x)];
            o[4] = o[5] = map[PathRef::Idx(r, s, row0 + z, x)];
        }
    return fc;
}

struct Run {
    Faces sum;        // the passes' outputs added
    int solves = 0;
    double seconds = 0.0;
};
// `passes` times: the cascades over the emitter image, then their output as the next image
inline Run Bounce(const Ctx& c, Emit e, const Params& p, int passes, float albedo) {
    Run r;
    for (int b = 0; b < passes; b++) {
        if (*std::max_element(e.v.begin(), e.v.end()) <= 0.0f) break;
        Layout l;
        const auto t0 = std::chrono::steady_clock::now();
        Faces out;
        out.v = Solve(c.f, e, p, &l);
        r.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), r.solves++;
        out.nx = l.nx, out.nz = l.nz, out.cell = p.spacing;
        if (r.sum.v.empty()) r.sum = out;
        else
            for (size_t i = 0; i < out.v.size(); i++) r.sum.v[i] += out.v[i];
        if (b + 1 < passes) e = EmitFaces(c, out, albedo, p.band);
    }
    return r;
}
// A light buffer at (x, z): bilinear over its cells, clamped at the border
inline void At(const Faces& fc, float x, float z, float out[kOut]) {
    std::fill(out, out + kOut, 0.0f);
    if (fc.v.empty()) return;
    const float ux = std::clamp(x / fc.cell - 0.5f, 0.0f, static_cast<float>(fc.nx - 1)), uz = std::clamp(z / fc.cell - 0.5f, 0.0f, static_cast<float>(fc.nz - 1));
    const int bx = std::min(static_cast<int>(ux), std::max(fc.nx - 2, 0)), bz = std::min(static_cast<int>(uz), std::max(fc.nz - 2, 0));
    const float fx = ux - static_cast<float>(bx), fz = uz - static_cast<float>(bz);
    for (int k = 0; k < 4; k++) {
        const int qx = std::min(bx + (k & 1), fc.nx - 1), qz = std::min(bz + (k >> 1), fc.nz - 1);
        const float w = ((k & 1) ? fx : 1.0f - fx) * ((k >> 1) ? fz : 1.0f - fz);
        for (int i = 0; i < kOut; i++) out[i] += w * fc.v[(static_cast<size_t>(qz) * fc.nx + qx) * kOut + i];
    }
}

struct Tally {
    double sum = 0, sumRef = 0, err = 0, errLit = 0, floorErr = 0, wallErr = 0, leakMax = 0;
    long n = 0, lit = 0, nFloor = 0, nWall = 0, leak = 0, outside = 0;
    void Add(float v, float ref, bool floorFace) {
        const double d = std::fabs(static_cast<double>(v) - ref);
        sum += v, sumRef += ref, err += d, n++;
        if (ref < 1.0f / 255.0f) return;
        errLit += d, lit++;
        (floorFace ? floorErr : wallErr) += d, (floorFace ? nFloor : nWall)++;
    }
    void Print(const char* what, double seconds = 0, int solves = 0) const {
        std::printf("   %-58s sum / reference %.3f | mean |diff| %.2f/255 all, %.2f lit (floor %.2f, faces %.2f)", what, sumRef > 0 ? sum / sumRef : 0.0, n ? 255 * err / n : 0.0,
                    lit ? 255 * errLit / lit : 0.0, nFloor ? 255 * floorErr / nFloor : 0.0, nWall ? 255 * wallErr / nWall : 0.0);
        if (outside) std::printf(" | outside the lamp's room: %ld of %ld lit, largest %.4f/255", leak, outside, 255 * leakMax);
        if (solves) std::printf(" | %.0f ms a solve", 1000.0 * seconds / solves);
        std::printf("\n");
    }
};

// The indirect light of all lamps at 4 floor points a metre inside x0..x1, z0..z1 of story 0 (path_trace.h's estimator at free points)
inline std::vector<float> FineFloor(const RtLampProxy::Mesh& m, const std::vector<GpuLampLaw::Lamp>& laws, int x0, int x1, int z0, int z1, int spp, int bounces, float albedo) {
    const int w = (x1 - x0) * 4, h = (z1 - z0) * 4, nL = static_cast<int>(laws.size());
    std::vector<float> out(static_cast<size_t>(w) * h * nL, 0.0f);
    Rows(h, [&](int cz) {
        Stamp st;
        st.seen.assign(m.tris.size(), 0);
        for (int cx = 0; cx < w; cx++) {
            Rng rng{0xF1E1Dull ^ (static_cast<uint64_t>(cz * w + cx) << 20)};
            float* o = &out[(static_cast<size_t>(cz) * w + cx) * nL];
            for (int k = 0; k < spp; k++) {
                float p[3] = {x0 + (cx + 0.5f) * 0.25f, 0.02f, z0 + (cz + 0.5f) * 0.25f}, n[3] = {0, 1, 0}, thr = 1.0f;
                for (int b = 1; b <= bounces; b++) {
                    float u[3], wv[3];
                    PathRef::Basis(n, u, wv);
                    const float r1 = rng.Next(), r2 = rng.Next(), r = std::sqrt(r1), a = 6.2831853f * r2, cz2 = std::sqrt(std::max(0.0f, 1.0f - r1));
                    float d[3];
                    for (int i = 0; i < 3; i++) d[i] = u[i] * r * std::cos(a) + wv[i] * r * std::sin(a) + n[i] * cz2;
                    float tHit, hn[3];
                    if (PathRef::Nearest(m, p, d, PathRef::kRay, st, tHit, hn) < 0) break;
                    thr *= albedo;
                    const float tb = std::max(tHit - PathRef::kEps, 0.5f * tHit);
                    for (int i = 0; i < 3; i++) p[i] += d[i] * tb + hn[i] * PathRef::kEps, n[i] = hn[i];
                    for (int l = 0; l < nL; l++) o[l] += thr * Direct(m, laws[l], p, n, 1, rng, st) / static_cast<float>(spp);
                }
            }
        }
    });
    return out;
}

inline int Main(int argc, char** argv) {
    int spp = 512;
    float albedo = 0.5f;
    for (int i = 1; i < argc; i++)
        if (std::string(argv[i]) == "--spp" && i + 1 < argc) spp = std::max(1, std::atoi(argv[++i]));
    PlanCascadesChecks();
    std::printf("\n[radiance cascades against the path-traced reference] %d samples a texel face, 4 bounces, albedo %.2f\n", spp, albedo);
    struct Job {
        Scene sc;
        std::vector<RtLampProxy::ObjShape> boxes;
        std::string tag;
    };
    std::vector<Job> jobs;
    jobs.push_back({IndoorRooms(), PathRef::Boxes(), "rooms"});
    jobs.push_back({PathRef::Sealed(), PathRef::Boxes(), "sealed"});
    jobs.push_back({Synthetic(), {}, "house"});
    struct Arm {
        std::string name;
        int feed; // 0 direct a pixel, 1 direct a tile, 2 the first bounce a tile
        Params p;
    };
    std::vector<Arm> arms;
    const Params base;
    arms.push_back({"all bounces, exact start", 0, base});
    arms.push_back({"all bounces, tile start", 1, base});
    arms.push_back({"bounces 2+", 2, base});
    for (const int levels : {1, 2}) {
        Params p = base;
        p.levels = levels;
        arms.push_back({std::format("bounces 2+, {} level{} (to {:.0f} m)", levels, levels > 1 ? "s" : "", p.l0 * ((1 << (2 * levels)) - 1) / 3.0f), 2, p});
    }
    {
        Params p = base;
        p.fix = false;
        arms.push_back({"bounces 2+, no fix", 2, p});
        arms.push_back({"all bounces, exact start, no fix", 0, p});
        p = base, p.d0 = 4, p.l0 = 0.25f, p.levels = 4;
        arms.push_back({"bounces 2+, 4 directions, l0 0.25 m, 4 levels", 2, p});
        p = base, p.spacing = 0.5f, p.l0 = 2.0f, p.levels = 2;
        arms.push_back({"bounces 2+, 0.5 m probes, l0 2 m, 2 levels (to 10 m)", 2, p});
        p = base, p.spacing = 0.5f, p.l0 = 2.0f, p.levels = 3;
        arms.push_back({"bounces 2+, 0.5 m probes, l0 2 m, 3 levels (to 42 m)", 2, p});
    }
    for (const Job& j : jobs) {
        const PathRef::Result ref = PathRef::Solve(j.sc, j.boxes, spp, 4, albedo, 1), one = PathRef::Solve(j.sc, j.boxes, spp, 1, albedo, 1), seed2 = PathRef::Solve(j.sc, j.boxes, spp, 4, albedo, 2);
        RtLampProxy::Mesh m;
        PathRef::SceneMesh(j.sc, j.boxes, m);
        std::vector<GpuLampLaw::Lamp> laws;
        for (LampDef d : j.sc.lamps) {
            d.colour[0] = d.colour[1] = d.colour[2] = 1.0f;
            laws.push_back(ToLamp(d).law);
        }
        const int nS = static_cast<int>(j.sc.lot.stories.size()), nL = static_cast<int>(laws.size());
        std::vector<Ctx> ctx;
        for (int s = 0; s < nS; s++) ctx.push_back(MakeCtx(j.sc.lot, s));
        std::printf("\n== %s: %d lamps, %d stories of %d x %d tiles\n", j.sc.name.c_str(), nL, nS, ref.w[0], ref.h[0]);
        // every texel face of the reference against `get(lamp, story)`'s light buffer added to `first` (null: nothing)
        const auto tally = [&](const std::function<const Faces*(int, int)>& get, const PathRef::Result* first) {
            Tally t;
            for (const PathRef::Texel& tx : ref.texels)
                for (int l = 0; l < nL; l++) {
                    const std::vector<float>& mp = ref.map[static_cast<size_t>(l) * nS + tx.story];
                    const size_t at = PathRef::Idx(ref, tx.story, tx.iz, tx.face * ref.w[tx.story] + tx.ix);
                    float v[kOut] = {};
                    if (const Faces* fc = get(l, tx.story)) At(*fc, tx.ix + 0.5f, tx.iz + 0.5f, v);
                    const float x = (tx.face == 0 ? v[4] : v[tx.face - 1]) + (first ? first->map[static_cast<size_t>(l) * nS + tx.story][at] : 0.0f);
                    t.Add(x, mp[at], tx.face == 0);
                    if (j.tag == "sealed" && PathRef::RoomOf(j.sc.lot.stories[tx.story].story, tx.ix, tx.iz) != j.sc.lamps[l].room) t.outside++, t.leak += x != 0.0f, t.leakMax = std::max(t.leakMax, static_cast<double>(x));
                }
            return t;
        };
        tally([&](int l, int s) { static Faces fc; return fc = FromMap(seed2, s, seed2.map[static_cast<size_t>(l) * nS + s], 0), &fc; }, nullptr).Print("the reference's second seed (its noise)");
        tally([](int, int) { return nullptr; }, &one).Print("the first bounce alone (no later bounces)");
        std::vector<std::vector<Faces>> kept(2); // (arms 0 and 2, for the detail test)
        for (size_t a = 0; a < arms.size(); a++) {
            const Arm& arm = arms[a];
            if (a >= 3 && j.tag != "rooms" && !(j.tag == "sealed" && !arm.p.fix)) continue; // (the sweep runs on the first scene)
            std::vector<Faces> res(static_cast<size_t>(nL) * nS);
            double seconds = 0;
            int solves = 0;
            for (int l = 0; l < nL; l++)
                for (int s = 0; s < nS; s++) {
                    const std::vector<float>& mp = ref.map[static_cast<size_t>(l) * nS + s];
                    const Emit e = arm.feed == 0   ? EmitDirect(ctx[s], m, laws[l], albedo, arm.p.band)
                                   : arm.feed == 1 ? EmitFaces(ctx[s], FromMap(ref, s, mp, ref.h[s]), albedo, arm.p.band)
                                                   : EmitFaces(ctx[s], FromMap(one, s, one.map[static_cast<size_t>(l) * nS + s], 0), albedo, arm.p.band);
                    Run r = Bounce(ctx[s], e, arm.p, arm.feed == 2 ? 3 : 4, albedo);
                    seconds += r.seconds, solves += r.solves, res[static_cast<size_t>(l) * nS + s] = std::move(r.sum);
                }
            const Tally t = tally([&](int l, int s) { return &res[static_cast<size_t>(l) * nS + s]; }, arm.feed == 2 ? &one : nullptr);
            t.Print(arm.name.c_str(), seconds, solves);
            // (not exactly 0: beside a wall corner the field's pixel holds one of the two walls' seeds, see wp/X-05.md)
            if (j.tag == "sealed" && arm.p.fix) Check(t.leakMax < 0.05 / 255.0, "cascades, sealed: light reaches a room no opening joins (" + arm.name + ")");
            if (a == 0 || a == 2) kept[a / 2] = std::move(res);
        }
        if (j.tag != "rooms") continue;
        // Detail: story 0's two rooms (x 4..14, z 4..12) at 4 floor points a metre. The tile layer is the reference's own map
        // read bilinearly inside a room; the cascades are read at their probes.
        std::printf("   -- detail, the floor of the two rooms at 4 points a metre (%d samples a point) --\n", spp);
        const std::vector<float> fine = FineFloor(m, laws, 4, 14, 4, 12, spp, 4, albedo), fine1 = FineFloor(m, laws, 4, 14, 4, 12, spp, 1, albedo);
        Tally tile, tileNear, exact, later, edgeTile, edgeLater;
        for (int cz = 0; cz < 32; cz++)
            for (int cx = 0; cx < 40; cx++)
                for (int l = 0; l < nL; l++) {
                    const float x = 4 + (cx + 0.5f) * 0.25f, z = 4 + (cz + 0.5f) * 0.25f, want = fine[(static_cast<size_t>(cz) * 40 + cx) * nL + l];
                    const int room = PathRef::RoomOf(0, static_cast<int>(x), static_cast<int>(z));
                    const std::vector<float>& mp = ref.map[static_cast<size_t>(l) * nS];
                    float bil = 0, wsum = 0; // the tile layer: bilinear over the tile centres of this room
                    const float ux = x - 0.5f, uz = z - 0.5f;
                    for (int k = 0; k < 4; k++) {
                        const int qx = static_cast<int>(std::floor(ux)) + (k & 1), qz = static_cast<int>(std::floor(uz)) + (k >> 1);
                        if (PathRef::RoomOf(0, qx, qz) != room) continue;
                        const float w = ((k & 1) ? ux - std::floor(ux) : 1 - (ux - std::floor(ux))) * ((k >> 1) ? uz - std::floor(uz) : 1 - (uz - std::floor(uz)));
                        bil += w * mp[PathRef::Idx(ref, 0, qz, qx)], wsum += w;
                    }
                    bil /= std::max(wsum, 1e-6f);
                    const float near1 = mp[PathRef::Idx(ref, 0, static_cast<int>(z), static_cast<int>(x))];
                    float v0[kOut], v2[kOut];
                    At(kept[0][static_cast<size_t>(l) * nS], x, z, v0), At(kept[1][static_cast<size_t>(l) * nS], x, z, v2);
                    const float lat = fine1[(static_cast<size_t>(cz) * 40 + cx) * nL + l] + v2[4]; // (a first bounce at this density + the cascades)
                    tile.Add(bil, want, true), tileNear.Add(near1, want, true), exact.Add(v0[4], want, true), later.Add(lat, want, true);
                    const float fx = x - std::floor(x), fz = z - std::floor(z);
                    const bool edge = (static_cast<int>(x) == 4 && fx < 0.5f) || (static_cast<int>(x) == 13 && fx > 0.5f) || (static_cast<int>(z) == 4 && fz < 0.5f) || (static_cast<int>(z) == 11 && fz > 0.5f) ||
                                      (static_cast<int>(x) == 8 && fx > 0.5f) || (static_cast<int>(x) == 9 && fx < 0.5f);
                    if (edge) edgeTile.Add(bil, want, true), edgeLater.Add(lat, want, true);
                }
        tileNear.Print("the reference's tile texel, unfiltered");
        tile.Print("the reference's tile texels, bilinear in the room");
        exact.Print("cascades, all bounces, exact start");
        later.Print("a first bounce at 4 a metre + cascades for bounces 2+");
        edgeTile.Print("within 0.5 m of a wall: tile texels, bilinear");
        edgeLater.Print("within 0.5 m of a wall: first bounce + cascades");
    }
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

} // namespace CascadesRef
