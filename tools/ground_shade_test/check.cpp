// Offline checks of the lamp shade on lot ground and on the world grass around lots (features/ground_lamp_shadow.h):
// the lot build's time on a synthetic 40 m lot (about 20 walls, 8 lamps), its result against a reference copy of the header
// (run.ps1 -Reference <revision>: bit for bit), and a chunk texture made again over a changed lot's box only against the
// whole chunk made again (bit for bit, every mip). Read-only, no game. Credits: @loinyx
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <type_traits>
#include "features/ground_lamp_shadow.h"
#ifdef APEX_SHADE_REF
#define GroundShadow GroundShadowRef
#include "ground_lamp_shadow_ref.h"
#undef GroundShadow
#endif

static int g_checks = 0, g_failures = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// ---- A synthetic lot: 40 x 40 m, a house with windows and a door, garden walls, fences; 8 lamps on it, 4 past it
struct Scene {
    GroundShadow::Grid grid;
    int ox = 0, oz = 0, lotW = 0, lotH = 0;
    std::vector<GroundShadow::Lamp> lamps, others;
    std::vector<GroundShadow::Wall> walls;
    std::vector<WallOpenings::Patch> patches;
};
static WallOpenings::Patch Opening(float u0, float u1, float sill, float head) {
    WallOpenings::Patch p;
    p.u0 = u0, p.u1 = u1, p.ou0 = u0 + 0.02f, p.ou1 = u1 - 0.02f, p.sill = sill, p.open = 1, p.jamb0 = p.jamb1 = 1;
    for (float& h : p.head) h = head;
    return p;
}
// turn: the lot's rotation (radians); soft: Grid::soft
static Scene MakeScene(float turn, bool soft, float tx = 1000.0f, float tz = 2000.0f) {
    Scene s;
    const float c = std::cos(turn), sn = std::sin(turn), ty = 10.0f, L = 40.0f;
    const auto world = [&](float lx, float lz, float& x, float& z) { x = c * lx - sn * lz + tx, z = sn * lx + c * lz + tz; };
    const auto lamp = [&](float lx, float lz, float h, float k) {
        GroundShadow::Lamp l;
        world(lx, lz, l.x, l.z);
        l.y = ty + h, l.k = k;
        l.rx0 = l.x - 20.0f, l.rx1 = l.x + 20.0f, l.rz0 = l.z - 20.0f, l.rz1 = l.z + 20.0f;
        return l;
    };
    const float lampAt[8][4] = {{4, 4, 2.0f, 6}, {36, 5, 2.2f, 8}, {20, 25, 1.8f, 5}, {5, 35, 2.5f, 10}, {35, 36, 2.0f, 7}, {14, 8, 0.3f, 3}, {28, 18, 2.4f, 9}, {22, 38, 2.0f, 4}};
    for (const auto& a : lampAt) s.lamps.push_back(lamp(a[0], a[1], a[2], a[3]));
    const float otherAt[4][4] = {{-8, 10, 2.0f, 6}, {50, 20, 2.0f, 5}, {20, -10, 2.2f, 8}, {45, 48, 2.0f, 4}};
    for (const auto& a : otherAt) s.others.push_back(lamp(a[0], a[1], a[2], a[3]));
    const auto wall = [&](float ax, float az, float bx, float bz, float h, int openings = 0) {
        GroundShadow::Wall w;
        world(ax, az, w.x0, w.z0);
        world(bx, bz, w.x1, w.z1);
        w.y0 = ty, w.y1 = ty + h;
        if (openings) {
            w.p0 = static_cast<uint32_t>(s.patches.size());
            if (openings & 1) s.patches.push_back(Opening(0.3f, 0.5f, ty + 0.9f, ty + 2.1f)); // a window
            if (openings & 2) s.patches.push_back(Opening(0.6f, 0.8f, ty, ty + 2.2f));        // a door
            w.pn = static_cast<uint32_t>(s.patches.size()) - w.p0;
        }
        s.walls.push_back(w);
    };
    // the house: 12 x 10 m, each side in 2 or 3 runs in line
    wall(10, 12, 16, 12, 3, 1), wall(16, 12, 22, 12, 3, 3), wall(22, 12, 22, 17, 3), wall(22, 17, 22, 22, 3, 1), wall(22, 22, 16, 22, 3),
        wall(16, 22, 10, 22, 3, 2), wall(10, 22, 10, 17, 3), wall(10, 17, 10, 12, 3, 1), wall(16, 12, 16, 17, 3), wall(16, 17, 16, 22, 3);
    // garden walls and fences
    wall(2, 28, 8, 28, 1.2f), wall(8, 28, 14, 28, 1.2f), wall(26, 4, 26, 10, 1.6f), wall(30, 26, 38, 26, 1.0f), wall(30, 30, 30, 38, 1.0f),
        wall(4, 15, 4, 22, 2.0f), wall(32, 10, 38, 14, 1.5f), wall(12, 32, 18, 34, 1.0f), wall(24, 30, 24, 34, 2.4f), wall(0.5f, 0.5f, 6, 0.5f, 1.0f);
    // the grid: the lot grown by its lamps' reach, at most 24 m (lot_light_bridge.cpp ShadeGrid)
    s.lotW = s.lotH = static_cast<int>(L * GroundShadow::kTexelsPerMetre);
    float lx0 = 0, lz0 = 0, lx1 = L, lz1 = L;
    for (const GroundShadow::Lamp& l : s.lamps) {
        const float dx = l.x - tx, dz = l.z - tz, lx = c * dx + sn * dz, lz = -sn * dx + c * dz, R = GroundShadow::Reach(l, ty);
        lx0 = std::min(lx0, lx - R), lz0 = std::min(lz0, lz - R), lx1 = std::max(lx1, lx + R), lz1 = std::max(lz1, lz + R);
    }
    const auto texels = [](float m) { return static_cast<int>(std::ceil(std::clamp(m, 0.0f, 24.0f) * GroundShadow::kTexelsPerMetre)); };
    s.ox = texels(-lx0), s.oz = texels(-lz0);
    GroundShadow::Grid& g = s.grid;
    g.w = s.lotW + s.ox + texels(lx1 - L), g.h = s.lotH + s.oz + texels(lz1 - L);
    g.m0 = c, g.m2 = sn, g.m8 = -sn, g.m10 = c, g.ty = ty, g.soft = soft;
    const float sx = -s.ox / GroundShadow::kTexelsPerMetre, sz = -s.oz / GroundShadow::kTexelsPerMetre;
    g.tx = tx + c * sx - sn * sz, g.tz = tz + sn * sx + c * sz;
    return s;
}

