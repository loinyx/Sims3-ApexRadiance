#include "lot_lod_streaming.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "game_version.h"
#include "memory_patch.h"
#include "s3ss_detect.h"
#include "map_view.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace LotLodStreaming {
namespace {

std::mutex g_lock;
bool g_running = false;
bool g_externalOwner = false;

uintptr_t g_throttleFlag = 0;
uintptr_t g_worldManagerGlobal = 0;

bool g_throttleOriginalValid = false;
bool g_throttleWritten = false;
bool g_throttleMaintainLogged = false;
uint8_t g_throttleOriginal = 0;

uintptr_t g_worldManager = 0;
bool g_thresholdOriginalValid = false;
bool g_thresholdWritten = false;
bool g_thresholdMaintainLogged = false;
float g_thresholdOriginal = 0.0f;

// The native throttle only applies while Max Active Lots (WorldManager+0xE4) is at most the "Max Active Lot Threshold"
// global (12): LotLodScoring tests `MaxActive <= threshold` right after the flag (0x00C6C6B7). With Extended lot detail at
// 16 lots the throttle would silently stay off, so while this part runs the threshold follows Max Active Lots (never
// lowered below its own value; restored when Apex stops, if it still holds Apex's value).
uintptr_t g_activeThreshold = 0;
bool g_activeOriginalValid = false;
bool g_activeAbandoned = false;
int g_activeOriginal = 0;
int g_activeWritten = -1; // the value Apex wrote last (-1 = none)

// Separate map-view blocker state. This intentionally does not detour WorldManager::Update: Apex already documents
// that entry as a cooperation point with S3SS. Instead, the existing patch-manager pump maintains the live +0x258
// skip gate while map view is open and for a short grace period after exit.
inline constexpr uintptr_t kMapViewSkipOffset = 0x258;
inline constexpr uint64_t kMapViewGraceMs = 1000;

bool g_mapRunning = false;
bool g_mapExternalOwner = false;
uintptr_t g_mapWorldManagerGlobal = 0;
uintptr_t g_mapWorldManager = 0;
bool g_mapOriginalValid = false;
bool g_mapWritten = false;
uint8_t g_mapOriginal = 0;
uint64_t g_mapLastOpenTick = 0;

template <typename T> bool Read(uintptr_t address, T& out) {
    return address && MemPatch::ReadBytes(address, &out, sizeof(T));
}

template <typename T> std::vector<BYTE> Bytes(const T& value) {
    const BYTE* p = reinterpret_cast<const BYTE*>(&value);
    return std::vector<BYTE>(p, p + sizeof(T));
}

template <typename T> bool WriteExpected(uintptr_t address, const T& value, const T& expected) {
    const std::vector<BYTE> bytes = Bytes(value);
    const std::vector<BYTE> old = Bytes(expected);
    return MemPatch::WriteBytes(address, bytes, nullptr, &old);
}

bool S3SSOwnsStreamingSettings() {
    // A stale S3SS.toml may remain in Game\\Bin after the ASI is removed. Configuration alone does not make S3SS an
    // active owner: only defer when the official S3SS module is actually loaded in this process.
    const S3SSDetect::Info info = S3SSDetect::Scan();
    if (!info.s3ssLoaded) return false;
    return S3SSDetect::S3SSPatchBoolSettingEnabled("LotStreamingOptimizations", "streamingSettings", true);
}

bool S3SSOwnsMapViewBlocker() {
    const S3SSDetect::Info info = S3SSDetect::Scan();
    if (!info.s3ssLoaded) return false;
    return S3SSDetect::S3SSPatchBoolSettingEnabled("LotStreamingOptimizations", "mapViewBlocker", true);
}

void ClearWorldState() {
    g_worldManager = 0;
    g_thresholdOriginalValid = false;
    g_thresholdWritten = false;
    g_thresholdMaintainLogged = false;
    g_thresholdOriginal = 0.0f;
}

bool ApplyThrottle(std::string* error) {
    uint8_t current = 0xFF;
    if (!Read(g_throttleFlag, current)) {
        if (error) *error = "Could not read the native Lot LoD throttle flag";
        return false;
    }
    if (current > 1) {
        if (error) *error = std::format("Native Lot LoD throttle has an unexpected value ({})", static_cast<unsigned>(current));
        return false;
    }

    g_throttleOriginal = current;
    g_throttleOriginalValid = true;
    if (current == 1) {
        LOG_INFO("[LotLod] Native transition throttle was already on; Apex leaves it on without taking ownership of that byte");
        return true;
    }

    const uint8_t enabled = 1;
    if (!WriteExpected(g_throttleFlag, enabled, current)) {
        if (error) *error = "Could not enable the native Lot LoD transition throttle";
        g_throttleOriginalValid = false;
        return false;
    }
    g_throttleWritten = true;
    LOG_INFO(std::format("[LotLod] Native transition throttle: {} -> 1 at {:#010x}", static_cast<unsigned>(current), g_throttleFlag));
    return true;
}

bool MaintainThrottle(std::string* error) {
    uint8_t current = 0xFF;
    if (!Read(g_throttleFlag, current)) {
        if (error) *error = "Could not read the native Lot LoD throttle flag while maintaining it";
        return false;
    }
    if (current > 1) {
        if (error) *error = std::format("Native Lot LoD throttle drifted to an unexpected value ({})", static_cast<unsigned>(current));
        return false;
    }
    if (current == 1) return true;

    const uint8_t enabled = 1;
    if (!WriteExpected(g_throttleFlag, enabled, current)) {
        if (error) *error = "Could not maintain the native Lot LoD transition throttle at 1";
        return false;
    }

    // Even if the byte was already 1 when Start() first saw it, Apex becomes an owner once it has to reassert 1.
    // Keep the first observed value in g_throttleOriginal so Stop() can restore that baseline safely.
    g_throttleWritten = true;
    if (!g_throttleMaintainLogged) {
        LOG_INFO(std::format("[LotLod] Native transition throttle drifted {} -> 1; maintained at 1", static_cast<unsigned>(current)));
        g_throttleMaintainLogged = true;
    }
    return true;
}

void MaintainActiveThreshold() {
    if (!g_activeThreshold || g_activeAbandoned || !g_worldManager) return;
    int maxActive = 0, current = 0;
    if (!Read(g_worldManager + 0xE4, maxActive) || maxActive < 1 || maxActive > 128) return;
    if (!Read(g_activeThreshold, current) || current < 0 || current > 128) return;
    if (!g_activeOriginalValid) {
        g_activeOriginal = current;
        g_activeOriginalValid = true;
    }
    const int desired = std::max(g_activeOriginal, maxActive);
    if (current == desired) return;
    if (current != g_activeOriginal && current != g_activeWritten) {
        g_activeAbandoned = true;
        LOG_WARNING(std::format("[LotLod] Another owner set the Max Active Lot Threshold to {}; Apex stops following Max Active Lots", current));
        return;
    }
    if (!WriteExpected(g_activeThreshold, desired, current)) return;
    g_activeWritten = desired;
    LOG_INFO(std::format("[LotLod] Max Active Lot Threshold {} -> {} so the transition throttle covers {} detailed lots", current, desired, maxActive));
}

void RestoreActiveThresholdIfOwned() {
    if (g_activeThreshold && g_activeWritten >= 0 && g_activeOriginalValid && !g_activeAbandoned) {
        int current = 0;
        if (Read(g_activeThreshold, current) && current == g_activeWritten && WriteExpected(g_activeThreshold, g_activeOriginal, current))
            LOG_INFO(std::format("[LotLod] Max Active Lot Threshold restored to {}", g_activeOriginal));
        else
            LOG_INFO("[LotLod] Max Active Lot Threshold left as it is (changed by another owner)");
    }
    g_activeOriginalValid = false;
    g_activeAbandoned = false;
    g_activeWritten = -1;
}

bool ApplyCurrentWorld(std::string* error) {
    uintptr_t world = 0;
    if (!Read(g_worldManagerGlobal, world) || !world) return true; // no world yet: Tick() retries

    const bool newWorld = world != g_worldManager;
    if (newWorld) {
        // A different WorldManager means the previous world is gone or being replaced. Never write back through a stale
        // pointer: the new manager gets its own original value and restoration state.
        ClearWorldState();
        g_worldManager = world;
    }

    float current = 0.0f;
    if (!Read(world + 0xEC, current)) {
        if (error) *error = "Could not read WorldManager+0xEC (camera speed threshold)";
        if (newWorld) ClearWorldState();
        return false;
    }
    if (!std::isfinite(current) || current < 0.0f || current > 100.0f) {
        if (error) *error = std::format("WorldManager+0xEC is not a plausible camera speed threshold ({})", current);
        if (newWorld) ClearWorldState();
        return false;
    }

    if (newWorld) {
        // Preserve the first value Apex sees for this WorldManager. Maintenance writes below must never replace this
        // baseline, otherwise Stop() would restore a drifted value instead of the pre-Apex value.
        g_thresholdOriginal = current;
        g_thresholdOriginalValid = true;
        LOG_INFO(std::format("[LotLod] WorldManager {:#010x}: camera speed threshold is {:.3f}", world, current));
    }

    if (current == kCameraThreshold) {
        if (newWorld) LOG_INFO("[LotLod] Camera speed threshold already equals 5.0; no write needed");
        return true;
    }

    if (!WriteExpected(world + 0xEC, kCameraThreshold, current)) {
        if (error) *error = newWorld ? "Could not apply camera speed threshold 5.0" : "Could not maintain camera speed threshold at 5.0";
        if (newWorld) ClearWorldState();
        return false;
    }

    g_thresholdWritten = true;
    if (newWorld) {
        LOG_INFO(std::format("[LotLod] Camera speed threshold: {:.3f} -> {:.3f}", current, kCameraThreshold));
    } else if (!g_thresholdMaintainLogged) {
        LOG_INFO(std::format("[LotLod] Camera speed threshold drifted {:.3f} -> {:.3f}; maintained at {:.3f}", current, kCameraThreshold, kCameraThreshold));
        g_thresholdMaintainLogged = true;
    }
    return true;
}

void RestoreWorldIfOwned() {
    if (!g_thresholdWritten || !g_thresholdOriginalValid || !g_worldManager) {
        ClearWorldState();
        return;
    }

    uintptr_t liveWorld = 0;
    if (!Read(g_worldManagerGlobal, liveWorld) || liveWorld != g_worldManager) {
        LOG_INFO("[LotLod] Camera threshold restore skipped: the WorldManager changed");
        ClearWorldState();
        return;
    }

    float current = 0.0f;
    if (!Read(g_worldManager + 0xEC, current)) {
        LOG_WARNING("[LotLod] Camera threshold restore skipped: current value is unreadable");
        ClearWorldState();
        return;
    }

    if (current != kCameraThreshold) {
        LOG_INFO(std::format("[LotLod] Camera threshold restore skipped: another owner changed {:.3f} to {:.3f}", kCameraThreshold, current));
        ClearWorldState();
        return;
    }

    if (WriteExpected(g_worldManager + 0xEC, g_thresholdOriginal, current))
        LOG_INFO(std::format("[LotLod] Camera speed threshold restored to {:.3f}", g_thresholdOriginal));
    else
        LOG_WARNING("[LotLod] Camera threshold restore failed; current value was left unchanged");

    ClearWorldState();
}

void RestoreThrottleIfOwned() {
    if (!g_throttleWritten || !g_throttleOriginalValid) {
        g_throttleWritten = false;
        g_throttleOriginalValid = false;
        return;
    }

    uint8_t current = 0xFF;
    if (!Read(g_throttleFlag, current)) {
        LOG_WARNING("[LotLod] Throttle restore skipped: current value is unreadable");
    } else if (current != 1) {
        LOG_INFO(std::format("[LotLod] Throttle restore skipped: another owner changed it to {}", static_cast<unsigned>(current)));
    } else if (WriteExpected(g_throttleFlag, g_throttleOriginal, current)) {
        LOG_INFO(std::format("[LotLod] Native transition throttle restored to {}", static_cast<unsigned>(g_throttleOriginal)));
    } else {
        LOG_WARNING("[LotLod] Throttle restore failed; current value was left unchanged");
    }

    g_throttleWritten = false;
    g_throttleOriginalValid = false;
    g_throttleMaintainLogged = false;
}

void ClearMapWorldState() {
    g_mapWorldManager = 0;
    g_mapOriginalValid = false;
    g_mapWritten = false;
    g_mapOriginal = 0;
}

void RestoreMapBlockIfOwned() {
    if (!g_mapOriginalValid || !g_mapWorldManager) {
        ClearMapWorldState();
        return;
    }

    // Never restore through an old WorldManager pointer after a world change.
    uintptr_t liveWorld = 0;
    if (!Read(g_mapWorldManagerGlobal, liveWorld) || liveWorld != g_mapWorldManager) {
        if (g_mapWritten) LOG_INFO("[MapStreaming] Restore skipped: the WorldManager changed");
        ClearMapWorldState();
        return;
    }

    if (!g_mapWritten) {
        // Apex never changed this byte. Preserve whatever the game/another owner has done since.
        ClearMapWorldState();
        return;
    }

    uint8_t current = 0xFF;
    if (!Read(g_mapWorldManager + kMapViewSkipOffset, current)) {
        LOG_WARNING("[MapStreaming] Restore skipped: WorldManager+0x258 is unreadable");
    } else if (current != 1) {
        LOG_INFO(std::format("[MapStreaming] Restore skipped: another owner changed the skip gate to {}", static_cast<unsigned>(current)));
    } else if (WriteExpected(g_mapWorldManager + kMapViewSkipOffset, g_mapOriginal, current)) {
        LOG_INFO(std::format("[MapStreaming] Lot streaming resumed; skip gate restored to {}", static_cast<unsigned>(g_mapOriginal)));
    } else {
        LOG_WARNING("[MapStreaming] Restore failed; current skip-gate value was left unchanged");
    }

    ClearMapWorldState();
}

bool MaintainMapBlock(std::string* error) {
    uintptr_t world = 0;
    if (!Read(g_mapWorldManagerGlobal, world) || !world) return true;

    if (world != g_mapWorldManager) {
        // The old object is stale once the singleton changes. Do not write through it.
        ClearMapWorldState();
        g_mapWorldManager = world;
    }

    uint8_t current = 0xFF;
    if (!Read(world + kMapViewSkipOffset, current)) {
        if (error) *error = "Could not read WorldManager+0x258 (skip lot streaming)";
        return false;
    }
    if (current > 1) {
        if (error) *error = std::format("WorldManager+0x258 has an unexpected value ({})", static_cast<unsigned>(current));
        return false;
    }

    if (!g_mapOriginalValid) {
        g_mapOriginal = current;
        g_mapOriginalValid = true;
    }
    if (current == 1) return true;

    const uint8_t blocked = 1;
    if (!WriteExpected(world + kMapViewSkipOffset, blocked, current)) {
        if (error) *error = "Could not pause lot streaming in map view";
        return false;
    }

    g_mapWritten = true;
    LOG_INFO(std::format("[MapStreaming] Map view: lot streaming paused (skip gate {} -> 1)", static_cast<unsigned>(current)));
    return true;
}

} // namespace

