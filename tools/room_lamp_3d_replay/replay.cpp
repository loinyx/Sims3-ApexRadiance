// Offline replay of The Sims 3 room light-map occlusion tests: 3D (furniture) and 2D (walls).
// Reconstructs TS3W.exe 0x0069FCE0 -> 0x0069D910 -> blocker vfunc+0x14
// (0x0069BEA0 quad blockers, 0x0069C1A0 disc blockers) and helpers
// 0x0069B590 / 0x0069B650 / 0x0069B4C0, and the wall test 0x0069FC40 -> 0x0069D4C0 -> 0x0069AA90 /
// 0x0069A8C0 with the per-light wall list 0x006A30B0 -> 0x0069DFF0, then compares against a RoomLampProbe dump.
// Usage: replay.exe <RoomLampProbe-*.txt>   (read-only, prints to stdout)
#include <xmmintrin.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

struct V { float x, y, z; };
static V sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
// game horizontal sum of a 4-lane product: (z + y) + x
static float dot(V a, V b) { float px = a.x * b.x, py = a.y * b.y, pz = a.z * b.z; return (pz + py) + px; }
static float rcp(float x) { return _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x))); }
static float clamp01(float x) { return std::min(std::max(x, 0.0f), 1.0f); }

// constants read from the dump's CONST lines (defaults = values seen in the game)
static float K_ZERO = 0.0f, K_TWO = 2.0f, K_NEG1 = -1.0f, K_FOUR = 4.0f, K_ONE = 1.0f,
             K_015 = 0.15f, K_00625 = 0.0625f, K_EPS = 1e-4f, K_STOP = 0.01f, K_DROP = 0.01f,
             K_HALF = 0.5f, K_PEN_C = 0.5f, K_WALL_Y = 1.0f;  // [00F9A5AC], [00FF307C], [01158A50]

struct Blocker { uint32_t vt; float f[64]; V v(int off) const { return {f[off / 4], f[off / 4 + 1], f[off / 4 + 2]}; } };
struct Record { V c; float r2; std::vector<Blocker> b; };
struct Wall { float f[64]; uint32_t u(int off) const { uint32_t x; memcpy(&x, &f[off / 4], 4); return x; } V v(int off) const { return {f[off / 4], f[off / 4 + 1], f[off / 4 + 2]}; } };
struct Room { std::vector<Record> rec; std::vector<Wall> walls; std::vector<int> idx2d; int mode = 0; };

