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
    measurements.store(get("measurements"));
    drawReadsNoWait.store(get("draw_reads_nowait"));
    occlusionRelight.store(get("occlusion_relight"));
    roomDrainBudget.store(get("room_drain_budget"));
    allocatorTrim.store(get("allocator_trim"));
    allocatorTopDown.store(get("allocator_top_down"));
    foliageVertexLamps.store(get("foliage_vertex_lamps"));
    canopyGroundShadow.store(get("canopy_ground_shadow"));
    const auto on = [](const std::atomic<bool>& b) { return b.load() ? "on" : "off"; };
    LOG_INFO(std::format("[DevBuild] Switches ([dev_build] in ApexRadiance.toml): measurements {}, draw_reads_nowait {}, occlusion_relight {}, "
                         "room_drain_budget {}, allocator_trim {}, allocator_top_down {}, foliage_vertex_lamps {}, canopy_ground_shadow {}",
                         on(measurements), on(drawReadsNoWait), on(occlusionRelight), on(roomDrainBudget), on(allocatorTrim), on(allocatorTopDown), on(foliageVertexLamps), on(canopyGroundShadow)));
}

} // namespace DevSwitches
