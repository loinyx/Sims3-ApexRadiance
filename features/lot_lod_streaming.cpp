#include "lot_lod_streaming.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "game_version.h"
#include "memory_patch.h"
#include "s3ss_detect.h"
#include "map_view.h"
#include <Windows.h>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace LotLodStreaming {
namespace {

std::mutex g_lock;

// Map-view blocker state. This intentionally does not detour WorldManager::Update: Apex already documents
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

bool S3SSOwnsMapViewBlocker() {
    const S3SSDetect::Info info = S3SSDetect::Scan();
    if (!info.s3ssLoaded) return false;
    return S3SSDetect::S3SSPatchBoolSettingEnabled("LotStreamingOptimizations", "mapViewBlocker", true);
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
