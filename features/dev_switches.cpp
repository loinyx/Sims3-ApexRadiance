// Switches of the numbered development builds (see dev_switches.h). Credits: @loinyx
#include "dev_switches.h"
#include "apex_config.h"
#include "apex_log.h"
#include <toml++/toml.hpp>
#include <format>

namespace DevSwitches {

void Load() {
    if constexpr (!kCompiled) return;
    toml::table root;
    const toml::table* t = nullptr;
    if (ApexConfig::ReadRoot(root)) t = root["dev_build"].as_table();
    const auto get = [&](const char* key) { return t ? (*t)[key].value_or(true) : true; };
    memoryOwners.store(get("memory_owners"));
    releaseDebugBuffers.store(get("release_debug_buffers"));
    leftLotRelease.store(get("left_lot_release"));
    roomSolveCauses.store(get("room_solve_causes"));
    restartSettle.store(t ? (*t)["restart_settle"].value_or(false) : false); // off unless set: measured in game without effect
    LOG_INFO(std::format("[DevBuild] Room solve switches: room_solve_causes {}, restart_settle {}", roomSolveCauses.load() ? "on" : "off",
                         restartSettle.load() ? "on" : "off"));
    LOG_INFO(std::format("[DevBuild] Memory switches: memory_owners {}, release_debug_buffers {}, left_lot_release {}", memoryOwners.load() ? "on" : "off", releaseDebugBuffers.load() ? "on" : "off", leftLotRelease.load() ? "on" : "off"));
    measurements.store(get("measurements"));
    drawReadsNoWait.store(get("draw_reads_nowait"));
    occlusionRelight.store(get("occlusion_relight"));
    roomDrainBudget.store(get("room_drain_budget"));
    allocatorTrim.store(get("allocator_trim"));
    allocatorTopDown.store(get("allocator_top_down"));
    foliageVertexLamps.store(get("foliage_vertex_lamps"));
    canopyGroundShadow.store(get("canopy_ground_shadow"));
    foliageSunShadow.store(get("foliage_sun_shadow"));
    autoLightPriority.store(get("auto_light_priority"));
    lampVisibilityMemo.store(get("lamp_visibility_memo"));
    daylightLog.store(get("daylight_log"));
    indoorLampWalls.store(get("indoor_lamp_walls"));
    archAmbient.store(get("arch_ambient"));
    basisWallFilter.store(get("basis_wall_filter"));
    hardWallTops.store(get("hard_wall_tops"));
    stairHeightLight.store(get("stair_height_light"));
    steadyIndoorLight.store(get("steady_indoor_light"));
    innerNormSettle.store(get("inner_norm_settle"));
    softOpeningEdges.store(get("soft_opening_edges"));
    stairRoomFill.store(get("stair_room_fill"));
    openingBounce.store(get("opening_bounce"));
    stairFloorTone.store(get("stair_floor_tone"));
    stairStoryBlend.store(get("stair_story_blend"));
    noOpeningBand.store(get("no_opening_band"));
    stairwellBounce.store(get("stairwell_bounce"));
    borrowedLightAdapt.store(get("borrowed_light_adapt"));
    softSlabEdges.store(get("soft_slab_edges"));
    roomFill.store(get("room_fill"));
    lotBuildShare.store(get("lot_build_share"));
    const auto on = [](const std::atomic<bool>& b) { return b.load() ? "on" : "off"; };
    LOG_INFO(std::format("[DevBuild] Switches ([dev_build] in ApexRadiance.toml): measurements {}, draw_reads_nowait {}, occlusion_relight {}, "
                         "room_drain_budget {}, allocator_trim {}, allocator_top_down {}, foliage_vertex_lamps {}, canopy_ground_shadow {}, foliage_sun_shadow {}, auto_light_priority {}, lamp_visibility_memo {}, daylight_log {}, indoor_lamp_walls {}, arch_ambient {}, basis_wall_filter {}, hard_wall_tops {}, stair_height_light {}, steady_indoor_light {}, inner_norm_settle {}, soft_opening_edges {}, stair_room_fill {}, opening_bounce {}, stair_floor_tone {}, stair_story_blend {}, no_opening_band {}, stairwell_bounce {}, borrowed_light_adapt {}, soft_slab_edges {}, room_fill {}, lot_build_share {}",
                         on(measurements), on(drawReadsNoWait), on(occlusionRelight), on(roomDrainBudget), on(allocatorTrim), on(allocatorTopDown), on(foliageVertexLamps), on(canopyGroundShadow), on(foliageSunShadow), on(autoLightPriority), on(lampVisibilityMemo), on(daylightLog), on(indoorLampWalls), on(archAmbient), on(basisWallFilter), on(hardWallTops), on(stairHeightLight), on(steadyIndoorLight), on(innerNormSettle), on(softOpeningEdges), on(stairRoomFill), on(openingBounce), on(stairFloorTone), on(stairStoryBlend), on(noOpeningBand), on(stairwellBounce), on(borrowedLightAdapt), on(softSlabEdges), on(roomFill), on(lotBuildShare)));
}

} // namespace DevSwitches