// 0x0069B4C0: plane (A, N) vs segment B->C with direction D and invDen = 1/(D.N).
// out = X - A is written as soon as t >= 0, even if X lies beyond C (game quirk).
static bool segPlane(V A, V N, V B, V C, V D, float invDen, V* out) {
    float t = (0.0f - dot(sub(B, A), N)) * invDen;
    if (0.0f > t) return false;
    V X = {D.x * t + B.x, D.y * t + B.y, D.z * t + B.z};
    *out = sub(X, A);
    return !(0.0f > dot(sub(C, X), D));
}
static bool invDenOf(V p1, V p2, V N, V* d, float* inv) {
    *d = sub(p2, p1);
    float dn = dot(N, *d);
    float a = std::max(K_NEG1 * dn, dn);
    if (K_EPS > a) return false;  // segment parallel to the plane(s)
    float r = rcp(dn);
    *inv = (K_TWO - r * dn) * r;  // one Newton step
    return true;
}
// 0x0069B590: one plane
static bool hit1(V p1, V p2, V N, V A, V* out) {
    V d; float inv;
    if (!invDenOf(p1, p2, N, &d, &inv)) return false;
    return segPlane(A, N, p1, p2, d, inv, out);
}
// 0x0069B650: two parallel planes sharing N
static bool hit2(V p1, V p2, V N, V A1, V A2, V* o1, V* o2) {
    V d; float inv;
    if (!invDenOf(p1, p2, N, &d, &inv)) return false;
    bool h1 = segPlane(A1, N, p1, p2, d, inv, o1);
    bool h2 = segPlane(A2, N, p1, p2, d, inv, o2);
    return h1 || h2;
}
// 0x0069BEA0 (vtable 0x00FF3118): soft-edged rectangle(s). +0x70/+0x80 plane points,
// +0x90 normal, +0xA0/+0xB0 in-plane axes U,V, +0xD0 half extents (hu,hv,hu,hv),
// +0xE0/+0xF0 default "miss" offsets, +0x10 != 0 selects the two-plane form.
static float quadT(const Blocker& k, V p1, V p2) {
    V U = k.v(0xA0), Vv = k.v(0xB0);
    const float* h = &k.f[0xD0 / 4];
    auto op = [&](float c, float hh) { float a = (std::max(K_NEG1 * c, c) - hh + K_015) * K_TWO; return clamp01(K_ONE - a); };
    if (k.f[4] != K_ZERO) {
        V v1 = k.v(0xE0), v2 = k.v(0xF0);
        if (!hit2(p1, p2, k.v(0x90), k.v(0x70), k.v(0x80), &v1, &v2)) return 1.0f;
        float s0 = op(dot(U, v1), h[0]), s1 = op(dot(Vv, v1), h[1]);
        float s2 = op(dot(U, v2), h[2]), s3 = op(dot(Vv, v2), h[3]);
        return (K_ONE - s3 * s2) * (K_ONE - s1 * s0);
    }
    V v;
    if (!hit1(p1, p2, k.v(0x90), k.v(0x70), &v)) return 1.0f;
    float s0 = op(dot(U, v), h[0]), s1 = op(dot(Vv, v), h[1]);
    return K_ONE - s1 * s0;
}
// 0x0069C1A0 (vtable 0x00FF3C24): soft-edged disc(s). +0x20/+0x30 centres, +0x40 normal,
// +0x70 radius^2, +0x80/+0x90 default "miss" offsets, +0x10 != 0 selects two discs.
static float discT(const Blocker& k, V p1, V p2) {
    float R = k.f[0x70 / 4];
    auto tr = [&](V v) { return std::min(std::max(0.0f, ((dot(v, v) - R) + K_00625) * K_FOUR), K_ONE); };
    if (k.f[4] != K_ZERO) {
        V v1 = k.v(0x80), v2 = k.v(0x90);
        if (!hit2(p1, p2, k.v(0x40), k.v(0x20), k.v(0x30), &v1, &v2)) return 1.0f;
        return tr(v2) * tr(v1);
    }
    V v;
    if (!hit1(p1, p2, k.v(0x40), k.v(0x20), &v)) return 1.0f;
    return tr(v);
}
// 0x0069D910 with the cache path off (0x0069FCE0 clears it): product of blocker transmissions.
static float occlude(const Room& r, V a, V b, int* unknownVt, bool verbose = false) {
    V d = sub(b, a);
    float len2 = dot(d, d);
    V dn = {d.x / len2, d.y / len2, d.z / len2};
    float T = 1.0f;
    for (const Record& rc : r.rec) {
        V ca = sub(rc.c, a);
        float s = dot(ca, d);
        V q = sub({s * dn.x, s * dn.y, s * dn.z}, ca);
        if (!(dot(q, q) - rc.r2 < 0.0f)) continue;  // broad phase: infinite line vs sphere
        for (const Blocker& k : rc.b) {
            float t;
            if (k.vt == 0x00FF3118) t = quadT(k, a, b);
            else if (k.vt == 0x00FF3C24) t = discT(k, a, b);
            else { ++*unknownVt; t = 1.0f; }
            if (verbose && t < 1.0f) printf("      rec %d c %.3f %.3f %.3f r2 %.3f vt %08X two %d t %.4f\n", (int)(&rc - &r.rec[0]), rc.c.x, rc.c.y, rc.c.z, rc.r2, k.vt, k.f[4] != K_ZERO, t);
            T = T * t;
            if (K_STOP >= T) return T;
        }
    }
    return T;
}

