#pragma once
#include <toml++/toml.hpp>
#include <algorithm>
#include <cmath>
#include "lamp_mark_filter.h"
#include "level_light_share.h"
#include "lot_light_bridge.h"
#include "tester_settings.h"
// Persistent developer preferences only. Never records running measurements or one-shot actions.
namespace FastDxt { void SaveDeveloperState(toml::table&); void LoadDeveloperState(const toml::table&); }
namespace FastRefPack { void SaveDeveloperState(toml::table&); void LoadDeveloperState(const toml::table&); }
namespace FastCas { void SaveDeveloperState(toml::table&); void LoadDeveloperState(const toml::table&); }
namespace ResourceCache { void SaveDeveloperState(toml::table&); void LoadDeveloperState(const toml::table&); }
namespace ObjectIndex { void SaveDeveloperState(toml::table&); void LoadDeveloperState(const toml::table&); }
namespace SceneBudget { void SaveDeveloperState(toml::table&); void LoadDeveloperState(const toml::table&); }
namespace LotLightingMotion { void SaveDeveloperState(toml::table&); void LoadDeveloperState(const toml::table&); }
namespace DeveloperSettings {
inline toml::table Capture() {
    toml::table result;
    { toml::table t; FastDxt::SaveDeveloperState(t); result.insert("fast_dxt", std::move(t)); }
    { toml::table t; FastRefPack::SaveDeveloperState(t); result.insert("fast_refpack", std::move(t)); }
    { toml::table t; FastCas::SaveDeveloperState(t); result.insert("fast_cas", std::move(t)); }
    { toml::table t; ResourceCache::SaveDeveloperState(t); result.insert("resource_cache", std::move(t)); }
    { toml::table t; ObjectIndex::SaveDeveloperState(t); result.insert("object_index", std::move(t)); }
    { toml::table t; SceneBudget::SaveDeveloperState(t); result.insert("scene_budget", std::move(t)); }
    { toml::table t; LotLightingMotion::SaveDeveloperState(t); result.insert("lot_lighting_motion", std::move(t)); }
    { toml::table t; TesterToggles::SaveDeveloperState(t); result.insert("tester", std::move(t)); }
    result.insert("keep_room_light", LampMarkFilter::Enabled());
    result.insert("false_color", LotLightBridge::FalseColor());
    return result;
}
inline void Apply(const toml::table& t) {
    if (auto v = t["keep_room_light"].value<bool>()) LampMarkFilter::SetEnabled(*v);
    // (story_samples is not restored: recording the F8 story samples turns the lamp visibility memos off, and a saved switch kept
    // fences and outdoor objects several times slower for every later session; it starts off, as the frame profiler)
    if (auto v = t["false_color"].value<bool>()) LotLightBridge::SetFalseColor(*v);
    if (const auto* p = t["fast_dxt"].as_table()) FastDxt::LoadDeveloperState(*p);
    if (const auto* p = t["fast_refpack"].as_table()) FastRefPack::LoadDeveloperState(*p);
    if (const auto* p = t["fast_cas"].as_table()) FastCas::LoadDeveloperState(*p);
    if (const auto* p = t["resource_cache"].as_table()) ResourceCache::LoadDeveloperState(*p);
    if (const auto* p = t["object_index"].as_table()) ObjectIndex::LoadDeveloperState(*p);
    if (const auto* p = t["scene_budget"].as_table()) SceneBudget::LoadDeveloperState(*p);
    if (const auto* p = t["lot_lighting_motion"].as_table()) LotLightingMotion::LoadDeveloperState(*p);
    if (const auto* p = t["tester"].as_table()) TesterToggles::LoadDeveloperState(*p);
}
}