bool Start(std::string* error) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_running) return true;

    g_throttleFlag = GameAddr::Get(GameAddr::Id::LotLodThrottleFlag);
    g_worldManagerGlobal = GameAddr::Get(GameAddr::Id::WorldManagerPtr);

    std::string missing;
    if (!GameAddr::GroupAvailable("LotLodStreaming", &missing)) {
        if (error) *error = "Lot LoD streaming addresses are incomplete: " + missing;
        return false;
    }

    LOG_INFO(std::format("[LotLod] Starting Smooth Lot Streaming on {}", GetGameVersionName()));
    LOG_INFO(std::format("[LotLod] Throttle flag {:#010x}; WorldManager global {:#010x}", g_throttleFlag, g_worldManagerGlobal));

    if (S3SSOwnsStreamingSettings()) {
        g_externalOwner = true;
        g_running = true;
        LOG_INFO("[LotLod] Official Sims3SettingsSetter owns LotStreamingOptimizations.streamingSettings; Apex makes no streaming-setting writes");
        return true;
    }

    g_externalOwner = false;
    g_activeThreshold = GameAddr::GroupAvailable("LotLodActiveFollow") ? GameAddr::Get(GameAddr::Id::LotLodActiveThreshold) : 0;
    if (!g_activeThreshold) LOG_WARNING("[LotLod] Max Active Lot Threshold not found: with more than 12 detailed lots the game's throttle stays off");
    if (!ApplyThrottle(error)) return false;

    std::string worldError;
    if (!ApplyCurrentWorld(&worldError)) {
        // The throttle was already changed: put it back if the second half cannot be validated.
        RestoreThrottleIfOwned();
        if (error) *error = worldError;
        return false;
    }

    MaintainActiveThreshold();
    g_running = true;
    LOG_INFO("[LotLod] Smooth Lot Streaming active");
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running) return;

    if (g_externalOwner) {
        LOG_INFO("[LotLod] Smooth Lot Streaming off in Apex; Sims3SettingsSetter remains the owner");
    } else {
        RestoreActiveThresholdIfOwned();
        RestoreWorldIfOwned();
        RestoreThrottleIfOwned();
        LOG_INFO("[LotLod] Smooth Lot Streaming stopped");
    }

    g_externalOwner = false;
    g_running = false;
    g_throttleFlag = 0;
    g_activeThreshold = 0;
    g_worldManagerGlobal = 0;
    ClearWorldState();
}