// 0x0069AA90 thiscall(wall, m, a, b, float* s): ray a->b against wall segment set m (x/z plane).
// m: 0 hard (+0x40 P, +0x60 E), 1 soft (+0x70, +0x90), 2 cull (+0xA0, +0xC0). *s is written before any check.
static bool wallHit(const Wall& w, int m, V a, V b, float* s) {
    const int base = 0x40 + m * 0x30;
    V P = w.v(base), E = w.v(base + 0x20);
    V D = sub(b, a), W = sub(a, P);
    float den = E.x * D.z - E.z * D.x;
    float n1 = E.z * W.x - E.x * W.z;
    float n2 = W.z * D.x - W.x * D.z;
    if (den == 0.0f) return false;
    float r = rcp(den);
    float k = (K_TWO - r * den) * r;
    float t = k * n1, ss = k * n2;
    *s = ss;
    // [00FF30A0] negates lanes 2,3; [011D0190] adds {0,0,1,1}; movmskps = any sign bit
    bool outside = std::signbit(t) || std::signbit(ss) || std::signbit(k * -n1 + 1.0f) || std::signbit(k * -n2 + 1.0f);
    if (m != 0 && (w.u(0xEC) & 2)) {
        if (outside) return false;
        // height test: crossing of plane (P, N) with W.y replaced by [01158A50]; hit while below P.y + 1
        V N = w.v(0xD0);
        V Wy = {W.x, K_WALL_Y, W.z};
        float den2 = dot(N, D);
        float r2 = rcp(den2);
        float k2 = (K_TWO - den2 * r2) * r2;
        float q = k2 * (0.0f - dot(N, Wy));
        float h = (q * D.y + a.y) - P.y;
        return K_ONE > h;
    }
    return !outside;
}
// 0x0069A8C0: soft penumbra near the wall's ends; +0xE0 = wall length
static float wallPenumbra(const Wall& w, float s) {
    float u = fabsf(s - K_PEN_C);
    float v = (K_PEN_C - u) * w.f[0xE0 / 4] / K_HALF;
    float mm = std::signbit(v - K_ONE) ? v : K_ONE;
    return K_ONE - mm;
}
// 0x0069FC40 -> 0x0069D4C0 (skip +0xEC bit 0, out = null). Returns ok; *t = transmission as the game leaves it.
static bool wallTest(const Room& r, const std::vector<int>& list, V a, V b, float* tOut) {
    float T = 1.0f; unsigned count = 0, maxJ = 0; bool hit = false, early = false;
    for (int i : list) {
        if (i < 0 || i >= (int)r.walls.size()) continue;
        const Wall& w = r.walls[i];
        if (w.u(0xEC) & 1) continue;
        float s;
        if (!wallHit(w, r.mode ? 1 : 0, a, b, &s)) continue;
        float x = r.mode ? wallPenumbra(w, s) : 0.0f;
        hit = true;
        if (x > K_STOP) { ++count; maxJ = std::max(maxJ, w.u(0.5f > s ? 0xE4 : 0xE8)); }
        T = x > T ? T : x;
        if (K_STOP > x) { early = true; break; }
    }
    if (!early && maxJ > count) T = 0.0f;
    float t = 1.0f;
    if (hit) t *= T;
    *tOut = t;
    return !(K_DROP > t);
}
// 0x006A30B0 -> 0x0069DFF0: walls whose cull segment (m = 2) the batch-centroid -> light ray hits
static std::vector<int> wallCull(const Room& r, V C, V Lp) {
    std::vector<int> out;
    for (int i = 0; i < (int)r.walls.size(); ++i) { float s; if (wallHit(r.walls[i], 2, C, Lp, &s)) out.push_back(i); }
    return out;
}

