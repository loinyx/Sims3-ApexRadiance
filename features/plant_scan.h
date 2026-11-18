#pragma once
// Plants of custom content for the GPU room lamps' 3D test (features/object_legs.h): the same bake as tools/plant_puffs_table
// (a model whose blockers are two crossed upright sheets, or whose name is a plant's, becomes balls of its shadow LOD), run on a
// background thread over Documents\...\Mods\**\*.package and DCCache\*.dbc when the game starts. The base game's plants are
// baked offline (plant_puffs_data.inc); a model found here that the table already has keeps the table's. Read-only.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "object_legs.h"

namespace PlantScan {

// Starts the scan once (any thread); Find answers nothing until it ends
void Start();
// The balls of a custom model's LITE instance, false when it has none or the scan has not ended
bool Find(uint64_t lite, const ObjectLegs::Puff*& first, uint16_t& count);
// The scan ended (Find answers for good from then on)
bool Done();

// The bake itself (pure: tools/plant_puffs_table/check.cpp runs it on the game's packages against the table). files: read in
// order, a later file's resource overriding an earlier one's; out: LITE instance -> balls; names: optional, LITE instance -> name
void Bake(const std::vector<std::wstring>& files, std::unordered_map<uint64_t, std::vector<ObjectLegs::Puff>>& out,
          std::unordered_map<uint64_t, std::string>* names = nullptr);

} // namespace PlantScan
