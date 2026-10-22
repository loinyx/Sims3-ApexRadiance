#include "../../features/world_lamp_policy.h"
#include "../../features/night_level.h"
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
    // (2.15.1) night level: a fall with the clock stopped (Build mode) keeps the level of before; a rise and a fall with
    // the clock moving are taken
    {
        alignas(4) unsigned char mgr[0x100] = {};
        float* f0 = reinterpret_cast<float*>(mgr + 0xF0);
        float* hour = reinterpret_cast<float*>(mgr + 0xB4);
        const uintptr_t m = reinterpret_cast<uintptr_t>(mgr);
        float lv = -1.0f;
        *hour = 18.952f, *f0 = 0.0f;
        check(NightLevel::Read(m, lv) && lv == 0.0f);
        *f0 = 0.6f;
        check(NightLevel::Read(m, lv) && lv == 0.6f); // a world loaded at night: the ramp with the clock still
        *f0 = 1.0f;
        check(NightLevel::Read(m, lv) && lv == 1.0f && !NightLevel::Holding());
        *f0 = 0.0f;
        check(NightLevel::Read(m, lv) && lv == 1.0f && NightLevel::Holding()); // Build mode
        *f0 = 1.0f;
        check(NightLevel::Read(m, lv) && lv == 1.0f && !NightLevel::Holding()); // back to live mode
        *hour = 6.5f, *f0 = 0.8f;
        check(NightLevel::Read(m, lv) && lv == 0.8f && !NightLevel::Holding()); // dawn, the clock moving
    }
    check(TerrainBakeRule::AreaType(9) && TerrainBakeRule::AreaType(10) && !TerrainBakeRule::AreaType(7) && !TerrainBakeRule::AreaType(11));
    std::printf("World lamp policy: %d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
