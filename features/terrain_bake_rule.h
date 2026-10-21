#pragma once
// Which lot lights the terrain light bake stamps (night_terrain_relight_patch.cpp TerrainLightTest / IsOutdoorLotLamp, and its
// mirrors LevelLightShare::InTerrainBake, RtOutdoorLamps InTerrainBake, LotLightBridge's lamp list) and how much of an area
// light reaches the ground (BakeColourStub and the same mirrors). One rule, so the ground, the floor gate and the GPU lamps'
// under-floor share agree.

namespace TerrainBakeRule {

// The lamp classes of a lot: bare bulb 3, spot 4, shaded lamp 5, tube 6, and (2.15.1) the rectangle and disc area lights 9 and
// 10. The game bakes a lot's lights into its own ground only (its room 0 maps): an area light of a park lot (100 m range, 21/10
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

} // namespace TerrainBakeRule
