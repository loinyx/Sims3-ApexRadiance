#pragma once
// Dragged lot lamps on the terrain light maps, drawn by the GPU (part of Night Lighting; see terrain_stamps.cpp).
//
// While a lamp of the active lot (as Real-time outdoor lamps lists them) is moved in Build mode, the game's terrain bake gives it no
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
    float cone[3][4] = {{0, 0, 0, 1}, {-1, 0, 0, 0}, {}}; // its cone rows as the bake takes them (TerrainBakeCone::Rows; these: none)
};
// Every frame from RtOutdoorLamps: the active lot's lamps its room-0 lists hold (their lights)
void Update(const std::vector<uintptr_t>& lights);
// The lamps StampPS adds to the chunk centred at `key` (x, z) whose game map was re-rendered `rver` times, and their signature
// (0 = none)
uint64_t ForChunk(const std::pair<int, int>& key, uint64_t rver, std::vector<Lamp>& out);
// This light is drawn here (Active) and moved within ms (the walls' shade on the ground leaves it out of its wall test)
bool MovedWithin(uintptr_t light, unsigned long ms);
void Clear(); // forget every lamp (a new world: its bake takes them all again)
std::string Status();
// Developer capture (GroundProfile): the lights of the last Update, and one lamp's state ("" when it has none)
const std::vector<uintptr_t>& LastLights();
std::string LampState(uintptr_t light);
} // namespace TerrainStamps

// Next to the bake stubs (patches/night_terrain_relight_patch.cpp)
namespace NightTerrainBake {
constexpr int kMaxExcluded = 8;
// The lights the bake gives no colour (render thread; at most kMaxExcluded)
void SetExcluded(const std::vector<uintptr_t>& lights);
// The bake has given this lot light colour at least once (false: it is in no chunk's map, e.g. new from the catalog)
bool EverBaked(uintptr_t light);
bool Excluded(uintptr_t light); // in the exclusion list now
bool ConeShader();              // the bake runs Apex's copy with the cones
// A lot lamp as the bake stamps it (colour with its gains, w, position with the low-lamp lift, cone rows), its light rect
// {minX, minZ, maxX, maxZ} and its lot's ground height; false when the bake would not take it
bool StampOf(uintptr_t light, float col[4], float pos[3], float rect[4], float& ground, float cone[3][4]);
} // namespace NightTerrainBake
