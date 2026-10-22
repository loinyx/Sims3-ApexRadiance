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
    // (2.15.1) night level: a fall that comes with a change of the sun times (Build mode) keeps the level of before; the
    // ramp of a world loaded at night, a dawn with the same sun times and a held fall outlasting kSunGateHours are taken
    {
        NightLevel::SunGate g;
        const uintptr_t m = 0x1000;
        check(g.Decide(m, 0.0f, 2.0f, -2.5f, 18.9f) == 0.0f);
        check(g.Decide(m, 0.6f, 2.0f, -2.5f, 18.9f) == 0.6f && g.Decide(m, 1.0f, 2.0f, -2.5f, 18.9f) == 1.0f);
        check(g.Decide(m, 0.6f, 0.0f, 0.0f, 18.94f) == 1.0f && g.holding);  // Build mode: first step, the clock's last tick
        check(g.Decide(m, 0.0f, 0.0f, 0.0f, 18.94f) == 1.0f && g.holding);
        check(g.Decide(m, 1.0f, 2.0f, -2.5f, 18.94f) == 1.0f && !g.holding); // back to live mode
        check(g.Decide(m, 0.8f, 2.0f, -2.5f, 6.5f) == 0.8f);                  // dawn, same sun times
        check(g.Decide(m, 0.6f, 1.0f, -1.0f, 6.6f) == 0.8f && g.holding);    // a season shift: held a while
        check(g.Decide(m, 0.6f, 1.0f, -1.0f, 6.9f) == 0.6f && !g.holding);   // then taken once the clock ran on
        check(g.Decide(0x2000, 0.0f, 0.0f, 0.0f, 12.0f) == 0.0f);             // another light manager: taken
    }
    // smooth dusk: the sky's smooth night mapped onto the setter's ends (0 under 0.2, 1 over 0.8, linear between)
    check(NightLevel::SmoothLevel(0.1f) == 0.0f && NightLevel::SmoothLevel(0.2f) == 0.0f && NightLevel::SmoothLevel(0.8f) == 1.0f &&
          NightLevel::SmoothLevel(0.95f) == 1.0f && std::fabs(NightLevel::SmoothLevel(0.5f) - 0.5f) < 1e-6f);
    // the sky's sun times: Build mode's 0 / 0 keeps the season's while the clock stands still; a world being loaded (0 / 0
    // to the season's) and a 0 / 0 that lasts with the clock running are taken
    {
        NightLevel::SunTimesGate g;
        const uintptr_t sky = 0x3000, w = 0x4000;
        float r = 0.0f, s = 0.0f;
        check(!g.Apply(sky, w, false, r, s, 0.0f) && r == 0.0f);                      // a world loading: 0 / 0 first
        r = 2.0f, s = -2.5f;
        check(!g.Apply(sky, w, false, r, s, 18.9f) && r == 2.0f && s == -2.5f);       // its season's times: taken at once
        r = 0.0f, s = 0.0f;
        check(g.Apply(sky, w, false, r, s, 18.95f) && r == 2.0f && s == -2.5f);       // Build mode: kept
        r = 0.0f, s = 0.0f;
        check(g.Apply(sky, w, false, r, s, 18.95f) && r == 2.0f && s == -2.5f);
        r = 2.0f, s = -2.5f;
        check(!g.Apply(sky, w, false, r, s, 18.95f) && r == 2.0f);                    // live mode again
        r = 0.0f, s = 0.0f;
        check(g.Apply(sky, w, false, r, s, 6.0f) && r == 2.0f);                       // a season without offsets, the clock running:
        r = 0.0f, s = 0.0f;
        check(!g.Apply(sky, w, false, r, s, 6.3f) && r == 0.0f && s == 0.0f);         // taken after kSunGateHours
        r = 1.0f, s = -1.0f;
        check(!g.Apply(sky, w, false, r, s, 6.4f) && r == 1.0f);                      // any other change: taken
        r = 2.0f, s = -2.5f;
        check(!g.Apply(sky, w, false, r, s, 6.5f) && r == 2.0f);
        r = 0.0f, s = 0.0f;
        check(!g.Apply(sky, w, true, r, s, 6.5f) && r == 0.0f);             // the debug weather set: never held
        r = 2.0f, s = -2.5f;
        check(!g.Apply(sky, w, false, r, s, 6.5f));
        r = 0.0f, s = 0.0f;
        check(!g.Apply(sky, 0x5000, false, r, s, 6.5f) && r == 0.0f);       // another weather manager (travel): taken
    }
    check(TerrainBakeRule::AreaType(9) && TerrainBakeRule::AreaType(10) && !TerrainBakeRule::AreaType(7) && !TerrainBakeRule::AreaType(11));
    std::printf("World lamp policy: %d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
