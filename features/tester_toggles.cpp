// Developer Tester tab switches: their list, persistence and defaults. See tester_settings.h.
#include "tester_settings.h"
#include "perf_toggles.h"
#include "penumbra.h"
#include <iterator>

namespace TesterToggles {
namespace {
const Item kItems[] = {
    {"ground_shade", &groundShade, Effect::None},
    {"rig_building_block", &rigBuildingBlock, Effect::Rigs},
    {"roofed_from_below", &roofedFromBelow, Effect::Rooms},
    {"penumbra", &Penumbra::g_on, Effect::Rooms},
    {"smooth_cut", &smoothCut, Effect::None},
    {"smooth_dither", &smoothDither, Effect::None},
    {"smooth_bspline", &smoothBSpline, Effect::None},
    {"ground_soft_edges", &groundSoftEdges, Effect::None},
    {"opening_lamp_cut", &openingLampCut, Effect::Rigs},
    {"disabled_lamp_fix", &disabledLampFix, Effect::Rigs},
    {"foliage_day_fix", &foliageDayFix, Effect::None},
    {"colonnade_fix", &colonnadeFix, Effect::Grids},
    {"classic_after_release", &classicAfterRelease, Effect::None},
    {"classic_together", &classicTogether, Effect::None},
    {"drag_defer_sends", &dragDeferSends, Effect::None},
    {"drag_repick_throttle", &dragRepickThrottle, Effect::None},
    {"drag_spread_cap", &dragSpreadCap, Effect::None},
    {"drag_terrain_idle", &dragTerrainIdle, Effect::None},
    {"drag_slow_refresh", &dragSlowRefresh, Effect::None},
    {"fast_own_relight", &fastOwnRelight, Effect::None},
    {"drag_let_go", &dragLetGo, Effect::None},
    {"own_room_at_once", &ownRoomAtOnce, Effect::None},
    {"furniture_with_room", &furnitureWithRoom, Effect::None},
    {"grid_memo", &gridMemo, Effect::None},
    {"perf_reissue_filter", &PerfToggles::ReissueFilter, Effect::None},
    {"perf_follow_bound_shaders", &PerfToggles::FollowBoundShaders, Effect::None},
    {"perf_sampled_dispatch_timing", &PerfToggles::SampledDispatchTiming, Effect::None},
    {"perf_precompile_answer_cache", &PerfToggles::PrecompileAnswerCache, Effect::None},
    {"perf_ao_last_shader_cache", &PerfToggles::AoLastShaderCache, Effect::None},
    {"perf_scene_dither_front_cache", &PerfToggles::SceneDitherFrontCache, Effect::None},
    {"perf_night_lighting_ps_facts", &PerfToggles::NightLightingPsFacts, Effect::None},
    {"perf_rig_mode_once_per_draw", &PerfToggles::RigModeOncePerDraw, Effect::None},
    {"perf_indoor_sampler_scan", &PerfToggles::IndoorSamplerScan, Effect::None},
    {"perf_solve_info_cache", &PerfToggles::SolveInfoCache, Effect::None},
    {"perf_window_channel_memo", &PerfToggles::WindowChannelMemo, Effect::None},
    {"perf_motion_paced_raise", &PerfToggles::MotionPacedRaise, Effect::None},
};
} // namespace

const Item* Items(int& count) {
    count = static_cast<int>(std::size(kItems));
    return kItems;
}

void SaveDeveloperState(toml::table& t) {
    for (const Item& i : kItems) t.insert_or_assign(i.key, i.value->load(std::memory_order_relaxed));
}

void LoadDeveloperState(const toml::table& t) {
    for (const Item& i : kItems)
        if (auto v = t[i.key].value<bool>()) i.value->store(*v, std::memory_order_relaxed);
}

void RestoreDefaults() {
    for (const Item& i : kItems) i.value->store(true, std::memory_order_relaxed);
}
} // namespace TesterToggles