// The scene's inputs as another copy of the header's types (the reference's: the same members, bit for bit)
template <class T, class S> static T As(const S& s) {
    static_assert(sizeof(T) == sizeof(S) && std::is_trivially_copyable_v<S>);
    T t;
    std::memcpy(static_cast<void*>(&t), &s, sizeof t);
    return t;
}
template <class T, class S> static std::vector<T> As(const std::vector<S>& v) {
    std::vector<T> out;
    for (const S& s : v) out.push_back(As<T>(s));
    return out;
}
template <class Job, class Grid, class Lamp, class Wall> static void Run(const Scene& s, std::vector<uint8_t>& res, std::vector<float>& den, double& ms) {
    const auto t0 = std::chrono::steady_clock::now();
    Job job;
    job.Init(As<Grid>(s.grid), As<Lamp>(s.lamps), As<Wall>(s.walls), s.patches, {}, As<Lamp>(s.others));
    job.Step([] { return true; });
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    res = job.Result();
    den = job.StampWeight();
}
// The fastest of reps builds (the least disturbed by other work on the machine)
template <class Job, class Grid, class Lamp, class Wall> static double Fastest(const Scene& s, int reps, std::vector<uint8_t>& res, std::vector<float>& den) {
    double best = 1e30;
    for (int r = 0; r < reps; r++) {
        double v;
        Run<Job, Grid, Lamp, Wall>(s, res, den, v);
        best = std::min(best, v);
    }
    return best;
}

