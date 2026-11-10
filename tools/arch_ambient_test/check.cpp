// Room ambient through arches (features/arch_ambient.h): the part of a neighbour's ambient a texel near an arch takes.
// Read-only; prints the checks and exits 1 on a failure.
#include "features/arch_ambient.h"
#include <cmath>
#include <cstdio>
#include <utility>

namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* text) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", text);
    }
}
bool Near(float a, float b, float e = 1e-6f) { return a - b < e && b - a < e; }
} // namespace

int main() {
    using namespace ArchAmbient;
    // a capture: room 3 (green lamp) and room 4 meet at the arch on the line x = 7, z 29..31
    const float room3[3] = {0.0104f, 0.0372f, 0.0068f}, room4[3] = {0.0099f, 0.0103f, 0.0068f};
    Opening into3{7.0f, 29.06f, 7.0f, 30.94f, {room3[0], room3[1], room3[2]}};
    Opening into4{7.0f, 29.06f, 7.0f, 30.94f, {room4[0], room4[1], room4[2]}};
    const float alpha = 0.8f;

    // room 4's texel on the arch's line: all of room 3's green over its own
    float rgb[3] = {0.1f, 0.1f, 0.1f};
    Check(Add(room4, &into3, 1, 7.0f, 30.0f, alpha, rgb), "on the arch's line room 4 takes room 3's ambient");
    Check(Near(rgb[1], 0.1f + alpha * (room3[1] - room4[1])), "on the line: the whole green difference, with the texel's own ambient weight");
    Check(Near(rgb[0], 0.1f + alpha * (room3[0] - room4[0])) && Near(rgb[2], 0.1f), "per channel: only where the neighbour's is higher");
    // room 3's texel on the same line: room 4's ambient is lower in every channel but red is (barely) lower too: nothing
    float own3[3] = {0.2f, 0.2f, 0.2f};
    Check(!Add(room3, &into4, 1, 6.99f, 30.0f, alpha, own3) && Near(own3[0], 0.2f) && Near(own3[1], 0.2f), "the brighter room takes nothing: never darker");
    // both sides on the line show the higher ambient: room 4 = its own + the difference, room 3 = its own
    const float side4 = room4[1] + (room3[1] - room4[1]) * Weight(0.0f), side3 = room3[1];
    Check(Near(side4, side3), "on the line both sides meet: no step");

    // the fade away from the arch
    Check(Near(Weight(0.0f), 1.0f) && Near(Weight(kReach), 0.0f) && Weight(kReach * 2.0f) == 0.0f, "weight: 1 on the line, 0 from kReach on");
    Check(Near(Weight(kReach * 0.5f), 0.5f), "weight: half way, half");
    bool falling = true;
    for (float d = 0.0f; d < kReach; d += 0.05f) falling &= Weight(d + 0.05f) <= Weight(d);
    Check(falling, "weight falls monotonically");
    float far[3] = {0.1f, 0.1f, 0.1f};
    Check(!Add(room4, &into3, 1, 7.0f + kReach + 0.1f, 30.0f, alpha, far), "beyond kReach from the arch: nothing");
    // beside the arch, past its jamb along the wall: the distance is to the opening's end
    Check(Near(SegDist(7.5f, 32.0f, 7.0f, 29.06f, 7.0f, 30.94f), 1.1720f, 1e-3f), "distance past the jamb is to the opening's end");
    float mid[3] = {0.1f, 0.1f, 0.1f};
    Add(room4, &into3, 1, 7.75f, 30.0f, alpha, mid);
    Check(mid[1] > 0.1f && mid[1] < 0.1f + alpha * (room3[1] - room4[1]), "half a metre in: part of it");

    // no ambient weight (outdoor texels, alpha 0): nothing; the colour stays within 1
    float none[3] = {0.1f, 0.1f, 0.1f};
    Check(!Add(room4, &into3, 1, 7.0f, 30.0f, 0.0f, none), "alpha 0: nothing");
    float top[3] = {0.999f, 0.999f, 0.999f};
    Add(room4, &into3, 1, 7.0f, 30.0f, alpha, top);
    Check(top[1] <= 1.0f, "clamped to 1");
    // two arches into rooms of different colours: the larger share per channel
    Opening red{7.0f, 26.0f, 7.0f, 27.5f, {0.05f, 0.0f, 0.0f}};
    const Opening both[2] = {into3, red};
    float two[3] = {0.0f, 0.0f, 0.0f};
    Add(room4, both, 2, 7.0f, 28.3f, 1.0f, two);
    Check(two[0] > 0.0f && two[1] > 0.0f, "two arches: each channel takes its own neighbour's share");

    // the drawn ambient against a room's (a recording of a story change): a little apart is the same room, another room is not
    const float r3[3] = {0.0104f, 0.0372f, 0.0068f}, r8[3] = {0.0304f, 0.0076f, 0.0068f}, unlit[3] = {0.0193f, 0.0193f, 0.0193f};
    const float r3b[3] = {0.0104f * 1.01f, 0.0372f * 0.995f, 0.0068f + 5e-5f};
    Check(Same(r3, r3) && Same(r3, r3b), "same ambient: equal, and within a percent");
    Check(!Same(r3, r8) && !Same(r8, unlit) && !Same(r3, unlit), "same ambient: another room's is not");

    // the edges of a floor opening (opening_bounce, F7): the landing's ambient against the lit room below, the
    // opening (20..29, 30..38) with its 4 sides; the landing's floor at the edge takes the room below's, 1.5 m away nothing
    const float landing[3] = {0.0083f, 0.0081f, 0.0078f}, belowRoom[3] = {0.0162f, 0.0145f, 0.0121f};
    const Opening sides[4] = {{20, 30, 29, 30, {belowRoom[0], belowRoom[1], belowRoom[2]}}, {20, 38, 29, 38, {belowRoom[0], belowRoom[1], belowRoom[2]}},
                              {20, 30, 20, 38, {belowRoom[0], belowRoom[1], belowRoom[2]}}, {29, 30, 29, 38, {belowRoom[0], belowRoom[1], belowRoom[2]}}};
    float edge[3] = {0, 0, 0}, away[3] = {0, 0, 0};
    Add(landing, sides, 4, 19.95f, 34.0f, 0.8f, edge);
    Add(landing, sides, 4, 18.0f, 34.0f, 0.8f, away);
    Check(Near(edge[0], 0.8f * (belowRoom[0] - landing[0]), 2e-4f) && Near(edge[1], 0.8f * (belowRoom[1] - landing[1]), 2e-4f),
          "floor opening: at its edge the landing shows the room below's ambient");
    Check(away[0] == 0.0f && away[1] == 0.0f, "floor opening: 2 m from its edge nothing");

    // the rooms a floor draw takes its arches from (Select): a room of another story or lot never gives its arches
    {
        const Candidate story1{0x1000, 0xA0}, story1b{0x1000, 0xA0}, story2{0x2000, 0xA0}, otherLot{0x3000, 0xB0};
        const Candidate offStory[1] = {story2};
        Check(Select(offStory, 1, 0x1000) == Pick::OffStory && !Takes(Pick::OffStory, story2, 0x1000), "off-story match: no rows");
        const Candidate same[2] = {story1, story1b};
        Check(Select(same, 2, 0x1000) == Pick::Story && Takes(Pick::Story, story1, 0x1000) && Takes(Pick::Story, story1b, 0x1000),
              "same-story matches: all of them, as before");
        const Candidate mixed[2] = {story1, story2};
        const Pick pm = Select(mixed, 2, 0x1000);
        Check(pm == Pick::Story && Takes(pm, story1, 0x1000) && !Takes(pm, story2, 0x1000), "story known: only its own rooms");
        Check(Select(same, 2, 0) == Pick::Fallback && Takes(Pick::Fallback, story1, 0), "no story known, one story matches: its rooms together");
        Check(Select(mixed, 2, 0) == Pick::Spread && !Takes(Pick::Spread, story1, 0), "no story known, two stories match: none");
        const Candidate lots[2] = {story1, otherLot};
        Check(Select(lots, 2, 0) == Pick::Spread, "no story known, two lots match: none");
        Check(Select(nullptr, 0, 0x1000) == Pick::None, "no match: none");
    }

    // A confirmed map owner can filter records before their live ambient read.
    for (unsigned pattern = 0; pattern < 4096; ++pattern) {
        Candidate all[6], filtered[6];
        int n = 0;
        for (unsigned i = 0; i < 6; ++i) {
            const unsigned kind = (pattern >> (2 * i)) & 3;
            all[i] = Candidate{0x1000u + kind * 0x100u, 0xA0u + kind};
            if (all[i].mgr == 0x1100u) filtered[n++] = all[i];
        }
        const Pick oldPick = Select(all, 6, 0x1100u), newPick = Select(filtered, n, 0x1100u);
        int selected = 0;
        bool equal = true;
        for (const Candidate& candidate : all)
            if (Takes(oldPick, candidate, 0x1100u)) {
                equal &= selected < n && Takes(newPick, filtered[selected], 0x1100u) &&
                         filtered[selected].mgr == candidate.mgr && filtered[selected].tracker == candidate.tracker;
                ++selected;
            }
        Check(equal && selected == n, "map-owner filtering preserves selected arch records");
    }
    // (S6) ReadOnce: 15 rooms with arches, 21 floor draws a frame each comparing every room twice (no match), 3 frames; room 4
    // unreadable, room 7's ambient written by the game in the middle of frame 1
    {
        float live[15][3];
        for (int r = 0; r < 15; ++r) live[r][0] = 0.01f * static_cast<float>(r), live[r][1] = 0.02f, live[r][2] = 0.03f;
        const float before7 = live[7][0];
        FrameRead memo[15];
        int reads = 0;
        bool same = true, failed = true, late = true;
        for (std::uint32_t frame = 0; frame < 3; ++frame) {
            float seen7 = -1.0f;
            for (int draw = 0; draw < 21; ++draw) {
                if (frame == 1 && draw == 10) live[7][0] = 0.5f;
                for (int pass = 0; pass < 2; ++pass)
                    for (int r = 0; r < 15; ++r) {
                        float a[3] = {-1.0f, -1.0f, -1.0f};
                        const bool ok = ReadOnce(memo[r], frame, [&](float* to) {
                            ++reads;
                            if (r == 4) return false;
                            to[0] = live[r][0], to[1] = live[r][1], to[2] = live[r][2];
                            return true;
                        }, a);
                        if (r == 4) failed &= !ok && a[0] == -1.0f;
                        else if (r == 7) {
                            if (seen7 < 0.0f) seen7 = a[0];
                            late &= ok && a[0] == seen7; // (one value through the frame)
                        } else same &= ok && a[0] == live[r][0] && a[1] == live[r][1] && a[2] == live[r][2];
                    }
            }
            if (frame < 2) late &= seen7 == before7;
            else late &= seen7 == 0.5f;
        }
        Check(reads == 3 * 15, "read once per room and frame (45 reads, not 3 x 21 x 2 x 15 = 1890)");
        Check(same, "every draw of a frame gets the room's value");
        Check(failed, "an unreadable room stays unreadable for the frame and writes nothing");
        Check(late, "a value the game writes during a frame is taken at the next one");
    }
    // room_bounce: the opening's form factor against numeric integration, the rows, and the floor's share at the line
    {
        const BounceRow arch{14.2f, 16.0f, 16.8f, 16.0f, 0.0f, 2.65f, {0.0f, 0.0314f, 0.0f}};
        const float N[3] = {0, 1, 0};
        const auto numeric = [&](const float P[3]) {
            constexpr int K = 200;
            double s = 0.0;
            for (int i = 0; i < K; i++)
                for (int j = 0; j < K; j++) {
                    const float x = arch.ax + (arch.bx - arch.ax) * (i + 0.5f) / K, y = arch.y0 + (arch.y1 - arch.y0) * (j + 0.5f) / K;
                    const double dx = x - P[0], dy = y - P[1], dz = arch.az - P[2], r2 = dx * dx + dy * dy + dz * dz;
                    s += dy * std::fabs(dz) / (3.14159265358979 * r2 * r2);
                }
            return s * (arch.bx - arch.ax) * (arch.y1 - arch.y0) / (K * K);
        };
        bool close = true;
        for (const float d : {0.25f, 1.0f, 3.0f, 6.0f}) {
            const float P[3] = {15.5f, 0.0f, 16.0f - d};
            close = close && std::fabs(BounceForm(arch, P, N) - numeric(P)) < 0.003 * numeric(P) + 1e-4;
        }
        Check(close, "bounce: the contour form factor is the integral's");
        float rows[2 * kBounceRows][4], hue[4], at[3] = {}, farAt[3] = {}, wide[3] = {};
        Check(BounceConsts(&arch, 1, rows, hue) == 1 && Near(hue[0], 0.0f) && Near(hue[1] * rows[0][6], 0.0314f, 1e-6f), "bounce: one neighbour's weight x hue is its share");
        const BounceRow dark{1, 1, 2, 1, 0, 2, {}};
        Check(BounceConsts(&dark, 1, rows, hue) == 0, "bounce: a neighbour with no lamp share gives no row");
        BounceConsts(&arch, 1, rows, hue);
        const float line[3] = {15.5f, 0.0f, 16.0f}, in[3] = {15.5f, 0.0f, 13.0f}, past[3] = {15.5f, 0.0f, 12.0f}, Nw[3] = {0, 0, 1};
        BounceAt(rows, 1, hue, line, N, at), BounceAt(rows, 1, hue, in, N, farAt), BounceAt(rows, 1, hue, past, N, wide);
        Check(at[1] == 0.0f, "bounce: the arch's own blend owns the line on a floor");
        Check(farAt[1] > wide[1] && wide[1] > 0.0f && farAt[0] == 0.0f, "bounce: it falls with distance, in the neighbour's hue");
        float wall[3] = {};
        const float opposite[3] = {15.5f, 1.2f, 10.0f};
        BounceAt(rows, 1, hue, opposite, Nw, wall);
        Check(wall[1] > 0.0f, "bounce: the wall facing the opening takes it too");
    }
    // (arch_reach) The blend's length, on F7 and log numbers of 05/11: the 2.66 m arch of (18.06,10.06)-(19.94,11.94)
    {
        const float dark[3] = {0.0116f, 0.0116f, 0.0115f}, lit[3] = {0.0288f, 0.0282f, 0.0256f}, wide = 2.66f, a = 0.816f;
        const float dim[3] = {0.0119f, 0.0113f, 0.0113f}, lamplessFill[3] = {0.0322f, 0.0322f, 0.0322f};
        const float near1[3] = {0.0300f, 0.0300f, 0.0300f}, near2[3] = {0.0322f, 0.0322f, 0.0322f}, grey[3] = {0.02f, 0.02f, 0.02f}, zero[3] = {};
        Check(Reach(wide, near1, near2) == kReach && Reach(wide, lit, dark) == kReach, "reach: rooms close in tone, and the brighter room, keep 1.5 m");
        Check(Reach(1.0f, dark, lit) == kReach, "reach: a narrow arch keeps 1.5 m");
        Check(Reach(20.0f, zero, grey) == kMaxReach, "reach: never past its cap");
        Check(Reach(wide, dark, lit) > 3.0f && Reach(wide, dark, lit) <= kReachPerWidth * wide, "reach: this arch's is sized by the opening");
        // the room's next arch: 2 m along it holds the blend off that line, the two arches of room 3 in the log (over 5 m apart) change nothing, a corner keeps kReach
        Check(HeldReach(3.3f, 0, 0, 2, 0, 0, 2, 2, 2) == 2.0f, "reach: a blend ends at the room's next arch");
        Check(Weight(2.0f, HeldReach(3.3f, 0, 0, 2, 0, 0, 2, 2, 2)) == 0.0f, "reach: nothing of it left on that arch's line");
        Check(HeldReach(3.3f, 18.06f, 10.06f, 19.94f, 11.94f, 14.2f, 16.0f, 16.8f, 16.0f) == 3.3f, "reach: arches further apart than it leave it alone");
        Check(HeldReach(3.3f, 0, 0, 2, 0, 2, 0, 2, 2) == kReach && HeldReach(1.5f, 0, 0, 2, 0, 0, 0.5f, 2, 0.5f) == kReach, "reach: never under kReach");
        bool grows = true;
        for (float k = 0.0121f; k < 0.2f; k += 0.001f) {
            const float x[3] = {k, k, k}, y[3] = {k + 0.001f, k + 0.001f, k + 0.001f};
            grows &= Reach(9.0f, dark, y) >= Reach(9.0f, dark, x);
        }
        Check(grows, "reach: grows with the difference, no jump backwards");
        // the floor's ambient at d metres into the dark room, at both sets of numbers
        for (const auto& [own, nb] : {std::pair<const float*, const float*>{dark, lit}, {dim, lamplessFill}}) {
            const float L = Reach(wide, own, nb), D = nb[0] - own[0];
            const auto floorAt = [&, own = own](float d, float r) { return a * (own[0] + D * Weight(d, r)); };
            const auto steepest = [&](float r, float dx, bool ratio) {
                float m = 0.0f;
                for (float d = 0.0f; d < 5.0f; d += 0.01f) {
                    const float p = floorAt(d, r), q = floorAt(d + dx, r);
                    m = std::max(m, ratio ? std::log(p / q) : p - q);
                }
                return m;
            };
            // the extra light over a 5 x 5 m room, the arch on one side
            const auto lift = [&](float r) {
                double s = 0.0;
                for (int i = 0; i < 100; i++)
                    for (int j = 0; j < 100; j++) s += floorAt(SegDist(i * 0.05f + 0.025f, j * 0.05f + 0.025f, 1.17f, 0.0f, 3.83f, 0.0f), r) - floorAt(99.0f, r);
                return static_cast<float>(s / 1e4);
            };
            const float base = a * own[0];
            std::printf("  reach %.2f m, own %.4f, neighbour %.4f: steepest fall per 25 cm %.1f%% -> %.1f%%, per 6.25 cm texel %.2f%% -> %.2f%%, of the ambient %.5f -> %.5f per 25 cm, "
                        "ln a metre %.2f -> %.2f; mean lift of a 5 x 5 m room %.5f -> %.5f (%.0f%% -> %.0f%% of its own)\n",
                        L, own[0], nb[0], 100.0f * (std::exp(steepest(kReach, 0.25f, true)) - 1.0f), 100.0f * (std::exp(steepest(L, 0.25f, true)) - 1.0f),
                        100.0f * (std::exp(steepest(kReach, 0.0625f, true)) - 1.0f), 100.0f * (std::exp(steepest(L, 0.0625f, true)) - 1.0f), steepest(kReach, 0.25f, false),
                        steepest(L, 0.25f, false), steepest(kReach, 1.0f, true), steepest(L, 1.0f, true), lift(kReach), lift(L), 100.0f * lift(kReach) / base, 100.0f * lift(L) / base);
            Check(steepest(L, 0.25f, true) <= 0.5f * steepest(kReach, 0.25f, true), "reach: the steepest fall per 25 cm at least halves");
            Check(lift(L) < 0.6f * base, "reach: a 5 x 5 m room stays well under its neighbour");
            Check(floorAt(0.0f, L) == floorAt(0.0f, kReach), "reach: on the line both sides still meet");
            Check(floorAt(L, L) == base, "reach: past it the room keeps its own ambient");
        }
        // the shader's sum: never past the neighbour's, whatever the distance, with the bounce at the same reach
        BounceRow row{1.17f, 0.0f, 3.83f, 0.0f, 0.0f, 2.4f, {lit[0] - dark[0], lit[1] - dark[1], lit[2] - dark[2]}, Reach(wide, dark, lit)};
        float rows[2 * kBounceRows][4], hue[4];
        BounceConsts(&row, 1, rows, hue);
        Check(Near(rows[1][3], 1.0f / row.reach), "bounce: its row holds the blend's reach");
        const float N[3] = {0, 1, 0};
        float worst = 0.0f, before = 1e9f;
        bool falling2 = true;
        for (float d = 0.0f; d < 5.0f; d += 0.05f) {
            const float P[3] = {2.5f, 0.0f, d};
            float b[3] = {};
            BounceAt(rows, 1, hue, P, N, b);
            const float sum = (lit[0] - dark[0]) * Weight(d, row.reach) + b[0];
            worst = std::max(worst, sum / (lit[0] - dark[0]));
            falling2 &= sum <= before + 1e-5f;
            before = sum;
        }
        std::printf("  blend + bounce at its most: %.3f of the difference\n", worst);
        Check(worst <= 1.0f + 1e-4f, "reach: blend and bounce never stack past the neighbour's level");
        Check(falling2, "reach: blend and bounce fall with distance");
        Opening into{7.0f, 29.06f, 7.0f, 30.94f, {lit[0], lit[1], lit[2]}, 3.0f};
        float o[3] = {}, o2[3] = {};
        Check(Add(dark, &into, 1, 9.0f, 30.0f, 1.0f, o) && o[0] > 0.0f, "reach: the CPU law takes the opening's reach");
        into.reach = kReach;
        Check(!Add(dark, &into, 1, 9.0f, 30.0f, 1.0f, o2), "reach: and 1.5 m as before");
    }
    // the reflection near an arch: the neighbour's cube on the line, the room's own from the reach on, whichever is brighter (F7 top faces)
    {
        const float pairs[2][2] = {{0.016f, 0.405f}, {0.405f, 0.016f}};
        for (const auto& c : pairs) {
            Check(Near(CubeAt(c[0], c[1], Weight(0.0f, 3.31f)), c[1]), "cube: at d = 0 the neighbour's, so the rooms meet");
            Check(CubeAt(c[0], c[1], Weight(3.31f, 3.31f)) == c[0], "cube: from the reach on the room's own");
            bool between = true;
            for (float d = 0.0f; d < 3.31f; d += 0.05f) {
                const float v = CubeAt(c[0], c[1], Weight(d, 3.31f));
                between &= v >= std::min(c[0], c[1]) - 1e-6f && v <= std::max(c[0], c[1]) + 1e-6f;
            }
            Check(between, "cube: always between the two");
        }
    }
    // TakeCube: rooms 1 (D 1), 2 (D 3, no cube), 3 (D 2), 4 (D 0.5); cell order 1, 3, 2, 1, 3, 4
    {
        int asked = 0;
        const auto cubeOf = [&](std::uintptr_t room) -> void* {
            ++asked;
            return room == 2 ? nullptr : reinterpret_cast<void*>(room * 16);
        };
        const std::uintptr_t rooms[6] = {1, 3, 2, 1, 3, 4};
        const float sums[6] = {1.0f, 2.0f, 3.0f, 1.0f, 2.0f, 0.5f};
        float flags[8] = {};
        CubePick p;
        for (int n = 0; n < 6; n++) TakeCube(p, rooms[n], sums[n], flags, n, cubeOf);
        Check(p.room == 3 && p.tex == reinterpret_cast<void*>(48), "cube: the largest difference with a cube");
        Check(flags[0] == 0.0f && flags[1] == 1.0f && flags[2] == 0.0f && flags[3] == 0.0f && flags[4] == 1.0f && flags[5] == 0.0f, "cube: only that room's cells flagged");
        Check(asked == 3, "cube: a room below the pick is not read");
        CubePick none;
        float zero[8] = {};
        TakeCube(none, 2, 3.0f, zero, 0, cubeOf);
        Check(!none.tex && zero[0] == 0.0f, "cube: no room with one, no flag");
    }
    // AtriumProbe: proxies 10 (own, not ready), 0 (a member without one), 20 (not ready), 30 (ready, same story), 40 (ready, story above)
    {
        const auto ready = [](std::uintptr_t p) { return p == 30 || p == 40 || p == 50; };
        const std::uintptr_t members[4] = {0, 20, 30, 40};
        const int gaps[4] = {1, 1, 0, 1};
        Check(AtriumProbe(10, members, gaps, 4, ready) == 40, "atrium probe: the other story's ready member before one of its own story");
        Check(AtriumProbe(10, members, gaps, 3, ready) == 30, "atrium probe: its own story's only when no other story has one");
        Check(AtriumProbe(40, members, gaps, 4, ready) == 40, "atrium probe: a ready probe of its own is kept");
        Check(AtriumProbe(10, members, gaps, 2, ready) == 10, "atrium probe: no member ready, its own (the game's default)");
        Check(AtriumProbe(10, nullptr, nullptr, 0, ready) == 10, "atrium probe: no members, its own");
        const std::uintptr_t far[2] = {50, 40};
        const int farGaps[2] = {2, 1};
        Check(AtriumProbe(10, far, farGaps, 2, ready) == 40, "atrium probe: the nearest other story first");
    }
    std::printf("Arch ambient: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
