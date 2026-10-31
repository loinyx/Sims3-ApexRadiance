// The extracted AllStamped (patches/night_terrain_relight_patch.cpp): which lamp list changes are a dragged lamp TerrainStamps
// draws on the GPU. Fake lights, no game process, file writes or GPU calls.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "features/lot_light_bridge.h"
using BYTE = unsigned char;
struct BakeFields {
    float pos[3];
    int type;
    uint64_t lot;
};
struct FakeLight {
    uintptr_t id;
    BakeFields f;
};
std::vector<FakeLight> g_lights;
bool ReadBakeFields(BYTE* light, BakeFields& f) {
    for (const FakeLight& L : g_lights)
        if (L.id == reinterpret_cast<uintptr_t>(light)) return f = L.f, true;
    return false;
}
uintptr_t g_bakeExcluded[8] = {};
int g_bakeExcludedCount = 0;
/* PRODUCTION_FUNCTION */
int assertions = 0, failures = 0;
void Assert(bool ok, const char* name) {
    assertions++;
    if (!ok) failures++, std::printf("FAIL: %s\n", name);
}
LotLightBridge::BakeChange Change(uintptr_t id, float x, bool added) {
    LotLightBridge::BakeChange c;
    c.lot = 7, c.type = 11, c.id = id, c.pos[0] = x, c.pos[2] = 10.0f, c.user = true;
    (added ? c.hasNew : c.hasOld) = true;
    return c;
}
int main() {
    constexpr uintptr_t kDragged = 0x4351F8C0, kOther = 0x4351F900;
    g_lights = {{kDragged, {{13.64f, 0.0f, 10.0f}, 11, 7}}, {kOther, {{40.0f, 0.0f, 10.0f}, 11, 7}}};
    g_bakeExcluded[0] = kDragged, g_bakeExcludedCount = 1;
    // the list's place 3.64 m behind the light's place now (a fast drag between two reads)
    Assert(AllStamped({Change(0, 5.0f, false), Change(kDragged, 10.0f, true)}), "the excluded light itself counts at any distance");
    Assert(!AllStamped({Change(0, 5.0f, false), Change(0, 10.0f, true)}), "an addition with no light named still needs to be within 1.5 m");
    Assert(!AllStamped({Change(0, 5.0f, false), Change(kOther, 10.0f, true)}), "another lamp of the lot and type 3.64 m away goes to the relight");
    Assert(AllStamped({Change(0, 5.0f, false), Change(kOther, 13.0f, true)}), "another light within 1.5 m of the excluded one still counts (the game may make the light again)");
    Assert(!AllStamped({Change(0, 5.0f, false)}), "a removal with no addition is no drag");
    Assert(!AllStamped({Change(0, 5.0f, false), Change(kDragged, 10.0f, true), Change(0, 30.0f, false)}), "a second removal beside the drag goes to the relight");
    g_bakeExcludedCount = 0;
    Assert(!AllStamped({Change(0, 5.0f, false), Change(kDragged, 10.0f, true)}), "nothing excluded: nothing is stamped");
    std::printf("stamped checks: %d assertions, %d failures\n", assertions, failures);
    return failures ? 1 : 0;
}
