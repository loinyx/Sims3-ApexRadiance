#pragma once
// Dragged lot lamps on the terrain light maps, drawn by the GPU (part of Night Lighting; see terrain_stamps.cpp).
//
// While a lot lamp the GPU draws (Real-time outdoor lamps) is moved in Build mode, the game's terrain bake gives it no
// colour (the bake stub's exclusion list) and StampPS (shaders/lightmap_smooth_ps.hlsl) adds its ground light to the
// smoothed chunk maps with the bake's own law, before the smoothing, so grass, roads, the atlas and every atlas reader
// follow the lamp at once instead of waiting for the CPU bake of whole chunks. One local relight takes the lamp out of
// the chunks it was baked in when its drag starts, one puts it back where it stays; each chunk changes from the bake to
// the stamp (and back) in the build that first shows its re-rendered map, so it never holds the lamp twice or not at all.
//
// Render thread only.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace TerrainStamps {
struct Lamp {
    float col[4] = {}; // colour as the bake gives it (gains, cone spill, area share); w = range x intensity x 0.2
    float pos[3] = {}; // where the bake stamps it (a low lot lamp lifted to TerrainBakeRule::kLowLampLift)
    float ground = 0.0f; // the flat ground StampPS lights (the lamp's lot ground)
};
// Every frame from RtOutdoorLamps: the active lot's lamps the GPU draws (their lights)
void Update(const std::vector<uintptr_t>& lights);
// The lamps StampPS adds to the chunk centred at `key` (x, z) whose game map was re-rendered `rver` times, and their signature
// (0 = none)
uint64_t ForChunk(const std::pair<int, int>& key, uint64_t rver, std::vector<Lamp>& out);
void Clear(); // a new world or the feature off: every lamp back to the bake
std::string Status();
} // namespace TerrainStamps

// Next to the bake stubs (patches/night_terrain_relight_patch.cpp)
namespace NightTerrainBake {
constexpr int kMaxExcluded = 8;
// The lights the bake gives no colour (render thread; at most kMaxExcluded)
void SetExcluded(const std::vector<uintptr_t>& lights);
// A lot lamp as the bake stamps it (colour with its gains, w, position with the low-lamp lift), its light rect
// {minX, minZ, maxX, maxZ} and its lot's ground height; false when the bake would not take it
bool StampOf(uintptr_t light, float col[4], float pos[3], float rect[4], float& ground);
} // namespace NightTerrainBake
