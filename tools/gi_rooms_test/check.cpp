// Real-time GI room ids and wall-aware upsample (features/gi_rooms.h), offline self-test on synthetic stories. Prints to stdout.
#include <cmath>
#include <cstdio>
#include "features/gi_rooms.h"

static int g_fail = 0;
#define CHECK(c, ...)                                  \
    do {                                               \
        if (!(c)) {                                    \
            g_fail++;                                  \
            std::printf("FAIL %s: ", #c);              \
            std::printf(__VA_ARGS__);                  \
            std::printf("\n");                         \
        }                                              \
    } while (0)

static RtLampProxy::Edge E(float x0, float z0, float x1, float z1) {
    RtLampProxy::Edge e;
    e.x0 = x0, e.z0 = z0, e.x1 = x1, e.z1 = z1;
    return e;
}

int main() {
    // 8 x 4 tiles: room 1 at x < 4, room 2 at x >= 4, a wall along x = 4; tile (1, 1) split by a diagonal wall, its -z and +x
    // quadrants room 3; a free-standing wall inside room 2 along x = 6, z 0..2; tile (7, 3) has no floor
    RtLampProxy::Story s;
    s.w = 8, s.h = 4;
    const size_t nt = 32;
    s.floorY.assign(nt, 0.0f), s.room.assign(4 * nt, 0);
    s.floorY[3 * 8 + 7] = RtLampProxy::kNoFloor;
    for (int t = 0; t < 32; t++)
        for (int q = 0; q < 4; q++) s.room[4 * t + q] = t % 8 < 4 ? 1 : 2;
    s.room[4 * (1 * 8 + 1) + 0] = s.room[4 * (1 * 8 + 1) + 1] = 3;
    for (int z = 0; z < 4; z++) s.edges.push_back(E(4, static_cast<float>(z), 4, static_cast<float>(z + 1)));
    s.edges.push_back(E(1, 1, 2, 2));
    s.edges.push_back(E(6, 0, 6, 1)), s.edges.push_back(E(6, 1, 6, 2));

    GiRooms::Grid g;
    CHECK(GiRooms::Build(s, g), "build");
    CHECK(g.id[0] == 1 && g.id[5] == 2 && g.id[1 * 8 + 1] == 3 && g.id[3 * 8 + 7] == GiRooms::kNone, "texel rooms %d %d %d %d", g.id[0], g.id[5], g.id[9], g.id[31]);
    CHECK(g.px[9] > 1.5f && g.pz[9] < 1.5f, "split texel point on its room's side (%.3f %.3f)", g.px[9], g.pz[9]);

    // every point: taps of its own room only, weights sum to 1, no tap seen through any wall (brute force over every edge)
    int points = 0, empty = 0, bad = 0;
    for (float z = 0.013f; z < 4.0f; z += 0.05f)
        for (float x = 0.013f; x < 8.0f; x += 0.05f) {
            const int32_t room = GiRooms::RoomAt(s, x, z);
            GiRooms::Tap t[4];
            const int n = GiRooms::Weights(g, x, z, room, t);
            points++;
            if (!n) {
                empty += !(x > 7 && z > 3); // the floorless tile's own points may find room 2 nearby or not
                continue;
            }
            float sum = 0;
            for (int k = 0; k < n; k++) {
                sum += t[k].w;
                bool through = false;
                for (const auto& e : s.edges) through |= GiRooms::Crosses(x, z, g.px[t[k].tile], g.pz[t[k].tile], e);
                bad += g.id[t[k].tile] != room || through || !(t[k].w > 0);
            }
            bad += std::fabs(sum - 1.0f) > 1e-5f;
        }
    CHECK(bad == 0, "%d bad taps or sums over %d points", bad, points);
    CHECK(empty == 0, "%d points with no texel", empty);

    // a room interior far from walls: plain bilinear
    {
        GiRooms::Tap t[4];
        const int n = GiRooms::Weights(g, 2.8f, 2.7f, 1, t);
        CHECK(n == 4 && t[0].tile == 2 * 8 + 2 && std::fabs(t[0].w - 0.7f * 0.8f) < 1e-5f, "bilinear n %d w %.4f", n, t[0].w);
    }
    // next to the free-standing wall in room 2: nothing from the far side
    {
        GiRooms::Tap t[4];
        const int n = GiRooms::Weights(g, 5.9f, 1.0f, 2, t);
        bool far = false;
        for (int k = 0; k < n; k++) far |= t[k].tile % 8 >= 6;
        CHECK(n > 0 && !far, "free-standing wall leak (n %d)", n);
    }
    // the dividing wall: a layer lit only in room 1 gives exactly 0 in room 2 (leak test)
    {
        float worst = 0;
        for (float z = 0.013f; z < 4.0f; z += 0.05f)
            for (float x = 4.01f; x < 8.0f; x += 0.05f) {
                GiRooms::Tap t[4];
                const int n = GiRooms::Weights(g, x, z, GiRooms::RoomAt(s, x, z), t);
                float v = 0;
                for (int k = 0; k < n; k++) v += t[k].w * (g.id[t[k].tile] == 1 ? 1.0f : 0.0f);
                worst = std::fmax(worst, v);
            }
        CHECK(worst == 0.0f, "leak %.6f", worst);
    }
    // the minority side of the split tile reads its own texel only
    {
        GiRooms::Tap t[4];
        const int n = GiRooms::Weights(g, 1.7f, 1.2f, 3, t);
        CHECK(n == 1 && t[0].tile == 9, "split tile room 3 n %d", n);
    }
    // a story without room ids builds nothing
    {
        RtLampProxy::Story bare = s;
        bare.room.clear();
        CHECK(!GiRooms::Build(bare, g), "bare story");
    }
    std::printf("gi_rooms: %d points, %s\n", points, g_fail ? "FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