void Tick() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running || g_externalOwner) return;

    // Match S3SS live-setting semantics for the two settings this Stage owns: while enabled, the game's own runtime
    // writes are allowed to happen, but Apex reasserts the requested values on the next patch-manager tick.
    std::string throttleError;
    if (!MaintainThrottle(&throttleError) && !throttleError.empty()) {
        LOG_WARNING("[LotLod] " + throttleError);
    }

    std::string error;
    if (!ApplyCurrentWorld(&error) && !error.empty()) {
        // Fail closed on an unreadable/implausible field. Keep the original baseline for an already-known world so a
        // transient read failure cannot destroy safe restoration state; a new invalid world is cleared by ApplyCurrentWorld().
        LOG_WARNING("[LotLod] " + error + "; camera threshold left unchanged on this tick");
    }
    MaintainActiveThreshold();
}

bool Running() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_running;
}

bool HandledByS3SS() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_running && g_externalOwner;
}

std::string StatusText() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running) return "Off";
    if (g_externalOwner) return "Handled by Sims3SettingsSetter";
    if (!g_worldManager) return "On; waiting for a world";
    return "On; native lot transitions throttled, camera threshold 5.0";
}

bool StartMapViewBlocker(std::string* error) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_mapRunning) return true;

    g_mapWorldManagerGlobal = GameAddr::Get(GameAddr::Id::WorldManagerPtr);
    if (!g_mapWorldManagerGlobal) {
        if (error) *error = "WorldManager singleton address is unavailable";
        return false;
    }

    LOG_INFO(std::format("[MapStreaming] Starting map-view lot streaming blocker on {}", GetGameVersionName()));

    if (S3SSOwnsMapViewBlocker()) {
        g_mapExternalOwner = true;
        g_mapRunning = true;
        LOG_INFO("[MapStreaming] Official Sims3SettingsSetter owns LotStreamingOptimizations.mapViewBlocker; Apex makes no map-view streaming writes");
        return true;
    }

    if (!MapView::Available()) {
        if (error) *error = "The game's map-view state could not be resolved";
        g_mapWorldManagerGlobal = 0;
        return false;
    }

    g_mapExternalOwner = false;
    g_mapLastOpenTick = 0;
    ClearMapWorldState();
    g_mapRunning = true;
    LOG_INFO("[MapStreaming] Map-view lot streaming blocker active");
    return true;
}

