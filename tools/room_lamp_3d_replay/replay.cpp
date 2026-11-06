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

// the tests themselves: the production header (features/room_light_tests.h)
#include "features/room_light_tests.h"
#include "features/rt_lamp_blockers.h"
#include "features/rt_lamp_blockers.h"
using namespace RoomLightTests;

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

    // ---- the same 3D test from the physical model's blocker table (features/rt_lamp_blockers.h): bit for bit ----
    RtLampGpu::BlockerTable table; std::vector<int> headers;
    for (const Room& r : rooms) headers.push_back(table.Append(r));
    size_t same = 0, shaded = 0; float worst = 0.0f;
    for (size_t i = 0; i < S.size(); ++i) {
        const float got = table.Occlude(headers[S[i].room], S[i].L, S[i].p);
        same += memcmp(&got, &T3[i], 4) == 0; shaded += T3[i] < 1.0f; worst = std::max(worst, fabsf(got - T3[i]));
    }
    printf("\n3D test from the blocker table: %zu rows (%d blockers of another vtable left out), %zu / %zu samples identical to occlude (%zu shaded), worst %.3g\n",
           table.rows.size(), table.skipped, same, S.size(), shaded, worst);

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