static void LotBuildChecks(int reps) {
    for (const float turn : {0.0f, 0.5235988f})
        for (const bool soft : {true, false}) {
            const Scene s = MakeScene(turn, soft);
            std::vector<uint8_t> res;
            std::vector<float> den;
            const double ms = Fastest<GroundShadow::ShadeJob, GroundShadow::Grid, GroundShadow::Lamp, GroundShadow::Wall>(s, reps, res, den);
            int dark = 0;
            for (uint8_t v : res) dark += v < 128;
            std::printf("lot build: 40 m lot, turn %.0f deg, soft %s: grid %d x %d, %zu lamps (+%zu), %zu walls: %.2f ms (fastest of %d), %d dark texels\n",
                        turn * 57.29578f, soft ? "on" : "off", s.grid.w, s.grid.h, s.lamps.size(), s.others.size(), s.walls.size(), ms, reps, dark);
            Check(res.size() == static_cast<size_t>(s.grid.w) * s.grid.h && dark > 500, "lot build: the mask builds and the walls shade it");
#ifdef APEX_SHADE_REF
            std::vector<uint8_t> refRes;
            std::vector<float> refDen;
            const double refMs = Fastest<GroundShadowRef::ShadeJob, GroundShadowRef::Grid, GroundShadowRef::Lamp, GroundShadowRef::Wall>(s, reps, refRes, refDen);
            std::printf("lot build: reference %.2f ms -> now %.2f ms (%.2fx)\n", refMs, ms, refMs / ms);
            Check(res == refRes, "lot build: the mask equals the reference's, bit for bit");
            Check(den.size() == refDen.size() && std::memcmp(den.data(), refDen.data(), den.size() * sizeof(float)) == 0,
                  "lot build: the stamp weight equals the reference's, bit for bit");
#endif
        }
}

