// Offline replay of The Sims 3 room light-map 3D (furniture) occlusion test.
// Reconstructs TS3W.exe 0x0069FCE0 -> 0x0069D910 -> blocker vfunc+0x14
// (0x0069BEA0 quad blockers, 0x0069C1A0 disc blockers) and helpers
// 0x0069B590 / 0x0069B650 / 0x0069B4C0, then compares against a RoomLampProbe dump.
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
             K_015 = 0.15f, K_00625 = 0.0625f, K_EPS = 1e-4f, K_STOP = 0.01f, K_DROP = 0.01f;

struct Blocker { uint32_t vt; float f[64]; V v(int off) const { return {f[off / 4], f[off / 4 + 1], f[off / 4 + 2]}; } };
struct Record { V c; float r2; std::vector<Blocker> b; };
struct Room { std::vector<Record> rec; };

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

static float lum(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
static float hexf(const char* s) { uint32_t u = (uint32_t)strtoul(s, nullptr, 16); float f; memcpy(&f, &u, 4); return f; }

struct Sample { int room, group; V p, L; float hook[3], wall[3], got[3]; };

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: replay <RoomLampProbe.txt>\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    std::vector<Room> rooms; std::vector<Sample> S; std::vector<std::string> texel;
    std::string lastKey; int groups = 0;
    static char line[1 << 16];
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
            }
        } else if (!strcmp(tok[0], "ROOM")) rooms.emplace_back();
        else if (!strcmp(tok[0], "R") && n >= 10 && !rooms.empty()) {
            Record r; r.c = {(float)atof(tok[3]), (float)atof(tok[4]), (float)atof(tok[5])};
            r.r2 = (float)atof(tok[8]);  // first float after "r" (record +0x10)
            rooms.back().rec.push_back(r);
        } else if (!strcmp(tok[0], "B") && n >= 70 && !rooms.empty() && !rooms.back().rec.empty()) {
            Blocker b; b.vt = (uint32_t)strtoul(tok[3], nullptr, 16);
            for (int i = 0; i < 64; ++i) { uint32_t u = (uint32_t)strtoul(tok[7 + i], nullptr, 16); memcpy(&b.f[i], &u, 4); }
            rooms.back().rec.back().b.push_back(b);
        } else if (!strcmp(tok[0], "T") && n >= 26 && !rooms.empty()) {
            Sample s; s.room = (int)rooms.size() - 1;
            // run of T lines with the same room, light and normal = one lit surface pass
            std::string key = std::string(tok[1]) + " " + tok[7] + " " + tok[8] + " " + tok[9];
            if (S.empty() || S.back().room != s.room || key != lastKey) ++groups;
            lastKey = key; s.group = groups - 1;
            texel.push_back(std::to_string(s.room) + " " + tok[1] + " " + tok[3] + " " + tok[4] + " " + tok[5]);
            s.p = {(float)atof(tok[3]), (float)atof(tok[4]), (float)atof(tok[5])};
            s.L = {(float)atof(tok[11]), (float)atof(tok[12]), (float)atof(tok[13])};
            for (int i = 0; i < 3; ++i) { s.hook[i] = (float)atof(tok[15 + i]); s.wall[i] = (float)atof(tok[19 + i]); s.got[i] = (float)atof(tok[23 + i]); }
            S.push_back(s);
        }
    }
    fclose(f);

    // The game lights some texels twice for the same light inside one room probe. A run (surface pass)
    // that repeats any texel already lit earlier is a second pass; in this dump those passes never apply
    // the 3D test (UNVERIFIED why: flags[1] off for that batch, or a <4-sample batch without a per-light list).
    std::vector<char> second(groups, 0);
    {
        std::vector<size_t> idx(S.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return texel[a] < texel[b]; });
        for (size_t j = 1; j < idx.size(); ++j)
            if (texel[idx[j]] == texel[idx[j - 1]]) second[S[idx[j]].group] = 1;  // stable: idx[j] is the later one
    }
    int secondN = 0;
    for (const Sample& s : S) secondN += second[s.group];
    printf("rooms %zu  samples %zu  surface passes %d  samples in second passes %d\n", rooms.size(), S.size(), groups, secondN);

    for (int order = 0; order < 2; ++order) {
        int unk = 0, used = 0, within = 0, mb = 0, mbHit = 0, mo = 0, moHit = 0, noRec = 0, noRecOpen = 0;
        int sec = 0, secOpen = 0, secWithin = 0;
        struct W { float err, m, p; size_t i; };
        std::vector<W> worst;
        for (size_t i = 0; i < S.size(); ++i) {
            const Sample& s = S[i];
            float lw = lum(s.wall), lh = lum(s.hook);
            if (lw < 1e-6f || lh < 1e-6f) continue;  // dropped by the 2D test or no light
            float m = lum(s.got) / lw;
            float T = order == 0 ? occlude(rooms[s.room], s.L, s.p, &unk) : occlude(rooms[s.room], s.p, s.L, &unk);
            float p = (lw / lh) * T < K_DROP ? 0.0f : T;  // 0x0069FCE0 drops the light below 0.01
            float e = fabsf(m - p);
            if (second[s.group]) { ++sec; secOpen += fabsf(m - 1.0f) <= 0.0001f; secWithin += e <= 0.02f; continue; }
            ++used;
            if (rooms[s.room].rec.empty()) { ++noRec; noRecOpen += fabsf(m - 1.0f) <= 0.02f; }
            within += e <= 0.02f;
            if (m < 0.98f) { ++mb; mbHit += p < 0.98f; } else { ++mo; moHit += p >= 0.98f; }
            worst.push_back({e, m, p, i});
        }
        std::sort(worst.begin(), worst.end(), [](const W& a, const W& b) { return a.err > b.err; });
        printf("\n== segment a = %s ==\n", order == 0 ? "light (L), b = sample (p)" : "sample (p), b = light (L)");
        printf("first-pass samples %d  within 0.02: %d (%.3f%%)\n", used, within, 100.0 * within / std::max(used, 1));
        printf("measured blocked (<0.98) %d, predicted blocked %d (%.3f%%)\n", mb, mbHit, 100.0 * mbHit / std::max(mb, 1));
        printf("measured open (>=0.98) %d, predicted open %d (%.3f%%)\n", mo, moHit, 100.0 * moHit / std::max(mo, 1));
        printf("rooms without records: %d samples, %d measured open\n", noRec, noRecOpen);
        printf("second-pass samples %d: measured exactly open %d, replay within 0.02 anyway %d\n", sec, secOpen, secWithin);
        if (unk) printf("unknown blocker vtables hit: %d\n", unk);
        printf("worst first-pass cases (err measured predicted | room p L):\n");
        for (size_t j = 0; j < worst.size() && j < 10; ++j) {
            const Sample& s = S[worst[j].i];
            printf("  %.4f  m %.4f  p %.4f | room %d p %.4f %.4f %.4f L %.4f %.4f %.4f\n", worst[j].err, worst[j].m, worst[j].p,
                   s.room, s.p.x, s.p.y, s.p.z, s.L.x, s.L.y, s.L.z);
            if (getenv("REPLAY_VERBOSE")) occlude(rooms[s.room], order == 0 ? s.L : s.p, order == 0 ? s.p : s.L, &unk, true);
        }
    }
    return 0;
}