static float lum(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
static float hexf(const char* s) { uint32_t u = (uint32_t)strtoul(s, nullptr, 16); float f; memcpy(&f, &u, 4); return f; }
struct Sample {
    int room; V p, L, from, C; float hook[3], wall[3], got[3];
    int rawOk = -1; float rawT = 1.0f;  // the game's own 0x0069FC40 answer (-1: not run)
    int batch = -1, count = 0, f0 = 1, f1 = 1;
};

struct Stat {
    int n = 0, exact = 0, within = 0, mb = 0, mbHit = 0, mo = 0, moHit = 0;
    struct W { float err, m, p; size_t i; };
    std::vector<W> worst;
    void add(float m, float p, size_t i, float exactTol = 1e-5f) {
        float e = fabsf(m - p);
        ++n; exact += e <= exactTol; within += e <= 0.02f;
        if (m < 0.98f) { ++mb; mbHit += p < 0.98f; } else { ++mo; moHit += p >= 0.98f; }
        worst.push_back({e, m, p, i});
    }
    void print(const char* title, const std::vector<struct Sample>& S, int nWorst = 8) {
        std::sort(worst.begin(), worst.end(), [](const W& a, const W& b) { return a.err > b.err; });
        printf("\n== %s ==\n", title);
        printf("samples %d  exact %d (%.3f%%)  within 0.02 %d (%.3f%%)\n", n, exact, 100.0 * exact / std::max(n, 1), within, 100.0 * within / std::max(n, 1));
        printf("measured blocked (<0.98) %d, predicted blocked %d (%.3f%%); measured open %d, predicted open %d (%.3f%%)\n",
               mb, mbHit, 100.0 * mbHit / std::max(mb, 1), mo, moHit, 100.0 * moHit / std::max(mo, 1));
        for (size_t j = 0; j < worst.size() && (int)j < nWorst && worst[j].err > 1e-5f; ++j) {
            const Sample& s = S[worst[j].i];
            printf("  %.4f  m %.5f  p %.5f | room %d p %.4f %.4f %.4f L %.4f %.4f %.4f batch %d n %d f %d %d\n", worst[j].err, worst[j].m,
                   worst[j].p, s.room, s.p.x, s.p.y, s.p.z, s.L.x, s.L.y, s.L.z, s.batch, s.count, s.f0, s.f1);
        }
    }
};

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: replay <RoomLampProbe.txt>\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    std::vector<Room> rooms; std::vector<Sample> S;
    static char line[1 << 16];
    auto F = [](const char* s) { return (float)atof(s); };
    while (fgets(line, sizeof line, f)) {
        char* tok[200]; int n = 0;
        for (char* t = strtok(line, " \r\n"); t && n < 200; t = strtok(nullptr, " \r\n")) tok[n++] = t;
        if (!n) continue;
        if (!strcmp(tok[0], "CONST") && n >= 3) {
            uint32_t a = (uint32_t)strtoul(tok[1], nullptr, 16); float v = hexf(tok[2]);
            switch (a) {
                case 0x00F98964: K_ZERO = v; break;   case 0x00F98A58: K_TWO = v; break;
                case 0x00FAA210: K_NEG1 = v; break;   case 0x00FB2F6C: K_FOUR = v; break;
                case 0x00FECE18: K_00625 = v; break;  case 0x00FF31AC: K_STOP = v; break;
                case 0x0106B4B4: K_015 = v; break;    case 0x0106D6CC: K_DROP = v; break;
                case 0x0107A538: K_ONE = v; break;    case 0x011D0240: K_EPS = v; break;
                case 0x00F9A5AC: K_HALF = v; break;   case 0x00FF307C: K_PEN_C = v; break;
                case 0x01158A50: K_WALL_Y = v; break;
            }
        } else if (!strcmp(tok[0], "ROOM")) rooms.emplace_back();
        else if (rooms.empty()) continue;
        else if (!strcmp(tok[0], "R") && n >= 10) {
            Record r; r.c = {F(tok[3]), F(tok[4]), F(tok[5])};
            r.r2 = F(tok[8]);  // first float after "r" (record +0x10)
            rooms.back().rec.push_back(r);
        } else if (!strcmp(tok[0], "B") && n >= 71 && !rooms.back().rec.empty()) {
            Blocker b; b.vt = (uint32_t)strtoul(tok[3], nullptr, 16);
            for (int i = 0; i < 64; ++i) { uint32_t u = (uint32_t)strtoul(tok[7 + i], nullptr, 16); memcpy(&b.f[i], &u, 4); }
            rooms.back().rec.back().b.push_back(b);
        } else if (!strcmp(tok[0], "WALLS") && n >= 4) {
            rooms.back().mode = atoi(tok[3]);
        } else if (!strcmp(tok[0], "W") && n >= 68) {
            Wall w;
            for (int i = 0; i < 64; ++i) { uint32_t u = (uint32_t)strtoul(tok[4 + i], nullptr, 16); memcpy(&w.f[i], &u, 4); }
            rooms.back().walls.push_back(w);
        } else if (!strcmp(tok[0], "IDX2D") && n >= 2) {
            for (int i = 2; i < n; ++i) rooms.back().idx2d.push_back(atoi(tok[i]));
        } else if (!strcmp(tok[0], "T") && n >= 26) {
            Sample s; s.room = (int)rooms.size() - 1;
            s.p = {F(tok[3]), F(tok[4]), F(tok[5])};
            s.L = {F(tok[11]), F(tok[12]), F(tok[13])};
            s.from = s.L; s.C = s.p;
            for (int i = 0; i < 3; ++i) { s.hook[i] = F(tok[15 + i]); s.wall[i] = F(tok[19 + i]); s.got[i] = F(tok[23 + i]); }
            for (int i = 26; i < n; ++i) {
                if (!strcmp(tok[i], "from") && i + 3 < n) s.from = {F(tok[i + 1]), F(tok[i + 2]), F(tok[i + 3])};
                else if (!strcmp(tok[i], "raw") && i + 2 < n && strcmp(tok[i + 1], "none")) { s.rawOk = atoi(tok[i + 1]); s.rawT = F(tok[i + 2]); }
                else if (!strcmp(tok[i], "batch") && i + 10 < n) {
                    s.batch = atoi(tok[i + 1]); s.count = atoi(tok[i + 3]); s.f0 = atoi(tok[i + 5]); s.f1 = atoi(tok[i + 6]);
                    s.C = {F(tok[i + 8]), F(tok[i + 9]), F(tok[i + 10])};
                }
            }
            S.push_back(s);
        }
    }
    fclose(f);
    printf("rooms %zu  samples %zu\n", rooms.size(), S.size());

    // ---- 2D wall test against the game's own answer (raw ok / t) ----
    Stat wallT, wallAll; int okMis = 0, okMisAll = 0, rawRuns = 0, rawNone = 0, fromMoved = 0, unk = 0;
    std::vector<int> list; int listKey = -1, listRoom = -1; V listL{0, 0, 0};
    std::vector<char> predOk(S.size(), 1); std::vector<float> predT(S.size(), 1.0f);
    for (size_t i = 0; i < S.size(); ++i) {
        const Sample& s = S[i];
        if (s.rawOk < 0) { ++rawNone; continue; }
        ++rawRuns;
        fromMoved += s.from.x != s.L.x || s.from.y != s.L.y || s.from.z != s.L.z;
        const Room& r = rooms[s.room];
        bool built = s.count >= 4 && (s.f0 || s.f1);  // 0x006A31D0 builds the per-light lists only then
        if (built && (listKey != s.batch || listRoom != s.room || listL.x != s.L.x || listL.y != s.L.y || listL.z != s.L.z)) {
            list = wallCull(r, s.C, s.L); listKey = s.batch; listRoom = s.room; listL = s.L;  // cull ray: centroid -> light (vfunc+0x24 = L)
        }
        float t; bool ok = wallTest(r, built ? list : r.idx2d, s.from, s.p, &t);
        predOk[i] = ok; predT[i] = t;
        okMis += ok != (s.rawOk != 0);
        wallT.add(s.rawT, t, i);
        float t2; bool ok2 = wallTest(r, r.idx2d, s.from, s.p, &t2);
        okMisAll += ok2 != (s.rawOk != 0);
        wallAll.add(s.rawT, t2, i);
    }
    printf("\nwall test run by the game: %d samples (raw none: %d); ray start moved by Apex (from != L): %d\n", rawRuns, rawNone, fromMoved);
    printf("ok mismatches: per-batch list %d, default list (IDX2D) for every sample %d\n", okMis, okMisAll);
    wallT.print("2D wall t: replay (per-batch culled list, IDX2D under 4 samples) vs game raw t (exact = 1e-5)", S);
    wallAll.print("2D wall t: replay with IDX2D for every sample vs game raw t", S, 4);

    // ---- 3D test, split by the pass's flags[1] ----
    Stat d3; int f1zero = 0, f1zeroOpen = 0, f1zeroWould = 0;
    std::vector<float> T3(S.size(), 1.0f);
    for (size_t i = 0; i < S.size(); ++i) {
        const Sample& s = S[i];
        float lw = lum(s.wall), lh = lum(s.hook);
        T3[i] = occlude(rooms[s.room], s.L, s.p, &unk);
        if (lw < 1e-6f || lh < 1e-6f) continue;
        float m = lum(s.got) / lw;
        if (!s.f1) { ++f1zero; f1zeroOpen += fabsf(m - 1.0f) <= 0.0001f; f1zeroWould += T3[i] < 0.98f; continue; }
        float p = (lw / lh) * T3[i] < K_DROP ? 0.0f : T3[i];
        d3.add(m, p, i, 0.0005f);
    }
    printf("\nflags[1] = 0 samples (lit): %d, measured 3D exactly 1: %d, replay would shade (<0.98): %d\n", f1zero, f1zeroOpen, f1zeroWould);
    d3.print("3D transmission got/wall where flags[1] = 1 (a = L, b = p, all records; exact = 5e-4)", S);
    if (unk) printf("unknown blocker vtables hit: %d\n", unk);

    // ---- combined: got = hook x wall_t x 3D_T, where Apex kept the game's wall answer ----
    Stat comb; int skipped = 0;
    for (size_t i = 0; i < S.size(); ++i) {
        const Sample& s = S[i];
        float lh = lum(s.hook), lw = lum(s.wall);
        if (lh < 1e-6f) continue;
        float wallShare = 1.0f;
        if (s.f0) {
            if (s.rawOk < 0) { ++skipped; continue; }  // game test not run (Apex skipped it)
            float gameShare = s.rawOk ? s.rawT : 0.0f;
            if (fabsf(lw - gameShare * lh) > 2e-5f + 2e-3f * lw) { ++skipped; continue; }  // Apex replaced the share
            wallShare = predOk[i] ? predT[i] : 0.0f;
        }
        float F3 = wallShare;
        if (F3 > 0.0f && s.f1) { float prod = F3 * T3[i]; F3 = K_DROP > prod ? 0.0f : prod; }
        comb.add(lum(s.got) / lh, F3, i, 0.0005f);
    }
    printf("\ncombined model: %d samples skipped (wall share overridden or test not run by Apex)\n", skipped);
    comb.print("combined got/hook vs predicted wall_t x 3D_T (exact = 5e-4)", S);
    return 0;
}
