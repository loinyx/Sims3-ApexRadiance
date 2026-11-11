#pragma once
// The Performance page's features (patches/performance_patches.cpp): "ResourceLookupCache" (features/resource_cache.h),
// "ResourceLookupMisses" (its "Remember missing files" extension), "FileListCache" (the GetKeyList cache, same module),
// "LotLightingMotion" and "WallShadingWhileMoving" (features/lot_lighting_motion.h), "FastTextureCompression"
// (features/fast_dxt.h), "FastCacheCompression" (features/fast_refpack.h), "SceneNodeBudget" (features/scene_budget.h)
// and "ObjectLookupIndex" (features/object_index.h). The menu draws their rows (apex_gui.cpp, PerformanceCard); these
// functions give it the one setting that is not a switch. docs/features/performance.md.
#include <string>

namespace Performance {

// TOML section names: never rename
inline constexpr const char* kResourceCacheName = "ResourceLookupCache";
inline constexpr const char* kLookupMissesName = "ResourceLookupMisses";
inline constexpr const char* kFileListName = "FileListCache";
inline constexpr const char* kLotLightingName = "LotLightingMotion";
inline constexpr const char* kWallShadingName = "WallShadingWhileMoving";
inline constexpr const char* kLotDetailRangeName = "LotDetailRange";
inline constexpr const char* kMapViewStreamingBlockerName = "MapViewStreamingBlocker";
inline constexpr const char* kLotVisibilityOverrideName = "LotVisibilityOverride";
inline constexpr const char* kFastTextureName = "FastTextureCompression";
inline constexpr const char* kFastCacheName = "FastCacheCompression";
inline constexpr const char* kFastCasName = "FastCasSort";
inline constexpr const char* kFastMemoryName = "FastMemory";
inline constexpr const char* kMemoryGuardName = "MemoryGuard";
inline constexpr const char* kWindowRepaintName = "WindowRepaint";
inline constexpr const char* kScriptMathName = "ScriptMath";
inline constexpr const char* kSceneBudgetName = "SceneNodeBudget";
inline constexpr const char* kObjectIndexName = "ObjectLookupIndex";
inline constexpr const char* kRoomLightQueueName = "RoomLightQueue";
inline constexpr int kLotLightingBudgetDefault = 3; // ms, the registered default of budgetWhileMovingMs

// "Lot lighting time while moving" (ms, 1..15): the saved setting; Set saves it and applies it at once
int LotLightingBudgetMs();
void SetLotLightingBudgetMs(int ms);

// "Quick update for lamp switches" under Faster room lighting ([patches.RoomLightQueue] quickPass, default on; not used while
// switches show all at once)
bool RoomQuickPass();
void SetRoomQuickPass(bool on);
// "Lamp edits all at once" under Faster room lighting ([patches.RoomLightQueue] switchAllAtOnce, default on; 12/10: every
// lamp edit, was switches only): the lamp's own story shows its new light when solved; the other stories' rooms (and, for a
// switch, furniture, ground and per-pixel lamps) wait for their final light and change together (AtriumHold)
bool RoomAllAtOnce();
void SetRoomAllAtOnce(bool on);

// "Use several cores" under Faster texture compression ([patches.FastTextureCompression] useSeveralCores, default on):
// the saved setting; Set saves it and applies it to the next texture
bool FastTextureSeveralCores();
void SetFastTextureSeveralCores(bool on);

// One-line states for the menu
std::string ResourceCacheStatus();
std::string LookupMissesStatus();
std::string FileListStatus();
std::string LotLightingStatus();
std::string WallShadingStatus();
std::string LotDetailRangeStatus();
int LotDetailDistance();
void SetLotDetailDistance(int value);
std::string MapViewStreamingBlockerStatus();
bool MapViewStreamingBlockerHandledByS3SS();
std::string LotVisibilityOverrideStatus();
bool LotVisibilityOverrideHandledByS3SS();
bool LotVisibilityOverrideAlreadyExternal();
std::string FastTextureStatus();
std::string FastCacheStatus();
std::string FastMemoryStatus();
std::string MemoryGuardStatus();
std::string WindowRepaintStatus();
std::string ScriptMathStatus();
std::string SceneBudgetStatus();
std::string ObjectIndexStatus();
std::string RoomLightQueueStatus();

} // namespace Performance
