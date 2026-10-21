// Room ambient through arches (features/arch_ambient.h): the part of a neighbour's ambient a texel near an arch takes.
// Read-only; prints the checks and exits 1 on a failure.
#include "features/arch_ambient.h"
#include <cstdio>

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

    std::printf("Arch ambient: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
