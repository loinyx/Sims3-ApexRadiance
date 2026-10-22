#pragma once
// Which lot lights the terrain light bake stamps (night_terrain_relight_patch.cpp TerrainLightTest / IsOutdoorLotLamp, and its
// mirrors LevelLightShare::InTerrainBake, RtOutdoorLamps InTerrainBake, LotLightBridge's lamp list) and how much of an area
// light reaches the ground (BakeColourStub and the same mirrors). One rule, so the ground, the floor gate and the GPU lamps'
// under-floor share agree.
#include <atomic>

namespace TerrainBakeRule {

// The lamp classes of a lot: bare bulb 3, spot 4, shaded lamp 5, tube 6, and (2.15.1) the rectangle and disc area lights 9 and
// 10. The game bakes a lot's lights into its own ground only (its room 0 maps): an area light of a park lot (100 m range,
// F7 and F8) lit the lot's snow and stopped in a straight line at the lot border.
inline bool LotLampType(int type) { return (type >= 3 && type <= 6) || type == 9 || type == 10; }
inline bool AreaType(int type) { return type == 9 || type == 10; }

// The game's area light law (0x006BFBA0 rectangle, 0x006BFDC0 disc, both through 0x006BFAF0) lights only the side its third
// axis (+0x180) points away from: sat of the axis component of the direction from the area to the point. The bake stamps a
// light all round from its position, so an area light enters it at the share of the ground below it that it faces: 1 when it
// shines down (axis y = 1), 0.5 sideways, 0 when it shines up.
inline float AreaGroundShare(float axisY) {
    const float s = 0.5f + 0.5f * axisY;
    return s > 0.0f ? (s < 1.0f ? s : 1.0f) : 0.0f; // (a NaN axis gives 0)
}
constexpr int kAreaAxisYOffset = 0x184; // +0x180 is the third axis (x, y, z)

// (2.15.1) Low lamps. A lamp lying on the lawn (its light 5 cm over the grass, F8 capture) lit a tree above it and left
// the lawn dark: the stamp's law (2 w cos / d^2) and the room law fall with the angle of incidence, and from 5 cm the lawn 2 m
// away takes about 1 % of what it would from above. A lot lamp whose light stands less than kLowLampLift over its lot's ground
// (and not kLowLampBelow under it: a lamp in a basement) is stamped from kLowLampLift: the terrain bake (BakePosStub), its
// mirrors (LevelLightShare::UnderFloorShare, RtLampModel::BakeStamp, GroundShadow::Weight) and Instant's story-0 floor map
// (RtOutdoorLamps). Walls, ceilings, objects and trees keep its real place. Developer A/B: off = the real height everywhere.
constexpr float kLowLampLift = 0.4f, kLowLampBelow = 0.5f;
inline std::atomic<bool> g_lowLampLift{true};
inline bool LowLampLift() { return g_lowLampLift.load(std::memory_order_relaxed); }
// The height over its lot's ground (h, metres) a lot lamp's light is stamped from
// (liftable: a lot lamp of the lamp classes, lot id != 0 and LotLampType, as the bake stub lifts; street lamps never are)
inline float StampHeight(float h, bool liftable = true) {
    return liftable && h < kLowLampLift && h > -kLowLampBelow && LowLampLift() ? kLowLampLift : h;
}
inline bool Liftable(bool lot, int type) { return lot && LotLampType(type); }

} // namespace TerrainBakeRule