void StopMapViewBlocker() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_mapRunning) return;

    if (g_mapExternalOwner) {
        LOG_INFO("[MapStreaming] Map-view blocker off in Apex; Sims3SettingsSetter remains the owner");
    } else {
        RestoreMapBlockIfOwned();
        LOG_INFO("[MapStreaming] Map-view lot streaming blocker stopped");
    }

    g_mapRunning = false;
    g_mapExternalOwner = false;
    g_mapWorldManagerGlobal = 0;
    g_mapLastOpenTick = 0;
    ClearMapWorldState();
}

void TickMapViewBlocker() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_mapRunning || g_mapExternalOwner) return;

    if (!MapView::Available()) {
        RestoreMapBlockIfOwned();
        return;
    }

    const bool inMapView = MapView::IsOpen();
    const uint64_t now = GetTickCount64();
    if (inMapView) g_mapLastOpenTick = now;

    const bool grace = g_mapLastOpenTick != 0 && (now - g_mapLastOpenTick) < kMapViewGraceMs;
    const bool shouldBlock = inMapView || grace;

    if (shouldBlock) {
        std::string error;
        if (!MaintainMapBlock(&error) && !error.empty()) LOG_WARNING("[MapStreaming] " + error);
        return;
    }

    if (g_mapOriginalValid) RestoreMapBlockIfOwned();
    g_mapLastOpenTick = 0;
}

bool MapViewBlockerRunning() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_mapRunning;
}

bool MapViewBlockerHandledByS3SS() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_mapRunning && g_mapExternalOwner;
}

std::string MapViewBlockerStatusText() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_mapRunning) return "Off";
    if (g_mapExternalOwner) return "Handled by Sims3SettingsSetter";
    if (!MapView::Available()) return "On; map-view detection unavailable";
    if (g_mapOriginalValid) return "On; lot streaming paused for map view";
    return "On; waiting for map view";
}

} // namespace LotLodStreaming