// ---- The chunk texture (GroundShadow::ChunkFull, ChunkUpdate)
using GroundShadow::ExtSrc;
using GroundShadow::TexRect;
// The whole chunk as lot_light_bridge.cpp BuildChunkMips made it before 2.15.3 (std::lround), for the lots in the given order
static std::vector<std::vector<uint8_t>> OldChunkMips(const std::vector<ExtSrc>& srcs, float wx0, float wz0) {
    constexpr int N = GroundShadow::kChunkTexels;
    constexpr float tpm = N / GroundShadow::kChunkSize;
    std::vector<std::vector<uint8_t>> mips;
    std::vector<float> sumW, sumMW;
    bool any = false;
    for (const ExtSrc& m : srcs) {
        const size_t n = static_cast<size_t>(m.grid.w) * m.grid.h;
        if (!m.v || m.v->v.size() != n || m.v->w.size() != n) continue;
        if (sumW.empty()) sumW.assign(static_cast<size_t>(N) * N, 0.0f), sumMW.assign(static_cast<size_t>(N) * N, 0.0f);
        const int i0 = std::max(0, static_cast<int>(std::floor((m.x0 - wx0) * tpm))), i1 = std::min(N - 1, static_cast<int>(std::ceil((m.x1 - wx0) * tpm)));
        const int j0 = std::max(0, static_cast<int>(std::floor((m.z0 - wz0) * tpm))), j1 = std::min(N - 1, static_cast<int>(std::ceil((m.z1 - wz0) * tpm)));
        for (int j = j0; j <= j1; j++)
            for (int i = i0; i <= i1; i++) {
                float v, wt;
                if (!GroundShadow::ExtShadeAt(m, wx0 + (i + 0.5f) / tpm, wz0 + (j + 0.5f) / tpm, v, wt)) continue;
                const float w = std::max(wt, 0.0f) + 1e-4f;
                const size_t at = static_cast<size_t>(j) * N + i;
                sumW[at] += w;
                sumMW[at] += w * v;
            }
    }
    if (sumW.empty()) return mips;
    std::vector<uint8_t> px(static_cast<size_t>(N) * N, 255);
    for (size_t k = 0; k < px.size(); k++)
        if (sumW[k] > 0.0f) {
            const uint8_t q = static_cast<uint8_t>(std::lround(std::clamp(sumMW[k] / sumW[k], 0.0f, 255.0f)));
            if (q < 255) px[k] = q, any = true;
        }
    if (!any) return mips;
    int n = N;
    mips.push_back(std::move(px));
    while (n > 1) {
        const std::vector<uint8_t>& a = mips.back();
        const int h = n / 2;
        std::vector<uint8_t> next(static_cast<size_t>(h) * h);
        for (int j = 0; j < h; j++)
            for (int i = 0; i < h; i++) {
                const size_t p = static_cast<size_t>(2 * j) * n + 2 * i, q = p + n;
                next[static_cast<size_t>(j) * h + i] = static_cast<uint8_t>((a[p] + a[p + 1] + a[q] + a[q + 1] + 2) / 4);
            }
        mips.push_back(std::move(next));
        n = h;
    }
    return mips;
}
// A synthetic lot's shade: a grid turned or not, mostly 255 with soft dark blobs, stamp weights that fade out
// size > 0: a square grid of that many texels, not turned, 80 m into the chunk
static ExtSrc RandomLot(std::mt19937& rng, uint64_t lot, float wx0, float wz0, int size = 0) {
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    ExtSrc s;
    s.lot = lot;
    GroundShadow::Grid& g = s.grid;
    g.w = 40 + static_cast<int>(U(rng) * 260), g.h = 40 + static_cast<int>(U(rng) * 260);
    const float turn = U(rng) < 0.6f ? 0.0f : U(rng) * 6.2831853f, c = std::cos(turn), sn = std::sin(turn);
    g.m0 = c, g.m2 = sn, g.m8 = -sn, g.m10 = c;
    g.tx = wx0 - 60.0f + U(rng) * 330.0f, g.tz = wz0 - 60.0f + U(rng) * 330.0f; // some over the chunk's edges, some past them
    if (size > 0) g.w = g.h = size, g.m0 = g.m10 = 1.0f, g.m2 = g.m8 = 0.0f, g.tx = wx0 + 80.0f, g.tz = wz0 + 80.0f;
    auto d = std::make_shared<GroundShadow::ExtData>();
    d->v.assign(static_cast<size_t>(g.w) * g.h, 255);
    d->w.assign(d->v.size(), 0.0f);
    const int blobs = 1 + static_cast<int>(U(rng) * 4);
    for (int b = 0; b < blobs; b++) {
        const float bx = U(rng) * g.w, bz = U(rng) * g.h, r = 6.0f + U(rng) * 40.0f, depth = U(rng);
        for (int j = 0; j < g.h; j++)
            for (int i = 0; i < g.w; i++) {
                const float q = std::max(0.0f, 1.0f - std::hypot(i - bx, j - bz) / r);
                uint8_t& v = d->v[static_cast<size_t>(j) * g.w + i];
                v = std::min(v, static_cast<uint8_t>(std::lround(255.0f * (1.0f - depth * q))));
            }
    }
    const float lx = U(rng) * g.w, lz = U(rng) * g.h, k = 0.2f + U(rng) * 3.0f;
    for (int j = 0; j < g.h; j++)
        for (int i = 0; i < g.w; i++) d->w[static_cast<size_t>(j) * g.w + i] = k / (1.0f + 0.01f * ((i - lx) * (i - lx) + (j - lz) * (j - lz)));
    s.v = d;
    float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
    const float W = g.w / GroundShadow::kTexelsPerMetre, H = g.h / GroundShadow::kTexelsPerMetre;
    for (const float lx2 : {0.0f, W})
        for (const float lz2 : {0.0f, H}) {
            const float x = g.m0 * lx2 + g.m8 * lz2 + g.tx, z = g.m2 * lx2 + g.m10 * lz2 + g.tz;
            x0 = std::min(x0, x), x1 = std::max(x1, x), z0 = std::min(z0, z), z1 = std::max(z1, z);
        }
    s.x0 = x0, s.z0 = z0, s.x1 = x1, s.z1 = z1;
    return s;
}
// The texture in video memory, as the render thread reads and writes it (the levels in full; none: no texture)
struct SimTexture {
    std::vector<std::vector<uint8_t>> levels;
    int shaded = 0;
};
static bool SameAsFull(const SimTexture& t, const GroundShadow::ChunkTexture& full) {
    if (t.levels.size() != full.levels.size() || (!t.levels.empty() && t.shaded != full.shaded)) return false;
    for (size_t l = 0; l < t.levels.size(); l++)
        if (t.levels[l] != full.levels[l]) return false;
    return true;
}
static void ChunkChecks() {
    using namespace GroundShadow;
    const float wx0 = 512.0f, wz0 = 768.0f; // the chunk centred on (640, 896)
    std::mt19937 rng(20251);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    // the whole chunk: the same bytes as before 2.15.3 for the lots in the same order
    {
        std::vector<ExtSrc> srcs;
        for (uint64_t lot = 1; lot <= 6; lot++) srcs.push_back(RandomLot(rng, lot * 7919, wx0, wz0));
        ChunkTexture full;
        ChunkFull(srcs, wx0, wz0, full);
        const auto old = OldChunkMips(srcs, wx0, wz0);
        bool same = old.size() == full.levels.size();
        for (size_t l = 0; same && l < old.size(); l++) same = old[l] == full.levels[l];
        Check(same && !old.empty(), "chunk: the whole chunk gives the bytes it gave before (every mip)");
    }
    // a lot's change made over its box only: the same bytes as the whole chunk made again, after every change
    std::vector<ExtSrc> lots;
    SimTexture tex;
    uint64_t next = 1;
    int updates = 0, fulls = 0, mismatches = 0, released = 0, steps = 0;
    double fullMs = 0.0, updateMs = 0.0;
    size_t fullBytes = 0, updateBytes = 0;
    for (int step = 0; step < 240 || !lots.empty(); step++) { // (then every lot goes, one a step: the texture goes with the last)
        // one to three changes before the chunk is made again (a lot added, changed, moved or gone)
        float bx0 = 1e30f, bz0 = 1e30f, bx1 = -1e30f, bz1 = -1e30f;
        const auto addBox = [&](const ExtSrc& s) { bx0 = std::min(bx0, s.x0), bz0 = std::min(bz0, s.z0), bx1 = std::max(bx1, s.x1), bz1 = std::max(bz1, s.z1); };
        const int changes = step >= 240 ? 1 : 1 + static_cast<int>(U(rng) * 3);
        for (int c = 0; c < changes; c++) {
            const float op = step >= 240 ? 0.4f : U(rng);
            if (step < 240 && lots.size() < 3 || (op < 0.3f && lots.size() < 12)) {
                lots.push_back(RandomLot(rng, (next++) * 104729 % 1000003, wx0, wz0));
                addBox(lots.back());
            } else if (op < 0.45f) {
                const size_t k = static_cast<size_t>(U(rng) * lots.size()) % lots.size();
                addBox(lots[k]);
                lots.erase(lots.begin() + static_cast<ptrdiff_t>(k));
            } else {
                const size_t k = static_cast<size_t>(U(rng) * lots.size()) % lots.size();
                addBox(lots[k]);
                const uint64_t id = lots[k].lot;
                if (op < 0.7f) { // the same grid, new shade
                    ExtSrc s = RandomLot(rng, id, wx0, wz0);
                    s.grid = lots[k].grid, s.x0 = lots[k].x0, s.z0 = lots[k].z0, s.x1 = lots[k].x1, s.z1 = lots[k].z1;
                    if (s.v->v.size() != static_cast<size_t>(s.grid.w) * s.grid.h) s.v = lots[k].v; // (another size: kept)
                    lots[k] = s;
                } else
                    lots[k] = RandomLot(rng, id, wx0, wz0);
                addBox(lots[k]);
            }
        }
        steps++;
        std::vector<ExtSrc> srcs = lots;
        std::sort(srcs.begin(), srcs.end(), [](const ExtSrc& a, const ExtSrc& b) { return a.lot < b.lot; });
        auto t0 = std::chrono::steady_clock::now();
        ChunkTexture full;
        ChunkFull(srcs, wx0, wz0, full);
        fullMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        for (const auto& l : full.levels) fullBytes += l.size();
        const TexRect r0 = ChunkRectOf(wx0, wz0, bx0, bz0, bx1, bz1);
        if (tex.levels.empty() || r0.Empty()) { // no texture yet: the whole chunk (as the render thread does)
            if (!r0.Empty() || tex.levels.empty()) tex.levels = full.levels, tex.shaded = full.shaded, fulls++;
        } else {
            std::vector<std::vector<uint8_t>> old(kChunkLevels - 1);
            for (int l = 0; l < kChunkLevels - 1; l++) { // the render thread's read of the texture around the box
                const TexRect b = BlockRect(r0, l);
                const int n = kChunkTexels >> l;
                for (int j = b.j0; j <= b.j1; j++)
                    old[l].insert(old[l].end(), tex.levels[l].begin() + static_cast<ptrdiff_t>(j) * n + b.i0, tex.levels[l].begin() + static_cast<ptrdiff_t>(j) * n + b.i1 + 1);
            }
            t0 = std::chrono::steady_clock::now();
            ChunkTexture up;
            const bool ok = ChunkUpdate(srcs, wx0, wz0, r0, old, tex.shaded, up);
            updateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            Check(ok, "chunk: the texture's texels around the box fit the update");
            updates++;
            if (up.levels.empty()) {
                tex.levels.clear(), tex.shaded = 0, released++;
            } else
                for (int l = 0; l < kChunkLevels; l++) { // the render thread's write of the changed texels
                    const TexRect& r = up.rects[l];
                    const int n = kChunkTexels >> l;
                    updateBytes += up.levels[l].size();
                    for (int j = r.j0; j <= r.j1; j++)
                        std::copy_n(&up.levels[l][static_cast<size_t>(j - r.j0) * r.W()], r.W(), &tex.levels[l][static_cast<size_t>(j) * n + r.i0]);
                }
            tex.shaded = up.levels.empty() ? 0 : up.shaded;
        }
        if (!SameAsFull(tex, full)) mismatches++;
    }
    std::printf("chunk: %d updates over a changed box, %d whole (no texture yet), %d textures gone; whole chunk %.2f ms and %zu KB a build, update "
                "%.2f ms and %zu KB\n", updates, fulls, released, fullMs / steps, fullBytes / steps / 1024, updates ? updateMs / updates : 0.0,
                updates ? updateBytes / updates / 1024 : 0);
    Check(updates > 150 && mismatches == 0, "chunk: every update over a changed box gives the whole chunk's bytes (240 changes, then every lot gone)");
    Check(released > 0 && tex.levels.empty(), "chunk: the texture goes with the last shaded texel");
    // a 40 m lot's shade made again in place (its grid grown by 24 m a side: 88 m), three other lots over the chunk
    {
        std::vector<ExtSrc> srcs;
        for (uint64_t lot = 1; lot <= 3; lot++) srcs.push_back(RandomLot(rng, lot, wx0, wz0));
        srcs.push_back(RandomLot(rng, 9, wx0, wz0, 352));
        ChunkTexture full;
        double fms = 1e30, ums = 1e30;
        size_t bytes = 0;
        for (int rep = 0; rep < 5; rep++) {
            auto t0 = std::chrono::steady_clock::now();
            ChunkFull(srcs, wx0, wz0, full);
            fms = std::min(fms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            const ExtSrc& a = srcs.back();
            const TexRect r0 = ChunkRectOf(wx0, wz0, a.x0, a.z0, a.x1, a.z1);
            std::vector<std::vector<uint8_t>> old(kChunkLevels - 1);
            for (int l = 0; l < kChunkLevels - 1; l++) {
                const TexRect b = BlockRect(r0, l);
                const int n = kChunkTexels >> l;
                for (int j = b.j0; j <= b.j1; j++)
                    old[l].insert(old[l].end(), full.levels[l].begin() + static_cast<ptrdiff_t>(j) * n + b.i0, full.levels[l].begin() + static_cast<ptrdiff_t>(j) * n + b.i1 + 1);
            }
            ChunkTexture up;
            t0 = std::chrono::steady_clock::now();
            ChunkUpdate(srcs, wx0, wz0, r0, old, full.shaded, up);
            ums = std::min(ums, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            bytes = 0;
            for (const auto& l : up.levels) bytes += l.size();
        }
        std::printf("chunk: a 40 m lot (88 m grid) made again in place: whole chunk %.2f ms, 1365 KB uploaded; its box %.2f ms, %zu KB uploaded\n", fms, ums, bytes / 1024);
    }
}

// GroundShadow::ToByte against std::lround over every float in [0, 255]
static void ToByteChecks() {
    uint32_t bad = 0;
    for (uint32_t b = 0; b <= 0x437F0000u; b++) { // 0 .. 255.0f
        float v;
        std::memcpy(&v, &b, sizeof v);
        bad += GroundShadow::ToByte(v) != static_cast<uint8_t>(std::lround(v));
    }
    Check(bad == 0, "ToByte: lround's byte for every float in [0, 255]");
}

int main(int argc, char** argv) {
    int reps = 9;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::max(1, std::atoi(argv[++i]));
    ToByteChecks();
    ChunkChecks();
    LotBuildChecks(reps);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
