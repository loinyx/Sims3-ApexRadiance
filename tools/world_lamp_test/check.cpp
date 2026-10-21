#include "../../features/world_lamp_policy.h"
#include <cmath>
#include <cstdio>
#include <initializer_list>

int main() {
    using namespace WorldLampPolicy;
    int checks = 0, failed = 0;
    auto check = [&](bool ok) { ++checks; if (!ok) ++failed; };
    for (int type = 0; type < 32; ++type) {
        check(Track(0, type) == (type == 11));
        check(Track(7, type));
    }
    for (int added = 0; added < 3; ++added)
        for (int removed = 0; removed < 3; ++removed)
            for (int edited = 0; edited < 250; ++edited)
                for (bool observed : {false, true}) {
                    check(AcceptEdit(0, added, removed, edited, observed)
                          == (added == 0 && removed == 0 && edited >= 1 && observed));
                    check(AcceptEdit(7, added, removed, edited, observed));
                }
    check(Eligible(0, 11, 0x73, 0));
    check(Eligible(0, 11, 0xF3, 0));
    check(!Eligible(7, 11, 0x73, 0));
    check(Eligible(7, 11, 0x77, 0));
    check(!Eligible(7, 11, 0x77, 1));
    for (unsigned flags = 0; flags < 256; ++flags)
        for (int type = 0; type < 32; ++type)
            for (int room = -1; room <= 1; ++room) {
                check(Eligible(0, type, flags, room) == (type == 11 && (flags & 1)));
                check(Eligible(7, type, flags, room) == ((flags & 1) && (flags & 4) && room == 0 && (type == 11 || (type >= 3 && type <= 6) || type == 9 || type == 10)));
            }
    // area lights (2.15.1): the share of the ground below they face
    check(TerrainBakeRule::AreaGroundShare(1.0f) == 1.0f && TerrainBakeRule::AreaGroundShare(0.0f) == 0.5f &&
          TerrainBakeRule::AreaGroundShare(-1.0f) == 0.0f && TerrainBakeRule::AreaGroundShare(2.0f) == 1.0f);
    check(TerrainBakeRule::AreaGroundShare(std::nanf("")) == 0.0f);
    // (2.15.1) low lamps: stamped from 40 cm over their lot's ground, not under it past 0.5 m, unchanged with the A/B off
    check(TerrainBakeRule::StampHeight(0.05f) == 0.4f && TerrainBakeRule::StampHeight(-0.2f) == 0.4f && TerrainBakeRule::StampHeight(0.4f) == 0.4f);
    check(TerrainBakeRule::StampHeight(1.8f) == 1.8f && TerrainBakeRule::StampHeight(-0.6f) == -0.6f && std::isnan(TerrainBakeRule::StampHeight(std::nanf(""))));
    TerrainBakeRule::g_lowLampLift = false;
    check(TerrainBakeRule::StampHeight(0.05f) == 0.05f);
    TerrainBakeRule::g_lowLampLift = true;
    check(TerrainBakeRule::AreaType(9) && TerrainBakeRule::AreaType(10) && !TerrainBakeRule::AreaType(7) && !TerrainBakeRule::AreaType(11));
    std::printf("World lamp policy: %d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
